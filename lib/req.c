/*
 *   REQ - Remote and local request API
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
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include "sslib.h"
#include "daemon.h"



void dspd_req_ctx_delete(struct dspd_req_ctx *ctx)
{
  close(ctx->recvfd);
  free(ctx->rxpkt);
  free(ctx->txpkt);
  free(ctx);
}

struct dspd_req_ctx *dspd_req_ctx_new(size_t pktlen,
				      size_t hdrlen,
				      const struct dspd_aio_ops *ops,
				      void *arg)
{
  struct dspd_req_ctx *ctx = calloc(1, sizeof(struct dspd_req_ctx));
  if ( ctx )
    {
      ctx->recvfd = -1;
      ctx->rxpkt = calloc(1, pktlen);
      ctx->txpkt = calloc(1, pktlen);
      if ( ! (ctx->rxpkt && ctx->txpkt) )
	goto out;
      ctx->ops = ops;
      ctx->arg = arg;
      ctx->hdrlen = hdrlen;
      ctx->rxmax = pktlen;
      ctx->txmax = pktlen;
      ctx->uid = -1;
      ctx->gid = -1;
      ctx->pid = -1;
    }
  
  return ctx;

 out:
  free(ctx->rxpkt);
  free(ctx->txpkt);
  free(ctx);
  return NULL;
}
			    
			    

int dspd_cmsg_recvfd(int s, struct iovec *iov)
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

  if((n=recvmsg(s, &msg, 0)) < 0)
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
	return -EPROTO;
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

static int32_t recv_cmsg(struct dspd_req_ctx *ctx)
{
  char *buf = (char*)ctx->rxpkt;
  struct iovec iov;
  ssize_t ret = -EINPROGRESS;
  size_t l;
  struct ucred uc = { -1, -1, -1 };
  /*
    Get the FD and at least 1 byte.  Best case is that the whole payload comes in.
  */
  if ( ctx->rxstat.offset == sizeof(*ctx->rxpkt) )
    {
      iov.iov_base = &buf[ctx->rxstat.offset];
      l = ctx->rxstat.len - ctx->rxstat.offset;
      iov.iov_len = l;
      if ( ctx->rxstat.isfd )
	{
	  ret = ctx->ops->recvfd(ctx->arg, &iov);
	  if ( ret <= 0 )
	    return ret;
	  ctx->recvfd = ret;
	  ctx->rxstat.offset += l - iov.iov_len;
	} else
	{
	  //Must be cred
	  ret = ctx->ops->recv_cred(ctx->arg, &uc, iov.iov_base, iov.iov_len);
	  if ( ret <= 0 )
	    return ret;
	  //Have at least 1 byte.  Save credentials since otherwise the
	  //client could stall and overwrite the inline credentials with
	  //arbitrary data.
	  ctx->rxstat.offset += ret;
	  ctx->pid = uc.pid;
	  ctx->uid = uc.uid;
	  ctx->gid = uc.gid;
	}
    }

  /*
    Get whatever is left over.
   */
  if ( ctx->rxstat.offset < ctx->rxstat.len && ctx->rxstat.offset > sizeof(*ctx->rxpkt) )
    {
      ret = ctx->ops->read(ctx->arg,
			   &buf[ctx->rxstat.offset],
			   ctx->rxstat.len - ctx->rxstat.offset);
      if ( ret > 0 )
	{
	  ctx->rxstat.offset += ret;
	  ret = -EINPROGRESS;
	}
    }
  /*
    Copy file descriptor to first 4 bytes of the payload and remember it in
    case the packet is never reaped.
  */
  if ( ctx->rxstat.offset == ctx->rxstat.len )
    {
      ret = ctx->rxstat.len;
      if ( ctx->txstat.isfd )
	{
	  memcpy(&buf[ctx->hdrlen], &ctx->recvfd, sizeof(ctx->recvfd));
	} else if ( ctx->txstat.iscred )
	{
	  struct ucred *u = (struct ucred*)&buf[ctx->hdrlen];
	  u->pid = ctx->pid;
	  u->gid = ctx->gid;
	  u->uid = ctx->uid;
	}
	
      
    }
  return ret;
}

static int32_t recv_data(struct dspd_req_ctx *ctx)
{
  ssize_t ret;
  char *buf = (char*)ctx->rxpkt;
  if ( ctx->rxstat.offset == ctx->rxstat.len )
    return ctx->rxstat.len;
  ret = ctx->ops->read(ctx->arg,
		       &buf[ctx->rxstat.offset],
		       ctx->rxstat.len - ctx->rxstat.offset);
  if ( ret > 0 )
    {
      ctx->rxstat.offset += ret;
      if ( ctx->rxstat.offset == ctx->rxstat.len )
	ret = ctx->rxstat.len;
      else
	ret = -EINPROGRESS;
    }
  return ret;
}

int32_t dspd_req_recv(struct dspd_req_ctx *ctx)
{
  ssize_t ret;

  if ( ctx->rxstat.offset < sizeof(*ctx->rxpkt ) )
    {
      ret = ctx->ops->read(ctx->arg,
			   ((char*)ctx->rxpkt) + ctx->rxstat.offset,
			   sizeof(*ctx->rxpkt) - ctx->rxstat.offset);
      if ( ret <= 0 )
	  return ret;
    
      ctx->rxstat.offset += ret;
      if ( ctx->rxstat.offset == sizeof(*ctx->rxpkt) )
	{
	  ctx->rxstat.isfd = !! (ctx->rxpkt->flags & PKT_CMSG_FD);
	  ctx->rxstat.iscred = !! (ctx->rxpkt->flags & DSPD_REQ_FLAG_CMSG_CRED);
	  ctx->rxstat.len = ctx->rxpkt->len & (~PKT_CMSG_FD);
	  if ( ctx->rxstat.len > ctx->rxmax ||
	       ctx->rxstat.len < ctx->hdrlen ||
	       ((ctx->rxstat.isfd || ctx->rxstat.iscred) && ctx->rxstat.len == ctx->hdrlen) || //Too short
	       (ctx->rxstat.iscred && ctx->rxstat.isfd) ) //Can't be both types
	    {

	      return -EPROTO;
	    }
	  ctx->rxstat.started = 1;
	}
    }

  if ( ctx->rxstat.started )
    {
      if ( ctx->rxstat.isfd || ctx->rxstat.iscred )
	{
	  return recv_cmsg(ctx);
	} else
	{
	  return recv_data(ctx);
	}
    }
  return -EINPROGRESS;
}


int dspd_cmsg_sendfd(int s, int fd, struct iovec *data)
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

  n = sendmsg(s, &msg, 0);
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


static int32_t send_cmsg(struct dspd_req_ctx *ctx, int32_t fd)
{
  ssize_t ret;
  char *buf;
  struct iovec iov;
  buf = (char*)ctx->txpkt;
  /*
    Send the size and PKT_CMSG_FD flag so the other end knows what comes next.
  */
  if ( ctx->txstat.offset < sizeof(*ctx->txpkt) )
    {
      ret = ctx->ops->write(ctx->arg,
			    &buf[ctx->txstat.offset],
			    sizeof(*ctx->txpkt) - ctx->txstat.offset);
      if ( ret <= 0 )
	return ret;
      ctx->txstat.offset += ret;
    }

  /*
    Send the file descriptor and at least 1 byte of data.  Probably send all of it, but
    not always.
   */
  if ( ctx->txstat.offset == sizeof(*ctx->txpkt) )
    {
      iov.iov_base = &buf[ctx->txstat.offset];
      iov.iov_len = ctx->txstat.len - ctx->txstat.offset;
      
      if ( ctx->txstat.isfd )
	{
	  //First 4 bytes of payload is FD in most cases
	  if ( fd < 0 )
	    memcpy(&fd, &buf[ctx->hdrlen], sizeof(fd));
	  ret = ctx->ops->sendfd(ctx->arg, fd, &iov);
	  DSPD_ASSERT(ret <= (ssize_t)iov.iov_len);
	} else
	{
	  //Must be cred
	  ret = ctx->ops->send_cred(ctx->arg, iov.iov_base, iov.iov_base, iov.iov_len);
	}
      if ( ret <= 0 )
	return ret;
      ctx->txstat.offset += ret;
	
    }
  /*
    Send the rest of the data, if any.
   */
  if ( ctx->txstat.offset > sizeof(*ctx->txpkt) )
    {
      DSPD_ASSERT(ctx->txstat.offset <= ctx->txstat.len);
      if ( ctx->txstat.offset < ctx->txstat.len )
	{
	  ret = ctx->ops->write(ctx->arg,
				&buf[ctx->txstat.offset],
				ctx->txstat.len - ctx->txstat.offset);
	  if ( ret <= 0 )
	    return ret;
	  ctx->txstat.offset += ret;
	  DSPD_ASSERT((size_t)ret <= ctx->txstat.offset);
	  if ( ctx->txstat.offset == ctx->txstat.len )
	    ret = ctx->txstat.len;
	  else
	    ret = -EINPROGRESS;
	} else
	{
	  ret = ctx->txstat.offset;
	}
    } else
    {
      ret = -EINPROGRESS;
    }
  return ret;
}

int32_t dspd_req_send(struct dspd_req_ctx *ctx, int32_t fd)
{
  ssize_t ret = 0;
  char *buf;
  if ( ! ctx->txstat.started )
    {
      ctx->txstat.isfd = !! (ctx->txpkt->flags & PKT_CMSG_FD);
      ctx->txstat.iscred = !! (ctx->txpkt->flags & DSPD_REQ_FLAG_CMSG_CRED);
      ctx->txstat.offset = 0;
      ctx->txstat.len = ctx->txpkt->len;
      if ( ctx->txstat.len > ctx->txmax ||
	   ((ctx->txstat.isfd || ctx->txstat.iscred) && ctx->txstat.len == ctx->hdrlen) ||
	   ctx->txstat.len < ctx->hdrlen ||
	   (ctx->txstat.iscred && ctx->txstat.isfd) )
	return -EINVAL;
      if ( ctx->txstat.isfd )
	ctx->fd_out = fd;
      else
	ctx->fd_out = -1;
      ctx->txstat.started = 1;
    }
  if ( ctx->txstat.isfd )
    {
      ret = send_cmsg(ctx, ctx->fd_out);
      DSPD_ASSERT(ctx->txstat.offset <= ctx->txstat.len);
    } else if ( ctx->txstat.offset < ctx->txstat.len )
    {
      buf = (char*)ctx->txpkt;
      size_t bytes_to_write = ctx->txstat.len - ctx->txstat.offset;
      ret = ctx->ops->write(ctx->arg,
			    &buf[ctx->txstat.offset],
			    bytes_to_write);
      if ( ret <= 0 )
	return ret;
      DSPD_ASSERT(ret <= bytes_to_write);
      ctx->txstat.offset += ret;
      DSPD_ASSERT(ctx->txstat.offset <= ctx->txstat.len);
    }
  DSPD_ASSERT(ctx->txstat.offset <= ctx->txstat.len);
  if ( ctx->txstat.offset == ctx->txstat.len )
    {
      ret = ctx->txstat.len;
      ctx->txstat.len = 0;
      ctx->txstat.offset = 0;
      ctx->txstat.started = 0;
    } else if ( ret > 0 )
    {
      ret = -EINPROGRESS;
    }
  return ret;
}


int32_t dspd_req_reap(struct dspd_req_ctx *ctx, void **buf, size_t *len, int32_t *fd)
{
  ssize_t ret;
  if ( ctx->rxstat.started == 0 ||
       ctx->rxstat.offset < ctx->rxstat.len )
    {
      ret = -EINPROGRESS;
    } else if ( ctx->rxstat.len == 0 )
    {
      ret = -EAGAIN;
    } else
    {
      DSPD_ASSERT(ctx->rxpkt);
      *buf = ctx->rxpkt;
      *len = ctx->rxstat.len;
      if ( ctx->rxstat.isfd )
	{
	  if ( ! fd )
	    close(ctx->recvfd);
	  else
	    *fd = ctx->recvfd;
	  ctx->recvfd = -1;
	} else
	{
	  if ( fd )
	    *fd = -1;
	}
      ret = 0;
      ctx->rxstat.len = 0;
      ctx->rxstat.isfd = 0;
      ctx->rxstat.offset = 0;
      ctx->rxstat.started = 0;
    }
  return ret;
}

ssize_t req_get_wbuf(struct dspd_req_ctx *ctx, void **buf, size_t len)
{
  ssize_t ret;
  if ( len > ctx->txmax )
    {
      ret = -EINVAL;
    } else if ( ctx->txstat.started )
    {
      ret = -EBUSY;
    } else
    {
      *buf = ctx->txpkt;
      ctx->txpkt->len = len;
      ret = len;
    }
  return ret;
}


ssize_t dspd_req_read_cb(void *arg, void *buf, size_t len)
{
  intptr_t fd = (intptr_t)arg;
  ssize_t ret;
  ret = read(fd, buf, len);
  if ( ret < 0 )
    {
      if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
	ret = -EINPROGRESS;
      else
	ret = -errno;
    } else if ( ret == 0 )
    {
      ret = -ECONNABORTED;
    }
  return ret;
}
ssize_t dspd_req_write_cb(void *arg, const void *buf, size_t len)
{
  intptr_t fd = (intptr_t)arg;
  ssize_t ret;
  ret = write(fd, buf, len);
  if ( ret < 0 )
    {
      if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
	ret = -EINPROGRESS;
      else
	ret = -errno;
    } else if ( ret == 0 )
    {
      ret = -ECONNABORTED;
    }
  return ret;
}

int dspd_req_getfd_cb(void *arg)
{
  return (intptr_t)arg;
}





int dspd_ctx_get_fd(void *context)
{
  uint32_t objtype = *(uint32_t*)context;
  int fd;
  struct dspd_conn *conn;
  if ( objtype == DSPD_OBJ_TYPE_IPC )
    {
      conn = context;
      fd = conn->sock_fd;
    } else
    {
      fd = -1;
    }
  return fd;
}



bool dspd_req_input_pending(struct dspd_req_ctx *ctx)
{
  return ctx->rxstat.started;
}
bool dspd_req_output_pending(struct dspd_req_ctx *ctx)
{
  return ctx->txstat.started;
}



int dspd_stream_ctl(void    *context, //DSPD object, such as dspd_dctx
		    uint32_t stream,  //Slot in object list (0 is for daemon)
		    uint32_t req,     //16 bit req + 2 bits flags + 14 bits reserved
		    const void          *inbuf,
		    size_t        inbufsize,
		    void         *outbuf,
		    size_t        outbufsize,
		    size_t       *bytes_returned)
{
  struct dspd_daemon_ctx *dctx;
  uint32_t objtype;
  int32_t ret;
  struct dspd_rctx rctx;
  size_t tmp;
  if ( context == NULL )
    return -EFAULT;
  objtype = *(uint32_t*)context;
  if ( bytes_returned == NULL )
    {
      if ( outbufsize == 0 )
	bytes_returned = &tmp;
      else
	return -EFAULT;
    }
  *bytes_returned = 0;
  //fprintf(stderr, "REQ %u STREAM %u LEN=%lu,%lu\n", req, stream, (long)inbufsize, (long)outbufsize);
  if ( objtype == DSPD_OBJ_TYPE_DAEMON_CTX )
    {
      dctx = context;
      rctx.ops = dctx->ctl_ops;
      rctx.user_data = NULL; //Will be filled in by dspd_slist_ctl()
      rctx.ops_arg = dctx; //Data for ops callbacks
      rctx.outbuf = outbuf;
      rctx.bytes_returned = 0;
      rctx.outbufsize = outbufsize;
      //Copy incoming file descriptor
      if ( DSPD_FD(req) && (inbufsize >= sizeof(rctx.fd)) )
	memcpy(&rctx.fd, inbuf, sizeof(rctx.fd));
      else
	rctx.fd = -1;
      rctx.index = stream;
      rctx.flags = req & (DSPD_REQ_FLAG_CMSG_FD|DSPD_REQ_FLAG_REMOTE);
      req &= ~(DSPD_REQ_FLAG_CMSG_FD|DSPD_REQ_FLAG_REMOTE);
      ret = dspd_slist_ctl(dctx->objects,
			   &rctx,
			   req,
			   inbuf,
			   inbufsize,
			   outbuf, 
			   outbufsize);
      *bytes_returned = rctx.bytes_returned;
      if ( rctx.fd >= 0 && DSPD_FD(req) )
	close(rctx.fd);
    } else if ( objtype == DSPD_OBJ_TYPE_IPC )
    {
      ret = dspd_conn_ctl(context, 
			  stream, 
			  req, 
			  inbuf, 
			  inbufsize, 
			  outbuf,
			  outbufsize,
			  bytes_returned);
    } else if ( objtype == DSPD_OBJ_TYPE_AIO )
    {
      ret = dspd_aio_sync_ctl(context,
			      stream, 
			      req, 
			      inbuf, 
			      inbufsize, 
			      outbuf,
			      outbufsize,
			      bytes_returned);
    } else
    {
      ret = -EINVAL;
    }
  
  return ret;
}
		    

int32_t dspd_req_reply_buf(struct dspd_rctx *r, 
			   int32_t flags, 
			   const void *buf, 
			   size_t len)
{
  return r->ops->reply_buf(r, flags, buf, len);
}

int32_t dspd_req_reply_fd(struct dspd_rctx *r, 
			  int32_t flags, 
			  const void *buf, 
			  size_t len,
			  int32_t fd)
{
  return r->ops->reply_fd(r, flags, buf, len, fd);
}

int32_t dspd_req_reply_err(struct dspd_rctx *r,
			   int32_t flags,
			   int32_t err)
{
  if ( err > 0 )
    err *= -1;
  return r->ops->reply_err(r, flags, err);
}

int32_t dspd_req_flags(const struct dspd_rctx *rctx)
{
  return rctx->flags;
}

/*
  Reap the incoming file descriptor.  The file descriptor
  should be in the first 4 bytes of the payload.  This is
  supposed to make the code less error prone because if
  a request callback does not process the file descriptor
  then it will not be leaked.
*/
int32_t dspd_req_get_fd(struct dspd_rctx *rctx)
{
  int ret = rctx->fd;
  rctx->fd = -1;
  return ret;
}

int32_t dspd_req_index(struct dspd_rctx *rctx)
{
  return rctx->index;
}
void *dspd_req_userdata(struct dspd_rctx *rctx)
{
  return rctx->user_data;
}

