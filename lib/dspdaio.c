/*
 *   DSPDAIO - Asynchronous io
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
#include <stdlib.h>
#include <string.h>
#include "sslib.h"
#include "daemon.h"
#include "dspdaio.h"

static void sync_ctl_cb(void *context, struct dspd_async_op *op)
{
  bool *complete = op->data;
  (void)context;
  *complete = true;
}

int dspd_aio_sync_ctl(struct dspd_aio_ctx *ctx,
		      uint32_t stream,
		      uint32_t req,
		      const void          *inbuf,
		      size_t        inbufsize,
		      void         *outbuf,
		      size_t        outbufsize,
		      size_t       *bytes_returned)
{
  struct dspd_async_op op;
  volatile bool complete = false;
  int32_t ret;
  memset(&op, 0, sizeof(op));
  if ( bytes_returned )
    *bytes_returned = 0;
  op.stream = stream;
  op.req = req;
  op.inbuf = inbuf;
  op.inbufsize = inbufsize;
  op.outbuf = outbuf;
  op.outbufsize = outbufsize;
  op.xfer = 0;
  op.tag = UINT32_MAX;
  op.complete = sync_ctl_cb;
  op.data = (void*)&complete;
  ret = dspd_aio_submit(ctx, &op);
  if ( ret == 0 )
    {
      while ( complete == false )
	{
	  ret = dspd_aio_process(ctx, 0, -1);
	  if ( ret == -EINPROGRESS )
	    continue;
	  else if ( ret < 0 )
	    break;
	}
      if ( ret < 0 && ret != -EINPROGRESS )
	{
	  if ( dspd_aio_cancel(ctx, &op) == 0 )
	    dspd_aio_process(ctx, 0, 0);
	} else
	{
	  ret = op.error;
	  if ( bytes_returned )
	    *bytes_returned = op.xfer;
	}
    }
  return ret;
}

static void insert_op(struct dspd_aio_ctx *ctx, size_t index, struct dspd_async_op *op)
{
  op->error = EINPROGRESS;
  op->xfer = 0;
  op->reserved = 0;
  ctx->pending_ops[index] = op;
  ctx->output_pending = true;
  assert(ctx->pending_ops_count >= 0);
  ctx->pending_ops_count++;
  assert(ctx->pending_ops_count <= (ssize_t)ctx->max_ops);
}

int32_t dspd_aio_submit(struct dspd_aio_ctx *ctx, struct dspd_async_op *op)
{
  size_t n;
  size_t i;
  size_t p;
  int32_t ret = -EAGAIN;
  void *ptr;
  if ( ctx->error )
    return ctx->error;
  for ( i = 1; i <= ctx->max_ops; i++ )
    {
      n = ctx->position - i;
      p = n % ctx->max_ops;
      if ( ctx->pending_ops[p] == NULL )
	{
	  if ( ctx->pending_ops_count == 0 && ctx->max_ops > ctx->user_max_ops )
	    {
	      ptr = dspd_reallocz(ctx->pending_ops, 
				  sizeof(ctx->pending_ops[0]) * ctx->user_max_ops,
				  sizeof(ctx->pending_ops[0]) * ctx->max_ops,
				  false);
	      if ( ptr )
		{
		  ctx->max_ops = ctx->user_max_ops;
		  ctx->pending_ops = ptr;
		}
	    }
	  insert_op(ctx, p, op);
	  ret = 0;
	  break;
	}
    }
  if ( ret == -EAGAIN )
    {
      if ( ctx->max_ops < UINT16_MAX )
	{
	  //Increase the buffer size.  This only happens if there weren't enough slots available
	  //and there is not too many already allocated.  I think 65535 should be way more than
	  //enough.
	  n = ctx->max_ops * 2;
	  if ( n > UINT16_MAX )
	    n = UINT16_MAX;
	  ptr = dspd_reallocz(ctx->pending_ops,
			      sizeof(ctx->pending_ops[0]) * n,
			      sizeof(ctx->pending_ops[0]) * ctx->max_ops,
			      false);
	  if ( ptr )
	    {
	      p = ctx->max_ops;
	      ctx->pending_ops = ptr;
	      ctx->max_ops = n;
	      insert_op(ctx, p, op);
	      ret = 0;
	    }
	}
    }
  return ret;
}

int32_t dspd_aio_cancel(struct dspd_aio_ctx *ctx, struct dspd_async_op *op)
{
  size_t i;
  int32_t ret = -ENOENT;
  for ( i = 0; i < ctx->max_ops; i++ )
    {
      if ( ctx->pending_ops[i] == op )
	{
	  if ( op->error == EINPROGRESS ) //Did not start sending
	    {
	      //If this op is next in line then remove it
	      if ( (size_t)ctx->current_op == i )
		ctx->current_op = -1;
	      ctx->cancel_pending = true; //Must cancel later
	      op->error = ECANCELED; //Pending cancellation
	      ret = 0;
	    } else
	    {
	      ret = -EBUSY;
	    }
	  break;
	}
    }
  return ret;
}

static uint64_t create_tag(struct dspd_aio_ctx *ctx, uint32_t usertag, uint16_t index)
{
  uint64_t ret = 0;
  ctx->seq_out++;
  ret = (uint64_t)ctx->seq_out << 48U;
  ret |= (uint64_t)index << 32U;
  ret |= usertag;
  return ret;
}
static void decode_tag(uint64_t tag, uint32_t *usertag, uint16_t *seq, uint16_t *index)
{
  *index = (tag >> 32U) & 0xFFFFU;
  *seq = (tag >> 48U) & 0xFFFFU;
  *usertag = tag & 0xFFFFFFFFU;
}

static bool find_next_op(struct dspd_aio_ctx *ctx)
{
  size_t i, n, p;
  struct dspd_async_op *op;
  //Check for existing op
  if ( ctx->current_op >= 0 )
    {
      op = ctx->pending_ops[ctx->current_op];
      if ( op != NULL && ctx->off_out < ctx->len_out )
	return true; //Current op is still pending.
    }
  //Find new op
  ctx->current_op = -1L;
  ctx->len_out = 0;
  ctx->off_out = 0;
  n = ctx->position + ctx->max_ops;
  for ( i = ctx->position; i < n; i++ )
    {
      p = i % ctx->max_ops;
      op = ctx->pending_ops[p];
      if ( op != NULL )
	{
	  if ( op->error == EINPROGRESS )
	    {
	      //Set up next op
	      ctx->current_op = p;
	      ctx->position = i;
	      ctx->req_out.len = sizeof(ctx->req_out);
	      ctx->req_out.flags = 0;
	      ctx->req_out.cmd = op->req;
	      ctx->req_out.stream = op->stream;
	      ctx->req_out.bytes_returned = 0;
	      ctx->req_out.rdata.rlen = op->outbufsize;
	      ctx->req_out.tag = create_tag(ctx, op->tag, p);
	      op->reserved = ctx->seq_out;
	      op->reserved |= (uint64_t)(p & 0xFFFFU) << 16U;
	      ctx->cnt_out = 1;
	      ctx->iov_out[0].iov_base = &ctx->req_out;
	      ctx->iov_out[0].iov_len = sizeof(ctx->req_out);
	      ctx->len_out = ctx->iov_out[0].iov_len;
	      if ( op->inbufsize )
		{
		  if ( ctx->local )
		    {
		      ctx->req_out.flags |= DSPD_REQ_FLAG_POINTER;
		      ctx->ptrs_out.inbuf = op->inbuf;
		      ctx->ptrs_out.inbufsize = op->inbufsize;
		      ctx->ptrs_out.outbuf = op->outbuf;
		      ctx->ptrs_out.outbufsize = op->outbufsize;
		      ctx->iov_out[1].iov_base = &ctx->ptrs_out;
		      ctx->iov_out[1].iov_len = sizeof(ctx->ptrs_out);
		    } else
		    {
		      ctx->iov_out[1].iov_base = (void*)op->inbuf;
		      ctx->iov_out[1].iov_len = op->inbufsize;
		    }
		  ctx->req_out.len += ctx->iov_out[1].iov_len;
		  ctx->len_out += ctx->iov_out[1].iov_len;
		  ctx->cnt_out++;
		}
	      break;
	    }
	}
    }
  return ctx->current_op >= 0;
}

static void io_complete(struct dspd_aio_ctx *ctx, struct dspd_async_op *op)
{
  size_t index = (op->reserved >> 16U) & 0xFFFFU;
  ctx->pending_ops[index] = NULL;
  if ( (size_t)ctx->current_op == index )
    ctx->current_op = -1;
  if ( (size_t)ctx->op_in == index )
    ctx->op_in = -1;
  ctx->pending_ops_count--;
  assert(ctx->pending_ops_count >= 0);
  if ( op->complete )
    op->complete(ctx, op);
}

//Return value: -EAGAIN=no space, 0=no work.
int32_t dspd_aio_send(struct dspd_aio_ctx *ctx)
{
  ssize_t ret = 0;
  struct dspd_async_op *op;
  while ( find_next_op(ctx) )
    {
      assert(ctx->current_op >= 0);
      op = ctx->pending_ops[ctx->current_op];
      assert(op != NULL);
      ret = ctx->ops->writev(ctx->ops_arg, ctx->iov_out, ctx->cnt_out);
      if ( ret == -ENODATA )
	{
	  op->error = ENODATA; //Submitted.  Request is still pending, but no more outgoing work to do.
	  ctx->current_op = -1;
	  ret = 0; //Might be done
	} else if ( ret > 0 )
	{
	  ctx->off_out += ret;
	  if ( ctx->off_out == ctx->len_out )
	    {
	      op->error = ENODATA; //Submitted (caught it early)
	      ret = 0;
	    } else
	    {
	      op->error = EBUSY; //Too late to cancel
	      ret = -EAGAIN; //Partial data
	    }
	} else if ( ret == 0 )
	{
	  //EOF
	  op->error = -EIO;
	  io_complete(ctx, op);
	} else if ( ret != -EAGAIN && ret != -EWOULDBLOCK && ret != -EINTR )
	{
	  op->error = ret;
	  io_complete(ctx, op);
	} else
	{
	  ret = -EAGAIN;
	}
    }
  ctx->output_pending = false;
  return ret;
}



static int32_t recv_header(struct dspd_aio_ctx *ctx)
{
  ssize_t ret = 0;
  if ( ctx->off_in < sizeof(ctx->req_in) )
    {
      ret = ctx->ops->read(ctx->ops_arg, 
			   ((char*)&ctx->req_in) + ctx->off_in,
			   sizeof(ctx->req_in) - ctx->off_in);
      if ( ret > 0 )
	{
	  ctx->off_in += ret;
	  if ( ctx->off_in == sizeof(ctx->req_in) )
	    ret = 0;
	  else
	    ret = -EINPROGRESS;
	} else if ( ret == 0 )
	{
	  ret = -EIO;
	} else if ( ret == -EAGAIN || ret == -EWOULDBLOCK || ret == -EINTR )
	{
	  ret = -EINPROGRESS;
	}
    }
  return ret;
}

static int32_t recv_async_event(struct dspd_aio_ctx *ctx)
{
  size_t len = ctx->req_in.len - sizeof(ctx->req_in), o;
  int32_t ret;
  if ( (ctx->req_in.flags & DSPD_REQ_FLAG_CMSG_FD) == 0 && len <= sizeof(ctx->buf_in) )
    {
      if ( ctx->off_in == ctx->req_in.len )
	{
	  ret = 0;
	} else
	{
	  o = ctx->off_in - sizeof(ctx->buf_in);
	  ret = ctx->ops->read(ctx->ops_arg, &ctx->buf_in[o], ctx->req_in.len - ctx->off_in);
	  assert(ret != 0);
	  if ( ret > 0 )
	    {
	      ctx->off_in += ret;
	      if ( ctx->off_in == ctx->req_in.len )
		ret = 0;
	      else
		ret = -EINPROGRESS;
	    } else if ( ret == -EAGAIN || ret == -EWOULDBLOCK || ret == -EINTR )
	    {
	      ret = -EINPROGRESS;
	    }
	}
    } else
    {
      ret = -EPROTO;
    }
  return ret;
}

static ssize_t find_pending_op(struct dspd_aio_ctx *ctx)
{
  uint16_t index, seq;
  uint32_t usertag;
  struct dspd_async_op *op = NULL;
  ssize_t ret = -1;
  decode_tag(ctx->req_in.tag, &usertag, &seq, &index);
  if ( index < ctx->max_ops )
    {
      op = ctx->pending_ops[index];
      if ( op )
	{
	  if ( (op->reserved & 0xFFFFU) == seq && op->tag == usertag )
	    {
	      assert(((op->reserved >> 16U) & 0xFFFFU) == index);
	      ret = index;
	    }
	}
    }
  return ret;
}

static int32_t recv_reply(struct dspd_aio_ctx *ctx)
{
  char   *ptr;
  size_t  maxlen, len, o;
  ssize_t ret;
  struct dspd_async_op *op;
  struct iovec iov;
  if ( ctx->op_in < 0 )
    ctx->op_in = find_pending_op(ctx);
  if ( ctx->op_in >= 0 )
    {
      op = ctx->pending_ops[ctx->op_in];
      assert(op != NULL);
      ptr = op->outbuf;
      maxlen = op->outbufsize;
    } else
    {
      ptr = ctx->buf_in;
      maxlen = sizeof(ctx->buf_in);
      op = NULL;
    }
  len = ctx->req_in.len - sizeof(ctx->req_in);
  if ( len > maxlen )
    return -EPROTO;

  if ( ctx->off_in == sizeof(ctx->req_in) && (ctx->req_in.flags & DSPD_REQ_FLAG_CMSG_FD) )
    {
      iov.iov_len = len;
      iov.iov_base = ptr;
      ret = ctx->ops->recvfd(ctx->ops_arg, &iov);
      if ( ret < 0 )
	{
	  if ( ret == -EAGAIN || ret == -EWOULDBLOCK || ret == -EINTR )
	    ret = -EINPROGRESS;
	  return ret;
	} else if ( ret >= 0 )
	{
	  ctx->off_in += len - iov.iov_len;
	  if ( ctx->last_fd >= 0 )
	    close(ctx->last_fd);
	  ctx->last_fd = ret;
	}
    }
  if ( ctx->off_in < ctx->req_in.len )
    {
      o = ctx->off_in - sizeof(ctx->req_in);
      ret = ctx->ops->read(ctx->ops_arg, &ptr[o], ctx->req_in.len - ctx->off_in);
      if ( ret > 0 )
	{
	  ctx->off_in += ret;
	  if ( ctx->off_in == ctx->req_in.len )
	    ret = 0;
	  else
	    ret = -EINPROGRESS;
	} else if ( ret == 0 )
	{
	  ret = -EIO;
	} else if ( ret == -EAGAIN || ret == -EWOULDBLOCK || ret == -EINTR )
	{
	  ret = -EINPROGRESS;
	}
    } else
    {
      assert(ctx->off_in == ctx->req_in.len);
      ret = 0;
    }
  return ret;
}

int32_t dspd_aio_recv(struct dspd_aio_ctx *ctx)
{
  ssize_t ret;
  void *buf;
  size_t len;
  struct dspd_async_op *op;
  ret = recv_header(ctx);
  if ( ret == 0 )
    {
      if ( ctx->req_in.cmd == DSPD_DCTL_ASYNC_EVENT )
	ret = recv_async_event(ctx);
      else
	ret = recv_reply(ctx);
      if ( ret == 0 )
	{
	  if ( ctx->op_in >= 0 )
	    op = ctx->pending_ops[ctx->op_in];
	  else
	    op = NULL;

	  if ( ctx->req_in.len > sizeof(ctx->req_in) )
	    {
	      if ( op != NULL && op->outbuf != NULL )
		buf = op->outbuf;
	      else
		buf = ctx->buf_in;
	      len = ctx->req_in.len - sizeof(ctx->req_in);
	    } else
	    {
	      len = 0;
	      buf = NULL;
	    }
	  
	  //It is possible to have event flags carried by a regular reply.
	  if ( ( ctx->req_in.cmd == DSPD_DCTL_ASYNC_EVENT ||
		 (ctx->req_in.flags & (0xFFFF ^ DSPD_REQ_FLAG_ERROR)) != 0 ) &&
	       ctx->async_event != NULL )
	    {
	      ctx->async_event(ctx,
			       ctx->async_event_arg,
			       ctx->req_in.cmd,
			       ctx->req_in.stream,
			       ctx->req_in.flags,
			       buf,
			       len);
	    }
	  if ( op != NULL )
	    {
	      if ( ctx->req_in.flags & DSPD_REQ_FLAG_ERROR )
		{
		  op->error = ctx->req_in.rdata.err;
		  if ( op->error > 0 )
		    op->error *= -1;
		} else
		{
		  op->error = 0;
		}
	      if ( ctx->local == true && len == 0 && ctx->req_in.bytes_returned >= 0 )
		op->xfer = ctx->req_in.bytes_returned; //Server thread wrote directly to client buffer
	      else
		op->xfer = len;
	      io_complete(ctx, op);
	    }
	  ctx->off_in = 0;
	} else if ( ret != -EINPROGRESS )
	{
	  if ( ctx->op_in >= 0 )
	    {
	      op = ctx->pending_ops[ctx->op_in];
	      assert(op != NULL);
	      op->error = ret;
	      op->xfer = 0;
	      io_complete(ctx, op);
	    }
	}
    }
  return ret;
}

int32_t dspd_aio_block_directions(struct dspd_aio_ctx *ctx)
{
  int32_t evt = POLLIN;
  if ( ctx->current_op >= 0 || ctx->output_pending )
    evt |= POLLOUT;
  return evt;
}

int32_t dspd_aio_process(struct dspd_aio_ctx *ctx, int32_t revents, int32_t timeout)
{
  int32_t ret = 0;
  size_t i;
  struct dspd_async_op *op;
  if ( ctx->cancel_pending )
    {
      ctx->cancel_pending = false;
      ctx->output_pending = false;
      for ( i = 0; i < ctx->max_ops; i++ )
	{
	  op = ctx->pending_ops[i];
	  if ( op )
	    {
	      if ( op->error == ECANCELED )
		{
		  ctx->pending_ops[i] = NULL;
		  op->error = -ECANCELED;
		  op->xfer = 0;
		  io_complete(ctx, op);
		} else if ( op->error == EBUSY || op->error == EINPROGRESS )
		{
		  //Operation has finished sending or got submitted but did not start.
		  ctx->output_pending = true;
		}
	    }
	}
    }
  if ( revents == 0 && (ctx->pending_ops_count > 0 || ctx->error == 0) )
    {
      ret = ctx->ops->poll(ctx->ops_arg, dspd_aio_block_directions(ctx), &revents, timeout);
      if ( ret < 0 && ret != -EAGAIN && ret != -EINTR )
	return ret;
      if ( ret > 0 )
	ret = 0;
    }
  if ( revents & POLLIN )
    ret = dspd_aio_recv(ctx);
  if ( ret == 0 && (revents & POLLOUT) )
    ret = dspd_aio_send(ctx);
  if ( ret == -EAGAIN )
    ret = -EINPROGRESS;
  return ret;
}

int32_t dspd_aio_reap_fd(struct dspd_aio_ctx *ctx)
{
  int32_t ret;
  if ( ctx->last_fd < 0 )
    {
      ret = -EAGAIN;
    } else
    {
      ret = ctx->last_fd;
      ctx->last_fd = -1;
    }
  return ret;
}







int32_t dspd_aio_init(struct dspd_aio_ctx *ctx, ssize_t max_req)
{
  int32_t ret;
  if ( max_req == DSPD_AIO_SYNC )
    max_req = 4L;
  else if ( max_req == DSPD_AIO_DEFAULT )
    max_req = 8L;
  else if ( max_req > INT16_MAX )
    return -EINVAL;
  else if ( max_req < 2L )
    max_req = 2L;
  memset(ctx, 0, sizeof(*ctx));
  ctx->last_fd = -1;
  ctx->op_in = -1;
  ctx->current_op = -1;
  ctx->pending_ops = calloc(max_req, sizeof(*ctx->pending_ops));
  ctx->max_ops = max_req;
  ctx->user_max_ops = max_req;
  ctx->iofd = -1;
  ctx->magic = DSPD_OBJ_TYPE_AIO;
  if ( ! ctx->pending_ops )
    ret = -ENOMEM;
  else
    ret = 0;
  return ret;
}

void dspd_aio_destroy(struct dspd_aio_ctx *ctx)
{
  size_t i;
  struct dspd_async_op *op;
  int32_t ret = -ECANCELED;
  if ( ! ctx )
    return;
  ctx->error = -ESHUTDOWN;
  if ( ctx->ops != NULL )
    {
      for ( i = 0; i < ctx->max_ops; i++ )
	{
	  op = ctx->pending_ops[i];
	  if ( op != NULL )
	    dspd_aio_cancel(ctx, op);
	}
      while ( ctx->pending_ops_count > 0 )
	{
	  if ( (ret = dspd_aio_process(ctx, 0, -1)) < 0 )
	    break;
	}
      if ( ret == 0 )
	ret = -ECANCELED;
      if ( ctx->pending_ops_count > 0 )
	{
	  for ( i = 0; i < ctx->max_ops; i++ )
	    {
	      op = ctx->pending_ops[i];
	      if ( op != NULL )
		{
		  op->error = ret;
		  io_complete(ctx, op);
		}
	    }
	}
      if ( ctx->ops->close )
	ctx->ops->close(ctx->ops_arg);
    }
  free(ctx->pending_ops);
  memset(ctx, 0, sizeof(*ctx));
}

void dspd_aio_delete(struct dspd_aio_ctx *ctx)
{
  dspd_aio_destroy(ctx);
  free(ctx);
}

int32_t dspd_aio_new(struct dspd_aio_ctx **ctxp, ssize_t max_req)
{
  struct dspd_aio_ctx *c = calloc(1, sizeof(struct dspd_aio_ctx));
  int32_t ret = -ENOMEM;
  if ( c )
    {
      ret = dspd_aio_init(c, max_req);
      if ( ret == 0 )
	*ctxp = c;
      else
	free(c);
    }
  return ret;
}

int32_t dspd_aio_connect(struct dspd_aio_ctx *ctx, const char *addr, void *context, const struct dspd_aio_fifo_ops *ops, void *arg)
{
  intptr_t sock[2] = { -1, -1 };
  int32_t ret;
  uint32_t magic;
  struct dspd_daemon_ctx *dctx;
  if ( context == NULL )
    {
      if ( addr == NULL )
	addr = "/var/run/dspd/dspd.sock";
      sock[0] = dspd_unix_sock_connect(addr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if ( sock[0] >= 0 )
	{
	  ret = dspd_aio_sock_new(sock, ctx->max_ops, 0);
	  if ( ret >= 0 )
	    {
	      ctx->ops_arg = (void*)sock[0];
	      ctx->ops = &dspd_aio_sock_ops;
	      ctx->iofd = sock[0];
	    } else
	    {
	      close(sock[0]);
	    }
	} else
	{
	  ret = sock[0];
	}
    } else
    {
      magic = *(uint32_t*)context;
      if ( ops != NULL && magic == DSPD_OBJ_TYPE_DAEMON_CTX )
	{
	  dctx = context;
	  if ( dctx->new_aio_ctx )
	    {
	      //Send this connection to the socket server
	      ret = dctx->new_aio_ctx(&ctx, ops, arg, NULL, ctx->max_ops, false);
	    } else
	    {
	      ret = -ENOSYS;
	    }
	} else
	{
	  ret = -EINVAL;
	}
    }
  return ret;
}





void dspd_aio_set_event_cb(struct dspd_aio_ctx *ctx, 
			   void (*async_event)(struct dspd_aio_ctx    *context,
					       void                   *arg,
					       uint32_t                req,
					       int32_t                 stream,
					       int32_t                 flags,
					       const void             *buf,
					       size_t                  len),
			   void  *async_event_arg)
{
  ctx->async_event = async_event;
  ctx->async_event_arg = async_event_arg;
}



static void dspd_aio_fifo_destroy(struct dspd_aio_fifo_master *master)
{
  assert(master->client == NULL);
  assert(master->server == NULL);
  dspd_fifo_delete(master->rx);
  dspd_fifo_delete(master->tx);
  dspd_fifo_delete(master->txoob);
  dspd_fifo_delete(master->rxoob);
  pthread_mutex_destroy(&master->lock);
}
static int32_t aio_fifo_ready(struct dspd_aio_fifo_ctx *ctx)
{
  if ( ctx->master->server && ctx->master->client )
    return 0;
  return -ECONNABORTED;
}
static bool dspd_aio_fifo_full(struct dspd_aio_fifo_ctx *ctx, size_t len)
{
  int32_t ret;
  uint32_t l;
  ret = dspd_fifo_space(ctx->rx, &l);
  return ( ret == 0 && l <= len );
}

int32_t dspd_aio_fifo_new(struct dspd_aio_fifo_ctx *ctx[2], 
			  ssize_t max_req,
			  const struct dspd_aio_fifo_ops *client_ops, 
			  void *client_arg,
			  const struct dspd_aio_fifo_ops *server_ops,
			  void *server_arg)
{
  struct dspd_aio_fifo_master *master;
  int32_t ret;
  size_t len, n, pagesize = SS_MAX_PAYLOAD;
  //The idea is to have room for some extra requests so that
  //a request can be submitted from inside a completion callback and async
  //events can be processed.
  if ( max_req < 4L )
    max_req = 4L;

  master = calloc(1, sizeof(*master));
  
  if ( master )
    {
      ret = pthread_mutex_init(&master->lock, NULL);
      if ( ret )
	{
	  free(master);
	  return -ret;
	}

      len = max_req * sizeof(struct dspd_req) * 2;
      n = len / pagesize;
      if ( len % pagesize )
	n++;
      n++;
      len = n * pagesize;
      ret = dspd_fifo_new(&master->rx, len, 1, NULL);
      if ( ret == 0 )
	{
	  ret = dspd_fifo_new(&master->tx, len, 1, NULL);
	  if ( ret == 0 )
	    {
	      ret = dspd_fifo_new(&master->txoob, max_req, sizeof(struct dspd_aio_fifo_oob_msg), NULL);
	      if ( ret == 0 )
		{
		  ret = dspd_fifo_new(&master->rxoob, max_req, sizeof(struct dspd_aio_fifo_oob_msg), NULL);
		  if ( ret == 0 )
		    {
		      master->slot = -1;
		      master->client = &master->ctx[0];
		      master->server = &master->ctx[1];

		      master->client->txoob = master->txoob;
		      master->client->tx = master->tx;
		      master->client->rxoob = master->rxoob;
		      master->client->rx = master->rx;

		      master->server->txoob = master->client->rxoob;
		      master->server->tx = master->client->rx;
		      master->server->rxoob = master->client->txoob;
		      master->server->rx = master->client->tx;
		      
		      master->client->ops = client_ops;
		      master->client->arg = client_arg;

		      master->server->ops = server_ops;
		      master->server->arg = server_arg;

		      master->server->master = master;
		      master->client->master = master;

		      master->client->peer = (struct dspd_aio_fifo_ctx*)master->server;
		      master->server->peer = (struct dspd_aio_fifo_ctx*)master->client;
		      
		      master->client->nonblocking = true;
		      master->server->nonblocking = true;

		      ctx[0] = (struct dspd_aio_fifo_ctx*)master->client;
		      ctx[1] = (struct dspd_aio_fifo_ctx*)master->server;
		    }
		}
	    }
	}
      if ( ret < 0 )
	dspd_aio_fifo_destroy(master);
    } else
    {
      ret = -ENOMEM;
    }
  return ret;
}

ssize_t dspd_aio_fifo_read(void *arg, void *buf, size_t len)
{
  struct iovec iov;
  iov.iov_base = buf;
  iov.iov_len = len;
  return dspd_aio_fifo_readv(arg, &iov, 1);
}

ssize_t dspd_aio_fifo_write(void *arg, const void *buf, size_t len)
{
  struct iovec iov;
  iov.iov_base = (void*)buf;
  iov.iov_len = len;
  return dspd_aio_fifo_writev(arg, &iov, 1);
}

int32_t dspd_aio_fifo_recvfd(void *arg, struct iovec *iov)
{
  int32_t ret;
  struct dspd_aio_fifo_oob_msg msg;
  struct dspd_aio_fifo_ctx *ctx = arg;
  ret = dspd_aio_fifo_read(ctx, iov->iov_base, iov->iov_len);
  if ( ret > 0 )
    {
      iov->iov_base += ret;
      iov->iov_len -= ret;
      ret = dspd_fifo_read(ctx->rxoob, &msg, 1);
      if ( ret == 1 )
	{
	  ret = msg.fd;
	} else
	{
	  ret = -EPROTO;
	}
    } 
  return ret;
}

int32_t dspd_aio_fifo_sendfd(void *arg, int32_t fd, struct iovec *data)
{
  struct dspd_aio_fifo_oob_msg msg;
  uint32_t len;
  int32_t ret;
  struct dspd_aio_fifo_ctx *ctx = arg;
  ret = dspd_fifo_len(ctx->txoob, &len);
  if ( ret == 0 )
    {
      //Too many requests pending or is the other end not taking the file descriptors?
      if ( len == 0 )
	return -EPROTO;
    }
  if ( ctx->nonblocking )
    {
      ret = dspd_fifo_len(ctx->tx, &len);
      if ( ret < 0 )
	return ret;
      if ( len == 0 )
	return -EAGAIN;
    }

  memset(&msg, 0, sizeof(msg));
  //need to duplicate even within the same process to avoid race conditions in most operations.
  msg.fd = dup(fd);
  if ( msg.fd < 0 )
    return -errno;
  ret = dspd_fifo_write(ctx->txoob, &msg, 1);
  if ( ret == 1 )
    ret = dspd_aio_fifo_writev(ctx, data, 1);
  else
    ret = -EPROTO;
  return ret;
}

ssize_t dspd_aio_fifo_readv(void *arg, struct iovec *iov, size_t iovcnt)
{
  size_t i = 0;
  size_t bytes = 0;
  int32_t ret = 0;
  struct iovec *v = iov;
  bool wake = false;
  struct dspd_aio_fifo_ctx *ctx = arg;
  for ( i = 0; i < iovcnt; i++ )
    {
      v = &iov[i];
      //A NULL iov_base would probably cause EFAULT if iov_len is nonzero, but
      //a 0 value for iov_len seems to be undefined.
      if ( v->iov_base && v->iov_len )
	break;
    }
  iovcnt -= i;
  iov = v;
  //The idea is to be able to safely call this function until there is no more data
  if ( iovcnt == 0 )
    return -ENODATA;
  do { 
    for ( i = 0; i < iovcnt; i++ )
      {
	v = &iov[i];
	ret = dspd_fifo_read(ctx->rx, v->iov_base, v->iov_len);
	if ( ret > 0 )
	  {
	    if ( wake == false )
	      wake = dspd_aio_fifo_full(ctx, ret);
	    bytes += ret;
	    v->iov_base += ret;
	    v->iov_len -= ret;
	    ret = 0;
	    if ( v->iov_len )
	      {
		ret = dspd_fifo_read(ctx->rx, v->iov_base, v->iov_len);
		if ( ret > 0 )
		  {
		    if ( wake == false )
		      wake = dspd_aio_fifo_full(ctx, ret);
		    bytes += ret;
		    v->iov_base += ret;
		    v->iov_len -= ret;
		  }
	      }
	  } 
	if ( v->iov_len )
	  break;
      }
    if ( wake )
      {
	//Have some space.  Peer can write.
	ret = dspd_aio_fifo_signal(ctx->peer, POLLOUT);
	if ( ret < 0 )
	  break;
	wake = false;
      }
    if ( bytes > 0 || ctx->nonblocking )
      break;
    
    ret = dspd_aio_fifo_wait(ctx, POLLIN, -1);
    if ( ret < 0 )
      break;
  } while ( (ret = aio_fifo_ready(ctx)) == 0 );
  
  if ( bytes > 0 )
    ret = bytes;
  else if ( ret == 0 && bytes == 0 )
    ret = -EAGAIN;
  return ret;
}

//Write vectors and modify them so the same function can be repeatedly called with the
//same arguments until the io is complete.
ssize_t dspd_aio_fifo_writev(void *arg, struct iovec *iov, size_t iovcnt)
{
  size_t i = 0;
  size_t bytes = 0;
  int32_t ret = 0;
  struct iovec *v = iov;
  struct dspd_aio_fifo_ctx *ctx = arg;
  for ( i = 0; i < iovcnt; i++ )
    {
      v = &iov[i];
      //A NULL iov_base would probably cause EFAULT if iov_len is nonzero, but
      //a 0 value for iov_len seems to be undefined.
      if ( v->iov_base && v->iov_len )
	break;
    }
  iovcnt -= i;
  iov = v;
  //The idea is to be able to safely call this function until there is no more data
  if ( iovcnt == 0 )
    return -ENODATA;

  //Loop while the context is still connected and no data was written.
  while ( (ret = aio_fifo_ready(ctx)) == 0 )
    {
      //Write as much as possible before waking the other end
      for ( i = 0; i < iovcnt; i++ )
	{
	  v = &iov[i];
	  ret = dspd_fifo_write(ctx->tx, v->iov_base, v->iov_len);
	  if ( ret > 0 )
	    {
	      v->iov_base += ret;
	      v->iov_len -= ret;
	      bytes += ret;
	      if ( v->iov_len )
		{
		  ret = dspd_fifo_write(ctx->tx, v->iov_base, v->iov_len);
		  if ( ret > 0 )
		    {
		      bytes += ret;
		      v->iov_base += ret;
		      v->iov_len -= ret;
		      ret = 0;
		    }
		} else
		{
		  ret = 0;
		}
	    }
	  if ( v->iov_len > 0 )
	    break;
	}
      if ( bytes > 0 )
	{
	  //Sent data, so the other side probably needs to wake up.
	  ret = dspd_aio_fifo_signal(ctx->peer, POLLIN);
	  break; //Bytes were sent, so return to the caller instead of blocking more.
	} else if ( ctx->nonblocking )
	{
	  break;
	}
      //No space, so sleep until there is space.
      ret = dspd_aio_fifo_wait(ctx, POLLOUT, -1);
      if ( ret < 0 )
	break;
    }
  if ( bytes > 0 )
    ret = bytes;
  else if ( ret == 0 && bytes == 0 )
    ret = -EAGAIN;
  return ret;
}

void dspd_aio_fifo_close(void *arg)
{
  bool destroy;
  struct dspd_aio_fifo_oob_msg msg;
  struct dspd_aio_fifo_ctx *ctx = arg;
  pthread_mutex_lock(&ctx->master->lock);
  if ( ctx == ctx->master->client )
    {
      ctx->master->client = NULL;
    } else if ( ctx == ctx->master->server )
    {
      ctx->master->server = NULL;
    } else
    {
      assert(ctx != ctx->master->server && ctx != ctx->master->client);
    }
  while ( dspd_fifo_read(ctx->rxoob, &msg, 1) == 1 )
    close(msg.fd);
  ctx->ops->wake(ctx, ctx->arg);
  destroy = ctx->master->client == NULL && ctx->master->server == NULL;
  pthread_mutex_unlock(&ctx->master->lock);
  if ( destroy )
    dspd_aio_fifo_destroy(ctx->master);
}

int32_t dspd_aio_fifo_space(struct dspd_aio_fifo_ctx *ctx)
{
  int32_t ret;
  uint32_t l;
  ret = aio_fifo_ready(ctx);
  if ( ret == 0 )
    {
      ret = dspd_fifo_space(ctx->tx, &l);
      if ( ret == 0 )
	ret = l;
    }
  return ret;
}

int32_t dspd_aio_fifo_len(struct dspd_aio_fifo_ctx *ctx)
{
  int32_t ret;
  uint32_t l;
  ret = dspd_fifo_len(ctx->rx, &l);
  if ( ret == 0 )
    {
      if ( l > 0 )
	ret = l;
      else
	ret = aio_fifo_ready(ctx);
    }
  return ret;
}

int32_t dspd_aio_fifo_signal(struct dspd_aio_fifo_ctx *ctx, int32_t events)
{
  int32_t ret = 0;
  if ( events & (POLLERR|POLLHUP|POLLRDHUP|POLLNVAL) )
    {
      ret = ctx->ops->wake(ctx, ctx->arg);
    } else if ( dspd_test_and_set(&ctx->lock) != DSPD_TS_SET )
    {
      if ( events & AO_load(&ctx->poll_events) )
	ret = ctx->ops->wake(ctx, ctx->arg);
      dspd_ts_clear(&ctx->lock);
    } else
    {
      ret = ctx->ops->wake(ctx, ctx->arg);
    }
  return ret;
}



int32_t dspd_aio_fifo_test_events(struct dspd_aio_fifo_ctx *ctx, int32_t events)
{
  int32_t revents = 0;
  int32_t ret;
  if ( events & POLLIN )
    {
      ret = dspd_aio_fifo_len(ctx);
      if ( ret < 0 )
	revents |= POLLIN | POLLHUP;
      else if ( ret > 0 )
	revents |= POLLIN;
    }
  if ( events & POLLOUT )
    {
      ret = dspd_aio_fifo_space(ctx);
      if ( ret < 0 )
	revents |= POLLOUT | POLLHUP;
      else if ( ret > 0 )
	revents |= POLLOUT;
    }
  return revents;
}


int32_t dspd_aio_fifo_wait(struct dspd_aio_fifo_ctx *ctx, int32_t events, int32_t timeout)
{
  int32_t revents = 0, ret;
  if ( dspd_test_and_set(&ctx->lock) == DSPD_TS_SET )
    {
      AO_store(&ctx->poll_events, events);
      revents |= dspd_aio_fifo_test_events(ctx, events);
      if ( (revents & events) == 0 && timeout != 0 )
	{
	  ret = ctx->ops->wait(ctx, ctx->arg, timeout);
	  if ( ret < 0 )
	    revents |= POLLERR;
	}
      dspd_ts_clear(&ctx->lock);
    } else
    {
      AO_store(&ctx->poll_events, events);
      revents |= dspd_aio_fifo_test_events(ctx, events);
      if ( (revents & events) == 0 && timeout != 0 )
	{
	  ret = ctx->ops->wait(ctx, ctx->arg, timeout);
	  if ( ret < 0 )
	    revents |= POLLERR;
	}
    }
  revents |= dspd_aio_fifo_test_events(ctx, events);
  return revents;
}


int32_t dspd_aio_fifo_poll(void *arg, int32_t events, int32_t *revents, int32_t timeout)
{
  *revents = dspd_aio_fifo_wait(arg, events, timeout);
  return !!*revents;
}
int32_t dspd_aio_fifo_set_nonblocking(void *arg, bool nonblocking)
{
  struct dspd_aio_fifo_ctx *ctx = arg;
  ctx->nonblocking = nonblocking;
  return 0;
}


static int32_t dspd_aio_fifo_ptevent_wake(struct dspd_aio_fifo_ctx *ctx, void *arg)
{
  struct dspd_aio_fifo_ptevent *evt = arg;
  (void)ctx;

  if ( dspd_test_and_set(&evt->tsval) != DSPD_TS_SET )
    {
      pthread_mutex_lock(evt->lock);
      pthread_cond_broadcast(evt->cond);
      pthread_mutex_unlock(evt->lock);
    }
  return 0;
}

static int32_t dspd_aio_fifo_ptevent_wait(struct dspd_aio_fifo_ctx *ctx, void *arg, int32_t timeout)
{
  struct dspd_aio_fifo_ptevent *evt = arg;
  dspd_time_t t;
  struct timespec ts;
  (void)ctx;

  if ( dspd_ts_load(&evt->tsval) == DSPD_TS_SET )
    {
      dspd_ts_clear(&evt->tsval);
    } else if ( timeout != 0 )
    {
      if ( timeout > 0 )
	{
	  t = dspd_get_time() + timeout;
	  ts.tv_sec = t / 1000000000;
	  ts.tv_nsec = t % 1000000000;
	  pthread_mutex_lock(evt->lock);
	  while ( dspd_ts_load(&evt->tsval) != DSPD_TS_SET )
	    {
	      if ( pthread_cond_timedwait(evt->cond, evt->lock, &ts) != 0 )
		break;
	    }
	} else
	{
	  pthread_mutex_lock(evt->lock);
	  while ( dspd_ts_load(&evt->tsval) != DSPD_TS_SET )
	    pthread_cond_wait(evt->cond, evt->lock);
	}
      pthread_mutex_unlock(evt->lock);
    }
  return 0;
}

static int32_t dspd_aio_fifo_eventfd_wake(struct dspd_aio_fifo_ctx *ctx, void *arg)
{
  struct dspd_aio_fifo_eventfd *evt = arg;
  uint64_t val = 1;
  int ret = 0;
  (void)ctx;

  if ( dspd_test_and_set(&evt->tsval) != DSPD_TS_SET )
    {
      while ( write(evt->fd, &val, sizeof(val)) < 0 )
	{
	  ret = -errno;
	  if ( ret != -EINTR && ret != -EWOULDBLOCK && ret != -EAGAIN )
	    break;
	}

    }
  return ret;
}

static int32_t dspd_aio_fifo_eventfd_wait(struct dspd_aio_fifo_ctx *ctx, void *arg, int32_t timeout)
{
  struct dspd_aio_fifo_eventfd *evt = arg;
  struct pollfd pfd;
  int32_t ret = 0;
  uint64_t val;
  (void)ctx;
  if ( dspd_ts_load(&evt->tsval) == DSPD_TS_SET )
    {
      if ( read(evt->fd, &val, sizeof(val)) == sizeof(val) )
	dspd_ts_clear(&evt->tsval);
    } else if ( timeout != 0 )
    {
      pfd.fd = evt->fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      ret = poll(&pfd, 1, timeout);
      if ( ret < 0 )
	{
	  ret = -errno;
	  if ( ret == -EINTR || ret == -EAGAIN )
	    ret = 0;
	} else if ( ret > 0 && (pfd.revents & POLLIN) )
	{
	  read(evt->fd, &val, sizeof(val));
	  dspd_ts_clear(&evt->tsval);
	  ret = 0;
	}
    }
  return ret;
}



struct dspd_aio_fifo_ops dspd_aio_fifo_ptevent_ops = {
  .wake = dspd_aio_fifo_ptevent_wake,
  .wait = dspd_aio_fifo_ptevent_wait,
};
struct dspd_aio_fifo_ops dspd_aio_fifo_eventfd_ops = {
  .wake = dspd_aio_fifo_eventfd_wake,
  .wait = dspd_aio_fifo_eventfd_wait,
};


int32_t dspd_aio_sock_new(intptr_t sv[2], ssize_t max_req, int32_t flags)
{
  int s[2];
  int32_t ret;
  intptr_t fd;
  int len, n;
  int pagesize;
  if ( sv[0] < 0 && sv[1] < 0 )
    {
      ret = socketpair(AF_UNIX, SOCK_STREAM|flags, 0, s);
      if ( ret < 0 )
	return -errno;
      sv[0] = s[0];
      sv[1] = s[1];
    }
  fd = sv[0];
  if ( max_req < 4L )
    max_req = 4L;
  pagesize = SS_MAX_PAYLOAD;
  len = max_req * sizeof(struct dspd_req) * 2;
  n = len / pagesize;
  if ( len % pagesize )
    n++;
  n++;
  len = n * pagesize;
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *)&len, sizeof(len));
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&len, sizeof(len));
  return 0;
}

int32_t dspd_aio_sock_sendfd(void *arg, int32_t fd, struct iovec *data)
{
  struct msghdr msg;
  struct cmsghdr *cmsg;
  int n;
  char cms[CMSG_SPACE(sizeof(int))];

  memset(&msg, 0, sizeof msg);
  msg.msg_iov = data;
  msg.msg_iovlen = 1;
  msg.msg_control = (caddr_t)cms;
  msg.msg_controllen = CMSG_LEN(sizeof(int));

  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  memmove(CMSG_DATA(cmsg), &fd, sizeof(int));

  errno = 0;

  n = sendmsg((intptr_t)arg, &msg, 0);
  if ( n < 0 )
    {
      if ( errno == 0 )
	n = -EIO;
      else
	n = -errno;
    } else if ( n == 0 ) //EOF
    {
      n = -ECONNABORTED;
    }
  return n;
}

int32_t dspd_aio_sock_recvfd(void *arg, struct iovec *iov)
{
  int n;
  int fd = -1;
  struct msghdr msg;
  struct cmsghdr *cmsg;
  char cms[CMSG_SPACE(sizeof(int))];


  memset(&msg, 0, sizeof msg);
  msg.msg_name = 0;
  msg.msg_namelen = 0;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;

  msg.msg_control = cms;
  msg.msg_controllen = sizeof(cms);

  if((n=recvmsg((intptr_t)arg, &msg, 0)) < 0)
    {
      if ( errno == 0 )
	errno = -EIO;
      return -errno;
    }
  if(n == 0 )
    {
      errno = ECONNABORTED;
      return -errno;
    }
  iov->iov_base += n;
  iov->iov_len -= n;
  int i = 0, f;

  //The sender should have only sent one control message.
  //Prevent a denial of service by getting rid of unwanted file descriptors.
  for ( cmsg = CMSG_FIRSTHDR(&msg);
	cmsg != NULL;
	cmsg = CMSG_NXTHDR(&msg, cmsg) )
    {
      if ( cmsg->cmsg_type != SCM_RIGHTS )
	continue;
      char *dptr = (char*)CMSG_DATA(cmsg);
      //Make sure address is valid.  Might not be if the message did not really contain a FD.
      if ( dptr < (char*)4096UL || dptr > (char*)(UINTPTR_MAX-4096UL))
	{
	  fd = -EPROTO;
	  break;
	}
      if ( i == 0 )
	{
	  //Only want the first one
	  memmove(&fd, dptr, sizeof(int));
	} else
	{
	  //Get rid of unwanted file descriptors
	  memmove(&f, dptr, sizeof(int));
	  close(f);
	}

      i++;
    }
  return fd;
}

ssize_t dspd_aio_sock_writev(void *arg, struct iovec *iov, size_t iovcnt)
{
  ssize_t ret;
  size_t i = 0, n;
  struct iovec *v = iov;
  for ( i = 0; i < iovcnt; i++ )
    {
      v = &iov[i];
      if ( v->iov_base && v->iov_len )
	break;
    }
  iovcnt -= i;
  iov = v;
  if ( iovcnt > 0 )
    {
      ret = writev((intptr_t)arg, iov, iovcnt);
      if ( ret > 0 )
	{
	  n = ret;
	  for ( i = 0; i < iovcnt; i++ )
	    {
	      v = &iov[i];
	      if ( v->iov_len < n )
		{
		  n -= v->iov_len;
		  v->iov_len = 0;
		} else
		{
		  v->iov_len -= n;
		  v->iov_base += n;
		  break;
		}
	    }
	} else if ( ret < 0 )
	{
	  ret = -errno;
	} else
	{
	  ret = -ECONNABORTED;
	}
    } else
    {
      //XSI streams error should not be used by Linux on a byte based unix domain socket.
      ret = -ENODATA;
    }
  return ret;
}

ssize_t dspd_aio_sock_readv(void *arg, struct iovec *iov, size_t iovcnt)
{
  ssize_t ret;
  size_t i, n;
  struct iovec *v;
  for ( i = 0; i < iovcnt; i++ )
    {
      v = &iov[i];
      if ( v->iov_base && v->iov_len )
	break;
    }
  iovcnt -= i;
  iov = v;
  if ( iovcnt > 0 )
    {
      ret = readv((intptr_t)arg, iov, iovcnt);
      if ( ret > 0 )
	{
	  n = ret;
	  for ( i = 0; i < iovcnt; i++ )
	    {
	      v = &iov[i];
	      if ( v->iov_len < n )
		{
		  n -= v->iov_len;
		  v->iov_len = 0;
		} else
		{
		  v->iov_len -= n;
		  v->iov_base += n;
		  break;
		}
	    }
	} else if ( ret < 0 )
	{
	  ret = -errno;
	} else
	{
	  ret = -ECONNABORTED;
	}
    } else
    {
      ret = -ENODATA;
    }
  return ret;
}

ssize_t dspd_aio_sock_read(void *arg, void *buf, size_t len)
{
  struct iovec iov;
  iov.iov_base = buf;
  iov.iov_len = len;
  return dspd_aio_sock_readv(arg, &iov, 1);
}

void dspd_aio_sock_close(void *arg)
{
  close((intptr_t)arg);
}


int32_t dspd_aio_sock_poll(void *arg, int32_t events, int32_t *revents, int32_t timeout)
{
  struct pollfd pfd;
  int32_t ret;
  pfd.fd = (intptr_t)arg;
  pfd.events = events;
  pfd.revents = 0;
  ret = poll(&pfd, 1, timeout);
  *revents = pfd.revents;
  if ( ret < 0 )
    ret = -errno;
  return ret;
}

ssize_t dspd_aio_sock_write(void *arg, const void *buf, size_t len)
{
  return write((intptr_t)arg, buf, len);
}

int32_t dspd_aio_sock_set_nonblocking(void *arg, bool nonblocking)
{
  int fd = (intptr_t)arg;
  if ( nonblocking )
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
  return 0;
}

struct dspd_aio_ops dspd_aio_sock_ops = {
  .writev = dspd_aio_sock_writev,
  .write = dspd_aio_sock_write,
  .read = dspd_aio_sock_read,
  .recvfd = dspd_aio_sock_recvfd,
  .sendfd = dspd_aio_sock_sendfd,
  .poll = dspd_aio_sock_poll,
  .set_nonblocking = dspd_aio_sock_set_nonblocking,
  .close = dspd_aio_sock_close,
};

struct dspd_aio_ops dspd_aio_fifo_ctx_ops = {
  .writev = dspd_aio_fifo_writev,
  .write = dspd_aio_fifo_write,
  .read = dspd_aio_fifo_read,
  .recvfd = dspd_aio_fifo_recvfd,
  .sendfd = dspd_aio_fifo_sendfd,
  .poll = dspd_aio_fifo_poll,
  .set_nonblocking = dspd_aio_fifo_set_nonblocking,
  .close = dspd_aio_fifo_close,
};
