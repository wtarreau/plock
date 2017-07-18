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
 *   gcc -I.. -O2 -fomit-frame-pointer -s -o concurrent concurrent.c -lpthread
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
unsigned int nbthreads = 2;
static volatile unsigned long actthreads;
static volatile unsigned long step;

static struct timeval start, stop;
static unsigned long final_work[MAXTHREADS];
static long mask;

volatile long lock;

void oneatwork(void *arg)
{
	int thr = (long)arg;
	long l = 0;
	int do_write = (mask & (1 << thr));

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
	while (step == 2) {
		if (do_write) {
			/* write */
			l++;
			//pl_cpu_relax();
			pl_inc(&lock);// = 1;
			//pl_cpu_relax();
		}
		else {
			/* read */
			l++;
			//pl_cpu_relax();
			if (!(l & 127)) pl_cpu_relax();
			if (!lock)
				lock++;
		}
	}

	final_work[thr] = l;
	pl_dec(&actthreads);
	pthread_exit(0);
}

void usage(int ret)
{
	printf("usage: concurrent [-h] [-n nice] [-t threads] [-m writer_mask]\n");
	exit(ret);
}

int main(int argc, char **argv)
{
	int i, err;
	unsigned long u;

	arg_nice = 0;

	argc--; argv++;
	while (argc > 0) {
		if (!strcmp(*argv, "-t")) {
			if (--argc < 0)
				usage(1);
			nbthreads = atol(*++argv);
		}
		else if (!strcmp(*argv, "-n")) {
			if (--argc < 0)
				usage(1);
			arg_nice = atol(*++argv);
		}
		else if (!strcmp(*argv, "-m")) {
			if (--argc < 0)
				usage(1);
			mask = atol(*++argv);
		}
		else if (!strcmp(*argv, "-h"))
			usage(0);
		else
			usage(1);
		argc--; argv++;
	}

	if (nbthreads >= MAXTHREADS)
		nbthreads = MAXTHREADS;

	nice(arg_nice);

	actthreads = 0;	step = 0;

	setbuf(stdout, NULL);

	for (u = 0; u < nbthreads; u++) {
		if ((err = pthread_create(&thr[u], NULL, (void *)&oneatwork, (void *)u)) != 0) {
			perror("");
			exit(1);
		}
		pthread_detach(thr[u]);
	}

	pl_inc(&step);  /* let the threads warm up and get ready to start */

	while (actthreads != nbthreads);

	gettimeofday(&start, NULL);
	pl_inc(&step); /* fire ! */

	sleep(2);
	pl_inc(&step);
	gettimeofday(&stop, NULL);

	while (actthreads)
		usleep(100000);

	i = (stop.tv_usec - start.tv_usec);
	while (i < 0) {
		i += 1000000;
		start.tv_sec++;
	}
	u = i / 1000 + (int)(stop.tv_sec - start.tv_sec) * 1000;

	for (i = 0; i < (int)nbthreads; i++) {
		printf("thread: %2d loops: %10lu time(ms): %lu rate(lps): %10Lu, access(ns): %3lu\n",
		       i, final_work[i], u, final_work[i] * 1000ULL / u, u * 1000000UL / final_work[i]);
	}

	/* All the work has ended */

	exit(0);
}
