#ifndef _DSPD_ARM_H_
#define _DSPD_ARM_H_
/*
  This is for new CPUs which have FPUs and generally don't use
  the SWP instruction.  Anything else is probably too slow.
*/
static inline AO_TS_t dspd_ts_read(volatile AO_TS_t *addr)
{
  AO_TS_t ret = *addr;
  return ret;
}

#define dspd_nop() AO_nop_full()
static inline int32_t dspd_cas_int32(volatile int32_t *addr,
				     int32_t old, int32_t new_val)
{
  return AO_compare_and_swap((volatile AO_t*)addr, (AO_t)old, (AO_t)new_val);
}
#define KL_FUTEX
#endif
