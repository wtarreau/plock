/*
 * RW lock speed tester.
 * (C) 1999,2005,2011 / Willy Tarreau  <w@1wt.eu>
 *
 * You can do whatever you want with this program, but I'm not
 * responsible for any misuse. Be aware that it can heavily load
 * a host. As it is multithreaded, it might take advantages of SMP.
 *
 * It renices itself to +20, creates the required amount of threads and
 * measures their cumulative work (i.e. loop iterations/second). It
 * allows a more accurate CPU usage measure than vmstat, especially
 * when time is spent in interrupt handling.
 *
 * To compile, you need libpthread :
 *
 * gcc -O2 -fomit-frame-pointer -s -o threads threads.c -lpthread
 *
 *
 */

#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define main _main
#include "plock.h"
#undef main

#define MAXTHREADS	64

pthread_t thr[MAXTHREADS];
unsigned int nbthreads, do_lock, arg_wait;
int arg_nice;
volatile unsigned long actthreads = 0;
int read_ratio = 256;
unsigned long global_lock = 0;

static volatile unsigned long step;

static struct timeval start, stop;
static unsigned long global_work;
static unsigned long final_work;


void oneatwork(int thr)
{
	int loops = 0;

	(void)thr; /* to mark it used */

	/* step 0: creating all threads */
	while (step == 0) {
		/* don't disturb pthread_create() */
		usleep(10000);
	}

	/* step 1 : waiting for signal to start */
	pl_inc_noret(&actthreads);
	while (step == 1);

	/* step 2 : running */
	do {
		volatile int i;

		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_rd(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_rd(&global_lock);
		} else {
			/* simulate a write */
			//pl_take_rd(&global_lock);
			pl_take_fr(&global_lock);
			//pl_take_wx(&global_lock);
			for (i = 0; i < 190; i++);
			pl_take_wr(&global_lock);
			for (i = 0; i < 10; i++);
			pl_drop_wx(&global_lock);
			//pl_drop_wx(&global_lock);
			//pl_drop_rd(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);
	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);

	/* only time the first finishing thread */
	if (pl_xadd(&step, 1) == 2) {
		final_work = global_work;
		gettimeofday(&stop, NULL);
	}

	pl_dec_noret(&actthreads);
	//fprintf(stderr, "actthreads=%d\n", actthreads);
	pthread_exit(0);
}

void usage(int ret)
{
	printf("usage: idletime [-h] [-l] [-n nice] [-w wait_time] [-t threads] [-r read_ratio(0..256)]\n");
	exit(ret);
}

int main(int argc, char **argv)
{
	int i, err;
	unsigned int u;

	nbthreads = 1;
	arg_wait = 1;
	do_lock = 0;
	arg_nice = 0;

	argc--; argv++;
	while (argc > 0) {
		if (!strcmp(*argv, "-t")) {
			if (--argc < 0)
				usage(1);
			nbthreads = atol(*++argv);
		}
		else if (!strcmp(*argv, "-w")) {
			if (--argc < 0)
				usage(1);
			arg_wait = atol(*++argv);
		}
		else if (!strcmp(*argv, "-n")) {
			if (--argc < 0)
				usage(1);
			arg_nice = atol(*++argv);
		}
		else if (!strcmp(*argv, "-r")) {
			if (--argc < 0)
				usage(1);
			read_ratio = atol(*++argv);
		}
		else if (!strcmp(*argv, "-l"))
			do_lock = 1;
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

	//printf("Starting %d thread%c\n", nbthreads, (nbthreads > 1)?'s':' ');

	for (u = 0; u < nbthreads; u++) {
		if ((err = pthread_create(&thr[u], NULL, (void *)&oneatwork, NULL)) != 0) {
			perror("");
			exit(1);
		}
		pthread_detach(thr[u]);
	}
	
	pl_inc_noret(&step);  /* let the threads warm up and get ready to start */

	while (actthreads != nbthreads);

	gettimeofday(&start, NULL);
	pl_inc_noret(&step); /* fire ! */

	while (actthreads)
		usleep(100000);

	i = (stop.tv_usec - start.tv_usec);
	while (i < 0) {
		i += 1000000;
		start.tv_sec++;
	}
	i = i / 1000 + (int)(stop.tv_sec - start.tv_sec) * 1000;
	printf("threads: %d loops: %lu time(ms): %d rate(lps): %Ld\n",
	       nbthreads, final_work, i, final_work * 1000ULL / i);

	/* All the work has ended */

	exit(0);
}
