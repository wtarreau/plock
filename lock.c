#include <stdio.h>

#include <linux/futex.h>
#include <sys/syscall.h>

static inline unsigned int xchg(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock xchgl %0,%1"
		     : "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

static inline unsigned int cmpxchg(volatile unsigned int *ptr, unsigned int old, unsigned int new)
{
	unsigned int ret;

	asm volatile("lock cmpxchgl %2,%1"
		     : "=a" (ret), "+m" (*ptr)
		     : "r" (new), "0" (old)
		     : "memory");
	return ret;
}

static inline unsigned char atomic_inc(volatile unsigned int *ptr)
{
	unsigned char ret;
	asm volatile("lock incl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

static inline unsigned char atomic_dec(volatile unsigned int *ptr)
{
	unsigned char ret;
	asm volatile("lock decl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* test and set a bit. Previous value is returned as 0 (not set) or -1 (set). */
static inline unsigned int atomic_bts(volatile unsigned int *ptr, unsigned bit)
{
	unsigned int ret;
	asm volatile("lock bts %2,%0\n\t"
		     "sbb %1,%1\n\t"
		     : "+m" (*ptr), "=r" (ret)
		     : "Ir" (bit)
		     : "memory");
	return ret;
}

static inline void atomic_inc64_noret(volatile unsigned long long *ptr)
{
	asm volatile("lock incq %0\n" : "+m" (*ptr) :: "memory");
}

static inline void atomic_dec64_noret(volatile unsigned long long *ptr)
{
	asm volatile("lock decq %0\n" : "+m" (*ptr) :: "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_add(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock addl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_and(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock andl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_sub(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock subl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

static inline void atomic_add64(volatile unsigned long long *ptr, unsigned long x)
{
	asm volatile("lock addq %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* temp = x; x = *ptr ; *ptr += temp */
static inline unsigned int xadd(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock xaddl %0, %1\n"
		     :  "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

static inline unsigned int xadd_const(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("movl %2, %0\n\t"
		     "lock xaddl %0, %1\n"
		     :  "=r" (x), "+m" (*ptr)
		     : "i" (x)
		     : "memory");
	return x;
}

/* temp = x; x = *ptr ; *ptr += temp */
static inline void relax()
{
	asm volatile("rep;nop\n" ::: "memory");
}

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
 * memory accesses.
 */

#define RL_1   0x00000004
#define RL_ANY 0x00000FFC
#define WL_1   0x00001000
#define WL_ANY 0x003FF000
#define WC_1   0x00400000
#define WC_ANY 0xFFC00000

/* provide SR access */
static inline
void ro_lock(volatile unsigned int *lock)
{
	/* try to get a shared read access, retry if the XW is there */
	if (xadd_const(lock, RL_1) & WC_ANY) {
		int j = 5;
		do {
			atomic_sub(lock, RL_1);
			do {
				int i;
				for (i = 0; i < j; i++) {
					relax();
					relax();
				}
				j = j << 1;
			} while (*lock & WC_ANY);
		} while (xadd_const(lock, RL_1) & WC_ANY);
	}
}

static inline
void mw_lock(volatile unsigned int *lock)
{
	if (!(xadd_const(lock, WL_1) & (WC_ANY | WL_ANY)))
		return;
	do {
		int i;
		for (i = 0; i < 6; i++) {
			relax();
			relax();
		}
	} while ((*lock & (WC_ANY | WL_ANY)) ||
		 xadd_const(lock, WL_1) & (WC_ANY | WL_ANY));
}

/* version with exponential backoff */
static inline
void mw_lock_backoff(volatile unsigned int *lock)
{
	if (xadd_const(lock, WL_1) & (WC_ANY | WL_ANY)) {
		unsigned int j = 4;
		do {
			int i;
			for (i = 0; i < j; i++) {
				relax();
				relax();
			}
			j = j << 1;
		} while ((*lock & (WC_ANY | WL_ANY)) ||
			 xadd_const(lock, WL_1) & (WC_ANY | WL_ANY));
	}
}

static inline
void wr_lock(volatile unsigned int *lock)
{
	unsigned int r;
	int j;

	/* Note: we already hold the WL lock, we don't care about other
	 * WL waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */

	r = xadd_const(lock, WC_1);

	for (j = 2; r & RL_ANY; r = *lock) {
		int i;
		for (i = 0; i < j; i++) {
			relax();
			relax();
		}
		j = j << 1;
	}
}

/* immediately take the WR lock */
static inline
void wr_fast_lock(volatile unsigned int *lock)
{
	unsigned int r;
	int j;

	/* try to get an exclusive write access */
	r = xadd_const(lock, WC_1);
	if (r & (WC_ANY | WL_ANY)) {
		j = 5;
		/* another thread was already there, wait for it to clear
		 * the lock for us.
		 */
		do {
			int i;
			for (i = 0; i < j; i++) {
				relax();
				relax();
			}
			j = j << 1;
		} while ((*lock & (WC_ANY | WL_ANY)) ||
			 ((r = xadd_const(lock, WC_1)) & (r & (WC_ANY | WL_ANY))));
	}

	/* wait for readers to go */
	for (j = 2; r & RL_ANY; r = *lock) {
		int i;
		for (i = 0; i < j; i++) {
			relax();
			relax();
		}
		j = j << 1;
	}
}

static inline
void ro_unlock(volatile unsigned int *lock)
{
	atomic_sub(lock, RL_1);
}

/* unlock both the shared and exclusive writes */
static inline
void wr_unlock(unsigned int *lock)
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
void ro_lock(volatile unsigned int *lock)
{
	int j = 5;
	while (1) {
		/* try to get a shared read access, retry if the XW is there */
		if (!(xadd_const(lock, SR_1) & XW))
			break;
		atomic_sub(lock, SR_1);
		do {
			int i;
			for (i = 0; i < j; i++) {
				relax();
				relax();
			}
			j = j << 1;
		} while (*lock & XW);
	}
}

void mw_lock0(volatile unsigned int *lock)
{
	int i, j = 5;

	while (1) {
		i = xadd_const(lock, XR_1 + SR_1);
		if (!(i & (XR_ANY | XW)))
			return;

		if (!(i & XR_ANY))
			break;

		/* Wait for XR to be released first. We need to release SR too
		 * so that the future writer makes progress.
		 */
		i = xadd_const(lock, - (XR_1 + SR_1)) - XR_1;
		while (i & (XR_ANY | XW)) {
			for (i = 0; i < j; i++) {
				relax();
				relax();
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
				relax();
				relax();
			}
			j = j << 1;
		} while (*lock & XW);
		i = xadd_const(lock, SR_1);
	} while (i & XW);
}

void mw_lock(volatile unsigned int *lock)
{
	unsigned int i, j = 3;
	unsigned int need_xr = XR_1;

	while (1) {
		i = xadd_const(lock, need_xr + SR_1);
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
		i = xadd_const(lock, - (need_xr + SR_1)) - need_xr;
		while ((i & XW) || (i & -need_xr)) {
			for (i = 0; i < j; i++) {
				relax();
				relax();
			}
			j = j << 1;
			i = *lock;
		}
	}
}

void wr_lock(volatile unsigned int *lock)
{
	unsigned int r;
	int j;

	/* Note: we already hold the XR lock, we don't care about other
	 * XR waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */
	/* Convert the XR+SR to XW */
	r = xadd_const(lock, XW - XR_1 - SR_1) - SR_1;

	for (j = 2; r & SR_ANY; r = *lock) {
		int i;
		for (i = 0; i < j; i++) {
			relax();
			relax();
		}
		j = j << 1;
	}
}

/* immediately take the WR lock */
void wr_fast_lock(volatile unsigned int *lock)
{
	unsigned int r;
	int j = 5;

	while (1) {
		/* try to get a shared read access, retry if the XW is there */
		r = xadd_const(lock, XW);
		if (!(r & XR_ANY))
			break;
		atomic_sub(lock, XW);
		do {
			int i;
			for (i = 0; i < j; i++) {
				relax();
				relax();
			}
			j = j << 1;
		} while (*lock & XR_ANY);
	}

	/* wait for readers to go */
	for (j = 2; r & SR_ANY; r = *lock) {
		int i;
		for (i = 0; i < j; i++) {
			relax();
			relax();
		}
		j = j << 1;
	}
}

void ro_unlock(volatile unsigned int *lock)
{
	atomic_add(lock, -SR_1);
}

void mw_unlock(volatile unsigned int *lock)
{
	atomic_add(lock, -(XR_1 + SR_1));
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned int *lock)
{
	atomic_add(lock, -XW);
}

#elif !defined(USE_FUTEX)
void ro_lock(volatile unsigned int *lock)
{
	int j = 5;
	while (1) {
		//for (j /= 2; *lock & 1;) {
		//	int i;
		//	for (i = 0; i < j; i++) {
		//		relax();
		//		relax();
		//	}
		//	j = (j << 1) + 1;
		//}


		if (!(xadd_const(lock, 0x10000) & 1))
			break;
		atomic_add(lock, -0x10000);
		do {
			int i;
			for (i = 0; i < j; i++) {
				relax();
				relax();
			}
			j = j << 1;
		} while (*lock & 1);
	}
}

void ro_unlock(volatile unsigned int *lock)
{
	atomic_add(lock, -0x10000);
}

void mw_lock0(volatile unsigned int *lock)
{
	int j = 8;
	while (1) {
		unsigned int r;

		r = xadd_const(lock, 2);
		if (!(r & 0xFFFF)) /* none of MW or WR */
			break;
		r = xadd_const(lock, -2);

		for (j /= 2; r & 0xFFFF; r = *lock) {
			int i;
			for (i = 0; i < j; i++) {
				relax();
				relax();
			}
			j = (j << 1) + 1;
		}
	}
}

void mw_lock(volatile unsigned int *lock)
{
	int j = 5;
	while (1) {
		if (!(xadd_const(lock, 2) & 0xFFFF))
			break;
		atomic_add(lock, -2);
		while (*lock & 0xFFFF) {
			/* either an MW or WR is present */
			int i;
			for (i = 0; i < j; i++) {
				relax();
				relax();
			}
			j = j << 1;
		}
	}
}

void mw_unlock(volatile unsigned int *lock)
{
	atomic_add(lock, -2);
}

void wr_lock(volatile unsigned int *lock)
{
	unsigned int r;
	int j;

	/* Note: we already hold the MW lock, we don't care about other
	 * MW waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */
	r = xadd_const(lock, 0x1);

	for (j = 2; r & 0xFFFF0000; r = *lock) {
		int i;
		for (i = 0; i < j; i++) {
			relax();
			relax();
		}
		j = j << 1;
	}
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned int *lock)
{
	atomic_add(lock, -3);
}
#elif USE_FUTEX2
/* USE_FUTEX */
void ro_lock(volatile unsigned int *lock)
{
	/* attempt to get a read lock (0x1000000) and if it fails,
	 * convert to a read request (0x10000), and wait for a slot.
	 */
	int j = xadd_const(lock, 0x1000000);
	if (!(j & 1))
		return;

	j = xadd_const(lock, 0x10000 - 0x1000000); /* transform the read lock into read request */
	j += 0x10000 - 0x1000000;

	while (j & 1) {
		syscall(SYS_futex, lock, FUTEX_WAIT, j, NULL, NULL, 0);
		j = *lock;
	}
	/* the writer cannot have caught us here */
	xadd_const(lock, 0x1000000-0x10000); /* switch the read req into read again */
}

void ro_unlock(volatile unsigned int *lock)
{
	int j = xadd_const(lock, -0x1000000);
	if (j & 1) /* at least one writer is waiting, we'll have to wait both writers and MW-ers */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void mw_lock(volatile unsigned int *lock)
{
	while (1) {
		int j = xadd_const(lock, 2);
		if (!(j & 0xFFFF))
			break;
		atomic_add(lock, -2);
		syscall(SYS_futex, lock, FUTEX_WAIT, j, NULL, NULL, 0);
	}
}

void mw_unlock(volatile unsigned int *lock)
{
	int j = xadd_const(lock, -0x2);
	if (j & 0xFFFFFFFE) /* at least one reader or MW-er is waiting, we'll have to wait everyone */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void wr_lock(volatile unsigned int *lock)
{
	unsigned int r;

	/* Note: we already hold the MW lock, we don't care about other
	 * MW waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */
	r = xadd_const(lock, 0x1);
	while (r & 0xFF000000) { /* ignore all read waiters, finish all readers */
		/* wait for all readers to go away */
		syscall(SYS_futex, lock, FUTEX_WAIT, r + 1, NULL, NULL, 0);
		r = *lock;
	}
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned int *lock)
{
	atomic_add(lock, -3);
	if (*lock) /* at least one reader or MW-er is waiting, we'll have to wait everyone */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}
#elif USE_FUTEX3
/* USE_FUTEX */
void ro_lock(volatile unsigned int *lock)
{
	int j;

	while ((j = xadd_const(lock, 0x10000)) & 1) {
		if ((j = xadd_const(lock, -0x10000)) & 1)
			syscall(SYS_futex, lock, FUTEX_WAIT, j - 0x10000, NULL, NULL, 0);
		else
			syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
	}
}

void ro_unlock(volatile unsigned int *lock)
{
	int j = xadd_const(lock, -0x10000);
	//if (/*(j & 1) &&*/ (j & 0xFFFF0000) == 0x10000) /* we were the last reader and there is a writer */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void mw_lock(volatile unsigned int *lock)
{
	int j;

	while ((j = xadd_const(lock, 0x2)) & 0xFFFE) {
		if (((j = xadd_const(lock, -0x2)) & 0xFFFE) != 0x2)
			syscall(SYS_futex, lock, FUTEX_WAIT, j - 2, NULL, NULL, 0);
		else
			syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
	}
}

void mw_unlock(volatile unsigned int *lock)
{
	int j = xadd_const(lock, -0x2);
	//	if ((j & 0xFFFE) != 0x2) /* at least one MW-er is waiting */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void wr_lock(volatile unsigned int *lock)
{
	unsigned int j;

	/* Note: we already hold the MW lock, we don't care about other
	 * MW waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */

	//while ((j = xadd_const(lock, 0x1)) & 0xFFFF0000) {
	//	if ((j = xadd_const(lock, -0x1)) & 0xFFFF0000)
	//		syscall(SYS_futex, lock, FUTEX_WAIT, j - 1, NULL, NULL, 0);
	//	//xadd_const(lock, -0x1);
	//}
	if (!(xadd_const(lock, 1) & 0xFFFF0000))
		return;
	while ((j = *lock) & 0xFFFF0000)
		syscall(SYS_futex, lock, FUTEX_WAIT, j, NULL, NULL, 0);
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned int *lock)
{
	unsigned int j = xadd_const(lock, -0x3);
	//if (j > 3) /* at least one reader or one MW-er was waiting */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}
#else
void ro_lock(volatile unsigned int *lock)
{
	if (!(xadd_const(lock, 0x10000) & 1)) /* maybe check 3 instead to be fairer */
		return;

	while (1) {
		int i;

		if ((i = xadd_const(lock, 4 - 0x10000)) & 1)
			syscall(SYS_futex, lock, FUTEX_WAIT, i + 4 - 0x10000, NULL, NULL, 0);
		if (!(xadd_const(lock, 0x10000 - 4) & 1))
			break;
	}
}

void ro_unlock(volatile unsigned int *lock)
{
	unsigned i = xadd_const(lock, -0x10000);

	if (((i & 0xFFFF0000) == 0x10000) && (i & 0xFFFC) > 0)
		/* we're the last reader and there are sleepers */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

void mw_lock(volatile unsigned int *lock)
{
	unsigned int r;

	while (1) {
		if (!atomic_bts(lock, 1)) /* try to be the first one to set bit 1 */
			break;
		if ((r = xadd_const(lock, 4)) & 2) /* otherwise wait */
			syscall(SYS_futex, lock, FUTEX_WAIT, r + 4, NULL, NULL, 0);
		r = xadd_const(lock, -4);
	}
}

void mw_unlock(volatile unsigned int *lock)
{
	atomic_add(lock, -2);
	/////// FIXME: unused
}

void wr_lock(volatile unsigned int *lock)
{
	unsigned int r;

	/* Note: we already hold the MW lock, we don't care about other
	 * MW waiters, since they won't get their lock, but we want the
	 * readers to leave before going on.
	 */
	r = xadd_const(lock, 0x1);

	while (r & 0xFFFF0000) {
		/* some readers still present */
		if ((r = xadd_const(lock, 4)) & 0xFFFF0000)
			syscall(SYS_futex, lock, FUTEX_WAIT, r + 4, NULL, NULL, 0);
		r = xadd_const(lock, -4);
	}
}

/* unlock both the MW and the WR */
void wr_unlock(unsigned int *lock)
{
	if ((xadd_const(lock, -3) & 0xFFFC) != 0) /* someone is waiting */
		syscall(SYS_futex, lock, FUTEX_WAKE, -1, NULL, NULL, 0);
}

#endif

#if 0
int total;
int main(int argc, char **argv)
{
	int l;

	l = atoi(argv[1]);
	total = 0;
	while (l--) {
		atomic_add(&total, 1);
	}
	printf("total=%d\n", total);
	return 0;
}
#endif

