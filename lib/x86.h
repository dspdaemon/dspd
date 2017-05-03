#ifndef _DSPD_X86_H_
#define _DSPD_X86_H_

#define KL_FUTEX

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

#define dspd_cas_intptr(_addr, _oldval, _newval) AO_compare_and_swap(_addr, _oldval, _newval)
#define dspd_nop() AO_nop_full()

static inline AO_TS_t dspd_ts_read(volatile AO_TS_t *addr)
{
  AO_TS_t ret = *addr;
  return ret;
}



#define DSPD_HAVE_ATOMIC_OR
#define dspd_atomic_or(_addr, _val) AO_or_full((volatile AO_t*)_addr, _val)

#define DSPD_HAVE_ATOMIC_AND
static inline void dspd_atomic_and (volatile uintptr_t *p, uintptr_t value)
{
  __asm__ __volatile__ ("lock; and %1, %0" :
                        "=m" (*p) : "r" (value), "m" (*p)
                        : "memory");
}
#endif
