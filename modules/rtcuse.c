/*
 *   RTCUSE - A simple direct CUSE API (no extra malloc()s or threads)
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
#define _FILE_OFFSET_BITS 64
#include <fuse.h>
#include <linux/fuse.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rtcuse.h"



//An offset of 0 is invalid so that some things can be
//unimplemented.
#define getfho(_s) (offsetof(struct _s, fh) + 1)

static size_t rtcuse_offsets[] = {
  [FUSE_LOOKUP] = 0,
  [FUSE_FORGET] = 0,
  [FUSE_GETATTR] = getfho(fuse_getattr_in),
  [FUSE_SETATTR] = getfho(fuse_setattr_in),
  [FUSE_READLINK] = 0,
  [FUSE_SYMLINK] = 0,
  [FUSE_MKNOD] = 0,
  [FUSE_MKDIR] = 0,
  [FUSE_UNLINK] = 0,
  [FUSE_RMDIR] = 0,
  [FUSE_RENAME] = 0,
  [FUSE_LINK] = 0,
  [FUSE_OPEN] = 0,
  [FUSE_READ] = getfho(fuse_read_in),
  [FUSE_WRITE] = getfho(fuse_write_in),
  [FUSE_STATFS] = 0,
  [FUSE_RELEASE] = getfho(fuse_release_in),
  [FUSE_FSYNC] = getfho(fuse_fsync_in),
  [FUSE_SETXATTR] = 0, 
  [FUSE_GETXATTR] = 0,
  [FUSE_LISTXATTR] = 0,
  [FUSE_REMOVEXATTR] = 0,
  [FUSE_FLUSH] = getfho(fuse_flush_in),
  [FUSE_INIT] = 0,
  [FUSE_OPENDIR] = 0,
  [FUSE_READDIR] = 0,
  [FUSE_RELEASEDIR] = 0,
  [FUSE_FSYNCDIR] = 0,
  [FUSE_GETLK] = 0,
  [FUSE_SETLK] = 0,
  [FUSE_SETLKW] = 0,
  [FUSE_ACCESS] = 0,
  [FUSE_CREATE] = 0,
  [FUSE_INTERRUPT] = 0, //This one requires searching all clients.
  [FUSE_BMAP] = 0,
  [FUSE_DESTROY] = 0,
  [FUSE_IOCTL] = getfho(fuse_ioctl_in),
  [FUSE_POLL] = getfho(fuse_poll_in),
};

/*
  Get the file handle for a request.  Returns false if the request has
  no associated file handle.  I think that all of the requests that
  CUSE actually uses have fh as the first member of the struct.  If
  that turns out to be the case I will need to get rid of this function.
  As it is, I need to try to follow the protocol as best as possible.

*/
bool rtcuse_get_fh(struct fuse_in_header *hdr, void *buf, uint64_t *fh)
{
  bool ret;
  size_t offset;
  if ( hdr->opcode > (sizeof(rtcuse_offsets)/sizeof(rtcuse_offsets[0])) )
    {
      ret = false;
    } else
    {
      offset = rtcuse_offsets[hdr->opcode];
      if ( offset )
	{
	  ret = true;
	  offset--;
	  *fh = *(uint64_t*)((char*)buf+offset);
	} else
	{
	  ret = false;
	}
    }
  return ret;
}



int rtcuse_create_cdev(const char *device, 
		       int oflags, 
		       struct rtcuse_cdev_params *devp,
		       struct rtcuse_cdev **dev)
{
  struct iovec iov[3];
  struct fuse_init_in   inarg;
  struct cuse_init_out outarg;
  struct fuse_in_header hdr;
  struct fuse_out_header ohdr;
  ssize_t ret;
  char buf[300];
  ssize_t namelen;
  struct pollfd pfd;
  struct rtcuse_cdev *d;

  d = calloc(1, sizeof(*d));
  if ( ! d )
    return -errno;
  memcpy(&d->params, devp, sizeof(*devp));

  if ( d->params.maxwrite < 0 )
    d->params.maxwrite = 65536;
  if ( d->params.maxread < 0 )
    d->params.maxread = 65536;

  //The kernel takes this to generate the device name
  namelen = sprintf(buf, "DEVNAME=%s", devp->devname) + 1;
  
  
  oflags |= O_RDWR;
  if ( device == NULL )
    device = "/dev/cuse";
  d->fd = open(device, oflags);
  if ( d->fd < 0 )
    goto out;
  

  /*
    The kernel will quickly send an init command
   */
  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = &inarg;
  iov[1].iov_len = sizeof(inarg);
  while ( (ret = readv(d->fd, iov, 2)) < 0 )
    {
      if ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
	goto out;
      
      pfd.fd = d->fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      ret = poll(&pfd, 1, 1000);
      if ( ret < 0 && errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN )
	goto out;
      
    }
  ohdr.len = sizeof(ohdr) + sizeof(outarg) + namelen;
  ohdr.error = 0;
  ohdr.unique = hdr.unique;
  

  /*
    The reply has the name of the device and some parameters.
   */
  memset(&outarg, 0, sizeof(outarg));
  outarg.max_read = d->params.maxread;
  outarg.max_write = d->params.maxwrite;
  outarg.major = FUSE_KERNEL_VERSION;
  outarg.minor = FUSE_KERNEL_MINOR_VERSION;
  outarg.dev_major = d->params.major;
  outarg.dev_minor = d->params.minor;
  
  iov[0].iov_base = &ohdr;
  iov[0].iov_len = sizeof(ohdr);
  iov[1].iov_base = &outarg;
  iov[1].iov_len = sizeof(outarg);
  iov[2].iov_base = buf;
  iov[2].iov_len = namelen;

  
  while ( (ret = writev(d->fd, iov, 3)) < 0 )
    {
      if ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
	goto out;
      
      pfd.fd = d->fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      ret = poll(&pfd, 1, 1000);
      if ( ret < 0 && errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN )
	goto out;
    }
  *dev = d;

  if ( d->params.maxread > d->params.maxwrite )
    d->params.pktlen = d->params.maxread;
  else
    d->params.pktlen = d->params.maxwrite;
  d->params.pktlen += sizeof(struct fuse_in_header) + sizeof(struct fuse_read_in);

  

  return 0;

 out:
  ret = -errno;
  close(d->fd);
  free(d);
  return ret;
}

void rtcuse_destroy_cdev(struct rtcuse_cdev *dev)
{
  close(dev->fd);
  free(dev);
}

ssize_t rtcuse_readv(struct rtcuse_cdev *dev, const struct iovec *iov, int iovcnt)
{
  ssize_t ret;
  ret = readv(dev->fd, iov, iovcnt);
  if ( ret < 0 )
    ret = -errno;
  return ret;
}

ssize_t rtcuse_readv_block(struct rtcuse_cdev *dev, const struct iovec *iov, int iovcnt)
{
  ssize_t ret;
  struct pollfd pfd;
  pfd.fd = dev->fd;
  pfd.revents = POLLIN;
  while ( (ret = rtcuse_readv(dev, iov, iovcnt)) < 0 )
    {
      if ( ret != -EAGAIN && ret != -EINTR && ret != -EWOULDBLOCK )
	break;
      ret = poll(&pfd, 1, 10);
      if ( ret < 0 )
	{
	  if ( errno != EINTR && errno != EAGAIN )
	    {
	      ret = -errno;
	      break;
	    }
	}
       if ( pfd.revents & (POLLERR|POLLNVAL|POLLHUP) )
	 {
	   ret = -ECONNABORTED;
	   break;
	 }
    }
  return ret;
}

ssize_t rtcuse_writev(struct rtcuse_cdev *dev, 
		      const struct iovec *iov, 
		      int iovcnt)
{
  ssize_t ret;
  ret = writev(dev->fd, iov, iovcnt);
  if ( ret < 0 )
    ret = -errno;
  return ret;
}

ssize_t rtcuse_writev_block(struct rtcuse_cdev *dev, const struct iovec *iov, int iovcnt)
{
  ssize_t ret;
  struct pollfd pfd;
  pfd.fd = dev->fd;
  pfd.revents = POLLOUT;
  while ( (ret = rtcuse_writev(dev, iov, iovcnt)) < 0 )
    {
      if ( ret != -EAGAIN && ret != -EINTR && ret != -EWOULDBLOCK )
	break;
      ret = poll(&pfd, 1, 10);
      if ( ret < 0 )
	{
	  if ( errno != EINTR && errno != EAGAIN )
	    {
	      ret = -errno;
	      break;
	    }
	}
       if ( pfd.revents & (POLLERR|POLLNVAL|POLLHUP) )
	 {
	   ret = -ECONNABORTED;
	   break;
	 }
    }
  return ret;
}


int rtcuse_reply_open(struct rtcuse_cdev *dev, uint64_t unique, uint64_t fh, uint32_t flags)
{
  struct fuse_open_out out = { 0 };
  struct fuse_out_header hdr;
  struct iovec iov[2];
  out.fh = fh;
  out.open_flags = flags;
  hdr.unique = unique;
  hdr.len = sizeof(hdr) + sizeof(out);
  hdr.error = 0;

  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = &out;
  iov[1].iov_len = sizeof(out);
  return rtcuse_writev_block(dev, iov, 2);
}



/*int main(void)
{
  int ret;
  struct rtcuse_cdev_params p = { 0 };
  struct rtcuse_cdev *d;

  strcpy(p.devname, "testdev");
  p.maxwrite = 4096;
  p.maxread = 4096;
  

  ret = rtcuse_create_cdev("/dev/cuse", O_NONBLOCK | O_RDWR, &p, &d);

  printf("RESULT %d\n", ret);
  while(1) sleep(1);

  }*/
