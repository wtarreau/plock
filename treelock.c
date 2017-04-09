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
int mode = 0;
int arg_nice;
volatile unsigned long actthreads = 0;
int read_ratio = 256;
unsigned long global_lock = 0;

static volatile unsigned long step;

static struct timeval start, stop;
static unsigned long global_work;
static unsigned long final_work;

/* read: U ; lookup : U ; write : U (reference only, not realistic) */
void loop_mode0(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			for (i = 0; i < 200; i++);
		} else {
			/* simulate a write */
			for (i = 0; i < 190; i++);
			for (i = 0; i < 10; i++);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

/* read: R ; lookup : R ; write : R (reference only, not realistic) */
void loop_mode1(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_r(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_r(&global_lock);
		} else {
			/* simulate a write */
			pl_take_r(&global_lock);
			for (i = 0; i < 190; i++);
			for (i = 0; i < 10; i++);
			pl_drop_r(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

/* read: S ; lookup : S ; write : W (typical of insert_unique) */
void loop_mode2(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_s(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_s(&global_lock);
		} else {
			/* simulate a write */
			pl_take_s(&global_lock);
			for (i = 0; i < 190; i++);
			pl_stow(&global_lock);
			for (i = 0; i < 10; i++);
			pl_drop_w(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

/* read: R ; lookup : S ; write : W (typical of lookup+insert) */
void loop_mode3(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_r(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_r(&global_lock);
		} else {
			/* simulate a write */
			pl_take_s(&global_lock);
			for (i = 0; i < 190; i++);
			pl_stow(&global_lock);
			for (i = 0; i < 10; i++);
			pl_drop_w(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

/* read: X ; lookup : X ; write : X (ext-locked insert_unique) */
void loop_mode4(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_x(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_x(&global_lock);
		} else {
			/* simulate a write */
			pl_take_x(&global_lock);
			for (i = 0; i < 190; i++);
			for (i = 0; i < 10; i++);
			pl_drop_x(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

/* read: R ; lookup : X ; write : X (ext-locked lookup+insert) */
void loop_mode5(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_r(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_r(&global_lock);
		} else {
			/* simulate a write */
			pl_take_x(&global_lock);
			for (i = 0; i < 190; i++);
			for (i = 0; i < 10; i++);
			pl_drop_x(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

/* read: R ; lookup : R ; write : A (typical of atomic pick) */
void loop_mode6(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_r(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_r(&global_lock);
		} else {
			/* simulate a write */
			while (1) {
				/* if we fail the upgrade, we have to run
				 * the lookup again.
				 */
				pl_take_r(&global_lock);
				for (i = 0; i < 190; i++);
				if (pl_try_rtoa(&global_lock))
					break;
				pl_drop_r(&global_lock);
			}
			for (i = 0; i < 10; i++);
			pl_drop_a(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

/* read: R ; lookup : A ; write : A (typical of insert+delete) */
void loop_mode7(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_r(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_r(&global_lock);
		} else {
			/* simulate a write */
			pl_take_a(&global_lock);
			for (i = 0; i < 190; i++);
			for (i = 0; i < 10; i++);
			pl_drop_a(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

/* R-locked lookup, F then W-locked write */
void loop_mode8(void)
{
	int loops = 0;
	volatile int i;

	do {
		if ((loops & 0xFF) < read_ratio) {
			/* simulate a read */
			pl_take_r(&global_lock);
			for (i = 0; i < 200; i++);
			pl_drop_r(&global_lock);
		} else {
			/* simulate a write */
			while (1) {
				/* if we fail the upgrade, we have to run
				 * the lookup again.
				 */
				pl_take_r(&global_lock);
				for (i = 0; i < 190; i++);
				if (pl_try_rtos(&global_lock))
					break;
				pl_drop_r(&global_lock);
			}
			/* now we are S-locked */
			pl_stow(&global_lock);
			for (i = 0; i < 10; i++);
			pl_drop_w(&global_lock);
		}
		/* simulate some real work */
		for (i = 0; i < 400; i++);

	} while ((++loops & 0x7f) || /* limit stress on global_work */
	         pl_xadd(&global_work, 128) < 20000000);
}

void oneatwork(int thr)
{
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
	switch(mode) {
	case 0: loop_mode0(); break;
	case 1: loop_mode1(); break;
	case 2: loop_mode2(); break;
	case 3: loop_mode3(); break;
	case 4: loop_mode4(); break;
	case 5: loop_mode5(); break;
	case 6: loop_mode6(); break;
	case 7: loop_mode7(); break;
	case 8: loop_mode8(); break;
	}

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
	printf("usage: treelock [-h] [-l] [-n nice] [-t threads] [-r read_ratio(0..256)] [-m <0..8>]\n"
	       "       modes (-m, default 0) :\n"
	       "         0 : read: U ; lookup : U ; write : U (reference only, not realistic)\n"
	       "         1 : read: R ; lookup : R ; write : R (reference only, not realistic)\n"
	       "         2 : read: S ; lookup : S ; write : W (typical of insert_unique)\n"
	       "         3 : read: R ; lookup : S ; write : W (typical of lookup+insert)\n"
	       "         4 : read: X ; lookup : X ; write : X (ext-locked insert_unique)\n"
	       "         5 : read: R ; lookup : X ; write : X (ext-locked lookup+insert)\n"
	       "         6 : read: R ; lookup : R ; write : A (typical of atomic pick)\n"
	       "         7 : read: R ; lookup : A ; write : A (typical of insert+delete)\n"
	       "         8 : read: R ; lookup : R ; write : W (cache with high hit ratio)\n"
	       "");
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
		else if (!strcmp(*argv, "-m")) {
			if (--argc < 0)
				usage(1);
			mode = atol(*++argv);
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
