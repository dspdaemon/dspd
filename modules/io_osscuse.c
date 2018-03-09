/*
 *   IO_OSSCUSE - Device IO for OSS
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */


#include <unistd.h>
#include <string.h>
#include <linux/fuse.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "soundcard.h"
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../lib/cbpoll.h"
#include "rtcuse.h"
#include "mod_osscuse.h"
struct dspd_ioctl_info {
  const struct dspd_req_handler *handlers;
  size_t                         count;
};

#define REQ_DSP(_ptr) (&((struct oss_cdev_client*)dspd_req_userdata(_ptr))->dsp)

static const struct dspd_ioctl_info *get_ioctl_handlers(int request);
static int first_bit(int mask);

static int dspd_chmap_to_oss(const struct dspd_chmap *dspd,
			     unsigned long long *oss);

static int check_policy(struct dsp_data *dsp, unsigned int policy, int32_t *fragsize, int32_t *bufsize);
static void dsp_ioctl(struct oss_cdev_client *cli,
		      int cmd,
		      void *arg,
		      int flags,
		      const void *in_buf,
		      size_t in_bufsz,
		      size_t out_bufsz);
static int32_t reply_buf_cb(struct dspd_rctx *arg, 
			    int32_t flags, 
			    const void *buf, 
			    size_t len)
{
  return oss_reply_ioctl(arg->ops_arg, 0, buf, len);
}
static int32_t reply_err_cb(struct dspd_rctx *arg, 
			    int32_t flags, 
			    int32_t err)
{
  /*
    This is consistent with how the regular dspd io protocol works.  libfuse
    requires an explicit ioctl reply but dspd does not.
   */
  if ( err == 0 )
    {
      return oss_reply_ioctl(arg->ops_arg, 0, NULL, 0);
    } else if ( err < 0 )
    {
      err *= -1;
    }
  return oss_reply_error(arg->ops_arg, err);
}
static struct dspd_rcb dsp_ioctl_rcb = {
  .reply_buf = reply_buf_cb,
  .reply_fd = NULL,
  .reply_err = reply_err_cb,
};

/*static bool check_ioctl(int request,
			const struct dspd_req_handler *handlers,
			int count,
			int ioc_type);*/




static int32_t dsp_commit_params(struct oss_cdev_client *cli)
{
  int32_t ret;
  size_t fr, br;
  struct dspd_cli_params devparams, cliparams;
  size_t maxio;
  struct dspd_fchmap map;
  struct dspd_rclient_swparams swp = { 0 };
  if ( cli->dsp.params_set )
    return 0;
  if ( cli->mode == O_RDONLY )
    {
      devparams = cli->dsp.devinfo.capture;
      maxio = cli->dsp.max_read;
    } else if ( cli->mode == O_WRONLY )
    {
      devparams = cli->dsp.devinfo.playback;
      maxio = cli->dsp.max_write;
    } else
    {
      dspd_fullduplex_parameters(&cli->dsp.devinfo.playback,
				 &cli->dsp.devinfo.capture,
				 &devparams);
      maxio = MIN(cli->dsp.max_write, cli->dsp.max_read);
    }
  assert(cli->dsp.subdivision);
  cli->dsp.params.fragsize /= cli->dsp.subdivision;
  cli->dsp.params.latency = cli->dsp.params.fragsize;


  if ( cli->dsp.params.fragsize > maxio )
    cli->dsp.params.fragsize = maxio;
  
  
  if ( cli->dsp.policy > 0 )
    if ( check_policy(&cli->dsp, cli->dsp.policy, 
		      &cli->dsp.params.fragsize, &cli->dsp.params.bufsize) == 0 )
      cli->dsp.params.latency = cli->dsp.params.fragsize;



  dspd_translate_parameters(&devparams, &cli->dsp.params);


  

  cliparams = cli->dsp.params;

  cli->dsp.frame_bytes = dspd_get_pcm_format_size(cliparams.format) *
    cliparams.channels;



  cliparams.latency /= cli->dsp.frame_bytes;
  cliparams.fragsize /= cli->dsp.frame_bytes;
  cliparams.bufsize /= cli->dsp.frame_bytes;

  swp = *dspd_rclient_get_sw_params(cli->dsp.rclient);

  if ( cli->dsp.low_water )
    {
      fr = cli->dsp.low_water / cli->dsp.frame_bytes;
      if ( fr == 0 )
	fr = 1;
      swp.avail_min = fr;
    } else
    {
      //OSSv4 default is 1 fragment.
      swp.avail_min = cli->dsp.params.latency / cli->dsp.frame_bytes;
    }
  ret = dspd_rclient_set_sw_params(cli->dsp.rclient, &swp);
  if ( ret < 0 )
    return ret * -1;

  struct dspd_rclient_hwparams hwp = { 0 };

  if ( cli->dsp.params.stream & DSPD_PCM_SBIT_PLAYBACK )
    hwp.playback_params = &cliparams;
  if ( cli->dsp.params.stream & DSPD_PCM_SBIT_CAPTURE )
    hwp.capture_params = &cliparams;
  ret = dspd_rclient_set_hw_params(cli->dsp.rclient, &hwp);
  if ( ret < 0 )
    return ret * -1;

  
  
  if ( cli->mode == O_RDONLY )
    cli->dsp.params = *dspd_rclient_get_hw_params(cli->dsp.rclient, DSPD_PCM_SBIT_CAPTURE);
  else if ( cli->mode == O_WRONLY )
    cli->dsp.params = *dspd_rclient_get_hw_params(cli->dsp.rclient, DSPD_PCM_SBIT_PLAYBACK);
  else
    dspd_fullduplex_parameters(&cli->dsp.devinfo.playback,
			       &cli->dsp.devinfo.capture,
			       &cli->dsp.params);

  cli->dsp.params.latency *= cli->dsp.frame_bytes;
  cli->dsp.params.fragsize *= cli->dsp.frame_bytes;
  cli->dsp.params.bufsize *= cli->dsp.frame_bytes;
  cli->dsp.params.xflags = DSPD_CLI_XFLAG_BYTES;

  cli->dsp.params_set = 1;
  cli->dsp.cooked = 1;

  cli->dsp.channelmap = 0;
  if ( (cli->dsp.params.stream & DSPD_PCM_SBIT_FULLDUPLEX) != DSPD_PCM_SBIT_FULLDUPLEX )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    cli->client_index,
			    DSPD_SCTL_CLIENT_GETCHANNELMAP,
			    &cli->dsp.params.stream,
			    sizeof(cli->dsp.params.stream),
			    &map,
			    sizeof(map),
			    &br);
      if ( ret == 0 )
	(void)dspd_chmap_to_oss(&map.map, &cli->dsp.channelmap);
    }
			


  return 0;
  
}

void dsp_read(struct oss_cdev_client *cli, size_t size, off_t off, int flags)
{
  int err = 0, ret;
  size_t offset = 0;
  uint32_t s;
  bool alertable = 1;
  dspd_time_t abstime;
  int32_t latency;
  err = dsp_commit_params(cli);
  if ( err )
    {
      oss_reply_error(cli, err);
      return;
    }
  assert(cli->dsp.trigger);
  flags |= cli->dsp.fflags;

  if ( size > cli->dsp.readlen )
    size = cli->dsp.readlen;

  size = (size / cli->dsp.frame_bytes) * cli->dsp.frame_bytes;
  if ( (cli->dsp.trigger & PCM_ENABLE_INPUT) != 0 &&
       (cli->dsp.started & PCM_ENABLE_INPUT) == 0 )
    {
      s = DSPD_PCM_SBIT_CAPTURE;
      ret = dspd_rclient_ctl(cli->dsp.rclient,
			     DSPD_SCTL_CLIENT_START,
			     &s,
			     sizeof(s),
			     NULL,
			     0,
			     NULL);
      if ( ret != 0 )
	{
	  oss_reply_error(cli, ret);
	  return;
	} else
	{
	  cli->dsp.started |= PCM_ENABLE_INPUT;
	}
    } else if ( (cli->dsp.trigger & PCM_ENABLE_INPUT) == 0 )
    {
      oss_reply_error(cli, EAGAIN);
      return;
    }
  latency = dspd_rclient_get_hw_params(cli->dsp.rclient, DSPD_PCM_SBIT_CAPTURE)->latency;
  while ( offset < size )
    {
      ret = dspd_rclient_read(cli->dsp.rclient,
			      &cli->dsp.readbuf[offset],
			      (size - offset) / cli->dsp.frame_bytes);
      if ( ret < 0 )
	{
	  err = ret;
	  break;
	} else if ( ret > 0 )
	{
	  cli->dsp.capture_read += ret;
	  offset += (ret * cli->dsp.frame_bytes);
	} else
	{

	  if ( (flags & O_NONBLOCK) || (offset > 0 && alertable == 0))
	    break;

	  if ( oss_req_interrupted(cli) )
	    {
	      if ( offset == 0 )
		err = EINTR;
	      break;
	    }

	  ret = dspd_rclient_status(cli->dsp.rclient,
				    DSPD_PCM_SBIT_CAPTURE,
				    NULL);
	  if ( ret < 0 && ret != -EAGAIN )
	    {
	      err = ret;
	      break;
	    }
	  ret = dspd_rclient_get_next_wakeup_avail(cli->dsp.rclient,
						   DSPD_PCM_SBIT_CAPTURE,
						   latency,
						   &abstime);
	  if ( ret < 0 )
	    {
	      err = ret;
	      break;
	    }
	  ret = dspd_cdev_client_sleep(cli, &abstime, alertable);
	  if ( ret != 0 )
	    {
	      if ( ret != ETIMEDOUT && ret != EINPROGRESS )
		{
		  err = ret;
		  break;
		} else if ( ret == EINTR )
		{
		  if ( offset == 0 )
		    err = EINTR;
		  break;
		} else
		{
		  //Need to hurry up but also need to do some work.
		  alertable = 0;
		 
		}
	    }

	}
    }
  if ( offset > 0 )
    {
      oss_reply_buf(cli, cli->dsp.readbuf, offset);
    } else if ( offset == 0 )
    {
      if ( err == 0 )
	err = EAGAIN;
      oss_reply_error(cli, err);
    }
}



void dsp_write(struct oss_cdev_client *cli, 
	       const char *buf, 
	       size_t size, 
	       off_t off, 
	       int flags)
{
  int ret = 0;
  size_t offset = 0;
  dspd_time_t abstime;
  ssize_t fill, avail;
  uint32_t s;
  bool alertable = true;
  const struct dspd_cli_params *hwp;
  ret = dsp_commit_params(cli);
  if ( ret )
    {
      oss_reply_error(cli, ret);
      return;
    }

  if ( ! (cli->dsp.trigger & PCM_ENABLE_OUTPUT) )
    flags |= O_NONBLOCK; //Can't block if trigger bit is not set.
  flags |= cli->dsp.fflags;

  //if ( dspd_dctx.debug && (size % cli->dsp.frame_bytes) )
  //fprintf(stderr, "Got invalid write size\n");

  size = (size / cli->dsp.frame_bytes) * cli->dsp.frame_bytes;

  hwp = dspd_rclient_get_hw_params(cli->dsp.rclient, DSPD_PCM_SBIT_PLAYBACK);
  while ( offset < size )
    {
      ret = dspd_rclient_write(cli->dsp.rclient,
			       &buf[offset],
			       (size - offset) / cli->dsp.frame_bytes);
      if ( ret < 0 )
	break;
      offset += ret * cli->dsp.frame_bytes;
      if ( flags & O_NONBLOCK )
	break;
      if ( ret == 0 )
	{
	  //Buffer is full.  Will need to either sleep or trigger.
	  avail = dspd_rclient_avail(cli->dsp.rclient, DSPD_PCM_SBIT_PLAYBACK);
	  if ( avail < 0 )
	    {
	      ret = avail;
	      break;
	    }
	  fill = cli->dsp.params.bufsize - (avail * cli->dsp.frame_bytes);
	  //OSS starts with at least 2 fragments
	  if ( (cli->dsp.started & PCM_ENABLE_OUTPUT) == 0 && fill >= (cli->dsp.params.fragsize * 2) )
	    {
	      s = DSPD_PCM_SBIT_PLAYBACK;
	      ret = dspd_rclient_ctl(cli->dsp.rclient,
				     DSPD_SCTL_CLIENT_START,
				     &s,
				     sizeof(s),
				     NULL,
				     0,
				     NULL);
	      if ( ret != 0 )
		break;
	      cli->dsp.started |= PCM_ENABLE_OUTPUT;
	      //After this it spins at least once just in case the device has already taken some data.
	    } else if ( offset < size )
	    {
	      //Sleep
	      if ( oss_req_interrupted(cli) )
		{
		  ret = EINTR;
		  break;
		}
	      if ( offset > 0 && alertable == false )
		break; //Can't sleep because the client is trying to do concurrent io.
                       //Finish early so it doesn't block.
	      ret = dspd_rclient_status(cli->dsp.rclient,
					DSPD_PCM_SBIT_PLAYBACK,
					NULL);
	      if ( ret == -EAGAIN )
		{
		  if ( dspd_rclient_get_trigger_tstamp(cli->dsp.rclient, DSPD_PCM_SBIT_PLAYBACK) == 0 )
		    {
		      usleep(1);
		      continue;
		    }
		} else if ( ret < 0 )
		{
		  break;
		}
	      ret = dspd_rclient_get_next_wakeup_avail(cli->dsp.rclient,
						       DSPD_PCM_SBIT_PLAYBACK,
						       hwp->latency,
						       &abstime);
	      if ( ret < 0 )
		break;
   
	      
	      ret = dspd_cdev_client_sleep(cli, &abstime, alertable);
	      if ( ret != 0 )
		{
		  if ( ret != ETIMEDOUT && ret != EINPROGRESS )
		    break;
		  else if ( ret == EINPROGRESS )
		    alertable = false; //Got new request, so hurry up.
		}
	    }
	}
    }


  if ( offset > 0 ) //Wrote something, so ignore errors for now
    oss_reply_write(cli, offset);
  else if ( ret == 0 ) //No write and no error
    oss_reply_error(cli, EAGAIN);
  else //Error (maybe not fatal)
    oss_reply_error(cli, ret);

  //Try to start after sending a reply.  The client may start another write pending at the same time
  //this is occuring which is slightly better for timing.  It is also possible that replying will block
  //in the kernel.  The client won't really be able to know that write is returning before playing actually
  //starts since it can't query the file descriptor until this function returns and the worker thread
  //processes another command.
  if ( (cli->dsp.trigger & PCM_ENABLE_OUTPUT) != 0 && (cli->dsp.started & PCM_ENABLE_OUTPUT) == 0 && offset > 0 )
    {
      avail = dspd_rclient_avail(cli->dsp.rclient, DSPD_PCM_SBIT_PLAYBACK);
      if ( avail >= 0 )
	{
	  fill = cli->dsp.params.bufsize - (avail * cli->dsp.frame_bytes);
	  if ( fill >= (cli->dsp.params.fragsize * 2) )
	    {
	      s = DSPD_PCM_SBIT_PLAYBACK;
	      ret = dspd_rclient_ctl(cli->dsp.rclient,
				     DSPD_SCTL_CLIENT_START,
				     &s,
				     sizeof(s),
				     NULL,
				     0,
				     NULL);
	      if ( ret )
		cli->error = ret;
	    } else if ( fill < 0 )
	    {
	      cli->error = ret;
	    }
	}
    }
}





static void dsp_poll(struct oss_cdev_client *cli, uint64_t ph)
{
  uint32_t revents;
  cli->pollhandle = ph;
  cli->poll_ok = 1;
  cli->poll_armed = 1;
  revents = dsp_check_revents(cli);
  oss_reply_poll(cli, revents);
}

static void dsp_release(struct oss_cdev_client *cli)
{
  uint32_t s;
  int32_t err;
  if ( (cli->dsp.params.stream & DSPD_PCM_SBIT_PLAYBACK) != 0 &&
       (cli->error == 0) && cli->dsp.params_set )
    {
      s = DSPD_PCM_SBIT_PLAYBACK;
      err = dspd_rclient_ctl(cli->dsp.rclient,
			     DSPD_SCTL_CLIENT_START,
			     &s,
			     sizeof(s),
			     NULL,
			     0,
			     NULL);
      if ( err == 0 )
	dspd_rclient_drain(cli->dsp.rclient);
    }
  oss_reply_error(cli, 0);
}

const struct oss_cdev_ops osscuse_dsp_ops = {
  .write = dsp_write,
  .read = dsp_read,
  .ioctl = dsp_ioctl,
  .poll = dsp_poll,
  .release = dsp_release,
};

/*
  Some future extension might use this code to implement osslib so it is better
  to handle it like this instead of processing ioctls directly.  It should be noted
  that if a future OSS version ever requires unrestricted ioctls then those ioctls
  will not ever be supported.  There is also no support for ioctls that return
  something other than an error code.  OSS does not need this since it either
  puts a response into the callers buffer or returns an error.


*/
#define _REQ_INBUF(_req)((_IOC_DIR(_req))&_IOC_WRITE)==_IOC_WRITE?(_IOC_SIZE(_req)):0
#define _REQ_OUTBUF(_req) ((_IOC_DIR(_req))&_IOC_READ)==_IOC_READ?(_IOC_SIZE(_req)):0
//#define IOCTL_HANDLER(_req,_handler) [_IOC_NR(_req)+1]={.handler=_handler,.xflags=DSPD_REQ_FLAG_CMSG_FD,.rflags=0,.inbufsize=_REQ_INBUF(_req),.outbufsize=_REQ_OUTBUF(_req),.data=_req}
//#define IOCTL_HANDLER2(_wreq,_rreq,_handler) [_IOC_NR(_wreq)+1]={.handler=_handler,.xflags=DSPD_REQ_FLAG_CMSG_FD,.rflags=0,.inbufsize=_REQ_INBUF(_wreq),.outbufsize=_REQ_OUTBUF(_rreq)}


#define IOCTL_HANDLER(_req,_handler) {.handler=_handler,.xflags=DSPD_REQ_FLAG_CMSG_FD,.rflags=0,.inbufsize=_REQ_INBUF(_req),.outbufsize=_REQ_OUTBUF(_req),.data=_req}
#define IOCTL_HANDLER2(_wreq,_rreq,_handler) IOCTL_HANDLER(_wreq,_handler),IOCTL_HANDLER(_rreq,_handler)


static const int format_list[] = {
  AFMT_A_LAW, DSPD_PCM_FORMAT_A_LAW,
  AFMT_MU_LAW, DSPD_PCM_FORMAT_MU_LAW,
  AFMT_U8, DSPD_PCM_FORMAT_U8,
  AFMT_S16_LE, DSPD_PCM_FORMAT_S16_LE,
  AFMT_S16_BE, DSPD_PCM_FORMAT_S16_BE,
  AFMT_S8, DSPD_PCM_FORMAT_S8,
  AFMT_U16_LE, DSPD_PCM_FORMAT_U16_LE,
  AFMT_U16_BE, DSPD_PCM_FORMAT_U16_BE,
  AFMT_S32_LE, DSPD_PCM_FORMAT_S32_LE,
  AFMT_S32_BE, DSPD_PCM_FORMAT_S32_LE,
  AFMT_S24_LE, DSPD_PCM_FORMAT_S24_LE,
  AFMT_S24_BE, DSPD_PCM_FORMAT_S24_BE,
  AFMT_FLOAT, DSPD_PCM_FORMAT_FLOAT64_NE,
  AFMT_S24_PACKED, DSPD_PCM_FORMAT_S24_3LE,
};

static int32_t dsp_req_getfmts(struct dspd_rctx *context,
			       uint32_t req,
			       const void   *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize)
{
  int fmts = 0;
  size_t i;
  for ( i = 0; i < ARRAY_SIZE(format_list); i += 2 )
    {
      if ( dspd_getconv(format_list[i+1]) )
	fmts |= format_list[i];
    }
  return dspd_req_reply_buf(context, 0, &fmts, sizeof(fmts));
}

static int32_t dsp_req_setfmt(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			 void         *outbuf,
			      size_t        outbufsize)
{
  int fmt = *(int32_t*)inbuf, f = AFMT_U8;
  size_t i;
  struct dsp_data *dsp = REQ_DSP(context);
  if ( dsp->params_set == false && fmt != AFMT_QUERY )
    {
      for ( i = 0; i < ARRAY_SIZE(format_list); i += 2 )
	{
	  if ( fmt == format_list[i] && dspd_getconv(format_list[i+1]) != NULL )
	    {
	      dsp->params.format = format_list[i+1];
	      f = fmt;
	      break;
	    }
	}
    } else
    {
      for ( i = 0; i < ARRAY_SIZE(format_list); i += 2 )
	{
	  if ( format_list[i+1] == dsp->params.format )
	    {
	      f = format_list[i];
	      break;
	    }
	}
    }
  return dspd_req_reply_buf(context, 0, &f, sizeof(f));
}
static int32_t dsp_req_getblksize(struct dspd_rctx *context,
				  uint32_t req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  int32_t err;
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  int len;
  //The ioctl definition is WR (write+read).  The 4Front documentation says it only
  //reports the size.  Actual implementations do the same.
  if ( ! cli->dsp.params_set )
    {
      err = dsp_commit_params(cli);
      if ( err )
	return dspd_req_reply_err(context, 0, err);
    }
  len = cli->dsp.params.fragsize;
  return dspd_req_reply_buf(context, 0, &len, sizeof(len));
}

static int32_t dsp_req_channels(struct dspd_rctx *context,
				uint32_t req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  struct dsp_data *dsp = &cli->dsp;
  int maxchan;
  struct dspd_fchmap map;
  int32_t ret;
  size_t br;
  if ( ! dsp->params_set )
    {
      dsp->params.channels = *(int*)inbuf;
      if ( dsp->params.stream == DSPD_PCM_SBIT_PLAYBACK )
	{
	  maxchan = dsp->devinfo.playback.channels;
	} else if ( dsp->params.stream == DSPD_PCM_SBIT_CAPTURE )
	{
	  maxchan = dsp->devinfo.capture.channels;
	} else
	{
	  maxchan = MIN(dsp->devinfo.playback.channels, dsp->devinfo.capture.channels);
	}
      if ( dsp->params.channels > maxchan )
	dsp->params.channels = maxchan;
      cli->dsp.channelmap = CHNORDER_UNDEF;
      if ( (dsp->params.stream & DSPD_PCM_SBIT_FULLDUPLEX) != DSPD_PCM_SBIT_FULLDUPLEX )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				cli->client_index,
				DSPD_SCTL_CLIENT_GETCHANNELMAP,
				&dsp->params.stream,
				sizeof(dsp->params.stream),
				&map,
				sizeof(map),
				&br);
	  if ( ret == 0 )
	    {
	      //If the channel map doesn't match then it must be reset
	      if ( map.map.channels != dsp->params.channels )
		{
		  //Request the default channel map
		  map.map.channels = 0;
		  ret = dspd_stream_ctl(&dspd_dctx,
					cli->client_index,
					DSPD_SCTL_CLIENT_SETCHANNELMAP,
					&map.map,
					dspd_chmap_sizeof(&map.map),
					NULL,
					0,
					&br);
		  if ( ret == 0 )
		    {
		      //Try again
		      ret = dspd_stream_ctl(&dspd_dctx,
					    cli->client_index,
					    DSPD_SCTL_CLIENT_GETCHANNELMAP,
					    &dsp->params.stream,
					    sizeof(dsp->params.stream),
					    &map,
					    sizeof(map),
					    &br);
		      //If the channels aren't known to OSS (very unlikely) then it will be undefined.
		      if ( ret == 0 )
			dspd_chmap_to_oss(&map.map, &dsp->channelmap);
		    }
		}
	    }
	}
      dsp->cooked = 1;
    }
  return dspd_req_reply_buf(context, 0, &dsp->params.channels, sizeof(dsp->params.channels));
}

static int32_t dsp_req_setfragment(struct dspd_rctx *context,
				   uint32_t req,
				   const void   *inbuf,
				   size_t        inbufsize,
				   void         *outbuf,
				   size_t        outbufsize)
{
  uint32_t frag = *(unsigned int*)inbuf;
  struct dsp_data *dsp = REQ_DSP(context);
  int32_t ret;
  if ( dsp->params_set )
    {
      //Parameters already set.  Return EINVAL just like OSS/Linux
      ret = dspd_req_reply_err(context, 0, EINVAL);
    } else
    {
      /*
	This is just a hint according to 4Front and actual implementations.  The
	ALSA OSS plugin (pcm_oss) doesn't handle this correctly.
       */
      dsp->params.fragsize = 1 << (frag & 0xFFFF);
      dsp->params.bufsize = dsp->params.fragsize * (frag >> 16);
      dsp->policy = -1;
      ret = dspd_req_reply_buf(context, 0, &frag, sizeof(frag));
    }
  return ret;
}


static int32_t dsp_req_getospace(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  int32_t ret;
  struct audio_buf_info *ai = outbuf;
  if ( ! (dsp->params.stream & DSPD_PCM_SBIT_PLAYBACK) )
    return dspd_req_reply_err(context, 0, EINVAL);
  if ( ! dsp->params_set )
    {
      ret = dsp_commit_params(context->ops_arg);
      if ( ret )
	return dspd_req_reply_err(context, 0, ret);
    }
  ai->fragstotal = dsp->params.bufsize / dsp->params.fragsize;
  ai->fragsize = dsp->params.fragsize;
  ai->bytes = dspd_rclient_avail(dsp->rclient, DSPD_PCM_SBIT_PLAYBACK) * dsp->frame_bytes;
  //This isn't completely correct.  If more than maxwrite is written then the write will be short
  //whether it blocks or not.
  if ( ai->bytes > dsp->max_write )
    ai->bytes = dsp->max_write;
  ai->fragments = ai->bytes / dsp->params.fragsize;
  return dspd_req_reply_buf(context, 0, ai, sizeof(*ai));
}

static int32_t dsp_req_getispace(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  int32_t ret;
  struct audio_buf_info *ai = outbuf;
  if ( ! (dsp->params.stream & DSPD_PCM_SBIT_CAPTURE) )
    return dspd_req_reply_err(context, 0, EINVAL);
  if ( ! dsp->params_set )
    {
      ret = dsp_commit_params(context->ops_arg);
      if ( ret )
	return dspd_req_reply_err(context, 0, ret);
    }
  ai->fragstotal = dsp->params.bufsize / dsp->params.fragsize;
  ai->fragsize = dsp->params.fragsize;
  ai->bytes = dspd_rclient_avail(dsp->rclient, DSPD_PCM_SBIT_CAPTURE) * dsp->frame_bytes;
  if ( ai->bytes > dsp->max_read )
    ai->bytes = dsp->max_read;
  ai->fragments = ai->bytes / dsp->params.fragsize;
  return dspd_req_reply_buf(context, 0, ai, sizeof(*ai));
  
}

static int32_t dsp_req_speed(struct dspd_rctx *context,
			     uint32_t req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  int rate = *(int*)inbuf;
  if ( dsp->params_set == false && dsp->force_raw == false )
    {
      dsp->params.rate = rate;
      dsp->cooked = 1;
    } else
    { 
      rate = dsp->params.rate;
    }
  return dspd_req_reply_buf(context, 0, &rate, sizeof(rate));
}

static int check_policy(struct dsp_data *dsp, unsigned int policy, int32_t *fragsize, int32_t *bufsize)
{
  static const int policies_ms[10] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
  struct dspd_cli_params params;
  int64_t l, pval = policies_ms[policy] * 1000000, n, f;
  int frags;
  if ( dsp->params.stream == DSPD_PCM_SBIT_CAPTURE )
    params = dsp->devinfo.capture;
  else if ( dsp->params.stream == DSPD_PCM_SBIT_PLAYBACK )
    params = dsp->devinfo.playback;
  else
    dspd_fullduplex_parameters(&dsp->devinfo.playback,
			       &dsp->devinfo.capture,
			       &params);
  if ( dsp->params.stream & DSPD_PCM_SBIT_CAPTURE )
    frags = 8;
  else
    frags = 4;
  l = pval / (1000000000/params.rate);
  n = 1 << get_lpo2(l);
  if ( n < params.min_latency )
    {
      n = 1 << get_hpo2(l);
      if ( n < params.min_latency )
	return EIO;
    }
  f = dspd_src_get_frame_count(params.rate, dsp->params.rate, n);
  f *= dsp->params.channels * dspd_get_pcm_format_size(dsp->params.format);
  if ( fragsize )
    *fragsize = f;
  if ( bufsize )
    *bufsize = f * frags;
  
  return 0;
}


static int32_t dsp_req_policy(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  int policy = *(int*)inbuf;
  int err;
  /*
    The specs are vague about what this does.  With a lot of real hardware it is possible to do
    latencies of around 1ms (as seen with mod_alsadrv).  That won't work here.  We need around
    1ms for mixing and at least 2ms for the client buffer.  With libfuse, even with gratuitous mallocs and
    lots of indirection, the limit seems to be around 3 0.6ms fragments on my machine.  I doubt that any less than
    about 3-4ms is going to be consistently possible with CUSE.
  */
  if ( policy < 1 || policy > 10 )
    {
      err = EIO;
    } else
    {
      err = check_policy(dsp, policy, NULL, NULL);
      if ( err == 0 )
	dsp->policy = policy; //Need to save this for later
    }
  return dspd_req_reply_err(context, 0, err);
}

static int32_t dsp_req_getcaps(struct dspd_rctx *context,
			       uint32_t req,
			       const void   *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize)
{
  /*
    The PCM_CAP_REALTIME bit is reported because 4Front says applications misuse it
    and may refuse to run.
  */
  int cap = PCM_CAP_BATCH | PCM_CAP_VIRTUAL | PCM_CAP_REALTIME;
  struct dsp_data *dsp = REQ_DSP(context);
  if ( dsp->devinfo.streams == (DSPD_PCM_SBIT_CAPTURE|DSPD_PCM_SBIT_PLAYBACK) )
    cap |= PCM_CAP_DUPLEX;
  cap |= PCM_CAP_TRIGGER;
  cap |= PCM_CAP_MULTI;
  if ( dsp->devinfo.streams & DSPD_PCM_SBIT_PLAYBACK )
    cap |= PCM_CAP_OUTPUT;
  if ( dsp->devinfo.streams & DSPD_PCM_SBIT_CAPTURE )
    cap |= PCM_CAP_INPUT;
  return dspd_req_reply_buf(context, 0, &cap, sizeof(cap));
}

static int32_t dsp_req_settrigger(struct dspd_rctx *context,
				  uint32_t req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  int trigger = *(int*)inbuf;
  uint32_t t;
  int32_t ret;
  size_t br;
  if ( trigger != cli->dsp.trigger )
    {
      t = 0;
      if ( trigger & PCM_ENABLE_INPUT )
	t |= DSPD_PCM_SBIT_CAPTURE;
      if ( trigger & PCM_ENABLE_OUTPUT )
	t |= DSPD_PCM_SBIT_PLAYBACK;
      ret = dspd_rclient_ctl(cli->dsp.rclient,
			     DSPD_SCTL_CLIENT_SETTRIGGER,
			     &t,
			     sizeof(t),
			     NULL,
			     0,
			     &br);
    } else
    {
      ret = 0;
    }
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t dsp_req_gettrigger(struct dspd_rctx *context,
				  uint32_t req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  return dspd_req_reply_buf(context, 0, &cli->dsp.trigger, sizeof(cli->dsp.trigger));
}



static int32_t dsp_req_sync(struct dspd_rctx *context,
			    uint32_t req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  int32_t err = 0;
  struct dsp_data *dsp = REQ_DSP(context);
  uint32_t s;
 
  if ( dsp->params.stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      s = DSPD_PCM_SBIT_PLAYBACK;
      err = dspd_rclient_ctl(dsp->rclient,
			     DSPD_SCTL_CLIENT_START,
			     &s,
			     sizeof(s),
			     NULL,
			     0,
			     NULL);
      if ( err == 0 )
	err = dspd_rclient_drain(dsp->rclient);
    }
  return dspd_req_reply_err(context, 0, err);
}

static int32_t dsp_req_reset(struct dspd_rctx *context,
			     uint32_t req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  if ( dsp->params_set )
    {
      //      fprintf(stderr, "RESET\n");
      dspd_rclient_disconnect(dsp->rclient, true);
      dsp->started = 0;
      if ( dsp->params.stream & DSPD_PCM_SBIT_PLAYBACK )
	dsp->trigger |= PCM_ENABLE_OUTPUT;
      if ( dsp->params.stream & DSPD_PCM_SBIT_CAPTURE )
	dsp->trigger |= PCM_ENABLE_INPUT;
      dsp->playback_written = 0;
      dsp->capture_read = 0;
      dsp->low_water = 0;
      dsp->cooked = 0;
      dsp->params_set = 0;
    }
  return dspd_req_reply_err(context, 0, 0);
}

static int32_t dsp_req_halt_output(struct dspd_rctx *context,
				   uint32_t req,
				   const void   *inbuf,
				   size_t        inbufsize,
				   void         *outbuf,
				   size_t        outbufsize)
{
  int32_t err, s;
  struct dsp_data *dsp = REQ_DSP(context);
  if ( dsp->params.stream == DSPD_PCM_SBIT_FULLDUPLEX )
    {
      /*
	Not sure if this should allow setting playback differently than
	capture.  If so, then it is write only because SNDCTL_DSP_CHANNELS
	and some others don't allow specifying which direction.
      */
      s = DSPD_PCM_SBIT_PLAYBACK;
      err = dspd_rclient_ctl(dsp->rclient,
			     DSPD_SCTL_CLIENT_STOP,
			     &s,
			     sizeof(s),
			     NULL,
			     0,
			     NULL);
      if ( err == 0 )
	{
	  dsp->playback_written = 0;
	  dsp->started &= ~PCM_ENABLE_OUTPUT;
	  dsp->trigger |= PCM_ENABLE_OUTPUT;
	} else
	{
	  err *= -1;
	}
    } else if ( dsp->params.stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      //Can completely reset
      return dsp_req_reset(context, req, inbuf, inbufsize, outbuf, outbufsize);
    } else
    {
      err = EINVAL;
    }
  return dspd_req_reply_err(context, 0, err);
}

static int32_t dsp_req_halt_input(struct dspd_rctx *context,
				  uint32_t req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  int32_t err, s;
  struct dsp_data *dsp = REQ_DSP(context);
  if ( dsp->params.stream == DSPD_PCM_SBIT_FULLDUPLEX )
    {
      /*
	Not sure if this should allow setting playback differently than
	capture.  If so, then it is write only because SNDCTL_DSP_CHANNELS
	and some others don't allow specifying which direction.
      */
      s = DSPD_PCM_SBIT_CAPTURE;
      err = dspd_rclient_ctl(dsp->rclient,
			     DSPD_SCTL_CLIENT_STOP,
			     &s,
			     sizeof(s),
			     NULL,
			     0,
			     NULL);
      if ( err == 0 )
	{
	  dsp->capture_read = 0;
	  dsp->started &= ~PCM_ENABLE_INPUT;
	  dsp->trigger |= PCM_ENABLE_INPUT;
	} else
	{
	  err *= -1;
	}
    } else if ( dsp->params.stream == DSPD_PCM_SBIT_CAPTURE )
    {
      //Can completely reset
      return dsp_req_reset(context, req, inbuf, inbufsize, outbuf, outbufsize);
    } else
    {
      err = EINVAL;
    }
  return dspd_req_reply_err(context, 0, err);
}

static int32_t dsp_req_nonblock(struct dspd_rctx *context,
				uint32_t req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  dsp->fflags |= O_NONBLOCK;
  return dspd_req_reply_err(context, 0, 0);
}





static int32_t dsp_req_stereo(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  bool stereo = *(int*)inbuf;
  int32_t chan;
  if ( dsp->params_set == false )
    {
      if ( dsp->params.stream & DSPD_PCM_SBIT_PLAYBACK )
	chan = dsp->devinfo.playback.channels;
      else
	chan = INT32_MAX;
      if ( dsp->params.stream & DSPD_PCM_SBIT_CAPTURE )
	{
	  if ( chan > dsp->devinfo.capture.channels )
	    chan = dsp->devinfo.capture.channels;
	}
      if ( chan == INT32_MAX )
	return dspd_req_reply_err(context, 0, EINVAL);
      if ( stereo )
	{
	  if ( chan < 0 )
	    stereo = 0;
	  else
	    dsp->params.channels = 2;
	} else
	{
	  dsp->params.channels = 1;
	}
      if ( ! dsp->force_raw )
	dsp->cooked = true;
    } else
    {
      //Return current mode
      stereo = dsp->params.channels == 2;
    }
  return dspd_req_reply_buf(context, 0, &stereo, sizeof(stereo));
}

static int32_t dsp_req_post(struct dspd_rctx *context,
			    uint32_t req,
			    const void   *inbuf,
			    size_t        inbufsize,
			    void         *outbuf,
			    size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  int32_t err, s;
  if ( dsp->params.stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( ! dsp->params_set )
	err = dsp_commit_params(context->ops_arg); //FIXME: Should not use this directly.
      else
	err = 0;
      if ( err == 0 )
	{
	  s = DSPD_PCM_SBIT_PLAYBACK;
	  err = dspd_rclient_ctl(dsp->rclient,
				 DSPD_SCTL_CLIENT_START,
				 &s,
				 sizeof(s),
				 NULL,
				 0,
				 NULL);
	}
    } else
    {
      err = EINVAL;
    }
  return dspd_req_reply_err(context, 0, err);
}

static int32_t dsp_req_subdivide(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  int32_t err, val;
  int sd = *(int*)inbuf;
  if ( dsp->params.stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( ! dsp->params_set )
	{
	  if ( sd <= 0 )
	    sd = 1;
	  else if ( sd > 4 )
	    sd = 4;
	  val = dsp->params.fragsize / sd;
	  if ( val == 0 )
	    {
	      sd = 1;
	    } else
	    {
	      dsp->subdivision = sd;
	    }
	  return dspd_req_reply_buf(context, 0, &sd, sizeof(sd));
	} else
	{
	  err = EINVAL; //This is what linux uses.  Why not EBUSY?
	}
    } else
    {
      err = EINVAL;
    }
  return dspd_req_reply_err(context, 0, err);
}



static int32_t dsp_req_getoptr(struct dspd_rctx *context,
			       uint32_t req,
			       const void   *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  count_info *ci = outbuf;
  int32_t ret;
  uint32_t p;
  if ( cli->dsp.params.flags & DSPD_PCM_SBIT_PLAYBACK )
    {
      memset(ci, 0, sizeof(*ci));
      if ( cli->dsp.params_set )
	{
	  ret = dspd_rclient_get_hw_ptr(cli->dsp.rclient,
					DSPD_PCM_SBIT_PLAYBACK,
					&p);
	  if ( ret == 0 )
	    {
	      ci->bytes = p * cli->dsp.frame_bytes;
	      ci->blocks = ci->bytes / cli->dsp.params.fragsize;
	      ci->ptr = ci->bytes % cli->dsp.params.bufsize;
	    } else
	    {
	      return dspd_req_reply_err(context, 0, ret);
	    }
	}
      ret = dspd_req_reply_buf(context, 0, ci, sizeof(*ci));
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}

static int32_t dsp_req_getiptr(struct dspd_rctx *context,
			       uint32_t req,
			       const void   *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  count_info *ci = outbuf;
  int32_t ret;
  uint32_t p;
  if ( cli->dsp.params.flags & DSPD_PCM_SBIT_CAPTURE )
    {
      memset(ci, 0, sizeof(*ci));
      if ( cli->dsp.params_set )
	{
	  ret = dspd_rclient_get_hw_ptr(cli->dsp.rclient,
					DSPD_PCM_SBIT_CAPTURE,
					&p);
	  if ( ret == 0 )
	    {
	      ci->bytes = p * cli->dsp.frame_bytes;
	      ci->blocks = ci->bytes / cli->dsp.params.fragsize;
	      ci->ptr = ci->bytes % cli->dsp.params.bufsize;
	    } else
	    {
	      return dspd_req_reply_err(context, 0, ret);
	    }
	}
      ret = dspd_req_reply_buf(context, 0, ci, sizeof(*ci));
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}


static int32_t dsp_req_get_current_ptr(struct dspd_rctx *context,
				       uint32_t req,
				       const void   *inbuf,
				       size_t        inbufsize,
				       void         *outbuf,
				       size_t        outbufsize)
{
  /*
   
    time=(ptr.samples+ptr.fifo_samples) * usecs_per_sample

    This returns frames, not samples.  Each "sample" is 1 or 
    more samples for however many channels are being used.

    oss_count_t consists of:

    fifo_samples:  Number of FRAMES on the device (extra delay)
    samples:       Hardware pointer (frames) 
    filler:        Unused.  Should be zero.
    
   */
  oss_count_t oc;
  struct dsp_data *dsp = REQ_DSP(context);
  uint32_t s, n, fill;
  struct dspd_pcmcli_status status;
  int32_t ret;
  const struct dspd_cli_params *hwp = dspd_rclient_get_hw_params(dsp->rclient, DSPD_PCM_SBIT_PLAYBACK);
  memset(&oc, 0, sizeof(oc));
  if ( req == SNDCTL_DSP_CURRENT_IPTR )
    s = DSPD_PCM_SBIT_CAPTURE;
  else
    s = DSPD_PCM_SBIT_PLAYBACK;

  if ( dsp->params_set && (dsp->params.stream & s) )
    {
      ret = dspd_rclient_status(dsp->rclient, s, &status);
      if ( ret == 0 )
	{
	  if ( s == DSPD_PCM_SBIT_PLAYBACK )
	    fill = hwp->bufsize - status.avail;
	  else
	    fill = status.avail;
	  if ( status.delay < fill )
	    oc.fifo_samples = 0;
	  else
	    oc.fifo_samples = status.delay - fill;
	} else if ( ret != -EAGAIN )
	{
	  return dspd_req_reply_err(context, 0, ret);
	}
      if ( s == DSPD_PCM_SBIT_PLAYBACK )
	{
	  n = hwp->bufsize - dspd_rclient_avail(dsp->rclient, s);
	  oc.samples = dsp->playback_written - n;
	} else
	{
	  n = dspd_rclient_avail(dsp->rclient, s);
	  oc.samples = dsp->capture_read + n;
	}
    }
  return dspd_req_reply_buf(context, 0, &oc, sizeof(oc));
}

static int32_t dsp_req_low_water(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  int bytes = *(int*)inbuf;
  int err;
  size_t fr;
  struct dspd_rclient_swparams swp;
  if ( bytes > 0 )
    {
      err = 0;
      dsp->low_water = bytes;
      if ( dsp->params_set )
	{
	  fr = dsp->low_water / dsp->frame_bytes;
	  if ( fr == 0 )
	    fr = 1;
	  swp = *dspd_rclient_get_sw_params(dsp->rclient);
	  swp.avail_min = fr;
	  err = dspd_rclient_set_sw_params(dsp->rclient, &swp);
	}
    } else
    {
      err = EINVAL;
    }
  return dspd_req_reply_err(context, 0, err);
}

static int32_t dsp_req_cookedmode(struct dspd_rctx *context,
				  uint32_t req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  struct dsp_data *dsp = REQ_DSP(context);
  int cm = *(int*)inbuf;
  if ( dsp->params_set == false && dsp->cooked == false )
    {
      if ( ! cm )
	{
	  dsp->params.xflags &= ~DSPD_CLI_XFLAG_COOKEDMODE;
	  dsp->force_raw = false;
	} else
	{
	  dsp->params.xflags |= DSPD_CLI_XFLAG_COOKEDMODE;
	  dsp->force_raw = true;
	}
    }
  return dspd_req_reply_err(context, 0, 0);
}

static int32_t dsp_req_getodelay(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct dspd_pcmcli_status status;
  struct dsp_data *dsp = REQ_DSP(context);
  int odelay, ret;
  if ( dsp->params.stream & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( ! dsp->params_set )
	{
	  ret = dsp_commit_params(context->ops_arg);
	  if ( ret )
	    return dspd_req_reply_err(context, 0, ret);
	}

      ret = dspd_rclient_status(dsp->rclient, dsp->params.stream, &status);
      if ( ret == 0 )
	{
	  odelay = status.delay * dsp->frame_bytes;
	} else if ( ret == -EAGAIN )
	{
	  ret = dspd_rclient_avail(dsp->rclient, DSPD_PCM_SBIT_PLAYBACK);
	  if ( ret >= 0 )
	    {
	      odelay = ret * dsp->frame_bytes;
	      ret = 0;
	    }
	}
     
      if ( ret )
	ret = dspd_req_reply_err(context, 0, ret);
      else
	ret = dspd_req_reply_buf(context, 0, &odelay, sizeof(odelay));
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}

static int32_t dsp_req_profile(struct dspd_rctx *context,
			       uint32_t req,
			       const void   *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  cli->dsp.profile = *(int*)inbuf;
  return dspd_req_reply_err(context, 0, 0);
}



static int32_t dsp_req_set_volume(struct dspd_rctx *context,
				  int32_t stream,
				  uint32_t req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  
  struct dspd_stream_volume sv;
  int v;
  float vol;
  size_t br;
  int32_t ret;
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  if ( cli->dsp.params.flags & stream )
    {
      v = *(int*)inbuf;
      memset(&sv, 0, sizeof(sv));
      if ( v > 100 )
	v = 100;
      else if ( v < 0 )
	v = 0;
      vol = v;
      vol /= 100.0;
      sv.stream = stream;
      sv.volume = vol;
      ret = dspd_rclient_ctl(cli->dsp.rclient,
			     DSPD_SCTL_CLIENT_SETVOLUME,
			     &sv,
			     sizeof(sv),
			     NULL,
			     0,
			     &br);
    } else
    {
      ret = EINVAL;
    }
  return dspd_req_reply_err(context, 0, ret);
}
static int32_t dsp_req_get_volume(struct dspd_rctx *context,
				  int32_t stream,
				  uint32_t req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  float vol;
  int32_t ret;
  size_t br;
  int v, result;
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  if ( cli->dsp.params.flags & stream )
    {
      ret = dspd_rclient_ctl(cli->dsp.rclient,
			     DSPD_SCTL_CLIENT_GETVOLUME,
			     &stream,
			     sizeof(stream),
			     &vol,
			     sizeof(vol),
			     &br);
      if ( ret == 0 )
	{
	  v = lrint(100.0 * vol);
	  result = v | (v << 8);
	  ret = dspd_req_reply_buf(context, 0, &result, sizeof(result));
	} else
	{
	  ret = dspd_req_reply_err(context, 0, ret);
	}
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}

static int32_t dsp_req_setrecvol(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  return dsp_req_set_volume(context,
			    DSPD_PCM_SBIT_CAPTURE,
			    req,
			    inbuf,
			    inbufsize,
			    outbuf,
			    outbufsize);
}

static int32_t dsp_req_getrecvol(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  return dsp_req_get_volume(context,
			    DSPD_PCM_SBIT_CAPTURE,
			    req,
			    inbuf,
			    inbufsize,
			    outbuf,
			    outbufsize);
}


static int32_t dsp_req_setplayvol(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  return dsp_req_set_volume(context,
			    DSPD_PCM_SBIT_PLAYBACK,
			    req,
			    inbuf,
			    inbufsize,
			    outbuf,
			    outbufsize);
}

static int32_t dsp_req_getplayvol(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  return dsp_req_get_volume(context,
			    DSPD_PCM_SBIT_PLAYBACK,
			    req,
			    inbuf,
			    inbufsize,
			    outbuf,
			    outbufsize);
}

static int32_t oss_dspd_map[] = {
  CHID_UNDEF, DSPD_CHMAP_UNKNOWN,
  CHID_L, DSPD_CHMAP_FL,
  CHID_R, DSPD_CHMAP_FR,
  CHID_C, DSPD_CHMAP_FC,
  CHID_LFE, DSPD_CHMAP_LFE,
  CHID_LS, DSPD_CHMAP_SL,
  CHID_RS, DSPD_CHMAP_SR,
  CHID_LR, DSPD_CHMAP_RL,
  CHID_RR, DSPD_CHMAP_RR,
};


static int32_t find_oss_channel(uint8_t channel)
{
  size_t i;
  int32_t ret = -1;
  for ( i = 0; i < ARRAY_SIZE(oss_dspd_map); i += 2 )
    {
      if ( channel == oss_dspd_map[i+1] )
	{
	  ret = oss_dspd_map[i];
	  break;
	}
    }
  return ret;
}

static int32_t find_dspd_channel(uint8_t channel)
{
  size_t i;
  int32_t ret = -1;
  for ( i = 0; i < ARRAY_SIZE(oss_dspd_map); i += 2 )
    {
      if ( channel == oss_dspd_map[i] )
	{
	  ret = oss_dspd_map[i+1];
	  break;
	}
    }
  return ret;
}


static int dspd_chmap_to_oss(const struct dspd_chmap *dspd,
			     unsigned long long *oss)
{
  size_t i;
  int32_t ch;
  uint32_t count = 0;
  unsigned long long val;
  *oss = 0ULL;
  for ( i = 0; i < dspd->channels; i++ )
    {
      ch = find_oss_channel(dspd->pos[i]);
      if ( ch > CHID_UNDEF )
	{
	  val = ch;
	  val <<= (count * 4U);
	  (*oss) |= val;
	  count++;
	}
    }
  return count;
}


static int oss_chmap_to_dspd(unsigned long long oss,
			     struct dspd_chmap *dspd)
{
  size_t i, count = 0;
  uint32_t val;
  for ( i = 0; i < 8; i++ )
    {
      val = (oss >> (i * 4U)) & 0xFU;
      if ( val != CHID_UNDEF )
	{
	  dspd->pos[count] = find_dspd_channel(val);
	  count++;
	}
    }
  dspd->channels = count;
  return count;
}



static int32_t dsp_req_get_chnorder(struct dspd_rctx *context,
				    uint32_t req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  unsigned long long result = 0;
  struct dspd_fchmap chmap;
  size_t br;
  int32_t ret = 0;
  memset(&chmap, 0, sizeof(chmap));
  if ( cli->dsp.channelmap == CHNORDER_UNDEF && (cli->dsp.params.stream & DSPD_PCM_SBIT_FULLDUPLEX) != DSPD_PCM_SBIT_FULLDUPLEX &&
       cli->dsp.params_set == false )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    cli->device_index,
			    DSPD_SCTL_SERVER_GETCHANNELMAP,
			    &cli->dsp.params.stream,
			    sizeof(cli->dsp.params.stream),
			    &chmap,
			    sizeof(chmap),
			    &br);
      if ( ret == 0 )
	{
	  (void)dspd_chmap_to_oss(&chmap.map, &result);
	  ret = dspd_req_reply_buf(context, 0, &result, sizeof(result));
	} else
	{
	  ret = dspd_req_reply_err(context, 0, ret);
	}
    } else
    {
      ret = dspd_req_reply_buf(context, 0, &cli->dsp.channelmap, sizeof(cli->dsp.channelmap));
    }
  return ret;
  
}
static int32_t dsp_req_set_chnorder(struct dspd_rctx *context,
				    uint32_t req,
				    const void   *inbuf,
				    size_t        inbufsize,
				    void         *outbuf,
				    size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  unsigned long long ossmap = *(unsigned long long*)inbuf;
  struct dspd_fchmap dspdmap;
  int32_t ret;
  size_t br;
  if ( (cli->dsp.params.stream & DSPD_PCM_SBIT_FULLDUPLEX) != DSPD_PCM_SBIT_FULLDUPLEX )
    {

      memset(&dspdmap, 0, sizeof(dspdmap));
      ret = oss_chmap_to_dspd(ossmap, &dspdmap.map);
      if ( ret > 0 )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				cli->client_index,
				DSPD_SCTL_CLIENT_SETCHANNELMAP,
				&dspdmap,
				sizeof(dspdmap),
				NULL,
				0,
				&br);
	  if ( ret == 0 )
	    {
	      ret = dspd_stream_ctl(&dspd_dctx,
				    cli->client_index,
				    DSPD_SCTL_CLIENT_GETCHANNELMAP,
				    &cli->dsp.params.stream,
				    sizeof(cli->dsp.params.stream),
				    &dspdmap,
				    sizeof(dspdmap),
				    &br);
	      if ( ret == 0 )
		dspd_chmap_to_oss(&dspdmap.map, &cli->dsp.channelmap);
	    }
	}
    }
  return dspd_req_reply_buf(context, 0, &cli->dsp.channelmap, sizeof(cli->dsp.channelmap));
}

static int32_t dsp_req_syncgroup(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  const oss_syncgroup *grp = inbuf;
  oss_syncgroup g;
  struct dspd_sg_info sgi, sgo;
  int32_t ret;
  size_t br;
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  memset(&sgi, 0, sizeof(sgi));
  if ( grp->id == 0 )
    {
      //Create new syncgroup
      if ( grp->mode & PCM_ENABLE_INPUT )
	sgi.sbits |= DSPD_PCM_SBIT_CAPTURE;
      if ( grp->mode & PCM_ENABLE_OUTPUT )
	sgi.sbits |= DSPD_PCM_SBIT_PLAYBACK;
      ret = dspd_stream_ctl(&dspd_dctx,
			    cli->client_index,
			    DSPD_SCTL_CLIENT_SYNCGROUP,
			    &sgi,
			    sizeof(sgi),
			    &sgo,
			    sizeof(sgo),
			    &br);
      if ( ret == 0 )
	{
	  memset(&g, 0, sizeof(g));
	  g.id = sgo.sgid;
	  if ( sgo.sbits & DSPD_PCM_SBIT_PLAYBACK )
	    g.mode |= PCM_ENABLE_OUTPUT;
	  if ( sgo.sbits & DSPD_PCM_SBIT_CAPTURE )
	    g.mode |= PCM_ENABLE_INPUT;
	  ret = dspd_req_reply_buf(context, 0, &g, sizeof(g));
	} else
	{
	  ret = dspd_req_reply_err(context, 0, ret);
	}
    } else
    {
      //Add to sync group
      sgi.sgid = grp->id;
      ret = dspd_stream_ctl(&dspd_dctx,
			    cli->client_index,
			    DSPD_SCTL_CLIENT_SYNCGROUP,
			    &sgi,
			    sizeof(sgi),
			    NULL,
			    0,
			    &br);
      if ( ret == 0 )
	ret= dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}

static int32_t dsp_req_syncstart(struct dspd_rctx *context,
				 uint32_t req,
				 const void   *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  struct dspd_sync_cmd scmd, out;
  size_t br;
  int32_t ret;
  memset(&scmd, 0, sizeof(scmd));
  scmd.cmd = SGCMD_STARTALL;
  scmd.sgid = *(unsigned int*)inbuf;
  ret = dspd_rclient_ctl(cli->dsp.rclient,
			 DSPD_SCTL_CLIENT_START,
			 &scmd,
			 sizeof(scmd),
			 &out,
			 sizeof(out),
			 &br);
  return dspd_req_reply_err(context, 0, ret);
}

static int32_t ctl_req_getversion(struct dspd_rctx *context,
				  uint32_t req,
				  const void   *inbuf,
				  size_t        inbufsize,
				  void         *outbuf,
				  size_t        outbufsize)
{
  int v = OSS_VERSION;
  return dspd_req_reply_buf(context, 0, &v, sizeof(v));
}

static int32_t nctl_req_sysinfo(struct dspd_rctx *context,
				uint32_t req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  oss_sysinfo si;
  osscuse_get_sysinfo(&si);
  return dspd_req_reply_buf(context, 0, &si, sizeof(si));
}


static int32_t nctl_getdev(struct dspd_rctx *ctx, int dev)
{
  struct oss_cdev_client *cli = dspd_req_userdata(ctx);
  struct oss_dsp_cdev *cdev = cli->cdev;
  int ret = -1;
  bool unlock = false;
  if ( cdev->cdev_index != dev )
    {
      cdev = oss_lock_cdev(dev);
      unlock = true;
    }
  if ( cdev )
    {
      if ( cdev->playback_index >= 0 )
	{
	  if ( dspd_daemon_ref(cdev->playback_index, DSPD_DCTL_ENUM_TYPE_SERVER) == 0)
	    ret = cdev->playback_index;
	}
      if ( cdev->capture_index >= 0 && ret == -1 )
	{
	  if ( dspd_daemon_ref(cdev->capture_index, DSPD_DCTL_ENUM_TYPE_SERVER) == 0)
	    ret = cdev->capture_index;
	}
      if ( unlock )
	oss_unlock_cdev(cdev);
    }

  return ret;
}

static int32_t nctl_mix_nrmix(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  int count = oss_mixer_count();
  return dspd_req_reply_buf(context, 0, &count, sizeof(count));
}

static int32_t nctl_mix_nrext(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  int devnum = *(int*)inbuf;
  int32_t idx = nctl_getdev(context, devnum);
  int32_t ret;
  int32_t count = 0;
  size_t len;
  if ( idx < 0 )
    return dspd_req_reply_err(context, 0, ENODEV);

  ret = oss_mixer_ctl(&dspd_dctx,
		      idx,
		      DSPD_SCTL_SERVER_MIXER_ELEM_COUNT,
		      NULL,
		      0,
		      &count,
		      sizeof(count),
		      &len);
  dspd_daemon_unref(idx);
  if ( ret == 0 )
    {
      count++; //Add 1 for root node
      ret = dspd_req_reply_buf(context, 0, &count, sizeof(count));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}

static int get_range(int devidx, int ctrlidx, int type, struct dspd_mix_range *r)
{
  struct dspd_mix_val val;
  size_t len;
  memset(&val, 0, sizeof(val));
  val.index = ctrlidx;
  val.type = type;
  return oss_mixer_ctl(&dspd_dctx,
		       devidx,
		       DSPD_SCTL_SERVER_MIXER_GETRANGE,
		       &val,
		       sizeof(val),
		       r,
		       sizeof(*r),
		       &len);
}

static int ctrl_channels(struct dspd_mix_info *info)
{
  size_t i, n1 = 0, n2 = 0;
  for ( i = 0; i < 32; i++ )
    {
      if ( info->pchan_mask & (1 << i) )
	n1++;
    }
  for ( i = 0; i < 32; i++ )
    {
      if ( info->cchan_mask & (1 << i) )
	n2++;
    }
  return MAX(n1, n2);
}

static int32_t nctl_mix_extinfo(struct dspd_rctx *context,
				uint32_t req,
				const void   *inbuf,
				size_t        inbufsize,
				void         *outbuf,
				size_t        outbufsize)
{
  const struct oss_mixext *ext = inbuf;
  struct oss_mixext *out = outbuf;
  int32_t idx = nctl_getdev(context, ext->dev);
  int32_t ret;
  size_t len, i;
  struct dspd_mix_info info;
  struct dspd_mix_range range;
  struct dspd_device_stat devinfo;
  struct dspd_mix_val val, cmd;
  int32_t ctrl = ext->ctrl - 1;
  int channels;
  if ( idx < 0 )
    return dspd_req_reply_err(context, 0, ENODEV);
 
  ret = dspd_stream_ctl(&dspd_dctx,
			idx,
			DSPD_SCTL_SERVER_STAT,
			NULL,
			0,
			&devinfo,
			sizeof(devinfo),
			&len);

  if ( ret == 0 )
    {
      if ( ctrl >= 0 )
	{
	  ret = oss_mixer_ctl(&dspd_dctx,
			      idx,
			      DSPD_SCTL_SERVER_MIXER_ELEM_INFO,
			      &ctrl,
			      sizeof(ctrl),
			      &info,
			      sizeof(info),
			      &len);
	} else
	{
	  memset(&cmd, 0, sizeof(cmd));
	  cmd.index = ctrl;
	  ret = oss_mixer_ctl(&dspd_dctx,
			      idx,
			      DSPD_SCTL_SERVER_MIXER_GETVAL,
			      &cmd,
			      sizeof(cmd),
			      &val,
			      sizeof(val),
			      &len);
	}
    }
  
  if ( ret == 0 )
    {
      memset(out, 0, sizeof(*out));
      channels = ctrl_channels(&info);
      if ( ext->ctrl == 0 )
	{
	  ret = dspd_stream_ctl(&dspd_dctx,
				idx,
				DSPD_SCTL_SERVER_STAT,
				NULL,
				0,
				&devinfo,
				sizeof(devinfo),
				&len);
	  if ( ret != 0 )
	    goto out;


	  out->type = MIXT_DEVROOT;
	  out->parent = -1;
	  out->timestamp = val.tstamp;
	  strlcpy(out->id, devinfo.name, sizeof(out->id));
	  out->control_no = ext->dev;
	  out->update_counter = val.update_count;
	  strlcpy(out->extname, devinfo.name, sizeof(out->extname));

	  //This is where ossxmix gets the tab titles
	  oss_mixext_root *root = (oss_mixext_root*)out->data;
	  strlcpy(root->id, out->id, sizeof(root->id));
	  strlcpy(root->name, devinfo.desc, sizeof(root->name));
	} else if ( info.flags & DSPD_MIXF_PVOL )
	{
	  ret = get_range(idx, ctrl, DSPD_MIXF_PVOL, &range);
	  if ( ret < 0 )
	    goto out;
	  out->flags = MIXF_FLAT;
	  out->maxvalue = range.max;
	  out->minvalue = range.min;
	  if ( range.max == 255 && range.min == 0 )
	    {
	      if ( channels == 2 )
		out->type = MIXT_STEREOSLIDER;
	      else
		out->type = MIXT_MONOSLIDER;
	    } else if ( range.max == 32767 && range.min == 0 )
	    {
	      if ( channels == 2 )
		out->type = MIXT_STEREOSLIDER16;
	      else
		out->type = MIXT_MONOSLIDER16;
	    } else
	    {
	      out->type = MIXT_SLIDER;
	    }
	} else if ( info.flags & DSPD_MIXF_CVOL )
	{
	  ret = get_range(idx, ctrl, DSPD_MIXF_CVOL, &range);
	  if ( ret < 0 )
	    goto out;
	  out->flags = MIXF_FLAT;
	  out->maxvalue = range.max;
	  out->minvalue = range.min;
	  if ( range.max == 255 && range.min == 0 )
	    {
	      if ( channels == 2 )
		out->type = MIXT_STEREOSLIDER;
	      else
		out->type = MIXT_MONOSLIDER;
	    } else if ( range.max == 32767 && range.min == 0 )
	    {
	      if ( channels == 2 )
		out->type = MIXT_STEREOSLIDER16;
	      else
		out->type = MIXT_MONOSLIDER16;
	    } else
	    {
	      out->type = MIXT_SLIDER;
	    }
	} else if ( info.flags & DSPD_MIXF_PDB )
	{
	  ret = get_range(idx, ctrl, DSPD_MIXF_PDB, &range);
	  if ( ret < 0 )
	    goto out;
	  out->flags = MIXF_DECIBEL;
	  out->maxvalue = range.max;
	  out->minvalue = range.min;
	  if ( channels == 2 )
	    out->type = MIXT_STEREODB;
	  else
	    out->type = MIXT_MONODB;
	} else if ( info.flags & DSPD_MIXF_CDB )
	{
	  ret = get_range(idx, ctrl, DSPD_MIXF_CDB, &range);
	  if ( ret < 0 )
	    goto out;
	  out->flags = MIXF_DECIBEL;
	  out->maxvalue = range.max;
	  out->minvalue = range.min;
	  //These are made obsolete by the flag above, but that implies older software
	  //may not know about MIXF_DECIBEL.
	  if ( channels == 2 )
	    out->type = MIXT_STEREODB;
	  else
	    out->type = MIXT_MONODB;
	} else if ( info.flags & DSPD_MIXF_ENUM )
	{
	  ret = get_range(idx, ctrl, DSPD_MIXF_ENUM, &range);
	  if ( ret < 0 )
	    goto out;
	  out->maxvalue = range.max;
	  out->minvalue = range.min;
	  out->type = MIXT_ENUM;
	  for ( i = 0; i < range.max; i++ )
	    dspd_set_bit(out->enum_present, i);
	} else if ( info.flags & DSPD_MIXF_PSWITCH )
	{
	  out->type = MIXT_ONOFF;
	  out->maxvalue = 1;
	  out->minvalue = 0;
	  ret = 0;
	} else if ( info.flags & DSPD_MIXF_CSWITCH )
	{
	  out->type = MIXT_ONOFF;
	  out->maxvalue = 1;
	  out->minvalue = 0;
	  ret = 0;
	} else //FIXME: No group support
	{
	  out->type = -1;
	}
      if ( out->type >= 0 )
	{
	  out->dev = ext->dev;
	  out->ctrl = ext->ctrl;
	  out->flags |= MIXF_READABLE | MIXF_WRITEABLE;
	  out->parent = 0;
	  out->timestamp = (unsigned int)((info.tstamp / 1000000ULL) % UINT32_MAX);
	  out->control_no = ext->dev;
	  out->update_counter = (unsigned int)(info.update_count % UINT32_MAX);
	  strlcpy(out->extname, info.name, sizeof(out->extname));
	  strlcpy(out->id, info.name, sizeof(out->id));
	}
    }

 out:
  dspd_daemon_unref(idx);

  if ( ret == 0 )
    {
      ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}

static int check_type(const struct dspd_mix_info *info)
{
  int32_t ret;
  if ( info->flags & DSPD_MIXF_PVOL )
    ret = DSPD_MIXF_PVOL;
  else if ( info->flags & DSPD_MIXF_CVOL )
    ret = DSPD_MIXF_CVOL;
  else if ( info->flags & DSPD_MIXF_PDB )
    ret = DSPD_MIXF_PDB;
  else if ( info->flags & DSPD_MIXF_CDB )
    ret = DSPD_MIXF_CDB;
  else if ( info->flags & DSPD_MIXF_ENUM )
    ret = DSPD_MIXF_ENUM;
  else if ( info->flags & DSPD_MIXF_PSWITCH )
    ret = DSPD_MIXF_PSWITCH;
  else if ( info->flags & DSPD_MIXF_CSWITCH )
    ret = DSPD_MIXF_CSWITCH;
  else
    ret = 0;
  return ret;
}

static int first_channel(struct dspd_mix_info *info)
{
  int32_t i;
  int32_t val;
  for ( i = 0; i < 32; i++ )
    {
      val = 1 << i;
      if ( (info->pchan_mask & val) || (info->cchan_mask & val) )
	return i;
    }
  return -1;
}
static int last_channel(struct dspd_mix_info *info)
{
  int32_t i, last = -1;
  int32_t val;
  for ( i = 0; i < 32; i++ )
    {
      val = 1 << i;
      if ( (info->pchan_mask & val) || (info->cchan_mask & val) )
	last = i;
    }
  return last;
}
static int32_t nctl_mix_read(struct dspd_rctx *context,
			     uint32_t       req,
			     const void    *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  const struct oss_mixer_value *in = inbuf;
  struct oss_mixer_value *out = outbuf;
  int32_t idx = nctl_getdev(context, in->dev);
  int32_t ctrl;
  struct dspd_mix_val cmd, val, val2;
  size_t len;
  struct dspd_mix_info info;
  int32_t ret;
  int channels;
  struct dspd_mix_range r;
  int shift = 0;
  int mask = 0xFFFFFFFF;
  if ( idx < 0 )
    return dspd_req_reply_err(context, 0, ENODEV);
  ctrl = in->ctrl - 1;
  ret = oss_mixer_ctl(&dspd_dctx,
		      idx,
		      DSPD_SCTL_SERVER_MIXER_ELEM_INFO,
		      &ctrl,
		      sizeof(ctrl),
		      &info,
		      sizeof(info),
		      &len);
  if ( ret == 0 )
    {
      channels = ctrl_channels(&info);
      if ( channels == 2 )
	{
	  int t = check_type(&info);
	  if ( t )
	    {
	      ret = get_range(idx, ctrl, t, &r);
	      if ( ret == 0 )
		{
		  if ( r.min == 0 && r.max == 255 )
		    {
		      shift = 8;
		      mask = 0xFF;
		    } else if ( r.min == 0 && r.max == 32767 )
		    {
		      shift = 16;
		      mask = 0xFFFF;
		    } else
		    {
		      channels = 1; //Can't do stereo for this control with OSS API
		    }
		}
	    } else
	    {
	      channels = 1;
	    }
	}
    }
  
  if ( ret == 0 )
    {
      memset(&cmd, 0, sizeof(cmd));
      cmd.channel = first_channel(&info);
      if ( cmd.channel == -1 )
	cmd.channel = 0;
      cmd.index = ctrl;
      cmd.type = check_type(&info);
      cmd.tstamp = (unsigned int)in->timestamp;
      cmd.flags = DSPD_CTRLF_TSTAMP_32BIT;
      ret = oss_mixer_ctl(&dspd_dctx,
			  idx,
			  DSPD_SCTL_SERVER_MIXER_GETVAL,
			  &cmd,
			  sizeof(cmd),
			  &val,
			  sizeof(val),
			  &len);
      if ( channels == 2 && ret == 0 )
	{
	  cmd.channel = last_channel(&info);
	  if ( cmd.channel == -1 )
	    cmd.channel = 0; //Seems to to work
	  ret = oss_mixer_ctl(&dspd_dctx,
			      idx,
			      DSPD_SCTL_SERVER_MIXER_GETVAL,
			      &cmd,
			      sizeof(cmd),
			      &val2,
			      sizeof(val2),
			      &len);
	}
      
    } 
  dspd_daemon_unref(idx);
  if ( ret == 0 )
    {
      memset(out, 0, sizeof(*out));
      out->dev = in->dev;
      out->ctrl = in->ctrl;
      if ( channels == 2 )
	{
	  out->value = val.value & mask;
	  out->value |= (val2.value << shift) & (mask << shift);
	} else
	{
	  out->value = val.value;
	}
      out->timestamp = cmd.tstamp;
      ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}
static int32_t nctl_mix_write(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  const struct oss_mixer_value *in = inbuf;
  struct oss_mixer_value *out = outbuf;
  int32_t idx = nctl_getdev(context, in->dev);
  struct dspd_mix_val cmd;
  size_t len;
  int32_t ctrl;
  struct dspd_mix_info info;
  int32_t ret;
  int channels;
  struct dspd_mix_range r;
  int shift = 0;
  int mask = 0xFFFFFFFF;
  if ( idx < 0 )
    return dspd_req_reply_err(context, 0, ENODEV);
  ctrl = in->ctrl - 1;
  ret = oss_mixer_ctl(&dspd_dctx,
		      idx,
		      DSPD_SCTL_SERVER_MIXER_ELEM_INFO,
		      &ctrl,
		      sizeof(ctrl),
		      &info,
		      sizeof(info),
		      &len);
  if ( ret == 0 )
    {
      channels = ctrl_channels(&info);
      
      if ( channels == 2 || channels == 1 )
	{
	  ret = get_range(idx, ctrl, check_type(&info), &r);
	  if ( ret == 0 )
	    {
	      if ( r.min == 0 && r.max == 255 )
		{
		  if ( channels == 2 )
		    shift = 8;
		  mask = 0xFF;
		} else if ( r.min == 0 && r.max == 32767 )
		{
		  if ( channels == 2 )
		    shift = 16;
		  mask = 0xFFFF;
		} else
		{
		  channels = 1;
		}
	    }
	} 
    }


  if ( ret == 0 )
    {
      memset(&cmd, 0, sizeof(cmd));
      cmd.index = ctrl;
      cmd.type = check_type(&info);
      cmd.tstamp = (unsigned int)in->timestamp;
      cmd.flags = DSPD_CTRLF_TSTAMP_32BIT;
      if ( channels == 2 || channels == 1 )
	{
	  cmd.value = in->value & mask;
	  cmd.channel = first_channel(&info);
	  if ( cmd.channel == -1 )
	    cmd.channel = 0;
	} else
	{
	  cmd.channel = -1; //All
	  cmd.value = in->value;
	}
      get_range(idx, ctrl, check_type(&info), &r);
      ret = oss_mixer_ctl(&dspd_dctx,
			  idx,
			  DSPD_SCTL_SERVER_MIXER_SETVAL,
			  &cmd,
			  sizeof(cmd),
			  NULL,
			  0,
			  &len);
      if ( ret == 0 && channels == 2 )
	{
	  cmd.value = in->value >> shift;
	  cmd.channel = last_channel(&info);
	  ret = oss_mixer_ctl(&dspd_dctx,
			      idx,
			      DSPD_SCTL_SERVER_MIXER_SETVAL,
			      &cmd,
			      sizeof(cmd),
			      NULL,
			      0,
			      &len);
	}
    } 
  dspd_daemon_unref(idx);
  if ( ret == 0 )
    {
      memset(out, 0, sizeof(*out));
      out->dev = in->dev;
      out->ctrl = in->ctrl;
      out->value = in->value;
      out->timestamp = cmd.tstamp;
      ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}



static int32_t nctl_audioinfo(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  //return dspd_req_reply_err(context, 0, EINVAL);
  /*
    This covers:
    SNDCTL_AUDIOINFO (audio engine info)
    SNDCTL_AUDIOINFO_EX (raw uncooked hardware info)
    SNDCTL_ENGINEINFO (same as AUDIOINFO on some platforms)

    It looks like engine info should return either the same as
    audio info for full duplex or point to multiple entries for
    half duplex (two threads).
    

  */
  const oss_audioinfo *in = inbuf;
  oss_audioinfo *out = outbuf;
  int32_t idx;
  struct dspd_device_stat devinfo;
  size_t len, i;
  int32_t ret;
  int32_t card, f, iafmt, oafmt, next = -1;
  if ( req == (int)SNDCTL_ENGINEINFO )
    {
      idx = osscuse_get_audio_engine(in->dev, &card, &next);
      if ( idx < 0 )
	return dspd_req_reply_err(context, 0, EINVAL);
      ret = dspd_stream_ctl(&dspd_dctx,
			    idx,
			    DSPD_SCTL_SERVER_STAT,
			    NULL,
			    0,
			    &devinfo,
			    sizeof(devinfo),
			    &len);
      if ( (devinfo.streams & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) == (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE))
	devinfo.reserved = OSS_CARDINFO_FULLDUPLEX;
      else
	devinfo.reserved = 0;
      dspd_daemon_unref(idx);
    } else
    {
      ret = osscuse_get_cardinfo(in->dev, &devinfo);
      card = in->dev;
    }
  
 
  if ( ret == 0 )
    {

      memset(out, 0, sizeof(*out));
      out->dev = in->dev;
      strlcpy(out->name, devinfo.desc, sizeof(out->name));
      strlcpy(out->label, devinfo.desc, sizeof(out->label));
      out->busy = 0; //FIXME: Keep track of this.  Should
                     //look for any clients, not just oss.

      out->pid = -1; //Might be meaningless if the device supports multiple access
      //out->cmd

      out->caps = 0;
      if ( (devinfo.streams & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) ==
	   (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE) &&
	   devinfo.reserved == OSS_CARDINFO_FULLDUPLEX )
	out->caps |= PCM_CAP_DUPLEX;
      out->caps |= PCM_CAP_BATCH;
      out->caps |= PCM_CAP_TRIGGER;
      out->caps |= PCM_CAP_MULTI;
      //TODO: out->caps |= PCM_CAP_BIND;
      out->caps |= PCM_CAP_VIRTUAL;
      if ( devinfo.streams & DSPD_PCM_SBIT_CAPTURE )
	out->caps |= PCM_CAP_INPUT;
      if ( devinfo.streams & DSPD_PCM_SBIT_PLAYBACK )
	out->caps |= PCM_CAP_OUTPUT;
      //TODO: DSP_CH_
      //TODO: PCM_CAP_DEFAULT

      if ( req == (int)SNDCTL_AUDIOINFO_EX )
	{
	  if ( devinfo.streams & DSPD_PCM_SBIT_PLAYBACK )
	    oafmt = devinfo.playback.format;
	  else
	    oafmt = -1;
	  if ( devinfo.streams & DSPD_PCM_SBIT_CAPTURE )
	    iafmt = devinfo.capture.format;
	  else
	    iafmt = -1;

	  f = AFMT_FLOAT | AFMT_S16_LE;
	  //This basically tells the approximate precision of the output.
	  //It isn't exactly right because everything gets converted to floating
	  //point, processed, and then the sound card itself might not have a very
	  //accurate DAC or ADC.
	  for ( i = 0; i < ARRAY_SIZE(format_list); i += 2 )
	    {
	      if ( format_list[i+1] == oafmt )
		out->oformats = format_list[i];
	      if ( format_list[i+1] == iafmt )
		out->iformats = format_list[i];
	    }
	  if ( (devinfo.streams & DSPD_PCM_SBIT_PLAYBACK) != 0 &&
	       out->oformats == 0 )
	    out->oformats = f;
	  if ( (devinfo.streams & DSPD_PCM_SBIT_CAPTURE) != 0 &&
	       out->iformats == 0 )
	    out->iformats = f;
	  
	  if ( devinfo.streams & DSPD_PCM_SBIT_PLAYBACK )
	    out->min_rate = devinfo.playback.rate;
	  else
	    out->min_rate = devinfo.capture.rate;
	  out->max_rate = out->min_rate;
	  out->rates[0] = out->max_rate;
	  out->nrates = 1;
	} else
	{
	  out->caps |= PCM_CAP_FREERATE;
	  f = 0;
	  for ( i = 0; i < ARRAY_SIZE(format_list); i += 2 )
	    f |= format_list[i];
	  if ( devinfo.streams & DSPD_PCM_SBIT_CAPTURE )
	    out->iformats = f;
	  if ( devinfo.streams & DSPD_PCM_SBIT_PLAYBACK )
	    out->oformats = f;

	  out->max_rate = 384000;
	  out->min_rate = 8000;
	  //No need to set rates[] and nrates with PCM_CAP_FREERATE
	}

      out->card_number = card;

      //out->song_name 
      //out->label

      out->port_number = 0;
      out->mixer_dev = card;
      out->legacy_device = card;
      snprintf(out->devnode, sizeof(out->devnode), "/dev/dsp%d", card);
      out->enabled = 1;
      out->min_channels = 1;

      if ( devinfo.playback.channels > devinfo.capture.channels )
	out->max_channels = devinfo.playback.channels;
      else
	out->max_channels = devinfo.capture.channels;
      
      out->rate_source = -1; //Not supported

      snprintf(out->handle, sizeof(out->handle), "%s@%s", devinfo.addr, devinfo.bus);
      
      /*
	There is never a next_play_engine.  For any card, the playback engine has the lowest number if
	the device is capable of playback and capture but not true full duplex.
	If there is no capture on the device or it is full duplex then next_rec_engine should be 0.
      */
      if ( req == (int)SNDCTL_ENGINEINFO && 
	   (devinfo.streams & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) == DSPD_PCM_SBIT_PLAYBACK &&
	   next > 0 )
	out->next_rec_engine = next;

      return dspd_req_reply_buf(context, 0, out, sizeof(*out));
    }

  return dspd_req_reply_err(context, 0, EINVAL);
}
static int32_t nctl_cardinfo(struct dspd_rctx *context,
			     uint32_t req,
			     const void   *inbuf,
			     size_t        inbufsize,
			     void         *outbuf,
			     size_t        outbufsize)
{
  const oss_card_info *in = inbuf;
  oss_card_info *out = outbuf;
  int32_t idx = nctl_getdev(context, in->card);
  int32_t ret;
  size_t len;
  struct dspd_device_stat devinfo;
  struct dspd_mix_irqinfo irqinfo;
  if ( idx < 0 )
    return dspd_req_reply_err(context, 0, ENODEV);
  
  ret = dspd_stream_ctl(&dspd_dctx,
			idx,
			DSPD_SCTL_SERVER_STAT,
			NULL,
			0,
			&devinfo,
			sizeof(devinfo),
			&len);
  if ( ret == 0 )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    idx,
			    DSPD_SCTL_SERVER_IRQINFO,
			    NULL,
			    0,
			    &irqinfo,
			    sizeof(irqinfo),
			    &len);
    }

  dspd_daemon_unref(idx);

  if ( ret == 0 )
    {
      memset(out, 0, sizeof(*out));
      out->card = in->card;
      strlcpy(out->shortname, devinfo.name, sizeof(out->shortname));
      out->intr_count = irqinfo.irq_count;
      out->ack_count = irqinfo.ack_count;
      //flags doesn't seem to be used
      //hw_info is specific to a particular combination of driver+card
      //filler should be all 0
      ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}

static int32_t nctl_mixerinfo(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  const oss_mixerinfo *in = inbuf;
  oss_mixerinfo *out = outbuf;
  int32_t idx = -1;
  int32_t ret;
  struct dspd_device_stat devinfo;
  size_t len;
  int32_t defaultdev;
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  struct oss_dsp_cdev *cdev;
  int devnum;
  bool unlock;
  int32_t nrext;
  cdev = cli->cdev;
  if ( cdev->cdev_index != in->dev )
    {
      cdev = oss_lock_cdev(in->dev);
      unlock = true;
    } else
    {
      unlock = false;
    }
  if ( cdev == NULL )
    return dspd_req_reply_err(context, 0, ENODEV);
  devnum = cdev->cdev_index;
  if ( cdev->playback_index >= 0 )
    {
      if ( dspd_daemon_ref(cdev->playback_index, DSPD_DCTL_ENUM_TYPE_SERVER) == 0)
	idx = cdev->playback_index;
    }
  if ( cdev->capture_index >= 0 && idx == -1 )
    {
      if ( dspd_daemon_ref(cdev->capture_index, DSPD_DCTL_ENUM_TYPE_SERVER) == 0)
	idx = cdev->capture_index;
    }
  if ( unlock )
    oss_unlock_cdev(cdev);
  if ( idx < 0 )
    return dspd_req_reply_err(context, 0, ENODEV);
  
  ret = dspd_stream_ctl(&dspd_dctx,
			idx,
			DSPD_SCTL_SERVER_STAT,
			NULL,
			0,
			&devinfo,
			sizeof(devinfo),
			&len);
  if ( ret == 0 )
    ret = oss_mixer_ctl(&dspd_dctx,
			idx,
			DSPD_SCTL_SERVER_MIXER_ELEM_COUNT,
			NULL,
			0,
			&nrext,
			sizeof(nrext),
			&len);
  if ( ret == 0 )
    {
      ret = dspd_stream_ctl(&dspd_dctx,
			    0,
			    DSPD_DCTL_GET_DEFAULTDEV,
			    &devinfo.streams,
			    sizeof(devinfo.streams),
			    &defaultdev,
			    sizeof(defaultdev),
			    &len);
    }
			  
  dspd_daemon_unref(idx);
  if ( ret == 0 )
    {
      memset(out, 0, sizeof(*out));
      out->dev = devnum;
      strlcpy(out->id, devinfo.name, sizeof(out->id));
      strlcpy(out->name, devinfo.desc, sizeof(out->name));
      out->card_number = devnum;
      out->port_number = 0; //I don't think alsa supports multiple ports
      out->magic = 0; //Should not be used
      out->enabled = 1; //Should always be enabled
      out->caps = 0;
      out->flags = 0; //Not used by applications
      if ( defaultdev == idx )
	out->priority = devnum;
      else
	out->priority = devnum * -1;
      snprintf(out->devnode, sizeof(out->devnode), "/dev/mixer%d", devnum);
      out->legacy_device = devnum;
      out->nrext = nrext + 1;
      ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
    } else
    {
      ret = dspd_req_reply_err(context, 0, EINVAL);
    }
  return ret;
}

static int insert_str(oss_mixer_enuminfo *ei, const char *str, int index)
{
  size_t offset, len, space;
  if ( index == 0 )
    {
      offset = 0;
    } else
    {
      offset = ei->strindex[index-1];
      offset += strlen(&ei->strings[offset]);
      offset++;
    }
  if ( offset >= (sizeof(ei->strings)-1) )
    return -ENOMEM;
  len = strlen(str);
  space = sizeof(ei->strings) - offset;
  if ( space <= len )
    return -ENOMEM;
  memcpy(&ei->strings[offset], str, len);
  ei->strindex[index] = offset;
  return 0;
}

static int32_t nctl_enuminfo(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  const oss_mixer_enuminfo *ei = inbuf;
  oss_mixer_enuminfo *out = outbuf;
  int32_t idx = nctl_getdev(context, ei->dev);
  int i;
  int32_t ret = -EINVAL;
  struct dspd_mix_info info;
  struct dspd_mix_enum_idx eidx;
  size_t len;
  if ( idx < 0 )
    return dspd_req_reply_err(context, 0, ENODEV);
  eidx.elem_idx = ei->ctrl - 1;
  memset(out, 0, sizeof(*out));
  out->dev = ei->dev;
  out->ctrl = ei->ctrl;
  out->version = 0; //Does not change
  for ( i = 0; i < OSS_ENUM_MAXVALUE; i++ )
    {
      eidx.enum_idx = i;
      ret = oss_mixer_ctl(&dspd_dctx,
			  idx,
			  DSPD_SCTL_SERVER_MIXER_ENUM_INFO,
			  &eidx,
			  sizeof(eidx),
			  &info,
			  sizeof(info),
			  &len);
      if ( ret == EINVAL )
	{
	  ret = 0;
	  break;
	}
      ret = insert_str(out, info.name, i);
      if ( ret < 0 )
	{
	  if ( ret == -ENOMEM )
	    ret = 0;
	  break;
	}
    }
  dspd_daemon_unref(idx);
  
  if ( ret == 0 && i == 0 )
    ret = EINVAL;
  if ( ret == 0 )
    {
      out->nvalues = i;
      ret = dspd_req_reply_buf(context, 0, out, sizeof(*out));
    } else
    {
      ret = dspd_req_reply_err(context, 0, ret);
    }
  return ret;
}


/*
  Legacy mixer
  This is based on 4Front's documentation.  Every program seems to have a slightly different idea
  about how the OSS legacy mixer API works.  The legacy mixer API really sucks.  It won't match up
  to a lot of modern hardware.  Don't use it if you can help it.  The new OSSv4 API is a lot better.


*/

static int count_bits(uint32_t val)
{
  int i, ret = 0;
  for ( i = 0; i < 32; i++ )
    if ( val & (1 << i) )
      ret++;
  return ret;
}
static struct oss_mix_elem *get_element(struct oss_legacy_mixer_table *tbl, int oss_idx)
{
  size_t i;
  struct oss_mix_elem *elem = NULL;
  if ( tbl )
    {
      for ( i = 0; i < tbl->count; i++ )
	{
	  if ( tbl->elements[i].elem_index == oss_idx )
	    {
	      elem = &tbl->elements[i];
	      break;
	    }
	}
    }
  return elem;
}

//This really means mixer elements, not devices (sound cards, etc).
static int32_t ctl_mixer_read_stereodevs(struct dspd_rctx *context,
					 uint32_t req,
					 const void   *inbuf,
					 size_t        inbufsize,
					 void         *outbuf,
					 size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  int mask = 0;
  int i;
  struct oss_mix_elem *elem;

  for ( i = 0; i < SOUND_MIXER_NONE; i++ )
    {
      elem = get_element(cli->elements, i);
      if ( elem )
	{
	  if ( count_bits(elem->channels) == 2 )
	    mask |= 1 << elem->elem_index;
	}
    }
  return dspd_req_reply_buf(context, 0, &mask, sizeof(mask));
}

//All elements (playback+capture)
static int32_t ctl_mixer_read_devmask(struct dspd_rctx *context,
				      uint32_t req,
				      const void   *inbuf,
				      size_t        inbufsize,
				      void         *outbuf,
				      size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  int mask = 0;
  int i;
  struct oss_mix_elem *elem;
  for ( i = 0; i < SOUND_MIXER_NONE; i++ )
    {
      elem = get_element(cli->elements, i);
      if ( elem )
	if ( elem->elem_index != SOUND_MIXER_NONE )
	  mask |= 1 << elem->elem_index;
    }

  return dspd_req_reply_buf(context, 0, &mask, sizeof(mask));
}

//All enumerated capture sources
static int32_t ctl_mixer_read_recmask(struct dspd_rctx *context,
				      uint32_t req,
				      const void   *inbuf,
				      size_t        inbufsize,
				      void         *outbuf,
				      size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  int mask = 0;
  int i, j;
  struct oss_mix_elem *elem;
  for ( i = 0; i < cli->elements->count; i++ )
    {
      elem = &cli->elements->elements[i];
      if ( (elem->type == DSPD_MIXF_ENUM) && (elem->flags & DSPD_MIXF_CAPTURE) )
	{
	  for ( j = 0; j < elem->enum_count; j++ )
	    {
	      if ( elem->enumerations[j] != SOUND_MIXER_NONE && elem->controls[j] >= 0 )
		{
		  mask |= 1 << elem->enumerations[j];
		}
	    }
	}
    }
  return dspd_req_reply_buf(context, 0, &mask, sizeof(mask));
}




static int32_t get_current_recsrc(struct oss_cdev_client *cli, int32_t *val)
{
  int32_t i, ret = 0, v = 0;
  struct dspd_mix_val cmd, data;
  const struct oss_mix_elem *elem;
  size_t len;
  memset(&cmd, 0, sizeof(cmd));
  for ( i = 0; i < cli->elements->count; i++ )
    {
      elem = &cli->elements->elements[i];
      if ( elem->type != DSPD_MIXF_ENUM || (elem->flags & DSPD_MIXF_CAPTURE) == 0 )
	continue;
      cmd.tstamp = elem->tstamp;
      cmd.index = i;
      cmd.type = DSPD_MIXF_ENUM;
      cmd.channel = first_bit(elem->channels);
      if ( cmd.channel < 0 )
	cmd.channel = 0;
      ret = oss_mixer_ctl(&dspd_dctx,
			  cli->device_index,
			  DSPD_SCTL_SERVER_MIXER_GETVAL,
			  &cmd,
			  sizeof(cmd),
			  &data,
			  sizeof(data),
			  &len);
      if ( ret != 0 )
	return ret;
      if ( data.value < elem->enum_count )
	{
	  if ( elem->enumerations[data.value] != SOUND_MIXER_NONE && elem->controls[data.value] >= 0 )
	    v |= 1 << elem->enumerations[data.value];
	}
    }
  *val = v;
  return ret;
}

static int32_t set_rec_bit(struct oss_cdev_client *cli, const struct oss_mix_elem *elem, int32_t index, int32_t *val)
{
  int32_t i, ret = 0, v = *val, n;
  size_t len;
  struct dspd_mix_val cmd;
  for ( i = 0; i < elem->enum_count; i++ )
    {
      if ( elem->enumerations[i] != SOUND_MIXER_NONE && elem->controls[i] >= 0 && (elem->flags & DSPD_MIXF_CAPTURE) )
	{
	  n = 1 << elem->enumerations[i];
	  if ( n & v )
	    {
	      memset(&cmd, 0, sizeof(cmd));
	      cmd.index = index;
	      cmd.tstamp = elem->tstamp;
	      cmd.value = i;
	      cmd.channel = -1;
	      cmd.type = elem->type;
	      ret = oss_mixer_ctl(&dspd_dctx,
				  cli->device_index,
				  DSPD_SCTL_SERVER_MIXER_SETVAL,
				  &cmd,
				  sizeof(cmd),
				  NULL,
				  0,
				  &len);
	      if ( ret == 0 )
		*val = v & ~n;
	      break;
	    }
	}
    }
  return ret;
}

static int32_t set_current_recsrc(struct oss_cdev_client *cli, int32_t val)
{
  int32_t i, ret = 0, v;
  struct oss_mix_elem *elem;
  if ( count_bits(val) == 1 )
    v = val;
  else
    v = 0;
  for ( i = 0; i < cli->elements->count; i++ )
    {
      elem = &cli->elements->elements[i];
      if ( elem->type != DSPD_MIXF_ENUM )
	continue;
      ret = set_rec_bit(cli, elem, i, &val);
      val |= v;
      if ( ret != 0 || val == 0 )
	break;
    }
  return ret;
}

static int32_t ctl_mixer_readwrite_recsrc(struct dspd_rctx *context,
					  uint32_t req,
					  const void   *inbuf,
					  size_t        inbufsize,
					  void         *outbuf,
					  size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  int32_t mask, ret;
  if ( ! cli->elements )
    return dspd_req_reply_err(context, 0, ENXIO);
  if ( inbuf )
    {
      mask = *(int32_t*)inbuf;
      ret = set_current_recsrc(cli, mask);
      if ( ret )
	return dspd_req_reply_err(context, 0, ret);
    }
  ret = get_current_recsrc(cli, &mask);
  if ( ret )
    return dspd_req_reply_err(context, 0, ret);
  return dspd_req_reply_buf(context, 0, &mask, sizeof(mask));
}



static int32_t ctl_mixer_read_caps(struct dspd_rctx *context,
				   uint32_t req,
				   const void   *inbuf,
				   size_t        inbufsize,
				   void         *outbuf,
				   size_t        outbufsize)
{
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  int mask = 0;
  int i, count = 0, ec = 0;
  const struct oss_mix_elem *elem;
  if ( ! cli->elements )
    return dspd_req_reply_err(context, 0, ENXIO);
  for ( i = 0; i < cli->elements->count; i++ )
    {
      elem = &cli->elements->elements[i];
      if ( elem->type & DSPD_MIXF_CAPTURE )
	count++;
      if ( (elem->type & DSPD_MIXF_ENUM) && (elem->flags & DSPD_MIXF_CAPTURE) )
	ec++;
	
      
    }
  if ( count == 0 )
    mask |= SOUND_CAP_NORECSRC;
  if ( ec == 1 )
    mask |= SOUND_CAP_EXCL_INPUT;
  return dspd_req_reply_buf(context, 0, &mask, sizeof(mask));
}

static int first_bit(int mask)
{
  int i, ret = -1;
  for ( i = 0; i < 32; i++ )
    {
      if ( mask & (1 << i) )
	{
	  ret = i;
	  break;
	}
    }
  return ret;
}
static int last_bit(int mask)
{
  int i, ret = -1;
  for ( i = 0; i < 32; i++ )
    if ( mask & (1 << i) )
      ret = i;
  return ret;
}


static int32_t ctl_mixer_readwrite(struct dspd_rctx *context,
				   uint32_t req,
				   const void   *inbuf,
				   size_t        inbufsize,
				   void         *outbuf,
				   size_t        outbufsize)
{
  int idx = _IOC_NR(req);
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  struct oss_mix_elem *elem = NULL;
  int result, ret, i;
  struct dspd_mix_val cmd, val, val2;
  size_t len;
  bool stereo;
  int32_t sreq;
  int input_val;
  if ( cli->elements == NULL || cli->elements->elements == NULL )
    return dspd_req_reply_err(context, 0, ENXIO);
  if ( inbufsize == sizeof(int) )
    {
      sreq = DSPD_SCTL_SERVER_MIXER_SETVAL;
      input_val = *(int*)inbuf;
    } else
    {
      sreq = DSPD_SCTL_SERVER_MIXER_GETVAL;
      input_val = 0;
    }
  for ( i = 0; i < cli->elements->count; i++ )
    {
      elem = &cli->elements->elements[i];
      if ( elem->elem_index == idx )
	break;
    }
  if ( i == cli->elements->count || elem == NULL )
    return dspd_req_reply_err(context, 0, EINVAL);

  memset(&cmd, 0, sizeof(cmd));
  stereo = count_bits(elem->channels) == 2;
  cmd.channel = first_bit(elem->channels);
  if ( cmd.channel < 0 )
    cmd.channel = 0;
  cmd.index = i;
  cmd.type = elem->type;
  cmd.tstamp = elem->tstamp;
  cmd.flags = DSPD_CTRLF_SCALE_PCT;
  cmd.value = input_val & 0xFF;
  ret = oss_mixer_ctl(&dspd_dctx,
		      cli->device_index,
		      sreq,
		      &cmd,
		      sizeof(cmd),
		      &val,
		      sizeof(val),
		      &len);
  if ( ret )
    goto error;
  if ( stereo )
    {
      cmd.channel = last_bit(elem->channels);
      cmd.value = input_val >> 8U;
      ret = oss_mixer_ctl(&dspd_dctx,
			  cli->device_index,
			  sreq,
			  &cmd,
			  sizeof(cmd),
			  &val2,
			  sizeof(val2),
			  &len);
      if ( ret )
	goto error;
      result = val.value;
      result |= val2.value << 8;
    } else
    {
      result = val.value | (val.value << 8U);
    }
  return dspd_req_reply_buf(context, 0, &result, sizeof(result));

 error:

  return dspd_req_reply_err(context, 0, ret);
}





static int32_t ctl_mixer_info(struct dspd_rctx *context,
			      uint32_t req,
			      const void   *inbuf,
			      size_t        inbufsize,
			      void         *outbuf,
			      size_t        outbufsize)
{
  struct mixer_info *info = outbuf;
  struct oss_cdev_client *cli = dspd_req_userdata(context);
  struct dspd_device_stat devinfo;
  size_t len;
  int ret;
  struct dspd_mix_val val, cmd;
  ret = dspd_stream_ctl(&dspd_dctx,
			cli->device_index,
			DSPD_SCTL_SERVER_STAT,
			NULL,
			0,
			&devinfo,
			sizeof(devinfo),
			&len);
  if ( ret )
    return dspd_req_reply_err(context, 0, ret);
  strlcpy(info->id, devinfo.name, sizeof(info->id));
  strlcpy(info->name, devinfo.desc, sizeof(info->name));
  if ( outbufsize == sizeof(mixer_info) )
    {
      //Got new mixer_info struct.
      memset(&cmd, 0, sizeof(cmd));
      cmd.index = UINT32_MAX;
      ret = oss_mixer_ctl(&dspd_dctx,
			  cli->device_index,
			  DSPD_SCTL_SERVER_MIXER_GETVAL,
			  &cmd,
			  sizeof(cmd),
			  &val,
			  sizeof(val),
			  &len);
      if ( ret )
	return dspd_req_reply_err(context, 0, ret);
      info->modify_counter = val.update_count;
      memset(info->fillers, 0, sizeof(info->fillers));
    }
  return dspd_req_reply_buf(context, 0, info, sizeof(*info));
}





struct dspd_req_handler dsp_ioctl_handlers[] = {
  IOCTL_HANDLER(SNDCTL_DSP_RESET, dsp_req_reset),
  IOCTL_HANDLER(SNDCTL_DSP_SYNC, dsp_req_sync),
  IOCTL_HANDLER(SNDCTL_DSP_SPEED, dsp_req_speed),
  IOCTL_HANDLER(SNDCTL_DSP_STEREO, dsp_req_stereo),
  IOCTL_HANDLER(SNDCTL_DSP_GETBLKSIZE, dsp_req_getblksize),
  IOCTL_HANDLER(SNDCTL_DSP_SETFMT, dsp_req_setfmt), 
  IOCTL_HANDLER(SNDCTL_DSP_CHANNELS, dsp_req_channels),
  IOCTL_HANDLER(SNDCTL_DSP_POST, dsp_req_post),
  IOCTL_HANDLER(SNDCTL_DSP_SUBDIVIDE, dsp_req_subdivide),
  IOCTL_HANDLER(SNDCTL_DSP_SETFRAGMENT, dsp_req_setfragment),
  IOCTL_HANDLER(SNDCTL_DSP_GETFMTS, dsp_req_getfmts),
  IOCTL_HANDLER(SNDCTL_DSP_GETOSPACE, dsp_req_getospace),
  IOCTL_HANDLER(SNDCTL_DSP_GETISPACE, dsp_req_getispace),
  IOCTL_HANDLER(SNDCTL_DSP_GETCAPS, dsp_req_getcaps), 
  IOCTL_HANDLER(SNDCTL_DSP_GETTRIGGER, dsp_req_gettrigger),
  IOCTL_HANDLER(SNDCTL_DSP_SETTRIGGER, dsp_req_settrigger),
  IOCTL_HANDLER(SNDCTL_DSP_GETIPTR, dsp_req_getiptr),
  IOCTL_HANDLER(SNDCTL_DSP_GETOPTR, dsp_req_getoptr),
  IOCTL_HANDLER(SNDCTL_DSP_SETSYNCRO, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_SETDUPLEX, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_PROFILE, dsp_req_profile),
  IOCTL_HANDLER(SNDCTL_DSP_GETODELAY, dsp_req_getodelay),
  IOCTL_HANDLER(SNDCTL_DSP_GETPLAYVOL, dsp_req_getplayvol),
  IOCTL_HANDLER(SNDCTL_DSP_SETPLAYVOL, dsp_req_setplayvol),
  IOCTL_HANDLER(SNDCTL_DSP_GETERROR, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_READCTL, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_WRITECTL, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_SYNCGROUP, dsp_req_syncgroup),
  IOCTL_HANDLER(SNDCTL_DSP_SYNCSTART, dsp_req_syncstart),
  IOCTL_HANDLER(SNDCTL_DSP_COOKEDMODE, dsp_req_cookedmode),
  IOCTL_HANDLER(SNDCTL_DSP_SILENCE, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_SKIP, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_HALT_INPUT, dsp_req_halt_input), //NEXT
  IOCTL_HANDLER(SNDCTL_DSP_HALT_OUTPUT, dsp_req_halt_output),
  IOCTL_HANDLER(SNDCTL_DSP_LOW_WATER, dsp_req_low_water),
  IOCTL_HANDLER(SNDCTL_DSP_CURRENT_IPTR, dsp_req_get_current_ptr), //CHECKME
  IOCTL_HANDLER(SNDCTL_DSP_CURRENT_OPTR, dsp_req_get_current_ptr),
  IOCTL_HANDLER(SNDCTL_DSP_GET_RECSRC_NAMES, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_GET_RECSRC, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_SET_RECSRC, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_GET_PLAYTGT_NAMES, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_GET_PLAYTGT, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_SET_PLAYTGT, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_GETRECVOL, dsp_req_getrecvol),
  IOCTL_HANDLER(SNDCTL_DSP_SETRECVOL, dsp_req_setrecvol),
  IOCTL_HANDLER(SNDCTL_DSP_GET_CHNORDER, dsp_req_get_chnorder),
  IOCTL_HANDLER(SNDCTL_DSP_SET_CHNORDER, dsp_req_set_chnorder),
  IOCTL_HANDLER(SNDCTL_DSP_GETIPEAKS, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_GETOPEAKS, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_POLICY, dsp_req_policy),
  IOCTL_HANDLER(SNDCTL_DSP_GETCHANNELMASK, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_BIND_CHANNEL, NULL),
  IOCTL_HANDLER(SNDCTL_DSP_NONBLOCK, dsp_req_nonblock),
};



/*static bool check_ioctl(int request,
			const struct dspd_req_handler *handlers,
			int count,
			int ioc_type)
{
  unsigned int nr;
  const struct dspd_req_handler *h = NULL;
  bool ret = 0;

  if ( _IOC_TYPE(request) == ioc_type )
    {
      nr = _IOC_NR(request) + 1; //ioctls are offset by 1
      if ( nr < count )
	{
	  h = &handlers[nr];
	  if ( h->handler ) 
	    {
	      if ( _IOC_DIR(request) == _IOC_WRITE )
		{
		  ret = _IOC_SIZE(request) == h->inbufsize;
		} else if ( _IOC_DIR(request) == _IOC_READ )
		{
		  ret = _IOC_SIZE(request) == h->outbufsize;
		} else if ( _IOC_DIR(request) == (_IOC_READ|_IOC_WRITE) )
		{
		  ret = (_IOC_SIZE(request) == h->outbufsize) && (_IOC_SIZE(request) == h->inbufsize);
		} else
		{
		  ret = (h->outbufsize == 0) && (h->inbufsize == 0);
		}
	    }
	}
    }
  return ret;
  }*/





static void dsp_ioctl(struct oss_cdev_client *cli,
		      int cmd,
		      void *arg,
		      int flags,
		      const void *in_buf,
		      size_t in_bufsz,
		      size_t out_bufsz)
{
  struct dspd_rctx rctx = { 0 };
  char outbuf[4096];
  uint64_t r;
  const struct dspd_ioctl_info *h;
  h = get_ioctl_handlers(cmd);
  if ( ! h )
    {
      oss_reply_error(cli, EINVAL);
      return;
    }

  rctx.ops = &dsp_ioctl_rcb;
  rctx.user_data = cli;
  rctx.ops_arg = cli;
  rctx.outbufsize = out_bufsz;
  rctx.outbuf = outbuf;
  rctx.fd = -1;
  rctx.index = -1;
  rctx.flags = DSPD_REQ_FLAG_UNIX_IOCTL;
  //The actual command can be anything, but it should normally be whatever the
  //caller used to make the request.
  r = cmd;
  r <<= 32;
  r |= _IOC_NR(cmd) + 1;


  dspd_daemon_dispatch_ctl(&rctx, 
			   h->handlers,
			   h->count,
			   r,
			   in_buf,
			   in_bufsz,
			   outbuf,
			   out_bufsz);
}



static const struct dspd_req_handler newmixer_ioctl_handlers[] = {
  IOCTL_HANDLER(SNDCTL_SYSINFO, nctl_req_sysinfo),
  IOCTL_HANDLER(SNDCTL_MIX_NRMIX, nctl_mix_nrmix),
  IOCTL_HANDLER(SNDCTL_MIX_NREXT, nctl_mix_nrext),
  IOCTL_HANDLER(SNDCTL_MIX_EXTINFO, nctl_mix_extinfo),
  IOCTL_HANDLER(SNDCTL_MIX_READ, nctl_mix_read),
  IOCTL_HANDLER(SNDCTL_MIX_WRITE, nctl_mix_write),
  IOCTL_HANDLER(SNDCTL_AUDIOINFO, nctl_audioinfo),
  IOCTL_HANDLER(SNDCTL_AUDIOINFO_EX, nctl_audioinfo),
  IOCTL_HANDLER(SNDCTL_ENGINEINFO, nctl_audioinfo),
  IOCTL_HANDLER(SNDCTL_CARDINFO, nctl_cardinfo),
  IOCTL_HANDLER(SNDCTL_MIXERINFO, nctl_mixerinfo),
  IOCTL_HANDLER(SNDCTL_MIX_ENUMINFO, nctl_enuminfo),
};

//#define LM_HANDLER(nr, fcn) IOCTL_HANDLER2(MIXER_WRITE(nr), MIXER_READ(nr), fcn)
#define LM_HANDLER(nr, fcn) IOCTL_HANDLER(MIXER_WRITE(nr), fcn), IOCTL_HANDLER(MIXER_READ(nr), fcn)

static const struct dspd_req_handler oldmixer_ioctl_handlers[] = {
  IOCTL_HANDLER(SOUND_MIXER_INFO, ctl_mixer_info),
  IOCTL_HANDLER(OSS_GETVERSION, ctl_req_getversion),
  LM_HANDLER(SOUND_MIXER_VOLUME, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_BASS, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_TREBLE, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_SYNTH, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_PCM, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_SPEAKER, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_LINE, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_MIC, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_CD, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_IMIX, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_ALTPCM, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_RECLEV, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_IGAIN, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_OGAIN, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_LINE1, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_LINE2, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_LINE3, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_MUTE, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_ENHANCE, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_LOUD, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_PHONE, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_MONO, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_VIDEO, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_DEPTH, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_REARVOL, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_CENTERVOL, ctl_mixer_readwrite),
  LM_HANDLER(SOUND_MIXER_SIDEVOL, ctl_mixer_readwrite),

  LM_HANDLER(SOUND_MIXER_RECSRC, ctl_mixer_readwrite_recsrc),
  IOCTL_HANDLER(SOUND_MIXER_READ_DEVMASK, ctl_mixer_read_devmask),
  IOCTL_HANDLER(SOUND_MIXER_READ_RECMASK, ctl_mixer_read_recmask),
  IOCTL_HANDLER(SOUND_MIXER_READ_STEREODEVS, ctl_mixer_read_stereodevs),
  IOCTL_HANDLER(SOUND_MIXER_READ_CAPS, ctl_mixer_read_caps),

  
};


static const struct dspd_ioctl_info *get_ioctl_handlers(int request)
{
  int type = _IOC_TYPE(request);
  const struct dspd_ioctl_info *i = NULL;
  static const struct dspd_ioctl_info info[] = {
    ['P'] = { .handlers = dsp_ioctl_handlers, .count = ARRAY_SIZE(dsp_ioctl_handlers) },
    ['M'] = { .handlers = oldmixer_ioctl_handlers, .count = ARRAY_SIZE(oldmixer_ioctl_handlers) },
    ['X'] = { .handlers = newmixer_ioctl_handlers, .count = ARRAY_SIZE(newmixer_ioctl_handlers) },
  };
  if ( type < ARRAY_SIZE(info) &&
       info[type].count > 0 )
    {
      //if ( check_ioctl(request, info[type].handlers, info[type].count, type) )
      i = &info[type];
    }
  return i;
}

static void mixer_ioctl(struct oss_cdev_client *cli,
			int cmd,
			void *arg,
			int flags,
			const void *in_buf,
			size_t in_bufsz,
			size_t out_bufsz)
{
  int type = _IOC_TYPE(cmd);

  if ( type != 'X' )
    oss_reply_error(cli, EINVAL);
  else
    dsp_ioctl(cli, cmd, arg, flags, in_buf, in_bufsz, out_bufsz);
}

static void legacy_mixer_ioctl(struct oss_cdev_client *cli,
			       int cmd,
			       void *arg,
			       int flags,
			       const void *in_buf,
			       size_t in_bufsz,
			       size_t out_bufsz)
{
  int type = _IOC_TYPE(cmd);
  //  fprintf(stderr, "NR=%d\n", _IOC_NR(cmd));
  if ( type != 'M' && type != 'X' )
    oss_reply_error(cli, EINVAL);
  else
    dsp_ioctl(cli, cmd, arg, flags, in_buf, in_bufsz, out_bufsz);
}




const struct oss_cdev_ops osscuse_legacy_ops = {
  .write = NULL,
  .read = NULL,
  .ioctl = legacy_mixer_ioctl,
  .poll = NULL,
};

const struct oss_cdev_ops osscuse_mixer_ops = {
  .write = NULL,
  .read = NULL,
  .ioctl = mixer_ioctl,
  .poll = NULL,
};







void oss_delete_legacy_mixer_assignments(struct oss_legacy_mixer_table *table)
{
  int32_t i;
  if ( table )
    {
      for ( i = 0; i < table->count; i++ )
	{
	  free(table->elements[i].enumerations);
	}
      free(table);
    }
}

static int init_enum(int32_t device, 
		     int32_t idx,
		     struct oss_mix_elem *elem, 
		     const struct dspd_mix_info *info)
{
  int i, ret;
  struct dspd_mix_range r;
  size_t len;
  struct dspd_mix_val val;
  struct dspd_mix_enum_idx eidx;
  struct dspd_mix_info einfo;
  memset(&val, 0, sizeof(val));
  elem->flags = info->flags;
  elem->tstamp = info->tstamp;
  elem->elem_index = SOUND_MIXER_NONE;

  if ( elem->flags & DSPD_MIXF_ENUMP )
    {
      elem->type = DSPD_MIXF_ENUM;
      elem->channels = info->pchan_mask;
    } else if ( elem->flags & DSPD_MIXF_ENUMC )
    {
      elem->type = DSPD_MIXF_ENUM;
      elem->channels = info->cchan_mask;
    } else
    {
      return 0;
    }

  val.index = idx;
  val.type = DSPD_MIXF_ENUM;
  ret = oss_mixer_ctl(&dspd_dctx,
		      device,
		      DSPD_SCTL_SERVER_MIXER_GETRANGE,
		      &val,
		      sizeof(val),
		      &r,
		      sizeof(r),
		      &len);
  if ( ret )
    return ret;
  
  
    
  
  elem->enumerations = malloc(sizeof(*elem->enumerations) * r.max * 2);
  if ( ! elem->enumerations )
    return -ENOMEM;
  elem->controls = &elem->enumerations[r.max];
  elem->enum_count = r.max;
  memset(&eidx, 0, sizeof(eidx));
  eidx.elem_idx = idx;
  for ( i = 0; i < r.max; i++ )
    {
      eidx.enum_idx = i;
      ret = oss_mixer_ctl(&dspd_dctx,
			  device,
			  DSPD_SCTL_SERVER_MIXER_ENUM_INFO,
			  &eidx,
			  sizeof(eidx),
			  &einfo,
			  sizeof(einfo),
			  &len);
      if ( ret )
	return ret;
      einfo.flags = DSPD_MIXF_CVOL;
      elem->enumerations[i] = SOUND_MIXER_NONE;
      elem->controls[i] = einfo.vol_index;
    }
  
  return 0;
}
static int init_volume(int32_t device, 
		       const struct snd_mixer_oss_assign_table *tbl,
		       struct oss_mix_elem *elem, 
		       const struct dspd_mix_info *info)
{
  elem->flags = info->flags;
  elem->tstamp = info->tstamp;
  elem->elem_index = tbl->oss_id;
  if ( tbl->flags & DSPD_MIXF_PVOL )
    elem->type = DSPD_MIXF_PVOL;
  else if ( tbl->flags & DSPD_MIXF_PDB )
    elem->type = DSPD_MIXF_PDB;
  else if ( tbl->flags & DSPD_MIXF_CVOL )
    elem->type = DSPD_MIXF_CVOL;
  else
    elem->type = DSPD_MIXF_CDB;
  
  if ( elem->type & DSPD_MIXF_PLAYBACK )
    elem->channels = info->pchan_mask;
  else
    elem->channels = info->cchan_mask;
  return 0;
}			     



int oss_new_legacy_mixer_assignments(int32_t device, struct oss_legacy_mixer_table **table)
{
  struct oss_legacy_mixer_table *tbl = calloc(1, sizeof(struct oss_legacy_mixer_table));
  int ret = 0;
  int i, idx, j;
  int32_t count;
  size_t br;
  struct dspd_mix_info info;
  struct oss_mix_elem *elem;
  const struct snd_mixer_oss_assign_table *oss;
  if ( ! tbl )
    return errno;
  for ( i = 0; i < ARRAY_SIZE(tbl->elements); i++ )
    tbl->elements[i].elem_index = -1;


  ret = oss_mixer_ctl(&dspd_dctx,
		      device,
		      DSPD_SCTL_SERVER_MIXER_ELEM_COUNT,
		      NULL,
		      0,
		      &count,
		      sizeof(count),
		      &br);
  if ( ret )
    goto out;
  if ( count == 0 )
    {
      free(tbl);
      return ENXIO;
    }

  tbl->elements = calloc(count, sizeof(*tbl->elements));
  if ( ! tbl->elements )
    {
      ret = ENOMEM;
      goto out;
    }
  
  for ( i = 0; i < count; i++ )
    {
      ret = oss_mixer_ctl(&dspd_dctx,
			  device,
			  DSPD_SCTL_SERVER_MIXER_ELEM_INFO,
			  &i,
			  sizeof(i),
			  &info,
			  sizeof(info),
			  &br);
      if ( ret < 0 )
	goto out;
      elem = &tbl->elements[i];
      oss = oss_get_mixer_assignment(&info);
      elem->elem_index = SOUND_MIXER_NONE;
      if ( oss )
	{
	  if ( oss->flags & DSPD_MIXF_VOL )
	    ret = init_volume(device, oss, elem, &info);
	} else if ( info.flags & DSPD_MIXF_ENUMC )
	{
	  ret = init_enum(device, i, elem, &info);
	}
      if ( ret )
	goto out;
    }
  tbl->count = count;
  
  //Initialize enumerations now that the entire list of controls is known.
  for ( i = 0; i < tbl->count; i++ )
    {
      elem = &tbl->elements[i];
      if ( (elem->flags & DSPD_MIXF_ENUMC) && elem->type == DSPD_MIXF_ENUM )
	{
	  for ( j = 0; j < elem->enum_count; j++ )
	    {
	      idx = elem->controls[j];
	      if ( idx >= 0 && idx < tbl->count )
		elem->enumerations[j] = tbl->elements[idx].elem_index;
	    }
	}
    }
  

 out:
  if ( tbl->count == 0 )
    {
      ret = ENXIO;
      oss_delete_legacy_mixer_assignments(tbl);
    } else if ( ret )
    {
      oss_delete_legacy_mixer_assignments(tbl);
    } else
    {
      *table = tbl;
    }
  return ret;
}
