/*
 * Benchmark for locking mechanisms -- 2017-07-23 - Willy Tarreau <w@1wt.eu>
 *
 * The benchmark models a very simple LRU cache. The idea is to measure
 * different locking mechanism for read-intensive workloads, like caches. A
 * typical LRU cache will be used to cache the result of some expensive
 * computations. Here as "expensive computation", we turn an integer into its
 * text representation. And to make it a bit heavier than a single snprintf()
 * call, we iterate over it 100 times so that a successful lookup is cheaper
 * than a computation. The storage is made of a simple hash table. Random keys
 * are emitted, and for each of them, a lookup is performed in the cache. If
 * the key is found, the associated string is used. Otherwise the string is
 * computed and the result stored into the cache. After an insertion, the cache
 * is trimmed to ensure it's never larger than the configured size. The result
 * is then checked using atoi() to verify that it matches (which mimmicks the
 * use of the looked up data). A lock is added so that it's possible to use
 * this cache from multiple threads. For two seconds, several threads run this
 * workload in parallel, looking up random values in this shared cache. The
 * number of operations per thread is collected and aggregated at the end, then
 * reported.
 *
 * You can do whatever you want with this program, I'm not responsible for any
 * misuse.
 *
 * To compile, libpthread is needed :
 *
 *   gcc -I.. -O2 -fomit-frame-pointer -s -o lrubench lrubench.c -lpthread
 *
 * The default cache size is set to 100 entries per list head, which is 3200
 * entries for 32 heads. This can be adjusted using "-s". The default key space
 * is set to 1% above the cache size to cause on average 1% miss and result in
 * 99% hit ratio. This can be adjusted using "-k". The default number of
 * threads is set to 2 and can be adjusted using "-t". The locking mechanisms
 * can be set using "-m". It defaults to 0 which is no lock and which is only
 * supported with a single thread (to serve as a reference). Run with "-h" to
 * get some help.
 *
 */

#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <plock.h>

#define MAXTHREADS	64
#define NBHEADS		32

/* string size for stored data : 12 is enough to store the largest ints */
#define STRSZ 12

/* runtime arguments */
unsigned int arg_cache_size = 100 * NBHEADS;
unsigned int arg_key_space = 101 * NBHEADS; /* 1% miss = 99% hit rate */
unsigned int arg_miss_cost = 100;
unsigned int nbthreads = 2;
int arg_nice = 0;
int arg_mode = 0;

/*
 * All we need to manage circular lists
 */
struct list {
	struct list *n;
	struct list *p;
};

#define LIST_HEAD(a)          ((void *)(&(a)))
#define LIST_INIT(l)          ((l)->n = (l)->p = (l))
#define LIST_ADD(lh, el)      ({ (el)->n = (lh)->n; (el)->n->p = (lh)->n = (el); (el)->p = (lh); (el); })
#define LIST_ADDQ(lh, el)     ({ (el)->p = (lh)->p; (el)->p->n = (lh)->p = (el); (el)->n = (lh); (el); })
#define LIST_DEL(el)          ({ typeof(el) __ret = (el); (el)->n->p = (el)->p; (el)->p->n = (el)->n; (__ret); })
#define LIST_ELEM(lh, pt, el) ((pt)(((void *)(lh)) - ((void *)&((pt)NULL)->el)))
#define LIST_ISEMPTY(lh)      ((lh)->n == (lh))
#define LIST_NEXT(lh, pt, el) (LIST_ELEM((lh)->n, pt, el))
#define LIST_PREV(lh, pt, el) (LIST_ELEM((lh)->p, pt, el))
#define LIST_FOR_EACH_ENTRY(item, list_head, member)                     \
        for (item = LIST_ELEM((list_head)->n, typeof(item), member);     \
             &item->member != (list_head);                               \
             item = LIST_ELEM(item->member.n, typeof(item), member))

/*
 * Simple cache management
 */

/* a cache instance */
struct cache_root {
	struct list head[NBHEADS];
	unsigned int used;
};

/* a cache element */
struct cache_item {
	struct list list;
	unsigned int key;
	char str[STRSZ];
};

/* the locks used by the cache */
struct cache_lock {
	unsigned long plock;
	pthread_spinlock_t spinlock;
	pthread_rwlock_t rwlock;
	char pad[0] __attribute__((aligned(64)));
};

struct cache_root cache_root __attribute__((aligned(64)));
struct cache_lock cache_lock __attribute__((aligned(64)));

__thread struct list cache_pool;
__thread unsigned int cache_unused = 0;

/* finds key <k> in the cache and returns the element or NULL if not found. */
static inline struct cache_item *cache_lookup(unsigned int k)
{
	struct cache_item *c;
	unsigned int entry = k % NBHEADS;

	LIST_FOR_EACH_ENTRY(c, &cache_root.head[entry], list)
		if (c->key == k)
			return c;

	return NULL;
}

/* allocates an entry from the local cache */
static inline struct cache_item *cache_alloc()
{
	struct list *l;

	if (!LIST_ISEMPTY(&cache_pool)) {
		l = cache_pool.n;
		LIST_DEL(l);
		cache_unused--;
		return LIST_ELEM(l, struct cache_item *, list);
	}
	return malloc(sizeof(struct cache_item));
}

/* inserts element <c> at the head of the cache */
static inline void cache_insert(struct cache_item *c)
{
	unsigned int entry = c->key % NBHEADS;

	LIST_ADD(&cache_root.head[entry], &c->list);
	cache_root.used++;
}

/* deletes element <c> from the cache and returns it */
static inline struct cache_item *cache_delete(struct cache_item *c)
{
	cache_root.used--;
	LIST_DEL(&c->list);
	return c;
}

/* trims cache until it's not larger than arg_cache_size entries. Excess
 * entries are brought into the thread-local cache. The final number of
 * entries is returned. In order to try to be fair and efficient, we only
 * trim the cache once it's larger than arg_cache_size + NBHEADS, and we
 * round robin over all entries each time.
 */
static inline unsigned int cache_trim()
{
	struct list *l;
	unsigned int entry;

	if (cache_root.used < arg_cache_size + NBHEADS)
		return cache_root.used;

	while (cache_root.used > arg_cache_size) {
		for (entry = 0; entry < NBHEADS; entry++) {
			if (LIST_ISEMPTY(&cache_root.head[entry]))
				continue;

			cache_root.used--;
			l = cache_root.head[entry].p;
			LIST_DEL(l);

			if (cache_unused < arg_cache_size) {
				/* keep up to arg_cache_size local objects */
				LIST_ADD(&cache_pool, l);
				cache_unused++;
			}
			else {
				free(LIST_ELEM(l, struct cache_item *, list));
			}
		}
	}
	return cache_root.used;
}


/*
 * The application stuff
 */

pthread_t thr[MAXTHREADS];
static volatile unsigned long actthreads;
static volatile unsigned long step;
static struct timeval start, stop;
static unsigned long final_work[MAXTHREADS];
static unsigned long final_misses[MAXTHREADS];


/* per-thread states for the randomizer and the local cache pool */
__thread uint32_t rnd32_state = 2463534242U;
__thread unsigned long thread_total_work = 0;
__thread unsigned long thread_misses = 0;

/* Xorshift RNGs from http://www.jstatsoft.org/v08/i14/paper */
static inline uint32_t rnd32()
{
        rnd32_state ^= rnd32_state << 13;
        rnd32_state ^= rnd32_state >> 17;
        rnd32_state ^= rnd32_state << 5;
        return rnd32_state;
}

static inline uint32_t rnd32_range(uint32_t range)
{
        uint64_t res = rnd32();

        res *= range;
        return res >> 32;
}

/* make the "expensive" work */
static void produce_data(unsigned int k, char *str, int size)
{
	unsigned int i = 0;

	do {
		snprintf(str, size, "%u", k);
	} while (i++ < arg_miss_cost);
}

/* consume the produced / retrieved data. Returns < 0 on error. */
static int consume_data(unsigned int k, const char *str)
{
	if ((int)k != atoi(str))
		return -1;
	return 0;
}

/* read: U ; lookup : U ; write : U (reference only, not realistic) */
void loop_mode0(void)
{
	unsigned int k;
	struct cache_item *c;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* lookup */
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			goto done;
		}

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));

		/* now try to store the new data */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));
			cache_insert(c);
			cache_trim();
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* read: pthread_rwlock_spinlock for everything */
void loop_mode1(void)
{
	unsigned int k;
	struct cache_item *c, *tmp;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* lookup */
		pthread_spin_lock(&cache_lock.spinlock);
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			pthread_spin_unlock(&cache_lock.spinlock);
			goto done;
		}
		pthread_spin_unlock(&cache_lock.spinlock);

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));

		/* now try to store the new data. It's possible that the
		 * same key was inserted in the mean time. If so we have
		 * to remove it.
		 */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));
			pthread_spin_lock(&cache_lock.spinlock);
			if ((tmp = cache_lookup(k)))
				cache_delete(tmp);
			cache_insert(c);
			cache_trim();
			pthread_spin_unlock(&cache_lock.spinlock);
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* read: pthread_rwlock_rdlock, delete+insert: pthread_rwlock_wrlock */
void loop_mode2(void)
{
	unsigned int k;
	struct cache_item *c, *tmp;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* first check if the key is present */
		pthread_rwlock_rdlock(&cache_lock.rwlock);
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			pthread_rwlock_unlock(&cache_lock.rwlock);
			goto done;
		}
		pthread_rwlock_unlock(&cache_lock.rwlock);

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));
		/* now try to store the new data. It's possible that the
		 * same key was inserted in the mean time. If so we have
		 * to remove it.
		 */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));

			pthread_rwlock_wrlock(&cache_lock.rwlock);
			if ((tmp = cache_lookup(k)))
				cache_delete(tmp);
			cache_insert(c);
			cache_trim();
			pthread_rwlock_unlock(&cache_lock.rwlock);
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* read+delete+insert: W */
void loop_mode3(void)
{
	unsigned int k;
	struct cache_item *c, *tmp;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* lookup */
		pl_take_w(&cache_lock.plock);
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			pl_drop_w(&cache_lock.plock);
			goto done;
		}
		pl_drop_w(&cache_lock.plock);

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));

		/* now try to store the new data. It's possible that the
		 * same key was inserted in the mean time. If so we have
		 * to remove it.
		 */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));
			pl_take_w(&cache_lock.plock);
			if ((tmp = cache_lookup(k)))
				cache_delete(tmp);
			cache_insert(c);
			cache_trim();
			pl_drop_w(&cache_lock.plock);
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* read+delete+insert: S */
void loop_mode4(void)
{
	unsigned int k;
	struct cache_item *c, *tmp;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* lookup */
		pl_take_s(&cache_lock.plock);
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			pl_drop_s(&cache_lock.plock);
			goto done;
		}
		pl_drop_s(&cache_lock.plock);

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));

		/* now try to store the new data. It's possible that the
		 * same key was inserted in the mean time. If so we have
		 * to remove it.
		 */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));
			pl_take_s(&cache_lock.plock);
			if ((tmp = cache_lookup(k)))
				cache_delete(tmp);
			cache_insert(c);
			cache_trim();
			pl_drop_s(&cache_lock.plock);
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* read: R, delete+insert: W */
void loop_mode5(void)
{
	unsigned int k;
	struct cache_item *c, *tmp;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* lookup */
		pl_take_r(&cache_lock.plock);
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			pl_drop_r(&cache_lock.plock);
			goto done;
		}
		pl_drop_r(&cache_lock.plock);

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));

		/* now try to store the new data. It's possible that the
		 * same key was inserted in the mean time. If so we have
		 * to remove it.
		 */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));
			pl_take_w(&cache_lock.plock);
			if ((tmp = cache_lookup(k)))
				cache_delete(tmp);
			cache_insert(c);
			cache_trim();
			pl_drop_w(&cache_lock.plock);
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* read: R, delete: lookup(S)+delete(W), insert: W */
void loop_mode6(void)
{
	unsigned int k;
	struct cache_item *c, *tmp;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* lookup */
		pl_take_r(&cache_lock.plock);
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			pl_drop_r(&cache_lock.plock);
			goto done;
		}
		pl_drop_r(&cache_lock.plock);

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));

		/* now try to store the new data. It's possible that the
		 * same key was inserted in the mean time. If so we have
		 * to remove it.
		 */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));
			pl_take_s(&cache_lock.plock);
			tmp = cache_lookup(k);
			pl_stow(&cache_lock.plock);
			if (tmp)
				cache_delete(tmp);
			cache_insert(c);
			cache_trim();
			pl_drop_w(&cache_lock.plock);
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* read: R, delete: RTOS|lookup(S)+delete(W), insert: W */
void loop_mode7(void)
{
	unsigned int k;
	struct cache_item *c, *tmp;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* lookup */
		pl_take_r(&cache_lock.plock);
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			pl_drop_r(&cache_lock.plock);
			goto done;
		}
		pl_drop_r(&cache_lock.plock);

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));

		/* now try to store the new data. It's possible that the
		 * same key was inserted in the mean time. If so we have
		 * to remove it.
		 */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));

			pl_take_r(&cache_lock.plock);
			tmp = cache_lookup(k);
			if (!pl_try_rtos(&cache_lock.plock)) {
				/* S or W already claimed, must drop R first */
				pl_drop_r(&cache_lock.plock);
				pl_take_s(&cache_lock.plock);
				tmp = cache_lookup(k);
			}
			/* S lock held here */
			pl_stow(&cache_lock.plock);
			if (tmp)
				cache_delete(tmp);
			cache_insert(c);
			cache_trim();
			pl_drop_w(&cache_lock.plock);
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* read: R, delete: RTOW|lookup(W)+delete(W), insert: W */
void loop_mode8(void)
{
	unsigned int k;
	struct cache_item *c, *tmp;
	char str[STRSZ];

	while (step == 2) {
		k = rnd32_range(arg_key_space);

		/* lookup */
		pl_take_r(&cache_lock.plock);
		if ((c = cache_lookup(k))) {
			/* entry found, let's use it */
			memcpy(str, c->str, sizeof(str));
			pl_drop_r(&cache_lock.plock);
			goto done;
		}
		pl_drop_r(&cache_lock.plock);

		/* miss: produce the expensive data locally */
		thread_misses++;
		produce_data(k, str, sizeof(str));

		/* now try to store the new data. It's possible that the
		 * same key was inserted in the mean time. If so we have
		 * to remove it.
		 */
		if ((c = cache_alloc())) {
			c->key = k;
			memcpy(c->str, str, sizeof(str));

			pl_take_r(&cache_lock.plock);
			tmp = cache_lookup(k);
			if (!pl_try_rtow(&cache_lock.plock)) {
				/* S or W already claimed, must drop R first */
				pl_drop_r(&cache_lock.plock);
				pl_take_w(&cache_lock.plock);
				tmp = cache_lookup(k);
			}
			/* W lock held here */
			if (tmp)
				cache_delete(tmp);
			cache_insert(c);
			cache_trim();
			pl_drop_w(&cache_lock.plock);
		}
	done:
		if (consume_data(k, str) < 0)
			exit(1);
		thread_total_work++;
	}
}

/* main thread preparation */
void oneatwork(int thr)
{
	LIST_INIT(&cache_pool);
	rnd32_state += thr;

	/* step 0: creating all threads */
	while (step == 0) {
		/* don't disturb pthread_create() */
		usleep(10000);
	}

	/* step 1 : waiting for signal to start */
	pl_inc_noret(&actthreads);
	while (step == 1);

	/* step 2 : running */
	switch(arg_mode) {
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
		gettimeofday(&stop, NULL);
	}

	final_work[thr] = thread_total_work;
	final_misses[thr] = thread_misses;
	pl_dec_noret(&actthreads);
	//fprintf(stderr, "actthreads=%d\n", actthreads);
	pthread_exit(0);
}

void usage(int ret)
{
	printf("usage: lrubench [-h] [-n nice] [-t threads] [-s size] [-k key_space] [-c miss_cost] [-m mode]\n"
	       "Modes :\n"
	       "  0 : no lock (only with -t 1)\n"
	       "  1 : pthread spinlock lock for everything\n"
	       "  2 : pthread rwlock : R lock for lookup, W for insertion\n"
	       "  3 : plock W lock for everything\n"
	       "  4 : plock S lock for everything\n"
	       "  5 : plock R lock for lookup, W for insertion\n"
	       "  6 : plock R lock for lookup, S->W for insertion\n"
	       "  7 : plock R lock for lookup, R->S->W for insertion\n"
	       "  8 : plock R lock for lookup, R->W for insertion\n"
	       "\n");
	exit(ret);
}

int main(int argc, char **argv)
{
	int i, err;
	unsigned long u;
	unsigned long total, misses;

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
			arg_mode = atol(*++argv);
		}
		else if (!strcmp(*argv, "-s")) {
			if (--argc < 0)
				usage(1);
			arg_cache_size = atol(*++argv);
		}
		else if (!strcmp(*argv, "-k")) {
			if (--argc < 0)
				usage(1);
			arg_key_space = atol(*++argv);
		}
		else if (!strcmp(*argv, "-c")) {
			if (--argc < 0)
				usage(1);
			arg_miss_cost = atol(*++argv);
		}
		else if (!strcmp(*argv, "-h"))
			usage(0);
		else
			usage(1);
		argc--; argv++;
	}

	if (nbthreads >= MAXTHREADS)
		nbthreads = MAXTHREADS;

	if (!arg_mode && nbthreads > 1) {
		fprintf(stderr, "Can't use unlocked mode in multi-threading mode.\n");
		usage(1);
	}

	nice(arg_nice);

	actthreads = 0;	step = 0;

	setbuf(stdout, NULL);

	cache_lock.plock = 0;
	pthread_spin_init(&cache_lock.spinlock, PTHREAD_PROCESS_PRIVATE);
	pthread_rwlock_init(&cache_lock.rwlock, NULL);

	cache_root.used = 0;
	for (u = 0; u < NBHEADS; u++) {
		LIST_INIT(&cache_root.head[u]);
	}

	for (u = 0; u < nbthreads; u++) {
		if ((err = pthread_create(&thr[u], NULL, (void *)&oneatwork, (void *)u)) != 0) {
			perror("");
			exit(1);
		}
		pthread_detach(thr[u]);
	}

	pl_inc_noret(&step);  /* let the threads warm up and get ready to start */

	while (actthreads != nbthreads);

	/* let CPUs burn at 100% to stabilize cpufreq */
	usleep(200000);

	gettimeofday(&start, NULL);
	pl_inc_noret(&step); /* fire ! */

	sleep(2);
	pl_inc_noret(&step);
	gettimeofday(&stop, NULL);

	while (actthreads)
		usleep(100000);

	/* All the work has ended */

	i = (stop.tv_usec - start.tv_usec);
	while (i < 0) {
		i += 1000000;
		start.tv_sec++;
	}
	u = i / 1000U + (int)(stop.tv_sec - start.tv_sec) * 1000U;

	total = misses = 0;
	for (i = 0; i < (int)nbthreads; i++) {
		total += final_work[i];
		misses += final_misses[i];
		printf("thread: %2d loops: %11lu time(ms): %lu rate(lps): %11Lu, access(ns): %3lu misses=%lu\n",
		       i, final_work[i], u, final_work[i] * 1000ULL / u, u * 1000000UL / final_work[i], final_misses[i]);
	}
	printf("Global:    loops: %11lu time(ms): %lu rate(lps): %11Lu, access(ns): %3lu, misses=%lu\n",
	       total, u, total * 1000ULL / u, u * 1000000UL / total, misses);

	exit(0);
}
