#ifndef PL_ATOMIC_OPS_H
#define PL_ATOMIC_OPS_H


#if defined(__i386__) || defined (__i486__) || defined (__i586__) || defined (__i686__) || defined (__x86_64__)

/*
 * Generic functions common to the x86 family
 */

static inline void pl_cpu_relax()
{
	asm volatile("rep;nop\n" ::: "memory");
}

/*
 * Explicit 32-bit functions for use with unsigned int, common to i386 and x86_64
 */

/* increment value and return non-zero if result is non-null */
static inline unsigned char pl32_inc(volatile unsigned int *ptr)
{
	unsigned char ret;
	asm volatile("lock incl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* decrement value and return non-zero if result is non-null */
static inline unsigned char pl32_dec(volatile unsigned int *ptr)
{
	unsigned char ret;
	asm volatile("lock decl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* increment bit value, no return */
static inline void pl32_inc_noret(volatile unsigned int *ptr)
{
	asm volatile("lock incl %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement bit value, no return */
static inline void pl32_dec_noret(volatile unsigned int *ptr)
{
	asm volatile("lock decl %0\n" : "+m" (*ptr) :: "memory");
}

/* add constant <x> to <*ptr>, no return */
static inline void pl32_add(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock addl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* subtract constant <x> from <*ptr>, no return */
static inline void pl32_sub(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock subl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* binary and <*ptr> with constant <x>, no return */
static inline void pl32_and(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock andl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* test and set a bit. Previous value is returned as 0 (not set) or -1 (set). */
static inline unsigned int pl32_bts(volatile unsigned int *ptr, const unsigned int bit)
{
	unsigned int ret;
	asm volatile("lock btsl %2, %0\n\t"
		     "sbb %1, %1\n\t"
		     : "+m" (*ptr), "=r" (ret)
		     : "Ir" (bit)
		     : "memory");
	return ret;
}

/* For an unclear reason, gcc's __sync_fetch_and_add() implementation produces
 * less optimal than hand-crafted asm code so let's implement here the
 * operations we need for the most common archs.
 */

/* temp = x; x = *ptr ; *ptr += temp */
static inline unsigned int pl32_xadd(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock xaddl %0, %1\n"
		     :  "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

/* exchange <x> with <*ptr> and return previous contents of <*ptr> */
static inline unsigned int pl32_xchg(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock xchg %0,%1"
		     : "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

/* compare <*ptr> with <old> and exchange with <new> if matches, and return <old>. */
static inline unsigned int pl32_cmpxchg(volatile unsigned int *ptr, unsigned int old, unsigned int new)
{
	unsigned int ret;

	asm volatile("lock cmpxchg %2,%1"
		     : "=a" (ret), "+m" (*ptr)
		     : "r" (new), "0" (old)
		     : "memory");
	return ret;
}

/*
 * Functions acting on "long" below
 */

#if defined(__x86_64__)
/* x86 64 bits */

/* increment value and return non-zero if result is non-null */
static inline unsigned char pl_inc(volatile unsigned long *ptr)
{
	unsigned char ret;
	asm volatile("lock incq %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* decrement value and return non-zero if result is non-null */
static inline unsigned char pl_dec(volatile unsigned long *ptr)
{
	unsigned char ret;
	asm volatile("lock decq %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* increment bit value, no return */
static inline void pl_inc_noret(volatile unsigned long *ptr)
{
	asm volatile("lock incq %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement bit value, no return */
static inline void pl_dec_noret(volatile unsigned long *ptr)
{
	asm volatile("lock decq %0\n" : "+m" (*ptr) :: "memory");
}

/* add constant <x> to <*ptr>, no return */
static inline void pl_add(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock addq %1, %0\n"
		     : "+m" (*ptr)
		     : "er" (x)
		     : "memory");
}

/* subtract constant <x> from <*ptr>, no return */
static inline void pl_sub(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock subq %1, %0\n"
		     : "+m" (*ptr)
		     : "er" (x)
		     : "memory");
}

/* binary and <*ptr> with constant <x>, no return */
static inline void pl_and(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock andq %1, %0\n"
		     : "+m" (*ptr)
		     : "er" (x)
		     : "memory");
}

/* test and set a bit. Previous value is returned as 0 (not set) or -1 (set). */
static inline unsigned long pl_bts(volatile unsigned long *ptr, const unsigned long bit)
{
	unsigned long ret;
	asm volatile("lock btsq %2, %0\n\t"
		     "sbb %1, %1\n\t"
		     : "+m" (*ptr), "=r" (ret)
		     : "Ir" (bit)
		     : "memory");
	return ret;
}

/* For an unclear reason, gcc's __sync_fetch_and_add() implementation produces
 * less optimal than hand-crafted asm code so let's implement here the
 * operations we need for the most common archs.
 */

/* temp = x; x = *ptr ; *ptr += temp */
static inline unsigned long pl_xadd(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xaddq %0, %1\n"
		     :  "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

/* exchange <x> with <*ptr> and return previous contents of <*ptr> */
static inline unsigned long pl_xchg(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xchg %0,%1"
		     : "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

/* compare <*ptr> with <old> and exchange with <new> if matches, and return <old>. */
static inline unsigned long pl_cmpxchg(volatile unsigned long *ptr, unsigned long old, unsigned long new)
{
	unsigned long ret;

	asm volatile("lock cmpxchg %2,%1"
		     : "=a" (ret), "+m" (*ptr)
		     : "r" (new), "0" (old)
		     : "memory");
	return ret;
}

#else 
/* x86 32 bits */

/* increment value and return non-zero if result is non-null */
static inline unsigned char pl_inc(volatile unsigned long *ptr)
{
	unsigned char ret;
	asm volatile("lock incl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* decrement value and return non-zero if result is non-null */
static inline unsigned char pl_dec(volatile unsigned long *ptr)
{
	unsigned char ret;
	asm volatile("lock decl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* increment bit value, no return */
static inline void pl_inc_noret(volatile unsigned long *ptr)
{
	asm volatile("lock incl %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement bit value, no return */
static inline void pl_dec_noret(volatile unsigned long *ptr)
{
	asm volatile("lock decl %0\n" : "+m" (*ptr) :: "memory");
}

/* add constant <x> to <*ptr>, no return */
static inline void pl_add(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock addl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* subtract constant <x> from <*ptr>, no return */
static inline void pl_sub(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock subl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* binary and <*ptr> with constant <x>, no return */
static inline void pl_and(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock andl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* test and set a bit. Previous value is returned as 0 (not set) or -1 (set). */
static inline unsigned long pl_bts(volatile unsigned long *ptr, const unsigned long bit)
{
	unsigned long ret;
	asm volatile("lock btsl %2, %0\n\t"
		     "sbb %1, %1\n\t"
		     : "+m" (*ptr), "=r" (ret)
		     : "Ir" (bit)
		     : "memory");
	return ret;
}

/* For an unclear reason, gcc's __sync_fetch_and_add() implementation produces
 * less optimal than hand-crafted asm code so let's implement here the
 * operations we need for the most common archs.
 */

/* temp = x; x = *ptr ; *ptr += temp */
static inline unsigned long pl_xadd(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xaddl %0, %1\n"
		     :  "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

/* exchange <x> with <*ptr> and return previous contents of <*ptr> */
static inline unsigned long pl_xchg(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xchg %0,%1"
		     : "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

/* compare <*ptr> with <old> and exchange with <new> if matches, and return <old>. */
static inline unsigned long pl_cmpxchg(volatile unsigned long *ptr, unsigned long old, unsigned long new)
{
	unsigned long ret;

	asm volatile("lock cmpxchg %2,%1"
		     : "=a" (ret), "+m" (*ptr)
		     : "r" (new), "0" (old)
		     : "memory");
	return ret;
}

#endif /* i386|x86_64 */
#else
/* generic implementations */

static inline void pl_cpu_relax()
{
	asm volatile("" ::: "memory");
}

/* explicit 32-bit values */

static inline void pl32_inc_noret(volatile unsigned int *ptr)
{
	__sync_add_and_fetch(ptr, 1);
}

static inline void pl32_dec_noret(volatile unsigned int *ptr)
{
	__sync_sub_and_fetch(ptr, 1);
}

static inline unsigned int pl32_inc(volatile unsigned int *ptr)
{
	return __sync_add_and_fetch(ptr, 1);
}

static inline unsigned int pl32_dec(volatile unsigned int *ptr)
{
	return __sync_sub_and_fetch(ptr, 1);
}

static inline unsigned int pl32_add(volatile unsigned int *ptr, unsigned int x)
{
	return __sync_add_and_fetch(ptr, x);
}

static inline unsigned int pl32_sub(volatile unsigned int *ptr, unsigned int x)
{
	return __sync_sub_and_fetch(ptr, x);
}

static inline unsigned int pl32_xadd(volatile unsigned int *ptr, unsigned int x)
{
	return __sync_fetch_and_add(ptr, x);
}

/* explicit long values */

static inline void pl_inc_noret(volatile unsigned long *ptr)
{
	__sync_add_and_fetch(ptr, 1);
}

static inline void pl_dec_noret(volatile unsigned long *ptr)
{
	__sync_sub_and_fetch(ptr, 1);
}

static inline unsigned long pl_inc(volatile unsigned long *ptr)
{
	return __sync_add_and_fetch(ptr, 1);
}

static inline unsigned long pl_dec(volatile unsigned long *ptr)
{
	return __sync_sub_and_fetch(ptr, 1);
}

static inline unsigned long pl_add(volatile unsigned long *ptr, unsigned long x)
{
	return __sync_add_and_fetch(ptr, x);
}

static inline unsigned long pl_sub(volatile unsigned long *ptr, unsigned long x)
{
	return __sync_sub_and_fetch(ptr, x);
}

static inline unsigned long pl_xadd(volatile unsigned long *ptr, unsigned long x)
{
	return __sync_fetch_and_add(ptr, x);
}

#endif
/* TODO: atomic ops on pointers (eg: lock xadd without length suffix) */

#endif /* PL_ATOMIC_OPS_H */
