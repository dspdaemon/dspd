/*
 *  DAEMON - Server support code
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
#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif
#include <sys/prctl.h>
#include <ctype.h>
#include <dirent.h>
#include "sslib.h"
#include "daemon.h"
#include "syncgroup.h"
#include "cbpoll.h"
#ifndef SCHED_ISO
#define SCHED_ISO 4
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif


struct dspd_daemon_ctx dspd_dctx;
static bool refresh_default_device_dict(struct dspd_hotplug *hp);
static bool refresh_default_device_name(struct dspd_hotplug *hp);
static int32_t daemon_reply_buf(struct dspd_rctx *arg, 
				int32_t flags, 
				const void *buf, 
				size_t len);
static int32_t daemon_reply_fd(struct dspd_rctx *arg, 
				int32_t flags, 
				const void *buf, 
				size_t len, 
				int32_t fd);
static int32_t daemon_reply_err(struct dspd_rctx *arg, 
				int32_t flags, 
				int32_t err);
static const struct dspd_rcb daemon_rcb = {
  .reply_buf = daemon_reply_buf,
  .reply_fd = daemon_reply_fd,
  .reply_err = daemon_reply_err,
};

static int32_t req_delcli(struct dspd_rctx         *context,
			  uint32_t      req,
			  const void   *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize);
static int32_t drh_get_objmask_size(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize);

static int32_t drh_enumerate_objects(struct dspd_rctx         *context,
				     uint32_t      req,
				     const void   *inbuf,
				     size_t        inbufsize,
				     void         *outbuf,
				     size_t        outbufsize);

static int32_t drh_get_module_count(struct dspd_rctx         *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize);

static int32_t drh_get_module_name(struct dspd_rctx         *context,
				   uint32_t      req,
				   const void   *inbuf,
				   size_t        inbufsize,
				   void         *outbuf,
				   size_t        outbufsize);

static int32_t drh_get_defaultdev(struct dspd_rctx         *context,
				  uint32_t      req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize);

static int32_t drh_syncstart(struct dspd_rctx         *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize);
static int32_t drh_syncstop(struct dspd_rctx         *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize);

static int32_t drh_change_route(struct dspd_rctx         *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize);
static const struct dspd_req_handler daemon_req_handlers[DSPD_DCTL_LAST+1] = {
  [DSPD_DCTL_GET_OBJMASK_SIZE] = {
    .handler = drh_get_objmask_size,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(uint32_t) //Get the mask size in bytes
  },
  [DSPD_DCTL_ENUMERATE_OBJECTS] = {
    .handler = drh_enumerate_objects,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(uint32_t),
    .outbufsize = sizeof(uint8_t), //Should really be DSPD_MAX_SIZE
  },
  [DSPD_DCTL_GET_MODULE_COUNT] = {
    .handler = drh_get_module_count,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(uint32_t),
  },
  [DSPD_DCTL_GET_MODULE_NAME] = {
    .handler = drh_get_module_name,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(uint32_t),
    .outbufsize = DSPD_MODULE_NAME_MAX,
  },
  [DSPD_DCTL_GET_DEFAULTDEV] = {
    .handler = drh_get_defaultdev,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(uint32_t),
    .outbufsize = sizeof(uint32_t),
  },
  [DSPD_DCTL_SYNCSTART] = {
    .handler = drh_syncstart,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_sg_info),
    .outbufsize = sizeof(dspd_time_t),
  },
  [DSPD_DCTL_SYNCSTOP] = {
    .handler = drh_syncstop,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_sg_info),
    .outbufsize = 0,
  },
  [DSPD_DCTL_CHANGE_ROUTE] = {
    .handler = drh_change_route,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_route_req),
    .outbufsize = sizeof(uint64_t),
  },
};




static int32_t dspd_daemon_dispatch_ioctl(struct dspd_rctx *rctx,
					  const struct dspd_req_handler *handlers,
					  uint32_t count,
					  uint64_t             req,
					  const void          *inbuf,
					  size_t        inbufsize,
					  void         *outbuf,
					  size_t        outbufsize)
{
  const struct dspd_req_handler *handler;
  int32_t ret = EINVAL;
  uint32_t i;
  uint32_t req_num = req >> 32;
  for ( i = 0; i < count; i++ )
    {
      handler = &handlers[i];
      if ( handler->data == req_num )
	{
	  if ( handler->handler )
	    ret = handler->handler(rctx,
				   req_num,
				   inbuf,
				   inbufsize,
				   outbuf,
				   outbufsize);
	  break;
	}
    }
  return ret;
}

int32_t dspd_daemon_dispatch_ctl(struct dspd_rctx *rctx,
				 const struct dspd_req_handler *handlers,
				 uint32_t count,
				 uint64_t             req,
				 const void          *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  const struct dspd_req_handler *handler;
  int32_t ret;
  uint32_t index = req & 0xFFFFFFFF;
  uint32_t req_num = req >> 32;
  if ( rctx->flags & DSPD_REQ_FLAG_UNIX_IOCTL )
    return dspd_daemon_dispatch_ioctl(rctx,
				      handlers,
				      count,
				      req,
				      inbuf,
				      inbufsize,
				      outbuf,
				      outbufsize);


  //The first handler is an optional filter.  If it returns -ENOSYS
  //then it should have not touched anything and the regular procedure
  //can continue.  Sending an -EINVAL is recommended as a way of denieing
  //a request.
  handler = &handlers[0];
  if ( handler->handler )
    ret = handler->handler(rctx,
			   req_num,
			   inbuf,
			   inbufsize,
			   outbuf,
			   outbufsize);
  else
    ret = -ENOSYS;
  if ( ret == -ENOSYS )
    {
      if ( index < count )
	{
	  if ( ret == -ENOSYS )
	    {
	      
	      handler = &handlers[index];
	      if ( handler->handler != NULL &&
		   (rctx->flags & handler->xflags) == 0 &&
		   (handler->rflags == 0 || (handler->rflags & rctx->flags) == rctx->flags) &&
		   ((rctx->flags & DSPD_REQ_FLAG_UNIX_FAST_IOCTL)|| //Unix ioctl means it was already verified.
		    ((inbufsize >= handler->inbufsize) &&       //Other stuff is still checked since ioctl handlers might
		     (outbufsize >= handler->outbufsize))) )     //be used in another context.

		{
		  ret = handler->handler(rctx,
					 req_num,
					 inbuf,
					 inbufsize,
					 outbuf,
					 outbufsize);
		} else
		{
		  
		  ret = dspd_req_reply_err(rctx, 0, EINVAL);
		}
	    }
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, EINVAL);
	}
    }
  return ret;
}

static int32_t req_quit(struct dspd_rctx         *context,
			uint32_t      req,
			const void   *inbuf,
			size_t        inbufsize,
			void         *outbuf,
			size_t        outbufsize)
{
  return dspd_req_reply_err(context, 0, 0);
}
static int32_t req_newcli(struct dspd_rctx         *context,
			  uint32_t      req,
			  const void   *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize)
{
  int32_t ret, idx;
  void *ptr;
  if ( inbufsize != 0 && inbufsize != sizeof(int32_t) )
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    } else
    {
      ret = dspd_client_new(dspd_dctx.objects, &ptr);
      if ( ret == 0 )
	{
	  if ( inbuf != NULL && inbufsize == sizeof(int32_t) )
	    {
	      idx = *(int32_t*)inbuf;
	      if ( idx > 0 )
		dspd_daemon_unref(idx);
	    }
	  idx = dspd_client_get_index(ptr);
	  ret = dspd_req_reply_buf(context, 0, &idx, sizeof(idx));
	} else
	{
	  ret = dspd_req_reply_err(context, 0, ret);
	}
    }
  return ret;
}
static int32_t req_delcli(struct dspd_rctx         *context,
			  uint32_t      req,
			  const void   *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize)
{
  int32_t ret;
  int32_t stream;
  if ( inbufsize == 0 )
    {
      ret = dspd_req_reply_err(context, 0, 0);
    } else if ( inbufsize == sizeof(stream) )
    {
      stream = *(int32_t*)inbuf;
      if ( stream > 0 )
	{
	  dspd_stream_ctl(&dspd_dctx, 
			  stream,
			  DSPD_SCTL_CLIENT_DISCONNECT,
			  NULL,
			  0,
			  NULL,
			  0,
			  NULL);

	  dspd_daemon_unref(stream);
	}
      ret = dspd_req_reply_err(context, 0, 0);
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}

static int32_t req_refsrv(struct dspd_rctx         *context,
			  uint32_t      req,
			  const void   *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize)
{
  int32_t odev = -1, ndev = -1;
  int64_t val;
  size_t br;
  int32_t ret;
  if ( inbufsize == sizeof(int64_t) )
    {
      val = *(int64_t*)inbuf;
      odev = val >> 32U;
      ndev = val & 0xFFFFFFFF;
    } else if ( inbufsize == sizeof(int32_t) )
    {
      ndev = *(int32_t*)inbuf;
    } else
    {
      return dspd_req_reply_err(context, 0, EINVAL);
    }
  if ( odev != ndev )
    {
      ret = dspd_daemon_ref(ndev, DSPD_DCTL_ENUM_TYPE_SERVER);
      if ( ret == 0 && odev > 0 )
	dspd_daemon_unref(odev);
    } else
    {
      ret = 0;
    }
  if ( ret == 0 && outbufsize > 0 )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    ndev,
			    DSPD_SCTL_SERVER_STAT,
			    NULL,
			    0,
			    outbuf,
			    outbufsize,
			    &br);
      if ( ret == 0 )
	return dspd_req_reply_buf(context, 0, outbuf, br);
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t req_unrefsrv(struct dspd_rctx         *context,
			    uint32_t      req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  int32_t ret;
  int32_t idx;
  if ( inbufsize == sizeof(int32_t) )
    {
      idx = *(int32_t*)inbuf;
      if ( idx > 0 )
	dspd_daemon_unref(idx);
      ret = dspd_req_reply_err(context, 0, 0);
    } else if ( inbufsize == 0 )
    {
      ret = dspd_req_reply_err(context, 0, 0);
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}

static const struct dspd_req_handler srv_req_handlers[] = {
  [DSPD_SOCKSRV_REQ_QUIT] = {
    .handler = req_quit,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },
  [DSPD_SOCKSRV_REQ_NEWCLI] = {
    .handler = req_newcli,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(int32_t),
  },
  [DSPD_SOCKSRV_REQ_DELCLI] = {
    .handler = req_delcli,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },
  [DSPD_SOCKSRV_REQ_REFSRV] = {
    .handler = req_refsrv,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },

  [DSPD_SOCKSRV_REQ_UNREFSRV] = {
    .handler = req_unrefsrv,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },
};

static int32_t dspd_daemon_obj_ctl(struct dspd_rctx *rctx,
				   uint32_t             req,
				   const void          *inbuf,
				   size_t        inbufsize,
				   void         *outbuf,
				   size_t        outbufsize)
{
  uint64_t r;
  int32_t ret;
  //Index and request number are the same.
  r = req;
  r <<= 32;
  r |= req;
  if ( rctx->index == -1 )
    {
      ret = dspd_daemon_dispatch_ctl(rctx,
				     srv_req_handlers,
				     sizeof(srv_req_handlers) / sizeof(srv_req_handlers[0]),
				     r,
				     inbuf,
				     inbufsize,
				     outbuf,
				     outbufsize);
    } else
    {
      if ( req >= DSPD_SCTL_SERVER_MIXER_FIRST && dspd_dctx.vctrl != NULL )
	ret = dspd_vctrl_stream_ctl(rctx, req, inbuf, inbufsize, outbuf, outbufsize);
      else
	ret = dspd_daemon_dispatch_ctl(rctx,
				       daemon_req_handlers,
				       sizeof(daemon_req_handlers) / sizeof(daemon_req_handlers[0]),
				       r,
				       inbuf,
				       inbufsize,
				       outbuf,
				       outbufsize);
    }
  return ret;
}

static int dspd_hotplug_init(struct dspd_daemon_ctx *ctx)
{
  int ret = pthread_mutex_init(&ctx->hotplug.lock, NULL);
  struct dspd_dict *dict;
  if ( ret )
    {
      ret *= -1;
    } else
    {
      ctx->hotplug.default_playback = -1;
      ctx->hotplug.default_capture = -1;

      dict = dspd_dict_find_section(dspd_dctx.config, "DEFAULT_DEVICE");
      ctx->hotplug.playback_search = dspd_dict_find_section(dspd_dctx.config, "DEFAULT_PLAYBACK");
      if ( ctx->hotplug.playback_search == NULL )
	ctx->hotplug.playback_search = dict;
      ctx->hotplug.capture_search = dspd_dict_find_section(dspd_dctx.config, "DEFAULT_CAPTURE");
      if ( ctx->hotplug.capture_search == NULL )
	ctx->hotplug.capture_search = dict;

    }
  return ret;
}

void print_usage(void)
{
  fprintf(stderr, "Usage: %s [OPTIONS]\n", dspd_dctx.argv[0]);
  fprintf(stderr,
	  "-c     Configuration directory\n"
	  "-b     Run in the background (daemon)\n"
	  "-h     Print help\n"
	  );
} 

static void set_policy(const char *policy, int32_t *curr)
{
  if ( strcasecmp(policy, "SCHED_ISO") == 0 )
    *curr = SCHED_ISO;
  else if ( strcasecmp(policy, "SCHED_RR") == 0 )
    *curr = SCHED_RR;
  else if ( strcasecmp(policy, "SCHED_FIFO") == 0 )
    *curr = SCHED_FIFO;
  else if ( strcasecmp(policy, "SCHED_OTHER") == 0 )
    *curr = SCHED_OTHER;
  else if ( strcasecmp(policy, "SCHED_IDLE") == 0 || strcasecmp(policy, "SCHED_IDLEPRIO") == 0 )
    *curr = SCHED_IDLE;
  else if ( strcasecmp(policy, "SCHED_BATCH") == 0 )
    *curr = SCHED_BATCH;
  else if ( strcasecmp(policy, "SCHED_DEADLINE") == 0 )
    *curr = SCHED_DEADLINE;
  else if ( strcasecmp(policy, "DEFAULT") != 0 )
    dspd_strtoi32(policy, curr, 0);
  
}

static void set_priority(const char *priority, int32_t *curr)
{
  if ( strcmp(priority, "DEFAULT") != 0 )
    dspd_strtoi32(priority, curr, 0);
}




static bool have_sched_iso(void)
{
  return sched_get_priority_max(SCHED_ISO) >= 0
    && sched_get_priority_min(SCHED_ISO) >= 0;
}

int dspd_setpriority(int which, int who, int prio, int *result)
{
  int r, err;
  errno = 0;
  r = setpriority(which, who, prio);
  err = errno;
  if ( r == -1 && err != 0 )
    {
      r = err;
    } else
    {
      *result = r;
      r = 0;
    }
  return r;
}



static int get_gp(const char *value)
{
  int32_t i = -1; //"default"
  if ( dspd_strtoi32(value, &i, 0) < 0 )
    {
      if ( strcasecmp(value, "off") == 0 )
	i = DSPD_GHCN_OFF;
      else if ( strcasecmp(value, "on") == 0 )
	i = DSPD_GHCN_ON;
      else if ( strcasecmp(value, "latch") == 0 )
	i = DSPD_GHCN_LATCH;
      else if ( strcasecmp(value, "auto") == 0 )
	i = DSPD_GHCN_AUTO;
    }
  return i;
}

/*
static void *dummy(void *p)
{
  return NULL;
}
static bool is_rt(void)
{
  dspd_threadattr_t attr = { .init = 0 };
  pthread_t thr;
  int r;
  //Must have a scheduling policy
  if ( dspd_dctx.rtio_policy != SCHED_RR && 
       dspd_dctx.rtio_policy != SCHED_FIFO &&
       dspd_dctx.rtio_policy != SCHED_ISO &&
       dspd_dctx.priority >= 0 )
    return false;
  //It must actually work
  if ( dspd_daemon_threadattr_init(&attr, sizeof(attr), DSPD_THREADATTR_RTIO) != 0 )
    return false;
  r = pthread_create(&thr, (pthread_attr_t*)&attr, dummy, NULL);
  if ( r == EPERM )
    return false;
  //It probably does work.  Nothing terrible will happen if this is wrong.
  if ( r == 0 )
    pthread_join(thr, NULL);
  return true;
  }*/

static int set_caps(void)
{
#ifdef HAVE_LIBCAP
  cap_t caps = cap_get_proc();
  if ( ! caps )
    return -ENOMEM;
  cap_value_t newcaps[2] = { CAP_SYS_NICE, CAP_IPC_LOCK };
  cap_set_flag(caps, CAP_PERMITTED, 2, newcaps, CAP_SET);
  cap_set_proc(caps);
  prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
  cap_free(caps);
#endif
  return 0;
}

static int reset_caps(void)
{
#ifdef HAVE_LIBCAP
  if ( dspd_dctx.uid > 0 )
    {
      cap_t caps = cap_get_proc();
      if ( ! caps )
	return -ENOMEM;
      cap_value_t newcaps[2] = { CAP_SYS_NICE, CAP_IPC_LOCK };
      cap_set_flag(caps, CAP_EFFECTIVE, 2, newcaps, CAP_SET);
      cap_set_proc(caps);
      cap_free(caps);
      prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
    }
#endif
  return 0;
}

int dspd_daemon_set_thread_nice(int tid, int thread_type)
{
  int ret = 0, r;
  if ( tid < 0 )
    tid = dspd_gettid();
  int prio = dspd_dctx.priority;
  if ( prio < 0 )
    {
      if ( thread_type & DSPD_THREADATTR_RTSVC )
	prio++;
      else if ( ! (thread_type & DSPD_THREADATTR_RTIO) )
	prio = 0;
      ret = dspd_setpriority(PRIO_PROCESS, tid, prio, &r);
    }
  return ret;
}

/*
  Automatically set the thread scheduler params.  There is no return
  value because threads should continue anyway even if it fails.
*/

void dspd_daemon_set_thread_schedparam(int tid, int thread_type)
{
  struct sched_param sp;
  if ( tid < 0 )
    tid = dspd_gettid();
  if ( (thread_type & DSPD_THREADATTR_RTIO) && 
       dspd_dctx.rtio_policy != SCHED_DEADLINE )
    {
      memset(&sp, 0, sizeof(sp));
      sp.sched_priority = dspd_dctx.rtio_priority;
      sched_setscheduler(tid, dspd_dctx.rtio_policy, &sp);
    } else if ( thread_type & DSPD_THREADATTR_RTSVC )
    {
      memset(&sp, 0, sizeof(sp));
      sp.sched_priority = dspd_dctx.rtsvc_priority;
      sched_setscheduler(tid, dspd_dctx.rtsvc_policy, &sp);
    }
  dspd_daemon_set_thread_nice(tid, thread_type);
}

static int solib_filter(const struct dirent *de)
{
  const char *p;
  int ret = 0;
  if ( strncmp(de->d_name, "mod_", 4) == 0 )
    {
      p = strrchr(de->d_name, '.');
      if ( p != NULL && strcmp(p, ".so") == 0 )
	ret = 1;
    }
  return ret;
}


//Generate the default modules list since no modules section in the config was found.
static struct dspd_dict *generate_modules_list(void)
{
  int count, i;
  struct dspd_dict *dict = dspd_dict_new("MODULES");
  struct dirent **namelist;
  char name[NAME_MAX];
  int e = 0;
  char *p;
  if ( dict )
    {
      count = scandir(dspd_get_modules_dir(), &namelist, solib_filter, alphasort);
      if ( count > 0 )
	{
	  for ( i = 0; i < count; i++ )
	    {
	      if ( dict != NULL )
		{
		  strlcpy(name, &namelist[i]->d_name[4U], sizeof(name));
		  p = strrchr(name, '.');
		  DSPD_ASSERT(p != NULL);
		  *p = 0;
		  if ( ! dspd_dict_set_value(dict, name, namelist[i]->d_name, true) )
		    {
		      dspd_dict_free(dict);
		      dict = NULL;
		      e = errno;
		    }
		}
	      free(namelist[i]);
	    }
	  free(namelist);
	  if ( dict == NULL )
	    errno = e;
	} else
	{
	  dspd_dict_free(dict);
	  dict = NULL;
	}
    }
  return dict;
}

int dspd_daemon_init(int argc, char **argv)
{
  int ret = -ENOMEM;
  char *tmp = malloc(PATH_MAX);
  size_t len;
  ssize_t l;
  char *p, *value;
  int pr, pf = 0, mxf, mnf, mxr, mnr, n;
  struct dspd_dict *dcfg;
  char *val;
  char *pwbuf = NULL;
  ssize_t pwsize = sysconf(_SC_GETPW_R_SIZE_MAX);
  struct rlimit rl;
  if ( pwsize == -1 )
    pwsize = 16384;
  if ( ! tmp )
    return -ENOMEM;
 
  dspd_dctx.argc = argc;
  dspd_dctx.argv = argv;

  dspd_dctx.rtio_priority = -1;
  dspd_dctx.rtsvc_priority = -1;
  dspd_dctx.glitch_correction = -1;
  dspd_dctx.uid = getuid();
  dspd_dctx.gid = getgid();
  dspd_dctx.ipc_mode = 0660;
  
  if ( have_sched_iso() )
    {
      dspd_dctx.rtio_policy = SCHED_ISO;
      dspd_dctx.rtsvc_policy = SCHED_ISO;
    } else
    {
      dspd_dctx.rtio_policy = SCHED_FIFO;
      dspd_dctx.rtsvc_policy = SCHED_RR;
    }

  dspd_dctx.main_thread_loop_context = calloc(1, sizeof(*dspd_dctx.main_thread_loop_context));
  if ( ! dspd_dctx.main_thread_loop_context )
    {
      ret = -errno;
      goto out;
    }

  if ( (ret = cbpoll_init(dspd_dctx.main_thread_loop_context, CBPOLL_FLAG_TIMER|CBPOLL_FLAG_AIO_FIFO|CBPOLL_FLAG_CBTIMER, DSPD_MAX_OBJECTS * 4UL)) < 0 )
    goto out;
  if ( (ret = cbpoll_set_name(dspd_dctx.main_thread_loop_context, "")) < 0 )
    goto out;

  if ( (ret = dspd_sglist_new(&dspd_dctx.syncgroups)) )
    goto out;


  dspd_dctx.args_list = dspd_parse_args(argc, argv);
  if ( ! dspd_dctx.args_list )
    {
      ret = -ENOMEM;
      goto out;
    }
  val = NULL;
  dspd_dict_find_value(dspd_dctx.args_list, "-D", (char**)&val);
  if ( val )
    dspd_dctx.debug = dspd_strtoidef(val, dspd_dctx.debug);


  l = readlink("/proc/self/exe", tmp, PATH_MAX-1);
  if ( l < 0 )
    {
      ret = -errno;
      goto out;
    }
  tmp[l] = 0;


  p = dirname(tmp);
  len = strlen(p);
  memmove(tmp, p, len);
  tmp[len] = 0;
  len++;
  dspd_dctx.path = realloc(tmp, len);
  if ( ! dspd_dctx.path )
    dspd_dctx.path = tmp;
  tmp = malloc(PATH_MAX);
  if ( ! tmp )
    {
      ret = -errno;
      goto out;
    }

  if ( dspd_dict_test_value(dspd_dctx.args_list, "-h", NULL) ||
       dspd_dict_test_value(dspd_dctx.args_list, "-?", NULL) )
    {
      free(tmp);
      print_usage();
      return -1;
    }
  if ( dspd_dict_test_value(dspd_dctx.args_list, "-b", NULL) )
    {
      if ( daemon(0, 0) < 0 )
	return -1;
    }

  dspd_dctx.config = dspd_read_config("dspd", true);
  if ( dspd_dctx.config == NULL && errno == ENOENT )
    dspd_dctx.config = dspd_dict_new("dspd");
  if ( ! dspd_dctx.config )
    goto out;
  

  dcfg = dspd_dict_find_section(dspd_dctx.config, "DAEMON");
  if ( ! dcfg )
    goto noconfig;

  if ( dspd_dict_find_value(dcfg, "glitch_correction", &value) )
    {
      if ( value )
	dspd_dctx.glitch_correction = get_gp(value);
    }

  if ( dspd_dict_find_value(dcfg, "rtprio", &value) )
    {
      if ( value )
	{
	  n = -1;
	  set_priority(value, &n);
	  if ( n > 0 )
	    {
	      dspd_dctx.rtio_priority = n;
	      dspd_dctx.rtsvc_priority = n;
	      if ( dspd_dctx.rtsvc_priority > 1 )
		dspd_dctx.rtsvc_priority--;
	    }
	}
    }


  if ( dspd_dict_find_value(dcfg, "rtpolicy", &value) )
    {
      if ( value )
	{
	  n = -1;
	  set_policy(value, &n);
	  if ( n > 0 )
	    {
	      dspd_dctx.rtio_policy = n;
	      dspd_dctx.rtsvc_policy = n;
	    }
	}
    }
  if ( dspd_dict_find_value(dcfg, "hiprio", &value) )
    {
      if ( value )
	{
	  n = INT32_MIN;
	  set_priority(value, &n);
	  if ( n != INT32_MIN && n < 0 )
	    {
	      dspd_dctx.priority = ret;
	      rl.rlim_cur = 20 + n;
	      rl.rlim_max = rl.rlim_cur;
	      setrlimit(RLIMIT_NICE, &rl);
	    }
	}
    }


  if ( dspd_dict_find_value(dcfg, "rtio_policy", &value) )
    {
      if ( value )
	set_policy(value, &dspd_dctx.rtio_policy);
    }

  if ( dspd_dict_find_value(dcfg, "rtio_priority", &value) )
    {
      if ( value )
	set_priority(value, &dspd_dctx.rtio_priority);
    }

  if ( dspd_dict_find_value(dcfg, "rtsvc_policy", &value) )
    {
      if ( value )
	set_policy(value, &dspd_dctx.rtsvc_policy);
    }
  if ( dspd_dict_find_value(dcfg, "rtsvc_priority", &value) )
    {
      if ( value )
	set_priority(value, &dspd_dctx.rtsvc_priority);
    }

  rl.rlim_cur = MAX(dspd_dctx.rtsvc_priority, dspd_dctx.rtio_priority);
  if ( rl.rlim_cur )
    {
      rl.rlim_max = rl.rlim_cur;
      setrlimit(RLIMIT_RTPRIO, &rl);
    }

  pwbuf = malloc(pwsize);
  if ( ! pwbuf )
    {
      ret = -errno;
      goto out;
    }
  struct passwd pwd;
  struct passwd *pres;
  if ( dspd_dict_find_value(dcfg, "user", &value) )
    {
      ret = getpwnam_r(value, &pwd, pwbuf, pwsize, &pres);
      if ( pres == NULL )
	{
	  if ( ret == 0 )
	    {
	      fprintf(stderr, "Invalid user - %s\n", value);
	      ret = -EPERM;
	      goto out;
	    } else
	    {
	      ret = -errno;
	      perror("getpwnam_r");
	      goto out;
	    }
	} else
	{
	  dspd_dctx.uid = pres->pw_uid;
	  dspd_dctx.gid = pres->pw_gid;
	  dspd_dctx.user = strdup(value);
	  if ( ! dspd_dctx.user )
	    {
	      ret = -errno;
	      goto out;
	    }
	}
    }
  struct group grp, *gres;
  if ( dspd_dict_find_value(dcfg, "group", &value) )
    {
      ret = getgrnam_r(value, &grp, pwbuf, pwsize, &gres);
      if ( gres == NULL )
	{
	  if ( ret == 0 )
	    {
	      fprintf(stderr, "Invalid group - %s\n", value);
	      ret = -EPERM;
	      goto out;
	    } else
	    {
	      ret = -errno;
	      perror("getgrnam_r");
	      goto out;
	    }
	} else
	{
	  dspd_dctx.gid = gres->gr_gid;
	}
    }
  if ( dspd_dctx.uid == 0 && dspd_dctx.gid == 0 )
    dspd_dctx.ipc_mode = 0666;

  if ( dspd_dict_find_value(dcfg, "ipc_mode", &value) )
    {
      int32_t m;
      ret = dspd_strtoi32(value, &m, 8);
      if ( ret < 0 )
	{
	  fprintf(stderr, "Invalid mode - %s\n", value);
	  goto out;
	}
      dspd_dctx.ipc_mode = m;
    }
  
  if ( (dspd_dctx.rtsvc_policy == SCHED_RR ||
	dspd_dctx.rtsvc_policy == SCHED_FIFO ||
	dspd_dctx.rtio_policy == SCHED_RR ||
	dspd_dctx.rtio_policy == SCHED_FIFO) &&
       dspd_dctx.uid > 0 )
    {
      ret = set_caps();
      if ( ret < 0 )
	goto out;
    }

 noconfig:
  if ( dcfg == NULL )
    dspd_dctx.ipc_mode = 0666;
  if ( dspd_dctx.uid == 0 )
    dspd_log(0, "WARNING: Running as root.  This is not recommended.");
    

  if ( dcfg != NULL && dspd_dict_find_value(dcfg, "modules", &value) != 0 )
    {
      dspd_dctx.modules_dir = strdup(value);
    } else
    {
      p = realpath(argv[0], tmp);
      if ( p )
	{
	  p = strrchr(p, '/');
	  if ( p != NULL && p != tmp )
	    {
	      *p = 0;
	      p = strrchr(tmp, '/');
	      if ( p != NULL )
		{
		  strcpy(p, "/lib"LIBSUFFIX"/dspd");
		  dspd_dctx.modules_dir = strdup(tmp);
		  if ( ! dspd_dctx.modules_dir )
		    {
		      ret = -ENOMEM;
		      goto out;
		    }
		}
	    }
	}
    }
  
  DSPD_ASSERT(dspd_dctx.modules_dir != NULL);
  if ( ! dspd_dict_find_section(dspd_dctx.config, "MODULES") )
    {
      struct dspd_dict *curr;
      for ( curr = dspd_dctx.config; curr; curr = curr->next )
	{
	  if ( curr->next == NULL )
	    {
	      curr->next = generate_modules_list();
	      if ( curr->next )
		curr->next->prev = curr;
	      break;
	    }
	}
    }
 

  /*
    Use sane defaults if the priorities are not specified.  This means
    half of maximum priority and device io threads have higher priority than
    other threads that are in the hot path between the client and io thread.
  */
  if ( dspd_dctx.rtio_priority == -1 )
    {
      mnf = sched_get_priority_min(dspd_dctx.rtio_policy);
      mxf = sched_get_priority_max(dspd_dctx.rtio_policy);
      if ( mnf < 0 || mxf < 0 )
	{

	  goto out;
	}
      pf = mxf - mnf;
      if ( pf > 1 )
	pf /= 2;
      pf += mnf;
      dspd_dctx.rtio_priority = pf;
    } else
    {
      mxf = sched_get_priority_max(dspd_dctx.rtio_policy);
      if ( dspd_dctx.rtio_priority > mxf )
	dspd_dctx.rtio_priority = mxf;
    }

  if ( dspd_dctx.rtsvc_priority == -1 )
    {
      mnr = sched_get_priority_min(dspd_dctx.rtsvc_policy);
      mxr = sched_get_priority_max(dspd_dctx.rtsvc_policy);
      if ( mnr < 0 || mxr < 0 )
	{
	  ret = errno;
	  goto out;
	}
      pr = pf - 1;
      if ( pr < mnr || pr > mxr )
	{
	  pr = mxr - mnr;
	  if ( pr > 1 )
	    pr /= 2;
	  pr += mnr;
	}
      dspd_dctx.rtsvc_priority = pr;
    } else 
    {
      mxr = sched_get_priority_max(dspd_dctx.rtio_policy);
      if ( dspd_dctx.rtsvc_priority > mxr )
	dspd_dctx.rtsvc_priority = mxr;
    }

  if ( dspd_dctx.glitch_correction == -1 )
    dspd_dctx.glitch_correction = DSPD_GHCN_OFF;


  dspd_log(0, "Set scheduler (policy,priority): rtio=(%d,%d) rtsvc=(%d,%d)",
	   dspd_dctx.rtio_policy, dspd_dctx.rtio_priority,
	   dspd_dctx.rtsvc_policy, dspd_dctx.rtsvc_priority);
  dspd_log(0, "Glitch correction policy is %d", dspd_dctx.glitch_correction);

  ret = dspd_hotplug_init(&dspd_dctx);
  if ( ret )
    goto out;
  dspd_dctx.objects = dspd_slist_new(DSPD_MAX_OBJECTS);
  if ( ! dspd_dctx.objects )
    {
      ret = -ENOMEM;
      goto out;
    }
  DSPD_ASSERT(dspd_dctx.objects != NULL);
  

  dspd_slist_entry_set_used(dspd_dctx.objects, 0, true);
  dspd_slist_entry_set_pointers(dspd_dctx.objects,
			   0,
			   &dspd_dctx,
			   NULL,
			   NULL);
  dspd_slist_ref(dspd_dctx.objects, 0);
  dspd_slist_set_ctl(dspd_dctx.objects,
		     0,
		     dspd_daemon_obj_ctl);
  dspd_dctx.ctl_ops = &daemon_rcb;
  dspd_dctx.magic = DSPD_OBJ_TYPE_DAEMON_CTX;
  
 

  /*tmp = malloc(PATH_MAX);
  if ( ! tmp )
  goto out;*/
  if ( dspd_dctx.modules_dir == NULL )
    {
      if ( readlink("/proc/self/exe", tmp, PATH_MAX) > 0 &&
	   (p = strrchr(tmp, '/')))
	{
	  *p = 0;
	  strcat(p, "/../lib"LIBSUFFIX"/dspd");
	  dspd_dctx.modules_dir = strdup(tmp);
	  tmp = NULL;
	} else
	{
	  
	  if ( sizeof(void*) == 8 )
	    dspd_dctx.modules_dir = strdup("/usr/lib64/dspd");
	  else
	    dspd_dctx.modules_dir = strdup("/usr/lib/dspd");
	}
      if ( ! dspd_dctx.modules_dir )
	{
	  ret = -ENOMEM;
	  goto out;
	}
    }
  dspd_log(0, "Modules directory is '%s'", dspd_dctx.modules_dir);
  
  ret = dspd_vctrl_list_new(&dspd_dctx.vctrl);
  if ( ret < 0 )
    goto out;


  ret = 0;
  
  
 out:
  free(tmp);
  free(pwbuf);
  if ( ret != 0 )
    {
      //This is where cleanup should happen.
      //It isn't implemented yet because the daemon just exits and everything gets cleaned up anyway.
    }
  return ret;
}



static struct dspd_ll *dspd_daemon_hotplug_last(const struct dspd_hotplug_cb *callbacks,
						void *arg)
{
  struct dspd_ll *curr, *prev = NULL;
  struct dspd_hotplug_handler *h;
  for ( curr = dspd_dctx.hotplug.handlers; curr; curr = curr->next )
    {
      h = curr->pointer;
      if ( h->callbacks == callbacks && h->arg == arg )
	return curr;
      prev = curr;
    }
  return prev;
}


int dspd_daemon_hotplug_register(const struct dspd_hotplug_cb *callbacks,
				 void *arg)
{
  struct dspd_ll *last;
  int ret = 0;
  struct dspd_hotplug_handler *h = NULL, *lh;
  pthread_mutex_lock(&dspd_dctx.hotplug.lock);
  last = dspd_daemon_hotplug_last(callbacks, arg);
  if ( ! last )
    {
      h = calloc(1, sizeof(*h));
      if ( ! h )
	goto out;
      dspd_dctx.hotplug.handlers = dspd_ll_new(h);
      if ( ! dspd_dctx.hotplug.handlers )
	goto out;
    } else 
    {
      lh = last->pointer;
      if ( !(lh->callbacks == callbacks && lh->arg == arg))
	{
	  h = calloc(1, sizeof(*h));
	  if ( ! h )
	    goto out;
	  if ( ! dspd_ll_append(dspd_dctx.hotplug.handlers, h) )
	    goto out;
	} else
	{
	  errno = EEXIST;
	  goto out;
	}
    }
  h->callbacks = callbacks;
  h->arg = arg;
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  return ret;

 out:
  ret = -errno;
  free(h);
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  return ret;
}

int dspd_daemon_hotplug_unregister(const struct dspd_hotplug_cb *callbacks,
				   void *arg)
{
  struct dspd_ll *curr;
  struct dspd_hotplug_handler *h;
  int ret = -ENOENT;
  pthread_mutex_lock(&dspd_dctx.hotplug.lock);
  for ( curr = dspd_dctx.hotplug.handlers; curr; curr = curr->next )
    {
      h = curr->pointer;
      if ( h->callbacks == callbacks && h->arg == arg )
	{
	  if ( curr->prev )
	    curr->prev->next = curr->next;
	  if ( curr->next )
	    curr->next->prev = curr->prev;
	  free(curr);
	  break;
	}
    }
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  return ret;
}

int32_t dspd_daemon_ref_by_name(const char *hwname, int32_t sbits, int32_t *playback, int32_t *capture)
{
  struct dspd_hotplug_devname *dev;
  int32_t ret = -ENOENT, s, pslot = -1, cslot = -1;
  uint64_t p_eid = 0, c_eid = 0;
  pthread_mutex_lock(&dspd_dctx.hotplug.lock);
  if ( playback )
    *playback = -1;
  if ( capture )
    *capture = -1;
  if ( strcmp(hwname, "default") == 0 )
    {
      if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
	{
	  pslot = dspd_dctx.hotplug.default_playback;
	  if ( pslot < 0 )
	    goto out;
	}
      if ( sbits & DSPD_PCM_SBIT_CAPTURE )
	{
	  cslot = dspd_dctx.hotplug.default_capture;
	  if ( cslot < 0 )
	    goto out;
	}
      if ( sbits == DSPD_PCM_SBIT_FULLDUPLEX && pslot < 0 )
	goto out;
    }
  for ( dev = dspd_dctx.hotplug.names; dev; dev = dev->next )
    {
      s = dev->sbits & sbits;
      if ( (s & DSPD_PCM_SBIT_PLAYBACK) && pslot >= 0 && dev->slot != pslot )
	continue;
      if ( (s & DSPD_PCM_SBIT_CAPTURE) && cslot >= 0 && dev->slot != cslot )
	continue;

      if ( s != 0 && (pslot >= 0 || cslot >= 0 || dspd_devname_cmp(dev->hwname, hwname) == true) )
	{
	  ret = dspd_daemon_ref(dev->slot, DSPD_DCTL_ENUM_TYPE_SERVER);
	  if ( ret == 0 )
	    {
	      if ( (s & DSPD_PCM_SBIT_PLAYBACK) && playback != NULL )
		{
		  *playback = dev->slot;
		  sbits &= ~DSPD_PCM_SBIT_PLAYBACK;
		  p_eid = dev->event_id;
		}
	      if ( (s & DSPD_PCM_SBIT_CAPTURE) && capture != NULL )
		{
		  *capture = dev->slot;
		  sbits &= ~DSPD_PCM_SBIT_CAPTURE;
		  c_eid = dev->event_id;
		  if ( s == DSPD_PCM_SBIT_FULLDUPLEX )
		    {
		      ret = dspd_daemon_ref(dev->slot, DSPD_DCTL_ENUM_TYPE_SERVER);
		      if ( ret < 0 )
			*capture = -1;
		    }
		}
	    }
	  if ( sbits == 0 )
	    break;
	}
    }
  if ( sbits != 0 )
    {
      //Did not find all streams
      ret = -ENOENT;
      if ( playback != NULL && *playback >= 0 )
	{
	  dspd_daemon_unref(*playback);
	  *playback = -1;
	}
      if ( capture != NULL && *capture >= 0 )
	{
	  dspd_daemon_unref(*capture);
	  *capture = -1;
	}
    } else if ( playback != NULL && capture != NULL && ret == 0 )
    {
      if ( p_eid != c_eid && p_eid != 0 && c_eid != 0 )
	{
	  if ( *playback >= 0 )
	    {
	      dspd_daemon_unref(*playback);
	      *playback = -1;
	    }
	  if ( *capture >= 0 )
	    {
	      dspd_daemon_unref(*capture);
	      *capture = -1;
	    }
	  ret = -ENOENT;
	}
    }

 out:
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  return ret;
}

static void remove_device(struct dspd_hotplug *hotplug, uint64_t event_id, const char *hwname)
{
  struct dspd_hotplug_devname *dev, **prev = &hotplug->names;
  bool found;
  do {
    found = false;
    for ( dev = hotplug->names; dev; dev = dev->next )
      {
	if ( dev->event_id == event_id && strcmp(dev->hwname, hwname) == 0 )
	  {
	    *prev = dev->next;
	    free(dev);
	    found = true;
	    break;
	  }
	prev = &dev->next;
      }
  } while ( found );
}

static void init_device_notify(struct dspd_hotplug *hotplug, const struct dspd_dict *device)
{
  struct dspd_ll *curr;
  struct dspd_hotplug_handler *h;
  struct dspd_hotplug_devname *dev;
  const char *slot, *stream, *eid, *name;
  dev = calloc(1, sizeof(*dev));
  if ( dev )
    {
      name = dspd_dict_value_for_key(device, DSPD_HOTPLUG_DEVNAME);
      stream = dspd_dict_value_for_key(device, DSPD_HOTPLUG_STREAM);
      slot = dspd_dict_value_for_key(device, DSPD_HOTPLUG_SLOT);
      eid = dspd_dict_value_for_key(device, DSPD_HOTPLUG_EVENT_ID);
      if ( name && stream && slot && eid )
	{
	  int sbits = 0;
	  if ( strcmp(stream, "playback") == 0 )
	    sbits = DSPD_PCM_SBIT_PLAYBACK;
	  else if ( strcmp(stream, "capture") == 0 )
	    sbits = DSPD_PCM_SBIT_CAPTURE;
	  else if ( strcmp(stream, "fullduplex") == 0 )
	    sbits = DSPD_PCM_SBIT_FULLDUPLEX;
	  if ( sbits != 0 &&
	       strlcpy(dev->hwname, name, sizeof(dev->hwname)) < sizeof(dev->hwname) &&
	       dspd_strtou64(eid, &dev->event_id, 0) == 0 &&
	       dspd_strtoi32(slot, &dev->slot, 0) == 0 )
	    {
	      dev->next = hotplug->names;
	      dev->sbits = sbits;
	      dev->info = device;
	      hotplug->names = dev;
	    } else
	    {
	      free(dev);
	    }
	} else
	{
	  free(dev);
	}
    }
  for ( curr = hotplug->handlers; curr; curr = curr->next )
    {
      h = curr->pointer;
      if ( h->callbacks->init_device != NULL )
	h->callbacks->init_device(h->arg, device);
    }
}


static void dspd_hotplug_updatedefault(struct dspd_dict *dict)
{
  if ( ! refresh_default_device_dict(&dspd_dctx.hotplug) )
    refresh_default_device_name(&dspd_dctx.hotplug);
  dspd_log(0, "Default devices: playback=%d capture=%d", 
	   dspd_dctx.hotplug.default_playback, dspd_dctx.hotplug.default_capture);
}

static bool str_equal(const char *str1, const char *str2)
{
  if ( str1 == NULL && str2 == NULL )
    return true;
  if ( (str1 == NULL && str2 != NULL) || (str1 != NULL && str2 == NULL) )
    return false;
  return strcmp(str1, str2) == 0;
}

static int get_stream(const char *str)
{
  int ret;
  if ( str_equal(str, "fullduplex") )
    ret = DSPD_PCM_SBIT_FULLDUPLEX;
  else if ( str_equal(str, "playback") )
    ret = DSPD_PCM_SBIT_PLAYBACK;
  else if ( str_equal(str, "capture") )
    ret = DSPD_PCM_SBIT_CAPTURE;
  else
    ret = 0;
  return ret;
}
static bool check_key(const struct dspd_dict *sect, const char *key, const char *value)
{
  return str_equal(dspd_dict_value_for_key(sect, key), value);
}
static bool check_duplicates(const struct dspd_dict *dict)
{
  struct dspd_dict *curr;
  const char *bus, *name, *addr, *desc, *s, *driver;
  int stream, stream2;
  bool ret = false;
  bus = dspd_dict_value_for_key(dict, DSPD_HOTPLUG_BUSNAME);
  name = dspd_dict_value_for_key(dict, DSPD_HOTPLUG_DEVNAME);
  addr = dspd_dict_value_for_key(dict, DSPD_HOTPLUG_ADDRESS);
  desc = dspd_dict_value_for_key(dict, DSPD_HOTPLUG_DESC);
  s = dspd_dict_value_for_key(dict, DSPD_HOTPLUG_STREAM);
  stream = get_stream(s);
  driver = dspd_dict_value_for_key(dict, DSPD_HOTPLUG_KDRIVER);
  


  for ( curr = dspd_dctx.hotplug.devices; curr; curr = curr->next )
    {
      s = dspd_dict_value_for_key(curr, DSPD_HOTPLUG_STREAM);
      stream2 = get_stream(s);
      if ( ! (stream & stream2) )
	continue;
      if ( check_key(curr, DSPD_HOTPLUG_BUSNAME, bus) &&
	   check_key(curr, DSPD_HOTPLUG_DEVNAME, name) &&
	   check_key(curr, DSPD_HOTPLUG_ADDRESS, addr) &&
	   check_key(curr, DSPD_HOTPLUG_DESC, desc) &&
	   check_key(curr, DSPD_HOTPLUG_KDRIVER, driver) )
	{
	  ret = true;
	  break;
	}
    }
  return ret;
}

uint64_t dspd_daemon_hotplug_event_id(char buf[32UL])
{
  static uint64_t last_id = UINT16_MAX;
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  uint64_t ret;
  pthread_mutex_lock(&lock);
  last_id++;
  ret = last_id;
  pthread_mutex_unlock(&lock);
  if ( buf )
    snprintf(buf, 32UL, "0x%llx", (long long)ret);
  return ret;
}

int dspd_daemon_hotplug_add(const struct dspd_dict *dict)
{
  int ret = -ENOENT;
  struct dspd_ll *curr;
  struct dspd_hotplug_handler *h, *sh = NULL;
  int r, score = -1;
  struct dspd_dict *kvs = NULL, *c, *prev;
  struct dspd_kvpair *p;
  pthread_mutex_lock(&dspd_dctx.hotplug.lock);
  if ( check_duplicates(dict) )
    {
      ret = -EEXIST;
      goto out;
    }
  for ( curr = dspd_dctx.hotplug.handlers; curr; curr = curr->next )
    {
      h = curr->pointer;
      if ( h->callbacks->add && h->callbacks->score )
	{
	  r = h->callbacks->score(h->arg, dict);
	  if ( r > 0 )
	    {
	      if ( r > score )
		{
		  score = r;
		  sh = h;
		}
	    }
	}
    }
  if ( score > 0 && sh != NULL )
    {
      kvs = dspd_dict_dup(dict);
      if ( kvs )
	{
	  if ( dspd_dict_insert_value(kvs, DSPD_HOTPLUG_SLOT, "0000") )
	    {
	      p = dspd_dict_find_pair(kvs, DSPD_HOTPLUG_SLOT);
	      DSPD_ASSERT(p);
	      r = sh->callbacks->add(sh->arg, dict);
	      if ( r >= 0 && r < DSPD_MAX_OBJECTS )
		{
		  sprintf(p->value, "%d", r);
		  if ( dspd_dctx.hotplug.devices )
		    {
		      prev = NULL;
		      for ( c = dspd_dctx.hotplug.devices; c; c = c->next )
			prev = c;
		      prev->next = kvs;
		      kvs->prev = prev;
		    } else
		    {
		      dspd_dctx.hotplug.devices = kvs;
		    }
		  ret = r;
		} else
		{
		  dspd_dict_free(kvs);
		  kvs = NULL;
		}
	    } else
	    {
	      ret = -ENOMEM;
	    }
	} else
	{
	  ret = -ENOMEM;
	}
      if ( ret >= 0 )
	init_device_notify(&dspd_dctx.hotplug, kvs);
      else
	kvs = NULL;
    }
  if ( ret >= 0 )
    dspd_hotplug_updatedefault(kvs);

 out:
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  return ret;
}

/*
  Look for one or more devices identified by dict or name.

  Return values:
  -ENOENT    Device not found
  -ENODEV    Device was found but nobody owns it
  -EBUSY     Device was found, someone owns it, but it is busy right now.
*/
int dspd_daemon_hotplug_remove(const struct dspd_dict *dict, const char *name)
{
  int ret = -ENOENT;
  struct dspd_dict *curr, *c;
  struct dspd_ll *l;
  struct dspd_hotplug_handler *h;
  int r;
  pthread_mutex_lock(&dspd_dctx.hotplug.lock);
  curr = dspd_dctx.hotplug.devices;
  do { 
    for ( ; curr; curr = curr->next )
      {
	if ( dict )
	  {
	    if ( dspd_dict_compare(dict, curr) )
	      break;
	  } else if ( name )
	  {
	    if ( check_key(curr, DSPD_HOTPLUG_DEVNAME, name) )
	      break;
	  }
      }
    if ( curr )
      {
	for ( l = dspd_dctx.hotplug.handlers; l; l = l->next )
	  {
	    h = l->pointer;
	    if ( h->callbacks->remove )
	      {
		r = h->callbacks->remove(h->arg, curr);
		if ( r == -EBUSY || r == 0 )
		  {
		    ret = r;
		  }
	      }
	  }
	if ( ret == 0 )
	  {
	    if ( curr->prev )
	      curr->prev->next = curr->next;
	    else
	      dspd_dctx.hotplug.devices = curr->next;
	    if ( curr->next )
	      curr->next->prev = curr->prev;
	    c = curr->next;
	    curr->next = NULL;
	    curr->prev = NULL;

	    const char *eid = dspd_dict_value_for_key(curr, DSPD_HOTPLUG_EVENT_ID);
	    const char *hwn = dspd_dict_value_for_key(curr, DSPD_HOTPLUG_DEVNAME);
	    if ( eid && hwn )
	      {
		uint64_t e;
		if ( dspd_strtou64(eid, &e, 0) == 0 )
		  remove_device(&dspd_dctx.hotplug, e, hwn);
	      }

	    dspd_dict_free(curr);
	    curr = c;
	  } else
	  {
	    curr = curr->next;
	  }
      }
  } while ( curr );
  

  
  dspd_hotplug_updatedefault(NULL);
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  return ret;
}


int dspd_hotplug_delete(const struct dspd_dict *dict)
{
  int ret = -ENOENT;
  struct dspd_dict *curr;
  pthread_mutex_lock(&dspd_dctx.hotplug.lock);
  for ( curr = dspd_dctx.hotplug.devices; curr; curr = curr->next )
    {
      if ( dspd_dict_compare(dict, curr) )
	{
	  if ( curr->prev )
	    curr->prev->next = curr->next;
	  if ( curr->next )
	    curr->next->prev = curr->prev;
	  curr->prev = NULL;
	  curr->next = NULL;
	  dspd_dict_free(curr);
	  ret = 0;
	  break;
	}
    }
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  return ret;
}

static void remove_spaces(char *str)
{
  size_t i = 0;
  while ( str[i] )
    {
      if ( isspace(str[i]) )
	str[i] = '_';
      i++;
    }
}

static struct dspd_dict *find_config(struct dspd_drv_params *params, const struct dspd_dict *hotplug)
{
  char buf[PATH_MAX];
  struct dspd_dict *dict = NULL;
  char *ptr = NULL;
  if ( dspd_strlen_safe(params->hwid) > 0 )
    {
      sprintf(buf, "hwcfg/hwid-%s", params->hwid);
      remove_spaces(buf);
      dict = dspd_read_config(buf, true);
    }
  if ( dict == NULL && dspd_strlen_safe(params->addr) > 0 )
    {
      sprintf(buf, "hwcfg/addr-%s", params->addr);
      dict = dspd_read_config(buf, true);
    }
  if ( dict == NULL && dspd_strlen_safe(params->bus) > 0 )
    {
      sprintf(buf, "hwcfg/bus-%s", params->bus);
      remove_spaces(buf);
      dict = dspd_read_config(buf, true);
    }
  if ( dict == NULL && dspd_strlen_safe(params->name) > 0 )
    {
      sprintf(buf, "hwcfg/name-%s", params->name);
      remove_spaces(buf);
      dict = dspd_read_config(buf, true);
    }
  if ( dict == NULL && dspd_strlen_safe(params->desc) > 0 )
    {
      sprintf(buf, "hwcfg/desc-%s", params->desc);
      remove_spaces(buf);
      dict = dspd_read_config(buf, true);
    }
  if ( dict == NULL && hotplug != NULL )
    {
      if ( dspd_dict_find_value(hotplug, DSPD_HOTPLUG_MODALIAS, &ptr) )
	{
	  sprintf(buf, "hwcfg/mod-%s", ptr);
	  dict = dspd_read_config(buf, true);
	}
      if ( dict == NULL && dspd_dict_find_value(hotplug, DSPD_HOTPLUG_KDRIVER, &ptr) == true )
	{
	  sprintf(buf, "hwcfg/kdrv-%s", ptr);
	  dict = dspd_read_config(buf, true);
	}
      if ( dict == NULL && dspd_dict_find_value(hotplug, DSPD_HOTPLUG_DEVTYPE, &ptr) == true )
	{
	  sprintf(buf, "hwcfg/type-%s", ptr);
	  dict = dspd_read_config(buf, true);
	}
    }
  if ( dict == NULL )
    dict = dspd_read_config("hwcfg/default", true);
  return dict;
}

static void apply_config(struct dspd_dict *dict, struct dspd_drv_params *params)
{
  char *ptr;
  int32_t val;
  if ( dspd_dict_find_value(dict, "format", &ptr) )
    {
      if ( dspd_strtoi32(ptr, &val, 0) < 0 )
	val = dspd_pcm_format_from_name(ptr);
      if ( val != DSPD_PCM_FORMAT_UNKNOWN )
	params->format = val;
    }
  if ( dspd_dict_find_value(dict, "channels", &ptr) )
    {
      if ( dspd_strtoi32(ptr, &val, 0) == 0 )
	params->channels = val;
    }
  if ( dspd_dict_find_value(dict, "rate", &ptr) )
    {
      if ( dspd_strtoi32(ptr, &val, 0) == 0 )
	params->rate = val;
    }
  if ( dspd_dict_find_value(dict, "fragsize", &ptr) )
    {
      if ( dspd_strtoi32(ptr, &val, 0) == 0 )
	params->fragsize = val;
    }
  if ( dspd_dict_find_value(dict, "bufsize", &ptr) )
    {
      if ( dspd_strtoi32(ptr, &val, 0) == 0 )
	params->bufsize = val;
    }
  if ( dspd_dict_find_value(dict, "min_latency", &ptr) )
    {
      if ( dspd_strtoi32(ptr, &val, 0) == 0 )
	params->min_latency = val;
    }
  if ( dspd_dict_find_value(dict, "max_latency", &ptr) )
    {
      if ( dspd_strtoi32(ptr, &val, 0) == 0 )
	params->max_latency = val;
    }
  if ( dspd_dict_find_value(dict, "min_dma", &ptr) )
    {
      if ( dspd_strtoi32(ptr, &val, 0) == 0 )
	params->min_dma = val;
    }
}


int dspd_daemon_get_config(const struct dspd_dict *dict,
			   struct dspd_drv_params *params)
{
  char *ptr;
  int ret;
  struct dspd_dict *cfg = NULL, *d;
  memset(params, 0, sizeof(*params));
  if ( dspd_dict_find_value(dict, DSPD_HOTPLUG_BUSNAME, &ptr) )
    {
      params->bus = strdup(ptr);
      if ( ! params->bus )
	goto out;
    }
  if ( dspd_dict_find_value(dict, DSPD_HOTPLUG_DEVNAME, &ptr) )
    {
      params->name = strdup(ptr);
      if ( ! params->name )
	goto out;
    }
  if ( dspd_dict_find_value(dict, DSPD_HOTPLUG_ADDRESS, &ptr) )
    {
      params->addr = strdup(ptr);
      if ( ! params->addr )
	goto out;
    }

  if ( dspd_dict_find_value(dict, DSPD_HOTPLUG_DESC, &ptr) )
    {
      params->desc = strdup(ptr);
      if ( ! params->addr )
	goto out;
    }
  if ( dspd_dict_find_value(dict, DSPD_HOTPLUG_HWID, &ptr) )
    {
      params->hwid = strdup(ptr);
      if ( ! params->addr )
	goto out;
    }

  if ( dspd_dict_find_value(dict, DSPD_HOTPLUG_STREAM, &ptr) )
    {
      if ( strcmp(ptr, "playback") == 0 )
	{
	  params->stream = DSPD_PCM_STREAM_PLAYBACK;
	} else if ( strcmp(ptr, "capture") == 0 )
	{
	  params->stream = DSPD_PCM_STREAM_CAPTURE;
	} else if ( strcmp(ptr, "fullduplex") == 0 )
	{
	  params->stream = DSPD_PCM_STREAM_FULLDUPLEX;
	} else
	{
	  errno = EINVAL;
	  goto out;
	}
    }
  
  params->format = DSPD_PCM_FORMAT_S16_LE;
  params->channels = 2;
  params->rate = 48000;
  params->bufsize = 16384;
  params->fragsize = 4096;

  cfg = find_config(params, dict);
  if ( cfg )
    {
      if ( params->stream == DSPD_PCM_STREAM_PLAYBACK || params->stream == DSPD_PCM_STREAM_FULLDUPLEX )
	{
	  d = dspd_dict_find_section(cfg, "PLAYBACK");
	  if ( d )
	    apply_config(d, params);
	} 
      if ( params->stream == DSPD_PCM_STREAM_CAPTURE || params->stream == DSPD_PCM_STREAM_FULLDUPLEX )
	{
	  d = dspd_dict_find_section(cfg, "CAPTURE");
	  if ( d )
	    apply_config(d, params);
	}
      dspd_dict_free(cfg);
    }

  return 0;
 out:
  ret = -errno;
  free(params->addr);
  free(params->name);
  free(params->bus);
  free(params->desc);
  memset(params, 0, sizeof(*params));
  dspd_dict_free(cfg);
  return ret;
}

int dspd_daemon_add_device(void **handles, 
			   int32_t stream,
			   uint64_t hotplug_event_id,
			   const struct dspd_pcmdrv_ops *playback_ops,
			   const struct dspd_pcmdrv_ops *capture_ops)
{
  struct dspd_pcmdev_params devparams;
  void *dev;
  int ret;
  memset(&devparams, 0, sizeof(devparams));
  devparams.stream = stream;
  devparams.ops[0] = playback_ops;
  devparams.ops[1] = capture_ops;
  devparams.driver_handles = handles;
  ret = dspd_pcm_device_new(&dev,
			    hotplug_event_id,
			    &devparams,
			    dspd_dctx.objects);
  //if ( ret == 0 )
  //ret = dspd_dev_get_slot(dev);
  return ret;
}

int dspd_daemon_register_startup(void (*callback)(void *arg),
				 void *arg)
{
  struct dspd_startup_callback *cb = calloc(1, sizeof(struct dspd_startup_callback));
  int ret = 0;
  if ( ! cb )
    return -errno;
  cb->callback = callback;
  cb->arg = arg;
  if ( dspd_dctx.startup_callbacks )
    {
      if ( ! dspd_ll_append(dspd_dctx.startup_callbacks, cb) )
	{
	  ret = -errno;
	  free(cb);
	}
    } else
    {
      dspd_dctx.startup_callbacks = dspd_ll_new(cb);
      if ( ! dspd_dctx.startup_callbacks )
	{
	  ret = -errno;
	  free(cb);
	}
    }
  return ret;
}
static void sigxcpu_default_handler(int sig, siginfo_t *signinfo, void *context)
{
  struct sched_param param = { 0 };
  sched_setscheduler(dspd_gettid(), SCHED_OTHER, &param);
}


int dspd_daemon_run(void)
{
  struct dspd_ll *curr, *prev = NULL;
  struct dspd_startup_callback *cb;
  int ret;
  struct sigaction act;
  struct rlimit rl;
  memset(&act, 0, sizeof(act));
  rl.rlim_cur = 50000;
  rl.rlim_max = 75000;
  setrlimit(RLIMIT_RTTIME, &rl);
  
  act.sa_sigaction = sigxcpu_default_handler;
  act.sa_flags = SA_SIGINFO;
  sigaction(SIGXCPU, &act, NULL);

  dspd_log(0, "UID=%d GID=%d NAME=%s", dspd_dctx.uid, dspd_dctx.gid, dspd_dctx.user);
  if ( dspd_dctx.gid >= 0 )
    {
      if ( setgid(dspd_dctx.gid) < 0 )
	{
	  dspd_log(0, "Error %d while setting group id to %d", errno, dspd_dctx.gid);
	  return -1;
	}
    }

  if ( dspd_dctx.user )
    initgroups(dspd_dctx.user, dspd_dctx.gid);

  if ( dspd_dctx.uid >= 0 )
    {
      if ( setuid(dspd_dctx.uid) < 0 )
	{
	  dspd_log(0, "Error %d while setting user id to %d", errno, dspd_dctx.uid);
	  return -1;
	}
    }
  if ( dspd_dctx.gid >= 0 )
    setgid(dspd_dctx.gid);




  if ( (dspd_dctx.rtsvc_policy == SCHED_RR ||
	dspd_dctx.rtsvc_policy == SCHED_FIFO ||
	dspd_dctx.rtio_policy == SCHED_RR ||
	dspd_dctx.rtio_policy == SCHED_FIFO) &&
       dspd_dctx.uid > 0 )
    reset_caps();

  
  dspd_log(0, "Running startup commands...");
  for ( curr = dspd_dctx.startup_callbacks; curr; curr = curr->next )
    {
      cb = curr->pointer;
      cb->callback(cb->arg);
      free(curr->prev);
      prev = curr;
    }
  free(prev);
  dspd_dctx.startup_callbacks = NULL;


  dspd_log(0, "Starting main thread loop.");
  ret = cbpoll_run(dspd_dctx.main_thread_loop_context);
  return ret;
}



  
int32_t dspd_daemon_ref(uint32_t stream, uint32_t flags)
{
  void *data, *server_ops, *client_ops;
  int32_t ret;
  if ( stream > DSPD_MAX_OBJECTS )
    return -ENOENT;
  ret = dspd_slist_entry_wrlock(dspd_dctx.objects, stream);
  if ( ret == 0 )
    {
      dspd_slist_entry_get_pointers(dspd_dctx.objects,
				    stream,
				    &data,
				    &server_ops,
				    &client_ops);
      if ( dspd_slist_refcnt(dspd_dctx.objects, stream) == 0 ||	(client_ops == NULL && server_ops == NULL) )
	{
	  ret = -ENOENT;
	} else if ( flags != 0 && (((flags & DSPD_DCTL_ENUM_TYPE_CLIENT) != 0 && client_ops == NULL) ||
				   ((flags & DSPD_DCTL_ENUM_TYPE_SERVER) != 0 && server_ops == NULL)) )
	{
	  ret = -EINVAL;
	} else
	{
	  ret = 0;
	  dspd_slist_ref(dspd_dctx.objects, stream);
	}
      dspd_slist_entry_rw_unlock(dspd_dctx.objects, stream);
    }
  return ret;
}

void dspd_daemon_unref(uint32_t stream)
{
  DSPD_ASSERT(stream < DSPD_MAX_OBJECTS);
  dspd_slist_entry_wrlock(dspd_dctx.objects, stream);
  DSPD_ASSERT(dspd_slist_refcnt(dspd_dctx.objects, stream) > 0);
  dspd_slist_unref(dspd_dctx.objects, stream);
  dspd_slist_entry_rw_unlock(dspd_dctx.objects, stream);
}





static int32_t drh_get_objmask_size(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  uint32_t len = DSPD_MASK_SIZE;
  memcpy(outbuf, &len, sizeof(len));
  return dspd_req_reply_buf(context,
			    0,
			    outbuf,
			    sizeof(len));
}


static int32_t drh_enumerate_objects(struct dspd_rctx *context,
				     uint32_t      req,
				     const void   *inbuf,
				     size_t        inbufsize,
				     void         *outbuf,
				     size_t        outbufsize)
{
  uint32_t flags = *(uint32_t*)inbuf;
  (void)dspd_slist_get_object_mask(dspd_dctx.objects,
				   outbuf,
				   outbufsize,
				   !!(flags & DSPD_DCTL_ENUM_TYPE_SERVER),
				   !!(flags & DSPD_DCTL_ENUM_TYPE_CLIENT));
  return dspd_req_reply_buf(context,
			    0,
			    outbuf,
			    outbufsize);
}

static int32_t drh_get_module_count(struct dspd_rctx         *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  memcpy(outbuf, &dspd_dctx.modules->count, sizeof(uint32_t));
  return dspd_req_reply_buf(context,
			    0,
			    outbuf,
			    sizeof(uint32_t));
}

static int32_t drh_get_module_name(struct dspd_rctx *context,
				   uint32_t      req,
				   const void   *inbuf,
				   size_t        inbufsize,
				   void         *outbuf,
				   size_t        outbufsize)
{
  uint32_t c = 0, idx = *(uint32_t*)inbuf;
  struct dspd_ll *curr;
  struct dspd_module *m;
  size_t l = 0;
  pthread_rwlock_rdlock(&dspd_dctx.modules->lock);
  for ( curr = dspd_dctx.modules->modules; curr; curr = curr->next )
    {
      m = curr->pointer;
      if ( c == idx )
	{
	  l = strlen(m->name);
	  l++;
	  if ( l <= DSPD_MODULE_NAME_MAX )
	    memcpy(outbuf, m->name, l);
	  else
	    l = 0;
	  break;
	}
      c++;
    }
  pthread_rwlock_unlock(&dspd_dctx.modules->lock);
  return dspd_req_reply_buf(context,
			    0,
			    outbuf,
			    l);
}


static int32_t drh_get_defaultdev(struct dspd_rctx         *context,
				  uint32_t      req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  int32_t streams = *(int32_t*)inbuf;
  int32_t dev, ret;
  uint64_t fdev;
  pthread_mutex_lock(&dspd_dctx.hotplug.lock);
  streams &= (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);
  if ( streams == DSPD_PCM_SBIT_PLAYBACK )
    {
      dev = dspd_dctx.hotplug.default_playback;
    } else if ( streams == DSPD_PCM_SBIT_CAPTURE )
    {
      dev = dspd_dctx.hotplug.default_capture;
    } else if ( streams == 0 && outbufsize == sizeof(fdev) )
    {
      if ( dspd_dctx.hotplug.default_playback > 0 )
	fdev = (uint32_t)dspd_dctx.hotplug.default_playback;
      else
	fdev = UINT32_MAX;
      fdev <<= 32U;
      if ( dspd_dctx.hotplug.default_capture > 0 )
	fdev |= (uint32_t)dspd_dctx.hotplug.default_capture;
      else
	fdev |= UINT32_MAX;
    } else
    {
      dev = -1;
    }
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  if ( streams == 0 && outbufsize == sizeof(fdev) )
    ret = dspd_req_reply_buf(context, 0, &fdev, sizeof(fdev));
  else
    ret = dspd_req_reply_buf(context, 0, &dev, sizeof(dev));
  return ret;
}


static int32_t drh_syncstart(struct dspd_rctx         *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  struct dspd_syncgroup *sg;
  dspd_time_t t;
  const struct dspd_sg_info *sgi = inbuf;
  int32_t ret;
  sg = dspd_sg_get(dspd_dctx.syncgroups, sgi->sgid);
  if ( sg )
    {
      t = dspd_get_time();
      if ( sgi->sbits )
	dspd_sg_start(sg, &sgi->sbits);
      else
	dspd_sg_start(sg, NULL);
      dspd_sg_put(dspd_dctx.syncgroups, dspd_sg_id(sg));
      ret = dspd_req_reply_buf(context, 0, &t, sizeof(t));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ENOENT);
    }
  return ret;
}

static int32_t drh_syncstop(struct dspd_rctx         *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  struct dspd_syncgroup *sg;
  const struct dspd_sg_info *sgi = inbuf;
  int32_t ret;
  sg = dspd_sg_get(dspd_dctx.syncgroups, sgi->sgid);
  if ( sg )
    {
      if ( sgi->sbits )
	dspd_sg_stop(sg, &sgi->sbits);
      else
	dspd_sg_stop(sg, NULL);
      dspd_sg_put(dspd_dctx.syncgroups, dspd_sg_id(sg));
      ret = 0;
    } else
    {
      ret = ENOENT;
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t drh_change_route(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  const struct dspd_route_req *rr = inbuf;
  int32_t ret = EINVAL;
  bool unref_cli = false, unref_dev = false;
  uint64_t tstamp;
  size_t br = 0;
  if ( rr->client > 0 && rr->device > 0 )
    {
      ret = dspd_daemon_ref(rr->client, DSPD_DCTL_ENUM_TYPE_CLIENT);
      if ( ret == 0 )
	{
	  unref_cli = true;
	  ret = dspd_daemon_ref(rr->device, DSPD_DCTL_ENUM_TYPE_SERVER);
	  if ( ret == 0 )
	    {
	      unref_dev = true;
	      ret = dspd_stream_ctl(&dspd_dctx,
				    rr->client,
				    DSPD_SCTL_CLIENT_CHANGE_ROUTE,
				    &rr->device,
				    sizeof(rr->device),
				    &tstamp,
				    sizeof(tstamp),
				    &br);

	    }
	}
      if ( unref_cli )
	dspd_daemon_unref(rr->client);
      if ( unref_dev )
	dspd_daemon_unref(rr->device);
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(context, 0, &tstamp, sizeof(tstamp));
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static int32_t daemon_reply_buf(struct dspd_rctx *rctx, 
				int32_t flags, 
				const void *buf, 
				size_t len)
{
  DSPD_ASSERT(len <= rctx->outbufsize);
  if ( buf != rctx->outbuf )
    memcpy(rctx->outbuf, buf, len);
  rctx->bytes_returned = len;
  return 0;
}

static int32_t daemon_reply_fd(struct dspd_rctx *rctx, 
			       int32_t flags, 
			       const void *buf, 
			       size_t len, 
			       int32_t fd)
{
  int ret = daemon_reply_buf(rctx, flags, buf, len);
  if ( ret == 0 )
    {
      memcpy(rctx->outbuf, &fd, sizeof(fd));
    } else if ( flags & DSPD_REPLY_FLAG_CLOSEFD )
    {
      close(fd);
    }
  return ret;
}

static int32_t daemon_reply_err(struct dspd_rctx *rctx, 
				int32_t flags, 
				int32_t err)
{
  if ( err > 0 )
    err *= -1;
  return err;
}



int dspd_daemon_threadattr_init(void *attr, size_t size, int flags)
{
  dspd_threadattr_t *a = attr;
  int ret;
  struct sched_param param;
  int policy = SCHED_OTHER;
  if ( size == sizeof(*a) )
    ret = dspd_threadattr_init(a);
  else
    ret = pthread_attr_init(&a->attr);
  if ( ret == 0 )
    {
      memset(&param, 0, sizeof(param));
      if ( flags & DSPD_THREADATTR_DETACHED )
	pthread_attr_setdetachstate(&a->attr, PTHREAD_CREATE_DETACHED);
      pthread_attr_setschedpolicy(&a->attr, SCHED_OTHER);
      if ( flags & DSPD_THREADATTR_RTIO )
	{
	  policy = dspd_dctx.rtio_policy;
	  if ( dspd_dctx.rtio_policy != SCHED_DEADLINE )
	    {
	      param.sched_priority = dspd_dctx.rtio_priority;
	      if ( pthread_attr_setschedpolicy(&a->attr, dspd_dctx.rtio_policy) == 0 )
		pthread_attr_setschedparam(&a->attr, &param);
	    }
	}
      if ( flags & DSPD_THREADATTR_RTSVC )
	{
	  param.sched_priority = dspd_dctx.rtsvc_priority;
	  policy = dspd_dctx.rtsvc_policy;
	  if ( pthread_attr_setschedpolicy(&a->attr, dspd_dctx.rtsvc_policy) == 0 )
	    pthread_attr_setschedparam(&a->attr, &param);
	}
      if ( param.sched_priority == 0 && (policy == SCHED_RR || policy == SCHED_FIFO) )
	{
	  if ( pthread_attr_setschedpolicy(&a->attr, SCHED_OTHER) == 0 )
	    pthread_attr_setschedparam(&a->attr, &param);
	}
      pthread_attr_setinheritsched(&a->attr, PTHREAD_EXPLICIT_SCHED);
    }
  return ret;
}

const char *dspd_get_config_dir(void)
{
  char *val = NULL;
  dspd_dict_find_value(dspd_dctx.args_list, "-c", &val);
  if ( ! val )
    val = "/etc/dspd";
  return val;
}

const char *dspd_get_modules_dir(void)
{
  /*  char *val = NULL;
  struct dspd_dict *sect = dspd_dict_find_section(dspd_dctx.config, "DAEMON");
  if ( sect )
    dspd_dict_find_value(sect, "modules", &val);
  if ( ! val )
    {
      if ( sizeof(val) == sizeof(uint64_t) )
	val = "/usr/lib64/dspd";
      else
	val = "/usr/lib/dspd";
    }
    return val;*/
  return dspd_dctx.modules_dir;
}
struct dspd_dict *dspd_read_config(const char *module_name, bool exec_ok)
{
  const char *dir = dspd_get_config_dir();
  char buf[PATH_MAX];
  FILE *fp;
  struct dspd_dict *ret;
  bool x_ok;
  int e;
  sprintf(buf, "%s/%s.conf", dir, module_name);
  if ( access(buf, X_OK) == 0 )
    {
      if ( ! exec_ok )
	return NULL;
      fp = popen(buf, "r");
      x_ok = true;
    } else
    {
      fp = fopen(buf, "r");
      x_ok = false;
    }
  if ( fp )
    {
      ret = dspd_dict_read(fp);
      if ( ! ret )
	e = errno;
      if ( x_ok == true )
	pclose(fp);
      else
	fclose(fp);
      if ( ! ret )
	errno = e;
    } else
    {
      ret = NULL;
    }
  return ret;
}

dspd_time_t dspd_get_min_latency(void)
{
  //In the future this might depend on having realtime priority
  //and the device code might be fixed to use clock_nanosleep() for
  //low latencies.  For now, regular priority and epoll() make
  //0.6ms latencies glitchy.
  //It might also be necessary to stop interpolating at very low latencies.
  //For now device registers are read when a whole buffer cycles through
  //and when the currently selected latency is cycled through.
  return 1000000;
}

int32_t dspd_get_glitch_correction(void)
{
  return dspd_dctx.glitch_correction;
}



//Dispatch again from inside a handler
int32_t dspd_daemon_dispatch_ctl2(struct dspd_rctx *rctx,
				  struct dspd_dispatch_ctl2_info *info,
				  const void       *inbuf,
				  size_t            inbufsize,
				  void             *outbuf,
				  size_t            outbufsize)
{
  const struct dspd_req_handler *handler;
  uint32_t idx = info->req - info->min_req;
  int32_t ret = -ENOSYS;
  if ( idx < info->handlers_count )
    {
      handler = &info->handlers[idx];
      if ( handler->handler != NULL &&
	   (rctx->flags & handler->xflags) == 0 &&
	   (handler->rflags == 0 || (handler->rflags & rctx->flags) == rctx->flags) &&
	   ((rctx->flags & DSPD_REQ_FLAG_UNIX_FAST_IOCTL)|| //Unix ioctl means it was already verified.
	    ((inbufsize >= handler->inbufsize) &&       //Other stuff is still checked since ioctl handlers might
	     (outbufsize >= handler->outbufsize))) )     //be used in another context.

	{
	  ret = handler->handler(rctx,
				 info->req,
				 inbuf,
				 inbufsize,
				 outbuf,
				 outbufsize);
	}
    }
  return ret;
}


int dspd_daemon_set_ipc_perm(const char *path)
{
  if ( chown(path, dspd_dctx.uid, dspd_dctx.gid) < 0 ||
       chmod(path, dspd_dctx.ipc_mode) < 0 )
    return -errno;
  return 0;
}

int dspd_daemon_set_ipc_perm_fd(int fd)
{
  if ( fchown(fd, dspd_dctx.uid, dspd_dctx.gid) < 0 ||
       fchmod(fd, dspd_dctx.ipc_mode) < 0 )
    return -errno;
  return 0;
}

//Compare dict against search using only what is in search
static bool dict_cmp(const struct dspd_dict *dict, const struct dspd_dict *search)
{
  size_t i;
  bool ret = true;
  const struct dspd_kvpair *kvp;
  for ( i = 0; i < search->count; i++ )
    {
      kvp = &search->list[i];
      if ( ! dspd_dict_test_value(dict, kvp->key, kvp->value) )
	{
	  ret = false;
	  break;
	}
    }
  return ret;
}

static bool refresh_default_device_dict(struct dspd_hotplug *hp)
{
  const struct dspd_dict *dev;
  const struct dspd_hotplug_devname *name;
  int32_t pdev = -1, cdev = -1;
  const char *slot;
  if ( hp->playback_search || hp->capture_search )
    {
      for ( name = hp->names; name != NULL && (pdev < 0 || cdev < 0); name = name->next )
	{
	  dev = name->info;
	  if ( (name->sbits & DSPD_PCM_SBIT_PLAYBACK) && 
	       hp->playback_search != NULL && pdev < 0 && dict_cmp(dev, hp->playback_search) == true )
	    {
	      slot = dspd_dict_value_for_key(dev, DSPD_HOTPLUG_SLOT);
	      if ( slot != NULL )
		if ( dspd_strtoi32(slot, &pdev, 0) == 0 )
		  if ( pdev >= DSPD_MAX_OBJECTS )
		    pdev = -1;
	    }
	  if ( (name->sbits & DSPD_PCM_SBIT_CAPTURE) && 
	       hp->capture_search != NULL && cdev < 0 && dict_cmp(dev, hp->capture_search) == true )
	    {
	      slot = dspd_dict_value_for_key(dev, DSPD_HOTPLUG_SLOT);
	      if ( slot != NULL )
		if ( dspd_strtoi32(slot, &cdev, 0) == 0 )
		  if ( cdev >= DSPD_MAX_OBJECTS )
		    cdev = -1;
	    }
	}
    }
  hp->default_playback = pdev;
  hp->default_capture = cdev;
  return pdev > 0 && cdev > 0;
}

static struct dspd_hotplug_devname *find_default_dev(struct dspd_hotplug *hp, 
						     struct dspd_hotplug_devname *match)
{
  struct dspd_hotplug_devname *dev, *ret = NULL;
  int32_t slot = -1;
  for ( dev = hp->names; dev; dev = dev->next )
    {
      if ( match != NULL )
	{
	  if ( dev->sbits != match->sbits )
	    {
	      if ( strcmp(dev->hwname, match->hwname) == 0 || dev->slot == match->slot )
		{
		  ret = dev;
		  break;
		}
	    } else if ( match->sbits == DSPD_PCM_SBIT_FULLDUPLEX && dev->slot == match->slot )
	    {
	      ret = dev;
	      break;
	    }
	} else if ( dev->slot > slot )
	{
	  slot = dev->slot;
	  ret = dev;
	}
    }
  return ret;
}

static bool refresh_default_device_name(struct dspd_hotplug *hp)
{
  struct dspd_hotplug_devname *dev, *dn1 = NULL, *dn2 = NULL, *match = NULL;
  if ( hp->default_playback < 0 || hp->default_capture < 0 )
    {
      for ( dev = hp->names; dev; dev = dev->next )
	{
	  if ( dev->slot == hp->default_playback || dev->slot == hp->default_capture )
	    {
	      match = dev;
	      break;
	    }
	}
      dn1 = find_default_dev(hp, match);
      if ( match != NULL )
	dn2 = match;
      else
	dn2 = find_default_dev(hp, dn1);
      if ( dn1 && (dn1->sbits & DSPD_PCM_SBIT_PLAYBACK) )
	hp->default_playback = dn1->slot;
      else if ( dn2 && (dn2->sbits & DSPD_PCM_SBIT_PLAYBACK) )
	hp->default_playback = dn2->slot;

      if ( dn1 && (dn1->sbits & DSPD_PCM_SBIT_CAPTURE) )
	hp->default_capture = dn1->slot;
      else if ( dn2 && (dn2->sbits & DSPD_PCM_SBIT_CAPTURE) )
	hp->default_capture = dn2->slot;
    }
  return hp->default_playback > 0 && hp->default_capture > 0;
}
