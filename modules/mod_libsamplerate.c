/*
 *   LIBSAMPLERATE - libsamplerate sample rate conversion
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
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <samplerate.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"

struct dspd_src_state {
  SRC_STATE *state;
  SRC_DATA   data;
  uint32_t   rate_in, rate_out;
  uint32_t   src_type;
  uint32_t   quality;
};
#define MAX_QUALITY 5
#define MIN_QUALITY 1
static int src_q2t(int quality)
{
  int q[] = {
    SRC_ZERO_ORDER_HOLD, 
    SRC_LINEAR,
    SRC_SINC_FASTEST, 
    SRC_SINC_MEDIUM_QUALITY, 
    SRC_SINC_BEST_QUALITY
  };
  size_t len = sizeof(q) / sizeof(q[0]);
  quality--;
  if ( quality >= len )
    quality = len - 1;
  else if ( quality < 0 )
    quality = 0;
  return q[quality];
}


static int32_t src_init_cb(void)
{
  return 0;
}

static int32_t src_set_rates_cb(dspd_src_t src,
			       uint32_t in,
			       uint32_t out)
{
  struct dspd_src_state *st = src;
  if ( in == 0 || out == 0 )
    return -EINVAL;
  st->rate_in = in;
  st->rate_out = out;
  st->data.src_ratio = (out * 1.0) / (in * 1.0);
  return 0;
}

static int32_t src_reset_cb(dspd_src_t src)
{
  struct dspd_src_state *st = src;
  if ( src_reset(st->state) )
    return -EINVAL;
  return 0;
}

static int32_t src_new_cb(dspd_src_t *src, int quality, int channels)
{
  struct dspd_src_state *st;
  int err;
  st = calloc(1, sizeof(*st));
  if ( ! st )
    return -errno;
  st->quality = quality;
  st->state = src_new(src_q2t(quality), channels, &err);
  fflush(NULL);
  if ( ! st->state )
    {
      free(st);
      return -ENOMEM;
    }
  *src = st;
  return 0;
}

static int32_t src_process_cb(dspd_src_t   src,
			     bool         eof,
			     const float * __restrict inbuf,
			     size_t      * __restrict frames_in,
			     float       * __restrict outbuf,
			     size_t      * __restrict frames_out)
{
  struct dspd_src_state *st = src;
  int ret;
  st->data.end_of_input = eof;
  st->data.input_frames = *frames_in;
  st->data.output_frames = *frames_out;
  st->data.data_in = (float*)inbuf;
  st->data.data_out = (float*)outbuf;
  ret = src_process(st->state, &st->data);
  if ( ret )
    {
      ret = -EINVAL;
    } else
    {
      *frames_in = st->data.input_frames_used;
      *frames_out = st->data.output_frames_gen;
    }
  return ret;
}


static void src_info_cb(struct dspd_src_info *info)
{
  strcpy(info->name, "SRC (libsamplerate) resampler");
  info->min_quality = MIN_QUALITY;
  info->max_quality = MAX_QUALITY;
  info->step = 1;
}

static int32_t src_free_cb(dspd_src_t src)
{
  struct dspd_src_state *st = src;
  src_delete(st->state);
  free(st);
  return 0;
}

static void src_get_params_cb(dspd_src_t src, 
			      uint32_t *quality,
			      uint32_t *rate_in,
			      uint32_t *rate_out)
{
  struct dspd_src_state *st = src;
  *quality = st->quality;
  *rate_in = st->rate_in;
  *rate_out = st->rate_out;
}

static int default_quality = 2;
static void src_set_default_quality(int q)
{
  if ( q > MAX_QUALITY )
    default_quality = MAX_QUALITY;
  else if ( q == 0 )
    default_quality = 2;
  else if ( q < MIN_QUALITY )
    default_quality = MIN_QUALITY;
  else
    default_quality = q;
}
static int src_get_default_quality(void)
{
  return default_quality;
}

static struct dspd_src_ops src_ops = {
  .init = src_init_cb,
  .set_rates = src_set_rates_cb,
  .reset = src_reset_cb,
  .process = src_process_cb,
  .newsrc = src_new_cb,
  .freesrc = src_free_cb,
  .info = src_info_cb,
  .get_params = src_get_params_cb,
  .get_default_quality = src_get_default_quality,
  .set_default_quality = src_set_default_quality,
};

static int lsr_init(void *daemon, void **context)
{
  if ( dspd_src_init(&src_ops) == 0 )
    dspd_log(0, "Installed libsamplerate samplerate conversion");
  return 0;
}

static void lsr_close(void *daemon, void **context)
{
  
}

static int lsr_ioctl(void         *daemon, 
		     void         *context,
		     int32_t       req,
		     const void   *inbuf,
		     size_t        inbufsize,
		     void         *outbuf,
		     size_t        outbufsize,
		     size_t       *bytes_returned)
{
  return -ENOSYS;
}



struct dspd_mod_cb dspd_mod_libsamplerate = {
  .desc = "SRC (libsamplerate) sample rate conversion",
  .init = lsr_init,
  .close = lsr_close,
  .ioctl = lsr_ioctl,
};
