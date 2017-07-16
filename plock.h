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
 * and then waits for all other participants to leave before writing.
 *
 * Principles of operations
 * ------------------------
 *
 * Locks have 4 states :
 *
 *   - UL: unlocked      : nobody claims the lock
 *   - RD: read-locked   : some users are reading the shared resource
 *   - FR: freezing read : reading is OK but nobody else may freeze nor write
 *   - WR: write-locked  : exclusive access for writing
 *
 * The locks are implemented using cumulable bit fields representing from
 * the lowest to the highest bits :
 *
 *   - the number of readers (read, freeze)
 *   - the number of freeze requests
 *   - the number of write requests
 *
 * The number of freeze requests remains on a low bit count and this number
 * is placed just below the write bit count so that if it overflows, it
 * temporarily overflows into the write bits and appears as requesting an
 * exclusive write access. This allows the number of freeze bits to remain
 * very low, 1 technically, but 2 to avoid needless lock/unlock sequences
 * during common conflicts.
 *
 * A freeze request also counts as a read request as technically it's a reader
 * which plans to write later.
 *
 * The FR lock cannot be taken if another FR or WR lock is already held. But
 * once the FR lock is held, the owner is automatically granted the right to
 * upgrade it to WR without checking. And it can take and release the WR lock
 * multiple times atomically if needed. It must only wait for last readers to
 * leave. This means that another thread may very well take the WR lock and
 * wait for the FR and RD locks to leave (in practice checking for RD is
 * enough for a writer).
 *
 * The lock can be upgraded between various states at the demand of the
 * requester :
 *
 *   - UL<->RD : take_rd() / drop_rd()   (adds/subs RD)
 *   - UL<->FR : take_fr() / drop_fr()   (adds/subs FR+RD)
 *   - UL<->WR : take_wx() / drop_wx()   (adds/subs WR)
 *   - FR<->WR : take_wr() / drop_wr()   (adds/subs WR-FR-RD)
 *
 * With the two lowest bits remaining reserved for other usages (eg: ebtrees),
 * we can have this split :
 *
 * - on 32-bit architectures :
 *   - 31..18 : 14 bits for writers
 *   - 17..16 : 2  bits for freezers
 *   - 16..2  : 14 bits for users
 *   => up to 16383 users (readers or writers)
 *
 * - on 64-bit architectures :
 *   - 63..34 : 30 bits for writers
 *   - 33..32 : 2  bits for freezers
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
#define PLOCK_FL_1   0x0000000100000000
#define PLOCK_FL_ANY 0x0000000300000000
#define PLOCK_WL_1   0x0000000400000000
#define PLOCK_WL_ANY 0xFFFFFFFC00000000
#elif (__SIZEOF_LONG__ == 4) || (__LONG_MAX__ == 2147483647L)
/* 32 bit */
#define PLOCK_RL_1   0x00000004
#define PLOCK_RL_ANY 0x0000FFFC
#define PLOCK_FL_1   0x00010000
#define PLOCK_FL_ANY 0x00030000
#define PLOCK_WL_1   0x00040000
#define PLOCK_WL_ANY 0xFFFC0000
#else
#error "Unknown machine word size"
#endif


/* request shared read access */
static inline void pl_take_rd(volatile unsigned long *lock)
{
	while (__builtin_expect(pl_xadd(lock, PLOCK_RL_1) &
	                        PLOCK_WL_ANY, 0)) {
		pl_sub(lock, PLOCK_RL_1);
		while (*lock & PLOCK_WL_ANY);
	}
}

static inline void pl_drop_rd(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_RL_1);
}

/* request a freezing read access (shared for reads only) */
static inline void pl_take_fr(volatile unsigned long *lock)
{
	while (__builtin_expect(pl_xadd(lock, PLOCK_FL_1 | PLOCK_RL_1) &
	                        (PLOCK_WL_ANY | PLOCK_FL_ANY), 0)) {
		pl_sub(lock, PLOCK_FL_1 | PLOCK_RL_1);
		do {
			pl_cpu_relax_long(4);
		} while (*lock & (PLOCK_WL_ANY | PLOCK_FL_ANY));
	}
}

static inline void pl_drop_fr(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_FL_1 + PLOCK_RL_1);
}

/* take the WR lock under the FR lock */
static inline void pl_take_wr(volatile unsigned long *lock)
{
	unsigned long r;

	r = pl_xadd(lock, PLOCK_WL_1 - PLOCK_FL_1 - PLOCK_RL_1);
	r -= PLOCK_RL_1; // subtract our own count
	while (r & PLOCK_RL_ANY)
		r = *lock;
}

/* drop the WR lock and go back to the FR lock */
static inline void pl_drop_wr(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1 - PLOCK_FL_1 - PLOCK_RL_1);
}

/* immediately take the WR lock from UL and wait for readers to leave. */
static inline void pl_take_wx(volatile unsigned long *lock)
{
	unsigned long r;

	while (__builtin_expect((r = pl_xadd(lock, PLOCK_WL_1)) &
	                        PLOCK_WL_ANY, 0)) {
		pl_sub(lock, PLOCK_WL_1);
		pl_cpu_relax_long(5);
	}

	/* wait for readers to leave, that also covers freezing ones */
	while (r & PLOCK_RL_ANY)
		r = *lock;
}

/* drop the WR lock entirely */
static inline void pl_drop_wx(volatile unsigned long *lock)
{
	pl_sub(lock, PLOCK_WL_1);
}
