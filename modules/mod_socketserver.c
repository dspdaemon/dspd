/*
 *   SOCKETSERVER - Unix domain socket interface for DSPD
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
#include <unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#define _DSPD_CTL_MACROS
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../lib/cbpoll.h"

#define MSG_EVENT_FLAGS (CBPOLL_PIPE_MSG_USER+1)
struct ss_cctx {
  struct dspd_req_ctx *req_ctx;

  /*
    Remember that there is a reference to these two
    so no need to lock.
  */
  int32_t stream;
  intptr_t device;

  int32_t playback_device;
  int32_t capture_device;
  int32_t playback_stream;
  int32_t capture_stream;

  struct dspd_req *pkt_in;
  int32_t          pkt_cmd;
  uint64_t         pkt_tag;
  size_t           pkt_size;
  int32_t          pkt_fd;
  int32_t          pkt_stream;
  int32_t          pkt_flags;
  struct dspd_req *pkt_out;
  int fd_out;

  struct cbpoll_ctx *cbctx;
  int fd;
  int index;
  
  struct dspd_rctx rctx;
  dspd_mutex_t  lock;

  uint16_t event_flags;
  bool     local;
 
};

struct ss_sctx {
  int fd;
  struct ss_cctx    *accepted_context;
  struct cbpoll_ctx  cbctx;
};



static int socksrv_dispatch_multi_req(struct dspd_rctx *rctx,
				      uint32_t             req,
				      const void          *inbuf,
				      size_t        inbufsize,
				      void         *outbuf,
				      size_t        outbufsize);

/*int32_t sendreq(struct ss_cctx *cli)
{
  if ( cli->pkt_out->cmd == DSPD_DCTL_ASYNC_EVENT )
    fprintf(stderr, "SEND ASYNC EVENT\n");
  return dspd_req_send(cli->req_ctx);
  }*/
#define sendreq(cli) dspd_req_send(cli->req_ctx)

static inline bool stream_valid(struct ss_cctx *ctx, int32_t stream)
{
  return (ctx->stream == stream ||
	  ctx->playback_stream == stream ||
	  ctx->capture_stream == stream);
}
static inline bool device_valid(struct ss_cctx *ctx, int32_t device)
{
  return (ctx->device == device ||
	  ctx->playback_device == device ||
	  ctx->capture_device == device);
}



static int32_t client_reply_buf(struct dspd_rctx *arg, 
				int32_t flags, 
				const void *buf, 
				size_t len)
{
  struct ss_cctx *cli = arg->ops_arg;
  struct dspd_req *req = (struct dspd_req*)cli->pkt_out;
  int32_t ret;
  if ( buf != arg->outbuf && len > 0 )
    memcpy(arg->outbuf, buf, len);
  req->cmd = cli->pkt_cmd & 0xFFFF;
  req->tag = cli->pkt_tag;
  req->flags = flags & 0xFFFF;
  req->flags |= cli->event_flags;
  req->len = sizeof(struct dspd_req);
  
  if ( arg->flags & DSPD_REQ_FLAG_POINTER )
    {
      req->bytes_returned = len;
    } else
    {
      req->len += len;
      req->bytes_returned = 0;
    } 


  cli->event_flags = 0;
  req->stream = cli->pkt_stream;
  req->rdata.rlen = 0;
  ret = sendreq(cli);
  if ( ret == req->len )
    ret = 0;
  return ret;
}

static int32_t client_reply_fd(struct dspd_rctx *arg, 
			       int32_t flags, 
			       const void *buf, 
			       size_t len, 
			       int32_t fd)
{
  struct ss_cctx *cli = arg->ops_arg;
  struct dspd_req *req = (struct dspd_req*)cli->pkt_out;
  int32_t ret;
  if ( buf != arg->outbuf && len > 0 )
    memcpy(arg->outbuf, buf, len);
  memcpy(arg->outbuf, &fd, sizeof(fd));
  req->len = sizeof(struct dspd_req);
  if ( arg->flags & DSPD_REQ_FLAG_POINTER )
    {
      req->bytes_returned = len;
    } else
    {
      req->len += len;
      req->bytes_returned = 0;
    } 
  
  

  req->cmd = cli->pkt_cmd & 0xFFFF;
  req->tag = cli->pkt_tag;

  req->flags = flags & 0xFFFF;
  req->flags |= DSPD_REQ_FLAG_CMSG_FD;
  req->flags |= cli->event_flags;

  cli->event_flags = 0;
  req->stream = cli->pkt_stream;
  req->rdata.rlen = 0;

  ret = sendreq(cli);
  if ( ret == req->len )
    {
      ret = 0;
      if ( flags & DSPD_REPLY_FLAG_CLOSEFD )
	close(fd);
    } else if ( flags & DSPD_REPLY_FLAG_CLOSEFD )
    {
      if ( ret < 0 && ret != -EINPROGRESS )
	close(fd);
      else
	cli->fd_out = fd;
    }
  return ret;
}

static int32_t client_reply_err(struct dspd_rctx *arg, 
				int32_t flags, 
				int32_t err)
{
  struct ss_cctx *cli = arg->ops_arg;
  struct dspd_req *req = (struct dspd_req*)cli->pkt_out;
  int32_t ret;
  req->flags = DSPD_REQ_FLAG_ERROR | (flags & 0xFFFF);
  req->len = sizeof(*req);
  req->stream = cli->pkt_stream;
  if ( err > 0 )
    err *= -1;
  req->rdata.err = err;
  req->flags |= cli->event_flags;
  req->cmd = cli->pkt_cmd & 0xFFFF;
  req->tag = cli->pkt_tag;
  req->bytes_returned = 0;
  cli->event_flags = 0;
  ret = sendreq(cli);
  if ( ret == req->len )
    ret = 0;
  return ret;
}


static const struct dspd_rcb client_rcb = { 
  .reply_buf = client_reply_buf,
  .reply_fd = client_reply_fd,
  .reply_err = client_reply_err,
};

static void socksrv_error(void *dev, int32_t index, void *client, int32_t err, void *arg)
{
  struct ss_cctx *cli = arg;
  struct cbpoll_pipe_event evt;
  //The client is not allowed to truncate the SHM.  This is a fatal protocol violation.
  if ( err == EFAULT )
    {
      dspd_mutex_lock(&cli->lock);
      if ( cli->fd >= 0 )
	shutdown(cli->fd, SHUT_RDWR);
      dspd_mutex_unlock(&cli->lock);
    } else if ( err == ENODEV )
    {
      dspd_mutex_lock(&cli->lock);
      if ( cli->cbctx )
	{
	  memset(&evt, 0, sizeof(evt));
	  evt.fd = cli->fd;
	  evt.index = cli->index;
	  evt.stream = cli->stream;
	  evt.msg = MSG_EVENT_FLAGS;
	  evt.arg = index;
	  evt.arg <<= 32U;
	  evt.arg |= DSPD_REQ_FLAG_POLLHUP;
	  cbpoll_send_event(cli->cbctx, &evt);
	}
      dspd_mutex_unlock(&cli->lock);
    }
}
static void close_client_stream(struct ss_cctx *cli, int32_t stream);
static int32_t open_client_stream(struct ss_cctx *cli)
{
  int32_t ret;
  void *clptr;
  struct dspd_client_cb cb;
  size_t br;
  ret = dspd_client_new(dspd_dctx.objects, &clptr);
  if ( ret == 0 )
    {
      cb.arg = cli;
      cb.callback = socksrv_error;
      cb.index = DSPD_CLIENT_CB_ERROR;
      dspd_stream_ctl(&dspd_dctx,
		      cli->stream,
		      DSPD_SCTL_CLIENT_SETCB,
		      &cb,
		      sizeof(cb),
		      NULL,
		      0,
		      &br);
      ret = dspd_client_get_index(clptr);
    }
  return ret;
}
static void close_client_stream(struct ss_cctx *cli, int32_t stream)
{
  size_t br;
  (void)cli;
  //The server retains the client, so disconnect to make sure
  //it is actually released.
  dspd_stream_ctl(&dspd_dctx, 
		  stream,
		  DSPD_SCTL_CLIENT_DISCONNECT,
		  NULL,
		  0,
		  NULL,
		  0,
		  &br);
  dspd_daemon_unref(stream);
}



static int32_t open_device(struct ss_cctx *cli, int32_t sbits, int32_t dev, struct dspd_device_stat *info)
{
  int32_t ret;
  size_t br;
  ret = dspd_daemon_ref(dev, DSPD_DCTL_ENUM_TYPE_SERVER);
  if ( ret == 0 )
    {
      br = sizeof(*info);
      ret = dspd_stream_ctl(&dspd_dctx,
			    dev,
			    DSPD_SCTL_SERVER_STAT,
			    NULL,
			    0,
			    info,
			    sizeof(*info),
			    &br);
      if ( ret == 0 )
	{
	  if ( (info->streams & sbits) == sbits )
	    {
	      ret = dev;
	    } else
	    {
	      ret = -ENOSR;
	      dspd_daemon_unref(dev);
	    }
	} else
	{
	  dspd_daemon_unref(dev);
	}
    }
  return ret;
}

static int socksrv_dispatch_req(struct dspd_rctx *rctx,
				uint32_t             req,
				const void          *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct ss_cctx *cli = dspd_req_userdata(rctx);
  int ret, err;
  size_t br;
  uint32_t dev;
  uint64_t val;
  int64_t i64;
  int32_t i32;
  struct dspd_device_stat info;
  void *ptr;
  size_t s;
  switch(req)
    {
    case DSPD_SOCKSRV_REQ_NEWCLI:
      if ( cli->stream >= 0 )
	{
	  ret = dspd_req_reply_err(rctx, 0, EBUSY);
	} else if ( outbufsize < sizeof(cli->stream) )
	{
	  ret = dspd_req_reply_err(rctx, 0, EINVAL);
	} else
	{
	  ret = open_client_stream(cli);
	  if ( ret >= 0 )
	    {
	      cli->stream = ret;
	      ret = dspd_req_reply_buf(rctx, 0, &cli->stream, sizeof(cli->stream));
	    } else
	    {
	      ret = dspd_req_reply_err(rctx, 0, ret);
	    }
	}
      break;
    case DSPD_SOCKSRV_REQ_DELCLI:
      if ( cli->stream >= 0 )
	{
	  close_client_stream(cli, cli->stream);
	  cli->stream = -1;
	  ret = dspd_req_reply_err(rctx, 0, 0);
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, ENOENT);
	}
      break;
    case DSPD_SOCKSRV_REQ_REFSRV:
       if ( inbufsize < 4 )
	{
	  ret = dspd_req_reply_err(rctx, 0, EINVAL);
	} else
	{
	  //Compatibility with internal APIs
	  if ( inbufsize == sizeof(uint64_t) )
	    {
	      val = *(uint64_t*)inbuf;
	      dev = val & 0xFFFFFFFFU;
	    } else
	    {
	      dev = *(uint32_t*)inbuf;
	    }

	  //Throw away the old reference
	  if ( cli->device != dev )
	    {
	      ret = dspd_daemon_ref(dev, DSPD_DCTL_ENUM_TYPE_SERVER);
	      if ( ret == 0 && cli->device >= 0 )
		dspd_daemon_unref(cli->device);

	    } else if ( dev >= DSPD_MAX_OBJECTS )
	    {
	      ret = -EINVAL;
	    } else
	    {
	      ret = 0;
	    }
	  if ( ret == 0 )
	    {
	      cli->device = dev;
	      if ( outbufsize >= sizeof(struct dspd_device_stat) )
		{
		  err = dspd_stream_ctl(&dspd_dctx,
					dev,
					DSPD_SCTL_SERVER_STAT,
					NULL,
					0,
					outbuf,
					outbufsize,
					&br);
		  if ( err != 0 || br == 0 ) //Got the device but no data
		    ret = dspd_req_reply_err(rctx, 0, err * -1);
		  else
		    ret = dspd_req_reply_buf(rctx, 0, outbuf, br);
		} else
		{
		  ret = dspd_req_reply_err(rctx, 0, 0);
		}
	    } else
	    {
	      cli->device = -1;
	      ret = dspd_req_reply_err(rctx, 0, ENOENT);
	    }
	}
      break;
    case DSPD_SOCKSRV_REQ_UNREFSRV:
      if ( cli->device >= 0 )
	{
	  dspd_daemon_unref(cli->device);
	  cli->device = -1;
	  ret = dspd_req_reply_err(rctx, 0, 0);
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, ENOENT);
	}
      break;
    case DSPD_SOCKSRV_REQ_NMCLI:
      if ( inbufsize == sizeof(int32_t) &&
	   outbufsize == sizeof(int64_t) )
	{
	  i32 = *(int32_t*)inbuf;
	  ret = 0;
	  if ( i32 )
	    {
	      //Open one or two half duplex streams.
	      if ( (i32 & DSPD_PCM_SBIT_PLAYBACK) && cli->playback_stream < 0 )
		{
		  ret = open_client_stream(cli);
		  if ( ret >= 0 )
		    {
		      cli->playback_stream = ret;
		      ret = 0;
		    }
		}
	      if ( (i32 & DSPD_PCM_SBIT_CAPTURE) && cli->capture_stream < 0 && ret == 0 )
		{
		  ret = open_client_stream(cli);
		  if ( ret >= 0 )
		    {
		      cli->capture_stream = ret;
		      ret = 0;
		    }
		}
	    } else
	    {
	      //Open one full duplex stream
	      if ( cli->playback_stream < 0 && cli->capture_stream < 0 && cli->stream < 0 )
		{
		  ret = open_client_stream(cli);
		  if ( ret >= 0 )
		    {
		      cli->stream = ret;
		      cli->playback_stream = ret;
		      dspd_slist_ref(dspd_dctx.objects, cli->playback_stream);
		      cli->capture_stream = ret;
		      dspd_slist_ref(dspd_dctx.objects, cli->capture_stream);
		      ret = 0; 
		    }
		}
	    }
	  if ( ret < 0 )
	    {
	      ret = dspd_req_reply_err(rctx, 0, ret);
	    } else
	    {
	      i64 = cli->playback_stream;
	      i64 <<= 32U;
	      i64 |= cli->capture_stream & 0x00000000FFFFFFFFULL;
	      ret = dspd_req_reply_buf(rctx, 0, &i64, sizeof(i64));
	    }
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, EINVAL);
	}
      break;
    case DSPD_SOCKSRV_REQ_DMCLI:
      if ( inbufsize == sizeof(int32_t) )
	{
	  i32 = *(int32_t*)inbuf;
	  if ( cli->playback_stream == cli->capture_stream &&
	       cli->playback_stream == cli->stream &&
	       cli->stream >= 0 &&
	       i32 == DSPD_PCM_SBIT_FULLDUPLEX )
	    {
	      close_client_stream(cli, cli->playback_stream);
	      close_client_stream(cli, cli->capture_stream);
	      close_client_stream(cli, cli->stream);
	      cli->playback_stream = -1;
	      cli->capture_stream = -1;
	      cli->stream = -1;
	    } else
	    {
	      if ( cli->playback_stream >= 0 && (i32 & DSPD_PCM_SBIT_PLAYBACK) )
		{
		  close_client_stream(cli, cli->playback_stream);
		  cli->playback_stream = -1;
		}
	      if ( cli->capture_stream >= 0 && (i32 & DSPD_PCM_SBIT_CAPTURE) )
		{
		  close_client_stream(cli, cli->capture_stream);
		  cli->capture_stream = -1;
		}
	    }
	  ret = dspd_req_reply_err(rctx, 0, 0);
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, EINVAL);
	}
      break;
    case DSPD_SOCKSRV_REQ_RMSRV:
      if ( inbufsize == sizeof(int64_t) )
	{
	  i64 = *(int64_t*)inbuf;
	  i32 = i64 >> 32U;
	  dev = i64 & 0xFFFFFFFFLL;
	  ret = open_device(cli, dev, i32, &info);
	  if ( ret >= 0 )
	    {
	      if ( (i32 & DSPD_PCM_SBIT_FULLDUPLEX) == DSPD_PCM_SBIT_FULLDUPLEX )
		{
		  if ( cli->playback_device >= 0 )
		    dspd_slist_unref(dspd_dctx.objects, cli->playback_device);
		  if ( cli->capture_device >= 0 )
		    dspd_slist_unref(dspd_dctx.objects, cli->capture_device);
		  if ( cli->device >= 0 )
		    dspd_slist_unref(dspd_dctx.objects, cli->device);
		  cli->device = ret;
		  cli->playback_device = cli->device;
		  dspd_slist_ref(dspd_dctx.objects, cli->playback_device);
		  cli->capture_device = cli->device;
		  dspd_slist_ref(dspd_dctx.objects, cli->capture_device);
		} else if ( i32 & DSPD_PCM_SBIT_PLAYBACK )
		{
		  if ( cli->playback_device >= 0 )
		    dspd_daemon_unref(cli->playback_device);
		  cli->playback_device = ret;
		} else if ( i32 & DSPD_PCM_SBIT_CAPTURE )
		{
		  if ( cli->capture_device >= 0 )
		    dspd_daemon_unref(cli->capture_device);
		  cli->capture_device = ret;
		} else
		{
		  if ( cli->device >= 0 )
		    dspd_daemon_unref(cli->device);
		  cli->device = ret;
		}
	      if ( outbufsize == sizeof(struct dspd_device_stat) )
		ret = dspd_req_reply_buf(rctx, 0, &info, sizeof(info));
	      else
		ret = dspd_req_reply_err(rctx, 0, 0);
	    } else
	    {
	      ret = dspd_req_reply_err(rctx, 0, EINVAL);
	    }
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, EINVAL);
	}
      break;
    case DSPD_SOCKSRV_REQ_UMSRV:
      if ( inbufsize == sizeof(int32_t) )
	{
	  i32 = *(int32_t*)inbuf;
	  if ( i32 == DSPD_PCM_SBIT_FULLDUPLEX &&
	       cli->playback_device == cli->capture_device &&
	       cli->capture_device == cli->device &&
	       cli->device >= 0 )
	    {
	      //Unref full duplex
	      dspd_daemon_unref(cli->playback_device);
	      cli->playback_device = -1;
	      dspd_daemon_unref(cli->capture_device);
	      cli->capture_device = -1;
	      dspd_daemon_unref(cli->device);
	      cli->device = -1;
	    } else if ( i32 == 0 )
	    {
	      //Unref all
	      if ( cli->playback_device >= 0 )
		{
		  dspd_daemon_unref(cli->playback_device);
		  cli->playback_device = -1;
		}
	      if ( cli->capture_device >= 0 )
		{
		  dspd_daemon_unref(cli->capture_device);
		  cli->capture_device = -1;
		}
	      if ( cli->device >= 0 )
		{
		  dspd_daemon_unref(cli->device);
		  cli->device = -1;
		}
	    } else
	    {
	      //Unref specified stream
	      if ( cli->playback_device >= 0 && (i32 & DSPD_PCM_SBIT_PLAYBACK) )
		{
		  dspd_daemon_unref(cli->playback_device);
		  cli->playback_device = -1;
		}
	      if ( cli->capture_device >= 0 && (i32 & DSPD_PCM_SBIT_CAPTURE) )
		{
		  dspd_daemon_unref(cli->capture_device);
		  cli->capture_device = -1;
		}
	    }
	  ret = dspd_req_reply_err(rctx, 0, 0);
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, EINVAL);
	}
      break;
    case DSPD_SOCKSRV_REQ_SETSRV:
      if ( inbufsize == sizeof(int32_t) )
	{
	  i32 = *(int32_t*)inbuf;
	  //Must have already referenced the device
	  if ( cli->device >= 0 )
	    {
	      ret = 0;
	      if ( i32 == DSPD_PCM_SBIT_FULLDUPLEX )
		{
		  if ( cli->playback_device >= 0 )
		    dspd_daemon_unref(cli->playback_device);
		  if ( cli->capture_device >= 0 )
		    dspd_daemon_unref(cli->capture_device);
		  cli->playback_device = cli->device;
		  cli->device = -1;
		  cli->capture_device = cli->capture_device;
		  dspd_slist_ref(dspd_dctx.objects, cli->capture_device);
		} else if ( i32 == DSPD_PCM_SBIT_PLAYBACK )
		{
		  if ( cli->playback_device >= 0 )
		    dspd_daemon_unref(cli->playback_device);
		  cli->playback_device = cli->device;
		  cli->device = -1;
		} else if ( i32 == DSPD_PCM_SBIT_CAPTURE )
		{
		   if ( cli->capture_device >= 0 )
		     dspd_daemon_unref(cli->capture_device);
		  cli->capture_device = cli->device;
		  cli->device = -1;
		} else
		{
		  ret = EINVAL;
		}
	    } else
	    {
	      ret = -EBADFD;
	    }
	} else
	{
	  ret = EINVAL;
	}
      ret = dspd_req_reply_err(rctx, 0, ret);
      break;
    case DSPD_SOCKSRV_REQ_ALLOCZ:
      if ( cli->local )
	{
	  if ( inbufsize == sizeof(s) )
	    {
	      s = *(size_t*)inbuf;
	      ptr = calloc(1, s);
	      if ( ptr == NULL )
		ret = dspd_req_reply_err(rctx, 0, ENOMEM);
	      else
		ret = dspd_req_reply_buf(rctx, 0, &ptr, sizeof(ptr));
	    } else
	    {
	      ret = dspd_req_reply_err(rctx, 0, EINVAL);
	    }
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, EPERM);
	}
      break;
    case DSPD_SOCKSRV_REQ_FREE:
      if ( cli->local )
	{
	  if ( inbufsize == sizeof(ptr) )
	    {
	      ptr = *(void**)inbuf;
	      free(ptr);
	      ret = dspd_req_reply_err(rctx, 0, 0);
	    } else
	    {
	      ret = dspd_req_reply_err(rctx, 0, EINVAL);
	    }
	} else
	{
	  ret = dspd_req_reply_err(rctx, 0, EPERM);
	}
      break;
    default:
      ret = dspd_req_reply_err(rctx, 0, EINVAL);
      break;
    }
  return ret;
}

static int32_t get_streams(struct dspd_rctx *ctx, struct ss_cctx **cli, int32_t *playback, int32_t *capture, int32_t sbits)
{
  struct ss_cctx *c;
  int32_t ret = 0;
  *playback = -1;
  *capture = -1;
  c = dspd_req_userdata(ctx);
  if ( c->playback_stream >= 0 || c->capture_stream >= 0 )
    {
      *playback = c->playback_stream;
      *capture = c->capture_stream;
      if ( sbits )
	{
	  if ( (sbits & DSPD_PCM_SBIT_PLAYBACK) && c->playback_stream < 0 )
	    ret = -EBADFD;
	  if ( (sbits & DSPD_PCM_SBIT_CAPTURE) && c->capture_stream < 0 )
	    ret = -EBADFD;
	}
    } else if ( c->stream >= 0 )
    {
      *playback = c->stream;
      *capture = c->stream;
    } else
    {
      ret = -EBADF;
    }
  *cli = c;
  return ret;
}

static int32_t client_start(struct dspd_rctx *context,
			    uint32_t      req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  struct ss_cctx *cli = dspd_req_userdata(context);
  uint32_t stream = *(int32_t*)inbuf, s;
  int32_t ret = 0;
  dspd_time_t tstamps[2] = { 0, 0 }, tmp;
  size_t br = 0;
  struct dspd_sync_cmd cmd;
  //Full duplex with 2 streams
  if ( cli->playback_stream >= 0 && cli->capture_stream >= 0 && cli->playback_stream != cli->capture_stream && stream == DSPD_PCM_SBIT_FULLDUPLEX )
    {
      //Start both at the same time without a real syncgroup.
      memset(&cmd, 0, sizeof(cmd));
      cmd.streams = DSPD_PCM_SBIT_PLAYBACK;
      cmd.cmd = SGCMD_START;
      s = DSPD_PCM_SBIT_CAPTURE;
      ret = dspd_stream_ctl(&dspd_dctx,
			    cli->capture_stream,
			    req,
			    &s,
			    sizeof(s),
			    tstamps,
			    sizeof(tstamps),
			    &br);
      if ( ret == 0 )
	{
	  cmd.tstamp = tstamps[DSPD_PCM_STREAM_CAPTURE];
	  ret = dspd_stream_ctl(&dspd_dctx,
				cli->playback_stream,
				req,
				&cmd,
				sizeof(cmd),
				NULL,
				0,
				&br);
	  if ( ret == 0 )
	    {
	      tstamps[DSPD_PCM_STREAM_PLAYBACK] = cmd.tstamp;
	      br = sizeof(tstamps);
	    }
	}
    } else if ( cli->playback_stream >= 0 || cli->capture_stream >= 0 )
    {
      //Full duplex or half duplex with a single stream
      if ( cli->playback_stream >= 0 && (stream & DSPD_PCM_SBIT_PLAYBACK) )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				cli->playback_stream,
				req,
				&stream,
				sizeof(stream),
				tstamps,
				sizeof(tstamps),
				&br);
	  if ( ret == 0 && br != sizeof(tstamps) )
	    ret = -EPROTO;
	}
      //Need to send to the other stream if it is a separate object.
      if ( ret == 0 && cli->playback_stream != cli->capture_stream && cli->capture_stream >= 0 && (stream & DSPD_PCM_SBIT_CAPTURE) )
	{
	  tmp = tstamps[DSPD_PCM_STREAM_PLAYBACK];
	  ret = dspd_stream_ctl(&dspd_dctx,
				cli->capture_stream,
				req,
				&stream,
				sizeof(stream),
				tstamps,
				sizeof(tstamps),
				&br);
	  if ( ret == 0 && br != sizeof(tstamps) )
	    ret = -EPROTO;
	  else
	    tstamps[DSPD_PCM_STREAM_PLAYBACK] = tmp;
	}
    } else if ( cli->stream >= 0 )
    {
      //Control the single stream created with the old protocol.
      ret = dspd_stream_ctl(&dspd_dctx,
			    cli->stream,
			    req,
			    &stream,
			    sizeof(stream),
			    tstamps,
			    sizeof(tstamps),
			    &br);
      if ( ret == 0 && br != sizeof(tstamps) )
	ret = -EPROTO;
    } else
    {
      ret = -EBADF;
    }
  //The internal request always generates timestamps
  if ( ret == 0 && br == sizeof(tstamps) )
    {
      if ( outbufsize == sizeof(tstamps) )
	ret = dspd_req_reply_buf(context, 0, tstamps, sizeof(tstamps));
      else
	ret = dspd_req_reply_err(context, 0, 0);
    } else
    {
      if ( ret == 0 )
	ret = -EPROTO;
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}

static int32_t client_stop(struct dspd_rctx *context,
			   uint32_t      req,
			   const void   *inbuf,
			   size_t        inbufsize,
			   void         *outbuf,
			   size_t        outbufsize)
{
  struct ss_cctx *cli;
  uint32_t stream = *(int32_t*)inbuf;
  int32_t ret = 0;
  int32_t pstream = -1, cstream = -1;
  size_t br;
  ret = get_streams(context, &cli, &pstream, &cstream, 0);
  if ( ret == 0 && stream == 0 )
    {
      ret = -EINVAL; //bits may be wrong
    } else
    {
      if ( ret == 0 && (stream & DSPD_PCM_SBIT_PLAYBACK) && pstream >= 0 )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				pstream,
				req,
				&stream,
				sizeof(stream),
				NULL,
				0,
				&br);
	}
      if ( ret == 0 && (stream & DSPD_PCM_SBIT_CAPTURE) && cstream >= 0 && cstream != pstream )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				cstream,
				req,
				&stream,
				sizeof(stream),
				NULL,
				0,
				&br);
	}
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t merge_params(struct dspd_cli_params *p, 
			    const struct dspd_cli_params *pparams,
			    const struct dspd_cli_params *cparams)
{
  int32_t ret;
  if ( pparams->format == cparams->format &&
       pparams->rate == cparams->rate &&
       pparams->flags == cparams->flags &&
       pparams->xflags == cparams->xflags )
    {
      memset(p, 0, sizeof(*p));
      p->channels = (cparams->channels << 16U) | pparams->channels;
      p->rate = pparams->rate;
      p->bufsize = MIN(pparams->bufsize, cparams->bufsize);
      p->fragsize = MIN(pparams->fragsize, cparams->fragsize);
      p->stream = DSPD_PCM_SBIT_FULLDUPLEX;
      p->latency = MIN(pparams->latency, cparams->latency);
      p->flags = pparams->flags;
      p->min_latency = 0;
      p->max_latency = 0;
      p->src_quality = MIN(pparams->src_quality, cparams->src_quality);
      p->xflags = pparams->xflags | DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
      ret = 0;
    } else
    {
      ret = EINVAL;
    }
  return ret;
}


static int32_t client_getparams(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct ss_cctx *cli;
  uint32_t stream = *(int32_t*)inbuf;
  int32_t ret = 0;
  int32_t pstream = -1, cstream = -1, s;
  size_t br;
  struct dspd_cli_params pparams, cparams, *p = outbuf;
  ret = get_streams(context, &cli, &pstream, &cstream, stream);
  if ( ret == 0 )
    {
      memset(&pparams, 0, sizeof(pparams));
      memset(&cparams, 0, sizeof(cparams));
      if ( (stream & DSPD_PCM_SBIT_PLAYBACK) && pstream >= 0 )
	{
	  s = DSPD_PCM_SBIT_PLAYBACK;
	  ret = dspd_stream_ctl(&dspd_dctx,
				pstream,
				req,
				&s,
				sizeof(s),
				&pparams,
				sizeof(pparams),
				&br);
	} else
	{
	  memset(&pparams, 0, sizeof(pparams));
	}
      if ( ret == 0 && (stream & DSPD_PCM_SBIT_CAPTURE) && cstream >= 0 )
	{
	  s = DSPD_PCM_SBIT_CAPTURE;
	  ret = dspd_stream_ctl(&dspd_dctx,
				cstream,
				req,
				&s,
				sizeof(s),
				&cparams,
				sizeof(cparams),
				&br);
	}
    }
  if ( ret == 0 )
    {
      //memset(p, 0, sizeof(p));
      if ( cparams.rate == 0 &&
	   cparams.channels == 0 &&
	   (stream & DSPD_PCM_SBIT_PLAYBACK) )
	{
	  memcpy(p, &pparams, sizeof(pparams));
	  p->xflags |= DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
	  p->stream = DSPD_PCM_SBIT_PLAYBACK;
	} else if ( pparams.rate == 0 &&
		    pparams.channels == 0 &&
		    (stream & DSPD_PCM_SBIT_CAPTURE))
	{
	  memcpy(p, &cparams, sizeof(cparams));
	  p->xflags |= DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
	  p->channels <<= 16U;
	  p->stream = DSPD_PCM_SBIT_CAPTURE;
	} else
	{
	  ret = merge_params(p, &pparams, &cparams);
	}
    }
	   
	   
	   
  if ( ret != 0 )
    ret = dspd_req_reply_err(context, 0, ret);
  else
    ret = dspd_req_reply_buf(context, 0, p, sizeof(*p));
  return ret;
}

static int32_t copy_cli_params(struct dspd_cli_params *out, const struct dspd_cli_params *in, int32_t stream)
{
  int32_t ret = 0;
  memcpy(out, in, sizeof(*in));
  if ( in->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
    {
      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
	out->channels = DSPD_CLI_PCHAN(in->channels);
      else if ( stream == DSPD_PCM_SBIT_CAPTURE )
	out->channels = DSPD_CLI_CCHAN(in->channels);
      out->xflags &= ~DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
    }
  if ( stream )
    {
      if ( stream & in->stream )
	out->stream = stream;
      else
	ret = EINVAL;
    }
  return ret;
}


static int32_t client_setparams(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  struct dspd_cli_params params, cparams, pparams;
  const struct dspd_cli_params *p = inbuf;
  int32_t ret;
  size_t br;
  memset(&cparams, 0, sizeof(cparams));
  memset(&pparams, 0, sizeof(pparams));
  if ( p->stream == 0 )
    ret = EBADF;
  else
    ret = get_streams(context, &cli, &pstream, &cstream, p->stream);
  if ( ret == 0 )
    {
      if ( (p->stream & DSPD_PCM_SBIT_PLAYBACK) && pstream >= 0 )
	{
	  ret = copy_cli_params(&params, p, DSPD_PCM_SBIT_PLAYBACK);
	  if ( ret == 0 )
	    ret = dspd_stream_ctl(&dspd_dctx,
				  pstream,
				  DSPD_SCTL_CLIENT_SETPARAMS,
				  &params,
				  sizeof(params),
				  &pparams,
				  sizeof(pparams),
				  &br);
	}
      if ( ret == 0 && (p->stream & DSPD_PCM_SBIT_CAPTURE) && cstream >= 0 )
	{
	  ret = copy_cli_params(&params, p, DSPD_PCM_SBIT_CAPTURE);
	  if ( ret == 0 )
	    ret = dspd_stream_ctl(&dspd_dctx,
				  cstream,
				  DSPD_SCTL_CLIENT_SETPARAMS,
				  &params,
				  sizeof(params),
				  &cparams,
				  sizeof(cparams),
				  &br);
	}
    }
  if ( ret == 0 )
    {
      if ( p->stream == DSPD_PCM_SBIT_FULLDUPLEX )
	{
	  ret = merge_params(&params, &pparams, &cparams);
	  if ( ret == 0 )
	    ret = dspd_req_reply_buf(context, 0, &params, sizeof(params));
	  else
	    ret = dspd_req_reply_err(context, 0, ret);
	} else if ( p->stream == DSPD_PCM_SBIT_PLAYBACK )
	{
	  ret = dspd_req_reply_buf(context, 0, &pparams, sizeof(pparams));
	} else
	{
	  ret = dspd_req_reply_buf(context, 0, &cparams, sizeof(cparams));
	}
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}

static int32_t client_setvolume(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  const struct dspd_stream_volume *sv = inbuf;
  int32_t ret;
  size_t br;
  ret = get_streams(context, &cli, &pstream, &cstream, sv->stream);
  if ( ret == 0 )
    {
      if ( sv->stream & DSPD_PCM_SBIT_PLAYBACK )
	ret = dspd_stream_ctl(&dspd_dctx,
			      pstream,
			      DSPD_SCTL_CLIENT_SETVOLUME,
			      sv,
			      sizeof(*sv),
			      NULL,
			      0,
			      &br);
      if ( ret == 0 && pstream != cstream && (sv->stream & DSPD_PCM_SBIT_CAPTURE) )
	ret = dspd_stream_ctl(&dspd_dctx,
			      cstream,
			      DSPD_SCTL_CLIENT_SETVOLUME,
			      sv,
			      sizeof(*sv),
			      NULL,
			      0,
			      &br);
    }
  return dspd_req_reply_err(context, 0, ret);
}
static int32_t client_getvolume(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1, s = -1;
  uint32_t stream = *(uint32_t*)inbuf;
  int32_t ret = EINVAL;
  size_t br;
  if ( stream )
    {
      ret = get_streams(context, &cli, &pstream, &cstream, stream);
      if ( ret == 0 )
	{
	  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
	    s = pstream;
	  else 
	    s = cstream;
	  ret = dspd_stream_ctl(&dspd_dctx,
				s,
				DSPD_SCTL_CLIENT_GETVOLUME,
				&stream,
				sizeof(stream),
				outbuf,
				outbufsize,
				&br);
	}
    }
  if ( ret == 0 )
    ret = dspd_req_reply_buf(context, 0, outbuf, outbufsize);
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static int32_t client_connect(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t index = *(int32_t*)inbuf, ret = EINVAL;
  size_t br;
  if ( index >= -1 )
    {
      ret = get_streams(context, &cli, &pstream, &cstream, 0);
      if ( index == -1 )
	{
	  if ( (pstream >= 0 && cli->playback_device >= 0) || 
	       (cstream >= 0 && cli->capture_device >= 0) )
	    {
	      //Connect all
	      if ( pstream >= 0 && cli->playback_device >= 0 )
		{
		  ret = dspd_stream_ctl(&dspd_dctx,
					pstream,
					req,
					&cli->playback_device,
					sizeof(cli->playback_device),
					NULL,
					0,
					&br);
		} else
		{
		  ret = 0;
		}
	      if ( ret == 0 && cstream >= 0 && cstream != pstream && cli->capture_device >= 0 )
		{
		  ret = dspd_stream_ctl(&dspd_dctx,
					cstream,
					req,
					&cli->capture_device,
					sizeof(cli->capture_device),
					NULL,
					0,
					&br);
		}
	    } else
	    {
	      ret = EBADFD;
	    }
	} else if ( index == cli->playback_device )
	{
	  if ( pstream >= 0 )
	    ret = dspd_stream_ctl(&dspd_dctx,
				  pstream,
				  req,
				  &index,
				  sizeof(index),
				  NULL,
				  0,
				  &br);
	} else if ( index == cli->capture_device )
	{
	  if ( cstream >= 0 )
	    ret = dspd_stream_ctl(&dspd_dctx,
				  cstream,
				  req,
				  &index,
				  sizeof(index),
				  NULL,
				  0,
				  &br);
	} else if ( index == cli->device )
	{
	  if ( cli->stream >= 0 )
	    ret = dspd_stream_ctl(&dspd_dctx,
				  cli->stream,
				  req,
				  &index,
				  sizeof(index),
				  NULL,
				  0,
				  &br);
	}
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t client_disconnect(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct ss_cctx *cli = dspd_req_userdata(context);
  size_t br;
  if ( cli->playback_stream >= 0 )
    dspd_stream_ctl(&dspd_dctx, cli->playback_stream, req, NULL, 0, NULL, 0, &br);
  if ( cli->capture_stream >= 0 )
    dspd_stream_ctl(&dspd_dctx, cli->capture_stream, req, NULL, 0, NULL, 0, &br);
  if ( cli->stream >= 0 )
    dspd_stream_ctl(&dspd_dctx, cli->stream, req, NULL, 0, NULL, 0, &br);
  return dspd_req_reply_err(context, 0, 0);
}

static int32_t client_mapbuf(struct dspd_rctx *context,
			     uint32_t      req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1, s;
  int32_t stream = *(int32_t*)inbuf, ret = EINVAL;
  size_t br = 0;
  struct dspd_client_shm *shm = outbuf;
  ret = get_streams(context, &cli, &pstream, &cstream, stream);
  if ( ret == 0 )
    {
      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
	s = pstream;
      else
	s = cstream;
      ret = dspd_stream_ctl(&dspd_dctx,
			    s,
			    req,
			    inbuf,
			    inbufsize,
			    outbuf,
			    outbufsize,
			    &br);
    }
  if ( ret == 0 && br > 0 )
    {
      if ( shm->flags & DSPD_SHM_FLAG_MMAP )
	ret = dspd_req_reply_fd(context, 0, shm, br, shm->arg);
      else
	ret = dspd_req_reply_buf(context, 0, shm, br);
    } else
    {
      ret = dspd_req_reply_err(context, 0, 0);
    }
  return ret;
}

static int32_t client_getchannelmap(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t stream = *(int32_t*)inbuf, ret = EINVAL;
  size_t br = 0;
  ret = get_streams(context, &cli, &pstream, &cstream, stream);
  if ( ret == 0 )
    {
      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				pstream,
				req,
				inbuf,
				inbufsize,
				outbuf,
				outbufsize,
				&br);
	} else if ( stream == DSPD_PCM_SBIT_CAPTURE )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				cstream,
				req,
				inbuf,
				inbufsize,
				outbuf,
				outbufsize,
				&br);
	} else
	{
	  ret = EBADF;
	}
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(context, 0, outbuf, outbufsize);
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static int32_t client_setchannelmap(struct dspd_rctx *context,
				    uint32_t      req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  const struct dspd_chmap *pkt = inbuf;
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret = EINVAL;
  size_t br = 0;
  ret = get_streams(context, &cli, &pstream, &cstream, pkt->stream);
  if ( ret == 0 )
    {
      if ( pkt->stream == DSPD_PCM_SBIT_PLAYBACK )
	ret = dspd_stream_ctl(&dspd_dctx, pstream, req, inbuf, inbufsize, outbuf, outbufsize, &br);
      else if ( pkt->stream == DSPD_PCM_SBIT_CAPTURE )
	ret = dspd_stream_ctl(&dspd_dctx, cstream, req, inbuf, inbufsize, outbuf, outbufsize, &br);
      else
	ret = EINVAL;
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t client_stat(struct dspd_rctx *context,
			   uint32_t      req,
			   const void   *inbuf,
			   size_t        inbufsize,
			   void         *outbuf,
			   size_t        outbufsize)
{
  struct dspd_cli_stat st, *out = outbuf;
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret = EINVAL;
  size_t br = 0;
  ret = get_streams(context, &cli, &pstream, &cstream, 0);
  if ( ret == 0 )
    {
      memset(out, 0, sizeof(*out));
      if ( pstream >= 0 )
	ret = dspd_stream_ctl(&dspd_dctx, pstream, req, NULL, 0, outbuf, outbufsize, &br);
      if ( ret == 0 && pstream >= 0 && pstream != cstream && cstream >= 0 )
	{
	  ret = dspd_stream_ctl(&dspd_dctx, cstream, req, NULL, 0, &st, sizeof(st), &br);
	  if ( ret == 0 )
	    {
	      memcpy(&out->capture, &st.capture, sizeof(st.capture));
	      if ( out->error == 0 && st.error != 0 )
		out->error = st.error;
	      out->streams |= st.streams;
	      out->flags |= st.flags;
	    }
	}
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static int32_t client_reserve(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret = EINVAL;
  size_t br = 0;
  int32_t server = -1;
  if ( inbufsize == sizeof(server) )
    server = *(int32_t*)inbuf;


  ret = get_streams(context, &cli, &pstream, &cstream, 0);
  if ( ret == 0 )
    {
      if ( server == -1 )
	{
	  if ( pstream >= 0 && cli->playback_device >= 0 )
	    ret = dspd_stream_ctl(&dspd_dctx, pstream, req, &cli->playback_device, sizeof(cli->playback_device), NULL, 0, &br);
	  if ( ret == 0 && cstream >= 0 && cli->capture_device >= 0 && cstream != pstream )
	    ret = dspd_stream_ctl(&dspd_dctx, cstream, req, &cli->capture_device, sizeof(cli->capture_device), NULL, 0, &br);
	} else if ( server >= 0 )
	{
	  if ( cli->device >= 0 && cli->stream >= 0 && pstream == cstream )
	    {
	      ret = dspd_stream_ctl(&dspd_dctx, cli->stream, req, &server, sizeof(server), NULL, 0, &br);
	    } else
	    {
	      if ( pstream >= 0 && cli->playback_device == server )
		ret = dspd_stream_ctl(&dspd_dctx, pstream, req, &server, sizeof(server), NULL, 0, &br);
	      if ( ret == 0 && cstream >= 0 && cli->capture_device == server && cstream != pstream )
		ret = dspd_stream_ctl(&dspd_dctx, cstream, req, &server, sizeof(server), NULL, 0, &br);
	    }
	} else
	{
	  ret = EINVAL;
	}
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t client_settrigger(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret = EINVAL;
  size_t br = 0;
  int32_t streams, s;
  dspd_time_t result[2], pts = 0, cts = 0;
  streams = *(int32_t*)inbuf;
  if ( streams == DSPD_PCM_SBIT_FULLDUPLEX )
    return client_start(context, req, inbuf, inbufsize, outbuf, outbufsize);
  else if ( streams == 0 )
    return client_stop(context, req, inbuf, inbufsize, outbuf, outbufsize);

  ret = get_streams(context, &cli, &pstream, &cstream, streams);
  if ( ret == 0 )
    {
      if ( pstream >= 0 )
	{
	  ret = dspd_stream_ctl(&dspd_dctx, pstream, req, &streams, sizeof(s), result, sizeof(result), &br);
	  if ( ret == 0 && br != sizeof(result) )
	    ret = -EPROTO;
	  else
	    pts = result[DSPD_PCM_STREAM_PLAYBACK];
	}
      if ( cstream >= 0 && cstream != pstream )
	{
	  ret = dspd_stream_ctl(&dspd_dctx, cstream, req, &streams, sizeof(s), result, sizeof(result), &br);
	  if ( ret == 0 && br != sizeof(result) )
	    ret = -EPROTO;
	  else
	    cts = result[DSPD_PCM_STREAM_CAPTURE];
	} else
	{
	  cts = result[DSPD_PCM_STREAM_CAPTURE];
	}
    }
  if ( ret == 0 && br > 0 && br == outbufsize )
    {
      result[0] = pts;
      result[1] = cts;
      ret = dspd_req_reply_buf(context, 0, result, sizeof(result));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}

static int32_t client_gettrigger(struct dspd_rctx *context,
				 uint32_t      req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret = EINVAL;
  size_t br = 0;
  int32_t streams = 0, s;  
  ret = get_streams(context, &cli, &pstream, &cstream, 0);
  if ( ret == 0 )
    {
      if ( pstream >= 0 && (streams & DSPD_PCM_SBIT_PLAYBACK) )
	{
	  if ( cstream == pstream && streams == DSPD_PCM_SBIT_FULLDUPLEX )
	    cstream = -1;
	  ret = dspd_stream_ctl(&dspd_dctx, pstream, req, NULL, 0, &s, sizeof(s), &br);
	  if ( ret == 0 )
	    streams |= s;
	}
      if ( ret == 0 && cstream >= 0 && (streams & DSPD_PCM_SBIT_CAPTURE) )
	{
	  ret = dspd_stream_ctl(&dspd_dctx, cstream, req, NULL, 0, &s, sizeof(s), &br);
	  if ( ret == 0 )
	    streams |= s;
	}
    }
  if ( ret == 0 && br == sizeof(s) )
    {
      ret = dspd_req_reply_buf(context, 0, &streams, sizeof(streams));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}

static int32_t client_syncgroup(struct dspd_rctx *context,
				uint32_t      req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret = EINVAL;
  size_t br = 0;
  const struct dspd_sg_info *in = inbuf;
  struct dspd_sg_info *out = outbuf, tmp;
  ret = get_streams(context, &cli, &pstream, &cstream, 0);
  if ( ret == 0 )
    {
      if ( (in != NULL && inbufsize < sizeof(*in) ) || (out != NULL && outbufsize < sizeof(*out)) )
	{
	  ret = EINVAL;
	} else if ( in && out )
	{
	  if ( pstream >= 0 && cstream >= 0 && pstream != cstream )
	    {
	      ret = dspd_stream_ctl(&dspd_dctx, pstream, req, in, inbufsize, &tmp, sizeof(tmp), &br);
	      if ( ret == 0 && br == sizeof(*out) )
		{
		  ret = dspd_stream_ctl(&dspd_dctx, cstream, req, &tmp, sizeof(tmp), outbuf, outbufsize, &br);
		  if ( ret == 0 && br != sizeof(*out) )
		    ret = EPROTO; //Should not happen
		} else if ( ret == 0 )
		{
		  ret = EPROTO; //Should not happen
		}
	    } else if ( pstream >= 0 )
	    {
	      ret = dspd_stream_ctl(&dspd_dctx, pstream, req, in, inbufsize, &tmp, sizeof(tmp), &br);
	    } else if ( cstream >= 0 )
	    {
	      ret = dspd_stream_ctl(&dspd_dctx, cstream, req, in, inbufsize, &tmp, sizeof(tmp), &br);
	    } else
	    {
	      ret = EBADF; //Should not happen.
	    }
	} else if ( in == NULL && out != NULL )
	{
	  //Get syncgroup.  Both streams are normally part of the same syncgroup.  If the client
	  //changed this then nothing bad is going to happen on the server side.
	  if ( pstream >= 0 )
	    ret = dspd_stream_ctl(&dspd_dctx, pstream, req, in, inbufsize, out, outbufsize, &br);
	  else if ( cstream >= 0 )
	    ret = dspd_stream_ctl(&dspd_dctx, pstream, req, in, inbufsize, out, outbufsize, &br);
	} else if ( in != NULL && outbuf == NULL )
	{
	  //Add to existing sync group
	  if ( pstream >= 0 )
	    ret = dspd_stream_ctl(&dspd_dctx, pstream, req, in, inbufsize, out, outbufsize, &br);
	  if ( ret == 0 && cstream >= 0 && pstream != cstream )
	    ret = dspd_stream_ctl(&dspd_dctx, cstream, req, in, inbufsize, out, outbufsize, &br);
	} else
	{
	  //Remove from syncgroup
	  if ( pstream >= 0 )
	    ret = dspd_stream_ctl(&dspd_dctx, pstream, req, in, inbufsize, out, outbufsize, &br);
	  if ( ret == 0 && cstream >= 0 && pstream != cstream )
	    ret = dspd_stream_ctl(&dspd_dctx, cstream, req, in, inbufsize, out, outbufsize, &br);
	}
    }
  if ( ret == 0 && br > 0 && outbufsize > 0 )
    ret = dspd_req_reply_buf(context, 0, outbuf, br);
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static int32_t client_synccmd(struct dspd_rctx *context,
			      uint32_t      req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret = EINVAL;
  size_t br = 0;
  ret = get_streams(context, &cli, &pstream, &cstream, 0);
  if ( ret == 0 )
    {
      //If there are 2 streams then they are normally part of the same syncgroup.  If the client
      //change this then nothing unsafe will happen.
      if ( pstream >= 0 )
	ret = dspd_stream_ctl(&dspd_dctx, pstream, req, inbuf, inbufsize, outbuf, outbufsize, &br);
      else if ( cstream >= 0 )
	ret = dspd_stream_ctl(&dspd_dctx, cstream, req, inbuf, inbufsize, outbuf, outbufsize, &br);
      else
	ret = EBADF; //Should not happen
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(context, 0, outbuf, br);
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static int32_t client_swparams(struct dspd_rctx *context,
			       uint32_t      req,
			       const void   *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret = EINVAL;
  size_t br = 0;
  ret = get_streams(context, &cli, &pstream, &cstream, 0);
  if ( ret == 0 )
    {
      //Full duplex single streams have one swparams so the same args should be sent to each stream.
      if ( pstream >= 0 )
	ret = dspd_stream_ctl(&dspd_dctx, pstream, req, inbuf, inbufsize, outbuf, outbufsize, &br);
      if ( ret == 0 && cstream >= 0 && pstream != cstream )
	ret = dspd_stream_ctl(&dspd_dctx, cstream, req, inbuf, inbufsize, outbuf, outbufsize, &br);
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(context, 0, outbuf, br);
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static int32_t client_pause(struct dspd_rctx *context,
			    uint32_t      req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  struct ss_cctx *cli;
  int32_t pstream = -1, cstream = -1;
  int32_t ret;
  size_t br = 0;
  dspd_time_t tstamps[2] = { 0, 0 }, ts;
  
  ret = get_streams(context, &cli, &pstream, &cstream, 0);
  if ( ret == 0 )
    {
      if ( pstream >= 0 )
	ret = dspd_stream_ctl(&dspd_dctx, pstream, req, inbuf, inbufsize, tstamps, sizeof(tstamps), &br);
      if ( ret == 0 && cstream >= 0 && pstream != cstream )
	{
	  ts = tstamps[DSPD_PCM_STREAM_PLAYBACK];
	  ret = dspd_stream_ctl(&dspd_dctx, cstream, req, inbuf, inbufsize, tstamps, sizeof(tstamps), &br);
	  tstamps[DSPD_PCM_STREAM_PLAYBACK] = ts;
	}
    }
  if ( ret == 0 && br > 0 && outbufsize >= br )
    ret = dspd_req_reply_buf(context, 0, tstamps, br);
  else
    ret = dspd_req_reply_err(context, 0, ret);
  return ret;
}

static const struct dspd_req_handler client_req_handlers[] = {
  [CLIDX(DSPD_SCTL_CLIENT_START)] = {
    .handler = client_start,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_STOP)] = {
    .handler = client_stop,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETPARAMS)] = {
    .handler = client_getparams,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_cli_params),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETPARAMS)] = {
    .handler = client_setparams,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_cli_params),
    .outbufsize = sizeof(struct dspd_cli_params),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETVOLUME)] = {
    .handler = client_setvolume,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_stream_volume),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETVOLUME)] = {
    .handler = client_getvolume,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(float),
  },
  [CLIDX(DSPD_SCTL_CLIENT_CONNECT)] = {
    .handler = client_connect,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_DISCONNECT)] = {
    .handler = client_disconnect,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_RAWPARAMS)] = {
    .handler = NULL,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(int64_t), //(stream<<32)|server
    .outbufsize = sizeof(struct dspd_cli_params),
  },
  [CLIDX(DSPD_SCTL_CLIENT_MAPBUF)] = {
    .handler = client_mapbuf,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_client_shm),
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETCHANNELMAP)] = {
    .handler = client_getchannelmap,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_chmap),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETCHANNELMAP)] = {
    .handler = client_setchannelmap,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_chmap),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_SETCB)] = {
    .handler = NULL,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_client_cb),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_STAT)] = {
    .handler = client_stat,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(struct dspd_cli_stat),
  },
  [CLIDX(DSPD_SCTL_CLIENT_RESERVE)] = {
    .handler = client_reserve,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },

  [CLIDX(DSPD_SCTL_CLIENT_SETTRIGGER)] = {
    .handler = client_settrigger,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_GETTRIGGER)] = {
    .handler = client_gettrigger,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(int32_t),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SYNCGROUP)] = {
    .handler = client_syncgroup,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_SYNCCMD)] = {
    .handler = client_synccmd,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_sync_cmd),
    .outbufsize = 0,
  },
  [CLIDX(DSPD_SCTL_CLIENT_LOCK)] = {
    .handler = NULL,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = sizeof(struct dspd_lock_result),
  },
  [CLIDX(DSPD_SCTL_CLIENT_SWPARAMS)] = {
    .handler = client_swparams,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = 0,

  },
  [CLIDX(DSPD_SCTL_CLIENT_PAUSE)] = {
    .handler = client_pause,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(int32_t),
    .outbufsize = 0,

  },
};

static int socksrv_dispatch_multi_req(struct dspd_rctx *rctx,
				      uint32_t             req,
				      const void          *inbuf,
				      size_t        inbufsize,
				      void         *outbuf,
				      size_t        outbufsize)
{
  int ret;
  uint64_t r = req;
  r <<= 32U;
  r |= CLIDX(req);
  ret = dspd_daemon_dispatch_ctl(rctx, client_req_handlers, 
				 ARRAY_SIZE(client_req_handlers), 
				 r,
				 inbuf,
				 inbufsize,
				 outbuf,
				 outbufsize);
 
  return ret;
}


static int client_dispatch_pkt(struct ss_cctx *cli)
{
  struct dspd_req *req = cli->pkt_in;
  int ret = 0;
  size_t len;
  void *iptr, *optr;
  struct dspd_req_pointers *ptrs;
  cli->rctx.user_data = NULL;
  cli->rctx.bytes_returned = 0;
  if ( cli->rctx.flags & DSPD_REQ_FLAG_ERROR )
    {
      cli->rctx.outbufsize = 0;
    } else
    {
      cli->rctx.outbufsize = req->rdata.rlen;
      if ( cli->rctx.outbufsize > SS_MAX_PAYLOAD )
	cli->rctx.outbufsize = SS_MAX_PAYLOAD;
    }
  cli->rctx.fd = cli->pkt_fd;
  cli->pkt_fd = -1; //FD handled by request dispatch
  cli->rctx.index = req->stream;
  cli->rctx.flags = cli->pkt_flags;
  cli->rctx.flags &= ~(DSPD_REQ_FLAG_UNIX_FAST_IOCTL|DSPD_REQ_FLAG_UNIX_IOCTL);
  //Remote connections can't send pointers.
  if ( cli->local == false )
    cli->rctx.flags &= ~DSPD_REQ_FLAG_POINTER;
  len = cli->pkt_size - sizeof(*cli->pkt_in);

  if ( cli->rctx.flags & DSPD_REQ_FLAG_POINTER )
    {
      if ( cli->local == false || len != sizeof(struct dspd_req_pointers) )
	return dspd_req_reply_err(&cli->rctx, 0, EINVAL);
    }

  if ( cli->rctx.flags & DSPD_REQ_FLAG_POINTER )
    {
      ptrs = (struct dspd_req_pointers*)cli->pkt_in->pdata;
      cli->rctx.outbuf = ptrs->outbuf;
      cli->rctx.outbufsize = ptrs->outbufsize;
      iptr = ptrs->inbuf;
      len = ptrs->inbufsize;
      optr = ptrs->outbuf;
    } else
    {
      if ( len == 0 )
	iptr = NULL;
      else
	iptr = cli->pkt_in->pdata;
      if ( cli->rctx.outbufsize == 0 )
	optr = NULL;
      else
	optr = cli->rctx.outbuf;
      cli->rctx.outbuf = cli->pkt_out->pdata;
    }
  if ( req->stream == -1 )
    {
      //Socket server request
      cli->rctx.user_data = cli;
      if ( cli->pkt_cmd >= DSPD_SCTL_CLIENT_MIN && cli->pkt_cmd <= DSPD_SCTL_CLIENT_MAX )
	{
	  ret = socksrv_dispatch_multi_req(&cli->rctx,
					   cli->pkt_cmd,
					   iptr,
					   len,
					   optr,
					   cli->rctx.outbufsize);
	} else
	{
	  ret = socksrv_dispatch_req(&cli->rctx,
				     cli->pkt_cmd,
				     iptr,
				     len,
				     optr,
				     cli->rctx.outbufsize);
	}
    } else if ( req->stream == 0 )
    {
      //All requests to object 0 are ok because this is the special
      //daemon context.
      ret = dspd_slist_ctl(dspd_dctx.objects,
			   &cli->rctx,
			   cli->pkt_cmd,
			   iptr,
			   len,
			   optr,
			   cli->rctx.outbufsize);
    } else if ( stream_valid(cli, req->stream) )
    {
      //Can make any request
      ret = dspd_slist_ctl(dspd_dctx.objects,
			   &cli->rctx,
			   cli->pkt_cmd,
			   iptr,
			   len,
			   optr,
			   cli->rctx.outbufsize);
    } else
    {
      if ( cli->pkt_cmd >= DSPD_SCTL_SERVER_MIN )
	{
	  //Send commands to any server.
	  ret = dspd_daemon_ref(cli->rctx.index, DSPD_DCTL_ENUM_TYPE_SERVER);
	  if ( ret == 0 )
	    {
	      ret = dspd_slist_ctl(dspd_dctx.objects,
				   &cli->rctx,
				   cli->pkt_cmd,
				   iptr,
				   len,
				   optr,
				   cli->rctx.outbufsize);
	      dspd_daemon_unref(cli->rctx.index);
	    } else
	    {
	      ret = dspd_req_reply_err(&cli->rctx, 0, ret);
	    }
	} else
	{
	  ret = dspd_req_reply_err(&cli->rctx, 0, EINVAL);
	}
    }

  //Close received fd if the handler did not get it.
  if ( cli->rctx.fd >= 0 )
    {
      close(cli->rctx.fd);
      cli->rctx.fd = -1;
    }

  return ret;
}
static void client_async_cmd_cb(struct cbpoll_ctx *ctx,
				void *data,
				int64_t arg,
				int32_t index,
				int32_t fd)
{
  int32_t ret;
  struct ss_cctx *cli = data;
  int64_t a;
  
  ret = client_dispatch_pkt(cli);
  if ( ret == -EINPROGRESS )
    {
      a = 0;         //Not an error
      a |= EPOLLOUT; //Need to finish
    } else
    {
      a = ret;      //pipe event returns same value
      a <<= 32;
      if ( ret >= 0 ) //If not an error then poll for more input
	a = EPOLLIN;
    }
 
  //Send the results back (decreases refcount on poll thread)
  cbpoll_deferred_work_complete(ctx, index, a);
}


static int client_async_dispatch(struct ss_cctx *cli)
{
  //Send to async work thread (increases refcount)
  cbpoll_queue_deferred_work(cli->cbctx,
			     cli->index,
			     0,
			     client_async_cmd_cb);
  return cbpoll_set_events(cli->cbctx, cli->index, 0);
}
static int client_dispatch_packet(struct ss_cctx *cli)
{
  struct dspd_req *req = cli->pkt_in;
  int ret;
  if ( (req->stream == -1 || stream_valid(cli, req->stream)) &&
       (cli->pkt_cmd == DSPD_SCTL_CLIENT_START ||
	cli->pkt_cmd == DSPD_SCTL_CLIENT_STOP ||
	cli->pkt_cmd == DSPD_SCTL_CLIENT_SETTRIGGER) )
    {
      ret = client_dispatch_pkt(cli);
      if ( ret == -EINPROGRESS )
	ret = cbpoll_set_events(cli->cbctx, cli->index, EPOLLOUT);
    } else if ( req->stream < 0 && cli->pkt_cmd == DSPD_SOCKSRV_REQ_QUIT )
    {
      ret = -1; //Disconnect 
    } else
    {
      ret = client_async_dispatch(cli);
    }
  return ret;
}
static int prepare_event_pkt(struct cbpoll_ctx *context, int index, struct ss_cctx *cli, bool pollin)
{
  struct dspd_req *req = (struct dspd_req*)cli->pkt_out;
  int ret;
  req->len = sizeof(*req);
  req->cmd = DSPD_DCTL_ASYNC_EVENT;
  req->flags = cli->event_flags;
  cli->event_flags = 0;
  //cli->event_sent = true;
  req->stream = -1;
  req->rdata.rlen = 0;
  req->bytes_returned = 0;
  ret = sendreq(cli);
  if ( ret == req->len )
    {
      if ( pollin )
	ret = cbpoll_set_events(context, index, POLLIN);
      else
	ret = 0;
    } else if ( ret == -EINPROGRESS )
    {
      ret = cbpoll_set_events(context, index, POLLOUT);
    }
  return ret;
}
static int client_fd_event(void *data, 
			   struct cbpoll_ctx *context,
			   int index,
			   int fd,
			   int revents)
{
  int32_t ret;
  struct ss_cctx *cli = data;
  if ( revents & (POLLERR|POLLNVAL|POLLHUP|POLLRDHUP) )
      return -1;

  if ( revents & POLLIN )
    {
      ret = dspd_req_recv(cli->req_ctx);
      if ( ret != -EINPROGRESS )
	{
	  if ( ret <= 0 )
	    return -1;
	  ret = dspd_req_reap(cli->req_ctx, 
			      (void**)&cli->pkt_in,
			      &cli->pkt_size,
			      &cli->pkt_fd);
	  if ( ret != -EAGAIN && ret != -EINPROGRESS )
	    {
	      if ( ret < 0 )
		return -1;
	      cli->pkt_cmd = cli->pkt_in->cmd;
	      cli->pkt_tag = cli->pkt_in->tag;
	      if ( ! cli->local )
		cli->pkt_flags = DSPD_REQ_FLAG_REMOTE;
	      else
		cli->pkt_flags = 0;
	      if ( cli->pkt_fd >= 0 )
		cli->pkt_flags |= DSPD_REQ_FLAG_CMSG_FD;
	      cli->pkt_flags |= cli->pkt_in->flags;
	      cli->pkt_stream = cli->pkt_in->stream;
	      //cli->event_sent = false;
	      ret = client_dispatch_packet(cli);
	      	      
	      return ret;
	    }
	}
    } else if ( revents & POLLOUT )
    {
      ret = sendreq(cli);
      if ( ret == -EINPROGRESS )
	{
	  return 0;
	} else if ( ret <= 0 )
	{
	  return -1;
	} else
	{
	  if ( cli->pkt_out->flags & DSPD_REPLY_FLAG_CLOSEFD )
	    {
	      close(cli->fd_out);
	      cli->fd_out = -1;
	    }
	  if ( cli->event_flags != 0 )
	    return prepare_event_pkt(context, index, cli, true);
	}
      return cbpoll_set_events(context, index, POLLIN);
    }
  return 0;
}

static int prepare_events(struct cbpoll_ctx *context, int index, struct ss_cctx *cli)
{
  //if ( cli->event_sent || (cbpoll_get_events(context, index) & POLLOUT) ||
  //dspd_req_input_pending(cli->req_ctx) )
  //return 0;
  if ( (cbpoll_get_events(context, index) & POLLOUT) || dspd_req_input_pending(cli->req_ctx) )
    return 0;
  return prepare_event_pkt(context, index, cli, true);
}

int client_pipe_event(void *data, 
		      struct cbpoll_ctx *context,
		      int index,
		      int fd,
		      const struct cbpoll_pipe_event *event)
{
  int32_t ret = 0;
  int32_t events;
  struct ss_cctx *cli = data;
  int32_t dev;
  if ( event->msg == MSG_EVENT_FLAGS )
    {
      dev = event->arg >> 32;
      if ( dev >= 0 )
	{
	  if ( device_valid(cli, dev) && (stream_valid(cli, event->stream) || (cli->stream < 0)) )
	    {
	      cli->event_flags |= event->arg & 0xFFFF;
	      ret = prepare_events(context, index, cli);
	    }
	} else
	{
	  if ( (cli->stream < 0) || stream_valid(cli, event->stream) )
	    {
	      cli->event_flags |= event->arg & 0xFFFF;
	      ret = prepare_events(context, index, cli);
	    }
	}
    } else
    {
      ret = event->arg >> 32;
      events = event->arg & 0xFFFFFFFF;
      if ( ret == 0 )
	ret = cbpoll_set_events(context, index, events);
    }
  return ret;
}

static bool client_destructor(void *data,
			      struct cbpoll_ctx *context,
			      int index,
			      int fd)
{
  struct ss_cctx *cli = data;

  dspd_mutex_lock(&cli->lock);
  cli->fd = -1;
  dspd_mutex_unlock(&cli->lock);
  if ( cli->stream >= 0 )
    close_client_stream(cli, cli->stream);
  
  if ( cli->playback_stream >= 0 )
    close_client_stream(cli, cli->playback_stream);

  if ( cli->capture_stream >= 0 )
    close_client_stream(cli, cli->capture_stream);
  
  dspd_mutex_destroy(&cli->lock);
  if ( cli->device >= 0 )
    dspd_daemon_unref(cli->device);
  if ( cli->playback_device >= 0 )
    dspd_daemon_unref(cli->playback_device);
  if ( cli->capture_device >= 0 )
    dspd_daemon_unref(cli->capture_device);
  if ( cli->req_ctx )
    dspd_req_ctx_delete(cli->req_ctx);
  if ( cli->pkt_fd >= 0 )
    close(cli->pkt_fd);
  if ( cli->fd_out >= 0 )
    close(cli->fd_out);
  free(cli);
  return true;
}

static const struct cbpoll_fd_ops socksrv_client_ops = {
  .fd_event = client_fd_event,
  .pipe_event = client_pipe_event,
  .destructor = client_destructor,
};



static struct ss_cctx *new_socksrv_client(int fd, struct cbpoll_ctx *cbctx)
{
  struct ss_cctx *ctx;
  ctx = calloc(1, sizeof(*ctx));
  if ( ! ctx )
    return NULL;

  if ( dspd_mutex_init(&ctx->lock, NULL) )
    {
      free(ctx);
      return NULL;
    }

  ctx->stream = -1;
  ctx->device = -1;
  ctx->playback_device = -1;
  ctx->capture_device = -1;
  ctx->playback_stream = -1;
  ctx->capture_stream = -1;
  ctx->cbctx = cbctx;
  ctx->fd = fd;
  ctx->index = -1;
  ctx->pkt_fd = -1;
  ctx->fd_out = -1;
  ctx->req_ctx = dspd_req_ctx_new(SS_MAX_PAYLOAD+sizeof(struct dspd_req),
				  sizeof(struct dspd_req),
				  dspd_req_read_cb,
				  dspd_req_write_cb,
				  dspd_req_getfd_cb,
				  (void*)(intptr_t)fd);
  ctx->pkt_in = (struct dspd_req*)ctx->req_ctx->rxpkt;
  ctx->pkt_out = (struct dspd_req*)ctx->req_ctx->txpkt;

  ctx->rctx.ops = &client_rcb;
  ctx->rctx.ops_arg = ctx;
  ctx->rctx.outbuf = ctx->pkt_out->pdata;
  ctx->rctx.fd = -1;
  ctx->rctx.index = -1;
  ctx->rctx.flags = DSPD_REQ_FLAG_REMOTE;


  if ( ! ctx->req_ctx )
    {
      dspd_mutex_destroy(&ctx->lock);
      free(ctx);
      ctx = NULL;
    }
  return ctx;
}

static void accept_fd(struct cbpoll_ctx *ctx,
		      void *data,
		      int64_t arg,
		      int32_t index,
		      int32_t fd)
{
  struct sockaddr_un addr;
  socklen_t len = sizeof(addr);
  int newfd = accept4(fd, (struct sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
  struct cbpoll_pipe_event evt;
  struct ss_cctx *cli;
  if ( newfd >= 0 )
    {
      cli = new_socksrv_client(newfd, ctx);
      if ( ! cli )
	close(newfd);
    } else
    {
      cli = NULL;
    }
  memset(&evt, 0, sizeof(evt));
  evt.fd = fd;
  evt.index = index;
  evt.stream = -1;
  evt.msg = 0;
  evt.arg = (intptr_t)cli;
  cbpoll_send_event(ctx, &evt); //Should not fail
}

static int listen_fd_event(void *data, 
			   struct cbpoll_ctx *context,
			   int index,
			   int fd,
			   int revents)
{
  struct cbpoll_work wrk;
  if ( revents & EPOLLIN )
    {
      wrk.fd = fd;
      wrk.index = index;
      wrk.msg = 0;
      wrk.arg = 0;
      wrk.callback = accept_fd;
      memset(wrk.extra_data, 0, sizeof(wrk.extra_data));
      cbpoll_queue_work(context, &wrk);
    }
  return 0;
}
static int listen_pipe_event(void *data, 
			     struct cbpoll_ctx *context,
			     int index,
			     int fd,
			     const struct cbpoll_pipe_event *event)
{
  struct ss_cctx *cli = (struct ss_cctx*)(intptr_t)event->arg;
  int32_t i;
  if ( cli )
    {
      i = cbpoll_add_fd(context, cli->fd, EPOLLIN, &socksrv_client_ops, cli);
      if ( i < 0 )
	{
	  close(cli->fd);
	  dspd_req_ctx_delete(cli->req_ctx);
	  free(cli);
	} else
	{
	  cli->index = i;
	}
    }
  cbpoll_set_events(context, index, EPOLLIN | EPOLLONESHOT);
  return 0;
}

static bool listen_destructor(void *data,
			      struct cbpoll_ctx *context,
			      int index,
			      int fd)
{
  dspd_log(0, "Destroying socket server listener");
  return true;
}

static const struct cbpoll_fd_ops socksrv_listen_ops = {
  .fd_event = listen_fd_event,
  .pipe_event = listen_pipe_event,
  .destructor = listen_destructor,
};


static int socksrv_init(void *daemon, void **context)
{
  struct ss_sctx *sctx;
  int ret;
  int fd = -1;
  sctx = calloc(1, sizeof(*sctx));
  if ( ! sctx )
    return -errno;
  sctx->fd = -1;
  ret = cbpoll_init(&sctx->cbctx, 0, DSPD_MAX_OBJECTS);
  if ( ret < 0 )
    {
      free(sctx);
      return ret;
    }
  ret = mkdir("/var/run/dspd", 0755);
  if ( ret < 0 && errno != EEXIST )
    goto out;
  unlink("/var/run/dspd/dspd.sock");
  sctx->fd = dspd_unix_sock_create("/var/run/dspd/dspd.sock", SOCK_CLOEXEC | SOCK_NONBLOCK);
  if ( sctx->fd < 0 )
    goto out;
  fd = sctx->fd;
  dspd_daemon_set_ipc_perm("/var/run/dspd/dspd.sock");
  ret = listen(sctx->fd, DSPD_MAX_OBJECTS);
  if ( ret < 0 )
    goto out;
  
 
  ret = cbpoll_add_fd(&sctx->cbctx, sctx->fd, EPOLLIN | EPOLLONESHOT, &socksrv_listen_ops, sctx);
  if ( ret < 0 )
    goto out;
  fd = -1;
  ret = cbpoll_set_name(&sctx->cbctx, "dspd-socksrv");
  if ( ret < 0 )
    goto out;
  ret = cbpoll_start(&sctx->cbctx);
  if ( ret < 0 )
    goto out;
  ret = 0;

 out:
  
  if ( ret < 0 )
    {
      dspd_log(0, "Failed to initialize socket server");
      cbpoll_destroy(&sctx->cbctx);
      close(fd);
      free(sctx);
    }
  return ret;
}

static void socksrv_close(void *daemon, void **context)
{
  
}

static int socksrv_ioctl(void         *daemon, 
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

struct dspd_mod_cb dspd_mod_socketserver = {
  .desc = "Socket server",
  .init = socksrv_init,
  .close = socksrv_close,
  .ioctl = socksrv_ioctl,
};
