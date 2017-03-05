#ifndef ATOMIC_OPS_H
#define ATOMIC_OPS_H


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
	asm volatile("lock inc %0\n"
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
	asm volatile("lock dec %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

/* increment bit value */
static inline void atomic_inc_noret(volatile unsigned long *ptr)
{
	asm volatile("lock inc %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement bit value */
static inline void atomic_dec_noret(volatile unsigned long *ptr)
{
	asm volatile("lock dec %0\n" : "+m" (*ptr) :: "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_add(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock add %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_sub(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock sub %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* cost for 100M ops on P-M 1.7: .36s without lock, 1.32s with lock ! */
static inline void atomic_and(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock and %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* test and set a bit. Previous value is returned as 0 (not set) or -1 (set). */
static inline unsigned long atomic_bts(volatile unsigned long *ptr, const unsigned long bit)
{
	unsigned long ret;
	asm volatile("lock bts %2, %0\n\t"
		     "sbb %1, %1\n\t"
		     : "+m" (*ptr), "=r" (ret)
		     : "Ir" (bit)
		     : "memory");
	return ret;
}

/* temp = x; x = *ptr ; *ptr += temp */
static inline unsigned long xadd(volatile unsigned long *ptr, unsigned long x)
{
	asm volatile("lock xadd %0, %1\n"
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

/* increment 64-bit value */
static inline void atomic_inc64_noret(volatile unsigned long long *ptr)
{
	asm volatile("lock inc %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement 64-bit value */
static inline void atomic_dec64_noret(volatile unsigned long long *ptr)
{
	asm volatile("lock dec %0\n" : "+m" (*ptr) :: "memory");
}

static inline void atomic_add64(volatile unsigned long long *ptr, unsigned long x)
{
	asm volatile("lock add %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* TODO: atomic ops on pointers (eg: lock xadd without length suffix) */

#endif /* ATOMIC_OPS_H */
