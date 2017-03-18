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


#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sched.h>
#define _DSPD_CTL_MACROS
#include "sslib.h"
#include "daemon.h"

#define DSPD_DEV_USE_TLS
static int stream_recover_fcn(struct dspd_pcmdev_stream *stream);
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
static bool schedule_capture_sleep(void *data, uint64_t *abstime, int32_t *reltime);
static bool schedule_playback_sleep(void *data, uint64_t *abstime, int32_t *reltime);
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
int32_t dspd_dev_get_slot(void *dev)
{
  struct dspd_pcm_device *d = dev;
  return d->key;
}

static int stream_recover_print(struct dspd_pcmdev_stream *s, const char *fcn, int line)
{
  // fprintf(stderr, "RECOVERY ON %s:%d\n", fcn, line);
  if ( dspd_dctx.debug )
    fprintf(stderr, "RECOVERY ON %s:%d\n", fcn, line);
  return stream_recover_fcn(s);
}

#define stream_recover(_s) stream_recover_print(_s, __FUNCTION__, __LINE__)


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

  // assert(latency != 32);
  //fprintf(stderr, "SETL %d FROM %d\n", i, latency);

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
  if ( client < DSPD_MAX_OBJECTS )
    {
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
	    }
	  count = dspd_dev_config_get_stream_count(dev, &config, DSPD_PCM_STREAM_PLAYBACK);
	  if ( count > 0 && dev->playback.streams == 0 )
	    {
	      if ( dev->playback.stop_threshold == 0 )
		{
		  if ( stream_prepare(&dev->playback) != 0 )
		    {
		      if ( stream_recover(&dev->playback) < 0 )
			dspd_scheduler_abort(dev->sched);
		      else
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
		   }
	       } else
	       {
		 dev->capture.latency = latency;
		 dev->capture.ops->set_latency(dev->capture.handle, latency, latency);
	       }
	   } 
	  count = dspd_dev_config_get_stream_count(dev, &config, DSPD_PCM_STREAM_CAPTURE);
	  if ( count > 0 && dev->capture.streams == 0 )
	    {
	      if ( dev->capture.stop_threshold == 0 )
		{
		  if ( stream_prepare(&dev->capture) != 0 )
		    {
		      if ( stream_recover(&dev->capture) != 0 )
			dspd_scheduler_abort(dev->sched);
		      else
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
   
      ret = dev->playback.ops->status(dev->playback.handle, &dev->playback.status);
      if ( ret < 0 )
	{
	  dev->playback.started = 0;

	  dev->playback.status = NULL;
	  ret = stream_recover(&dev->playback);
	  if ( ret < 0 )
	    dspd_scheduler_abort(dev->sched);
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
      ret = dev->capture.ops->status(dev->capture.handle, &dev->capture.status);
      if ( ret < 0 )
	{

	  dev->capture.status = NULL;
	  dev->capture.started = 0;
	  ret = stream_recover(&dev->capture);
	  if ( ret < 0 )
	    dspd_scheduler_abort(dev->sched);
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
//is bad for multitasking.  Who cares about power if the whole damn
//system except for audio stutters?
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

static bool schedule_playback_sleep(void *data, uint64_t *abstime, int32_t *reltime)
{
  struct dspd_pcm_device *dev = data;
  uint64_t f;
  uint64_t sleep_frames;
  if ( ! dev->capture.ops )
    dspd_sync_reg(dev);
  if ( dev->playback.latency_changed )
    {
      dev->playback.latency_changed = 0;
      dev->playback.check_status = 1;
      *reltime = DSPD_SCHED_SPIN;
      *abstime = UINT64_MAX;
    } else if ( dev->playback.status )
    {
      /*
	Keep checking the status for a while.  ALSA device startup isn't always perfectly accurate.
	This will use some extra CPU for a short time until the device is really synchronized with the
	system clock.
      */
      if ( dev->playback.status->appl_ptr < dev->playback.latency || dev->playback.check_status != 0 )
	dev->playback.ops->status(dev->playback.handle, &dev->playback.status);
    

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

      *abstime = dev->playback.status->tstamp + get_sleep_reltime(f, dev->playback.check_status);
      dev->playback.next_wakeup = *abstime;
      *reltime = DSPD_SCHED_WAIT;
      dev->playback.check_status = 0;

    } else if ( dev->playback.running )
    {
      *reltime = DSPD_SCHED_SPIN;
      *abstime = UINT64_MAX;
    } else
    {
      *reltime = DSPD_SCHED_STOP;
      *abstime = UINT64_MAX;
    }
  return true;
}

static bool process_rewound_playback(struct dspd_pcm_device *dev,
				     void *client,
				     const struct dspd_pcmcli_ops *ops,
				     uint64_t *pointer,
				     uint32_t  frames,
				     uint32_t  rw)
{
  uintptr_t offset = 0, l, o;
  int ret;
  double *buf;
  uintptr_t len;
  bool result = true;
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
      ops->playback_xfer(dev,
			 client,
			 &buf[o * dev->playback.params.channels],
			 len,
			 dev->playback.cycle.start_count,
			 dev->playback.status);
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
				    const struct dspd_pcmcli_ops *ops)
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
      //TODO: Make the inout args a struct and allow the client to pass a timestamp.  The timestamp can be
      //used to schedule the stream to run at a later time.
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
	      assert(dev->playback.status->appl_ptr <= orig_ptr);
	   
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
  assert(dev->playback.status->appl_ptr == orig_ptr);
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
				 dev->playback.cycle.start_count,
				 dev->playback.status);
	      
	      if ( f > 0 )
		if ( dev->playback.ops->adjust_pointer(dev->playback.handle, f * -1) == 0 )
		  goto out;
	    }
	}
    }

  
 
  assert(dev->playback.status->appl_ptr == orig_ptr);
  return true;

 out:
 
  stream_recover(&dev->playback);
  return false;

}

static bool process_rewound_capture(struct dspd_pcm_device *dev,
				    void *client,
				    const struct dspd_pcmcli_ops *ops,
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
				   const struct dspd_pcmcli_ops *ops)
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
	  offset = adj + dev->capture.cycle.offset;
	  if ( max_xfer > len )
	    max_xfer = len;
	  ops->capture_xfer(dev,
			    client,
			    &ptr[offset*dev->capture.params.channels],
			    max_xfer,
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
		    dev->capture.status);
  return;

 err:
  stream_recover(&dev->capture);
}


/*static void process_client_capture(struct dspd_pcm_device *dev,
				   void *client,
				   const struct dspd_pcmcli_ops *ops)
{
  uint32_t latency;
  uint64_t pointer, start_count, max_ptr;
  float *ptr;
  intptr_t rw, diff, max_xfer, ret, xfer;
  bool starting;
  size_t offset;
  intptr_t adj;

  process_client_capture_new(dev, client, ops); return;


  start_count = dev->capture.cycle.start_count;
  pointer = dev->capture.status->appl_ptr;
  latency = dev->capture.latency;
  if ( ops->get_capture_status(dev,
			       client,
			       &pointer,
			       &start_count,
			       &latency,
			       dev->capture.status) != -EAGAIN )
    {
      starting = start_count != dev->capture.cycle.start_count;
      max_xfer = latency * 2;
      max_ptr = dev->capture.cycle.len + dev->capture.status->appl_ptr;
      if ( pointer < dev->capture.status->appl_ptr )
	{

	  rw = dev->capture.ops->rewindable(dev->capture.handle);
	  if ( rw < 0 )
	    goto err;
	  diff = dev->capture.status->appl_ptr - pointer;
	  
	  if ( rw > diff )
	    rw = diff;
	  ret = dev->capture.ops->rewind(dev->capture.handle, rw);
	  if ( ret < 0 )
	    goto err;
	  if ( ret > 0 )
	    {
	      if ( ret > max_xfer )
		xfer = max_xfer;
	      else
		xfer = ret;
	      pointer = dev->capture.status->appl_ptr;
	      if ( ! process_rewound_capture(dev,
					     client,
					     ops,
					     &pointer,
					     xfer) )
		goto err;
	      max_xfer -= xfer;
	      if ( xfer < ret )
		{
		  if ( dev->capture.ops->forward(dev->capture.handle, ret - xfer) < 0 )
		    goto err;
		}
	    }
	} else if ( pointer >= max_ptr )
	{
	  //No new data for this client
	  return;
	}
      if ( max_xfer )
	{

	  ptr = dev->capture.cycle.addr;
	  offset = dev->capture.cycle.offset;
	  if ( dev->capture.cycle.len < max_xfer )
	    {
	      max_xfer = dev->capture.cycle.len;
	    } else if ( dev->capture.cycle.len > max_xfer && starting != 0 )
	    {
	      //Adjust the offset to start at the correct position.
	      offset = dev->capture.cycle.len - max_xfer;
	    }
	  //Adjust in case the fragment was partial.  This is a very unlikely "retry" situation.
	  //The last time it put some data into the buffer from the current application pointer
	  //but it did not take all of the data.  Either the buffer was full or it appeared to
	  //be too much when the latency was taken into consideration.
	  if ( pointer > dev->capture.status->appl_ptr )
	    {
	      diff = pointer - dev->capture.status->appl_ptr;
	      if ( diff < max_xfer )
		{
		  max_xfer -= diff;
		  offset += diff;
		  adj = dev->capture.ops->adjust_pointer(dev->capture.handle, diff * -1UL);
		} else
		{
		  max_xfer = 0;
		  adj = 0;
		}
	    } else
	    {
	      adj = 0;
	    }
	  if ( max_xfer )
	    {
	      ops->capture_xfer(dev,
				client,
				&ptr[offset*dev->capture.params.channels],
				max_xfer,
				dev->capture.status);
	      if ( adj )
		if ( dev->capture.ops->adjust_pointer(dev->capture.handle, adj) == 0 )
		  goto err;
	    }
	}
    }
  return;

 err:
  stream_recover(&dev->capture);
  }*/

static void _alert_one_client(struct dspd_pcm_device *dev, int32_t client, int32_t error)
{
  struct dspd_pcmcli_ops *cli_ops;
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

	  if ( dspd_client_srv_lock(dev->list, client, dev->key) ) //Might sleep
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
	      dspd_client_srv_unlock(dev->list, client);
	    }
	  dspd_daemon_unref(client); //Might sleep and free resources
	}
    }
}	

static void alert_one_client(struct dspd_pcm_device *dev, int32_t client, int32_t error)
{
  if ( error == EFAULT )
    {
      //Can't double fault.  Allowing this increases the chances and severity of glitches
      _alert_one_client(dev, client, error);
    } else
    {
      dev->current_exception = error;
      if ( sigsetjmp(dev->sbh_except, SIGBUS) == 1 )
	_alert_one_client(dev, client, -1);
      else
	_alert_one_client(dev, client, error);
      dev->current_exception = 0;
    }
}
		       
static void alert_all_clients(struct dspd_pcm_device *dev, int32_t error)
{
  int i;
  dev->current_exception = error;
  if ( sigsetjmp(dev->sbh_except, SIGBUS) == 1 )
    {
      i = dev->exc_client;
      _alert_one_client(dev, i, -1);
      i++;
    } else
    {
      i = 0;
    }

  while ( i < DSPD_MAX_OBJECTS )
    {
      dev->exc_client = i;
      alert_one_client(dev, i, error);
      i++;
    }
  dev->exc_client = 0;
  dev->current_exception = 0;
}

static bool process_clients_once(struct dspd_pcm_device *dev, uint32_t ops)
{
  //Process all clients.  Must lock and unlock as they
  //are processed.  Later I will implement lock optimization.

  int32_t maxidx, i, c;
  bool playback = false, capture = false, pr, cr, err = 0;
  uint8_t tm;
  struct dspd_pcmcli_ops *cli_ops;
  struct dspd_pcmsrv_ops *srv_ops;
  void *cli;

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

  maxidx *= 2;

  for ( i = 0; i < maxidx; i += 2 )
    {
      tm = get_trigger_mask((uint8_t*)dev->reg.client_mask, i);
      pr = playback && (tm & DSPD_PCM_SBIT_PLAYBACK);
      cr = capture && (tm & DSPD_PCM_SBIT_CAPTURE);

     

      if ( pr || cr )
	{
	 
	  c = i / 2;
	  if ( dspd_client_srv_trylock(dev->list, c, dev->key) )
	    {
	      dev->current_client = c;
	      //Try again with the lock
	      tm = get_trigger_mask((uint8_t*)dev->reg.client_mask, i);
	      pr = playback && (tm & DSPD_PCM_SBIT_PLAYBACK);
	      cr = capture && (tm & DSPD_PCM_SBIT_CAPTURE);
	      dspd_slist_entry_get_pointers(dev->list,
					    c,
					    &cli,
					    (void**)&srv_ops,
					    (void**)&cli_ops);
	    
	      if ( pr )
		{
	
		  err = ! process_client_playback(dev, cli, cli_ops);
		}
	      

	      if ( cr )
		process_client_capture(dev, cli, cli_ops);
	      dev->current_client = -1;
	      dspd_client_srv_unlock(dev->list, c);
	      if ( err )
		break;
	    }
	}
    }

  return err;
}

static int stream_prepare(struct dspd_pcmdev_stream *stream)
{
  stream->cycle.start_count++;
  if ( stream->cycle.start_count == 0 )
    stream->cycle.start_count++;
  
  

  return stream->ops->prepare(stream->handle);
}



static int stream_recover_fcn(struct dspd_pcmdev_stream *stream)
{
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

  ret = stream->ops->recover(stream->handle);

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
	      if ( ! dev->playback.started )
		{
		  if ( (err = dev->playback.ops->start(dev->playback.handle)) < 0 )
		    {
		      playback_error = true;
		      break;
		    } else
		    {
		      dev->playback.started = 1;
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
	  dspd_scheduler_abort(dev->sched);
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
  uintptr_t len, count, i, l, written = 0, total;
  uint16_t revents;
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
	      dspd_scheduler_abort(dev->sched);
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
	  ret = dev->playback.ops->status(dev->playback.handle, &dev->playback.status);
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
	  process_clients_once(dev, EPOLLOUT);
	} 

      if ( dev->playback.status->space > 0 )
	{
	  dev->playback.early_cycle = UINT64_MAX;
	  count = get_io_cycle_count(dev->playback.status->fill, dev->playback.status->space);
	  if ( count == 1 )
	    {
	      //Write it all
	      len = dev->playback.status->space;
	    } else
	    {
	      //Start with the amount of data that is in the buffer.
	      //This keeps it from underrunning when latency changes
	      //from very low to very high and when a deadline is missed
	      //but it did not quite underrun.
	      len = dev->playback.status->fill;
	      assert(len <= dev->playback.status->space);
	    }
	  
	  if ( dev->playback.glitch && dev->playback.status && 
	       dev->playback.requested_latency < dev->playback.glitch_threshold )
	    {	      
	      l = dev->playback.requested_latency / 4;
	      //if ( len > l && (len / l) > 0 )
		{
		  total = dev->playback.status->space;
		  len = l;
		  if ( len > total )
		    len = total;
		  if ( len > dev->playback.status->fill && dev->playback.status->fill > 0 )
		    len = dev->playback.status->fill;
	
		  while ( written < total )
		    {
		     
		      if ( ! device_playback_cycle(dev, len) )
			break;
		      if ( ! dev->playback.status )
			break;
		      written += len;
		      len *= 2;
		      if ( len > dev->playback.status->space )
			len = dev->playback.status->space;
		      if ( len > dev->playback.requested_latency )
			len = dev->playback.requested_latency;
		     

		      if ( len > 0 && written > dev->playback.requested_latency )
			{
			  if ( dev->playback.ops->status(dev->playback.handle, &dev->playback.status) < 0 )
			    goto out;
			} else if ( len == 0 )
			{
			  break;
			}
		    }
		}
		
	    } else
	    {
	      for ( i = 0; i < count; i++ )
		{
		  written += len;
		  if ( ! device_playback_cycle(dev, len) )
		    break;
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
	  if ( dev->playback.stop_threshold > 0 && 
	       dev->playback.stop_count >= dev->playback.stop_threshold &&
	       dev->playback.started != 0 )
	    {
	      if ( stream_drop(&dev->playback) < 0 )
		{
		  dspd_scheduler_abort(dev->sched);
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
    dspd_scheduler_abort(dev->playback.handle);
  return;
}

static bool schedule_capture_sleep(void *data, uint64_t *abstime, int32_t *reltime)
{
  struct dspd_pcm_device *dev = data;
  uint64_t f;
  bool ret = true;

  if ( ! dev->playback.ops )
    dspd_sync_reg(dev);
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
	
      if ( dev->capture.status->fill > 0 )
	{
	  *abstime = dev->capture.status->tstamp;
	  *reltime = DSPD_SCHED_SPIN;
	  ret = false;
	} else
	{
	  f = dev->capture.latency * (1000000000 / dev->capture.params.rate);
	  *abstime = dev->capture.status->tstamp + (f / 2);
	  *reltime = DSPD_SCHED_WAIT;
	}
    } else if ( dev->capture.running )
    {
      *reltime = DSPD_SCHED_SPIN;
      *abstime = UINT64_MAX;
    } else
    {
      *reltime = DSPD_SCHED_STOP;
      *abstime = UINT64_MAX;
    }
 
  return ret;
}
static void schedule_capture_wake(void *data)
{
  struct dspd_pcm_device *dev = data;
  int32_t ret;
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
		    dspd_scheduler_abort(dev->sched);
		  return;
		} else
		{
		  dev->capture.started = true;
		}
	    }
	  ret = dev->capture.ops->status(dev->capture.handle, &dev->capture.status);
	  if ( ret < 0 )
	    {
	      if ( stream_recover(&dev->capture) < 0 )
		dspd_scheduler_abort(dev->sched);
	      return;
	    }
	}
      if ( dev->capture.status->fill > dev->capture.latency )
	dev->capture.cycle.len = dev->capture.status->fill;
      else
	dev->capture.cycle.len = dev->capture.latency;

      ret = dev->capture.ops->mmap_begin(dev->capture.handle,
					 (void**)&dev->capture.cycle.addr,
					 &dev->capture.cycle.offset,
					 &dev->capture.cycle.len);
      if ( ret < 0 )
	{
	  if ( ret != -EAGAIN )
	    {
	      if ( stream_recover(&dev->capture) < 0 )
		dspd_scheduler_abort(dev->sched);
	    }
	  return;
	}


      process_clients_once(dev, POLLIN);

      ret = dev->capture.ops->mmap_commit(dev->capture.handle,
					  dev->capture.cycle.offset,
					  dev->capture.cycle.len);
     
      if ( ret < 0 )
	{
	  if ( stream_recover(&dev->capture) < 0 )
	    dspd_scheduler_abort(dev->sched);
	} else 
	{
	  if ( dev->capture.stop_threshold > 0 )
	    dev->capture.stop_count += dev->capture.cycle.len;
	  if ( dev->capture.stop_threshold > 0 &&
	       dev->capture.stop_count >= dev->capture.stop_threshold &&
	       dev->capture.started != 0 )
	    {
	      if ( stream_drop(&dev->capture) < 0 )
		dspd_scheduler_abort(dev->sched);
	    }
	}
    }
  return;
}



#ifndef DSPD_DEV_USE_TLS
/*
  This one works if SYS_gettid is signal safe.  It should work
  because the mutex is only locked when writing to the list and
  that happens in a context where a SIGBUS should be fatal.

  The slot is only freed from inside the thread that created it so
  no two slots will have the same thread id.

*/
struct dev_tls_slot {
  int   thread;
  void *context;
};
//There only needs to be enough slots to have 1 for each object.
static pthread_mutex_t dev_tls_mutex = PTHREAD_MUTEX_INITIALIZER;
struct dev_tls_slot devtls[DSPD_MAX_OBJECTS];

static void dspd_pcm_device_register_tls(void *current_device)
{
  int tid = syscall(SYS_gettid);
  size_t i;
  pthread_mutex_lock(&dev_tls_mutex);
  for ( i = 0; i < DSPD_MAX_OBJECTS; i++ )
    {
      if ( devtls[i].thread == 0 )
	{
	  devtls[i].thread = tid;
	  devtls[i].context = current_device;
	  break;
	}
    }
  pthread_mutex_unlock(&dev_tls_mutex);
}
static void dspd_pcm_device_unregister_tls(void)
{
  int tid = syscall(SYS_gettid);
  size_t i;
  pthread_mutex_lock(&dev_tls_mutex);
  for ( i = 0; i < DSPD_MAX_OBJECTS; i++ )
    {
      if ( devtls[i].thread == tid )
	{
	  devtls[i].thread = 0;
	  devtls[i].context = NULL;
	  break;
	}
    }
  pthread_mutex_unlock(&dev_tls_mutex);
}
static void *dspd_pcm_device_get_context(void)
{
  int tid = syscall(SYS_gettid);
  size_t i;
  void *ret = NULL;
  for ( i = 0; i < DSPD_MAX_OBJECTS; i++ )
    {
      if ( devtls[i].thread == tid )
	{
	  ret = devtls[i].context;
	  break;
	}
    }
  return ret;
}
#else
/*
  This one works if TLS is at least partially signal safe.  It must be safe
  to read a variable from a signal handler but writing to the variable (actually  just initializing it) does not have to be thread safe.
*/
static __thread void *devtls;
static void dspd_pcm_device_register_tls(void *current_device)
{
  devtls = current_device;
}
static void dspd_pcm_device_unregister_tls(void)
{
  devtls = NULL;
}
static void *dspd_pcm_device_get_context(void)
{
  return devtls;
}
#endif


  
static void sigbus_handler(int sig, siginfo_t *signinfo, void *context)
{
  struct dspd_pcm_device *dev = dspd_pcm_device_get_context();
  if ( dev->current_exception )
    siglongjmp(dev->sbh_except, 1);

  assert(dev);
  assert(dev->current_client >= 0);
  siglongjmp(dev->sbh_env, 1);
}
static void *dspd_dev_thread(void *arg)
{
  struct dspd_scheduler *sched = arg;
  struct sigaction act;

  struct dspd_pcm_device *dev = sched->udata;
  char name[32];
  sprintf(name, "dspd-io-%d", dev->key);
  prctl(PR_SET_NAME, name, 0, 0, 0);

  dspd_pcm_device_register_tls(dev);
  memset(&act, 0, sizeof(act));
  act.sa_sigaction = sigbus_handler;
  act.sa_flags = SA_SIGINFO;
  sigaction(SIGBUS, &act, NULL);
  
  if ( sigsetjmp(dev->sbh_env, SIGBUS) == 1 )
    {
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
    }
  

  dspd_scheduler_run(arg);

  //Drop realtime priority
  struct sched_param param;
  memset(&param, 0, sizeof(param));
  param.sched_priority = 0;
  pthread_setschedparam(dev->iothread.thread, SCHED_OTHER, &param);
  
  AO_store(&dev->error, ENODEV);
  alert_all_clients(dev, ENODEV);

  dspd_pcm_device_unregister_tls();
  dspd_daemon_unref(dev->key);
  return NULL;
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

static bool schedule_fullduplex_sleep(void *data, uint64_t *abstime, int32_t *reltime)
{
  bool ret = 0;
  uint64_t p_abstime, c_abstime; int32_t p_reltime, c_reltime;
  struct dspd_pcm_device *dev = data;

  dspd_sync_reg(dev);
  ret |= schedule_playback_sleep(data, &p_abstime, &p_reltime);
  ret |= schedule_capture_sleep(data, &c_abstime, &c_reltime);
  /*
    Relative timeouts aren't actually used right now, so only SPIN, WAIT, and STOP
    must be handled.
    
    Only handle SPIN if a stream should be running (streams are connected or they were recently connecting
    and it is waiting to shut off).
  */
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
    }
  return ret;
}

static const struct dspd_scheduler_ops playback_ops = {
  .wake = schedule_playback_wake,
  .sleep = schedule_playback_sleep,
  .timer_event = schedule_timer_event,
  .trigger_event = schedule_trigger_event,
};

static const struct dspd_scheduler_ops capture_ops = {
  .wake = schedule_capture_wake,
  .sleep = schedule_capture_sleep,
  .timer_event = schedule_timer_event,
  .trigger_event = schedule_trigger_event,
};

static const struct dspd_scheduler_ops fullduplex_ops = {
  .wake = schedule_fullduplex_wake,
  .sleep = schedule_fullduplex_sleep,
  .timer_event = schedule_timer_event,
  .trigger_event = schedule_trigger_event,
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




   
int32_t dspd_pcm_device_new(void **dev,
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
  devptr = calloc(1, sizeof(struct dspd_pcm_device));
  if ( ! devptr )
    return -errno;
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
  
  s = params->stream & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);
  if ( s == DSPD_PCM_SBIT_PLAYBACK )
    {
      devptr->sched = dspd_scheduler_new(&playback_ops, devptr, devptr->playback.nfds);
    } else if ( s == DSPD_PCM_SBIT_CAPTURE )
    {
      devptr->sched = dspd_scheduler_new(&capture_ops, devptr, devptr->capture.nfds);
    } else
    {
      devptr->sched = dspd_scheduler_new(&fullduplex_ops, devptr, devptr->playback.nfds+devptr->capture.nfds);
    }
    
  if ( ! devptr->sched )
    goto out;
  


  ret = dspd_daemon_threadattr_init(&attr, sizeof(attr), DSPD_THREADATTR_DETACHED | DSPD_THREADATTR_RTIO);
  if ( ret != 0 )
    {
      ret *= -1;
      goto out;
    }
  
  devptr->arg = params->arg;
  devptr->current_client = -1;
  ret = dspd_thread_create(&devptr->iothread,
			   &attr,
			   dspd_dev_thread,
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
				     dspd_dev_thread,
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
  assert(dev);
  dspd_scheduler_abort(dev->sched);
  dspd_sched_trigger(dev->sched);
  dspd_mutex_destroy(&dev->reg_lock);
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
    dspd_scheduler_delete(dev->sched);
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
  //fprintf(stderr, "CPARAMS %d %d\n", cparams->rate, cparams->latency);
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
      //fprintf(stderr, "CHECK\n");
      latency = cparams.latency;
      if ( stream_compatible(&cparams, &dev->playback.params) )
	sbits |= DSPD_PCM_SBIT_PLAYBACK;
      else
	return dspd_req_reply_err(context, 0, EINVAL);
      //   fprintf(stderr, "OK\n");
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
  dspd_dev_unlock(dev);
  if ( ret == 0 )
    {
      dspd_dev_client_configure(dev, client);
      dspd_slist_unref(dev->list, client);
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
  if ( sv->stream & DSPD_PCM_SBIT_PLAYBACK )
    dspd_dev_set_stream_volume(dev, DSPD_PCM_STREAM_PLAYBACK, sv->volume);
  if ( sv->stream & DSPD_PCM_SBIT_CAPTURE )
    dspd_dev_set_stream_volume(dev, DSPD_PCM_STREAM_CAPTURE, sv->volume);
  return dspd_req_reply_err(context, 0, 0);
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
  size_t len = 0;
  float vol[2];
  if ( (stream & DSPD_PCM_SBIT_PLAYBACK) && (stream & DSPD_PCM_SBIT_CAPTURE) )
    {
      vol[DSPD_PCM_STREAM_PLAYBACK] = dspd_dev_get_stream_volume(dev, DSPD_PCM_STREAM_PLAYBACK);
      vol[DSPD_PCM_STREAM_CAPTURE] = dspd_dev_get_stream_volume(dev, DSPD_PCM_STREAM_CAPTURE);
      len = sizeof(vol);
    } else if ( stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      vol[0] = dspd_dev_get_stream_volume(dev, DSPD_PCM_STREAM_PLAYBACK);
      len = sizeof(vol[0]);
    } else
    {
      vol[0] = dspd_dev_get_stream_volume(dev, DSPD_PCM_STREAM_CAPTURE);
      len = sizeof(vol[0]);
    }
  return dspd_req_reply_buf(context, 0, vol, len);
}

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
  ret = dspd_dev_attach_client(dev, client);
  if ( ret == 0 || ret == -EEXIST )
    {
      ret = dspd_dev_set_client_latency(dev, client, INT32_MAX);
      if ( ret > 0 )
	ret = 0;
    }
  if ( ret < 0 )
    ret *= -1;
    
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
  assert(irq64 || irq);
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

static int32_t create_channelmap_generic(const struct dspd_fchmap *smap, struct dspd_fchmap *cmap, int32_t channels)
{
  int32_t ret = EINVAL, i;
  struct dspd_fchmap tmp;
  if ( channels == 1 || (channels == 2 && channels > smap->map.channels) )
    {
      memset(cmap, 0, sizeof(*cmap));
      cmap->map.channels = channels;
      ret = dspd_chmap_create_generic((const struct dspd_chmap*)smap, (struct dspd_chmap*)cmap);
      ret *= -1;
    } else if ( channels <= smap->map.channels )
    {
      memset(&tmp, 0, sizeof(tmp));
      tmp.map.channels = channels;
      switch(channels)
	{
	case 2:
	  tmp.map.pos[0] = DSPD_CHMAP_FL;
	  tmp.map.pos[1] = DSPD_CHMAP_FR;
	  break;
	case 3:
	  tmp.map.pos[0] = DSPD_CHMAP_FL;
	  tmp.map.pos[1] = DSPD_CHMAP_FR;
	  if ( dspd_chmap_index((struct dspd_chmap*)smap, DSPD_CHMAP_FC) >= 0 )
	    tmp.map.pos[2] = DSPD_CHMAP_FC;
	  else
	    tmp.map.pos[2] = DSPD_CHMAP_LFE;
	  break;
	case 4:
	  tmp.map.pos[0] = DSPD_CHMAP_FL;
	  tmp.map.pos[1] = DSPD_CHMAP_FR;
	  if ( dspd_chmap_index((struct dspd_chmap*)smap, DSPD_CHMAP_RL) >= 0 &&
	       dspd_chmap_index((struct dspd_chmap*)smap, DSPD_CHMAP_RR) >= 0 )
	    {
	      tmp.map.pos[2] = DSPD_CHMAP_RL;
	      tmp.map.pos[3] = DSPD_CHMAP_RR;
	    } else
	    {
	      tmp.map.pos[2] = DSPD_CHMAP_FC;
	      tmp.map.pos[3] = DSPD_CHMAP_LFE;
	    }
	  break;
	case 5:
	  tmp.map.pos[0] = DSPD_CHMAP_FL;
	  tmp.map.pos[1] = DSPD_CHMAP_FR;
	  tmp.map.pos[2] = DSPD_CHMAP_RL;
	  tmp.map.pos[3] = DSPD_CHMAP_RR;
	  if ( dspd_chmap_index((struct dspd_chmap*)smap, DSPD_CHMAP_LFE) >= 0 )
	    tmp.map.pos[4] = DSPD_CHMAP_LFE;
	  else
	    tmp.map.pos[4] = DSPD_CHMAP_FC;
	  break;
	case 6:
	  tmp.map.pos[0] = DSPD_CHMAP_FL;
	  tmp.map.pos[1] = DSPD_CHMAP_FR;
	  tmp.map.pos[2] = DSPD_CHMAP_RL;
	  tmp.map.pos[3] = DSPD_CHMAP_RR;
	  tmp.map.pos[4] = DSPD_CHMAP_FC;
	  tmp.map.pos[5] = DSPD_CHMAP_LFE;
	  break;
	case 7:
	  tmp.map.pos[0] = DSPD_CHMAP_FL;
	  tmp.map.pos[1] = DSPD_CHMAP_FR;
	  tmp.map.pos[2] = DSPD_CHMAP_RL;
	  tmp.map.pos[3] = DSPD_CHMAP_RR;
	  tmp.map.pos[4] = DSPD_CHMAP_FC;
	  tmp.map.pos[5] = DSPD_CHMAP_LFE;
	  tmp.map.pos[6] = DSPD_CHMAP_RC;
	  break;
	case 8:
	  tmp.map.pos[0] = DSPD_CHMAP_FL;
	  tmp.map.pos[1] = DSPD_CHMAP_FR;
	  tmp.map.pos[2] = DSPD_CHMAP_RL;
	  tmp.map.pos[3] = DSPD_CHMAP_RR;
	  tmp.map.pos[4] = DSPD_CHMAP_FC;
	  tmp.map.pos[5] = DSPD_CHMAP_LFE;
	  tmp.map.pos[6] = DSPD_CHMAP_SL;
	  tmp.map.pos[7] = DSPD_CHMAP_SR;
	  break;
	default:
	  memcpy(&tmp, smap, dspd_fchmap_sizeof(smap));
	  break;
	}
      ret = dspd_chmap_getconv((const struct dspd_chmap*)&tmp, 
			       (const struct dspd_chmap*)smap, 
			       (struct dspd_chmap*)cmap);
      if ( ! ret )
	{
	  tmp.map.channels = channels;
	  for ( i = 0; i < channels; i++ )
	    tmp.map.pos[i] = cmap->map.pos[i];
	  if ( dspd_chmap_getconv((const struct dspd_chmap*)&tmp, 
				  (const struct dspd_chmap*)smap, 
				  (struct dspd_chmap*)cmap) )
	    ret = 0;
	  else
	    ret = EINVAL;
	} else
	{
	  ret = 0;
	}
    }
  if ( ret == 0 )
    {
      if ( ! dspd_chmap_test((const struct dspd_chmap*)smap,
			     (const struct dspd_chmap*)cmap,
			     channels) )
	ret = EINVAL;
      else
	ret = 0;
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
  struct dspd_fchmap chmap, map2;
  struct dspd_pcmdev_stream *s;
  int32_t ret;
  if ( inbufsize == sizeof(val) )
    val = *(uint64_t*)inbuf;
  else
    val = *(uint32_t*)inbuf;
  stream = val & 0xFFFFFFFF;
  channels = val >> 32U;
  if ( channels > DSPD_CHMAP_MAXCHAN )
    return dspd_req_reply_err(context, 0, EINVAL);
  memset(&map2, 0, sizeof(map2));
  if ( stream == DSPD_PCM_SBIT_PLAYBACK && dev->playback.ops )
    {
      s = &dev->playback;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE && dev->capture.ops )
    {
      s = &dev->capture;
    } else
    {
      return dspd_req_reply_err(context, 0, EINVAL);
    }

  if ( s->ops->get_chmap )
    {
      if ( channels == 0 )
	{
	  //Get the device channel map (channel ids)
	  ret = s->ops->get_chmap(s->handle, (struct dspd_chmap*)&chmap);
	  if ( ret == 0 )
	    {
	      if ( outbufsize < dspd_fchmap_sizeof(&chmap) )
		ret = dspd_req_reply_err(context, 0, ENOSPC);
	      else
		ret = dspd_req_reply_buf(context, 0, &chmap, dspd_fchmap_sizeof(&chmap));
	    } else
	    {
	      ret = dspd_req_reply_err(context, 0, ret);
	    }
	} else if ( s->ops->create_chmap )
	{ 
	  //Create channel map with specified number of channels using the driver
	  ret = s->ops->create_chmap(s->handle, channels, (struct dspd_chmap*)&chmap);
	  if ( ret == 0 )
	    ret = dspd_req_reply_buf(context, 0, &chmap, dspd_fchmap_sizeof(&chmap));
	  else
	    ret = dspd_req_reply_err(context, 0, ret);
	} else
	{
	  //Try to create a channel map using generic routines
	  ret = s->ops->get_chmap(s->handle, (struct dspd_chmap*)&chmap);
	  if ( ret == 0 )
	    ret = create_channelmap_generic(&chmap, &map2, channels);
	  if ( ret == 0 )
	    ret = dspd_req_reply_buf(context, 0, &map2, dspd_fchmap_sizeof(&map2));
	  else
	    ret = dspd_req_reply_err(context, 0, ret);
	}
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}

static int32_t server_convert_chmap(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  
  const struct dspd_chmap *in = inbuf;
  struct dspd_fchmap tmp, out;
  int32_t ret;
  struct dspd_pcm_device *dev = dspd_req_userdata(context);
  struct dspd_pcmdev_stream *s;
  if ( inbufsize < dspd_chmap_sizeof(in) || in->channels > DSPD_CHMAP_MAXCHAN )
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    } else
    {
      if ( (in->stream & DSPD_PCM_SBIT_PLAYBACK) && dev->playback.ops )
	{
	  s = &dev->playback;
	} else if ( (in->stream & DSPD_PCM_SBIT_CAPTURE) && dev->capture.ops )
	{
	  s = &dev->capture;
	} else
	{
	  return dspd_req_reply_err(context, 0, EINVAL);
	}
      if ( s->ops->translate_chmap )
	{
	  ret = s->ops->translate_chmap(s->handle, in, (struct dspd_chmap*)&out);
	} else if ( s->ops->get_chmap )
	{
	  ret = s->ops->get_chmap(s->handle, (struct dspd_chmap*)&tmp);
	  if ( ret == 0 )
	    {
	      if ( dspd_chmap_getconv(in, 
				      (const struct dspd_chmap*)&tmp, 
				      (struct dspd_chmap*)&out) &&
		   dspd_chmap_test((const struct dspd_chmap*)&tmp,
				   (const struct dspd_chmap*)&out,
				   in->channels) &&
		   dspd_fchmap_sizeof(&out) <= outbufsize )
		{
		  ret = 0;
		} else
		{
		  ret = EINVAL;
		}
	    }
	} else
	{
	  ret = EINVAL;
	}
      if ( ret == 0 )
	ret = dspd_req_reply_buf(context, 0, &out, dspd_fchmap_sizeof(&out));
      else
	ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
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
    .outbufsize = sizeof(struct dspd_stream_volume),
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
