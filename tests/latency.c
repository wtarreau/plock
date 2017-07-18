/*
 * Lock acquisition speed tester.
 * (C) 1999,2005,2011,2017 / Willy Tarreau  <w@1wt.eu>
 *
 * You can do whatever you want with this program, but I'm not
 * responsible for any misuse. Be aware that it can heavily load
 * a host. As it is multithreaded, it might take advantages of SMP.
 *
 * To compile, you need libpthread :
 *
 *   gcc -I.. -O2 -fomit-frame-pointer -s -o latency latency.c -lpthread
 *
 *
 */

#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <plock.h>

#define MAXTHREADS	64

pthread_t thr[MAXTHREADS];
int arg_nice;
int read_ratio = 256;
static volatile unsigned long actthreads;
static volatile unsigned long step;

static struct timeval start, stop;
static unsigned long final_work;

volatile long lock;

void oneatwork(void *arg)
{
	int thr = (long)arg;
	int loops = 0;
	long l = lock;

	(void)thr; /* to mark it used */

	/* step 0: creating all threads */
	while (step == 0) {
		/* don't disturb pthread_create() */
		usleep(10000);
	}

	/* step 1 : waiting for signal to start */
	pl_inc(&actthreads);
	while (step == 1);

	/* step 2 : running */
	if (!thr) {
		/* thread 0 :
		 *  wait !&2
		 *  add 1
		 *  wait &2
		 *  sub 1
		 */
		while (step == 2) {
			while ((l & 2))
				l = lock;

			l = __atomic_add_fetch(&lock, 5, __ATOMIC_RELAXED);

			while (!(l & 2) && l < (20000000 << 2))
				l = lock;

			l = __atomic_sub_fetch(&lock, 1, __ATOMIC_RELAXED);

			if (l >= 20000000 << 2)
				break;
		}
	}
	else {
		/* thread 1 :
		 *  wait &1
		 *  add 2
		 *  wait !&1
		 *  sub 2
		 */
		while (1) {
			while (!(l & 1) && l < (20000000 << 2))
				l = lock;

			l = __atomic_add_fetch(&lock, 6, __ATOMIC_RELAXED);

			while ((l & 1))
				l = lock;

			l = __atomic_sub_fetch(&lock, 2, __ATOMIC_RELAXED);

			if (l >= 20000000 << 2)
				break;
		}
	}

	if (pl_xadd(&step, 1) == 2) { /* only take the first to end */
		final_work = l >> 2;
		gettimeofday(&stop, NULL);
	}

	pl_dec(&actthreads);
	pthread_exit(0);
}

void usage(int ret)
{
	printf("usage: latency [-h] [-n nice]\n");
	exit(ret);
}

int main(int argc, char **argv)
{
	int i, err;
	unsigned long u;

	arg_nice = 0;

	argc--; argv++;
	while (argc > 0) {
		if (!strcmp(*argv, "-n")) {
			if (--argc < 0)
				usage(1);
			arg_nice = atol(*++argv);
		}
		else if (!strcmp(*argv, "-h"))
			usage(0);
		else
			usage(1);
		argc--; argv++;
	}

	nice(arg_nice);

	actthreads = 0;	step = 0;

	setbuf(stdout, NULL);

	for (u = 0; u < 2; u++) {
		if ((err = pthread_create(&thr[u], NULL, (void *)&oneatwork, (void *)u)) != 0) {
			perror("");
			exit(1);
		}
		pthread_detach(thr[u]);
	}

	pl_inc(&step);  /* let the threads warm up and get ready to start */

	while (actthreads != 2);

	gettimeofday(&start, NULL);
	pl_inc(&step); /* fire ! */

	while (actthreads)
		usleep(100000);

	i = (stop.tv_usec - start.tv_usec);
	while (i < 0) {
		i += 1000000;
		start.tv_sec++;
	}
	u = i / 1000 + (int)(stop.tv_sec - start.tv_sec) * 1000;
	printf("threads: %d loops: %d time(ms): %lu rate(lps): %Lu, bounce(ns): %lu\n",
	       2, final_work, u, final_work * 1000ULL / u, u * 1000000UL / final_work);

	/* All the work has ended */

	exit(0);
}
