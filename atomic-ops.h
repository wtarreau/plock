#ifndef ATOMIC_OPS_H
#define ATOMIC_OPS_H

/* increment value and return non-zero if result is non-null */
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

/* decrement value and return non-zero if result is non-null */
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

/* increment bit value */
static inline void atomic_inc_noret(volatile unsigned int *ptr)
{
	asm volatile("lock incl %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement bit value */
static inline void atomic_dec_noret(volatile unsigned int *ptr)
{
	asm volatile("lock decl %0\n" : "+m" (*ptr) :: "memory");
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
static inline void atomic_sub(volatile unsigned int *ptr, unsigned int x)
{
	asm volatile("lock subl %1, %0\n"
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

/* test and set a bit. Previous value is returned as 0 (not set) or -1 (set). */
static inline unsigned int atomic_bts(volatile unsigned int *ptr, const unsigned int bit)
{
	unsigned int ret;
	asm volatile("lock bts %2, %0\n\t"
		     "sbb %1, %1\n\t"
		     : "+m" (*ptr), "=r" (ret)
		     : "Ir" (bit)
		     : "memory");
	return ret;
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

/* exchange <x> with <*ptr> and return previous contents of <*ptr> */
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

/* increment 64-bit value */
static inline void atomic_inc64_noret(volatile unsigned long long *ptr)
{
	asm volatile("lock incq %0\n" : "+m" (*ptr) :: "memory");
}

/* decrement 64-bit value */
static inline void atomic_dec64_noret(volatile unsigned long long *ptr)
{
	asm volatile("lock decq %0\n" : "+m" (*ptr) :: "memory");
}

static inline void atomic_add64(volatile unsigned long long *ptr, unsigned long x)
{
	asm volatile("lock addq %1, %0\n"
		     : "+m" (*ptr)
		     : "ir" (x)
		     : "memory");
}

/* TODO: atomic ops on pointers (eg: lock xadd without length suffix) */

#endif /* ATOMIC_OPS_H */
