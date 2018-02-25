/*
 *  RCLIENT - Remote client API: a simple layer for the CLIENT API for
 *            internal and external clients.
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
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <mqueue.h>
#define _DSPD_CTL_MACROS
#include "sslib.h"
#include "daemon.h"
#include "rclient_priv.h"
#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif
#define PLAYBACK_ENABLED(_c) ((_c)->playback.enabled&&(_c)->playback.ready)
#define CAPTURE_ENABLED(_c) ((_c)->capture.enabled&&(_c)->capture.ready)

static void rclient_set_trigger(struct dspd_rclient *cli, int32_t bits)
{
  uint32_t len = 0;
  if ( (bits & DSPD_PCM_SBIT_PLAYBACK) == 0 && (cli->trigger & DSPD_PCM_SBIT_PLAYBACK) != 0 && cli->playback.ready )
    {
      dspd_fifo_len(&cli->playback.fifo, &len);
      if ( len == 0 )
	cli->playback_xfer = 0;
    }
  cli->trigger = bits;
}
static void rclient_clr_trigger(struct dspd_rclient *cli, int32_t bits)
{
  uint32_t len = 0;
  if ( (bits & DSPD_PCM_SBIT_PLAYBACK) && (cli->trigger & DSPD_PCM_SBIT_PLAYBACK) && cli->playback.ready )
    {
      dspd_fifo_len(&cli->playback.fifo, &len);
      if ( len == 0 )
	cli->playback_xfer = 0;
    }
  cli->trigger &= ~bits;
}
static void rclient_add_trigger(struct dspd_rclient *cli, int32_t bits)
{
  cli->trigger |= bits;
}

static void rclient_eventfd_set(struct dspd_rclient *client)
{
  static const uint64_t val = 1;
  if ( ! client->eventfd_set )
    {
      if ( write(client->eventfd, &val, sizeof(val)) == sizeof(val) )
	client->eventfd_set = 1;
    }
}

static void rclient_eventfd_clear(struct dspd_rclient *client)
{
  uint64_t val;
  if ( client->eventfd_set )
    {
      if ( read(client->eventfd, &val, sizeof(val)) == sizeof(val) )
	client->eventfd_set = 0;
    }
}

int32_t dspd_rclient_new(struct dspd_rclient **client, int32_t streams)
{
  struct dspd_rclient *rc = calloc(1, sizeof(struct dspd_rclient));
  int32_t ret = 0;
  if ( rc )
    {
      ret = dspd_rclient_init(rc, streams);
      if ( ret < 0 )
	free(rc);
      else
	*client = rc;
    } else
    {
      ret = -errno;
    }
  return ret;
}

void dspd_rclient_delete(struct dspd_rclient *client)
{
  if ( client )
    {
      dspd_rclient_destroy(client);
      free(client);
    }
}


int32_t dspd_rclient_init(struct dspd_rclient *client, int32_t streams)
{
  if ( ! (streams & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) )
    return -EINVAL;
  memset(client, 0, sizeof(*client));
  client->eventfd = -1;
  client->swparams.avail_min = 1;
  client->bparams.client = -1;
  client->bparams.device = -1;
  client->streams = streams;
  client->mq_fd = -1;
  client->init = true;
  return 0;
}

int32_t dspd_rclient_bind(struct dspd_rclient *client,
			  struct dspd_rclient_bindparams *bparams)
{
  int32_t ret;
  if ( client == NULL || bparams == NULL )
    {
      ret = -EFAULT;
    } else if ( PLAYBACK_ENABLED(client) || CAPTURE_ENABLED(client) )
    {
      ret = -EINVAL;
    } else
    {
      memcpy(&client->bparams, bparams, sizeof(*bparams));
      ret = 0;
    }
  return ret;
}



int32_t dspd_rclient_open_dev(struct dspd_rclient *client, 
			      const char *name, 
			      int32_t stream,
			      struct dspd_device_stat *info)
{
  int32_t err;
  int32_t defaultdev = -1, devidx = -1;
  size_t br;
  uint32_t mask_size;
  char *mask;
  int32_t mtype = DSPD_DCTL_ENUM_TYPE_SERVER;
  ssize_t i, bits;
  uint32_t dev, prev = 0;
  uint64_t d = 0;
  if ( PLAYBACK_ENABLED(client) || CAPTURE_ENABLED(client) || client->bparams.conn == NULL || client->bparams.device > 0 )
    return -EINVAL;



  if ( strcmp(name, "default") == 0 )
    {
      err = dspd_stream_ctl(client->bparams.conn,
			    0,
			    DSPD_DCTL_GET_DEFAULTDEV,
			    &stream,
			    sizeof(stream),
			    &defaultdev,
			    sizeof(defaultdev),
			    &br);
      if ( err )
	return err;
    }
  err = dspd_stream_ctl(client->bparams.conn,
			0,
			DSPD_DCTL_GET_OBJMASK_SIZE,
			NULL,
			0,
			&mask_size,
			sizeof(mask_size),
			&br);
  if ( err )
    return err;
  mask = calloc(1, mask_size);
  if ( ! mask )
    return -ENOMEM;
  
  err = dspd_stream_ctl(client->bparams.conn,
			0,
			DSPD_DCTL_ENUMERATE_OBJECTS,
			&mtype,
			sizeof(mtype),
			mask,
			mask_size,
			&br);
   if ( err )
     goto out;
   bits = br * 8;
   for ( i = bits - 1; i >= 0; i-- )
     {
       if ( ! dspd_test_bit((uint8_t*)mask, i) )
	 continue;
       dev = i;
       d = prev;
       d <<= 32U;
       d |= dev;
       prev = 0;
       //Create a reference, remove any existing references, and get the device information.
       err = dspd_stream_ctl(client->bparams.conn,
			     -1,
			     DSPD_SOCKSRV_REQ_REFSRV,
			     &d,
			     sizeof(d),
			     &client->devinfo,
			     sizeof(client->devinfo),
			     &br);
       if ( err == 0 && br == sizeof(client->devinfo) )
	 {
	   prev = dev;
	   if ( (client->devinfo.streams & stream) == stream )
	     {
	       devidx = i;
	       if ( defaultdev == devidx || (defaultdev < 0 && strcmp(client->devinfo.name, name) == 0) )
		 break;
	     }
	 }
     }
   
   if ( devidx < 0 )
     {
       err = -ENOENT;
       memset(&client->devinfo, 0, sizeof(client->devinfo));
     } else
     {
       err = devidx;
       if ( info )
	 memcpy(info, &client->devinfo, sizeof(*info));
     }
   client->bparams.device = devidx;
   

 out:

   if ( err < 0 )
     {
       if ( devidx > 0 )
	 {
	   dspd_stream_ctl(client->bparams.conn,
			   -1,
			   DSPD_SOCKSRV_REQ_UNREFSRV,
			   &devidx,
			   sizeof(devidx),
			   NULL,
			   0,
			   NULL);
	 } else if ( prev > 0 && prev != devidx )
	 {
	   dspd_stream_ctl(client->bparams.conn,
			   -1,
			   DSPD_SOCKSRV_REQ_UNREFSRV,
			   &prev,
			   sizeof(prev),
			   NULL,
			   0,
			   NULL);
	 }
     }
   free(mask);
   return err;
}

void dspd_rclient_destroy(struct dspd_rclient *client)
{
  size_t br;
  if ( ! client->init )
    return;
  
  if ( client->eventfd >= 0 )
    {
      close(client->eventfd);
      dspd_timer_destroy(&client->timer);
    }

  dspd_rclient_detach(client, DSPD_PCM_SBIT_PLAYBACK);
  dspd_rclient_detach(client, DSPD_PCM_SBIT_CAPTURE);

  if ( client->autoclose && client->bparams.conn )
    {
      uint32_t t = *(int32_t*)client->bparams.conn;
      if ( t == DSPD_OBJ_TYPE_DAEMON_CTX )
	{
	  (void)dspd_stream_ctl(client->bparams.conn,
				-1,
				DSPD_SOCKSRV_REQ_UNREFSRV,
				&client->bparams.device,
				sizeof(client->bparams.device),
				NULL,
				0,
				&br);
	  
	  (void)dspd_stream_ctl(client->bparams.conn,
				-1,
				DSPD_SOCKSRV_REQ_DELCLI,
				&client->bparams.client,
				sizeof(client->bparams.client),
				NULL,
				0,
				&br);
	
	  
	} else
	{
	  dspd_conn_delete(client->bparams.conn);
	  if ( client->mq_fd >= 0 )
	    close(client->mq_fd);
	}
      client->bparams.conn = NULL;
      client->bparams.client = -1;
      client->bparams.device = -1;
      client->mq_fd = -1;
    }
  memset(client, 0, sizeof(*client));
}

int32_t dspd_rclient_set_hw_params(struct dspd_rclient *cli, 
				   const struct dspd_rclient_hwparams *hwp)
{
  int s = 0;
  int err = 0;
  struct dspd_cli_params p;
  if ( ! hwp )
    return -EFAULT;
  if ( hwp->playback_params )
    s |= DSPD_PCM_SBIT_PLAYBACK;
  if ( hwp->capture_params )
    s |= DSPD_PCM_SBIT_CAPTURE;
  if ( s != cli->streams )
    return -EINVAL;
  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
    dspd_rclient_detach(cli, DSPD_PCM_SBIT_PLAYBACK);
  if ( cli->streams & DSPD_PCM_SBIT_CAPTURE )
    dspd_rclient_detach(cli, DSPD_PCM_SBIT_CAPTURE);
  
  if ( s & DSPD_PCM_SBIT_PLAYBACK )
    {
      p = *hwp->playback_params;
      p.stream = DSPD_PCM_SBIT_PLAYBACK;
      p.flags |= DSPD_CLI_FLAG_RESERVED;
      err = dspd_rclient_connect(cli, 
				 &p,
				 hwp->playback_chmap,
				 NULL,
				 hwp->context,
				 hwp->stream,
				 hwp->device);
    }
  if ( err == 0 && (s & DSPD_PCM_SBIT_CAPTURE) )
    {
      p = *hwp->capture_params;
      p.stream = DSPD_PCM_SBIT_CAPTURE;
      p.flags |= DSPD_CLI_FLAG_RESERVED;
      err = dspd_rclient_connect(cli, 
				 &p,
				 NULL,
				 hwp->capture_chmap,
				 hwp->context,
				 hwp->stream,
				 hwp->device);

    }
  if ( err )
    {
      if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	dspd_rclient_detach(cli, DSPD_PCM_SBIT_PLAYBACK);

      if ( cli->streams & DSPD_PCM_SBIT_CAPTURE )
	dspd_rclient_detach(cli, DSPD_PCM_SBIT_CAPTURE);
    }
  return err;
}

int32_t dspd_rclient_connect(struct dspd_rclient *client, 
			     const struct dspd_cli_params *params, //required, may be full duplex
			     const struct dspd_chmap *playback_map, //optional
			     const struct dspd_chmap *capture_map, //optional
			     void *context, //connection or dspd_dctx
			     int32_t stream, //client stream
			     int32_t device)
{
  
  struct dspd_cli_params pp, cp, p;
  int32_t ret;
  size_t br;
  struct dspd_client_shm shm;
  int shm_fd = -1;
  int32_t s;
  struct dspd_rclient_bindparams bp;
  bool preset_params = !params;
  int sbits = 0;

 
  
  if ( context == NULL || stream == -1 || device == -1 )
    {
      context = client->bparams.conn;
      stream = client->bparams.client;
      device = client->bparams.device;
    } else
    {
      client->bparams.conn = context;
      client->bparams.client = stream;
      client->bparams.device = device;
    }
  
  if ( preset_params )
    {
      if ( client->playback.params.channels )
	sbits |= DSPD_PCM_SBIT_PLAYBACK;
      if ( client->capture.params.channels )
	sbits |= DSPD_PCM_SBIT_CAPTURE;

      if ( PLAYBACK_ENABLED(client) )
	{
	  dspd_shm_close(&client->playback.shm);
	  memset(&client->playback.shm, 0, sizeof(client->playback.shm));
	  client->playback.ready = 0;
	}
      if ( CAPTURE_ENABLED(client) )
	{
	  dspd_shm_close(&client->capture.shm);
	  memset(&client->capture.shm, 0, sizeof(client->capture.shm));
	  client->capture.ready = 0;
	}
    } else
    {
      if ( ! (params->flags & DSPD_CLI_FLAG_RESERVED) )
	{
	  if ( PLAYBACK_ENABLED(client) || CAPTURE_ENABLED(client) )
	    return -EBUSY;
	} else if ( ! (params->stream == DSPD_PCM_SBIT_PLAYBACK ||
		       params->stream == DSPD_PCM_SBIT_CAPTURE) )
	{
	  return -EINVAL;
	}

      sbits = params->stream;
      p = *params;
    }
  

  if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( ! preset_params )
	{
	  p.stream = DSPD_PCM_SBIT_PLAYBACK;
	  ret = dspd_rclient_ctl(client,
				 DSPD_SCTL_CLIENT_SETPARAMS,
				 &p,
				 sizeof(p),
				 &pp,
				 sizeof(pp),
				 &br);
	  if ( ret )
	    return ret;
	} else
	{
	  pp = client->playback.params;
	  
	}

	
      if ( playback_map )
	{
	  ret = dspd_rclient_ctl(client,
				 DSPD_SCTL_CLIENT_SETCHANNELMAP,
				 playback_map,
				 dspd_chmap_sizeof(playback_map),
				 NULL,
				 0,
				 &br);
	  if ( ret )
	    return ret;
	}
    }
  if ( sbits & DSPD_PCM_SBIT_CAPTURE )
    {
      if ( ! preset_params )
	{
	  p.stream = DSPD_PCM_SBIT_CAPTURE;
	  ret = dspd_rclient_ctl(client,
				DSPD_SCTL_CLIENT_SETPARAMS,
				&p,
				sizeof(p),
				&cp,
				sizeof(cp),
				&br);
	  if ( ret )
	    return ret;
	} else
	{
	  cp = client->capture.params;
	}
      if ( capture_map )
	{
	  ret = dspd_rclient_ctl(client,
				DSPD_SCTL_CLIENT_SETCHANNELMAP,
				capture_map,
				dspd_chmap_sizeof(capture_map),
				NULL,
				0,
				&br);
	  if ( ret )
	    return ret;
	}
    }

  ret = dspd_rclient_ctl(client,
			DSPD_SCTL_CLIENT_CONNECT,
			&device,
			sizeof(device),
			NULL,
			0,
			&br);

  if ( ret )
    return ret;

  bp.conn = context;
  bp.client = stream;
  bp.device = device;

  if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
    {
      s = DSPD_PCM_SBIT_PLAYBACK;
      ret = dspd_rclient_ctl(client,
			    DSPD_SCTL_CLIENT_MAPBUF,
			    &s,
			    sizeof(s),
			    &shm,
			    sizeof(shm),
			    &br);
      if ( ret )
	return ret;
      if ( br != sizeof(shm) )
	return -EPROTO;
      if ( shm.flags & DSPD_SHM_FLAG_MMAP )
	shm_fd = dspd_conn_recv_fd(context);
      else
	shm_fd = -1;
      ret = dspd_rclient_attach(client, &shm, &pp, &bp);
      if ( ret )
	{

	  close(shm_fd);
	  goto error;
	}

    }

  if ( sbits & DSPD_PCM_SBIT_CAPTURE )
    {
      s = DSPD_PCM_SBIT_CAPTURE;
      ret = dspd_rclient_ctl(client,
			    DSPD_SCTL_CLIENT_MAPBUF,
			    &s,
			    sizeof(s),
			    &shm,
			    sizeof(shm),
			    &br);
      if ( ret )
	goto error;
      if ( br != sizeof(shm) )
	return -EPROTO;
      if ( shm.flags & DSPD_SHM_FLAG_MMAP )
	shm_fd = dspd_conn_recv_fd(context);
      else
	shm_fd = -1;
      
      ret = dspd_rclient_attach(client,
				&shm,
				&cp,
				&bp);
      if ( ret )
	{
	  close(shm_fd);
	  goto error;
	}
    }

  if ( PLAYBACK_ENABLED(client) && client->playback.params.rate )
    dspd_intrp_reset2(&client->playback_intrp, client->playback.params.rate);

  if ( CAPTURE_ENABLED(client) && client->capture.params.rate )
    dspd_intrp_reset2(&client->capture_intrp, client->capture.params.rate);

  return 0;

 error:
  if ( PLAYBACK_ENABLED(client) )
    dspd_rclient_detach(client, DSPD_PCM_SBIT_PLAYBACK);
  if ( CAPTURE_ENABLED(client) )
    dspd_rclient_detach(client, DSPD_PCM_SBIT_CAPTURE);
  return ret;
}

int32_t dspd_rclient_disconnect(struct dspd_rclient *cli, bool reserve)
{
  
  int32_t ret;
  
  if ( reserve )
    {
      ret = dspd_stream_ctl(cli->bparams.conn,
			    cli->bparams.client,
			    DSPD_SCTL_CLIENT_RESERVE,
			    NULL,
			    0,
			    NULL,
			    0,
			    NULL);
    } else
    {
      ret = dspd_stream_ctl(cli->bparams.conn,
			    cli->bparams.client,
			    DSPD_SCTL_CLIENT_DISCONNECT,
			    NULL,
			    0,
			    NULL,
			    0,
			    NULL);
    }
  dspd_rclient_detach(cli, DSPD_PCM_SBIT_PLAYBACK);
  dspd_rclient_detach(cli, DSPD_PCM_SBIT_CAPTURE);
  

  return ret;

}
			     

int32_t dspd_rclient_avail(struct dspd_rclient *client, int32_t stream)
{
  int32_t ret;
  uint32_t avail;
  if ( stream == DSPD_PCM_SBIT_PLAYBACK && PLAYBACK_ENABLED(client) )
    {
      ret = dspd_fifo_get_error(&client->playback.fifo);
      if ( ret == 0 )
	{
	  if ( dspd_fifo_space(&client->playback.fifo, &avail) == 0 )
	    {
	      if ( avail == client->playback.params.bufsize && 
		   dspd_rclient_test_xrun(client, DSPD_PCM_SBIT_PLAYBACK) == true )
		ret = -EPIPE;
	      else
		ret = avail;
	    } else
	    {
	      ret = -EIO;
	    }
	}
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE && CAPTURE_ENABLED(client) )
    {
      ret = dspd_fifo_get_error(&client->capture.fifo);
      if ( ret == 0 )
	{
	  if ( dspd_fifo_len(&client->capture.fifo, &avail) == 0 )
	    {
	      if ( avail == 0 )
		ret = -EPIPE;
	      else
		ret = avail;
	    } else
	    {
	      ret = -EPIPE;
	    }
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}

/*
  Attach a client.  This can be called for playback and capture on the same client.
  The buffer pointed to by the data argument must contain the same values as the
  previous call if dspd_rclient_attach() is called more than once.  In the future
  the device might change for routing but this isn't implemented right now.

*/
int32_t dspd_rclient_attach(struct dspd_rclient *client,
			    const struct dspd_client_shm *cshm,
			    const struct dspd_cli_params *params,
			    const struct dspd_rclient_bindparams     *bparams)
{
  struct dspd_shm_map *map;
  struct dspd_client_shm *shm;
  struct dspd_client_stream *stream;
  int ret = -EINVAL;
  uint64_t p;
  struct dspd_shm_addr addr;
  const struct pcm_conv *conv;
  struct dspd_client_shm tmpshm;
  struct dspd_intrp *intrp;
  int shm_fd = -1;
  size_t br;
  if ( params->stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      map = &client->playback.shm;
      shm = &client->playback_shm;
      stream = &client->playback;
      intrp = &client->playback_intrp;
    } else if ( params->stream == DSPD_PCM_SBIT_CAPTURE )
    {
      map = &client->capture.shm;
      shm = &client->capture_shm;
      stream = &client->capture;
      intrp = &client->capture_intrp;
    } else
    {
      return -EINVAL;
    }
  conv = dspd_getconv(params->format);
  if ( ! conv )
    return -EINVAL;
  client->playback_conv = conv->tofloat32;
  client->capture_conv = conv->fromfloat32;

  

  stream->frame_size = dspd_get_pcm_format_size(params->format) * params->channels;
  

  if ( stream->enabled )
    dspd_rclient_detach(client, params->stream);

  if ( cshm == NULL )
    {
      ret = dspd_stream_ctl(bparams->conn,
			    bparams->client,
			    DSPD_SCTL_CLIENT_MAPBUF,
			    &params->stream,
			    sizeof(params->stream),
			    &tmpshm,
			    sizeof(tmpshm),
			    &br);
      if ( ret < 0 )
	return ret;
      if ( br != sizeof(tmpshm) )
	return -EINVAL;

      

      cshm = &tmpshm;
      if ( cshm->flags & DSPD_SHM_FLAG_MMAP )
	{
	  shm_fd = dspd_conn_recv_fd(bparams->conn);
	  if ( shm_fd < 0 )
	    return shm_fd;
	}
    }
 


  if ( cshm->flags & DSPD_SHM_FLAG_PRIVATE )
    {
      if ( sizeof(uintptr_t) == 8 )
	{
	  p = (uint32_t)cshm->reserved;
	  p <<= 32;
	} else
	{
	  p = 0;
	}
      p |= (uint32_t)cshm->arg;
      map->addr = (struct dspd_shm_header*)(uintptr_t)p;
    } else
    {
      map->arg = cshm->arg;
    }
  map->key = cshm->key;
  map->flags = cshm->flags;
  map->length = cshm->len;
  map->section_count = cshm->section_count;

  ret = dspd_shm_attach(map);
  if ( ret != 0 )
    { 
      ret *= -1;
      return ret;
    }
  assert(map->addr);

  
  //Find sections and set up objects
  memset(&addr, 0, sizeof(addr));
  addr.section_id = DSPD_CLIENT_SECTION_MBX;
  ret = dspd_shm_get_addr(map, &addr);

  if ( ret != 0 )
    {
      ret *= -1;
      goto out;
    }
  if ( addr.length < dspd_mbx_bufsize(sizeof(struct dspd_pcm_status)) )
    {
      ret = -EINVAL;
      goto out;
    }

  ret = dspd_mbx_init(&stream->mbx, sizeof(struct dspd_pcm_status), addr.addr);
  if ( ret )
    {
      ret *= -1;
      goto out;
    }

  memset(&addr, 0, sizeof(addr));
  addr.section_id = DSPD_CLIENT_SECTION_FIFO;
  ret = dspd_shm_get_addr(map, &addr);
  if ( ret != 0 )
    {
      ret *= -1;
      goto out;
    }


  ret = dspd_fifo_init(&stream->fifo,
		       params->bufsize,
		       params->channels * sizeof(float),
		       addr.addr);
  
  if ( ret )
    {
      ret *= -1;
      goto out;
    }
  assert(stream->fifo.data == addr.addr);

  if ( addr.length < dspd_fifo_size(params->bufsize,
				    params->channels * sizeof(float)) )
    {
      ret = -EINVAL;
      goto out;
    }

  stream->enabled = 1;
  stream->ready = 1;
  memcpy(shm, cshm, sizeof(*cshm));
  memmove(&stream->params, params, sizeof(*params));
  stream->sample_time = 1000000000 / stream->params.rate;
  if ( bparams )
    memcpy(&client->bparams, bparams, sizeof(*bparams));
  memset(&stream->status, 0, sizeof(stream->status));
  client->trigger &= ~params->stream;
  dspd_intrp_reset2(intrp, stream->params.rate);
  
 out:
  //If not private and an error occured then close shm.
  if ( ret != 0 && (cshm->flags & DSPD_SHM_FLAG_PRIVATE) == 0 )
    dspd_shm_close(map);
  
 
  return ret;

}


void dspd_rclient_detach(struct dspd_rclient *client, int32_t stream)
{
  struct dspd_client_stream *s;
  struct dspd_client_shm *shm;
  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      s = &client->playback;
      shm = &client->playback_shm;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      s = &client->capture;
      shm = &client->capture_shm;
    } else
    {
      return;
    }
  if ( s->enabled )
    {
      if ( !(shm->flags & DSPD_SHM_FLAG_PRIVATE) )
	dspd_shm_close(&s->shm);
      memset(s, 0, sizeof(*s));
      memset(shm, 0, sizeof(*shm));
      s->enabled = 0;
    }
}

int32_t dspd_rclient_write_rewind(struct dspd_rclient *client, uint32_t len)
{
  dspd_fifo_wcommit(&client->playback.fifo, len * -1);
  return len;
}


int32_t dspd_rclient_get_streams(struct dspd_rclient *client)
{
  int32_t s;
  if ( PLAYBACK_ENABLED(client) )
    s = DSPD_PCM_SBIT_PLAYBACK;
  else
    s = 0;
  if ( CAPTURE_ENABLED(client) )
    s |= DSPD_PCM_SBIT_CAPTURE;
  return s;
}

/*
  This function maintains the increasing pointer value so it wraps and it can be
  used for various forms of tracking.
*/
int32_t dspd_rclient_set_write_ptr(struct dspd_rclient *client, uintptr_t ptr)
{
  uint32_t p, diff, h, len;
  int32_t ret;
  if ( ! PLAYBACK_ENABLED(client) )
    return 0;
  p = dspd_fifo_iptr(&client->playback.fifo);
  diff = (ptr % client->playback.params.bufsize) - (p % client->playback.params.bufsize);
  
  h = dspd_fifo_optr(&client->playback.fifo);
  p += diff;
  len = p - h;

  //It should rewind all the way back to 0 frames even though that will often cause a glitch.
  //In theory the server may rewind and correct it or the client might write the data before
  //the server ever sees it.  Since the integer is unsigned, an underrun normally looks like more frames
  //than the buffer size.
  if ( len > client->playback.params.bufsize )
    {
      ret = -EPIPE;
    } else
    {
      ret = 0;
      if ( diff != 0 )
	dspd_fifo_wcommit(&client->playback.fifo, diff);
    }
  return ret;
}


int32_t dspd_rclient_set_read_ptr(struct dspd_rclient *client, uintptr_t ptr)
{
  uint32_t p, diff, h, len;
  int32_t ret;
  if ( ! CAPTURE_ENABLED(client) )
    return 0;
  p = dspd_fifo_optr(&client->capture.fifo);
  diff = (ptr % client->capture.params.bufsize) - (p % client->capture.params.bufsize);
  
  h = dspd_fifo_iptr(&client->capture.fifo);
  p += diff;
  len = h - p;

  //It should rewind all the way back to 0 frames even though that will often cause a glitch.
  //In theory the server may rewind and correct it or the client might write the data before
  //the server ever sees it.  Since the integer is unsigned, an underrun normally looks like more frames
  //than the buffer size.
  if ( len > client->capture.params.bufsize )
    {
      ret = -EPIPE;
    } else
    {
      ret = 0;
      if ( diff != 0 )
	dspd_fifo_rcommit(&client->capture.fifo, diff);
    }
  return ret;
}



int32_t dspd_rclient_get_hw_ptr(struct dspd_rclient *client, int32_t stream, uint32_t *ptr)
{
  int32_t ret = 0;
  uint32_t i, o, f;
  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( client->playback.ready && client->playback.enabled )
	{
	  o = dspd_fifo_optr(&client->playback.fifo);
	  i = dspd_fifo_iptr(&client->playback.fifo);
	  f = i - o;
	  *ptr = o;
	  if ( (( f == 0 && (client->trigger & DSPD_PCM_SBIT_PLAYBACK) && i > 0)) ||
	       (f > client->playback.params.bufsize) )
	    {
	      if ( dspd_rclient_test_xrun(client, DSPD_PCM_SBIT_PLAYBACK) )
		ret = -EPIPE;
	    }
	} else
	{
	  ret = -EBADF;
	}
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      if ( client->capture.ready && client->capture.enabled )
	{
	  i = dspd_fifo_iptr(&client->capture.fifo);
	  o = dspd_fifo_optr(&client->capture.fifo);
	  f = i - o;
	  if ( f >= client->capture.params.bufsize )
	    ret = -EPIPE;
	} else
	{
	  ret = -EBADF;
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}



static int32_t rclient_write(struct dspd_rclient *client, 
			     const void          *buf,
			     uint32_t             length)
{
  int ret;
  float *ptr;
  uint32_t offset;
  ret = dspd_fifo_wiov_ex(&client->playback.fifo, 
			  (void**)&ptr,
			  &offset,
			  &length);
  if ( ret == 0 )
    {
      if ( buf )
	{
	  client->playback_conv(buf, 
				&ptr[offset*client->playback.params.channels],
				client->playback.params.channels*length);
	} else
	{
	  memset(&ptr[offset*client->playback.params.channels], 
		 0, 
		 sizeof(*ptr) * client->playback.params.channels * length);
	}

      dspd_fifo_wcommit(&client->playback.fifo, length);
      ret = length;
    } else
    {
      ret *= -1;
    }
  return ret;
}


int32_t dspd_rclient_write(struct dspd_rclient *client, 
			   const void          *buf,
			   uint32_t             length)
{
  uint32_t offset = 0;
  int32_t ret = 0;
  bool notify;
  if ( length > INT32_MAX )
    length = INT32_MAX; //Should not happen
  if ( client->mq_fd >= 0 )
    notify = dspd_rclient_avail(client, DSPD_PCM_SBIT_PLAYBACK) == client->playback.params.bufsize;
  else
    notify = false;

  while ( offset < length )
    {
      ret = rclient_write(client, 
			  (const char*)buf + (client->playback.frame_size * offset),
			  length - offset);
      if ( ret <= 0 )
	break;
      offset += ret;
    }
  if ( offset > 0 )
    {
      client->playback_xfer = true;
      ret = offset;
      if ( notify )
	(void)mq_send(client->mq_fd, 
		      (const char*)&client->notification, 
		      sizeof(client->notification),
		      0);
    }
  return ret;
}

static int32_t rclient_read(struct dspd_rclient *client, 
			    void                *buf,
			    uint32_t             length)
{
  int ret;
  const float *ptr;
  uint32_t offset;
  ret = dspd_fifo_riov_ex(&client->capture.fifo, 
			  (void**)&ptr,
			  &offset,
			  &length);

  if ( ret == 0 )
    {
      if ( buf )
	client->capture_conv(&ptr[offset*client->capture.params.channels],
			     buf,
			     client->capture.params.channels*length);
      dspd_fifo_rcommit(&client->capture.fifo, length);
      ret = length;
    } else
    {
      ret *= -1;
    }
  return ret;
}


int32_t dspd_rclient_read(struct dspd_rclient *client, 
			  void                *buf,
			  uint32_t             length)
{
  uint32_t offset = 0;
  int32_t ret = 0;
  if ( length > INT32_MAX )
    length = INT32_MAX; //Should not happen
  while ( offset < length )
    {
      ret = rclient_read(client, 
			 (char*)buf + (client->playback.frame_size * offset),
			 length - offset);
      if ( ret <= 0 )
	break;
      offset += ret;
    }
  if ( offset > 0 )
    ret = offset;
  return ret;
}

int32_t dspd_rclient_get_error(struct dspd_rclient *client, int32_t stream)
{
  int32_t ret;
  struct dspd_client_stream *cs;

  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    cs = &client->playback;
  else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    cs = &client->capture;
  else
    return -EINVAL;
  if ( cs->enabled && cs->ready )
    ret = dspd_fifo_get_error(&cs->fifo);
  else
    ret = -EBADF;
  return ret;
}

static int32_t dspd_rclient_status_ex(struct dspd_rclient *client, int32_t stream, struct dspd_pcmcli_status *status, bool hwsync)
{
  int32_t ret;
  struct dspd_pcm_status *s;
  struct dspd_client_stream *cs;
  struct dspd_intrp *intrp;
  uint32_t len;
  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      cs = &client->playback;
      intrp = &client->playback_intrp;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      cs = &client->capture;
      intrp = &client->capture_intrp;
    } else
    {
      return -EINVAL;
    }

  if ( cs->enabled )
    {
      if ( cs->ready )
	{
	  ret = dspd_fifo_get_error(&cs->fifo);
	  if ( ret == 0 )
	    {
	      if ( ! hwsync )
		goto have_status;
	      s = dspd_mbx_acquire_read(&cs->mbx, 1);
	      if ( s )
		{
		  dspd_intrp_set(intrp, s->tstamp, s->hw_ptr - cs->status.hw_ptr);
		  cs->status.appl_ptr = s->appl_ptr;
		  cs->status.hw_ptr = s->hw_ptr;
		  cs->status.tstamp = s->tstamp;
		  cs->status.error = s->error;
		  cs->status.fill = s->fill;
		  cs->status.space = s->space;
		  cs->status.delay = s->delay;

		  dspd_mbx_release_read(&cs->mbx, s);
		have_status:
		  if ( status )
		    {
		      uint32_t appl;
		      status->appl_ptr = cs->status.appl_ptr;
		      status->hw_ptr = cs->status.hw_ptr;
		      status->tstamp = cs->status.tstamp;
		      if ( hwsync )
			status->delay_tstamp = dspd_get_time();
		      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
			{
			  status->avail = cs->params.bufsize - cs->status.fill;
			  appl = dspd_fifo_iptr(&cs->fifo);
			} else
			{
			  status->avail = cs->status.fill;
			  appl = dspd_fifo_optr(&cs->fifo);
			}

		      if ( dspd_fifo_len(&cs->fifo, &len) != 0 )
			len = 0;

		      uint64_t diff = status->delay_tstamp - status->tstamp;
		      uint32_t f = diff / cs->sample_time;
		      uint32_t delay_ptr = cs->status.hw_ptr + f;

		      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
			status->delay = (appl - delay_ptr) + cs->status.delay;
		      else
			status->delay = (delay_ptr - appl) + cs->status.delay;



		      status->error = cs->status.error;
		      status->reserved = 0;

		      status->trigger_tstamp = cs->trigger_tstamp;
		    }
		} else
		{
		  ret = -EAGAIN;
		}
	    }
	} else
	{
	  ret = -EBADF;
	}
    } else
    {
      ret = -EBADF;
    }
  return ret;
}

int32_t dspd_rclient_status(struct dspd_rclient *client, int32_t stream, struct dspd_pcmcli_status *status)
{
  return dspd_rclient_status_ex(client, stream, status, true);
}

int32_t dspd_rclient_fast_status(struct dspd_rclient *client, int32_t stream, struct dspd_pcmcli_status *status)
{
  return dspd_rclient_status_ex(client, stream, status, false);
}

static int32_t dspd_rclient_wait_fd(struct dspd_rclient *client, dspd_time_t abstime)
{
  int ret;
  struct pollfd pfd[2];
  pfd[0].fd = client->eventfd;
  pfd[0].events = POLLIN;
  pfd[0].revents = 0;
  ret = dspd_timer_getpollfd(&client->timer, &pfd[1]);
  if ( ret == 0 )
    {
      dspd_timer_set(&client->timer, abstime, 0);

      ret = poll(&pfd[0], 2, -1);
      if ( ret < 0 )
	{
	  ret = -errno;
	} else
	{
	  if ( PLAYBACK_ENABLED(client) )
	    if ( dspd_fifo_get_error(&client->playback.fifo) )
	      ret = -EIO;
	  if ( ret >= 0 && CAPTURE_ENABLED(client) )
	    if ( dspd_fifo_get_error(&client->capture.fifo) )
	      ret = -EIO;
	}
    } else
    {
      ret *= -1;
    }
  return ret;
}

static int32_t dspd_rclient_wait_nofd(struct dspd_rclient *client, dspd_time_t abstime)
{
  uint64_t waketime;
  int ret = dspd_sleep(abstime, &waketime);
  if ( ret == EINTR || ret == 0 )
    {
      if ( PLAYBACK_ENABLED(client) )
	if ( dspd_fifo_get_error(&client->playback.fifo) )
	  ret = -EIO;
      if ( ret >= 0 && CAPTURE_ENABLED(client) )
	if ( dspd_fifo_get_error(&client->capture.fifo) )
	  ret = -EIO;
    } else if ( ret != 0 )
    {
      ret *= -1;
    }
  
  return ret;
}

int32_t dspd_rclient_get_next_wakeup(struct dspd_rclient *client, 
				     int32_t sbits, 
				     dspd_time_t *waketime)
{
  return dspd_rclient_get_next_wakeup_avail(client, 
					    sbits,
					    client->swparams.avail_min,
					    waketime);
}

int32_t dspd_rclient_get_next_wakeup_avail(struct dspd_rclient *client, 
					   int32_t sbits, 
					   uint32_t avail_min,
					   dspd_time_t *waketime)
{
  int32_t ret;
  dspd_time_t abstime = UINT64_MAX, a;
  uint32_t appl, hw, len, p, avail, diff, am, n, fr;
  if ( ! (sbits & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)))
    return -EINVAL;
  if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( client->playback.enabled && client->playback.ready )
	{
	  if ( client->trigger & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      ret = dspd_fifo_get_error(&client->playback.fifo);
	      if ( ret != 0 )
		return ret;
	      ret = dspd_fifo_len_ptrs(&client->playback.fifo, 
				       &len,
				       &appl,
				       &hw);
	      if ( ret )
		return -ret;
	      if ( len > client->playback.params.bufsize )
		return -EPIPE;
	      if ( client->playback.params.latency == 0 )
		return -EINVAL;
	      avail = client->playback.params.bufsize - len;

	      am = avail_min;
	      if ( am > client->playback.params.bufsize )
		am = client->playback.params.bufsize;
	      if ( am == 0 )
		{
		  am = client->playback.params.latency;
		} else
		{
		  n = am / client->playback.params.latency;
		  if ( am % client->playback.params.latency )
		    n++;
		  am = n * client->playback.params.latency;
		}
	      

	      client->wakeup_streams |= DSPD_PCM_SBIT_PLAYBACK;

	      if ( avail < am )
		{
		  p = am - avail;
		  
		  hw += p;
		  diff = hw - client->playback.status.hw_ptr;

		  n = diff / client->playback.params.latency;
		  if ( diff % client->playback.params.latency )
		    n++;
		  diff = n * client->playback.params.latency;
		  
		  fr = dspd_intrp_frames(&client->playback_intrp, diff);
		  if ( fr < diff )
		    diff = fr;
		  n = client->playback.params.bufsize / client->playback.params.fragsize;
		  if ( n < 3 || client->playback.status.tstamp == 0 )
		    diff /= 2;

		  abstime = diff;
		  abstime *= client->playback.sample_time;
		  
		  if ( client->playback.status.tstamp )
		    abstime += client->playback.status.tstamp;
		  else
		    abstime += client->playback.trigger_tstamp;
		  client->playback_next_wakeup = abstime;
		} else
		{
		  client->playback_next_wakeup = 0;
		}
	    }
	}
    }
  if ( (sbits & DSPD_PCM_SBIT_CAPTURE) && abstime && (client->trigger & DSPD_PCM_SBIT_CAPTURE))
    {
      if ( client->capture.enabled && client->capture.ready )
	{
	  ret = dspd_fifo_get_error(&client->capture.fifo);
	  if ( ret != 0 )
	    return ret;

	  ret = dspd_fifo_len_ptrs(&client->capture.fifo, 
				   &len,
				   &hw,
				   &appl);
	  if ( ret )
	    return -ret;
	  if ( len > client->capture.params.bufsize )
	    return -EPIPE;
	  if ( client->capture.params.latency == 0 )
	    return -EINVAL;
	  avail = len;
	  am = avail_min;
	  if ( am > client->capture.params.bufsize )
	    am = client->capture.params.bufsize;
	  
	  if ( am == 0 )
	    {
	      am = client->capture.params.latency;
	    } else
	    {
	      n = am / client->capture.params.latency;
	      if ( am % client->capture.params.latency )
		n++;
	      am = n * client->capture.params.latency;
	    }

	  client->wakeup_streams |= DSPD_PCM_SBIT_CAPTURE;
	  if ( avail < am )
	    {
	      p = am - avail;
	      hw += p;
	      diff = hw - client->capture.status.hw_ptr;

	      
	      a = diff;
	      a *= client->capture.sample_time;
	      
	      if ( client->capture.status.tstamp )
		a += client->capture.status.tstamp;
	      else
		a += client->capture.trigger_tstamp;
	      if ( a < abstime )
		abstime = a;
	      client->capture_next_wakeup = abstime;
	    } else
	    {
	      *waketime = 0;
	      client->capture_next_wakeup = 0;
	    }
	}
    }
  if ( abstime == UINT64_MAX )
    abstime = 0;
  *waketime = abstime;
  return 0;
}

int32_t dspd_rclient_wait(struct dspd_rclient *client, int32_t sbits)
{
  dspd_time_t t;
  int32_t ret;
  uint32_t l;
  if ( PLAYBACK_ENABLED(client) )
    l = client->playback.params.latency;
  else if ( CAPTURE_ENABLED(client) )
    l = client->capture.params.latency;
  else
    l = 0;
  if ( l )
    ret = dspd_rclient_get_next_wakeup_avail(client, sbits, l, &t);
  else
    ret = dspd_rclient_get_next_wakeup(client, sbits, &t);
  if ( ret == 0 )
    {
      if ( client->eventfd < 0 )
	ret = dspd_rclient_wait_nofd(client, t);
      else
	ret = dspd_rclient_wait_fd(client, t);
    }
  return ret;
}


int32_t dspd_rclient_pollfd(struct dspd_rclient *client, uint32_t count, struct pollfd *pfd)
{
  int32_t ret = 0, r, fd;
  if ( client->eventfd >= 0 )
    {
      if ( count > 0 )
	{
	  pfd[0].events = POLLIN;
	  pfd[0].revents = 0;
	  pfd[0].fd = client->eventfd;
	  ret = 1;
	}
      if ( count > 1 )
	{
	  r = dspd_timer_getpollfd(&client->timer, &pfd[ret]);
	  if ( r != 0 )
	    ret = r * -1;
	  else
	    ret++;
	}
      if ( count > 2 )
	{
	  fd = dspd_ctx_get_fd(client->bparams.conn);
	  if ( fd >= 0 )
	    {
	      pfd[ret].events = POLLIN | POLLRDHUP;
	      pfd[ret].revents = 0;
	      pfd[ret].fd = fd;
	      ret++;
	    }
	}
      if ( count > 3 && client->mq_fd >= 0 )
	{
	  pfd[ret].events = POLLIN;
	  pfd[ret].revents = 0;
	  pfd[ret].fd = client->mq_fd;
	  ret++;
	}
    }
  return ret;
}

int32_t dspd_rclient_pollfd_count(struct dspd_rclient *client)
{
  int ret;
  if ( client->eventfd < 0 )
    return 0;
  if ( dspd_ctx_get_fd(client->bparams.conn) >= 0 )
    ret = 3;
  else
    ret = 2;
  if ( client->mq_fd >= 0 )
    ret++;
  return ret;
}

int32_t dspd_rclient_enable_pollfd(struct dspd_rclient *client, bool enable)
{
  int32_t ret = 0;
  if ( enable != 0 && client->eventfd < 0 )
    {
      ret = dspd_timer_init(&client->timer);
      if ( ret == 0 )
	{
	  client->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	  if ( client->eventfd < 0 )
	    {
	      ret = -errno;
	      dspd_timer_destroy(&client->timer);
	    }
	}
    } else if ( enable == 0 && client->eventfd >= 0 )
    {
      dspd_timer_destroy(&client->timer);
      close(client->eventfd);
      client->eventfd = -1;
    }
  return ret;
}

int32_t dspd_poll_ack(struct dspd_rclient *client)
{
  rclient_eventfd_clear(client);
  return 0;
}

static uint32_t rclient_fragtime(struct dspd_rclient *client)
{
  uint32_t st, ft;
  if ( client->capture.enabled )
    {
      st = client->capture.sample_time;
      ft = client->capture.params.fragsize;
    } else if ( client->playback.enabled )
    {
      st = client->playback.sample_time;
      ft = client->playback.params.fragsize;
    } else
    {
      st = 0;
      ft = 0;
    }
  return ft * st;
}

int32_t dspd_update_timer(struct dspd_rclient *client, int streams)
{
  int32_t ret;
  dspd_time_t time = 0;
  if ( client->eventfd < 0 )
    return 0;
    
  ret = dspd_rclient_get_next_wakeup(client, streams, &time);

  if ( ret == 0 )
    {
      if ( time == 0 )
	{
	  rclient_eventfd_set(client);
	} else if ( time == UINT64_MAX )
	{
	  ret = dspd_timer_set(&client->timer, 0, 0);
	} else
	{
	  ret = dspd_timer_set(&client->timer, time, rclient_fragtime(client));
	}
    }
  return ret;
}

int32_t dspd_force_poll_events(struct dspd_rclient *client, uint32_t events)
{
  client->forced_events = events;
  if ( events )
    rclient_eventfd_set(client);
  return 0;
}



//Set the file descriptors if something needs attention.  Does not make
//syscalls if it is already set.
void dspd_rclient_poll_notify(struct dspd_rclient *client, uint32_t sbits)
{
  client->stream_poll_events |= sbits;
  if ( client->stream_poll_events )
    rclient_eventfd_set(client);
}

//Clear the file descriptors if there is nothing to do.
void dspd_rclient_poll_clear(struct dspd_rclient *client, uint32_t sbits)
{
  client->stream_poll_events &= ~sbits;
  if ( ! client->stream_poll_events )
    rclient_eventfd_clear(client);
}

/*
  Need to check what events are possible and reset the file descriptors only if nothing is
  available and they are not set.
*/
int32_t dspd_rclient_poll_revents(struct dspd_rclient *client, struct pollfd *pfd, int32_t count)
{
  int32_t revents = 0;
  int32_t i;
  struct pollfd *p;
  int32_t ret;
  uint32_t avail;

  unsigned u;
  int en = 0, fd = dspd_ctx_get_fd(client->bparams.conn);
  char *tmp;
  tmp = alloca(client->mq_msgsize);
  if ( client->stream_poll_events & DSPD_PCM_SBIT_PLAYBACK )
    revents |= POLLOUT;
  if ( client->stream_poll_events & DSPD_PCM_SBIT_CAPTURE )
    revents |= POLLIN;

  if ( PLAYBACK_ENABLED(client) )
    {
      ret = dspd_fifo_space(&client->playback.fifo, &avail);
      if ( ret )
	{
	  revents |= POLLERR | POLLOUT;
	} else if ( avail >= client->swparams.avail_min )
	{
	  revents |= POLLOUT;
	}
      if ( dspd_fifo_get_error(&client->playback.fifo) )
	revents |= POLLERR | POLLOUT;
      en = 1;
    }
  if ( CAPTURE_ENABLED(client) )
    {
      ret = dspd_fifo_len(&client->capture.fifo, &avail);
      if ( ret )
	{
	  //Reading now will not block but it won't get you any data either.
	  revents |= POLLERR | POLLIN;
	} else
	{
	  revents |= POLLIN;
	}
      if ( dspd_fifo_get_error(&client->capture.fifo) )
	revents |= POLLERR | POLLIN;
      en = 1;
    }
  if ( ! en )
    revents = POLLNVAL;

  //Check the file descriptors.  If a buggy program closed them
  //then that is an error condition.
  for ( i = 0; i < count; i++ )
    {
      p = &pfd[i];
      if ( p->fd == client->eventfd )
	{
	  if ( revents == 0 )
	    rclient_eventfd_clear(client);
	} else if ( p->fd == client->timer.fd )
	{
	  if ( p->revents & (POLLNVAL|POLLERR|POLLHUP) )
	    revents |= p->revents & (~POLLIN);
	  //Reset the timer if there is nothing to do
	  if ( revents == 0 )
	    {
	      uint64_t val;
	      read(client->timer.fd, &val, sizeof(val));
	    }
	} else if ( p->fd == fd )
	{
	  if ( p->revents & (POLLNVAL|POLLERR|POLLRDHUP) )
	    revents |= POLLERR;
	  else if ( p->revents & POLLIN )
	    revents |= POLLMSG;
	} else if ( client->mq_fd >= 0 && p->fd == client->mq_fd )
	{
	  
	  if ( p->revents & POLLIN )
	    {
	      if ( mq_receive(client->mq_fd, tmp, client->mq_msgsize, &u) < 0 && errno != EAGAIN )
		revents |= POLLHUP;
	    }

	}
    }
  //Make sure the poll event is enabled if some io is possible
  if ( revents != 0 )
    rclient_eventfd_set(client);
  if ( revents & POLLERR )
    revents |= POLLHUP;
  revents |= client->forced_events;
  return revents;
}


static int32_t rclient_start(struct dspd_rclient *client,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize,
			     size_t       *bytes_returned)
{
  dspd_time_t tstamps[2];
  int32_t ret;
  size_t br;
  *bytes_returned = 0;
  ret = dspd_stream_ctl(client->bparams.conn,
			client->bparams.client,
			req,
			inbuf,
			inbufsize,
			tstamps,
			sizeof(tstamps),
			&br);
  if ( ret == 0 && br == sizeof(tstamps) )
    {
      if ( outbufsize == br )
	{
	  *bytes_returned = br;
	  memcpy(outbuf, tstamps, sizeof(tstamps));
	}
      client->playback.trigger_tstamp = tstamps[DSPD_PCM_STREAM_PLAYBACK];
      if ( client->playback.trigger_tstamp )
	dspd_intrp_set(&client->playback_intrp, client->playback.trigger_tstamp, 0);
      client->capture.trigger_tstamp = tstamps[DSPD_PCM_STREAM_CAPTURE];
      if ( client->capture.trigger_tstamp )
	dspd_intrp_set(&client->capture_intrp, client->capture.trigger_tstamp, 0);
      rclient_add_trigger(client, *(uint32_t*)inbuf);
      client->playback.ready = !! ( client->trigger & DSPD_PCM_SBIT_PLAYBACK );
      client->capture.ready = !! ( client->trigger & DSPD_PCM_SBIT_CAPTURE );
      dspd_update_timer(client, DSPD_PCM_SBIT_PLAYBACK | DSPD_PCM_SBIT_CAPTURE);
    }
  return ret;
}
static int32_t rclient_stop(struct dspd_rclient *client,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize,
			     size_t       *bytes_returned)
{
  int32_t ret;
  size_t br;
  int32_t bits = *(int32_t*)inbuf;
  *bytes_returned = 0;
  ret = dspd_stream_ctl(client->bparams.conn,
			client->bparams.client,
			req,
			inbuf,
			inbufsize,
			NULL,
			0,
			&br);
  if ( ret == 0 )
    {
      *bytes_returned = br;
      rclient_clr_trigger(client, bits);
      if ( bits & DSPD_PCM_SBIT_PLAYBACK )
	{
	  client->playback.trigger_tstamp = 0;
	  dspd_intrp_reset(&client->playback_intrp);
	  dspd_fifo_reset(&client->playback.fifo);
	  dspd_mbx_reset(&client->playback.mbx);
	}
      if ( bits & DSPD_PCM_SBIT_CAPTURE )
	{
	  client->capture.trigger_tstamp = 0;
	  dspd_intrp_reset(&client->capture_intrp);
	  dspd_fifo_reset(&client->capture.fifo);
	  dspd_mbx_reset(&client->capture.mbx);
	}
      if ( client->trigger == 0 )
	dspd_timer_reset(&client->timer);
      dspd_update_timer(client, DSPD_PCM_SBIT_PLAYBACK | DSPD_PCM_SBIT_CAPTURE);
    }
  return ret;
}
static int32_t rclient_settrigger(struct dspd_rclient *client,
				  uint32_t      req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize,
				  size_t       *bytes_returned)
{
  dspd_time_t tstamps[2];
  int32_t ret;
  size_t br;
  int32_t sbits = *(int32_t*)inbuf;
  *bytes_returned = 0;
  ret = dspd_stream_ctl(client->bparams.conn,
			client->bparams.client,
			req,
			inbuf,
			inbufsize,
			tstamps,
			sizeof(tstamps),
			&br);
  if ( ret == 0 && br == sizeof(tstamps) )
    {
      if ( outbufsize == br )
	{
	  *bytes_returned = br;
	  memcpy(outbuf, tstamps, sizeof(tstamps));
	}
      if ( (client->trigger & DSPD_PCM_SBIT_PLAYBACK) != 0 && (sbits & DSPD_PCM_SBIT_PLAYBACK) == 0 )
	{
	  dspd_fifo_reset(&client->playback.fifo);
	  dspd_mbx_reset(&client->playback.mbx);
	}
      if ( (client->trigger & DSPD_PCM_SBIT_CAPTURE) != 0 && (sbits & DSPD_PCM_SBIT_CAPTURE) == 0 )
	{
	  dspd_fifo_reset(&client->capture.fifo);
	  dspd_mbx_reset(&client->capture.mbx);
	}

      client->playback.trigger_tstamp = tstamps[DSPD_PCM_STREAM_PLAYBACK];
      if ( client->playback.trigger_tstamp )
	dspd_intrp_set(&client->playback_intrp, client->playback.trigger_tstamp, 0);
      client->capture.trigger_tstamp = tstamps[DSPD_PCM_STREAM_CAPTURE];
      if ( client->capture.trigger_tstamp )
	dspd_intrp_set(&client->capture_intrp, client->capture.trigger_tstamp, 0);
      rclient_set_trigger(client, sbits);
      client->playback.ready = !! ( client->trigger & DSPD_PCM_SBIT_PLAYBACK );
      client->capture.ready = !! ( client->trigger & DSPD_PCM_SBIT_CAPTURE );
      dspd_update_timer(client, DSPD_PCM_SBIT_PLAYBACK | DSPD_PCM_SBIT_CAPTURE);
    }
  return ret;
}

static int32_t rclient_setparams(struct dspd_rclient *client,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize,
				 size_t       *bytes_returned)
{
  int32_t ret;
  struct dspd_cli_params *oparams;
  const struct dspd_cli_params *iparams;
  size_t br;
  iparams = inbuf;
  if ( iparams->stream == DSPD_PCM_SBIT_PLAYBACK )
    oparams = &client->playback.params;
  else
    oparams = &client->capture.params;
  ret = dspd_stream_ctl(client->bparams.conn,
			client->bparams.client,
			req,
			iparams,
			inbufsize,
			oparams,
			sizeof(*oparams),
			&br);
  if ( ret == 0 )
    {
      if ( br > outbufsize )
	br = outbufsize;
      if ( br )
	memcpy(outbuf, oparams, br);
      *bytes_returned = br;

      if ( iparams->stream == DSPD_PCM_SBIT_PLAYBACK )
	{
	  dspd_intrp_reset2(&client->playback_intrp, client->playback.params.rate);
	  client->playback_xfer = false;
	}
  
      if ( iparams->stream == DSPD_PCM_SBIT_CAPTURE )
	dspd_intrp_reset2(&client->capture_intrp, client->capture.params.rate);
      

    }
  return ret;
}
static int32_t rclient_synccmd(struct dspd_rclient *client,
			       uint32_t req,
			       const void   *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize,
			       size_t       *bytes_returned)
{
  int32_t ret;
  
  const struct dspd_sync_cmd *cmd = inbuf;
  struct dspd_sync_cmd *ocmd = outbuf;
  ret = dspd_stream_ctl(client->bparams.conn,
			client->bparams.client,
			req,
			inbuf,
			inbufsize,
			outbuf,
			outbufsize,
			bytes_returned);
  if ( ret == 0 && *bytes_returned == sizeof(*cmd) && cmd->cmd == SGCMD_STARTALL )
    {
      if ( PLAYBACK_ENABLED(client) && 
	   (ocmd->streams & DSPD_PCM_SBIT_PLAYBACK) )
	{
	  client->playback.trigger_tstamp = ocmd->tstamp;
	  dspd_intrp_set(&client->playback_intrp, client->playback.trigger_tstamp, 0);
	}

       if ( CAPTURE_ENABLED(client) && 
	   (ocmd->streams & DSPD_PCM_SBIT_CAPTURE) )
	 {
	   client->capture.trigger_tstamp = ocmd->tstamp;
	   dspd_intrp_set(&client->capture_intrp, client->capture.trigger_tstamp, 0);
	 }
       rclient_add_trigger(client, ocmd->streams);
       client->playback.ready = !! ( client->trigger & DSPD_PCM_SBIT_PLAYBACK );
       client->capture.ready = !! ( client->trigger & DSPD_PCM_SBIT_CAPTURE );
    }
  return ret;
}

static int32_t rclient_pause(struct dspd_rclient *client,
			     uint32_t req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize,
			     size_t       *bytes_returned)
{
  dspd_time_t tstamps[2] = { 0, 0 };
  int32_t ret;
  int32_t enable;
  size_t br;
  if ( inbufsize != sizeof(enable) )
    return -EINVAL;
  enable = *(int32_t*)inbuf;
  *bytes_returned = 0;
  ret = dspd_stream_ctl(client->bparams.conn,
			client->bparams.client,
			req,
			&enable,
			sizeof(enable),
			tstamps,
			sizeof(tstamps),
			&br);
  if ( ret == 0 )
    {
      if ( outbuf != NULL && outbufsize >= sizeof(tstamps) )
	{
	  memcpy(outbuf, tstamps, sizeof(tstamps));
	  *bytes_returned = sizeof(tstamps);
	}
      if ( PLAYBACK_ENABLED(client) )
	{
	  if ( enable )
	    {
	      dspd_mbx_reset(&client->playback.mbx);
	      dspd_intrp_reset(&client->playback_intrp);
	      client->playback.trigger_tstamp = 0;
	    } else if ( br == sizeof(tstamps) )
	    {
	      client->playback.trigger_tstamp = tstamps[DSPD_PCM_STREAM_PLAYBACK];
	    }
	}
      if ( CAPTURE_ENABLED(client) )
	{
	  if ( enable )
	    {
	      dspd_intrp_reset(&client->capture_intrp);
	      dspd_mbx_reset(&client->capture.mbx);
	      client->capture.trigger_tstamp = 0;
	    } else
	    {
	      client->capture.trigger_tstamp = tstamps[DSPD_PCM_STREAM_CAPTURE];
	    }
	}
    }
  return ret;
}


typedef int32_t (*dspd_req_filter_t)(struct dspd_rclient *client,
				     uint32_t      req,
				     const void   *inbuf,
				     size_t        inbufsize,
				     void         *outbuf,
				     size_t        outbufsize,
				     size_t       *bytes_returned);

static const dspd_req_filter_t rclient_filters[] = {
  [CLIDX(DSPD_SCTL_CLIENT_START)] = rclient_start,
  [CLIDX(DSPD_SCTL_CLIENT_STOP)] = rclient_stop,
  [CLIDX(DSPD_SCTL_CLIENT_SETTRIGGER)] = rclient_settrigger,
  [CLIDX(DSPD_SCTL_CLIENT_SETPARAMS)] = rclient_setparams,
  [CLIDX(DSPD_SCTL_CLIENT_SYNCCMD)] = rclient_synccmd,
  [CLIDX(DSPD_SCTL_CLIENT_PAUSE)] = rclient_pause,
};


int32_t dspd_rclient_ctl(struct dspd_rclient *client,
			 uint32_t    req,
			 const void *inbuf,
			 size_t      inbufsize,
			 void       *outbuf,
			 size_t      outbufsize,
			 size_t     *br)
{
  uint32_t r = req & ~(DSPD_REQ_FLAG_CMSG_FD|DSPD_REQ_FLAG_REMOTE);
  int32_t ret;
  dspd_req_filter_t filter;
  size_t b = 0;
  if ( client->bparams.conn == NULL )
    return -EBADF;

  if ( br == NULL )
    br = &b;
  if ( r <= DSPD_DCTL_MAX )
    {
      ret = dspd_stream_ctl(client->bparams.conn,
			    0,
			    req,
			    inbuf,
			    inbufsize,
			    outbuf,
			    outbufsize,
			    br);
    } else if ( r >= DSPD_SCTL_CLIENT_MIN && r <= DSPD_SCTL_CLIENT_MAX )
    {
      r -= DSPD_SCTL_CLIENT_MIN;
      if ( r < (sizeof(rclient_filters) / sizeof(rclient_filters[0])) )
	{
	  filter = rclient_filters[r];
	} else
	{
	  filter = NULL;
	}
 
      if ( filter )
	{
	  ret = filter(client,
		       req,
		       inbuf,
		       inbufsize,
		       outbuf,
		       outbufsize,
		       br);
	} else
	{
	  ret = dspd_stream_ctl(client->bparams.conn,
				client->bparams.client,
				req,
				inbuf,
				inbufsize,
				outbuf,
				outbufsize,
				br);
	}
    } else if ( r >= DSPD_SCTL_SERVER_MIN && r <= DSPD_SCTL_SERVER_MAX )
    {
      ret = dspd_stream_ctl(client,
			    client->bparams.device,
			    req,
			    inbuf,
			    inbufsize,
			    outbuf,
			    outbufsize,
			    br);
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}

int32_t dspd_rclient_drain(struct dspd_rclient *client)
{
  int32_t ret, err;
  uint64_t abstime, wt, diff;
  uint32_t len, appl, hw;
  if ( PLAYBACK_ENABLED(client) && (client->trigger & DSPD_PCM_SBIT_PLAYBACK) )
    {
      do {
	if ( (err = dspd_fifo_get_error(&client->playback.fifo)) )
	  break;
	err = dspd_rclient_status(client, DSPD_PCM_SBIT_PLAYBACK, NULL);
	if ( err != -EAGAIN )
	  break;

	ret = dspd_fifo_len_ptrs(&client->playback.fifo, 
				 &len,
				 &appl,
				 &hw);
	if ( ret < 0 )
	  break;
	if ( len > 0 )
	  {
	    if ( err == -EAGAIN )
	      {
		usleep((client->playback.sample_time * client->playback.params.fragsize) / 1000);
	      } else
	      {
		hw += len;
		diff = hw - client->playback.status.hw_ptr;
		abstime = diff;
		abstime *= client->playback.sample_time;
		abstime += client->playback.status.tstamp;
		dspd_sleep(abstime, &wt);
	      }
	  }
      } while ( len > 0 );
      usleep((client->playback.sample_time * client->playback.params.latency) / 1000);
    } else
    {
      err = -EBADFD;
    }
  return err;
}

int32_t dspd_rclient_reset(struct dspd_rclient *client, int32_t stream)
{
  int32_t reset = 0, ret;
  size_t br;
  if ( (stream & DSPD_PCM_SBIT_PLAYBACK) && PLAYBACK_ENABLED(client) )
    reset |= DSPD_PCM_SBIT_PLAYBACK;
  if ( (stream & DSPD_PCM_SBIT_CAPTURE) && CAPTURE_ENABLED(client) )
    reset |= DSPD_PCM_SBIT_CAPTURE;
  ret = dspd_rclient_ctl(client,
			 DSPD_SCTL_CLIENT_STOP,
			 &reset,
			 sizeof(reset),
			 NULL,
			 0,
			 &br);
  if ( ret == 0 )
    {
      if ( (stream & DSPD_PCM_SBIT_PLAYBACK) && PLAYBACK_ENABLED(client) )
	{
	  client->playback.trigger_tstamp = 0;
	  dspd_mbx_reset(&client->playback.mbx);
	  dspd_fifo_reset(&client->playback.fifo);
	  dspd_mbx_reset(&client->playback.mbx);
	  dspd_intrp_reset(&client->playback_intrp);
	  client->playback.trigger_tstamp = 0;
	  client->playback_xfer = 0;
	  memset(&client->playback.status, 0, sizeof(client->playback.status));
	}
      if ( (stream & DSPD_PCM_SBIT_CAPTURE) && CAPTURE_ENABLED(client) )
	{
	  client->playback.trigger_tstamp = 0;
	  dspd_mbx_reset(&client->capture.mbx);
	  dspd_fifo_reset(&client->capture.fifo);
	  dspd_mbx_reset(&client->capture.mbx);
	  dspd_intrp_reset(&client->capture_intrp);
	  client->capture.trigger_tstamp = 0;
	  memset(&client->capture.status, 0, sizeof(client->capture.status));
	}
       client->trigger &= ~reset;
    }
  return ret;
}

int32_t dspd_rclient_set_avail_min(struct dspd_rclient *client, uint32_t avail_min)
{
  int ret;
  struct dspd_rclient_swparams swp = client->swparams;
  swp.avail_min = avail_min;
  ret = dspd_rclient_ctl(client,
			 DSPD_SCTL_CLIENT_SWPARAMS,
			 &swp,
			 sizeof(swp),
			 NULL,
			 0,
			 NULL);
  if ( ret == 0 )
    client->swparams.avail_min = avail_min;
  return ret;
  
}

/*
  Available frames for constant playback latency.  The application should plan on filling up to 3/4 of the buffer.
  The other 1/4 is to compensate for latency changes.

  The idea is that the application buffer size is (params.bufsize - params.latency) and the remaining buffer
  is either unused (server running at full latency) or partially used to make up for decreased latency.
  It uses the A/V sync latency to do this and tops out at reserving up to whatever the connection latency
  is.
  
  Any client using constant latency should probably request at least 4 fragments.  The minimum delay is typically
  going to be about 4-5 ms depending on the device.  Video players would do well to select a 100-300ms.

*/
int32_t dspd_rclient_avail_cl(struct dspd_rclient *client, uint32_t *avail_min, int32_t *delay)
{
  int32_t ret;
  uint32_t fill, avail, wdiff;
  struct dspd_pcmcli_status status;
  if ( ! PLAYBACK_ENABLED(client) )
    return -EINVAL;
  ret = dspd_rclient_avail(client, DSPD_PCM_SBIT_PLAYBACK);
  if ( ret < 0 )
    return ret;
  avail = ret;
  *avail_min = client->playback.params.fragsize;
  *delay = -1;
  if ( dspd_rclient_status(client, DSPD_PCM_SBIT_PLAYBACK, &status) == 0 )
    {
      fill = client->playback.params.bufsize - avail;
      if ( status.delay > fill && status.delay > 0 )
	{
	  *delay = status.delay;
	  wdiff = status.delay - fill; //Find the additional delay
	  if ( wdiff <= client->playback.params.latency ) //Figure out how much extra space to use
	    *avail_min = client->playback.params.fragsize + (client->playback.params.latency - wdiff);
	  else
	    *avail_min = client->playback.params.fragsize + client->playback.params.latency;
	  if ( wdiff > avail )
	    avail = 0;
	  else
	    avail -= wdiff;
	}
    } else
    {
      *avail_min = client->playback.params.latency + client->playback.params.fragsize;
      if ( avail <= client->playback.params.latency )
	avail = 0;
      else
	avail -= client->playback.params.latency;
    }
  return avail;
}

int dspd_rclient_update_pollfd(struct dspd_rclient *client, uint32_t sbits, bool constant_latency)
{
  int32_t c_avail = 0, p_avail = 0, delay;
  uint32_t avail_min;
  avail_min = client->swparams.avail_min;
  if ( sbits & DSPD_PCM_SBIT_CAPTURE )
    {
      c_avail = dspd_rclient_avail(client, DSPD_PCM_SBIT_CAPTURE);
      if ( c_avail < 0 )
	{
	  if ( c_avail != -EINVAL )
	    dspd_rclient_poll_notify(client, DSPD_PCM_SBIT_CAPTURE);
	  return c_avail;
	} else if ( c_avail < avail_min )
	{
	  c_avail = 0;
	}
    }
  
  if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( constant_latency )
	{
	  p_avail = dspd_rclient_avail_cl(client, &avail_min, &delay);
	} else
	{
	  p_avail = dspd_rclient_avail(client, DSPD_PCM_SBIT_PLAYBACK);
	  if ( p_avail < avail_min )
	    p_avail = 0;
	}
      if ( p_avail < 0 )
	{
	  if ( p_avail != -EINVAL )
	    dspd_rclient_poll_notify(client, DSPD_PCM_SBIT_PLAYBACK);
	  return p_avail;
	} else
	{
	  client->swparams.avail_min = avail_min;
	}
      if ( p_avail < avail_min )
	p_avail = 0;
    }
  if ( p_avail && c_avail )
    {
      dspd_rclient_poll_notify(client, DSPD_PCM_SBIT_PLAYBACK | DSPD_PCM_SBIT_CAPTURE);
    } else if ( p_avail )
    {
      dspd_rclient_poll_notify(client, DSPD_PCM_SBIT_PLAYBACK);
    } else if ( c_avail )
    {
      dspd_rclient_poll_notify(client, DSPD_PCM_SBIT_CAPTURE);
    } else
    {
      dspd_rclient_poll_clear(client, sbits);
      dspd_rclient_status(client, sbits, NULL);
      dspd_update_timer(client, sbits);
    }
  return 0;
}

int dspd_rclient_open(void *context,
		      const char *addr, 
		      const char *name,
		      int stream,
		      struct dspd_rclient **clptr)
{
  struct dspd_conn *conn = NULL;
  struct dspd_rclient *client = NULL;
  int ret;
  struct dspd_rclient_bindparams bp;
  size_t br;

  memset(&bp, 0, sizeof(bp));
  if ( context == NULL )
    {
      ret = dspd_conn_new(addr, &conn);
      if ( ret )
	return ret;
      bp.conn = conn;
    } else
    {
      bp.conn = context;
    }
  ret = dspd_rclient_new(&client, stream);
  if ( ret )
    goto out;

  bp.client = -1;
  bp.device = -1;
  ret = dspd_rclient_bind(client, &bp);
  if ( ret )
    goto out;
  

  ret = dspd_rclient_open_dev(client, name, stream, NULL);
  if ( ret < 0 )
    {
      client->bparams.conn = NULL;
      goto out;
    }
  client->autoclose = true;


  if ( client->bparams.client < 0 )
    {
      ret = dspd_stream_ctl(client->bparams.conn,
			    -1,
			    DSPD_SOCKSRV_REQ_NEWCLI,
			    &client->bparams.client,
			    sizeof(client->bparams.client),
			    &client->bparams.client,
			    sizeof(client->bparams.client),
			    &br);
      if ( ret < 0 )
	goto out;
    } 


  //Create a reservation on the device (kind of a dummy connect)
  ret = dspd_stream_ctl(client->bparams.conn,
			client->bparams.client,
			DSPD_SCTL_CLIENT_RESERVE,
			&client->bparams.device,
			sizeof(client->bparams.device),
			NULL,
			0,
			&br);
  
 out:

  if ( ret < 0 )
    {
      if ( client )
	dspd_rclient_delete(client);
      else if ( conn )
	dspd_conn_delete(conn);
    } else
    {
      *clptr = client;
    }
  return ret;
}

const struct dspd_device_stat *dspd_rclient_devinfo(const struct dspd_rclient *client)
{
  return &client->devinfo;
}

bool dspd_rclient_test_xrun(struct dspd_rclient *client, int sbits)
{
  uint32_t len;
  bool ret = false;
  dspd_time_t diff;
  if ( (sbits & DSPD_PCM_SBIT_PLAYBACK) && 
       client->playback.enabled && client->playback_xfer && (client->trigger & DSPD_PCM_SBIT_PLAYBACK) &&
       client->playback.params.channels )
    {
      if ( dspd_fifo_len(&client->playback.fifo, &len) == 0 )
	{
	  if ( len == 0 )
	    {
	      if ( dspd_rclient_status_ex(client, DSPD_PCM_SBIT_PLAYBACK, NULL, true) == 0 )
		diff = (dspd_get_time() - client->playback.status.tstamp) / client->playback.sample_time;
	      else if ( client->playback.trigger_tstamp )
		diff = (dspd_get_time() - client->playback.trigger_tstamp) / client->playback.sample_time;
	      else
		diff = 0;
	      if ( diff > (client->playback.params.fragsize / 2) )
		ret = true;
	      //else it is probably close enough to recover on the server side (actually tested, not just a theory)
	    }
	}
    }
  if ( (sbits & DSPD_PCM_SBIT_CAPTURE) && client->capture.params.channels && (ret == false))
    {
      if ( dspd_fifo_space(&client->capture.fifo, &len) == 0 )
	if ( len == 0 )
	  ret = true;
    }
  return ret;
}

const struct dspd_cli_params *dspd_rclient_get_hw_params(const struct dspd_rclient *client, int32_t sbit)
{
  const struct dspd_cli_params *ret = NULL;
  if ( sbit & client->streams )
    {
      if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
	ret = &client->playback.params;
      else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
	ret = &client->capture.params;
    } 
  return ret;
}

int32_t dspd_rclient_set_excl(struct dspd_rclient *client, int32_t flags)
{
  
  int32_t ret = -EBADFD;
  struct dspd_lock_result res;
  size_t br, i;
  char c;
  unsigned u;
  struct mq_attr attr;
  if ( client->bparams.conn != NULL && client->bparams.device >= 0 && client->bparams.client >= 0 &&
       client->mq_fd < 0 )
    {
      ret = dspd_rclient_ctl(client,
			     DSPD_SCTL_CLIENT_LOCK,
			     &flags,
			     sizeof(flags),
			     &res,
			     sizeof(res),
			     &br);
      if ( ret == 0 )
	{
	  if ( br == sizeof(res) )
	    {
	      if ( *((int32_t*)client->bparams.conn) == DSPD_OBJ_TYPE_DAEMON_CTX )
		client->mq_fd = res.fd;
	      else
		client->mq_fd = dspd_conn_recv_fd(client->bparams.conn);
	      if ( client->mq_fd >= 0 )
		{
		  client->notification.client = client->bparams.client;
		  client->notification.flags = flags;
		  client->notification.cookie = res.cookie;
		  //Empty the queue just in case there are leftover events from another
		  //client.
		  for ( i = 0; i < 32; i++ )
		    {
		      if ( mq_receive(client->mq_fd, &c, sizeof(c), &u) < 0 )
			break;
		    }

		  if ( mq_getattr(client->mq_fd, &attr) == 0 )
		    client->mq_msgsize = attr.mq_msgsize;
		  else
		    client->mq_msgsize = 1;
		  
		} else
		{
		  ret = -EPROTO;
		}
	    } else
	    {
	      ret = -EPROTO;
	    }
	}
    }
  return ret;
}

const struct dspd_rclient_swparams *dspd_rclient_get_sw_params(struct dspd_rclient *client)
{
  return &client->swparams;
}

int32_t dspd_rclient_set_sw_params(struct dspd_rclient *client, const struct dspd_rclient_swparams *params)
{
  client->swparams = *params;
  return 0;
}

dspd_time_t dspd_rclient_get_trigger_tstamp(struct dspd_rclient *client, int32_t sbit)
{
  dspd_time_t ret;
  if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
    ret = client->playback.trigger_tstamp;
  else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
    ret = client->capture.trigger_tstamp;
  else
    ret = 0;
  return ret;
}
int32_t dspd_rclient_get_trigger(struct dspd_rclient *client)
{
  return client->trigger;
}

int32_t dspd_rclient_client_index(struct dspd_rclient *client)
{
  return client->bparams.client;
}
