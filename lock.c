#include <stdio.h>

#include <linux/futex.h>
#include <sys/syscall.h>

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

//#define USE_FUTEX
#define USE_EXP1
#if defined(USE_EXP1)

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
	unsigned long j;

	if (__builtin_expect((r = xadd(lock, WL_1)) & WL_ANY, 0)) {
		/* wait for other writers to leave */
		unsigned long j = 8;
		do {
			int must_unlock = *lock >= 2*WL_1;

			if (must_unlock)
				atomic_sub(lock, WL_1);
			cpu_relax_long(j);
			j = j << 1;
			if (must_unlock)
				xadd(lock, WL_1);
		} while ((r = *lock) >= 2*WL_1);
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

#elif defined(USE_EXP)

/* 3 modes of operation :
 *   - shared read     (SR) : all such threads may access the data at the same
 *                             time.
 *   - exclusive read  (XR) : only one such thread is allowed to pass, access
 *                            is still shared with shared readers.
 *   - exclusive write (XW) : only one such thread has access at all.
 *
 * bits:
 *   - 31: XW
 *   - 30..16: modifiers (up to 32k modifiers), threads waiting for XR
 *   - 15..0: users (up to 64k users), either readers or writers
 *
 * An exclusive writer holds only XW and neither XR nor SR.
 * An exclusive reader holds both XR and SR, or only XR when waiting for XW to
 * be release. A shared reader only holds SR.
 */

#define XW     0x80000000
#define XR_1   0x00010000
#define XR_ANY 0x7FFF0000
#define SR_1   0x00000001
#define SR_ANY 0x0000FFFF

/* provide SR access */
void ro_lock(volatile unsigned long *lock)
{
	long j = 5;
	while (1) {
		/* try to get a shared read access, retry if the XW is there */
		if (!(xadd(lock, SR_1) & XW))
			break;
		atomic_sub(lock, SR_1);
		do {
			long i;
			for (i = 0; i < j; i++) {
				cpu_relax();
				cpu_relax();
			}
			j = j << 1;
		} while (*lock & XW);
	}
}

void mw_lock0(volatile unsigned long *lock)
{
	long i, j = 5;

	while (1) {
		i = xadd(lock, XR_1 + SR_1);
		if (!(i & (XR_ANY | XW)))
			return;

		if (!(i & XR_ANY))
			break;

		/* Wait for XR to be released first. We need to release SR too
		 * so that the future writer makes progress.
		 */
		i = xadd(lock, - (XR_1 + SR_1)) - XR_1;
		while (i & (XR_ANY | XW)) {
			for (i = 0; i < j; i++) {
				cpu_relax();
				cpu_relax();
			}
			j = j << 1;
			i = *lock;
		}
		/* OK, lock was released */
	}

	/* only the XW lock was held, so we keep our XR lock ;-) */
	do {
		atomic_sub(lock, SR_1); /* let the writer make progress */
		do {
			for (i = 0; i < j; i++) {
				cpu_relax();
				cpu_relax();
			}
			j = j << 1;
		} while (*lock & XW);
		i = xadd(lock, SR_1);
	} while (i & XW);
}

void mw_lock(volatile unsigned long *lock)
{
	unsigned long i, j = 3;
	unsigned long need_xr = XR_1;

	while (1) {
		i = xadd(lock, need_xr + SR_1);
		/* We want to take the XR bit first. Once we have it, we'll
		 * wait for XW to be clear.
		 */
		if (need_xr > (i & XR_ANY))
			need_xr = 0; /* Got the XR bit ! */

		if (!(need_xr | (i & XW)))
			return;

		/* We need to release SR so that the future writer makes
		 * progress and finally releases XW.
		 */
		i = xadd(lock, - (need_xr + SR_1)) - need_xr;
		while ((i & XW) || (i & -need_xr)) {
			for (i = 0; i < j; i++) {
				cpu_relax();
				cpu_relax();
			}
			j = j << 1;
			i = *lock;
		}
	}
}

void wr_lock(volatile unsigned long *lock)
{
	unsigned long r;
	long j;

	/* Note: we already hold the XR lock, we don't care about other
	 * XR waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */
	/* Convert the XR+SR to XW */
	r = xadd(lock, XW - XR_1 - SR_1) - SR_1;

	for (j = 2; r & SR_ANY; r = *lock) {
		long i;
		for (i = 0; i < j; i++) {
			cpu_relax();
			cpu_relax();
		}
		j = j << 1;
	}
}

/* immediately take the WR lock */
void wr_fast_lock(volatile unsigned long *lock)
{
	unsigned long r;
	long j = 5;

	while (1) {
		/* try to get a shared read access, retry if the XW is there */
		r = xadd(lock, XW);
		if (!(r & XR_ANY))
			break;
		atomic_sub(lock, XW);
		do {
			long i;
			for (i = 0; i < j; i++) {
				cpu_relax();
				cpu_relax();
			}
			j = j << 1;
		} while (*lock & XR_ANY);
	}

	/* wait for readers to go */
	for (j = 2; r & SR_ANY; r = *lock) {
		long i;
		for (i = 0; i < j; i++) {
			cpu_relax();
			cpu_relax();
		}
		j = j << 1;
	}
}

void ro_unlock(volatile unsigned long *lock)
{
	atomic_add(lock, -SR_1);
}

void mw_unlock(volatile unsigned long *lock)
{
	atomic_add(lock, -(XR_1 + SR_1));
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned long *lock)
{
	atomic_add(lock, -XW);
}

#elif !defined(USE_FUTEX)
void ro_lock(volatile unsigned long *lock)
{
	long j = 5;
	while (1) {
		//for (j /= 2; *lock & 1;) {
		//	long i;
		//	for (i = 0; i < j; i++) {
		//		cpu_relax();
		//		cpu_relax();
		//	}
		//	j = (j << 1) + 1;
		//}


		if (!(xadd(lock, 0x10000) & 1))
			break;
		atomic_add(lock, -0x10000);
		do {
			long i;
			for (i = 0; i < j; i++) {
				cpu_relax();
				cpu_relax();
			}
			j = j << 1;
		} while (*lock & 1);
	}
}

void ro_unlock(volatile unsigned long *lock)
{
	atomic_add(lock, -0x10000);
}

void mw_lock0(volatile unsigned long *lock)
{
	long j = 8;
	while (1) {
		unsigned long r;

		r = xadd(lock, 2);
		if (!(r & 0xFFFF)) /* none of MW or WR */
			break;
		r = xadd(lock, -2);

		for (j /= 2; r & 0xFFFF; r = *lock) {
			long i;
			for (i = 0; i < j; i++) {
				cpu_relax();
				cpu_relax();
			}
			j = (j << 1) + 1;
		}
	}
}

void mw_lock(volatile unsigned long *lock)
{
	long j = 5;
	while (1) {
		if (!(xadd(lock, 2) & 0xFFFF))
			break;
		atomic_add(lock, -2);
		while (*lock & 0xFFFF) {
			/* either an MW or WR is present */
			long i;
			for (i = 0; i < j; i++) {
				cpu_relax();
				cpu_relax();
			}
			j = j << 1;
		}
	}
}

void mw_unlock(volatile unsigned long *lock)
{
	atomic_add(lock, -2);
}

void wr_lock(volatile unsigned long *lock)
{
	unsigned long r;
	long j;

	/* Note: we already hold the MW lock, we don't care about other
	 * MW waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */
	r = xadd(lock, 0x1);

	for (j = 2; r & 0xFFFF0000; r = *lock) {
		long i;
		for (i = 0; i < j; i++) {
			cpu_relax();
			cpu_relax();
		}
		j = j << 1;
	}
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned long *lock)
{
	atomic_add(lock, -3);
}
#elif USE_FUTEX2
/* USE_FUTEX */
void ro_lock(volatile unsigned long *lock)
{
	/* attempt to get a read lock (0x1000000) and if it fails,
	 * convert to a read request (0x10000), and wait for a slot.
	 */
	long j = xadd(lock, 0x1000000);
	if (!(j & 1))
		return;

	j = xadd(lock, 0x10000 - 0x1000000); /* transform the read lock into read request */
	j += 0x10000 - 0x1000000;

	while (j & 1) {
		syscall(SYS_futex, lock, FUTEX_WAIT, j, NULL, NULL, 0);
		j = *lock;
	}
	/* the writer cannot have caught us here */
	xadd(lock, 0x1000000-0x10000); /* switch the read req into read again */
}

void ro_unlock(volatile unsigned long *lock)
{
	long j = xadd(lock, -0x1000000);
	if (j & 1) /* at least one writer is waiting, we'll have to wait both writers and MW-ers */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void mw_lock(volatile unsigned long *lock)
{
	while (1) {
		long j = xadd(lock, 2);
		if (!(j & 0xFFFF))
			break;
		atomic_add(lock, -2);
		syscall(SYS_futex, lock, FUTEX_WAIT, j, NULL, NULL, 0);
	}
}

void mw_unlock(volatile unsigned long *lock)
{
	long j = xadd(lock, -0x2);
	if (j & 0xFFFFFFFE) /* at least one reader or MW-er is waiting, we'll have to wait everyone */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void wr_lock(volatile unsigned long *lock)
{
	unsigned long r;

	/* Note: we already hold the MW lock, we don't care about other
	 * MW waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */
	r = xadd(lock, 0x1);
	while (r & 0xFF000000) { /* ignore all read waiters, finish all readers */
		/* wait for all readers to go away */
		syscall(SYS_futex, lock, FUTEX_WAIT, r + 1, NULL, NULL, 0);
		r = *lock;
	}
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned long *lock)
{
	atomic_add(lock, -3);
	if (*lock) /* at least one reader or MW-er is waiting, we'll have to wait everyone */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}
#elif USE_FUTEX3
/* USE_FUTEX */
void ro_lock(volatile unsigned long *lock)
{
	long j;

	while ((j = xadd(lock, 0x10000)) & 1) {
		if ((j = xadd(lock, -0x10000)) & 1)
			syscall(SYS_futex, lock, FUTEX_WAIT, j - 0x10000, NULL, NULL, 0);
		else
			syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
	}
}

void ro_unlock(volatile unsigned long *lock)
{
	long j = xadd(lock, -0x10000);
	//if (/*(j & 1) &&*/ (j & 0xFFFF0000) == 0x10000) /* we were the last reader and there is a writer */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void mw_lock(volatile unsigned long *lock)
{
	long j;

	while ((j = xadd(lock, 0x2)) & 0xFFFE) {
		if (((j = xadd(lock, -0x2)) & 0xFFFE) != 0x2)
			syscall(SYS_futex, lock, FUTEX_WAIT, j - 2, NULL, NULL, 0);
		else
			syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
	}
}

void mw_unlock(volatile unsigned long *lock)
{
	long j = xadd(lock, -0x2);
	//	if ((j & 0xFFFE) != 0x2) /* at least one MW-er is waiting */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void wr_lock(volatile unsigned long *lock)
{
	unsigned long j;

	/* Note: we already hold the MW lock, we don't care about other
	 * MW waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */

	//while ((j = xadd(lock, 0x1)) & 0xFFFF0000) {
	//	if ((j = xadd(lock, -0x1)) & 0xFFFF0000)
	//		syscall(SYS_futex, lock, FUTEX_WAIT, j - 1, NULL, NULL, 0);
	//	//xadd(lock, -0x1);
	//}
	if (!(xadd(lock, 1) & 0xFFFF0000))
		return;
	while ((j = *lock) & 0xFFFF0000)
		syscall(SYS_futex, lock, FUTEX_WAIT, j, NULL, NULL, 0);
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned long *lock)
{
	unsigned long j = xadd(lock, -0x3);
	//if (j > 3) /* at least one reader or one MW-er was waiting */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}
#else
void ro_lock(volatile unsigned long *lock)
{
	if (!(xadd(lock, 0x10000) & 1)) /* maybe check 3 instead to be fairer */
		return;

	while (1) {
		long i;

		if ((i = xadd(lock, 4 - 0x10000)) & 1)
			syscall(SYS_futex, lock, FUTEX_WAIT, i + 4 - 0x10000, NULL, NULL, 0);
		if (!(xadd(lock, 0x10000 - 4) & 1))
			break;
	}
}

void ro_unlock(volatile unsigned long *lock)
{
	unsigned i = xadd(lock, -0x10000);

	if (((i & 0xFFFF0000) == 0x10000) && (i & 0xFFFC) > 0)
		/* we're the last reader and there are sleepers */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void mw_lock(volatile unsigned long *lock)
{
	unsigned long r;

	while (1) {
		if (!atomic_bts(lock, 1)) /* try to be the first one to set bit 1 */
			break;
		if ((r = xadd(lock, 4)) & 2) /* otherwise wait */
			syscall(SYS_futex, lock, FUTEX_WAIT, r + 4, NULL, NULL, 0);
		r = xadd(lock, -4);
	}
}

void mw_unlock(volatile unsigned long *lock)
{
	atomic_add(lock, -2);
	/////// FIXME: unused
}

void wr_lock(volatile unsigned long *lock)
{
	unsigned long r;

	/* Note: we already hold the MW lock, we don't care about other
	 * MW waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */
	r = xadd(lock, 0x1);

	while (r & 0xFFFF0000) {
		/* some readers still present */
		if ((r = xadd(lock, 4)) & 0xFFFF0000)
			syscall(SYS_futex, lock, FUTEX_WAIT, r + 4, NULL, NULL, 0);
		r = xadd(lock, -4);
	}
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned long *lock)
{
	if ((xadd(lock, -3) & 0xFFFC) != 0) /* someone is waiting */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

#endif

#if 0
int total;
int main(int argc, char **argv)
{
	long l;

	l = atoi(argv[1]);
	total = 0;
	while (l--) {
		atomic_add(&total, 1);
	}
	printf("total=%d\n", total);
	return 0;
}
#endif

