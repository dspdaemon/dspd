/*
 *  ALSAHW - ALSA PCM hardware support
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
#include <ctype.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "mod_alsahw.h"


struct alsahw_mcfg {
  int32_t   format;
  uint32_t  channels;
  int32_t   access;
  uint32_t  rate, max_rate;
  uint32_t  fragsize;
  uint32_t  bufsize;
};

static int alsahw_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, int format);
static int alsahw_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, unsigned int channels);
static int alsahw_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, unsigned int rate);
static int alsahw_set_fragsize(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, unsigned int size);
static int alsahw_set_bufsize(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, unsigned int size);
static int alsahw_is_batch(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, const char *bus, const char *addr, const struct alsahw_mcfg *cfg);

static struct alsahw_notifier *global_notifier;


static inline void calculate_playback_space(struct alsahw_handle *hdl)
{
  if ( hdl->status.fill >= hdl->vbufsize )
    hdl->status.space = 0;
  else
    hdl->status.space = hdl->vbufsize - hdl->status.fill;
}

int32_t alsahw_pcm_read_begin(void *handle,
			       void **ptr,
			       uintptr_t *offset,
			       uintptr_t *frames)
{
  struct alsahw_handle *hdl = handle;
  uintptr_t len, space, off;
  snd_pcm_sframes_t ret;
  if ( hdl->err )
    return hdl->err;
  if ( *frames > hdl->status.fill )
    len = hdl->status.fill;
  else
    len = *frames;
  *ptr = hdl->buffer.addr32;

  if ( len > 0 )
    {
      off = hdl->status.hw_ptr % hdl->params.bufsize;
      space = hdl->params.bufsize - off;
      if ( space < len )
	len = space;
      //Data is already there
      if ( hdl->is_rewound )
	{
	  *offset = off;
	  *frames = len;
	  return 0;
	}

      ret = snd_pcm_readi(hdl->handle,
			  hdl->hw_addr,
			  len);
      if ( ret < 0 && ret != -EAGAIN )
	{
	  hdl->err = ret;
	  return ret;
	} else if ( ret == 0 )
	{
	  ret = -EBADF;
	  hdl->err = ret;
	  return ret;
	} else if ( ret > 0 )
	{
	  hdl->convert.tofloat(hdl->hw_addr,
			       &hdl->buffer.addr32[off*hdl->params.channels],
			       ret * hdl->params.channels,
			       hdl->volume);
	  hdl->status.hw_ptr += ret;
	  hdl->status.fill += ret;
	  hdl->status.space -= ret;
	  hdl->samples_read += ret;
	  *offset = off;
	  *frames = ret;
	  ret = 0;
	}
      
    } else
    {
      ret = -EAGAIN;
    }
  return ret;
}


intptr_t alsahw_pcm_read_commit(void *handle,
				 uintptr_t offset,
				 uintptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  if ( hdl->err )
    return hdl->err;
  hdl->status.appl_ptr += frames;
  hdl->status.fill -= frames;
  hdl->status.space += frames;
  hdl->status.delay -= frames;
  if ( hdl->status.appl_ptr >= hdl->saved_appl )
    hdl->is_rewound = 0;
  return 0;
}



int32_t alsahw_pcm_mmap_read_begin(void *handle,
				    void **ptr,
				    uintptr_t *offset,
				    uintptr_t *frames)
{
  struct alsahw_handle *hdl = handle;
  int32_t ret;
  snd_pcm_uframes_t mm_offset = *offset, mm_frames = *frames;
  const snd_pcm_channel_area_t *area;

  //The requested data is already in the buffer
  if ( hdl->is_rewound )
    return alsahw_pcm_read_begin(handle, ptr, offset, frames);

  /*
    Some drivers will return more data than is actually available and then
    cause the read pointer to go out of range and never indicate any errors.
   */
  if ( mm_frames > hdl->status.fill )
    mm_frames = hdl->status.fill;


  ret = snd_pcm_mmap_begin(hdl->handle,
			   &area,
			   &mm_offset,
			   &mm_frames);
  if ( ret == 0 )
    {
      assert(area->addr);

      /*
	Some drivers might return more than requested.
       */
      if ( mm_frames > *frames )
	mm_frames = *frames;

      hdl->hw_addr = area->addr;
      hdl->convert.tofloat((char*)area->addr+(mm_offset*hdl->frame_size),
			   &hdl->buffer.addr32[hdl->channels*mm_offset],
			   mm_frames*hdl->channels,
			   hdl->volume);
      hdl->samples_read += mm_frames;
      *ptr = hdl->buffer.addr32;
      *offset = mm_offset;
      *frames = mm_frames;
    } else
    {
      hdl->err = ret;
    }
  return ret;
}


intptr_t alsahw_pcm_mmap_read_commit(void *handle,
				      uintptr_t offset,
				      uintptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  snd_pcm_sframes_t ret;
  if ( hdl->err )
    return hdl->err;
  if ( ! hdl->is_rewound )
    ret = snd_pcm_mmap_commit(hdl->handle, offset, frames);
  else
    ret = frames;
  if ( ret < 0 )
    {
      hdl->err = ret;
    } else
    {
      hdl->status.appl_ptr += ret;
      hdl->status.space += ret;
      hdl->status.fill -= ret;
      hdl->status.delay -= ret;
    }
  if ( hdl->status.appl_ptr >= hdl->saved_appl )
    hdl->is_rewound = 0;
  return ret;
}

int32_t alsahw_pcm_write_begin(void *handle,
				void **ptr,
				uintptr_t *offset,
				uintptr_t *frames)
{
  struct alsahw_handle *hdl = handle;
  snd_pcm_sframes_t fr;
  uintptr_t f, o;
  double **buf = (double**)ptr;
  if ( hdl->err )
    return hdl->err;

  /*fr = snd_pcm_avail_update(hdl->handle);
  //Check for error or no space
  if ( fr < 0 && fr != -EAGAIN )
    {
      hdl->err = fr;
      return hdl->err;
    }
  if ( fr == 0 || fr == -EAGAIN )
  return -EAGAIN;*/

  fr = hdl->status.space;
  if ( fr == 0 )
    return -EAGAIN;
  

  //Find contiguous space <= requested size.
  if ( fr > *frames )
    fr = *frames;
  o = hdl->status.appl_ptr % hdl->buffer_size;
  f = hdl->buffer_size - o;
  if ( fr > f )
    fr = f;
  *frames = fr; 
  *offset = o;
  *buf = hdl->buffer.addr64;

  //Force real status update next time.
  if ( fr == 0 )
    hdl->got_tstamp = 0;
  if ( fr > 0 && hdl->status.appl_ptr == hdl->erase_ptr )
    {
      memset(&hdl->buffer.addr64[o * hdl->channels], 0, sizeof(double) * fr * hdl->channels);
      hdl->erase_ptr += fr;
    }


  return 0;
}

intptr_t alsahw_pcm_write_commit(void *handle,
				  uintptr_t offset,
				  uintptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  int ret;
  uintptr_t off = 0;
  hdl->convert.fromdouble(&hdl->buffer.addr64[offset * hdl->channels],
	       hdl->hw_addr,
	       frames * hdl->channels,
	       hdl->volume);
  while ( off < frames )
    {
      ret = snd_pcm_writei(hdl->handle, 
			   (char*)hdl->hw_addr+(hdl->frame_size*hdl->channels*off), 
			   frames - off);
      if ( ret == -EAGAIN )
	continue;
      if ( ret == 0 )
	{
	  //This should be EOF, which maybe should not occur.
	  //It seems like a bad state for the file descriptor.
	  hdl->err = -EBADF;
	  return ret;
	}
      if ( ret < 0 )
	{
	  hdl->err = ret;
	  return ret;
	}
      off += ret;
      hdl->status.appl_ptr += ret;
      hdl->status.space -= ret;
      hdl->status.fill += ret;
      hdl->status.delay += ret;
    }
  return frames;
}

int32_t alsahw_pcm_mmap_begin(void *handle,
			       void **ptr,
			       uintptr_t *offset,
			       uintptr_t *frames)
{
  struct alsahw_handle *hdl = handle;
  snd_pcm_uframes_t mm_offset, mm_frames;
  int ret;
  const snd_pcm_channel_area_t *area;
  double **buf = (double**)ptr;
  if ( hdl->err )
    return hdl->err;
  mm_frames = *frames;
  ret = snd_pcm_mmap_begin(hdl->handle,
			   &area,
			   &mm_offset,
			   &mm_frames);
  if ( ret == 0 )
    {
      hdl->hw_addr = area->addr;
      *buf = hdl->buffer.addr64;
      *offset = mm_offset;
      *frames = mm_frames;
      if ( mm_frames == 0 )
	hdl->got_tstamp = 0;
      if ( mm_frames > 0 && hdl->status.appl_ptr == hdl->erase_ptr )
	{
	  memset(&hdl->buffer.addr64[mm_offset * hdl->channels], 0, sizeof(double) * mm_frames * hdl->channels);
	  hdl->erase_ptr += mm_frames;
	}
    } else
    {
      hdl->err = ret;
    }
  return ret;
}

intptr_t alsahw_pcm_mmap_commit(void *handle,
				 uintptr_t offset,
				 uintptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  snd_pcm_sframes_t ret;
  uintptr_t maxframes;
  if ( hdl->err )
    return hdl->err;
  
  maxframes = hdl->erase_ptr - hdl->status.appl_ptr;
  if ( frames > maxframes )
    frames = maxframes;
  hdl->convert.fromdouble(&hdl->buffer.addr64[offset * hdl->channels],
			  (char*)hdl->hw_addr + (offset * hdl->frame_size),
			  frames * hdl->channels,
			  hdl->volume);
  ret = snd_pcm_mmap_commit(hdl->handle, offset, frames);
  if ( ret < 0 )
    {
      hdl->err = ret;
    } else
    {
      assert(ret <= frames);
      hdl->status.appl_ptr += ret;
      hdl->status.fill += ret;
      hdl->status.delay += ret;
      calculate_playback_space(hdl);
    }
  return ret;
}

int32_t alsahw_pcm_recover(void *handle)
{
  struct alsahw_handle *hdl = handle;
  if ( hdl->err == -ESTRPIPE )
    hdl->err = snd_pcm_resume(hdl->handle);
  if ( hdl->err )
    {
      while ( (hdl->err = snd_pcm_prepare(hdl->handle)) == -EBUSY )
	{
	  snd_pcm_drop(hdl->handle);
	  usleep(1);
	}
    }
  if ( hdl->err )
    hdl->err = snd_pcm_recover(hdl->handle, hdl->err, 0);
  
  if ( hdl->err == 0 )
    {
      hdl->erase_ptr = 0;
      hdl->got_tstamp = 0;
      hdl->xfer = 0;
      hdl->started = 0;
      hdl->samples_read = 0;
      memset(&hdl->status, 0, sizeof(hdl->status));
    }
  return hdl->err;
}

int32_t alsahw_pcm_start(void *handle)
{
  struct alsahw_handle *hdl = handle;
  if ( hdl->err )
    return hdl->err;
  hdl->got_tstamp = 0;
  hdl->xfer = 0;
  hdl->started = 1;
  hdl->err = snd_pcm_start(hdl->handle);
  return hdl->err;
}

static void alsahw_reset(struct alsahw_handle *hdl)
{
  hdl->erase_ptr = 0;
  hdl->got_tstamp = 0;
  hdl->xfer = 0;
  hdl->started = 0;
  hdl->samples_read = 0;
  hdl->is_rewound = 0;
  memset(&hdl->status, 0, sizeof(hdl->status));
}

int32_t alsahw_pcm_drop(void *handle)
{
  //Drop.  No drain because it should just drop after
  //playing whatever is left.  Basically, run after all
  //clients are done until a full buffer length is played
  //then drop with some data in the buffer.

  struct alsahw_handle *hdl = handle;
  if ( hdl->err )
    return hdl->err;
  alsahw_reset(hdl);
  hdl->err = snd_pcm_drop(hdl->handle);
  return hdl->err;
}

int32_t alsahw_pcm_prepare(void *handle)
{
  struct alsahw_handle *hdl = handle;
  int ret;
  ret = snd_pcm_prepare(hdl->handle);
  hdl->err = ret;
  alsahw_reset(hdl);
  return ret;
}

/*
  This saves a lot of CPU when it works.  On a ESS Allegro PCI with 512 frames latency
  and 48KHz stereo the CPU usage in the test loop goes from 8% to 1%.  Adding an extra
  snd_pcm_status() call adds 7% CPU so that means 7% of the overhead is reading the
  position register or something related to it.
*/
static int32_t alsahw_pcm_status_intrp(struct alsahw_handle *handle)
{
  //return -1;
  dspd_time_t now = dspd_get_time();
  uintptr_t diff = (now - handle->status.tstamp) / handle->sample_time, f;
  if ( diff >= handle->status.fill )
    return -1;
  handle->status.tstamp = now;
  if ( handle->stream != SND_PCM_STREAM_PLAYBACK )
    {
      if ( diff >= handle->status.fill )
	return -1;
     
      //calculate_playback_space(handle);
      handle->xfer += diff;
      if ( handle->xfer >= (handle->hlatency / 2) )
	return -1;
      
      handle->status.space += diff;
      handle->status.hw_ptr += diff;
      handle->status.fill -= diff;
      handle->status.delay -= diff;
      if ( handle->status.fill > handle->vbufsize )
	handle->status.space = 0;
    } else
    {
      if ( diff >= handle->status.space )
	return -1;
      f = handle->status.fill + diff;
      if ( f > handle->params.bufsize )
	return -1;
      handle->xfer += diff;
      if ( handle->xfer >= (handle->hlatency / 2) )
	return -1;

      handle->status.fill += diff;
      handle->status.space -= diff;
      handle->status.hw_ptr += diff;
      
    }
  return 0;
}

int32_t alsahw_pcm_status(void *handle, const struct dspd_pcm_status **status)
{
  struct alsahw_handle *hdl = handle;
  snd_htimestamp_t hts, tts;
  int state;
  uintptr_t p;
  if ( hdl->err )
    return hdl->err;

  /*
    It is sometimes possible to interpolate the status if:

    1.  There is a previous timestamp
    2.  The amount played by the hardware (generally similar to time elapsed)
    is not too much.
    3.  The amount of space last known to be in the buffer is greater than the
    amount we might write.
    
    This is the only use of the device fragment size.  CoreAudio is supposed to check
    exactly when an interrupt is raised.  Here we fudge it and check about when an
    interrupt should be raised.  Since it is possible to disable interrupts for some
    hardware it also checks if at least 1/2 of the buffer has been played (iterated since
    it may not actually be filled up all the way at any time).

   */
  if ( hdl->got_tstamp != 0 && hdl->stream == SND_PCM_STREAM_PLAYBACK )
    {
      if ( hdl->xfer < hdl->params.fragsize &&
	   hdl->xfer < (hdl->buffer_size/2) &&
	   (hdl->buffer_size-hdl->status.fill) > hdl->hlatency &&
	   hdl->started != 0 &&
	   hdl->status.space > 0 &&
	   hdl->xfer < hdl->hlatency &&
	   hdl->status.appl_ptr > hdl->params.min_latency &&
	   hdl->vbufsize > (hdl->min_dma * 2) )
	{
	  //Don't interpolate an xrun state.  The timer might be wrong so
	  //check the real hardware.  If the timer is always right then this
	  //won't make much of a difference.
	  if ( alsahw_pcm_status_intrp(hdl) == 0 )
	    {
	      *status = &hdl->status;
	      // if ( hdl->status.fill > hdl->params.bufsize )
	      //fprintf(stderr, "GLITCH FROM INTRP %lu\n", (long)hdl->status.fill);
	      return 0;
	    }
	}
    }
  
  hdl->err = snd_pcm_status(hdl->handle, hdl->alsa_status);
  if ( ! hdl->err )
    {
      state = snd_pcm_status_get_state(hdl->alsa_status);
      if ( state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_PREPARED )
	{
	  switch(state)
	    {
	    case SND_PCM_STATE_OPEN:
	    case SND_PCM_STATE_SETUP:
	      hdl->err = -EBADF;
	      break;
	    case SND_PCM_STATE_XRUN:
	      hdl->err = -EPIPE;
	      break;
	    case SND_PCM_STATE_DRAINING:
	      hdl->err = -EBADF;
	      break;
	    case SND_PCM_STATE_PAUSED:
	      hdl->err = -ESTRPIPE;
	      break;
	    case SND_PCM_STATE_SUSPENDED:
	      hdl->err = -ESTRPIPE;
	      break;
	    case SND_PCM_STATE_DISCONNECTED:
	      hdl->err = -ENODEV;
	      break;
	    default:
	      hdl->err = -EBADF;
	      break;
	    }
	  hdl->status.error = hdl->err;
	  *status = &hdl->status;
	  return hdl->err;
	}
      
      hdl->status.error = hdl->err;
      snd_pcm_status_get_trigger_htstamp(hdl->alsa_status, &hts);
      snd_pcm_status_get_htstamp(hdl->alsa_status, &tts);
      if ( tts.tv_sec == 0 && tts.tv_nsec == 0 )
	hdl->status.tstamp = (hts.tv_sec * 1000000000ULL) + hts.tv_nsec;
      else
	hdl->status.tstamp = (tts.tv_sec * 1000000000ULL) + tts.tv_nsec;

      hdl->xfer = 0;
      if ( hdl->stream == SND_PCM_STREAM_PLAYBACK )
	{
	  hdl->status.space = snd_pcm_status_get_avail(hdl->alsa_status);
	  hdl->status.fill = hdl->buffer_size - hdl->status.space;
	  p = hdl->status.appl_ptr - hdl->status.fill;
	  hdl->status.hw_ptr = p;
	  if ( hdl->vbufsize < hdl->status.fill )
	    hdl->status.space = 0;
	  else
	    hdl->status.space = hdl->vbufsize - hdl->status.fill;
	  hdl->status.delay = snd_pcm_status_get_delay(hdl->alsa_status);
	  /*if ( hdl->status.delay > 0 )
	    {
	      //assert(hdl->status.delay >= hdl->status.fill);
	      //hdl->status.delay -= hdl->status.fill;
	      //hdl->status.delay *= 1000000000 / hdl->params.rate;
	      }*/
	} else
	{
	  hdl->status.fill = snd_pcm_status_get_avail(hdl->alsa_status);
	  hdl->status.space = hdl->buffer_size - hdl->status.fill;
	  hdl->status.hw_ptr = hdl->status.appl_ptr + hdl->status.fill;
	}
      if ( state == SND_PCM_STATE_RUNNING )
	hdl->got_tstamp = 1;

      if ( hdl->status.fill > hdl->buffer_size ||
	   hdl->status.space > hdl->buffer_size )
	hdl->err = -EPIPE;
      if ( hdl->status.fill > hdl->params.bufsize )
	fprintf(stderr, "GLITCH FROM ALSA %lu %lu %u %u %lu\n", (long)hdl->status.fill,
		snd_pcm_status_get_avail(hdl->alsa_status), hdl->params.bufsize, hdl->status.space,
		(long)hdl->buffer_size);
      *status = &hdl->status;
      
    }
  return hdl->err;
}

intptr_t alsahw_pcm_adjust_pointer(void *handle, intptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  intptr_t ret;
  if ( hdl->err )
    ret = 0;
  else
    ret = frames;
  hdl->status.appl_ptr += frames;
  if ( hdl->stream == SND_PCM_STREAM_PLAYBACK )
    {
       hdl->status.delay += frames;
       hdl->status.fill += frames;
       calculate_playback_space(hdl);
    } else
    {
       hdl->status.delay -= frames;
       hdl->status.fill -= frames;
       hdl->status.space += frames;
    }
  return ret;
}

intptr_t alsahw_pcm_rewind(void *handle, uintptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  snd_pcm_sframes_t ret;
  if ( hdl->err )
    return hdl->err;
  ret = snd_pcm_rewind(hdl->handle, frames);
  if ( ret < 0 )
    {
      hdl->err = ret;
    } else
    {
      hdl->status.appl_ptr -= ret;
      hdl->status.delay -= ret;
      hdl->status.fill -= ret;
      calculate_playback_space(hdl);
    }
  return ret;
}

intptr_t alsahw_pcm_forward(void *handle, uintptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  uintptr_t maxfw;
  snd_pcm_sframes_t ret;
  if ( hdl->err )
    return hdl->err;
  maxfw = hdl->erase_ptr - hdl->status.appl_ptr;
  if ( frames > maxfw )
    frames = maxfw;
  ret = snd_pcm_forward(hdl->handle, frames);
  if ( ret < 0 )
    {
      hdl->err = ret;
    } else
    {
      hdl->status.appl_ptr += ret;
      hdl->status.delay += ret;
      hdl->status.fill += ret;
    }
  return ret;
}

intptr_t alsahw_pcm_capture_rewindable(void *handle)
{
  struct alsahw_handle *hdl = handle;
  intptr_t ret;
  uint64_t minptr;
  if ( hdl->err )
    {
      ret = hdl->err;
    } else
    {
      if ( hdl->is_rewound )
	{
	  minptr = hdl->saved_appl - hdl->samples_read;
	  if ( hdl->status.appl_ptr < minptr )
	    {
	      ret = 0;
	    } else
	    {
	      ret = hdl->status.appl_ptr - minptr;
	    }
	} else
	{
	  ret = hdl->samples_read;
	}
    }
  return ret;
}

intptr_t alsahw_pcm_capture_forward(void *handle, uintptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  uintptr_t maxfw, f = frames;
  intptr_t ret;
  if ( hdl->is_rewound )
    {
      maxfw = hdl->saved_appl - hdl->status.appl_ptr;
      if ( maxfw > frames )
	maxfw = frames;
      hdl->status.appl_ptr += maxfw;
      hdl->status.delay -= maxfw;
      hdl->status.fill -= maxfw;
      hdl->status.space += maxfw;
      frames -= maxfw;
    }
  if ( hdl->status.appl_ptr >= hdl->saved_appl )
    hdl->is_rewound = 0;
  if ( frames )
    {
      ret = snd_pcm_forward(hdl->handle, frames);
      if ( ret < 0 )
	{
	  hdl->err = ret;
	} else
	{
	  frames -= ret;
	  hdl->status.appl_ptr += ret;
	  hdl->status.delay -= ret;
	  hdl->status.fill -= ret;
	  hdl->status.space += ret;
	}
    }
  if ( hdl->err )
    ret = hdl->err;
  else
    ret = f - frames;
  return ret;
}

intptr_t alsahw_pcm_capture_rewind(void *handle, uintptr_t frames)
{
  struct alsahw_handle *hdl = handle;
  uintptr_t rw, f = frames;
  intptr_t ret;
  if ( ! hdl->is_rewound )
    {
      if ( hdl->samples_read < frames )
	rw = hdl->samples_read;
      else
	rw = frames;
      hdl->saved_appl = hdl->status.appl_ptr;
      hdl->status.appl_ptr -= rw;
      hdl->status.fill += rw;
      hdl->status.delay += rw;
      hdl->status.space -= rw;
      frames -= rw;
    }
  if ( hdl->status.appl_ptr < hdl->saved_appl )
    hdl->is_rewound = 1;
  if ( frames )
    {
      // fprintf(stderr, "ALSA REWIND!!!!!\n");
      ret = snd_pcm_rewind(hdl->handle, frames);
      if ( ret < 0 )
	{
	  hdl->err = ret;
	} else
	{
	  frames -= ret;
	  hdl->status.appl_ptr -= ret;
	  hdl->status.fill += ret;
	  hdl->status.delay += ret;
	  hdl->status.space -= ret;
	}
    }
  if ( hdl->err )
    ret = hdl->err;
  else
    ret = f - frames;
  return ret;
}



intptr_t alsahw_pcm_rewindable(void *handle)
{
  struct alsahw_handle *hdl = handle;
  uint64_t now, diff;
  intptr_t maxframes;
  snd_pcm_sframes_t ret;
  if ( hdl->err )
    return hdl->err;
  now = dspd_get_time();
  diff = (now - hdl->status.tstamp) / hdl->sample_time;
  maxframes = hdl->status.appl_ptr - (hdl->status.hw_ptr+diff);
  ret = snd_pcm_rewindable(hdl->handle);
  if ( ret >= 0 )
    {
      //Use smallest (most conservative) value
      if ( ret > maxframes )
	ret = maxframes;

      //Try to stay at least 1 dma chunk ahead of hw pointer.
      maxframes = ret - (ret % hdl->min_dma);
      maxframes -= hdl->min_dma;
      if ( maxframes < 0 )
	ret = 0;
      else
	ret = maxframes;
    } else
    {
      hdl->err = ret;
    }
  return ret;
}

void alsahw_set_volume(void *handle, double volume)
{
  struct alsahw_handle *hdl = handle;
  hdl->volume = volume;
}

/*
  This works for mmap and system timers.  It won't work if the device IRQ is used.
  That will require changing avail_min and using a multiple of the fragment size.

  The only thing that a driver really needs to implement is buffer_size.  That is
  actually the maximum latency that is currently wanted.  The latency argument is
  a hint for optimization.  It is possible that a server may set a latency lower
  than the buffer size rewind a lot.  It is also possible that a server may be
  configured to disable clock interpolation (latency=0).  Even in that case it is
  optional for the driver to do anything with the latency hint.

*/
uintptr_t alsahw_set_latency(void *handle, uintptr_t buffer_size, uintptr_t latency)
{
  struct alsahw_handle *hdl = handle;

  //The virtual buffer size is the maximum amount that the server wants to fill.
  //Most of the time buffer_size==latency.
  if ( buffer_size < hdl->params.min_latency )
    hdl->vbufsize = hdl->params.min_latency;
  else if ( buffer_size > hdl->params.max_latency )
    hdl->vbufsize = hdl->params.max_latency;
  else
    hdl->vbufsize = buffer_size;
  if ( hdl->xfer < hdl->vbufsize )
    hdl->xfer = hdl->vbufsize; //Reset interpolation because latency changes make glitches more likely.

  //Set a latency hint.  Don't go above the ALSA buffer size since that is unlikely to work.
  if ( latency > hdl->params.bufsize )
    hdl->hlatency = hdl->params.bufsize;
  else
    hdl->hlatency = latency; //A value of zero disables interpolation.

  //The return value is the latency.  This call does not fail.
  //That means if the device is in a recoverable error state (usually xrun) then 
  //the buffer size must be applied when the device is ready again or in this case
  //immediately since ALSA has no knowledge of what is going on here.
  return hdl->vbufsize;
}

int32_t alsahw_poll_descriptors_count(void *handle)
{
  struct alsahw_handle *hdl = handle;
  return snd_pcm_poll_descriptors_count(hdl->handle);
}

int32_t alsahw_poll_descriptors(void *handle, 
				 struct pollfd *pfds,
				 uint32_t space)
{
  struct alsahw_handle *hdl = handle;
  return snd_pcm_poll_descriptors(hdl->handle,
				  pfds,
				  space);
}

int32_t alsahw_poll_revents(void *handle,
			     struct pollfd *pfds,
			     uint32_t nfds,
			     uint16_t *revents)
{
  struct alsahw_handle *hdl = handle;
  return snd_pcm_poll_descriptors_revents(hdl->handle,
					  pfds,
					  nfds,
					  revents);
}

int32_t alsahw_get_params(void *handle,
			   struct dspd_drv_params *params)
{
  struct alsahw_handle *hdl = handle;
  memcpy(params, &hdl->params, sizeof(*params));
  return 0;
}

static int alsahw_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, int format)
{
  int err, i;
  snd_pcm_format_mask_t *mask;
  snd_pcm_format_mask_alloca(&mask);
  //Try requested format.
  err = snd_pcm_hw_params_set_format(pcm, hwp, format);
  if ( err )
    {
      //Get a mask of supported formats.
      snd_pcm_hw_params_get_format_mask(hwp, mask);
      //Look for a linear format that is supported in software and hardware
      format = -1;
      for ( i = SND_PCM_FORMAT_LAST; i >= 0; i-- )
	{
	  if ( snd_pcm_format_mask_test(mask, i) && 
	       snd_pcm_format_linear(i) &&
	       dspd_getconv(i) )
	    {
	      err = snd_pcm_hw_params_set_format(pcm, hwp, i);
	      if ( err == 0 )
		{
		  format = i;
		  break;
		}
	    }
	}
    }
  return format;
}

static int alsahw_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, unsigned int channels)
{
  int err;
  err = snd_pcm_hw_params_set_channels(pcm, hwp, channels);
  if ( err )
    err = snd_pcm_hw_params_set_channels_near(pcm, hwp, &channels);
  if ( err )
    err = snd_pcm_hw_params_set_channels_last(pcm, hwp, &channels);
  if ( err )
    err = snd_pcm_hw_params_set_channels_first(pcm, hwp, &channels);
  if ( err )
    channels = err;
  return channels;
}

static int alsahw_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, unsigned int rate)
{
  int err, d;
  d = 0;
  err = snd_pcm_hw_params_set_rate(pcm, hwp, rate, d);
  if ( err )
    {
      d = 0;
      err = snd_pcm_hw_params_set_rate_near(pcm, hwp, &rate, &d);
    }
  if ( err )
    {
      d = 0;
      err = snd_pcm_hw_params_set_rate_max(pcm, hwp, &rate, &d);
    }
  if ( err )
    {
      d = 0;
      err = snd_pcm_hw_params_set_rate_min(pcm, hwp, &rate, &d);
    }
  if ( err == 0 )
    err = rate;
  return err;
}

static int alsahw_set_fragsize(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, unsigned int size)
{
  int d, err;
  snd_pcm_uframes_t fr = size;
  d = 0;
  err = snd_pcm_hw_params_set_period_size(pcm, hwp, size, d);
  if ( err )
    err = snd_pcm_hw_params_set_period_size_near(pcm, hwp, &fr, &d);
     
  if ( err == 0 )
    err = fr;
  return err;
}

static int alsahw_set_bufsize(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, unsigned int size)
{
  int err;
  snd_pcm_uframes_t fr = size;;
  err = snd_pcm_hw_params_set_buffer_size(pcm, hwp, size);
  if ( err )
    err = snd_pcm_hw_params_set_buffer_size_near(pcm, hwp, &fr);
  if ( err == 0 )
    err = fr;
  return err;
}

static int alsahw_is_batch(snd_pcm_t *pcm, snd_pcm_hw_params_t *hwp, const char *bus, const char *addr, const struct alsahw_mcfg *cfg)
{
  int ret = 0;
  int32_t t;
  if ( snd_pcm_hw_params_is_batch(hwp) ||
       snd_pcm_hw_params_is_double(hwp) ||
       snd_pcm_type(pcm) != SND_PCM_TYPE_HW )
    {
      ret = 1;
    } else if ( bus != NULL )
    {
      t = (1000000000 / cfg->max_rate) * cfg->fragsize;
      if ( strcasecmp(bus, "USB") == 0 || strcasecmp(bus, "IEEE1394") == 0 || t >= 1000000 )
	ret = 1;
    }
  return  ret;
}

static int get_min_cfg(snd_pcm_t *handle, snd_pcm_hw_params_t *hwp, struct alsahw_mcfg *cfg)
{
  unsigned int val;
  int ret;
  snd_pcm_format_t fmt;
  snd_pcm_uframes_t fr;
  int d;
  memset(cfg, 0, sizeof(*cfg));
  ret = snd_pcm_hw_params_any(handle, hwp);
  if ( ret )
    return ret;

  //Only support interleaved MMAP for now.
  ret = snd_pcm_hw_params_set_access(handle, hwp, SND_PCM_ACCESS_MMAP_INTERLEAVED);
  if ( ret )
    {
      ret = snd_pcm_hw_params_set_access(handle, hwp, SND_PCM_ACCESS_RW_INTERLEAVED);
      if ( ret )
	return ret;
    }

  ret = snd_pcm_hw_params_set_format_first(handle, hwp, &fmt);
  if ( ret )
    return ret;
  cfg->format = fmt;

  val = 2;
  val = 1;
  ret = snd_pcm_hw_params_set_channels_min(handle, hwp, &val);
  if ( ret )
    return ret;
  cfg->channels = val;

  d = 0;
  val = 11025;
  ret = snd_pcm_hw_params_set_rate_min(handle, hwp, &val, &d);
  if ( ret )
    return ret;
  cfg->rate = val;

  fr = 1;
  ret = snd_pcm_hw_params_set_buffer_size_min(handle, hwp, &fr);
  if ( ret )
    return ret;
  cfg->bufsize = fr;
  
  d = 0;
  fr = 1;
  ret = snd_pcm_hw_params_set_period_size_min(handle, hwp, &fr, &d);
  if ( ret )
    return ret;
  cfg->fragsize = fr;
  
  val = 192000;
  d = -1;
  ret = snd_pcm_hw_params_set_rate_max(handle, hwp, &val, &d);
  if ( ret == 0 )
    cfg->max_rate = val;
  else
    cfg->max_rate = cfg->rate;

  return 0;
}

/*
  Only works with min config.  The idea is that the minimum possible fragment size
  is the real dma size or close to it (probably larger).  This is important to know
  because it is unsafe to get within this distance of the hardware pointer.
*/
static int get_min_dma(struct alsahw_mcfg *cfg)
{
  size_t ret;
  ret = snd_pcm_format_size(cfg->format, cfg->channels);
  return ret * cfg->fragsize;
}

static void destroy_ctl(struct alsahw_handle *hdl)
{
  size_t i;
  for ( i = 0; i < hdl->elements_count; i++ )
    free(hdl->elements[i].sid);
  free(hdl->elements);
  hdl->elements = NULL;
  hdl->elements_count = 0;
  if ( hdl->mixer )
    snd_mixer_close(hdl->mixer);
  hdl->mixer = NULL;
}

static void alsahw_destructor(void *handle)
{
  struct alsahw_handle *hdl = handle;
  destroy_ctl(hdl);
  if ( hdl->handle )
    snd_pcm_close(hdl->handle);
  dspd_mutex_destroy(&hdl->mixer_lock);
  free(hdl->alsa_status);
  free(hdl->swparams);
  free(hdl->hwparams);
  free(hdl->buffer.addr);
  free(hdl->channel_map);
  free(hdl->params.desc);
  free(hdl->params.name);
  free(hdl->params.bus);
  free(hdl->params.addr);
  free(handle);
}


static int32_t alsa_mixer_elem_count(struct dspd_rctx *rctx,
				     uint32_t          req,
				     const void       *inbuf,
				     size_t            inbufsize,
				     void             *outbuf,
				     size_t            outbufsize)
{
  struct alsahw_handle *hdl = dspd_req_userdata(rctx);
  uint32_t count = hdl->elements_count;
  return dspd_req_reply_buf(rctx, 0, &count, sizeof(count));
}
static int32_t alsa_mixer_elem_info(struct dspd_rctx *rctx,
				    uint32_t          req,
				    const void       *inbuf,
				    size_t            inbufsize,
				    void             *outbuf,
				    size_t            outbufsize)
{
  uint32_t idx = *(uint32_t*)inbuf;
  struct alsahw_handle *hdl = dspd_req_userdata(rctx);
  int32_t ret;
  struct dspd_mix_info *info = outbuf;
  struct alsahw_mix_elem *elem;
  const char *name = NULL;
  int i;
  dspd_mutex_lock(&hdl->mixer_lock);
  if ( idx < hdl->elements_count )
    {
      memset(info, 0, sizeof(*info));
      elem = &hdl->elements[idx];
      snd_mixer_elem_t *e = snd_mixer_find_selem(hdl->mixer, elem->sid);
      if ( e )
	name = snd_mixer_selem_get_name(e);
      if ( name )
	{
	   info->flags = elem->flags;
	   info->tstamp = elem->tstamp;
	   info->update_count = elem->update_count;
	   info->ctl_index = snd_mixer_selem_get_index(e);
	   strlcpy(info->name, name, sizeof(info->name));
	   
	   if ( elem->flags & DSPD_MIXF_PLAYBACK )
	     {
	       for ( i = 0; i < DSPD_MIXER_CHN_LAST; i++ )
		 if ( snd_mixer_selem_has_playback_channel(e, i) )
		   info->pchan_mask |= 1 << i;
	     }
	   if ( elem->flags & DSPD_MIXF_CAPTURE )
	     {
	       for ( i = 0; i < DSPD_MIXER_CHN_LAST; i++ )
		 if ( snd_mixer_selem_has_capture_channel(e, i) )
		   info->cchan_mask |= 1 << i;
	     }

	   ret = 0;
	} else
	{
	  ret = -EINVAL;
	}
    } else
    {
      ret = -EINVAL;
    }
  dspd_mutex_unlock(&hdl->mixer_lock);
  if ( ret )
    ret = dspd_req_reply_err(rctx, 0, ret);
  else
    ret = dspd_req_reply_buf(rctx, 0, info, sizeof(*info));
  return ret;
}

static void find_volume_for_enum(struct alsahw_handle *handle, struct dspd_mix_info *info)
{
  size_t i;
  const struct alsahw_mix_elem *elem;
  const char *name;
  int32_t match1 = -1, match2 = -1;
  size_t len = strlen(info->name);
  int32_t flags;
  if ( info->flags & DSPD_MIXF_PLAYBACK )
    flags = DSPD_MIXF_PLAYBACK;
  else
    flags = DSPD_MIXF_CAPTURE;
  for ( i = 0; i < handle->elements_count; i++ )
    {
      elem = &handle->elements[i];
      if ( (elem->flags & DSPD_MIXF_VOL) && (elem->flags & flags) )
	{
	  snd_mixer_elem_t *e = snd_mixer_find_selem(handle->mixer, elem->sid);
	  if ( e )
	    {
	      name = snd_mixer_selem_get_name(e);
	      if ( name )
		{
		  if ( strcmp(info->name, name) != 0 )
		    {
		      if ( strstr(name, info->name) == name && match2 == -1 )
			{
			  //Close match
			  if ( isspace(name[len]) )
			    match2 = i;
			}
		    } else
		    {
		      //Exact match
		      match1 = i;
		      break;
		    }
		}
	    }
	}
    }
  if ( match1 >= 0 )
    info->vol_index = match1;
  else
    info->vol_index = match2;
}

static int32_t alsa_mixer_enum_info(struct dspd_rctx *rctx,
				    uint32_t          req,
				    const void       *inbuf,
				    size_t            inbufsize,
				    void             *outbuf,
				    size_t            outbufsize)
{
  const struct dspd_mix_enum_idx *idx = inbuf;
  struct alsahw_handle *hdl = dspd_req_userdata(rctx);
  int32_t ret;
  struct dspd_mix_info *info = outbuf;
  struct alsahw_mix_elem *elem;
  dspd_mutex_lock(&hdl->mixer_lock);
  if ( idx->elem_idx < hdl->elements_count )
    {
      memset(info, 0, sizeof(*info));
      elem = &hdl->elements[idx->elem_idx];
      snd_mixer_elem_t *e = snd_mixer_find_selem(hdl->mixer, elem->sid);
      if ( e && (elem->flags & DSPD_MIXF_ENUM) )
	{
	  info->flags = elem->flags;
	  ret = snd_mixer_selem_get_enum_item_name(e, idx->enum_idx, sizeof(info->name), info->name);
	  if ( ret == 0 )
	    {
	      info->ctl_index = idx->elem_idx;
	      find_volume_for_enum(hdl, info);
	    }
	} else
	{
	  ret = -EINVAL;
	}
    } else
    {
      ret = -EINVAL;
    }
  dspd_mutex_unlock(&hdl->mixer_lock);

  if ( ret )
    ret = dspd_req_reply_err(rctx, 0, ret);
  else
    ret = dspd_req_reply_buf(rctx, 0, info, sizeof(*info));

  return ret;
}

static void scale_to_pct(long *val, long minval, long maxval)
{
  double pct, p;
  if ( *val == minval )
    {
      *val = 0;
    } else if ( *val == maxval )
    {
      *val = 100;
    } else 
    {
      pct = abs(maxval - minval) / (double)100.0;
      p = abs(*val - minval);
      *val = lrint(p / pct);
    }
}
static void scale_from_pct(long *val, long minval, long maxval, int dir)
{
  double pct, p;
  if ( *val == 0 )
    {
      *val = minval;
    } else if ( *val == 100 )
    {
      *val = maxval;
    } else 
    {
      pct = abs(maxval - minval) / (double)100.0;
      p = (*val) * pct;
      if ( dir < 0 )
	p = floor(p);
      else if ( dir > 0 )
	p = ceil(p);
      *val = lrint(p) + minval;
    }
}


static int32_t alsa_mixer_elem_getval(struct dspd_rctx *rctx,
				      uint32_t          req,
				      const void       *inbuf,
				      size_t            inbufsize,
				      void             *outbuf,
				      size_t            outbufsize)
{
  struct alsahw_handle *hdl = dspd_req_userdata(rctx);
  const struct dspd_mix_val *cmd = inbuf;
  struct dspd_mix_val *val = outbuf;
  struct alsahw_mix_elem *elem;
  snd_mixer_elem_t *e;
  int err = -EINVAL;
  long l, minval, maxval, v;
  int i;
  if ( req == DSPD_SCTL_SERVER_MIXER_GETVAL )
    dspd_mutex_lock(&hdl->mixer_lock);
  
  if ( cmd->index == UINT32_MAX )
    {
      memset(val, 0, sizeof(*val));
      val->index = cmd->index;
      val->update_count = hdl->mixer_update_count;
      val->tstamp = hdl->mixer_tstamp;
    } else if ( cmd->index < hdl->elements_count )
    {
      elem = &hdl->elements[cmd->index];
      if ( (elem->flags & cmd->type) != cmd->type )
	goto error;
      e = snd_mixer_find_selem(hdl->mixer, elem->sid);
      if ( ! e )
	goto error;
      switch(cmd->type)
	{
	case DSPD_MIXF_PVOL:
	  if ( cmd->channel == DSPD_MIX_CONVERT && req == DSPD_SCTL_SERVER_MIXER_GETVAL )
	    {
	      v = cmd->value;
	      if ( cmd->flags & DSPD_CTRLF_SCALE_PCT )
		{
		  err = snd_mixer_selem_get_playback_volume_range(e, &minval, &maxval);
		  if ( err )
		    goto error;
		  scale_from_pct(&v, minval, maxval, cmd->dir);
		}
	      err = snd_mixer_selem_ask_playback_vol_dB(e, v, &l);
	    } else
	    {
	      err = snd_mixer_selem_get_playback_volume(e, cmd->channel, &l);
	      if ( err == 0 && (cmd->flags & DSPD_CTRLF_SCALE_PCT) )
		{
		  err = snd_mixer_selem_get_playback_volume_range(e, &minval, &maxval);
		  if ( err == 0 )
		    scale_to_pct(&l, minval, maxval);
		}
	    }
	  if ( err )
	    goto error;

	  break;
	case DSPD_MIXF_CVOL:
	  if ( cmd->channel == DSPD_MIX_CONVERT && req == DSPD_SCTL_SERVER_MIXER_GETVAL )
	    {
	      v = cmd->value;
	      if ( cmd->flags & DSPD_CTRLF_SCALE_PCT )
		{
		  err = snd_mixer_selem_get_capture_volume_range(e, &minval, &maxval);
		  if ( err )
		    goto error;
		  scale_from_pct(&v, minval, maxval, cmd->dir);
		}
	      err = snd_mixer_selem_ask_capture_vol_dB(e, v, &l);
	    } else
	    {
	      err = snd_mixer_selem_get_capture_volume(e, cmd->channel, &l);
	      if ( err == 0 && (cmd->flags & DSPD_CTRLF_SCALE_PCT) )
		{
		  err = snd_mixer_selem_get_capture_volume_range(e, &minval, &maxval);
		  if ( err == 0 )
		    scale_to_pct(&l, minval, maxval);
		}
	    }
	  if ( err )
	    goto error;
	  break;
	case DSPD_MIXF_PSWITCH:
	  if ( (err = snd_mixer_selem_get_playback_switch(e, cmd->channel, &i)) )
	    goto error;
	  l = i;
	  break;
	case DSPD_MIXF_CSWITCH:
	  if ( (err = snd_mixer_selem_get_capture_switch(e, cmd->channel, &i)) )
	    goto error;
	  l = i;
	  break;
	case DSPD_MIXF_CDB:
	  if ( cmd->channel == DSPD_MIX_CONVERT && req == DSPD_SCTL_SERVER_MIXER_GETVAL )
	    {
	      err = snd_mixer_selem_ask_capture_dB_vol(e, cmd->value, cmd->dir, &l);
	      if ( (cmd->flags & DSPD_CTRLF_SCALE_PCT) && err == 0 )
		{
		  err = snd_mixer_selem_get_capture_volume_range(e, &minval, &maxval);
		  if ( err == 0 )
		    scale_to_pct(&l, minval, maxval);
		}
	    } else
	    {
	      err = snd_mixer_selem_get_capture_dB(e, cmd->channel, &l);
	      if ( (cmd->flags & DSPD_CTRLF_SCALE_PCT) && err == 0 )
		{
		  err = snd_mixer_selem_get_capture_dB_range(e, &minval, &maxval);
		  if ( err == 0 )
		    scale_to_pct(&l, minval, maxval);
		}
	    }
	  if ( err )
	    goto error;
	  break;
	case DSPD_MIXF_PDB:
	  if ( cmd->channel == DSPD_MIX_CONVERT && req == DSPD_SCTL_SERVER_MIXER_GETVAL )
	    {
	      err = snd_mixer_selem_ask_playback_dB_vol(e, cmd->value, cmd->dir, &l);
	      if ( (cmd->flags & DSPD_CTRLF_SCALE_PCT) && err == 0 )
		{
		  err = snd_mixer_selem_get_playback_volume_range(e, &minval, &maxval);
		  if ( err == 0 )
		    scale_to_pct(&l, minval, maxval);
		}
	    } else
	    {
	      err = snd_mixer_selem_get_playback_dB(e, cmd->channel, &l);
	      if ( (cmd->flags & DSPD_CTRLF_SCALE_PCT) && err == 0 )
		{
		  err = snd_mixer_selem_get_playback_dB_range(e, &minval, &maxval);
		  if ( err == 0 )
		    scale_to_pct(&l, minval, maxval);
		}
	    }
	  if ( err )
	    goto error;
	  break;
	case DSPD_MIXF_ENUM:
	  err = snd_mixer_selem_get_enum_item(e, cmd->channel, (unsigned int*)&i);
	  if ( err )
	    goto error;
	  l = i;
	  break;
	default:
	  goto error;
	}
      memcpy(val, cmd, sizeof(*cmd));
      val->value = l;
      val->hwinfo = 0;
      val->tstamp = elem->tstamp;
      val->update_count = elem->update_count;
    } else
    {
      goto error;
    }
  dspd_mutex_unlock(&hdl->mixer_lock);
  return dspd_req_reply_buf(rctx, 0, val, sizeof(*val));

 error:
  dspd_mutex_unlock(&hdl->mixer_lock);
  return dspd_req_reply_err(rctx, 0, err);
}




static int32_t alsa_mixer_elem_setval(struct dspd_rctx *rctx,
				      uint32_t          req,
				      const void       *inbuf,
				      size_t            inbufsize,
				      void             *outbuf,
				      size_t            outbufsize)
{
  struct alsahw_handle *hdl = dspd_req_userdata(rctx);
  const struct dspd_mix_val *cmd = inbuf;
  struct alsahw_mix_elem *elem;
  snd_mixer_elem_t *e;
  int err = -EINVAL;
  int i;
  bool success;
  long l, minval, maxval;
  dspd_mutex_lock(&hdl->mixer_lock);

  if ( cmd->index < hdl->elements_count )
    {
      elem = &hdl->elements[cmd->index];
      if ( (elem->flags & cmd->type) != cmd->type )
	goto out;
      //OSSv4 compat: Use 32 bit ms timestamp instead of 64 bit ns.
      if ( cmd->flags & DSPD_CTRLF_TSTAMP_32BIT )
	{
	  uint32_t t = (elem->tstamp / 1000000ULL) % UINT32_MAX;
	  if ( t != cmd->tstamp )
	    {
	      err = -EIDRM;
	      goto out;
	    }
	} else if ( cmd->tstamp != 0 && cmd->tstamp != elem->tstamp )
	{
	  err = -EIDRM;
	  goto out;
	}

      e = snd_mixer_find_selem(hdl->mixer, elem->sid);
      if ( ! e )
	goto out;
      switch(cmd->type)
	{
	case DSPD_MIXF_PVOL:
	  l = cmd->value;
	  if ( cmd->flags & DSPD_CTRLF_SCALE_PCT )
	    {
	      err = snd_mixer_selem_get_playback_volume_range(e, &minval, &maxval);
	      if ( err )
		break;
	      scale_from_pct(&l, minval, maxval, cmd->dir);
	    }
	  if ( cmd->channel == -1 )
	    {
	      err = snd_mixer_selem_set_playback_volume_all(e, l);
	    } else
	    {
	      err = snd_mixer_selem_set_playback_volume(e, cmd->channel, l);
	    }
	  break;
	case DSPD_MIXF_CVOL:
	  l = cmd->value;
	  if ( cmd->flags & DSPD_CTRLF_SCALE_PCT )
	    {
	      err = snd_mixer_selem_get_capture_volume_range(e, &minval, &maxval);
	      if ( err )
		break;
	      scale_from_pct(&l, minval, maxval, cmd->dir);
	    }
	  if ( cmd->channel == -1 )
	    err = snd_mixer_selem_set_capture_volume_all(e, l);
	  else
	    err = snd_mixer_selem_set_capture_volume(e, cmd->channel, l);
	  break;
	case DSPD_MIXF_PSWITCH:
	  if ( cmd->channel == -1 )
	    err = snd_mixer_selem_set_playback_switch_all(e, cmd->value);
	  else
	    err = snd_mixer_selem_set_playback_switch(e, cmd->channel, cmd->value);
	  break;
	case DSPD_MIXF_CSWITCH:
	  if ( cmd->channel == -1 )
	    err = snd_mixer_selem_set_capture_switch_all(e, cmd->value);
	  else
	    err = snd_mixer_selem_set_capture_switch(e, cmd->channel, cmd->value);
	  break;
	case DSPD_MIXF_CDB:
	  l = cmd->value;
	  if ( cmd->flags & DSPD_CTRLF_SCALE_PCT )
	    {
	      err = snd_mixer_selem_get_capture_dB_range(e, &minval, &maxval);
	      if ( err )
		break;
	      scale_from_pct(&l, minval, maxval, cmd->dir);
	    }
	  if ( cmd->channel == -1 )
	    err = snd_mixer_selem_set_capture_dB_all(e, l, cmd->dir);
	  else
	    err = snd_mixer_selem_set_capture_dB(e, cmd->channel, l, cmd->dir);
	  break;
	case DSPD_MIXF_PDB:
	  l = cmd->value;
	  if ( cmd->flags & DSPD_CTRLF_SCALE_PCT )
	    {
	      err = snd_mixer_selem_get_playback_dB_range(e, &minval, &maxval);
	      if ( err )
		break;
	      scale_from_pct(&l, minval, maxval, cmd->dir);
	    }
	  if ( cmd->channel == -1 )
	    err = snd_mixer_selem_set_playback_dB_all(e, l, cmd->dir);
	  else
	    err = snd_mixer_selem_set_playback_dB(e, cmd->channel, l, cmd->dir);
	  break;
	case DSPD_MIXF_ENUM:
	  if ( cmd->channel == -1 )
	    {
	      success = false;
	      for ( i = 0; i <= DSPD_MIXER_CHN_LAST; i++ )
		{
		  if ( snd_mixer_selem_has_playback_channel(e, i) ||
		       snd_mixer_selem_has_capture_channel(e, i) )
		    {
		      err = snd_mixer_selem_set_enum_item(e, i, cmd->value);
		      if ( err == 0 )
			success = true;
		    }
		}  
	      //This means it probably worked.
	      if ( err && success )
		err = 0;
	    } else
	    {
	      err = snd_mixer_selem_set_enum_item(e, cmd->channel, cmd->value);
	    }
	  break;
	}
    }
  //dspd_mutex_unlock(&hdl->mixer_lock);
  //Save a round trip if the client has requested the results
  if ( err == 0 )
    {
      snd_mixer_handle_events(hdl->mixer);
      elem->update_count++;
      hdl->mixer_update_count++;
      if ( outbufsize >= sizeof(struct dspd_mix_val) )
	{
	  struct dspd_mix_val v;
	  if ( cmd->channel == -1 )
	    {
	     
	      v = *cmd;
	      inbuf = &v;
	      if ( cmd->type & DSPD_MIXF_PLAYBACK )
		{
		  for ( i = 0; i <= DSPD_MIXER_CHN_LAST; i++ )
		    {
		      if ( snd_mixer_selem_has_playback_channel(e, i) )
			{
			  v.channel = i;
			  break;
			}
		    }
		} else if ( cmd->type & DSPD_MIXF_CAPTURE )
		{
		  for ( i = 0; i <= DSPD_MIXER_CHN_LAST; i++ )
		    {
		      if ( snd_mixer_selem_has_capture_channel(e, i) )
			{
			  v.channel = i;
			  break;
			}
		    }
		}
	    }
	  return alsa_mixer_elem_getval(rctx, req, inbuf, inbufsize, outbuf, outbufsize);
	}
    }

 out:
  dspd_mutex_unlock(&hdl->mixer_lock);
  return dspd_req_reply_err(rctx, 0, err);
}

static int32_t alsa_mixer_elem_getrange(struct dspd_rctx *rctx,
					uint32_t          req,
					const void       *inbuf,
					size_t            inbufsize,
					void             *outbuf,
					size_t            outbufsize)
{
  struct alsahw_handle *hdl = dspd_req_userdata(rctx);
  const struct dspd_mix_val *cmd = inbuf;
  struct alsahw_mix_elem *elem;
  snd_mixer_elem_t *e;
  int err = -EINVAL;
  struct dspd_mix_range r;
  long minval, maxval;
  dspd_mutex_lock(&hdl->mixer_lock);
  if ( cmd->index < hdl->elements_count )
    {
      elem = &hdl->elements[cmd->index];
      e = snd_mixer_find_selem(hdl->mixer, elem->sid);
      if ( e != NULL && (elem->flags & cmd->type) == cmd->type )
	{
	  switch(cmd->type)
	    {
	    case DSPD_MIXF_PVOL:
	      err = snd_mixer_selem_get_playback_volume_range(e, &minval, &maxval);
	      r.min = minval;
	      r.max = maxval;
	      break;
	    case DSPD_MIXF_CVOL:
	      err = snd_mixer_selem_get_capture_volume_range(e, &minval, &maxval);
	      r.min = minval;
	      r.max = maxval;
	      break;
	    case DSPD_MIXF_PDB:
	      err = snd_mixer_selem_get_playback_dB_range(e, &minval, &maxval);
	      r.min = minval;
	      r.max = maxval;
	      break;
	    case DSPD_MIXF_CDB:
	      err = snd_mixer_selem_get_capture_dB_range(e, &minval, &maxval);
	      r.min = minval;
	      r.max = maxval;
	      break;
	    case DSPD_MIXF_ENUM:
	      err = snd_mixer_selem_get_enum_items(e);
	      if ( err >= 0 )
		{
		  r.max = err;
		  r.min = 0;
		  err = 0;
		}
	      break;
	    case DSPD_MIXF_PSWITCH:
	    case DSPD_MIXF_CSWITCH:
	      r.min = 0;
	      r.max = 1;
	      err = 0;
	      break;
	    }
	} 
    }
  dspd_mutex_unlock(&hdl->mixer_lock);

  
  if ( err == 0 )
    err = dspd_req_reply_buf(rctx, 0, &r, sizeof(r));
  else
    err = dspd_req_reply_err(rctx, 0, err);
  return err;
}


static int32_t alsa_mixer_set_cb(struct dspd_rctx *rctx,
				 uint32_t          req,
				 const void       *inbuf,
				 size_t            inbufsize,
				 void             *outbuf,
				 size_t            outbufsize)
{
  struct alsahw_handle *hdl = dspd_req_userdata(rctx);
  const struct dspd_mixer_cbinfo *cb = inbuf;
  int ret;
  //The mixer point is used as a reference (like intptr_t) so
  //this is safe without the mutex.
  if ( cb->remove )
    ret = alsahw_unregister_mixer_callback(global_notifier,
					   hdl->mixer, 
					   cb->callback,
					   cb->arg);
  else
    ret = alsahw_register_mixer_callback(global_notifier,
					 hdl->mixer,
					 cb->callback,
					 cb->arg);
  return dspd_req_reply_err(rctx, 0, ret);
}

static struct dspd_req_handler mixer_handlers[] = {
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ELEM_COUNT)] = {
    .handler = alsa_mixer_elem_count,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(uint32_t),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ELEM_INFO)] = {
    .handler = alsa_mixer_elem_info,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(uint32_t),
    .outbufsize = sizeof(struct dspd_mix_info),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ENUM_INFO)] = {
    .handler = alsa_mixer_enum_info,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_enum_idx),
    .outbufsize = sizeof(struct dspd_mix_info),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_GETVAL)] = {
    .handler = alsa_mixer_elem_getval,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = sizeof(struct dspd_mix_val),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_SETVAL)] = {
    .handler = alsa_mixer_elem_setval,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = 0,
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_GETRANGE)] = {
    .handler = alsa_mixer_elem_getrange,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = sizeof(struct dspd_mix_range),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_SETCB)] = { 
    .handler = alsa_mixer_set_cb,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mixer_cbinfo),
    .outbufsize = 0,
  },
};


static int32_t alsahw_stream_ioctl(struct dspd_rctx *rctx,
				   uint32_t          req,
				   const void       *inbuf,
				   size_t            inbufsize,
				   void             *outbuf,
				   size_t            outbufsize)
{
  int32_t ret = -ENOSYS;
  //  uint32_t stream;
  // struct alsahw_handle *hdl = dspd_req_userdata(rctx);
  struct dspd_dispatch_ctl2_info info;
  /*switch(req)
    {
      case DSPD_SCTL_SERVER_GETCHANNELMAP:
      if ( ! hdl->channel_map )
	break;
      //TODO: If sizeof(uint64_t) then last 32 bits is channel count
      if ( inbufsize < sizeof(stream) )
	{
	  ret = dspd_req_reply_err(rctx, 0, EINVAL);
	} else
	{
	  stream = *(uint32_t*)inbuf;
	  if ( ((hdl->stream == SND_PCM_STREAM_PLAYBACK) && (stream == DSPD_PCM_SBIT_PLAYBACK)) ||
	       ((hdl->stream == SND_PCM_STREAM_CAPTURE) && (stream == DSPD_PCM_SBIT_CAPTURE)) )
	    {
	      ret = dspd_req_reply_buf(rctx, 0, hdl->channel_map, DSPD_CHMAP_SIZEOF(hdl->channel_map->channels));
	    } else if ( hdl->other_handle )
	    {
	      //Check other stream.
	      hdl = hdl->other_handle;
	      if ( hdl->stream == stream )
		ret = dspd_req_reply_buf(rctx, 0, hdl->channel_map, DSPD_CHMAP_SIZEOF(hdl->channel_map->channels));
	    }
	}
      break;
      default:*/
      info.min_req = DSPD_SCTL_SERVER_MIXER_FIRST;
      info.handlers_count = ARRAY_SIZE(mixer_handlers);
      info.req = req;
      info.handlers = mixer_handlers;
      ret = dspd_daemon_dispatch_ctl2(rctx,
				      &info,
				      inbuf,
				      inbufsize,
				      outbuf,
				      outbufsize);

      /*  break;
	  }*/
  return ret;
}

static void *alsahw_get_handle(void *handle, int stream)
{
  struct alsahw_handle *hdl = handle;
  if ( stream == hdl->stream )
    return hdl;
  return hdl->other_handle;
}

static int32_t alsahw_get_error(void *handle)
{
  struct alsahw_handle *hdl = handle;
  return hdl->err;
}

static void set_stream_index(void *handle, int32_t idx)
{
  struct alsahw_handle *hdl = handle;
  hdl->stream_index = idx;
}

static int32_t alsahw_get_chmap(void *handle, struct dspd_chmap *map)
{
  struct alsahw_handle *hdl = handle;
  memcpy(map, hdl->channel_map, dspd_chmap_sizeof(hdl->channel_map));
  return 0;
}


static const struct dspd_pcmdrv_ops alsa_mmap_read_ops = {
  .mmap_begin = alsahw_pcm_mmap_read_begin,
  .mmap_commit = alsahw_pcm_mmap_read_commit,
  .recover = alsahw_pcm_recover,
  .start = alsahw_pcm_start,
  .prepare = alsahw_pcm_prepare,
  .status = alsahw_pcm_status,
  .rewind = alsahw_pcm_capture_rewind,
  .forward = alsahw_pcm_capture_forward,
  .rewindable = alsahw_pcm_capture_rewindable,
  .set_volume = alsahw_set_volume,
  .set_latency = alsahw_set_latency,
  .drop = alsahw_pcm_drop,
  .poll_descriptors_count = alsahw_poll_descriptors_count,
  .poll_descriptors = alsahw_poll_descriptors,
  .poll_revents = alsahw_poll_revents,
  .get_params = alsahw_get_params,
  .destructor = alsahw_destructor,
  .ioctl = alsahw_stream_ioctl,
  .get_handle = alsahw_get_handle,
  .get_error = alsahw_get_error,
  .set_stream_index = set_stream_index,
  .get_chmap = alsahw_get_chmap,
  .adjust_pointer = alsahw_pcm_adjust_pointer,
};
static const struct dspd_pcmdrv_ops alsa_read_ops = {
  .mmap_begin = alsahw_pcm_read_begin,
  .mmap_commit = alsahw_pcm_read_commit,
  .recover = alsahw_pcm_recover,
  .start = alsahw_pcm_start,
  .prepare = alsahw_pcm_prepare,
  .status = alsahw_pcm_status,
  .rewind = alsahw_pcm_capture_rewind,
  .forward = alsahw_pcm_capture_forward,
  .rewindable = alsahw_pcm_capture_rewindable,
  .set_volume = alsahw_set_volume,
  .set_latency = alsahw_set_latency,
  .drop = alsahw_pcm_drop,
  .poll_descriptors_count = alsahw_poll_descriptors_count,
  .poll_descriptors = alsahw_poll_descriptors,
  .poll_revents = alsahw_poll_revents,
  .get_params = alsahw_get_params,
  .destructor = alsahw_destructor,
  .ioctl = alsahw_stream_ioctl,
  .get_error = alsahw_get_error,
  .set_stream_index = set_stream_index,
  .get_chmap = alsahw_get_chmap,
  .adjust_pointer = alsahw_pcm_adjust_pointer,
};

static const struct dspd_pcmdrv_ops alsa_mmap_write_ops = {
  .mmap_begin = alsahw_pcm_mmap_begin,
  .mmap_commit = alsahw_pcm_mmap_commit,
  .recover = alsahw_pcm_recover,
  .start = alsahw_pcm_start,
  .prepare = alsahw_pcm_prepare,
  .status = alsahw_pcm_status,
  .rewind = alsahw_pcm_rewind,
  .forward = alsahw_pcm_forward,
  .rewindable = alsahw_pcm_rewindable,
  .set_volume = alsahw_set_volume,
  .set_latency = alsahw_set_latency,
  .drop = alsahw_pcm_drop,
  .poll_descriptors_count = alsahw_poll_descriptors_count,
  .poll_descriptors = alsahw_poll_descriptors,
  .poll_revents = alsahw_poll_revents,
  .get_params = alsahw_get_params,
  .destructor = alsahw_destructor,
  .ioctl = alsahw_stream_ioctl,
  .get_handle = alsahw_get_handle,
  .get_error = alsahw_get_error,
  .set_stream_index = set_stream_index,
  .get_chmap = alsahw_get_chmap,
  .adjust_pointer = alsahw_pcm_adjust_pointer,
};

static const struct dspd_pcmdrv_ops alsa_write_ops = {
  .mmap_begin = alsahw_pcm_write_begin,
  .mmap_commit = alsahw_pcm_write_commit,
  .recover = alsahw_pcm_recover,
  .start = alsahw_pcm_start,
  .prepare = alsahw_pcm_prepare,
  .status = alsahw_pcm_status,
  .rewind = alsahw_pcm_rewind,
  .forward = alsahw_pcm_forward,
  .rewindable = alsahw_pcm_rewindable,
  .set_volume = alsahw_set_volume,
  .set_latency = alsahw_set_latency,
  .drop = alsahw_pcm_drop,
  .poll_descriptors_count = alsahw_poll_descriptors_count,
  .poll_descriptors = alsahw_poll_descriptors,
  .poll_revents = alsahw_poll_revents,
  .get_params = alsahw_get_params,
  .destructor = alsahw_destructor,
  .ioctl = alsahw_stream_ioctl,
  .get_error = alsahw_get_error,
  .set_stream_index = set_stream_index,
  .get_chmap = alsahw_get_chmap,
  .adjust_pointer = alsahw_pcm_adjust_pointer,
};

static uint32_t get_min_frags(time_t sample_time, unsigned int min_dma)
{
  time_t t = min_dma * sample_time;
  time_t tick = dspd_get_tick();
  if ( t >= (tick * 2) )
    return 2;
  return 4;
}

#define CHECKPTR(_ptr) if(!_ptr){ret=errno;goto out;}

int alsahw_open(const struct dspd_drv_params *params,
		 const struct dspd_pcmdrv_ops **ops,
		 void **handle)
{
  int ret, i;
  unsigned int val, sample_time;
  snd_pcm_hw_params_t *hwp = NULL;
  snd_pcm_sw_params_t *swp = NULL;
  struct alsahw_handle *hbuf = NULL;
  struct alsahw_mcfg mincfg;
  const struct pcm_conv *conv;
  snd_pcm_chmap_t *chmap;
  int mmap = 0, batch = 0;
  unsigned int fragsize, bufsize;
  ret = snd_pcm_hw_params_malloc(&hwp);
  if ( ret )
    goto out;
  ret = snd_pcm_sw_params_malloc(&swp);
  if ( ret )
    goto out;
  hbuf = calloc(1, sizeof(*hbuf));
  if ( ! hbuf )
    {
      ret = -errno;
      goto out;
    }
  ret = snd_pcm_status_malloc(&hbuf->alsa_status);
  if ( ret )
    goto out;
  ret = dspd_mutex_init(&hbuf->mixer_lock, NULL);
  if ( ret )
    {
      ret *= -1;
      goto out;
    }
 
  ret = snd_pcm_open(&hbuf->handle, params->name, params->stream, SND_PCM_NONBLOCK);
  if ( ret )
    goto out;
  

  ret = get_min_cfg(hbuf->handle, hwp, &mincfg);
  if ( ret )
    goto out;

  
  hbuf->min_dma_bytes = get_min_dma(&mincfg);

  ret = snd_pcm_hw_params_any(hbuf->handle, hwp);
  if ( ret )
    goto out;


  ret = snd_pcm_hw_params_set_access(hbuf->handle, hwp, SND_PCM_ACCESS_MMAP_INTERLEAVED);
  if ( ret )
    {
      ret = snd_pcm_hw_params_set_access(hbuf->handle, hwp, SND_PCM_ACCESS_RW_INTERLEAVED);
      if ( ret )
	goto out;
    } else
    {
      mmap = 1;
    }

  bufsize = params->bufsize;
  fragsize = params->fragsize;


  hbuf->params.stream = params->stream;

  ret = alsahw_set_format(hbuf->handle, hwp, params->format);
  if ( ret < 0 )
    goto out;
  hbuf->params.format = ret;
  
  ret = alsahw_set_channels(hbuf->handle, hwp, params->channels);
  if ( ret < 0 )
    goto out;
  hbuf->params.channels = ret;

  ret = alsahw_set_rate(hbuf->handle, hwp, params->rate);
  if ( ret < 0 )
    goto out;
  hbuf->params.rate = ret;



  if ( alsahw_is_batch(hbuf->handle, hwp, params->bus, params->addr, &mincfg) )
    {
      dspd_log(0, "Device %s @ %s:%s is batch", 
	       params->name,
	       params->bus,
	       params->addr);
      /*
	Try for ~1ms fragments and an even sized buffer.
      */
      sample_time = (1000000000 / hbuf->params.rate);
      val = 1000000 / sample_time;
      fragsize = 1 << get_hpo2(val);
      val = bufsize / fragsize;
      if ( bufsize % fragsize )
	val++;
      if ( val < 4 )
	val = 4;
      else if ( val % 2 )
	val++;
      bufsize = val * fragsize;
      batch = 1;
    }

  ret = alsahw_set_bufsize(hbuf->handle, hwp, bufsize);
  if ( ret < 0 )
    goto out;
  hbuf->params.bufsize = ret;

  ret = alsahw_set_fragsize(hbuf->handle, hwp, fragsize);
  if ( ret < 0 )
    goto out;
  hbuf->params.fragsize = ret;
  //  fprintf(stderr, "CONFIG %d\n", (int)ret);
  ret = snd_pcm_hw_params(hbuf->handle, hwp);
  if ( ret )
    goto out;


  if ( params->desc )
    {
      hbuf->params.desc = strdup(params->desc);
      CHECKPTR(hbuf->params.desc);
    }
  if ( strncmp(params->name, "realhw:", 7) == 0 )
    hbuf->params.name = strdup(&params->name[4]);
  else
    hbuf->params.name = strdup(params->name);
  CHECKPTR(hbuf->params.name);
  if ( params->bus )
    {
      hbuf->params.bus = strdup(params->bus);
      CHECKPTR(hbuf->params.bus);
    }
  if ( params->addr )
    {
      hbuf->params.addr = strdup(params->addr);
      CHECKPTR(hbuf->params.addr);
    }

  hbuf->volume = 1.0;


  hbuf->buffer_size = params->bufsize;
  hbuf->channels = hbuf->params.channels;
  hbuf->frame_size = snd_pcm_format_size(hbuf->params.format, hbuf->channels);
  if ( batch )
    hbuf->min_dma = hbuf->params.fragsize;
  else
    hbuf->min_dma = hbuf->min_dma_bytes / hbuf->frame_size;
  hbuf->stream = params->stream;

  if ( ! batch )
    {
      for ( i = 0; i < 32; i++ )
	{
	  val = 1 << i;
	  if ( hbuf->min_dma == val )
	    {
	      break;
	    } else if ( val > hbuf->min_dma )
	    {
	      hbuf->min_dma = val;
	      break;
	    }
	} 
    }
  
  ret = snd_pcm_sw_params_current(hbuf->handle, swp);
  if ( ret )
    goto out;
  
  ret = snd_pcm_sw_params_set_tstamp_mode(hbuf->handle,
					  swp,
					  SND_PCM_TSTAMP_MMAP);
  if ( ret )
    goto out;

  ret = snd_pcm_sw_params_set_start_threshold(hbuf->handle,
					      swp,
					      INT32_MAX);
  if ( ret )
    goto out;

  ret = snd_pcm_sw_params_set_start_threshold(hbuf->handle,
					      swp,
					      INT32_MAX);
  if ( ret < 0 )
    goto out;

  ret = snd_pcm_sw_params(hbuf->handle, swp);
  if ( ret )
    goto out;



  if ( params->stream == SND_PCM_STREAM_PLAYBACK )
    hbuf->buffer.addr = calloc(hbuf->buffer_size, hbuf->channels * sizeof(*hbuf->buffer.addr64));
  else
    hbuf->buffer.addr = calloc(hbuf->buffer_size, hbuf->channels * sizeof(*hbuf->buffer.addr32));
  if ( ! hbuf->buffer.addr )
    {
      ret = -errno;
      goto out;
    }
  if ( ! mmap )
    {
      hbuf->hw_addr = calloc(hbuf->buffer_size, hbuf->channels * hbuf->frame_size);
      if ( ! hbuf->hw_addr )
	{
	  ret = -errno;
	  goto out;
	}
    }

  conv = dspd_getconv(hbuf->params.format);
  if ( ! conv )
    {
      ret = -EINVAL;
      goto out;
    }
  
  if ( params->stream == SND_PCM_STREAM_PLAYBACK )
    hbuf->convert.fromdouble = conv->fromfloat64wv;
  else
    hbuf->convert.tofloat = conv->tofloat32wv;

  unsigned int minf;

  hbuf->swparams = swp;
  hbuf->hwparams = hwp;
  hbuf->vbufsize = hbuf->buffer_size;
 
  hbuf->sample_time = 1000000000 / hbuf->params.rate;

  //It doesn't seem to do less than 1ms reliably.  That
  //might be fixed in the future.
  if ( params->min_latency == 0 )
    {
      minf = dspd_get_min_latency() / hbuf->sample_time;
      //hbuf->params.min_latency = hbuf->min_dma * 4;
      hbuf->params.min_latency = hbuf->min_dma * get_min_frags(hbuf->sample_time, hbuf->min_dma);
      while ( hbuf->params.min_latency < minf )
	hbuf->params.min_latency *= 2;
    } else
    {
      hbuf->params.min_latency = params->min_latency;
    }
  if ( hbuf->params.min_latency > hbuf->params.bufsize )
    hbuf->params.min_latency = hbuf->params.bufsize;
      
  
  if ( params->max_latency == 0 )
    hbuf->params.max_latency = hbuf->buffer_size;
  else
    hbuf->params.max_latency = params->max_latency;

  if ( hbuf->params.max_latency < hbuf->params.min_latency )
    hbuf->params.max_latency = hbuf->params.min_latency;
  else if ( hbuf->params.max_latency > hbuf->buffer_size )
    hbuf->params.max_latency = hbuf->buffer_size;

  

  hbuf->params.min_dma = hbuf->min_dma;

  /*
    Try to get a channel map.  It is not supported on all cards.  Many of those
    cards are 2 channel stereo so a default map will work just fine.
  */
  chmap = snd_pcm_get_chmap(hbuf->handle);
  if ( chmap )
    {
      hbuf->channel_map = calloc(1, dspd_chmap_bufsize(chmap->channels, 0));
      if ( ! hbuf->channel_map )
	goto out;
      for ( i = 0; i < chmap->channels; i++ )
	hbuf->channel_map->pos[i] = chmap->pos[i];
      hbuf->channel_map->channels = chmap->channels;
    } else
    {
      //Make a default channel map
      hbuf->channel_map = calloc(1, dspd_chmap_bufsize(hbuf->params.channels, 0));
      if ( ! hbuf->channel_map )
	goto out;
      hbuf->channel_map->channels = hbuf->params.channels;
      if ( hbuf->channel_map->channels == 1 )
	{
	  hbuf->channel_map->pos[0] = DSPD_CHMAP_MONO;
	} else
	{
	  for ( i = 0; i < hbuf->channel_map->channels; i++ )
	    {
	      hbuf->channel_map->pos[i] = i + DSPD_CHMAP_FL;
	    }
	}
    }
  if ( params->stream == SND_PCM_STREAM_PLAYBACK )
    hbuf->channel_map->stream = DSPD_PCM_SBIT_PLAYBACK;
  else
    hbuf->channel_map->stream = DSPD_PCM_SBIT_CAPTURE;


  ret = 0;

  if ( params->stream == SND_PCM_STREAM_PLAYBACK )
    {
      if ( ! mmap )
	*ops = &alsa_write_ops;
      else
	*ops = &alsa_mmap_write_ops;
    } else
    {
      if ( mmap )
	*ops = &alsa_mmap_read_ops;
      else
	*ops = &alsa_read_ops;
    }

 out:
  if ( ret )
    {
      if ( hbuf )
	alsahw_destructor(hbuf);
    } else
    {
      *handle = hbuf;
    }
  return ret;  
}

static int alsahw_score(void *arg, const struct dspd_dict *device)
{
  int ret;
  if ( dspd_dict_test_value(device, DSPD_HOTPLUG_DEVTYPE, "alsa") )
    ret = 127;
  else
    ret = 0;
  return ret;
}

static uint64_t dspd_alsa_selem_getflags(snd_mixer_elem_t *elem)
{
  uint64_t ret = 0;
  long maxval, minval;
  if ( snd_mixer_selem_has_playback_volume(elem) )
    ret |= DSPD_MIXF_PVOL;
  if ( snd_mixer_selem_has_capture_volume(elem) )
    ret |= DSPD_MIXF_CVOL;
  if ( snd_mixer_selem_is_playback_mono(elem) )
    ret |= DSPD_MIXF_PMONO;
  if ( snd_mixer_selem_is_capture_mono(elem) )
    ret |= DSPD_MIXF_CMONO;
  if ( snd_mixer_selem_has_playback_switch(elem) )
    ret |= DSPD_MIXF_PSWITCH;
  if ( snd_mixer_selem_has_capture_switch(elem) )
    ret |= DSPD_MIXF_CSWITCH;
  if ( snd_mixer_selem_get_playback_dB_range(elem, &minval, &maxval) == 0 &&
       (minval || maxval) )
    ret |= DSPD_MIXF_PDB;
  if ( snd_mixer_selem_get_capture_dB_range(elem, &minval, &maxval) == 0 &&
       (minval || maxval) )
    ret |= DSPD_MIXF_CDB;
  if ( snd_mixer_selem_has_common_volume(elem) )
    ret |= DSPD_MIXF_COMMVOL;
  if ( snd_mixer_selem_has_common_switch(elem) )
    ret |= DSPD_MIXF_COMMSWITCH;
  if ( snd_mixer_selem_has_playback_volume_joined(elem) )
    ret |= DSPD_MIXF_PVJOINED;
  if ( snd_mixer_selem_has_capture_volume_joined(elem) )
    ret |= DSPD_MIXF_CVJOINED;
  if ( snd_mixer_selem_has_playback_switch_joined(elem) )
    ret |= DSPD_MIXF_PSWJOINED;
  if ( snd_mixer_selem_has_capture_switch_joined(elem) )
    ret |= DSPD_MIXF_CSWJOINED;
  if ( snd_mixer_selem_has_capture_switch_exclusive(elem) )
    ret |= DSPD_MIXF_CSWEXCL;
  if ( snd_mixer_selem_is_enumerated(elem) )
    ret |= DSPD_MIXF_ENUM;
  if ( snd_mixer_selem_is_enum_playback(elem) )
    ret |= DSPD_MIXF_ENUMP;
  if ( snd_mixer_selem_is_enum_capture(elem) )
    ret |= DSPD_MIXF_ENUMC;
  return ret;
}
static int dspd_selem_to_mix_elem(snd_mixer_elem_t *elem, 
				  struct alsahw_mix_elem *e)
{
  e->flags = dspd_alsa_selem_getflags(elem);
  e->elem = elem;
  return 0;
}

static struct alsahw_mix_elem *get_alsahw_mix_elem(struct alsahw_handle *hdl, snd_mixer_elem_t *elem)
{
  size_t i;
  const char *n1, *n2;
  struct alsahw_mix_elem *e;
  for ( i = 0; i < hdl->elements_count; i++ )
    {
      e = &hdl->elements[i];
      //Pointer values are the same.  This is most likely the correct one.
      if ( e->elem == elem )
	{
	  if ( snd_mixer_selem_id_get_index(e->sid) == snd_mixer_selem_get_index(elem) )
	    {
	      n1 = snd_mixer_selem_id_get_name(e->sid);
	      n2 = snd_mixer_selem_get_name(elem);
	      if ( n1 && n2 && strcmp(n1, n2) == 0 )
		return &hdl->elements[i];
	    }
	  //Probably won't get here but the specs don't make that clear
	  break;
	}
    }
  //Find it with a memory safe reference
  for ( i = 0; i < hdl->elements_count; i++ )
    {
      if ( snd_mixer_find_selem(hdl->mixer, hdl->elements[i].sid) == elem )
	return &hdl->elements[i];
    }
  return NULL;
}

static void remove_elements(struct alsahw_handle *hdl, struct alsahw_mix_elem *e)
{
  size_t i, o = 0;
  dspd_time_t t = dspd_get_time();
  free(e->sid);
  e->sid = NULL;
  e->elem = NULL;
  hdl->mixer_tstamp = t;
  for ( i = 0; i < hdl->elements_count; i++ )
    {
      if ( hdl->elements[i].sid )
	{
	  if ( o != i )
	    hdl->elements[o] = hdl->elements[i];
	  hdl->elements[o].index = i;
	  hdl->elements[o].tstamp = t;
	  o++;
	}
    }
  hdl->elements_count = o;
}
static struct alsahw_mix_elem *get_free_element(struct alsahw_handle *hdl)
{
  void *ptr;
  if ( hdl->elements_count == hdl->max_elements )
    {
      hdl->max_elements += 32;
      ptr = dspd_reallocz(hdl->elements, 
			  hdl->max_elements * sizeof(hdl->elements[0]),
			  hdl->elements_count * sizeof(hdl->elements[0]),
			  false);
      if ( ! ptr )
	{
	  hdl->max_elements -= 32;
	  return NULL;
	}
      hdl->elements = ptr;
    }
  return &hdl->elements[hdl->elements_count];
}

static void alsahw_mixer_cb(snd_mixer_t *ctl,
			    struct alsahw_ctldata *data,
			    uint32_t mask,
			    snd_mixer_elem_t *elem,
			    void *arg)
{
  struct alsahw_handle *hdl = arg;
  struct alsahw_mix_elem *e;
  int ret;
  if ( ! elem )
    return;
 
  if ( mask == SND_CTL_EVENT_MASK_REMOVE )
    {
      e = get_alsahw_mix_elem(hdl, elem);
      if ( e )
	{
	  //This is why notificaton can't happen automatically.
	  //Need to be able to notify about which element is being removed
	  //and that won't work very well if the callback removes the last
	  //reference to it.
	  alsahw_mixer_event_notify(data,
				    hdl->stream_index,
				    e->index,
				    mask);

	  remove_elements(hdl, e);
	  hdl->mixer_update_count++;
	} 
    } else
    {
      if ( mask & SND_CTL_EVENT_MASK_ADD )
	{
	  e = get_free_element(hdl);
	  if ( e )
	    {
	      ret = snd_mixer_selem_id_malloc(&e->sid);
	      if ( ret == 0 )
		{
		  snd_mixer_selem_get_id(elem, e->sid);
		  ret = dspd_selem_to_mix_elem(elem, e);
		  if ( ret == 0 )
		    {
		      e->tstamp = dspd_get_time();
		      hdl->mixer_tstamp = e->tstamp;
		      hdl->elements_count++;
		      hdl->mixer_update_count++;
		      alsahw_mixer_event_notify(data,
						hdl->stream_index,
						e->index,
						mask);
		    } else
		    {
		      free(e->sid);
		      e->sid = NULL;
		    }
		}
	    }
	}

      if ( mask & DSPD_CTL_EVENT_MASK_CHANGED )
	{
	  e = get_alsahw_mix_elem(hdl, elem);
	  if ( e )
	    {
	      alsahw_mixer_event_notify(data,
					hdl->stream_index,
					e->index,
					mask);
	      e->update_count++;
	      hdl->mixer_update_count++;
	    }
	}
    }

}


static int init_ctl(struct alsahw_handle *hdl, int streams)
{
  int ret = snd_mixer_open(&hdl->mixer, 0);
  int count, i;
  snd_mixer_elem_t *curr;
  size_t c;
  if ( ret < 0 )
    return ret;
  ret = snd_mixer_attach(hdl->mixer, hdl->params.name);
  if ( ret == 0 )
    ret = snd_mixer_selem_register(hdl->mixer, NULL, NULL);
  if ( ret == 0 )
    ret = snd_mixer_load(hdl->mixer);
  if ( ret == -EINVAL || ret == -ENXIO )
    {
      ret = -ENOENT;
      goto out;
    }
  
  count = snd_mixer_get_count(hdl->mixer);
  if ( count == 0 )
    {
      ret = -ENOENT;
      goto out;
    }
  c = count * 2;
  if ( c < 32 )
    c = 32;

  hdl->elements = calloc(c, sizeof(*hdl->elements));
  if ( ! hdl->elements )
    {
      ret = -ENOMEM;
      goto out;
    }
  hdl->elements_count = count;
  hdl->max_elements = c;
  i = 0;
  dspd_time_t t = dspd_get_time();
  if ( t == 0 )
    t = 1; //Should not happen.
  hdl->mixer_tstamp = t;
  for ( curr = snd_mixer_first_elem(hdl->mixer);
	curr != NULL && i < count;
	curr = snd_mixer_elem_next(curr) )
    {
      ret = snd_mixer_selem_id_malloc(&hdl->elements[i].sid);
      if ( ret < 0 )
	break;
      snd_mixer_selem_get_id(curr, hdl->elements[i].sid);
      
      ret = dspd_selem_to_mix_elem(curr, &hdl->elements[i]);
      if ( ret < 0 )
	break;

      hdl->elements[i].tstamp = t;
      hdl->elements[i].index = i;
      i++;
    }

  ret = alsahw_register_mixer(global_notifier,
			      hdl->mixer,
			      (pthread_mutex_t*)&hdl->mixer_lock,
			      alsahw_mixer_cb,
			      hdl);

  


 out:
  if ( ret < 0 )
    destroy_ctl(hdl);

  return ret;
}
static int alsahw_add(void *arg, const struct dspd_dict *device)
{
  struct dspd_drv_params params;
  int ret;
  const struct dspd_pcmdrv_ops *ops = NULL, *playback_ops = NULL, *capture_ops = NULL;
  void *handle = NULL;
  char *desc = NULL, *name = NULL;
  void *hlist[2] = { NULL, NULL };
  int flags = 0;
  dspd_dict_find_value(device, DSPD_HOTPLUG_DESC, &desc);
  dspd_dict_find_value(device, DSPD_HOTPLUG_DEVNAME, &name);

  dspd_log(0, "alsahw: Got device: desc='%s' name='%s'", 
	   desc, name);

  memset(&params, 0, sizeof(params));
  ret = dspd_daemon_get_config(device, &params);
  if ( ret )
    goto out;

  if ( params.stream != DSPD_PCM_STREAM_FULLDUPLEX )
    {
      ret = alsahw_open(&params,
			&ops,
			&handle);
      if ( ret )
	goto out;

      //flags = params.stream;
      ret = init_ctl(handle, flags);
      if ( ret == 0 )
	flags |= DSPD_PCM_SBIT_CTL;
      else if ( ret != -ENOENT )
	goto out;

      if ( params.stream == DSPD_PCM_STREAM_PLAYBACK )
	ret = dspd_daemon_add_device(&handle, 
				     flags | DSPD_PCM_SBIT_PLAYBACK,
				     ops,
				     NULL);
      else
	ret = dspd_daemon_add_device(&handle, 
				     flags | DSPD_PCM_SBIT_CAPTURE,
				     NULL,
				     ops);
      
      
      if ( ret < 0 )
	goto out;
    } else
    {
      params.stream = DSPD_PCM_STREAM_PLAYBACK;
      ret = alsahw_open(&params,
			 &playback_ops,
			 &hlist[DSPD_PCM_STREAM_PLAYBACK]);
      if ( ret )
	goto out;
      params.stream = DSPD_PCM_STREAM_CAPTURE;
      ret = alsahw_open(&params,
			 &capture_ops,
			 &hlist[DSPD_PCM_STREAM_CAPTURE]);
      if ( ret )
	goto out;
      flags = DSPD_PCM_SBIT_PLAYBACK | DSPD_PCM_SBIT_CAPTURE;
      ret = init_ctl(hlist[DSPD_PCM_STREAM_PLAYBACK], flags);
      if ( ret == 0 )
	flags |= DSPD_PCM_SBIT_CTL;
      else if ( ret != -ENOENT )
	goto out;
      ret = dspd_daemon_add_device(hlist, 
				   flags | DSPD_PCM_SBIT_PLAYBACK | DSPD_PCM_SBIT_CAPTURE,
				   playback_ops,
				   capture_ops);
      if ( ret < 0 )
	goto out;
    }


  return ret;
  
 out:
  if ( handle && ops )
    ops->destructor(handle);
  if ( hlist[DSPD_PCM_STREAM_PLAYBACK] && playback_ops )
    playback_ops->destructor(hlist[DSPD_PCM_STREAM_PLAYBACK]);
  if ( hlist[DSPD_PCM_STREAM_CAPTURE] && capture_ops )
    capture_ops->destructor(hlist[DSPD_PCM_STREAM_CAPTURE]);
  free(params.addr);
  free(params.name);
  free(params.bus);
  return ret;
}

static int alsahw_remove(void *arg, const struct dspd_dict *device)
{
  //FIXME: It might be better to have the device thread cleanup and
  //use this to signal the device thread.
  return 0;
}


static struct dspd_hotplug_cb alsahw_hotplug = {
  .score = alsahw_score,
  .add = alsahw_add,
  .remove = alsahw_remove,
};

static int alsahw_init(void *daemon, void **context)
{
  int ret = dspd_daemon_hotplug_register(&alsahw_hotplug, NULL);
  if ( ret != 0 )
    {
      dspd_log(0, "Could not register hotplug handler for alsahw: error %d", ret);
    } else
    {
      dspd_log(0, "Registered hotplug handler for alsahw");
      ret = alsahw_init_notifier(&global_notifier);
      if ( ret != 0 )
	dspd_log(0, "Could not create notifier for alsahw: error %d", ret);
    }
  

  return ret;

}

static void alsahw_close(void *daemon, void **context)
{
  
}

static int alsahw_ioctl(void         *daemon, 
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

struct dspd_mod_cb dspd_mod_alsahw = {
  .desc = "ALSA PCM Hardware Driver",
  .init = alsahw_init,
  .close = alsahw_close,
  .ioctl = alsahw_ioctl,
};
