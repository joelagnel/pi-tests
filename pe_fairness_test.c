#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define __USE_GNU
#define __USE_UNIX98
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

double measure_time(void (*fun)(void)) {
    // Calculate the time taken by fun()
    clock_t t;
    t = clock();
    fun();
    t = clock() - t;
    double time_taken = ((double)t)/CLOCKS_PER_SEC; // in seconds
 
	return time_taken;
}

#define gettid() syscall(__NR_gettid)

static int debug;
static int mark_fd = -1;
static __thread char buff[BUFSIZ+1];

static void setup_ftrace_marker(void)
{
	struct stat st;
	char *files[] = {
		"/sys/kernel/debug/tracing/trace_marker",
		"/debug/tracing/trace_marker",
		"/debugfs/tracing/trace_marker",
	};
	int ret;
	int i;

	if (debug) {
		mark_fd = 1;
		return;
	}

	for (i = 0; i < (sizeof(files) / sizeof(char *)); i++) {
		ret = stat(files[i], &st);
		if (ret >= 0)
			goto found;
	}
	/* todo, check mounts system */
	return;
found:
	mark_fd = open(files[i], O_WRONLY);
}

static void ftrace_write(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (mark_fd < 0)
		return;

	va_start(ap, fmt);
	n = vsnprintf(buff, BUFSIZ, fmt, ap);
	va_end(ap);

	write(mark_fd, buff, n);
}

/*
 * Three threads A B and C. A is the highest prio
 * C is the lowest.  C starts out and grabs mutex L
 * then A starts, preempting C and it to grabs mutex L
 * So C starts again to finish up, but this time we also
 * wake up B. If B runs and preempts C, we fail,  since
 * that's unbounded priority inversion!
 */
#define LOW_PRIO 3
#define MAIN_START_PRIO 1
#define MAIN_PRIO 6
#define A_PRIO 5
#define B_PRIO 4
#define C_PRIO 2

#define A_NICE -10
#define B_NICE -10
#define C_NICE -10
#define D_NICE -10

static int a_prio;
static pid_t a_pid;

#define SLEEP_SECS 5

/* A_lock, keep A from running */
pthread_mutex_t A_lock;
/* B_lock, keep B from running */
pthread_mutex_t B_lock;

/* L_lock, the lock that A and C want */
pthread_mutex_t L_lock;

/* ordering of events */
int running = 1;
static pthread_barrier_t tell_main_A_is_running;
static pthread_barrier_t flag_C_to_start_B;
static pthread_barrier_t tell_main_B_is_running;
static pthread_barrier_t tell_main_C_has_lock_L;
int B_ran_before_us;
int B_spins_a_long_time = 1;
int B_ran_a_lot_more;
int PI_has_failed;
double C_spin_time;

pthread_mutexattr_t attr;

static void init_flags(void)
{
	running = 1;
	B_ran_before_us = 0;
	B_spins_a_long_time = 1;
	B_ran_a_lot_more = 0;
	PI_has_failed = 0;
}

#define barrier() asm volatile (" " : : : "memory")

static void set_thread_prio(pid_t pid, int prio)
{
	struct sched_param sp = { .sched_priority = prio };

	/* set up our priority */
	sched_setscheduler(pid, SCHED_FIFO, &sp);
}

static void set_prio(int prio)
{
	set_thread_prio(0, prio);
}

struct sched_attr {
	__u32 size;

	__u32 sched_policy;
	__u64 sched_flags;

	/* SCHED_NORMAL, SCHED_BATCH */
	__s32 sched_nice;

	/* SCHED_FIFO, SCHED_RR */
	__u32 sched_priority;

	/* SCHED_DEADLINE */
	__u64 sched_runtime;
	__u64 sched_deadline;
	__u64 sched_period;

	/* Utilization hints */
	__u32 sched_util_min;
	__u32 sched_util_max;
};

static void set_prio_nice(int nice)
{
	struct sched_attr attr = {};

	attr.size = sizeof(attr);
	attr.sched_policy = SCHED_OTHER;
	attr.sched_nice = nice;

	if (syscall(SYS_sched_setattr, 0 /* pid */, &attr, 0) < 0) {
		perror("set_prio_nice: setattr failed.");
	}
}

static void set_thread_prio_bind_cpu(pid_t pid, int prio)
{
	cpu_set_t cpumask;

	CPU_ZERO(&cpumask);
	CPU_SET(0, &cpumask);

	set_thread_prio(pid, prio);

	/* bind to cpu 0 */
	sched_setaffinity(pid, sizeof(cpumask), &cpumask);
}

static void set_prio_bind_cpu(int prio)
{
	set_thread_prio_bind_cpu(0, prio);
}

static void bind_cpu(int cpu)
{
	cpu_set_t cpumask;

	CPU_ZERO(&cpumask);
	CPU_SET(cpu, &cpumask);
	sched_setaffinity(0, sizeof(cpumask), &cpumask);
}
/* do crap */
static int x;
void func(void)
{
	x++;
}

static void *thread_A(void *arg)
{
	bind_cpu(0);
	// set_prio_nice(A_NICE);

	a_pid = gettid();

	ftrace_write("A is running\n");
	// pthread_barrier_wait(&tell_main_A_is_running);

	while (1)
		func();
#if 0
	/* now wait till we should start */
	pthread_mutex_lock(&A_lock);

	ftrace_write("A flags C to start B\n");
	pthread_barrier_wait(&flag_C_to_start_B);

	/* We should have preempted C so we grab lock L */
	ftrace_write("A grabs lock L\n");
	pthread_mutex_lock(&L_lock);
	ftrace_write("A has lock L\n");
	pthread_mutex_unlock(&L_lock);

	pthread_mutex_unlock(&A_lock);

	ftrace_write("A exits\n");
#endif
	return NULL;
}

static void *thread_B(void *arg)
{
	bind_cpu(0);
	// set_prio_nice(B_NICE);

	b_pid = gettid();

	ftrace_write("B is running\n");
	// pthread_barrier_wait(&tell_main_A_is_running);

	while (1)
		func();
#if 0
	/* now wait till we should start */
	pthread_mutex_lock(&A_lock);

	ftrace_write("A flags C to start B\n");
	pthread_barrier_wait(&flag_C_to_start_B);

	/* We should have preempted C so we grab lock L */
	ftrace_write("A grabs lock L\n");
	pthread_mutex_lock(&L_lock);
	ftrace_write("A has lock L\n");
	pthread_mutex_unlock(&L_lock);

	pthread_mutex_unlock(&A_lock);

	ftrace_write("A exits\n");
#endif
	return NULL;
}

static void *thread_C(void *arg)
{
	bind_cpu(0);
	// set_prio_nice(C_NICE);

	c_pid = gettid();

	ftrace_write("C is running\n");
	// pthread_barrier_wait(&tell_main_A_is_running);

	while (1)
		func();
#if 0
	/* now wait till we should start */
	pthread_mutex_lock(&A_lock);

	ftrace_write("A flags C to start B\n");
	pthread_barrier_wait(&flag_C_to_start_B);

	/* We should have preempted C so we grab lock L */
	ftrace_write("A grabs lock L\n");
	pthread_mutex_lock(&L_lock);
	ftrace_write("A has lock L\n");
	pthread_mutex_unlock(&L_lock);

	pthread_mutex_unlock(&A_lock);

	ftrace_write("A exits\n");
#endif
	return NULL;
}

static void *thread_D(void *arg)
{
	bind_cpu(0);
	// set_prio_nice(D_NICE);

	d_pid = gettid();

	ftrace_write("D is running\n");
	// pthread_barrier_wait(&tell_main_A_is_running);

	while (1)
		func();
#if 0
	/* now wait till we should start */
	pthread_mutex_lock(&A_lock);

	ftrace_write("A flags C to start B\n");
	pthread_barrier_wait(&flag_C_to_start_B);

	/* We should have preempted C so we grab lock L */
	ftrace_write("A grabs lock L\n");
	pthread_mutex_lock(&L_lock);
	ftrace_write("A has lock L\n");
	pthread_mutex_unlock(&L_lock);

	pthread_mutex_unlock(&A_lock);

	ftrace_write("A exits\n");
#endif
	return NULL;
}


#if 0
static void *thread_B(void *arg)
{
	set_prio_bind_cpu(B_PRIO);
	set_prio_nice(B_NICE);

	ftrace_write("B tells main, it is running\n");
	pthread_barrier_wait(&tell_main_B_is_running);

	pthread_mutex_lock(&B_lock);

	ftrace_write("B is running\n");
	B_ran_before_us = 1;
	barrier();

	while (B_spins_a_long_time)
		func();
	ftrace_write("B is done running\n");

	B_ran_a_lot_more = 1;

	pthread_mutex_unlock(&B_lock);

	ftrace_write("B exits\n");
	return NULL;
}

static inline void c_spin_a_little(void)
{
	/* spin a little */
	for (unsigned long long i=0; i < 1000000000 && running; i++) {
		func();
	}
}

static void *thread_C(void *arg)
{
	set_prio_bind_cpu(C_PRIO);

	// Drop to OTHER with nice -10 for testing.
	set_prio_nice(C_NICE);

	ftrace_write("C is running\n");
	pthread_mutex_lock(&B_lock);

	pthread_mutex_lock(&L_lock);

	ftrace_write("C has all locks\n");
	pthread_barrier_wait(&tell_main_C_has_lock_L);

	pthread_barrier_wait(&flag_C_to_start_B);

	ftrace_write("C wakes up B\n");
	pthread_mutex_unlock(&B_lock);
	ftrace_write("C wakes up B and start C's loop\n");

	C_spin_time = measure_time(c_spin_a_little);

	ftrace_write("C is done with loop\n");

	if (B_ran_before_us)
		PI_has_failed = 1;
	if (B_ran_a_lot_more)
		PI_has_failed = 2;

	ftrace_write("C releases lock L\n");
	pthread_mutex_unlock(&L_lock);

	ftrace_write("C exits\n");
	return NULL;
}
#endif

static void perr(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(-1);
}

int test_task_spin(int block)
{
	pthread_t A,B,C,D;
	int ret = pthread_create(&A, NULL, thread_A, NULL);
	if (ret < 0)
		perr("creating thread A");

	int ret = pthread_create(&B, NULL, thread_B, NULL);
	if (ret < 0)
		perr("creating thread A");

	int ret = pthread_create(&C, NULL, thread_C, NULL);
	if (ret < 0)
		perr("creating thread A");

	int ret = pthread_create(&D, NULL, thread_D, NULL);
	if (ret < 0)
		perr("creating thread A");

	pthread_join(A, NULL);
	pthread_join(B, NULL);
	pthread_join(C, NULL);
	pthread_join(D, NULL);
}

#if 0
int pi_inherit_test(int up_prio)
{
	pthread_t A,B,C;
	int ret;
	int secs = SLEEP_SECS;

	init_flags();

	if (up_prio)
		a_prio = LOW_PRIO;
	else
		a_prio = A_PRIO;

	/* We start main off at low prio to let other threads run */
	set_prio_bind_cpu(MAIN_START_PRIO);

	/* grab lock A and kick A off */
	pthread_mutex_lock(&A_lock);
	ret = pthread_create(&A, NULL, thread_A, NULL);
	if (ret < 0)
		perr("creating thread A");

	pthread_barrier_wait(&tell_main_A_is_running);

	/* thread A should be blocked on A_lock */

	/* up our prio to preempt all other threads */
	set_prio(MAIN_PRIO);

	ftrace_write("main: start C thread\n");
	/* create thread C and let run wild */
	ret = pthread_create(&C, NULL, thread_C, NULL);
	if (ret < 0)
		perr("creating thread C");

	/* wait for thread C to get the locks */
	pthread_barrier_wait(&tell_main_C_has_lock_L);

	ftrace_write("main: C is started\n");

	ftrace_write("main: create B thread\n");
	/* now we can start B */
	ret = pthread_create(&B, NULL, thread_B, NULL);
	if (ret < 0)
		perr("creating thread B");

	/* B inherited our prio, so spin till we see it running */
	pthread_barrier_wait(&tell_main_B_is_running);

	ftrace_write("main: B is running\n");

	ftrace_write("main: let A loose\n");
	/* OK, we are all set, lets wake up A */
	pthread_mutex_unlock(&A_lock);

	sleep(secs);

	if (up_prio) {
		ftrace_write("main: up A's prio\n");
		set_thread_prio(a_pid, A_PRIO);
		sleep(secs);
	}

	ftrace_write("main: stop all\n");
	B_spins_a_long_time = 0;
	running = 0;

	ftrace_write("wait for A\n");
	pthread_join(A, NULL);
	ftrace_write("wait for B\n");
	pthread_join(B, NULL);
	ftrace_write("wait for C\n");
	pthread_join(C, NULL);

	if (PI_has_failed && (!up_prio || PI_has_failed > 1)) {
		printf("PI failed!\n");
		if (PI_has_failed > 1)
			printf("  and it failed badly!\n");
		ret = -1;
	} else {
		printf("PI worked like a charm\n");
		ret = 0;
	}

	return ret;
}

#endif

int main (int argc, char **argv)
{
	int nopi = 0;
	int c;
	int ret;

#if 0
	while ((c=getopt(argc, argv, "nd")) >= 0) {
		switch (c) {
			case 'n':
				nopi = 1;
				break;
			case 'd':
				debug = 1;
				break;
		}
	}
#endif

	// printf("When C is not blocked, it shoud take: %lf secs\n",  measure_time(c_spin_a_little));

	setup_ftrace_marker();

	if (pthread_mutexattr_init(&attr))
		perr("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT))
		perr("pthread_mutexattr_setprotocol");

	pthread_mutex_init(&A_lock, NULL);
	pthread_mutex_init(&B_lock, NULL);

	pthread_barrier_init(&tell_main_A_is_running, NULL, 2);
	pthread_barrier_init(&flag_C_to_start_B, NULL, 2);
	pthread_barrier_init(&tell_main_B_is_running, NULL, 2);
	pthread_barrier_init(&tell_main_C_has_lock_L, NULL, 2);

	if (nopi)
		pthread_mutex_init(&L_lock, NULL);
	else
		if (pthread_mutex_init(&L_lock, &attr))
			perr("pthread_mutex_init");

	test_task_spin(0);
#if 0
	ret = pi_inherit_test(0);
	printf("Time C took to do work: %lf secs\n", C_spin_time);
	if (ret)
		exit(ret);

	printf("Now testing by boosting A to high prio\n");
	// ret = pi_inherit_test(1);
	// if (ret)
	//	exit(ret);
#endif
	exit(0);
}
