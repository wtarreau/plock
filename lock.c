#include "atomic-ops.h"

/*
 * Progressive locks - principles of operations
 *
 * Locks have 4 states :
 *
 *   - UL: unlocked     : nobody claims the lock
 *   - RD: read-locked  : some users are reading the shared resource
 *   - FR: frozen       : reading is OK but nobody else may freeze nor write
 *   - WR: write-locked : exclusive access for writing
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

#if defined(__i386__) || defined (__i486__) || defined (__i586__) || defined (__i686__)

#define RL_1   0x00000004
#define RL_ANY 0x0000FFFC
#define FL_1   0x00010000
#define FL_ANY 0x00030000
#define WL_1   0x00040000
#define WL_ANY 0xFFFC0000

#elif defined(__x86_64__)

#define RL_1   0x0000000000000004
#define RL_ANY 0x00000000FFFFFFFC
#define FL_1   0x0000000100000000
#define FL_ANY 0x0000000300000000
#define WL_1   0x0000000400000000
#define WL_ANY 0xFFFFFFFC00000000

#endif

/* request shared read access */
static inline
void take_rd(volatile unsigned long *lock)
{
	if (__builtin_expect(xadd(lock, RL_1) & WL_ANY, 0)) {
		do {
			atomic_sub(lock, RL_1);
			while (*lock & WL_ANY);
		} while (xadd(lock, RL_1) & WL_ANY);
	}
}

static inline
void drop_rd(volatile unsigned long *lock)
{
	atomic_sub(lock, RL_1);
}

/* request a frozen read access (shared for reads only) */
static inline
void take_fr(volatile unsigned long *lock)
{
	if (__builtin_expect(xadd(lock, FL_1 | RL_1) & (WL_ANY | FL_ANY), 0)) {
		do {
			atomic_sub(lock, FL_1 | RL_1);
			do {
				cpu_relax_long(4);
			} while (*lock & (WL_ANY | FL_ANY));
		} while (xadd(lock, FL_1 | RL_1) & (WL_ANY | FL_ANY));
	}
}

static inline
void drop_fr(volatile unsigned long *lock)
{
	atomic_sub(lock, FL_1 | RL_1);
}

/* take the WR lock under the FR lock */
static inline
void take_wr(volatile unsigned long *lock)
{
	unsigned long r;

	r = xadd(lock, WL_1 - FL_1 - RL_1);
	r -= RL_1; // subtract our own count
	while (r & RL_ANY)
		r = *lock;
}

/* drop the WR lock and go back to the FR lock */
static inline
void drop_wr(volatile unsigned long *lock)
{
	atomic_sub(lock, WL_1 - FL_1 - RL_1);
}

/* immediately take the WR lock from UL and wait for readers to leave */
static inline
void take_wx(volatile unsigned long *lock)
{
	unsigned long r;

	while (__builtin_expect((r = xadd(lock, WL_1)) & WL_ANY, 0)) {
		atomic_sub(lock, WL_1);
		cpu_relax_long(5);
	}

	/* wait for readers to go */
	while (r & RL_ANY)
		r = *lock;
}

/* drop the WR lock entirely */
static inline
void drop_wx(volatile unsigned long *lock)
{
	atomic_sub(lock, WL_1);
}

static inline
void ro_lock(volatile unsigned long *lock)
{
	return take_rd(lock);
}

static inline
void mw_lock(volatile unsigned long *lock)
{
	return take_fr(lock);
}

static inline
void wr_lock(volatile unsigned long *lock)
{
	return take_wr(lock);
}

/* immediately take the WR lock */
static inline
void wr_fast_lock(volatile unsigned long *lock)
{
	return take_wx(lock);
}

static inline
void ro_unlock(volatile unsigned long *lock)
{
	drop_rd(lock);
}

/* goes back to unlock state from exclusive write */
static inline
void wr_unlock(unsigned long *lock)
{
	drop_wx(lock);
}
