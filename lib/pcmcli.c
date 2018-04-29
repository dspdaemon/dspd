#include "pcmcli.h"

static int timer_reset_cb(dspd_pcmcli_timer_t *t)
{
  return dspd_timer_reset((struct dspd_timer*)t);
}

static const struct dspd_pcmcli_timer_ops pcmcli_timer_ops = {
  .fire = (dspd_pcmcli_timer_fire_t)dspd_timer_fire,
  .reset = (dspd_pcmcli_timer_reset_t)timer_reset_cb,
  .get = (dspd_pcmcli_timer_get_t)dspd_timer_get,
  .set = (dspd_pcmcli_timer_set_t)dspd_timer_set,
  .getpollfd = (dspd_pcmcli_timer_getpollfd_t)dspd_timer_getpollfd,
  .destroy = (dspd_pcmcli_timer_destroy_t)dspd_timer_destroy,
};

struct pcmcli_stream_data {
  struct dspd_pcmcli_stream stream;
  struct dspd_shm_map       shm;
  char                     *frame_buf;
  size_t                    frame_off;
  int32_t                   stream_idx;
  int32_t                   device_idx;
  bool                      enabled;
  uint64_t                  xfer;
  struct dspd_device_stat   info;
};
struct dspd_pcmcli {
  struct pcmcli_stream_data playback;
  struct pcmcli_stream_data capture;
  struct dspd_rclient_swparams swparams;
  struct dspd_aio_ctx *conn;
  int32_t state;
  int32_t error;
  bool    nonblocking;
  bool    no_xrun;
  bool    constant_latency;
  int32_t streams;
  int32_t wait_streams;
  uint32_t fragtime;
  uint32_t fragsize;
  uint64_t lasthw;
  struct dspd_timer timer;
  
  size_t nfds;
  struct pollfd pfds[2];
  int32_t connfd;
  bool close;
  struct dspd_async_op pending_op;
  void (*complete)(void *context, struct dspd_async_op *op);
  void *completion_data;
  //The dummy op is so that synchronous operations can pretend to complete asynchronously
  //for consistency.
  bool dummy_op;
  int32_t dummy_result;
  char input[512];
  char output[512];

  //Threshold for reporting POLLIN and POLLOUT when demangling poll events.
  size_t poll_threshold;

  int64_t clockdiff, max_clockdiff, min_clockdiff;

  const struct dspd_pcmcli_timer_ops *timer_ops;
  dspd_pcmcli_timer_t                *timer_arg;

};




static int null_timer_reset(dspd_pcmcli_timer_t *tmr)
{
  struct dspd_timer *t = (struct dspd_timer*)tmr;
  t->oneshot_next = 0;
  t->interval = 0;
  return 0;
}

static int null_timer_get(dspd_pcmcli_timer_t *tmr, dspd_time_t *abstime, uint32_t *per)
{
  struct dspd_timer *t = (struct dspd_timer*)tmr;
  if ( abstime )
    *abstime = t->oneshot_next;
  if ( per )
    *per = t->interval;
  return 0;
}


static int null_timer_getpollfd(dspd_pcmcli_timer_t *tmr, struct pollfd *pfd)
{
  return -ENOSYS;
}
static void null_timer_destroy(dspd_pcmcli_timer_t *tmr)
{
  struct dspd_timer *t = (struct dspd_timer*)tmr;
  memset(t, 0, sizeof(*t));
}
static int null_timer_set(dspd_pcmcli_timer_t *tmr, uint64_t abstime, uint32_t per)
{
  struct dspd_timer *t = (struct dspd_timer*)tmr;
  t->oneshot_next = abstime;
  t->interval = per;
  return 0;
}

static int null_timer_fire(dspd_pcmcli_timer_t *tmr, bool latch)
{
  struct dspd_timer *dt = (struct dspd_timer*)tmr;
  int ret = 0;
  uint64_t t;
  if ( dt->trigger == false )
    {
      t = dt->oneshot_next;
      ret = null_timer_set(tmr, 1, dt->interval);
      if ( ret == 0 )
	{
	  if ( latch == true )
	    dt->latched = true;
	  dt->trigger = true;
	}
      dt->oneshot_next = t;
    }
  return ret;
}

static const struct dspd_pcmcli_timer_ops null_timer_ops = {
  .fire = null_timer_fire,
  .reset = null_timer_reset,
  .get = null_timer_get,
  .set = null_timer_set,
  .getpollfd = null_timer_getpollfd,
  .destroy = null_timer_destroy,
};


static void set_error(struct dspd_pcmcli *client, int32_t result)
{
  if ( result == -EPIPE )
    client->state = PCMCLI_STATE_XRUN;
  else if ( result == -ENODEV )
    client->state = PCMCLI_STATE_DISCONNECTED;
  else if ( dspd_tmperr(result) == false && result != -EBADFD )
    client->error = result;
}

static void complete_event(struct dspd_pcmcli *client, int32_t result)
{
  void (*complete)(void *context, struct dspd_async_op *op) = client->complete;
  //Don't store some temporary errors
  if ( dspd_tmperr(result) == false && result != -EBADFD )
    client->error = result;
  if ( complete )
    {
      client->complete = NULL;
      client->pending_op.error = result;
      client->pending_op.data = client->completion_data;
      //Run the callback.  Since everything is really complete, it is possible to 
      //start a new operation here.
      complete(client, &client->pending_op);
    }
}



static int32_t submit_io(struct dspd_pcmcli *client,
			 uint32_t req,
			 const void          *inbuf,
			 size_t        inbufsize,
			 void         *outbuf,
			 size_t        outbufsize,
			 void (*complete)(void *context, struct dspd_async_op *op))
{
  int32_t ret;
  if ( client->complete == NULL && client->pending_op.error <= 0 )
    {
      if ( inbuf != NULL && inbuf != client->input )
	{
	  assert(inbufsize <= sizeof(client->input));
	  memcpy(client->input, inbuf, inbufsize);
	  inbuf = client->input;
	}
      assert(outbuf != client->output || outbufsize <= sizeof(client->output));
      memset(&client->pending_op, 0, sizeof(client->pending_op));
      client->pending_op.stream = -1;
      client->pending_op.req = req;
      client->pending_op.inbuf = inbuf;
      client->pending_op.inbufsize = inbufsize;
      client->pending_op.outbuf = outbuf;
      client->pending_op.outbufsize = outbufsize;
      client->pending_op.complete = complete;
      client->pending_op.data = client;
      client->pending_op.error = 0;
      ret = dspd_aio_submit(client->conn, &client->pending_op);
    } else
    {
      ret = -EAGAIN;
    }
  return ret;
}

static int32_t wait_for_io(struct dspd_pcmcli *client)
{
  int32_t ret = 0;
  while ( client->pending_op.error > 0 )
    {
      ret = dspd_pcmcli_process_io(client, 0, -1);
      if ( ret < 0 && ret != -EINPROGRESS )
	{
	  if ( client->pending_op.error > 0 )
	    (void)dspd_aio_cancel(client->conn, &client->pending_op, false);
	  break;
	}
    }
  if ( ret == 0 )
    ret = client->pending_op.error;
  return ret;
}

//The async operation either succeeds at starting or fails to start.  There are no synchronous completions of async
//requests.  Calling with complete==NULL means the request is supposed to complete synchronously.
static int32_t complete_io(struct dspd_pcmcli *client, int32_t lastresult, dspd_aio_ccb_t complete, void *data)
{
  int32_t ret;
  if ( lastresult < 0 )
    {
      ret = lastresult;
    } else if ( complete )
    {
      client->complete = complete;
      client->completion_data = data;
      ret = -EINPROGRESS;
    } else
    {
      ret = wait_for_io(client);
    }
  return ret;
}


int32_t dspd_pcmcli_get_next_wakeup(struct dspd_pcmcli *client, uint32_t *avail, int32_t *streams, dspd_time_t *next)
{
  dspd_time_t t1 = 0, t2 = 0;
  int32_t ret = -EBADFD, r1, r2;
  uint32_t avail_min, a;
  if ( avail )
    {
      a = *avail;
      if ( a < (client->fragsize / 2) )
	a = client->fragsize / 2;
    } else
    {
      a = client->fragsize;
    }
  

  if ( client->swparams.avail_min < client->fragsize )
    avail_min = client->fragsize;
  else
    avail_min = client->swparams.avail_min;
  avail_min = MIN(avail_min, a);


  if ( (client->streams & DSPD_PCM_SBIT_FULLDUPLEX) == DSPD_PCM_SBIT_FULLDUPLEX &&
       (*streams) == DSPD_PCM_SBIT_FULLDUPLEX )
    {
      ret = 0;
      (*streams) = 0;
      r1 = dspd_pcmcli_stream_get_next_wakeup(&client->playback.stream, NULL, avail_min, &t1);
      r2 = dspd_pcmcli_stream_get_next_wakeup(&client->capture.stream, NULL, avail_min, &t2);
      if ( r1 == PCMCS_WAKEUP_NOW )
	{
	  //Already time to wake up
	  r1 = 0;
	  ret = 0;
	  (*streams) |= DSPD_PCM_SBIT_PLAYBACK;
	} else if ( r1 == PCMCS_WAKEUP_NONE )
	{
	  //Nothing to wait on
	  r1 = 0;
	  ret = 0;
	}
      if ( r2 == PCMCS_WAKEUP_NOW )
	{
	  r2 = 0;
	  ret = 0;
	  (*streams) |= DSPD_PCM_SBIT_CAPTURE;
	} else if ( r2 == PCMCS_WAKEUP_NONE )
	{
	  r2 = 0;
	  ret = 0;
	}
      if ( r1 < 0 )
	ret = r1;
      else if ( r2 < 0 )
	ret = r2;
      *next = MIN(t1, t2);
    } else if ( ((*streams) & client->streams) == DSPD_PCM_SBIT_PLAYBACK )
    {
      (*streams) = 0;
      ret = dspd_pcmcli_stream_get_next_wakeup(&client->playback.stream, NULL, avail_min, next);
      if ( ret == PCMCS_WAKEUP_NOW )
	{
	  ret = 0;
	  (*streams) = DSPD_PCM_SBIT_PLAYBACK;
	} else if ( ret == PCMCS_WAKEUP_NONE )
	{
	  ret = 0;
	}
    } else if ( ((*streams) & client->streams) == DSPD_PCM_SBIT_CAPTURE )
    {
      (*streams) = 0;
      ret = dspd_pcmcli_stream_get_next_wakeup(&client->capture.stream, NULL, avail_min, next);
      if ( ret == PCMCS_WAKEUP_NOW )
	{
	  ret = 0;
	  (*streams) = DSPD_PCM_SBIT_CAPTURE;
	} else if ( ret == PCMCS_WAKEUP_NONE )
	{
	  ret = 0;
	}
    } else
    {
      ret = -EINVAL;
    }
  return ret;
}





int32_t dspd_pcmcli_wait(struct dspd_pcmcli *client, int32_t streams, uint32_t avail, bool async)
{
  int32_t ret = 0, s = streams;
  dspd_time_t next;
  size_t i;
  if ( client->nfds < 2 )
    return -EAGAIN;
  if ( avail == 0 )
    avail = client->swparams.avail_min;
  if ( client->error )
    {
      ret = client->error;
    } else if ( streams == 0 )
    {
      ret = client->timer_ops->reset(client->timer_arg);
    } else
    {
      ret = dspd_pcmcli_get_next_wakeup(client, &avail, &s, &next);
      if ( ret == 0 )
	{
	  next += client->clockdiff;
	  if ( s & streams )
	    {
	      //Streams are ready now
	      ret = client->timer_ops->fire(client->timer_arg, false);
	    } else
	    {
	      //Streams will be ready later
	      ret = client->timer_ops->set(client->timer_arg, next, client->fragtime);
	      if ( ret == 0 )
		{
		  if ( async )
		    {
		      //Remember this later because internal code borrows the pollfds.
		      client->wait_streams = streams;
		      ret = poll(client->pfds, client->nfds, 0);
		    } else
		    {
		      ret = poll(client->pfds, client->nfds, -1);
		    }
		  if ( ret < 0 )
		    {
		      ret = errno;
		      if ( ret == EINTR || ret == EAGAIN )
			ret = 0;
		      else
			ret *= -1;
		    } else if ( ret > 0 )
		    {
		      //Find errors
		      ret = 0;
		      for ( i = 0; i < client->nfds; i++ )
			{
			  if ( client->pfds[i].revents & DSPD_POLLERR )
			    {
			      ret = -EIO;
			      break;
			    } else if ( client->pfds[i].fd == client->connfd && (client->pfds[i].revents & POLLIN) )
			    {
				 
			      ret = dspd_pcmcli_process_io(client, 0, 0);
			      if ( ret < 0 || (dspd_aio_revents(client->conn) & DSPD_POLLERR) )
				ret = -EIO;
			    }
			}
		    }
		  if ( client->connfd < 0 )
		    {
		      ret = dspd_pcmcli_process_io(client, 0, 0);
		      if ( ret < 0 || (dspd_aio_revents(client->conn) & DSPD_POLLERR) )
			ret = -EIO;
		    }
		    
		}
	    }
	}
    }
  if ( ret < 0 && ret != -EAGAIN && ret != -EINVAL )
    {
      client->error = ret;
      client->timer_ops->fire(client->timer_arg, false);
    }
  return ret;
}

void dspd_pcmcli_restore_wait(struct dspd_pcmcli *client)
{
  if ( client->wait_streams )
    dspd_pcmcli_wait(client, client->wait_streams, 0, true);
}


int32_t dspd_pcmcli_get_pollfd(struct dspd_pcmcli *client, struct pollfd *pfds, size_t nfds, int32_t events)
{
  int32_t ret = 0;
  int32_t streams = 0;
  size_t i;
  if ( (events & POLLIN) && (client->streams & DSPD_PCM_SBIT_CAPTURE) )
    streams |= DSPD_PCM_SBIT_CAPTURE;
  if ( (events & POLLOUT) && (client->streams & DSPD_PCM_SBIT_PLAYBACK) )
    streams |= DSPD_PCM_SBIT_PLAYBACK;
  //if ( client->state == PCMCLI_STATE_RUNNING )
  ret = dspd_pcmcli_wait(client, streams, 0, true);
  if ( ret == 0 )
    {
      for ( i = 0; i < client->nfds && i < nfds; i++ )
	pfds[i] = client->pfds[i];
      ret = i;
    }

  return ret;
}

int32_t dspd_pcmcli_pollfd_count(struct dspd_pcmcli *client)
{
  return client->nfds;
}

int32_t dspd_pcmcli_pollfd_revents(struct dspd_pcmcli *client, const struct pollfd *pfds, size_t nfds, int32_t *revents)
{
  int32_t ret = 0, err;
  size_t i;
  uint32_t avail;
  bool reset_timer = false;
  uint64_t timer_pval = UINT64_MAX, timer_nval = UINT64_MAX;
  dspd_time_t n, next_exp = 0;
  uint32_t p;
  (void)client->timer_ops->get(client->timer_arg, &next_exp, &p);
  *revents = 0;
  
  if ( client->nfds > 0 )
    {
      for ( i = 0; i < nfds; i++ )
	{
	  if ( pfds[i].fd == client->connfd )
	    {
	      if ( pfds[i].revents & DSPD_POLLERR )
		(*revents) |= POLLHUP;
	      err = dspd_pcmcli_process_io(client, 0, 0);
	      if ( err < 0 || (dspd_aio_revents(client->conn) & DSPD_POLLERR) )
		(*revents) |= POLLHUP;
	    } else if ( pfds[i].fd == client->timer.fd )
	    {
	      if ( pfds[i].revents & DSPD_POLLERR )
		(*revents) |= POLLHUP;
	    }
	}
    }
  if ( client->error != 0 )
    {
      (*revents) |= POLLERR;
    } else if ( client->state == PCMCLI_STATE_XRUN )
    {
      if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
	(*revents) |= POLLOUT;
      if ( client->streams & DSPD_PCM_SBIT_CAPTURE )
	(*revents) |= POLLIN;
    }

  if ( client->state == PCMCLI_STATE_PREPARED || client->state == PCMCLI_STATE_RUNNING )
    {
      if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
	{
	  ret = dspd_pcmcli_stream_avail(&client->playback.stream, NULL, NULL);
	  if ( ret >= 0 )
	    {
	      avail = ret;
	      //This is a separate poll threshold for reporting revents.  This is because ALSA will busy wait
	      //instead of just filling the buffer if avail < avail_min.  Other APIs aren't so picky so it makes
	      //sense to report any time data can be transferred and service the buffer as soon as possible to
	      //prevent xruns.
	      if ( avail >= client->poll_threshold )
		{
		  if ( client->poll_threshold > 1 && next_exp == client->playback.stream.next_wakeup )
		    {
		      n = avail - client->poll_threshold;
		      if ( n > 1 )
			timer_nval = n * client->playback.stream.sample_time;
		    }
		  (*revents) |= POLLOUT;
		} else if ( avail > 0 && client->playback.stream.got_status == true )
		{
		  timer_pval = client->playback.stream.sample_time * (client->poll_threshold - avail);
		  reset_timer = true;
		}
	    } else if ( ret == -EPIPE )
	    {
	      (*revents) |= POLLOUT;
	    }
	}
      if ( client->streams & DSPD_PCM_SBIT_CAPTURE )
	{
	  ret = dspd_pcmcli_stream_avail(&client->capture.stream, NULL, NULL);
	  if ( ret >= 0 )
	    {
	      avail = ret;
	      if ( avail >= client->poll_threshold )
		{
		  (*revents) |= POLLIN;
		  if ( client->poll_threshold > 1 && next_exp == client->capture.stream.next_wakeup && client->capture.stream.got_status == true )
		    {
		      n = avail - client->poll_threshold;
		      if ( n > 1 )
			timer_nval = MIN(timer_nval, n * client->capture.stream.sample_time);
		    }

		} else if ( avail > 0 )
		{
		  n = client->capture.stream.sample_time * (client->poll_threshold - avail);
		  if ( n < timer_pval )
		    {
		      timer_pval = n;
		      reset_timer = true;
		    }
		}
	    } else if ( ret == -EPIPE )
	    {
	      (*revents) |= POLLIN;
	    }
	}
    }
  
  
 
  if ( timer_nval != UINT64_MAX )
    client->clockdiff -= timer_nval;
  if ( timer_pval != UINT64_MAX )
    client->clockdiff += timer_pval;
  if ( client->clockdiff > client->max_clockdiff )
    client->clockdiff = client->max_clockdiff;
  else if ( client->clockdiff < client->min_clockdiff )
    client->clockdiff = client->min_clockdiff;
  if ( reset_timer )
    client->timer_ops->set(client->timer_arg, timer_pval + next_exp, client->fragtime);
  
  return 0;
}

ssize_t dspd_pcmcli_write_frames(struct dspd_pcmcli *client,
				   const void *data,
				   size_t frames)
{
  ssize_t ret;
  size_t offset;
  bool waited = false;
  uint32_t wtime;
  size_t len;
  struct dspd_pcmcli_status status;
  if ( client->error )
    {
      ret = client->error;
    } else if ( client->state == PCMCLI_STATE_XRUN )
    {
      ret = -EPIPE;
    } else if ( client->state == PCMCLI_STATE_SUSPENDED )
    {
      ret = -ESTRPIPE;
    } else if ( client->state != PCMCLI_STATE_PREPARED && client->state != PCMCLI_STATE_RUNNING )
    {
      ret = -EBADFD;
    } else if ( client->playback.enabled == false )
    {
      ret = -EBADF;
    } else
    {
      if ( client->no_xrun == true || (ret = dspd_pcmcli_stream_check_xrun(&client->playback.stream)) == 0 )
	{
	  offset = 0;
	  wtime = client->playback.stream.params.latency / 2;
	  bool nonblocking = client->nonblocking || (client->state != PCMCLI_STATE_RUNNING);
	  while ( offset < frames )
	    {
	      if ( client->constant_latency )
		{
		  ret = dspd_pcmcli_get_status(client, DSPD_PCM_SBIT_PLAYBACK, true, &status);
		  if ( status.avail > 0 )
		    {
		      len = frames - offset;
		      if ( len > status.avail )
			len = status.avail;
		      
		      ret = dspd_pcmcli_stream_write(&client->playback.stream, 
						(const char*)data+(offset*client->playback.stream.framesize), 
						len);
		    }
		  
		} else
		{
		  ret = dspd_pcmcli_stream_write(&client->playback.stream, 
						 (const char*)data+(offset*client->playback.stream.framesize), 
						 frames - offset);
		}
	      if ( ret < 0 )
		{
		  if ( ret != -EAGAIN )
		    break;
		  ret = 0;
		}
	      offset += ret;
	      client->playback.xfer += ret;
	      //Can't block if not running
	      if ( nonblocking || offset > 0 )
		break;
	      if ( offset < frames )
		{
		  waited = true;
		  ret = dspd_pcmcli_wait(client, DSPD_PCM_SBIT_PLAYBACK, wtime, false);
		  if ( ret < 0 )
		    break;
		  if ( client->no_xrun == false )
		    {
		      ret = dspd_pcmcli_stream_check_xrun(&client->playback.stream);
		      if ( ret )
			break;
		    }
		}
	    }
	  if ( ret >= 0 )
	    {
	      if ( offset > 0 )
		ret = offset;
	      else
		ret = -EAGAIN;
	    }
	  if ( waited || ret > 0 || (ret < 0 && dspd_fatal_err(ret)) )
	    dspd_pcmcli_restore_wait(client);
	} 
      if ( ret < 0 && ret != -EAGAIN )
	{
	  client->error = ret;
	  if ( client->error == -EPIPE )
	    client->state = PCMCLI_STATE_XRUN;
	}
    }
  if ( ret < 0 )
    set_error(client, ret);
  

  return ret;
}


ssize_t dspd_pcmcli_write_bytes(struct dspd_pcmcli *client,
				  const void *data,
				  size_t bytes)
{
  ssize_t ret = 0;
  size_t offset = 0, n;
  if ( client->error )
    {
      ret = client->error;
    } else if ( client->playback.enabled == false )
    {
      ret = -EBADF;
    } else if ( client->playback.stream.framesize == 0 )
    {
      ret = -EBADFD;
    } else
    {
      //Fill remaining frame bytes if necessary
      if ( client->playback.frame_off > 0 && client->playback.frame_off < client->playback.stream.framesize )
	{
	  assert(client->playback.frame_buf != NULL);
	  n = client->playback.stream.framesize - client->playback.frame_off;
	  if ( n > bytes )
	    n = bytes;
	  memcpy(client->playback.frame_buf+client->playback.frame_off,
		 data,
		 n);
	  offset += n;
	  client->playback.frame_off += n;
	}

      //Write the buffer if it is full.
      if ( client->playback.frame_off == client->playback.stream.framesize )
	{
	  assert(client->playback.frame_buf != NULL);
	  ret = dspd_pcmcli_write_frames(client, client->playback.frame_buf, 1);
	  if ( ret == 1 )
	    {
	      client->playback.frame_off = 0;
	      ret = 0;
	    }
	}

      //Write remaining bytes if there is enough left over
      if ( ret == 0 )
	{
	  n = (bytes - offset) / client->playback.stream.framesize;
	  if ( n > 0 )
	    {
	      ret = dspd_pcmcli_write_frames(client, (const char*)data+offset, n);
	      if ( ret > 0 )
		offset += (ret * client->playback.stream.framesize);
	    }
	}

      //Save bytes if less than one frame, otherwise it was already going to be a short write.  Handling partial
      //frames has extra overhead so it should be avoided.
      if ( ret >= 0 )
	{
	  n = bytes - offset;
	  //It is possible that the frame_buf might be NULL if partial writes are not enabled.
	  if ( n < client->playback.stream.framesize && client->playback.frame_buf != NULL && client->playback.frame_off == 0 )
	    {
	      memcpy(client->playback.frame_buf, (const char*)data+offset, n);
	      client->playback.frame_off = n;
	      offset += n;
	    }
	}
      assert(offset <= bytes);
      if ( offset > 0 )
	ret = offset;
      if ( ret == 0 )
	ret = -EAGAIN;
    }
  if ( ret < 0 )
    set_error(client, ret);
  return ret;
}





ssize_t dspd_pcmcli_read_frames(struct dspd_pcmcli *client,
				  void *data,
				  size_t frames)
{
  ssize_t ret;
  size_t offset;
  bool waited = false;
  uint32_t wtime;
  if ( client->error )
    {
      ret = client->error;
    } else if ( client->state == PCMCLI_STATE_XRUN )
    {
      ret = -EPIPE;
    } else if ( client->state == PCMCLI_STATE_SUSPENDED )
    {
      ret = -ESTRPIPE;
    } else if ( client->state != PCMCLI_STATE_PREPARED && client->state != PCMCLI_STATE_RUNNING )
    {
      ret = -EBADFD;
    } else if ( client->capture.enabled == false )
    {
      ret = -EBADF;
    } else
    {
      bool nonblocking = client->nonblocking || (client->state != PCMCLI_STATE_RUNNING);
      if ( client->no_xrun == true || (ret = dspd_pcmcli_stream_check_xrun(&client->capture.stream)) == 0 )
	{
	  offset = 0;
	  wtime = client->fragsize;
	  while ( offset < frames )
	    {
	      ret = dspd_pcmcli_stream_read(&client->capture.stream, 
					    (char*)data+(offset*client->capture.stream.framesize), 
					    frames - offset);
	      
	      if ( ret < 0 )
		break;
	      offset += ret;
	      client->capture.xfer += ret;
	      if ( nonblocking )
		break;
	      if ( offset < frames )
		{
		  waited = true;
		  ret = dspd_pcmcli_wait(client, DSPD_PCM_SBIT_CAPTURE, wtime, false);
		  if ( ret < 0 )
		    break;
		}
	    }
	  if ( ret < 0 && ret != -EAGAIN )
	    client->error = ret;
	  if ( offset > 0 )
	    ret = offset;
	  if ( ret == 0 )
	    ret = -EAGAIN;
	  if ( waited || ret > 0 || (ret < 0 && dspd_fatal_err(ret)) )
	    dspd_pcmcli_restore_wait(client);
	} else
	{
	  if ( ret < 0 && ret != -EAGAIN )
	    client->error = ret;
	}
    }
  if ( ret < 0 )
    set_error(client, ret);
  return ret;
}


ssize_t dspd_pcmcli_read_bytes(struct dspd_pcmcli *client,
				 void *data,
				 size_t bytes)
{
  ssize_t ret = 0;
  size_t offset = 0, n;
  if ( client->error )
    {
      ret = client->error;
    } else if ( client->capture.enabled == false )
    {
      ret = -EBADF;
    } else if ( client->capture.stream.framesize == 0 )
    {
      ret = -EBADFD;
    } else
    {
      //The offset is always nonzero if there is extra data available because the
      //buffer is only used if a partial frame must be filled.
      if ( client->capture.frame_off > 0 )
	{
	  assert(client->capture.frame_buf != NULL);
	  n = client->capture.stream.framesize - client->capture.frame_off;
	  if ( n > bytes )
	    n = bytes;
	  memcpy(data, 
		 client->capture.frame_buf+client->capture.frame_off,
		 n);
	  client->capture.frame_off += n;
	  client->capture.frame_off %= client->capture.stream.framesize;
	  offset += n;
	}
      n = (bytes - offset) / client->capture.stream.framesize;
      if ( n > 0 )
	{
	  assert(client->capture.frame_off == 0);
	  ret = dspd_pcmcli_stream_read(&client->capture.stream, 
					(char*)data + offset,
					n);
	  if ( ret > 0 )
	    offset += (ret * client->capture.stream.framesize);
	}
      if ( offset < bytes && client->capture.frame_buf != NULL && ret >= 0 && client->capture.frame_off == 0 )
	{
	  n = bytes - offset;
	  //The caller requested a partial read
	  if ( n < client->capture.stream.framesize )
	    {
	      //A small number of bytes will be copied if any data is available.
	      ret = dspd_pcmcli_stream_read(&client->capture.stream,
				       client->capture.frame_buf,
				       1);
	      if ( ret == 1 )
		{
		  memcpy(data+offset, client->capture.frame_buf, n);
		  //Now the offset is nonzero.
		  client->capture.frame_off += n;
		  offset += n;
		}
	    }
	}
      if ( offset > 0 )
	ret = offset;
      if ( ret == 0 )
	ret = -EAGAIN;      
    }
  if ( ret < 0 )
    set_error(client, ret);
  return ret;
}


int32_t dspd_pcmcli_get_status(struct dspd_pcmcli *client, int32_t stream, bool hwsync, struct dspd_pcmcli_status *status)
{
  int32_t ret;
  struct pcmcli_stream_data *s;
  dspd_time_t now, diff;
  if ( client->error )
    {
      ret = client->error;
    } else if ( stream != DSPD_PCM_SBIT_FULLDUPLEX && (stream & client->streams) )
    {
      if ( client->state > PCMCLI_STATE_SETUP )
	{
	  if ( stream == DSPD_PCM_SBIT_PLAYBACK && client->playback.enabled )
	    s = &client->playback;
	  else if ( stream == DSPD_PCM_SBIT_CAPTURE && client->capture.enabled )
	    s = &client->capture;
	  else
	    return -EBADFD;
	  
	  ret = dspd_pcmcli_stream_status(&s->stream, status, hwsync);
	  if ( ret == 0 && status != NULL )
	    {
	      now = dspd_get_time();
	      if ( now > status->delay_tstamp )
		{
		  diff = (now - status->delay_tstamp) / s->stream.sample_time;
		  if ( stream == DSPD_PCM_SBIT_PLAYBACK )
		    {
		      status->delay -= diff;
		      if ( client->constant_latency )
			{
			  if (  status->delay < client->playback.stream.params.bufsize )
			    status->avail = client->playback.stream.params.bufsize - status->delay;
			  else
			    status->avail = 0;
			}
		    } else
		    {
		      status->delay += diff;
		    }
		  if ( status->error == 0 && status->avail >= s->stream.params.bufsize && client->no_xrun == false )
		    status->error = -EPIPE;
		}
	    }
	} else
	{
	  ret = -EBADFD;
	}
    } else
    {
      ret = -EINVAL;
    }
  if ( ret < 0 )
    set_error(client, ret);
  return ret;
}

int32_t dspd_pcmcli_avail(struct dspd_pcmcli *client, int32_t stream, uint64_t *hw_ptr, uint64_t *appl_ptr)
{
  struct dspd_pcmcli_status status;
  int32_t ret;
  if ( ! (stream & client->streams) )
    {
      ret = -EBADF;
    } else if ( client->state < PCMCLI_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( client->state == PCMCLI_STATE_XRUN )
    {
      ret = -EPIPE;
    } else if ( stream == DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( ! client->constant_latency )
	{
	  ret = dspd_pcmcli_stream_avail(&client->playback.stream, hw_ptr, appl_ptr);
	  if ( client->state == PCMCLI_STATE_RUNNING && 
	       client->no_xrun == false && 
	       ret >= client->playback.stream.params.bufsize )
	    ret = -EPIPE;
	} else
	{
	  ret = dspd_pcmcli_get_status(client, stream, false, &status);
	  if ( ret == 0 )
	    ret = status.avail;
	  if ( hw_ptr )
	    *hw_ptr = status.hw_ptr;
	  if ( appl_ptr )
	    *appl_ptr = status.appl_ptr;
	}
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE )
    {
      ret = dspd_pcmcli_stream_avail(&client->capture.stream, hw_ptr, appl_ptr);
      if ( client->state == PCMCLI_STATE_RUNNING && 
	   client->no_xrun == false && 
	   ret >= client->swparams.stop_threshold )
	ret = -EPIPE;
    } else
    {
      ret = -EINVAL;
    }
  if ( ret < 0 )
    set_error(client, ret);
  return ret;
}





static void bind_complete_cb(void *context, struct dspd_async_op *op)
{
  struct dspd_pcmcli *cli = op->data;
  if ( op->error == 0 )
    cli->state = PCMCLI_STATE_OPEN;
  complete_event(cli, op->error);
}

int32_t dspd_pcmcli_bind(struct dspd_pcmcli *client, const struct dspd_pcmcli_bindparams *params, bool autoclose, dspd_aio_ccb_t complete, void *data)
{
  int32_t ret = 0, bits, *s;
  if ( client->state > PCMCLI_STATE_INIT )
    {
      ret = -EBUSY;
    } else
    {
      if ( (params->playback_stream >= 0 && 
	    params->playback_device >= 0 && 
	    (client->streams & DSPD_PCM_SBIT_PLAYBACK) == 0) ||
	   (params->capture_stream >= 0 && 
	    params->capture_device >= 0 && 
	    (client->streams & DSPD_PCM_SBIT_CAPTURE) == 0) ||
	   (params->context != NULL && client->conn != NULL) )
	{
	  ret = -EINVAL;
	} else 
	{
	  if ( params->playback_stream >= 0 && params->playback_device >= 0 )
	    {
	      client->playback.device_idx = params->playback_device;
	      client->playback.stream_idx = params->playback_stream;
	    }
	  if ( params->capture_stream >= 0 && params->capture_device >= 0 )
	    {
	      client->capture.device_idx = params->capture_device;
	      client->capture.stream_idx = params->capture_stream;
	    }
	  if ( params->context != NULL )
	    {
	      client->conn = params->context;
	      client->nfds = 0;
	      client->close = autoclose;
	      client->connfd = dspd_aio_get_iofd(client->conn);
	      if ( client->connfd >= 0 )
		{
		  client->pfds[0].fd = client->connfd;
		  client->pfds[0].events = POLLIN;
		  client->pfds[0].revents = 0;
		  client->nfds++;
		}
	      if ( client->timer.fd >= 0 )
		{
		  client->pfds[client->nfds].fd = client->timer.fd;
		  client->pfds[client->nfds].events = POLLIN;
		  client->pfds[client->nfds].revents = 0;
		  client->nfds++;
		}
	    }
	  bits = 0;
	  if ( client->playback.device_idx >= 0 && client->playback.stream_idx >= 0 )
	    bits |= DSPD_PCM_SBIT_PLAYBACK;
	  if ( client->capture.device_idx >= 0 && client->capture.stream_idx  >= 0 )
	    bits |= DSPD_PCM_SBIT_CAPTURE;
	  //It isn't open until all streams are properly bound
	  if ( bits == client->streams && client->conn != NULL )
	    {
	      s = (int32_t*)client->input;
	      *s = -1;
	      ret = submit_io(client,
			      DSPD_SCTL_CLIENT_RESERVE,
			      s,
			      sizeof(*s),
			      NULL,
			      0,
			      bind_complete_cb);
	      if ( ret == 0 )
		{
		  if ( complete )
		    {
		      client->complete = complete;
		      client->completion_data = data;
		      ret = -EINPROGRESS;
		    } else
		    {
		      ret = wait_for_io(client);
		    }
		}
	    }
	  
	}
    }
  return ret;
}


void dspd_pcmcli_unbind(struct dspd_pcmcli *client)
{
  if ( client->close && client->conn )
    {
      //The server will do the cleanup without any additional commands.
      dspd_aio_delete(client->conn);
      client->conn = NULL;
    }
  if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      client->playback.stream_idx = -1;
      client->playback.device_idx = -1;
      dspd_pcmcli_stream_detach(&client->playback.stream);
      dspd_shm_close2(&client->playback.shm, true);
    }
  if ( client->streams & DSPD_PCM_SBIT_CAPTURE )
    {
      client->capture.stream_idx = -1;
      client->capture.device_idx = -1;
      dspd_pcmcli_stream_detach(&client->capture.stream);
      dspd_shm_close2(&client->capture.shm, true);
    }
}


static void prepare_complete_cb(void *context, struct dspd_async_op *op)
{
  struct dspd_pcmcli *client = op->data;
  uint32_t st = 1;
  client->error = op->error;
  if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      dspd_pcmcli_stream_reset(&client->playback.stream);
      st = client->playback.stream.sample_time;
    }
  if ( client->streams & DSPD_PCM_SBIT_CAPTURE )
    {
      dspd_pcmcli_stream_reset(&client->capture.stream);
      st = client->capture.stream.sample_time;
    }
  client->playback.xfer = 0;
  client->capture.xfer = 0;
  client->lasthw = 0;
  client->state = PCMCLI_STATE_PREPARED;
  client->wait_streams = 0;
  client->playback.frame_off = 0;
  client->capture.frame_off = 0;
  client->clockdiff = 0;
  
  client->max_clockdiff = MAX(st, client->fragtime / 10);
  client->min_clockdiff = client->max_clockdiff * -1;
  
  complete_event(client, op->error);
}


int32_t dspd_pcmcli_prepare(struct dspd_pcmcli *client, dspd_aio_ccb_t complete, void *data)
{
  int32_t ret;
  if ( client->error < 0 && client->error != -EPIPE )
    {
      ret = client->error;
    } else if ( client->state >= PCMCLI_STATE_SETUP )
    {
      ret = submit_io(client,
		      DSPD_SCTL_CLIENT_STOP,
		      &client->streams,
		      sizeof(client->streams),
		      NULL,
		      0,
		      prepare_complete_cb);
      ret = complete_io(client, ret, complete, data);
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}



static int32_t map_stream(struct pcmcli_stream_data *stream, int32_t sbit, const struct dspd_cli_params *hwparams, const struct dspd_client_shm *shm, int32_t fd)
{
  int32_t ret;
  uint64_t p;
  dspd_shm_close2(&stream->shm, true);
  if ( shm->flags & DSPD_SHM_FLAG_PRIVATE )
    {
      if ( sizeof(uintptr_t) == 8U )
	{
	  p = (uint32_t)shm->reserved;
	  p <<= 32U;
	} else
	{
	  p = 0;
	}
      p |= (uint32_t)shm->arg;
      stream->shm.addr = (struct dspd_shm_header*)(uintptr_t)p;
    } else
    {
      stream->shm.arg = shm->arg;
    }
  stream->shm.key = shm->key;
  stream->shm.flags = shm->flags;
  stream->shm.length = shm->len;
  stream->shm.section_count = shm->section_count;
  
  ret = dspd_shm_attach(&stream->shm);

  if ( ret < 0 && fd >= 0 )
    {
      close(fd);
    } else if ( ret == 0 )
    {
      dspd_pcmcli_stream_detach(&stream->stream);
      ret = dspd_pcmcli_stream_attach(&stream->stream, hwparams, &stream->shm);
    }


  if ( ret < 0 )
    {
      dspd_pcmcli_stream_detach(&stream->stream);
      dspd_shm_close2(&stream->shm, true);
    } else
    {
      dspd_shm_close2(&stream->shm, false);
    }
  return ret;
}

int32_t dspd_pcmcli_set_hwparams(struct dspd_pcmcli *client, 
				   const struct dspd_cli_params *hwparams, 
				   const struct dspd_client_shm *playback_shm,
				   const struct dspd_client_shm *capture_shm,
				   bool sync)
{
  struct dspd_cli_params hwp, out;
  struct dspd_client_shm cshm, pshm;
  int32_t ret = 0;
  int32_t s, pfd = -1, cfd = -1;
  size_t br;
  if ( ((hwparams->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS) == 0 && (client->streams & DSPD_PCM_SBIT_FULLDUPLEX) == DSPD_PCM_SBIT_FULLDUPLEX) ||
       hwparams->stream != client->streams )
    return -EINVAL;
  if ( client->state > PCMCLI_STATE_PREPARED )
    return -EBADFD;

  if ( (hwparams->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS) == 0 )
    {
      hwp = *hwparams;
      hwp.channels = (hwparams->channels << 16U) | hwparams->channels;
      hwp.xflags |= DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
      hwparams = &hwp;
    }
  if ( sync )
    {
      ret = dspd_stream_ctl(client->conn,
			    -1,
			    DSPD_SCTL_CLIENT_SETPARAMS,
			    hwparams,
			    sizeof(*hwparams),
			    &out,
			    sizeof(out),
			    &br);
      
      if ( ret == 0 )
	{
	  if ( br != sizeof(out) )
	    ret = -EPROTO;
	  else
	    hwparams = &out;
	}

    }
  if ( ret == 0 && (client->streams & DSPD_PCM_SBIT_PLAYBACK) && playback_shm == NULL )
    {
      s = DSPD_PCM_SBIT_PLAYBACK;
      ret = dspd_stream_ctl(client->conn,
			    -1,
			    DSPD_SCTL_CLIENT_MAPBUF,
			    &s,
			    sizeof(s),
			    &pshm,
			    sizeof(pshm),
			    &br);
      
      if ( ret == 0 )
	{
	  if ( br != sizeof(pshm) )
	    {
	      ret = -EPROTO;
	    } else if ( pshm.flags & DSPD_SHM_FLAG_MMAP )
	    {
	      ret = dspd_aio_recv_fd(client->conn);
	      if ( ret > 0 )
		{
		  pfd = ret;
		  ret = 0;
		}
	    }
	}
    }
  if ( ret == 0 && (client->streams & DSPD_PCM_SBIT_CAPTURE) && capture_shm == NULL )
    {
      s = DSPD_PCM_SBIT_CAPTURE;
      ret = dspd_stream_ctl(client->conn,
			    -1,
			    DSPD_SCTL_CLIENT_MAPBUF,
			    &s,
			    sizeof(s),
			    &cshm,
			    sizeof(cshm),
			    &br);
      if ( ret == 0 )
	{
	  if ( br != sizeof(pshm) )
	    {
	      ret = -EPROTO;
	    } else if ( cshm.flags & DSPD_SHM_FLAG_MMAP )
	    {
	      ret = dspd_aio_recv_fd(client->conn);
	      if ( ret > 0 )
		{
		  cfd = ret;
		  ret = 0;
		}
	    }
	}
    }
 
  if ( ret == 0 )
    {
      if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
	{
	  ret = map_stream(&client->playback, DSPD_PCM_SBIT_PLAYBACK, &out, &pshm, pfd);
	  pfd = -1;
	}
      if ( ret == 0 && (client->streams & DSPD_PCM_SBIT_CAPTURE) )
	{
	  
	  ret = map_stream(&client->capture, DSPD_PCM_SBIT_CAPTURE, &out, &cshm, cfd);

	  cfd = -1;
	}
      if ( ret == 0 )
	{
	  client->fragsize = hwparams->fragsize;
	  client->swparams.stop_threshold = hwparams->bufsize;
	  s = -1;
	  ret = dspd_stream_ctl(client->conn, -1, DSPD_SCTL_CLIENT_CONNECT, &s, sizeof(s), NULL, 0, &br);
	}
    }
  if ( pfd >= 0 )
    close(pfd);
  if ( cfd >= 0 )
    close(cfd);
  if ( ret == 0 )
    client->state = PCMCLI_STATE_SETUP;
  else
    client->state = PCMCLI_STATE_OPEN;
  return ret;
}

int32_t dspd_pcmcli_get_hwparams(struct dspd_pcmcli *client, struct dspd_cli_params *hwparams)
{
  int32_t ret;
  const struct dspd_cli_params *p, *c;
  if ( client->state < PCMCLI_STATE_SETUP )
    {
      ret = -EBADFD;
    } else if ( client->error )
    {
      ret = client->error;
    } else
    {
      ret = 0;
      if ( client->streams == DSPD_PCM_SBIT_FULLDUPLEX )
	{
	  p = &client->playback.stream.params;
	  c = &client->playback.stream.params;
	  *hwparams = *p;
	  hwparams->channels = 0;
	  hwparams->xflags |= DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
	  hwparams->channels |= p->channels & 0x0000FFFFU;
	  hwparams->stream = DSPD_PCM_SBIT_FULLDUPLEX;
	  hwparams->bufsize = MAX(c->bufsize, p->bufsize);
	  hwparams->fragsize = MAX(c->fragsize, p->bufsize);
	  hwparams->latency = MAX(c->latency, p->latency);
	  if ( c->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
	    hwparams->channels |= c->channels & 0x0000FFFFU;
	  else
	    hwparams->channels |= c->channels << 16U;
	} else if ( client->streams == DSPD_PCM_SBIT_PLAYBACK )
	{
	  *hwparams = client->playback.stream.params;
	} else if ( client->streams == DSPD_PCM_SBIT_CAPTURE )
	{
	  *hwparams = client->capture.stream.params;
	} else
	{
	  ret = -EBADFD;
	}
    }
  return ret;
}

static void swparams_complete(void *context, struct dspd_async_op *op)
{
  struct dspd_pcmcli *cli = op->data;
  if ( op->error == 0 )
    {
      if ( op->xfer > 0 )
	{
	  memmove(&cli->swparams, op->outbuf, op->xfer);
	  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	    cli->playback.stream.xrun_threshold = cli->swparams.stop_threshold;
	  if ( cli->streams & DSPD_PCM_SBIT_CAPTURE )
	    cli->capture.stream.xrun_threshold = cli->swparams.stop_threshold;
	}
    }
  complete_event(cli, op->error);
}

int32_t dspd_pcmcli_set_swparams(struct dspd_pcmcli *client, 
				 const struct dspd_rclient_swparams *swparams, 
				 bool sync, 
				 dspd_aio_ccb_t complete,
				 void *data)
{
  int32_t ret;
  if ( client->state >= PCMCLI_STATE_SETUP )
    {
      if ( sync )
	{
	  ret = submit_io(client, 
			  DSPD_SCTL_CLIENT_SWPARAMS,
			  swparams,
			  sizeof(*swparams),
			  client->output,
			  sizeof(*swparams),
			  swparams_complete);
	  ret = complete_io(client, ret, complete, data);
	} else
	{
	  if ( complete && client->complete )
	    {
	      ret = -EBUSY;
	    } else
	    {
	      memmove(&client->swparams, swparams, sizeof(*swparams));
	      if ( complete )
		{
		  //Start a dummy op to make it complete asynchronously
		  client->complete = complete;
		  client->completion_data = data;
		  client->dummy_op = true;
		  client->dummy_result = 0;
		  memset(&client->pending_op, 0, sizeof(client->pending_op));
		  client->pending_op.error = 0;
		  client->pending_op.inbuf = swparams;
		  client->pending_op.inbufsize = sizeof(*swparams);
		  client->pending_op.outbuf = &client->swparams;
		  client->pending_op.outbufsize = sizeof(client->swparams);
		  client->pending_op.xfer = sizeof(client->swparams);
		  ret = -EINPROGRESS;
		} else
		{
		  ret = 0;
		}
	    }
	}
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}

int32_t dspd_pcmcli_get_swparams(struct dspd_pcmcli *client, struct dspd_rclient_swparams *swparams)
{
  int32_t ret;
  if ( client->state == PCMCLI_STATE_SETUP )
    {
      *swparams = client->swparams;
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}

static void channelmap_complete(void *context, struct dspd_async_op *op)
{
  struct dspd_pcmcli *cli = op->data;
  complete_event(cli, op->error);
}


int32_t dspd_pcmcli_set_channelmap(struct dspd_pcmcli *client, 
				   const struct dspd_pcm_chmap *chmap, 
				   bool sync, 
				   dspd_aio_ccb_t complete,
				   void *data)
{
  int32_t ret;
  if ( client->state == PCMCLI_STATE_SETUP )
    {
      ret = submit_io(client, 
		      DSPD_SCTL_CLIENT_SETCHANNELMAP,
		      chmap,
		      dspd_pcm_chmap_sizeof(chmap->count, chmap->flags),
		      NULL,
		      0,
		      channelmap_complete);
      ret = complete_io(client, ret, complete, data);
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}


int32_t dspd_pcmcli_get_channelmap(struct dspd_pcmcli *client, 
				   int32_t stream, 
				   struct dspd_pcm_chmap *chmap,
				   size_t chmap_bufsize,
				   dspd_aio_ccb_t complete,
				   void *data)
{
   int32_t ret;
  if ( client->state == PCMCLI_STATE_SETUP )
    {
      ret = submit_io(client, 
		      DSPD_SCTL_CLIENT_GETCHANNELMAP,
		      &stream,
		      sizeof(stream),
		      chmap,
		      chmap_bufsize,
		      channelmap_complete);
      ret = complete_io(client, ret, complete, data);
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}



int32_t dspd_pcmcli_ctl(struct dspd_pcmcli *client,
			  int32_t stream,
			  int32_t req,
			  const void *inbuf,
			  size_t inbufsize,
			  void *outbuf,
			  size_t outbufsize,
			  size_t *bytes_returned)
{
  int32_t ret = -EBADFD;
  if ( client->state >= PCMCLI_STATE_INIT )
    ret = dspd_stream_ctl(client->conn, stream, req, inbuf, inbufsize, outbuf, outbufsize, bytes_returned);
  return ret;
}
			  




int32_t dspd_pcmcli_set_constant_latency(struct dspd_pcmcli *client, bool enable)
{
  int32_t ret;
  if ( client->playback.enabled == false )
    {
      ret = -EBADFD;
    } else if ( client->state < PCMCLI_STATE_RUNNING )
    {
      ret = dspd_pcmcli_stream_set_constant_latency(&client->playback.stream, enable);
      if ( ret == 0 )
	client->constant_latency = enable;
    } else
    {
      ret = -EBUSY;
    }
  return ret;
}

void dspd_pcmcli_set_timer_callbacks(struct dspd_pcmcli *client, const struct dspd_pcmcli_timer_ops *ops, void *arg)
{
  if ( client->timer_ops )
    client->timer_ops->destroy(client->timer_arg);
  if ( ops )
    {
      client->timer_ops = ops;
      client->timer_arg = arg;
    } else
    {
      client->timer_ops = &null_timer_ops;
      client->timer_arg = (dspd_pcmcli_timer_t*)&client->timer;
    }
}

int32_t dspd_pcmcli_init(struct dspd_pcmcli *client, int32_t streams, int32_t flags)
{
  int32_t ret = 0;
  if ( ! (streams & (DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) )
    return -EINVAL;

  memset(client, 0, sizeof(*client));
  client->poll_threshold = 1;
  client->connfd = -1;
  client->pfds[0].fd = -1;
  client->pfds[1].fd = -1;
  client->timer.fd = -1;
  client->streams = streams;
  client->nonblocking = !!(flags & DSPD_PCMCLI_NONBLOCK);
  client->constant_latency = !!(flags & DSPD_PCMCLI_CONSTANT_LATENCY);

  client->playback.device_idx = -1;
  client->playback.stream_idx = -1;
  client->capture.stream_idx = -1;
  client->capture.device_idx = -1;
  client->timer_ops = &null_timer_ops;
  client->timer_arg = (dspd_pcmcli_timer_t*)&client->timer;


  if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      ret = dspd_pcmcli_stream_init(&client->playback.stream, DSPD_PCM_SBIT_PLAYBACK);
      if ( ret == 0 )
	{
	  ret = dspd_pcmcli_stream_set_constant_latency(&client->playback.stream, client->constant_latency);
	  if ( ret == 0 && (flags & DSPD_PCMCLI_BYTE_MODE) )
	    {
	      client->playback.frame_buf = calloc(DSPD_CHMAP_LAST, sizeof(uint64_t));
	      if ( ! client->playback.frame_buf )
		ret = -ENOMEM;
	      
	    }
	}
      if ( ret == 0 )
	client->playback.enabled = true;
    }
  if ( ret == 0 && (client->streams & DSPD_PCM_SBIT_CAPTURE) )
    {
      ret = dspd_pcmcli_stream_init(&client->capture.stream, DSPD_PCM_SBIT_CAPTURE);
      if ( ret == 0 )
	{
	  assert(client->capture.stream.stream_flags == DSPD_PCM_SBIT_CAPTURE);
	  if ( flags & DSPD_PCMCLI_BYTE_MODE )
	    {
	      client->capture.frame_buf = calloc(DSPD_CHMAP_LAST, sizeof(uint64_t));
	      if ( ! client->capture.frame_buf )
		ret = -ENOMEM;
	    }
	}
      if ( ret == 0 )
	client->capture.enabled = true;
    }
  
  if ( ret == 0 && (flags & DSPD_PCMCLI_NOTIMER) == 0 )
    {
      ret = dspd_timer_init(&client->timer);
      if ( ret == 0 )
	client->timer_ops = &pcmcli_timer_ops;
      if ( ret == 0 )
	{
	  ret = client->timer_ops->getpollfd(client->timer_arg, &client->pfds[client->nfds]);
	  if ( ret == 0 )
	    client->nfds++;
	}
    }
  if ( ret == 0 && (client->streams & DSPD_PCM_SBIT_PLAYBACK) )
    ret = dspd_pcmcli_stream_set_constant_latency(&client->playback.stream, client->constant_latency);
  if ( ret < 0 )
    dspd_pcmcli_destroy(client);
  else
    client->state = PCMCLI_STATE_INIT;
  return ret;
}

void dspd_pcmcli_destroy(struct dspd_pcmcli *client)
{
  dspd_pcmcli_unbind(client);
  client->timer_ops->destroy(client->timer_arg);
  if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      dspd_pcmcli_stream_destroy(&client->playback.stream); 
      free(client->playback.frame_buf);
    }
  if ( client->streams & DSPD_PCM_SBIT_CAPTURE )
    {
      dspd_pcmcli_stream_destroy(&client->capture.stream);
      free(client->capture.frame_buf);
    }
  client->state = PCMCLI_STATE_ALLOC;
  return;
}

int32_t dspd_pcmcli_new(struct dspd_pcmcli **client, int32_t streams, int32_t flags)
{
  struct dspd_pcmcli *c;
  int32_t ret;
  c = malloc(sizeof(*c));
  if ( c != NULL )
    {
      ret = dspd_pcmcli_init(c, streams, flags);
      if ( ret < 0 )
	free(c);
      else
	*client = c;
    } else
    {
      ret = -ENOMEM;
    }
  return ret;
}

void dspd_pcmcli_delete(struct dspd_pcmcli *client)
{
  dspd_pcmcli_destroy(client);
  free(client);
}


int32_t dspd_pcmcli_set_nonblocking(struct dspd_pcmcli *client, bool nonblocking)
{
  client->nonblocking = nonblocking;
  return 0;
}

int32_t dspd_pcmcli_set_poll_threshold(struct dspd_pcmcli *client, size_t frames)
{
  size_t f;
  int32_t ret = 0;
  if ( client->state < PCMCLI_STATE_SETUP )
    {
      ret = -EINVAL;
    } else
    {
      f = frames / client->fragsize;
      if ( frames % client->fragsize )
	f++;
      client->poll_threshold = f * client->fragsize;
      ret = client->poll_threshold;
    }
  return ret;
}

int32_t dspd_pcmcli_set_no_xrun(struct dspd_pcmcli *client, bool no_xrun)
{
  client->no_xrun = no_xrun;
  return 0;
}

struct sd_args {
  struct dspd_pcmcli *client;
  struct dspd_pcmcli_bindparams *params;
  int32_t streams;
  int32_t (*select_device)(void *arg, int32_t streams, int32_t index, const struct dspd_device_stat *info, struct dspd_pcmcli *client);
  void *arg;
};

static int32_t sd_callback(void *arg, int32_t streams, int32_t index, const struct dspd_device_stat *info)
{
  struct sd_args *sd = arg;
  int32_t ret = SELECT_DEV_REJECT;
  int32_t s = info->streams & sd->streams;
  if ( s )
    {
      ret = sd->select_device(sd->arg, sd->streams, index, info, sd->client);
      
      if ( ret == SELECT_DEV_OK || ret == SELECT_DEV_OK_ABORT )
	{
	  if ( s & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      sd->params->playback_device = index;
	      sd->client->playback.info = *info;
	      if ( ret == SELECT_DEV_OK_ABORT )
		sd->streams &= ~DSPD_PCM_SBIT_PLAYBACK;
	    }
	  if ( s & DSPD_PCM_SBIT_CAPTURE )
	    {
	      sd->params->capture_device = index;
	      sd->client->capture.info = *info;
	      if ( ret == SELECT_DEV_OK_ABORT )
		sd->streams &= ~DSPD_PCM_SBIT_CAPTURE;
	    }
	  if ( sd->streams == 0 )
	    ret = SELECT_DEV_OK_ABORT;
	}
    }
  
  return ret;
}

int32_t dspd_pcmcli_select_byname_cb(void *arg, int32_t streams, int32_t index, const struct dspd_device_stat *info, struct dspd_pcmcli *client)
{
  int32_t ret = SELECT_DEV_REJECT;
  const char *p = arg;
  if ( arg == NULL )
    {
      if ( info->flags & streams )
	ret = SELECT_DEV_OK;
    } else if ( p[0] == '*' && p[1] == ':' )
    {
      p = strchr(info->name, ':');
      if ( p != NULL )
	{
	  if ( strcmp(arg+2UL, p) == 0 )
	    ret = SELECT_DEV_OK;
	}
    } else if ( strcmp(info->name, arg) == 0 )
    {
      ret = SELECT_DEV_OK;
    } 
  return ret;
}
 
static int32_t ref_stream(struct dspd_aio_ctx *aio, struct pcmcli_stream_data *data, uint32_t sbit, uint32_t dev)
{
  int32_t ret;
  struct dspd_device_stat info;
  uint64_t val;
  size_t br;
  val = sbit;
  val <<= 32U;
  val |= dev;
  ret = dspd_stream_ctl(aio, 
			-1, 
			DSPD_SOCKSRV_REQ_RMSRV, 
			&val, 
			sizeof(val), 
			&info, 
			sizeof(info),
			&br);
  if ( ret == 0 )
    {
      if ( br != sizeof(info) )
	{
	  ret = -EPROTO;
	} else if ( strcmp(info.name, data->info.name) == 0 &&
		    strcmp(info.bus, data->info.bus) == 0 &&
		    strcmp(info.addr, data->info.addr) == 0 &&
		    strcmp(info.desc, data->info.desc) == 0 &&
		    (info.streams & sbit) == sbit )
	{
	  data->info = info;
	} else
	{
	  //Not the same device
	  ret = -EAGAIN;
	}
    }
  return ret;
}

//Names such as hw:0,1 and hw:0,2 and hw:0, but not hw:1 and hw:2 should all be considered
//equal for the purposes of joining two half duplex streams.
static bool compare_names(const char *n1, const char *n2)
{
  size_t len1, len2;
  const char *p;
  p = strchr(n1, ',');
  if ( p )
    len1 = (size_t)p - (size_t)n1;
  else
    len1 = strlen(n1);
  p = strchr(n2, ',');
  if ( p )
    len2 = (size_t)p - (size_t)n2;
  else
    len2 = strlen(n2);
  return len1 == len2 && memcmp(n1, n2, len1) == 0;
}
static bool check_latencies(const struct dspd_cli_params *playback, const struct dspd_cli_params *capture)
{
  int32_t minl, maxl;
  minl = MAX(playback->min_latency, capture->min_latency);
  maxl = MIN(playback->max_latency, capture->max_latency);
  return minl <= maxl;
}

void event_cb(struct dspd_aio_ctx    *context,
	      void                   *arg,
	      uint32_t                req,
	      int32_t                 stream,
	      int32_t                 flags,
	      const struct dspd_async_event *evt,
	      const void             *buf,
	      size_t                  len)
{
  printf("GOT EVENT\n");
}

int32_t dspd_pcmcli_open_device(struct dspd_pcmcli *client, 
				const char *server,
				int32_t (*select_device)(void *arg, int32_t streams, int32_t index, const struct dspd_device_stat *info, struct dspd_pcmcli *client),
				void *arg)
{
  struct dspd_pcmcli_bindparams params = {
    .playback_stream = -1,
    .playback_device = -1,
    .capture_stream = -1,
    .capture_device = -1,
    .context = NULL
  };
  int32_t ret;
  struct sd_args args;
  int32_t s;
  int64_t o;
  int64_t val;
  size_t br;
  struct dspd_device_stat info;
  struct dspd_device_mstat minfo;
  if ( client->state > PCMCLI_STATE_INIT )
    return -EBADFD;
  ret = dspd_aio_new(&params.context, DSPD_AIO_DEFAULT);
  if ( ret == 0 )
    ret = dspd_aio_connect(params.context, server, NULL, NULL, NULL);
  if ( ret == 0 )
    {
      if ( select_device == NULL )
	{
	  arg = NULL;
	  ret = dspd_stream_ctl(params.context,
				-1,
				DSPD_SOCKSRV_REQ_DEFAULTDEV,
				&client->streams,
				sizeof(client->streams),
				&minfo,
				sizeof(minfo),
				&br);
	  if ( ret == 0 )
	    {
	      if ( br == sizeof(minfo) )
		{
		  
		  if ( (client->streams & DSPD_PCM_SBIT_PLAYBACK) && minfo.playback_slot >= 0 )
		    {
		      params.playback_device = minfo.playback_slot;
		      client->playback.info = minfo.playback_info;
		    }
		  if ( client->streams & DSPD_PCM_SBIT_CAPTURE && minfo.capture_slot >= 0 )
		    {
		      params.capture_device = minfo.capture_slot;
		      client->capture.info = minfo.capture_info;
		    }
		} else
		{
		  ret = -ENOENT;
		}
	    } else if ( ret == -EINVAL )
	    {
	      ret = -ENOENT;
	    }
	} else
	{
	  ret = -ENOENT;
	}

      if ( ret == -ENOENT )
	{
	  memset(&args, 0, sizeof(args));
	  args.client = client;
	  args.params = &params;
	  if ( select_device )
	    args.select_device = select_device;
	  else
	    args.select_device = dspd_pcmcli_select_byname_cb;
	  args.arg = arg;
	  args.streams = client->streams;
	  ret = dspd_select_device((struct dspd_conn*)params.context, 0, sd_callback, &args);
	  if ( ret == 0 && 
	       ((params.playback_device < 0 && (client->streams & DSPD_PCM_SBIT_PLAYBACK)) ||
		(params.capture_device < 0 && (client->streams & DSPD_PCM_SBIT_CAPTURE))))
	    ret = -ENODEV;

	}
      if ( ret == 0 )
	{
	  if ( params.playback_device >= 0 && params.capture_device >= 0 && 
	       params.playback_device == params.capture_device )
	    {
 	      val = DSPD_PCM_SBIT_FULLDUPLEX;
	      val <<= 32U;
	      val |= params.playback_device;
	      ret = dspd_stream_ctl(params.context, 
				    -1, 
				    DSPD_SOCKSRV_REQ_RMSRV, 
				    &val, 
				    sizeof(val), 
				    NULL, 
				    sizeof(info),
				    &br);
	    } else
	    {
	      if ( client->streams == DSPD_PCM_SBIT_FULLDUPLEX && (params.playback_device < 0 || params.capture_device < 0) )
		{
		  ret = -ENOENT;
		} else
		{
		  if ( params.playback_device >= 0 )
		    ret = ref_stream(params.context, &client->playback, DSPD_PCM_SBIT_PLAYBACK, params.playback_device);
		  if ( ret == 0 && params.capture_device >= 0 )
		    ret = ref_stream(params.context, &client->capture, DSPD_PCM_SBIT_CAPTURE, params.capture_device);

		  if ( params.playback_device >= 0 && params.capture_device >= 0 )
		    {
		      //The two devices must be two halves of the same device.
		      if ( compare_names(client->playback.info.name, client->capture.info.name) == false ||
			   check_latencies(&client->playback.info.playback, &client->capture.info.capture) == false ||
			   strcmp(client->playback.info.bus, client->capture.info.bus) != 0 ||
			   client->playback.info.playback.rate != client->capture.info.capture.rate )
			ret = -EINVAL;
		    }
		}
	    }
	}
	

      if ( ret == 0 )
	{
	  if ( ! (((client->streams & DSPD_PCM_SBIT_PLAYBACK) != 0 && params.playback_device < 0 ) ||
		  ((client->streams & DSPD_PCM_SBIT_CAPTURE) != 0 && params.capture_device < 0 )) )
	    {
	      s = 0;
	      if ( params.playback_device != params.capture_device )
		{
		  if ( params.playback_device >= 0 )
		    s |= DSPD_PCM_SBIT_PLAYBACK;
		  if ( params.capture_device >= 0 )
		    s |= DSPD_PCM_SBIT_CAPTURE;
		}
	      ret = dspd_stream_ctl(params.context, -1, DSPD_SOCKSRV_REQ_NMCLI, &s, sizeof(s), &o, sizeof(o), &br);
	      if ( ret == 0 )
		{
		  if ( br == sizeof(o) )
		    {
		      if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
			params.playback_stream = o >> 32U;
		      if ( client->streams & DSPD_PCM_SBIT_CAPTURE )
			params.capture_stream = o & 0xFFFFFFFFU;
		      ret = dspd_pcmcli_bind(client, &params, true, NULL, NULL);
		    } else
		    {
		      ret = -EPROTO;
		    }
		}
	    }
	}
    }

  if ( params.context )
    {
      dspd_aio_set_event_cb(params.context, event_cb, client);
    }

  if ( ret < 0 )
    {
      if ( params.context )
	dspd_aio_delete(params.context);
    }
  return ret;
}


static void start_complete_cb(void *context, struct dspd_async_op *op)
{
  struct dspd_pcmcli *client = op->data;
  dspd_time_t *tstamps = op->outbuf;
  int32_t sbits = *(int32_t*)op->inbuf;
  int32_t ret = op->error;
  if ( ret == 0 )
    {
      if ( (sbits & client->streams & DSPD_PCM_SBIT_PLAYBACK) )
	{
	  ret = dspd_pcmcli_stream_set_trigger_tstamp(&client->playback.stream, tstamps[DSPD_PCM_STREAM_PLAYBACK]);
	  if ( ret == 0 )
	    ret = dspd_pcmcli_stream_set_running(&client->playback.stream, true); 
	}
      if ( ret == 0 && (sbits & client->streams & DSPD_PCM_SBIT_CAPTURE) )
	{
	  ret = dspd_pcmcli_stream_set_trigger_tstamp(&client->capture.stream, tstamps[DSPD_PCM_STREAM_CAPTURE]);
	  if ( ret == 0 )
	    ret = dspd_pcmcli_stream_set_running(&client->capture.stream, true); 
	}
      
      if ( ret == 0 )
	{
	  client->state = PCMCLI_STATE_RUNNING;
	  dspd_pcmcli_wait(client, client->streams, 0, true);
	}
    }
  complete_event(client, ret);
}

int32_t dspd_pcmcli_start(struct dspd_pcmcli *client, int32_t sbits, dspd_aio_ccb_t complete, void *data)
{
  int32_t ret;
  if ( client->state == PCMCLI_STATE_PREPARED || client->state == PCMCLI_STATE_PAUSED )
    {
      if ( sbits & client->streams )
	{
	  //The stream may be ready for io right now, but this only matters when doing async io.
	  if ( complete )
	    dspd_pcmcli_wait(client, client->streams, 0, true);
	  ret = submit_io(client,
			  DSPD_SCTL_CLIENT_START,
			  &sbits,
			  sizeof(sbits),
			  client->output,
			  sizeof(dspd_time_t) * 2UL,
			  start_complete_cb);
	  ret = complete_io(client, ret, complete, data);
	} else
	{
	  ret = -EINVAL;
	}
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}

static void stop_complete_cb(void *context, struct dspd_async_op *op)
{
  struct dspd_pcmcli *client = op->data;
  int32_t err;
  int32_t sbits = *(const int32_t*)op->inbuf;
  err = op->error;
  if ( err == 0 )
    {
      if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
	err = dspd_pcmcli_stream_set_running(&client->playback.stream, false);
      if ( err == 0 && (sbits & DSPD_PCM_SBIT_CAPTURE) )
	err = dspd_pcmcli_stream_set_running(&client->capture.stream, false);
      client->state = PCMCLI_STATE_SETUP;
    }
  complete_event(client, err);
}

int32_t dspd_pcmcli_stop(struct dspd_pcmcli *client, int32_t sbits, dspd_aio_ccb_t complete, void *data)
{
  int32_t ret = -EBADFD;
  if ( client->state == PCMCLI_STATE_RUNNING || client->state == PCMCLI_STATE_DRAINING )
    {
      sbits &= client->streams;
      ret = submit_io(client,
		      DSPD_SCTL_CLIENT_STOP,
		      &sbits,
		      sizeof(sbits),
		      NULL,
		      0,
		      stop_complete_cb);
      ret = complete_io(client, ret, complete, data);
    }
  return ret;
}

static void pause_complete_cb(void *context, struct dspd_async_op *op)
{
  int32_t err = op->error;
  int32_t paused = *(const int32_t*)op->inbuf;
  dspd_time_t *tstamps = op->outbuf;
  struct dspd_pcmcli *client = op->data;
  if ( err == 0 )
    {
      client->clockdiff = 0;
      if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
	err = dspd_pcmcli_stream_set_paused(&client->playback.stream, paused);
      if ( err == 0 && (client->streams & DSPD_PCM_SBIT_CAPTURE) )
	err = dspd_pcmcli_stream_set_paused(&client->capture.stream, paused);
      if ( err == 0 && paused == 0 )
	{
	  if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
	    err = dspd_pcmcli_stream_set_trigger_tstamp(&client->playback.stream, tstamps[DSPD_PCM_STREAM_PLAYBACK]);
	  if ( err == 0 && (client->streams & DSPD_PCM_SBIT_CAPTURE) )
	    err = dspd_pcmcli_stream_set_trigger_tstamp(&client->capture.stream, tstamps[DSPD_PCM_STREAM_CAPTURE]);
	}
      if ( err == 0 )
	{
	  if ( paused )
	    client->state = PCMCLI_STATE_PAUSED;
	  else
	    client->state = PCMCLI_STATE_PREPARED;
	}
    }
  complete_event(client, err);
}

int32_t dspd_pcmcli_pause(struct dspd_pcmcli *client, bool paused, dspd_aio_ccb_t complete, void *arg)
{
  int32_t ret = 0;
  int32_t p;
  if ( client->state == PCMCLI_STATE_RUNNING || client->state == PCMCLI_STATE_PAUSED )
    {
      p = paused;
      ret = submit_io(client,
		      DSPD_SCTL_CLIENT_PAUSE,
		      &p,
		      sizeof(p),
		      client->output,
		      sizeof(dspd_time_t) * 2UL,
		      pause_complete_cb);
      ret = complete_io(client, ret, complete, arg);

      if ( ret == 0 )
	{
	  if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
	    ret = dspd_pcmcli_stream_set_paused(&client->playback.stream, paused);
	  if ( ret == 0 && (client->streams & DSPD_PCM_SBIT_CAPTURE) )
	    ret = dspd_pcmcli_stream_set_paused(&client->capture.stream, paused);
	  if ( ret == 0 )
	    {
	      if ( paused )
		client->state = PCMCLI_STATE_PAUSED;
	      else
		client->state = PCMCLI_STATE_PREPARED;
	    }
	}
    } else if ( client->state < PCMCLI_STATE_PREPARED )
    {
      ret = -EBADFD;
    }

  return ret;
}


int32_t dspd_pcmcli_get_client_index(const struct dspd_pcmcli *client, 
				       int32_t sbit)
{
  int32_t ret;
  if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
    ret = client->playback.stream_idx;
  else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
    ret = client->capture.stream_idx;
  else
    ret = -EINVAL;
  return ret;
}

int32_t dspd_pcmcli_get_device_index(const struct dspd_pcmcli *client, 
				       int32_t sbit)
{
  int32_t ret;
  if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
    ret = client->playback.device_idx;
  else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
    ret = client->capture.device_idx;
  else
    ret = -EINVAL;
  return ret;
}

int32_t dspd_pcmcli_process_io(struct dspd_pcmcli *client, int32_t revents, int32_t timeout)
{
  int32_t ret;
  if ( client->dummy_op )
    {
      client->dummy_op = false;
      if ( client->pending_op.complete )
	client->pending_op.complete(client, &client->pending_op);
      else if ( client->complete )
	complete_event(client, client->pending_op.error);
    }
  if ( client->conn )
    ret = dspd_aio_process(client->conn, revents, timeout);
  else
    ret = 0;
  return ret;
}

int32_t dspd_pcmcli_cancel_io(struct dspd_pcmcli *client)
{
  int32_t ret = 0;
  if ( client->dummy_op )
    {
      client->dummy_op = false;
      if ( client->pending_op.complete )
	client->pending_op.complete(client, &client->pending_op);
      else if ( client->complete )
	complete_event(client, client->pending_op.error);
    } else if ( client->pending_op.error > 0 && client->conn != NULL )
    {
      ret = dspd_aio_cancel(client->conn, &client->pending_op, true);
    }
  return ret;
}

int32_t dspd_pcmcli_rewind(struct dspd_pcmcli *client, int32_t sbits, uint64_t *frames)
{
  int32_t ret = 0;
  if ( client->state < PCMCLI_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( (sbits & client->streams) != sbits )
    {
      ret = -EINVAL;
    } else
    {
      if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
	ret = dspd_pcmcli_stream_rewind(&client->playback.stream, frames);
      if ( ret == 0 && (sbits & DSPD_PCM_SBIT_CAPTURE) )
	ret = dspd_pcmcli_stream_rewind(&client->capture.stream, frames);
    }
  return ret;
}

int32_t dspd_pcmcli_forward(struct dspd_pcmcli *client, int32_t sbits, uint64_t *frames)
{
  int32_t ret = 0;
  if ( client->state < PCMCLI_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( (sbits & client->streams) != sbits )
    {
      ret = -EINVAL;
    } else
    {
      if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
	ret = dspd_pcmcli_stream_forward(&client->playback.stream, frames);
      if ( ret == 0 && (sbits & DSPD_PCM_SBIT_CAPTURE) )
	ret = dspd_pcmcli_stream_forward(&client->capture.stream, frames);
    }
  return ret;
}

int32_t dspd_pcmcli_set_appl_pointer(struct dspd_pcmcli *client, int32_t sbits, bool relative, uint64_t frames)
{
  int32_t ret = 0;
  if ( client->state < PCMCLI_STATE_PREPARED )
    {
      ret = -EBADFD;
    } else if ( (sbits & client->streams) != sbits )
    {
      ret = -EINVAL;
    } else
    {
      if ( sbits & DSPD_PCM_SBIT_PLAYBACK )
	ret = dspd_pcmcli_stream_set_pointer(&client->playback.stream, relative, frames);
      if ( ret == 0 && (sbits & DSPD_PCM_SBIT_CAPTURE) )
	ret = dspd_pcmcli_stream_set_pointer(&client->capture.stream, relative, frames);
    }
  return ret;
}



int32_t dspd_pcmcli_delay(struct dspd_pcmcli *client, int32_t sbit, int64_t *frames)
{
  int32_t ret;
  struct dspd_pcmcli_status s;
  uint64_t hw = 0, appl = 0;
  ret = dspd_pcmcli_get_status(client, sbit, true, &s);
  if ( ret == 0 )
    {
      *frames = s.delay;
    } else
    {
      ret = dspd_pcmcli_avail(client, sbit, &hw, &appl);
      if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
	*frames = appl - hw;
      else
	*frames = hw - appl;
    }
  return ret;
}

int32_t dspd_pcmcli_drain(struct dspd_pcmcli *client)
{
  int32_t ret = 0;
  size_t pt, fill, am;
  if ( (client->state < PCMCLI_STATE_PREPARED) || ((client->streams & DSPD_PCM_SBIT_PLAYBACK) == 0) )
    {
      ret = -EBADFD;
    } else
    {
      if ( client->state == PCMCLI_STATE_PREPARED )
	ret = dspd_pcmcli_start(client, DSPD_PCM_SBIT_PLAYBACK, NULL, NULL);
      else if ( client->state == PCMCLI_STATE_PAUSED )
	ret = dspd_pcmcli_pause(client, false, NULL, NULL);
      if ( ret == 0 && client->state == PCMCLI_STATE_RUNNING && (client->streams & DSPD_PCM_SBIT_PLAYBACK) )
	{
	  client->state = PCMCLI_STATE_DRAINING;
	  pt = client->poll_threshold;
	  am = client->swparams.avail_min;
	  client->swparams.avail_min = client->playback.stream.params.bufsize;
	  client->poll_threshold = client->playback.stream.params.bufsize;
	  ret = dspd_pcmcli_wait(client, DSPD_PCM_SBIT_PLAYBACK, client->poll_threshold, false);
	  client->swparams.avail_min = am;
	  client->poll_threshold = pt;
	  if ( ret == 0 )
	    {
	      while ( (ret = dspd_pcmcli_avail(client, DSPD_PCM_SBIT_PLAYBACK, NULL, NULL)) <
		      client->playback.stream.params.bufsize )
		{
		  if ( ret < 0 )
		    break;
		  fill = client->playback.stream.params.bufsize - ret;
		  fill *= client->playback.stream.sample_time;
		  fill /= 1000;
		  if ( fill < 100 )
		    fill = 100;
		  else
		    fill = MIN(fill, 10000);
		  usleep(fill);
		}
	      dspd_pcmcli_restore_wait(client);
	      if ( ret > 0 )
		ret = 0;
	    }
	}
      if ( ret == 0 )
	ret = dspd_pcmcli_stop(client, client->streams, NULL, NULL);
    }
  return ret;
}

ssize_t dspd_pcmcli_frames_to_bytes(struct dspd_pcmcli *client, 
				   const struct dspd_cli_params *params,
				   int32_t stream, 
				   size_t frames)
{
  ssize_t ret = -EINVAL;
  size_t c;
  stream &= client->streams;
  if ( params != NULL )
    {
      if ( (params->stream & stream) == stream )
	{
	  if ( params->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
	    {
	      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
		c = DSPD_CLI_PCHAN(params->channels);
	      else
		c = DSPD_CLI_CCHAN(params->channels);
	    } else
	    {
	      c = params->channels;
	    }
	  ret = dspd_get_pcm_format_size(params->format) * c * frames;
	}
    } else if ( client->state < PCMCLI_STATE_SETUP )
    {
      ret = -EBADFD;
    } else
    {
      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
	ret = client->playback.stream.framesize * frames;
      else if ( stream == DSPD_PCM_SBIT_CAPTURE )
	ret = client->capture.stream.framesize * frames;
    }
  return ret;
}

ssize_t dspd_pcmcli_bytes_to_frames(struct dspd_pcmcli *client, 
				    const struct dspd_cli_params *params,
				    int32_t stream, 
				    size_t bytes)
{
  ssize_t ret = -EINVAL;
  size_t c;
  stream &= client->streams;
  if ( params != NULL )
    {
      if ( (params->stream & stream) == stream )
	{
	  if ( params->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
	    {
	      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
		c = DSPD_CLI_PCHAN(params->channels);
	      else
		c = DSPD_CLI_CCHAN(params->channels);
	    } else
	    {
	      c = params->channels;
	    }
	  ret = dspd_get_pcm_format_size(params->format) * c;
	  if ( ret > 0 )
	    ret = bytes / ret;
	}
    } else if ( client->state < PCMCLI_STATE_SETUP )
    {
      ret = -EBADFD;
    } else
    {
      if ( stream == DSPD_PCM_SBIT_PLAYBACK && client->playback.stream.framesize > 0 )
	ret = bytes / client->playback.stream.framesize;
      else if ( stream == DSPD_PCM_SBIT_CAPTURE && client->capture.stream.framesize > 0 )
	ret = bytes / client->capture.stream.framesize;
    }
  return ret;
}

int32_t dspd_pcmcli_hw_params_default(struct dspd_pcmcli *client, struct dspd_cli_params *params)
{
  int32_t ret = 0;
  uint32_t fs, n, t;
  memset(params, 0, sizeof(*params));
  if ( client->state < PCMCLI_STATE_OPEN )
    {
      ret = -EBADFD;
    } else if ( client->streams == DSPD_PCM_SBIT_FULLDUPLEX )
    {
      params->format = DSPD_PCM_FORMAT_FLOAT_LE;
      params->channels = DSPD_CLI_FDCHAN(client->playback.info.playback.channels,
					 client->capture.info.capture.channels);
      params->rate = MIN(client->capture.info.capture.rate,
			 client->playback.info.playback.rate);
      params->min_latency = MAX(client->playback.info.playback.min_latency,
			       client->capture.info.capture.min_latency);
      params->max_latency = MIN(client->playback.info.playback.max_latency,
				client->capture.info.capture.max_latency);
      if ( params->max_latency < params->min_latency )
	params->max_latency = params->min_latency;
      params->stream = DSPD_PCM_SBIT_FULLDUPLEX;
      params->latency = params->fragsize;
      params->xflags = DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
    } else if ( client->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      *params = client->playback.info.playback;
    } else if ( client->streams & DSPD_PCM_SBIT_CAPTURE )
    {
      *params = client->capture.info.capture;
    } else
    { 
      ret = -EBADFD; //Should not happen
    }
  if ( ret == 0 )
    {
      t = 1000000000 / params->rate;
      fs = 10000000 / t;
      if ( fs < params->min_latency )
	{
	  fs = params->min_latency;
	} else if ( fs > params->max_latency )
	{
	  fs = params->max_latency;
	} else
	{
	  n = fs / params->min_latency;
	  if ( fs % params->min_latency )
	    n++;
	  fs = n * params->min_latency;
	}
      //Min+max latency is nanoseconds
      params->xflags |= DSPD_CLI_XFLAG_LATENCY_NS;
      params->min_latency *= t;
      params->max_latency *= t;
      params->bufsize = fs * 4;
      params->src_quality = 0; //default
      if ( ! dspd_aio_is_local(client->conn) )
	params->flags |= DSPD_CLI_FLAG_SHM;

    }
  return ret;
}

int32_t dspd_pcmcli_hw_params_get_channels(struct dspd_pcmcli *client, struct dspd_cli_params *params, int32_t stream)
{
  int32_t ret = -EINVAL;
  stream &= params->stream;
  stream &= client->streams;
  if ( params->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
    {
      if ( stream == DSPD_PCM_SBIT_PLAYBACK )
	ret = DSPD_CLI_PCHAN(params->channels);
      else if ( stream == DSPD_PCM_SBIT_CAPTURE )
	ret = DSPD_CLI_CCHAN(params->channels);
    } else if ( stream )
    {
      ret = params->channels;
    }
  return ret;
}

int32_t dspd_pcmcli_hw_params_set_channels(struct dspd_pcmcli *client, 
					   struct dspd_cli_params *params,
					   int32_t stream, 
					   int32_t channels)
{
  int32_t ret = -EINVAL;
  if ( client->state < PCMCLI_STATE_OPEN )
    {
      ret = -EBADFD;
    } else if ( stream == DSPD_PCM_SBIT_PLAYBACK && (client->streams & DSPD_PCM_SBIT_PLAYBACK) )
    {
      if ( channels > client->playback.info.playback.channels && channels != 2 )
	ret = client->playback.info.playback.channels;
      else if ( channels < 1 )
	ret = 1;
      else
	ret = channels;
      if ( params->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
	params->channels = DSPD_CLI_FDCHAN(ret, DSPD_CLI_CCHAN(params->channels));
      else
	params->channels = ret;
    } else if ( stream == DSPD_PCM_SBIT_CAPTURE && (client->streams & DSPD_PCM_SBIT_CAPTURE) )
    {
      if ( channels > client->capture.info.capture.channels && channels != 2 )
	ret = client->capture.info.capture.channels;
      else if ( channels < 1 )
	ret = 1;
      else
	ret = channels;
      if ( params->xflags & DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS )
	params->channels = DSPD_CLI_FDCHAN(DSPD_CLI_PCHAN(params->channels), ret);
      else
	params->channels = ret;
    }
  return ret;
}

int32_t dspd_pcmcli_hw_params_set_rate(struct dspd_pcmcli *client, 
				       struct dspd_cli_params *params,
				       int32_t rate)
{
  int32_t t, n, t2;
  (void)client;
  if ( rate < 8000 )
    rate = 8000;
  else if ( rate > 384000 )
    rate = 384000;
  if ( params->rate <= 0 )
    params->rate = rate;
  t = 1000000000 / params->rate;

  n = params->bufsize * t;
  t2 = 1000000000 / rate;
  params->bufsize = n / t2;
  if ( n % t2 )
    params->bufsize++;

  n = params->fragsize * t;
  params->fragsize = n / t2;
  if ( n % t2 )
    params->fragsize++;

  n = params->fragsize * 2;
  if ( params->bufsize < n )
    params->bufsize = n;

  n = params->latency * t;
  params->latency = n / t2;
  if ( n % t2 )
    params->latency++;
  if ( params->latency > params->fragsize )
    params->latency = params->fragsize;

  params->rate = rate;
  return rate;
}


#define MIN_FRAGMENTS 2
#define MAX_BUFTIME 2000000000

int32_t dspd_pcmcli_hw_params_set_bufsize(struct dspd_pcmcli *client, 
					  struct dspd_cli_params *params,
					  int32_t size)
{
  int64_t maxsize, minsize;
  int64_t t = 1000000000 / params->rate, n, f;
  (void)client;
  maxsize = ((1 << get_hpo2(params->rate)) * t) + params->max_latency;
  if ( maxsize >= MAX_BUFTIME )
    {
      maxsize = ((1 << get_lpo2(params->rate)) * t) + params->max_latency;
      if ( maxsize >= MAX_BUFTIME )
	maxsize = params->rate * t;
    }
  minsize = params->min_latency * MIN_FRAGMENTS;
  size *= t;
  if ( size > maxsize )
    size = maxsize;
  else if ( size < minsize)
    size = minsize;
  params->bufsize = size / t;
  assert(params->fragsize > 0 && params->min_latency > 0);
  
  n = params->bufsize / params->fragsize;
  if ( n < MIN_FRAGMENTS )
    n = MIN_FRAGMENTS;
  f = t * (params->bufsize / n);
  if ( f > params->min_latency )
    {
      n = get_lpo2(f / params->min_latency);
      if ( n > 31 )
	n = 31;
      n = 1 << n;
      params->fragsize = ((int64_t)params->min_latency * n) / t;
    }
  params->latency = params->fragsize;
  return params->bufsize;
}

int32_t dspd_pcmcli_hw_params_set_fragsize(struct dspd_pcmcli *client, 
					   struct dspd_cli_params *params,
					   int32_t size)
{
  int64_t t, maxsize, minsize, n, f;
  (void)client;
  t = 1000000000 / params->rate;
  minsize = params->min_latency / t;
  maxsize = params->bufsize / MIN_FRAGMENTS;
  if ( size < maxsize && size > minsize )
    {
      f = t * size;
      n = get_hpo2(f / params->min_latency);
      size = ((1 << n) * (int64_t)params->min_latency) / t;
    }
  if ( size > maxsize )
    size = maxsize;
  else if ( size < minsize )
    size = minsize;
  params->fragsize = size;
  params->latency = params->fragsize;
  return params->fragsize;
}

int32_t dspd_pcmcli_hw_params_set_latency(struct dspd_pcmcli *client, 
					  struct dspd_cli_params *params,
					  int32_t frames)
{
  int32_t fs = params->fragsize, l = params->latency;
  int32_t ret;
  ret = dspd_pcmcli_hw_params_set_fragsize(client, params, frames);
  if ( fs > 0 )
    params->fragsize = fs;
  if ( ret > 0 )
    params->latency = ret;
  else
    params->latency = l;
  return ret;
}

int32_t dspd_pcmcli_hw_params_set_format(struct dspd_pcmcli *cli,
					 struct dspd_cli_params *params,
					 int32_t format)
{
  int32_t ret = -EINVAL;
  const struct pcm_conv *conv;
  (void)cli;
  conv = dspd_getconv(format);
  if ( conv != NULL )
    {
      if ( ((params->stream & DSPD_PCM_SBIT_PLAYBACK) && conv->tofloat32) ||
	   ((params->stream & DSPD_PCM_SBIT_PLAYBACK) == 0) )
	{
	  if ( ((params->stream & DSPD_PCM_SBIT_CAPTURE) && conv->fromfloat32) ||
	       ((params->stream & DSPD_PCM_SBIT_CAPTURE) == 0) )
	    {
	      params->format = format;
	      ret = format;
	    }
	}
    }
  return ret;
}

const struct dspd_device_stat *dspd_pcmcli_device_info(struct dspd_pcmcli *client, int32_t sbit)
{
  const struct dspd_device_stat *ret = NULL;
  sbit &= client->streams;
  if ( client->state >= PCMCLI_STATE_OPEN && sbit != 0 )
    {
      if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
	ret = &client->playback.info;
      else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
	ret = &client->capture.info;
    }
  return ret;
}
static void setinfo_complete_cb(void *context, struct dspd_async_op *op)
{
  struct dspd_pcmcli *cli = op->data;
  //Fake completion of a pcmcli event.
  cli->pending_op = *op;
  complete_event(cli, op->error);
  
}

int32_t dspd_pcmcli_set_info(struct dspd_pcmcli *client, 
			     const struct dspd_cli_info *info,
			     dspd_aio_ccb_t complete,
			     void *arg)
{
  int32_t ret;
  struct dspd_cli_info *iptr;
  if ( client->state < PCMCLI_STATE_OPEN )
    {
      ret = -EBADFD;
    } else if ( client->complete == NULL && client->pending_op.error <= 0 )
    {
      if ( info == NULL )
	{
	  /*
	    Send default info.  This means take some info from /proc and let the
	    kernel fill in the rest.
	  */
	  memset(client->input, 0, sizeof(*info));
	  iptr = (struct dspd_cli_info*)client->input;
	  iptr->stream = -1;
	} else
	{
	  memcpy(client->input, info, sizeof(*info));
	}
      info = (const struct dspd_cli_info*)client->input;
      if ( complete )
	{
	  client->complete = complete;
	  client->completion_data = arg;
	  complete = setinfo_complete_cb;
	  memset(&client->pending_op, 0, sizeof(client->pending_op));
	  client->pending_op.error = EINPROGRESS;
	}
      //This is different than most operations because the aio context supports synchronous
      //calls tp dspd_aio_set_info in the same way as pcmcli supports synchronous calls to
      //most asynchronous functions.
      ret = dspd_aio_set_info(client->conn, info, complete, client);
      if ( ret < 0 )
	{
	  //The operation was not submitted.
	  client->pending_op.error = 0;
	  client->complete = NULL;
	  client->completion_data = NULL;
	}
    } else
    {
      ret = -EBUSY;
    }
  return ret;
}

struct dspd_aio_ctx *dspd_pcmcli_get_aio_ctx(struct dspd_pcmcli *client)
{
  return client->conn;
}

int32_t dspd_pcmcli_get_stream_index(struct dspd_pcmcli *cli, int32_t sbit)
{
  int32_t ret = -EINVAL;
  sbit &= ~cli->streams;
  if ( sbit == DSPD_PCM_SBIT_PLAYBACK )
    ret = cli->playback.stream_idx;
  else if ( sbit == DSPD_PCM_SBIT_CAPTURE )
    ret = cli->capture.stream_idx;
  return ret;
}

int32_t dspd_pcmcli_get_state(struct dspd_pcmcli *cli)
{
  return cli->state;
}
