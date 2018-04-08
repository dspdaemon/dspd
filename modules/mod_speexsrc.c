/*
 *   SPEEXSRC - Speex samplerate conversion
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
#include <stdint.h>
#include <speex/speex_resampler.h>
#include <string.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"

static int32_t speex2errno(int err)
{
  int32_t ret;
  switch(err)
    {
    case RESAMPLER_ERR_SUCCESS:
      ret = 0;
      break;
    case RESAMPLER_ERR_ALLOC_FAILED:
      ret = -ENOMEM;
      break;
    case RESAMPLER_ERR_BAD_STATE:
      ret = -EBADF;
      break;
    case RESAMPLER_ERR_INVALID_ARG:
      ret = -EINVAL;
      break;
    case RESAMPLER_ERR_PTR_OVERLAP:
      ret = -EFAULT;
      break;
    default:
      ret = -EUNKNOWN;
    }
  return ret;
}

static int32_t speex_init(void)
{
  return 0;
}

static int32_t speex_set_rates(dspd_src_t src,
			       uint32_t in,
			       uint32_t out)
{
  return speex2errno(speex_resampler_set_rate(src,
					      in,
					      out));
}

static int32_t speex_reset(dspd_src_t src)
{
  return speex2errno(speex_resampler_reset_mem(src));
}

static int32_t speex_new(dspd_src_t *src, int quality, int channels)
{
  int err;
  quality--;
  if ( quality < 0 )
    quality = 0;
  else if ( quality > 10 )
    quality = 10;
  *src = speex_resampler_init(channels,
			      48000,
			      48000,
			      quality,
			      &err);
  if ( ! *src )
    return speex2errno(err);
  return 0;
}

static int32_t speex_process(dspd_src_t   src,
			     bool         eof,
			     const float * __restrict inbuf,
			     size_t      * __restrict frames_in,
			     float       * __restrict outbuf,
			     size_t      * __restrict frames_out)
{
  uint32_t in, out;
  int32_t ret;
  in = *frames_in;
  out = *frames_out;
  ret = speex_resampler_process_interleaved_float(src, inbuf, &in, outbuf, &out);
  if ( ret != 0 )
    {
      ret = speex2errno(ret);
    } else
    {
      *frames_in = in;
      *frames_out = out;
    }
  return ret;
}


static void speex_info(struct dspd_src_info *info)
{
  strcpy(info->name, "Speex Resampler");
  info->min_quality = SPEEX_RESAMPLER_QUALITY_MIN + 1;
  info->max_quality = SPEEX_RESAMPLER_QUALITY_MAX + 1;
  info->step = 1;
}

static int32_t speex_free(dspd_src_t src)
{
  speex_resampler_destroy(src);
  return 0;
}

static void speex_get_params(dspd_src_t src, 
			     uint32_t *quality,
			     uint32_t *rate_in,
			     uint32_t *rate_out)
{
  int q;
  speex_resampler_get_rate(src,
			   rate_in,
			   rate_out);
  speex_resampler_get_quality(src, &q);
  *quality = q + 1;
}

static int default_quality = SPEEX_RESAMPLER_QUALITY_DEFAULT + 1;
static int speex_get_default_quality(void)
{
  return default_quality;
}
static void speex_set_default_quality(int q)
{
  if ( q == 0 )
    default_quality = SPEEX_RESAMPLER_QUALITY_DEFAULT + 1;
  else if ( q > (SPEEX_RESAMPLER_QUALITY_MAX+1) )
    default_quality = (SPEEX_RESAMPLER_QUALITY_MAX+1);
  else if ( q < (SPEEX_RESAMPLER_QUALITY_MIN+1) )
    default_quality = (SPEEX_RESAMPLER_QUALITY_MIN+1);
  else
    default_quality = q;
}

static struct dspd_src_ops speex_ops = {
  .init = speex_init,
  .set_rates = speex_set_rates,
  .reset = speex_reset,
  .process = speex_process,
  .newsrc = speex_new,
  .freesrc = speex_free,
  .info = speex_info,
  .get_params = speex_get_params,
  .set_default_quality = speex_set_default_quality,
  .get_default_quality = speex_get_default_quality,
};

static int ssrc_init(void *daemon, void **context)
{
  if ( dspd_src_init(&speex_ops) == 0 )
    dspd_log(0, "Installed speex samplerate conversion");
  return 0;
}

static void ssrc_close(void *daemon, void **context)
{
  
}


struct dspd_mod_cb dspd_mod_speexsrc = {
  .init_priority = DSPD_MOD_INIT_PRIO_SRC + 1U,
  .desc = "Speex samplerate conversion",
  .init = ssrc_init,
  .close = ssrc_close,
};
