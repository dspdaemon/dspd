/*
 *  PCM - PCM floating point conversion routines
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as
 *   published by the Free Software Foundation; either version 2.1 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <byteswap.h>
#include <math.h>
#include "pcm.h"
#include "util.h"

#if defined(__x86_64) || defined(i386)
#ifdef __SSE3__
static inline long _float2int(float64 f)
{  
  long i;
  __asm__ __volatile__ ( "fisttpl %0" : "=m" (i) : "t" (f) : "st" );
  return i;
}
#else
static inline long _float2int(float64 f)
{	
  long i;
  __asm__ __volatile__ ("fistpl %0" : "=m" (i) : "t" (f) : "st");
  return i;
}
#endif
#else
#define _float2int(x) lrint(x)
#endif



union val32 {
  int32_t  i32;
  uint32_t u32;
  char     c[4];
};

static inline void int32_to_24(int32_t in, char *out)
{
  union val32 v;
  v.i32 = in / 256;

#if __BYTE_ORDER == __LITTLE_ENDIAN
  out[0] = v.c[0];
  out[1] = v.c[1];
  out[2] = v.c[2];
#else
  out[3] = v.c[0];
  out[2] = v.c[1];
  out[1] = v.c[2];
#endif
}

static inline void bswap_24(const char in[3], char out[3])
{
  out[0] = in[2];
  out[1] = in[1];
  out[2] = in[0];
}

//Output is always little endian
static inline int32_t int24p_to_int32(const char *val)
{
  //int i[3];
  union val32 v;
#if __BYTE_ORDER == __LITTLE_ENDIAN
  v.c[0] = val[0];
  v.c[1] = val[1];
  v.c[2] = val[2];
  v.c[3] = 0;
#else
  v.c[3] = val[0];
  v.c[2] = val[1];
  v.c[1] = val[2];
  v.c[0] = 0;
#endif
  return (v.i32 * 256);
}



#define conv_ptrs(idx, type, order) [idx]={				\
    .tofloat32 = (dspd_tofloat32_t)type####order##_to_float32_array,	\
    .tofloat64 = (dspd_tofloat64_t)type####order##_to_float64_array,	\
    .tofloat64wv = (dspd_tofloat64wv_t)type####order##_to_float64_array_wv, \
    .tofloat32wv = (dspd_tofloat32wv_t)type####order##_to_float32_array_wv, \
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_##type##_array,	\
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_##type##_array,	\
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_##type##_array_wv,	\
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_##type##_array_wv,	\
  }

#define signed_to_float(itype, otype, umax)				\
  void itype##ne_to_##otype##_array(const itype##_t *in, otype *out, size_t len) { \
    size_t i; otype f; u##itype##_t val;					\
    for ( i = 0; i < len; i++ ) {					\
      val = in[i] ^ (0x80 << ((sizeof(val)-1)*8));			\
      f = val / (umax / 2.0); out[i] = f - 1.0; } }

#define unsigned_to_float(itype, otype, umax)				\
  void u##itype##ne_to_##otype##_array(const itype##_t *in, otype *out, size_t len) { \
    size_t i; otype f;							\
    for ( i = 0; i < len; i++ ) {					\
      f = in[i] / (umax / 2.0); out[i] = f - 1.0; } }

#define oe_unsigned_to_float(itype, otype, umax, bswap)		\
  void u##itype##oe_to_##otype##_array(const itype##_t *in, otype *out, size_t len) { \
    size_t i; otype f;							\
    for ( i = 0; i < len; i++ ) {					\
      f = bswap(in[i]) / (umax / 2.0); out[i] = f - 1.0; } }

#define oe_signed_to_float(itype, otype, umax, bswap)			\
  void itype##oe_to_##otype##_array(const itype##_t *in, otype *out, size_t len) { \
    size_t i; otype f; u##itype##_t val;				\
    for ( i = 0; i < len; i++ ) {					\
      val = bswap(in[i]) ^ (0x80 << ((sizeof(val)-1)*8));		\
      f = val / (umax / 2.0); out[i] = f - 1.0; } }



#define signed_to_float_wv(itype, otype, umax)				\
  void itype##ne_to_##otype##_array_wv(const itype##_t *in, otype *out, size_t len, float64 volume) { \
    size_t i; otype f; u##itype##_t val;					\
    for ( i = 0; i < len; i++ ) {					\
      val = in[i] ^ (0x80 << ((sizeof(val)-1)*8));			\
      f = val / (umax / 2.0); out[i] = (f - 1.0) * volume; } }

#define unsigned_to_float_wv(itype, otype, umax)				\
  void u##itype##ne_to_##otype##_array_wv(const itype##_t *in, otype *out, size_t len, float64 volume) { \
    size_t i; otype f;							\
    for ( i = 0; i < len; i++ ) {					\
      f = in[i] / (umax / 2.0); out[i] = (f - 1.0) * volume; } }

#define oe_unsigned_to_float_wv(itype, otype, umax, bswap)		\
  void u##itype##oe_to_##otype##_array_wv(const itype##_t *in, otype *out, size_t len, float64 volume) { \
    size_t i; otype f;							\
    for ( i = 0; i < len; i++ ) {					\
      f = bswap(in[i]) / (umax / 2.0); out[i] = (f - 1.0) * volume; } }
#define oe_signed_to_float_wv(itype, otype, umax, bswap)			\
  void itype##oe_to_##otype##_array_wv(const itype##_t *in, otype *out, size_t len, float64 volume) { \
    size_t i; otype f; u##itype##_t val;				\
    for ( i = 0; i < len; i++ ) {					\
      val = in[i] ^ (0x80 << ((sizeof(val)-1)*8));			\
      f = val / (umax / 2.0); out[i] = (f - 1.0) * volume; } }

#define flip_sign(_val,type) ((_val) ^ (0x80 << ((sizeof(type)-1)*8)))

#define float_to_signed(itype, otype, umax)		\
  void itype##_to_##otype##_array(const itype *in, otype##_t *out, size_t len) { \
  float64 sv; size_t i;							\
  for ( i = 0; i < len; i++ ) {						\
    sv = in[i];								\
    if ( sv >= 1.0 )							\
      { out[i] = flip_sign(umax,otype##_t); }					\
    else if ( sv <= -1.0 )						\
      { out[i] = flip_sign(0,otype##_t); }				\
    else {								\
      sv *= (8.0 * 0x10000000);						\
      out[i] = (otype##_t) (_float2int(sv) >> (((sizeof(int32_t)-sizeof(otype##_t))*8))); \
    }									\
  }									}

#define float_to_signed_oe(itype, otype, umax, bswap)				\
  void itype##_to_##otype##oe_array(const itype *in, otype##_t *out, size_t len) { \
  float64 sv; size_t i;							\
  otype##_t val;							\
  for ( i = 0; i < len; i++ ) {						\
    sv = in[i];								\
    if ( sv >= 1.0 )							\
      { val = flip_sign(umax,otype##_t); }					\
    else if ( sv <= -1.0 )						\
      { val = flip_sign(0,otype##_t); }				\
    else {								\
      sv *= (8.0 * 0x10000000);						\
      val = (otype##_t) (_float2int(sv) >> (((sizeof(int32_t)-sizeof(otype##_t))*8))); \
    }									\
    out[i] = bswap(val);						\
									\
  }									}



#define float_to_unsigned(itype, otype, umax)	\
  void itype##_to_u##otype##_array(const itype *in, u##otype##_t *out, size_t len) { \
  float64 sv; size_t i; otype##_t o;						\
  for ( i = 0; i < len; i++ ) {						\
    sv = in[i];								\
    if ( sv >= 1.0 )							\
      { out[i] = umax; }						\
    else if ( sv <= -1.0 )						\
      { out[i] = 0; }							\
    else {								\
      sv *= (8.0 * 0x10000000);						\
      o = (u##otype##_t) (_float2int(sv) >> (((sizeof(int32_t)-sizeof(otype##_t))*8))); \
      out[i] = flip_sign(o,otype##_t);						\
    }									\
  }}		

#define float_to_signed_wv(itype, otype, umax)		\
  void itype##_to_##otype##_array_wv(const itype *in, otype##_t *out, size_t len, float64 volume) { \
  float64 sv; size_t i;							\
  for ( i = 0; i < len; i++ ) {						\
    sv = in[i] * volume;								\
    if ( sv >= 1.0 )							\
      { out[i] = flip_sign(umax,otype##_t); }					\
    else if ( sv <= -1.0 )						\
      { out[i] = flip_sign(0,otype##_t); }				\
    else {								\
      sv *= (8.0 * 0x10000000);						\
      out[i] = (otype##_t) (_float2int(sv) >> (((sizeof(int32_t)-sizeof(otype##_t))*8))); \
    }									\
  }									}

#define float_to_unsigned_wv(itype, otype, umax)	\
  void itype##_to_u##otype##_array_wv(const itype *in, u##otype##_t *out, size_t len, float64 volume) { \
  float64 sv; size_t i; u##otype##_t o;						\
  for ( i = 0; i < len; i++ ) {						\
    sv = in[i] * volume;								\
    if ( sv >= 1.0 )							\
      { out[i] = umax; }						\
    else if ( sv <= -1.0 )						\
      { out[i] = 0; }							\
    else {								\
      sv *= (8.0 * 0x10000000);						\
      o = (u##otype##_t) (_float2int(sv) >> (((sizeof(int32_t)-sizeof(otype##_t))*8))); \
      out[i] = flip_sign(o,otype##_t);						\
    }									\
  }}	

#define float_to_unsigned_oe_wv(itype, otype, umax, bswap)			\
  void itype##_to_u##otype##oe_array_wv(const itype *in, u##otype##_t *out, size_t len, float64 volume) { \
    float64 sv; size_t i; u##otype##_t o, val;					\
  for ( i = 0; i < len; i++ ) {						\
    sv = in[i] * volume;								\
    if ( sv >= 1.0 )							\
      { val = umax; }						\
    else if ( sv <= -1.0 )						\
      { val = 0; }							\
    else {								\
      sv *= (8.0 * 0x10000000);						\
      o = (u##otype##_t) (_float2int(sv) >> (((sizeof(int32_t)-sizeof(u##otype##_t))*8))); \
      val = flip_sign(o,u##otype##_t);					\
    }									\
    out[i] = bswap(val);						\
  }									\
  }	
#define float_to_unsigned_oe(itype, otype, umax, bswap)			\
  void itype##_to_u##otype##oe_array(const itype *in, u##otype##_t *out, size_t len) { \
    float64 sv; size_t i; u##otype##_t o, val;					\
  for ( i = 0; i < len; i++ ) {						\
    sv = in[i];								\
    if ( sv >= 1.0 )							\
      { val = umax; }						\
    else if ( sv <= -1.0 )						\
      { val = 0; }							\
    else {								\
      sv *= (8.0 * 0x10000000);						\
      o = (u##otype##_t) (_float2int(sv) >> (((sizeof(int32_t)-sizeof(otype##_t))*8))); \
      val = flip_sign(o,otype##_t);					\
    }									\
    out[i] = bswap(val);						\
  }									\
  }	

#define float_to_signed_oe_wv(itype, otype, umax, bswap)		\
  void itype##_to_##otype##oe_array_wv(const itype *in, otype##_t *out, size_t len, float64 volume) { \
    float64 sv; size_t i; otype##_t val;					\
  for ( i = 0; i < len; i++ ) {						\
    sv = in[i] * volume;						\
    if ( sv >= 1.0 )							\
      { val = flip_sign(umax,otype##_t); }				\
    else if ( sv <= -1.0 )						\
      { val = flip_sign(0,otype##_t); }					\
    else {								\
      sv *= (8.0 * 0x10000000);						\
      val = (otype##_t) (_float2int(sv) >> (((sizeof(int32_t)-sizeof(otype##_t))*8))); \
    }									\
    out[i] = bswap(val);						\
  }									\
  }	

static void int24ple_to_float32_array(const char *in, float *out, size_t len)
{
  size_t i;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      out[i/3] = (float) (int24p_to_int32(&in[i]) / (8.0 * 0x10000000));
      
    }
}

static void int24ple_to_float64_array(const char *in, float64 *out, size_t len)
{
  size_t i;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      out[i/3] = (float64) (int24p_to_int32(&in[i]) / (8.0 * 0x10000000));
      
    }
}

static void int24ple_to_float64_array_wv(const char *in, float64 *out, size_t len, float64 volume)
{
  size_t i;
  float64 sv;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (float64)(int24p_to_int32(&in[i]) / (8.0 * 0x10000000));
      sv *= volume;
      out[i/3] = sv;
    }
}

static void int24ple_to_float32_array_wv(const char *in, float32 *out, size_t len, float64 volume)
{
  size_t i;
  float64 sv;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (float32) (int24p_to_int32(&in[i]) / (8.0 * 0x10000000));
      sv *= volume;
      out[i/3] = sv;
    }
}


static void uint24ple_to_float32_array(const char *in, float32 *out, size_t len)
{
  size_t i;
  int32_t val;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      val = int24p_to_int32(&in[i]) ^ 0x80000000;
      out[i/3] = (float32)(val / (8.0 * 0x10000000));
    }
}

static void uint24ple_to_float64_array(const char *in, float64 *out, size_t len)
{
  size_t i;
  int32_t val;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      val = int24p_to_int32(&in[i]) ^ 0x80000000;
      out[i/3] = (float64)(val / (8.0 * 0x10000000));
    }
}


static void uint24ple_to_float64_array_wv(const char *in, float64 *out, size_t len, float64 volume)
{
  size_t i;
  int32_t val;
  float64 sv;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      val = int24p_to_int32(&in[i]) ^ 0x80000000;
      sv = (float64)(val / (8.0 * 0x10000000));
      sv *= volume;
      out[i/3] = sv;
    }
}

static void uint24ple_to_float32_array_wv(const char *in, float32 *out, size_t len, float64 volume)
{
  size_t i;
  int32_t val;
  float64 sv;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      val = int24p_to_int32(&in[i]) ^ 0x80000000;
      sv = (float32)(val / (8.0 * 0x10000000));
      sv *= volume;
      out[i/3] = sv;
    }
}

static void int24pbe_to_float32_array(const char *in, float32 *out, size_t len)
{
  size_t i;
  char c[3];
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      c[0] = in[i+2];
      c[1] = in[i+1];
      c[2] = in[i];
      out[i/3] = (float32)(int24p_to_int32(c) / (8.0 * 0x10000000));
      
    }
}
static void int24pbe_to_float64_array(const char *in, float64 *out, size_t len)
{
  size_t i;
  char c[3];
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      c[0] = in[i+2];
      c[1] = in[i+1];
      c[2] = in[i];
      out[i/3] = (float64)(int24p_to_int32(c) / (8.0 * 0x10000000));
    }
}

static void int24pbe_to_float64_array_wv(const char *in, float64 *out, size_t len, float64 volume)
{
  size_t i;
  char c[3];
  float64 sv;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      c[0] = in[i+2];
      c[1] = in[i+1];
      c[2] = in[i];
      sv = (float64)(int24p_to_int32(c) / (8.0 * 0x10000000));
      sv *= volume;
      out[i/3] = sv;
    }
}
static void int24pbe_to_float32_array_wv(const char *in, float32 *out, size_t len, float64 volume)
{
  size_t i;
  char c[3];
  float64 sv;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      c[0] = in[i+2];
      c[1] = in[i+1];
      c[2] = in[i];
      sv = (float32)(int24p_to_int32(c) / (8.0 * 0x10000000));
      sv *= volume;
      out[i/3] = sv;
    }
}


static void uint24pbe_to_float32_array(const char *in, float32 *out, size_t len)
{
  size_t i;
  int32_t val;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      val = int24p_to_int32(&in[i]) ^ 0x80000000;
      out[i/3] = (float32)(val / (8.0 * 0x10000000));
    }
}

static void uint24pbe_to_float64_array(const char *in, float64 *out, size_t len)
{
  size_t i;
  int32_t val;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      val = int24p_to_int32(&in[i]) ^ 0x80000000;
      out[i/3] = (float64)(val / (8.0 * 0x10000000));
    }
}


static void uint24pbe_to_float32_array_wv(const char *in, float32 *out, size_t len, float64 volume)
{
  size_t i;
  int32_t val;
  float64 sv;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      val = int24p_to_int32(&in[i]) ^ 0x80000000;
      sv = (float32)(val / (8.0 * 0x10000000));
      sv *= volume;
      out[i/3] = sv;
    }
}

static void uint24pbe_to_float64_array_wv(const char *in, float64 *out, size_t len, float64 volume)
{
  size_t i;
  int32_t val;
  float64 sv;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      val = int24p_to_int32(&in[i]) ^ 0x80000000;
      sv = (float64)(val / (8.0 * 0x10000000));
      sv *= volume;
      out[i/3] = sv;
    }
}

static void float32_to_int24ple_array(const float32 *in, char *out, size_t len)
{
  int32_t val;
  float64 sv;
  size_t i;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = in[i/3] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      int32_to_24(val, &out[i]);
    }
}

static void float64_to_int24ple_array(const float64 *in, char *out, size_t len)
{
  int32_t val;
  float64 sv;
  size_t i;
  len *= 3;
  
  for ( i = 0; i < len; i += 3 )
    {
      sv = in[i/3] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 

      int32_to_24(val, &out[i]);
    }
}


static void float32_to_int24ple_array_wv(const float32 *in, char *out, size_t len, float64 volume)
{
  int32_t val;
  float64 sv;
  size_t i;
  len *= 3;
  
  for ( i = 0; i < len; i += 3 )
    {
      sv = (in[i/3] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 

      int32_to_24(val, &out[i]);
    }
}

static void float64_to_int24ple_array_wv(const float64 *in, char *out, size_t len, float64 volume)
{
  int32_t val;
  float64 sv;
  size_t i;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (in[i/3]*volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      int32_to_24(val, &out[i]);
    }
}



static void float64_to_uint24ple_array(const float64 *in, char *out, size_t len)
{
  int32_t val;
  float64 sv;
  size_t i;
  len *= 3;
  
  for ( i = 0; i < len; i += 3 )
    {
      sv = in[i/3] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      val ^= 0x80000000;
      int32_to_24(val, &out[i]);
    }
}
static void float32_to_uint24ple_array(const float32 *in, char *out, size_t len)
{
  int32_t val;
  float64 sv;
  size_t i;
  len *= 3;
  
  for ( i = 0; i < len; i += 3 )
    {
      sv = in[i/3] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      val ^= 0x80000000;
      int32_to_24(val, &out[i]);
    }
}

static void float32_to_int24pbe_array(const float *in, char *out, size_t len)
{
  int32_t val;
  float64 sv;
  size_t i;
  char c[3];
  len *= 3;
  
  for ( i = 0; i < len; i += 3 )
    {
      sv = in[i/3] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      int32_to_24(val, c);
      out[i] = c[2];
      out[i+1] = c[1];
      out[i+2] = c[0];
    }
}

static void float64_to_int24pbe_array(const float *in, char *out, size_t len)
{
  int32_t val;
  float64 sv;
  size_t i;
  char c[3];
  len *= 3;
  
  for ( i = 0; i < len; i += 3 )
    {
      sv = in[i/3] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      int32_to_24(val, c);
      out[i] = c[2];
      out[i+1] = c[1];
      out[i+2] = c[0];
    }
}

static void int24ne_to_float32_array(const int32_t *in, float32 *out, size_t len)
{
  int32_t sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      sv = in[i] * 256;
      out[i] = (float32)(sv / (8.0 * 0x10000000));
    }
}

static void int24ne_to_float64_array(const int32_t *in, float64 *out, size_t len)
{
  int32_t sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      sv = in[i] * 256;
      out[i] = (float64) (sv / (8.0 * 0x10000000));
    }
}

static void int24ne_to_float64_array_wv(const int32_t *in, float64 *out, size_t len, float64 volume)
{
  int32_t sv;
  size_t i;
  float64 v;
  for ( i = 0; i < len; i++ )
    {
      sv = in[i] * 256;
      v = (float64)(sv / (8.0 * 0x10000000));
      v *= volume;
      out[i] = v;
    }
}


static void int24ne_to_float32_array_wv(const int32_t *in, float32 *out, size_t len, float64 volume)
{
  int32_t sv;
  size_t i;
  float64 v;
  for ( i = 0; i < len; i++ )
    {
      sv = in[i] * 256;
      v = (float32)(sv / (8.0 * 0x10000000));
      v *= volume;
      out[i] = v;
    }
}


static void int24oe_to_float32_array(const int32_t *in, float32 *out, size_t len)
{
  int32_t sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      sv = bswap_32(in[i]) * 256;
      out[i] = (float32)(sv / (8.0 * 0x10000000));
    }
}
static void int24oe_to_float64_array(const int32_t *in, float64 *out, int len)
{
  int32_t sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      sv = bswap_32(in[i]) * 256;
      out[i] = (float64)(sv / (8.0 * 0x10000000));
    }
}

static void int24oe_to_float32_array_wv(const int32_t *in, float32 *out, size_t len, float64 volume)
{
  int32_t sv;
  size_t i;
  float64 v;
  for ( i = 0; i < len; i++ )
    {
      sv = bswap_32(in[i]) * 256;
      v = (float32)(sv / (8.0 * 0x10000000));
      v *= volume;
      out[i] = v;
    }
}
static void int24oe_to_float64_array_wv(const int32_t *in, float64 *out, int len, float64 volume)
{
  int32_t sv;
  size_t i;
  float64 v;
  for ( i = 0; i < len; i++ )
    {
      sv = bswap_32(in[i]) * 256;
      v = (float64)(sv / (8.0 * 0x10000000));
      v *= volume;
      out[i] = v;
    }
}



static void uint24ne_to_float32_array(const int32_t *in, float32 *out, size_t len)
{
  int32_t sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      sv = in[i] * 256;
      sv ^= 0x80000000;
      out[i] = (float32)(sv / (8.0 * 0x10000000));
    }
}

static void uint24ne_to_float64_array(const int32_t *in, float64 *out, size_t len)
{
  int32_t sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      sv = in[i] * 256;
      sv ^= 0x80000000;
      out[i] = (float64)(sv / (8.0 * 0x10000000));
    }
}


static void uint24ne_to_float32_array_wv(const int32_t *in, float32 *out, size_t len, float64 volume)
{
  int32_t sv;
  size_t i;
  float64 v;
  for ( i = 0; i < len; i++ )
    {
      sv = in[i] * 256;
      sv ^= 0x80000000;
      v = (float32)(sv / (8.0 * 0x10000000));
      v *= volume;
      out[i] = v;
    }
}

static void uint24ne_to_float64_array_wv(const int32_t *in, float64 *out, size_t len, float64 volume)
{
  int32_t sv;
  size_t i;
  float64 v;
  for ( i = 0; i < len; i++ )
    {
      sv = in[i] * 256;
      sv ^= 0x80000000;
      v = (float64)(sv / (8.0 * 0x10000000));
      v *= volume;
      out[i] = v;
    }
}


static void uint24oe_to_float64_array(const int32_t *in, float64 *out, size_t len)
{
  int32_t sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      sv = bswap_32(in[i]) * 256;
      sv ^= 0x80000000;
      out[i] = (float64)(sv / (8.0 * 0x10000000));
    }
}

static void uint24oe_to_float32_array(const int32_t *in, float32 *out, size_t len)
{
  int32_t sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      sv = bswap_32(in[i]) * 256;
      sv ^= 0x80000000;
      out[i] = (float32)(sv / (8.0 * 0x10000000));
    }
}

static void uint24oe_to_float64_array_wv(const int32_t *in, float64 *out, size_t len, float64 volume)
{
  int32_t sv;
  size_t i;
  float64 v;
  for ( i = 0; i < len; i++ )
    {
      sv = bswap_32(in[i]) * 256;
      sv ^= 0x80000000;
      v = (float64)(sv / (8.0 * 0x10000000));
      v *= volume;
      out[i] = v;
    }
}
static void uint24oe_to_float32_array_wv(const int32_t *in, float32 *out, size_t len, float64 volume)
{
  int32_t sv;
  size_t i;
  float64 v;
  for ( i = 0; i < len; i++ )
    {
      sv = bswap_32(in[i]) * 256;
      sv ^= 0x80000000;
      v = (float32)(sv / (8.0 * 0x10000000));
      v *= volume;
      out[i] = v;
    }
}


static void float32_to_int24ne_array(const float32 *in, int32_t *out, size_t len)
{
  float64 sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = in[i] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  out[i] = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  out[i] = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  out[i] = _float2int(sv) / 256;
	}
    }
}

static void float64_to_int24ne_array(const float64 *in, int32_t *out, size_t len)
{
  float64 sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = in[i] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  out[i] = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  out[i] = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  out[i] = _float2int(sv) / 256;
	}
    }
}


static void float32_to_int24ne_array_wv(const float32 *in, int32_t *out, size_t len, float64 volume)
{
  float64 sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = (in[i] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  out[i] = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  out[i] = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  out[i] = _float2int(sv) / 256;
	}
    }
}

static void float64_to_int24ne_array_wv(const float64 *in, int32_t *out, size_t len, float64 volume)
{
  float64 sv;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = (in[i] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  out[i] = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  out[i] = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  out[i] = _float2int(sv) / 256;
	}
    }
}




static void float64_to_int24oe_array(const float64 *in, int32_t *out, size_t len)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = in[i] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      out[i] = bswap_32(o);
    }
}


static void float32_to_int24oe_array_wv(const float32 *in, int32_t *out, size_t len, float64 volume)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = (in[i] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      out[i] = bswap_32(o);
    }
}

static void float64_to_int24oe_array_wv(const float64 *in, int32_t *out, size_t len, float64 volume)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = (in[i] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      out[i] = bswap_32(o);
    }
}

static void float32_to_uint24ne_array(const float32 *in, int32_t *out, size_t len)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = in[i] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      out[i] = o ^ 0x80000000;
    }
}


static void float64_to_uint24ne_array(const float64 *in, int32_t *out, size_t len)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = in[i] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      out[i] = o ^ 0x80000000;
    }
}


static void float32_to_uint24oe_array(const float32 *in, int32_t *out, size_t len)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = in[i] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      o ^= 0x80000000;
      out[i] = bswap_32(o);
    }
}

static void float64_to_uint24oe_array(const float64 *in, int32_t *out, size_t len)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = in[i] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      o ^= 0x80000000;
      out[i] = bswap_32(o);
    }
}



static void float64_to_uint24ple_array_wv(const float64 *in, 
					  char *out, 
					  int len, 
					  float64 volume)
{
  int32_t val;
  float64 sv;
  int i;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (in[i/3] * volume) * (8.0 * 0x10000000);
      if ( sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      val ^= 0x80000000;
      int32_to_24(val, &out[i]);
    }
}

static void float32_to_uint24ple_array_wv(const float32 *in, 
					  char *out, 
					  int len, 
					  float64 volume)
{
  int32_t val;
  float64 sv;
  int i;
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (in[i/3] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      val ^= 0x80000000;
      int32_to_24(val, &out[i]);
    }
}

static void float64_to_uint24pbe_array_wv(const float64 *in, 
					  char *out, 
					  int len, 
					  float64 volume)
{
  int32_t val;
  float64 sv;
  int i;
  char c[3];
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (in[i/3] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      val ^= 0x80000000;
      int32_to_24(val, c);
      out[i] = c[2];
      out[i+1] = c[1];
      out[i+2] = c[0];
    }
}

static void float32_to_uint24pbe_array_wv(const float32 *in, 
					  char *out, 
					  int len, 
					  float64 volume)
{
  int32_t val;
  float64 sv;
  int i;
  char c[3];
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (in[i/3] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      val ^= 0x80000000;
      int32_to_24(val, c);
      out[i] = c[2];
      out[i+1] = c[1];
      out[i+2] = c[0];
    }
}

static void float32_to_uint24pbe_array(const float32 *in, 
				       char *out, 
				       int len) 
{
  int32_t val;
  float64 sv;
  int i;
  char c[3];
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = in[i/3]  * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      val ^= 0x80000000;
      int32_to_24(val, c);
      out[i] = c[2];
      out[i+1] = c[1];
      out[i+2] = c[0];
    }
}

static void float64_to_uint24pbe_array(const float64 *in, 
				       char *out, 
				       int len) 
{
  int32_t val;
  float64 sv;
  int i;
  char c[3];
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = in[i/3]  * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      val ^= 0x80000000;
      int32_to_24(val, c);
      out[i] = c[2];
      out[i+1] = c[1];
      out[i+2] = c[0];
    }
}


static void float64_to_int24pbe_array_wv(const float64 *in, 
					 char *out, 
					 int len, 
					 float64 volume)
{
  int32_t val;
  float64 sv;
  int i;
  char c[3];
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (in[i/3] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      int32_to_24(val, c);
      out[i] = c[2];
      out[i+1] = c[1];
      out[i+2] = c[0];
    }
}

static void float32_to_int24pbe_array_wv(const float32 *in, 
					 char *out, 
					 int len, 
					 float64 volume)
{
  int32_t val;
  float64 sv;
  int i;
  char c[3];
  len *= 3;
  for ( i = 0; i < len; i += 3 )
    {
      sv = (in[i/3] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{	
	  val = 0x7fffffff;
	} else if (sv <= (-8.0 * 0x10000000))
	{	
	  val = -1 - 0x7fffffff;
	} else
	{
	  val = _float2int(sv);
	} 
      int32_to_24(val, c);
      out[i] = c[2];
      out[i+1] = c[1];
      out[i+2] = c[0];
    }
}








static void float32_to_int24oe_array(const float32 *in, 
				     int32_t *out, 
				     size_t len)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = in[i] * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      out[i] = bswap_32(o);
    }
}


static void float64_to_uint24ne_array_wv(const float64 *in, 
					 int32_t *out, 
					 size_t len,
					 float64 volume)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = (in[i] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      out[i] = o ^ 0x80000000;
    }
}

static void float32_to_uint24ne_array_wv(const float32 *in, 
					 int32_t *out, 
					 size_t len,
					 float64 volume)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = (in[i] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      out[i] = o ^ 0x80000000;
    }
}


static void float64_to_uint24oe_array_wv(const float64 *in, 
					 int32_t *out, 
					 size_t len,
					 float64 volume)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = (in[i] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      o ^= 0x80000000;
      out[i] = bswap_32(o);
    }
}

static void float32_to_uint24oe_array_wv(const float32 *in, 
					 int32_t *out, 
					 size_t len,
					 float64 volume)
{
  float64 sv;
  int32_t o;
  size_t i;
  for ( i = 0; i < len; i++ )
    {	
      sv = (in[i] * volume) * (8.0 * 0x10000000);
      if (sv >= (1.0 * 0x7FFFFFFF))
	{
	  o = 0x7fffffff / 256;
	} else if (sv <= (-8.0 * 0x10000000))
	{
	  o = (-1 - 0x7fffffff) / 256;
	} else
	{ 
	  o = _float2int(sv) / 256;
	}
      o ^= 0x80000000;
      out[i] = bswap_32(o);
    }
}

#define declare_conv(intype, outtype, maxval,bs)			\
  signed_to_float(intype, outtype, maxval);				\
  oe_signed_to_float(intype, outtype, maxval, bs);			\
  unsigned_to_float(intype, outtype, maxval);				\
  oe_unsigned_to_float(intype, outtype, maxval, bs);			\
  signed_to_float_wv(intype, outtype, maxval);				\
  oe_signed_to_float_wv(intype, outtype, maxval, bs);			\
  unsigned_to_float_wv(intype, outtype, maxval);			\
  oe_unsigned_to_float_wv(intype, outtype, maxval, bs);			\
  float_to_unsigned(outtype, intype, maxval);				\
  float_to_signed(outtype, intype, maxval);				\
  float_to_unsigned_wv(outtype, intype, maxval);			\
  float_to_signed_wv(outtype, intype, maxval);				

  declare_conv(int16, float32, UINT16_MAX, bswap_16);
  declare_conv(int32, float32, UINT32_MAX, bswap_32);
  declare_conv(int16, float64, UINT16_MAX, bswap_16);
  declare_conv(int32, float64, UINT32_MAX, bswap_32);

  unsigned_to_float(int8, float32, UINT8_MAX);
  unsigned_to_float_wv(int8, float32, UINT8_MAX);
  unsigned_to_float(int8, float64, UINT8_MAX);
  unsigned_to_float_wv(int8, float64, UINT8_MAX);


  signed_to_float(int8, float32, UINT8_MAX);
  signed_to_float_wv(int8, float32, UINT8_MAX);
  signed_to_float(int8, float64, UINT8_MAX);
  signed_to_float_wv(int8, float64, UINT8_MAX);
  float_to_signed_wv(float32, int8, UINT8_MAX);
  float_to_unsigned_wv(float32, int8, UINT8_MAX);
  float_to_signed_wv(float64, int8, UINT8_MAX);
  float_to_unsigned_wv(float64, int8, UINT8_MAX);
  float_to_signed(float64, int8, UINT8_MAX);
  float_to_unsigned(float64, int8, UINT8_MAX);
  float_to_signed(float32, int8, UINT8_MAX);
  float_to_unsigned(float32, int8, UINT8_MAX);

float_to_unsigned_oe_wv(float32, int16, UINT16_MAX, bswap_16);
float_to_unsigned_oe(float32, int16, UINT16_MAX, bswap_16);
float_to_unsigned_oe_wv(float64, int16, UINT16_MAX, bswap_16);
float_to_unsigned_oe(float64, int16, UINT16_MAX, bswap_16);
float_to_signed_oe(float64, int16, UINT16_MAX, bswap_16);
float_to_signed_oe(float32, int16, UINT16_MAX, bswap_16);
float_to_signed_oe_wv(float32, int16, UINT16_MAX, bswap_16);
float_to_signed_oe_wv(float64, int16, UINT16_MAX, bswap_16);

float_to_unsigned_oe_wv(float32, int32, UINT32_MAX, bswap_32);
float_to_unsigned_oe(float32, int32, UINT32_MAX, bswap_32);
float_to_unsigned_oe_wv(float64, int32, UINT32_MAX, bswap_32);
float_to_unsigned_oe(float64, int32, UINT32_MAX, bswap_32);
float_to_signed_oe(float64, int32, UINT32_MAX, bswap_32);
float_to_signed_oe(float32, int32, UINT32_MAX, bswap_32);
float_to_signed_oe_wv(float32, int32, UINT32_MAX, bswap_32);
float_to_signed_oe_wv(float64, int32, UINT32_MAX, bswap_32);

//TODO: 24 bit

void float32_to_float32_array(const float32 *in, float32 *out, size_t len)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    out[i] = in[i];
}
void float32_to_float32_array_wv(const float32 *in, float32 *out, size_t len, float32 volume)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    out[i] = in[i] * volume;
}

void float64_to_float32_array(const float64 *in, float32 *out, size_t len)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    out[i] = in[i];
}
void float32_to_float64_array_wv(const float32 *in, float64 *out, size_t len, float64 volume)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    out[i] = in[i] * volume;
}

void float32_to_float64_array(const float32 *in, float64 *out, size_t len)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    out[i] = in[i];
}

void float64_to_float32_array_wv(const float64 *in, float32 *out, size_t len, float32 volume)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    out[i] = in[i] * volume;
}

static inline int val_seg_alaw(int val)
{
  int r = 1;
  val >>= 8;
  if (val & 0xf0) {
    val >>= 4;
    r += 4;
  }
  if (val & 0x0c) {
    val >>= 2;
    r += 2;
  }
  if (val & 0x02)
    r += 1;
  return r;
}

static inline float alaw_to_float(unsigned char a_val)
{
  int           t;
  int           seg;

  a_val ^= 0x55;
  t = a_val & 0x7f;
  if (t < 16)
    t = (t << 4) + 8;
  else {
    seg = (t >> 4) & 0x07;
    t = ((t & 0x0f) << 4) + 0x108;
    t <<= seg -1;
  }
  return (((a_val & 0x80) ? t : -t) / (1.0 * 0x8000)) ;
}

static inline unsigned char s16_to_alaw(int pcm_val)
{
  int mask;
  int seg;
  unsigned char aval;

  if (pcm_val >= 0) {
    mask = 0xD5;
  } else {
    mask = 0x55;
    pcm_val = -pcm_val;
    if (pcm_val > 0x7fff)
      pcm_val = 0x7fff;
  }

  if (pcm_val < 256)
    aval = pcm_val >> 4;
  else {
    /* Convert the scaled magnitude to segment number. */
    seg = val_seg_alaw(pcm_val);
    aval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0x0f);
  }
  return aval ^ mask;
}


static void alaw_to_float32_array(const void *in, float32 *out, size_t len)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      out[i] = alaw_to_float(((unsigned char*)in)[i]);
    }
}

static void alaw_to_float64_array(const void *in, double *out, size_t len)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      out[i] = alaw_to_float(((unsigned char*)in)[i]);
    }
}

static void alaw_to_float32_array_wv(const void *in, float32 *out, size_t len, float64 volume)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      out[i] = alaw_to_float(((unsigned char*)in)[i]) * volume;
    }
}

static void alaw_to_float64_array_wv(const void *in, float64 *out, size_t len, float64 volume)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      out[i] = alaw_to_float(((unsigned char*)in)[i]) * volume;
    }
}


static void float32_to_alaw_array(const float32 *in, void *out, size_t len)
{
  size_t i;
  int32_t val;
  uint32_t v;
  int16_t val16;
  for ( i = 0; i < len; i++ )
    {
      val = _float2int(in[i]);
      v = val ^ 0x80000000;
      val16 = ((v / (UINT32_MAX/UINT16_MAX)) ^ 0x80000000);
      ((unsigned char*)out)[i] = s16_to_alaw(val16);
    }
}
static void float64_to_alaw_array(const float64 *in, void *out, size_t len)
{
  size_t i;
  int32_t val;
  uint32_t v;
  int16_t val16;
  for ( i = 0; i < len; i++ )
    {
      val = _float2int(in[i]);
      v = val ^ 0x80000000;
      val16 = ((v / (UINT32_MAX/UINT16_MAX)) ^ 0x80000000);
      ((unsigned char*)out)[i] = s16_to_alaw(val16);
    }
}

static void float32_to_alaw_array_wv(const float32 *in, void *out, size_t len, float64 volume)
{
  size_t i;
  int32_t val;
  uint32_t v;
  int16_t val16;
  for ( i = 0; i < len; i++ )
    {
      val = _float2int(in[i] * volume);
      v = val ^ 0x80000000;
      val16 = ((v / (UINT32_MAX/UINT16_MAX)) ^ 0x80000000);
      ((unsigned char*)out)[i] = s16_to_alaw(val16);
    }
}
static void float64_to_alaw_array_wv(const float64 *in, void *out, size_t len, float64 volume)
{
  size_t i;
  int32_t val;
  uint32_t v;
  int16_t val16;
  for ( i = 0; i < len; i++ )
    {
      val = _float2int(in[i] * volume);
      v = val ^ 0x80000000;
      val16 = ((v / (UINT32_MAX/UINT16_MAX)) ^ 0x80000000);
      ((unsigned char*)out)[i] = s16_to_alaw(val16);
    }
}


static inline int val_seg_ulaw(int val)
{
  int r = 0;
  val >>= 7;
  if (val & 0xf0) {
    val >>= 4;
    r += 4;
  }
  if (val & 0x0c) {
    val >>= 2;
    r += 2;
  }
  if (val & 0x02)
    r += 1;
  return r;
}

static inline float ulaw_to_float(unsigned char u_val)
{
  int t;

  /* Complement to obtain normal u-law value. */
  u_val = ~u_val;

  /*
   * Extract and bias the quantization bits. Then
   * shift up by the segment number and subtract out the bias.
   */
  t = ((u_val & 0x0f) << 3) + 0x84;
  t <<= (u_val & 0x70) >> 4;

  return (((u_val & 0x80) ? (0x84 - t) : (t - 0x84)) / (1.0 * 0x8000));
}
static inline unsigned char s16_to_ulaw(int pcm_val)	
/* 2's complement (16-bit range) */
{
	int mask;
	int seg;
	unsigned char uval;

	if (pcm_val < 0) {
		pcm_val = 0x84 - pcm_val;
		mask = 0x7f;
	} else {
		pcm_val += 0x84;
		mask = 0xff;
	}
	if (pcm_val > 0x7fff)
		pcm_val = 0x7fff;

	/* Convert the scaled magnitude to segment number. */
	seg = val_seg_ulaw(pcm_val);

	/*
	 * Combine the sign, segment, quantization bits;
	 * and complement the code word.
	 */
	uval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0x0f);
	return uval ^ mask;
}


static void ulaw_to_float32_array(const void *in, float32 *out, size_t len)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      out[i] = ulaw_to_float(((unsigned char*)in)[i]);
    }
}

static void ulaw_to_float64_array(const void *in, double *out, size_t len)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      out[i] = ulaw_to_float(((unsigned char*)in)[i]);
    }
}

static void ulaw_to_float32_array_wv(const void *in, float32 *out, size_t len, float64 volume)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      out[i] = ulaw_to_float(((unsigned char*)in)[i]) * volume;
    }
}

static void ulaw_to_float64_array_wv(const void *in, float64 *out, size_t len, float64 volume)
{
  size_t i;
  for ( i = 0; i < len; i++ )
    {
      out[i] = ulaw_to_float(((unsigned char*)in)[i]) * volume;
    }
}


static void float32_to_ulaw_array(const float32 *in, void *out, size_t len)
{
  size_t i;
  int32_t val;
  uint32_t v;
  int16_t val16;
  for ( i = 0; i < len; i++ )
    {
      val = _float2int(in[i]);
      v = val ^ 0x80000000;
      val16 = ((v / (UINT32_MAX/UINT16_MAX)) ^ 0x80000000);
      ((unsigned char*)out)[i] = s16_to_ulaw(val16);
    }
}
static void float64_to_ulaw_array(const float64 *in, void *out, size_t len)
{
  size_t i;
  int32_t val;
  uint32_t v;
  int16_t val16;
  for ( i = 0; i < len; i++ )
    {
      val = _float2int(in[i]);
      v = val ^ 0x80000000;
      val16 = ((v / (UINT32_MAX/UINT16_MAX)) ^ 0x80000000);
      ((unsigned char*)out)[i] = s16_to_ulaw(val16);
    }
}

static void float32_to_ulaw_array_wv(const float32 *in, void *out, size_t len, float64 volume)
{
  size_t i;
  int32_t val;
  uint32_t v;
  int16_t val16;
  for ( i = 0; i < len; i++ )
    {
      val = _float2int(in[i] * volume);
      v = val ^ 0x80000000;
      val16 = ((v / (UINT32_MAX/UINT16_MAX)) ^ 0x80000000);
      ((unsigned char*)out)[i] = s16_to_ulaw(val16);
    }
}
static void float64_to_ulaw_array_wv(const float64 *in, void *out, size_t len, float64 volume)
{
  size_t i;
  int32_t val;
  uint32_t v;
  int16_t val16;
  for ( i = 0; i < len; i++ )
    {
      val = _float2int(in[i] * volume);
      v = val ^ 0x80000000;
      val16 = ((v / (UINT32_MAX/UINT16_MAX)) ^ 0x80000000);
      ((unsigned char*)out)[i] = s16_to_ulaw(val16);
    }
}




/*struct pcm_conv {
  dspd_tofloat32_t     tofloat32;
  dspd_tofloat64_t     tofloat64;
  dspd_tofloat64wv_t   tofloat64wv;
  dspd_tofloat32wv_t   tofloat32wv;
  dspd_fromfloat32_t   fromfloat32;
  dspd_fromfloat64_t   fromfloat64;
  dspd_fromfloat32wv_t fromfloat32wv;
  dspd_fromfloat64wv_t fromfloat64wv;
  };*/


static struct pcm_conv conv[DSPD_PCM_FORMAT_LAST+1] = {
  [DSPD_PCM_FORMAT_S8] = {
    .tofloat32 = (dspd_tofloat32_t)int8ne_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int8ne_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int8ne_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int8ne_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int8_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int8_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int8_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int8_array_wv,
  },
  [DSPD_PCM_FORMAT_U8] = {
    .tofloat32 = (dspd_tofloat32_t)uint8ne_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint8ne_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint8ne_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint8ne_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint8_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint8_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint8_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint8_array_wv,
  },
  [DSPD_PCM_FORMAT_S16_NE] = {
    .tofloat32 = (dspd_tofloat32_t)int16ne_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int16ne_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int16ne_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int16ne_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int16_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int16_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int16_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int16_array_wv,
  },
  [DSPD_PCM_FORMAT_U16_NE] = {
    .tofloat32 = (dspd_tofloat32_t)uint16ne_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint16ne_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint16ne_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint16ne_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint16_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint16_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint16_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint16_array_wv,
  },
  [DSPD_PCM_FORMAT_U16_OE] = {
    .tofloat32 = (dspd_tofloat32_t)uint16oe_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint16oe_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint16oe_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint16oe_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint16oe_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint16oe_array,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint16oe_array_wv,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint16oe_array_wv,
  },
  [DSPD_PCM_FORMAT_S16_OE] = {
    .tofloat32 = (dspd_tofloat32_t)int16oe_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int16oe_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int16oe_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int16oe_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int16oe_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int16oe_array,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int16oe_array_wv,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int16oe_array_wv,
  },

  [DSPD_PCM_FORMAT_S32_NE] = {
    .tofloat32 = (dspd_tofloat32_t)int32ne_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int32ne_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int32ne_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int32ne_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int32_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int32_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int32_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int32_array_wv,
  },
  [DSPD_PCM_FORMAT_U32_NE] = {
    .tofloat32 = (dspd_tofloat32_t)uint32ne_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint32ne_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint32ne_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint32ne_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint32_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint32_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint32_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint32_array_wv,
  },
  [DSPD_PCM_FORMAT_S32_OE] = {
    .tofloat32 = (dspd_tofloat32_t)int32oe_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int32oe_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int32oe_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int32oe_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int32oe_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int32oe_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int32oe_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int32oe_array_wv,
  },
  [DSPD_PCM_FORMAT_U32_OE] = {
    .tofloat32 = (dspd_tofloat32_t)uint32oe_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint32oe_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint32oe_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint32oe_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint32oe_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint32oe_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint32oe_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint32oe_array_wv,
  },

  [DSPD_PCM_FORMAT_FLOAT_NE] = {
    .tofloat32 = (dspd_tofloat32_t)float32_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)float32_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)float32_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)float32_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_float32_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_float32_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_float32_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_float32_array_wv,
  },
 
  //TODO: FLOAT OE

  [DSPD_PCM_FORMAT_S24_3LE] = {
    .tofloat32 = (dspd_tofloat32_t)int24ple_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int24ple_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int24ple_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int24ple_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int24ple_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int24ple_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int24ple_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int24ple_array_wv,
  },
  [DSPD_PCM_FORMAT_S24_3BE] = {
    .tofloat32 = (dspd_tofloat32_t)int24pbe_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int24pbe_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int24pbe_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int24pbe_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int24pbe_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int24pbe_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int24pbe_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int24pbe_array_wv,
  },
  [DSPD_PCM_FORMAT_U24_3LE] = {
    .tofloat32 = (dspd_tofloat32_t)uint24ple_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint24ple_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint24ple_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint24ple_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint24ple_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint24ple_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint24ple_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint24ple_array_wv,
  },
  [DSPD_PCM_FORMAT_U24_3BE] = {
    .tofloat32 = (dspd_tofloat32_t)uint24pbe_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint24pbe_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint24pbe_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint24pbe_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint24pbe_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint24pbe_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint24pbe_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint24pbe_array_wv,
  },

  [DSPD_PCM_FORMAT_S24_NE] = {
    .tofloat32 = (dspd_tofloat32_t)int24ne_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int24ne_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int24ne_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int24ne_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int24ne_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int24ne_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int24ne_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int24ne_array_wv,
  },
  [DSPD_PCM_FORMAT_S24_OE] = {
    .tofloat32 = (dspd_tofloat32_t)int24oe_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)int24oe_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)int24oe_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)int24oe_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_int24oe_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_int24oe_array,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_int24oe_array_wv,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_int24oe_array_wv,
  },
  [DSPD_PCM_FORMAT_U24_NE] = {
    .tofloat32 = (dspd_tofloat32_t)uint24ne_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint24ne_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint24ne_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint24ne_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint24ne_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint24ne_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint24ne_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint24ne_array_wv,
  },
  [DSPD_PCM_FORMAT_U24_OE] = {
    .tofloat32 = (dspd_tofloat32_t)uint24oe_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)uint24oe_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)uint24oe_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)uint24oe_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_uint24oe_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_uint24oe_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_uint24oe_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_uint24oe_array_wv,
  },
  [DSPD_PCM_FORMAT_A_LAW] = {
    .tofloat32 = (dspd_tofloat32_t)alaw_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)alaw_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)alaw_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)alaw_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_alaw_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_alaw_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_alaw_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_alaw_array_wv,
  },
  [DSPD_PCM_FORMAT_MU_LAW] = {
    .tofloat32 = (dspd_tofloat32_t)ulaw_to_float32_array,
    .tofloat64 = (dspd_tofloat64_t)ulaw_to_float64_array,
    .tofloat64wv = (dspd_tofloat64wv_t)ulaw_to_float64_array_wv,
    .tofloat32wv = (dspd_tofloat32wv_t)ulaw_to_float32_array_wv,
    .fromfloat32 = (dspd_fromfloat32_t)float32_to_ulaw_array,
    .fromfloat64 = (dspd_fromfloat64_t)float64_to_ulaw_array,
    .fromfloat64wv = (dspd_fromfloat64wv_t)float64_to_ulaw_array_wv,
    .fromfloat32wv = (dspd_fromfloat32wv_t)float32_to_ulaw_array_wv,
  },

  //TODO: 18+20 bit formats
 

  

};

const struct pcm_conv *dspd_getconv(int format)
{
  const char *bytes; char c = 0; size_t i;
  if ( format < 0 || format > (int)(sizeof(conv)/sizeof(conv[0])) )
    return NULL;
  bytes = (const char*)&conv[format];
  //If the whole struct is 0 then there is no conversion routines.
  //In the future, I probably won't support any format where a full set
  //of conversion routines is not available.
  for ( i = 0; i < sizeof(conv[0]); i++ )
    c |= bytes[i];
  if ( ! c )
    return NULL;
  return &conv[format];
}

#define DSPD_PCM_FMT_FLAG_SIGNED 1
#define DSPD_PCM_FMT_FLAG_MSB    2
#define DSPD_PCM_FMT_FLAG_LE     4
#define DSPD_PCM_FMT_FLAG_BE     8

struct dspd_pcm_format_info {
  bool     integer;
  uint16_t length;
  uint16_t bits;
  uint16_t flags;
};

static const struct dspd_pcm_format_info format_info_table[] = {
  [DSPD_PCM_FORMAT_S8] = { .integer = true, .length = 1, .bits = 8, .flags = DSPD_PCM_FMT_FLAG_SIGNED },
  /** Unsigned 8 bit */
  [DSPD_PCM_FORMAT_U8] = { .integer = true, .length = 1, .bits = 8, .flags = 0 },
  /** Signed 16 bit Little Endian */
  [DSPD_PCM_FORMAT_S16_LE] = { .integer = true, .length = 2, .bits = 16, .flags = DSPD_PCM_FMT_FLAG_LE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Signed 16 bit Big Endian */
  [DSPD_PCM_FORMAT_S16_BE] = { .integer = true, .length = 2, .bits = 16, .flags = DSPD_PCM_FMT_FLAG_BE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Unsigned 16 bit Little Endian */
  [DSPD_PCM_FORMAT_U16_LE] = { .integer = true, .length = 2, .bits = 16, .flags = DSPD_PCM_FMT_FLAG_LE },
  /** Unsigned 16 bit Big Endian */
  [DSPD_PCM_FORMAT_U16_BE] = { .integer = true, .length = 2, .bits = 16, .flags = DSPD_PCM_FMT_FLAG_BE },
  /** Signed 24 bit Little Endian using low three bytes in 32-bit word */
  [DSPD_PCM_FORMAT_S24_LE] = { .integer = true, .length = 4, .bits = 24, .flags = DSPD_PCM_FMT_FLAG_LE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Signed 24 bit Big Endian using low three bytes in 32-bit word */
  [DSPD_PCM_FORMAT_S24_BE] = { .integer = true, .length = 4, .bits = 24, .flags = DSPD_PCM_FMT_FLAG_BE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Unsigned 24 bit Little Endian using low three bytes in 32-bit word */
  [DSPD_PCM_FORMAT_U24_LE] = { .integer = true, .length = 4, .bits = 24, .flags = DSPD_PCM_FMT_FLAG_LE },
  /** Unsigned 24 bit Big Endian using low three bytes in 32-bit word */
  [DSPD_PCM_FORMAT_U24_BE] = { .integer = true, .length = 4, .bits = 24, .flags = DSPD_PCM_FMT_FLAG_BE },
  /** Signed 32 bit Little Endian */
  [DSPD_PCM_FORMAT_S32_LE] = { .integer = true, .length = 4, .bits = 32, .flags = DSPD_PCM_FMT_FLAG_LE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Signed 32 bit Big Endian */
  [DSPD_PCM_FORMAT_S32_BE] = { .integer = true, .length = 4, .bits = 32, .flags = DSPD_PCM_FMT_FLAG_BE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Unsigned 32 bit Little Endian */
  [DSPD_PCM_FORMAT_U32_LE] = { .integer = true, .length = 4, .bits = 32, .flags = DSPD_PCM_FMT_FLAG_LE },
  /** Unsigned 32 bit Big Endian */
  [DSPD_PCM_FORMAT_U32_BE] = { .integer = true, .length = 4, .bits = 32, .flags = DSPD_PCM_FMT_FLAG_BE },
  /** Float 32 bit Little Endian, Range -1.0 to 1.0 */
  [DSPD_PCM_FORMAT_FLOAT_LE] = { .length = 4, .bits = 32, .flags = DSPD_PCM_FMT_FLAG_LE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Float 32 bit Big Endian, Range -1.0 to 1.0 */
  [DSPD_PCM_FORMAT_FLOAT_BE] = { .length = 4, .bits = 32, .flags = DSPD_PCM_FMT_FLAG_BE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Float 64 bit Little Endian, Range -1.0 to 1.0 */
  [DSPD_PCM_FORMAT_FLOAT64_LE] = { .length = 8, .bits = 64, .flags = DSPD_PCM_FMT_FLAG_LE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Float 64 bit Big Endian, Range -1.0 to 1.0 */
  [DSPD_PCM_FORMAT_FLOAT64_BE] = { .length = 8, .bits = 64, .flags = DSPD_PCM_FMT_FLAG_BE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** IEC-958 Little Endian */
  [DSPD_PCM_FORMAT_IEC958_SUBFRAME_LE] = { .length = 0 },
  /** IEC-958 Big Endian */
  [DSPD_PCM_FORMAT_IEC958_SUBFRAME_BE] = { .length = 0 },
  /** Mu-Law */
  [DSPD_PCM_FORMAT_MU_LAW] = { .length = 1, .bits = 8 },
  /** A-Law */
  [DSPD_PCM_FORMAT_A_LAW] = { .length = 1, .bits = 8 },
  /** Ima-ADPCM */
  [DSPD_PCM_FORMAT_IMA_ADPCM] = { .length = 0 },
  /** MPEG */
  [DSPD_PCM_FORMAT_MPEG] = { .length = 0 },
  /** GSM */
  [DSPD_PCM_FORMAT_GSM] = { .length = 0 },
  /** Special */
  [DSPD_PCM_FORMAT_SPECIAL] = { .length = 0 },
  /** Signed 24bit Little Endian in 3bytes format */
  [DSPD_PCM_FORMAT_S24_3LE] = { .integer = true, .length = 3, .bits = 24, .flags = DSPD_PCM_FMT_FLAG_LE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Signed 24bit Big Endian in 3bytes format */
  [DSPD_PCM_FORMAT_S24_3BE] = { .integer = true, .length = 3, .bits = 24, .flags = DSPD_PCM_FMT_FLAG_BE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Unsigned 24bit Little Endian in 3bytes format */
  [DSPD_PCM_FORMAT_U24_3LE] = { .integer = true, .length = 3, .bits = 24, .flags = DSPD_PCM_FMT_FLAG_LE },
  /** Unsigned 24bit Big Endian in 3bytes format */
  [DSPD_PCM_FORMAT_U24_3BE] = { .integer = true, .length = 3, .bits = 24, .flags = DSPD_PCM_FMT_FLAG_BE },
  /** Signed 20bit Little Endian in 3bytes format */
  [DSPD_PCM_FORMAT_S20_3LE] = { .integer = true, .length = 3, .bits = 20, .flags = DSPD_PCM_FMT_FLAG_LE },
  /** Signed 20bit Big Endian in 3bytes format */
  [DSPD_PCM_FORMAT_S20_3BE] = { .integer = true, .length = 3, .bits = 20, .flags = DSPD_PCM_FMT_FLAG_BE },
  /** Unsigned 20bit Little Endian in 3bytes format */
  [DSPD_PCM_FORMAT_U20_3LE] = { .integer = true, .length = 3, .bits = 20, .flags = DSPD_PCM_FMT_FLAG_LE  },
  /** Unsigned 20bit Big Endian in 3bytes format */
  [DSPD_PCM_FORMAT_U20_3BE] = { .integer = true, .length = 3, .bits = 20, .flags = DSPD_PCM_FMT_FLAG_BE  },
  /** Signed 18bit Little Endian in 3bytes format */
  [DSPD_PCM_FORMAT_S18_3LE] = { .integer = true, .length = 3, .bits = 18, .flags = DSPD_PCM_FMT_FLAG_LE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Signed 18bit Big Endian in 3bytes format */
  [DSPD_PCM_FORMAT_S18_3BE] = { .integer = true, .length = 3, .bits = 18, .flags = DSPD_PCM_FMT_FLAG_BE | DSPD_PCM_FMT_FLAG_SIGNED },
  /** Unsigned 18bit Little Endian in 3bytes format */
  [DSPD_PCM_FORMAT_U18_3LE] = { .integer = true, .length = 3, .bits = 18, .flags = DSPD_PCM_FMT_FLAG_LE },
  /** Unsigned 18bit Big Endian in 3bytes format */
  [DSPD_PCM_FORMAT_U18_3BE] = { .integer = true, .length = 3, .bits = 18, .flags = DSPD_PCM_FMT_FLAG_BE  },
  /* G.723 (ADPCM) 24 kbit/s, 8 samples in 3 bytes */
  [DSPD_PCM_FORMAT_G723_24] = { .length = 3, .bits = 24 },
  /* G.723 (ADPCM) 24 kbit/s, 1 sample in 1 byte */
  [DSPD_PCM_FORMAT_G723_24_1B] = { .length = 1, .bits = 24 },
  /* G.723 (ADPCM) 40 kbit/s, 8 samples in 3 bytes */
  [DSPD_PCM_FORMAT_G723_40] = { .length = 3, .bits = 24 },
  /* G.723 (ADPCM) 40 kbit/s, 1 sample in 1 byte */
  [DSPD_PCM_FORMAT_G723_40_1B] = { .length = 1, .bits = 24 },
  /* Direct Stream Digital (DSD) in 1-byte samples (x8) */
  [DSPD_PCM_FORMAT_DSD_U8] = { .length = 1, .bits = 24 },
  /* Direct Stream Digital (DSD) in 2-byte samples (x16) */
  [DSPD_PCM_FORMAT_DSD_U16_LE] = { .length = 2, .bits = 16 },

};


size_t dspd_get_pcm_format_size(int format)
{
  
  if ( format < 0 || format > (int)(sizeof(format_info_table)/sizeof(format_info_table[0])) )
    return 0;
  return format_info_table[format].length;
}

bool dspd_pcm_format_is_integer(int format)
{
  if ( format < 0 || format > (int)(sizeof(format_info_table)/sizeof(format_info_table[0])) )
    return false;
  return format_info_table[format].integer;
}

/*
  Doesn't handle LSB/MSB alignment.  The caller should consider a format
  valid if (remember LSB/MSB is meaningless for 1 byte formats):
  
  ((bits == (length*8)) && msb == 1) || ((bits < (length*8)) && msb == 0) || (bits == 8 && bps == 1)

*/
int dspd_pcm_build_format(unsigned int bits, unsigned int length, unsigned int usig, unsigned int big_endian)
{
  size_t i;
  const struct dspd_pcm_format_info *info;
  int flags = 0;
  int format = -1;
  if ( big_endian )
    flags |= DSPD_PCM_FMT_FLAG_BE;
  else
    flags |= DSPD_PCM_FMT_FLAG_LE;
  if ( ! usig )
    flags |= DSPD_PCM_FMT_FLAG_SIGNED;
  for ( i = 0; i < ARRAY_SIZE(format_info_table); i++ )
    {
      info = &format_info_table[i];
      if ( (info->flags & flags) == flags &&
	   info->bits == bits && info->length == length )
	{
	  format = i;
	  break;
	}
    }
  return format;
}

bool dspd_pcm_format_info(int format, unsigned int *bits, unsigned int *length, unsigned int *usig, unsigned int *big_endian)
{
  bool ret = false;
  const struct dspd_pcm_format_info *info;
  if ( format >= 0 && format < ARRAY_SIZE(format_info_table) )
    {
      info = &format_info_table[format];
      if ( info->length )
	{
	  *bits = info->bits;
	  *length = info->length;
	  *usig = !! (info->flags & DSPD_PCM_FMT_FLAG_SIGNED);
	  *big_endian = !! (info->flags & DSPD_PCM_FMT_FLAG_BE);
	  ret = true;
	}
    }
  return ret;
}
