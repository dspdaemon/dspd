
#include <stdlib.h>
#include <string.h>
#include "sslib.h"
#include "pcmcli_stream.h"

int32_t dspd_pcmcli_stream_set_constant_latency(struct dspd_pcmcli_stream *stream, bool enable)
{
  int32_t ret = -EBADFD;
  if ( stream->state >= PCMCS_STATE_INIT )
    {
      stream->constant_latency = enable;
      ret = 0;
    }
  return ret;
}

int32_t dspd_pcmcli_stream_set_paused(struct dspd_pcmcli_stream *stream, bool paused)
{
  int32_t ret;
  if ( stream->error )
    {
      ret = stream->error;
    } else if ( stream->state < PCMCS_STATE_PAUSED )
    {
      ret = EBADFD;
    } else
    {
      if ( paused )
	{
	  dspd_mbx_reset(&stream->mbx);
	  stream->got_status = false;
	  stream->got_tstamp = false;
	  stream->write_size = 0;
	  stream->state = PCMCS_STATE_PAUSED;
	  //Save the "hardware" pointer so the correct status will be reported when resuming.
	  if ( stream->stream_flags == DSPD_PCM_SBIT_PLAYBACK )
	    stream->hw_pause_ptr = dspd_fifo_optr(&stream->fifo);
	  else
	    stream->hw_pause_ptr = dspd_fifo_iptr(&stream->fifo);

	} else if ( stream->state == PCMCS_STATE_PAUSED )
	{
	  stream->state = PCMCS_STATE_PREPARED;
	}
      ret = 0;
    }
  return ret;
}

int32_t dspd_pcmcli_stream_set_running(struct dspd_pcmcli_stream *stream, bool running)
{
  int32_t ret;
  if ( stream->error )
    {
      ret = stream->error;
    } else if ( stream->state != PCMCS_STATE_PREPARED && stream->state != PCMCS_STATE_RUNNING && stream->state != PCMCS_STATE_PAUSED )
    {
      ret = -EBADFD;
    } else
    {
      if ( running )
	{
	  stream->state = PCMCS_STATE_RUNNING;
	} else
	{
	  stream->state = PCMCS_STATE_BOUND;
	  stream->hw_pause_ptr = 0;
	  stream->write_size = 0;
	}
      ret = 0;
    }
  return ret;
}

int32_t dspd_pcmcli_stream_attach(struct dspd_pcmcli_stream *stream,
				  const struct dspd_cli_params *params,
				  const struct dspd_shm_map *map)
{
  struct dspd_shm_addr addr;
  int32_t ret;
  const struct pcm_conv *conv;
  size_t channels;
  if ( ! (params->stream & stream->stream_flags) )
    {
      ret = -EINVAL;
    } else if ( stream->state >= PCMCS_STATE_BOUND )
    {
      ret = -EBUSY;
    } else
    {
      conv = dspd_getconv(params->format);
      if ( conv )
	{
	  if ( ((params->stream & DSPD_PCM_SBIT_PLAYBACK) && conv->tofloat32 == NULL) ||
	       ((params->stream & DSPD_PCM_SBIT_CAPTURE) && conv->fromfloat32 == NULL) )
	    return -EINVAL;
	}
      if ( params->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
	{
	  if ( stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK )
	    channels = DSPD_CLI_PCHAN(params->channels);
	  else
	    channels = DSPD_CLI_CCHAN(params->channels);
	} else
	{
	  channels = params->channels;
	}
      if ( channels == 0 )
	return -EINVAL;
      memset(&addr, 0, sizeof(addr));
      addr.section_id = DSPD_CLIENT_SECTION_MBX;
      ret = dspd_shm_get_addr(map, &addr);
      if ( ret == 0 )
	{
	  if ( addr.length >= dspd_mbx_bufsize(sizeof(struct dspd_pcm_status)) )
	    {
	      ret = dspd_mbx_init(&stream->mbx, sizeof(struct dspd_pcm_status), addr.addr);
	      if ( ret == 0 )
		{
		  memset(&addr, 0, sizeof(addr));
		  addr.section_id = DSPD_CLIENT_SECTION_FIFO;
		  ret = dspd_shm_get_addr(map, &addr);
		  if ( ret == 0 )
		    {
		      ret = dspd_fifo_init(&stream->fifo,
					   params->bufsize,
					   channels * sizeof(float),
					   addr.addr);
		      if ( ret == 0 )
			{
			  stream->params = *params;
			  stream->state = PCMCS_STATE_BOUND;
			  if ( stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK )
			    stream->playback_conv = conv->tofloat32;
			  else
			    stream->capture_conv = conv->fromfloat32;
			  stream->params = *params;
			  stream->params.channels = channels;
			  stream->params.xflags &= ~DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
			  stream->framesize = dspd_get_pcm_format_size(stream->params.format) * channels;
			  stream->sample_time = 1000000000 / stream->params.rate;
			  stream->xrun_threshold = stream->params.bufsize;
			  memset(&stream->intrp, 0, sizeof(stream->intrp));
			  stream->intrp.sample_time = stream->sample_time;
			  stream->intrp.maxdiff = MAX(1, stream->sample_time / 10);
			}
		    }
		}
	    } else
	    {
	      ret = -EINVAL;
	    }
	}
    }
  return ret;
}

void dspd_pcmcli_stream_detach(struct dspd_pcmcli_stream *stream)
{
  if ( stream->state >= PCMCS_STATE_BOUND )
    {
      dspd_mbx_destroy(&stream->mbx);
      dspd_fifo_destroy(&stream->fifo);
      int32_t f = stream->stream_flags;
      memset(stream, 0, sizeof(*stream));
      stream->stream_flags = f;
      stream->state = PCMCS_STATE_INIT;
    }
}

int32_t dspd_pcmcli_stream_init(struct dspd_pcmcli_stream *stream, int32_t sbit)
{
  int32_t ret = -EINVAL;
  if ( sbit == DSPD_PCM_SBIT_PLAYBACK || sbit == DSPD_PCM_SBIT_CAPTURE )
    {
      memset(stream, 0, sizeof(*stream));
      stream->state = PCMCS_STATE_INIT;
      stream->stream_flags = sbit;
      ret = 0;
    }
  return ret;
}

void dspd_pcmcli_stream_destroy(struct dspd_pcmcli_stream *stream)
{
  dspd_pcmcli_stream_detach(stream);
  stream->state = PCMCS_STATE_ALLOC;
}

int32_t dspd_pcmcli_stream_new(struct dspd_pcmcli_stream **stream, int32_t sbit)
{
  struct dspd_pcmcli_stream *c = malloc(sizeof(struct dspd_pcmcli_stream));
  int32_t ret = -ENOMEM;
  if ( c )
    {
      ret = dspd_pcmcli_stream_init(c, sbit);
      if ( ret == 0 )
	*stream = c;
      else
	free(c);
    }
  return ret;
}
void dspd_pcmcli_stream_delete(struct dspd_pcmcli_stream *stream)
{
  dspd_pcmcli_stream_destroy(stream);
  free(stream);
}
size_t dspd_pcmcli_stream_sizeof(void)
{
  return sizeof(struct dspd_pcmcli_stream);
}



int32_t dspd_pcmcli_stream_reset(struct dspd_pcmcli_stream *stream)
{
  int32_t ret;
  if ( stream->state >= PCMCS_STATE_BOUND )
    {
      stream->got_status = false;
      stream->got_tstamp = false;
      stream->appl_ptr = 0;
      stream->hw_ptr = 0;
      stream->last_hw_ptr = 0;
      stream->state = PCMCS_STATE_PREPARED;
      dspd_mbx_reset(&stream->mbx);
      dspd_fifo_reset(&stream->fifo);
      dspd_intrp_reset(&stream->intrp);
      stream->error = 0;
      stream->hw_pause_ptr = 0;
      stream->next_wakeup = 0;
      ret = 0;
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}

int32_t dspd_pcmcli_stream_set_trigger_tstamp(struct dspd_pcmcli_stream *stream, dspd_time_t tstamp)
{
  int32_t ret;
  if ( stream->state == PCMCS_STATE_PREPARED )
    {
      stream->trigger_tstamp = tstamp;
      dspd_intrp_set(&stream->intrp, stream->trigger_tstamp, 0);
      stream->got_tstamp = true;
      ret = 0;
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}




int32_t dspd_pcmcli_stream_status(struct dspd_pcmcli_stream *stream,
				  struct dspd_pcmcli_status *status,
				  bool hwsync)
		    
{
  struct dspd_pcm_status *s;
  int32_t ret = 0;
  uint64_t d;
  uint64_t hw, appl;
  if ( stream->state >= PCMCS_STATE_PREPARED )
    {
      if ( hwsync == true || stream->got_status == false )
	{
	  s = dspd_mbx_acquire_read(&stream->mbx, 1);
	  if ( s )
	    {
	      memcpy(&stream->status, s, sizeof(*s));
	      dspd_mbx_release_read(&stream->mbx, s);
	      ret = s->error;
	      stream->got_status = true;
	    }
	}
      if ( stream->got_status )
	{
	  s = &stream->status;
	  if ( status )
	    {
	      status->appl_ptr = s->appl_ptr;
	      status->hw_ptr = s->hw_ptr;
	      status->tstamp = s->tstamp - (s->cycle_length * stream->sample_time);
	      status->reserved = s->cycle_length;
	      if ( status->reserved > stream->params.latency )
		status->reserved = stream->params.latency;
	      //May have EAGAIN if no error and the status sync temporarily failed.
	      //The status is still valid.
	      if ( s->error == 0 )
		status->error = ret;
	      else
		status->error = s->error;
	      status->delay = s->delay;
	      status->delay_tstamp = status->tstamp;
	    }
	} else if ( stream->got_tstamp )
	{
	  if ( status )
	    {
	      status->delay = 0;
	      status->appl_ptr = stream->appl_ptr;
	      status->hw_ptr = stream->hw_pause_ptr;
	      status->tstamp = stream->trigger_tstamp - (stream->params.fragsize * stream->sample_time);
	      status->error = EINPROGRESS; //Don't have the status yet.
	      status->delay = 0;
	      status->delay_tstamp = status->tstamp;
	      status->reserved = 0;
	    }
	} else
	{
	  //No status data available.
	  ret = -EAGAIN;
	}
      if ( ret == 0 )
	{
	  ret = dspd_pcmcli_stream_avail(stream, &hw, &appl);
	  if ( ret < 0 )
	    {
	      status->error = ret;
	    } else
	    {
	      d = hw - status->hw_ptr;
	      status->tstamp += (d * stream->sample_time);
	      status->hw_ptr = hw;
	      status->appl_ptr = appl;
	      if ( stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK )
		status->delay += status->appl_ptr - status->hw_ptr;
	      else
		status->delay += ret;
	      status->avail = ret;
	      ret = 0;
	      d = status->hw_ptr - stream->hw_iptr;
	      stream->hw_iptr = status->hw_ptr;
	      dspd_intrp_update(&stream->intrp, status->tstamp, d);
	      status->tstamp -= (d - dspd_intrp_frames(&stream->intrp, d)) * stream->sample_time;
	    }
	}
      if ( status != NULL && ret == 0 && stream->no_xrun == false && status->error == 0 )
	{
	  status->error = dspd_fifo_get_error(&stream->fifo);
	  if ( status->error > 0 )
	    status->error *= -1;
	  if ( status->error == 0 )
	    status->error = dspd_pcmcli_stream_check_xrun(stream);
	  if ( status->error < 0 && stream->error == 0 )
	    stream->error = status->error;
	}
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}





static inline void check_error(struct dspd_pcmcli_stream *stream, int32_t err)
{
  if ( err < 0 && err != -EAGAIN && err != -EWOULDBLOCK && err != -EBADFD && err != -EBUSY )
    {
      stream->error = err;
      stream->state = PCMCS_STATE_ERROR;
    }
}


ssize_t dspd_pcmcli_stream_write(struct dspd_pcmcli_stream *stream,
				 const void           *data,
				 size_t                len)
{
  float *ptr;
  ssize_t ret = 0;
  int32_t err;
  size_t offset = 0; uint32_t l, off;
  if ( stream->error )
    {
      ret = stream->error;
    } else if ( stream->state < PCMCS_STATE_PREPARED )
    {
      ret = -EBADF;
    } else if ( ! (stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK) )
    {
      ret = -EINVAL;
    } else
    {
      if ( len > INTPTR_MAX ) 
	len = INTPTR_MAX;
      while ( offset < len )
	{
	  l = len - offset;
	  err = dspd_fifo_wiov_ex(&stream->fifo,
				  (void**)&ptr,
				  &off,
				  &l);
	  if ( err )
	    {
	      ret = -err;
	      break;
	    }
	  if ( l == 0 )
	    {
	      ret = -EAGAIN;
	      break;
	    }
	  DSPD_ASSERT(l <= (len - offset));
	  if ( data )
	    {
	      stream->playback_conv(data+(offset*stream->framesize), 
				    &ptr[off*stream->params.channels],
				    stream->params.channels*l);
	    } else
	    {
	      memset(&ptr[off*stream->params.channels],
		     0,
		     sizeof(*ptr) * stream->params.channels * l);
	    }
	  dspd_fifo_wcommit(&stream->fifo, l);
	  offset += l;
	}
      if ( offset > 0 )
	{
	  stream->appl_ptr += offset;
	  ret = offset;
	  if ( stream->state == PCMCS_STATE_RUNNING && stream->write_size < stream->params.bufsize )
	    stream->write_size += offset;
	}
    }
  check_error(stream, ret);
  return ret;
}

ssize_t dspd_pcmcli_stream_read(struct dspd_pcmcli_stream *stream,
				void                 *data,
				size_t                len)
{
  const float *ptr;
  ssize_t ret = 0;
  int32_t err;
  size_t offset = 0; uint32_t l, off;
  if ( stream->error )
    {
      ret = stream->error;
    } else if ( stream->state < PCMCS_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( ! (stream->stream_flags & DSPD_PCM_SBIT_CAPTURE) )
    {
      ret = -EINVAL;
    } else
    {
      if ( len > INTPTR_MAX ) 
	len = INTPTR_MAX;
      while ( offset < len )
	{
	  l = len - offset;
	  err = dspd_fifo_riov_ex(&stream->fifo,
				  (void**)&ptr,
				  &off,
				  &l);
	  if ( err )
	    {
	      ret = -err;
	      break;
	    }
	  if ( l == 0 )
	    {
	      ret = -EAGAIN;
	      break;
	    }
	  if ( data )
	    stream->capture_conv(&ptr[off*stream->params.channels],
				 data+(offset*stream->framesize),
				 stream->params.channels * l);
	  dspd_fifo_rcommit(&stream->fifo, l);
	  offset += l;
	}
      if ( offset > 0 )
	{
	  stream->appl_ptr += offset;
	  ret = offset;
	}
    }
  check_error(stream, ret);
  return ret;
}


int32_t dspd_pcmcli_stream_set_pointer(struct dspd_pcmcli_stream *stream, bool relative, uint64_t ptr)
{
  int32_t ret;
  uint64_t diff;
  uint32_t p;
  if ( stream->state < PCMCS_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( stream->error != 0 && stream->error != -EPIPE )
    {
      ret = stream->error;
    } else 
    {
      ret = 0;
      if ( relative )
	{
	  stream->appl_ptr += ptr;
	  p = ptr % UINT32_MAX;
	} else
	{
	  diff = ptr - stream->appl_ptr;
	  stream->appl_ptr = ptr;
	  p = diff % UINT32_MAX;
	}

      if ( stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK )
	dspd_fifo_wcommit(&stream->fifo, p);
      else
	dspd_fifo_rcommit(&stream->fifo, p);
    }
  return ret;
}

int32_t dspd_pcmcli_stream_rewind(struct dspd_pcmcli_stream *stream, uint64_t *frames)
{
  int32_t ret = 0;
  uint32_t len, f;
  if ( stream->state < PCMCS_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( stream->error )
    {
      ret = stream->error;
    } else if ( stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( ! stream->no_xrun )
	{
	  ret = dspd_fifo_len(&stream->fifo, &len);
	  if ( ret == 0 && len < *frames )
	    *frames = len;
	} else
	{
	  ret = 0;
	}
      if ( ret == 0 )
	{
	  f = *frames % UINT32_MAX;
	  dspd_fifo_wcommit(&stream->fifo, f * -1);
	  stream->appl_ptr -= *frames;
	} else if ( ! stream->no_xrun )
	{
	  ret = -EPIPE;
	}
    } else if ( stream->stream_flags & DSPD_PCM_SBIT_CAPTURE )
    {
      
      if ( ! stream->no_xrun )
	{
	  ret = dspd_fifo_space(&stream->fifo, &len);
	  if ( ret == 0 && len < *frames )
	    *frames = len;
	} else
	{
	  ret = 0;
	}
      if ( ret == 0 )
	{
	  f = *frames % UINT32_MAX;
	  dspd_fifo_rcommit(&stream->fifo, f * -1);
	  stream->appl_ptr -= *frames;
	} else if ( ! stream->no_xrun )
	{
	  ret = -EPIPE;
	}
    } else
    {
      ret = -EBADFD;
    }
  check_error(stream, ret);
  return ret;
}
int32_t dspd_pcmcli_stream_forward(struct dspd_pcmcli_stream *stream, uint64_t *frames)
{
  int32_t ret;
  uint32_t len;
  if ( stream->state < PCMCS_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( stream->error )
    {
      ret = stream->error;
    } else
    {
      if ( stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK )
	{
	  if ( ! stream->no_xrun )
	    {
	      ret = dspd_fifo_space(&stream->fifo, &len);
	      if ( ret == 0 && len < *frames )
		*frames = len;
	    } else
	    {
	      ret = 0;
	    }
	  if ( ret == 0 )
	    {
	      dspd_fifo_wcommit(&stream->fifo, *frames % UINT32_MAX);
	      stream->appl_ptr += *frames;
	    } else if ( ! stream->no_xrun )
	    {
	      ret = -EPIPE;
	    }
	} else if ( stream->stream_flags & DSPD_PCM_SBIT_CAPTURE )
	{
	  if ( ! stream->no_xrun )
	    {
	      ret = dspd_fifo_len(&stream->fifo, &len);
	      if ( ret == 0 && len < *frames )
		*frames = len;
	    } else
	    {
	      ret = 0;
	    }
	  if ( ret == 0 )
	    {
	      dspd_fifo_rcommit(&stream->fifo, *frames % UINT32_MAX);
	      stream->appl_ptr += *frames;
	    } else if ( ! stream->no_xrun )
	    {
	      ret = -EPIPE;
	    }
	} else
	{
	  ret = -EBADFD;
	}
      check_error(stream, ret);
    }
  return ret;
}

int32_t dspd_pcmcli_stream_avail(struct dspd_pcmcli_stream *stream, uint64_t *hwptr, uint64_t *appl_ptr)
{
  int32_t ret = 0;
  uint32_t i, o, l;
  if ( stream->state < PCMCS_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( stream->error )
    {
      ret = stream->error;
    } else
    {
      ret = dspd_fifo_len_ptrs(&stream->fifo, &l, &i, &o);
      if ( ret == 0 )
	{
	  if ( stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      stream->hw_ptr += o - stream->last_hw_ptr;
	      stream->last_hw_ptr = o;
	      if ( hwptr )
		*hwptr = MAX(stream->hw_ptr, (stream->appl_ptr - l));
	      if ( appl_ptr )
		*appl_ptr = i;
	      ret = stream->params.bufsize - l;
	    } else if ( stream->stream_flags & DSPD_PCM_SBIT_CAPTURE )
	    {
	      stream->hw_ptr += i - stream->last_hw_ptr;
	      stream->last_hw_ptr = i;
	      if ( hwptr )
		*hwptr = MAX(stream->hw_ptr, (stream->appl_ptr + l));
	      if ( appl_ptr )
		*appl_ptr = stream->appl_ptr;
	      ret = l;
	    }
	} else if ( ! stream->no_xrun )
	{
	  ret = -EPIPE;
	}
      check_error(stream, ret);
    }
  return ret;
}

int32_t dspd_pcmcli_stream_check_xrun(struct dspd_pcmcli_stream *stream)
{
  int32_t ret;
  if ( stream->state < PCMCS_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( stream->error )
    {
      ret = stream->error;
    } else
    {
      ret = dspd_pcmcli_stream_avail(stream, NULL, NULL);
      if ( (uint32_t)ret >= stream->xrun_threshold && stream->no_xrun == false )
	{
	  if ( stream->stream_flags & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      if ( stream->state == PCMCS_STATE_RUNNING ||
		   stream->state == PCMCS_STATE_ERROR )
		ret = -EPIPE;
	      else
		ret = 0;
	    } else
	    {
	      ret = -EPIPE;
	    }
	} else
	{
	  ret = 0;
	}
    }
  if ( ret == -EPIPE && stream->write_size < stream->params.bufsize && stream->no_xrun == false )
    {
      //Catch the somewhat likely initial xrun caused by apps starting too early.
      //For example, SDL sets ALSA start threshold to 1 and fragments to 2 then writes
      //one fragment.  This method could possibly be used full time, but it seems good
      //enough to use xrun correction on the server side for the rare race condition where
      //a client writes with the fill being nonzero then the server consumes the data and creates
      //a gap in the buffer.
      dspd_time_t t = 0;
      if ( stream->got_status )
	t = stream->status.tstamp;
      else if ( stream->got_tstamp )
	t = stream->trigger_tstamp;
      if ( t )
	{
	  dspd_time_t now = dspd_get_time();
	  dspd_time_t diff;
	  if ( now >= t )
	    {
	      diff = (now - t) / stream->sample_time;
	      if ( diff < (stream->params.fragsize / 2) )
		ret = 0;
	    }
	}
      ret = 0;
    }
  return ret;
}

int32_t dspd_pcmcli_stream_state(struct dspd_pcmcli_stream *stream)
{
  return stream->state;
}



int32_t dspd_pcmcli_stream_get_next_wakeup(struct dspd_pcmcli_stream *stream, const struct dspd_pcmcli_status *status, size_t avail, dspd_time_t *next)
{
  int32_t ret;
  struct dspd_pcmcli_status s;
  uint32_t avail_min;
  if ( ! status )
    {
      ret = dspd_pcmcli_stream_status(stream, &s, true);
      status = &s;
    } else
    {
      ret = 0;
    }
  if ( ret == 0 )
    {
      if ( status->error != 0 && status->error != EINPROGRESS )
	{
	  ret = status->error;
	  if ( ret > 0 )
	    ret *= -1;
	} else if ( status->avail >= avail && stream->constant_latency == false )
	{
	  *next = 1; //This is valid (nonzero) and in the past
	  ret = PCMCS_WAKEUP_NOW;
	} else
	{
	  if ( stream->constant_latency )
	    {
	      if ( stream->got_status )
		{
		  if ( stream->status.delay >= stream->params.latency )
		    avail_min = stream->params.latency;
		  else
		    avail_min = stream->status.delay;
		  avail_min += avail;
		} else
		{
		  avail_min = stream->params.latency + avail;
		}
	      if ( avail_min > status->avail )
		{
		  stream->next_wakeup = status->tstamp + ((avail_min - status->avail) * stream->sample_time);
		  *next = stream->next_wakeup;
		} else
		{
		  *next = 1;
		  ret = PCMCS_WAKEUP_NOW;
		}
	    } else
	    {
	      stream->next_wakeup = status->tstamp + ((avail - status->avail) * stream->sample_time);
	      *next = stream->next_wakeup;
	    }
	}
    } else if ( ret == -EAGAIN )
    {
      ret = dspd_pcmcli_stream_avail(stream, NULL, NULL);
      if ( ret >= 0 )
	{
	  if ( (uint32_t)ret < avail )
	    {
	      //Nothing to do.
	      *next = UINT64_MAX;
	      ret = PCMCS_WAKEUP_NONE;
	    } else
	    {
	      *next = 1;
	      ret = PCMCS_WAKEUP_NOW;
	    }
	}
      
    }
  return ret;
}
