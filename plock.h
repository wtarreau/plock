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
 * Locks have 5 states :
 *
 *   - U: unlocked      : nobody claims the lock
 *   - R: read-locked   : some users are reading the shared resource
 *   - S: seek-locked   : reading is OK but nobody else may seek nor write
 *   - W: write-locked  : exclusive access for writing after S
 *   - X: exclusive     : immediate exclusive access
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
 * The S lock cannot be taken if another S or W lock is already held. But once
 * once the S lock is held, the owner is automatically granted the right to
 * upgrade it to W without checking for other writers. And it can take and
 * release the W lock multiple times atomically if needed. It must only wait
 * for last readers to leave. The X lock is a combination of R and W locks
 * that can immediately be granted from the unlocked state. It differs from
 * the W lock in that it will be weaker than atomic locks.
 *
 * In terms of representation, we have this :
 *   - R lock is made of the R bit
 *   - S lock is made of R + S bits
 *   - W lock is made of R + W + S bits
 *   - X lock is made of R + W bits
 *
 * The lock can be upgraded between various states at the demand of the
 * requester :
 *
 *   - U<->R : pl_take_r() / pl_drop_r()   (adds/subs R)
 *   - U<->S : pl_take_s() / pl_drop_s()   (adds/subs S+R)
 *   - U<->X : pl_take_x() / pl_drop_x()   (adds/subs W+R)
 *   - S<->W : pl_stow()   / pl_wtos()     (adds/subs W)
 *
 * Other transitions are permitted in opportunistic mode.
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


/* request shared read access (R), return non-zero on success, otherwise 0 */
static unsigned long pl_try_r(volatile unsigned long *lock)
{
	unsigned long ret;

	ret = *lock & PLOCK_WL_ANY;
	if (__builtin_expect(ret, 0))
		return !ret;

	ret = pl_xadd(lock, PLOCK_RL_1) & PLOCK_WL_ANY;
	if (__builtin_expect(ret, 0))
		pl_sub(lock, PLOCK_RL_1);

	return !ret;
}

/* request shared read access (R) and wait for it */
static void pl_take_r(volatile unsigned long *lock)
{
	while (!pl_try_r(lock));
}

/* release the read access (R) lock */
static void pl_drop_r(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_RL_1);
}

/* request a seek access (S), return non-zero on success, otherwise 0 */
static unsigned long pl_try_s(volatile unsigned long *lock)
{
	unsigned long ret;

	ret = *lock;
	if (__builtin_expect(ret & (PLOCK_WL_ANY | PLOCK_SL_ANY), 0))
		return !ret;

	ret = pl_xadd(lock, PLOCK_SL_1 | PLOCK_RL_1) & (PLOCK_WL_ANY | PLOCK_SL_ANY);

	if (__builtin_expect(ret, 0))
		pl_sub(lock, PLOCK_SL_1 | PLOCK_RL_1);
	return !ret;
}

/* request a seek access (S) and wait for it */
static void pl_take_s(volatile unsigned long *lock)
{
	while (!pl_try_s(lock));
}

/* release the seek access (S) lock */
static void pl_drop_s(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_SL_1 + PLOCK_RL_1);
}


/* take the W lock under the S lock */
static void pl_stow(volatile unsigned long *lock)
{
	unsigned long r;

	r = pl_xadd(lock, PLOCK_WL_1);
	while ((r & PLOCK_RL_ANY) != PLOCK_RL_1)
		r = *lock;
}

/* drop the W lock and go back to the S lock */
static void pl_wtos(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1);
}

/* drop the write (W) lock entirely */
static void pl_drop_w(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1 | PLOCK_SL_1 | PLOCK_RL_1);
}

/* try to grab the exclusive (X) lock from U then wait for readers to leave.
 * returns non-zero on success otherwise zero.
 */
static unsigned long pl_try_x(volatile unsigned long *lock)
{
	unsigned long r;

	r = *lock;
	if (__builtin_expect(r & (PLOCK_WL_ANY | PLOCK_SL_ANY), 0))
		return !r;

	r = pl_xadd(lock, PLOCK_WL_1 | PLOCK_RL_1);

	if (__builtin_expect(r & (PLOCK_WL_ANY | PLOCK_SL_ANY), 0)) {
		pl_sub(lock, PLOCK_WL_1 | PLOCK_RL_1);
		return !r;
	}

	/* wait for readers to leave, that also covers seekers */
	while (__builtin_expect(r &= PLOCK_RL_ANY, 0))
		r = *lock - PLOCK_RL_1;

	return !r;
}

/* immediately take the exclusive (X) lock from U and wait for readers to
 * leave.
 */
static void pl_take_x(volatile unsigned long *lock)
{
	while (!pl_try_x(lock));
}

/* drop the exclusive (X) lock entirely */
static void pl_drop_x(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1 | PLOCK_RL_1);
}
