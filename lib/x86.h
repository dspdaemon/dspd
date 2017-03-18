#ifndef _DSPD_X86_H_
#define _DSPD_X86_H_

#define DSPD_HAVE_CAS

static inline int32_t dspd_cas_int32(volatile int32_t *addr,
				     int32_t old, int32_t new_val) 
{
  char result;
  __asm__ __volatile__("lock; cmpxchg %3, %0; setz %1"
	    	       : "=m"(*addr), "=q"(result)
		       : "m"(*addr), "r" (new_val), "a"(old) : "memory");
  return (int) result;
}

#define dspd_cas_intptr(__addr, __oldval, __newval) AO_compare_and_swap(__addr, __oldval, __newval)
#define dspd_nop() AO_nop_full()

#define dspd_ts_read(__addr) (*__addr)

#define DSPD_HAVE_ATOMIC_OR
#define dspd_atomic_or(__addr, __val) AO_or_full((volatile AO_t*)__addr, __val)

#define DSPD_HAVE_ATOMIC_AND
static inline void dspd_atomic_and (volatile uintptr_t *p, uintptr_t value)
{
  __asm__ __volatile__ ("lock; and %1, %0" :
                        "=m" (*p) : "r" (value), "m" (*p)
                        : "memory");
}
#endif
