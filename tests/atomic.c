/* Check output code of atomic operations :
 *
 *    gcc -I.. -O3 -W -Wall -Wextra -fomit-frame-pointer -c atomic.c
 *    objdump -dr atomic.o
 */
#include <atomic-ops.h>

char pl8_inc(char *ptr)
{
	return pl_inc(ptr);
}

short pl16_inc(short *ptr)
{
	return pl_inc(ptr);
}

int pl32_inc(int *ptr)
{
	return pl_inc(ptr);
}

long long pl64_inc(long long *ptr)
{
	return pl_inc(ptr);
}

////////////////////////////////////////////

char pl8_dec(char *ptr)
{
	return pl_dec(ptr);
}

short pl16_dec(short *ptr)
{
	return pl_dec(ptr);
}

int pl32_dec(int *ptr)
{
	return pl_dec(ptr);
}

long long pl64_dec(long long *ptr)
{
	return pl_dec(ptr);
}

//////////////////////////////////////////////

void pl8_inc_noret(char *ptr)
{
	pl_inc_noret(ptr);
}

void pl16_inc_noret(short *ptr)
{
	pl_inc_noret(ptr);
}

void pl32_inc_noret(int *ptr)
{
	pl_inc_noret(ptr);
}

void pl64_inc_noret(long long *ptr)
{
	pl_inc_noret(ptr);
}

///////////////////////////////////////

void pl8_dec_noret(char *ptr)
{
	pl_dec_noret(ptr);
}

void pl16_dec_noret(short *ptr)
{
	pl_dec_noret(ptr);
}

void pl32_dec_noret(int *ptr)
{
	pl_dec_noret(ptr);
}

void pl64_dec_noret(long long *ptr)
{
	pl_dec_noret(ptr);
}

///////////////////////////////////////////

void pl8_add(char *ptr, char x)
{
	pl_add(ptr, x);
}

void pl16_add(short *ptr, short x)
{
	pl_add(ptr, x);
}

void pl32_add(int *ptr, int x)
{
	pl_add(ptr, x);
}

void pl64_add(long long *ptr, long long x)
{
	pl_add(ptr, x);
}

///////////////////////////////////////////

void pl8_sub(char *ptr, char x)
{
	pl_sub(ptr, x);
}

void pl16_sub(short *ptr, short x)
{
	pl_sub(ptr, x);
}

void pl32_sub(int *ptr, int x)
{
	pl_sub(ptr, x);
}

void pl64_sub(long long *ptr, long long x)
{
	pl_sub(ptr, x);
}

///////////////////////////////////////////

void pl8_and(char *ptr, char x)
{
	pl_and(ptr, x);
}

void pl16_and(short *ptr, short x)
{
	pl_and(ptr, x);
}

void pl32_and(int *ptr, int x)
{
	pl_and(ptr, x);
}

void pl64_and(long long *ptr, long long x)
{
	pl_and(ptr, x);
}

///////////////////////////////////////////

void pl8_or(char *ptr, char x)
{
	pl_or(ptr, x);
}

void pl16_or(short *ptr, short x)
{
	pl_or(ptr, x);
}

void pl32_or(int *ptr, int x)
{
	pl_or(ptr, x);
}

void pl64_or(long long *ptr, long long x)
{
	pl_or(ptr, x);
}

///////////////////////////////////////////

void pl8_xor(char *ptr, char x)
{
	pl_xor(ptr, x);
}

void pl16_xor(short *ptr, short x)
{
	pl_xor(ptr, x);
}

void pl32_xor(int *ptr, int x)
{
	pl_xor(ptr, x);
}

void pl64_xor(long long *ptr, long long x)
{
	pl_xor(ptr, x);
}

///////////////////////////////////////////

short pl16_bts(short *ptr, short x)
{
	return pl_bts(ptr, x);
}

int pl32_bts(int *ptr, int x)
{
	return pl_bts(ptr, x);
}

long long pl64_bts(long long *ptr, long long x)
{
	return pl_bts(ptr, x);
}

///////////////////////////////////////////

char pl8_xadd(char *ptr, char x)
{
	return pl_xadd(ptr, x);
}

short pl16_xadd(short *ptr, short x)
{
	return pl_xadd(ptr, x);
}

int pl32_xadd(int *ptr, int x)
{
	return pl_xadd(ptr, x);
}

long long pl64_xadd(long long *ptr, long long x)
{
	return pl_xadd(ptr, x);
}

///////////////////////////////////////////

char pl8_xchg(char *ptr, char x)
{
	return pl_xchg(ptr, x);
}

short pl16_xchg(short *ptr, short x)
{
	return pl_xchg(ptr, x);
}

int pl32_xchg(int *ptr, int x)
{
	return pl_xchg(ptr, x);
}

long long pl64_xchg(long long *ptr, long long x)
{
	return pl_xchg(ptr, x);
}

///////////////////////////////////////////

char pl8_cmpxchg(char *ptr, char x, char y)
{
	return pl_cmpxchg(ptr, x, y);
}

short pl16_cmpxchg(short *ptr, short x, short y)
{
	return pl_cmpxchg(ptr, x, y);
}

int pl32_cmpxchg(int *ptr, int x, int y)
{
	return pl_cmpxchg(ptr, x, y);
}

long long pl64_cmpxchg(long long *ptr, long long x, long long y)
{
	return pl_cmpxchg(ptr, x, y);
}

