/*
 * Cache line sharing performance impact measurement.
 * (C) 1999,2005,2011,2017 / Willy Tarreau  <w@1wt.eu>
 *
 * You can do whatever you want with this program, but I'm not
 * responsible for any misuse. Be aware that it can heavily load
 * a host. As it is multithreaded, it might take advantages of SMP.
 *
 * To compile, you need libpthread :
 *
 *   gcc -I.. -O2 -fomit-frame-pointer -s -o sharing sharing.c -lpthread
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
int arg_am = 0;
unsigned int nbthreads = 2;
static volatile unsigned long actthreads;
static volatile unsigned long step;

static struct timeval start, stop;
static unsigned long final_work[MAXTHREADS];

unsigned long *locks[MAXTHREADS];

void oneatwork(void *arg)
{
	int thr = (long)arg;
	unsigned long *lock = locks[thr];
	unsigned long l = 0;

	/* step 0: creating all threads */
	while (step == 0) {
		/* don't disturb pthread_create() */
		usleep(10000);
	}

	/* step 1 : waiting for signal to start */
	pl_inc_noret(&actthreads);
	while (step == 1);

	/* step 2 : running */
	if (arg_am == 0) {
		while (step == 2) {
			l++;
			*(volatile unsigned long*)lock = l;
		}
	}
	else if (arg_am == 1) {
		while (step == 2) {
			l++;
			(*(volatile unsigned long*)lock)++;
		}
	}
	else if (arg_am == 2) {
		while (step == 2) {
			l++;
			pl_inc_noret(lock);
		}
	}

	final_work[thr] = l;
	pl_dec_noret(&actthreads);
	pthread_exit(0);
}

void usage(int ret)
{
	printf("usage: sharing [-h] [-n nice] [-t threads] [-a access]\n"
	       "Access modes :\n"
	       "  0 : (*value)++\n"
	       "  1 : (volatile *value)++\n"
	       "  2 : lock_inc(value)\n"
	       "\n");
	exit(ret);
}

int main(int argc, char **argv)
{
	int i, err;
	unsigned long u;
	unsigned long *work_area;
	unsigned long total;
	unsigned long incr;
	unsigned long dist;
	unsigned long ms;

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
		else if (!strcmp(*argv, "-a")) {
			if (--argc < 0)
				usage(1);
			arg_am = atoi(*++argv);
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
	setbuf(stdout, NULL);

	work_area = calloc(1, 4096 * nbthreads);

	printf("Dist.   Loops/ms  Incr/ms (loops-per-thread/ms)\n");
	for (dist = 0; dist < 4096 / sizeof(long); dist = dist ? (dist << 1) : 1) {
		printf(" %4lu: ", dist * sizeof(long));

		for (u = 0; u < nbthreads; u++) {
			locks[u] = work_area + u * dist;
			*locks[u] = 0;
			final_work[u] = 0;
		}

		actthreads = 0;	step = 0;

		for (u = 0; u < nbthreads; u++) {
			if ((err = pthread_create(&thr[u], NULL, (void *)&oneatwork, (void *)u)) != 0) {
				perror("");
				exit(1);
			}
			pthread_detach(thr[u]);
		}

		pl_inc_noret(&step);  /* let the threads warm up and get ready to start */

		while (actthreads != nbthreads);

		gettimeofday(&start, NULL);
		pl_inc_noret(&step); /* fire ! */

		usleep(100000);
		pl_inc_noret(&step);
		gettimeofday(&stop, NULL);

		i = (stop.tv_usec - start.tv_usec);
		while (i < 0) {
			i += 1000000;
			start.tv_sec++;
		}
		ms = i / 1000 + (int)(stop.tv_sec - start.tv_sec) * 1000;

		while (actthreads)
			usleep(100000);

		/* count and report total work done */
		total = incr = 0;
		for (u = 0; u < nbthreads; u++) {
			total += final_work[u];
			/* don't count the final value multiple times if it's the same location */
			if (dist || !u)
				incr  += *locks[u];
		}

		printf(" %8lu %8lu (", total/ms, incr/ms);
		for (u = 0; u < nbthreads; u++)
			printf("%lu%s", final_work[u]/ms, (u < nbthreads-1) ? " " : "");
		printf(")\n");
	}

	/* All the work has ended */

	exit(0);
}
