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
 *   - W: write-locked  : exclusive access for writing (direct or after S)
 *   - A: atomic        : some atomic updates are being performed
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
 * for last readers to leave. The A lock supports concurrent write accesses
 * and is used when certain atomic operations can be performed on a structure
 * which also supports non-atomic operations. It is exclusive with the other
 * locks. It is weaker than the S/W locks (S being a promise of upgradability),
 * and will wait for any readers to leave before proceeding.
 *
 * In terms of representation, we have this :
 *   - R lock is made of the R bit
 *   - S lock is made of S + R bits
 *   - W lock is made of W + S + R bits
 *   - A lock is made of W bits only
 *
 * The lock can be upgraded between various states at the demand of the
 * requester :
 *
 *   - U<->A : pl_take_a() / pl_drop_a()   (adds/subs W)
 *   - U<->R : pl_take_r() / pl_drop_r()   (adds/subs R)
 *   - U<->S : pl_take_s() / pl_drop_s()   (adds/subs S+R)
 *   - U<->W : pl_take_w() / pl_drop_w()   (adds/subs W+S+R)
 *   - S<->W : pl_stow()   / pl_wtos()     (adds/subs W)
 *
 * Other transitions are permitted in opportunistic mode, such as R to A.
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
static inline unsigned long pl_try_r(volatile unsigned long *lock)
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
static inline void pl_take_r(volatile unsigned long *lock)
{
	while (!pl_try_r(lock));
}

/* release the read access (R) lock */
static inline void pl_drop_r(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_RL_1);
}

/* request a seek access (S), return non-zero on success, otherwise 0 */
static inline unsigned long pl_try_s(volatile unsigned long *lock)
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
static inline void pl_take_s(volatile unsigned long *lock)
{
	while (!pl_try_s(lock));
}

/* release the seek access (S) lock */
static inline void pl_drop_s(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_SL_1 + PLOCK_RL_1);
}


/* take the W lock under the S lock */
static inline void pl_stow(volatile unsigned long *lock)
{
	unsigned long r;

	r = pl_xadd(lock, PLOCK_WL_1);
	while ((r & PLOCK_RL_ANY) != PLOCK_RL_1)
		r = *lock;
}

/* drop the W lock and go back to the S lock */
static inline void pl_wtos(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1);
}

/* request a write access (W), return non-zero on success, otherwise 0 */
static inline unsigned long pl_try_w(volatile unsigned long *lock)
{
	unsigned long ret;

	ret = *lock;
	if (__builtin_expect(ret & (PLOCK_WL_ANY | PLOCK_SL_ANY), 0))
		return !ret;

	/* Below there is something important : by taking both W and S, we will
	 * cause an overflow of W at 4/5 of the maximum value that can be stored
	 * into W due to the fact that S is 2 bits, so we're effectively adding
	 * 5 to the word composed by W:S. But for all words multiple of 4 bits,
	 * the maximum value is multiple of 15 thus of 5. So the largest value
	 * we can store with all bits set to one will be met by adding 5, and
	 * then adding 5 again will place value 1 in W and value 0 in S, so we
	 * never leave W with 0. Also, even upon such an overflow, there's no
	 * risk to confuse it with an atomic lock because R is not null since
	 * it will not have overflown. For 32-bit locks, this situation happens
	 * when exactly 13108 threads try to grab the lock at once, W=1, S=0 and
	 * R=13108. For 64-bit locks, it happens at 858993460 concurrent writers
	 * where W=1, S=0 and R=858993460.
	 */
	ret = pl_xadd(lock, PLOCK_WL_1 | PLOCK_SL_1 | PLOCK_RL_1);

	/* a writer, seeker or atomic is present, let's leave */
	if (__builtin_expect(ret & (PLOCK_WL_ANY | PLOCK_SL_ANY), 0)) {
		pl_sub(lock, PLOCK_WL_1 | PLOCK_SL_1 | PLOCK_RL_1);
		return !(ret & (PLOCK_WL_ANY | PLOCK_SL_ANY)); // always 0
	}

	/* wait for all other readers to leave */
	while (ret)
		ret = *lock - (PLOCK_WL_1 | PLOCK_SL_1 | PLOCK_RL_1);

	/* always return true, this one is cheaper to return than 1 because
	 * the compiler has placed it in a register for other operations above,
	 * and thus saves one specific exit path.
	 */
	return PLOCK_WL_1 | PLOCK_SL_1 | PLOCK_RL_1;
}

/* request a seek access (W) and wait for it */
static inline void pl_take_w(volatile unsigned long *lock)
{
	while (__builtin_expect(pl_try_w(lock), 1) == 0)
		pl_cpu_relax();
}

/* drop the write (W) lock entirely */
static inline void pl_drop_w(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1 | PLOCK_SL_1 | PLOCK_RL_1);
}

/* Try to upgrade from R to S, return non-zero on success, otherwise 0.
 * This lock will fail if S or W are already held. In case of failure to grab
 * the lock, it MUST NOT be retried without first dropping R, or it may never
 * complete due to S waiting for R to leave before upgrading to W.
 */
static inline unsigned long pl_try_rtos(volatile unsigned long *lock)
{
	unsigned long ret;

	ret = *lock;
	if (__builtin_expect(ret & (PLOCK_WL_ANY | PLOCK_SL_ANY), 0))
		return !ret;

	ret = pl_xadd(lock, PLOCK_SL_1) & (PLOCK_WL_ANY | PLOCK_SL_ANY);

	if (__builtin_expect(ret, 0))
		pl_sub(lock, PLOCK_SL_1);

	return !ret;
}

/* request atomic write access (A), return non-zero on success, otherwise 0 */
static inline unsigned long pl_try_a(volatile unsigned long *lock)
{
	unsigned long ret;

	ret = *lock & PLOCK_SL_ANY;
	if (__builtin_expect(ret, 0))
		return !ret;

	ret = pl_xadd(lock, PLOCK_WL_1);

	while (1) {
		/* we have to give up if an S lock appears. It's possible that
		 * such a lock stayed hidden in the W bits after an overflow,
		 * but in this case R is still held, ensuring we loop back here
		 * until we discover the conflict. We only return successfully
		 * once all readers are gone (or converted to A).
		 */
		if (__builtin_expect(ret & PLOCK_SL_ANY, 0)) {
			pl_sub(lock, PLOCK_WL_1);
			return !ret;
		}

		ret &= PLOCK_RL_ANY;
		if (__builtin_expect(ret, 0) == 0)
			break;
		ret = *lock;
	}

	return !ret;
}

/* request atomic write access (A) and wait for it */
static inline void pl_take_a(volatile unsigned long *lock)
{
	while (!pl_try_a(lock));
}

/* release atomc write access (A) lock */
static inline void pl_drop_a(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1);
}

/* Try to upgrade from R to A, return non-zero on success, otherwise 0.
 * This lock will fail if S is held or appears while waiting (typically due to
 * a previous grab that was disguised as a W due to an overflow). In case of
 * failure to grab the lock, it MUST NOT be retried without first dropping R,
 * or it may never complete due to S waiting for R to leave before upgrading
 * to W. The lock succeeds once there's no more R (ie all of them have either
 * completed or were turned to A).
 */
static inline unsigned long pl_try_rtoa(volatile unsigned long *lock)
{
	unsigned long ret;

	ret = *lock & PLOCK_SL_ANY;
	if (__builtin_expect(ret, 0))
		return !ret;

	ret = pl_xadd(lock, PLOCK_WL_1 - PLOCK_RL_1);

	while (1) {
		if (__builtin_expect(ret & PLOCK_SL_ANY, 0)) {
			pl_sub(lock, PLOCK_WL_1 - PLOCK_RL_1);
			return !ret;
		}

		ret &= PLOCK_RL_ANY;
		if (__builtin_expect(ret, 0) == 0)
			break;
		ret = *lock;
	}

	return !ret;
}
