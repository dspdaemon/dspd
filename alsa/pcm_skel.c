/*
 *   PCM_SKEL - Skeleton for ioplug
 *
 *   This file is just an example
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
#define ARRAY_SIZE(ary)	(sizeof(ary)/sizeof(ary[0]))
typedef struct _snd_pcm_skel {
  int data;
} snd_pcm_skel_t;

static int skel_alsa_start(snd_pcm_ioplug_t *io)
{
  snd_pcm_skel_t *skel = io->private_data;
  return 0;
}

static int skel_stop(snd_pcm_ioplug_t *io)
{
  snd_pcm_skel_t *skel = io->private_data;
  return 0;
}


static snd_pcm_sframes_t skel_read_pcm(snd_pcm_ioplug_t *io,
				       const snd_pcm_channel_area_t *areas,
				       snd_pcm_uframes_t offset,
				       snd_pcm_uframes_t size)
{
  snd_pcm_skel_t *skel = io->private_data;
  return size;
}

static snd_pcm_sframes_t skel_write_pcm(snd_pcm_ioplug_t *io,
					const snd_pcm_channel_area_t *areas,
					snd_pcm_uframes_t offset,
					snd_pcm_uframes_t size)
{
  snd_pcm_skel_t *skel = io->private_data;
  return size;
}

static snd_pcm_sframes_t skel_playback_pointer(snd_pcm_ioplug_t *io)
{
  snd_pcm_skel_t *skel = io->private_data;
  return -1;
}

static snd_pcm_sframes_t skel_capture_pointer(snd_pcm_ioplug_t *io)
{
  snd_pcm_skel_t *skel = io->private_data;
  return -1;
}

static int skel_alsa_close(snd_pcm_ioplug_t *io)
{
  snd_pcm_skel_t *skel = io->private_data;
  return 0;
}

int skel_hw_params(snd_pcm_ioplug_t *io,
		   snd_pcm_hw_params_t *params)
{
  snd_pcm_skel_t *skel = io->private_data;
  return 0;
}

int skel_sw_params(snd_pcm_ioplug_t *io,
		   snd_pcm_sw_params_t *params)
{
  snd_pcm_skel_t *skel = io->private_data;
  return 0;
}

static int skel_prepare(snd_pcm_ioplug_t *io)
{
  snd_pcm_skel_t *skel = io->private_data;
  return 0;
}

static int skel_drain(snd_pcm_ioplug_t *io)
{
  snd_pcm_skel_t *skel = io->private_data;
  return 0;
}

static int skel_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
  snd_pcm_skel_t *skel = io->private_data;
  *delayp = 0;
  return 0;
}


static int skel_poll_revents(snd_pcm_ioplug_t *io,
			     struct pollfd *pfd,
			     unsigned int nfds,
			     unsigned short *revents)
{
  snd_pcm_skel_t *skel = io->private_data;
  *revents = POLLERR;
  return 0;
}

static int skel_poll_descriptors(snd_pcm_ioplug_t *io,
				 struct pollfd *pfd,
				 unsigned int space)
{
  return 0;
}

static int skel_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
  return 0;
}

const snd_pcm_ioplug_callback_t dspd_playback_callback = {
  .start = skel_alsa_start,
  .stop = skel_stop,
  .transfer = skel_write_pcm,
  .pointer = skel_playback_pointer,
  .close = skel_alsa_close,
  .hw_params = skel_hw_params,
  .sw_params = skel_sw_params,
  .prepare = skel_prepare,
  .drain = skel_drain,
  .delay = skel_delay,
  .pause = NULL, //dspd_pause,
  .resume = NULL, //dspd_resume,
  .poll_revents = skel_poll_revents,
  .poll_descriptors_count = skel_poll_descriptors_count,
  .poll_descriptors = skel_poll_descriptors,
};

const snd_pcm_ioplug_callback_t dspd_capture_callback = {
  .start = skel_alsa_start,
  .stop = skel_stop,
  .transfer = skel_read_pcm,
  .pointer = skel_capture_pointer,
  .close = skel_alsa_close,
  .hw_params = skel_hw_params,
  .sw_params = skel_sw_params,
  .prepare = skel_prepare,
  .drain = skel_drain,
  .delay = skel_delay,
  .pause = NULL, //dspd_pause,
  .resume = NULL, //dspd_resume,
  .poll_revents = skel_poll_revents,
  .poll_descriptors_count = skel_poll_descriptors_count,
  .poll_descriptors = skel_poll_descriptors,
};


static int skel_hw_constraint(snd_pcm_skel_t *skel)
{
  return 0;
}


SND_PCM_PLUGIN_DEFINE_FUNC(skel)
{
  
  return 0;
}

SND_PCM_PLUGIN_SYMBOL(skel);
