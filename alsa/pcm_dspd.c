/*
 *   PCM_DSPD - ALSA device emulation for DSPD
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

//Write some info to stderr for debugging
//#define DEBUG

#define MIN_ALSA_PERIODS 2
#define MAX_ALSA_BUFFER_BYTES (1024UL*1024UL)
#define MIN_ALSA_BUFFER_BYTES 128UL
#define MAX_ALSA_PERIODS 64UL
#define MAX_ALSA_PERIOD_BYTES 65536UL
typedef struct _snd_pcm_dspd {
  struct dspd_pcmcli *client;
  snd_pcm_uframes_t   appl_ptr;
  snd_pcm_ioplug_t    io;
  int32_t             stream;
  int                 nonblock;
  int32_t             start_threshold;
  bool                started;
  bool                xrun;
  bool                raw_channels;
  bool                raw_rate;
  bool                raw_formats;

  int32_t             alsa_dev;
  int32_t             alsa_subdev;
  int32_t             alsa_rate;
  int32_t             alsa_format;
  int32_t             dspd_index;
  char                dspd_name[64];
  char                dspd_desc[128];

  size_t              avail_min;
  uint32_t            max_latency;

  uint32_t            alsa_max_buftime;
  uint32_t            alsa_min_buftime;

  uint32_t            alsa_max_ptime;
  uint32_t            alsa_min_ptime;

  int32_t             current_channels;

  int32_t             epollfd;
} snd_pcm_dspd_t;
static const char *default_plugin_name = "ALSA <-> DSPD PCM I/O Plugin";
static int check_stream(snd_pcm_dspd_t *dspd)
{
  snd_pcm_uframes_t diff;
  int ret = 0;
  if ( dspd->io.appl_ptr != dspd->appl_ptr )
    {
      diff = dspd->io.appl_ptr - dspd->appl_ptr;
      int64_t i64 = (snd_pcm_sframes_t)diff;
      dspd->appl_ptr = dspd->io.appl_ptr;
      ret = dspd_pcmcli_set_appl_pointer(dspd->client, dspd->stream, true, i64);
      dspd_pcmcli_wait(dspd->client, dspd->stream, dspd->avail_min, true);
    }
  if ( ret == 0 && (dspd->io.nonblock != dspd->nonblock) )
    {
      dspd->nonblock = dspd->io.nonblock;
      ret = dspd_pcmcli_set_nonblocking(dspd->client, !!dspd->nonblock);
    }
  return ret;
}



static int dspd_alsa_start(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  ret = check_stream(dspd);
  if ( ret == 0 && dspd->started == false )
    {
      ret = dspd_pcmcli_start(dspd->client, dspd->stream, NULL, NULL);
      if ( ret == 0 )
	dspd->started = true;
    }
  return ret;
}

static int dspd_alsa_stop(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  ret = check_stream(dspd);
  if ( ret == 0 )
    {
      ret = dspd_pcmcli_stop(dspd->client, dspd->stream, NULL, NULL);
      if ( ret == 0 )
	dspd->started = false;
    }
  return ret;
}


static snd_pcm_sframes_t dspd_alsa_read_pcm(snd_pcm_ioplug_t *io,
					    const snd_pcm_channel_area_t *areas,
					    snd_pcm_uframes_t offset,
					    snd_pcm_uframes_t size)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  snd_pcm_sframes_t ret;
  char *buf = (char*)areas->addr + (areas->first + areas->step * offset) / 8;
  if ( dspd->started == false )
    {
      if ( size >= dspd->start_threshold )
	{
	  ret = dspd_pcmcli_start(dspd->client, dspd->stream, NULL, NULL);
	  if ( ret == 0 )
	    {
	      dspd->started = true;
	    } else
	    {
	      assert(ret < 0);
	      return ret;
	    }
	}
    }
  ret = check_stream(dspd);
  if ( ret == 0 )
    {
      ret = dspd_pcmcli_read_frames(dspd->client, buf, size);
      if ( ret > 0 )
	dspd->appl_ptr += ret;
    }
  if ( ret == -EPIPE )
    {
      ret = snd_pcm_ioplug_set_state(&dspd->io, SND_PCM_STATE_XRUN);
      dspd->xrun = true;
    }
  return ret;
}

static snd_pcm_sframes_t dspd_alsa_write_pcm(snd_pcm_ioplug_t *io,
					     const snd_pcm_channel_area_t *areas,
					     snd_pcm_uframes_t offset,
					     snd_pcm_uframes_t size)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  snd_pcm_sframes_t ret;
  ssize_t fill, avail;
  const char *buf = (const char*)areas->addr + (areas->first + areas->step * offset) / 8;
  int32_t err;
  ret = check_stream(dspd);
  if ( ret == 0 )
    {
      ret = dspd_pcmcli_write_frames(dspd->client, buf, size);
      if ( ret > 0 )
	dspd->appl_ptr += ret;
    }
  if ( dspd->started == false )
    {
      avail = dspd_pcmcli_avail(dspd->client, dspd->stream, NULL, NULL);
      if ( avail >= 0 )
	{
	  fill = io->buffer_size - avail;
	  if ( fill >= dspd->start_threshold )
	    {
	      err = dspd_pcmcli_start(dspd->client, dspd->stream, NULL, NULL);
	      if ( err == 0 )
		{
		  dspd->started = true;
		} else
		{
		  assert(ret < 0);
		  if ( ret == -EAGAIN || ret == 0 )
		    ret = err;
		}
	    }
	}
    }
  if ( ret == -EPIPE )
    ret = snd_pcm_ioplug_set_state(&dspd->io, SND_PCM_STATE_XRUN);

  return ret;
}

static snd_pcm_sframes_t dspd_pointer(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  snd_pcm_sframes_t ret;
  uint64_t hw, appl;  
  ret = check_stream(dspd);
  if ( ret == 0 )
    {
      if ( dspd->nonblock )
	ret = dspd_pcmcli_process_io(dspd->client, 0, 0);
      if ( ret == 0 )
	{
	  ret = dspd_pcmcli_avail(dspd->client, dspd->stream, &hw, &appl);
	  if ( ret == -EPIPE )
	    snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
	  ret = hw % io->buffer_size;
	}
    }
  return ret;
}


static int dspd_alsa_close(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  if ( dspd )
    {
      if ( dspd->epollfd >= 0 )
	{
	  close(dspd->epollfd);
	  dspd->epollfd = -1;
	}
      if ( dspd->client )
	{
	  dspd_pcmcli_delete(dspd->client);
	  dspd->client = NULL;
	}
      dspd->io.name = default_plugin_name;
      free(dspd);
      io->private_data = NULL;
    }
  return 0;
}

int dspd_alsa_hw_params(snd_pcm_ioplug_t *io,
			snd_pcm_hw_params_t *params)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  snd_pcm_format_t format = 0;
  unsigned int channels, rate;
  int dir;
  snd_pcm_uframes_t bufsize, fragsize;
  struct dspd_cli_params dparams;
  int32_t dfmt;
  ret = snd_pcm_hw_params_get_format(params, &format);
  if ( ret < 0 )
    return ret;
  

  if ( ! ((snd_pcm_format_float(format) > 0 || snd_pcm_format_linear(format) > 0) ||
	  snd_pcm_format_unsigned(format) < 0) )
    return -EINVAL;

  dfmt = dspd_pcm_build_format(snd_pcm_format_width(format), 
			       snd_pcm_format_size(format, 1),
			       snd_pcm_format_unsigned(format) > 0,
			       snd_pcm_format_big_endian(format) > 0,
			       snd_pcm_format_float(format));

  if ( dfmt < 0 || (size_t)snd_pcm_format_size(format, 1) != dspd_get_pcm_format_size(dfmt) )
    return -EINVAL;

  

  ret = snd_pcm_hw_params_get_channels(params, &channels);
  if ( ret < 0 )
    return ret;
  dir = 0;
  ret = snd_pcm_hw_params_get_rate(params, &rate, &dir);
  if ( ret < 0 )
    return ret;
  ret = snd_pcm_hw_params_get_buffer_size(params, &bufsize);
  if ( ret < 0 )
    return ret;
  dir = 0;
  ret = snd_pcm_hw_params_get_period_size(params, &fragsize, &dir);
  if ( ret < 0 )
    return ret;

  ret = dspd_pcmcli_hw_params_default(dspd->client, &dparams);
  if ( ret < 0 )
    return ret;




  if ( (ret = dspd_pcmcli_hw_params_set_format(dspd->client, &dparams, dfmt)) != dfmt ||
       (ret = dspd_pcmcli_hw_params_set_channels(dspd->client, &dparams, dspd->stream, channels)) != channels ||
       (ret = dspd_pcmcli_hw_params_set_rate(dspd->client, &dparams, rate)) < 0 ||
       (ret = dspd_pcmcli_hw_params_set_bufsize(dspd->client, &dparams, bufsize)) < 0 ||
       (ret = dspd_pcmcli_hw_params_set_fragsize(dspd->client, &dparams, fragsize)) < 0 )
    return ret;

  //Try for a smaller server side latency (real block size) because most apps don't 
  //handle 2 fragments very well.
  if ( (bufsize / fragsize) == 2 )
    (void)dspd_pcmcli_hw_params_set_latency(dspd->client, &dparams, fragsize/2);
  

  if ( dspd->max_latency > 0 )
    {
      int32_t l = (dspd->max_latency * 1000) / (1000000000 / dparams.rate);
      if ( dparams.latency > l )
	{
	  ret = dspd_pcmcli_hw_params_set_latency(dspd->client, &dparams, l);
	  

	  if ( ret < 0 )
	    return ret;
	}
    }
  
  
  ret = dspd_pcmcli_set_hwparams(dspd->client, &dparams, NULL, NULL, true);
#ifdef DEBUG
  dspd_dump_params(&dparams, stderr);
#endif
  if ( ret == 0 )
    {
      ret = dspd_pcmcli_get_hwparams(dspd->client, &dparams);
      if ( ret == 0 )
	{
	  ret = snd_pcm_hw_params_set_rate(dspd->io.pcm, params, dparams.rate, 0);

	  if ( ret == 0 )
	    {
	      ret = snd_pcm_hw_params_set_buffer_size(dspd->io.pcm, params, dparams.bufsize);

	      if ( ret == 0 )
		{
		  ret = snd_pcm_hw_params_set_period_size(dspd->io.pcm, params, dparams.fragsize, 0);
		  if ( ret )
		    {
		      ret = snd_pcm_hw_params_set_period_size(dspd->io.pcm, params, dparams.fragsize, -1);
		      if ( ret )
			{
			  ret = snd_pcm_hw_params_set_period_size(dspd->io.pcm, params, dparams.fragsize, 1);
			  if ( ret == -EINVAL )
			    ret = 0; //It will probably still work.
			}
		    }
		}
	    }
	}
    }
  if ( ret == 0 )
    dspd->current_channels = channels;
  return ret;
}

int dspd_alsa_sw_params(snd_pcm_ioplug_t *io,
			snd_pcm_sw_params_t *params)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret;
  snd_pcm_uframes_t frames;
  struct dspd_rclient_swparams swp;
  snd_pcm_uframes_t boundary;
  memset(&swp, 0, sizeof(swp));
  ret = snd_pcm_sw_params_get_start_threshold(params, &frames);
  if ( ret == 0 && snd_pcm_sw_params_get_boundary(params, &boundary) == 0 )
    {
      dspd->start_threshold = frames;
      swp.start_threshold = frames;
      ret = snd_pcm_sw_params_get_avail_min(params, &frames);
      if ( ret == 0 )
	{
	  swp.avail_min = frames;
	  ret = snd_pcm_sw_params_get_stop_threshold(params, &frames);
	  if ( ret == 0 )
	    {
	      (void)dspd_pcmcli_set_no_xrun(dspd->client, frames == boundary);
	      ret = dspd_pcmcli_set_poll_threshold(dspd->client, swp.avail_min);
	      if ( ret > 0 )
		{
		  swp.stop_threshold = frames;
		  swp.avail_min = ret;
		  dspd->avail_min = swp.avail_min;
		  ret = dspd_pcmcli_set_swparams(dspd->client, &swp, true, NULL, NULL);
		}
	    }
	}
    }
  return ret;
}

static int dspd_alsa_prepare(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret = dspd_pcmcli_prepare(dspd->client, NULL, NULL);
  if ( ret == 0 )
    {
      dspd->appl_ptr = 0;
      dspd->started = false;
      dspd->xrun = false;
    }
  return ret;
}

static int dspd_alsa_drain(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret = dspd_pcmcli_drain(dspd->client);
  if ( ret == 0 )
    dspd->started = false;
  return ret;
}

static int dspd_alsa_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int64_t fr;
  int ret = dspd_pcmcli_delay(dspd->client, dspd->stream, &fr);
  *delayp = fr;
  if ( *delayp < 0 )
    *delayp = 0;
  return ret;
}

static int dspd_alsa_pause(snd_pcm_ioplug_t *io, int enable)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int32_t ret, len;
  ret = check_stream(dspd);
  if ( ret == 0 )
    {
      if ( enable != 0 )
	{
	  ret = dspd_pcmcli_avail(dspd->client, dspd->stream, NULL, NULL);
	  if ( ret >= 0 )
	    {
	      len = io->buffer_size - ret;
	      if ( len >= dspd->start_threshold )
		{
		  ret = dspd_pcmcli_pause(dspd->client, 1, NULL, NULL);
		  if ( ret == 0 )
		    dspd->started = true;
		} else
		{
		  ret = 0;
		}
	    }
	} else
	{
	  ret = dspd_pcmcli_pause(dspd->client, 0, NULL, NULL);
	  if ( ret == 0 )
	    dspd->started = false;
	}
    }

  return ret;
}

static int dspd_alsa_resume(snd_pcm_ioplug_t *io)
{
  return dspd_alsa_pause(io, 0);
}

static int dspd_alsa_poll_revents(snd_pcm_ioplug_t *io,
				  struct pollfd *pfd,
				  unsigned int nfds,
				  unsigned short *revents)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int32_t ret, re = 0;
  struct pollfd *p;
  int32_t e, nf;
  int i, j;
  struct epoll_event *ep;
  ret = check_stream(dspd);
  if ( ret == 0 )
    {
      if ( dspd->epollfd >= 0 )
	{
	  if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
	    e = POLLOUT;
	  else
	    e = POLLIN;
	  ret = dspd_pcmcli_pollfd_count(dspd->client);
	  if ( ret < 0 )
	    {
	      re = POLLERR;
	      goto out;
	    }
	  nf = ret;
	  p = alloca(sizeof(*p) * nf);
	  ep = alloca(sizeof(*ep) * nf);
	  ret = dspd_pcmcli_get_pollfd(dspd->client, p, nf, e);
	  if ( ret < 0 )
	    {
	      re = POLLERR;
	      goto out;
	    }
	  while ( (ret = epoll_wait(dspd->epollfd, ep, nf, 0)) < 0 )
	    {
	      if ( errno != EINTR )
		{
		  re = POLLERR;
		  goto out;
		}
	    }
	  for ( i = 0; i < ret; i++ )
	    {
	      for ( j = 0; j < nf; j++ )
		{
		  if ( ep[i].data.fd == p[i].fd )
		    {
		      p[i].revents = ep[i].events;
		      break;
		    }
		}
	    }
	  nfds = ret;
	  pfd = p;
	}

      ret = dspd_pcmcli_pollfd_revents(dspd->client, pfd, nfds, &re);
      if ( re & POLLHUP )
	re |= POLLERR;
    } else
    {
      re |= POLLERR;
    }

 out:
  *revents = re;
  return ret;
}

static int dspd_alsa_poll_descriptors(snd_pcm_ioplug_t *io,
				      struct pollfd *pfd,
				      unsigned int space)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int32_t ret = -EINVAL;
  int32_t events;
  if ( space > 0 )
    {
      if ( dspd->epollfd >= 0 )
	{
	  pfd->fd = dspd->epollfd;
	  pfd->events = POLLIN;
	  pfd->revents = 0;
	  ret = 1;
	} else
	{
	  if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
	    events = POLLOUT;
	  else
	    events = POLLIN;
	  ret = dspd_pcmcli_get_pollfd(dspd->client, pfd, space, events);
	}
    }
  return ret;
}

static int dspd_alsa_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  int ret = 1;
  if ( dspd->epollfd < 0 )
    ret = dspd_pcmcli_pollfd_count(dspd->client);
  return ret;
}

struct dspd_alsa_chmap {
  snd_pcm_chmap_t map;
  unsigned int    channels[SND_CHMAP_LAST+1];
};

static snd_pcm_chmap_t *dspd_alsa_get_chmap(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  struct dspd_pcm_chmap_container map;
  struct dspd_alsa_chmap *result = NULL;
  int32_t ret;
  size_t br;
  size_t i;
  if ( dspd->current_channels > 0 )
    {
      ret = dspd_pcmcli_ctl(dspd->client,
			    dspd_pcmcli_get_client_index(dspd->client, dspd->stream),
			    DSPD_SCTL_CLIENT_GETCHANNELMAP,
			    &dspd->stream,
			    sizeof(dspd->stream),
			    &map,
			    sizeof(map),
			    &br);
    } else
    {
      ret = dspd_pcmcli_ctl(dspd->client,
			    dspd_pcmcli_get_device_index(dspd->client, dspd->stream),
			    DSPD_SCTL_SERVER_GETCHANNELMAP,
			    &dspd->stream,
			    sizeof(dspd->stream),
			    &map,
			    sizeof(map),
			    &br);
    }
  if ( ret == 0 )
    {
      if ( br > sizeof(map.map) && (map.map.flags & DSPD_CHMAP_MATRIX) == 0 )
	{
	  result = calloc(1, sizeof(struct dspd_alsa_chmap));
	  if ( result )
	    {
	      result->map.channels = map.map.count;
	      for ( i = 0; i < map.map.count; i++ )
		{
		  result->map.pos[i] = map.map.pos[i]& DSPD_CHMAP_POSITION_MASK;
		  if ( map.map.pos[i] & DSPD_CHMAP_PHASE_INVERSE )
		    result->map.pos[i] |= SND_CHMAP_PHASE_INVERSE;
		  if ( map.map.pos[i] & DSPD_CHMAP_DRIVER_SPEC )
		    result->map.pos[i] |= SND_CHMAP_DRIVER_SPEC;
		}
	    }
	} else
	{
	  errno = EPROTO;
	}
    }
  return &result->map;
}


static snd_pcm_chmap_query_t **dspd_alsa_query_chmaps(snd_pcm_ioplug_t *io)
{
  snd_pcm_dspd_t *dspd = io->private_data;
  const struct dspd_device_stat *info = dspd_pcmcli_device_info(dspd->client, dspd->stream);
  const struct dspd_cli_params *params;
  snd_pcm_chmap_query_t **query = NULL;
  struct dspd_pcm_chmap_container map;
  int32_t ret;
  size_t i, o, n, br, j;
  if ( ! info )
    return NULL; //Should not happen
  if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
    params = &info->playback;
  else
    params = &info->capture;
  if ( params->channels == 1 )
    n = 2U;
  else
    n = params->channels;
  query = calloc(n+1UL, sizeof(*query));
  if ( query )
    {
      for ( i = 1, o = 0; i <= n; i++ )
	{
	  ret = dspd_pcmcli_ctl(dspd->client,
				dspd_pcmcli_get_device_index(dspd->client, dspd->stream),
				DSPD_SCTL_SERVER_GETCHANNELMAP,
				&dspd->stream,
				sizeof(dspd->stream),
				&map,
				sizeof(map),
				&br);
	  if ( ret == 0 && br > sizeof(map.map) )
	    {
	      query[o] = calloc(1, sizeof(snd_pcm_chmap_query_t));
	      if ( ! query[o] )
		{
		  for ( j = 0; j < o; i++ )
		    {
		      free(query[j]);
		      query[j] = NULL;
		    }
		  break;
		}
	      for ( j = 0; j < map.map.count; j++ )
		{
		  query[o]->map.pos[j] = map.map.pos[j] & DSPD_CHMAP_POSITION_MASK;
		  if ( map.map.pos[j] & DSPD_CHMAP_PHASE_INVERSE )
		    query[o]->map.pos[j] |= SND_CHMAP_PHASE_INVERSE;
		  if ( map.map.pos[j] & DSPD_CHMAP_DRIVER_SPEC )
		    query[o]->map.pos[j] |= SND_CHMAP_DRIVER_SPEC;
		}
	      query[o]->type = SND_CHMAP_TYPE_VAR;
	    }
	}
      if ( query[0] == NULL )
	{
	  free(query);
	  query = NULL;
	}
    }
  return query;
}

static int dspd_alsa_set_chmap(snd_pcm_ioplug_t *io, const snd_pcm_chmap_t *map)
{
  struct dspd_pcm_chmap_container m;
  snd_pcm_dspd_t *dspd = io->private_data;
  size_t i;
  size_t br;
  int32_t ret;
  memset(&m, 0, sizeof(m));
  m.map.flags = dspd->stream;
  m.map.count = map->channels;
  for ( i = 0; i < map->channels; i++ )
    {
      m.map.pos[i] = map->pos[i] & DSPD_CHMAP_POSITION_MASK;
      if ( map->pos[i] & SND_CHMAP_PHASE_INVERSE )
	m.map.pos[i] |= DSPD_CHMAP_PHASE_INVERSE;
      if ( map->pos[i] & SND_CHMAP_DRIVER_SPEC )
	m.map.pos[i] |= DSPD_CHMAP_DRIVER_SPEC;
    }
  ret = dspd_pcmcli_ctl(dspd->client,
			dspd_pcmcli_get_client_index(dspd->client, dspd->stream),
			DSPD_SCTL_CLIENT_SETCHANNELMAP,
			&m,
			sizeof(m),
			NULL,
			0,
			&br);
  if ( ret == 0 && dspd->current_channels == 0 )
    dspd->current_channels = m.map.count;
  return ret;
}



static const snd_pcm_ioplug_callback_t dspd_playback_callback = {
  .start = dspd_alsa_start,
  .stop = dspd_alsa_stop,
  .transfer = dspd_alsa_write_pcm,
  .pointer = dspd_pointer,
  .close = dspd_alsa_close,
  .hw_params = dspd_alsa_hw_params,
  .sw_params = dspd_alsa_sw_params,
  .prepare = dspd_alsa_prepare,
  .drain = dspd_alsa_drain,
  .delay = dspd_alsa_delay,
  .pause = dspd_alsa_pause,
  .resume = dspd_alsa_resume,
  .poll_revents = dspd_alsa_poll_revents,
  .poll_descriptors_count = dspd_alsa_poll_descriptors_count,
  .poll_descriptors = dspd_alsa_poll_descriptors,
  .get_chmap = dspd_alsa_get_chmap,
  .set_chmap = dspd_alsa_set_chmap,
  .query_chmaps = dspd_alsa_query_chmaps
};

static const snd_pcm_ioplug_callback_t dspd_capture_callback = {
  .start = dspd_alsa_start,
  .stop = dspd_alsa_stop,
  .transfer = dspd_alsa_read_pcm,
  .pointer = dspd_pointer,
  .close = dspd_alsa_close,
  .hw_params = dspd_alsa_hw_params,
  .sw_params = dspd_alsa_sw_params,
  .prepare = dspd_alsa_prepare,
  .drain = dspd_alsa_drain,
  .delay = dspd_alsa_delay,
  .pause = dspd_alsa_pause,
  .resume = dspd_alsa_resume,
  .poll_revents = dspd_alsa_poll_revents,
  .poll_descriptors_count = dspd_alsa_poll_descriptors_count,
  .poll_descriptors = dspd_alsa_poll_descriptors,
  .get_chmap = dspd_alsa_get_chmap,
  .set_chmap = dspd_alsa_set_chmap,
  .query_chmaps = dspd_alsa_query_chmaps
};


static int dspd_hw_constraint(snd_pcm_dspd_t *dspd)
{
  const snd_pcm_access_t access_list[] = {
    SND_PCM_ACCESS_RW_INTERLEAVED,
    SND_PCM_ACCESS_MMAP_INTERLEAVED
  };
  const unsigned int raw_formats[] = {
    SND_PCM_FORMAT_S32_LE,
    SND_PCM_FORMAT_S32_BE,
    SND_PCM_FORMAT_U32_LE,
    SND_PCM_FORMAT_U32_BE,
    SND_PCM_FORMAT_FLOAT_LE,
    SND_PCM_FORMAT_FLOAT_BE,
  };
  
  unsigned int bytes_list[32] = { 0 };
  unsigned int formats[SND_PCM_FORMAT_LAST+1];
  int err, i, count, dfmt, n;
  const struct pcm_conv *conv;
  size_t min_fragsize, max_fragsize, maxfrags, min_framesize, max_framesize;
  struct dspd_cli_params params;
  size_t c;
  size_t max_buffer_bytes;
  err = dspd_pcmcli_hw_params_default(dspd->client, &params);
  if ( err < 0 )
    return err;
  n = dspd_pcmcli_hw_params_get_channels(dspd->client, &params, dspd->stream);
  if ( n < 0 )
    return n;

  //n channels * 16 bit sample size * 1 second max
  //The idea is to place an upper limit on the buffer size so that most broken apps will
  //negotiate something that will be in range.  Most apps use 16 bit samples and most devices
  //are stereo.  The rate is equal to 1 second of samples.
  max_buffer_bytes = n * sizeof(uint16_t) * params.rate;
  

  err = snd_pcm_ioplug_set_param_list(&dspd->io,
				      SND_PCM_IOPLUG_HW_ACCESS,
				      2,
				      access_list);
  if ( err < 0 )
    return err;
  count = 0;
  for ( i = 0; i <= SND_PCM_FORMAT_LAST; i++ )
    {
      bool isfloat = snd_pcm_format_float(i) > 0;
      bool islinear =  snd_pcm_format_linear(i) > 0;
      if ( ! (isfloat || islinear) )
	continue;
   
      if ( snd_pcm_format_unsigned(i) < 0 && isfloat == false )
	continue;

      dfmt = dspd_pcm_build_format(snd_pcm_format_width(i), 
				   snd_pcm_format_size(i, 1),
				   snd_pcm_format_unsigned(i) == 1,
				   snd_pcm_format_big_endian(i) == 1,
				   snd_pcm_format_float(i) == 1 );
      if ( dfmt >= 0 )
	{
	  conv = dspd_getconv(dfmt);
	  if ( conv != NULL )
	    {
	      if ( (dspd->stream == DSPD_PCM_SBIT_PLAYBACK && conv->tofloat32 != NULL) ||
		   (dspd->stream == DSPD_PCM_SBIT_CAPTURE && conv->fromfloat32 != NULL) )
		{
		  formats[count] = i;
		  count++;
		}
	    }
	}
    }

  if ( dspd->alsa_format >= 0 )
    {
      min_framesize = snd_pcm_format_size(dspd->alsa_format, 1);
      max_framesize = min_framesize;
      err = snd_pcm_ioplug_set_param_list(&dspd->io,
					  SND_PCM_IOPLUG_HW_FORMAT,
					  1,
					  (unsigned*)&dspd->alsa_format);
    } else if ( dspd->raw_formats )
    {
      min_framesize = sizeof(int32_t);
      max_framesize = min_framesize;
      err = snd_pcm_ioplug_set_param_list(&dspd->io,
					  SND_PCM_IOPLUG_HW_FORMAT,
					  ARRAY_SIZE(raw_formats),
					  raw_formats);
    } else
    {
      min_framesize = sizeof(uint8_t);
      max_framesize = sizeof(uint64_t);
      err = snd_pcm_ioplug_set_param_list(&dspd->io,
					  SND_PCM_IOPLUG_HW_FORMAT,
					  count,
					  formats);
    }
  if ( err < 0 )
    return err;
  
  if ( dspd->alsa_rate > 0 )
    {
      err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					    SND_PCM_IOPLUG_HW_RATE,
					    dspd->alsa_rate,
					    dspd->alsa_rate);
    } else if ( dspd->raw_rate )
    {
      err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					    SND_PCM_IOPLUG_HW_RATE,
					    params.rate,
					    params.rate);

    } else
    {
      err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					    SND_PCM_IOPLUG_HW_RATE,
					    8000,
					    384000);
    }
  if ( err < 0 )
    return err;
  
  

  min_fragsize = params.min_latency;
  max_fragsize = params.max_latency;
  if ( params.xflags & DSPD_CLI_XFLAG_LATENCY_NS )
    {
      min_fragsize /= 1000000000 / params.rate;
      max_fragsize /= 1000000000 / params.rate;
    }
  maxfrags = params.rate / min_fragsize;
  if ( maxfrags > MAX_ALSA_PERIODS )
    maxfrags = MAX_ALSA_PERIODS;
  if ( params.xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
    {
      if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
	c = DSPD_CLI_PCHAN(params.channels);
      else
	c = DSPD_CLI_CCHAN(params.channels);
    } else
    {
      c = params.channels;
    }
  if ( c == 1 && dspd->raw_channels == false )
    c = 2;

  if ( dspd->raw_channels )
    {
      err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					    SND_PCM_IOPLUG_HW_CHANNELS,
					    c,
					    c);
      if ( err < 0 )
	return err;
      max_framesize *= c;
      min_framesize *= c;
    } else
    {
      if ( c == 1 )
	c = 2;
      err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					    SND_PCM_IOPLUG_HW_CHANNELS,
					    1,
					    c);
      if ( err < 0 )
	return err;
      max_framesize *= c;
    }

  min_fragsize *= min_framesize;
  max_fragsize *= max_framesize;

  if ( max_fragsize > MAX_ALSA_PERIOD_BYTES )
    max_fragsize = MAX_ALSA_PERIOD_BYTES;
  if ( min_fragsize > max_fragsize )
    min_fragsize = max_fragsize;

  if ( dspd->raw_channels && dspd->raw_formats && dspd->raw_rate )
    {
      n = 0;
      for ( i = 1; i < 32; i *= 2 )
	{
	  bytes_list[n] = i * min_fragsize;
	  if ( bytes_list[n] > max_fragsize )
	    break;
	  n++;
	}
      err = snd_pcm_ioplug_set_param_list(&dspd->io, 
					  SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					  n,
					  bytes_list);
    } else
    {
      err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					    SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					    min_fragsize,
					    max_fragsize);
    }

  if ( err < 0 )
    return err;

  err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					SND_PCM_IOPLUG_HW_PERIODS,
					MIN_ALSA_PERIODS,
					maxfrags);


  if ( err < 0 )
    return err;

  size_t maxbuf = max_fragsize * maxfrags;

  

  if ( maxbuf > MAX_ALSA_BUFFER_BYTES )
    maxbuf = MAX_ALSA_BUFFER_BYTES;
  if ( dspd->raw_formats == false && maxbuf > max_buffer_bytes )
    maxbuf = max_buffer_bytes;
  size_t minbuf = min_fragsize * MIN_ALSA_PERIODS;
  if ( minbuf < MIN_ALSA_BUFFER_BYTES )
    minbuf = MIN_ALSA_BUFFER_BYTES;

  err = snd_pcm_ioplug_set_param_minmax(&dspd->io,
					SND_PCM_IOPLUG_HW_BUFFER_BYTES,
					minbuf,
					maxbuf);
  if ( err < 0 )
    return err;

  dspd->alsa_min_buftime = (params.min_latency * MIN_ALSA_PERIODS) / 1000;
  dspd->alsa_max_buftime = 1000000;

  dspd->alsa_min_ptime = params.min_latency / 1000;
  dspd->alsa_max_ptime = params.max_latency / 1000;

  return 0;
}
static int32_t dspd_alsa_select_cb(void *arg, 
				   int32_t streams, 
				   int32_t index,
				   const struct dspd_device_stat *info, 
				   struct dspd_pcmcli *client)
{
  snd_pcm_dspd_t *dspd = arg;
  char buf[64];
  const char *tok;
  char *ptr;
  int32_t dev = -1, subdev = -1, i;
  if ( dspd->dspd_name[0] != 0 && strcmp(dspd->dspd_name, info->name) != 0 )
    return SELECT_DEV_REJECT;
  if ( dspd->dspd_desc[0] != 0 && strcmp(dspd->dspd_desc, info->desc) != 0 )
    return SELECT_DEV_REJECT;
  if ( dspd->dspd_index >= 0 && index != dspd->dspd_index )
    return SELECT_DEV_REJECT;
  if ( dspd->alsa_dev < 0 && (info->flags & streams) )
    return SELECT_DEV_OK_ABORT; //Found default device

  ptr = strchr(info->name, ':');
  if ( ! ptr )
    return SELECT_DEV_REJECT;
  strlcpy(buf, &ptr[1], sizeof(buf));
  i = 0;
  for ( tok = strtok_r(buf, ",", &ptr); tok; tok = strtok_r(NULL, ",", &ptr) )
    {
      if ( i == 0 )
	{
	  if ( dspd_strtoi32(tok, &dev, 10) < 0 && dspd->alsa_dev >= 0 )
	    return SELECT_DEV_REJECT;
	} else if ( i == 1 && dspd->alsa_subdev >= 0 )
	{
	   if ( dspd_strtoi32(tok, &subdev, 10) < 0 )
	    return SELECT_DEV_REJECT;
	}
      i++;
    }
  if ( dev == dspd->alsa_dev && subdev == dspd->alsa_subdev )
    return SELECT_DEV_OK_ABORT;
  return SELECT_DEV_REJECT;
}

#define cfg_error(_e,_v) {fprintf(stderr, "Error %d for option '%s'", _e, _v);goto out;}

SND_PCM_PLUGIN_DEFINE_FUNC(dspd)
{
  snd_pcm_dspd_t *dspd;
  int ret;
  snd_config_iterator_t cfgi, next;
  long int ival;
  const char *sval;
  char fallback[32] = { 0 };
  *pcmp = NULL;
  dspd = calloc(1, sizeof(*dspd));
  if ( ! dspd )
    return -ENOMEM;
  dspd->alsa_rate = -1;
  dspd->alsa_format = -1;
  dspd->alsa_dev = -1;
  dspd->alsa_subdev = -1;
  dspd->dspd_index = -1;
  dspd->epollfd = -1;
  dspd->io.nonblock = !!(mode & SND_PCM_NONBLOCK);
  dspd->nonblock = dspd->io.nonblock;
  dspd->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
  dspd->io.version = SND_PCM_IOPLUG_VERSION;
  dspd->io.name = default_plugin_name;
  dspd->io.poll_fd = -1;
  dspd->io.mmap_rw = 0;

  dspd->io.callback = stream == SND_PCM_STREAM_PLAYBACK ?
    &dspd_playback_callback : &dspd_capture_callback;
  dspd->stream = stream == SND_PCM_STREAM_PLAYBACK ?
    DSPD_PCM_SBIT_PLAYBACK : DSPD_PCM_SBIT_CAPTURE;
  dspd_time_init();

  

  

  snd_config_for_each(cfgi, next, conf) 
    {
      snd_config_t *n = snd_config_iterator_entry(cfgi);
      const char *key;
      if (snd_config_get_id(n, &key) < 0)
	continue;
      if ( strcmp(key, "raw") == 0 || strcmp(key, "hookmode") == 0 )
	{
	  ret = snd_config_get_bool(n);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->raw_channels = ret;
	      dspd->raw_rate = ret;
	      dspd->raw_formats = ret;
	    }
	} else if ( strcmp(key, "raw_channels") == 0 )
	{
	  ret = snd_config_get_bool(n);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->raw_channels = ret;
	    }
	} else if ( strcmp(key, "raw_rate") == 0 )
	{
	  ret = snd_config_get_bool(n);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->raw_rate = ret;
	    }
	} else if ( strcmp(key, "raw_formats") == 0 )
	{
	  ret = snd_config_get_bool(n);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->raw_formats = ret;
	    }
	} else if ( strcmp(key, "card") == 0 || strcmp(key, "device") == 0 )
	{
	  ret = snd_config_get_integer(n, &ival);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->alsa_dev = ival;
	    }
	} else if ( strcmp(key, "subdevice") == 0 )
	{
	  ret = snd_config_get_integer(n, &ival);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->alsa_subdev = ival;
	    }
	} else if ( strcmp(key, "nonblock") == 0 )
	{
	  ret = snd_config_get_bool(n);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else if ( ret == 1 )
	    {
	      mode |= SND_PCM_NONBLOCK;
	    }
	} else if ( strcmp(key, "rate") == 0 )
	{
	  ret = snd_config_get_integer(n, &ival);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->alsa_rate = ival;
	    }
	} else if ( strcmp(key, "format") == 0 )
	{
	  ret = snd_config_get_integer(n, &ival);
	  if ( ret < 0 )
	    {
	      ret = snd_config_get_string(n, &sval);
	      if ( ret < 0 )
		{
		  cfg_error(ret, key);
		}
	      ival = snd_pcm_format_value(sval);
	    }
	  if ( snd_pcm_format_name(ival) == NULL )
	    {
	      ret = -EINVAL;
	      cfg_error(ret, key);
	    }
	  dspd->alsa_format = ival;
	} else if ( strcmp(key, "index") == 0 )
	{
	  ret = snd_config_get_integer(n, &ival);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->dspd_index = ival;
	    }
	} else if ( strcmp(key, "name") == 0 )
	{
	  ret = snd_config_get_string(n, &sval);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else if ( sval != NULL )
	    {
	      if ( strlcpy(dspd->dspd_name, sval, sizeof(dspd->dspd_name)) >= sizeof(dspd->dspd_name) )
		{
		  ret = -ENAMETOOLONG;
		  cfg_error(ret, key);
		}
	    }
	} else if ( strcmp(key, "desc") == 0 )
	{
	  ret = snd_config_get_string(n, &sval);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else if ( sval != NULL )
	    {
	      if ( strlcpy(dspd->dspd_desc, sval, sizeof(dspd->dspd_desc)) >= sizeof(dspd->dspd_desc) )
		{
		  ret = -ENAMETOOLONG;
		  cfg_error(ret, key);
		}
	    }
	} else if ( strcmp(key, "max_latency") == 0 )
	{
	  ret = snd_config_get_integer(n, &ival);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else
	    {
	      dspd->max_latency = ival;
	    }
	} else if ( strcmp(key, "fallback") == 0 )
	{
	  ret = snd_config_get_string(n, &sval);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else if ( sval != NULL )
	    {
	      if ( strlcpy(fallback, sval, sizeof(fallback)) >= sizeof(fallback) )
		{
		  fallback[0] = 0;
		  ret = -ENAMETOOLONG;
		  goto out;
		}
	    }
	} else if ( strcmp(key, "singlefd") == 0 )
	{
	  //Support applications that don't like multiple pollfds.  Any app that needs
	  //this has a bug.
	  ret = snd_config_get_bool(n);
	  if ( ret < 0 )
	    {
	      cfg_error(ret, key);
	    } else if ( ret == 1 )
	    {
#ifdef EPOLL_CLOEXEC
	      dspd->epollfd = epoll_create1(EPOLL_CLOEXEC);
#else
	      dspd->epollfd = epoll_create(2);
#endif
	      if ( dspd->epollfd < 0 )
		{
		  ret = -errno;
		  goto out;
		}
	    }

	}
    }


  ret = dspd_pcmcli_new(&dspd->client, dspd->stream, 0);
  if ( ret < 0 )
    goto out;
  dspd_pcmcli_set_nonblocking(dspd->client, dspd->io.nonblock);


  sval = getenv("SND_PCM_DSPD_MAX_LATENCY");
  if ( sval )
    {
      if ( dspd_strtol(sval, &ival, 10) == 0 )
	dspd->max_latency = ival;
    }


  ret = dspd_pcmcli_open_device(dspd->client, NULL, dspd_alsa_select_cb, dspd);

  if ( ret < 0 )
    goto out;
  ret = dspd_pcmcli_set_info(dspd->client, NULL, NULL, NULL);
  if ( ret < 0 )
    goto out;

  const struct dspd_device_stat *info = dspd_pcmcli_device_info(dspd->client, dspd->stream);
  
  if ( info )
    {
      strlcpy(dspd->dspd_desc, info->desc, sizeof(dspd->dspd_desc));
      dspd->io.name = dspd->dspd_desc;
    }

  if ( dspd->epollfd >= 0 )
    {
      struct epoll_event evt;
      struct pollfd *pfd;
      int i, nfd, e;
      ret = dspd_pcmcli_pollfd_count(dspd->client);
      if ( ret < 0 )
	goto out;
      pfd = alloca(sizeof(*pfd) * ret);
      memset(pfd, 0, sizeof(*pfd) * ret);
      memset(&evt, 0, sizeof(evt));
      if ( dspd->stream == DSPD_PCM_SBIT_PLAYBACK )
	e = POLLOUT;
      else
	e = POLLIN;
      ret = dspd_pcmcli_get_pollfd(dspd->client, pfd, ret, e);
      if ( ret < 0 )
	goto out;
      nfd = ret;
      for ( i = 0; i < nfd; i++ )
	{
	  evt.data.fd = pfd[i].fd;
	  evt.events = pfd[i].events;
	  ret = epoll_ctl(dspd->epollfd, EPOLL_CTL_ADD, pfd[i].fd, &evt);
	  if ( ret < 0 )
	    {
	      ret = -errno;
	      goto out;
	    }
	}
    }

  ret = snd_pcm_ioplug_create(&dspd->io, default_plugin_name, stream, mode);
  
  if ( ret == 0 )
    {
      dspd->io.private_data = dspd;
      *pcmp = dspd->io.pcm;
      ret = dspd_hw_constraint(dspd);
     

    }
  


 out:
  if ( ret < 0 )
    {
      
      if ( *pcmp )
	{
	  snd_pcm_ioplug_delete(&dspd->io);
	  *pcmp = NULL;
	  dspd = NULL;
	}
      if ( dspd != NULL )
	{
	  if ( strcmp(fallback, "plughw") == 0 || strcmp(fallback, "hw") == 0 )
	    {
	      size_t len = strlen(fallback);
	      if ( dspd->alsa_dev >= 0 )
		{
		  if ( dspd->alsa_subdev >= 0 )
		    sprintf(&fallback[len], ":%d,%d", dspd->alsa_dev, dspd->alsa_subdev);
		  else
		    sprintf(&fallback[len], ":%d", dspd->alsa_dev);
		}
	    }
	  if ( dspd->client )
	    dspd_pcmcli_delete(dspd->client);
	  if ( dspd->epollfd >= 0 )
	    close(dspd->epollfd);
	  free(dspd);
	  if ( fallback[0] )
	    ret = snd_pcm_open_fallback(pcmp, root, fallback, name, stream, mode);
	}
    }

  return ret;
}

SND_PCM_PLUGIN_SYMBOL(dspd);
