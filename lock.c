#include <stdio.h>

#include <linux/futex.h>
#include <sys/syscall.h>

#include "atomic-ops.h"


/* TODO: find names.
 *   - read-only / shared-read / visitor locks
 *   - upgradable locks
 *   - exclusive locks
 *
 * Invert names ?
 *  - dont-write
 *  - no-more-reader
 *  - dont-read
 *
 * - Read Shared                  => RS, rs_lock(), rs_unlock()
 * - Upgradable Shared/Exclusive  => US, us_lock(), ux_lock(), ux_unlock(), us_unlock()
 *                                   US, us_lock(), us_upgrade(), us_downgrade(), us_unlock()
 *                                   US, us_lock(), us_exclusive(), us_shared(), us_unlock()
 * - Write Exclusive              => WX, wx_lock(), wx_unlock()
 *
 *
 * - Read       => r_lock(), r_unlock()
 * - Upgradable => u_lock(), u_lock_ex(), u_unlock_ex(), u_unlock()
 * - Write      => w_lock(), w_unlock()
 *
 */

/*
 * TODO ebtrees:
 *   - plan on setting a refcount on returned nodes (first, next, lookup, insert_unique, ...)
 *   - for instance, next() would do  next->refcnt++; curr->refcnt--;
 */
//#define USE_FUTEX
#define USE_EXP1
#if defined(USE_EXP1)

/* 3 modes of operation :
 *   - read lookup  (RL) : all such threads may access the data at the same
 *                         time, this is just for read operations.
 *   - write lookup (WL) : only one such thread is allowed to pass, access
 *                         is still shared with normal readers above.
 *   - write commit (WC) : only one such thread has access at all.
 *
 * bits on 32 bits archs :
 *   - 31..22: WC
 *   - 21..12: WL
 *   - 11..2:  RL
 *
 * bits on 64 bits archs :
 *   - 61..42: WC
 *   - 41..22: WL
 *   - 21..2:  RL
 *
 * This allows up to 1023 threads to access the resource on 32-bit archs, and
 * up to 1048575 threads on 64-bit archs, with the two lower bits reserved for
 * other purposes. Writer has precedence : the bit is set until all readers go
 * away. Since conflicts will most often be writer waiting for readers to go,
 * we don't ant to leave the W lock during this time so that we limit locked
 * memory accesses. Using a 4^N backoff on exclusive resources seems to be the
 * most efficient method (and by far).
 */

#define RL_1   0x00000004
#define RL_ANY 0x00000FFC
#define WL_1   0x00001000
#define WL_ANY 0x003FF000
#define WC_1   0x00400000
#define WC_ANY 0xFFC00000

/* provide SR access */
static inline
void ro_lock(volatile unsigned long *lock)
{
	/* try to get a shared read access, retry if the XW is there, which is
	 * supposed to be rare since the W lock is for the final commit.
	 * Retrying in 2^N is enough here since we're waiting for a very short
	 * period.
	 */
	if (xadd(lock, RL_1) & WC_ANY) {
		unsigned long j = 8;
		do {
			atomic_sub(lock, RL_1);
			do {
				cpu_relax_long(j);
				j = j << 1;
			} while (*lock & WC_ANY);
		} while (xadd(lock, RL_1) & WC_ANY);
	}
}

static inline
void mw_lock(volatile unsigned long *lock)
{
	unsigned long j;

	j = 10;
	while (xadd(lock, WL_1) & (WC_ANY | WL_ANY)) {
		do {
			cpu_relax_long(j);
			j = j << 2;
		} while (*lock & (WC_ANY | WL_ANY));
	}
}

static inline
void wr_lock(volatile unsigned long *lock)
{
	unsigned long r;
	unsigned long j;

	/* Note: we already hold the WL lock, we don't care about other
	 * WL waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */

	r = xadd(lock, WC_1);

	for (j = 10; r & RL_ANY; r = *lock) {
		cpu_relax_long(j);
		j = j << 2;
	}
}

/* immediately take the WR lock */
//static inline
void wr_fast_lock(volatile unsigned long *lock)
{
	unsigned long r;
	unsigned long j;

	/* try to get an exclusive write access */

	j = 10;
	while ((r = xadd(lock, WC_1)) & (WC_ANY | WL_ANY)) {
		/* another thread was already there, wait for it to clear
		 * the lock for us.
		 */
		do {
			cpu_relax_long(j);
			j = j << 2;       //  grow in 4^N
		} while (*lock & (WC_ANY | WL_ANY));
	}

	/* wait for readers to go */
	for (j = 10; r & RL_ANY; r = *lock) {
		cpu_relax_long(j);
		j = j << 2;
	}
}

static inline
void ro_unlock(volatile unsigned long *lock)
{
	atomic_sub(lock, RL_1);
}

/* unlock both the shared and exclusive writes */
static inline
void wr_unlock(unsigned long *lock)
{
	atomic_and(lock, ~(WC_ANY | WL_ANY));
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

