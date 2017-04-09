/* plock - progressive locks
 *
 * Copyright (C) 2012-2017 Willy Tarreau <w@1wt.eu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Overview
 * --------
 *
 * Locking in a tree can create an important loss of performance during writes
 * if the write lock is held during the tree descent. And if the lock is not
 * held during the descent, then each node has to be locked so that a reader
 * approching the area being modified doesn't walk out of the tree. The idea
 * behind the progressive locks is to have an extra locked state to freeze the
 * structure of the tree so that only one actor may decide to switch to writes,
 * and then waits for all other participants to leave before writing. We call
 * this state "seek" as it is used to find the proper place to insert or remove
 * elements.
 *
 * Principles of operations
 * ------------------------
 *
 * Locks have 4 states :
 *
 *   - UL: unlocked      : nobody claims the lock
 *   - RD: read-locked   : some users are reading the shared resource
 *   - SK: seek-locked   : reading is OK but nobody else may seek nor write
 *   - WR: write-locked  : exclusive access for writing
 *
 * The locks are implemented using cumulable bit fields representing from
 * the lowest to the highest bits :
 *
 *   - the number of readers (read, seek, write)
 *   - the number of seek requests
 *   - the number of write requests
 *
 * The number of seek requests remains on a low bit count and this number
 * is placed just below the write bit count so that if it overflows, it
 * temporarily overflows into the write bits and appears as requesting an
 * exclusive write access. This allows the number of seek bits to remain
 * very low, 1 technically, but 2 to avoid needless lock/unlock sequences
 * during common conflicts.
 *
 * A seek request also counts as a read request as technically it's a reader
 * which plans to write later.
 *
 * The SK lock cannot be taken if another SK or WR lock is already held. But
 * once the SK lock is held, the owner is automatically granted the right to
 * upgrade it to WR without checking. And it can take and release the WR lock
 * multiple times atomically if needed. It must only wait for last readers to
 * leave.
 *
 * The lock can be upgraded between various states at the demand of the
 * requester :
 *
 *   - UL<->RD : take_r() / drop_r()   (adds/subs RD)
 *   - UL<->SK : take_s() / drop_s()   (adds/subs SK+RD)
 *   - UL<->WR : take_w() / drop_w()   (adds/subs WR+RD)
 *   - SK<->WR : stow()   / wtos()     (adds/subs WR-SK)
 *
 * With the two lowest bits remaining reserved for other usages (eg: ebtrees),
 * we can have this split :
 *
 * - on 32-bit architectures :
 *   - 31..18 : 14 bits for writers
 *   - 17..16 : 2  bits for seekers
 *   - 16..2  : 14 bits for users
 *   => up to 16383 users (readers or writers)
 *
 * - on 64-bit architectures :
 *   - 63..34 : 30 bits for writers
 *   - 33..32 : 2  bits for seekers
 *   - 31..2  : 30 bits for users
 *   => up to ~1.07B users (readers or writers)
 */

#include "atomic-ops.h"

/* Portable way to detect most 64-bit archs, as gcc < 4 doesn't provide
 * __SIZEOF_LONG__ but provides __LONG_MAX__.
 */
#if (__SIZEOF_LONG__ >= 8) || (__LONG_MAX__ > 2147483647L)
/* 64 bit */
#define PLOCK_RL_1   0x0000000000000004
#define PLOCK_RL_ANY 0x00000000FFFFFFFC
#define PLOCK_SL_1   0x0000000100000000
#define PLOCK_SL_ANY 0x0000000300000000
#define PLOCK_WL_1   0x0000000400000000
#define PLOCK_WL_ANY 0xFFFFFFFC00000000
#elif (__SIZEOF_LONG__ == 4) || (__LONG_MAX__ == 2147483647L)
/* 32 bit */
#define PLOCK_RL_1   0x00000004
#define PLOCK_RL_ANY 0x0000FFFC
#define PLOCK_SL_1   0x00010000
#define PLOCK_SL_ANY 0x00030000
#define PLOCK_WL_1   0x00040000
#define PLOCK_WL_ANY 0xFFFC0000
#else
#error "Unknown machine word size"
#endif


/* request shared read access and wait for it */
static inline void pl_take_r(volatile unsigned long *lock)
{
	while (__builtin_expect(pl_xadd(lock, PLOCK_RL_1) &
	                        PLOCK_WL_ANY, 0)) {
		pl_sub(lock, PLOCK_RL_1);
		while (*lock & PLOCK_WL_ANY);
	}
}

/* request shared read access, return non-zero on success, otherwise 0 */
static inline long pl_try_r(volatile unsigned long *lock)
{
	unsigned long ret;

	ret = pl_xadd(lock, PLOCK_RL_1) & PLOCK_WL_ANY;
	if (ret)
		pl_sub(lock, PLOCK_RL_1);
	return !ret;
}

static inline void pl_drop_r(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_RL_1);
}

/* request a seek access and wait for it */
static inline void pl_take_s(volatile unsigned long *lock)
{
	while (__builtin_expect(pl_xadd(lock, PLOCK_SL_1 | PLOCK_RL_1) &
	                        (PLOCK_WL_ANY | PLOCK_SL_ANY), 0)) {
		pl_sub(lock, PLOCK_SL_1 | PLOCK_RL_1);
		do {
			pl_cpu_relax_long(4);
		} while (*lock & (PLOCK_WL_ANY | PLOCK_SL_ANY));
	}
}

/* request a seek access, return non-zero on success, otherwise 0 */
static inline unsigned long pl_try_s(volatile unsigned long *lock)
{
	unsigned long ret;

	ret  = pl_xadd(lock, PLOCK_SL_1 | PLOCK_RL_1);
	ret &= (PLOCK_WL_ANY | PLOCK_SL_ANY);
	if (ret)
		pl_sub(lock, PLOCK_SL_1 | PLOCK_RL_1);
	return !ret;
}

static inline void pl_drop_s(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_SL_1 + PLOCK_RL_1);
}

/* take the WR lock under the SK lock */
static inline void pl_stow(volatile unsigned long *lock)
{
	unsigned long r;

	r = pl_xadd(lock, PLOCK_WL_1 - PLOCK_SL_1);
	while ((r & PLOCK_RL_ANY) != PLOCK_RL_1)
		r = *lock;
}

/* drop the WR lock and go back to the SK lock */
static inline void pl_wtos(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1 - PLOCK_SL_1);
}

/* immediately take the WR lock from UL and wait for readers to leave. */
static inline void pl_take_w(volatile unsigned long *lock)
{
	unsigned long r;

	while (__builtin_expect((r = pl_xadd(lock, PLOCK_WL_1 | PLOCK_RL_1)) &
	                        PLOCK_WL_ANY, 0)) {
		pl_sub(lock, PLOCK_WL_1 | PLOCK_RL_1);
		pl_cpu_relax_long(5);
	}

	/* wait for readers to leave, that also covers seekers */
	r += PLOCK_RL_1; // count our own presence
	while ((r & PLOCK_RL_ANY) != PLOCK_RL_1)
		r = *lock;
}

/* try to grab the WR lock from UL then wait for readers to leave.
 * returns non-zero on success otherwise zero.
 */
static inline unsigned long pl_try_w(volatile unsigned long *lock)
{
	unsigned long r;

	r = pl_xadd(lock, PLOCK_WL_1 | PLOCK_RL_1);
	if (r & PLOCK_WL_ANY) {
		pl_sub(lock, PLOCK_WL_1 | PLOCK_RL_1);
		return 0;
	}

	/* wait for readers to leave, that also covers seekers */
	r += PLOCK_RL_1; // count our own presence
	while ((r & PLOCK_RL_ANY) != PLOCK_RL_1)
		r = *lock;
	return r;
}

/* drop the WR lock entirely */
static inline void pl_drop_w(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1 | PLOCK_RL_1);
}
