static inline unsigned int xchg(unsigned int *ptr, unsigned int x)
{
	asm volatile("lock xchgl %0,%1"
		     : "=r" (x), "+m" (*ptr)
		     : "0" (x)
		     : "memory");
	return x;
}

static inline unsigned int cmpxchg(unsigned int *ptr, unsigned int old, unsigned int new)
{
	unsigned int ret;

	asm volatile("lock cmpxchgl %2,%1"
		     : "=a" (ret), "+m" (*ptr)
		     : "r" (new), "0" (old)
		     : "memory");
	return ret;
}

static inline unsigned char atomic_inc(unsigned int *ptr)
{
	unsigned char ret;
	asm volatile("lock incl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

static inline unsigned char atomic_dec(unsigned int *ptr)
{
	unsigned char ret;
	asm volatile("lock decl %0\n"
		     "setne %1\n"
		     : "+m" (*ptr), "=qm" (ret)
		     :
		     : "memory");
	return ret;
}

