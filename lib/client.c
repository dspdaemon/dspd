/*
 *  CLIENT - PCM internal client API
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


#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <mqueue.h>
#define _DSPD_CTL_MACROS
#include "sslib.h"
#include "daemon.h"
#include "dctrl.h"
#include "syncgroup.h"


static int32_t client_stop_now(struct dspd_client *cli, uint32_t streams);



static void playback_set_volume(void *handle, double volume);
static double playback_get_volume(void *handle);
static int32_t playback_get_params(void *handle, struct dspd_cli_params *params);
static int32_t playback_set_params(void *handle, const struct dspd_cli_params *params);





static void capture_set_volume(void *handle, double volume);
static double capture_get_volume(void *handle);
static int32_t capture_get_params(void *handle, struct dspd_cli_params *params);
static int32_t capture_set_params(void *handle, const struct dspd_cli_params *params);

static void dspd_client_srvunlock(void *client);
static void dspd_client_srvlock(void *client);
static void dspd_client_lock(void *client, bool write);
static void dspd_client_unlock(void *client);
static int32_t client_start_at_time(struct dspd_client *cli, dspd_time_t tstamp, uint32_t streams, dspd_time_t tslist[2], bool set);


static int32_t get_playback_status(void     *dev,
				   void     *client,      
				   uint64_t *pointer,
				   uint64_t *start_count,
				   uint32_t *latency,
				   uintptr_t frames,
				   const struct dspd_pcm_status *status);
static void playback_xfer(void                            *dev,
			  void                            *client,
			  double                          *buf,
			  uintptr_t                        frames,
			  uint64_t                         start_count,
			  const struct dspd_pcm_status    *status);

static int32_t get_capture_status(void     *dev,
				  void     *client,      
				  uint64_t *pointer,
				  uint64_t *start_count,
				  uint32_t *latency,
				  const struct dspd_pcm_status *status);
static void capture_xfer(void                            *dev,
			 void                            *client,
			 const float                     *buf,
			 uintptr_t                        frames,
			 const struct dspd_pcm_status    *status);


static int32_t client_ctl(struct dspd_rctx *rctx,
			  uint32_t             req,
			  const void          *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize);

static dspd_time_t get_start_time(struct dspd_client *cli)
{
  dspd_time_t now = dspd_get_time();
  if ( cli->min_latency > 0 )
    now += cli->min_latency / 2;
  return now;
}

/*
  This callback happens with srv_lock held.  A SIGBUS here will cause
  the caller to unlock and move on.
*/
static void client_error(void *dev, int32_t index, void *client, int32_t err)
{
  struct dspd_client *cli = client;
  //Call custom handler
  if ( cli->error_cb )
    cli->error_cb(dev, index, client, err, cli->error_arg);
  cli->err = err;
  if ( err != EFAULT )
    {
      //Do the usual stuff.  Might have a SIGBUS here if the client
      //is really bad.
      if ( cli->playback.enabled )
	dspd_fifo_set_error(&cli->playback.fifo, -err);
      if ( cli->capture.enabled )
	dspd_fifo_set_error(&cli->capture.fifo, -err);
    }

}


static const struct dspd_pcmcli_ops client_ops = {
  .get_playback_status = get_playback_status,
  .playback_xfer = playback_xfer,

  .get_capture_status = get_capture_status,
  .capture_xfer = capture_xfer,
 
  .error = client_error,
};

int32_t dspd_client_get_index(void *client)
{
  struct dspd_client *cli = client;
  return cli->index;
}

static int32_t dspd_client_release(struct dspd_client *cli);



static void client_destructor(void *obj)
{
  struct dspd_client *cli = obj;
  if ( cli->syncgroup )
    {
      dspd_sg_remove(cli->syncgroup, cli->index);
      dspd_sg_put(dspd_dctx.syncgroups, dspd_sg_id(cli->syncgroup));
    }
  dspd_client_release(cli);
  dspd_mbx_destroy(&cli->playback.mbx);
  dspd_mbx_destroy(&cli->capture.mbx);
  dspd_fifo_destroy(&cli->playback.fifo);
  dspd_fifo_destroy(&cli->capture.fifo);
  dspd_shm_close(&cli->playback.shm);
  dspd_shm_close(&cli->capture.shm);
  free(cli->playback_src.buf);
  cli->playback_src.buf = NULL;
  free(cli->capture_src.buf);
  cli->capture_src.buf = NULL;
  if ( cli->capture_src.src )
    dspd_src_delete(cli->capture_src.src);
  if ( cli->playback_src.src )
    dspd_src_delete(cli->playback_src.src);
  dspd_mbx_delete(cli->sync_start_tstamp);
  dspd_mutex_destroy(&cli->sync_start_lock);
  free(cli);
}

int32_t dspd_client_new(struct dspd_slist *list,
			void **client)
{
  struct dspd_client *cli;
  intptr_t index = -1;
  int32_t ret;
  cli = calloc(1, sizeof(*cli));
  if ( ! cli )
    return -ENOMEM;
  ret = dspd_mutex_init(&cli->sync_start_lock, NULL);
  if ( ret != 0 )
    {
      ret *= -1;
      goto error;
    }
  ret = dspd_mbx_new(&cli->sync_start_tstamp, sizeof(struct dspd_client_trigger_tstamp), NULL);
  if ( ret )
    {
      ret *= -1;
      goto error;
    }
  
  if ( list )
    {
      dspd_slist_wrlock(list);
      index = dspd_slist_get_free(list, -1);
      if ( index < 0 )
	{
	  dspd_slist_unlock(list);
	  dspd_mutex_destroy(&cli->sync_start_lock);
	  ret = -ENOSPC;
	  goto error;
	}
    }
  
  cli->index = index;
  cli->list = list;
  cli->device = -1;
  cli->mq_fd = -1;
  cli->min_latency = dspd_get_tick();
  dspd_store_float32(&cli->playback.volume, 1.0);
  dspd_store_float32(&cli->capture.volume, 1.0);

  if ( list )
    {
      dspd_slist_entry_set_used(list, (uintptr_t)index, true);
      dspd_slist_entry_set_pointers(list,
				    (uintptr_t)index,
				    cli,
				    NULL,
				    (void*)&client_ops);
      dspd_slist_set_destructor(list, (uintptr_t)index, client_destructor);
      dspd_slist_set_ctl(list, (uintptr_t)index, client_ctl);
      dspd_slist_ref(list, (uintptr_t)index);

      dspd_slist_entry_srvunlock(list, (uintptr_t)index);
      dspd_slist_entry_rw_unlock(list, (uintptr_t)index);
      dspd_slist_unlock(list);
    }

  *client = cli;
  return 0;

  
 error:
  dspd_mbx_delete(cli->sync_start_tstamp);
  dspd_mutex_destroy(&cli->sync_start_lock);
  free(cli);
  return ret;
}

void dspd_client_delete(void *client)
{
  struct dspd_client *cli = client;
  if ( cli->list != NULL && cli->index >= 0 )
    {
      dspd_slist_entry_set_used(cli->list, (uintptr_t)cli->index, false);
      dspd_slist_entry_srvunlock(cli->list, (uintptr_t)cli->index);
      dspd_slist_entry_rw_unlock(cli->list, (uintptr_t)cli->index);
    }
  dspd_mbx_destroy(&cli->playback.mbx);
  dspd_mbx_destroy(&cli->capture.mbx);
  dspd_fifo_destroy(&cli->playback.fifo);
  dspd_fifo_destroy(&cli->capture.fifo);
  dspd_shm_close(&cli->playback.shm);
  dspd_shm_close(&cli->capture.shm);
  free(cli);
}

static int32_t dspd_client_release(struct dspd_client *cli)
{
  int32_t ret;
  size_t br;
  if ( cli->device >= 0 )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    cli->device,
			    DSPD_SCTL_SERVER_DISCONNECT,
			    &cli->index,
			    sizeof(cli->index),
			    NULL,
			    0,
			    &br);
      dspd_slist_entry_wrlock(cli->list, cli->device);
      dspd_slist_unref(cli->list, cli->device);
      dspd_slist_entry_rw_unlock(cli->list, cli->device);
      cli->device = -1;
      dspd_slist_entry_srvlock(cli->list, cli->index);
      dspd_slist_entry_set_key(cli->list, cli->index, 0);
      dspd_slist_entry_srvunlock(cli->list, cli->index);
    } else
    {
      ret = -ENOTCONN;
    }
  return ret;
}




static void dspd_client_srvlock(void *client)
{
  struct dspd_client *cli = client;
  //dspd_mutex_lock(&cli->sync_start_lock);
  dspd_slist_entry_srvlock(cli->list, cli->index);
}

static void dspd_client_srvunlock(void *client)
{
  struct dspd_client *cli = client;
  dspd_slist_entry_srvunlock(cli->list, cli->index);
  //dspd_mutex_unlock(&cli->sync_start_lock);
}

static void dspd_client_lock(void *client, bool write)
{
  struct dspd_client *cli = client;
  if ( write )
    dspd_slist_entry_wrlock(cli->list, cli->index);
  else
    dspd_slist_entry_rdlock(cli->list, cli->index);
}

static void dspd_client_unlock(void *client)
{
  struct dspd_client *cli = client;
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
}







static int32_t realloc_src(struct dspd_client_src *src, size_t samples)
{
  size_t len = samples * sizeof(float);
  void *ptr;
  int32_t ret = 0;
  if ( samples != src->nsamples )
    {
      if ( samples == 0 )
	{
	  free(src->buf);
	  src->buf = NULL;
	  src->nsamples = 0;
	} else
	{
	  ptr = realloc(src->buf, len);
	  if ( ptr == NULL )
	    {
	      if ( samples > src->nsamples )
		ret = -errno;
	      //It is not an error to fail to shrink the buffer.
	    } else
	    {
	      src->buf = ptr;
	      src->nsamples = samples;
	    }
	}
    }
  return ret;
}

static int32_t dspd_stream_setsrc(struct dspd_client *cli, bool always_realloc)
{
  size_t newsize, newcount;
  int32_t ret = 0;
  if ( cli->playback.enabled &&
       cli->playback_src.rate )
    {
      newcount = dspd_src_get_frame_count(cli->playback.params.rate,
					  cli->playback_src.rate,
					  cli->playback.params.fragsize * 2);
      newsize = newcount * cli->playback.params.channels;
      if ( always_realloc )
	{
	  if ( newsize != cli->playback_src.nsamples )
	    {
	      ret = realloc_src(&cli->playback_src, newsize);
	      if ( ret < 0 )
		return ret;
	    }
	} else
	{
	  if ( newsize > cli->playback_src.nsamples )
	    {
	      ret = realloc_src(&cli->playback_src, newsize);
	      if ( ret < 0 )
		return ret;
	    }
	}

      if ( ret == 0 && cli->playback.params.channels != cli->playback_src.channels )
	{
	  cli->playback_src.channels = cli->playback.params.channels;
	  if ( cli->playback_src.channels == 0 )
	    {
	      dspd_src_delete(cli->playback_src.src);
	      cli->playback_src.src = NULL;
	    } else
	    {
	      ret = dspd_src_new(&cli->playback_src.src, 0, cli->playback_src.channels);
	      if ( ret )
		return ret;
	    }
	}
      if ( cli->playback_src.src )
	{
	  dspd_src_set_rates(cli->playback_src.src,
			     cli->playback.params.rate,
			     cli->playback_src.rate);
	}
    }
  
  if ( cli->capture.enabled &&
       cli->capture_src.rate )
    {
      //Need to demangle input before SRC, otherwise it gets more complicated.
      newcount = dspd_src_get_frame_count(cli->capture.params.rate,
					  cli->capture_src.rate,
					  cli->capture.params.fragsize * 2);
      newsize = newcount * MAX(cli->capture.params.channels, cli->capture_schan);
      if ( always_realloc )
	{
	  if ( newsize != cli->capture_src.nsamples )
	    {
	      ret = realloc_src(&cli->capture_src, newsize);
	      if ( ret < 0 )
		return ret;
	    }
	} else
	{
	  if ( newsize > cli->capture_src.nsamples )
	    {
	      ret = realloc_src(&cli->capture_src, newsize);
	      if ( ret < 0 )
		return ret;
	    }
	}
      if ( ret == 0 && cli->capture.params.channels != cli->capture_src.channels )
	{
	  cli->capture_src.channels = cli->capture.params.channels;
	  if ( cli->capture_src.channels == 0 )
	    {
	      dspd_src_delete(cli->capture_src.src);
	      cli->capture_src.src = NULL;
	    } else
	    {
	      ret = dspd_src_new(&cli->capture_src.src, 0, cli->capture_src.channels);
	      if ( ret )
		return ret;
	    }
	}
      if ( cli->capture_src.src )
	{
	  dspd_src_set_rates(cli->capture_src.src,
			     cli->capture_src.rate,
			     cli->capture.params.rate);
	}
    }
  return 0;
}

static int32_t dspd_stream_setchannelmap(struct dspd_client *cli,
					 const struct dspd_chmap *chmap,
					 int32_t sbit)
{
  struct dspd_client_stream *stream;
  struct dspd_chmap *imap, *cmap;
  const struct dspd_chmap *p;
  struct dspd_fchmap stmp, ctmp;
  size_t *schan;
  size_t br;
  int32_t ret;
  uint64_t s;
  const struct dspd_cli_params *params;

  sbit &= (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);
  if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
    {
      stream = &cli->playback;
      imap = &cli->playback_inmap.map;
      cmap = &cli->playback_cmap.map;
      schan = &cli->playback_schan;
      params = &cli->playback.params;
    } else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
    {
      stream = &cli->capture;
      imap = &cli->capture_inmap.map;
      cmap = &cli->capture_cmap.map;
      schan = &cli->capture_schan;
      params = &cli->capture.params;
    } else
    {
      return -EINVAL;
    }
  *schan = 0;
  if ( cli->device >= 0 )
    {

      ret = dspd_stream_ctl(&dspd_dctx,
			    cli->device,
			    DSPD_SCTL_SERVER_GETCHANNELMAP,
			    &sbit,
			    sizeof(sbit),
			    &stmp,
			    sizeof(stmp),
			    &br);
      if ( ret )
	return ret;
      
      if ( chmap )
	p = chmap;
      else
	p = imap;

       //This line means that a client could reset to the default channel map explicitly
       //by sending a channel map with 0 channels.
      if ( p->channels > 0 )
	{

	  ret = dspd_stream_ctl(&dspd_dctx,
				cli->device,
				DSPD_SCTL_SERVER_CONVERT_CHMAP,
				p,
				dspd_chmap_sizeof(p),
				&ctmp,
				sizeof(ctmp),
				&br);
	} else
	{
	  s = params->channels;
	  s <<= 32U;
	  s |= sbit;
	  ret = dspd_stream_ctl(&dspd_dctx,
				cli->device,
				DSPD_SCTL_SERVER_GETCHANNELMAP,
				&s,
				sizeof(s),
				&ctmp,
				sizeof(ctmp),
				&br);
	}
      
      if ( ret == 0 )
	{
	  ret = 0;
	  *schan = stmp.map.channels;
	  assert(*schan);
	  memmove(imap, p, dspd_chmap_sizeof(p));
	  imap->stream |= sbit;
	  memcpy(cmap, &ctmp, dspd_fchmap_sizeof(&ctmp));
	} else
	{
	  if ( chmap == NULL )
	    cmap->channels = 0;
	  ret = -EINVAL;
	}

    } else
    {
      if ( chmap )
	{
	  if ( chmap->channels > stream->params.channels )
	    {
	      ret = -EINVAL;
	    } else
	    {
	      memcpy(imap, chmap, dspd_chmap_sizeof(chmap));
	      ret = 0;
	    }
	} else
	{
	  ret = 0;
	}
      if ( ret == 0 )
	{
	  //Disabled because there is no input
	  cmap->channels = 0;
	}
    }
  return ret;
}


static int32_t dspd_stream_setparams(struct dspd_client_stream *stream,
				     const struct dspd_cli_params *params)
{
  struct dspd_shm_addr addr[2];
  int ret;
  struct dspd_shm_addr a;
  int c;
 
  if ( params )
    c = params->channels;
  else
    c = 0;
  dspd_fifo_destroy(&stream->fifo);
  dspd_mbx_destroy(&stream->mbx);
  dspd_shm_close(&stream->shm);
  if ( c == 0 )
    {
      stream->enabled = 0;
      stream->params.channels = 0;
      return 0;
    }

  stream->params.stream = params->stream;
  stream->ready = 0;
  addr[0].length = dspd_fifo_size(params->bufsize,
				  params->channels * sizeof(float));
  addr[0].section_id = DSPD_CLIENT_SECTION_FIFO;
  addr[0].addr = NULL;

  addr[1].length = dspd_mbx_bufsize(sizeof(struct dspd_pcm_status));
  addr[1].section_id = DSPD_CLIENT_SECTION_MBX;
  addr[1].addr = NULL;
  
  if ( params->flags & DSPD_CLI_FLAG_SHM )
    stream->shm.flags = 0;
  else
    stream->shm.flags = DSPD_SHM_FLAG_PRIVATE;
  stream->shm.flags |= (DSPD_SHM_FLAG_READ | DSPD_SHM_FLAG_WRITE);
  ret = dspd_shm_create(&stream->shm,
			addr,
			2);
  if ( ret == 0 )
    {
      a.length = 0;
      a.section_id = DSPD_CLIENT_SECTION_MBX;
      a.addr = NULL;
      ret = dspd_shm_get_addr(&stream->shm,
			      &a);
      if ( ret == 0 )
	{
	  ret = dspd_mbx_init(&stream->mbx,
			      sizeof(struct dspd_pcm_status),
			      a.addr);
	}

      if ( ret == 0 )
	{
	  a.length = 0;
	  a.addr = NULL;
	  a.section_id = DSPD_CLIENT_SECTION_FIFO;
	  ret = dspd_shm_get_addr(&stream->shm,
				  &a);
	  if ( ret == 0 )
	    ret = dspd_fifo_init(&stream->fifo,
				 params->bufsize,
				 sizeof(float) * params->channels,
				 a.addr);
	}
      if ( ret == 0 )
	{
	  memcpy(&stream->params, params, sizeof(*params));
	  stream->sample_time = 1000000000 / params->rate;
	  stream->ready = 1;
	}
    }

  return ret;
}




static int32_t get_playback_status(void     *dev,
				   void     *client,      
				   uint64_t *pointer,    
				   uint64_t *start_count,
				   uint32_t *latency,
				   uintptr_t frames,
				   const struct dspd_pcm_status *status)
{
  int32_t ret;
  struct dspd_client *cli = client;
  uint32_t len;
  const struct dspd_client_trigger_tstamp *ts;
  dspd_time_t start_tstamp = 0; 
  bool start_valid = 0;
  if ( cli->playback.enabled )
    {
      //fprintf(stderr, "LATENCIES: %u %u\n", *latency, cli->latency);

      /*
	The latency selection protocol works by checking a "register"
	for a configuration value after submitting a buffer of data
	at the current application pointer (not a previous one it may
	have rewound back to).

	If the latency transitions from high to low then the buffer
	is considered to be overfilled with small fragments and the
	server will wake up often enough to service a small buffer
	size while actually letting the buffer fill level get lower.
	This gives a chances for the new client to put data into
	the buffer at a rewound position.

	If the latency transitions from low to high then the fragment
	size changes immediately.  Since i/o always happens in fractions
	of the fill size there will not be a problem of the buffer going
	empty while rendering a large chunk.

	This is what happens for a 128 frame to 1024 frame transition.  I am 
	keeping	it short here.  The real issue would be more like a 
	128=>16384 transition and the fill levels are not exactly where
	they are expected to be since the timing is not perfect and never
	will be on a general purpose computer.

	WAKEUP: 64 frames in buffer
	WRITE: 32 frames
	WRITE: 64
	WRITE: 128
	WRITE: 256
	WRITE: 512
	WRITE: 32
	SLEEP: BUFFER @ ~1024 frames (not exactly because
	data might have been consumed while writing).
	

      */
      

      //Do not accept this latency if it is too high
      //if ( *latency > cli->latency )
      //	*latency = cli->latency;
      *latency = cli->latency;
      
      if ( cli->playback.started == false )
	{
	  ts = dspd_mbx_acquire_read(cli->sync_start_tstamp, true);
	  if ( ts )
	    {
	      if ( ts->streams & DSPD_PCM_SBIT_PLAYBACK )
		{
		  /*
		    See if there is a timestamp already read by capture.  Make sure it is valid for
		    this context.
		   */
		  if ( cli->trigger_state.valid == true &&
		       cli->trigger_state.stream == DSPD_PCM_SBIT_PLAYBACK &&
		       cli->trigger_state.trigger_tstamp == ts->playback_tstamp &&
		       (ts->streams & DSPD_PCM_SBIT_CAPTURE) )
		    {
		      start_valid = true;
		      //This is when capture actually started
		      start_tstamp = cli->trigger_state.preferred_start;
		    } else
		    {
		      /*
			The timestamp isn't valid.  Try to prepare some data for the capture stream if it exists.
		       */
		      start_valid = true;
		      start_tstamp = ts->playback_tstamp;
		      if ( (ts->capture_tstamp == ts->playback_tstamp) && (ts->streams & DSPD_PCM_SBIT_CAPTURE) )
			{
			  cli->trigger_state.valid = false;
			  cli->trigger_state.trigger_tstamp = start_tstamp;
			}
		    }
		} else
		{
		  cli->trigger_state.valid = false;
		}
	      dspd_mbx_release_read(cli->sync_start_tstamp, (void*)ts);
	    } else
	    {
	      cli->trigger_state.valid = false;
	    }
	}
      if ( start_valid && status->tstamp )
	{
	  dspd_time_t diff, sample_time;
	  if ( cli->playback_src.rate > 0 )
	    sample_time = 1000000000 / cli->playback_src.rate;
	  else
	    sample_time = 1000000000 / cli->playback.params.rate;
	  if ( status->tstamp >= start_tstamp )
	    {
	      //This would actually underrun but the server will try to select a safe value.
	      *pointer = status->hw_ptr;
	    } else if ( status->tstamp < start_tstamp )
	    {
	      diff = start_tstamp - status->tstamp;
	      diff /= sample_time;
	      *pointer = diff + status->hw_ptr;
	      if ( *pointer >= (status->appl_ptr + frames) )
		{
		  *latency = diff;
		  return -EAGAIN;
		}
	      
	    }
	  cli->playback.dev_appl_ptr = *pointer;
	} else
	{
	  //Reset device application pointer if the device was restarted.
	  if ( *start_count != cli->playback.start_count )
	    cli->playback.dev_appl_ptr = 0;
	  *pointer = cli->playback.dev_appl_ptr;
	}
      *start_count = cli->playback.start_count;
	

      if ( dspd_fifo_len(&cli->playback.fifo, &len) != 0 )
	ret = -EAGAIN;
      else if ( len == 0 )
	ret = -EAGAIN;
      else
	ret = 0;
    } else
    {
      ret = -EAGAIN;
    }
 
  return ret;
}

static int32_t playback_src_read(struct dspd_client *cli,
				 float **ptr,
				 uint32_t *len)
{
  size_t frames_requested = *len;
  size_t count, maxf, offset, l, infr;
  float *sbuf, *fbuf;
  uint32_t n;
  int32_t ret;
  if ( ! (cli->playback_src.buf && cli->playback_src.src) )
    return -1;

  //How many frames at the client rate is this?
  count = dspd_src_get_frame_count(cli->playback_src.rate,
				   cli->playback.params.rate,
				   frames_requested);

  //Can't generate more than will fit in the buffer.
  maxf = cli->playback_src.nsamples / cli->playback.params.channels;
  if ( frames_requested > maxf )
    frames_requested = maxf;
  *len = 0;
  offset = 0;
  while ( offset < frames_requested )
    {
      sbuf = &cli->playback_src.buf[offset*cli->playback.params.channels];
      l = frames_requested - offset;
      ret = dspd_fifo_riov(&cli->playback.fifo, 
			   (void**)&fbuf,
			   &n);
      if ( ret < 0 )
	break;
      //Stop when fifo is empty.
      if ( n == 0 )
	break;
      //Don't give the resampler too many frames or it may do undefined
      //things like store them for the next call.  Do give it an extra
      //round of processing just in case it is buggy or slightly off.
      //It does happen and it used to make PulseAudio barf with some resamplers.
      if ( n > count && count > 0 )
	infr = count;
      else
	infr = n;
      if ( dspd_src_process(cli->playback_src.src,
			    0,
			    fbuf,
			    &infr,
			    sbuf,
			    &l) < 0 )
	{
	  break;
	}
      dspd_fifo_rcommit(&cli->playback.fifo, infr);
      if ( count > 0 )
	count -= infr;
      offset += l;
    }
  
  *len = offset;
  *ptr = cli->playback_src.buf;

  return 0;
}

static void playback_xfer(void                            *dev,
			  void                            *client,
			  double                          *buf,
			  uintptr_t                        frames,
			  uint64_t                         start_count,
			  const struct dspd_pcm_status    *status)
{
  struct dspd_client *cli = client;
  uintptr_t offset = 0;
  float *ptr, *infr;
  double *out, *outfr;
  int32_t ret;
  uint32_t count, commit_size;
  struct dspd_pcm_status *cs;
  struct dspd_chmap *map;
  size_t i, c, j, n;
  float volume = dspd_load_float32(&cli->playback.volume);
  uint32_t client_hwptr;
  uint64_t optr = status->appl_ptr;
  uint32_t len;

  client_hwptr = dspd_fifo_optr(&cli->playback.fifo);
       


  cli->playback.start_count = start_count;
  cli->playback.dev_appl_ptr = status->appl_ptr;
  map = &cli->playback_cmap.map;
 
  while ( offset < frames )
    {
      commit_size = 0;
      if ( cli->playback_src.rate != cli->playback.params.rate )
	{
	  count = frames - offset;
	  c = count;
	  ret = playback_src_read(cli,
				  &ptr,
				  &count);
	
	    
	  if ( count == 0 )
	    {
	      ret = -EAGAIN;
	    } 
	} else
	{
	  ret = dspd_fifo_riov(&cli->playback.fifo, 
			       (void**)&ptr,
			       &count);
	  if ( count > 0 )
	    {
	      c = frames - offset;
	   
	      if ( count > c )
		count = c;
	      commit_size = count;
	    } else
	    {
	      ret = -EAGAIN;
	    }
	}
      if ( ret == 0 )
	{
	  
	  //Pointer to this block of output
	  out = &buf[cli->playback_schan*offset];

	  if ( ! (map->stream & DSPD_CHMAP_MULTI) )
	    {
	      //Iterate over all frames
	      for ( i = 0; i < count; i++ )
		{

		  //Pointer to device frame
		  outfr = &out[i*cli->playback_schan];
		  //Pointer to client frame
		  infr = &ptr[i*cli->playback.params.channels];

		  //Mix this frame with a possibly truncated or
		  //enlarged channel map.  Technically
		  //it should be possible to set pos[0]=pos[1]=0
		  //and emulate 2ch stereo on a mono device.
		  for ( j = 0; j < map->channels; j++ )
		    outfr[map->pos[j]] += (infr[j] * volume);

		}
	    } else
	    {
	      //Iterate over all frames
	      for ( i = 0; i < count; i++ )
		{

		  //Pointer to device frame
		  outfr = &out[i*cli->playback_schan];
		  //Pointer to client frame
		  infr = &ptr[i*cli->playback.params.channels];

		  //Multi map has possibly discontiguous channels interleaved (in,out,...)
		  n = map->channels * 2;
		  for ( j = 0; j < n; j += 2 )
		    outfr[map->pos[j+1]] += (infr[map->pos[j]] * volume);

		}
	    }
	  offset += count;
	} else
	{
	  break;
	  count = 0;
	}
      if ( commit_size )
	dspd_fifo_rcommit(&cli->playback.fifo, commit_size);
      cli->playback.dev_appl_ptr += count;
    } 
    
  assert(offset <= frames);

  if ( dspd_dctx.debug && offset < frames )
    fprintf(stderr, "CLIENT PLAYBACK XRUN: wanted %lu got %lu\n", (long)offset, (long)frames);

  assert(cli->playback.dev_appl_ptr == (optr+offset));
  if ( status->tstamp == cli->playback.last_hw_tstamp )
    return;

 

  cs = dspd_mbx_acquire_write(&cli->playback.mbx);
  if ( cs )
    {
      cs->appl_ptr = dspd_fifo_iptr(&cli->playback.fifo);
      cs->hw_ptr = client_hwptr; //dspd_fifo_optr(&cli->playback.fifo);
      cs->tstamp = status->tstamp;
      cs->fill = cs->appl_ptr - cs->hw_ptr;
      cs->space = cli->playback.params.bufsize - cs->fill;

      /*
	Translate server frames to client frames.  This is the
	additional delay.  The client will estimate how far the
	hardware pointer has gone and completely ignore actual
	fill levels for A/V sync purposes.  This is fine for
	most sound systems because reading the delay is not considered
	an appropriate way to figure out if an operation will block.
	Fill and delay are often the same on real hardware (or really close
	and the drivers fudge it) but that does not have to be the case.
      */
      if ( cli->playback_src.rate != cli->playback.params.rate )
	{
	  cs->delay = dspd_src_get_frame_count(cli->playback_src.rate,
					       cli->playback.params.rate,
					       status->delay);
	} else
	{
	  cs->delay = status->delay;
	}
      cs->error = status->error;
      dspd_mbx_release_write(&cli->playback.mbx, cs);
      cli->playback.last_hw_tstamp = status->tstamp;
    }

  if ( cli->playback.started == false )
    {
      cli->playback.started = true;
     
      /* dspd_time_t t, sample_time;
      if ( cli->playback_src.rate > 0 )
	sample_time = 1000000000ULL / cli->playback_src.rate;
      else
	sample_time = 1000000000ULL / cli->playback.params.rate;
      t = (status->appl_ptr - status->hw_ptr) * sample_time;
      fprintf(stderr, "DIFF=%lld %lld\n", (long long)(cli->playback.trigger_tstamp - (t + status->tstamp)) / sample_time, status->tstamp);*/
      


      if ( cli->trigger_state.stream == DSPD_PCM_SBIT_PLAYBACK )
	{
	  cli->trigger_state.valid = false;
	  cli->trigger_state.stream = 0;

	  

	} else if ( (cli->trigger_state.stream == DSPD_PCM_SBIT_CAPTURE) &&
		    cli->trigger_state.valid == false )
	{

	  dspd_time_t diff, sample_time;
	  if ( cli->playback_src.rate > 0 )
	    sample_time = 1000000000ULL / cli->playback_src.rate;
	  else
	    sample_time = 1000000000ULL / cli->playback.params.rate;
	  diff = status->appl_ptr - status->hw_ptr;
	  cli->trigger_state.preferred_start = status->tstamp + (diff * sample_time);
	  cli->trigger_state.valid = true;
	}
    }

  if ( cli->mq_fd >= 0 )
    {
      if ( dspd_fifo_len(&cli->playback.fifo, &len) == 0 )
	{
	  if ( len >= dspd_load_uint32(&cli->avail_min) )
	    {
	      char c = 0;
	      (void)mq_send(cli->mq_fd, &c, sizeof(c), 0);	    }
	}
    }
   
}





static void playback_set_volume(void *handle, double volume)
{
  struct dspd_client *cli = handle;
  dspd_store_float32(&cli->playback.volume, volume);
  return;
}


static double playback_get_volume(void *handle)
{
  struct dspd_client *cli = handle;
  return dspd_load_float32(&cli->playback.volume);
}

static int32_t playback_get_params(void *handle, struct dspd_cli_params *params)
{
  struct dspd_client *cli = handle;
  int32_t ret;
  if ( cli->err )
    {
      ret = cli->err;
    } else if ( cli->playback.ready )
    {
      memcpy(params, &cli->playback.params, sizeof(*params));
      ret = 0;
    } else
    {
      ret = -EBADF;
    }
  return ret;
}

static int32_t playback_set_params(void *handle, const struct dspd_cli_params *params)
{
  struct dspd_client *cli = handle;
  int32_t ret;
  if ( cli->err )
    {
      ret = cli->err;
    } else
    {
      dspd_slist_entry_srvlock(cli->list, (uintptr_t)cli->index);
      ret = dspd_stream_setparams(&cli->playback, params);
      if ( ret == 0 && params != NULL )
	cli->latency = params->latency;
      dspd_slist_entry_srvunlock(cli->list, (uintptr_t)cli->index);
    }
  return ret;
}

static void capture_set_volume(void *handle, double volume)
{
  struct dspd_client *cli = handle;
  dspd_store_float32(&cli->capture.volume, volume);
}

static double capture_get_volume(void *handle)
{
  struct dspd_client *cli = handle;
  return dspd_load_float32(&cli->capture.volume);
}

static int32_t capture_get_params(void *handle, struct dspd_cli_params *params)
{
  struct dspd_client *cli = handle;
  int32_t ret;
  if ( cli->err )
    {
      ret = cli->err;
    } else if ( cli->capture.ready )
    {
      memcpy(params, &cli->capture.params, sizeof(*params));
      ret = 0;
    } else
    {
      ret = -EBADF;
    }
  return ret;
}

static int32_t capture_set_params(void *handle, const struct dspd_cli_params *params)
{
  struct dspd_client *cli = handle;
  int32_t ret;
  if ( cli->err )
    {
      ret = cli->err;
    } else
    {
      dspd_slist_entry_srvlock(cli->list, (uintptr_t)cli->index);
      cli->capture.started = 0;
      dspd_mbx_reset(cli->sync_start_tstamp);
      ret = dspd_stream_setparams(&cli->capture, params);
      if ( ret == 0 && params != NULL )
	cli->latency = params->latency;
      dspd_slist_entry_srvunlock(cli->list, (uintptr_t)cli->index);
    }
  return ret;
}


static int32_t get_capture_status(void     *dev,
				  void     *client,     
				  uint64_t *pointer,
				  uint64_t *start_count,
				  uint32_t *latency,
				  const struct dspd_pcm_status *status)
{
  struct dspd_client *cli = client;
  uint32_t space;
  int32_t ret;
  dspd_time_t start_tstamp;
  bool start_valid = false;
  const struct dspd_client_trigger_tstamp *ts;
  
  if ( ! cli->capture.enabled )
    return -EAGAIN;
  if ( dspd_fifo_space(&cli->capture.fifo, &space) != 0 )
    return -EAGAIN;
  if ( space == 0 )
    return -EAGAIN;
  if ( ! cli->capture.started )
    {
      ts = dspd_mbx_acquire_read(cli->sync_start_tstamp, true);
      if ( ts )
	{
	  if ( ts->streams & DSPD_PCM_SBIT_CAPTURE )
	    {
	      /*
		See if there is a timestamp already read by capture.  Make sure it is valid for
		this context.
	      */
	      if ( cli->trigger_state.valid == true &&
		   cli->trigger_state.stream == DSPD_PCM_SBIT_CAPTURE &&
		   cli->trigger_state.trigger_tstamp == ts->capture_tstamp &&
		   (ts->streams & DSPD_PCM_SBIT_CAPTURE) )
		{
		  start_valid = true;
		  //This is when capture actually started
		  start_tstamp = cli->trigger_state.preferred_start;
		} else
		{
		  /*
		    The timestamp isn't valid.  Try to prepare some data for the capture stream if it exists.
		  */
		  start_valid = true;
		  start_tstamp = ts->capture_tstamp;
		  if ( (ts->capture_tstamp == ts->capture_tstamp) && (ts->streams & DSPD_PCM_SBIT_PLAYBACK) )
		    {
		      cli->trigger_state.valid = false;
		      cli->trigger_state.trigger_tstamp = start_tstamp;
		    }
		}
	    } else
	    {
	      cli->trigger_state.valid = false;
	    }
	  dspd_mbx_release_read(cli->sync_start_tstamp, (void*)ts);
	} else
	{
	  cli->trigger_state.valid = false;
	}
    }
 
  if ( start_valid && status->tstamp )
    {
      dspd_time_t diff, sample_time, l;
      if ( cli->capture_src.rate > 0 )
	sample_time = 1000000000 / cli->capture_src.rate;
      else
	sample_time = 1000000000 / cli->capture.params.rate;
      if ( start_tstamp >= status->tstamp )
	{
	  diff = (start_tstamp - status->tstamp) / sample_time;
	  if ( diff > 0 )
	    *latency = diff;
	  ret = -EAGAIN;
	} else
	{
	  diff = (status->tstamp - start_tstamp) / sample_time;
	  l = (1000000000 / cli->capture.params.rate) * cli->capture.params.latency;
	  if ( diff > l )
	    diff = l;
	  *pointer = status->hw_ptr - diff;
	  *start_count = cli->capture.start_count;
	  ret = 0;
	}
    } else
    {
      ret = 0;
      if ( *start_count != cli->capture.start_count )
	cli->capture.dev_appl_ptr = 0;
      *pointer = cli->capture.dev_appl_ptr;
    }

  return ret;
}

static uint32_t capture_src_xfer(struct dspd_client *cli, float32 *buf, size_t frames)
{
  int32_t ret;
  float32 *ptr;
  uint32_t count;
  size_t c;
  size_t offset = 0;
  size_t fi;
  
  uint32_t ri, ro, q;

  dspd_src_get_params(cli->capture_src.src, &q, &ri, &ro);


  while ( offset < frames )
    {
      ret = dspd_fifo_wiov(&cli->capture.fifo,
			   (void**)&ptr,
			   &count);
      if ( ret != 0 || count == 0 )
	break;
      fi = frames - offset;
      c = count;
      ret = dspd_src_process(cli->capture_src.src, 
			     0,
			     &buf[offset*cli->capture.params.channels],
			     &fi,
			     ptr,
			     &c);

      //fwrite(ptr, c * sizeof(*buf) * cli->capture.params.channels, 1, fp);

      assert(c <= count);
      if ( ret != 0 )
	{
	  break;
	}
      dspd_fifo_wcommit(&cli->capture.fifo, c);
      offset += fi;
    }
  return offset;
}

static void capture_xfer(void                            *dev,
			 void                            *client,
			 const float                     *buf,
			 uintptr_t                        frames,
			 const struct dspd_pcm_status    *status)
{
  struct dspd_client *cli = client;
  uintptr_t offset = 0;
  int32_t ret;
  float *ptr, *outfr;
  const float *in;
  uint32_t count;
  struct dspd_pcm_status *cs;
  const float *infr;
  size_t i, c, j;
  struct dspd_chmap *map = &cli->capture_cmap.map;
  float volume = dspd_load_float32(&cli->capture.volume);
  uint32_t client_hwptr;
  bool do_src = cli->capture.params.rate != cli->capture_src.rate;
  uint32_t n;

 
  if ( dspd_fifo_space(&cli->capture.fifo, &n) != 0 )
    return;

  n = dspd_src_get_frame_count(cli->capture.params.rate, cli->capture_src.rate, n);
  
  if ( frames > n )
    frames = n;

  

 
 
  while ( offset < frames )
    {
      
      if ( do_src )
	{
	  count = cli->capture_src.nsamples / cli->capture_schan;
	  ptr = cli->capture_src.buf;
	  ret = 0;
	} else
	{
	  ret = dspd_fifo_wiov(&cli->capture.fifo,
			       (void**)&ptr,
			       &count);
	}
      if ( ret == 0 && count > 0 )
	{
	  c = frames - offset;
	  if ( count > c )
	    count = c;
	  c = count * cli->capture_schan;

	  //Clear out the memory so the channel map
	  //can be mixed correctly.  I don't think it is likely
	  //that anyone would use a channel map this way but 
	  //it probably won't hurt peformance to bad to allow it.
	  memset(ptr, 0, sizeof(*ptr) * c);

	  
	  
	  in = &buf[cli->capture_schan * offset];
	  if ( ! (map->stream & DSPD_CHMAP_MULTI) )
	    {
	      for ( i = 0; i < count; i++ )
		{
		  infr = &in[i*cli->capture_schan];
		  outfr = &ptr[i*cli->capture.params.channels];
		  for ( j = 0; j < map->channels; j++ )
		    outfr[j] += (infr[map->pos[j]] * volume);
		}
	    } else
	    {
	      for ( i = 0; i < count; i++ )
		{
		  infr = &in[i*cli->capture_schan];
		  outfr = &ptr[i*cli->capture.params.channels];
		  n = map->channels * 2;
		  for ( j = 0; j < n; j += 2 )
		    outfr[map->pos[j+1]] += (infr[map->pos[j]] * volume);
		}
	    }
	  offset += count;
	  cli->capture.dev_appl_ptr += count;
	  if ( do_src )
	    {
	      if ( capture_src_xfer(cli, ptr, count) != count )
		break;
	    } else
	    {
	      dspd_fifo_wcommit(&cli->capture.fifo, count);
	    }
	} else
	{
	  count = 0;
	  break;
	}
   
    }
    

  if ( status->tstamp == cli->capture.last_hw_tstamp )
    return;

  client_hwptr = dspd_fifo_iptr(&cli->capture.fifo);

  cs = dspd_mbx_acquire_write(&cli->capture.mbx);
  if ( cs )
    {
      cs->appl_ptr = dspd_fifo_optr(&cli->capture.fifo);
      cs->hw_ptr = client_hwptr;
      cs->tstamp = status->tstamp;
      cs->fill = cs->hw_ptr - cs->appl_ptr;
      cs->space = cli->capture.params.bufsize = cs->fill;
      cs->delay = status->delay;
      cs->error = status->error;
      dspd_mbx_release_write(&cli->capture.mbx, cs);
      cli->capture.last_hw_tstamp = status->tstamp;
    }
  
  if ( cli->capture.started == false )
    {
      cli->capture.started = true;
      if ( cli->trigger_state.stream == DSPD_PCM_SBIT_CAPTURE )
	{
	  cli->trigger_state.valid = false;
	  cli->trigger_state.stream = 0;
	} else if ( (cli->trigger_state.stream == DSPD_PCM_SBIT_PLAYBACK) &&
		    cli->trigger_state.valid == false )
	{

	  dspd_time_t diff, sample_time;
	  if ( cli->capture_src.rate > 0 )
	    sample_time = 1000000000ULL / cli->capture_src.rate;
	  else
	    sample_time = 1000000000ULL / cli->capture.params.rate;
	  diff = status->hw_ptr - status->appl_ptr;
	  cli->trigger_state.preferred_start = status->tstamp - (diff * sample_time);
	  cli->trigger_state.valid = true;

	}
    }

}

static int32_t client_start(struct dspd_rctx *context,
			    uint32_t      req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  uint32_t stream = *(int32_t*)inbuf;
  int32_t err, ret;
  struct dspd_client *cli = dspd_req_userdata(context);
  dspd_time_t tstamps[2];
  dspd_time_t ts = get_start_time(cli);
  err = client_start_at_time(cli, ts, stream, tstamps, false);
  if ( err == 0 && outbufsize == sizeof(tstamps) )
    {
      ret = dspd_req_reply_buf(context, 0, tstamps, sizeof(tstamps));
    } else
    {
      ret = dspd_req_reply_err(context, 0, err);
    }
  return ret;
}

static int32_t client_stop(struct dspd_rctx *context,
			    uint32_t      req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  uint32_t stream = *(int32_t*)inbuf;
  int32_t err;
  struct dspd_client *cli = dspd_req_userdata(context); 
  err = client_stop_now(cli, stream);
  return dspd_req_reply_err(context, 0, err);
}

static int32_t client_getparams(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  int err, ret;
  uint32_t stream = *(int32_t*)inbuf;
  struct dspd_client *cli = dspd_req_userdata(context);
  struct dspd_cli_params *cp = outbuf;
  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      err = playback_get_params(cli, cp);
      cp->stream = DSPD_PCM_SBIT_PLAYBACK;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      err = capture_get_params(cli, cp);
      cp->stream = DSPD_PCM_SBIT_CAPTURE;
    } else
    {
      err = EINVAL;
    }
  if ( err )
    ret = dspd_req_reply_err(context, 0, err);
  else
    ret = dspd_req_reply_buf(context, 0, outbuf, sizeof(struct dspd_cli_params));
  return ret;
}

static int32_t client_setparams(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  int err = 0, ret;
  const struct dspd_cli_params *params = inbuf;
  struct dspd_cli_params *oparams;
  struct dspd_client *cli = dspd_req_userdata(context);

  if ( params->rate < 1000 || params->rate > 384000 )
    return dspd_req_reply_err(context, 0, EINVAL);

  if ( params->stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      err = playback_set_params(cli, params);
      if ( err == 0 && params->channels > 0 )
	{
	  err = dspd_stream_setsrc(cli, true);
	  if ( err )
	    {
	      playback_set_params(cli, NULL);
	    } else
	    {
	      cli->playback.enabled = 1;
	      playback_get_params(cli, outbuf);
	    }
	}
    } else if ( params->stream == DSPD_PCM_SBIT_CAPTURE )
    {
      err = capture_set_params(cli, params);
      if ( err == 0 && params->channels > 0 )
	{
	  err = dspd_stream_setsrc(cli, true);
	  if ( err )
	    {
	      capture_set_params(cli, NULL);
	    } else
	    {
	      cli->capture.enabled = 1;
	      capture_get_params(cli, outbuf);
	    }
	}
    } else
    {
      err = EINVAL;
    }
  if ( err == 0 )
    {
      oparams = outbuf;
      if ( oparams->channels != params->channels )
	{
	  //Reset to default channel map if the currently installed map does not
	  //work.
	  if ( params->stream == DSPD_PCM_SBIT_CAPTURE )
	    cli->capture_inmap.map.channels = 0;
	  else
	    cli->playback_inmap.map.channels = 0;
	}
      dspd_stream_setchannelmap(cli, NULL, params->stream);
    }


  if ( err )
    ret = dspd_req_reply_err(context, 0, err);
  else
    ret = dspd_req_reply_buf(context, 0, outbuf, sizeof(struct dspd_cli_params));
  return ret;
}

static int32_t client_setvolume(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  const struct dspd_stream_volume *sv = inbuf;
  struct dspd_client *cli = dspd_req_userdata(context);
  if ( sv->stream & DSPD_PCM_SBIT_PLAYBACK )
    playback_set_volume(cli, sv->volume);
  if ( sv->stream & DSPD_PCM_SBIT_CAPTURE )
    capture_set_volume(cli, sv->volume);
  return dspd_req_reply_err(context, 0, 0);
}

static int32_t client_getvolume(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  int32_t stream = *(int32_t*)inbuf;
  float vol;
  if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      vol = capture_get_volume(cli);
    } else if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      vol = playback_get_volume(cli);
    } else
    {
      vol = 0.0;
    }
  return dspd_req_reply_buf(context, 0, &vol, sizeof(vol));
}

static int32_t client_connect(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  uint32_t idx = *(uint32_t*)inbuf;
  void *server_ops, *client_ops, *data;
  int32_t err = EINVAL, sb;
  size_t br;
  struct dspd_cli_params params;
  uint32_t playback_rate = 0, capture_rate = 0, latency;
  dspd_slist_entry_wrlock(cli->list, cli->index);
  dspd_slist_entry_srvlock(cli->list, cli->index);

  //Reconnecting is ok
  if ( cli->device >= 0 )
    {
      if ( cli->device != idx )
	{
	  err = EALREADY;
	  goto out;
	}
    }
  if ( idx < 0 || idx >= DSPD_MAX_OBJECTS )
    {
      err = EINVAL;
      goto out;
    }

  //Make sure it is a server and get a reference if it is.
  dspd_slist_entry_wrlock(cli->list, idx);
  if ( dspd_slist_refcnt(cli->list, idx) > 0 )
    {
      dspd_slist_entry_get_pointers(cli->list,
				    idx,
				    &data,
				    &server_ops,
				    &client_ops);
      if ( server_ops )
	{
	  dspd_slist_ref(cli->list, idx);
	  err = 0;
	} else
	{
	  err = EINVAL;
	}
    } else
    {
      err = ENOENT;
    }
  dspd_slist_entry_rw_unlock(cli->list, idx);

  if ( err == 0 )
    {
      sb = DSPD_PCM_SBIT_PLAYBACK;
      if ( dspd_stream_ctl(&dspd_dctx,
			  idx,
			  DSPD_SCTL_SERVER_GETPARAMS,
			  &sb,
			  sizeof(sb),
			  &params,
			  sizeof(params),
			  &br) == 0 )
	{
	  if ( br == sizeof(params) )
	    {
	      playback_rate = params.rate;
	      if ( playback_rate )
		cli->min_latency = (1000000000 / playback_rate) * params.min_latency;
	    }
	}
      sb = DSPD_PCM_SBIT_CAPTURE;
      if ( dspd_stream_ctl(&dspd_dctx,
			  idx,
			  DSPD_SCTL_SERVER_GETPARAMS,
			  &sb,
			  sizeof(sb),
			  &params,
			  sizeof(params),
			  &br) == 0 )
	{
	  if ( br == sizeof(params) )
	    {
	      capture_rate = params.rate;
	      if ( capture_rate )
		cli->min_latency = (1000000000 / capture_rate) * params.min_latency;
	    }
	}
      if ( capture_rate == 0 && playback_rate == 0 )
	err = ENOSR;
    }

  //Try to connect if no errors occured
  if ( err == 0 )
    {
      cli->playback_src.rate = playback_rate;
      cli->capture_src.rate = capture_rate;
      /*

	The server should query params and stuff.  It should
	increase the reference count of the client if successful.
      */
      err = dspd_stream_ctl(&dspd_dctx,
			    idx,
			    DSPD_SCTL_SERVER_CONNECT,
			    &cli->index,
			    sizeof(cli->index),
			    NULL,
			    0,
			    &br);
      if ( err != 0 )
	{
	  err *= -1;
	  cli->playback_src.rate = 0;
	  cli->capture_src.rate = 0;
	  if ( cli->device_reserved == 0 )
	    {
	      dspd_slist_entry_wrlock(cli->list, idx);
	      dspd_slist_unref(cli->list, idx);
	      dspd_slist_entry_rw_unlock(cli->list, idx);
	    }
	} else
	{
	  err = dspd_stream_ctl(&dspd_dctx,
				idx,
				DSPD_SCTL_SERVER_GETLATENCY,
				&cli->index,
				sizeof(cli->index),
				&latency,
				sizeof(latency),
				&br);
	  if ( err == 0 && latency > 0 )
	    cli->latency = latency;
	  cli->device = idx;
	  cli->server_ops = server_ops;
	  cli->server = data;
	  if ( cli->playback.params.rate )
	    dspd_stream_setchannelmap(cli, NULL, DSPD_PCM_SBIT_PLAYBACK);
	  if ( cli->capture.params.rate )
	    dspd_stream_setchannelmap(cli, NULL, DSPD_PCM_SBIT_CAPTURE);
	  dspd_stream_setsrc(cli, false);
	  dspd_slist_entry_set_key(cli->list, cli->index, idx);
	  cli->device_reserved = 0;
	}
    }

 out:
  dspd_slist_entry_srvunlock(cli->list, cli->index);
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  return dspd_req_reply_err(context, 0, err);
}

static int32_t client_disconnect(struct dspd_rctx *context,
				  uint32_t      req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  
  int32_t ret;
  struct dspd_client *cli = dspd_req_userdata(context);
  dspd_slist_entry_wrlock(cli->list, cli->index);
  ret = dspd_client_release(cli);
  cli->device_reserved = 0;
  cli->min_latency = dspd_get_tick();
  cli->mq_fd = -1;
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  return dspd_req_reply_err(context, 0, ret);
}


static int32_t client_rawparams(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  int32_t ret;
  struct dspd_client *cli = dspd_req_userdata(context);
  const uint64_t *val = inbuf;
  uint32_t stream = ((*val) >> 32U);
  struct dspd_cli_params params;

  
  if ( stream & DSPD_PCM_SBIT_PLAYBACK )
    {

      params = cli->playback.params;
      if ( cli->playback.params.rate && cli->playback_src.rate )
	{
	  params.rate = cli->playback_src.rate;
	  params.channels = -1;
	  params.latency = dspd_src_get_frame_count(cli->playback.params.rate,
						    cli->playback_src.rate,
						    cli->playback.params.latency);
	}
      ret = dspd_req_reply_buf(context, 
			       0, 
			       &params, 
			       sizeof(params));
    } else if ( stream & DSPD_PCM_SBIT_CAPTURE )
    {
      params = cli->capture.params;
      if ( cli->capture.params.rate && cli->capture_src.rate )
	{
	  params.rate = cli->capture_src.rate;
	  params.channels = -1;
	  params.latency = dspd_src_get_frame_count(cli->capture.params.rate,
						    cli->capture_src.rate,
						    cli->capture.params.latency);
	}
      ret = dspd_req_reply_buf(context, 
			       0, 
			       &params, 
			       sizeof(params));
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}


static int32_t client_mapbuf(struct dspd_rctx *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  uint32_t stream = *(uint32_t*)inbuf;
  struct dspd_client *cli = dspd_req_userdata(context);
  int32_t err;
  struct dspd_client_shm shm;
  struct dspd_shm_map *map;
  uint64_t addr;
  int32_t ret;

  memset(&shm, 0, sizeof(shm));
  dspd_slist_entry_rdlock(cli->list, cli->index);
  if ( stream == DSPD_PCM_SBIT_PLAYBACK && 
       cli->playback.enabled )
    {
      map = &cli->playback.shm;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE &&
		cli->capture.enabled )
    {
      map = &cli->capture.shm;
    } else
    {
      map = NULL;
    }
  if ( map )
    {
      shm.len = map->length;
      shm.flags = map->flags;
      shm.section_count = map->section_count;
      if ( map->flags & DSPD_SHM_FLAG_PRIVATE &&
	   (dspd_req_flags(context) & DSPD_REQ_FLAG_REMOTE) )
	{
	  err = EINVAL;
	} else if ( map->flags & DSPD_SHM_FLAG_PRIVATE )
	{
	  if ( sizeof(void*) == 8 )
	    {
	      addr = (uintptr_t)map->addr;
	      shm.arg = addr & 0xFFFFFFFF;
	      shm.reserved = (uint32_t)(addr >> 32U);
	    } else
	    {
	      shm.arg = (uintptr_t)map->addr;
	      shm.reserved = 0;
	    }
	  err = 0;
	  shm.key = map->key;
	} else
	{
	  shm.arg = map->arg;
	  shm.key = map->key;
	  shm.reserved = 0;
	  err = 0;
	}
    } else
    {
      err = EINVAL;
    }
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  if ( err )
    {
      ret = dspd_req_reply_err(context, 0, err);
    } else
    {
      if ( shm.flags & DSPD_SHM_FLAG_MMAP )
	ret = dspd_req_reply_fd(context, 0, &shm, sizeof(shm), shm.arg);
      else
	ret = dspd_req_reply_buf(context, 0, &shm, sizeof(shm));
    }
  return ret;
}

static int32_t client_getchannelmap(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  struct dspd_chmap *omap = outbuf, *imap;
  int32_t stream = *(int32_t*)inbuf;
  int32_t err, ret;
  size_t len;
  struct dspd_client *cli = dspd_req_userdata(context);
  dspd_client_lock(cli, false);
  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      imap = &cli->playback_inmap.map;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      imap = &cli->capture_inmap.map;
    } else
    {
      imap = NULL;
    }
  if ( imap )
    {
      len = dspd_chmap_sizeof(omap);
      if ( len <= outbufsize )
	{
	  memcpy(omap, imap, len);
	  err = 0;
	} else
	{
	  err = ENOBUFS;
	}
    } else
    {
      err = -EINVAL;
    }
  dspd_client_unlock(cli);
  if ( err == 0 )
    ret = dspd_req_reply_buf(context, 0, omap, len);
  else
    ret = dspd_req_reply_err(context, 0, err);
  return ret;
}

static int32_t client_setchannelmap(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  const struct dspd_chmap *pkt = inbuf;
  int32_t err;
  struct dspd_client *cli = dspd_req_userdata(context);

  if ( dspd_chmap_sizeof(pkt) <= inbufsize )
    {
      dspd_client_lock(cli, true);
      dspd_client_srvlock(cli);
      err = dspd_stream_setchannelmap(cli, pkt, pkt->stream);
      dspd_client_srvunlock(cli);
      dspd_client_unlock(cli);
    } else
    {
      err = E2BIG;
    }
  return dspd_req_reply_err(context, 0, err);
  
}
static int32_t client_setcb(struct dspd_rctx *context,
			    uint32_t      req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  const struct dspd_client_cb *cb = inbuf;
  int ret = 0;
  dspd_client_srvlock(cli);
  switch(cb->index)
    {
    case DSPD_CLIENT_CB_ERROR:
      cli->error_cb = cb->callback;
      cli->error_arg = cb->arg;
      break;
    default:
      ret = EINVAL;
      break;
    }
  dspd_client_srvunlock(cli);
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t client_stat(struct dspd_rctx *context,
			   uint32_t      req,
			   const void   *inbuf,
			   size_t        inbufsize,
			   void         *outbuf,
			   size_t        outbufsize)
{
  struct dspd_cli_stat *params = outbuf;
  struct dspd_client *cli = dspd_req_userdata(context);
  memset(params, 0, sizeof(*params));
  dspd_slist_entry_rdlock(cli->list, cli->index);
  if ( cli->playback.params.channels && cli->playback.enabled )
    {
      params->streams |= DSPD_PCM_SBIT_PLAYBACK;
      memcpy(&params->playback, &cli->playback.params, sizeof(cli->playback.params));
    }
  if ( cli->capture.params.channels && cli->capture.enabled )
    {
      params->streams |= DSPD_PCM_SBIT_CAPTURE;
      memcpy(&params->capture, &cli->capture.params, sizeof(cli->capture.params));
    }
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  return dspd_req_reply_buf(context, 0, params, sizeof(*params));
}

static int32_t client_reserve(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  uint32_t server;
  int32_t ret;
  size_t br;
  dspd_slist_entry_wrlock(cli->list, cli->index);
  dspd_slist_entry_srvlock(cli->list, cli->index);

  if ( inbufsize == sizeof(server) )
    {
      server = *(uint32_t*)inbuf;
      if ( cli->device >= 0 )
	{
	  ret = EALREADY;
	  goto out;
	}
    } else
    {
      if ( cli->device < 0 )
	{
	  ret = EINVAL;
	  goto out;
	}

      cli->device_reserved = 1;
      /*if ( cli->playback.enabled )
	playback_drop(cli);
      if ( cli->capture.enabled )
      capture_drop(cli);*/
      ret = 0;
      goto out;
    }
    
  

  ret = dspd_daemon_ref(server, DSPD_DCTL_ENUM_TYPE_SERVER);
  if ( ret == 0 )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    server,
			    DSPD_SCTL_SERVER_RESERVE,
			    &cli->index,
			    sizeof(cli->index),
			    NULL,
			    0,
			    &br);
      if ( ret == 0 )
	{
	  dspd_slist_ref(cli->list, cli->index);
	  cli->device = server;
	  cli->device_reserved = 1;
	} else
	{
	  dspd_daemon_unref(server);
	}
    }

 out:
  dspd_slist_entry_srvunlock(cli->list, cli->index);
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  ret *= -1;
  return dspd_req_reply_err(context, 0, ret);
  
}

static int32_t client_settrigger(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  uint32_t val = *(uint32_t*)inbuf, v = 0;
  int32_t ret, old = cli->trigger, noset = 0;
  dspd_time_t now, ts, result[2];
  if ( cli->device < 0 || cli->server_ops == NULL )
    return dspd_req_reply_err(context, 0, EINVAL);
  if ( val )
    now = get_start_time(cli);
  else
    now = 0;
  ret = client_start_at_time(cli, now, val, result, true);
  if ( ret == 0 && outbufsize == sizeof(result) )
    ret = dspd_req_reply_buf(context, 0, result, sizeof(result));
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
  

  
  if ( val & DSPD_PCM_SBIT_TRIGGER_BOTH )
    {
      if ( ! cli->playback.enabled )
	val &= ~DSPD_PCM_SBIT_PLAYBACK;
      if ( ! cli->capture.enabled )
	val &= ~DSPD_PCM_SBIT_CAPTURE;
    }

  if ( val & DSPD_PCM_SBIT_PLAYBACK )
    v |= DSPD_PCM_SBIT_PLAYBACK;
  if ( val & DSPD_PCM_SBIT_CAPTURE )
    v |= DSPD_PCM_SBIT_CAPTURE;
  if ( (val & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) == (DSPD_PCM_SBIT_CAPTURE|DSPD_PCM_SBIT_PLAYBACK) &&
       cli->trigger == 0 )
    {
      cli->trigger = DSPD_PCM_SBIT_FULLDUPLEX | DSPD_PCM_SBIT_TRIGGER_BOTH;
      noset = 1;
    }
  ret = cli->server_ops->trigger(cli->server, (uint32_t)cli->index, v);
  if ( ret == 0 )
    {
      ts = now + ((dspd_get_time()-now)/2);
      if ( val & DSPD_PCM_SBIT_PLAYBACK )
	cli->playback.trigger_tstamp = ts;
      else
	cli->playback.trigger_tstamp = 0;
      if ( val & DSPD_PCM_SBIT_CAPTURE )
	cli->capture.trigger_tstamp = ts;
      else
	cli->capture.trigger_tstamp = 0;
      if ( ! noset )
	cli->trigger = val;
      if ( outbufsize == sizeof(result) )
	{
	  result[0] = cli->playback.trigger_tstamp;
	  result[1] = cli->capture.trigger_tstamp;
	  return dspd_req_reply_buf(context, 0, result, sizeof(result));
	}
    } else
    {
      cli->trigger = old;
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t client_gettrigger(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  return dspd_req_reply_buf(context, 0, &cli->trigger, sizeof(cli->trigger));
}

static int32_t client_syncgroup(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  int32_t ret, bits;
  const struct dspd_sg_info *in;
  struct dspd_sg_info *out;
  struct dspd_syncgroup *s;
  in = inbuf;
  out = outbuf;
  if ( (in != NULL && inbufsize < sizeof(struct dspd_sg_info)) ||
       (out != NULL && outbufsize < sizeof(struct dspd_sg_info)))
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    } else if ( in && out )
    {
      //Create syncgroup
      memset(out, 0, sizeof(*out));
      if ( in->sgid == 0 )
	{
	  if ( in->sbits == 0 )
	    {
	      bits = 0;
	      if ( cli->playback.enabled )
		bits |= DSPD_PCM_SBIT_PLAYBACK;
	      if ( cli->capture.enabled )
		bits |= DSPD_PCM_SBIT_CAPTURE;
	    } else
	    {
	      bits = in->sbits;
	    }
	  ret = dspd_sg_new(dspd_dctx.syncgroups, &s, bits);
	  if ( ret == 0 )
	    {
	      out->sbits = bits;
	      if ( cli->syncgroup )
		{
		  dspd_sg_remove(cli->syncgroup, cli->index);
		  dspd_sg_put(dspd_dctx.syncgroups, dspd_sg_id(cli->syncgroup));
		}
	      cli->syncgroup = s;
	      out->sgid = dspd_sg_id(s);
	      dspd_sg_add(s, cli->index);
	      ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
	    } else
	    {
	      ret = dspd_req_reply_err(context, 0, ret);
	    }
	} else
	{
	  ret = dspd_req_reply_err(context, 0, EINVAL);
	}
    } else if ( in == NULL && out != NULL )
    {
      //Get syncgroup
      if ( cli->syncgroup )
	{
	  memset(out, 0, sizeof(*out));
	  out->sgid = dspd_sg_id(cli->syncgroup);
	  out->sbits = dspd_sg_streams(cli->syncgroup);
	  ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
	} else
	{
	  ret = dspd_req_reply_err(context, 0, EIDRM);
	}
    } else if ( in != NULL && outbuf == NULL )
    {
      //Add to syncgroup
      if ( in->sgid )
	{
	  s = dspd_sg_get(dspd_dctx.syncgroups, in->sgid);
	  if ( s )
	    {
	      if ( cli->syncgroup != NULL && s != cli->syncgroup )
		{
		  dspd_sg_remove(cli->syncgroup, cli->index);
		  dspd_sg_put(dspd_dctx.syncgroups, dspd_sg_id(cli->syncgroup));
		}
	      if ( s != cli->syncgroup )
		{
		  dspd_sg_add(s, cli->index);
		  cli->syncgroup = s;
		}
	      ret = 0;
	    } else
	    {
	      ret = EIDRM;
	    }
	  ret = dspd_req_reply_err(context, 0, ret);
	} else
	{
	  ret = dspd_req_reply_err(context, 0, EINVAL);
	}
    } else
    {
      //Remove from syncgroup
      if ( cli->syncgroup )
	{
	  dspd_sg_remove(cli->syncgroup, cli->index);
	  dspd_sg_put(dspd_dctx.syncgroups, dspd_sg_id(cli->syncgroup));
	  cli->syncgroup = NULL;
	}
      ret = dspd_req_reply_err(context, 0, 0);
    }

  return ret;
}

static int32_t client_stop_now(struct dspd_client *cli, uint32_t streams)
{
  int32_t ret, old;
  struct dspd_client_trigger_tstamp *ts;
  dspd_mutex_lock(&cli->sync_start_lock);
  if ( cli->playback.ready == false )
    streams &= ~DSPD_PCM_SBIT_PLAYBACK;
  if ( cli->capture.ready == false )
    streams &= ~DSPD_PCM_SBIT_CAPTURE;

  old = cli->trigger;

  ts = dspd_mbx_acquire_write(cli->sync_start_tstamp);

  if ( ts )
    {

      cli->trigger &= ~streams;
      if ( streams & DSPD_PCM_SBIT_PLAYBACK )
	cli->playback.trigger_tstamp = 0;
      if ( streams & DSPD_PCM_SBIT_CAPTURE )
	cli->capture.trigger_tstamp = 0;
      ts->streams = cli->trigger;
      ts->playback_tstamp = cli->playback.trigger_tstamp;
      ts->capture_tstamp = cli->capture.trigger_tstamp;
      dspd_mbx_release_write(cli->sync_start_tstamp, ts);

    }
  ret = cli->server_ops->trigger(cli->server, (uint32_t)cli->index, cli->trigger);

  if ( ret == 0 && cli->trigger != old )
    {
      dspd_slist_entry_srvlock(cli->list, cli->index);
      if ( streams & DSPD_PCM_SBIT_PLAYBACK )
	{
	  dspd_fifo_reset(&cli->playback.fifo);
	  dspd_mbx_reset(&cli->playback.mbx);
	  cli->playback.dev_appl_ptr = 0;
	  cli->playback.cli_appl_ptr = 0;
	  cli->playback.start_count = 0;
	  cli->playback.started = false;
	}
      if ( streams & DSPD_PCM_SBIT_CAPTURE )
	{
	  dspd_fifo_reset(&cli->capture.fifo);
	  dspd_mbx_reset(&cli->capture.mbx);
	  //cli->capture.dev_appl_ptr = 0;
	  //cli->capture.cli_appl_ptr = 0;
	  //cli->capture.start_count = 0;
	  cli->capture.started = false;
	}
      dspd_slist_entry_srvunlock(cli->list, cli->index);
    }

  dspd_mutex_unlock(&cli->sync_start_lock);
  return ret;
}


//The "set" arg makes it like SNDCTL_DSP_SETTRIGGER
static int32_t client_start_at_time(struct dspd_client *cli, dspd_time_t tstamp, uint32_t streams, dspd_time_t tslist[2], bool set)
{
  int32_t ret, old;
  struct dspd_client_trigger_tstamp *ts;

  //This lock is almost never held by anything else.  It is necessary because
  //this structure might be accessed by another thread.
  dspd_mutex_lock(&cli->sync_start_lock);
  if ( cli->playback.ready == false )
    streams &= ~DSPD_PCM_SBIT_PLAYBACK;
  if ( cli->capture.ready == false )
    streams &= ~DSPD_PCM_SBIT_CAPTURE;



  old = cli->trigger;

  
  ts = dspd_mbx_acquire_write(cli->sync_start_tstamp);
  if ( ts )
    {
      if ( set )
	cli->trigger = streams;
      else
	cli->trigger |= streams;

      if ( (cli->trigger & DSPD_PCM_SBIT_PLAYBACK) != 0 && (old & DSPD_PCM_SBIT_PLAYBACK) == 0 )
	cli->playback.trigger_tstamp = tstamp;
      if ( (cli->trigger & DSPD_PCM_SBIT_CAPTURE) != 0 && (old & DSPD_PCM_SBIT_CAPTURE) == 0 )
	cli->capture.trigger_tstamp = tstamp;
      ts->streams = cli->trigger;
      ts->playback_tstamp = cli->playback.trigger_tstamp;
      ts->capture_tstamp = cli->capture.trigger_tstamp;
      dspd_mbx_release_write(cli->sync_start_tstamp, ts);
    }
  if ( cli->trigger != old )
    {
      ret = cli->server_ops->trigger(cli->server, (uint32_t)cli->index, cli->trigger);
      if ( ret == 0 && set == true )
	{
	  bool locked;
	  if ( (old & DSPD_PCM_SBIT_PLAYBACK) != 0 && (cli->trigger & DSPD_PCM_SBIT_PLAYBACK) == 0 )
	    {
	      dspd_slist_entry_srvlock(cli->list, cli->index); 
	      locked = true;
	      
	      dspd_fifo_reset(&cli->playback.fifo);
	      dspd_mbx_reset(&cli->playback.mbx);
	      /*cli->playback.dev_appl_ptr = 0;
	      cli->playback.cli_appl_ptr = 0;
	      cli->playback.start_count = 0;*/
	      cli->playback.started = false;
	    } else
	    {
	      locked = false;
	    }
	  if ( (old & DSPD_PCM_SBIT_CAPTURE) != 0 && (cli->trigger & DSPD_PCM_SBIT_CAPTURE) == 0 )
	    {
	      if ( ! locked )
		{
		  dspd_slist_entry_srvlock(cli->list, cli->index); 
		  locked = true;
		}
	      dspd_fifo_reset(&cli->capture.fifo);
	      dspd_mbx_reset(&cli->capture.mbx);
	      /*cli->capture.dev_appl_ptr = 0;
	      cli->capture.cli_appl_ptr = 0;
	      cli->capture.start_count = 0;*/
	      cli->capture.started = false;
	    }
	  if ( locked )
	    dspd_slist_entry_srvunlock(cli->list, cli->index); 
	}
    } else
    {
      ret = 0;
    }
  if ( ret == 0 && tslist != NULL )
    {
      tslist[0] = cli->playback.trigger_tstamp;
      tslist[1] = cli->capture.trigger_tstamp;
    }
  dspd_mutex_unlock(&cli->sync_start_lock);
  return ret;
}

static int32_t client_synccmd(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  int ret = EINVAL;
  uint32_t sbits;
  const struct dspd_sync_cmd *scmd = inbuf;
  dspd_time_t tstamp;
  struct dspd_sync_cmd info;
  if ( cli->device >= 0 )
    {
      if ( scmd->cmd == SGCMD_STARTALL )
	{
	  struct dspd_syncgroup *sg = dspd_sg_get(dspd_dctx.syncgroups, scmd->sgid);
	  if ( sg )
	    {
	      if ( scmd->streams )
		{
		  sbits = scmd->streams;
		  tstamp = dspd_sg_start(sg, &sbits);
		} else
		{
		  sbits = dspd_sg_streams(sg);
		  tstamp = dspd_sg_start(sg, NULL);
		}
	      dspd_sg_put(dspd_dctx.syncgroups, scmd->sgid);
	      memset(&info, 0, sizeof(info));
	      info.tstamp = tstamp;
	      info.streams = sbits;
	      return dspd_req_reply_buf(context, 0, &info, sizeof(info));
	    }
	} else
	{
	  if ( scmd->cmd == SGCMD_START )
	    {
	      ret = client_start_at_time(cli, scmd->tstamp, scmd->streams, NULL, false);
	    } else if ( scmd->cmd == SGCMD_STOP )
	    {
	      ret = client_stop_now(cli->server, scmd->streams);
	    }
	}
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t client_lock(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  int32_t err = EBADFD;
  int32_t flags = *(int32_t*)inbuf;
  struct dspd_lock_result res = { 0 };
  struct dspd_dev_lock_req r = { 0 }, r2 = { 0 };
  size_t br = 0;
  if ( cli->device >= 0 )
    {
      r.client = cli->index;
      r.flags = flags;
      err = dspd_stream_ctl(&dspd_dctx,
			    cli->device,
			    DSPD_SCTL_SERVER_LOCK,
			    &r,
			    sizeof(r),
			    &r2,
			    sizeof(r2),
			    &br);
      if ( err == 0 && br != sizeof(r2) )
	{
	  err = EPROTO;
	} else if ( err == 0 )
	{
	  res.fd = r2.client_fd;
	  res.cookie = r2.cookie;
	  cli->mq_fd = r2.server_fd;
	  return dspd_req_reply_fd(context,
				   0,
				   &res,
				   sizeof(res),
				   res.fd);
	}
    }
  return dspd_req_reply_err(context, 0, err);
}


static int32_t client_swparams(struct dspd_rctx *context,
			       uint32_t      req,
			       const void   *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  struct dspd_rclient_swparams swparams = { 0 };
  int32_t ret;
  if ( inbuf && inbufsize )
    {
      if ( inbufsize > sizeof(swparams) )
	inbufsize = sizeof(swparams);
      memcpy(&swparams, inbuf, sizeof(swparams));
      dspd_store_uint32(&cli->avail_min, swparams.avail_min);
    }
  if ( outbuf && outbufsize )
    {
      swparams.avail_min = dspd_load_uint32(&cli->avail_min);
      if ( outbufsize > sizeof(swparams) )
	outbufsize = sizeof(swparams);
      ret = dspd_req_reply_buf(context, 0, &swparams, outbufsize);
    } else
    {
      ret = dspd_req_reply_err(context, 0, 0);
    }
  return ret;
}

static const struct dspd_req_handler client_req_handlers[] = {
  [CLIDX(DSPD_SCTL_CLIENT_START)] = {
    .handler = client_start,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_STOP)] = {
    .handler = client_stop,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETPARAMS)] = {
    .handler = client_getparams,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_cli_params),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETPARAMS)] = {
    .handler = client_setparams,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_cli_params),
    .outbufsize = sizeof(struct dspd_cli_params),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETVOLUME)] = {
    .handler = client_setvolume,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_stream_volume),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETVOLUME)] = {
    .handler = client_getvolume,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(float),
  },
  [CLIDX(DSPD_SCTL_CLIENT_CONNECT)] = {
    .handler = client_connect,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_DISCONNECT)] = {
    .handler = client_disconnect,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_RAWPARAMS)] = {
    .handler = client_rawparams,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(int64_t), //(stream<<32)|server
    .outbufsize = sizeof(struct dspd_cli_params),
  },
  [CLIDX(DSPD_SCTL_CLIENT_MAPBUF)] = {
    .handler = client_mapbuf,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_client_shm),
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETCHANNELMAP)] = {
    .handler = client_getchannelmap,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_chmap),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETCHANNELMAP)] = {
    .handler = client_setchannelmap,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_chmap),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETCB)] = {
    .handler = client_setcb,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_client_cb),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_STAT)] = {
    .handler = client_stat,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(struct dspd_cli_stat),
  },
  [CLIDX(DSPD_SCTL_CLIENT_RESERVE)] = {
    .handler = client_reserve,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETTRIGGER)] = {
    .handler = client_settrigger,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETTRIGGER)] = {
    .handler = client_gettrigger,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(int32_t),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SYNCGROUP)] = {
    .handler = client_syncgroup,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_SYNCCMD)] = {
    .handler = client_synccmd,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_sync_cmd),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_LOCK)] = {
    .handler = client_lock,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_lock_result),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SWPARAMS)] = {
    .handler = client_swparams,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,

  },
};




static int32_t client_ctl(struct dspd_rctx *rctx,
			  uint32_t             req,
			  const void          *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize)
{
  uint64_t r;
  r = req;
  r <<= 32;
  r |= req - DSPD_SCTL_CLIENT_MIN;
  return dspd_daemon_dispatch_ctl(rctx,
				  client_req_handlers,
				  sizeof(client_req_handlers) / sizeof(client_req_handlers[0]),
				  r,
				  inbuf,
				  inbufsize,
				  outbuf,
				  outbufsize);
}
