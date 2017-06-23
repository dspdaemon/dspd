/*
 *  SRC - Sample Rate Conversion
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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include "src.h"
struct dspd_bcr_params {
  const float  *inbuf;
  unsigned int  in_size;
  float        *outbuf;
  unsigned int  out_size;
  unsigned int  frames_used;
  unsigned int  frames_generated;
};
struct dspd_bcr_state {
  uint32_t iper, oper;
  uint32_t iclk, oclk;
  union {
    float  *last_sample;
    double  *val;
  } data;
  unsigned int channels;
  unsigned int n;
  unsigned int cont;
  void (*process)(struct dspd_bcr_state * __restrict,
		  const float * __restrict in,
		  size_t * __restrict used,
		  float * __restrict out,
		 size_t * __restrict gen);
};
static int default_quality = 0;


static void process_compress(struct dspd_bcr_state * __restrict st, 
			     const float * __restrict in,
			     size_t * __restrict used,
			     float * __restrict out,
			     size_t * __restrict gen)
{
  unsigned int len = *gen, o = 0, i = 0, c, imax;
  assert(st->channels);
  len *= st->channels;
  imax = (*used) * st->channels;
  if ( st->cont )
    goto cont1;
  st->n = 0;
  for ( o = 0; o < len; o += st->channels )
    {
      st->iclk %= st->oper;
      if ( st->iclk == 0 )
	{
	  for ( c = 0; c < st->channels; c++ )
	    st->data.val[c] = 0;
	  st->n = 0;
	} else
	{
	  st->n = 1;
	}
      while ( st->iclk < st->oper )
	{
	  for ( c = 0; c < st->channels; c++ )
	    st->data.val[c] += in[i+c];
	  st->n++;
	  i += st->channels;
	  st->iclk += st->iper;
	cont1:
	  if ( i == imax )
	    {
	      st->cont = 1;
	      goto finished;
	    }
	}
      for ( c = 0; c < st->channels; c++ )
	{
	  st->data.val[c] /= st->n;
	  out[o+c] = st->data.val[c];
	}
    }
  st->cont = 0;
 finished:
  *gen = o / st->channels;
  *used = i / st->channels;
}

static void process_expand(struct dspd_bcr_state * __restrict st, 
			   const float * __restrict in, 
			   size_t * __restrict used, 
			   float * __restrict out, 
			   size_t * __restrict gen)
{
  unsigned int len = *used, mx = *gen;
  int o = 0, i = 0;
  double val;
  int c;
  assert(st->channels);
  mx *= st->channels;
  len *= st->channels;
  switch(st->cont)
    {
    case 1:
      goto cont1;
    case 2:
      goto cont2;
    }
  for ( i = 0; i < len; i += st->channels )
    {
      st->n++;
      st->oclk %= st->iper;
      if ( st->oclk > 0 )
	{
	  for ( c = 0; c < st->channels; c++ )
	    {

	      val = st->data.last_sample[c] / st->n;
	     
	      out[o+c] = val / 2;
	    }
	  st->oclk += st->oper;
	  o += st->channels;
	cont1:
	  if ( o == mx )
	    { st->cont = 1; goto finished; }
	}

      for ( c = 0; c < st->channels; c++ )
	{
	  st->data.last_sample[c] += in[i+c];
	}


      while ( st->oclk < st->iper )
	{
	  for ( c = 0; c < st->channels; c++ )
	    out[o+c] = in[i+c];
	    

	  st->oclk += st->oper;
	  o += st->channels;
	cont2:
	  if ( o == mx )
	    { st->cont = 2; goto finished; }
	}
    }
  st->cont = 0;
 finished:
  (*gen) = o / st->channels;
  (*used) = i / st->channels;
}

static int32_t bcr_new(dspd_src_t *src,
		       int quality, 
		       int channels)
{
  struct dspd_bcr_state *st;
  uint8_t *ptr;
  size_t len, offset;

  len = sizeof(struct dspd_bcr_state);
  if ( len % 8 )
    len = ((len / 8) + 1) * 8;
  offset = len;
  len += sizeof(double) * channels;
  ptr = calloc(1, len);
  if ( ! ptr )
    return -errno;

  st = (struct dspd_bcr_state*)ptr;
  st->data.last_sample = (float*)&ptr[offset];



  st->channels = channels;
  *src = st;
  return 0;
}

static int bcr_set_rates(dspd_src_t src, 
			 uint32_t in, 
			 uint32_t out)
{
  struct dspd_bcr_state *st = src;
  st->iper = 1000000000 / in;
  st->oper = 1000000000 / out;
  if ( (out % in) == 0 && (st->iper % st->oper) != 0 )
    {
      st->iper = (out / in) * st->oper; 
    } else if ( (in % out) == 0 && (st->oper % st->iper) != 0 )
    {
      st->oper = (in / out) * st->iper;
    }
  if ( in > out )
    st->process = process_compress;
  else
    st->process = process_expand;
  return 0;
}

static int32_t bcr_reset(dspd_src_t src)
{
  size_t i;
  struct dspd_bcr_state *st = src;
  for ( i = 0; i < st->channels; i++ )
    st->data.last_sample[i] = 0.0;
  st->iclk = 0;
  st->oclk = 0;
  st->cont = 0;
  return 0;
}

static int32_t bcr_free(dspd_src_t src)
{
  free(src);
  return 0;
}

static int32_t bcr_process(dspd_src_t   src,
			   bool         eof,
			   const float * __restrict inbuf,
			   size_t      * __restrict frames_in,
			   float       * __restrict outbuf,
			   size_t      * __restrict frames_out)
{
  struct dspd_bcr_state *st = src;
 
  st->process(st, inbuf, frames_in, outbuf, frames_out);
  return 0;
}


static void bcr_info(struct dspd_src_info *info)
{
  strcpy(info->name, "DSPD Builtin Crappy Resampler");
  info->max_quality = 0;
  info->min_quality = 0;
  info->step = 0;
}

static void bcr_get_params(dspd_src_t src, 
			   uint32_t *quality,
			   uint32_t *rate_in,
			   uint32_t *rate_out)
{
  struct dspd_bcr_state *st = src;
  *quality = 0;
  *rate_in = 1000000000 / st->iper;
  *rate_out = 1000000000 / st->oper;
}

static const struct dspd_src_ops default_ops = {
  .init = NULL,
  .set_rates = bcr_set_rates,
  .reset = bcr_reset,
  .process = bcr_process,
  .newsrc = bcr_new,
  .freesrc = bcr_free,
  .info = bcr_info,
  .get_params = bcr_get_params,
};

static const struct dspd_src_ops *current_ops = &default_ops;
int32_t dspd_src_set_rates(dspd_src_t src, int32_t in, int32_t out)
{
  return current_ops->set_rates(src, in, out);
}

int32_t dspd_src_reset(dspd_src_t src)
{
  return current_ops->reset(src);
}

int32_t dspd_src_process(dspd_src_t   src,
			 bool eof,
			 const float * __restrict inbuf,
			 size_t      * __restrict frames_in,
			 float       * __restrict outbuf,
			 size_t      * __restrict frames_out)
{
  return current_ops->process(src, eof, inbuf, frames_in, outbuf, frames_out);
}
int dspd_src_init(const struct dspd_src_ops *ops)
{
  int32_t ret;
  if ( ops == NULL )
    {
      ret = -EINVAL;
    } else if ( current_ops != &default_ops )
    {
      ret = -EBUSY; //Already initialized once.
    } else if ( ops->init )
    {
      ret = ops->init();
    } else
    {
      ret = 0;
    }
  if ( ret == 0 )
    current_ops = ops;
  return ret;
}

/*
  Create a new resampler object.  The quality is only a hint.  Actual
  values may differ and callers should not bail out if the quality
  differs from what is specified.
*/
int32_t dspd_src_new(dspd_src_t *newsrc, int quality, int channels)
{
  float r, q;
  struct dspd_src_info info;
  current_ops->info(&info);
  //Get default value.
  if ( quality == 0 )
    {
      quality = dspd_src_get_default_quality();
    } else if ( quality == 1 )
    {
      quality = info.min_quality;
    } else if ( quality == 100 )
    {
      quality = info.max_quality;
    } else if ( quality < 0 )
    {
      quality *= -1;
      if ( quality > info.max_quality )
	quality = info.max_quality;
      else if ( quality < info.min_quality )
	quality = info.min_quality;
    } else
    {
      q = quality / 100.0;
      if ( q > 1.0 )
	q = 1.0;
      
      
      r = info.max_quality - info.min_quality;
      r *= q;
      quality = info.min_quality + r;
    }
  return current_ops->newsrc(newsrc, quality, channels);
}

int32_t dspd_src_delete(dspd_src_t src)
{
  return current_ops->freesrc(src);
}

uint64_t dspd_src_get_frame_count(uint64_t rate_in, 
				  uint64_t rate_out, 
				  uint64_t frames_in)
{
  uint32_t iper = 1000000000 / rate_in;
  uint32_t oper = 1000000000 / rate_out;
  uint32_t ret, t;
  t = frames_in * iper;
  ret = t / oper;
  if ( t % oper )
    ret++;
  return ret;
}

void dspd_src_get_params(dspd_src_t src, 
			 uint32_t *quality,
			 uint32_t *rate_in,
			 uint32_t *rate_out)
{
  uint32_t dummy;
  if ( ! quality )
    quality = &dummy;
  if ( ! rate_in )
    rate_in = &dummy;
  if ( ! rate_out )
    rate_out = &dummy;
  current_ops->get_params(src, quality, rate_in, rate_out);
}

int dspd_src_get_default_quality(void)
{
  int ret;
  if ( current_ops->get_default_quality )
    ret = current_ops->get_default_quality();
  else
    ret = default_quality;
  return ret;
}

void dspd_src_set_default_quality(int q)
{
  if ( current_ops->set_default_quality )
    current_ops->set_default_quality(q);
  else
    default_quality = q;
}

