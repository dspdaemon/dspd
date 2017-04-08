/*
 *  DAEMON - Server support code
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

#include "sslib.h"
#include "daemon.h"
#include "syncgroup.h"
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
	dspd_daemon_unref(stream);
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
  if ( ret )
    {
      ret *= -1;
    } else
    {
      ctx->hotplug.default_playback = -1;
      ctx->hotplug.default_capture = -1;
      ctx->hotplug.default_fullduplex = -1;
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

static void *dummy(void *p)
{
  return NULL;
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
}

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

  if ( (ret = dspd_wq_new(&dspd_dctx.wq)) )
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

  if ( dspd_dict_test_value(dspd_dctx.args_list, "-h", NULL) )
    {
      print_usage();
      return -1;
    }
  if ( dspd_dict_test_value(dspd_dctx.args_list, "-b", NULL) )
    {
      if ( daemon(0, 0) < 0 )
	return -1;
    }

  dspd_dctx.config = dspd_read_config("dspd", true);
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

  if ( dspd_dict_find_value(dcfg, "preferred_defaultdev", &value) )
    {
      if ( value )
	{
	  p = strchr(value, ',');
	  if ( p )
	    {
	      dspd_dctx.default_device.key = strndup(value, (size_t)p - (size_t)value);
	      dspd_dctx.default_device.value = strdup(&p[1]);
	      if ( ! (dspd_dctx.default_device.key && dspd_dctx.default_device.value) )
		goto out;
	    }
	}
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

  if ( dspd_dict_find_value(dcfg, "modules", &value) )
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
	      p = strrchr(p, '/');
	      if ( p != NULL )
		{
		  sprintf(p, "/lib"LIBSUFFIX"/dspd");
		  dspd_dctx.modules_dir = strdup(p);
		  if ( ! dspd_dctx.modules_dir )
		    {
		      ret = -ENOMEM;
		      goto out;
		    }
		}
	    }
	}
      /*if ( dspd_dctx.modules_dir == NULL )
	{
	  sprintf(tmp, "/usr/lib"LIBSUFFIX"/dspd");
	  dspd_dctx.modules_dir = strdup(tmp);
	  }*/
    }
  /*  if ( ! dspd_dctx.modules_dir )
    {
      ret = -ENOMEM;
      goto out;
      }*/

 noconfig:

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
    {
      if ( is_rt() )
	dspd_dctx.glitch_correction = DSPD_GHCN_AUTO;
      else
	dspd_dctx.glitch_correction = DSPD_GHCN_LATCH;
    }

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
  assert(dspd_dctx.objects != NULL);
  

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
  

  ret = 0;
  
  
 out:
  free(tmp);
  free(pwbuf);
  if ( ret != 0 )
    {
      //This is where cleanup should happen
    }
  return ret;
}


int dspd_daemon_register_mainthread_loop(int (*mainthread_loop)(int argc,
								char **argv,
								struct dspd_daemon_ctx *ctx))
{
  dspd_dctx.mainthread_loop = mainthread_loop;
  return 0;
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

static void init_device_notify(const struct dspd_dict *device)
{
  struct dspd_ll *curr;
  struct dspd_hotplug_handler *h;
  for ( curr = dspd_dctx.hotplug.handlers; curr; curr = curr->next )
    {
      h = curr->pointer;
      if ( h->callbacks->init_device != NULL )
	h->callbacks->init_device(h->arg, device);
    }
}

static int32_t getdevnum(const char *name)
{
  const char *p = strchr(name, ':');
  int32_t i = -1;
  if ( p )
    dspd_strtoi32(&p[1], &i, 10);
  return i;
}

static void dspd_hotplug_updatedefault(struct dspd_dict *sect)
{
  uint8_t mask[DSPD_MASK_SIZE];
  struct dspd_device_stat info;
  int32_t playback = INT32_MAX, capture = INT32_MAX, fulldup = INT32_MAX;
  int32_t pidx, cidx, fidx = -1;
  size_t i, bits = sizeof(mask) * 8;
  int32_t ret;
  size_t br;
  char *n; int32_t dev_num = -1;
  int32_t dev_idx = -1; char *slot;
  if ( sect == NULL && dspd_dctx.default_device.key != NULL )
    sect = dspd_dctx.default_dev_info;
  
  if ( sect && dspd_dctx.default_device.key )
    {
	      
      if ( dspd_dict_test_value(sect, dspd_dctx.default_device.key, dspd_dctx.default_device.value) )
	{
	  if ( dspd_dict_find_value(sect, DSPD_HOTPLUG_SLOT, &slot) )
	    {
	      if ( slot )
		{
		  dev_idx = atoi(slot);
		}
	    }
	  if ( dspd_dict_find_value(sect, DSPD_HOTPLUG_DEVNAME, &n) )
	    {
	      if ( n )
		{
		  dev_num = getdevnum(n);
		}
	    }
	}
    }


  dspd_slist_get_object_mask(dspd_dctx.objects,
			     mask,
			     sizeof(mask),
			     1,
			     0);
  for ( i = 0; i < bits; i++ )
    {
      if ( dspd_test_bit(mask, i) )
	{
	  if ( dspd_daemon_ref(i, DSPD_DCTL_ENUM_TYPE_SERVER) == 0 )
	    {
	      ret = dspd_stream_ctl(&dspd_dctx,
				    i,
				    DSPD_SCTL_SERVER_STAT,
				    NULL,
				    0,
				    &info,
				    sizeof(info),
				    &br);
	      if ( ret == 0 && br == sizeof(info) )
		{
		  ret = getdevnum(info.name);
		  if ( ret >= 0 )
		    {
		      if ( info.streams == DSPD_PCM_SBIT_PLAYBACK )
			{
			  if ( ret < playback && playback != dev_num )
			    {
			      playback = ret;
			      pidx = i;
			    }
			} else if ( info.streams == DSPD_PCM_SBIT_CAPTURE )
			{
			  if ( ret < capture && capture != dev_num )
			    {
			      capture = ret;
			      cidx = i;
			    }
			} else if ( info.streams == (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE) )
			{
			  if ( ret < fulldup && fulldup != dev_num )
			    {
			      fulldup = ret;
			      fidx = i;
			    }
			}
		    }
		}
	      dspd_daemon_unref(i);
	    }
	}
    }

  
  
  if ( fulldup == INT32_MAX )
    {
      dspd_dctx.hotplug.default_fullduplex = -1;
      dspd_dctx.hotplug.default_capture = -1;
      dspd_dctx.hotplug.default_playback = -1;
    } else if ( dspd_dctx.default_dev_info == NULL )
    {
      dspd_dctx.hotplug.default_fullduplex = fidx;
      dspd_dctx.hotplug.default_playback = fidx;
      dspd_dctx.hotplug.default_capture = fidx;
      if ( fidx == dev_idx )
	dspd_dctx.default_dev_info = sect;
    }

  if ( playback != INT32_MAX && dspd_dctx.hotplug.default_playback == -1 )
    dspd_dctx.hotplug.default_playback = pidx;
  if ( capture != INT32_MAX && dspd_dctx.hotplug.default_capture == -1 )
    dspd_dctx.hotplug.default_capture = cidx;
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
	      assert(p);
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
	init_device_notify(kvs);
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
  int r, isdefault = 0;
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
	if ( curr == dspd_dctx.default_dev_info )
	  isdefault = 1;
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
	    dspd_dict_free(curr);
	    curr = c;
	  } else
	  {
	    curr = curr->next;
	  }
      }
  } while ( curr );
  

  
  if ( isdefault != 0 && ret == 0 )
    {
      dspd_dctx.default_dev_info = NULL;
      dspd_hotplug_updatedefault(NULL);
    }
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

int dspd_daemon_get_config(const struct dspd_dict *sect,
			   struct dspd_drv_params *params)
{
  char *ptr;
  int ret;
  memset(params, 0, sizeof(*params));
  if ( dspd_dict_find_value(sect, DSPD_HOTPLUG_BUSNAME, &ptr) )
    {
      params->bus = strdup(ptr);
      if ( ! params->bus )
	goto out;
    }
  if ( dspd_dict_find_value(sect, DSPD_HOTPLUG_DEVNAME, &ptr) )
    {
      params->name = strdup(ptr);
      if ( ! params->name )
	goto out;
    }
  if ( dspd_dict_find_value(sect, DSPD_HOTPLUG_ADDRESS, &ptr) )
    {
      params->addr = strdup(ptr);
      if ( ! params->addr )
	goto out;
    }

  if ( dspd_dict_find_value(sect, DSPD_HOTPLUG_DESC, &ptr) )
    {
      params->desc = strdup(ptr);
      if ( ! params->addr )
	goto out;
    }

  if ( dspd_dict_find_value(sect, DSPD_HOTPLUG_STREAM, &ptr) )
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
  params->rate = 48000; //44100 / 2;
  params->bufsize = 16384;
  params->fragsize = 4096;
  return 0;
 out:
  ret = -errno;
  free(params->addr);
  free(params->name);
  free(params->bus);
  free(params->desc);
  memset(params, 0, sizeof(*params));
  return ret;
}

int dspd_daemon_add_device(void **handles, 
			   int32_t stream,
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
  if ( ! dspd_dctx.mainthread_loop )
    {
      while (1)
	sleep(1);
      ret = 0;
    } else
    {
      ret = dspd_dctx.mainthread_loop(dspd_dctx.argc, dspd_dctx.argv, &dspd_dctx);
    }
  return ret;
}

//TODO: Open a device or return unbound client (device==-1)
int dspd_stream_open(int32_t device)
{
  return 0;
}

/*int dspd_stream_ctl(int32_t stream,
		    uint32_t req,
		    const void   *inbuf,
		    size_t        inbufsize,
		    void         *outbuf,
		    size_t        outbufsize,
		    size_t       *bytes_returned)
		    {*/
  /*
    TODO: Dispatch commands using client methods.
    Must make sure the slot has the appropriate methods.
    Assume that locking is already done or maybe handle it
    on the client.

    This should handle read, write, getospace, getispace, status, setparams, getparams, trigger.

    
  */
/*
  

  return -EINVAL;
  }*/

int dspd_stream_ref(int32_t stream)
{
  return 0;
}

int dspd_stream_unref(int32_t stream)
{
  return 0;
}


/*int dspd_daemon_ctl(const char *module,
		    uint32_t    req,
		    const void   *inbuf,
		    size_t        inbufsize,
		    void         *outbuf,
		    size_t        outbufsize,
		    size_t       *bytes_returned)
{
  int ret;
  struct dspd_ll *curr;
  struct dspd_module *m;
  if ( module == NULL )
    {
      ret = dspd_dispatch_request(daemon_req_handlers,
				  sizeof(daemon_req_handlers) / sizeof(daemon_req_handlers[0]),
				  NULL,
				  req,
				  inbuf,
				  inbufsize,
				  outbuf,
				  outbufsize,
				  bytes_returned);
    } else
    {
      ret = -ENOENT;
      for ( curr = dspd_dctx.modules->modules; curr; curr = curr->next )
	{
	  m = curr->pointer;
	  if ( strcmp(m->callbacks->desc, module) == 0 )
	    {
	      ret = m->callbacks->ioctl(&dspd_dctx,
					m->context,
					req,
					inbuf,
					inbufsize,
					outbuf,
					outbufsize,
					bytes_returned);
	      break;
	    }
	}
    }
  return ret;
  }*/

  
int32_t dspd_daemon_ref(uint32_t stream, uint32_t flags)
{
  void *data, *server_ops, *client_ops;
  int32_t ret;
  if ( stream > DSPD_MAX_OBJECTS )
    return -ENOENT;
  dspd_slist_entry_wrlock(dspd_dctx.objects, stream);
  dspd_slist_entry_get_pointers(dspd_dctx.objects,
				stream,
				&data,
				&server_ops,
				&client_ops);
  if ( dspd_slist_refcnt(dspd_dctx.objects, stream) == 0 )
    {
      ret = -ENOENT;
    } else if ( ((flags & DSPD_DCTL_ENUM_TYPE_CLIENT) != 0 && client_ops == NULL) ||
		((flags & DSPD_DCTL_ENUM_TYPE_SERVER) != 0 && server_ops == NULL) )
    {
      ret = -EINVAL;
    } else
    {
      ret = 0;
      dspd_slist_ref(dspd_dctx.objects, stream);
    }
  dspd_slist_entry_rw_unlock(dspd_dctx.objects, stream);
  return ret;
}

void dspd_daemon_unref(uint32_t stream)
{
  assert(stream < DSPD_MAX_OBJECTS);
  dspd_slist_entry_wrlock(dspd_dctx.objects, stream);
  assert(dspd_slist_refcnt(dspd_dctx.objects, stream) > 0);
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
  int32_t dev;
  pthread_mutex_lock(&dspd_dctx.hotplug.lock);
  streams &= (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE);
  if ( streams == DSPD_PCM_SBIT_PLAYBACK )
    dev = dspd_dctx.hotplug.default_playback;
  else if ( streams == DSPD_PCM_SBIT_CAPTURE )
    dev = dspd_dctx.hotplug.default_capture;
  else if ( streams == (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE) )
    dev = dspd_dctx.hotplug.default_fullduplex;
  else
    dev = -1;
  pthread_mutex_unlock(&dspd_dctx.hotplug.lock);
  return dspd_req_reply_buf(context, 0, &dev, sizeof(dev));
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

static int32_t daemon_reply_buf(struct dspd_rctx *rctx, 
				int32_t flags, 
				const void *buf, 
				size_t len)
{
  assert(len <= rctx->outbufsize);
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
  return err * -1;
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
      if ( x_ok == true )
	pclose(fp);
      else
	fclose(fp);
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

bool dspd_daemon_queue_work(const struct dspd_wq_item *item)
{
  return dspd_queue_work(dspd_dctx.wq, item);
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
