/*
  Client side request protocol. 
*/
#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include "sslib.h"
#include "daemon.h"
#include "util.h"
#include "ssclient.h"



static bool check_async_event(struct dspd_conn *conn)
{
  bool ret;
  int16_t flags;
  if ( conn->header_in.cmd == DSPD_DCTL_ASYNC_EVENT )
    {
      ret = true;
      flags = conn->header_in.flags;
      if ( flags & DSPD_REQ_FLAG_POLLIN )
	conn->revents |= POLLIN;
      if ( flags & DSPD_REQ_FLAG_POLLOUT )
	conn->revents |= POLLOUT;
      if ( flags & DSPD_REQ_FLAG_POLLPRI )
	conn->revents |= POLLPRI;
    } else
    {
      ret = false;
    }
  return ret;
}

static int conn_poll(struct dspd_conn *conn, int events, int *revents)
{
  int ret;
  conn->pfd.events = events | POLLRDHUP;
  conn->pfd.revents = 0;
  ret = poll(&conn->pfd, 1, conn->timeout);
  if ( ret < 0 )
    {
      ret = -errno;
    } else if ( ret == 0 )
    {
      ret = -ETIMEDOUT;
    } else
    {
      if ( revents )
	*revents = conn->pfd.revents;
      if ( conn->pfd.revents & DSPD_POLLERR )
	ret = -ECONNABORTED;
      else
	ret = 0;
    }
  return ret;
}

static ssize_t conn_read(struct dspd_conn *conn, void *addr, size_t len)
{
  ssize_t ret;
  ret = read(conn->sock_fd, addr, len);
  if ( ret < 0 )
    ret = -errno;
  else if ( ret == 0 )
    ret = -ECONNABORTED;
  return ret;
}

/*
  Receiving file descriptors that have been requested is supported.  Sending file
  descriptors is not supported.  The server should generally not be doing anything
  requiring a client to send file descriptors.  Doing so might be a security issue.
  If for some reason this code is updated to allow sending file descriptors to the
  server then the practical uses of it will be limited.
*/

static ssize_t cmsg_recvfd(int s, void *buf, size_t len, int *fdptr)
{
  int n;
  int fd = -1;
  struct msghdr msg;
  struct cmsghdr *cmsg;
  char cms[CMSG_SPACE(sizeof(int))];
  struct iovec iov;
  ssize_t ret;
  memset(&msg, 0, sizeof msg);
  iov.iov_len = len;
  iov.iov_base = buf;
  msg.msg_name = 0;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
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
  ret = n;

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
	  memmove(&fd, dptr, sizeof(fd));
	} else
	{
	  //Get rid of unwanted file descriptors
	  memmove(&f, dptr, sizeof(int));
	  close(f);
	}

      i++;
    }
  *fdptr = fd;
  return ret;
}



static void reset_input(struct dspd_conn *conn)
{
  conn->async_input_pending = false;
  conn->offset_in = 0;
  conn->header_in.len = 0;
  conn->header_in.cmd = 0;
}
static int dispatch_input(struct dspd_conn *conn)
{
  if ( check_async_event(conn) )
    {
      conn->event_flags |= conn->header_in.flags;
      if ( conn->event_flags_changed )
	conn->event_flags_changed(conn->arg, &conn->event_flags);
      conn->event_processed = true;
    }
  reset_input(conn);
  return 0;
}

static int recv_async(struct dspd_conn *conn)
{
  ssize_t ret = 0;
  int e;
  size_t o;
  int result = 0;
  if ( conn->offset_in < sizeof(conn->header_in) )
    {
      ret = read(conn->sock_fd, 
		 ((char*)&conn->header_in) + conn->offset_in,
		 sizeof(conn->header_in) - conn->offset_in);
      if ( ret < 0 )
	{
	  e = errno;
	  if ( e != EINTR && e != EAGAIN && e != EWOULDBLOCK )
	    return -ECONNABORTED;
	} else if ( ret == 0 )
	{
	  return -ECONNABORTED;
	} else
	{
	  conn->offset_in += ret;
	  if ( conn->offset_in )
	    conn->async_input_pending = true;
	  if ( conn->offset_in == sizeof(conn->header_in) )
	    {
	      if ( conn->header_in.len < sizeof(conn->header_in) ||
		   conn->header_in.len > (sizeof(conn->header_in) + sizeof(conn->data_in)) ||
		   (conn->header_in.flags & DSPD_REQ_FLAG_CMSG_FD) ||
		   ((conn->header_in.flags & DSPD_REQ_FLAG_EVENT) == 0 &&
		    conn->header_in.cmd != DSPD_DCTL_ASYNC_EVENT))
		{
		  return -EPROTO;
		}
	    }
	}
    }
  if ( conn->offset_in >= sizeof(conn->header_in) )
    {
      if ( conn->offset_in < conn->header_in.len )
	{
	  o = conn->offset_in - sizeof(conn->header_in);
	  ret = read(conn->sock_fd,
		     &conn->data_in[o],
		     conn->header_in.len - conn->offset_in);
	  if ( ret < 0 )
	    {
	      e = errno;
	      if ( e != EINTR && e != EAGAIN && e != EWOULDBLOCK )
		result = -e;

	    } else if ( ret == 0 )
	    {
	      result = -ECONNABORTED;
	    } else
	    {
	      conn->offset_in += ret;
	    }
	}
      if ( result == 0 && conn->offset_in == conn->header_in.len )
	result = dispatch_input(conn);
    }
  

  return result;
}

static int send_req(struct dspd_conn *conn, const void *inbuf, size_t len)
{
  size_t offset = 0;
  size_t total = len + sizeof(conn->header_out);
  ssize_t ret, result = 0;
  int niov = 1;
  struct iovec iov[2];
  int revents;
  iov[0].iov_base = &conn->header_out;
  iov[0].iov_len = sizeof(conn->header_out);
  if ( len )
    {
      iov[1].iov_base = (void*)inbuf;
      iov[1].iov_len = len;
      niov++;
    }
  
  while ( offset < total )
    {
      ret = dspd_writev(conn->sock_fd, iov, niov, &offset);
      if ( ret == -EINTR )
	continue;
      if ( ret == -EAGAIN || ret == -EWOULDBLOCK )
	{
	  revents = 0;
	  ret = conn_poll(conn, POLLIN|POLLOUT, &revents);
	  if ( ret < 0 )
	    {
	      result = ret;
	      break;
	    }
	  if ( revents & POLLIN )
	    {
	      ret = recv_async(conn);
	      if ( ret != 0 )
		{
		  result = ret;
		  break;
		}
	    }
	} else if ( ret < 0 )
	{
	  if ( ret == DSPD_IOV_EOF )
	    result = -ECONNABORTED;
	  else
	    result = ret;
	  break;
	}
    }
  return result;
}

static int recv_header(struct dspd_conn *conn)
{
  ssize_t ret;
  int result = 0;
  while ( conn->offset_in < sizeof(conn->header_in) )
    {
      ret = conn_read(conn,  
		     ((char*)&conn->header_in) + conn->offset_in,
		     sizeof(conn->header_in) - conn->offset_in);
      if ( ret < 0 )
	{
	  if ( ret == -EWOULDBLOCK || ret == -EAGAIN )
	    {
	      ret = conn_poll(conn, POLLIN, NULL);
	      if ( ret < 0 && ret != -EINTR && ret != -EAGAIN )
		{
		  result = ret;
		  break;
		}
	    } else if ( ret != -EINTR )
	    {
	      result = ret;
	      break;
	    }
	} else
	{
	  conn->offset_in += ret;
	}
    }
  return result;
}

static int recv_data(struct dspd_conn *conn, char *buf, size_t len)
{
  size_t l = conn->header_in.len - sizeof(conn->header_in);
  size_t offset = conn->offset_in - sizeof(conn->header_in);
  ssize_t ret;
  int result = 0;
  if ( l > len )
    return EPROTO;
  while ( offset < l )
    {
      ret = conn_read(conn, &buf[offset], len - offset);
      if ( ret < 0 )
	{
	  if ( ret == -EAGAIN || ret == -EWOULDBLOCK )
	    {
	      ret = conn_poll(conn, POLLIN, NULL);
	      if ( ret < 0 && ret != -EINTR && ret != -EAGAIN )
		{
		  result = ret;
		  break;
		}
	    } else if ( ret != -EINTR )
	    {
	      result = -ECONNABORTED;
	      break;
	    }
	} else
	{
	  offset += ret;
	  conn->offset_in += ret;
	}
    }
  return result;
}
static int recv_fd(struct dspd_conn *conn, void *buf, size_t len)
{
  size_t l = conn->header_in.len - sizeof(conn->header_in);
  size_t offset = 0;
  int fd = -1;
  ssize_t ret;
  if ( l < sizeof(int32_t) )
    return -EPROTO;
  while ( offset < 1 )
    {
      ret = cmsg_recvfd(conn->sock_fd, 
			(char*)buf + offset,
			l - offset,
			&fd);
      if ( ret < 0 )
	{
	  if ( ret == -EAGAIN || ret == -EWOULDBLOCK )
	    {

	      ret = conn_poll(conn, POLLIN, NULL);
	      if ( ret < 0 && ret != -EAGAIN && ret != -EINTR )
		return ret;
	    } else if ( ret != -EINTR )
	    {
	      return ret;
	    }
	} else
	{
	  offset += ret;
	  conn->offset_in += ret;
	}
    }
  ret = recv_data(conn, buf, len);
  if ( conn->fd_in >= 0 )
    close(conn->fd_in);
  conn->fd_in = fd;
  if ( ret == 0 )
    memcpy(buf, &conn->fd_in, sizeof(fd));
  return ret;
}

static int recv_req(struct dspd_conn *conn, void *outbuf, size_t outbufsize)
{
  size_t offset = 0, total = outbufsize + sizeof(conn->header_in);
  int ret, result = 0;
  while ( offset < total )
    {
      //Get the entire header
      ret = recv_header(conn);
      if ( ret )
	{
	  result = ret;
	  break;
	}
      if ( (conn->header_in.cmd == DSPD_DCTL_ASYNC_EVENT) ||
	   (conn->header_in.flags & DSPD_REQ_FLAG_EVENT) )
	{
	  if ( (conn->header_in.len - sizeof(conn->header_in)) > sizeof(conn->data_in) )
	    {
	      result = -EPROTO;
	      break;
	    }
	  //Receive async event (not the expected reply)
	  conn->async_input_pending = true;
	  while ( conn->async_input_pending )
	    {
	      ret = recv_async(conn);
	      if ( ret )
		{
		  result = ret;
		  break;
		}
	      if ( conn->async_input_pending )
		{
		  ret = conn_poll(conn, POLLIN, NULL);
		  if ( ret < 0 && ret != -EINTR && ret != -EAGAIN )
		    {
		      result = ret;
		      break;
		    }
		}
	    }
	} else
	{
	  if ( (conn->header_in.len - sizeof(conn->header_in)) > outbufsize )
	    {
	      result = -EPROTO;
	      break;
	    }

	  //Get the reply.  Nothing more to do.
	  if ( conn->header_in.flags & DSPD_REQ_FLAG_CMSG_FD )
	    result = recv_fd(conn, outbuf, outbufsize);
	  else
	    result = recv_data(conn, outbuf, outbufsize);
	  
	  break;
	}
    }
  return result;
}

int dspd_ipc_process_messages(struct dspd_conn *conn, int timeout)
{
  struct pollfd pfd;
  int ret, result = 0;
  uint32_t objtype = *(uint32_t*)conn;
  if ( objtype != DSPD_OBJ_TYPE_IPC )
    return 0;
  dspd_mutex_lock(&conn->lock);
  pfd.fd = conn->sock_fd;
  pfd.events = POLLIN | POLLRDHUP;
  pfd.revents = 0;
  conn->event_processed = false;
  while ( (ret = poll(&pfd, 1, timeout)) == 1 )
    {
      if ( pfd.revents & (POLLRDHUP|POLLHUP|POLLERR|POLLNVAL) )
	{
	  result = -ECONNABORTED;
	  goto out;
	}
      ret = recv_async(conn);
      if ( ret )
	{
	  result = ret;
	  goto out;
	} else if ( conn->event_processed )
	{
	  conn->event_processed = false;
	  break;
	} else if ( conn->async_input_pending == true && timeout == 0 )
	{
	  timeout = 10;
	}
    }
  if ( ret < 0 )
    {
      result = -errno;
      if ( result == -EAGAIN || result == -EINTR )
	result = 0;
    }
  
 out:
  dspd_mutex_unlock(&conn->lock);
  return result;
}



int dspd_conn_ctl(struct dspd_conn *conn,
		  uint32_t stream,
		  uint32_t req,
		  const void          *inbuf,
		  size_t        inbufsize,
		  void         *outbuf,
		  size_t        outbufsize,
		  size_t       *bytes_returned)
{
  int ret;
  dspd_mutex_lock(&conn->lock);
  conn->header_out.len = sizeof(conn->header_out) + inbufsize;
  conn->header_out.flags = 0;
  conn->header_out.cmd = req & 0xFFFF; //16 bits for now
  conn->header_out.stream = stream;
  conn->header_out.bytes_returned = 0;
  conn->header_out.rdata.rlen = outbufsize;

  if ( conn->fd_in >= 0 )
    {
      close(conn->fd_in);
      conn->fd_in = -1;
    }
  ret = send_req(conn, inbuf, inbufsize);
  if ( ret == 0 )
    {
      ret = recv_req(conn, outbuf, outbufsize);
      if ( ret == 0 )
	{
	  if ( conn->header_in.flags & DSPD_REQ_FLAG_ERROR )
	    {
	      ret = conn->header_in.rdata.err;
	      if ( ret > 0 )
		ret *= -1;
	    } else if ( bytes_returned )
	    {
	      *bytes_returned = conn->header_in.len - sizeof(conn->header_in);
	    }
	  reset_input(conn);
	}
    }
  dspd_mutex_unlock(&conn->lock);
  return ret;
}


int32_t dspd_conn_recv_fd(struct dspd_conn *conn)
{
  int ret;
  uint32_t objtype = *(uint32_t*)conn;
  if ( objtype == DSPD_OBJ_TYPE_IPC )
    {
      dspd_mutex_lock(&conn->lock);
      ret = conn->fd_in;
      conn->fd_in = -1;
      dspd_mutex_unlock(&conn->lock);
    } else
    {
      ret = -EAGAIN;
    }
  return ret;
}

void dspd_conn_delete(struct dspd_conn *conn)
{
  uint32_t t = *(int32_t*)conn;
  if ( t == DSPD_OBJ_TYPE_IPC )
    {
      if ( conn->fd_in >= 0 )
	close(conn->fd_in);
      dspd_mutex_destroy(&conn->lock);
      if ( conn->sock_fd >= 0 )
	close(conn->sock_fd);
      free(conn);
    }
}


int dspd_conn_new(const char *addr, struct dspd_conn **ptr)
{
  struct dspd_conn *conn = calloc(1, sizeof(struct dspd_conn));
  int ret;
  if ( ! conn )
    return -ENOMEM;
  conn->magic = DSPD_OBJ_TYPE_IPC;
  if ( addr == NULL )
    addr = "/var/run/dspd/dspd.sock";
  conn->fd_in = -1;
  conn->sock_fd = dspd_unix_sock_connect(addr, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if ( conn->sock_fd < 0 )
    {
      ret = conn->sock_fd;
      dspd_conn_delete(conn);
      return ret;
    }
  if ( (ret = dspd_mutex_init(&conn->lock, NULL)) != 0 )
    {
      ret *= -1;
      dspd_conn_delete(conn);
      return ret;
    }
  
  conn->pfd.fd = conn->sock_fd;
  conn->timeout = -1;
  *ptr = conn;
  return 0;
}




uint32_t dspd_conn_revents(struct dspd_conn *conn)
{
  uint32_t ret;
  dspd_mutex_lock(&conn->lock);
  ret = conn->revents;
  conn->revents = 0;
  dspd_mutex_unlock(&conn->lock);
  return ret;
}

void dspd_conn_set_event_flag_cb(struct dspd_conn *conn, 
				 void (*event_flags_changed)(void *arg, uint32_t *flags),
				 void *arg)
{
  dspd_mutex_lock(&conn->lock);
  conn->event_flags_changed = event_flags_changed;
  conn->arg = arg;
  dspd_mutex_unlock(&conn->lock);
}
uint32_t dspd_conn_get_event_flags(struct dspd_conn *conn, bool clear)
{
  uint32_t ret;
  dspd_mutex_lock(&conn->lock);
  ret = conn->event_flags;
  if ( clear )
    conn->event_flags = 0;
  dspd_mutex_unlock(&conn->lock);
  return ret;
}


int32_t dspd_select_device(struct dspd_conn *ssc, 
			   int32_t streams,
			   int32_t (*select_device)(void *arg, int32_t streams, int32_t index, const struct dspd_device_stat *info),
			   void *arg)
{
  int32_t pdef = -1, cdef = -1, def = -1, s, dev;
  uint8_t mask[DSPD_MASK_MAX];
  int32_t err = 0, ret;
  size_t br;
  struct dspd_device_stat dstat;
  uint32_t mtype;
  size_t n, i;
  if ( select_device == NULL )
    {
      err = dspd_stream_ctl(ssc, 0, DSPD_DCTL_GET_DEFAULTDEV, &streams, sizeof(streams), &def, sizeof(def), &br);
      if ( err == 0 && def == -1 )
	{
	  if ( streams == DSPD_PCM_SBIT_FULLDUPLEX )
	    {
	      s = DSPD_PCM_SBIT_PLAYBACK;
	      err = dspd_stream_ctl(ssc, 
				    0, 
				    DSPD_DCTL_GET_DEFAULTDEV, 
				    &s, 
				    sizeof(s), 
				    &pdef, 
				    sizeof(pdef), 
				    &br);
	      if ( err == 0 )
		{
		  s = DSPD_PCM_SBIT_CAPTURE;
		  err = dspd_stream_ctl(ssc, 
					0, 
					DSPD_DCTL_GET_DEFAULTDEV, 
					&s, 
					sizeof(s), 
					&cdef, 
					sizeof(cdef), 
					&br);
		}
	    }
	} else
	{
	  if ( streams & DSPD_PCM_SBIT_PLAYBACK )
	    pdef = def;
	  if ( streams & DSPD_PCM_SBIT_CAPTURE )
	    cdef = def;
	}
      if ( err == 0 && pdef >= 0 )
	{
	  err = dspd_stream_ctl(ssc,
				-1,
				DSPD_SOCKSRV_REQ_REFSRV,
				&pdef,
				sizeof(pdef),
				&dstat,
				sizeof(dstat),
				&br);
	  if ( err == 0 && (dstat.streams & DSPD_PCM_SBIT_PLAYBACK) )
	    {
	      s = DSPD_PCM_SBIT_PLAYBACK;
	      if ( cdef >= 0 )
		s |= DSPD_PCM_SBIT_CAPTURE;
	      err = dspd_stream_ctl(ssc,
				    -1,
				    DSPD_SOCKSRV_REQ_SETSRV,
				    &s,
				    sizeof(s),
				    NULL,
				    0,
				    &br);
	    }
	}
      if ( err == 0 && cdef >= 0 && cdef != pdef )
	{
	  s = DSPD_PCM_SBIT_CAPTURE;
	  err = dspd_stream_ctl(ssc,
				-1,
				DSPD_SOCKSRV_REQ_SETSRV,
				&s,
				sizeof(s),
				NULL,
				0,
				&br);
	}
    } else
    {
      mtype = DSPD_DCTL_ENUM_TYPE_SERVER;
      err = dspd_stream_ctl(ssc,
			    0,
			    DSPD_DCTL_ENUMERATE_OBJECTS,
			    &mtype,
			    sizeof(mtype),
			    mask,
			    sizeof(mask),
			    &br);
      if ( err == 0 )
	{
	  n = br * 8;
	  memset(&dstat, 0, sizeof(dstat));
	  br = 0;
	  for ( i = 0; i < n; i++ )
	    {
	      if ( dspd_test_bit(mask, i) )
		{
		  dev = i;
		  err = dspd_stream_ctl(ssc,
					-1,
					DSPD_SOCKSRV_REQ_REFSRV,
					&dev,
					sizeof(dev),
					&dstat,
					sizeof(dstat),
					&br);
		  if ( err < 0 && err != -ENOENT )
		    break;
		  if ( br == sizeof(dstat) && (streams == 0 || (dstat.streams & streams)) )
		    {
		      ret = select_device(arg, streams, dev, &dstat);
		      if ( ret == SELECT_DEV_OK )
			{
			  err = dspd_stream_ctl(ssc,
						-1,
						DSPD_SOCKSRV_REQ_SETSRV,
						&dstat.streams,
						sizeof(dstat.streams),
						NULL,
						0,
						&br);
			} else if ( ret == SELECT_DEV_REJECT )
			{
			  continue;
			} else if ( ret == SELECT_DEV_ABORT )
			{
			  err = -EINTR;
			  break;
			} else if ( ret == SELECT_DEV_OK_ABORT )
			{
			  err = dspd_stream_ctl(ssc,
						-1,
						DSPD_SOCKSRV_REQ_SETSRV,
						&dstat.streams,
						sizeof(dstat.streams),
						NULL,
						0,
						&br);
			  break;
			}
		      if ( err < 0 )
			break;
 		    }
		}
	    }
	}
    }

  return err;
}

int dspd_conn_get_socket(struct dspd_conn *conn)
{
  uint32_t objtype = *(uint32_t*)conn;
  int32_t ret;
  if ( objtype == DSPD_OBJ_TYPE_IPC )
    ret = conn->sock_fd;
  else
    ret = -ENOTSOCK;
  return ret;
}

