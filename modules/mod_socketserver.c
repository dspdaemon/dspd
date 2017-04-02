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

  struct dspd_req *pkt_in;
  int32_t          pkt_cmd;
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
  //bool     event_sent;
};

struct ss_sctx {
  int fd;
  struct ss_cctx    *accepted_context;
  struct cbpoll_ctx  cbctx;
};


/*int32_t sendreq(struct ss_cctx *cli)
{
  if ( cli->pkt_out->cmd == DSPD_DCTL_ASYNC_EVENT )
    fprintf(stderr, "SEND ASYNC EVENT\n");
  return dspd_req_send(cli->req_ctx);
  }*/
#define sendreq(cli) dspd_req_send(cli->req_ctx)


static int32_t client_reply_buf(struct dspd_rctx *arg, 
				int32_t flags, 
				const void *buf, 
				size_t len)
{
  struct ss_cctx *cli = arg->ops_arg;
  struct dspd_req *req = (struct dspd_req*)cli->pkt_out;
  int32_t ret;
  if ( buf != req->pdata && len > 0 )
    memcpy(req->pdata, buf, len);
  req->len = len + sizeof(struct dspd_req);
  req->cmd = cli->pkt_cmd & 0xFFFF;
 
  req->flags = flags & 0xFFFF;
  req->flags |= cli->event_flags;
 


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
  if ( buf != req->pdata && len > 0 )
    memcpy(req->pdata, buf, len);
  memcpy(req->pdata, &fd, sizeof(fd));
  req->len = len + sizeof(struct dspd_req);
  //req->len |= DSPD_REQ_FLAG_CMSG_FD;
  req->cmd = cli->pkt_cmd & 0xFFFF;


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
  req->rdata.err = err;
  req->flags |= cli->event_flags;
  req->cmd = cli->pkt_cmd & 0xFFFF;
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

static int socksrv_dispatch_req(struct dspd_rctx *rctx,
				uint32_t             req,
				const void          *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct ss_cctx *cli = dspd_req_userdata(rctx);
  int ret, err;
  void *clptr;
  size_t br;
  uint32_t dev;
  uint64_t val;
  struct dspd_client_cb cb;
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
	  ret = dspd_client_new(dspd_dctx.objects, &clptr);
	  if ( ret == 0 )
	    {
	      cli->stream = dspd_client_get_index(clptr);
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

	      cli->stream = dspd_client_get_index(clptr);
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
	  dspd_stream_ctl(&dspd_dctx, 
			  cli->stream,
			  DSPD_SCTL_CLIENT_DISCONNECT,
			  NULL,
			  0,
			  NULL,
			  0,
			  &br);
	  dspd_daemon_unref(cli->stream);
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
    default:
      ret = dspd_req_reply_err(rctx, 0, EINVAL);
      break;
    }
  return ret;
}

static int client_dispatch_pkt(struct ss_cctx *cli)
{
  struct dspd_req *req = cli->pkt_in;
  int ret = 0;
  size_t len;
  void *iptr, *optr;

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
  len = cli->pkt_size - sizeof(*cli->pkt_in);
  if ( len == 0 )
    iptr = NULL;
  else
    iptr = cli->pkt_in->pdata;
  if ( cli->rctx.outbufsize == 0 )
    optr = NULL;
  else
    optr = cli->rctx.outbuf;
  if ( req->stream == -1 )
    {
      //Socket server request
      cli->rctx.user_data = cli;
      ret = socksrv_dispatch_req(&cli->rctx,
				 cli->pkt_cmd,
				 iptr,
				 len,
				 optr,
				 cli->rctx.outbufsize);
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
    } else if ( req->stream == cli->stream )
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
  if ( req->stream == cli->stream && 
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
  req->reserved = 0;
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
	      cli->pkt_flags = DSPD_REQ_FLAG_REMOTE;
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
	  if ( (cli->device == dev) && ((cli->stream == event->stream) || (cli->stream < 0)) )
	    {
	      cli->event_flags |= event->arg & 0xFFFF;
	      ret = prepare_events(context, index, cli);
	    }
	} else
	{
	  if ( (cli->stream < 0) || (cli->stream == event->stream) )
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
  size_t br;
  dspd_mutex_lock(&cli->lock);
  cli->fd = -1;
  dspd_mutex_unlock(&cli->lock);
  if ( cli->stream >= 0 )
    {
      //Server retains the client, so disconnect to make sure
      //it is actually released.
      dspd_stream_ctl(&dspd_dctx, 
		      cli->stream,
		      DSPD_SCTL_CLIENT_DISCONNECT,
		      NULL,
		      0,
		      NULL,
		      0,
		      &br);
      dspd_daemon_unref(cli->stream);
    }
  dspd_mutex_destroy(&cli->lock);
  if ( cli->device >= 0 )
    dspd_daemon_unref(cli->device);
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
