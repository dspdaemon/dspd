/*
 *   OSSCUSE - OSS API implementation using CUSE
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */


/*
  CUSE OSS4 Server

  This module will use /dev/cuse directly because libfuse has some problems:

  1.  It sends request packets at random worker threads.  If a request blocks then
  other requests might get stuck.
  2.  It spawns threads while processing.
  3.  It calls malloc() while processing.
  4.  It requires at least one thread per device.  I think it is possible to do
  all of each type of device node on 1 thread per type.  Then use a thread per client
  and another to defer blocking work.


  

  MEMORY ALLOCATION SCHEME:
  
  Each dsp device has a dspd_rtalloc object associated with it.  The associated
  object has enough room for 2 full sized packets.  This means that most of the
  time the packet can be received and serviced without copying.  There is also
  another dspd_rtalloc for the entire module.

  If that buffer is full then a single buffer for the whole thread can be used as temporary
  storage since all dsp devices have requests received on the same thread.  The packet
  can then be checked to figure out which client it belongs to.  After that it can be copied
  into a client dspd_rtalloc or if that fails then malloc is used.  Since libfuse does this
  all of the time and usually works well enough this can be an acceptible last resort.


  


*/


#include <unistd.h>
#include <string.h>
#include <linux/fuse.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../lib/cbpoll.h"
#include "rtcuse.h"
#include "mod_osscuse.h"
#include "soundcard.h"
static const struct snd_mixer_oss_assign_table ossv3_table[];
static const struct snd_mixer_oss_assign_table ossv4_table[];
#define CTL_PKTLEN 5120

static struct dspd_oss_server server_context;
static void *dsp_worker(void *p);
static void remove_cdev(struct oss_dsp_cdev *cdev);

static void insert_cdev(struct oss_dsp_cdev *cdev);
static int32_t cdev_ioctl(struct oss_cdev_client *cli);


int osscuse_create_cdev(int devnum,
			int devtype,
			struct rtcuse_cdev_params *devp,
			struct rtcuse_cdev **dev);

static inline void set_ibit(int *bits, unsigned int num)
{
  bits[num/4] |= (1 << (num%32));
}
static bool check_busy(const struct oss_dsp_cdev *dev)
{
  size_t i;
  uintptr_t val;
  for ( i = 0; i < ARRAY_SIZE(dev->clients); i++ )
    {
      val = (uintptr_t)dev->clients[i];
      if ( val > 0 && val < UINTPTR_MAX )
	return true;
    }
  return false;
}
void osscuse_get_sysinfo(oss_sysinfo *info)
{
  const struct oss_dsp_cdev *dev;
  size_t i;
  memset(info, 0, sizeof(*info));
  strcpy(info->product, "OSS/DSPD");
  strcpy(info->license, "LGPL");
  sprintf(info->version, "%d.%d.%d", OSS_VERSION >> 16, (OSS_VERSION >> 8) & 0xFF, OSS_VERSION & 0xFF);
  info->versionnum = OSS_GETVERSION;
  pthread_rwlock_rdlock(&server_context.devtable_lock);
  for ( i = 0; i < ARRAY_SIZE(server_context.dsp_table); i++ )
    {
      dev = server_context.dsp_table[i];
      if ( ! dev )
	continue;
      if ( dev->error == -ENODEV )
	continue;
      if ( dev->playback_index > 0 || dev->capture_index > 0 )
	{
	  info->numaudios++;
	  /*
	    The documentation says each device may have one or more audio engines
	    and oss_audioinfo.dev is a number between 0 and oss_sysinfo.numaudios-1.
	    It says oss_sysinfo.numaudios is the number of device files.  For now,
	    I am going with the most sane definition which means there will often
	    be more audio engines than cards.
	   */
	  if ( dev->playback_index > 0 && dev->capture_index > 0 &&
	       dev->playback_index != dev->capture_index )
	    info->numaudioengines += 2;
	  else
	    info->numaudioengines++;
	  info->numcards++;
	  if ( check_busy(dev) )
	    set_ibit(info->openedaudio, i);
	}
    }
   for ( i = 0; i < ARRAY_SIZE(server_context.dsp_table); i++ )
    {
      dev = server_context.ctl_table[i];
      if ( ! dev )
	continue;
      if ( dev->error == -ENODEV )
	continue;
      info->nummixers++;
    }
  pthread_rwlock_unlock(&server_context.devtable_lock);
}

int32_t osscuse_get_audio_engine(int32_t num, int32_t *card, int32_t *next)
{
  int32_t ret = -1, count = 0;
  const struct oss_dsp_cdev *dev;
  size_t i;
  if ( next )
    *next = -1;
  pthread_rwlock_rdlock(&server_context.devtable_lock);
  for ( i = 0; i < ARRAY_SIZE(server_context.dsp_table); i++ )
    {
      dev = server_context.dsp_table[i];
      if ( ! dev )
	continue;
      if ( dev->error == -ENODEV )
	continue;
      if ( dev->playback_index != dev->capture_index )
	{
	  if ( dev->playback_index > 0 )
	    {
	      if ( count == num )
		{
		  ret = dev->playback_index;
		  if ( dev->capture_index > 0 && next != NULL )
		    *next = ret + 1;
		  break;
		}
	      count++;
	    }
	  if ( dev->capture_index > 0 )
	    {
	      if ( count == num )
		{
		  ret = dev->capture_index;
		  if ( dev->playback_index > 0 && next != NULL )
		    *next = ret - 1;
		  break;
		}
	      count++;
	    }
	} else if ( dev->playback_index > 0 )
	{
	  if ( count == num )
	    {
	      ret = dev->playback_index;
	      break;
	    }
	  count++;
	} else if ( dev->capture_index > 0 )
	{
	  if ( count == num )
	    {
	      ret = dev->capture_index;
	      break;
	    }
	  count++;
	}

    }
  if ( ret > 0 )
    {
      if ( dspd_daemon_ref(ret, DSPD_DCTL_ENUM_TYPE_SERVER) != 0 )
	{
	  ret = -1;
	} else if ( card )
	{
	  *card = server_context.dsp_table[i]->cdev_index;
	}
    }
  pthread_rwlock_unlock(&server_context.devtable_lock);
  return ret;
}

/*
  Get full card information, including separate playback+capture.
*/
int osscuse_get_cardinfo(int dev, 
			 struct dspd_device_stat *info)
{
  struct oss_dsp_cdev *cdev = oss_lock_cdev(dev);
  struct dspd_device_stat tmp;
  size_t len;
  int ret;
  if ( ! cdev )
    return ENODEV;

  if ( cdev->playback_index == cdev->capture_index )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    cdev->playback_index,
			    DSPD_SCTL_SERVER_STAT,
			    NULL,
			    0,
			    info,
			    sizeof(*info),
			    &len);
      if ( ret == 0 )
	info->reserved = OSS_CARDINFO_FULLDUPLEX;
    } else if ( cdev->playback_index > 0 && cdev->capture_index > 0 )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    cdev->playback_index,
			    DSPD_SCTL_SERVER_STAT,
			    NULL,
			    0,
			    info,
			    sizeof(*info),
			    &len);
      if ( ret == 0 )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				cdev->capture_index,
				DSPD_SCTL_SERVER_STAT,
				NULL,
				0,
				&tmp,
				sizeof(tmp),
				&len);
	  if ( ret == 0 )
	    {
	      info->capture = tmp.capture;
	      info->reserved = 0;
	    }
	}
    } else if ( cdev->playback_index > 0 )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    cdev->playback_index,
			    DSPD_SCTL_SERVER_STAT,
			    NULL,
			    0,
			    info,
			    sizeof(*info),
			    &len);
      info->reserved = 0;
    } else if ( cdev->capture_index > 0 )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    cdev->capture_index,
			    DSPD_SCTL_SERVER_STAT,
			    NULL,
			    0,
			    info,
			    sizeof(*info),
			    &len);
      info->reserved = 0;
    } else
    {
      ret = -ENODEV;
    }
  

  oss_unlock_cdev(cdev);
  return ret;
}

struct oss_cdev_client *dsp_find_client(struct oss_dsp_cdev *cdev, uint64_t fh)
{
  uintptr_t index = fh >> 32;
  uintptr_t client = fh & 0xFFFFFFFF;
  struct oss_cdev_client *ptr;
  if ( index < DSPD_MAX_OBJECTS )
    {
      ptr = cdev->clients[index];
      if ( ptr != NULL && ptr != (void*)UINTPTR_MAX )
	{
	  if ( ptr->client_index != client && ptr->client_index != -1 )
	    ptr = NULL;
	} else if ( ptr == (void*)UINTPTR_MAX )
	{
	  ptr = NULL;
	}
    } else
    {
      ptr = NULL;
    }
  return ptr;
}

static int32_t dsp_get_iorp_buffer(struct oss_cdev_client *client, size_t len, void **buf, void **alloc_ctx)
{
  /*
    Get a buffer that the io request packet can be queued on.
   */

  void *mem;
  int32_t ret = 0;
  mem = dspd_rtalloc_malloc(client->alloc, len);
  if ( ! mem )
    {
      *alloc_ctx = NULL; 
      mem = malloc(len);
      if ( ! mem )
	ret = -errno;
    } else
    {
      *alloc_ctx = client->alloc;
    }
  *buf = mem;
  return ret;
}

static void dsp_get_input_buffer(struct oss_dsp_cdev *dev, void **buf, void **alloc_ctx)
{
  void *mem;
  size_t pktlen = dev->cdev->params.pktlen;
  if ( (mem = dspd_rtalloc_malloc(dev->alloc, pktlen)) )
    {
      *alloc_ctx = dev->alloc;
      *buf = mem;
    } else if ( (mem = dspd_rtalloc_malloc(server_context.alloc, pktlen)) )
    {
      *alloc_ctx = server_context.alloc;
      *buf = mem;
    } else
    {
      assert(server_context.inbuf);
      *buf = server_context.inbuf;
      *alloc_ctx = NULL;
    }
}

static int dsp_reply_generic_error(struct oss_dsp_cdev *cdev, uint64_t unique, int32_t error)
{
  struct iovec iov;
  struct fuse_out_header hdr;
  int ret;
  //  fprintf(stderr, "REPLY GENERIC ERROR %d\n", error);
  hdr.len = sizeof(hdr);
  //The protocol only allows a negative error number which represents a positive errno value
  //so to reduce errors all numbers are made positive.
  if ( error > 0 )
    hdr.error = error * -1;
  else
    hdr.error = error;
  hdr.unique = unique;
  iov.iov_base = &hdr;
  iov.iov_len = sizeof(hdr);
  ret = rtcuse_writev_block(cdev->cdev, &iov, 1);
  if ( ret == iov.iov_len )
    ret = 0;
  return ret;
}

static bool cancel_req(struct oss_cdev_client *cli, uint64_t unique)
{
  struct iorp *pkt;
  uint32_t offset = 0;
  uint32_t ret;
  bool result = 0;
  /*
    Look for the request so it can be canceled.  It is safe to do this
    even with the race condition because the canceled request is in safe
    memory.  This means a request either properly cancels or finishes,
    possibly with a short read or write.

    If we cancel a complete request and all requests come from this thread
    then nothing happens.

  */
  while ( (ret = dspd_fifo_peek(cli->eventq, offset, (void**)&pkt)) > 0 )
    {
      if ( pkt->unique == unique )
	{
	  AO_store(&pkt->canceled, IORP_CANCELED);
	  result = 1;
	  /*
	    Here is a good place to notify the thread.  Since libfuse
	    doesn't do that kind of thing it should be safe not to do it here.
	    The thread will get around to it eventually.
	  */
	  break;
	}
      offset++;
    }
  return result;
}
static int dsp_interrupt(struct oss_dsp_cdev *cdev, uint64_t unique)
{
  int i, ret = -ENOENT;
  struct oss_cdev_client *cli;
  for ( i = 0; i < DSPD_MAX_OBJECTS; i++ )
    {
      cli = cdev->clients[i];
      if ( cli != NULL && cli != (void*)UINTPTR_MAX )
	{
	  if ( cancel_req(cli, unique) )
	    {
	      ret = 0;
	      break;
	    }
	}
    }
  return ret;
}

static int dsp_queue_req(struct oss_cdev_client *cli, struct rtcuse_ipkt *pkt, void *alloc)
{

  struct iorp *p;
  uint32_t len;
  int ret = dspd_fifo_wiov(cli->eventq,
			   (void**)&p,
			   &len);
  if ( ret != 0 || len == 0 )
    return EAGAIN;
  p->addr = pkt;
  p->alloc_ctx = alloc;
  AO_store(&p->canceled, IORP_OK);
  p->unique = pkt->header.unique;
  dspd_fifo_wcommit(cli->eventq, 1);
  if ( AO_test_and_set(&cli->wakeup) != AO_TS_SET )
    {
      dspd_mutex_lock(&cli->lock);
      assert(pthread_mutex_trylock(&cli->lock.mutex) != 0);
      dspd_cond_signal(&cli->event);
      dspd_mutex_unlock(&cli->lock);
    }
  return 0;
}

static int cdev_find_slot(struct oss_dsp_cdev *dev)
{
  int i, ret = -1;
  for ( i = 0; i < DSPD_MAX_OBJECTS; i++ )
    {
      if ( dev->clients[i] == NULL )
	{
	  dev->clients[i] = (void*)UINTPTR_MAX;
	  ret = i;
	  break;
	}
    }
  return ret;
}

struct cdev_open_req {
  uint64_t unique;
  uint32_t flags; //IMPORTANT NOTE: fuse_read_in and fuse_write_in both can change this
};

static void free_client_cb(struct cbpoll_ctx *ctx,
			   void *data,
			   int64_t arg,
			   int32_t index,
			   int32_t fd)
{
  struct oss_cdev_client *cli = (struct oss_cdev_client*)(intptr_t)arg;
  size_t br;
  if ( cli->ops && cli->ops->free )
    {
      cli->ops->free(cli);
    } else
    {
      dspd_rclient_destroy(&cli->dsp.rclient);
      dspd_rtalloc_delete(cli->alloc);
      dspd_fifo_delete(cli->eventq);
      dspd_mutex_destroy(&cli->lock);
      dspd_cond_destroy(&cli->event);
      free(cli->dsp.readbuf);
      cli->dsp.readbuf = NULL;
      dspd_stream_ctl(&dspd_dctx, 
		      cli->client_index,
		      DSPD_SCTL_CLIENT_DISCONNECT,
		      NULL,
		      0,
		      NULL,
		      0,
		      &br);
  
      if ( cli->client_index > 0 )
	dspd_daemon_unref(cli->client_index);
      free(cli);
    }
}

void dsp_client_release_cb(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *evt)
{
  struct oss_dsp_cdev *dev = (void*)(intptr_t)evt->arg;
  struct cbpoll_work wrk;
  //static int call_count = 0;
  int count, idx = -1;
  //call_count++;
  assert(dev->clients[evt->index]);
 
  if ( dev->clients[evt->index] != (void*)UINTPTR_MAX )
    {
      wrk.index = dev->cbpoll_index;
      wrk.fd = dev->cdev->fd;
      wrk.msg = 0;
      wrk.arg = (intptr_t)dev->clients[evt->index];
      wrk.callback = free_client_cb;
      cbpoll_queue_work(ctx, &wrk);

    }
  dev->clients[evt->index] = NULL;

  if ( dev->error )
    {
      dspd_mutex_lock(&dev->lock);
      if ( dev->error )
	{
	  count = 0;
	  size_t i;
	  for ( i = 0; i < ARRAY_SIZE(dev->clients); i++ )
	    if ( dev->clients[i] != NULL )
	      count++;
	  if ( count == 0 && dev->dead == false )
	    {
	      cbpoll_close_fd(ctx, dev->cbpoll_index);
	      idx = dev->cbpoll_index;
	    }
	}
      dspd_mutex_unlock(&dev->lock);
    }

  cbpoll_unref(ctx, dev->cbpoll_index);
  if ( idx >= 0 )
    cbpoll_unref(ctx, dev->cbpoll_index);
}

static void dsp_cdev_seterror_cb(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *evt)
{
  struct oss_dsp_cdev *dev = (void*)(intptr_t)evt->arg;
  struct oss_cdev_client *cli;
  size_t i;
  size_t count = 0;
  int idx = -1;
  //This lock is rarely taken and that would only happen during hotplugging
  dspd_mutex_lock(&dev->lock);
  dev->error = evt->index; //Should be a negative number
  for ( i = 0; i < ARRAY_SIZE(dev->clients); i++ )
    {
      cli = dev->clients[i];
      if ( cli != NULL )
	{
	  if ( cli != (void*)UINTPTR_MAX )
	    {
	      cli->op_error = evt->index;
	      if ( ! dev->is_mixer )
		{
		  cli->wakeup = AO_TS_SET;
		  dspd_cond_signal(&cli->event);
		}
	    }
	  count++;
	}
    }
  if ( count == 0 && server_context.persistent_devnodes == false && dev->cdev_index != 0 )
    {
      //Don't even think about resurecting the device.  It is really dead now.
      dev->dead = true;
      idx = dev->cbpoll_index;
      //Remove the file descriptor from the event loop
      cbpoll_close_fd(ctx, dev->cbpoll_index);
    }
  dspd_mutex_unlock(&dev->lock);
  //Kill the device context.
  if ( idx >= 0 )
    cbpoll_unref(ctx, idx);
}
static void dsp_cdev_seterror(struct oss_dsp_cdev *dev, int err)
{
  struct cbpoll_pipe_event pe = { 0 };
  pe.fd = dev->cdev->fd;
  pe.index = err;
  pe.stream = -1;
  pe.msg = CBPOLL_PIPE_MSG_CALLBACK;
  pe.arg = (intptr_t)dev;
  pe.callback = dsp_cdev_seterror_cb;
  if ( dev->is_mixer )
    cbpoll_send_event(&server_context.ctl_cbpoll, &pe);
  else
    cbpoll_send_event(&server_context.cbpoll, &pe);
}


void dsp_client_release_notify(struct oss_dsp_cdev *dev, int slot)
{
  struct cbpoll_pipe_event pe = { 0 };
  pe.fd = dev->cdev->fd;
  pe.index = slot;
  pe.stream = -1;
  pe.msg = CBPOLL_PIPE_MSG_CALLBACK;
  pe.arg = (intptr_t)dev;
  pe.callback = dsp_client_release_cb;
  cbpoll_send_event(&server_context.cbpoll, &pe);
}

static bool check_mode(mode_t mode, int sbits)
{
  return ((sbits & DSPD_PCM_SBIT_PLAYBACK) && mode == O_WRONLY) ||
    ((sbits & DSPD_PCM_SBIT_CAPTURE) && mode == O_RDONLY) ||
    (((sbits & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) == (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) &&
     mode == O_RDWR);
}

static void async_dsp_new_client(struct cbpoll_ctx *ctx,
				 void *data,
				 int64_t arg,
				 int32_t index,
				 int32_t fd)
{
  struct oss_cdev_client *cli;
  struct oss_dsp_cdev *dev = data;
  struct cdev_open_req *req = cbpoll_get_extra_data(ctx);
  int32_t slot = arg, device_index;
  int err = 0;
  size_t pgsize = 256, pgcount, n;
  size_t br;
  int s = 0;


  cli = calloc(1, sizeof(*cli));
  if ( ! cli )
    {
      err = ENOMEM;
      goto error;
    }

  if ( cli->mode == O_RDWR )
    s = DSPD_PCM_SBIT_FULLDUPLEX;
  else if ( cli->mode == O_RDONLY )
    s = DSPD_PCM_SBIT_CAPTURE;
  else if ( cli->mode == O_WRONLY )
    s = DSPD_PCM_SBIT_PLAYBACK;
 
  err = dspd_rclient_init(&cli->dsp.rclient, s);
  if ( err )
    {
      err *= -1;
      goto error;
    }

  cli->cdev_slot = slot;
  err = dspd_cond_init(&cli->event, &server_context.client_condattr);
  if ( err )
    goto error;
  err = dspd_mutex_init(&cli->lock, NULL);
  if ( err )
    goto error;

  cli->flags = req->flags;
  cli->unique = req->unique;

  cli->mode = req->flags & O_ACCMODE;

  if ( cli->mode == O_RDWR || cli->mode == O_RDONLY )
    {
      cli->dsp.readbuf = calloc(1, server_context.dsp_params.maxread);
      if ( ! cli->dsp.readbuf )
	{
	  err = ENOMEM;
	  goto error;
	}
      cli->dsp.readlen = server_context.dsp_params.maxread;
    }

  cli->dsp.subdivision = 1;
 
  cli->cdev = dev;
 
  n = (dev->cdev->params.pktlen * 2);
  pgcount = n / pgsize;
  if ( n % pgsize )
    pgcount++;
  
  cli->alloc = dspd_rtalloc_new(pgcount, pgsize);
  if ( ! cli->alloc )
    {
      err = ENOMEM;
      goto error;
    }
  err = dspd_fifo_new(&cli->eventq, 32, sizeof(struct iorp), NULL);
  if ( err )
    goto error;

  err = dspd_client_new(dspd_dctx.objects, &cli->client_ptr);
  if ( err )
    {
      err *= -1;
      goto error;
    }
  
  uint32_t p = dev->playback_index, c = dev->capture_index;

  //The error here is ENODEV since it was already checked once.
  if ( cli->mode == O_RDWR )
    {
      if ( p != c || p <= 0 || c <= 0 )
	{
	  err = ENODEV;
	  goto error;
	}
      device_index = p;
    } else if ( cli->mode == O_WRONLY )
    {
      if ( p <= 0 )
	{
	  err = ENODEV;
	  goto error;
	}
      device_index = p;
    } else
    {
      if ( c <= 0 )
	{
	  err = ENODEV;
	  goto error;
	}
      device_index = c;
    }

  if ( cli->mode == O_RDONLY )
    cli->dsp.trigger = PCM_ENABLE_INPUT;
  else if ( cli->mode == O_WRONLY )
    cli->dsp.trigger = PCM_ENABLE_OUTPUT;
  else
    cli->dsp.trigger = PCM_ENABLE_OUTPUT | PCM_ENABLE_INPUT;

  cli->client_index = dspd_client_get_index(cli->client_ptr);
  
  cli->dsp.max_write = cli->cdev->cdev->params.maxwrite;
  cli->dsp.max_read = cli->cdev->cdev->params.maxread;

  err = dspd_daemon_ref(device_index, DSPD_DCTL_ENUM_TYPE_SERVER);
  if ( err )
    {
      err = ENODEV;
      goto error;
    }
  cli->device_index = device_index;



  err = dspd_stream_ctl(&dspd_dctx,
			cli->device_index,
			DSPD_SCTL_SERVER_STAT,
			NULL,
			0,
			&cli->dsp.devinfo,
			sizeof(cli->dsp.devinfo),
			&br);
  if ( err )
    {
      err = ENODEV;
      goto error;
    }
  if ( ! check_mode(cli->mode, cli->dsp.devinfo.streams) )
    {
      err = EACCES;
      goto error;
    }

  //Reserve a spot on the device.  When we have a secure reference the only errors
  //will be ENODEV or maybe EIO.

  err = dspd_stream_ctl(&dspd_dctx,
			cli->client_index,
			DSPD_SCTL_CLIENT_RESERVE,
			&device_index,
			sizeof(device_index),
			NULL,
			0,
			&br);

  if ( err )
    {
      err *= -1;
      goto error;
    }


 

  
  
  dspd_daemon_unref(device_index);


  cli->fh = slot;
  cli->fh <<= 32;
  cli->fh |= cli->client_index;

  cli->ops = &osscuse_dsp_ops;



  //These are all obsolete.  Use .params instead.
  cli->dsp.rate = 48000;
  cli->dsp.channels = 2;
  cli->dsp.format = DSPD_PCM_FORMAT_S16_LE;
  cli->dsp.buffer_bytes = 16384;
  cli->dsp.frag_bytes = 4096;
  cli->dsp.frame_bytes = 4;

  cli->dsp.params.format = DSPD_PCM_FORMAT_U8;
  cli->dsp.params.channels = 2;
  cli->dsp.params.rate = 8000;
  cli->dsp.params.bufsize = 1024;
  cli->dsp.params.fragsize = 256;
  cli->dsp.params.latency = cli->dsp.params.fragsize;
  cli->dsp.params.xflags = DSPD_CLI_XFLAG_BYTES | DSPD_CLI_XFLAG_COOKEDMODE;
  cli->dsp.policy = 5; //range 0-10

  if ( cli->mode == O_RDONLY )
    cli->dsp.params.stream = DSPD_PCM_SBIT_CAPTURE;
  else if ( cli->mode == O_WRONLY )
    cli->dsp.params.stream = DSPD_PCM_SBIT_PLAYBACK;
  else
    cli->dsp.params.stream = DSPD_PCM_SBIT_PLAYBACK | DSPD_PCM_SBIT_CAPTURE;

    
 
  err = pthread_create(&cli->thread, &server_context.client_threadattr, dsp_worker, cli);
  if ( err == EPERM )
    {
      pthread_attr_t attr;
      if ( dspd_daemon_threadattr_init(&attr,
				       sizeof(attr),
				       DSPD_THREADATTR_DETACHED) == 0 )
	{
	  err = pthread_create(&cli->thread, &server_context.client_threadattr, dsp_worker, cli);
	  pthread_attr_destroy(&attr);
	}
    }
  if ( err )
    goto error;
 


  return;

 error:

  if ( cli )
    {
      dspd_rclient_destroy(&cli->dsp.rclient);
      if ( cli->client_index > 0 )
	dspd_daemon_unref(cli->client_index);
      if ( cli->device_index > 0 )
	dspd_daemon_unref(cli->device_index);
      if ( cli->alloc )
	dspd_rtalloc_delete(cli->alloc);
      if ( cli->eventq )
	dspd_fifo_delete(cli->eventq);
      dspd_cond_destroy(&cli->event);
      dspd_mutex_destroy(&cli->lock);

      free(cli->dsp.readbuf);
      free(cli);
    }

  if ( err != 0 )
    dsp_reply_generic_error(dev, req->unique, err);
  
  dsp_client_release_notify(dev, slot);

  return;
}

static int dsp_new_client(struct oss_dsp_cdev *dev, struct rtcuse_ipkt *pkt)
{
  int slot = cdev_find_slot(dev);
  struct cbpoll_work work = { 0 };
  struct cdev_open_req *req = (void*)work.extra_data;
  struct fuse_open_in *in = (struct fuse_open_in*)pkt->data;
  int mode;
  if ( slot < 0 )
    return -EBUSY;
  if ( dev->error )
    return dev->error;

  mode = in->flags & O_ACCMODE;
  if ( mode == O_RDONLY )
    {
      if ( dev->capture_index <= 0 )
	return -EACCES;
    } else if ( mode == O_WRONLY )
    {
      if ( dev->playback_index <= 0 )
	return -EACCES;
    } else if ( mode == O_RDWR )
    {
      if ( dev->capture_index != dev->playback_index || 
	   dev->capture_index <= 0 ||
	   dev->playback_index <= 0 )
	return -EACCES;
    } else
    {
      //This would be O_RDONLY|O_WRONLY.  The man page doesn't say
      //EINVAL is ever returned by open but that would make more sense
      //than EACCES.
      return -EACCES;
    }
 
  
  req->unique = pkt->header.unique;
  req->flags = in->flags;
  work.fd = dev->cdev->fd;
  work.index = dev->cbpoll_index;
  work.arg = slot;

  work.msg = CBPOLL_PIPE_MSG_CALLBACK;
  work.callback = async_dsp_new_client;
  cbpoll_ref(&server_context.cbpoll, dev->cbpoll_index);
  cbpoll_queue_work(&server_context.cbpoll, &work);
  return 0;
}


static int dsp_fd_event(void *data, 
			struct cbpoll_ctx *context,
			int index,
			int fd,
			int revents)
{
  /*
    Need to get a buffer, read the packet, and find the client.

    If the request is an interrupt request then either cancel a pending
    request immediately or defer the cancellation.

    It should be possible to figure out which request is being actively worked on.
    If not, then just cancel the packet and go back to work.

   */
  ssize_t ret;
  void *alloc = NULL;
  struct oss_dsp_cdev *dev = data;
  struct rtcuse_ipkt *pkt, *p;
  struct fuse_interrupt_in *intr;
  struct oss_cdev_client *cli;
  uint64_t fh;
  dsp_get_input_buffer(dev, (void**)&pkt, &alloc);
  assert(pkt);
  ret = read(fd, pkt, dev->cdev->params.pktlen);
  if ( ret < 0 )
    {
      if ( alloc )
	dspd_rtalloc_free(alloc, pkt);
      //else Might have ENOMEM later, but otherwise safe.
      if ( errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK )
	return -1;
      
      return 0;
    }
 
  if ( pkt->header.opcode == FUSE_INTERRUPT )
    {
      intr = (struct fuse_interrupt_in*)&pkt->data[0];
      //Failing to find the request is not an error.
      dsp_interrupt(dev, intr->unique);
      if ( alloc )
	dspd_rtalloc_free(alloc, pkt);
      ret = 0;
    } else if ( pkt->header.opcode == FUSE_OPEN )
    {
      ret = dsp_new_client(dev, pkt);
      if ( ret )
	ret = dsp_reply_generic_error(dev, pkt->header.unique, ret);
      if ( alloc )
	dspd_rtalloc_free(alloc, pkt);
    } else
    {
      /*
	Need to find the client and shrink the buffer or copy it.
      */
      if ( rtcuse_get_fh(&pkt->header, &pkt->data[0], &fh) )
	{
	  cli = dsp_find_client(dev, fh);
	  if ( cli )
	    {
	      if ( cli->error )
		{
		  ret = dsp_reply_generic_error(dev, pkt->header.unique, cli->error);
		  if ( alloc )
		    dspd_rtalloc_free(alloc, pkt);
		  return ret;
		}

	      if ( alloc )
		{
		  //Give up unused memory.  Most packets are probably
		  //not full sized.
		  dspd_rtalloc_shrink(alloc, pkt, pkt->header.len);
		  p = pkt;
		  ret = 0;
		} else
		{
		  //Find a buffer that can be safely queued.
		  ret = dsp_get_iorp_buffer(cli, pkt->header.len, (void**)&p, &alloc);
		  if ( p )
		    {
		      assert(rtalloc_check_buffer(alloc, p));
		      memcpy(p, pkt, pkt->header.len);
		    }
		}
	      
	      if ( ret == 0 )
		{
		  assert(rtalloc_check_buffer(alloc, p));
		  ret = dsp_queue_req(cli, p, alloc);
		  if ( ret != 0 )
		    {
		      ret = dsp_reply_generic_error(dev, p->header.unique, ret);
		      dspd_rtalloc_free(alloc, p);
		    }
		} else
		{
		  ret = dsp_reply_generic_error(dev, p->header.unique, ret);
		}
		
	    } else
	    {
	      ret = dsp_reply_generic_error(dev, pkt->header.unique, EBADF);
	    }
	  
	} else
	{
	  ret = dsp_reply_generic_error(dev, pkt->header.unique, ENOSYS);
	}
    }
  return ret;
}

static int ctl_new_client(struct oss_dsp_cdev *dev, struct oss_cdev_client **cliptr)
{
  struct oss_cdev_client *c = calloc(1, sizeof(struct oss_cdev_client));
  int err = -ENOMEM;
  size_t br;
  if ( c )
    {
      
      c->cdev_slot = cdev_find_slot(dev);
      
      
      if ( c->cdev_slot < 0 )
	{
	  err = -EBUSY;
	  goto error;
	}
      if ( dev->cdev_index >= 0 )
	{
	  if ( dev->playback_index < 0 )
	    c->device_index = dev->capture_index;
	  else
	    c->device_index = dev->playback_index;
	  err = dspd_daemon_ref(c->device_index, DSPD_DCTL_ENUM_TYPE_SERVER);
	  if ( err )
	    {
	      err = -ENODEV;
	      goto error;
	    }
	  err = dspd_stream_ctl(&dspd_dctx,
				c->device_index,
				DSPD_SCTL_SERVER_STAT,
				NULL,
				0,
				&c->dsp.devinfo,
				sizeof(c->dsp.devinfo),
				&br);
	  if ( err )
	    {
	      err = -ENODEV;
	      goto error;
	    }
	} else
	{
	  c->device_index = -1;
	}
   
      if ( c->device_index > 0 )
	{
	  err = oss_new_legacy_mixer_assignments(c->device_index, &c->elements);
	  if ( err != ENXIO && err != 0 )
	    goto error;
	}

      c->client_index = -1;
      c->cdev = dev;
      c->dsp.max_write = c->cdev->cdev->params.maxwrite;
      c->dsp.max_read = c->cdev->cdev->params.maxread;
      c->fh = c->cdev_slot;
      c->fh <<= 32;
      if ( dev->cdev_index >= 0 )
	c->ops = &osscuse_legacy_ops;
      else
	c->ops = &osscuse_mixer_ops;
    } else
    {
      goto error;
    }
  c->cdev->clients[c->cdev_slot] = c;

  *cliptr = c;
  return 0;

 error:
  if ( c )
    {
      oss_delete_legacy_mixer_assignments(c->elements);
      free(c);
    }
  return err;
}

static int ctl_fd_event(void *data, 
			struct cbpoll_ctx *context,
			int index,
			int fd,
			int revents)
{
  ssize_t ret;
  struct oss_dsp_cdev *dev = data;
  struct oss_cdev_client *pcli = NULL;
  uint64_t fh;
  ret = read(fd, dev->ctlpkt, CTL_PKTLEN);
  if ( ret < 0 )
    {
      /*ret = errno;
      perror("read");
      errno = ret;*/
      //else Might have ENOMEM later, but otherwise safe.
      if ( errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK )
	return -1;
      return 0;
    }

  ret = -1;
  switch(dev->ctlpkt->header.opcode)
    {
    case FUSE_OPEN:
      //can always open mixer0
      if ( dev->error != 0 && dev->cdev_index != 0 )
	{
	  ret = dsp_reply_generic_error(dev, dev->ctlpkt->header.unique, dev->error);
	} else
	{
	  ret = ctl_new_client(dev, &pcli);
	  if ( ret < 0 )
	    {
	      ret = dsp_reply_generic_error(dev, dev->ctlpkt->header.unique, ret);
	    } else
	    {
	      assert(pcli);
	      ret = rtcuse_reply_open(pcli->cdev->cdev,
				      dev->ctlpkt->header.unique,
				      pcli->fh,
				      pcli->flags);
	    }
	}
      break;
    case FUSE_IOCTL:
      if ( dev->error )
	{
	  ret = dsp_reply_generic_error(dev, dev->ctlpkt->header.unique, dev->error);
	} else if ( rtcuse_get_fh(&dev->ctlpkt->header, &dev->ctlpkt->data[0], &fh) )
	{
	  pcli = dsp_find_client(dev, fh);
	  if ( pcli )
	    {
	      pcli->current_pkt = dev->ctlpkt;
	      ret = cdev_ioctl(pcli);
	      if ( ret > 0 )
		ret *= -1;
	    }
	} else
	{
	  ret = dsp_reply_generic_error(dev, dev->ctlpkt->header.unique, EBADFD);
	}
      break;
    case FUSE_RELEASE:
      if ( rtcuse_get_fh(&dev->ctlpkt->header, &dev->ctlpkt->data[0], &fh) )
	{
	  pcli = dsp_find_client(dev, fh);
	  if ( pcli )
	    {
	      dev->clients[pcli->cdev_slot] = NULL;
	      oss_delete_legacy_mixer_assignments(pcli->elements);
	      free(pcli);
	      ret = dsp_reply_generic_error(dev, dev->ctlpkt->header.unique, 0);
	    } else
	    {
	      ret = dsp_reply_generic_error(dev, dev->ctlpkt->header.unique, EBADFD);
	    }
	} else
	{
	  ret = dsp_reply_generic_error(dev, dev->ctlpkt->header.unique, EBADFD);
	}
      break;
    default:

      ret = dsp_reply_generic_error(dev, dev->ctlpkt->header.unique, ENOSYS);
    }

  if ( ret > 0 )
    ret = 0;
  return ret;
}


static bool session_exited(struct oss_cdev_client *cli)
{
  return cli->error != 0;
}

static void free_current_pkt(struct oss_cdev_client *cli)
{
  if ( cli->cdev->is_mixer )
    return;
  dspd_rtalloc_free(cli->current_iorp->alloc_ctx, cli->current_iorp->addr);
  dspd_fifo_rcommit(cli->eventq, 1);
  cli->current_count--;
  cli->current_iorp = NULL;
  cli->current_pkt = NULL;
  return;
}
			   

int oss_reply_write(struct oss_cdev_client *cli, size_t count)
{
  struct fuse_out_header hdr;
  struct fuse_write_out outarg;
  struct iovec iov[2];
  memset(&hdr, 0, sizeof(hdr));
  memset(&outarg, 0, sizeof(outarg));
  hdr.len = sizeof(outarg) + sizeof(hdr);
  hdr.error = 0;
  assert(count > 0);
  assert(cli->current_pkt->header.unique == cli->current_iorp->unique);
  assert(count <= (cli->current_pkt->header.len - (sizeof(struct fuse_in_header)+sizeof(struct fuse_write_in))));
  hdr.unique = cli->current_pkt->header.unique;
  outarg.size = count;
  outarg.padding = 0;

  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = &outarg;
  iov[1].iov_len = sizeof(outarg);
  free_current_pkt(cli);
  return rtcuse_writev_block(cli->cdev->cdev, iov, 2);
}
	
int oss_reply_buf(struct oss_cdev_client *cli, const char *buf, size_t size)
{
  struct iovec iov[2];
  struct fuse_out_header hdr;
  hdr.len = sizeof(hdr) + size;
  hdr.error = 0;
  hdr.unique = cli->current_pkt->header.unique;

  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = (void*)buf;
  iov[1].iov_len = size;
  free_current_pkt(cli);
  return rtcuse_writev_block(cli->cdev->cdev, iov, 2);
}

int oss_reply_poll(struct oss_cdev_client *cli, uint32_t revents)
{
  struct fuse_poll_out out = { 0 };
  struct fuse_out_header hdr;
  struct iovec iov[2];
  out.revents = revents;
  hdr.unique = cli->current_pkt->header.unique;
  hdr.error = 0;
  hdr.len = sizeof(hdr) + sizeof(out);
  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = &out;
  iov[1].iov_len = sizeof(out);
  free_current_pkt(cli);
  return rtcuse_writev_block(cli->cdev->cdev, iov, 2);
}

int oss_notify_poll(struct oss_cdev_client *cli)
{
  int ret;
  struct iovec iov[2];
  struct fuse_notify_poll_wakeup_out out;
  struct fuse_out_header hdr;
  if ( cli->pollhandle )
    {
      hdr.unique = 0;
      hdr.error = FUSE_NOTIFY_POLL;
      hdr.len = sizeof(hdr) + sizeof(out);
      out.kh = cli->pollhandle;
      iov[0].iov_base = &hdr;
      iov[0].iov_len = sizeof(hdr);
      iov[1].iov_base = &out;
      iov[1].iov_len = sizeof(out);
      ret = rtcuse_writev_block(cli->cdev->cdev, iov, 2);
    } else
    {
      ret = 0;
    }
  return ret;
}		       

int oss_reply_ioctl(struct oss_cdev_client *cli, uint32_t result, const void *buf, size_t size)
{
  struct fuse_ioctl_out arg = { 0 };
  struct fuse_out_header hdr;
  struct iovec iov[3];
  int count = 2;
  hdr.len = sizeof(hdr) + sizeof(arg) + size;
  hdr.error = 0;
  hdr.unique = cli->current_pkt->header.unique;
  arg.result = result;

  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = &arg;
  iov[1].iov_len = sizeof(arg);
  
  if ( size )
    {
      iov[2].iov_base = (void*)buf;
      iov[2].iov_len = size;
      count++;
    }
  free_current_pkt(cli);
  return rtcuse_writev_block(cli->cdev->cdev, iov, count);
}

/*
  The return values should be:
  EINTR: Current request was interrupted.
  EINPROGRESS: A new request is in progress, so try to finish up
  as soon as possible (maybe do a short io op).  Do try to do some of
  the work if possible.
*/
int oss_req_interrupted(struct oss_cdev_client *cli)
{
  int intr = 0;
  uint32_t len;
  if ( AO_load(&cli->current_iorp->canceled) == IORP_CANCELED )
    {
      intr = EINTR;
    } else
    {
      //If the length changed then some commands were added
      if ( dspd_fifo_len(cli->eventq, &len) == 0 )
	{
	  if ( len > cli->current_count )
	    intr = EINPROGRESS;
	}
    }
  return intr;
}

/*
  Send an error to the client.  Note that if error==0 and a write 
  was expected then the client gets EIO.
*/
int oss_reply_error(struct oss_cdev_client *cli, int32_t error)
{
  struct iovec iov;
  struct fuse_out_header hdr;
  int ret;
  memset(&hdr, 0, sizeof(hdr));
  
  hdr.len = sizeof(hdr);
  hdr.error = error * -1;
  hdr.unique = cli->current_pkt->header.unique;
  iov.iov_base = &hdr;
  iov.iov_len = sizeof(hdr);
  free_current_pkt(cli);

  ret = rtcuse_writev_block(cli->cdev->cdev, &iov, 1);
  if ( ret == iov.iov_len )
    ret = 0;
  return ret;
}




static bool queue_ready(struct oss_cdev_client *cli)
{
  bool ret;
  uint32_t len;
  if ( cli->wakeup == AO_TS_SET )
    {
      AO_CLEAR(&cli->wakeup);
      ret = 1;
    } else if ( dspd_fifo_len(cli->eventq, &len) == 0 )
    {
      ret = (len > 0);
    } else
    {
      ret = 0;
    }
  return ret;
}

static bool fifo_changed(struct oss_cdev_client *cli)
{
  uint32_t len;
  bool ret = 0;
  if ( dspd_fifo_len(cli->eventq, &len) == 0 )
    {
      ret = ( len != cli->current_count );
    }
  return ret;
}

int dspd_cdev_client_sleep(struct oss_cdev_client *cli, 
			   dspd_time_t *abstime, 
			   bool alertable)
{
  /*

    Try to sleep until abstime.

    Args:
    cli         A dsp client instance
    abstime     Sleep until this time or indefinitely if NULL
    alertable   If true, then return early when a new io request arrives.

    Return values:
    0: Possible spurious wakeup.  Caller should check some stuff and go back to sleep
       or finish the operation if appropriate.
    EINTR: Request interrupted.  If no further read or write happens then
    call again with abstime.
    EBADF: File descriptor is being closed.
    EINPROGRESS: New requests were queued
    ETIMEDOUT: Timed out and nothing else happened.
   */
  uint32_t c;
  int ret = 0, err;
  struct timespec t;
  if ( abstime == NULL )
    {
      dspd_mutex_lock(&cli->lock);
      while ( cli->wakeup != AO_TS_SET && cli->op_error == 0 )
	dspd_cond_wait(&cli->event, &cli->lock);
      dspd_mutex_unlock(&cli->lock);
    } else
    {
      dspd_time_to_timespec(*abstime, &t);
      dspd_mutex_lock(&cli->lock);
      
      while ( (alertable != 0 && fifo_changed(cli) == 0) || (alertable == 0) )
	{
	  if ( AO_load(&cli->current_iorp->canceled) )
	    {
	      if ( cli->current_pkt->header.opcode != FUSE_RELEASE &&
		   cli->current_pkt->header.opcode != FUSE_IOCTL )
		break;
	    }
	  ret = dspd_cond_timedwait(&cli->event, &cli->lock, &t);
	  if ( ret == ETIMEDOUT )
	    {
	      dspd_mutex_unlock(&cli->lock);
	      return ETIMEDOUT;
	    }
	}
      dspd_mutex_unlock(&cli->lock);
    }

  err = dspd_fifo_len(cli->eventq, &c);
  assert(err == 0);
  if ( cli->error )
    {
      ret = EBADF;
    } else if ( AO_load(&cli->current_iorp->canceled) )
    {
      ret = EINTR;
    } else if ( cli->current_count != c )
    {
      ret = EINPROGRESS;
    } else if ( cli->op_error )
    {
      ret = cli->op_error;
      if ( ret < 0 )
	ret *= -1;
    }
  return ret;
}


static int32_t cdev_read(struct oss_cdev_client *cli)
{
  int ret;
  struct fuse_read_in *in = (struct fuse_read_in*)cli->current_pkt->data;
  if ( cli->ops && cli->ops->read )
    {
      cli->ops->read(cli, in->size, in->offset, in->flags);
      ret = 0;
    } else
    {
      ret = ENOSYS;
    }
  return ret;
}
static int32_t cdev_write(struct oss_cdev_client *cli)
{
  int ret;
  struct fuse_write_in *in = (struct fuse_write_in*)cli->current_pkt->data;
  const char *ptr;
  if ( cli->ops && cli->ops->write )
    {
      ptr = (const char*)in;
      cli->ops->write(cli, &ptr[sizeof(*in)], in->size, in->offset, in->flags);
      ret = 0;
    } else
    {
      ret = ENOSYS;
    }
  return ret;
}
static int32_t cdev_ioctl(struct oss_cdev_client *cli)
{
  int ret;
  struct fuse_ioctl_in *in = (struct fuse_ioctl_in *)cli->current_pkt->data;
  void *in_buf;
  if ( in->in_size )
    in_buf = &cli->current_pkt->data[sizeof(*in)];
  else
    in_buf = NULL;
  if ( cli->ops && cli->ops->ioctl )
    {
      cli->ops->ioctl(cli,
		      in->cmd,
		      (void*)(uintptr_t)in->arg,
		      in->flags,
		      in_buf,
		      in->in_size,
		      in->out_size);
      ret = 0;
    } else
    {
      ret = ENOSYS;
    }
  return ret;
}
static int32_t cdev_poll(struct oss_cdev_client *cli)
{
  int ret;
  struct fuse_poll_in *in = (struct fuse_poll_in*)cli->current_pkt->data;
  if ( cli->ops && cli->ops->poll )
    {
      cli->ops->poll(cli, in->kh);
      ret = 0;
    } else
    {
      ret = ENOSYS;
    }
  return ret;
}
static int32_t cdev_release(struct oss_cdev_client *cli)
{
  int ret;
  if ( cli->ops && cli->ops->release )
    {
      cli->ops->release(cli);
      ret = 0;
    } else
    {
      ret = 0;
      oss_reply_error(cli, 0);
    }
  //All further access (should not happen) gets EBADF 
  cli->error = EBADF;
  
 
  return ret;
}
static cdev_callback_t dsp_cdev_ops[] = {
  [FUSE_READ] = cdev_read,
  [FUSE_WRITE] = cdev_write,
  [FUSE_RELEASE] = cdev_release,
  [FUSE_FSYNC] = NULL,
  [FUSE_FLUSH] = NULL,
  [FUSE_IOCTL] = cdev_ioctl,
  [FUSE_POLL] = cdev_poll,
};


uint32_t dsp_check_revents(struct oss_cdev_client *cli)
{
  int32_t revents = 0;
  int ret;
  if ( cli->error )
    {
      revents |= POLLHUP;
      if ( cli->dsp.params.stream & DSPD_PCM_SBIT_PLAYBACK )
	revents |= POLLOUT;
      if ( cli->dsp.params.stream & DSPD_PCM_SBIT_CAPTURE )
	revents |= POLLHUP;
    }
  if ( cli->dsp.params.stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      ret = dspd_rclient_avail(&cli->dsp.rclient, DSPD_PCM_SBIT_PLAYBACK);
      if ( ret < 0 && ret != -EPIPE )
	revents |= (POLLHUP | POLLOUT);
      else if ( ret >= cli->dsp.rclient.swparams.avail_min || ret == -EPIPE )
	revents |= POLLOUT;
    }
  if ( cli->dsp.params.stream & DSPD_PCM_SBIT_CAPTURE )
    {
      ret = dspd_rclient_avail(&cli->dsp.rclient, DSPD_PCM_SBIT_CAPTURE);
      if ( ret < 0 && ret != -EPIPE )
	revents |= (POLLHUP | POLLIN);
      else if ( ret >= cli->dsp.rclient.swparams.avail_min || ret == -EPIPE )
	revents |= POLLIN;
    }
  return revents;
}

bool dsp_check_poll(struct oss_cdev_client *cli)
{
  uint32_t revents;
  bool ret = 0;
  if ( cli->poll_ok && cli->poll_armed )
    {
      revents = dsp_check_revents(cli);
      if ( revents )
	{
	  oss_notify_poll(cli);
	  cli->poll_armed = 0;
	  ret = 1;
	}
    }
  return ret;
}

static void *dsp_worker(void *p)
{
  struct oss_cdev_client *cli = p;
  struct iorp *req;
  uint32_t len;
  int c, ret;
  cdev_callback_t cb;
  dspd_time_t waketime;
  struct timespec ts;
  char name[33];
  sprintf(name, "dsp-worker-%s", cli->dsp.devinfo.name);
  prctl(PR_SET_NAME, name, 0, 0, 0);


  //Assign the slot so new messages have somewhere to go.
  cli->cdev->clients[cli->fh >> 32] = cli;
  if ( rtcuse_reply_open(cli->cdev->cdev,
			 cli->unique,
			 cli->fh,
			 cli->flags) < 0 )
    goto out;
  while ( ! session_exited(cli) )
    {
      /*
	Should wait for an incoming request or a change in the buffers (poll notify).

	To get this running it is probably a good idea to implement raw writes and 
	a single ioctl.  When that is verified then create an oss ioctl library
	that uses rclient.

	Note that an write callbacks will need to set up cli->fh.

      */

      if ( cli->wakeup != AO_TS_SET )
	{

	  dspd_mutex_lock(&cli->lock);
	 
	  if ( ! dsp_check_poll(cli) )
	    {
	      if ( ! queue_ready(cli) )
		{
		  if ( cli->dsp.params.stream & DSPD_PCM_SBIT_PLAYBACK )
		    dspd_rclient_status(&cli->dsp.rclient, DSPD_PCM_SBIT_PLAYBACK, NULL);
		  if ( cli->dsp.params.stream & DSPD_PCM_SBIT_CAPTURE )
		    dspd_rclient_status(&cli->dsp.rclient, DSPD_PCM_SBIT_CAPTURE, NULL);

		  ret = dspd_rclient_get_next_wakeup(&cli->dsp.rclient,
						     cli->dsp.params.stream,
						     &waketime);

		  if ( ret == 0 )
		    {
		      if ( ! queue_ready(cli) )
			{
			  ts.tv_sec = waketime / 1000000000;
			  ts.tv_nsec = waketime % 1000000000;
			  dspd_cond_timedwait(&cli->event, &cli->lock, &ts);
			}
		    } else
		    {
		      usleep(1);
		    }
		  dsp_check_poll(cli);
		}
	    }
	    

	  //Sleep after notifying
	  while ( ! (queue_ready(cli) || session_exited(cli)) )
	    dspd_cond_wait(&cli->event, &cli->lock);
	  dspd_mutex_unlock(&cli->lock);

	} else
	{
	  AO_CLEAR(&cli->wakeup);
	}

      while ( dspd_fifo_riov(cli->eventq, (void**)&req, &len) == 0 )
	{
	  dsp_check_poll(cli);

	  if ( len == 0 )
	    break;
	  
	  cli->current_iorp = req;
	  cli->current_pkt = req->addr;
	  

	  dspd_fifo_len(cli->eventq, &cli->current_count);
	  if ( (c = AO_load(&req->canceled)) )
	    {
	      if ( c == IORP_CANCELED )
		{
		  if ( cli->current_pkt->header.opcode != FUSE_IOCTL && 
		       cli->current_pkt->header.opcode != FUSE_RELEASE )
		    {
		      oss_reply_error(cli, EINTR);
		      continue;
		    }
		} else
		{
		  dspd_fifo_rcommit(cli->eventq, 1);
		  continue;
		}
	    }

	  if ( cli->current_pkt->header.opcode >= (sizeof(dsp_cdev_ops)/sizeof(dsp_cdev_ops[0])) )
	    {
	      oss_reply_error(cli, ENOSYS);
	      continue;
	    }
	  int e;
	  if ( cli->op_error )
	    e = cli->op_error;
	  else
	    e = cli->cdev->error;
	  if ( cli->current_pkt->header.opcode != FUSE_RELEASE && e != 0 )
	    {
	      if ( cli->op_error == 0 )
		cli->op_error = e;
	      oss_reply_error(cli, e);
	    } else
	    {

	      cb = dsp_cdev_ops[cli->current_pkt->header.opcode];

	      if ( cb == NULL )
		{
		  if ( cli->current_pkt->header.opcode == FUSE_RELEASE )
		    {
		      oss_reply_error(cli, 0);
		      break;
		    } else
		    {
		      oss_reply_error(cli, ENOSYS);
		    }
		} else
		{
		  ret = cb(cli);
		  if ( ret )
		    oss_reply_error(cli, ret);
		}
	    }
	  

	  if ( session_exited(cli) )
	    break;
	}
    }

  out:

  dsp_client_release_notify(cli->cdev, cli->cdev_slot);
  return NULL;
}


static void cdev_destroy(struct oss_dsp_cdev *cdev)
{
  remove_cdev(cdev);
  if ( cdev->alloc )
    dspd_rtalloc_delete(cdev->alloc);
  rtcuse_destroy_cdev(cdev->cdev);

  if ( cdev->playback_index > 0 )
    dspd_daemon_unref(cdev->playback_index);

  if ( cdev->capture_index > 0 )
    dspd_daemon_unref(cdev->capture_index);

  dspd_mutex_destroy(&cdev->lock);
  free(cdev->ctlpkt);


  free(cdev);
}

static bool cdev_destructor(void *data,
			    struct cbpoll_ctx *context,
			    int index,
			    int fd)
{
  struct oss_dsp_cdev *cdev = data;
  if ( cdev->is_mixer )
    dspd_log(0, "Destroying control device %d", cdev->cdev_index);
  else
    dspd_log(0, "Destroying character device %d", cdev->cdev_index);
  cdev_destroy(cdev);
  return false;
}




static const struct cbpoll_fd_ops dsp_ops = {
  .fd_event = dsp_fd_event,
  .destructor = cdev_destructor,
};

static void async_add_fd(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *pe)
{
  struct oss_dsp_cdev *dev = (void*)(uintptr_t)pe->arg;
  int ret = cbpoll_add_fd(ctx, 
			  dev->cdev->fd, 
			  POLLIN,
			  &dsp_ops,
			  dev);
  if ( ret < 0 )
    cdev_destroy(dev);
  else
    dev->cbpoll_index = ret;
}

static const struct cbpoll_fd_ops ctl_ops = {
  .fd_event = ctl_fd_event,
  .destructor = cdev_destructor,
};


static void async_add_ctlfd(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *pe)
{
  struct oss_dsp_cdev *dev = (void*)(uintptr_t)pe->arg;
  int ret = cbpoll_add_fd(ctx, 
			  dev->cdev->fd, 
			  POLLIN,
			  &ctl_ops,
			  dev);
  if ( ret < 0 )
    cdev_destroy(dev);
  else
    dev->cbpoll_index = ret;
}



static struct oss_dsp_cdev *cdev_new(int32_t device, const struct dspd_device_stat *stat)
{
  const char *p = strchr(stat->name, ':');
  struct oss_dsp_cdev *dev;
  size_t npages, pagelen;
  struct rtcuse_cdev_params params;
  int32_t dspnum, err;
  if ( ! p )
    return NULL;
  p = &p[1];
  if ( dspd_strtoi32(p, &dspnum, 10) < 0 )
    return NULL;

  dev = calloc(1, sizeof(*dev));
  if ( ! dev )
    return NULL;
  params = server_context.dsp_params;
  //This is not really what it is supposed to do, but that doesn't seem to matter on 
  //modern systems.
  params.minor += dspnum * 2;

  sprintf(params.devname, "%sdsp%d", server_context.devnode_prefix, dspnum);
  dev->cdev_index = dspnum;
  if ( stat->streams & DSPD_PCM_SBIT_PLAYBACK )
    dev->playback_index = device;
  else
    dev->playback_index = -1;
  if ( stat->streams & DSPD_PCM_SBIT_CAPTURE )
    dev->capture_index = device;
  else
    dev->capture_index = -1;
  err = osscuse_create_cdev(dspnum, DEVTYPE_DSP, &params, &dev->cdev);
  if ( err != 0 )
    goto error;
  
  
  pagelen = 128;
  npages = dev->cdev->params.pktlen / pagelen;
  if ( dev->cdev->params.pktlen % pagelen )
    npages++;
  dev->alloc = dspd_rtalloc_new(pagelen, npages * 2);
  if ( ! dev->alloc )
    goto error;
  if ( dspd_mutex_init(&dev->lock, NULL) != 0 )
    goto error;


  return dev;

 error:
  dspd_mutex_destroy(&dev->lock);
  if ( dev->alloc )
    dspd_rtalloc_delete(dev->alloc);
  if ( dev->cdev )
    rtcuse_destroy_cdev(dev->cdev);
  free(dev);
  return NULL;
}

static struct oss_dsp_cdev *ctl_new(int32_t device, const struct dspd_device_stat *stat)
{
  const char *p;
  struct oss_dsp_cdev *dev;
  struct rtcuse_cdev_params params;
  int32_t dspnum, err;

  if ( stat )
    {
      p = strchr(stat->name, ':');
      if ( ! p )
	return NULL;
      
      p = &p[1];
      if ( dspd_strtoi32(p, &dspnum, 10) < 0 )
	return NULL;
    } else
    {
      dspnum = -1;
    }
  
  dev = calloc(1, sizeof(*dev));
  if ( ! dev )
    return NULL;
  dev->is_mixer = true;
  params = server_context.dsp_params;
  
  if ( dspnum > 0 )
    {
      sprintf(params.devname, "%smixer%d", server_context.devnode_prefix, dspnum);
    } else
    {
      sprintf(params.devname, "%smixer0", server_context.devnode_prefix);
      dspnum = 0;
    }
  dev->cdev_index = dspnum;
  if ( stat )
    {
      if ( stat->streams & DSPD_PCM_SBIT_PLAYBACK )
	dev->playback_index = device;
      else
	dev->playback_index = -1;
      if ( stat->streams & DSPD_PCM_SBIT_CAPTURE )
	dev->capture_index = device;
      else
	dev->capture_index = -1;
    }
  params.maxwrite = 4096;
  params.maxread = 4096;
  //Not really correct.
  params.minor += (dspnum * 2) + 1;
  err = osscuse_create_cdev(dspnum, DEVTYPE_MIXER, &params, &dev->cdev);
  if ( err != 0 )
    goto error;
    
  dev->ctlpkt = calloc(1, CTL_PKTLEN);
  if ( dev->ctlpkt == NULL )
    goto error;
  if ( dspd_mutex_init(&dev->lock, NULL) != 0 )
    goto error;

  
 
  return dev;

 error:
  dspd_mutex_destroy(&dev->lock);
  free(dev->ctlpkt);
  if ( dev->alloc )
    dspd_rtalloc_delete(dev->alloc);
  if ( dev->cdev )
    rtcuse_destroy_cdev(dev->cdev);
  free(dev);
  return NULL;
}

struct oss_dsp_cdev *oss_find_cdev(int dspnum)
{
  size_t i;
  struct oss_dsp_cdev *dev;
  if ( dspnum < 0 || dspnum >= ARRAY_SIZE(server_context.dsp_table) )
    return NULL;
  if ( server_context.dsp_table[dspnum] != NULL &&
       server_context.dsp_table[dspnum]->cdev_index == dspnum &&
       server_context.dsp_table[dspnum]->dead == false )
    return server_context.dsp_table[dspnum];
  for ( i = 0; i < ARRAY_SIZE(server_context.dsp_table); i++ )
    {
      dev = server_context.dsp_table[i];
      if ( dev )
	{
	  if ( dev->cdev_index == dspnum && dev->dead != true )
	    {
	      return dev;
	    }
	}
    }
  return NULL;
}

struct oss_dsp_cdev *oss_find_ctl(int dspnum)
{
  size_t i;
  struct oss_dsp_cdev *dev;
  if ( dspnum < 0 || dspnum >= ARRAY_SIZE(server_context.ctl_table) )
    return NULL;
  if ( server_context.ctl_table[dspnum] != NULL &&
       server_context.ctl_table[dspnum]->cdev_index == dspnum &&
       server_context.ctl_table[dspnum]->dead == false )
    return server_context.ctl_table[dspnum];
  for ( i = 0; i < ARRAY_SIZE(server_context.ctl_table); i++ )
    {
      dev = server_context.ctl_table[i];
      if ( dev )
	{
	  if ( dev->cdev_index == dspnum && dev->dead != true )
	    {
	      return dev;
	    }
	}
    }
  return NULL;
}


struct oss_dsp_cdev *oss_lock_cdev(int dspnum)
{
  struct oss_dsp_cdev *dev;
  pthread_rwlock_rdlock(&server_context.devtable_lock);
  dev = oss_find_cdev(dspnum);
  if ( ! dev )
    pthread_rwlock_unlock(&server_context.devtable_lock);
  return dev;
}

void oss_unlock_cdev(struct oss_dsp_cdev *dev)
{
  pthread_rwlock_unlock(&server_context.devtable_lock);
}

struct oss_dsp_cdev *oss_lock_ctldev(int dspnum)
{
  struct oss_dsp_cdev *dev;
  pthread_rwlock_rdlock(&server_context.devtable_lock);
  dev = oss_find_ctl(dspnum);
  if ( ! dev )
    pthread_rwlock_unlock(&server_context.devtable_lock);
  return dev;
}

void oss_unlock_ctldev(struct oss_dsp_cdev *dev)
{
  pthread_rwlock_unlock(&server_context.devtable_lock);
}



size_t oss_mixer_count(void)
{
  size_t i, count = 0;
  const struct oss_dsp_cdev *dev;
  for ( i = 0; i < ARRAY_SIZE(server_context.dsp_table); i++ )
    {
      dev = server_context.dsp_table[i];
      if ( dev )
	if ( dev->cdev_index >= 0 &&
	     (dev->playback_index >= 0 || dev->capture_index >= 0))
	  count++;
    }
  return count;
}







static void insert_cdev(struct oss_dsp_cdev *cdev)
{
  pthread_rwlock_wrlock(&server_context.devtable_lock);
  server_context.dsp_table[cdev->cdev_index] = cdev;
  pthread_rwlock_unlock(&server_context.devtable_lock);
}

static void remove_cdev(struct oss_dsp_cdev *cdev)
{
  size_t i;

  pthread_rwlock_wrlock(&server_context.devtable_lock);

  if ( cdev->cdev_index >= 0 )
    {
      struct oss_dsp_cdev **arr;
      if ( cdev->is_mixer )
	arr = server_context.ctl_table;
      else
	arr = server_context.dsp_table;
      //assert(arr[cdev->cdev_index]);
      //arr[cdev->cdev_index] = NULL;

      if ( arr[cdev->cdev_index] == cdev )
	{
	  arr[cdev->cdev_index] = NULL;
	} else
	{
	  assert(ARRAY_SIZE(server_context.dsp_table) == ARRAY_SIZE(server_context.ctl_table));
	  for ( i = 0; i < ARRAY_SIZE(server_context.dsp_table); i++ )
	    {
	      if ( arr[i] == cdev )
		{
		  arr[i] = NULL;
		  break;
		}
	    }
	}

    } else
    {
      assert(cdev == server_context.v4_mixer);
      server_context.v4_mixer = NULL;
    }
  pthread_rwlock_unlock(&server_context.devtable_lock);
}

static void insert_ctldev(struct oss_dsp_cdev *cdev)
{
  pthread_rwlock_wrlock(&server_context.devtable_lock);
  if ( cdev->cdev_index >= 0 )
    {
      server_context.ctl_table[cdev->cdev_index] = cdev;
    } else
    {
      assert(server_context.v4_mixer == NULL);
      server_context.v4_mixer = cdev;
    }
  pthread_rwlock_unlock(&server_context.devtable_lock);
}








static void oss_init_device(void *arg, const struct dspd_dict *device)
{
  struct oss_dsp_cdev *cdev, *ctl = NULL;
  const struct dspd_kvpair *slot;
  int32_t ret;
  int32_t index, dspnum;
  struct dspd_device_stat info;
  size_t br;
  struct cbpoll_pipe_event pe = { 0 };
  slot = dspd_dict_find_pair(device, DSPD_HOTPLUG_SLOT);
  bool dead;
  if ( ! slot )
    return;
  if ( ! slot->value )
    return;
  index = atoi(slot->value);
  if ( dspd_daemon_ref((uint32_t)index, DSPD_DCTL_ENUM_TYPE_SERVER) != 0 )
    return;

  ret = dspd_stream_npctl({.context = &dspd_dctx,
	.stream = index,
	.request = DSPD_SCTL_SERVER_STAT,
	.outbuf = &info,
	.outbufsize = sizeof(info),
	.bytes_returned = &br});

  //dspd_daemon_unref((uint32_t)index);
  
  if ( ret != 0 || br != sizeof(info) )
    goto out;
  

  const char *p = strchr(info.name, ':');
  if ( ! p )
    goto out;

  p = &p[1];

  dspnum = atoi(p);

  
  pthread_rwlock_wrlock(&server_context.devtable_lock);
  cdev = oss_find_cdev(dspnum);
  if ( cdev )
    {
      dead = true;
      if ( ! cdev->dead )
	{
	  dspd_mutex_lock(&cdev->lock);
	  if ( ! cdev->dead )
	    {
	      dead = false;
	      dspd_daemon_ref(index, DSPD_DCTL_ENUM_TYPE_SERVER);
	      if ( info.streams & DSPD_PCM_SBIT_PLAYBACK )
		{
		  if ( cdev->playback_index > 0 )
		    dspd_daemon_unref(cdev->playback_index);
		  cdev->playback_index = index;
		}
	      if ( info.streams & DSPD_PCM_SBIT_CAPTURE )
		{
		  if ( cdev->capture_index > 0 )
		    dspd_daemon_unref(cdev->capture_index);
		  cdev->capture_index = index;
		}
	      cdev->error = 0;
	    }
	  dspd_mutex_unlock(&cdev->lock);
	}
      if ( dead )
	cdev = NULL;
    }
  if ( server_context.enable_legacymixer )
    {
      ctl = oss_find_ctl(dspnum);
      if ( ctl )
	{
	  dead = true;
	  if ( ! ctl->dead )
	    {
	      dspd_mutex_lock(&ctl->lock);
	      if ( ! ctl->dead )
		{
		  dead = false;
		  dspd_daemon_ref(index, DSPD_DCTL_ENUM_TYPE_SERVER);
		  if ( info.streams & DSPD_PCM_SBIT_PLAYBACK )
		    {
		      if ( ctl->playback_index > 0 )
			dspd_daemon_unref(ctl->playback_index);
		      ctl->playback_index = index;
	  
		    }
		  if ( info.streams & DSPD_PCM_SBIT_CAPTURE )
		    {
		      if ( ctl->capture_index > 0 )
			dspd_daemon_unref(ctl->capture_index);
		      ctl->capture_index = index;
		    }
		  ctl->error = 0;
		}
	      dspd_mutex_unlock(&ctl->lock);
	    }
	  if ( dead )
	    ctl = NULL;
	}
    }
  pthread_rwlock_unlock(&server_context.devtable_lock);
  if ( ! cdev )
    {

      cdev = cdev_new(index, &info);

      if ( cdev )
	{
	  dspd_daemon_ref(index, DSPD_DCTL_ENUM_TYPE_SERVER);
	  pe.fd = cdev->cdev->fd;
	  pe.callback = async_add_fd;
	  pe.index = -1;

	  pe.stream = -1;
	  pe.msg = CBPOLL_PIPE_MSG_CALLBACK;
	  pe.arg = (intptr_t)cdev;
	  insert_cdev(cdev);
	  if ( cbpoll_send_event(&server_context.cbpoll, &pe) != 0 )
	    {
	      close(pe.fd);
	      cdev_destroy(cdev);
	    }
	}
    }
  if ( ctl == NULL && server_context.enable_legacymixer == true )
    {
      ctl = ctl_new(index, &info);
      if ( ctl )
	{
	  dspd_daemon_ref(index, DSPD_DCTL_ENUM_TYPE_SERVER);
	  pe.fd = ctl->cdev->fd;
	  pe.callback = async_add_ctlfd;
	  pe.index = -1;

	  pe.stream = -1;
	  pe.msg = CBPOLL_PIPE_MSG_CALLBACK;
	  pe.arg = (intptr_t)ctl;
	  insert_ctldev(ctl);

	  

	  if ( cbpoll_send_event(&server_context.ctl_cbpoll, &pe) != 0 )
	    {
	      close(pe.fd);
	      cdev_destroy(ctl);
	    }
	}

    }
  
 out:
  dspd_daemon_unref((uint32_t)index);
}



static int oss_remove(void *arg, const struct dspd_dict *device)
{
  const struct dspd_kvpair *dev = dspd_dict_find_pair(device, DSPD_HOTPLUG_DEVNAME);
  const char *p;
  int32_t n;
  struct oss_dsp_cdev *cdev;
  if ( dev != NULL && dev->value != NULL )
    {
      p = strchr(dev->value, ':');
      if ( p != NULL && dspd_strtoi32(&p[1], &n, 0) == 0 )
	{
	  cdev = oss_lock_cdev(n);
	  if ( cdev )
	    {
	      dsp_cdev_seterror(cdev, -ENODEV);
	      if ( cdev->playback_index > 0 )
		{
		  dspd_daemon_unref(cdev->playback_index);
		  cdev->playback_index = -1;
		}
	      if ( cdev->capture_index > 0 )
		{
		  dspd_daemon_unref(cdev->capture_index);
		  cdev->capture_index = -1;
		}
	      oss_unlock_cdev(cdev);
	    }
	  cdev = oss_lock_ctldev(n);
	  if ( cdev )
	    {
	      dsp_cdev_seterror(cdev, -ENODEV);
	      if ( cdev->playback_index > 0 )
		{
		  dspd_daemon_unref(cdev->playback_index);
		  cdev->playback_index = -1;
		}
	      if ( cdev->capture_index > 0 )
		{
		  dspd_daemon_unref(cdev->capture_index);
		  cdev->capture_index = -1;
		}
	      oss_unlock_cdev(cdev);
	    }
	}
    }
  return -ENODEV;
}


static struct dspd_hotplug_cb oss_hotplug = {
  .score = NULL,
  .add = NULL,
  .remove = oss_remove,
  .init_device = oss_init_device,
};


static void oc_close(void *daemon, void **context)
{

}

static void *dummy_thread(void *p)
{
  return NULL;
}



static ssize_t write_data(int fd, const void *ptr, size_t len)
{
  size_t offset = 0;
  ssize_t ret;
  while ( offset < len )
    {
      ret = write(fd, (const char*)ptr + offset, len - offset);
      if ( ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
	return -errno;
      else if ( ret == 0 )
	return -EIO;
      offset += ret;
    }
  return 0;
}
static ssize_t read_data(int fd, void *ptr, size_t len)
{
  size_t offset = 0;
  ssize_t ret;
  while ( offset < len )
    {
      ret = read(fd, (char*)ptr + offset, len - offset);
      if ( ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
	return -errno;
      else if ( ret == 0 )
	return -EIO;
      offset += ret;
    }
  return 0;
}



static void osscuse_start_helper(void)
{
  int sockets[2];
  char path[PATH_MAX];
  char uid[16], gid[16], fd[16];
  struct stat fi;
  if ( server_context.helper == OSSCUSE_DISABLE_HELPER )
    return;
  if ( server_context.helper != OSSCUSE_ENABLE_HELPER )
    {
      if ( dspd_dctx.uid <= 0 )
	return;
      if ( stat("/dev/cuse", &fi) == 0 )
	{
	  if ( (fi.st_mode & S_IROTH) && (fi.st_mode & S_IWOTH) )
	    return;
	  if ( (fi.st_gid == dspd_dctx.gid) && (fi.st_mode & S_IRGRP) && (fi.st_mode & S_IWGRP) )
	    return;
	  if ( (fi.st_uid == dspd_dctx.uid) && (fi.st_mode & S_IRUSR) && (fi.st_mode & S_IWUSR) )
	    return;
	}
    }
  
  sprintf(path, "%s/osscuse_cdev_helper", dspd_get_modules_dir());
  if ( access(path, X_OK) < 0 )
    return;
  int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
  if ( ret < 0 )
    return;
 
  sprintf(uid, "%d", dspd_dctx.uid);
  sprintf(gid, "%d", dspd_dctx.gid);
  sprintf(fd, "%d", sockets[1]);
  ret = vfork();
  if ( ret > 0 )
    {
      server_context.cuse_helper_fd = sockets[0];
      close(sockets[1]);
    } else if ( ret == 0 )
    {
      close(sockets[0]);
      if ( execl(path, path, fd, uid, gid, NULL) < 0 )
	exit(1);
    } else
    {
      close(sockets[0]);
      close(sockets[1]);
    }
}

int osscuse_create_cdev(int devnum,
			int devtype,
			struct rtcuse_cdev_params *devp,
			struct rtcuse_cdev **dev)
{
  if ( server_context.cuse_helper_fd < 0 )
    return rtcuse_create_cdev(NULL, O_NONBLOCK, devp, dev);
  struct osscuse_open_req req;
  struct rtcuse_cdev *dptr;
  struct iovec iov;
  int ret, fd;
  int32_t result;
  dptr = calloc(1, sizeof(*dptr));
  memset(&req, 0, sizeof(req));
  req.params = *devp;
  req.devnum = devnum;
  req.devtype = devtype;
  ret = write_data(server_context.cuse_helper_fd, &req, sizeof(req));
  if ( ret < 0 )
    goto out;
  ret = read_data(server_context.cuse_helper_fd, &result, sizeof(result));
  if ( ret < 0 )
    goto out;
  if ( result < 0 )
    {
      ret = result;
      goto out;
    }
  iov.iov_len = sizeof(*dptr);
  iov.iov_base = dptr;
  ret = dspd_cmsg_recvfd(server_context.cuse_helper_fd, &iov);
  if ( ret < 0 )
    goto out;
  fd = ret;
  if ( iov.iov_len > 0 )
    {
      ret = read_data(server_context.cuse_helper_fd, iov.iov_base, iov.iov_len);
      if ( ret < 0 )
	goto out;
    }
  dptr->fd = fd;
  *dev = dptr;
  return 0;
  


 out:
  free(dptr);
  return ret;
}


#define MAXIO 65536
static int oc_init(void *daemon, void **context)
{
  int ret = dspd_daemon_hotplug_register(&oss_hotplug, NULL);
  const char *val;
  pthread_t thr;
  struct oss_dsp_cdev *ctl;
  struct cbpoll_pipe_event pe = { 0 };
  server_context.cuse_helper_fd = -1;
  server_context.config = dspd_read_config("mod_osscuse", true);
  server_context.devnode_prefix = "";
  if ( ret != 0 )
    dspd_log(0, "Could not register hotplug handler for osscuse: error %d", ret);
  else
    dspd_log(0, "Registered hotplug handler for osscuse");

  //Try to find the correct thread attributes.
  ret = dspd_daemon_threadattr_init(&server_context.client_threadattr,
				    sizeof(server_context.client_threadattr),
				    DSPD_THREADATTR_DETACHED | DSPD_THREADATTR_RTSVC);
  if ( ret == 0 )
    ret = pthread_create(&thr, &server_context.client_threadattr, dummy_thread, NULL);
  if ( ret == EPERM )
    {
      pthread_attr_destroy(&server_context.client_threadattr);
      ret = dspd_daemon_threadattr_init(&server_context.client_threadattr,
					sizeof(server_context.client_threadattr),
					DSPD_THREADATTR_DETACHED);
    }
  if ( ret != 0 )
    dspd_log(0, "Could not get thread attributes");
  
  pthread_condattr_init(&server_context.client_condattr);
  pthread_condattr_setclock(&server_context.client_condattr, CLOCK_MONOTONIC);
  pthread_rwlock_init(&server_context.devtable_lock, NULL);
  server_context.ctl_assignments = ossv4_table;
  server_context.helper = OSSCUSE_HELPER_AUTO;
  if ( ret == 0 )
    {
      server_context.dsp_params.maxwrite = MAXIO;
      server_context.dsp_params.maxread = MAXIO;
      server_context.dsp_params.major = 14;
      server_context.dsp_params.minor = 0;
      server_context.alloc = dspd_rtalloc_new(128, (MAXIO / 128) * 2);
      server_context.inbuf = malloc(MAXIO);
      server_context.enable_v4mixer = true;
      server_context.enable_legacymixer = true;
      server_context.persistent_devnodes = true;
      if ( server_context.config )
	{
	  val = NULL;
	  dspd_dict_find_value(server_context.config, "devnode_prefix", (char**)&val);
	  if ( val )
	    server_context.devnode_prefix = val;
	  val = NULL;
	  dspd_dict_find_value(server_context.config, "legacy_mixer", (char**)&val);
	  server_context.enable_legacymixer = !!dspd_strtoidef(val, server_context.enable_legacymixer);

	  val = NULL;
	  dspd_dict_find_value(server_context.config, "persistent_devnodes", (char**)&val);
	  server_context.persistent_devnodes = !!dspd_strtoidef(val, server_context.persistent_devnodes);

	  val = NULL;
	  dspd_dict_find_value(server_context.config, "dev_major", (char**)&val);
	  server_context.dsp_params.major = dspd_strtoidef(val, server_context.dsp_params.major);

	  
	  val = NULL;
	  dspd_dict_find_value(server_context.config, "cdev_helper", (char**)&val);
	  if ( val != NULL )
	    server_context.helper = dspd_strtoidef(val, server_context.helper);


	  val = NULL;
	  dspd_dict_find_value(server_context.config, "legacy_mixer_type", (char**)&val);
	  if ( val )
	    {
	      if ( strcmp(val, "compat") == 0 )
		server_context.ctl_assignments = ossv3_table;
	      else if ( strcmp(val, "4front") == 0 )
		server_context.ctl_assignments = ossv4_table;
	      else
		dspd_log(0, "Invalid legacy_mixer_type: '%s'", val);
	    }
	}

      ret = cbpoll_init(&server_context.cbpoll, 0, DSPD_MAX_OBJECTS);
      if ( ret == 0 )
	ret = cbpoll_set_name(&server_context.cbpoll, "dspd-ossdsp");
      if ( ret == 0 )
	ret = cbpoll_start(&server_context.cbpoll);
      if ( ret != 0 )
	dspd_log(0, "Could not create async event handler: error %d", ret);
      if ( ret == 0 )
	ret = cbpoll_init(&server_context.ctl_cbpoll, 0, DSPD_MAX_OBJECTS);
      if ( ret == 0 )
	ret = cbpoll_set_name(&server_context.ctl_cbpoll, "dspd-ossctl");
      if ( ret == 0 )
	ret = cbpoll_start(&server_context.ctl_cbpoll);
      if ( ret != 0 )
	dspd_log(0, "Could not create control event handler: error %d", ret);
      osscuse_start_helper();

      if ( ret == 0 )
	{
	  ctl = ctl_new(-1, NULL);
	  if ( ! ctl )
	    {
	      server_context.dsp_params.major = 0;
	      ctl = ctl_new(-1, NULL);
	    }
	  if ( ctl )
	    {
	      pe.fd = ctl->cdev->fd;
	      pe.callback = async_add_ctlfd;
	      pe.index = -1;

	      pe.stream = -1;
	      pe.msg = CBPOLL_PIPE_MSG_CALLBACK;
	      pe.arg = (intptr_t)ctl;
	      insert_ctldev(ctl);

	      if ( cbpoll_send_event(&server_context.ctl_cbpoll, &pe) != 0 )
		{
		  close(pe.fd);
		  cdev_destroy(ctl);
		}
	    }
	}


    }

  return ret;
}

static int oc_ioctl(void         *daemon, 
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

struct dspd_mod_cb dspd_mod_osscuse = {
  .desc = "OSSv4 CUSE Server",
  .init = oc_init,
  .close = oc_close,
  .ioctl = oc_ioctl,
};



#define SOUND_MIXER_PHONEIN SOUND_MIXER_PHONE
#define SOUND_MIXER_PHONEOUT SOUND_MIXER_MONO
#define SOUND_MIXER_MONITOR SOUND_MIXER_DEPTH
#define PVOL (DSPD_MIXF_PDB|DSPD_MIXF_PVOL)
#define CVOL (DSPD_MIXF_CDB|DSPD_MIXF_CVOL)
#define UNUSED_VOL 0
static const struct snd_mixer_oss_assign_table ossv4_table[] = {
  { SOUND_MIXER_VOLUME, 	"Master",		0, PVOL },
  { SOUND_MIXER_VOLUME, 	"Front",		0, PVOL }, /* fallback */
  { SOUND_MIXER_BASS,	"Tone Control - Bass",	0, PVOL },
  { SOUND_MIXER_TREBLE,	"Tone Control - Treble", 0, PVOL },
  { SOUND_MIXER_SYNTH,	"Synth",		0, PVOL },
  { SOUND_MIXER_SYNTH,	"FM",			0, PVOL }, /* fallback */
  { SOUND_MIXER_SYNTH,	"Music",		0, PVOL }, /* fallback */
  { SOUND_MIXER_PCM,	"PCM",			0, PVOL },
  { SOUND_MIXER_SPEAKER,	"Beep", 		0, PVOL },
  { SOUND_MIXER_SPEAKER,	"PC Speaker", 		0, PVOL }, /* fallback */
  { SOUND_MIXER_SPEAKER,	"Speaker", 		0, PVOL }, /* fallback */
  { SOUND_MIXER_LINE,	"Line", 		0, CVOL },
  { SOUND_MIXER_MIC,	"Mic", 			0, CVOL },
  { SOUND_MIXER_CD,	"CD", 			0, CVOL },
  { SOUND_MIXER_IMIX,	"Monitor Mix", 		0, UNUSED_VOL },
  { SOUND_MIXER_ALTPCM,	"PCM",			1, PVOL },
  { SOUND_MIXER_ALTPCM,	"Headphone",		0, PVOL }, /* fallback */
  { SOUND_MIXER_ALTPCM,	"Wave",			0, PVOL }, /* fallback */
  { SOUND_MIXER_RECLEV,	"-- nothing --",	0, UNUSED_VOL },
  { SOUND_MIXER_IGAIN,	"Capture",		0, PVOL },
  { SOUND_MIXER_OGAIN,	"Playback",		0, PVOL },
  { SOUND_MIXER_LINE1,	"Aux",			0, CVOL },
  { SOUND_MIXER_LINE2,	"Aux",			1, CVOL },
  { SOUND_MIXER_LINE3,	"Aux",			2, CVOL },
  { SOUND_MIXER_DIGITAL1,	"Digital",		0, PVOL },
  { SOUND_MIXER_DIGITAL1,	"IEC958",		0, PVOL }, /* fallback */
  { SOUND_MIXER_DIGITAL1,	"IEC958 Optical",	0, PVOL }, /* fallback */
  { SOUND_MIXER_DIGITAL1,	"IEC958 Coaxial",	0, PVOL }, /* fallback */
  { SOUND_MIXER_DIGITAL2,	"Digital",		1, PVOL },
  { SOUND_MIXER_DIGITAL3,	"Digital",		2, PVOL },
  { SOUND_MIXER_PHONEIN,	"Phone",		0, PVOL },
  { SOUND_MIXER_PHONEOUT,	"Master Mono",		0, PVOL },
  { SOUND_MIXER_PHONEOUT,	"Speaker",		0, PVOL }, /*fallback*/
  { SOUND_MIXER_PHONEOUT,	"Mono",			0, PVOL }, /*fallback*/
  { SOUND_MIXER_PHONEOUT,	"Phone",		0, PVOL }, /* fallback */
  { SOUND_MIXER_VIDEO,	"Video",		0, CVOL },
  { SOUND_MIXER_RADIO,	"Radio",		0, CVOL },
  { SOUND_MIXER_DEPTH, "Depth", 0, PVOL },
  { SOUND_MIXER_DEPTH, "3D Control - Depth", 0, PVOL },
  { SOUND_MIXER_REARVOL, "Rear", 0, PVOL},
  { SOUND_MIXER_CENTERVOL, "Center", 0, PVOL },
  { SOUND_MIXER_CENTERVOL, "LFE", 0, PVOL },
  { SOUND_MIXER_SIDEVOL, "Side", 0, PVOL },
  { SOUND_MIXER_SIDEVOL, "Surround", 0, PVOL },
  { SOUND_MIXER_NONE, NULL, -1 },
};



static const struct snd_mixer_oss_assign_table ossv3_table[] = {
  { SOUND_MIXER_VOLUME, 	"Master",		0, PVOL },
  { SOUND_MIXER_VOLUME, 	"Front",		0, PVOL }, /* fallback */
  { SOUND_MIXER_BASS,	"Tone Control - Bass",	0, PVOL },
  { SOUND_MIXER_TREBLE,	"Tone Control - Treble", 0, PVOL },
  { SOUND_MIXER_SYNTH,	"Synth",		0, PVOL },
  { SOUND_MIXER_SYNTH,	"FM",			0, PVOL }, /* fallback */
  { SOUND_MIXER_SYNTH,	"Music",		0, PVOL }, /* fallback */
  { SOUND_MIXER_PCM,	"PCM",			0, PVOL },
  { SOUND_MIXER_SPEAKER,	"Beep", 		0, PVOL },
  { SOUND_MIXER_SPEAKER,	"PC Speaker", 		0, PVOL }, /* fallback */
  { SOUND_MIXER_SPEAKER,	"Speaker", 		0, PVOL }, /* fallback */
  { SOUND_MIXER_LINE,	"Line", 		0, CVOL },
  { SOUND_MIXER_MIC,	"Mic", 			0, CVOL },
  { SOUND_MIXER_CD,	"CD", 			0, CVOL },
  { SOUND_MIXER_IMIX,	"Monitor Mix", 		0, UNUSED_VOL },
  { SOUND_MIXER_ALTPCM,	"PCM",			1, PVOL },
  { SOUND_MIXER_ALTPCM,	"Headphone",		0, PVOL }, /* fallback */
  { SOUND_MIXER_ALTPCM,	"Wave",			0, PVOL }, /* fallback */
  { SOUND_MIXER_RECLEV,	"-- nothing --",	0, UNUSED_VOL },
  { SOUND_MIXER_IGAIN,	"Capture",		0, PVOL },
  { SOUND_MIXER_OGAIN,	"Playback",		0, PVOL },
  { SOUND_MIXER_LINE1,	"Aux",			0,  CVOL },
  { SOUND_MIXER_LINE2,	"Aux",			1,  CVOL },
  { SOUND_MIXER_LINE3,	"Aux",			2, CVOL },
  { SOUND_MIXER_DIGITAL1,	"Digital",		0, PVOL },
  { SOUND_MIXER_DIGITAL1,	"IEC958",		0, PVOL }, /* fallback */
  { SOUND_MIXER_DIGITAL1,	"IEC958 Optical",	0, PVOL }, /* fallback */
  { SOUND_MIXER_DIGITAL1,	"IEC958 Coaxial",	0, PVOL }, /* fallback */
  { SOUND_MIXER_DIGITAL2,	"Digital",		1, PVOL },
  { SOUND_MIXER_DIGITAL3,	"Digital",		2, PVOL },
  { SOUND_MIXER_PHONEIN,	"Phone",		0, PVOL },
  { SOUND_MIXER_PHONEOUT,	"Master Mono",		0, PVOL },
  { SOUND_MIXER_PHONEOUT,	"Speaker",		0, PVOL }, /*fallback*/
  { SOUND_MIXER_PHONEOUT,	"Mono",			0, PVOL }, /*fallback*/
  { SOUND_MIXER_PHONEOUT,	"Phone",		0, PVOL }, /* fallback */
  { SOUND_MIXER_VIDEO,	"Video",		0, CVOL },
  { SOUND_MIXER_RADIO,	"Radio",		0, CVOL },
  { SOUND_MIXER_MONITOR,	"Monitor",		0, PVOL },
  { SOUND_MIXER_REARVOL, "Rear", 0, PVOL },
  { SOUND_MIXER_CENTERVOL, "Center", 0, PVOL },
  { SOUND_MIXER_CENTERVOL, "LFE", 0, PVOL },
  { SOUND_MIXER_SIDEVOL, "Side", 0, PVOL },
  { SOUND_MIXER_SIDEVOL, "Surround", 0, PVOL },
  { SOUND_MIXER_NONE, NULL, -1, UNUSED_VOL },

};

//Not sure how correct this is but I think it should work.
static const struct snd_mixer_oss_assign_table extra_table[] = {
  { SOUND_MIXER_MIC,	"Front Mic", 			0, CVOL },
  { SOUND_MIXER_MIC,	"Rear Mic", 			0, CVOL },
  { SOUND_MIXER_MIC,	"Front Mic Boost", 			0, CVOL },
  { SOUND_MIXER_MIC,	"Rear Mic Boost", 			0, CVOL },
 { SOUND_MIXER_LINE,	"Line Boost", 		0, CVOL },
  { SOUND_MIXER_NONE, NULL, -1, UNUSED_VOL },
};

const struct snd_mixer_oss_assign_table *oss_get_mixer_assignment(const struct dspd_mix_info *info)
{
  const struct snd_mixer_oss_assign_table *e, *ret = NULL;
  size_t i = 0;
  
  while ( server_context.ctl_assignments[i].name )
    {
      e = &server_context.ctl_assignments[i];
      if ( info->ctl_index == e->index && strcmp(e->name, info->name) == 0 &&
	   (info->flags & e->flags) )
	{
	  ret = e;
	  break;
	}
      i++;
    }
  if ( ret == NULL )
    {
      i = 0;
      while ( extra_table[i].name )
	{
	  e = &extra_table[i];
	  if ( info->ctl_index == e->index && strcmp(e->name, info->name) == 0 &&
	       (e->flags & info->flags) )
	    {
	      ret = e;
	      break;
	    }
	  i++;
	}
    }
  return ret;
}

