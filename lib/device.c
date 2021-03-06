/*
 *  DEVICE - generic PCM device support
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
#define _FILE_OFFSET_BITS 64
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <mqueue.h>
#define _DSPD_CTL_MACROS
#include "sslib.h"
#include "daemon.h"
/*
  Lock optimization saves up to 30% CPU.  The idea is that any io cycle that is split into
  multiple chunks can avoid locking and unlocking a client multiple times during the io cycle.
  The rare syscalls made for a futex based lock should be deferred until the buffers are completely
  serviced and the atomic ops of an uncontended futex consume a lot of CPU and bus cycles compared
  to other instructions.

  If the device thread knows it already has a lock, it can hold onto it throughout multiple
  iterations until it is time to sleep or start a new io cycle.
*/
#define ENABLE_LOCK_OPTIMIZATION
struct dspd_pcm_device {
  struct dspd_pcmdev_stream        playback;
  struct dspd_pcmdev_stream        capture;
  struct dspd_scheduler  *sched;
  dspd_mutex_t            reg_lock;
  struct dspd_dev_reg     reg;
  uint32_t                current_config;
  void (*process_data)(void *arg, struct dspd_pcm_device *device);

  //This basically indicates that an IRQ occurred.  It is up to the client
  //to decide what to do with it.
  void (*process_client)(struct dspd_pcm_device *device,
			 uint32_t client, //client number
			 uint32_t ioc,    //io cycle type (lock,release,both)
			 uint32_t key);    //Identifier for this device


  void  *arg;
  uintptr_t current_latency;
  bool trigger;
  dspd_thread_t iothread;
  struct dspd_slist *list;
 


  //8 bits per client: 3-7=latency, 2=present, 1=capture, 0=playback
  uint8_t client_configs[DSPD_MAX_OBJECTS];

  volatile intptr_t current_client;
  int32_t  key;

  volatile int current_exception, exc_client;
  volatile AO_t error;

  volatile AO_t irq_count, ack_count;
  volatile bool reset_scheduler, idle;
  struct sched_param sched_param;
  //  int sched_policy;
  int32_t mq[2];
  
#ifndef DSPD_HAVE_ATOMIC_INT64
  dspd_mutex_t cookie_lock;
  uint64_t cookie;
#else
  volatile uint64_t cookie;
#endif


  uint32_t access_flags;
  int32_t  excl_client;
  unsigned int seed;
  uint32_t wakeup_count;
  uint64_t hotplug_event_id;


  
  bool    must_unlock;
  uint8_t lock_mask[DSPD_MASK_SIZE];
  size_t  lock_count;

  uintptr_t  pxferlen_hint, cxferlen_hint;

  bool must_spin;
};

#define DSPD_DEV_USE_TLS
static int _stream_recover_fcn(struct dspd_pcmdev_stream *stream);
static int stream_prepare(struct dspd_pcmdev_stream *stream);
static int32_t dspd_pcmdev_connect(void *dev, int32_t client);
static int32_t dspd_pcmdev_disconnect(void *dev, int32_t client);
static int32_t dspd_pcmdev_set_latency(void *dev, uint32_t client, uint32_t latency);
static int32_t dspd_pcmdev_get_latency(void *dev, uint32_t client);
static int32_t dspd_pcmdev_trigger(void *dev, uint32_t client, uint32_t streams);
static int32_t dspd_pcmdev_getparams(void *dev, int32_t stream, struct dspd_drv_params *params);
static void alert_all_clients(struct dspd_pcm_device *dev, int32_t error);
static void alert_one_client(struct dspd_pcm_device *dev, int32_t client, int32_t error);
static void schedule_fullduplex_wake(void *data);
static void schedule_capture_wake(void *data);
static void schedule_playback_wake(void *data);
static bool schedule_capture_sleep(void *data, uint64_t *abstime, uint64_t *deadline, int32_t *reltime);
static bool schedule_playback_sleep(void *data, uint64_t *abstime, uint64_t *deadline, int32_t *reltime);
static void schedule_timer_event(void *data);
static void schedule_trigger_event(void *data);
static int32_t dspd_dev_client_settrigger(struct dspd_pcm_device *dev, uint32_t client, uint32_t bits, bool now);
static uint32_t dspd_dev_config_get_latency(struct dspd_pcm_device *dev, uint32_t *config);
static void dspd_dev_set_stream_volume(struct dspd_pcm_device *dev,
				       int32_t stream,
				       float volume);
static float dspd_dev_get_stream_volume(struct dspd_pcm_device *dev,
					int32_t stream);
static void incr_intr_count(struct dspd_pcm_device *dev);
static void dev_thread_destructor(struct dspd_scheduler *sch, void *arg);
static void dev_started(void *user_data);
static void unlock_all_clients(struct dspd_pcm_device *dev);

int32_t dspd_dev_get_slot(void *dev)
{
  struct dspd_pcm_device *d = dev;
  return d->key;
}


static bool check_io(struct dspd_pcm_device *dev, struct dspd_pcmdev_stream *stream)
{
  bool busy = false;
  int32_t oldmask = stream->io_pending, newmask;
  if ( oldmask )
    {
      //Find out what the driver has pending.
      newmask = stream->ops->io_pending(stream->handle, oldmask);

      //Handle completion
      if ( oldmask & DSPD_PCMDRV_PREPARE_PENDING )
	{
	  if ( newmask & DSPD_PCMDRV_PREPARE_COMPLETE )
	    {
	      stream->running = true;
	      newmask &= ~DSPD_PCMDRV_PREPARE_COMPLETE;
	    }
	}
      if ( oldmask & DSPD_PCMDRV_RECOVER_PENDING )
	{
	  if ( newmask & DSPD_PCMDRV_RECOVER_COMPLETE )
	    {
	      stream->running = true;
	      newmask &= ~DSPD_PCMDRV_RECOVER_COMPLETE;
	    }
	}
      if ( newmask & DSPD_PCMDRV_IO_ERROR )
	dspd_sched_abort(dev->sched);
      stream->io_pending = newmask;
      //The stream is busy if the device has io pending.
      busy = !!(stream->io_pending & (~DSPD_PCMDRV_IO_ERROR));
    }
  return busy;
}

static void set_idle(struct dspd_pcm_device *dev)
{
  dev->idle = true;
  if ( dev->reset_scheduler )
    {
      dev->reset_scheduler = false;
      //pthread_setschedparam(pthread_self(), dev->sched_policy, &dev->sched_param);
    }
  dspd_sched_set_latency(dev->sched, UINT64_MAX);
}

static int _stream_recover_print(struct dspd_pcmdev_stream *s, const char *fcn, int line)
{
  // fprintf(stderr, "RECOVERY ON %s:%d\n", fcn, line);
  if ( dspd_dctx.debug )
    fprintf(stderr, "RECOVERY ON %s:%d\n", fcn, line);
  return _stream_recover_fcn(s);
}

#define stream_recover(_s) _stream_recover_print(_s, __FUNCTION__, __LINE__)


static void dspd_dev_set_config(struct dspd_pcm_device *dev, uint32_t config)
{
  AO_store(&dev->reg.config, config);
}

static void dspd_dev_lock(struct dspd_pcm_device *dev)
{
  dspd_mutex_lock(&dev->reg_lock);
}

static void dspd_dev_unlock(struct dspd_pcm_device *dev)
{
  dspd_mutex_unlock(&dev->reg_lock);
}


static void dspd_dev_config_set_stream_count(struct dspd_pcm_device *dev, 
					     uint32_t *config,
					     int32_t  stream,
					     int32_t  count)

{
  int32_t bits;
  count &= 0x1FF;
  switch(stream)
    {
    case DSPD_PCM_STREAM_PLAYBACK:
      bits = 5;
      break;
    case DSPD_PCM_STREAM_CAPTURE:
      bits = 14;
      break;
    default:
      return;
    }
  count <<= bits;
  (*config) &= ~(0x1FF << bits);
  (*config) |= count;
}


static uint32_t dspd_dev_config_get_stream_count(struct dspd_pcm_device *dev, 
						 uint32_t *config,
						 int32_t stream)
{
  int32_t bits;
  uint32_t count = *config;
  switch(stream)
    {
    case DSPD_PCM_STREAM_PLAYBACK:
      bits = 5;
      break;
    case DSPD_PCM_STREAM_CAPTURE:
      bits = 14;
      break;
    default:
      return 0;
    }
  count >>= bits;
  count &= 0x1FF;
  return count;
}

static void dspd_dev_set_stream_volume(struct dspd_pcm_device *dev,
				       int32_t stream,
				       float volume)
{
  switch(stream)
    {
    case DSPD_PCM_STREAM_PLAYBACK:
      dspd_store_float32(&dev->reg.playback_volume, volume);
      break;
    case DSPD_PCM_STREAM_CAPTURE:
      dspd_store_float32(&dev->reg.capture_volume, volume);
      break;
    }
}

static float dspd_dev_get_stream_volume(struct dspd_pcm_device *dev,
					int32_t stream)
{
  float ret;
  switch(stream)
    {
    case DSPD_PCM_STREAM_PLAYBACK:
      ret = dspd_load_float32(&dev->reg.playback_volume);
      break;
    case DSPD_PCM_STREAM_CAPTURE:
      ret = dspd_load_float32(&dev->reg.capture_volume);
      break;
    default:
      ret = INFINITY;
    }
  return ret;
}

static void dspd_dev_config_set_latency(struct dspd_pcm_device *dev, uint32_t *config, uint32_t latency)
{
  uint32_t n, i;

  //Adjust it to be in the supported range
  if ( dev->playback.ops )
    {
      if ( latency > dev->playback.params.max_latency )
	latency = dev->playback.params.max_latency;
      else if ( latency < dev->playback.params.min_latency )
	latency = dev->playback.params.min_latency;
    }
  if ( dev->capture.ops )
    {
      if ( latency > dev->capture.params.max_latency )
	latency = dev->capture.params.max_latency;
      else if ( latency < dev->capture.params.min_latency )
	latency = dev->capture.params.min_latency;
    }

  /*
    Select a power of 2.  This is required since it is the only way to
    allow differing latency requirements without making the timing incorrect
    and affecting compatibility with applications.
    For example, if a client specifies 256 frames and another specifies 128 frames
    then 128 frames would be chosen.  The client would see that 256 frames are consumed
    when expected and generally not notice the server waking up twice as often.
    
  */
  for ( i = 2; i < 32; i++ )
    {
      n = 1 << i;
      if ( n >= latency )
	break;
    }


  //Use only first 5 bits.
  (*config) &= ~0x1F; 
  (*config) |= i;
}
			      
			      
static uint32_t dspd_dev_config_get_latency(struct dspd_pcm_device *dev, uint32_t *config)
{
  return 1 << ((*config) & 0x1F);
}

static uint32_t dspd_dev_get_real_latency(struct dspd_pcm_device *dev, uint32_t latency)
{
  uint32_t i, val;
  for ( i = 1; i < 31; i++ )
    {
      val = 1 << i;
      if ( val > latency )
	{
	  i--;
	  break;
	} else if ( val == latency )
	{
	  break;
	}
    }
  if ( dev->playback.ops )
    {
      if ( val > dev->playback.params.max_latency )
	val = dev->playback.params.max_latency;
      else if ( val < dev->playback.params.min_latency )
	val = dev->playback.params.min_latency;
    }
  if ( dev->capture.ops )
    {
      if ( val > dev->capture.params.max_latency )
	val = dev->capture.params.max_latency;
      else if ( val < dev->capture.params.min_latency )
	val = dev->capture.params.min_latency;
    }
  latency = val;
  for ( i = 1; i < 31; i++ )
    {
      val = 1 << i;
      if ( val > latency )
	{
	  i--;
	  break;
	} else if ( val == latency )
	{
	  break;
	}
    }
  return i;
}


static int32_t dspd_dev_set_client_latency(struct dspd_pcm_device *dev, 
					   uint32_t client,
					   uint32_t latency)
{
  uint8_t cbits;
  int32_t ret;
  uint32_t l;
  uint32_t cfg, cfgl;
  if ( client < DSPD_MAX_OBJECTS )
    {
      if ( dev->access_flags & DSPD_DEV_LOCK_LATENCY )
	{
	  cfg = AO_load(&dev->reg.config);
	  cfgl = dspd_dev_config_get_latency(dev, &cfg);
	  if ( latency < cfgl && client != dev->excl_client )
	    return -ETIME;
	}
      cbits = dev->client_configs[client];
      if ( cbits & DSPD_CBIT_PRESENT )
	{
	  l = dspd_dev_get_real_latency(dev, latency);
	  cbits &= ~DSPD_CBIT_LATENCY;
	  cbits |= l << 3U;
	  ret = 1 << l;
	  dev->client_configs[client] = cbits;
	} else
	{
	  ret = -ENOENT;
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}


static int32_t dspd_dev_get_client_latency(struct dspd_pcm_device *dev,
					   uint32_t client)
{
  uint8_t cbits;
  int32_t ret;
  if ( client < DSPD_MAX_OBJECTS )
    {
      cbits = dev->client_configs[client];
      if ( cbits & DSPD_CBIT_PRESENT )
	{
	  ret = 1 << ((cbits & DSPD_CBIT_LATENCY) >> 3U); 
	} else
	{
	  ret = -ENOENT;
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}

//Attach client (reserve slot)
static int32_t dspd_dev_attach_client(struct dspd_pcm_device *dev,
				      uint32_t client)
{
  uint32_t cbits;
  int32_t ret;
  if ( client < DSPD_MAX_OBJECTS )
    {

      if ( ! (dev->access_flags & DSPD_DEV_LOCK_EXCL) )
	{
	  cbits = dev->client_configs[client];
	  if ( cbits & DSPD_CBIT_PRESENT )
	    {
	      ret = -EEXIST;
	    } else
	    {
	      dev->client_configs[client] = DSPD_CBIT_PRESENT;
	      ret = 0;
	    }
	} else
	{
	  ret = -EBUSY;
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}

//Free the slot
static int32_t dspd_dev_detach_client(struct dspd_pcm_device *dev,
				      uint32_t client)
{
  uint32_t cbits;
  int32_t ret;
  if ( client < DSPD_MAX_OBJECTS )
    {
      cbits = dev->client_configs[client];
      if ( cbits & DSPD_CBIT_PRESENT )
	{
	  if ( client == dev->excl_client )
	    {
	      dev->excl_client = -1;
	      dev->access_flags = 0;
	    }
	  dspd_dev_client_settrigger(dev, client, 0, 0);
	  dev->client_configs[client] = 0;
	  ret = 0;
	} else
	{
	  ret = -ENOENT;
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}

static bool dspd_dev_get_client_attach(struct dspd_pcm_device *dev, uint32_t client)
{
  bool ret;
  if ( client < DSPD_MAX_OBJECTS )
    {
      ret = !! (dev->client_configs[client] & DSPD_CBIT_PRESENT);
    } else
    {
      ret = false;
    }
  return ret;
}

static void set_trigger_mask(uint8_t *mask, uintptr_t index, uint8_t bits)
{
  uintptr_t i = index / 8, o;
  uint8_t val = mask[i];
  bits &= (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);
  //bits >>= 6; //Make trigger bits the lowest position
  o = index % 8;
  bits <<= o; //Shift bits to correct position
  val &= ~(3<<o); //Clear original bits
  val |= bits;
  mask[i] = val;
}

static uint8_t get_trigger_mask(const uint8_t *mask, uintptr_t index)
{
  uintptr_t i = index / 8, o = index % 8;
  uint8_t val;
  val = mask[i];
  val >>= o;
  val &= 3;
  //return val << 6;
  return val;
}


static int32_t dspd_dev_client_settrigger(struct dspd_pcm_device *dev, uint32_t client, uint32_t bits, bool now)
{
  int32_t ret, cbits;
  uintptr_t b;
  if ( client < DSPD_MAX_OBJECTS )
    {
      cbits = dev->client_configs[client];
      if ( cbits & DSPD_CBIT_PRESENT )
	{
	  bits &= (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);
	  ret = 0;
	  cbits &= ~(DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);
	  cbits |= bits;
	  dev->client_configs[client] = cbits;
	  if ( now )
	    {
	      b = client * 2;
	      set_trigger_mask((uint8_t*)dev->reg.client_mask, b, cbits);
	      dspd_sched_trigger(dev->sched);
	    }
	} else
	{
	  ret = -ENOENT;
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}



static int32_t dspd_dev_params_get_max_latency(struct dspd_pcm_device *dev)
{
  uint32_t pl, cl, ret;
  if ( dev->playback.ops )
    pl = dev->playback.params.max_latency;
  else
    pl = dev->capture.params.max_latency;
  if ( dev->capture.ops )
    cl = dev->capture.params.max_latency;
  else
    cl = pl;
  if ( cl < pl )
    ret = cl;
  else
    ret = pl;
  return ret;
}

static int32_t dspd_dev_params_get_min_latency(struct dspd_pcm_device *dev)
{
  uint32_t pl, cl, ret;
  if ( dev->playback.ops )
    pl = dev->playback.params.min_latency;
  else
    pl = dev->capture.params.min_latency;
  if ( dev->capture.ops )
    cl = dev->capture.params.min_latency;
  else
    cl = pl;
  if ( cl > pl )
    ret = cl;
  else
    ret = pl;
  return ret;
}




static int32_t dspd_dev_client_configure(struct dspd_pcm_device *dev, uint32_t client)
{
  //Setup the configuration registers.  That means find the lowest latency,
  //set the bitmask (register) for this client, and find the highest client index.
  uint32_t config = 0, b;
  int32_t i, cbits, maxp = -1, maxc = -1, min_latency, l;
  min_latency = dspd_dev_params_get_max_latency(dev);

  for ( i = 0; i < DSPD_MAX_OBJECTS; i++ )
    {
      cbits = dev->client_configs[i];
      if ( cbits & DSPD_CBIT_PRESENT )
	{
	  l = 30;
	  if ( cbits & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      maxp = i;
	      l = (cbits & DSPD_CBIT_LATENCY) >> 3U;
	    }
	  if ( cbits & DSPD_PCM_SBIT_CAPTURE )
	    {
	      maxc = i;
	      l = (cbits & DSPD_CBIT_LATENCY) >> 3U;
	    }
	  l = 1 << l;
	  if ( l < min_latency )
	    min_latency = l;
	}
    }

  l = dspd_dev_params_get_min_latency(dev);
  if ( min_latency < l )
    min_latency = l;

  cbits = dev->client_configs[client];


  b = client * 2;
  if ( cbits & DSPD_CBIT_PRESENT )
    set_trigger_mask((uint8_t*)dev->reg.client_mask, b, cbits);
  else
    set_trigger_mask((uint8_t*)dev->reg.client_mask, b, 0);
  
  dspd_dev_config_set_stream_count(dev,
				   &config,
				   DSPD_PCM_STREAM_PLAYBACK,
				   maxp+1);
  //fprintf(stderr, "SETMAX %u\n", maxp);
  dspd_dev_config_set_stream_count(dev,
				   &config,
				   DSPD_PCM_STREAM_CAPTURE,
				   maxc+1);
  //fprintf(stderr, "LATENCY %d\n", min_latency);
  dspd_dev_config_set_latency(dev,
			      &config,
			      min_latency);
  dspd_dev_set_config(dev, config);
  return 0;
}


/*
  Synchronize the fake device register.  It is a 32 bit integer and 2 floating point values.
  The latency and number of streams attached can be set atomically.  The volume does not
  need to be atomic.  It is important that this not block or use too much CPU so it is limited
  to reading 3 32 bit values (2 volume and 1 latency).
*/
void dspd_sync_reg(struct dspd_pcm_device *dev)
{
  uint32_t config = AO_load(&dev->reg.config);
  uint32_t latency, count;
  float volume;
  size_t l;
  int32_t ret;
  if ( config != dev->current_config )
    {
      latency = dspd_dev_config_get_latency(dev, &config);
      dev->current_latency = latency; //Capture may not take this immediately
      if ( dev->playback.ops != NULL )
	{
	  if ( dev->playback.requested_latency != latency )
	    {
	     
	      if ( latency < dev->playback.latency )
		dev->playback.latency_changed = true;
	      dev->playback.requested_latency = latency;
	      //See if glitch mode is on and if so then use a bigger buffer if the requested size is
	      //too small. 
	      if ( dev->playback.glitch && latency < dev->playback.glitch_threshold )
		l = dev->playback.glitch_threshold;
	      else
		l = latency;
	      dev->playback.latency = dev->playback.ops->set_latency(dev->playback.handle, l, latency);
	      dspd_sched_set_latency(dev->sched, dev->playback.latency * dev->playback.sample_time); 
	    }
	  count = dspd_dev_config_get_stream_count(dev, &config, DSPD_PCM_STREAM_PLAYBACK);
	  if ( count > 0 && dev->playback.streams == 0 )
	    {
	      if ( dev->playback.stop_threshold == 0 )
		{
		  if ( stream_prepare(&dev->playback) != 0 )
		    {
		      ret = stream_recover(&dev->playback);
		      
		      if ( ret < 0 )
			dspd_sched_abort(dev->sched);
		      else if ( ret != DSPD_PCMDRV_IO_PENDING )
			dev->playback.running = true;
		    } else
		    {
		      dev->playback.running = true;
		    }
		}
	    } else if ( count == 0 && dev->playback.streams > 0 )
	    {
	      //Prepare to stop when samples are played
	      dev->playback.stop_threshold = dev->playback.params.bufsize * 2;
	    }
	  if ( count > 0 )
	    dev->playback.stop_threshold = 0;
	  dev->playback.streams = count;
	}
      if ( dev->capture.ops != NULL )
	{
	  if ( dev->capture.latency != latency )
	   {
	     if ( dev->capture.status != NULL )
	       {
		 if ( dev->capture.status->fill <= dev->capture.latency )
		   {
		     dev->capture.latency = latency;
		     dev->capture.ops->set_latency(dev->capture.handle, latency, latency);
		     dspd_sched_set_latency(dev->sched, dev->capture.latency * dev->capture.sample_time);
		   }
	       } else
	       {
		 dev->capture.latency = latency;
		 dev->capture.latency = dev->capture.ops->set_latency(dev->capture.handle, latency, latency);
		 dspd_sched_set_latency(dev->sched, dev->capture.latency * dev->capture.sample_time);
	       }
	   } 
	  count = dspd_dev_config_get_stream_count(dev, &config, DSPD_PCM_STREAM_CAPTURE);
	  if ( count > 0 && dev->capture.streams == 0 )
	    {
	      if ( dev->capture.stop_threshold == 0 )
		{
		  if ( stream_prepare(&dev->capture) != 0 )
		    {
		      ret = stream_recover(&dev->capture);
		      if ( ret < 0 )
			dspd_sched_abort(dev->sched);
		      else if ( ret != DSPD_PCMDRV_IO_PENDING )
			dev->capture.running = true;
		    } else
		    {
		      dev->capture.running = true;
		    }
		}
	    } else if ( count == 0 && dev->capture.streams > 0 )
	    {
	      dev->capture.stop_threshold = dev->capture.params.bufsize;
	    }
	  if ( count > 0 )
	    dev->capture.stop_threshold = 0;
	  dev->capture.streams = count;
	}
      dev->current_config = config;
    }
  if ( dev->playback.ops )
    {
      volume = dspd_load_float32(&dev->reg.playback_volume);
      if ( volume != dev->playback.volume )
	{
	  dev->playback.volume = volume;
	  dev->playback.ops->set_volume(dev->playback.handle, volume);
	}
    }
  if ( dev->capture.ops )
    {
      volume = dspd_load_float32(&dev->reg.capture_volume);
      if ( volume != dev->capture.volume )
	{
	  dev->capture.volume = volume;
	  dev->capture.ops->set_volume(dev->capture.handle, volume);
	}
    }
}

static void dspd_dev_notify(void *dev)
{
  struct dspd_pcm_device *device = dev;
  dspd_sched_trigger(device->sched);
}

static void schedule_trigger_event(void *data)
{
  /*
    The only sane thing to do when we get an external trigger is to
    check the device just like when a timer fires.  That way it is possible
    to do an io cycle accurately.  It is important to avoid a double trigger
    since device position registers can be slow to access.
   */
  struct dspd_pcm_device *dev = data;
  if ( dev->trigger )
    return;
  schedule_timer_event(dev);
  dev->trigger = true;
}

static void schedule_timer_event(void *data)
{
  struct dspd_pcm_device *dev = data;
  int32_t ret;

  if ( dev->trigger )
    return;
  if ( dev->playback.started )
    {
   
      ret = dev->playback.ops->status(dev->playback.handle, &dev->playback.status, false);
      if ( ret < 0 )
	{
	  dev->playback.started = 0;

	  dev->playback.status = NULL;
	  ret = stream_recover(&dev->playback);
	  if ( ret < 0 )
	    dspd_sched_abort(dev->sched);
	} else
	{
	  if ( dev->playback.status->tstamp )
	    {
	      dspd_intrp_update(&dev->playback.intrp, 
				dev->playback.status->tstamp,
				dev->playback.status->hw_ptr - dev->playback.last_hw);
	      dev->playback.last_hw = dev->playback.status->hw_ptr;
	    }
	}
    }
  if ( dev->capture.started )
    {
      ret = dev->capture.ops->status(dev->capture.handle, &dev->capture.status, false);
      if ( ret < 0 )
	{

	  dev->capture.status = NULL;
	  dev->capture.started = 0;
	  ret = stream_recover(&dev->capture);
	  if ( ret < 0 )
	    dspd_sched_abort(dev->sched);
	} 
    }
  dev->trigger = true;
}

/*
  The idea is that for low latencies we need to specify an earlier wakeup time
  because the OS might not be very accurate.  If the latency is really high
  then it should be possible to sleep for a very long time and still fill
  the buffer.  At some point, sleeping for too long causes long running
  computations to catch up.  That is fine for a music player but kind of
  shitty on multitasking systems where something else might need the CPU.
  It really seems like waking up a few times per second should not be an
  issue and might be an overall win in most cases.
  
  I think this needs adjusted for RT vs non RT.

  Non rt needs to sleep for up to 500ms and RT needs to sleep for up to 333ms.  RT can't
  be doing long computations.  Non RT can, but it will need to write more often to avoid
  problems with preemption.
  Non rt needs to sleep for up to 1/4 to 1/2 of the buffer and RT needs 1/3 to 2/3
  of the buffer.  Non RT can't sleep for as long because it can get preempted for a longer
  time.  RT has more control over when it gets CPU time.
  Non rt curve should be 10-50ms and RT curve should be 1-100ms.  Non RT may
  have a lot of trouble below 10ms or so.  RT should do much better.

*/
//Sleep for 1/3 to 2/3 of the buffer time depending on the fill level.
#define FILLTIME_MAX (100000*1000)
#define FILLTIME_MIN (1000*1000)
#define FILLTIME_RANGE (FILLTIME_MAX-FILLTIME_MIN)
#define FILLTIME_FRACT (FILLTIME_RANGE/100)
//Sleeping for too long and running long computations to catch up
//is bad for multitasking.  A thread somewhere might lag causing
//multimedia glitches or other annoying issues.  Maybe that thread
//is what is feeding this one audio data?
#define SLEEPTIME_MAX (500000*1000)
static uint64_t get_sleep_reltime(uint64_t filltime, bool reset)
{

  uint64_t ft = (filltime / 3);
  uint64_t pct;
  uint64_t ret;
 
  if ( filltime > FILLTIME_MAX )
    {
      ret = ft * 2;
    } else if ( filltime < FILLTIME_MIN )
    {
      ret = ft;
    } else
    {
      pct = filltime / FILLTIME_FRACT;
      ret = ft + ((ft / 100) * pct);
    }
  if ( ret > SLEEPTIME_MAX )
    ret = SLEEPTIME_MAX;
  if ( reset != 0 && ret > (filltime / 2) )
    ret = filltime / 2;

  return ret;
}



static bool schedule_playback_sleep(void *data, uint64_t *abstime, uint64_t *deadline, int32_t *reltime)
{
  struct dspd_pcm_device *dev = data;
  uint64_t f;
  uint64_t sleep_frames;
  uint64_t rt;
  *deadline = UINT64_MAX;
  dev->idle = false;

  if ( dev->playback.io_pending )
    {
      if ( check_io(dev, &dev->playback) )
	{
	  *abstime = UINT64_MAX;
	  *reltime = DSPD_SCHED_STOP;
	  *deadline = UINT64_MAX;
	  return true;
	}
    }
  
  if ( dev->wakeup_count )
    {
      dev->wakeup_count--;
      if ( dev->wakeup_count == 0 )
	dspd_sched_set_fd_event(dev->sched, dev->mq[0], POLLIN);
    }
  
  if ( ! dev->capture.ops )
    dspd_sync_reg(dev);
  if ( dev->playback.latency_changed )
    {
      dev->playback.latency_changed = 0;
      dev->playback.check_status = 1;
      *reltime = DSPD_SCHED_SPIN;
      *abstime = UINT64_MAX;
      if ( dev->playback.requested_latency > 0 )
	*deadline = dev->playback.requested_latency * dev->playback.sample_time;
      else
	*deadline = dev->playback.latency * dev->playback.sample_time;
    } else if ( dev->must_spin )
    {
      *reltime = DSPD_SCHED_SPIN;
      *abstime = UINT64_MAX;
      *deadline = dev->playback.latency * dev->playback.sample_time;
      dev->must_spin = false;
    } else if ( dev->playback.status )
    {
      /*
	Keep checking the status for a while.  ALSA device startup isn't always perfectly accurate.
	This will use some extra CPU for a short time until the device is really synchronized with the
	system clock.
      */
      if ( dev->playback.status->appl_ptr < dev->playback.latency || dev->playback.check_status != 0 )
	dev->playback.ops->status(dev->playback.handle, &dev->playback.status, false);
    

      if ( dev->playback.glitch && dev->playback.requested_latency < dev->playback.glitch_threshold )
	{
	  f = dev->playback.requested_latency;
	} else
	{
	  if ( dev->playback.status->fill > dev->playback.latency )
	    f = dev->playback.latency;
	  else
	    f = dev->playback.status->fill;
	}
      /*
	See if any clients triggered a wakeup that will probably leave the buffer overfilled.  Those
	clients have just started so rewinding was used to rewrite some of the buffer.  That means the
	application pointer isn't yet synchronized with all of the other clients.

	The early wakeup is set when a client needs it and reset when clients are going to be processed again.
	When clients are processed they may turn the early wakeup back on again.  This will happen as many times
	as necessary until clients are sychronized.  In glitch correction mode it may do this indefinitely when
	low latency clients are connected.
	

      */
      if ( f > dev->playback.early_cycle )
	{
	  if ( dspd_dctx.debug )
	    fprintf(stderr, "Early cycle: %llu > %llu\n", (long long)f, (long long)dev->playback.early_cycle);
	  f = dev->playback.early_cycle;
	}


      sleep_frames = dspd_intrp_frames(&dev->playback.intrp, f);
      

      /*
	In practice, this is pretty close to doing a sched_yield().  It will
	try to yield with no known race condition so it should wake this thread up a lot to see if anything funny
	happens.  The CPU will be really busy for a while but it should be shared fairly among processes.
      */
      if ( dev->playback.status->appl_ptr < dev->playback.params.min_latency )
	{
	  f = dev->playback.params.min_latency / 4;
	  if ( f < dev->playback.params.min_dma )
	    f = dev->playback.params.min_dma;
	  if ( sleep_frames < f )
	    f = sleep_frames;
	} else
	{
	  f = sleep_frames;
	}

      f *= (1000000000 / dev->playback.params.rate);

      rt = get_sleep_reltime(f, dev->playback.check_status);

      *abstime = dev->playback.status->tstamp + rt;
      *deadline = dev->playback.status->fill;
      dev->playback.next_wakeup = *abstime;
      *reltime = DSPD_SCHED_WAIT;
      dev->playback.check_status = 0;
    } else if ( dev->playback.running )
    {
      *reltime = DSPD_SCHED_SPIN;
      *abstime = UINT64_MAX;
      *deadline = dev->playback.latency * dev->playback.sample_time;
    } else
    {
      *reltime = DSPD_SCHED_STOP;
      *abstime = UINT64_MAX;
      *deadline = UINT64_MAX;
      set_idle(dev);
    }
  return true;
}

static bool process_rewound_playback(struct dspd_pcm_device *dev,
				     void *client,
				     const struct dspd_client_ops *ops,
				     uint64_t *pointer,
				     uint32_t  frames,
				     uint32_t  rw)
{
  uintptr_t offset = 0, l, o;
  int ret;
  double *buf;
  uintptr_t len;
  bool result = true;
  uintptr_t rem = dev->playback.cycle.remaining, cl = dev->playback.cycle.len;
  dev->playback.cycle.remaining = frames;
  while ( offset < frames )
    {
      l = frames - offset;
      len = l;
      ret = dev->playback.ops->mmap_begin(dev->playback.handle,
					  (void**)&buf,
					  &o,
					  &len);
      if ( ret < 0 )
	{
	  result = false;
	  break;
	}
      dev->playback.cycle.len = len;
      ops->playback_xfer(dev,
			 client,
			 &buf[o * dev->playback.params.channels],
			 len,
			 &dev->playback.cycle,
			 dev->playback.status);
      dev->playback.cycle.remaining -= len;
      ret = dev->playback.ops->mmap_commit(dev->playback.handle,
					   o,
					   len);
      if ( ret < 0 )
	{
	  result = false;
	  break;
	}
      offset += len;
    }
  (*pointer) += offset;
  dev->playback.cycle.remaining = rem;
  dev->playback.cycle.len = cl;
  return result;
}

static intptr_t safe_rewindable(struct dspd_pcm_device *dev, intptr_t latency)
{
  intptr_t rw, minfill, rwf;
  minfill = 10000000 / dev->playback.sample_time;
  if ( minfill > latency )
    minfill = latency;
  rw = dev->playback.ops->rewindable(dev->playback.handle);
  if ( rw > 0 )
    {
      rwf = dev->playback.status->fill - rw;
      if ( rwf < minfill )
	{
	  rw -= (minfill - rwf);
	}
    }
  return rw;
}


static bool process_client_playback(struct dspd_pcm_device *dev,
				    void *client,
				    const struct dspd_client_ops *ops)
{
  uint64_t pointer, start_count;
  uint32_t latency;
  int32_t ret;
  uint64_t diff, client_gap, client_space, orig_ptr;
  intptr_t rw, frames;
  double *ptr;
  bool starting;
  size_t offset = 0;
  //This is the actual latency.  In glitch correction mode this is often more than
  //the client requested.
  latency = dev->playback.latency;
  pointer = dev->playback.status->appl_ptr;
  orig_ptr = pointer;
  start_count = dev->playback.cycle.start_count;

  if ( ops->get_playback_status )
    {
      ret = ops->get_playback_status(dev,
				     client,
				     &pointer,
				     &start_count,
				     &latency,
				     dev->playback.cycle.len,
				     dev->playback.status);
      if ( ret == -EAGAIN )
	{
	  if ( latency < dev->playback.latency &&
	       latency < dev->playback.early_cycle )
	    dev->playback.early_cycle = latency;
	  return true;
	}


      /*
	If the client needs a lower latency than what is currently accepted or
	the buffer is possibly overfilled then schedule an early wakeup.  In many cases
	a scheduled early wakeup was already going to wake up early anyway.  In that case
	the scheduled wakeup would signal that there is work to do.  The early wakeup will
	be reset whenever clients get processed again and then it could possibly be turned back
	on again here.
      */
      if ( latency < dev->playback.early_cycle && 
	   (latency < (dev->playback.status->fill+dev->playback.cycle.len) ||
	    latency < dev->playback.latency))
	{
	  dev->playback.early_cycle = latency;
	}
      
      diff = pointer - dev->playback.status->hw_ptr;
      //If there is too much data in the buffer and there is not an underrun
      if ( diff >= latency && diff <= dev->playback.params.bufsize )
	return true;
      
      if ( latency > dev->playback.latency )
	latency = dev->playback.latency;

      //The space in the playback buffer for this client is no more than the client
      //latency and usually less.
      if ( diff >= dev->playback.params.bufsize )
	client_space = latency;
      else //How many frames to fill up to this clients latency requirement?
	client_space = latency - diff;
      if ( client_space == 0 )
	return true;
      



      starting = start_count != dev->playback.cycle.start_count;
      if ( starting == true && pointer > dev->playback.status->appl_ptr )
	{
	  client_gap = 0;
	  offset = pointer - dev->playback.status->appl_ptr;
	} else
	{
	  //Space between client application pointer and the current device application pointer.
	  client_gap = dev->playback.status->appl_ptr - pointer;
	}
    } else
    {
      client_space = dev->playback.cycle.len;
      client_gap = 0;
    }
 
  if ( client_gap )
    {
      if ( starting )
	rw = safe_rewindable(dev, latency);
      else
	rw = dev->playback.ops->rewindable(dev->playback.handle);
      if ( rw < 0 )
	goto out;
      if ( dspd_dctx.debug && rw < client_gap )
	fprintf(stderr, "Error rewinding for client: wanted %ld, got %ld\n", (long)client_gap, (long)rw);
      if ( rw < client_gap )
	frames = rw;
      else
	frames = client_gap;
      if ( frames > 0 )
	{
	  rw = dev->playback.ops->rewind(dev->playback.handle, frames);
	  if ( rw < 0 )
	    goto out;
	  if ( rw > 0 )
	    {

	      //The pointer is possibly ahead of the clients application pointer
	      //because rewinding that far isn't always possible.
	      pointer = dev->playback.status->appl_ptr;
	      //Should not be writing beyond the requested buffer space for this client.
	      if ( rw > client_gap )
		frames = client_gap;
	      else
		frames = rw;
	      if ( frames > client_space )
		frames = client_space;
	      client_space -= frames;
	      if ( ! process_rewound_playback(dev,
					      client,
					      ops,
					      &pointer,
					      frames,
					      rw) )
		{
		  goto out;
		}
	      DSPD_ASSERT(dev->playback.status->appl_ptr <= orig_ptr);
	   
	      frames = orig_ptr - dev->playback.status->appl_ptr;
	      if ( frames > 0 )
		{
		  ret = dev->playback.ops->forward(dev->playback.handle, frames);
		  if ( ret < 0 )
		    goto out;
		  if ( dspd_dctx.debug && ret < frames )
		    fprintf(stderr, "Error moving pointer forward: expected %lu, got %d\n", (long)frames, ret);
		}
	    }
	}
    }
  DSPD_ASSERT(dev->playback.status->appl_ptr == orig_ptr);
  if ( client_space > 0 )
    {
      //Can't go beyond the current segment.
      if ( client_space > dev->playback.cycle.len )
	client_space = dev->playback.cycle.len;
      if ( dev->playback.cycle.addr &&
	   dev->playback.cycle.len )
	{
	  if ( offset < client_space )
	    {
	      client_space -= offset;
	      intptr_t f;
	      if ( offset > 0 )
		{
		  f = dev->playback.ops->adjust_pointer(dev->playback.handle, offset);
		} else
		{
		  f = 0;
		}
	      offset *= dev->playback.params.channels;
	      offset += dev->playback.cycle.offset * dev->playback.params.channels;
	      ptr = dev->playback.cycle.addr;
	      ops->playback_xfer(dev,
				 client,
				 &ptr[offset],
				 client_space,
				 &dev->playback.cycle,
				 dev->playback.status);
	      
	      if ( f > 0 )
		if ( dev->playback.ops->adjust_pointer(dev->playback.handle, f * -1) == 0 )
		  goto out;
	    }
	}
    }

  
 
  DSPD_ASSERT(dev->playback.status->appl_ptr == orig_ptr);
  return true;

 out:
 
  stream_recover(&dev->playback);
  return false;

}

static bool process_rewound_capture(struct dspd_pcm_device *dev,
				    void *client,
				    const struct dspd_client_ops *ops,
				    uint64_t *pointer,
				    uint32_t  frames)
{
  uintptr_t offset = 0, l, o;
  int ret;
  float *buf;
  uintptr_t len;
  bool result = true;
  while ( offset < frames )
    {
      l = frames - offset;
      len = l;
      ret = dev->capture.ops->mmap_begin(dev->capture.handle,
					 (void**)&buf,
					 &o,
					 &len);
      if ( ret < 0 )
	{
	  result = false;
	  break;
	}
      ops->capture_xfer(dev,
			client,
			&buf[o * dev->capture.params.channels],
			len,
			&dev->capture.cycle,
			dev->capture.status);
      ret = dev->capture.ops->mmap_commit(dev->capture.handle,
					  o,
					  len);
      if ( ret < 0 )
	{
	  result = false;
	  break;
	}
      offset += len;
    }
  (*pointer) += offset;
  return result;
}

static void process_client_capture(struct dspd_pcm_device *dev,
				   void *client,
				   const struct dspd_client_ops *ops)
{
  int32_t ret;
  uint64_t pointer, start_count;
  uint32_t latency;
  intptr_t rw, max_xfer, adj, diff, offset = 0, len;
  float *ptr;
  ptr = dev->capture.cycle.addr;
  start_count = dev->capture.cycle.start_count;
  pointer = dev->capture.status->appl_ptr;
  latency = dev->capture.latency;
  ret = ops->get_capture_status(dev,
				client,
				&pointer,
				&start_count,
				&latency,
				dev->capture.status);
  if ( ret == -EAGAIN )
    {
      if ( latency < dev->capture.latency )
	dev->capture.early_cycle = latency;
      return;
    }
  max_xfer = latency * 2;
  /*
    Rewinding and forwarding are only done at startup to find the correct position.  Otherwise
    it would increase latency by effectively changing the buffer size.
   */
  if ( start_count != dev->capture.cycle.start_count )
    {
      if ( pointer < dev->capture.status->appl_ptr )
	{
	  rw = dev->capture.ops->rewindable(dev->capture.handle);
	  if ( rw < 0 )
	    goto err;
	  if ( ret > 0 )
	    {
	      
	      
	      rw = dev->capture.ops->rewindable(dev->capture.handle);
	      if ( rw < 0 )
		goto err;
	      if ( rw == 0 )
		goto xfer;
	      ret = dev->capture.ops->rewind(dev->capture.handle, rw);
	      if ( ret < 0 )
		goto err;
	      if ( ret == 0 )
		goto xfer;

	      if ( ret >= max_xfer )
		max_xfer = 0;
	      else
		max_xfer -= ret;
	      pointer = dev->capture.status->appl_ptr;


	      if ( ! process_rewound_capture(dev,
					     client,
					     ops,
					     &pointer,
					     ret) )
		goto err;
	      
	      if ( max_xfer == 0 )
		return;

	    }

	} else if ( pointer > dev->capture.status->appl_ptr )
	{
	  diff = pointer - dev->capture.status->appl_ptr;
	  adj = dev->capture.ops->adjust_pointer(dev->capture.handle, diff);
	  len = dev->capture.cycle.len - adj;
	  offset = (adj + dev->capture.cycle.offset) % dev->capture.params.bufsize;
	  
	  if ( max_xfer > len )
	    max_xfer = len;
	  ops->capture_xfer(dev,
			    client,
			    &ptr[offset*dev->capture.params.channels],
			    max_xfer,
			    &dev->capture.cycle,
			    dev->capture.status);

	  if ( adj )
	    if ( dev->capture.ops->adjust_pointer(dev->capture.handle, adj * -1L) == 0 )
	      goto err;
	  return;
	}
    }

 xfer:
  if ( max_xfer > dev->capture.cycle.len )
    max_xfer = dev->capture.cycle.len;
  offset = dev->capture.cycle.offset;
  ops->capture_xfer(dev,
		    client,
		    &ptr[offset*dev->capture.params.channels],
		    max_xfer,
		    &dev->capture.cycle,
		    dev->capture.status);
  return;

 err:
  stream_recover(&dev->capture);
}



static void _alert_one_client(struct dspd_pcm_device *dev, int32_t client, int32_t error)
{
  struct dspd_client_ops *cli_ops;
  struct dspd_pcmsrv_ops *srv_ops;
  void *cli;
  if ( error == -1 )
    {
      //Must have had a SIGBUS
      dspd_slist_entry_get_pointers(dev->list,
				    client,
				    &cli,
				    (void**)&srv_ops,
				    (void**)&cli_ops);
      if ( cli_ops )
	{
	  if ( cli_ops->error )
	    cli_ops->error(dev, dev->key, cli, EFAULT);
	}
      dspd_client_srv_unlock(dev->list, client);
      dspd_daemon_unref(client); //Might sleep and free resources
      dspd_clr_bit(dev->lock_mask, client);
      return;
    }

  /*
    This is not realtime safe.  The best way to prevent glitches is to try not to cause a
    SIGBUS or remove a device.  It should be good enough that the daemon doesn't crash or
    exit when these things happen.

    Theoretically this might lock a client that had just migrated to another device.  On the
    other hand, it gives a client that is still present a chance to migrate to another device.

    How shitty would it be to make the client code arrange to migrate in some other context when
    we have a perfectly good unused thread right here?

  */
  if ( dev->client_configs[client] & DSPD_CBIT_PRESENT ) //Might be wrong (race condition, not unsafe memory)
    {
      /*
	Increment reference count so the client can change its locks if necessary.
	Might sleep because it takes a lock to do it.
      */

      

      if ( dspd_daemon_ref(client, DSPD_DCTL_ENUM_TYPE_CLIENT) == 0 )
	{

	  if ( dspd_test_bit(dev->lock_mask, client) || dspd_client_srv_lock(dev->list, client, dev->key) ) //Might sleep
	    {
	      dspd_slist_entry_get_pointers(dev->list,
					    client,
					    &cli,
					    (void**)&srv_ops,
					    (void**)&cli_ops);
	      if ( cli_ops )
		{
		  if ( cli_ops->error )
		    cli_ops->error(dev, dev->key, cli, error);
		}
	      dspd_clr_bit(dev->lock_mask, client);
	      dspd_client_srv_unlock(dev->list, client);
	    }
	  dspd_daemon_unref(client); //Might sleep and free resources
	}
    }
}	

static void alert_one_client(struct dspd_pcm_device *dev, int32_t client, int32_t error)
{
  dev->current_exception = error;
  _alert_one_client(dev, client, error);
  dev->current_exception = 0;
}
		       
static void alert_all_clients(struct dspd_pcm_device *dev, int32_t error)
{
  int i = 0;
  while ( i < DSPD_MAX_OBJECTS )
    {
      dev->exc_client = i;
      alert_one_client(dev, i, error);
      i++;
    }
  dev->exc_client = 0;
  dev->current_exception = 0;
}

static void unlock_all_clients(struct dspd_pcm_device *dev)
{
#ifdef ENABLE_LOCK_OPTIMIZATION
  size_t i;
  dev->must_unlock = true;
  for ( i = 0; i < DSPD_MAX_OBJECTS; i++ )
    {
      if ( dspd_test_bit(dev->lock_mask, i) )
	dspd_client_srv_unlock(dev->list, i);
    }
  memset(dev->lock_mask, 0, sizeof(dev->lock_mask));
  dev->lock_count = 0;
#endif
}

static bool process_clients_once(struct dspd_pcm_device *dev, uint32_t ops)
{
  //Process all clients.  Must lock and unlock as they
  //are processed.

  size_t maxidx, i, trigger_index;
  bool playback = false, capture = false, playback_ready, capture_ready, err = 0;
  bool ready;
  uint8_t tm;
  struct dspd_client_ops *cli_ops;
  struct dspd_pcmsrv_ops *srv_ops;
  void *cli;
  bool unlock;
  if ( (ops & (EPOLLIN|EPOLLOUT)) == (EPOLLIN|EPOLLOUT) )
    {
      if ( dev->playback.streams > dev->capture.streams )
	maxidx = dev->playback.streams;
      else
	maxidx = dev->capture.streams;
      playback = capture = true;
    } else if ( ops & EPOLLIN )
    {
      maxidx = dev->capture.streams;
      capture = true;
    } else if ( ops & EPOLLOUT )
    {
      maxidx = dev->playback.streams;
      playback = true;
    } else
    {
      maxidx = 0;
    }
  if ( dev->process_data )
    dev->process_data(dev->arg, dev);

  if ( maxidx < dev->lock_count )
    maxidx = dev->lock_count;

  

  for ( i = 0; i < maxidx; i++ )
    {
      trigger_index = i << 1U; //i*2
      tm = get_trigger_mask((uint8_t*)dev->reg.client_mask, trigger_index);
      playback_ready = playback && (tm & DSPD_PCM_SBIT_PLAYBACK);
      capture_ready = capture && (tm & DSPD_PCM_SBIT_CAPTURE);
      ready = playback_ready | capture_ready;
     

      if ( ready )
	{
#ifdef ENABLE_LOCK_OPTIMIZATION
	  if ( ! dspd_test_bit(dev->lock_mask, i) )
	    {
	      if ( ! dspd_client_srv_trylock(dev->list, i, dev->key) )
		continue;
	      dspd_set_bit(dev->lock_mask, i);
	      if ( i >= dev->lock_count )
		dev->lock_count = i + 1U;
	    }
#else
	  if ( dspd_client_srv_trylock(dev->list, i, dev->key) )
#endif
	    {
	      dev->current_client = i;
	      //Try again with the lock held
	      tm = get_trigger_mask((uint8_t*)dev->reg.client_mask, trigger_index);
	      playback_ready = playback && (tm & DSPD_PCM_SBIT_PLAYBACK);
	      capture_ready = capture && (tm & DSPD_PCM_SBIT_CAPTURE);
	      ready = playback_ready | capture_ready;
	      dspd_slist_entry_get_pointers(dev->list,
					    i,
					    &cli,
					    (void**)&srv_ops,
					    (void**)&cli_ops);
	    
	      if ( playback_ready )
		err = ! process_client_playback(dev, cli, cli_ops);
	      
	      if ( capture_ready )
		process_client_capture(dev, cli, cli_ops);
	      dev->current_client = -1;

	      //This could have been two branches (not considering what the cc optimizer might do), but
	      //it can easily be a NOT, OR, and branch (JMP).
	      //if ( dev->must_unlock || ready == false )
	      unlock = !ready; unlock |= dev->must_unlock; if ( unlock ) { 
		dspd_client_srv_unlock(dev->list, i);
		dspd_clr_bit(dev->lock_mask, i);
	      }
	      
	      if ( err )
		break;
	    }
	} else if ( dev->must_unlock && dspd_test_bit(dev->lock_mask, i) )
	{
#ifdef ENABLE_LOCK_OPTIMIZATION
	  dspd_client_srv_unlock(dev->list, i);
	  dspd_clr_bit(dev->lock_mask, i);
#endif
	}
    }


  if ( dev->must_unlock )
    dev->lock_count = 0;
  return err;
}

static int stream_prepare(struct dspd_pcmdev_stream *stream)
{
  int32_t ret = DSPD_PCMDRV_IO_PENDING;
  if ( ! (stream->io_pending & DSPD_PCMDRV_PREPARE_PENDING) )
    {
      stream->cycle.start_count++;
      if ( stream->cycle.start_count == 0 )
	stream->cycle.start_count++;
      if ( stream->glitch && dspd_get_glitch_correction() == DSPD_GHCN_AUTO )
	stream->glitch = false;
      ret = stream->ops->prepare(stream->handle);
      if ( ret == DSPD_PCMDRV_IO_PENDING )
	stream->io_pending |= DSPD_PCMDRV_PREPARE_PENDING;
    }
  return ret;
}



static int _stream_recover_fcn(struct dspd_pcmdev_stream *stream)
{
  unlock_all_clients(stream->dev);
  stream->started = false;
  stream->status = NULL;
  stream->cycle.start_count++;
  if ( stream->cycle.start_count == 0 )
    stream->cycle.start_count++;
  int ret;
  if ( stream->glitch == false )
    {
      ret = dspd_get_glitch_correction();
      if ( ret == DSPD_GHCN_LATCH || ret == DSPD_GHCN_AUTO )
	{
	  if ( stream->ops->get_error )
	    ret = stream->ops->get_error(stream->handle);
	  else
	    ret = -EPIPE;
	  if ( ret == -EPIPE )
	    {
	      stream->glitch = true;
	      if ( stream->latency < stream->glitch_threshold )
		{
		  stream->latency = stream->ops->set_latency(stream->handle,
							     stream->glitch_threshold,
							     stream->latency);
		}
	    }
	}
    }
  if ( ! (stream->io_pending & DSPD_PCMDRV_RECOVER_PENDING) )
    {
      ret = stream->ops->recover(stream->handle);
      if ( ret == DSPD_PCMDRV_IO_PENDING )
	stream->io_pending |= DSPD_PCMDRV_RECOVER_PENDING;
    } else
    {
      ret = DSPD_PCMDRV_IO_PENDING;
    }
  return ret;
}

static bool device_playback_cycle(struct dspd_pcm_device *dev, uintptr_t frames)
{
  uintptr_t offset = 0;
  int32_t ret, ops, err;
  bool playback_error = false;
  while ( offset < frames )
    {
      if ( dev->playback.running )
	{
	  dev->playback.cycle.len = frames - offset;
	  ret = dev->playback.ops->mmap_begin(dev->playback.handle,
					      (void**)&dev->playback.cycle.addr,
					      &dev->playback.cycle.offset,
					      &dev->playback.cycle.len);
	  if ( ret < 0 )
	    {
	      if ( ret != -EAGAIN )
		playback_error = true;
	      break;
	    }
	}
      ops = 0;
      if ( dev->playback.running )
	ops |= EPOLLOUT;
      playback_error = process_clients_once(dev, ops);
      if ( playback_error )
	break;
      if ( dev->playback.running )
	{
	  ret = dev->playback.ops->mmap_commit(dev->playback.handle,
					       dev->playback.cycle.offset,
					       dev->playback.cycle.len);
	  if ( ret < 0 )
	    {
	      playback_error = true;
	      break;
	    } else
	    {
	      offset += ret;
	      dev->playback.cycle.remaining -= ret;
	      if ( ! dev->playback.started )
		{
		  if ( (err = dev->playback.ops->start(dev->playback.handle)) < 0 )
		    {
		      playback_error = true;
		      break;
		    } else
		    {
		      dev->playback.started = 1;
		      //Wake up early the first time just in case the device
		      //consumes data fast at startup.  This is often the case with 
		      //USB and IEEE1394 and some embedded devices.
		      if ( dev->playback.early_cycle > dev->playback.params.min_dma )
                        dev->playback.early_cycle = dev->playback.params.min_dma;

		    }
		}
	      if ( frames > 0 && dev->playback.stop_threshold > 0 )
		{
		  dev->playback.stop_count += ret;
		}
	    }
	}
    }
  if ( playback_error )
    {
      if ( stream_recover(&dev->playback) < 0 )
	{
	  dspd_sched_abort(dev->sched);
	  return false;
	}
    }
  return true;
}

/*
  Figure out how many loops are needed to fill up the buffer.  This assumes
  that the CPU can generate data as fast as the sound card can use it.
  In reality it must be faster, but the system timer and sound card may
  not have perfectly accurate clocks.

  The problems this is really designed to prevent is the issue where it
  wakes up late and there is a tiny amount of data in the buffer and
  a huge amount needs rendered.  For example, it might wake up with 100us
  and need to render 100ms.  It should not render 100ms, but instead some
  smaller amount to ensure that the buffer does not go empty.  It isn't
  practical to really count CPU cycles.  It is possible to observe the
  passage of time using the sound card and system clocks.  That has to
  be done anyway, so a number of io cycles that actually works reliably
  can be guessed from the known values.

  This slightly increases CPU usage.  At high latencies it the wakeup
  count is so low it won't really matter.  At low latencies the system
  usually keeps up but if it didn't then it could recover and may not
  fall behind all the time.

*/
static uint32_t get_io_cycle_count(uint32_t fill, uint32_t space)
{
  uint32_t f, i;
  if ( space > fill && fill > 0 )
    {
      f = fill;
      i = 1;
      do {
	  f *= 2;
	  i++;
      } while ( f < space );
    } else
    {
      i = 1;
    }
  return i;
}


static int stream_drop(struct dspd_pcmdev_stream *stream)
{
  stream->status = NULL;
  stream->stop_count = 0;
  stream->stop_threshold = 0;
  stream->status = NULL;
  stream->started = 0;
  stream->running = 0;
  stream->cycle.addr = NULL;
  stream->cycle.len = 0;
  stream->cycle.offset = 0;
  memset(&stream->cycle, 0, sizeof(stream->cycle));
  stream->last_hw = 0;
  dspd_intrp_reset(&stream->intrp);
  return stream->ops->drop(stream->handle);
}

static void schedule_playback_wake(void *userdata)
{
  struct dspd_pcm_device *dev = userdata;
  int32_t ret;
  uintptr_t len, count, i, l, written = 0, total, n;
  uint16_t revents;
  if ( AO_load(&dev->error) != 0 )
    dspd_sched_abort(dev->sched);

  dev->trigger = false; //If it triggered then that was processed already.
  dspd_sync_reg(dev);
  if ( ! dev->capture.ops )
    incr_intr_count(dev);
  if ( dev->playback.fds_set )
    {
      dev->playback.fds_set = 0;
      ret = dev->playback.ops->poll_revents(dev->playback.handle,
					    dev->playback.pfds,
					    dev->playback.nfds,
					    &revents);
      if ( (revents & (POLLERR|POLLNVAL|POLLHUP)) || ret < 0 )
	{
	  ret = stream_recover(&dev->playback);
	  if ( ret < 0 )
	    {
	      dspd_sched_abort(dev->sched);
	      return;
	    }
	}
      for ( i = 0; i < dev->playback.nfds; i++ )
	dev->playback.pfds[i].revents = 0;
    }

  if ( dev->playback.running )
    {
      if ( dev->playback.status == NULL )
	{
	  ret = dev->playback.ops->status(dev->playback.handle, &dev->playback.status, false);
	  if ( ret < 0 )
	    goto out;
	} else if ( dev->playback.status->space > dev->playback.params.bufsize )
	{
	  goto out;
	}

      if ( dev->playback.status->tstamp )
	{
	  dspd_intrp_update(&dev->playback.intrp, 
			    dev->playback.status->tstamp,
			    dev->playback.status->hw_ptr - dev->playback.last_hw);
	  dev->playback.last_hw = dev->playback.status->hw_ptr;
	}
      if ( dev->playback.status->space == 0 && dev->playback.early_cycle != UINT64_MAX )
	{
	  dev->playback.cycle.len = 0;
	  dev->playback.early_cycle = UINT64_MAX;
	  dev->must_unlock = true;
	  process_clients_once(dev, EPOLLOUT);
	} 

      if ( dev->playback.status->space > 0 )
	{
	  dev->playback.early_cycle = UINT64_MAX;
	  if ( dev->playback.status->space > dev->pxferlen_hint )
	    {
	      n = dev->pxferlen_hint;
	      dev->must_spin = true;
	    } else
	    {
	      n = dev->playback.status->space;
	    }
	  count = get_io_cycle_count(dev->playback.status->fill, n);
	  if ( count == 1 )
	    {
	      //Write it all
	      len = dev->playback.status->space;
	    } else
	    {
	      //Start with the amount of data that is in the buffer.
	      //This keeps it from underrunning when latency changes
	      //from very low to very high and when a deadline is missed
	      //but it did not quite underrun.  It is possible that this
	      //thread will get preempted or take too long rendering data
	      //and the buffer will go empty even though the OS scheduled it
	      //early enough to not xrun.  The amount rendered will be doubled
	      //until the correct amount is reached.
	      len = dev->playback.status->fill;
	      DSPD_ASSERT(len <= dev->playback.status->space);
	    }
	  
	  if ( dev->playback.glitch && dev->playback.status && 
	       dev->playback.requested_latency < dev->playback.glitch_threshold )
	    {	      
	      l = dev->playback.requested_latency / 4;
	      
	      total = dev->playback.status->space;
	      len = l;
	      if ( len > total )
		len = total;

	      //This probably won't make much of a difference.  Make sure the first chunk is definitely
	      //going to be completed in the available time.
	      int32_t buftime, avail_min;
	      dspd_sched_get_deadline_hint(dev->sched, &avail_min, &buftime);
	      if ( avail_min > 0 && avail_min < len )
		len = avail_min;
	      

	      if ( len > dev->playback.status->fill && dev->playback.status->fill > 0 )
		len = dev->playback.status->fill;
	      dev->playback.cycle.remaining = total;
	      while ( written < total )
		{
		  n = written + len;
#ifdef ENABLE_LOCK_OPTIMIZATION
		  dev->must_unlock = n >= total;
#endif
		  if ( ! device_playback_cycle(dev, len) )
		    {
#ifdef ENABLE_LOCK_OPTIMIZATION
		      //might happen for an error state
		      if ( dev->lock_count > 0 )
			unlock_all_clients(dev);
#endif
		      break;
		    }
		  if ( ! dev->playback.status )
		    break;
		  written = n;
		  len *= 2;
		  if ( len > dev->playback.status->space )
		    len = dev->playback.status->space;
		  if ( len > dev->playback.requested_latency )
		    len = dev->playback.requested_latency;

		  if ( len > 0 && written > dev->playback.requested_latency )
		    {
		      if ( dev->playback.ops->status(dev->playback.handle, &dev->playback.status, false) < 0 )
			goto out;
		    } else if ( len == 0 )
		    {
		      break;
		    }
		}
	    } else
	    {
	      dev->playback.cycle.remaining = dev->playback.status->space;
	      n = count - 1;
	      total = dev->playback.status->space;
	      for ( i = 0; i < count; i++ )
		{
		  written += len;
#ifdef ENABLE_LOCK_OPTIMIZATION
		  dev->must_unlock = i == n || written >= total;
#else
		  (void)n;
#endif
		  if ( ! device_playback_cycle(dev, len) )
		    {
#ifdef ENABLE_LOCK_OPTIMIZATION
		      //might happen for an error state
		      if ( dev->lock_count > 0 )
			unlock_all_clients(dev);
#endif
		      break;
		    }
		  
		  if ( dev->playback.status )
		    {
		      len *= 2;
		      if ( len > dev->playback.status->space )
			len = dev->playback.status->space;
		      if ( len == 0 )
			break;
		    } else
		    {
		      break;
		    }
		}
	    }
#ifdef ENABLE_LOCK_OPTIMIZATION	  
	  if ( dev->must_unlock == false && dev->lock_count > 0 ) //unlikely
	    unlock_all_clients(dev);
#endif
	  if ( dev->playback.stop_threshold > 0 && 
	       dev->playback.stop_count >= dev->playback.stop_threshold &&
	       dev->playback.started != 0 )
	    {
	      if ( stream_drop(&dev->playback) < 0 )
		{
		  dspd_sched_abort(dev->sched);
		} else if ( dev->playback.glitch )
		{
		  if ( dspd_get_glitch_correction() == DSPD_GHCN_AUTO )
		    dev->playback.glitch = false;
		}
	    }
	}
    } 
  return;

 out:
  ret = stream_recover(&dev->playback);
  if ( ret < 0 )
    dspd_sched_abort(dev->playback.handle);
  DSPD_ASSERT(dev->must_unlock == true);
  return;
}

static bool schedule_capture_sleep(void *data, uint64_t *abstime, uint64_t *deadline, int32_t *reltime)
{
  struct dspd_pcm_device *dev = data;
  uint64_t f;
  bool ret = true;
  dev->idle = false;
  *deadline = UINT64_MAX;
  if ( dev->capture.io_pending )
    {
      if ( check_io(dev, &dev->capture) )
	{
	  //Nothing to do until io is complete
	  *abstime = UINT64_MAX;
	  *reltime = DSPD_SCHED_STOP;
	  *deadline = UINT64_MAX;
	  return true;
	}
    }

  if ( ! dev->playback.ops )
    dspd_sync_reg(dev);

  //dev->must_spin isn't used here because capture already handles the case where data is purposely left over
  if ( dev->capture.status )
    {
      //A latency change may not be applied until the buffer level
      //is low enough, otherwise clients have no good way to see when
      //they can safely start reading.  A client should make sure
      //that dev->capture.latency is less than or equal to what it
      //requested.
      if ( dev->capture.status->fill <= dev->current_latency &&
	   dev->current_latency != dev->capture.latency )
	{
	  dev->capture.latency = dev->current_latency;
	  dev->capture.ops->set_latency(dev->capture.handle, dev->capture.latency, dev->capture.latency);
	}
      *deadline = dev->capture.status->space * dev->capture.sample_time;
      if ( dev->capture.status->fill > 0 )
	{
	  *abstime = dev->capture.status->tstamp;
	  *reltime = DSPD_SCHED_SPIN;
	  ret = false;
	} else
	{
	  f = dev->capture.latency * dev->capture.sample_time;
	  *abstime = dev->capture.status->tstamp + (f / 2ULL);
	  *reltime = DSPD_SCHED_WAIT;
	}
    } else if ( dev->capture.running )
    {
      *reltime = DSPD_SCHED_SPIN;
      *abstime = UINT64_MAX;
      *deadline = dev->capture.latency * dev->capture.sample_time;
    } else
    {
      *reltime = DSPD_SCHED_STOP;
      *abstime = UINT64_MAX;
      *deadline = UINT64_MAX;
      set_idle(dev);
    }
  return ret;
}
static void schedule_capture_wake(void *data)
{
  struct dspd_pcm_device *dev = data;
  int32_t ret;
  if ( AO_load(&dev->error) != 0 )
    dspd_sched_abort(dev->sched);

  dev->trigger = false; //If it triggered then that was processed already.
  dspd_sync_reg(dev);
  if ( ! dev->playback.ops )
    incr_intr_count(dev);
  if ( dev->capture.running )
    {
      if ( dev->capture.status == NULL )
	{
	  if ( ! dev->capture.started )
	    {
	      ret = dev->capture.ops->start(dev->capture.handle);
	      if ( ret < 0 )
		{
		  if ( stream_recover(&dev->capture) < 0 )
		    dspd_sched_abort(dev->sched);
		  return;
		} else
		{
		  dev->capture.started = true;
		}
	    }
	  ret = dev->capture.ops->status(dev->capture.handle, &dev->capture.status, false);
	  if ( ret < 0 )
	    {
	      if ( stream_recover(&dev->capture) < 0 )
		dspd_sched_abort(dev->sched);
	      return;
	    }
	}
      if ( dev->capture.status->fill > dev->capture.latency )
	dev->capture.cycle.len = dev->capture.status->fill;
      else
	dev->capture.cycle.len = dev->capture.latency;
      
      //Don't xfer too much, otherwise long running computations might make other devices have glitches.
      if ( dev->capture.cycle.len > dev->cxferlen_hint )
	dev->capture.cycle.len = dev->cxferlen_hint;

      ret = dev->capture.ops->mmap_begin(dev->capture.handle,
					 (void**)&dev->capture.cycle.addr,
					 &dev->capture.cycle.offset,
					 &dev->capture.cycle.len);
      if ( ret < 0 )
	{
	  if ( ret != -EAGAIN )
	    {
	      if ( stream_recover(&dev->capture) < 0 )
		dspd_sched_abort(dev->sched);
	    }
	  return;
	}

      dev->capture.cycle.remaining = dev->capture.status->fill - dev->capture.cycle.len;
      dev->must_unlock = true;
      process_clients_once(dev, POLLIN);

      ret = dev->capture.ops->mmap_commit(dev->capture.handle,
					  dev->capture.cycle.offset,
					  dev->capture.cycle.len);
     
      if ( ret < 0 )
	{
	  if ( stream_recover(&dev->capture) < 0 )
	    dspd_sched_abort(dev->sched);
	} else 
	{
	  if ( dev->capture.stop_threshold > 0 )
	    dev->capture.stop_count += dev->capture.cycle.len;
	  if ( dev->capture.stop_threshold > 0 &&
	       dev->capture.stop_count >= dev->capture.stop_threshold &&
	       dev->capture.started != 0 )
	    {
	      if ( stream_drop(&dev->capture) < 0 )
		dspd_sched_abort(dev->sched);
	    }
	}
    }
  return;
}

struct dspd_pcm_device *dspd_pcm_device_get_context(void)
{
  struct dspd_scheduler *sched = dspd_sched_get();
  struct dspd_pcm_device *dev = NULL;
  if ( sched )
    dev = sched->udata;
  return dev;
}

static void dev_thread_destructor(struct dspd_scheduler *sch, void *arg)
{
   //Drop realtime priority
  struct sched_param param;
  struct dspd_pcm_device *dev = arg;

  dspd_sched_stop_workq(sch);

  memset(&param, 0, sizeof(param));
  param.sched_priority = 0;
  if ( dev->iothread.init )
    pthread_setschedparam(dev->iothread.thread, SCHED_OTHER, &param);
 

  AO_store(&dev->error, ENODEV);
  unlock_all_clients(dev);
  alert_all_clients(dev, ENODEV);
  

  const char *devname = NULL;
  if ( dev->playback.handle )
    devname = dev->playback.params.name;
  else if ( dev->capture.handle )
    devname = dev->capture.params.name;
  dspd_log(0, "Device thread %s exiting.  refcount=%d", devname, dspd_slist_refcnt(dspd_dctx.objects, dev->key));

  dspd_daemon_unref(dev->key);
}
  
static void dev_started(void *user_data)
{
  struct dspd_pcm_device *dev = user_data;
  struct dspd_scheduler *sched = dev->sched;
  if ( ! (sched->flags & DSPD_SCHED_SLAVE) )
    dspd_daemon_set_thread_nice(-1, DSPD_THREADATTR_RTIO);
    
}



static void incr_intr_count(struct dspd_pcm_device *dev)
{
  AO_t val;
  if ( dev->playback.running || dev->capture.running )
    {
      val = AO_load(&dev->irq_count);
      val++;
      AO_store(&dev->irq_count, val);
      if ( dev->playback.streams || dev->capture.streams )
	{
	  val = AO_load(&dev->ack_count);
	  val++;
	  AO_store(&dev->ack_count, val);
	}
    }
}

static void schedule_fullduplex_wake(void *data)
{
  schedule_playback_wake(data);
  schedule_capture_wake(data);
  incr_intr_count(data);
}

static bool schedule_fullduplex_sleep(void *data, uint64_t *abstime, uint64_t *deadline, int32_t *reltime)
{
  bool ret = 0;
  uint64_t p_abstime, c_abstime, p_deadline, c_deadline; 
  int32_t p_reltime, c_reltime;
  struct dspd_pcm_device *dev = data;
  dev->idle = false;
  dspd_sync_reg(dev);
  ret |= schedule_playback_sleep(data, &p_abstime, &p_deadline, &p_reltime);
  ret |= schedule_capture_sleep(data, &c_abstime, &c_deadline, &c_reltime);
  /*
    Relative timeouts aren't actually used right now, so only SPIN, WAIT, and STOP
    must be handled.
    
    Only handle SPIN if a stream should be running (streams are connected or they were recently connecting
    and it is waiting to shut off).
  */
  if ( c_deadline > p_deadline )
    *deadline = p_deadline;
  else
    *deadline = c_deadline;

  if ( (p_reltime == DSPD_SCHED_SPIN && (dev->playback.streams > 0 || dev->playback.stop_threshold > 0))
       || (c_reltime == DSPD_SCHED_SPIN && (dev->capture.streams > 0 || dev->capture.stop_threshold > 0)))
    {
      //Spin for first priority (no sleep, just iterate again)
      *reltime = DSPD_SCHED_SPIN;
      *abstime = UINT64_MAX;
    } else if ( c_reltime == DSPD_SCHED_WAIT || p_reltime == DSPD_SCHED_WAIT )
    {
      //Wait a certain amount of time.  Lowest wait wins.
      *reltime = DSPD_SCHED_WAIT;
      if ( c_reltime == DSPD_SCHED_WAIT && p_reltime == DSPD_SCHED_WAIT )
	{
	  if ( p_abstime < c_abstime && p_abstime != UINT64_MAX )
	    *abstime = p_abstime;
	  else
	    *abstime = c_abstime;
	} else if ( p_reltime == DSPD_SCHED_WAIT )
	{
	  *abstime = p_abstime;
	} else if ( c_reltime == DSPD_SCHED_WAIT )
	{
	  *abstime = c_abstime;
	}
    } else
    {
      //Stop (thread goes idle).
      *reltime = DSPD_SCHED_STOP;
      *abstime = UINT64_MAX;
      set_idle(dev);
    }
  return ret;
}

static void schedule_sigbus_handler(void *data)
{
  struct dspd_pcm_device *dev = data;
  if ( dev->current_client >= 0 )
    {
      uint32_t refcnt;
      uint64_t slotid = dspd_slist_id(dev->list, dev->current_client); //get slot id while we have a reference
      dspd_client_srv_unlock(dev->list, dev->current_client); //Still locked from earlier
      dspd_slist_entry_wrlock(dev->list, dev->current_client); //This lock must be taken first
      //Try to get a reference count
      refcnt = dspd_slist_ref(dev->list, dev->current_client);
      if ( refcnt <= 1 )
	dspd_slist_unref(dev->list, dev->current_client);
      dspd_slist_entry_rw_unlock(dev->list, dev->current_client);

      if ( refcnt > 1 )
	{
	  /*
	    At this point the device either owns the client or the trigger
	    bits should not be set because the slot is either empty or belongs
	    to something else.
	  */
	  if ( dspd_slist_id(dev->list, dev->current_client) == slotid )
	    {
	      /*
		The slot is still the one we think it is.
	      */
	      dspd_dev_lock(dev);
	      dspd_dev_client_settrigger(dev, dev->current_client, 0, 0);
	      dspd_dev_unlock(dev);
	    }
	  //This is a good place to call shutdown(client_sock, SHUT_RDWR).
	  //Anyone who causes a SIGBUS needs kicked off, even at the cost of
	  //increasing the chances of a glitch.
	  alert_one_client(dev, dev->current_client, EFAULT);
	  dspd_slist_entry_wrlock(dev->list, dev->current_client);
	  dspd_slist_unref(dev->list, dev->current_client);
	  dspd_slist_entry_rw_unlock(dev->list, dev->current_client);
	}
    }
  unlock_all_clients(dev);
}

static void dev_latency_changed(void *udata, dspd_time_t latency)
{
  struct dspd_pcm_device *dev = udata;
  if ( latency == UINT64_MAX )
    {
      dev->pxferlen_hint = UINTPTR_MAX;
      dev->cxferlen_hint = UINTPTR_MAX;
    } else
    {
      if ( dev->playback.sample_time )
	dev->pxferlen_hint = latency / dev->playback.sample_time;
      if ( dev->capture.sample_time )
	dev->cxferlen_hint = latency / dev->capture.sample_time;
    }
}

static const struct dspd_scheduler_ops playback_ops = {
  .started = dev_started,
  .wake = schedule_playback_wake,
  .sleep = schedule_playback_sleep,
  .timer_event = schedule_timer_event,
  .trigger_event = schedule_trigger_event,
  .sigbus = schedule_sigbus_handler,
  .destructor = dev_thread_destructor,
  .setlatency = dev_latency_changed,
};

static const struct dspd_scheduler_ops capture_ops = {
  .started = dev_started,
  .wake = schedule_capture_wake,
  .sleep = schedule_capture_sleep,
  .timer_event = schedule_timer_event,
  .trigger_event = schedule_trigger_event,
  .sigbus = schedule_sigbus_handler,
  .destructor = dev_thread_destructor,
  .setlatency = dev_latency_changed,
};

static const struct dspd_scheduler_ops fullduplex_ops = {
  .started = dev_started,
  .wake = schedule_fullduplex_wake,
  .sleep = schedule_fullduplex_sleep,
  .timer_event = schedule_timer_event,
  .trigger_event = schedule_trigger_event,
  .sigbus = schedule_sigbus_handler,
  .destructor = dev_thread_destructor,
  .setlatency = dev_latency_changed,
};

static const struct dspd_pcmdev_ops device_ops = {
  .connect = dspd_pcmdev_connect,
  .disconnect = dspd_pcmdev_disconnect,
  .set_latency = dspd_pcmdev_set_latency,
  .get_latency = dspd_pcmdev_get_latency,
  .trigger = dspd_pcmdev_trigger,
  .getparams = dspd_pcmdev_getparams,
};

static int32_t device_ctl(struct dspd_rctx *rctx,
			  uint32_t             req,
			  const void          *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize);

void dspd_pcm_device_delete_ex(struct dspd_pcm_device *dev, bool closedev);

static void dev_destructor(void *arg)
{
  dspd_pcm_device_delete_ex(arg, false);
}



static int dspd_mq_validate_event(struct dspd_pcm_device *dev, 
				  struct dspd_mq_notification *event,
				  int len)
{
  int ret = -1;
  if ( len == sizeof(*event) && dev->playback.running )
    {
      if ( event->client < DSPD_MAX_OBJECTS )
	{
#ifndef DSPD_HAVE_ATOMIC_INT64
	  if ( dspd_mutex_trylock(&dev->cookie_lock) == 0 )
	    {
	      if ( dev->cookie != 0 && dev->cookie == event->cookie )
		ret = event->client;
	      dspd_mutex_unlock(&dev->cookie_lock);
	    }
#else
	  uint64_t c = dspd_load_uint64(&dev->cookie);
	  if ( c != 0 && c == event->cookie )
	    ret = event->client;
#endif
	}
    }
  return ret;
}

/*
  This event is called if a client requests immediate attention.  There can be only one client doing
  that because otherwise it gets ugly and there may be glitches.
 */
static void dspd_dev_mq_event(void *udata, int32_t fd, void *fdata, uint32_t events)
{
  struct dspd_mq_notification event;
  unsigned prio;
  struct dspd_pcm_device *dev = udata;
  int ret;
  intptr_t client, c;
  uint8_t tm;
  struct dspd_client_ops *cli_ops;
  struct dspd_pcmsrv_ops *srv_ops;
  void *cli;
  bool error = false;
  ret = mq_receive(dev->mq[0], (char*)&event, sizeof(event), &prio);

  client = dspd_mq_validate_event(dev, &event, ret);
  ret = dev->playback.ops->status(dev->playback.handle, &dev->playback.status, true);
  if ( ret < 0 )
    {
      error = true;
      goto out;
    } 
  if ( dev->playback.status->tstamp )
    {
      dspd_intrp_update(&dev->playback.intrp, 
			dev->playback.status->tstamp,
			dev->playback.status->hw_ptr - dev->playback.last_hw);
      dev->playback.last_hw = dev->playback.status->hw_ptr;
    } 
  if ( client < 0 )
    {
      /*
	It is possible that some program got exclusive access then released the device and
	tried to abuse the message queue.  If so, then drain the queue and increase the wakeup
	count.  Each sleep will reduce the count by 1, so if the next wakeup is invalid
	the POLLIN event will be turned off.  It will stay off for 2 cycles before turning
	back on again.  That will probably keep the device thread from using too much CPU due to
	message queue abuse.  Another client that wants exclusive access may not work correctly
	but that can be fixed by killing the misbehaving process.
	
      */
      dev->wakeup_count += 2;
      (void)mq_receive(dev->mq[0], (char*)&event, sizeof(event), &prio);
      if ( dev->wakeup_count > 2 )
	dspd_sched_set_fd_event(dev->sched, fd, 0);
      goto out;
    }


  c = (uintptr_t)client << 1U;
  
  tm = get_trigger_mask((uint8_t*)dev->reg.client_mask, c);
  if ( tm & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( dspd_test_bit(dev->lock_mask, client) || dspd_client_srv_trylock(dev->list, client, dev->key) )
	{
	  dev->current_client = client;
	  //Try again with the lock held
	  tm = get_trigger_mask((uint8_t*)dev->reg.client_mask, c);
	  dspd_slist_entry_get_pointers(dev->list,
					client,
					&cli,
					(void**)&srv_ops,
					(void**)&cli_ops);
	  
	  if ( tm & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      dev->playback.cycle.len = 0; //Not ready, must rewind.
	      error = ! process_client_playback(dev, cli, cli_ops);
	    }
	  dev->current_client = -1;
	  dspd_client_srv_unlock(dev->list, client);
	  dspd_clr_bit(dev->lock_mask, client);
	}
    }

 out:

  if ( error )
    {
      dev->playback.started = 0;

      dev->playback.status = NULL;
      ret = stream_recover(&dev->playback);
      if ( ret < 0 )
	dspd_sched_abort(dev->sched);
    }
 
  /*
    After returning, the wakeup callback will be executed as if a timer expired.  The additional
    hwsync here is enough to improve reliability.
   */
  return;
}

   
int32_t dspd_pcm_device_new(void **dev,
			    uint64_t hotplug_event_id,
			    const struct dspd_pcmdev_params *params,
			    struct dspd_slist *list)
{
  struct dspd_pcm_device *devptr;
  int ret, s;
  struct dspd_pcmdev_stream *sptr;
  intptr_t index = 0;
  dspd_threadattr_t attr = { .init = 0 };
  void *h;
  dspd_time_t t;
  bool fullduplex;
  char name[32UL] = { 0 };
  devptr = calloc(1, sizeof(struct dspd_pcm_device));
  if ( ! devptr )
    return -errno;
  devptr->mq[0] = -1;
  devptr->mq[1] = -1;
  devptr->excl_client = -1;
  devptr->hotplug_event_id = hotplug_event_id;
  devptr->playback.dev = devptr;
  devptr->capture.dev = devptr;
  devptr->must_unlock = true;
  devptr->pxferlen_hint = UINTPTR_MAX;
  devptr->cxferlen_hint = UINTPTR_MAX;
  if ( list )
    {
      dspd_slist_wrlock(list);
      index = dspd_slist_get_free(list, 1);
      if ( index < 0 )
	{
	  free(devptr);
	  return -ENOSPC;
	}
      dspd_slist_entry_set_used(list, (uintptr_t)index, true);
      dspd_slist_unlock(list);
    }

  ret = dspd_mutex_init(&devptr->reg_lock, NULL);
  if ( ret )
    {
      ret *= -1;
      goto out;
    }
  devptr->list = list;
  devptr->key = index;

  if ( params->stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      /*
	Failure is not a fatal error because the device will work without the low latency protocol.
      */
      char buf[PATH_MAX];
      struct mq_attr mqattr;
      sprintf(buf, "/dspd-%d-dev-%ld", getpid(), (long)index);
      memset(&mqattr, 0, sizeof(mqattr));
      mqattr.mq_flags = 0;
      mqattr.mq_maxmsg = 4;
      mqattr.mq_msgsize = MIN(sizeof(struct dspd_mq_notification), sizeof(struct dspd_pcm_status));
      mqattr.mq_curmsgs = 0;
      mq_unlink(buf);
      devptr->mq[0] = mq_open(buf, O_RDWR | O_CREAT | O_NONBLOCK, 0666, &mqattr);
      if ( devptr->mq[0] >= 0 )
	{
	  devptr->mq[1] = mq_open(buf, O_RDWR | O_NONBLOCK);
	  if ( devptr->mq[1] < 0 )
	    {
	      mq_close(devptr->mq[0]);
	      devptr->mq[0] = -1;
	    }
	  mq_unlink(buf);
	}
    }

  fullduplex = (params->stream & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) == 
    (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);

  if ( params->stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( ! fullduplex )
	h = params->driver_handles[0];
      else
	h = params->driver_handles[DSPD_PCM_STREAM_PLAYBACK];

      sptr = &devptr->playback;
      sptr->ops = params->ops[DSPD_PCM_STREAM_PLAYBACK];
      sptr->ops->set_stream_index(h, index);
      ret = sptr->ops->poll_descriptors_count(h);
      if ( ret > 0 )
	{
	  sptr->nfds = ret;
	  sptr->pfds = calloc(sptr->nfds, sizeof(*sptr->pfds));
	  if ( ! sptr->pfds )
	    goto out;
	  ret = sptr->ops->poll_descriptors(h,
					    sptr->pfds,
					    sptr->nfds);
	  if ( ret < 0 )
	    goto out;
	  sptr->nfds = ret;
	}
      ret = sptr->ops->get_params(h, &sptr->params);
      if ( ret != 0 )
	goto out;
      sptr->handle = h;
      sptr->volume = 1.0;
      sptr->sample_time = 1000000000 / sptr->params.rate;
      sptr->intrp.sample_time = sptr->sample_time;
      sptr->intrp.maxdiff = sptr->sample_time / 10;
      dspd_dev_set_stream_volume(devptr, DSPD_PCM_STREAM_PLAYBACK, 1.0);

      //Try for 10-20ms of minimum buffer space in case of a glitch.
      //This won't be applied unless there is a glitch and the server is configured to do it.
      t = 10000000ULL / sptr->sample_time;
      if ( t < (sptr->params.min_latency*2) )
	t = sptr->params.min_latency * 2;
      if ( t > sptr->params.max_latency )
	sptr->glitch_threshold = sptr->params.max_latency;
      else
	sptr->glitch_threshold = 1 << get_hpo2(t);
      if ( dspd_get_glitch_correction() == DSPD_GHCN_ON )
	sptr->glitch = true;
      
    }
  
  if ( params->stream & DSPD_PCM_SBIT_CAPTURE )
    {
      sptr = &devptr->capture;
      sptr->ops = params->ops[DSPD_PCM_STREAM_CAPTURE];
      if ( ! fullduplex )
	h = params->driver_handles[0];
      else
	h = params->driver_handles[DSPD_PCM_STREAM_CAPTURE];
      sptr->ops->set_stream_index(h, index);
      ret = sptr->ops->poll_descriptors_count(h);
      if ( ret > 0 )
	{
	  sptr->nfds = ret;
	  sptr->pfds = calloc(sptr->nfds, sizeof(*sptr->pfds));
	  if ( ! sptr->pfds )
	    goto out;
	  ret = sptr->ops->poll_descriptors(h,
					    sptr->pfds,
					    sptr->nfds);
	  if ( ret < 0 )
	    goto out;
	  sptr->nfds = ret;
	}
      ret = sptr->ops->get_params(h, &sptr->params);
      if ( ret != 0 )
	goto out;

      sptr->handle = h;
      sptr->volume = 1.0;
      sptr->sample_time = 1000000000 / sptr->params.rate;
      sptr->intrp.sample_time = sptr->sample_time;
      sptr->intrp.maxdiff = sptr->sample_time / 10;
      dspd_dev_set_stream_volume(devptr, DSPD_PCM_STREAM_CAPTURE, 1.0);
    }

  struct dspd_sched_params schedparams;
  memset(&schedparams, 0, sizeof(schedparams));

  //Have single io thread
  if ( dspd_dctx.rtio_sched )
    {
      schedparams.flags |= DSPD_SCHED_SLAVE;
      //Enable async io so devices don't block each other and cause glitches.
      schedparams.flags |= DSPD_SCHED_WORKQ;
    }
  
  if ( ! (schedparams.flags & DSPD_SCHED_SLAVE) )
    {
      schedparams.flags |= DSPD_SCHED_SIGBUS;
      schedparams.flags |= DSPD_SCHED_SIGXCPU;
    }

  schedparams.nmsgs = 2;
  schedparams.nfds = 1;
  
  
  snprintf(name, sizeof(name), "dspd-io-%d", devptr->key);
  schedparams.thread_name = name;


  s = params->stream & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);
  if ( s == DSPD_PCM_SBIT_PLAYBACK )
    {
      schedparams.nfds += devptr->playback.nfds;
      schedparams.nmsgs++;
      devptr->sched = dspd_sched_new(&playback_ops, devptr, &schedparams);
    } else if ( s == DSPD_PCM_SBIT_CAPTURE )
    {
      schedparams.nmsgs++;
      schedparams.nfds += devptr->capture.nfds;
      devptr->sched = dspd_sched_new(&capture_ops, devptr, &schedparams);
    } else
    {
      schedparams.nmsgs += 2;
      schedparams.nfds += devptr->playback.nfds+devptr->capture.nfds;
      devptr->sched = dspd_sched_new(&fullduplex_ops, devptr, &schedparams);
    }
  schedparams.nmsgs += schedparams.nfds;
  schedparams.nmsgs *= 2;

  if ( ! devptr->sched )
    goto out;
  
  
  if ( devptr->playback.ops && devptr->playback.ops->set_scheduler )
    devptr->playback.ops->set_scheduler(devptr->playback.handle, devptr->sched);
  if ( devptr->capture.ops && devptr->capture.ops->set_scheduler )
    devptr->capture.ops->set_scheduler(devptr->capture.handle, devptr->sched);
  
  if ( devptr->mq[0] >= 0 )
    {
      ret = dspd_sched_add_fd(devptr->sched, devptr->mq[0], POLLIN, devptr, dspd_dev_mq_event);
      if ( ret != 0 )
	{
	  ret *= -1;
	  goto out;
	}
    }
#ifndef DSPD_HAVE_ATOMIC_INT64
  ret = dspd_mutex_init(&devptr->cookie_lock, NULL);
  if ( ret )
    {
      ret *= -1;
      goto out;
    }
#endif

  

 
  
  devptr->arg = params->arg;
  devptr->current_client = -1;

  if ( ! (devptr->sched->flags & DSPD_SCHED_SLAVE) )
    {
      
      ret = dspd_daemon_threadattr_init(&attr, sizeof(attr), DSPD_THREADATTR_DETACHED | DSPD_THREADATTR_RTIO);
      if ( ret != 0 )
	{
	  ret *= -1;
	  goto out;
	}

      //devptr->sched_policy = dspd_dctx.rtio_policy;
      //devptr->sched_param.sched_priority = dspd_dctx.rtio_priority;
      ret = dspd_thread_create(&devptr->iothread,
			       &attr,
			       dspd_sched_run,
			       devptr->sched);
      if ( ret != 0 )
	{
	  if ( ret == EPERM )
	    {
	      dspd_log(0, "Retrying RTIO thread creation without realtime priority");
	      dspd_threadattr_destroy(&attr);
	      ret = dspd_daemon_threadattr_init(&attr, sizeof(attr), DSPD_THREADATTR_DETACHED);
	      if ( ret == 0 )
		ret = dspd_thread_create(&devptr->iothread,
					 &attr,
					 dspd_sched_run,
					 devptr->sched);
	    }
	  if ( ret )
	    {
	      dspd_log(0, "Failed to create RTIO thread for device %ld", (long)index);
	      ret *= -1;
	      goto out;
	    }
	} else
	{
	  dspd_log(0, "Created RTIO thread for device %ld", (long)index);
	}
    } else
    {
      ret = dspd_sched_send_slave(dspd_dctx.rtio_sched, devptr->sched);
      if ( ret < 0 )
	goto out;
    }
  if ( list )
    {
      dspd_slist_entry_set_pointers(list, 
				    (uintptr_t)index,
				    devptr,
				    (void*)&device_ops,
				    NULL);
      dspd_slist_set_destructor(list, (uintptr_t)index, dev_destructor);
      dspd_slist_set_ctl(list, (uintptr_t)index, device_ctl);
      dspd_slist_ref(list, (uintptr_t)index);
      dspd_slist_entry_srvunlock(list, (uintptr_t)index);
      dspd_slist_entry_rw_unlock(list, (uintptr_t)index);

    }
  dspd_threadattr_destroy(&attr);
  *dev = devptr;


  return index;

 out:
  dspd_threadattr_destroy(&attr);
  dspd_pcm_device_delete_ex(devptr, 0);
  dspd_slist_entry_set_used(list, (uintptr_t)index, false);
  dspd_slist_entry_srvunlock(list, (uintptr_t)index);
  dspd_slist_entry_rw_unlock(list, (uintptr_t)index);

  return ret;
}



void dspd_pcm_device_delete(struct dspd_pcm_device *dev)
{
  return dspd_pcm_device_delete_ex(dev, 1);
}

void dspd_pcm_device_delete_ex(struct dspd_pcm_device *dev, bool closedev)
{
  DSPD_ASSERT(dev);
  dspd_sched_abort(dev->sched);
  dspd_sched_trigger(dev->sched);
  dspd_mutex_destroy(&dev->reg_lock);
#ifndef DSPD_HAVE_ATOMIC_INT64
  dspd_mutex_destroy(&dev->cookie_lock);
#endif
  if ( dev->mq[0] >= 0 )
    mq_close(dev->mq[0]);
  if ( dev->mq[1] >= 0 )
    mq_close(dev->mq[1]);

  if ( dev->playback.ops )
    {
      free(dev->playback.pfds);
      if ( closedev )
	dev->playback.ops->destructor(dev->playback.handle);
    }
  if ( dev->capture.ops )
    {
      if ( closedev )
	free(dev->capture.pfds);
      dev->capture.ops->destructor(dev->capture.handle);
    }
  if ( dev->sched )
    dspd_sched_delete(dev->sched);
  free(dev);
  return ;
}

intptr_t dspd_pcm_device_get_index(struct dspd_pcm_device *dev)
{
  return dev->key;
}



static int32_t dspd_pcmdev_connect(void *device, int32_t client)
{
  struct dspd_pcm_device *dev = device;
  int32_t ret;
  if ( client < DSPD_MAX_OBJECTS && client >= 0 )
    {
      ret = dspd_dev_attach_client(dev, (uint32_t)client);
      if ( ret == 0 )
	{
	  dspd_slist_entry_set_key(dev->list, (uintptr_t)client, dev->key);
	  dspd_slist_ref(dev->list, (uintptr_t)client);
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}

static int32_t dspd_pcmdev_disconnect(void *device, int32_t client)
{
  struct dspd_pcm_device *dev = device;
  int32_t ret;
  if ( client < DSPD_MAX_OBJECTS && client >= 0 )
    {
      ret = dspd_dev_detach_client(dev, (uint32_t)client);
      if ( ret == 0 )
	{
	  dspd_slist_entry_set_key(dev->list, (uintptr_t)client, 0);
	  dspd_slist_unref(dev->list, (uintptr_t)client);
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}


static int32_t dspd_pcmdev_set_latency(void *device, uint32_t client, uint32_t latency)
{
  struct dspd_pcm_device *dev = device;
  return dspd_dev_set_client_latency(dev, client, latency);
}

static int32_t dspd_pcmdev_get_latency(void *device, uint32_t client)
{
  struct dspd_pcm_device *dev = device;
  return dspd_dev_get_client_latency(dev, client);
}

static int32_t dspd_pcmdev_trigger(void *device, uint32_t client, uint32_t streams)
{
  struct dspd_pcm_device *dev = device;
  int32_t ret;
  dspd_dev_lock(dev);
  ret = dspd_dev_client_settrigger(dev, 
				   client,
				   streams,
				   0);
  if ( ret == 0 )
    {
      ret = dspd_dev_client_configure(dev, client);
      if ( ret == 0 )
	dspd_dev_notify(dev);
    }
  dspd_dev_unlock(dev);
  return ret;
}

static int32_t dspd_pcmdev_getparams(void *device, int32_t stream, struct dspd_drv_params *params)
{
  struct dspd_pcm_device *dev = device;
  int32_t ret = 0;
  if ( stream == DSPD_PCM_STREAM_PLAYBACK )
    {
      if ( dev->playback.ops )
	memcpy(params, &dev->playback.params, sizeof(*params));
      else
	ret = -EINVAL;
    } else if ( stream == DSPD_PCM_STREAM_CAPTURE )
    {
      if ( dev->capture.ops )
	memcpy(params, &dev->capture.params, sizeof(*params));
      else
	ret = -EINVAL;
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}



void *dspd_pcm_device_get_driver_handle(struct dspd_pcm_device *dev, uint32_t stream)
{
  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    return dev->playback.handle;
  else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    return dev->capture.handle;
  return NULL;
}

static int stream_compatible(const struct dspd_cli_params *cparams,
			     const struct dspd_drv_params *dparams)
{
  /*
    Some values can be "default" (0 or -1).
  */
  return ( (cparams->rate == dparams->rate || cparams->rate == 0) &&
	   (cparams->latency >= dparams->min_latency || cparams->latency == 0) &&
	   (cparams->channels == dparams->channels || cparams->channels == -1));
}

static int32_t server_connect(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  uint32_t client = *(int32_t*)inbuf;
  struct dspd_cli_params cparams;
  int64_t stream;
  int32_t sbits = 0;
  size_t br;
  uint32_t latency = 0;
  int ret;

  ret = AO_load(&dev->error);
  if ( ret )
    return dspd_req_reply_err(context, 0, ret);

  stream = DSPD_PCM_SBIT_PLAYBACK;
  stream <<= 32;
  stream |= dev->key;
  memset(&cparams, 0, sizeof(cparams));
  ret = dspd_stream_ctl(&dspd_dctx,
			client,
			DSPD_SCTL_CLIENT_RAWPARAMS,
			&stream,
			sizeof(stream),
			&cparams,
			sizeof(cparams),
			&br);
  if ( ret == 0 && br == sizeof(cparams) && cparams.channels != 0 )
    {
      latency = cparams.latency;
      if ( stream_compatible(&cparams, &dev->playback.params) )
	sbits |= DSPD_PCM_SBIT_PLAYBACK;
      else
	return dspd_req_reply_err(context, 0, EINVAL);
    }
  memset(&cparams, 0, sizeof(cparams));
  stream = DSPD_PCM_SBIT_CAPTURE;
  stream <<= 32;
  stream |= dev->key;
  ret = dspd_stream_ctl(&dspd_dctx,
			client,
			DSPD_SCTL_CLIENT_RAWPARAMS,
			&stream,
			sizeof(stream),
			&cparams,
			sizeof(cparams),
			&br);
  if ( ret == 0 && br == sizeof(cparams) && cparams.channels != 0 )
    {
      if ( latency == 0 || cparams.latency < latency )
	latency = cparams.latency;
      if ( stream_compatible(&cparams, &dev->capture.params) )
	sbits |= DSPD_PCM_SBIT_CAPTURE;
      else
	return dspd_req_reply_err(context, 0, EINVAL);
    }
  if ( sbits == 0 )
    return dspd_req_reply_err(context, 0, EINVAL);

  uint32_t trigger = 0, t = 0;
  if ( dspd_stream_ctl(&dspd_dctx,
		       client,
		       DSPD_SCTL_CLIENT_GETTRIGGER,
		       NULL,
		       0,
		       &trigger,
		       sizeof(trigger),
		       &br) == 0 )
    {
      if ( br == sizeof(trigger) )
	{
	  if ( trigger & DSPD_PCM_SBIT_PLAYBACK )
	    t |= DSPD_PCM_SBIT_PLAYBACK;
	  if ( trigger & DSPD_PCM_SBIT_CAPTURE )
	    t |= DSPD_PCM_SBIT_CAPTURE;
	}
    }


  dspd_dev_lock(dev);
  if ( dspd_dev_attach_client(dev, client) != -EEXIST )
    dspd_slist_ref(dev->list, (uintptr_t)client);
  if ( latency > 0 )
    dspd_pcmdev_set_latency(dev, client, latency);
  dspd_dev_client_settrigger(dev, client, t, 0);
  dspd_dev_client_configure(dev, client);
  dspd_dev_unlock(dev);
  dspd_dev_notify(dev);
  return dspd_req_reply_err(context, 0, 0);
}

static int32_t server_disconnect(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  int32_t client = *(int32_t*)inbuf;
  int32_t ret;
  dspd_dev_lock(dev);
  ret = dspd_dev_detach_client(dev, client);
  if ( ret == 0 )
    {
      dspd_dev_client_configure(dev, client);
      dspd_dev_unlock(dev);
      dspd_slist_unref(dev->list, client);
    } else
    {
      dspd_dev_unlock(dev);
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t server_settrigger(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  int32_t err;
  uint64_t val = *(uint64_t*)inbuf;
  uint32_t client = val >> 32U, trigger = val & 0xFFFFFFFF;
  dspd_dev_lock(dev);
  err = dspd_dev_client_settrigger(dev,
				   client,
				   trigger,
				   0);
  dspd_dev_unlock(dev);
  if ( err == 0 )
    dspd_dev_notify(dev);
  return dspd_req_reply_err(context, 0, err);
}


static int32_t server_setlatency(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  int32_t err, ret;
  uint64_t val = *(uint64_t*)inbuf;
  uint32_t client = val >> 32U, latency = val & 0xFFFFFFFF, l;
  dspd_dev_lock(dev);
  err = dspd_dev_set_client_latency(dev, client, latency);
  if ( err > 0 )
    {
      l = err;
      err = dspd_dev_client_configure(dev, client);
    }
  dspd_dev_unlock(dev);
  if ( err )
    {
      ret = dspd_req_reply_err(context, 0, err);
    } else
    {
      dspd_dev_notify(dev);
      ret = dspd_req_reply_buf(context, 0, &l, sizeof(l));
    }
  return ret;
}

static void getparams(const struct dspd_pcmdev_stream *s,
		      struct dspd_cli_params *params)
{
  params->format = s->params.format;
  params->channels = s->params.channels;
  params->rate = s->params.rate;
  params->bufsize = s->params.bufsize;
  params->fragsize = s->params.fragsize;
  params->stream = s->params.stream;
  params->latency = s->latency;
  params->flags = 0;
  params->min_latency = s->params.min_latency;
  params->max_latency = s->params.max_latency;
}

static int32_t server_getparams(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct dspd_cli_params *params = outbuf;
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  uint32_t stream = *(uint32_t*)inbuf;
  struct dspd_pcmdev_stream *s;
  int ret;
  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      s = &dev->playback;
      params->stream = DSPD_PCM_SBIT_PLAYBACK;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      s = &dev->capture;
      params->stream = DSPD_PCM_SBIT_CAPTURE;
    } else
    {
      s = NULL;
    }
  if ( s )
    {
      getparams(s, params);
      ret = dspd_req_reply_buf(context, 0, params, sizeof(*params));
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}

static int32_t server_setvolume(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  const struct dspd_stream_volume *sv = inbuf;
  int err;
  if ( ((sv->stream & DSPD_PCM_SBIT_PLAYBACK) != 0 && dev->playback.ops == NULL) ||
       ((sv->stream & DSPD_PCM_SBIT_CAPTURE) != 0 && dev->capture.ops == NULL) )
    {
      err = EINVAL;
    } else
    {
      if ( sv->stream & DSPD_PCM_SBIT_PLAYBACK )
	{
	  dspd_dev_set_stream_volume(dev, DSPD_PCM_STREAM_PLAYBACK, sv->volume);
	  dspd_daemon_vctrl_set_value(dev->key, DSPD_PCM_SBIT_PLAYBACK, sv->volume, NULL);
	}
      if ( sv->stream & DSPD_PCM_SBIT_CAPTURE )
	{
	  dspd_dev_set_stream_volume(dev, DSPD_PCM_STREAM_CAPTURE, sv->volume);
	  dspd_daemon_vctrl_set_value(dev->key, DSPD_PCM_SBIT_CAPTURE, sv->volume, NULL);
	}
      err = 0;
    }
  return dspd_req_reply_err(context, 0, err);
}

static int32_t server_getvolume(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  int32_t stream = *(int32_t*)inbuf;
  size_t len = 0, i;
  float vol[2] = { 0.0, 0.0 };
  if ( (stream & DSPD_PCM_SBIT_PLAYBACK) && (stream & DSPD_PCM_SBIT_CAPTURE) )
    {
      if ( outbufsize < sizeof(vol) || (dev->playback.ops == NULL || dev->capture.ops == NULL) )
	return dspd_req_reply_err(context, 0, EINVAL);
      vol[DSPD_PCM_STREAM_PLAYBACK] = dspd_dev_get_stream_volume(dev, DSPD_PCM_STREAM_PLAYBACK);
      vol[DSPD_PCM_STREAM_CAPTURE] = dspd_dev_get_stream_volume(dev, DSPD_PCM_STREAM_CAPTURE);
      len = sizeof(vol);
    } else if ( stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      vol[0] = dspd_dev_get_stream_volume(dev, DSPD_PCM_STREAM_PLAYBACK);
      if ( outbufsize == sizeof(vol) )
	len = outbufsize;
      else
	len = sizeof(vol[0]);
    } else if ( stream & DSPD_PCM_SBIT_CAPTURE )
    {
      if ( outbufsize == sizeof(vol) )
	{
	  i = DSPD_PCM_STREAM_CAPTURE;
	  len = outbufsize;
	} else
	{
	  len = sizeof(vol[0]);
	  i = 0;
	}
      vol[i] = dspd_dev_get_stream_volume(dev, DSPD_PCM_STREAM_CAPTURE);
    } else
    {
      return dspd_req_reply_err(context, 0, EINVAL);
    }
  return dspd_req_reply_buf(context, 0, vol, len);
}

/*
  Unimplemented commands are sent to the driver.
*/
static int32_t server_filter(struct dspd_rctx *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  int32_t ret = -ENOSYS;
  
  if ( dev->playback.handle && dev->playback.ops )
    {
      if ( dev->playback.ops->ioctl )
	{
	  context->user_data = dev->playback.handle;
	  ret = dev->playback.ops->ioctl(context,
					 req,
					 inbuf,
					 inbufsize,
					 outbuf,
					 outbufsize);
	}
    }
  if ( ret == -ENOSYS && dev->capture.handle && dev->capture.ops )
    {
      if ( dev->capture.ops->ioctl )
	{
	  context->user_data = dev->capture.handle;
	  ret = dev->capture.ops->ioctl(context,
					req,
					inbuf,
					inbufsize,
					outbuf,
					outbufsize);
	}
    }
  context->user_data = dev;
  return ret;
}

static int32_t server_getlatency(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  int32_t client = *(int32_t*)inbuf;
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  uint32_t latency;
  int32_t ret;
  if ( client == -1 )
    {
      latency = dev->current_latency;
    } else
    {
      ret = dspd_dev_get_client_latency(dev, client);
      if ( ret < 0 )
	return dspd_req_reply_err(context, 0, ret);
      latency = ret;
    }
  return dspd_req_reply_buf(context, 0, &latency, sizeof(latency));
}

static int32_t server_stat(struct dspd_rctx *context,
			   uint32_t      req,
			   const void   *inbuf,
			   size_t        inbufsize,
			   void         *outbuf,
			   size_t        outbufsize)
{
  struct dspd_device_stat *stat = outbuf;
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  const struct dspd_drv_params *params;
  memset(stat, 0, sizeof(*stat));
  stat->hotplug_event_id = dev->hotplug_event_id;
  if ( dev->playback.handle )
    {
      params = &dev->playback.params;
      getparams(&dev->playback, &stat->playback);
      stat->playback.stream = DSPD_PCM_SBIT_PLAYBACK;
      stat->streams = DSPD_PCM_SBIT_PLAYBACK;
    }
  if ( dev->capture.handle )
    {
      params = &dev->capture.params;
      getparams(&dev->capture, &stat->capture);
      stat->capture.stream = DSPD_PCM_SBIT_CAPTURE;
      stat->streams |= DSPD_PCM_SBIT_CAPTURE;
    }
  if ( ! (dev->capture.handle || dev->playback.handle) )
    return dspd_req_reply_err(context, 0, EINVAL);

  if ( params->desc )
    strcpy(stat->desc, params->desc);
  if ( params->name )
    strcpy(stat->name, params->name);
  if ( params->bus )
    strcpy(stat->bus, params->bus);
  if ( params->addr )
    strcpy(stat->addr, params->addr);
  stat->error = AO_load(&dev->error);

  stat->slot_id = dspd_slist_id(dev->list, dev->key);

  if ( dspd_dctx.hotplug.default_playback == dev->key )
    stat->flags |= DSPD_DEV_DEFAULT_PLAYBACK;
  else if ( dspd_dctx.hotplug.default_capture == dev->key )
    stat->flags |= DSPD_DEV_DEFAULT_CAPTURE;
  stat->refcount = dspd_slist_refcnt(dspd_dctx.objects, dev->key);

  return dspd_req_reply_buf(context, 0, stat, sizeof(*stat));
}


static int32_t server_reserve(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  int32_t ret;
  uint32_t client = *(uint32_t*)inbuf;
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  dspd_dev_lock(dev);
  ret = dspd_dev_attach_client(dev, client);
  if ( ret == 0 || ret == -EEXIST )
    {
      ret = dspd_dev_set_client_latency(dev, client, INT32_MAX);
      if ( ret > 0 )
	ret = 0;
    }
  if ( ret < 0 )
    ret *= -1;
  dspd_dev_unlock(dev);

  return dspd_req_reply_err(context, 0, ret);
}
static int32_t server_irqinfo(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  struct dspd_mix_irqinfo64 *irq64 = NULL;
  struct dspd_mix_irqinfo *irq = NULL;
  AO_t icount, acount;
  uint32_t i = 0;
  int32_t ret;
  if ( outbufsize == sizeof(struct dspd_mix_irqinfo64) )
    {
      if ( sizeof(AO_t) == 4 )
	return dspd_req_reply_err(context, 0, EINVAL);
      irq64 = outbuf;
    } else
    {
      irq = outbuf;
    }
  DSPD_ASSERT(irq64 || irq);
  do {
    icount = AO_load(&dev->irq_count);
    acount = AO_load(&dev->ack_count);
    i++;
  } while ( AO_load(&dev->irq_count) != icount && 
	    AO_load(&dev->ack_count) != acount &&
	    i < 1000000 );
  if ( irq64 )
    {
      irq64->irq_count = icount;
      irq64->ack_count = acount;
      ret = dspd_req_reply_buf(context, 0, irq64, sizeof(*irq64));
    } else
    {
      irq->irq_count = icount % UINT32_MAX;
      irq->ack_count = acount % UINT32_MAX;
      ret = dspd_req_reply_buf(context, 0, irq, sizeof(*irq));
    }
  return ret;
}



static int32_t server_getchannelmap(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  uint64_t val;
  uint32_t stream;
  uint32_t channels;
  int32_t ret = -EINVAL;
  struct dspd_pcmdev_stream *s = NULL;
  struct dspd_pcm_chmap_container inmap, outmap;
  size_t buflen;
  if ( inbufsize == sizeof(val) )
    val = *(uint64_t*)inbuf;
  else
    val = *(uint32_t*)inbuf;
  stream = val & 0xFFFFFFFF;
  channels = val >> 32U;
  if ( stream == DSPD_PCM_SBIT_PLAYBACK && dev->playback.ops )
    s = &dev->playback;
  else if ( stream == DSPD_PCM_SBIT_CAPTURE && dev->capture.ops )
    s = &dev->capture;
  if ( channels <= DSPD_CHMAP_MAXCHAN && s != NULL && s->ops->get_chmap != NULL )
    {
      memset(&outmap, 0, sizeof(outmap));
      if ( channels == 0 )
	{
	  //Raw device map
	  ret = s->ops->get_chmap(s->handle, &outmap.map);
	} else if ( s->ops->create_chmap )
	{ 
	  //Create channel map with specified number of channels using the driver
	  ret = s->ops->create_chmap(s->handle, channels, &outmap.map);
	} else
	{
	  //Create a generic channel map based on the driver map
	  ret = s->ops->get_chmap(s->handle, &inmap.map);
	  if ( ret == 0 )
	    {
	      outmap.map.count = channels;
	      ret = dspd_pcm_chmap_any(&inmap.map, &outmap.map);
	    }
	}
      buflen = dspd_pcm_chmap_sizeof(outmap.map.count, outmap.map.flags);
      if ( ret == 0 && outbufsize < buflen )
	ret = -ENOBUFS;
      else
	outbufsize = buflen;
    }
  if ( ret == 0 )
    ret = dspd_req_reply_buf(context, 0, &outmap, outbufsize);
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static int32_t server_convert_chmap(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  int32_t ret = -EINVAL;
  const struct dspd_pcm_chmap *in = inbuf;
  struct dspd_pcm_chmap_container out, tmp;
  struct dspd_pcmdev_stream *s = NULL;
  int32_t sbit;
  if ( inbufsize >= dspd_pcm_chmap_sizeof(in->count, in->flags) )
    {
      sbit = in->flags & DSPD_PCM_SBIT_FULLDUPLEX;
      if ( sbit == DSPD_PCM_SBIT_PLAYBACK && dev->playback.ops )
	s = &dev->playback;
      else if ( sbit == DSPD_PCM_SBIT_CAPTURE && dev->capture.ops )
	s = &dev->capture;
      if ( s )
	{
	  memset(&out, 0, sizeof(out));
	  if ( s->ops->translate_chmap )
	    {
	      ret = s->ops->translate_chmap(s->handle, in, &out.map);
	    } else if ( s->ops->get_chmap )
	    {
	      ret = s->ops->get_chmap(s->handle, &tmp.map);
	      if ( ret == 0 )
		{
		  ret = dspd_pcm_chmap_test(in, &tmp.map);
		  if ( ret == 0 )
		    {
		      out.map.flags = DSPD_CHMAP_SIMPLE;
		      if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
			{
			  out.map.ichan = in->count;
			  out.map.ochan = tmp.map.count;
			  out.map.count = out.map.ichan;
			} else
			{
			  out.map.ichan = tmp.map.count;
			  out.map.ochan = in->count;
			  out.map.count = in->count;
			}
		    } else
		    {
		      if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
			ret = dspd_pcm_chmap_translate(in, &tmp.map, &out.map);
		      else
			ret = dspd_pcm_chmap_translate(&tmp.map, in, &out.map);
		    }
		}
	    }
	}
    }
  if ( ret == 0 )
    ret = dspd_req_reply_buf(context, 0, &out, dspd_pcm_chmap_sizeof(out.map.count, out.map.flags));
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}



static int32_t server_lock(struct dspd_rctx *context,
			   uint32_t      req,
			   const void   *inbuf,
			   size_t        inbufsize,
			   void         *outbuf,
			   size_t        outbufsize)

{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  const struct dspd_dev_lock_req *lr = inbuf;
  struct dspd_dev_lock_req result = { 0 };
  int32_t ret = EBUSY;
  uint32_t config;
  uint64_t cookie;
  if ( dev->mq[0] < 0 )
    return dspd_req_reply_err(context, 0, EOPNOTSUPP);
  dspd_dev_lock(dev);
  if ( dev->excl_client < 0 )
    {
      ret = EINVAL;
      if ( (lr->flags & ~(DSPD_DEV_LOCK_EXCL|DSPD_DEV_LOCK_LATENCY)) )
	goto out;
      if ( lr->client >= DSPD_MAX_OBJECTS ||
	   lr->cookie != 0 )
	goto out;
      if ( ! dspd_dev_get_client_attach(dev, lr->client) )
	{
	  ret = ESRCH;
	  goto out;
	}
      config = AO_load(&dev->reg.config);
      if ( (lr->flags & DSPD_DEV_LOCK_EXCL) && 
	   (dspd_dev_config_get_stream_count(dev, &config, DSPD_PCM_STREAM_PLAYBACK) ||
	    dspd_dev_config_get_stream_count(dev, &config, DSPD_PCM_STREAM_CAPTURE)) )
	{
	  ret = EBUSY;
	  goto out;
	}
      if ( ! dev->seed )
	dev->seed = dspd_get_time() % UINT32_MAX;

      cookie = rand_r(&dev->seed);
      cookie <<= 32U;
      cookie |= rand_r(&dev->seed);

#ifdef DSPD_HAVE_ATOMIC_INT64
      dspd_store_uint64(&dev->cookie, cookie);
#else
      dspd_mutex_lock(&dev->cookie_lock);
      dev->cookie = cookie;
      dspd_mutex_unlock(&dev->cookie_lock);
#endif
      dev->excl_client = lr->client;
      dev->access_flags = lr->flags;
      ret = 0;

      /*
	The idea is that the client can request immediate attention from the server through client_fd.
	The internal client could eventually raise POLLIN events on another thread if the buffer is drained
	during a callback.
	
	The required buffer size may be reduced since the client no longer needs extra buffer space to compensate
	for incorrect wakeup times.  It may also be possible to eliminate most if not all spurious wakeups on
	the client side.

      */
      result.server_fd = dev->mq[0];
      result.client_fd = dev->mq[1];
      result.client = lr->client;
      result.flags = dev->access_flags;
      result.cookie = cookie;
    }
 out:
  dspd_dev_unlock(dev);
  if ( ret != 0 )
    ret = dspd_req_reply_err(context, 0, ret);
  else
    ret = dspd_req_reply_buf(context, 0, &result, sizeof(result));
  return ret;
}

static int32_t server_remove(struct dspd_rctx *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
  
{
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  AO_store(&dev->error, ENODEV);
  dspd_sched_trigger(dev->sched);
  return dspd_req_reply_err(context, 0, 0);
}

static const struct dspd_req_handler device_req_handlers[] = {
  [SRVIDX(DSPD_SCTL_SERVER_MIN)] = {
    .handler = server_filter,
    //All other params are ignored.
  },
  [SRVIDX(DSPD_SCTL_SERVER_CONNECT)] = {
    .handler = server_connect,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [SRVIDX(DSPD_SCTL_SERVER_DISCONNECT)] = {
    .handler = server_disconnect,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [SRVIDX(DSPD_SCTL_SERVER_SETTRIGGER)] = {
    .handler = server_settrigger,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(int64_t), //(client<<32)|bits
    .outbufsize = 0,
  },
  [SRVIDX(DSPD_SCTL_SERVER_SETLATENCY)] = {
    .handler = server_setlatency,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(int64_t), //(client<<32)|latency
    .outbufsize = sizeof(int32_t),
  },
  [SRVIDX(DSPD_SCTL_SERVER_GETPARAMS)] = {
    .handler = server_getparams,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_cli_params),
  },
  [SRVIDX(DSPD_SCTL_SERVER_SETVOLUME)] = {
    .handler = server_setvolume,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_stream_volume),
    .outbufsize = 0,
  },
  [SRVIDX(DSPD_SCTL_SERVER_GETVOLUME)] = {
    .handler = server_getvolume,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(float),
  },
  [SRVIDX(DSPD_SCTL_SERVER_GETLATENCY)] = {
    .handler = server_getlatency,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(uint32_t),
  },
  [SRVIDX(DSPD_SCTL_SERVER_STAT)] = {
    .handler = server_stat,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(struct dspd_device_stat),
  },
  [SRVIDX(DSPD_SCTL_SERVER_RESERVE)] = {
    .handler = server_reserve,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(uint32_t),
    .outbufsize = 0,
  },
  [SRVIDX(DSPD_SCTL_SERVER_IRQINFO)] = {
    .handler = server_irqinfo,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(struct dspd_mix_irqinfo),
  },
  [SRVIDX(DSPD_SCTL_SERVER_GETCHANNELMAP)] = {
    .handler = server_getchannelmap,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_chmap),
  },
  [SRVIDX(DSPD_SCTL_SERVER_CONVERT_CHMAP)] = {
    .handler = server_convert_chmap,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_chmap),
    .outbufsize = sizeof(struct dspd_chmap),
  },
  [SRVIDX(DSPD_SCTL_SERVER_LOCK)] = {
    .handler = server_lock,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_dev_lock_req),
    .outbufsize = sizeof(struct dspd_dev_lock_req)
  },
  [SRVIDX(DSPD_SCTL_SERVER_REMOVE)] = {
    .handler = server_remove,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0
  },
  
};

static int32_t device_ctl(struct dspd_rctx *rctx,
			  uint32_t             req,
			  const void          *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize)
{
  uint64_t r;
  r = req;
  r <<= 32;
  r |= req - DSPD_SCTL_SERVER_MIN;
  return dspd_daemon_dispatch_ctl(rctx,
				  device_req_handlers,
				  sizeof(device_req_handlers) / sizeof(device_req_handlers[0]),
				  r,
				  inbuf,
				  inbufsize,
				  outbuf,
				  outbufsize);
}
