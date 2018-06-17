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

#define _DSPD_HAVE_UCRED
#include "socket.h"
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


struct dspd_client {
  struct dspd_client_stream     playback;
  struct dspd_client_stream     capture;
  const struct dspd_client_ops *ops;
  uint32_t                      latency;
  int32_t                       index;
  int32_t                       device;
  bool                          device_reserved;
  struct dspd_slist            *list;
  int                           err;

  
  

  struct dspd_pcm_chmap_container playback_usermap;
  struct dspd_pcm_chmap_container capture_usermap;

  struct dspd_pcm_chmap_container playback_mixmap;
  void (*playback_write)(const struct dspd_pcm_chmap * __restrict map, 
			 const float                 * __restrict inbuf,
			 double                      * __restrict outbuf,
			 size_t                                   frames,
			 double                                   volume);
  struct dspd_pcm_chmap_container capture_mixmap;
  void (*capture_read)(const struct dspd_pcm_chmap * __restrict map, 
		       const float                 * __restrict inbuf,
		       float                       * __restrict outbuf,
		       size_t                                   frames,
		       float                                    volume);

  

  struct dspd_fchmap             playback_cmap;
  struct dspd_fchmap             capture_cmap;
  struct dspd_pcmdev_ops        *server_ops;
  void                          *server;


  struct dspd_fchmap            playback_inmap, capture_inmap;
  

  struct dspd_client_src        playback_src;
  struct dspd_client_src        capture_src;

  void (*error_cb)(void *dev, int32_t index, void *client, int32_t err, void *arg);
  void *error_arg;

  int32_t  trigger;
 

  struct dspd_syncgroup        *syncgroup;

  dspd_mutex_t               sync_start_lock;
  struct dspd_mbx_header    *sync_start_tstamp;
  struct dspd_client_trigger_state trigger_state;
  dspd_time_t                   min_latency;
  int mq_fd;
  volatile uint32_t avail_min;
  bool alloc;

  char name[DSPD_MIX_NAME_MAX];
  bool vctrl_registered;
  int32_t uid;
  int32_t gid;
  int32_t pid;

  dspd_client_change_route_cb_t route_changed_cb;
  void *route_changed_arg;
  bool  dontroute;
};


static int32_t client_stop_now(struct dspd_client *cli, uint32_t streams, bool reset);



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
			  const struct dspd_io_cycle      *cycle,
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
			 const struct dspd_io_cycle      *cycle,
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


static const struct dspd_client_ops client_ops = {
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

  if ( cli->vctrl_registered )
    dspd_daemon_vctrl_unregister(cli->index, cli->index, NULL);

  DSPD_ASSERT(cli->alloc);
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
  cli->alloc = false;
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
  cli->alloc = true;
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
  snprintf(cli->name, sizeof(cli->name), "client #%d", cli->index);
  cli->list = list;
  cli->device = -1;
  cli->mq_fd = -1;
  cli->uid = -1;
  cli->gid = -1;
  cli->pid = -1;
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
      dspd_mutex_lock(&cli->sync_start_lock);
      cli->server_ops = NULL;
      dspd_mutex_unlock(&cli->sync_start_lock);
      
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
	      ret = dspd_src_new(&cli->playback_src.src, cli->playback.params.src_quality, cli->playback_src.channels);
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
      newsize = newcount * MAX(cli->capture.params.channels, cli->capture_mixmap.map.ichan);
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
	      ret = dspd_src_new(&cli->capture_src.src, cli->capture.params.src_quality, cli->capture_src.channels);
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


static int32_t set_matrix(struct dspd_client *cli,
			  struct dspd_client_stream *stream,
			  struct dspd_pcm_chmap *usermap,
			  struct dspd_pcm_chmap *mixmap,
			  const struct dspd_pcm_chmap *map)
{
  int32_t ret = 0;
  size_t br = 0;
  struct dspd_pcm_chmap_container tmp;
  uint32_t f;
  if ( cli->device < 0 )
    {
      //The stream is not connected.  Validate the channel map with known info.
      if ( stream->params.channels )
	{
	  if ( map->flags & DSPD_PCM_SBIT_PLAYBACK )
	    ret = dspd_pcm_chmap_test_channels(map, stream->params.channels, 0);
	  else
	    ret = dspd_pcm_chmap_test_channels(map, 0, stream->params.channels);
	}
      if ( ret == 0 )
	{
	  memmove(usermap, map, dspd_pcm_chmap_sizeof(map->count, map->flags));
	  mixmap->flags = DSPD_CHMAP_SIMPLE;
	  mixmap->ichan = map->count;
	  mixmap->ochan = map->count;
	  mixmap->count = map->count;
	}
      return ret;
    }
  if ( stream->params.channels != 0 )
    {
      //Client channels must match otherwise the xfer size will be wrong.
      if ( ((map->flags & DSPD_PCM_SBIT_PLAYBACK) && map->ichan != stream->params.channels) ||
	   ((map->flags & DSPD_PCM_SBIT_CAPTURE) && map->ochan != stream->params.channels))
	return -EINVAL;
    }

  //Get the full channel map from the device
  f = map->flags & DSPD_PCM_SBIT_FULLDUPLEX;
  ret = dspd_stream_ctl(&dspd_dctx,
			cli->device,
			DSPD_SCTL_SERVER_GETCHANNELMAP,
			&f,
			sizeof(f),
			&tmp,
			sizeof(tmp),
			&br);
  if ( ret == 0 )
    {
      if ( br < sizeof(tmp.map) || br < dspd_pcm_chmap_sizeof(tmp.map.count, tmp.map.flags) )
	{
	  ret = -EPROTO;
	} else
	{
	  ret = dspd_pcm_chmap_test(map, &tmp.map);
	  if ( ret == 0 )
	    {
	      if ( ((map->flags & DSPD_PCM_SBIT_PLAYBACK) && (map->ochan != tmp.map.count)) ||
		   ((map->flags & DSPD_PCM_SBIT_CAPTURE) && (map->ichan != tmp.map.count)) )
		{
		  ret = -EINVAL;
		} else
		{
		  memmove(mixmap, map, dspd_pcm_chmap_sizeof(map->count, map->flags));
		  //The usermap is not valid because the caller only specified a matrix.
		  memset(usermap, 0, sizeof(*usermap));
		}
	    } else if ( ret == 1 )
	    {
	       memmove(mixmap, map, dspd_pcm_chmap_sizeof(map->count, map->flags));
	       //The usermap is not valid because the caller only specified a matrix.
	       memset(usermap, 0, sizeof(*usermap));
	       mixmap->flags &= DSPD_PCM_SBIT_FULLDUPLEX;
	       mixmap->flags |= DSPD_CHMAP_SIMPLE;
	       if ( mixmap->flags & DSPD_PCM_SBIT_PLAYBACK )
		 mixmap->count = mixmap->ichan;
	       else
		 mixmap->count = mixmap->ochan;
	       ret = 0;
	    }
	}
    }
  return ret;
}
static int32_t set_enum(struct dspd_client *cli,
			struct dspd_client_stream *stream,
			struct dspd_pcm_chmap *usermap,
			struct dspd_pcm_chmap *mixmap,
			const struct dspd_pcm_chmap *map)
{
  int32_t ret = 0;
  struct dspd_pcm_chmap_container tmp;
  size_t br = 0;
  if ( cli->device < 0 )
    {
      //The stream is not connected.  Validate the channel map with known info.
      if ( stream->params.channels )
	{
	  if ( map->flags & DSPD_PCM_SBIT_PLAYBACK )
	    ret = dspd_pcm_chmap_test_channels(map, stream->params.channels, 0);
	  else
	    ret = dspd_pcm_chmap_test_channels(map, 0, stream->params.channels);
	}
      if ( ret == 0 )
	{
	  memmove(usermap, map, dspd_pcm_chmap_sizeof(map->count, map->flags));
	  mixmap->flags = DSPD_CHMAP_SIMPLE;
	  mixmap->ichan = map->count;
	  mixmap->ochan = map->count;
	  mixmap->count = map->count;
	}
      return ret;
    }
  ret = dspd_stream_ctl(&dspd_dctx,
			cli->device,
			DSPD_SCTL_SERVER_CONVERT_CHMAP,
			map,
			dspd_pcm_chmap_sizeof(map->count, map->flags),
			&tmp,
			sizeof(tmp),
			&br);

  if ( ret == 0 )
    {
      if ( br < sizeof(tmp.map) || br < dspd_pcm_chmap_sizeof(tmp.map.count, tmp.map.flags) )
	{
	  ret = -EPROTO;
	} else
	{
	  memmove(mixmap, &tmp, br);
	  memmove(usermap, map, dspd_pcm_chmap_sizeof(tmp.map.count, tmp.map.flags));
	}
    }
  return ret;
}


static int32_t set_chmap(struct dspd_client *cli, const struct dspd_pcm_chmap *map)
{
  struct dspd_client_stream *stream;
  struct dspd_pcm_chmap *usermap;
  struct dspd_pcm_chmap *mixmap;
  int32_t ret;
  if ( map->flags & DSPD_PCM_SBIT_PLAYBACK )
    {
      stream = &cli->playback;
      usermap = &cli->playback_usermap.map;
      mixmap = &cli->playback_mixmap.map;
    } else if ( map->flags & DSPD_PCM_SBIT_CAPTURE )
    {
      stream = &cli->capture;
      usermap = &cli->capture_usermap.map;
      mixmap = &cli->capture_mixmap.map;
    } else
    {
      return -EINVAL;
    }
  if ( map->flags & DSPD_CHMAP_MATRIX )
    ret = set_matrix(cli, stream, usermap, mixmap, map);
  else
    ret = set_enum(cli, stream, usermap, mixmap, map);
  return ret;
}

static int32_t refresh_chmap(struct dspd_client *cli, int32_t sbit)
{
  int32_t ret = -EINVAL;
  struct dspd_pcm_chmap_container *result = NULL, *usermap = NULL, *mixmap = NULL, *tmp;
  struct dspd_client_stream *stream = NULL; 
  if ( sbit == DSPD_PCM_SBIT_FULLDUPLEX )
    {
      ret = refresh_chmap(cli, DSPD_PCM_SBIT_PLAYBACK);
      if ( ret == 0 )
	ret = refresh_chmap(cli, DSPD_PCM_SBIT_CAPTURE);
      return ret;
    }
  if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
    {
      stream = &cli->playback;
      usermap = &cli->playback_usermap;
      mixmap = &cli->playback_mixmap;
    } else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
    {
      stream = &cli->capture;
      usermap = &cli->capture_usermap;
      mixmap = &cli->capture_mixmap;
    }
  if ( stream )
    {
      if ( stream->params.channels )
	{
	  if ( usermap->map.count )
	    {
	      ret = set_chmap(cli, &usermap->map);
	    } else if ( mixmap->map.count )
	    {
	      ret = set_chmap(cli, &mixmap->map);
	    }
	  if ( ret < 0 )
	    {
	      uint32_t val;
	      size_t br;	 
	      result = alloca(sizeof(*result));
	      memset(result, 0, sizeof(*result));
	      result->map.count = stream->params.channels;
	      if ( cli->device < 0 )
		{
		  ret = dspd_pcm_chmap_any(NULL, &result->map);
		  if ( ret == 0 )
		    {
		      memcpy(usermap, result, dspd_pcm_chmap_sizeof(result->map.count, result->map.flags));
		      memset(mixmap, 0, sizeof(*mixmap));
		    }
		} else
		{
		  val = sbit;
		  ret = dspd_stream_ctl(&dspd_dctx,
					cli->device,
					DSPD_SCTL_SERVER_GETCHANNELMAP,
					&val,
					sizeof(val),
					result,
					sizeof(*result),
					&br);
		  if ( ret == 0 && (br >= sizeof(result->map) && br >= dspd_pcm_chmap_sizeof(result->map.count, result->map.flags)) )
		    {
		      tmp = alloca(sizeof(*tmp));
		      memset(tmp, 0, sizeof(*tmp));
		      tmp->map.count = stream->params.channels;
		      tmp->map.flags = sbit;
		      ret = dspd_pcm_chmap_any(&result->map, &tmp->map);
		      if ( ret == 0 )
			ret = set_chmap(cli, &tmp->map);
		    } else if ( ret == 0 )
		    {
		      ret = -EPROTO;
		    }
		}
	    }
	} else
	{
	  ret = 0;
	}
    }
  if ( ret == 0 )
    {
      if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
	{
	  if ( mixmap->map.flags & DSPD_CHMAP_SIMPLE )
	    cli->playback_write = dspd_pcm_chmap_write_buf_simple;
	  else if ( mixmap->map.flags & DSPD_CHMAP_MULTI )
	    cli->playback_write = dspd_pcm_chmap_write_buf_multi;
	  else
	    cli->playback_write = dspd_pcm_chmap_write_buf;
	} else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
	{
	  if ( mixmap->map.flags & DSPD_CHMAP_SIMPLE )
	    cli->capture_read = dspd_pcm_chmap_read_buf_simple;
	  else if ( mixmap->map.flags & DSPD_CHMAP_MULTI )
	    cli->capture_read = dspd_pcm_chmap_read_buf_multi;
	  else
	    cli->capture_read = dspd_pcm_chmap_read_buf;
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
				 uint32_t *len,
				 uint32_t *rem)
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
  while ( offset < frames_requested && *rem > 0 )
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
      if ( infr > *rem )
	infr = *rem;
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
      (*rem) -= infr;
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
			  const struct dspd_io_cycle      *cycle,
			  const struct dspd_pcm_status    *status)
{
  struct dspd_client *cli = client;
  uintptr_t offset = 0;
  float *ptr;
  double *out;
  int32_t ret;
  uint32_t count, commit_size;
  struct dspd_pcm_status *cs;
  size_t c, n;
  float volume = dspd_load_float32(&cli->playback.volume);
  uint32_t client_hwptr;
  uint64_t optr = status->appl_ptr;
  uint32_t len, p;
  uint64_t start_count = cycle->start_count;
  uint32_t client_aptr;
  uint32_t rem;
  client_hwptr = dspd_fifo_optr(&cli->playback.fifo);
  client_aptr = dspd_fifo_iptr(&cli->playback.fifo);

  //Frames remaining.  This is the amount that will be read from the client fifo.  It is technically
  //possible to read more due to a race condition, but that could cause inaccurate status updates.
  rem = client_aptr - client_hwptr;

  cli->playback.start_count = start_count;
  cli->playback.dev_appl_ptr = status->appl_ptr;

 
  while ( offset < frames && rem > 0 )
    {
      commit_size = 0;
      if ( cli->playback_src.rate != cli->playback.params.rate )
	{
	  count = frames - offset;
	  ret = playback_src_read(cli,
				  &ptr,
				  &count,
				  &rem);
	
	    
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
	      if ( commit_size > rem )
		commit_size = rem;
	    } else
	    {
	      ret = -EAGAIN;
	    }
	}
      if ( ret == 0 )
	{
	  
	  //Pointer to this block of output
	  out = &buf[cli->playback_mixmap.map.ochan*offset];

	  cli->playback_write(&cli->playback_mixmap.map,
			      ptr,
			      out,
			      count,
			      volume);
	  offset += count;
	} else
	{
	  break;
	  count = 0;
	}
      if ( commit_size )
	{
	  dspd_fifo_rcommit(&cli->playback.fifo, commit_size);
	  rem -= commit_size;
	}
      cli->playback.dev_appl_ptr += count;
    } 
    
  DSPD_ASSERT(offset <= frames);

  if ( dspd_dctx.debug && offset < frames )
    fprintf(stderr, "CLIENT PLAYBACK XRUN: wanted %lu got %lu\n", (long)offset, (long)frames);

  DSPD_ASSERT(cli->playback.dev_appl_ptr == (optr+offset));
  //  if ( status->tstamp == cli->playback.last_hw_tstamp )
  //return;
  
   
  //If this io cycle is not complete and there is data left in the client fifo then don't update the status.
  if ( cycle->remaining > cycle->len && rem > 0 )
    return;



  cs = dspd_mbx_acquire_write(&cli->playback.mbx);
  if ( cs )
    {
      //The client application pointer moved a certain amount.
      len = (client_aptr - client_hwptr) - rem;

      p = client_hwptr + len;
      cli->playback.curr_hw += p - cli->playback.last_hw;
      cli->playback.last_hw = p;

      cs->appl_ptr = cli->playback.curr_hw + (uint64_t)len;
      cs->hw_ptr = cli->playback.curr_hw;
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

	The actual delay is the status delay plus whatever was just added to the device buffer.  This is usually
	equal to the expected latency for the client.

	The cycle length is the size to be fetched from the buffer.  Normally this will be 0, but sometimes a client
	may run out of data.  This can be expected (draining the buffer) or unexpected (xrun).  A nonzero cycle length
	could be compensated for by adjusting the timestamp backwards.  That would normally still be ahead of the
	previous timestamp.  If not, then interpolating with the current monotonic time should fix it.
      */
      len = status->delay + (cli->playback.dev_appl_ptr - status->appl_ptr);
      if ( cli->playback_src.rate != cli->playback.params.rate )
	{
	  cs->delay = dspd_src_get_frame_count(cli->playback_src.rate,
					       cli->playback.params.rate,
					       len);
	  cs->cycle_length = dspd_src_get_frame_count(cli->playback_src.rate, cli->playback.params.rate, cycle->remaining - frames);
	} else
	{
	  cs->delay = len;
	  cs->cycle_length = cycle->remaining - frames;
	}
      
      len = cs->appl_ptr - cs->hw_ptr;
      if ( cs->cycle_length > len )
	cs->cycle_length = len;
      if ( cs->cycle_length > cli->playback.params.latency )
	cs->cycle_length = cli->playback.params.latency;
      

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
	  n = dspd_load_uint32(&cli->avail_min);
	  if ( n > 0 && len >= n )
	    {
	      char c = 0;
	      (void)mq_send(cli->mq_fd, &c, sizeof(c), 0);	    
	    }
	}
    }
   
}





static void playback_set_volume(void *handle, double volume)
{
  struct dspd_client *cli = handle;
  if ( volume > 1.0 )
    volume = 1.0;
  else if ( volume < 0.0 )
    volume = 0.0;
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
  struct dspd_vctrl_reg info;
  if ( cli->err )
    {
      ret = cli->err;
    } else
    {
      dspd_slist_entry_srvlock(cli->list, (uintptr_t)cli->index);
      ret = dspd_stream_setparams(&cli->playback, params);
      if ( ret == 0 && params != NULL )
	{
	  if ( params->latency )
	    cli->latency = params->latency;
	  else
	    cli->latency = params->fragsize;
	}
      dspd_slist_entry_srvunlock(cli->list, (uintptr_t)cli->index);
      if ( ! cli->vctrl_registered )
	{
	  dspd_slist_entry_wrlock(cli->list, cli->index);
	  memset(&info, 0, sizeof(info));
	  info.playback = cli->index;
	  info.capture = -1;
	  info.type = DSPD_VCTRL_CLIENT;
	  info.initval = playback_get_volume(cli);
	  info.displayname = cli->name;
	  dspd_daemon_vctrl_register(&info);
	  cli->vctrl_registered = true;
	  dspd_slist_entry_rw_unlock(cli->list, cli->index);
	}
    }
  return ret;
}

static void capture_set_volume(void *handle, double volume)
{
  struct dspd_client *cli = handle;
  if ( volume > 1.0 )
    volume = 1.0;
  else if ( volume < 0.0 )
    volume = 0.0;
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
	{
	  if ( params->latency )
	    cli->latency = params->latency;
	  else
	    cli->latency = params->fragsize;
	}
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

static uint32_t capture_src_xfer(struct dspd_client *cli, float32 *buf, size_t frames, uint32_t *space)
{
  int32_t ret;
  float32 *ptr;
  uint32_t count;
  size_t c;
  size_t offset = 0;
  size_t fi;
  
  uint32_t ri, ro, q;

  dspd_src_get_params(cli->capture_src.src, &q, &ri, &ro);


  while ( offset < frames && *space > 0 )
    {
      ret = dspd_fifo_wiov(&cli->capture.fifo,
			   (void**)&ptr,
			   &count);
      if ( ret != 0 || count == 0 )
	break;
      fi = frames - offset;
      c = count;
      if ( c > *space )
	c = *space;
      ret = dspd_src_process(cli->capture_src.src, 
			     0,
			     &buf[offset*cli->capture.params.channels],
			     &fi,
			     ptr,
			     &c);

      //fwrite(ptr, c * sizeof(*buf) * cli->capture.params.channels, 1, fp);

      DSPD_ASSERT(c <= count);
      if ( ret != 0 )
	{
	  break;
	}
      dspd_fifo_wcommit(&cli->capture.fifo, c);
      offset += fi;
      (*space) -= c;
    }
  return offset;
}

static void capture_xfer(void                            *dev,
			 void                            *client,
			 const float                     *buf,
			 uintptr_t                        frames,
			 const struct dspd_io_cycle      *cycle,
			 const struct dspd_pcm_status    *status)
{
  struct dspd_client *cli = client;
  uintptr_t offset = 0;
  int32_t ret;
  float *ptr;
  const float *in;
  uint32_t count = 0;
  struct dspd_pcm_status *cs;
  volatile size_t c;
  float volume = dspd_load_float32(&cli->capture.volume);
  uint32_t client_hwptr, client_aptr;
  bool do_src = cli->capture.params.rate != cli->capture_src.rate;
  uint32_t n, fill, space, total;

  client_hwptr = dspd_fifo_iptr(&cli->capture.fifo);
  client_aptr = dspd_fifo_optr(&cli->capture.fifo);
  fill = client_hwptr - client_aptr;
  if ( fill >= cli->capture.params.bufsize )
    return;

  n = cli->capture.params.bufsize - fill;
  space = n;
  total = n;
  //if ( dspd_fifo_space(&cli->capture.fifo, &n) != 0 )
  //return;

  n = dspd_src_get_frame_count(cli->capture.params.rate, cli->capture_src.rate, n);
  
  if ( frames > n )
    frames = n;

  

 
  while ( offset < frames && space > 0 )
    {
      
      if ( do_src )
	{
	  count = cli->capture_src.nsamples / cli->capture_mixmap.map.ichan;
	  ptr = cli->capture_src.buf;
	  ret = 0;
	} else
	{
	  ret = dspd_fifo_wiov(&cli->capture.fifo,
			       (void**)&ptr,
			       &count);
	  if ( count > space )
	    count = space;
	}
      if ( ret == 0 && count > 0 )
	{
	  c = frames - offset;
	  if ( count > c )
	    count = c;
	  c = count * cli->capture_mixmap.map.ichan;

	  //Clear out the memory so the channel map
	  //can be mixed correctly.  I don't think it is likely
	  //that anyone would use a channel map this way but 
	  //it probably won't hurt peformance to bad to allow it.
	  memset(ptr, 0, sizeof(*ptr) * c);

	  
	  
	  in = &buf[cli->capture_mixmap.map.ichan * offset];

	  cli->capture_read(&cli->capture_mixmap.map, 
			    in,
			    ptr,
			    count,
			    volume);
	  offset += count;
	  cli->capture.dev_appl_ptr += count;
	  if ( do_src )
	    {
	      if ( capture_src_xfer(cli, ptr, count, &space) != count )
		break;
	    } else
	    {
	      dspd_fifo_wcommit(&cli->capture.fifo, count);
	      space -= count;
	    }
	} else
	{
	  count = 0;
	  break;
	}
   
    }
    

  if ( status->tstamp == cli->capture.last_hw_tstamp )
    return;


  cs = dspd_mbx_acquire_write(&cli->capture.mbx);
  if ( cs )
    {
      n = total - space;
      client_hwptr += n;
      fill += n;

      n = client_hwptr - cli->capture.last_hw;
      cli->capture.curr_hw += n;
      cli->capture.last_hw = client_hwptr;

      cs->hw_ptr = cli->capture.curr_hw;
      cs->appl_ptr = cli->capture.curr_hw - fill;
      
      cs->tstamp = status->tstamp;
      
      cs->fill = cs->hw_ptr - cs->appl_ptr;
      cs->space = cli->capture.params.bufsize - cs->fill;

      if ( do_src )
	{
	  cs->delay = dspd_src_get_frame_count(cli->capture_src.rate,
					       cli->capture.params.rate,
					       status->delay);
	  cs->cycle_length = dspd_src_get_frame_count(cli->capture_src.rate, 
						      cli->capture.params.rate, 
						      cycle->remaining);
	} else
	{
	  cs->delay = status->delay;
	  cs->cycle_length = cycle->remaining;
	}
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
  err = client_stop_now(cli, stream, true);
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
    }
  if ( err == 0 )
    err = refresh_chmap(cli, DSPD_PCM_SBIT_FULLDUPLEX);



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
    {
      playback_set_volume(cli, sv->volume);
      dspd_daemon_vctrl_set_value(cli->index, DSPD_PCM_SBIT_PLAYBACK, sv->volume, NULL);
    }
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

static int32_t connect_to_device(struct dspd_client *cli, uint32_t idx)
{
  void *server_ops, *client_ops, *data;
  int32_t err = EINVAL, sb;
  size_t br;
  struct dspd_cli_params params;
  uint32_t playback_rate = 0, capture_rate = 0, latency;

  //Reconnecting is ok
  if ( cli->device >= 0 )
    {
      if ( cli->device != idx )
	return EALREADY;
    }
  if ( idx < 0 || idx >= DSPD_MAX_OBJECTS )
    return EINVAL;
  
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
	  if ( cli->device != idx )
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
	  dspd_mutex_lock(&cli->sync_start_lock);
	  cli->server_ops = server_ops;
	  dspd_mutex_unlock(&cli->sync_start_lock);
	  cli->server = data;
	  dspd_stream_setsrc(cli, false);
	  dspd_slist_entry_set_key(cli->list, cli->index, idx);
	  cli->device_reserved = 0;
	}
      if ( err == 0 )
	{
	  if ( cli->playback.params.channels > 0 )
	    {
	      if ( cli->playback.params.channels != cli->playback_usermap.map.count )
		{
		  memset(&cli->playback_usermap, 0, sizeof(cli->playback_usermap));
		  memset(&cli->playback_mixmap, 0, sizeof(cli->playback_mixmap));
		} else if ( cli->playback.params.channels != cli->playback_mixmap.map.ichan )
		{
		  memset(&cli->playback_mixmap, 0, sizeof(cli->playback_mixmap));
		}
	    }
	  if ( cli->capture.params.channels > 0 )
	    {
	      if ( cli->capture.params.channels != cli->capture_usermap.map.count )
		{
		  memset(&cli->capture_usermap, 0, sizeof(cli->capture_usermap));
		  memset(&cli->capture_mixmap, 0, sizeof(cli->capture_mixmap));
		} else if ( cli->capture.params.channels != cli->capture_mixmap.map.ichan )
		{
		  memset(&cli->capture_mixmap, 0, sizeof(cli->capture_mixmap));
		}
	    }
	  err = refresh_chmap(cli, DSPD_PCM_SBIT_FULLDUPLEX);
	}
    }
 
  return err;
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
  int32_t err;
  dspd_slist_entry_wrlock(cli->list, cli->index);
  dspd_slist_entry_srvlock(cli->list, cli->index);
  err = connect_to_device(cli, idx);
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
  int32_t sbits;
  uint64_t delay, d1, d2;
  if ( inbufsize == sizeof(sbits) )
    {
      sbits = *(int32_t*)inbuf;
      client_stop_now(cli, sbits, true);
    }
  dspd_slist_entry_wrlock(cli->list, cli->index);
  ret = dspd_client_release(cli);
  cli->device_reserved = 0;
  cli->min_latency = dspd_get_tick();
  cli->mq_fd = -1;
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  if ( outbufsize == sizeof(delay) )
    {
      if ( cli->playback.ready && cli->playback.params.rate )
	d1 = 1000000000 / cli->playback.params.rate;
      else
	d1 = 0;
      if ( cli->capture.ready && cli->capture.params.rate )
	d2 = 1000000000 / cli->capture.params.rate;
      else
	d2 = 0;
      delay = MAX(d1, d2);
      ret = dspd_req_reply_buf(context, 0, &delay, sizeof(delay));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
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
  const struct dspd_pcm_chmap_container *usermap = NULL, *mixmap = NULL, *map = NULL;
  int32_t stream = *(int32_t*)inbuf;
  struct dspd_client *cli = dspd_req_userdata(context);
  size_t len = 0;
  int32_t ret;
  dspd_client_lock(cli, false);
  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      usermap = &cli->playback_usermap;
      mixmap = &cli->playback_mixmap;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      usermap = &cli->capture_usermap;
      mixmap = &cli->capture_mixmap;
    }
  if ( usermap && mixmap )
    {
      if ( usermap->map.count > 0 )
	{
	  //A enumerated map was specified.  The matrix map is automatically generated.
	  map = usermap;
	  len = dspd_pcm_chmap_sizeof(map->map.count, map->map.flags);
	} else if ( mixmap->map.count > 0 )
	{
	  //Only a matrix map was specified
	  map = mixmap;
	  len = dspd_pcm_chmap_sizeof(map->map.count, map->map.flags);
	}
    }
  dspd_client_unlock(cli);
  if ( map != NULL && len <= outbufsize )
    ret = dspd_req_reply_buf(context, 0, map, len);
  else
    ret = dspd_req_reply_err(context, 0, EINVAL);
  return ret;
}



static int32_t client_setchannelmap(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  const struct dspd_pcm_chmap *map = inbuf;
  struct dspd_client *cli = dspd_req_userdata(context);
  int32_t err;
  if ( dspd_pcm_chmap_sizeof(map->count, map->flags) <= inbufsize )
    {
      dspd_client_lock(cli, true);
      dspd_client_srvlock(cli);
      err = set_chmap(cli, map);
      dspd_client_srvunlock(cli);
      dspd_client_unlock(cli);
    } else
    {
      err = -E2BIG;
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
    case DSPD_CLIENT_CB_CLEAR_ALL:
      cli->error_cb = NULL;
      cli->error_arg = NULL;
      cli->route_changed_cb = NULL;
      cli->route_changed_arg = NULL;
      break;
    case DSPD_CLIENT_CB_ERROR:
      cli->error_cb = cb->callback.error;
      cli->error_arg = cb->arg;
      break;
    case DSPD_CLIENT_CB_ROUTE_CHANGED:
      cli->route_changed_cb = cb->callback.route_changed;
      cli->route_changed_arg = cb->arg;
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
  strlcpy(params->name, cli->name, sizeof(params->name));
  params->pid = cli->pid;
  params->uid = cli->uid;
  params->gid = cli->gid;
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  return dspd_req_reply_buf(context, 0, params, sizeof(*params));
}

static int32_t reserve_device(struct dspd_client *cli, int32_t server)
{
  int32_t ret = dspd_daemon_ref(server, DSPD_DCTL_ENUM_TYPE_SERVER);
  size_t br;
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
	  cli->device = server;
	  cli->device_reserved = 1;
	  dspd_slist_ref(cli->list, cli->index);
	} else
	{
	  dspd_daemon_unref(server);
	}
    }
  return ret;
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
      if ( server == cli->index )
	{
	  ret = -EINVAL; //Can't reserve self
	} else if ( cli->device >= 0 )
	{
	  ret = EALREADY; //Already connected
	} else
	{
	  ret = 0; //Not reserved (this context is available for a reservation)
	}
    } else
    {
      if ( cli->device < 0 )
	{
	  ret = EINVAL;
	} else
	{
	  //This context is already connected or reserved.  Put it into the reserved state.
	  cli->device_reserved = 1;
	  ret = dspd_stream_ctl(&dspd_dctx,
				cli->device,
				DSPD_SCTL_SERVER_RESERVE,
				&cli->index,
				sizeof(cli->index),
				NULL,
				0,
				&br);
	  goto out;
	}
    }
  
  
  if ( ret == 0 )
    ret = reserve_device(cli, server);

 out:
  dspd_slist_entry_srvunlock(cli->list, cli->index);
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
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
  uint32_t val = *(uint32_t*)inbuf;
  int32_t ret;
  dspd_time_t now, result[2];
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

static int32_t client_stop_now(struct dspd_client *cli, uint32_t streams, bool reset)
{
  int32_t ret, old;
  struct dspd_client_trigger_tstamp *ts;
  if ( ! cli->server_ops )
    return -EBADFD;
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
	  if ( reset )
	    {
	      cli->playback.last_hw = 0;
	      cli->playback.curr_hw = 0;
	      cli->playback.dev_appl_ptr = 0;
	      cli->playback.cli_appl_ptr = 0;
	      cli->playback.start_count = 0;
	    }
	  cli->playback.started = false;
	}
      if ( streams & DSPD_PCM_SBIT_CAPTURE )
	{
	  if ( reset )
	    {
	      cli->capture.last_hw = 0;
	      cli->capture.curr_hw = 0;
	      cli->capture.dev_appl_ptr = 0;
	      cli->capture.cli_appl_ptr = 0;
	      cli->capture.start_count = 0;
	    }
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
  if ( ! cli->server_ops )
    return -EBADFD;

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
	      
	      cli->playback.last_hw = 0;
	      cli->playback.curr_hw = 0;
	      
	      cli->playback.dev_appl_ptr = 0;
	      cli->playback.cli_appl_ptr = 0;

	      cli->playback.start_count = 0;

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
	      cli->capture.last_hw = 0;
	      cli->capture.curr_hw = 0;
	  
	      cli->capture.dev_appl_ptr = 0;
	      cli->capture.cli_appl_ptr = 0;
	      cli->capture.start_count = 0;

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
	      ret = client_stop_now(cli->server, scmd->streams, true);
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
      memcpy(&swparams, inbuf, inbufsize);
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

static int32_t client_pause(struct dspd_rctx *context,
			    uint32_t      req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  uint32_t pause = *(const uint32_t*)inbuf;
  int32_t err, ret;
  int32_t s = 0;
  dspd_time_t tstamps[2] = { 0, 0 };
  if ( cli->playback.enabled )
    s |= DSPD_PCM_SBIT_PLAYBACK;
  if ( cli->capture.enabled )
    s |= DSPD_PCM_SBIT_CAPTURE;
  if ( pause )
    err = client_stop_now(cli, s, false);
  else
    err = client_start_at_time(cli, dspd_get_time(), s, tstamps, false);
  if ( err == 0 && outbufsize >= sizeof(tstamps) && pause == 0 )
    ret = dspd_req_reply_buf(context, 0, tstamps, sizeof(tstamps));
  else
    ret = dspd_req_reply_err(context, 0, err);
  return ret;
}

static int32_t client_setinfo(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  int32_t ret;
  const struct dspd_cli_info_pkt *info = inbuf;
  int32_t flags;
  dspd_slist_entry_wrlock(cli->list, cli->index);
  flags = dspd_req_flags(context);
  if ( ((flags & DSPD_REQ_FLAG_CMSG_CRED) != 0 || (flags & DSPD_REQ_FLAG_REMOTE) == 0) &&
       memchr(info->name, 0, sizeof(info->name)) != NULL )
    {
      if ( info->cred.cred.pid >= 0 )
	cli->pid = info->cred.cred.pid;
      if ( info->cred.cred.uid >= 0 )
	cli->uid = info->cred.cred.uid;
      if ( info->cred.cred.gid >= 0 )
	cli->gid = info->cred.cred.gid;
      if ( info->name[0] ) //This buffer is validated with memchr
	snprintf(cli->name, sizeof(cli->name), "%d: %s", cli->index, info->name);
      

      if ( cli->vctrl_registered )
	dspd_daemon_vctrl_set_value(cli->index, DSPD_PCM_SBIT_PLAYBACK, -1, cli->name);
      ret = 0;
    } else
    {
      ret = EPERM;
    }
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  return dspd_req_reply_err(context, 0, ret);
}

#define ENABLE_ROUTING

static int32_t client_change_route(struct dspd_rctx *context,
				   uint32_t      req,
				   const void   *inbuf,
				   size_t        inbufsize,
				   void         *outbuf,
				   size_t        outbufsize)
{
  int32_t ret = EINVAL;
#ifdef ENABLE_ROUTING
  size_t br;
  struct dspd_client_trigger_tstamp *ts;
  struct dspd_client *cli = dspd_req_userdata(context);
  bool restart = false;
  bool reconnect;
  int32_t device = *(int32_t*)inbuf;
  dspd_time_t tslist[2] = { 0, 0 };
  int32_t oldroute;
  dspd_slist_entry_wrlock(cli->list, cli->index);
  dspd_slist_entry_srvlock(cli->list, cli->index);
  if ( cli->playback.ready == false || cli->capture.ready == true || cli->device < 0 || cli->route_changed_cb == NULL || cli->mq_fd >= 0 )
    goto out;
  if ( cli->dontroute )
    {
      ret = EAGAIN;
      goto out;
    }
  if ( device < 0 )
    {
      ret = EIO;
      goto out;
    }

  oldroute = cli->device;
  
  dspd_mutex_lock(&cli->sync_start_lock);
  cli->server_ops = NULL;
  dspd_mutex_unlock(&cli->sync_start_lock);
      
  dspd_slist_entry_set_key(cli->list, cli->index, 0);
  reconnect = ! cli->device_reserved;
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
  cli->device_reserved = 0;
  cli->mq_fd = -1;
  
  if ( ret == 0 && device > 0 )
    {
      dspd_mutex_lock(&cli->sync_start_lock);
      if ( cli->playback.ready && cli->playback.started && (cli->trigger & DSPD_PCM_SBIT_PLAYBACK) )
	{
	  restart = true;
	  cli->trigger &= ~DSPD_PCM_SBIT_PLAYBACK;
	  ts = dspd_mbx_acquire_write(cli->sync_start_tstamp);
	  if ( ts )
	    {
	      ts->streams = cli->trigger;
	      cli->playback.trigger_tstamp = 0;
	      ts->playback_tstamp = 0;
	      dspd_mbx_release_write(cli->sync_start_tstamp, ts);
	    }
	  cli->playback.last_hw = 0;
	  cli->playback.curr_hw = 0;
	  cli->playback.dev_appl_ptr = 0;
	  cli->playback.cli_appl_ptr = 0;
	  cli->playback.start_count = 0;
	  cli->playback.started = false;
	}
      dspd_mutex_unlock(&cli->sync_start_lock);

      if ( reconnect )
	ret = connect_to_device(cli, device);
      else
	ret = reserve_device(cli, device);
      if ( restart == true && ret == 0 )
	ret = client_start_at_time(cli, dspd_get_time(), DSPD_PCM_SBIT_PLAYBACK, tslist, true);
      if ( cli->device != oldroute && cli->route_changed_cb )
	cli->route_changed_cb(cli->device, cli->index, cli, 0, cli->route_changed_arg);
    }


 out:
  if ( ret < 0 && cli->route_changed_cb )
    cli->route_changed_cb(cli->device, cli->index, cli, ret, cli->route_changed_arg);

  dspd_slist_entry_srvunlock(cli->list, cli->index);
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  if ( ret == 0 && restart == true && reconnect == true )
    ret = dspd_req_reply_buf(context, 0, &tslist[DSPD_PCM_STREAM_PLAYBACK], sizeof(tslist[0]));
  else
#endif
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

//Return the connected device with a retain count of +1
static int32_t client_getdev(struct dspd_rctx *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  struct dspd_client *cli = dspd_req_userdata(context);
  int32_t ret = -ENOTCONN, dev = -1;
  bool retain = false;
  dspd_slist_entry_rdlock(cli->list, cli->index);
  if ( cli->device > 0 )
    {
      if ( inbufsize == sizeof(retain) )
	retain = *(const bool*)inbuf;

      if ( (dspd_req_flags(context) & DSPD_REQ_FLAG_REMOTE) && retain == true )
	{
	  retain = *(const bool*)inbuf;
	  if ( retain )
	    {
	      ret = dspd_daemon_ref(cli->device, DSPD_DCTL_ENUM_TYPE_SERVER);
	      if ( ret == 0 )
		dev = cli->device;
	    }
	} else if ( retain == true )
	{
	  //Can't retain for external clients
	  ret = -EINVAL;
	} else
	{
	  dev = cli->device;
	  ret = 0;
	}
    }
  dspd_slist_entry_rw_unlock(cli->list, cli->index);
  if ( ret == 0 )
    ret = dspd_req_reply_buf(context, 0, &dev, sizeof(dev));
  else
    ret = dspd_req_reply_err(context, 0, ret);
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
  [CLIDX(DSPD_SCTL_CLIENT_PAUSE)] = {
    .handler = client_pause,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETINFO)] = {
    .handler = client_setinfo,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_cli_info_pkt),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_CHANGE_ROUTE)] = {
    .handler = client_change_route,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(uint64_t),
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETDEV)] = {
    .handler = client_getdev,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(int32_t),
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
