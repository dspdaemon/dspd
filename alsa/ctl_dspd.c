/*
 *   ALSA <-> DSPD mixer control plugin
 *
 *   Copyright (c) 2018 Tim Smith <dspdaemon _AT_ yandex.com>
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
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include <linux/soundcard.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include "../lib/sslib.h"




typedef struct snd_ctl_dspd {
  snd_ctl_ext_t ext;
  struct dspd_ctl_client *cli;
  struct dspd_aio_ctx *aio;
  bool refresh_count;
  int pipe[2];
  uint32_t subscribed;
} snd_ctl_dspd_t;


struct dspd_pipe_event {
  struct dspd_mix_info info;
  int32_t              index;
  int32_t              event;
};

static int dspd_poll_descriptors_count(snd_ctl_ext_t *ext)
{
  return 2;
}

static int dspd_poll_descriptors(snd_ctl_ext_t *ext, struct pollfd *pfds, unsigned int space)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  int ret = 0;
  //An application may get poll descriptors, call poll(), read events, then never check revents.
  //This ensures that pending aio gets processed.
  ret = dspd_aio_process(dspd->aio, 0, 0);
  if ( ret == 0 || ret == -EINPROGRESS )
    {
      if ( space > 0 )
	{
	  pfds[0].fd = dspd_aio_get_iofd(dspd->aio);
	  pfds[0].events = dspd_aio_block_directions(dspd->aio);
	  pfds[0].revents = 0;
	  ret++;
	}
      if ( space > ret )
	{
	  pfds[1].fd = dspd->pipe[0];
	  pfds[1].events = POLLIN | POLLRDHUP;
	  pfds[1].revents = 0;
	  ret++;
	}
    }
  return ret;
}

static int dspd_poll_revents(snd_ctl_ext_t *ext, struct pollfd *pfds, unsigned int nfds, unsigned short *revents)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  int ret;
  unsigned int i;
  int iofd = dspd_aio_get_iofd(dspd->aio);
  int re = 0;
  *revents = 0;
  for ( i = 0; i < nfds; i++ )
    {
      if ( pfds[i].fd == iofd )
	{
	  re = pfds[i].revents;
	} else if ( pfds[i].fd == dspd->pipe[0] && dspd->pipe[0] >= 0 )
	{
	  *revents |= pfds[i].revents;
	}
    }
 
  ret = dspd_aio_process(dspd->aio, re, 0);
  if ( ret < 0 && ret != -EINPROGRESS )
    *revents |= POLLERR;
  return 0;
}





static int dspd_read_event(snd_ctl_ext_t *ext,
			   snd_ctl_elem_id_t *id,
			   unsigned int *event_mask)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  int32_t ret = -EAGAIN, bytes;
  struct dspd_pipe_event pe;
  ret = dspd_aio_process(dspd->aio, 0, 0);
  if ( ret == 0 || ret == -EINPROGRESS )
    {
      bytes = read(dspd->pipe[0], &pe, sizeof(pe));
      if ( bytes == sizeof(pe) )
	{
	  snd_ctl_elem_id_set_numid(id, pe.index+1);
	  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	  snd_ctl_elem_id_set_name(id, pe.info.name);
	  *event_mask = pe.event;
	  ret = 1;
	} else if ( bytes < 0 )
	{
	  ret = -errno;
	  if ( ret == -EINTR )
	    ret = -EAGAIN;
	} else
	{
	  ret = -EIO;
	}
    }
  return ret;
}

static void dspd_subscribe_events(snd_ctl_ext_t *ext, int subscribe)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  /*
    The control client context remains subscribed to track the items that get added and removed.
  */
  dspd->subscribed = subscribe;
}


/*
  The values here are pointers to an array.  The number of elements depends on the number of
  channels.
*/

static int dspd_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  return dspd_ctlcli_elem_set_int32(dspd->cli, key, DSPD_MIXER_CHN_MONO, *value, NULL, NULL, NULL);
}

static int dspd_write_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *items)
{
  long val = *items;
  int ret = dspd_write_integer(ext, key, &val);
  if ( ret == 0 )
    *items = val;
  return ret;
}

static int dspd_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
  int32_t val;
  snd_ctl_dspd_t *dspd = ext->private_data;
  int ret = dspd_ctlcli_elem_get_int32(dspd->cli, key, DSPD_MIXER_CHN_MONO, &val, NULL, NULL);
  if ( ret == 0 )
    *value = val;
  return ret;
}

static int dspd_read_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *items)
{
  long val;
  int ret = dspd_read_integer(ext, key, &val);
  if ( ret == 0 )
    *items = val;
  return ret;
}

static int dspd_get_integer_info(snd_ctl_ext_t *ext,
				 snd_ctl_ext_key_t key,
				 long *imin, long *imax, long *istep)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  struct dspd_mix_range r;
  int ret = dspd_ctlcli_elem_get_range(dspd->cli, key, &r, NULL, NULL);
  if ( ret == 0 )
    {
      *istep = 1;
      *imin = r.min;
      *imax = r.max;
    }
  return ret;
}

static int dspd_get_enumerated_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *items)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  struct dspd_mix_range r;
  int ret = dspd_ctlcli_elem_get_range(dspd->cli, key, &r, NULL, NULL);
  if ( ret == 0 )
    *items = r.max - r.min;
  return ret;
}

static int dspd_get_enumerated_name(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int item,
				    char *name, size_t name_max_len)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  struct dspd_mix_info info;
  int ret = dspd_ctlcli_elem_get_enum_info(dspd->cli, key, item, &info, NULL, NULL);
  if ( ret == 0 )
    strlcpy(name, info.name, name_max_len);
  return ret;
}

static int dspd_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
			      int *type, unsigned int *acc, unsigned int *count)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  int32_t ret;
  struct dspd_mix_info info;
  unsigned int c;
  uint32_t i;
  uint32_t mask;
  ret = dspd_ctlcli_elem_get_info(dspd->cli, key, &info);
  if ( ret == 0 )
    {
      if ( info.flags & (DSPD_MIXF_PVOL|DSPD_MIXF_CVOL|DSPD_MIXF_CDB|DSPD_MIXF_PDB) )
	*type = SND_CTL_ELEM_TYPE_INTEGER;
      else if ( info.flags & (DSPD_MIXF_ENUM|DSPD_MIXF_ENUMP|DSPD_MIXF_ENUMC) )
	*type = SND_CTL_ELEM_TYPE_ENUMERATED;
      else if ( info.flags & DSPD_MIXF_SWITCH )
	*type = SND_CTL_ELEM_TYPE_BOOLEAN;
      else
	*type = SND_CTL_ELEM_TYPE_NONE;
      *acc = SND_CTL_EXT_ACCESS_READWRITE; 
      if ( info.flags & DSPD_MIXF_PLAYBACK )
	mask = info.pchan_mask;
      else if ( info.flags & DSPD_MIXF_CAPTURE )
	mask = info.cchan_mask;
      else
	mask = 1U;
      c = 0;
      for ( i = 0; i < (sizeof(mask) * 8U); i++ )
	{
	  if ( mask & (1<<i) )
	    c++;
	}
      *count = c;
    }
  return ret;
}
static snd_ctl_ext_key_t dspd_find_elem(snd_ctl_ext_t *ext,
					const snd_ctl_elem_id_t *id)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  snd_ctl_ext_key_t key = SND_CTL_EXT_KEY_NOT_FOUND;
  unsigned int numid = snd_ctl_elem_id_get_numid(id);
  struct dspd_mix_info info;
  if ( numid > 0 )
    {
      numid--;
      if ( dspd_ctlcli_elem_get_info(dspd->cli, numid, &info) == 0 )
	key = numid;
    }
  return key;
}

static int dspd_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  int err;
  struct dspd_mix_info info;
  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  err = dspd_ctlcli_elem_get_info(dspd->cli, offset, &info);
  if ( err == 0 )
    {
      /*
	Controls are identified by name and index.
      */
      snd_ctl_elem_id_set_name(id, info.name);

      /*
	This is the index of the control.  It is 1 based, not 0 based.  The number 0
	is reserved for an invalid value.
      */
      snd_ctl_elem_id_set_numid(id, offset+1U);
    }
  return err;
}

static int dspd_elem_count(snd_ctl_ext_t *ext)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  int ret;
  ret = dspd_aio_process(dspd->aio, 0, 0);
  if ( ret == 0 || ret == -EINPROGRESS )
    ret = dspd_ctlcli_elem_count(dspd->cli);
  return ret;
}

static void dspd_close(snd_ctl_ext_t *ext)
{
  snd_ctl_dspd_t *dspd = ext->private_data;
  dspd_ctlcli_delete(dspd->cli);
  dspd_aio_delete(dspd->aio);
  if ( dspd->pipe[0] >= 0 )
    close(dspd->pipe[0]);
  if ( dspd->pipe[1] >= 0 )
    close(dspd->pipe[1]);
  free(dspd);
}




static snd_ctl_ext_callback_t dspd_ext_callback = {
  .close = dspd_close,
  .elem_count = dspd_elem_count,
  .elem_list = dspd_elem_list,
  .find_elem = dspd_find_elem,
  .get_attribute = dspd_get_attribute,
  .get_integer_info = dspd_get_integer_info,
  .get_enumerated_info = dspd_get_enumerated_info,
  .get_enumerated_name = dspd_get_enumerated_name,
  .read_integer = dspd_read_integer,
  .read_enumerated = dspd_read_enumerated,
  .write_integer = dspd_write_integer,
  .write_enumerated = dspd_write_enumerated,
  .read_event = dspd_read_event,
  .subscribe_events = dspd_subscribe_events,
  .poll_descriptors_count = dspd_poll_descriptors_count,
  .poll_descriptors = dspd_poll_descriptors,
  .poll_revents = dspd_poll_revents,
};

static void dspd_change_cb(struct dspd_ctl_client *cli, void *arg, int32_t err, uint32_t elem, int32_t evt, const struct dspd_mix_info *info)
{
  snd_ctl_dspd_t *dspd = arg;
  struct dspd_pipe_event pe;
  int e;
  if ( dspd->subscribed != 0 && dspd->refresh_count == false && info != NULL && dspd->pipe[1] >= 0 )
    {
      /*
	This should work as long as the application doesn't stall for so long that the pipe fills up.
	The pipe should be some multiple of PIPE_BUF, which is 4K on old versions of Linux and 64K
	on new versions.
      */
      memset(&pe, 0, sizeof(pe));
      pe.info = *info;
      pe.event = evt;
      pe.index = elem;
      while ( write(dspd->pipe[1], &pe, sizeof(pe)) < 0 )
	{
	  e = errno;
	  if ( e == EAGAIN )
	    {
	      close(dspd->pipe[1]);
	      dspd->pipe[1] = -1;
	    }
	  if ( e != EINTR )
	    break;
	}
      
    }
}

SND_CTL_PLUGIN_DEFINE_FUNC(dspd)
{
  snd_ctl_dspd_t *dspd;
  int ret;
  uint32_t count;


  dspd = calloc(1, sizeof(*dspd));
  if ( ! dspd )
    return -ENOMEM;
  dspd->pipe[0] = -1;
  dspd->pipe[1] = -1;
  dspd->ext.private_data = dspd;
  ret = dspd_aio_new(&dspd->aio, 256UL);
  if ( ret == 0 )
    {
      ret = dspd_aio_connect(dspd->aio, NULL, NULL, NULL, NULL);
      if ( ret == 0 )
	{
	  ret = dspd_ctlcli_new(&dspd->cli, DSPD_CC_IO_SYNC, 0);
	  if ( ret < 0 )
	    {
	      dspd_close(&dspd->ext);
	      return ret;
	    }
	} else
	{
	  goto out;
	}
    }
  dspd_ctlcli_bind(dspd->cli, dspd->aio, 0);

  ret = dspd_ctlcli_subscribe(dspd->cli, true, &count, NULL, NULL);
  if ( ret < 0 )
    goto out;

  ret = dspd_ctlcli_refresh_count(dspd->cli, &count, NULL, NULL);
  if ( ret < 0 )
    goto out;
  ret = pipe2(dspd->pipe, O_CLOEXEC|O_NONBLOCK);
  if ( ret < 0 )
    {
      ret = -errno;
      goto out;
    }
  dspd_ctlcli_set_event_cb(dspd->cli, dspd_change_cb, dspd);

  dspd->ext.version = SND_CTL_EXT_VERSION;

  
  
  dspd->ext.card_idx = 0; //use dspd stream object number
  strcpy(dspd->ext.driver, "dspd");
  strcpy(dspd->ext.name, "DSPD");
  strcpy(dspd->ext.longname, "DSP Daemon Device and Client Streams");
  strcpy(dspd->ext.mixername, "DSP Daemon Software Controls");
  dspd->ext.poll_fd = -1;
  dspd->ext.callback = &dspd_ext_callback;
  

  ret = snd_ctl_ext_create(&dspd->ext, name, mode);

 out:
  if ( ret < 0 )
    dspd_close(&dspd->ext);
  else
    *handlep = dspd->ext.handle;

  return ret;

 

}
SND_CTL_PLUGIN_SYMBOL(dspd);



