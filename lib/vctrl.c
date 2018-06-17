
/*
 *   DSPD Software Controls
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



#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <grp.h>
#include <sys/types.h>
#include <pwd.h>
#include <sys/stat.h>
#include <ctype.h>
#include "sslib.h"
#include "daemon.h"
#include "syncgroup.h"
#include "vctrl.h"


#define DSPD_VCTRL_DEVICE (1<<2)
#define DSPD_VCTRL_CLIENT (1<<3)
#define DSPD_VCTRL_VALUE (1<<4)
#define DSPD_VCTRL_INFO (1<<5)
#define DSPD_VCTRL_ADD (1<<6)
#define DSPD_VCTRL_TLV (1<<7)
#define DSPD_VCTRL_CHANGED (1<<8)
#define DSPD_VCTRL_REMOVED (1<<9)
#define VCTRL_EVENTS (DSPD_VCTRL_CHANGED|DSPD_VCTRL_ADD|DSPD_VCTRL_INFO|DSPD_VCTRL_TLV|DSPD_VCTRL_VALUE)
struct dspd_vctrl {
  int16_t  flags;
  int16_t  playback;
  int16_t  capture;
  int16_t  index;
  uint64_t tstamp;  //Time of creation
  uint64_t update_count; //Number of times updated
  uint64_t event_id;
  int32_t  values[2];
  char     name[32];
};

struct dspd_vctrl_callback {
  dspd_mixer_callback         callback;
  void                       *arg;
  struct dspd_vctrl_callback *next;
};

struct dspd_vctrl_list {
  ssize_t                     ctrl_count;
  //This list is necessary because all controls for 0 to count must
  //be valid.  So this is accessed by control index.
  struct dspd_vctrl         *ctrl_list[DSPD_MAX_OBJECTS];
  //Pointers to list entries by object index.
  //This is to speed up vctrl_set_value() especially in the
  //case where the control is not registered.
  struct dspd_vctrl         *ctrl_pointers[DSPD_MAX_OBJECTS];
  //List of removed objects.  This must be separate from the others
  //so that ctrl_list is always valid.
  struct dspd_vctrl         *removed_list[DSPD_MAX_OBJECTS];
  struct dspd_vctrl_callback *cb_list;
  //Must be a recursive mutex so that setting values can happen without
  //a race condition or any special cases.
  dspd_mutex_t                list_lock;
  dspd_cond_t                 event;
  bool                        updated;
  pthread_t                   thread;
  volatile AO_t               terminate;
  dspd_time_t                 mixer_tstamp;
  struct dspd_vctrl_callback *callbacks;
  uint64_t                    update_count;
};



static void *vctrl_thread(void *p);

struct dspd_vctrl_list *get_list(struct dspd_rctx *rctx)
{
  struct dspd_daemon_ctx *dctx = dspd_req_userdata(rctx);
  return dctx->vctrl;
}

static int32_t vctrl_init(struct dspd_vctrl_list *list)
{
  int32_t ret;
  pthread_mutexattr_t attr;
  ret = pthread_mutexattr_init(&attr);
  if ( ret )
    return -ret;
  ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  if ( ret )
    {
      ret *= -1;
      goto out;
    }
  memset(list, 0, sizeof(*list));
  ret = dspd_mutex_init(&list->list_lock, &attr);
  if ( ret == 0 )
    {
      ret = dspd_cond_init(&list->event, NULL);
      if ( ret == 0 )
	ret = pthread_create(&list->thread, NULL, vctrl_thread, list);
    }

 out:
  pthread_mutexattr_destroy(&attr);
  if ( ret != 0 )
    {
      ret *= -1;
      dspd_mutex_destroy(&list->list_lock);
      dspd_cond_destroy(&list->event);
    }
  return ret;
}

int32_t dspd_vctrl_list_new(struct dspd_vctrl_list **list)
{
  struct dspd_vctrl_list *l;
  int32_t ret;
  l = malloc(sizeof(*l));
  if ( l )
    {
      ret = vctrl_init(l);
      if ( ret < 0 )
	free(l);
      else
	*list = l;
    } else
    {
      ret = -ENOMEM;
    }
  return ret;
}


static void vctrl_destroy(struct dspd_vctrl_list *list)
{
  size_t i;
  struct dspd_vctrl_callback *curr, *prev;
  AO_store(&list->terminate, 1);
  dspd_mutex_lock(&list->list_lock);
  list->updated = true;
  dspd_cond_signal(&list->event);
  dspd_mutex_unlock(&list->list_lock);
  pthread_join(list->thread, NULL);
  dspd_cond_destroy(&list->event);
  dspd_mutex_destroy(&list->list_lock);
  for ( i = 0; i < ARRAY_SIZE(list->ctrl_pointers); i++ )
    free(list->ctrl_pointers[i]);
  prev = NULL;
  for ( curr = list->callbacks; curr; curr = curr->next )
    {
      free(prev);
      prev = curr;
    }
  free(prev);
  memset(list, 0, sizeof(*list));
}

void dspd_vctrl_list_delete(struct dspd_vctrl_list *list)
{
  if ( list )
    {
      vctrl_destroy(list);
      free(list);
    }
}

static void vctrl_notify(struct dspd_vctrl_list *list, struct dspd_vctrl *ctrl)
		  
{
  uint32_t mask = 0;
  struct dspd_vctrl_callback *curr;
  if ( ctrl->flags & DSPD_VCTRL_REMOVED )
    {
      mask = DSPD_CTL_EVENT_MASK_REMOVE;
    } else if ( ctrl->flags & DSPD_VCTRL_CHANGED )
    {
      mask = ctrl->flags & ~DSPD_VCTRL_CHANGED;
      mask >>= 4U;
      mask |= DSPD_VCTRL_CHANGED;
    } else
    {
      mask = ctrl->flags;
      mask >>= 4U;
    }
  for ( curr = list->callbacks; curr; curr = curr->next )
    {
      if ( curr->callback )
	curr->callback(0, 
		       ctrl->index, 
		       mask,
		       curr->arg);
    }
}

static void vctrl_remove_notify(struct dspd_vctrl_list *list,
				struct dspd_vctrl *ctrl)
{
  struct dspd_vctrl_callback *curr;
  DSPD_ASSERT(ctrl->flags & DSPD_VCTRL_REMOVED);
  if ( ctrl->playback >= 0 )
    {
      DSPD_ASSERT(list->ctrl_pointers[ctrl->playback] != ctrl);
    }
  if ( ctrl->capture >= 0 )
    {
      DSPD_ASSERT(list->ctrl_pointers[ctrl->capture] != ctrl);
    }
  for ( curr = list->callbacks; curr; curr = curr->next )
    {
      if ( curr->callback )
	curr->callback(0, 
		       ctrl->index, 
		       DSPD_CTL_EVENT_MASK_REMOVE,
		       curr->arg);
    }
}

//Events must be dispatched from a dedicated thread to avoid deadlocks.
static void *vctrl_thread(void *p)
{
  struct dspd_vctrl_list *list = p;
  size_t i;
  struct dspd_vctrl *ctrl;
  while ( AO_load(&list->terminate) == 0 )
    {
      dspd_mutex_lock(&list->list_lock);
      while ( list->updated == false )
	dspd_cond_wait(&list->event, &list->list_lock);
      list->updated = false;
      if ( AO_load(&list->terminate) == 0 )
	{
	  //It is necessary to notify about removed controls first since there it is possible
	  //that another control with the same stream and slot was added before this thread woke up.
	  for ( i = 0; i < ARRAY_SIZE(list->removed_list); i++ )
	    {
	      ctrl = list->removed_list[i];
	      if ( ! ctrl )
		break;
	      vctrl_remove_notify(list, ctrl);
	      free(ctrl);
	      list->removed_list[i] = NULL;
	    }
	  for ( i = 0; i < list->ctrl_count; i++ )
	    {
	      ctrl = list->ctrl_list[i];
	      DSPD_ASSERT(ctrl);
	      if ( ctrl->flags & VCTRL_EVENTS )
		{
		  vctrl_notify(list, ctrl);
		  ctrl->flags &= ~VCTRL_EVENTS;
		}
	    }
	}
      dspd_mutex_unlock(&list->list_lock);
    }
  return NULL;
}



static void vctrl_wake(struct dspd_vctrl_list *list)
{
  if ( ! list->updated )
    {
      list->updated = true;
      dspd_cond_signal(&list->event);
    }
}

//This is called by devices and clients when a DCTRL command is executed.
static void vctrl_set_value(struct dspd_vctrl_list *list, 
			    uint32_t stream, 
			    uint32_t sbits,
			    float value,
			    const char *name)
{
  bool changed;
  struct dspd_vctrl *ctrl;
  int32_t val;
  if ( stream < ARRAY_SIZE(list->ctrl_pointers) )
    {
      if ( list->ctrl_pointers[stream] )
	{
	  dspd_mutex_lock(&list->list_lock);
	  ctrl = list->ctrl_pointers[stream];
	  if ( ctrl )
	    {
	      if ( value >= 0.0 )
		{
		  if ( value > 1.0 )
		    value = 1.0;
		  if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
		    {
		      val = (int32_t)(VCTRL_RANGE_MAX * value);
		      if ( val != ctrl->values[DSPD_PCM_STREAM_PLAYBACK] )
			{
			  ctrl->values[DSPD_PCM_STREAM_PLAYBACK] = val;
			  ctrl->flags |= DSPD_VCTRL_VALUE;
			  changed = true;
			}
		    }
		  if ( sbits & DSPD_PCM_SBIT_CAPTURE )
		    {
		      val = (int32_t)(VCTRL_RANGE_MAX * value);
		      if ( val != ctrl->values[DSPD_PCM_STREAM_CAPTURE] )
			{
			  ctrl->values[DSPD_PCM_STREAM_CAPTURE] = val;
			  ctrl->flags |= DSPD_VCTRL_VALUE;
			  changed = true;
			}
		    }
		}
	       if ( name )
		 {
		   if ( strcmp(name, ctrl->name) != 0 )
		     {
		       changed = true;
		       ctrl->flags |= DSPD_VCTRL_INFO;
		       strlcpy(ctrl->name, name, sizeof(ctrl->name));
		     }
		 }

	      if ( changed && list->callbacks )
		{
		  ctrl->update_count++;
		  ctrl->flags |= DSPD_VCTRL_CHANGED;
		  list->update_count++;
		  vctrl_wake(list);
		}
	    }
	  dspd_mutex_unlock(&list->list_lock);
	}
    }
}

void dspd_daemon_vctrl_set_value(uint32_t stream, 
				 uint32_t sbits,
				 float value,
				 const char *name)
{
  if ( dspd_dctx.vctrl )
    vctrl_set_value(dspd_dctx.vctrl, stream, sbits, value, name);
}


static int32_t vctrl_register(struct dspd_vctrl_list *list,
			      int32_t playback,
			      int32_t capture,
			      int32_t type,
			      float value,
			      uint64_t event_id,
			      const char *name)
			      
{
  int32_t ret = -EEXIST;
  struct dspd_vctrl *ctrl;
  //Objects that implement client and server (device) interfaces are not yet supported
  //because there is not yet an implementation of any such object.
  if ( type == (DSPD_VCTRL_CLIENT|DSPD_VCTRL_DEVICE) || //Client+server
       type == 0 || //No type specified
       (type & ~(DSPD_VCTRL_CLIENT|DSPD_VCTRL_DEVICE)) != 0 ) //Invalid flags
    return -EINVAL;
    
  dspd_mutex_lock(&list->list_lock);
  if ( playback >= DSPD_MAX_OBJECTS ||
       capture >= DSPD_MAX_OBJECTS ||
       (playback < 0 && capture < 0) )
    {
      ret = -EINVAL;
      goto out;
    }
  if ( playback >= 0 )
    {
      if ( list->ctrl_pointers[playback] )
	goto out;
    }
  if ( capture >= 0 )
    {
      if ( list->ctrl_pointers[capture] )
	goto out;
    }
  ctrl = calloc(1, sizeof(*ctrl));
  if ( ! ctrl )
    {
      ret = -ENOMEM;
      goto out;
    }
  if ( name )
    strlcpy(ctrl->name, name, sizeof(ctrl->name));
  ctrl->playback = playback;
  ctrl->capture = capture;
  ctrl->flags = type;
  ctrl->flags |= DSPD_VCTRL_ADD;
  ctrl->event_id = event_id;
  if ( value < 0.0 )
    value = 0.0;
  else if ( value > 1.0 )
    value = 1.0;
  if ( playback >= 0 )
    {
      ctrl->values[DSPD_PCM_STREAM_PLAYBACK] = (VCTRL_RANGE_MAX * value);
      ctrl->flags |= DSPD_PCM_SBIT_PLAYBACK;
      list->ctrl_pointers[playback] = ctrl;
    }
  if ( capture >= 0 )
    {
      ctrl->values[DSPD_PCM_STREAM_CAPTURE] = (VCTRL_RANGE_MAX * value);
      ctrl->flags |= DSPD_PCM_SBIT_CAPTURE;
      list->ctrl_pointers[capture] = ctrl;
    }
  DSPD_ASSERT(list->ctrl_count < ARRAY_SIZE(list->ctrl_list));
  list->ctrl_list[list->ctrl_count] = ctrl;
  ctrl->index = list->ctrl_count;
  list->ctrl_count++;
  list->mixer_tstamp = dspd_get_time();
  ctrl->tstamp = list->mixer_tstamp;
  vctrl_wake(list);
  ret = 0;
 out:
  dspd_mutex_unlock(&list->list_lock);
  return ret;
}

int32_t dspd_daemon_vctrl_register(const struct dspd_vctrl_reg *info)
{
  int32_t ret = -ENOSYS;
  if ( dspd_dctx.vctrl )
    ret = vctrl_register(dspd_dctx.vctrl, 
			 info->playback, 
			 info->capture, 
			 info->type, 
			 info->initval, 
			 info->hotplug_event_id, 
			 info->displayname);
  return ret;
}

static bool unreg(struct dspd_vctrl_list *list, int32_t stream, const uint64_t *event_id)
{
  size_t i, j, o;
  struct dspd_vctrl *ctrl;
  bool ret = false;
  o = 0;
  for ( i = 0; i < list->ctrl_count; i++ )
    {
      ctrl = list->ctrl_list[i];
      if ( (ctrl->playback == stream || ctrl->capture == stream) && (event_id == NULL || *event_id == ctrl->event_id) )
	{
	  //Add to the list of items to be removed.
	  for ( j = 0; j < ARRAY_SIZE(list->removed_list); j++ )
	    {
	      if ( list->removed_list[j] == NULL )
		{
		  list->removed_list[j] = ctrl;
		  ctrl->flags |= DSPD_VCTRL_REMOVED;
		  break;
		}
	    }
	  //The pointers must be removed here because they could be registered again before
	  //the dispatch thread wakes up.
	  if ( ctrl->playback >= 0 )
	    {
	      DSPD_ASSERT(list->ctrl_pointers[ctrl->playback] == ctrl);
	      list->ctrl_pointers[ctrl->playback] = NULL;
	    }
	  if ( ctrl->capture >= 0 )
	    {
	      DSPD_ASSERT(list->ctrl_pointers[ctrl->capture] == ctrl);
	      list->ctrl_pointers[ctrl->capture] = NULL;
	    }
	  DSPD_ASSERT(j < ARRAY_SIZE(list->removed_list));
	  ret = true;
	} else
	{
	  list->ctrl_list[o] = ctrl;
	  ctrl->index = o;
	  o++;
	}
    }
  list->ctrl_count = o;
  return ret;
}

static int32_t vctrl_unregister(struct dspd_vctrl_list *list, 
				int32_t playback,
				int32_t capture,
				const uint64_t *event)
{
  int32_t ret = -ENOENT;
  dspd_mutex_lock(&list->list_lock);
  if ( playback >= 0 )
    if ( unreg(list, playback, event) )
      ret = 0;
  if ( capture >= 0 )
    if ( unreg(list, capture, event) )
      ret = 0;
  if ( ret == 0 )
    {
      DSPD_ASSERT(list->ctrl_count >= 0);
      list->mixer_tstamp = dspd_get_time();
      vctrl_wake(list);
    }
  dspd_mutex_unlock(&list->list_lock);
  return ret;
}

int32_t dspd_daemon_vctrl_unregister(int32_t playback,
				     int32_t capture,
				     const uint64_t *event_id)
{
  int32_t ret = -ENOSYS;
  if ( dspd_dctx.vctrl )
    ret = vctrl_unregister(dspd_dctx.vctrl, playback, capture, event_id);
  return ret;
}

static int32_t vctrl_mixer_elem_count(struct dspd_rctx *rctx,
				     uint32_t          req,
				     const void       *inbuf,
				     size_t            inbufsize,
				     void             *outbuf,
				     size_t            outbufsize)
{
  struct dspd_vctrl_list *list = get_list(rctx);
  uint32_t count;
  uint64_t c;
  int32_t ret;
  dspd_mutex_lock(&list->list_lock);
  count = list->ctrl_count;
  dspd_mutex_unlock(&list->list_lock);
  if ( outbufsize >= sizeof(c) )
    {
      c = DSPD_MAX_OBJECTS;
      c <<= 32U;
      c |= (uint64_t)count;
      ret = dspd_req_reply_buf(rctx, 0, &c, sizeof(c));
    } else
    {
      ret = dspd_req_reply_buf(rctx, 0, &count, sizeof(count));
    }
  return ret;
}

static int32_t vctrl_mixer_elem_info(struct dspd_rctx *rctx,
				     uint32_t          req,
				     const void       *inbuf,
				     size_t            inbufsize,
				     void             *outbuf,
				     size_t            outbufsize)
{
  struct dspd_vctrl_list *list = get_list(rctx);
  int32_t ret = EIDRM;
  struct dspd_mix_info *info = outbuf;
  struct dspd_vctrl *ctrl;
  uint32_t idx = *(const uint32_t*)inbuf;
  dspd_mutex_lock(&list->list_lock);
  if ( idx < list->ctrl_count )
    {
      memset(info, 0, sizeof(*info));
      ctrl = list->ctrl_list[idx];
      //Make it possible to identify client and device controls.  A device control could
      //probably be implemented in hardware but a client control probably can't be.
      if ( ctrl->flags & DSPD_VCTRL_CLIENT )
	info->flags |= DSPD_MIXF_VIRTUAL;
      if ( ctrl->flags & DSPD_PCM_SBIT_PLAYBACK )
	{
	  info->flags |= DSPD_MIXF_PVOL | DSPD_MIXF_PMONO;
	  info->pchan_mask = 1 << DSPD_MIXER_CHN_MONO;
	}
      if ( ctrl->flags & DSPD_PCM_SBIT_CAPTURE )
	{
	  info->flags |= DSPD_MIXF_CVOL | DSPD_MIXF_CMONO;
	  info->cchan_mask = 1 << DSPD_MIXER_CHN_MONO;
	}
      info->tstamp = ctrl->tstamp;
      info->update_count = ctrl->update_count;
      info->ctl_index = idx;
      //First 32 bits is stream numbers.  This is hardware specific, so the meaning here is
      //specific to dspd vctrl.
      info->hwinfo = (uint16_t)ctrl->capture;
      info->hwinfo <<= 16U;
      info->hwinfo |= (uint16_t)ctrl->playback;
      strlcpy(info->name, ctrl->name, sizeof(info->name));
      ret = 0;
    }
  dspd_mutex_unlock(&list->list_lock);
  if ( ret != 0 )
    ret = dspd_req_reply_err(rctx, 0, ret);
  else
    ret = dspd_req_reply_buf(rctx, 0, info, sizeof(*info));
  return ret;
}

static int32_t vctrl_mixer_enum_info(struct dspd_rctx *rctx,
				    uint32_t          req,
				    const void       *inbuf,
				    size_t            inbufsize,
				    void             *outbuf,
				    size_t            outbufsize)
{
  return dspd_req_reply_err(rctx, 0, EINVAL);
}

static int32_t vctrl_mixer_elem_getval(struct dspd_rctx *rctx,
				       uint32_t          req,
				       const void       *inbuf,
				       size_t            inbufsize,
				       void             *outbuf,
				       size_t            outbufsize)
{
  struct dspd_vctrl_list *list = get_list(rctx);
  const struct dspd_mix_val *cmd = inbuf;
  struct dspd_mix_val *val = outbuf;
  struct dspd_vctrl *ctrl;
  int32_t ret = EINVAL;
  if ( req == DSPD_SCTL_SERVER_MIXER_GETVAL )
    dspd_mutex_lock(&list->list_lock);
  //else {/*reply from cmd==DSPD_SCTL_SERVER_SETVAL*/}
  if ( cmd->index == UINT32_MAX )
    {
      memset(val, 0, sizeof(*val));
      val->index = cmd->index;
      val->update_count = list->update_count;
      val->tstamp = list->mixer_tstamp;
      ret = 0;
    } else if ( cmd->index < list->ctrl_count && (cmd->channel == DSPD_MIXER_CHN_MONO || cmd->channel == -1) )
    {
      memset(val, 0, sizeof(*val));
      ctrl = list->ctrl_list[cmd->index];
      switch(cmd->type)
	{
	case DSPD_MIXF_PVOL:
	  if ( ctrl->flags & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      val->value = ctrl->values[DSPD_PCM_STREAM_PLAYBACK];
	      ret = 0;
	    }
	  break;
	case DSPD_MIXF_CVOL:
	  if ( ctrl->flags & DSPD_PCM_SBIT_CAPTURE )
	    {
	      val->value = ctrl->values[DSPD_PCM_STREAM_CAPTURE];
	      ret = 0;
	    }
	  break;
	default:
	  break;
	}
      if ( ret == 0 )
	{
	  if ( cmd->flags & DSPD_CTRLF_SCALE_PCT )
	    val->value = val->value / (VCTRL_RANGE_MAX / 100);
	  val->tstamp = ctrl->tstamp;
	  val->update_count = ctrl->update_count;
	  val->index = cmd->index;
	  val->type = cmd->type;
	}
    }
  dspd_mutex_unlock(&list->list_lock);
  if ( ret == 0 )
    ret = dspd_req_reply_buf(rctx, 0, val, sizeof(*val));
  else
    ret = dspd_req_reply_err(rctx, 0, ret);
  return ret;
}

static int32_t vctrl_mixer_elem_setval(struct dspd_rctx *rctx,
				       uint32_t          req,
				       const void       *inbuf,
				       size_t            inbufsize,
				       void             *outbuf,
				       size_t            outbufsize)

{  
  struct dspd_vctrl_list *list = get_list(rctx);
  const struct dspd_mix_val *cmd = inbuf;
  struct dspd_vctrl *ctrl;
  int32_t ret = EINVAL;
  bool changed = false;
  uint64_t t;
  int32_t val;
  int32_t volreq;
  int32_t stream;
  size_t br;
  struct dspd_stream_volume svol;
  dspd_mutex_lock(&list->list_lock);
  if ( cmd->index < list->ctrl_count )
    {
      ctrl = list->ctrl_list[cmd->index];
      if ( cmd->flags & DSPD_CTRLF_TSTAMP_32BIT )
	t = (ctrl->tstamp / 1000000ULL) % UINT32_MAX;
      else
	t = ctrl->tstamp;
      if ( cmd->tstamp == 0 || cmd->tstamp == t )
	{
	  if ( cmd->flags & DSPD_CTRLF_SCALE_PCT )
	    val = cmd->value * (VCTRL_RANGE_MAX / 100);
	  else
	    val = cmd->value;
	  if ( val > VCTRL_RANGE_MAX )
	    val = VCTRL_RANGE_MAX;
	  else if ( val < 0 )
	    val = 0;
	  if ( ctrl->flags & DSPD_VCTRL_DEVICE )
	    volreq = DSPD_SCTL_SERVER_SETVOLUME;
	  else
	    volreq = DSPD_SCTL_CLIENT_SETVOLUME;
	  memset(&svol, 0, sizeof(svol));
	  switch(cmd->type)
	    {
	    case DSPD_MIXF_PVOL:
	      if ( ctrl->flags & DSPD_PCM_SBIT_PLAYBACK )
		{
		  svol.stream = DSPD_PCM_SBIT_PLAYBACK;
		  svol.volume = (val * 1.0) / (VCTRL_RANGE_MAX * 1.0);
		  stream = ctrl->playback;
		  ret = 0;
		}
	      break;
	    case DSPD_MIXF_CVOL:
	      if ( ctrl->flags & DSPD_PCM_SBIT_CAPTURE )
		{
		  svol.stream = DSPD_PCM_SBIT_CAPTURE;
		  svol.volume = (val * 1.0) / (VCTRL_RANGE_MAX * 1.0);
		  stream = ctrl->capture;
		  ret = 0;
		}
	      break;
	    default:
	      break;
	    }
	  if ( ret == 0 )
	    ret = dspd_stream_ctl(&dspd_dctx,
				  stream,
				  volreq,
				  &svol,
				  sizeof(svol),
				  NULL,
				  0,
				  &br);
	} else
	{
	  ret = EIDRM;
	}
    }
  
  if ( ret == 0 && changed == true )
    vctrl_wake(list);
  if ( ret == 0 && outbufsize >= sizeof(struct dspd_mix_val) )
    {
      //Unlocking and replying happen in vctrl_mixer_elem_getval
      ret = vctrl_mixer_elem_getval(rctx, req, inbuf, inbufsize, outbuf, outbufsize);
    } else
    {
      //Client does not want results so send error code only.
      dspd_mutex_unlock(&list->list_lock);
      ret = dspd_req_reply_err(rctx, 0, ret);
    }
  return ret;
}

static int32_t vctrl_mixer_elem_getrange(struct dspd_rctx *rctx,
					uint32_t          req,
					const void       *inbuf,
					size_t            inbufsize,
					void             *outbuf,
					size_t            outbufsize)
{
  struct dspd_vctrl_list *list = get_list(rctx);
  const struct dspd_mix_val *cmd = inbuf;
  int32_t ret = EINVAL;
  struct dspd_mix_range *range = outbuf;
  struct dspd_vctrl *ctrl;
  dspd_mutex_lock(&list->list_lock);
  if ( cmd->index < list->ctrl_count )
    {
      ctrl = list->ctrl_list[cmd->index];
      switch(cmd->type)
	{
	case DSPD_MIXF_PVOL:
	  if ( ctrl->flags & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      range->min = 0;
	      range->max = VCTRL_RANGE_MAX;
	      ret = 0;
	    }
	  break;
	case DSPD_MIXF_CVOL:
	  if ( ctrl->flags & DSPD_PCM_SBIT_CAPTURE )
	    {
	      range->min = 0;
	      range->max = VCTRL_RANGE_MAX;
	      ret = 0;
	    }
	  break;
	}
    }
  dspd_mutex_unlock(&list->list_lock);
  if ( ret == 0 )
    ret = dspd_req_reply_buf(rctx, 0, range, sizeof(*range));
  else
    ret = dspd_req_reply_err(rctx, 0, ret);
  return ret;
}

static int32_t vctrl_mixer_set_cb(struct dspd_rctx *rctx,
				  uint32_t          req,
				  const void       *inbuf,
				  size_t            inbufsize,
				  void             *outbuf,
				  size_t            outbufsize)
{
  struct dspd_vctrl_list *list = get_list(rctx);
  const struct dspd_mixer_cbinfo *cb = inbuf;
  struct dspd_vctrl_callback *newcb;
  struct dspd_vctrl_callback *curr, **next;
  int32_t err;
  if ( ! cb->remove )
    {
      newcb = calloc(1, sizeof(*newcb));
      if ( newcb )
	{
	  err = 0;
	  newcb->callback = cb->callback;
	  newcb->arg = cb->arg;
	  dspd_mutex_lock(&list->list_lock);
	  next = &list->callbacks;
	  for ( curr = list->callbacks; curr; curr = curr->next )
	    next = &curr->next;
	  *next = newcb;
	  dspd_mutex_unlock(&list->list_lock);
	} else
	{
	  err = ENOMEM;
	}
    } else
    {
      err = ENOENT;
      dspd_mutex_lock(&list->list_lock);
      next = &list->callbacks;
      for ( curr = list->callbacks; curr; curr = curr->next )
	{
	  if ( curr->callback == cb->callback && curr->arg == cb->arg )
	    {
	      *next = curr->next;
	      err = 0;
	      break;
	    }
	  next = &curr->next;
	}
      dspd_mutex_unlock(&list->list_lock);
      free(curr);
    }
  return dspd_req_reply_err(rctx, 0, err);
}

static struct dspd_req_handler mixer_handlers[] = {
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ELEM_COUNT)] = {
    .handler = vctrl_mixer_elem_count,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(uint32_t),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ELEM_INFO)] = {
    .handler = vctrl_mixer_elem_info,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(uint32_t),
    .outbufsize = sizeof(struct dspd_mix_info),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ENUM_INFO)] = {
    .handler = vctrl_mixer_enum_info,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_enum_idx),
    .outbufsize = sizeof(struct dspd_mix_info),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_GETVAL)] = {
    .handler = vctrl_mixer_elem_getval,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = sizeof(struct dspd_mix_val),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_SETVAL)] = {
    .handler = vctrl_mixer_elem_setval,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = 0,
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_GETRANGE)] = {
    .handler = vctrl_mixer_elem_getrange,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = sizeof(struct dspd_mix_range),
  },
  [DSPD_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_SETCB)] = { 
    .handler = vctrl_mixer_set_cb,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mixer_cbinfo),
    .outbufsize = 0,
  },
};

int32_t dspd_vctrl_stream_ctl(struct dspd_rctx *rctx,
			      uint32_t          req,
			      const void       *inbuf,
			      size_t            inbufsize,
			      void             *outbuf,
			      size_t            outbufsize)
{
  struct dspd_dispatch_ctl2_info info;
  int32_t ret;
  info.min_req = DSPD_SCTL_SERVER_MIXER_FIRST;
  info.handlers_count = ARRAY_SIZE(mixer_handlers);
  info.req = req;
  info.handlers = mixer_handlers;
  ret = dspd_daemon_dispatch_ctl2(rctx,
				  &info,
				  inbuf,
				  inbufsize,
				  outbuf,
				  outbufsize);
  if ( ret == -ENOSYS )
    ret = dspd_req_reply_err(rctx, 0, EINVAL);
  return ret;
}
