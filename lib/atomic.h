#ifndef _DSPD_ATOMIC_H_
#define _DSPD_ATOMIC_H_
#include <atomic_ops.h>
#include <stdint.h>

static inline int dspd_test_bit(const uint8_t *mask, uintptr_t bit)
{
  uint8_t val = (1 << (bit % 8));
  AO_nop_full();
  return mask[bit/8] & val;
}

static inline void dspd_clr_bit(uint8_t *mask, uintptr_t bit)
{
  uint8_t val = ~(1 << (bit % 8));
  AO_nop_full();
  mask[bit/8] &= val;
}

static inline void dspd_set_bit(uint8_t *mask, uintptr_t bit)
{
  uint8_t val = 1 << (bit % 8);
  AO_nop_full();
  mask[bit/8] |= val;
}


union dspd_atomic_float32 {
  volatile AO_t  ival;
  volatile float fval;
};

static inline float dspd_load_float32(volatile union dspd_atomic_float32 *addr)
{
  union dspd_atomic_float32 f;
  f.ival = AO_load(&addr->ival);
  return (float)f.fval;
}

static inline void dspd_store_float32(volatile union dspd_atomic_float32 *addr,
				      float val)
{
  union dspd_atomic_float32 f;
  f.fval = val;
  AO_store(&addr->ival, f.ival);
}

//Should work on most if not all systems
#define dspd_load_uint32(__addr) AO_int_load(__addr)
#define dspd_store_uint32(__addr,__val) AO_int_store(__addr, __val)

//Assumes sizeof(intptr_t)==sizeof(AO_t)
#define dspd_store_uintptr(__addr, __val) AO_store((volatile AO_t*)__addr, __val)
#define dspd_load_uintptr(__addr) AO_load((volatile AO_t*)__addr)

#define dspd_ts_t AO_TS_t

#ifdef AO_HAVE_fetch_and_add_full
#define DSPD_HAVE_ATOMIC_INCDEC
static inline uintptr_t dspd_atomic_inc(volatile uintptr_t *addr)
{
  return AO_fetch_and_add1((volatile AO_t*)addr);
}
static inline uintptr_t dspd_atomic_dec(volatile uintptr_t *addr)
{
  return AO_fetch_and_sub1((volatile AO_t*)addr);
}
#endif

#ifdef AO_AO_TS_T
#define dspd_ts_load(__addr) AO_load(__addr)
#else
#define dspd_ts_load(__addr) AO_char_load(__addr)
#endif
#define dspd_ts_clear(__addr) AO_CLEAR(__addr)
#define dspd_test_and_set(__addr) AO_test_and_set(__addr)
#define DSPD_TS_SET AO_TS_SET

#ifndef DSPD_WORDSIZE
#if defined(UINTPTR_MAX) && defined(UINT64_MAX) && (UINTPTR_MAX == UINT64_MAX)
#define DSPD_WORDSIZE              64
#define DSPD_HWORDMASK 0xFFFFFFFF
#else
#define DSPD_WORDSIZE              32
#define DSPD_HWORDMASK 0xFFFF
#endif
#endif

#if defined(__i386) || defined(__x86_64)
#include "x86.h"
#else
#if DSPD_WORDSIZE == 32
#include "generic32.h"
#endif
#endif

#define dspd_test_and_set(__addr) AO_test_and_set(__addr)
#define dspd_test_and_set_clear(__addr) AO_CLEAR(__addr)

#endif
