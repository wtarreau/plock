#ifndef ATOMIC_OPS_H
#define ATOMIC_OPS_H

#if defined(__i386__) || defined (__i486__) || defined (__i586__) || defined (__i686__)

/* x86 32 bits */

static inline void cpu_relax()
{
	asm volatile("rep;nop\n" ::: "memory");
}

static inline void cpu_relax_long(unsigned long cycles)
{
	do {
		cpu_relax();
	} while (--cycles);
}

/* increment value and return non-zero if result is non-null */
static inline unsigned char atomic_inc(volatile unsigned long *ptr)
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
static inline unsigned char atomic_dec(volatile unsigned long *ptr)
{
	unsigned char ret;
	asm volatile("lock decl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* increment bit value */
static inline void atomic_inc_noret(volatile unsigned long *ptr)
{
	asm volatile("lock incl %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement bit value */
static inline void atomic_dec_noret(volatile unsigned long *ptr)
{
	asm volatile("lock decl %0\n" : "+m" (*ptr) :: "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_add(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock addl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_sub(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock subl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_and(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock andl %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* test and set a bit. Previous value is returned as 0 (not set) or -1 (set). */
static inline unsigned long atomic_bts(volatile unsigned long *ptr, const unsigned long bit)
{
	unsigned long ret;
	asm volatile("lock btsl %2, %0\n\t"
		     "sbb %1, %1\n\t"
		     : "+m" (*ptr), "=r" (ret)
		     : "Ir" (bit)
		     : "memory");
	return ret;
}

/* temp = x; x = *ptr ; *ptr += temp */
static inline unsigned long xadd(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xaddl %0, %1\n"
		     :  "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

/* exchange <x> with <*ptr> and return previous contents of <*ptr> */
static inline unsigned long xchg(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xchg %0,%1"
		     : "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

static inline unsigned long cmpxchg(volatile unsigned long *ptr, unsigned long old, unsigned long new)
{
	unsigned long ret;

	asm volatile("lock cmpxchg %2,%1"
		     : "=a" (ret), "+m" (*ptr)
		     : "r" (new), "0" (old)
		     : "memory");
	return ret;
}


#elif defined(__x86_64__)
/* x86 64 bits */

static inline void cpu_relax()
{
	asm volatile("rep;nop\n" ::: "memory");
}

static inline void cpu_relax_long(unsigned long cycles)
{
	do {
		cpu_relax();
	} while (--cycles);
}

/* increment value and return non-zero if result is non-null */
static inline unsigned char atomic_inc(volatile unsigned long *ptr)
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
static inline unsigned char atomic_dec(volatile unsigned long *ptr)
{
	unsigned char ret;
	asm volatile("lock decq %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* increment bit value */
static inline void atomic_inc_noret(volatile unsigned long *ptr)
{
	asm volatile("lock incq %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement bit value */
static inline void atomic_dec_noret(volatile unsigned long *ptr)
{
	asm volatile("lock decq %0\n" : "+m" (*ptr) :: "memory");
}

static inline void atomic_add(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock addq %1, %0\n"
		     : "+m" (*ptr)
		     : "er" (x)
		     : "memory");
}

static inline void atomic_sub(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock subq %1, %0\n"
		     : "+m" (*ptr)
		     : "er" (x)
		     : "memory");
}

static inline void atomic_and(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock andq %1, %0\n"
		     : "+m" (*ptr)
		     : "er" (x)
		     : "memory");
}

/* test and set a bit. Previous value is returned as 0 (not set) or -1 (set). */
static inline unsigned long atomic_bts(volatile unsigned long *ptr, const unsigned long bit)
{
	unsigned long ret;
	asm volatile("lock btsq %2, %0\n\t"
		     "sbb %1, %1\n\t"
		     : "+m" (*ptr), "=r" (ret)
		     : "Ir" (bit)
		     : "memory");
	return ret;
}

/* temp = x; x = *ptr ; *ptr += temp */
static inline unsigned long xadd(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xaddq %0, %1\n"
		     :  "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

/* exchange <x> with <*ptr> and return previous contents of <*ptr> */
static inline unsigned long xchg(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xchg %0,%1"
		     : "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

static inline unsigned long cmpxchg(volatile unsigned long *ptr, unsigned long old, unsigned long new)
{
	unsigned long ret;

	asm volatile("lock cmpxchg %2,%1"
		     : "=a" (ret), "+m" (*ptr)
		     : "r" (new), "0" (old)
		     : "memory");
	return ret;
}

#endif
/* TODO: atomic ops on pointers (eg: lock xadd without length suffix) */

#endif /* ATOMIC_OPS_H */
