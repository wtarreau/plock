/* Detect races between read and write locks. Aborts on anomaly such as
 * fixed by commit a68f743. It also shows the number of loops for both
 * reader and writers, and will stop after 10s.
 *   gcc -O2 tests/rwrace.c -pthread
 *   gcc -O2 tests/rwrace.c -pthread -DPLOCK_INLINE_EBO
 *   gcc -O2 tests/rwrace.c -pthread -DPLOCK_DISABLE_EBO
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include "../plock.h"

#define NB_READER 1
#define NB_WRITER 7

static unsigned long lock;

volatile int check = 0;
volatile long tot_wloops = 0;
volatile long tot_rloops = 0;

static void* thread_reader(void *arg)
{
	int loop_count = 0;

	while (1) {
		pl_take_r(&lock);
		check = 0;
		loop_count = 0;
		loop_count = random() % 1000;
		while (loop_count--) {
			if (check != 0)
				abort();

		}
		pl_drop_r(&lock);
		if (!(pl_ldadd(&tot_rloops, 1) & 0x1fffff))
			printf("loops=%ld (w=%ld r=%ld)\n", tot_wloops + tot_rloops, tot_wloops, tot_rloops);
	}
}

static void* thread_writer(void *arg)
{
	while (1) {
		pl_take_w(&lock);
		++check;
		pl_drop_w(&lock);
		if (!(pl_ldadd(&tot_wloops, 1) & 0x1fffff))
			printf("loops=%ld (w=%ld r=%ld)\n", tot_wloops + tot_rloops, tot_wloops, tot_rloops);
	}
}

int main(void)
{
	int i = 0;
	pthread_t *threads = calloc(NB_READER + NB_WRITER, sizeof(*threads));

	lock = 0;

	for (i = 0; i < NB_READER; ++i) {
		pthread_create(&threads[i], NULL, &thread_reader, (void*)(long)(i));
	}

	for (i = 0; i < NB_WRITER; ++i) {
		pthread_create(&threads[NB_READER+i], NULL, &thread_writer, (void*)(long)(NB_READER+i));
	}

	sleep(10);

	for (i = 0; i < NB_READER + NB_WRITER; ++i) {
		pthread_kill(threads[i], SIGTERM);
	}

	for (i = 0; i < NB_READER + NB_WRITER; ++i) {
		pthread_join(threads[i], NULL);
	}

	return 0;
}
