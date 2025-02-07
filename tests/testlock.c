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
 *   gcc -I.. -O2 -fomit-frame-pointer -s -o testlock testlock.c -lpthread
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
unsigned int nbthreads, do_lock, arg_wait;
int arg_nice;
volatile unsigned int actthreads = 0;
int read_ratio = 256;
unsigned int global_lock = 0;

static volatile unsigned int step;
volatile unsigned int shared = 0;

static struct timeval start, stop;
static unsigned int global_work;
static unsigned int final_work;


void oneatwork(void *arg)
{
	int thr = (long)arg;
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
	while (1) {
		pl_take_r(&global_lock);
		if (shared & (1 << thr))
			fprintf(stderr, "thr=%d shared=0x%08x : unexpected 1\n", thr, shared);
		pl_drop_r(&global_lock);

		if ((loops & 0xFF) >= read_ratio) {
			pl_take_w(&global_lock);
			shared |= (1 << thr);
			pl_drop_w(&global_lock);
			//pl_or(&shared, (1 << thr));

			pl_take_s(&global_lock);
			if (!(shared & (1 << thr)))
				fprintf(stderr, "thr=%d shared=0x%08x : unexpected 0\n", thr, shared);
			pl_stow(&global_lock);
			shared &= ~(1 << thr);
			pl_drop_w(&global_lock);
			//pl_and(&shared, ~(1 << thr));
		}

		pl_take_r(&global_lock);
		if (shared & (1 << thr))
			fprintf(stderr, "thr=%d shared=0x%08x : unexpected 1\n", thr, shared);
		pl_drop_r(&global_lock);

		loops++;
		if (!(loops & 0x7F)) {	/* don't access RAM too often */
			if (pl_xadd(&global_work, 128) >= 20000000) {
				if (pl_xadd(&step, 1) == 2) { /* only take the first to end */
					final_work = global_work;
					gettimeofday(&stop, NULL);
				}
				break;
			}
		}
	}
	pl_dec_noret(&actthreads);
	//fprintf(stderr, "actthreads=%d\n", actthreads);
	pthread_exit(0);
}

void usage(int ret)
{
	printf("usage: testlock [-h] [-l] [-n nice] [-w wait_time] [-t threads] [-r read_ratio(0..256)]\n");
	exit(ret);
}

int main(int argc, char **argv)
{
	int i, err;
	unsigned long u;

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

	while (actthreads)
		usleep(100000);

	i = (stop.tv_usec - start.tv_usec);
	while (i < 0) {
		i += 1000000;
		start.tv_sec++;
	}
	i = i / 1000 + (int)(stop.tv_sec - start.tv_sec) * 1000;
	printf("threads: %d loops: %d time(ms): %d rate(lps): %Ld\n",
	       nbthreads, final_work, i, final_work * 1000ULL / i);

	/* All the work has ended */

	exit(0);
}
