/*
 *  PCM_DSPD - DSPD PCM IO plugin for ALSA
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

#include <sys/epoll.h>
#include <atomic_ops.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <samplerate.h>
#include <sys/uio.h>
#include <fcntl.h>
#include "../lib/sslib.h"
static const char *default_plugin_name = "ALSA <-> DSPD PCM I/O Plugin";
#define MIN_PERIODS 3

/*

  Here is the sample config for hooking hw devices.  The idea is to fool all programs into
  thinking that hardware mixing is available.  This plugin is almost equivalent to a USB
  sound device so some applications may not work well if they assume that certain hardware
  features are present.

 #This one allows hooking "hw:" and replacing it.
pcm.hw {
 @args [ CARD SUBDEV ]
 @args.CARD {
   type integer	
#   default 0
 }
 @args.SUBDEV {
   type integer
#   default 0
 }   
 device $CARD
 subdevice $SUBDEV
 hookmode 1
 type dspd
}

#Real hardware.  If the hook above is installed then this is necessary.
pcm.realhw {
 @args [ card subdevice ]
 @args.card {
   type integer
   default 0
 }
 @args.subdevice {
   type integer
   default 0
 }
 card $card
 subdevice $subdevice
 type hw
}


 */

typedef struct _snd_pcm_dspd {
  struct dspd_rclient client;
  struct dspd_conn   *conn;
  int32_t             stream;
  int32_t             frame_bytes;
  struct dspd_device_stat devinfo;
  int                 translate_format;
  int                 resample;
  int                 no_mmap;
  int                 translate_channels;
  snd_pcm_ioplug_t    io;
  int32_t             device;
  int32_t             client_stream;
  snd_pcm_uframes_t   xfer;
  uint32_t            channels;
  struct {
    snd_pcm_chmap_t map;
    unsigned int    channels[SND_CHMAP_LAST+1];
  } chmap;

  snd_pcm_uframes_t appl_ptr;

  bool dead;

} snd_pcm_dspd_t;

static void dspd_event_flags_changed(void *arg, uint32_t *flags)
{
  snd_pcm_dspd_t *dspd = arg;
  if ( (*flags) & DSPD_REQ_FLAG_POLLHUP )
    {
      dspd->dead = true;
      dspd_force_poll_events(&dspd->client, POLLERR|POLLHUP);
    }
  *flags = 0;
}

static bool dspd_check_dead(snd_pcm_dspd_t *dspd)
{
  dspd_ipc_process_messages(dspd->conn, 0);
  return dspd->dead;
}

/*
  Update the application pointer in the FIFO.  The idea is that
  if the application pointer is adjusted manually then it can usually
  be caught and corrected.
*/
static int dspd_update_pointer(snd_pcm_dspd_t *dspd)
{
  snd_pcm_uframes_t p = dspd->io.appl_ptr;
  int ret = 0;
  if ( dspd->dead )
    {
      ret = -ENODEV;
    } else if ( p != dspd->appl_ptr )
    {
      if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
	ret = dspd_rclient_set_write_ptr(&dspd->client, p);
      else
	ret = dspd_rclient_set_read_ptr(&dspd->client, p);
      dspd->appl_ptr = p;
    }
  return ret;
}

static int dspd_alsa_start(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  size_t br;
  if ( dspd_rclient_get_streams(&dspd->client) != dspd->stream )
    return -EBADFD;

  ret = dspd_rclient_get_error(&dspd->client, dspd->stream);
  //Don't send an extra trigger because it might cause a glitch.
  if ( ret == 0 && dspd->client.trigger != dspd->stream )
    {
      //Here is where it is possible to get the real monotonic trigger timestamp
      //but ALSA won't allow it to be used.
      ret = dspd_rclient_ctl(&dspd->client,
			     DSPD_SCTL_CLIENT_START,
			     &dspd->stream,
			     sizeof(dspd->stream),
			     NULL,
			     0,
			     &br);
      if ( ret == 0 )
	dspd->client.trigger = dspd->stream;
    }
  if ( ret == 0 )
    ret = dspd_update_pointer(dspd);
  return ret;
}



static int dspd_stop(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  if ( dspd->client.trigger != dspd->stream ||
       dspd_rclient_get_streams(&dspd->client) != dspd->stream )
    {
      ret = -EBADFD;
    } else
    {
      ret = dspd_rclient_reset(&dspd->client, dspd->stream);
      if ( ret == 0 )
	ret = dspd_update_pointer(dspd);
    }
  return ret;
}

static int dspd_alsa_pause(snd_pcm_ioplug_t *io, int enable)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  size_t br;
  if ( dspd->client.trigger != dspd->stream ||
       dspd_rclient_get_streams(&dspd->client) != dspd->stream )
    return -EBADFD;
  dspd_update_pointer(dspd);

  if ( enable )
    {
      ret = dspd_rclient_ctl(&dspd->client,
			     DSPD_SCTL_CLIENT_START,
			     &dspd->stream,
			     sizeof(dspd->stream),
			     NULL,
			     0,
			     &br);
    } else
    {
      ret = dspd_rclient_ctl(&dspd->client,
			     DSPD_SCTL_CLIENT_STOP,
			     &dspd->stream,
			     sizeof(dspd->stream),
			     NULL,
			     0,
			     &br);
    }
  if ( ret == 0 )
    dspd->client.trigger = 0;
  
  return ret;
}

static int dspd_alsa_resume(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  size_t br;
  ret = dspd_rclient_get_error(&dspd->client, dspd->stream);
  //Don't send an extra trigger because it might theoretically cause a glitch.
  if ( ret == 0 )
    {
      ret = dspd_rclient_ctl(&dspd->client,
			     DSPD_SCTL_CLIENT_START,
			     &dspd->stream,
			     sizeof(dspd->stream),
			     NULL,
			     0,
			     &br);
      if ( ret == 0 )
	{
	  dspd->client.trigger = dspd->stream;
	  ret = dspd_update_pointer(dspd);
	}
    }
  return ret;
}


static snd_pcm_sframes_t dspd_read_pcm(snd_pcm_ioplug_t *io,
				       const snd_pcm_channel_area_t *areas,
				       snd_pcm_uframes_t offset,
				       snd_pcm_uframes_t size)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  char *buf;
  snd_pcm_uframes_t pos = 0;
  snd_pcm_sframes_t ret = 0;
  int32_t avail;
  int err;
  bool waited = false;
  if ( dspd_rclient_get_streams(&dspd->client) != DSPD_PCM_SBIT_CAPTURE )
    return -EBADFD;

  if ( size >= dspd->client.swparams.start_threshold )
    {
      if ( dspd->client.trigger == 0 )
	{
	  ret = dspd_alsa_start(io);
	  if ( ret < 0 )
	    return ret;
	}
    }


  buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;

  err = dspd_update_pointer(dspd);
  if ( err < 0 )
    return err;
  
  

  while ( pos < size )
    {
      
      ret = dspd_rclient_read(&dspd->client, &buf[pos*dspd->frame_bytes], size - pos);
      if ( ret < 0 )
	{
	  break;
	} else if ( ret > 0 )
	{
	  pos += ret;
	  dspd->xfer += ret;
	  waited = false;
	} else
	{
	  if ( waited )
	    if ( dspd_check_dead(dspd) )
	      break;
	  if ( io->nonblock == 0 && pos < size )
	    {
	      ret = dspd_rclient_wait(&dspd->client, dspd->stream);
	      if ( ret < 0 && ret != -EINTR && ret != -EAGAIN )
		break;
	      waited = true;
	    } else if ( io->nonblock )
	    {
	      break;
	    }
	}
    }
  if ( pos == 0 && ret == 0 )
    ret = -EAGAIN;
  else if ( pos > 0 )
    ret = pos;
  
  if ( ret > 0 )
    {
      dspd->appl_ptr += ret;
      avail = dspd_rclient_avail(&dspd->client, DSPD_PCM_SBIT_CAPTURE);
      if ( avail >= 0 )
	{
	  if ( avail >= dspd->client.swparams.avail_min )
	    {
	      dspd_rclient_poll_notify(&dspd->client, DSPD_PCM_SBIT_CAPTURE);
	    } else
	    {
	      dspd_rclient_poll_clear(&dspd->client, DSPD_PCM_SBIT_CAPTURE);
	      dspd_rclient_status(&dspd->client, dspd->stream, NULL);
	      dspd_update_timer(&dspd->client, dspd->stream);
	    }
	}
    } else if ( ret != -EAGAIN )
    {
      dspd_rclient_poll_notify(&dspd->client, DSPD_PCM_SBIT_CAPTURE);
    }
  return ret;
}



static snd_pcm_sframes_t dspd_write_pcm(snd_pcm_ioplug_t *io,
					const snd_pcm_channel_area_t *areas,
					snd_pcm_uframes_t offset,
					snd_pcm_uframes_t size)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  const char *buf;
  snd_pcm_uframes_t pos = 0;
  snd_pcm_sframes_t ret = 0;
  int32_t avail;
  int err;
  bool waited = false;
  if ( dspd_rclient_get_streams(&dspd->client) != DSPD_PCM_SBIT_PLAYBACK )
    return -EBADFD;

  buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;

  err = dspd_update_pointer(dspd);
  if ( err < 0 )
    return err;
  
  while ( pos < size )
    {
      ret = dspd_rclient_write(&dspd->client, &buf[pos*dspd->frame_bytes], size - pos);

      

      if ( ret < 0 )
	{
	  break;
	} else if ( ret > 0 )
	{
	  waited = false;
	  pos += ret;
	  dspd->xfer += ret;
	  if ( dspd->xfer >= dspd->client.swparams.start_threshold )
	    {
	      if ( dspd->client.trigger == 0 )
		{
		  ret = dspd_alsa_start(io);
		  if ( ret < 0 )
		    break;
		}
	    }
	} else
	{
	  if ( waited )
	    if ( dspd_check_dead(dspd) )
	      break;

	  if ( io->nonblock == 0 && pos < offset )
	    {
	      ret = dspd_rclient_wait(&dspd->client, dspd->stream);
	      if ( ret < 0 && ret != -EINTR && ret != -EAGAIN )
		break;
	      waited = true;
	    } else if ( io->nonblock )
	    {
	      break;
	    }
	}
    }
  if ( pos == 0 && ret == 0 )
    ret = -EAGAIN;
  else if ( pos > 0 )
    ret = pos;
  
  if ( ret > 0 )
    {
      dspd->appl_ptr += ret;
      avail = dspd_rclient_avail(&dspd->client, DSPD_PCM_SBIT_PLAYBACK);
      if ( avail >= 0 )
	{
	  if ( avail >= dspd->client.swparams.avail_min )
	    {
	      dspd_rclient_poll_notify(&dspd->client, DSPD_PCM_SBIT_PLAYBACK);
	    } else
	    {
	      dspd_rclient_poll_clear(&dspd->client, DSPD_PCM_SBIT_PLAYBACK);
	      dspd_rclient_status(&dspd->client, dspd->stream, NULL);
	      dspd_update_timer(&dspd->client, dspd->stream);
	    }
	}
    } else if ( ret != -EAGAIN )
    {
      dspd_rclient_poll_notify(&dspd->client, DSPD_PCM_SBIT_PLAYBACK);
    }

  return ret;
}

static snd_pcm_sframes_t dspd_pointer(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int32_t ret;
  uint32_t ptr;
  if ( dspd_rclient_get_streams(&dspd->client) == 0 )
    {
      ret = -EBADFD;
    } else if ( io->state == SND_PCM_STATE_XRUN && dspd->stream == dspd->client.trigger )
    {
      //XRUN only if it was first triggered
      dspd_rclient_poll_notify(&dspd->client, dspd->stream);
      ret = -EPIPE;
    } else
    {
      ret = dspd_update_pointer(dspd);
      if ( ret == 0 )
	{
	  ret = dspd_rclient_get_hw_ptr(&dspd->client, dspd->stream, &ptr);
	  if ( ret == 0 )
	    {
	      ret = ptr % io->buffer_size;
	    } else if ( ret == -EPIPE )
	    {
	      snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
	      ret = ptr % io->buffer_size;
	      dspd_rclient_poll_notify(&dspd->client, dspd->stream);
	    }
	} else
	{
	  snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
	}
    }
  return ret;
}


static int dspd_alsa_close(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  if ( io->name != default_plugin_name )
    io->name = default_plugin_name;
  dspd_rclient_destroy(&dspd->client);
  dspd_conn_delete(dspd->conn);
  free(dspd);
  return 0;
}

int dspd_hw_params(snd_pcm_ioplug_t *io,
		   snd_pcm_hw_params_t *params)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int dir, ret;
  struct dspd_cli_params cp, p;
  snd_pcm_format_t fmt;
  snd_pcm_uframes_t frames;
  unsigned int ui;
  size_t br;
  struct dspd_client_shm shm;
  int shm_fd = -1;
  snd_pcm_hw_params_t *newparams;
  snd_pcm_uframes_t maxbuf, minbuf, n;
  snd_pcm_hw_params_alloca(&newparams);
  snd_pcm_hw_params_copy(newparams, params);
  memset(&cp, 0, sizeof(cp));
  cp.stream = dspd->stream;
  cp.flags = DSPD_CLI_FLAG_SHM;
 
  //fprintf(stderr, "SETHWPARAMS\n");

  ret = snd_pcm_hw_params_get_buffer_size_min(params, &minbuf);
  if ( ret )
    goto error;
  ret = snd_pcm_hw_params_get_buffer_size_max(params, &maxbuf);
  if ( ret )
    goto error;

  dir = 0;
  ret = snd_pcm_hw_params_get_rate(newparams, &ui, &dir);
  if ( ret )
    return ret;
  cp.rate = ui;

  ret = snd_pcm_hw_params_get_format(newparams, &fmt);
  if ( ret )
    return ret;
  cp.format = fmt; 
  
  dir = 0;
  ret = snd_pcm_hw_params_get_period_size(newparams, &frames, &dir);
  if ( ret )
    return ret;
  if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
    cp.fragsize = dspd_get_fragsize(&dspd->devinfo.playback, cp.rate, frames);
  else
    cp.fragsize = dspd_get_fragsize(&dspd->devinfo.capture, cp.rate, frames);
  n = cp.fragsize * MIN_PERIODS;
  if ( n > maxbuf )
    cp.fragsize = maxbuf / MIN_PERIODS;
  cp.latency = cp.fragsize;
 

  ret = snd_pcm_hw_params_get_buffer_size(newparams, &frames);
  if ( ret )
    return ret;
  cp.bufsize = frames;
  if ( cp.bufsize < (cp.fragsize * MIN_PERIODS) )
    cp.bufsize = cp.fragsize * MIN_PERIODS;
  if ( cp.bufsize % cp.fragsize )
    {
      ret = cp.bufsize / cp.fragsize;
      ret++;
      cp.bufsize = ret * cp.fragsize;
      
    }
  if ( cp.bufsize < minbuf )
    cp.bufsize = minbuf;

  if ( cp.bufsize > maxbuf )
    {
      maxbuf = cp.bufsize;
      ret = snd_pcm_hw_params_set_buffer_size_max(dspd->io.pcm, newparams, &maxbuf);
      if ( ret )
	goto error;
      cp.bufsize = maxbuf;
    }
  //fprintf(stderr, "SETTING\n");

  dir = 0;
  ret = snd_pcm_hw_params_get_channels(newparams, &ui);
  if ( ret )
    return ret;
  cp.channels = ui;
  
  dspd->frame_bytes = snd_pcm_format_size(cp.format, cp.channels);
  ret = dspd_stream_ctl(dspd->conn,
			dspd->client_stream,
			DSPD_SCTL_CLIENT_SETPARAMS,
			&cp,
			sizeof(cp),
			&p,
			sizeof(p),
			&br);
  if ( ret )
    return ret;
  ret = dspd_stream_ctl(dspd->conn,
			dspd->client_stream,
			DSPD_SCTL_CLIENT_MAPBUF,
			&dspd->stream,
			sizeof(dspd->stream),
			&shm,
			sizeof(shm),
			&br);
  if ( ret )
    goto error;
  
  //fprintf(stderr, "PARAMS: IN=%d,%d OUT=%d,%d\n", cp.bufsize, cp.fragsize, p.bufsize, p.fragsize);

  shm_fd = dspd_conn_recv_fd(dspd->conn);

  //fprintf(stderr, "CONNECT\n");
  ret = dspd_stream_ctl(dspd->conn,
			dspd->client_stream,
			DSPD_SCTL_CLIENT_CONNECT,
			&dspd->device,
			sizeof(dspd->device),
			NULL,
			0,
			&br);

  if ( ret )
    {
      //fprintf(stderr, "RET=%d %ld %ld\n", ret, (long)p.bufsize, (long)p.fragsize);
      goto error;
    }

  
  //fprintf(stderr, "ATTACH\n");
  struct dspd_rclient_data rd;
  rd.conn = dspd->conn;
  rd.client = dspd->client_stream;
  rd.device = dspd->device;
  
  ret = dspd_rclient_attach(&dspd->client,
			    &shm,
			    &p,
			    &rd);

  if ( ret )
    goto error;
  //fprintf(stderr, "DATA %p %p %p\n", &dspd->client, dspd, dspd->conn);

  ret = snd_pcm_hw_params_set_buffer_size(dspd->io.pcm, newparams, p.bufsize);
  if ( ret )
    goto error;
  
  ret = snd_pcm_hw_params_set_period_size(dspd->io.pcm, newparams, p.fragsize, 0);
  if ( ret )
    ret = snd_pcm_hw_params_set_period_size(dspd->io.pcm, newparams, p.fragsize, 1);
  if ( ret )
    (void)snd_pcm_hw_params_set_period_size(dspd->io.pcm, newparams, p.fragsize, -1);
  

  ret = snd_pcm_hw_params_set_rate(dspd->io.pcm, newparams, p.rate, 0);
  if ( ret )
    goto error;

  ret = snd_pcm_hw_params_set_channels(dspd->io.pcm, newparams, p.channels);
  if ( ret )
    goto error;
  io->buffer_size = p.bufsize;
  io->period_size = p.fragsize;
  dspd->channels = p.channels;
  snd_pcm_hw_params_copy(params, newparams);

  return 0;
 
 error:
  if ( shm_fd >= 0 )
    close(shm_fd);
  if ( ret > 0 )
    ret *= -1;
  return ret;
  
}

int dspd_sw_params(snd_pcm_ioplug_t *io,
		   snd_pcm_sw_params_t *params)
{
  int err;
  snd_pcm_uframes_t val;
  snd_pcm_dspd_t *dspd = io->private_data;

  err = snd_pcm_sw_params_get_avail_min(params, &val);
  if ( err != 0 )
    return err;
  dspd->client.swparams.avail_min = val;
  
  err = snd_pcm_sw_params_get_start_threshold(params, &val);
  if ( err != 0 )
    return err;
  dspd->client.swparams.start_threshold = val;

  err = snd_pcm_sw_params_get_stop_threshold(params, &val);
  if ( err != 0 )
    return err;
  dspd->client.swparams.stop_threshold = val;
  return 0;
}

static int dspd_prepare(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  ret = dspd_rclient_reset(&dspd->client, dspd->stream);
  if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK && ret == 0 )
    dspd_rclient_poll_notify(&dspd->client, dspd->stream);
  else
    dspd_rclient_poll_notify(&dspd->client, 0);
  return ret;
}

static int dspd_drain(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;

  if ( dspd->stream != dspd->client.trigger )
    {
      int ret = dspd_alsa_start(io);
      if ( ret < 0 )
	return ret;
    }
  return dspd_rclient_drain(&dspd->client);
}

static int dspd_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret, avail;
  struct dspd_pcmcli_status status;
  if ( ! dspd->client.trigger == dspd->stream )
    return -EBADFD;
  ret = dspd_update_pointer(dspd);
  if ( ret < 0 )
    return ret;
  ret = dspd_rclient_avail(&dspd->client, dspd->stream);
  if ( ret < 0 )
    return ret;
  if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK && ret == io->buffer_size )
    return -EIO;
  avail = ret;
  ret = dspd_rclient_status(&dspd->client, dspd->stream, &status);
  if ( status.delay < 0 && dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( avail > 0 && avail < io->buffer_size )
	{
	  *delayp = io->buffer_size - avail;
	  ret = 0;
	} else
	{
	  ret = -EPIPE;
	}
    } else if ( ret == 0 )
    {
      if ( status.delay >= 0 )
	*delayp = status.delay;
      else
	ret = -EPIPE;
    } else
    {
      *delayp = 0;
      ret = 0;
    }
  return ret;
}


static int dspd_poll_revents(snd_pcm_ioplug_t *io,
			     struct pollfd *pfd,
			     unsigned int nfds,
			     unsigned short *revents)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ev;
  dspd_update_pointer(dspd);
  ev = dspd_rclient_poll_revents(&dspd->client, pfd, nfds);
  if ( ev & POLLMSG )
    dspd_ipc_process_messages(dspd->conn, 1);
  if ( dspd->dead )
    ev |= (POLLHUP|POLLERR);
  if ( io->state == SND_PCM_STATE_XRUN )
    ev |= (POLLHUP|POLLERR);
  *revents = ev;
  return 0;
}

static int dspd_poll_descriptors(snd_pcm_ioplug_t *io,
				 struct pollfd *pfd,
				 unsigned int space)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  dspd_update_pointer(dspd);
  return dspd_rclient_pollfd(&dspd->client, space, pfd);
}

static int dspd_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  dspd_update_pointer(dspd);
  return dspd_rclient_pollfd_count(&dspd->client);
}

static snd_pcm_chmap_t *dspd_get_chmap(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  size_t br;
  int32_t ret;
  unsigned int i;
  struct dspd_fchmap map;
  snd_pcm_chmap_t *m;
  int32_t stream, req;

  if ( dspd->channels == 0 )
    {
      stream = dspd->device;
      req = DSPD_SCTL_SERVER_GETCHANNELMAP;
    } else
    {
      stream = dspd->client_stream;
      req = DSPD_SCTL_CLIENT_GETCHANNELMAP;
    }
  ret = dspd_stream_ctl(dspd->conn,
			stream,
			req,
			&dspd->stream,
			sizeof(dspd->stream),
			&map,
			sizeof(map),
			&br);
  if ( ret == 0 )
    {
      for ( i = 0; i < map.map.channels; i++ )
	dspd->chmap.map.pos[i] = map.map.pos[i];
      dspd->chmap.map.channels = map.map.channels;
      m = &dspd->chmap.map;
    } else
    {
      m = NULL;
    }
  return m;
}

static int dspd_set_chmap(snd_pcm_ioplug_t *io, const snd_pcm_chmap_t *map)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  struct dspd_fchmap fmap;
  unsigned int i, ret;
  uint32_t channels;
  if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
    channels = dspd->devinfo.playback.channels;
  else
    channels = dspd->devinfo.capture.channels;
  if ( (map->channels > 2 && map->channels > channels) || map->channels == 0 )
    {
      ret = -EINVAL;
    } else
    {
      memset(&fmap, 0, sizeof(fmap));
      for ( i = 0; i < map->channels; i++ )
	fmap.map.pos[i] = map->pos[i];
      fmap.map.channels = map->channels;
      fmap.map.stream = dspd->stream;
      ret = dspd_stream_ctl(dspd->conn,
			    dspd->client_stream,
			    DSPD_SCTL_CLIENT_SETCHANNELMAP,
			    &fmap,
			    sizeof(fmap),
			    NULL,
			    0,
			    NULL);
      if ( ret > 0 )
	ret *= -1;
    }
  return ret;
}

const snd_pcm_ioplug_callback_t dspd_playback_callback = {
  .start = dspd_alsa_start,
  .stop = dspd_stop,
  .transfer = dspd_write_pcm,
  .pointer = dspd_pointer,
  .close = dspd_alsa_close,
  .hw_params = dspd_hw_params,
  .sw_params = dspd_sw_params,
  .prepare = dspd_prepare,
  .drain = dspd_drain,
  .delay = dspd_delay,
  .pause = dspd_alsa_pause,
  .resume = dspd_alsa_resume,
  .poll_revents = dspd_poll_revents,
  .poll_descriptors_count = dspd_poll_descriptors_count,
  .poll_descriptors = dspd_poll_descriptors,
  .get_chmap = dspd_get_chmap,
  .set_chmap = dspd_set_chmap,
};

const snd_pcm_ioplug_callback_t dspd_capture_callback = {
  .start = dspd_alsa_start,
  .stop = dspd_stop,
  .transfer = dspd_read_pcm,
  .pointer = dspd_pointer,
  .close = dspd_alsa_close,
  .hw_params = dspd_hw_params,
  .sw_params = dspd_sw_params,
  .prepare = dspd_prepare,
  .drain = dspd_drain,
  .delay = dspd_delay,
  .pause = NULL,
  .resume = NULL,
  .poll_revents = dspd_poll_revents,
  .poll_descriptors_count = dspd_poll_descriptors_count,
  .poll_descriptors = dspd_poll_descriptors,
  .get_chmap = dspd_get_chmap,
  .set_chmap = dspd_set_chmap,
};

#define MAXBUF         (1024*1024)
#define MINBUF          128
#define MAX_PERIODS     64
#define MAX_PERIOD_SIZE 65536
static int dspd_hw_constraint(snd_pcm_dspd_t *dspd)
{
  static const snd_pcm_access_t access_list[] = {
    SND_PCM_ACCESS_RW_INTERLEAVED,
    SND_PCM_ACCESS_MMAP_INTERLEAVED
  };
  size_t count, i;
  unsigned int formats[SND_PCM_FORMAT_LAST+1];
  const struct pcm_conv *conv;
  int max_chan, min_chan, min_rate, max_rate, err, maxb, minb;
  const struct dspd_cli_params *params;
  size_t min_fmtlen = INTPTR_MAX, max_fmtlen = 0, len, min_fragsize, max_fragsize, f, min_fragtime;
  size_t max_fragtime;
  if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
    params = &dspd->devinfo.playback;
  else
    params = &dspd->devinfo.capture;

  if ( dspd->no_mmap )
    count = 1;
  else
    count = 2;

  err = snd_pcm_ioplug_set_param_list(&dspd->io,
				      SND_PCM_IOPLUG_HW_ACCESS,
				      count,
				      access_list);
  if ( err < 0 )
    return err;

  if ( dspd->translate_format )
    {
      count = 0;
      for ( i = 0; i <= SND_PCM_FORMAT_LAST; i++ )
	{
	  conv = dspd_getconv(i);
	  if ( conv != NULL && conv->tofloat32 && conv->fromfloat32 )
	    {
	      formats[count] = i;
	      count++;
	      len = dspd_get_pcm_format_size((int)i);
	      if ( len < min_fmtlen )
		min_fmtlen = len;
	      if ( len > max_fmtlen )
		max_fmtlen = len;
	    }
	}
    } else
    {
      //All 32 bit formats.
      count = 0;
      min_fmtlen = 4;
      max_fmtlen = min_fmtlen;
      for ( i = SND_PCM_FORMAT_S24_LE; i <= SND_PCM_FORMAT_FLOAT64_BE; i++ )
	{
	  formats[count] = i;
	  count++;
	}
    }
  err = snd_pcm_ioplug_set_param_list(&dspd->io,
				      SND_PCM_IOPLUG_HW_FORMAT,
				      count,
				      formats);
  if ( err < 0 )
    return err;

  max_chan = params->channels;
  if ( dspd->translate_channels )
    min_chan = 1;
  else
    min_chan = max_chan;

  err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					SND_PCM_IOPLUG_HW_CHANNELS,
					min_chan,
					max_chan);
  if ( err < 0 )
    return err;


  if ( dspd->resample )
    {
      min_rate = 8000;
      max_rate = 192000;
    } else
    {
      min_rate = params->rate;
      max_rate = params->rate;
    }
  err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					SND_PCM_IOPLUG_HW_RATE,
					min_rate,
					max_rate);

  if ( err < 0 )
    return err;

  min_fragtime = dspd_src_get_frame_count(params->rate, min_rate, params->min_latency);
  min_fragsize = min_fragtime * min_fmtlen;
  max_fragtime = dspd_src_get_frame_count(params->rate, max_rate, params->max_latency);
  max_fragsize = max_fragtime * max_fmtlen;

  

  if ( min_fragsize < 128 )
    min_fragsize = 128;
  
  if ( max_fragsize > min_fragsize && max_fragsize > MAX_PERIOD_SIZE )
    max_fragsize = MAX_PERIOD_SIZE;


  err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					min_fragsize,
					max_fragsize);


  if ( err < 0 )
    return err;


  
  f = params->rate / min_fragtime;
  if ( f < MIN_PERIODS )
    f = MIN_PERIODS;
  else if ( f > MAX_PERIODS )
    f = MAX_PERIODS;
  err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					SND_PCM_IOPLUG_HW_PERIODS,
					MIN_PERIODS,
					f);

  if ( err < 0 )
    return err;

  minb = MIN_PERIODS * min_fragsize;
  if ( minb < MINBUF )
    minb = MINBUF;
  maxb = max_rate * max_fmtlen * max_chan;
  if ( maxb > MAXBUF )
    maxb = MAXBUF;

  err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					SND_PCM_IOPLUG_HW_BUFFER_BYTES,
					minb,
					maxb);
  if ( err < 0 )
    return err;
  return 0;
}


SND_PCM_PLUGIN_DEFINE_FUNC(dspd)
{
  snd_pcm_dspd_t *dspd;
  unsigned char *mask = NULL;
  int32_t err;
  uint32_t mask_size, mtype, dev;
  size_t br, i, bits;
  int32_t defaultdev = -1;
  snd_config_iterator_t cfgi, next;
  char hwdev[32];
  *pcmp = NULL;
  dspd = calloc(1, sizeof(*dspd));
  if ( ! dspd )
    return -ENOMEM;
  dspd->device = -1;
  dspd->io.nonblock = !!(mode & SND_PCM_NONBLOCK);
  dspd->client_stream = -1;
  //The SND_PCM_IOPLUG_FLAG_MONOTONIC is not really implemented in libasound
  //for ioplug.
  dspd->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
  dspd->io.version = SND_PCM_IOPLUG_VERSION;
  dspd->io.name = default_plugin_name;
  dspd->io.poll_fd = -1;
  dspd->io.mmap_rw = 0;
  //fprintf(stderr, "CREATE %p\n", dspd);

  /*
    The original idea was to disable the translations and use "plug:" instead
    because the hardware constraints for io plugins don't have a timing constraint.
    The lack of translation would make the buffer sizes effectively a timing constraint.
    Most apps behave so well it isn't really necessary and the plug layer seems to be buggy.
    It will generally spin in a loop checking the hardware pointer even when avail > avail_min.
    It usually likes to do about 2 or 3 times avail_min and it doesn't appear to be related to
    sample rates.
  */
  dspd->translate_channels = 1;
  dspd->translate_format = 1;
  dspd->resample = 1;


  dspd->io.callback = stream == SND_PCM_STREAM_PLAYBACK ?
    &dspd_playback_callback : &dspd_capture_callback;
  dspd->stream = stream == SND_PCM_STREAM_PLAYBACK ?
    DSPD_PCM_SBIT_PLAYBACK : DSPD_PCM_SBIT_CAPTURE;
  long device = -1, subdevice = -1, hookmode = 0;
  hwdev[0] = 0;

  snd_config_for_each(cfgi, next, conf) 
    {
      snd_config_t *n = snd_config_iterator_entry(cfgi);
      const char *id;
      if (snd_config_get_id(n, &id) < 0)
	continue;

      if ( strcmp(id, "device") == 0 || strcmp(id, "card") == 0 ) 
	{
	  if ( snd_config_get_integer(n, &device) < 0 )
	    {
	      SNDERR("Invalid type for %s", id);
	      err = -EINVAL;
	      goto error;
	    }
	} else if ( strcmp(id, "subdevice") == 0 )
	{
	  if ( snd_config_get_integer(n, &subdevice) < 0 )
	    {
	      SNDERR("Invalid type for %s", id);
	      err = -EINVAL;
	      goto error;
	    }
	} else if ( strcmp(id, "hookmode") == 0 )
	{
	  hookmode = snd_config_get_bool(n);
	}
    }
  if ( subdevice > 0 && device >= 0 )
    {
      sprintf(hwdev, "hw:%ld,%ld", device, subdevice);
    } else if ( device >= 0 )
    {
      sprintf(hwdev, "hw:%ld", device);
    }

  dspd_time_init();

  err = dspd_rclient_init(&dspd->client);
  if ( err < 0 )
    goto error;

  

  err = dspd_conn_new(NULL, &dspd->conn);
  if ( err < 0 )
    goto error;
  
  dspd_conn_set_event_flag_cb(dspd->conn, dspd_event_flags_changed, dspd);

  if ( hwdev[0] == 0 )
    {
      err = dspd_stream_ctl(dspd->conn,
			    0,
			    DSPD_DCTL_GET_DEFAULTDEV,
			    &dspd->stream,
			    sizeof(dspd->stream),
			    &defaultdev,
			    sizeof(defaultdev),
			    &br);
      if ( err != 0 || br != sizeof(defaultdev) )
	defaultdev = -1;
    }

  err = dspd_stream_ctl(dspd->conn,
			0,
			DSPD_DCTL_GET_OBJMASK_SIZE,
			NULL,
			0,
			&mask_size,
			sizeof(mask_size),
			&br);
  if ( err )
    goto error;

  mask = calloc(1, mask_size);
  if ( ! mask )
    {
      err = -errno;
      goto error;
    }
  mtype = DSPD_DCTL_ENUM_TYPE_SERVER;
  err = dspd_stream_ctl(dspd->conn,
			0,
			DSPD_DCTL_ENUMERATE_OBJECTS,
			&mtype,
			sizeof(mtype),
			mask,
			mask_size,
			&br);
  if ( err )
    goto error;

  bits = br * 8;
  for ( i = 0; i < bits; i++ )
    {
      if ( dspd_test_bit(mask, i) )
	{
	  dev = i;
	  err = dspd_stream_ctl(dspd->conn,
				-1,
				DSPD_SOCKSRV_REQ_REFSRV,
				&dev,
				sizeof(dev),
				&dspd->devinfo,
				sizeof(dspd->devinfo),
				&br);
	  if ( err == 0 && br == sizeof(dspd->devinfo) )
	    {
	      if ( dspd->devinfo.streams & dspd->stream )
		{
		  if ( hwdev[0] == 0 )
		    dspd->device = i;
		  if ( (defaultdev < 0 && hwdev[0] == 0) || 
		       (defaultdev == dspd->device && hwdev[0] == 0 ) || 
		       (hwdev[0] != 0 && strcmp(hwdev, dspd->devinfo.name) == 0) )
		    {
		      dspd->device = i;
		      if ( hookmode )
			{
			  if ( dspd->devinfo.desc[0] )
			    dspd->io.name = dspd->devinfo.desc;
			}
		      break;
		    }
		}
	    }
	}
    }


  if ( dspd->device < 0 )
    {
      err = -ENODEV;
      goto error;
    }

  if ( ! (dspd->devinfo.streams & dspd->stream) )
    {
      err = -EACCES;
      goto error;
    }

  err = dspd_stream_ctl(dspd->conn,
			-1,
			DSPD_SOCKSRV_REQ_NEWCLI,
			NULL,
			0,
			&dspd->client_stream,
			sizeof(dspd->client_stream),
			&br);

  if ( err < 0 )
    goto error;



  err = dspd_rclient_enable_pollfd(&dspd->client, 1);
  if ( err < 0 )
    goto error;

  free(mask);
  mask = NULL;
  err = snd_pcm_ioplug_create(&dspd->io, name, stream, mode);
  if (err < 0)
    goto error;


  *pcmp = dspd->io.pcm;

  err = dspd_hw_constraint(dspd);
  if ( err < 0 )
    goto error;
 
  dspd->io.private_data = dspd;
  snd_pcm_ioplug_reinit_status(&dspd->io);
  return 0;
  
 error:
  if ( *pcmp )
    {
      snd_pcm_ioplug_delete(&dspd->io);
      *pcmp = NULL;
    }
  free(mask);
  if ( dspd->conn )
    dspd_conn_delete(dspd->conn);
  free(dspd);
  assert(err != 0);
  if ( err > 0 )
    err *= -1;
  return err;
}

SND_PCM_PLUGIN_SYMBOL(dspd);