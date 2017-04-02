/*
  OSSCUSE_CDEV_HELPER: A secure helper for using mod_osscuse with server running as 
  an unprivileged user.

  It is possible to use ACLs or otherwise grant any user access to /dev/cuse.  This
  might be overkill since mod_osscuse only needs to create /dev/mixer and /dev/dsp nodes
  and only needs one major number.

  

*/

#include <unistd.h>
#include <string.h>
#include <linux/fuse.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif

#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../lib/cbpoll.h"
#include "rtcuse.h"
#include "mod_osscuse.h"
#include "soundcard.h"

static ssize_t write_data(int fd, const void *ptr, size_t len)
{
  size_t offset = 0;
  ssize_t ret;
  while ( offset < len )
    {
      ret = write(fd, (const char*)ptr + offset, len - offset);
      if ( ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
	return -errno;
      else if ( ret == 0 )
	return -EIO;
      offset += ret;
    }
  return 0;
}

static void osscuse_device_handler(int fd, int major)
{
  ssize_t ret, offset = 0;
  int32_t result;
  struct osscuse_open_req ipkt;
  struct rtcuse_cdev *dev;
  struct iovec iov;
  while ( (ret = read(fd, ((char*)&ipkt)+offset, sizeof(ipkt) - offset)) )
    {
      if ( ret < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR )
	break;
      offset += ret;
      if ( offset == sizeof(ipkt) )
	{
	  offset = 0;
	  
	  if ( major > 0 )
	    ipkt.params.major = major;
	  if ( ipkt.devtype == DEVTYPE_MIXER )
	    sprintf(ipkt.params.devname, "mixer%d", ipkt.devnum);
	  else if ( ipkt.devtype == DEVTYPE_DSP )
	    sprintf(ipkt.params.devname, "dsp%d", ipkt.devnum);
	  else
	    break;
	  ret = rtcuse_create_cdev(NULL, O_NONBLOCK, &ipkt.params, &dev);
	  result = ret;
	  ret = write_data(fd, &result, sizeof(result));
	  if ( ret != 0 )
	    break;
	  
	  if ( result == 0 )
	    {
	      iov.iov_base = dev;
	      iov.iov_len = sizeof(*dev);
	      ret = dspd_cmsg_sendfd(fd, dev->fd, &iov);
	      if ( ret < 0 && ret != -EINTR && ret != -EAGAIN && ret != -EWOULDBLOCK )
		{
		  break;
		} else if ( ret > 0 )
		{
		  iov.iov_len -= ret;
		  iov.iov_base += ret;
		  if ( iov.iov_len > 0 )
		    if ( write_data(fd, iov.iov_base, iov.iov_len) < 0 )
		      break;
		  if ( major == 0 )
		    major = dev->params.major;
		  rtcuse_destroy_cdev(dev);
		}
	    }
	}
    }
  exit(0);
}
int set_caps(void)
{
  int ret = -1;
#ifdef HAVE_LIBCAP
  cap_t caps = cap_get_proc();
  if ( ! caps )
    return -1;
  cap_value_t newcaps[1] = { CAP_DAC_OVERRIDE };
  if ( cap_set_flag(caps, CAP_PERMITTED, 1, newcaps, CAP_SET) < 0 )
    goto out;
  if ( cap_set_proc(caps) < 0 )
    goto out;
  if ( prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0 )
    goto out;
  ret = 0;
 out:
  cap_free(caps);
#endif
  return ret;
}

int reset_caps(void)
{
  int ret = -1;
#ifdef HAVE_LIBCAP
  cap_t caps = cap_get_proc();
  if ( ! caps )
    return -1;
  cap_value_t newcaps[1] = { CAP_DAC_OVERRIDE };
  if ( cap_set_flag(caps, CAP_EFFECTIVE, 1, newcaps, CAP_SET) < 0 )
    goto out;
  if ( cap_set_proc(caps) < 0 )
    goto out;
  prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
  ret = 0;

 out:
  cap_free(caps);
#endif
  return ret;
}


int main(int argc, char **argv)
{
  int32_t ipc_fd, uid, gid;
  struct stat fi;
  if ( argc < 4 )
    return 1;
  if ( dspd_strtoi32(argv[1], &ipc_fd, 10) < 0 ||
       dspd_strtoi32(argv[2], &uid, 10) < 0 ||
       dspd_strtoi32(argv[3], &gid, 10) < 0 )
    return 1;
  if ( stat("/dev/cuse", &fi) == 0 )
    {
      if ( ((fi.st_gid == gid) && (fi.st_mode & S_IRGRP) && (fi.st_mode & S_IWGRP)) ||
	   ((fi.st_uid == uid) && (fi.st_mode & S_IRUSR) && (fi.st_mode & S_IWUSR)))
	{
	  setgid(gid);
	  setuid(uid);
	} else if ( (fi.st_mode & S_IRGRP) && (fi.st_mode & S_IWGRP) )
	{
	  setgid(fi.st_gid);
	  setuid(uid);
	} else
	{
#ifdef HAVE_LIBCAP
	  if ( set_caps() )
	    {
	      setgid(gid);
	      setuid(uid);
	      reset_caps();
	    }
#endif	  
	}
    }
  if ( daemon(0, 0) < 0 )
    return 1;
  osscuse_device_handler(ipc_fd, 0);
  return 0;
}
