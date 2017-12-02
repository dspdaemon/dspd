/*
 *  CBPOLL - epoll event loop
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
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include "sslib.h"
#include "cbpoll.h"

void cbpoll_close_fd(struct cbpoll_ctx *ctx, int index);


static int32_t find_fdata(struct cbpoll_ctx *ctx, int fd)
{
  size_t i;
  int32_t ret = -1;
  struct cbpoll_fd *f;
  for ( i = 0; i < ctx->max_fd; i++ )
    {
      f = &ctx->fdata[i];
      if ( f->refcnt > 0 && f->fd == fd )
	{
	  ret = i;
	  break;
	}
    }
  return ret;
}

static int msgpipe_fd_event(void *data, 
			    struct cbpoll_ctx *context,
			    int index,
			    int fd,
			    int revents)
{
  struct cbpoll_pipe_event pe;
  ssize_t ret;
  struct cbpoll_fd *f;
  if ( revents & POLLIN )
    {
      ret = read(fd, &pe, sizeof(pe));
      if ( ret == sizeof(pe) )
	{
	  if ( pe.msg == CBPOLL_PIPE_MSG_CALLBACK )
	    {
	      if ( pe.callback )
		pe.callback(context, &pe);
	      return 0;
	    }

	  if ( pe.index < 0 )
	    pe.index = find_fdata(context, pe.fd);
	      
	  if ( pe.index >= 0 )
	    {
	      f = &context->fdata[pe.index];
	      if ( (pe.fd < 0 || (pe.fd == f->fd)) && f->ops->pipe_event )
		{
		  if ( f->ops->pipe_event(f->data,
					  context,
					  pe.index,
					  f->fd,
					  &pe) < 0 )
		    {
		      cbpoll_close_fd(context, pe.index);
		    } else if ( pe.msg == CBPOLL_PIPE_MSG_DEFERRED_WORK )
		    {
		      cbpoll_unref(context, pe.index);
		    }
		}
	    }
	}
    }
  return 0;
}

static const struct cbpoll_fd_ops msgpipe_fd_ops = {
  .fd_event = msgpipe_fd_event,
};


void cbpoll_unref(struct cbpoll_ctx *ctx, int index)
{
  struct cbpoll_fd *fd;
  assert((size_t)index < ctx->max_fd);
  fd = &ctx->fdata[index];
  assert(fd->refcnt);
  fd->refcnt--;
  if ( fd->refcnt == 0 )
    {
      if ( (fd->flags & CBPOLLFD_FLAG_REMOVED) == 0 && fd->fd >= 0 )
	epoll_ctl(ctx->epfd,
		  EPOLL_CTL_DEL,
		  fd->fd,
		  NULL);
      if ( fd->ops->destructor )
	{
	  if ( fd->ops->destructor(fd->data,
				   ctx,
				   index,
				   fd->fd) )
	    {
	      if ( fd->fd >= 0 )
		close(fd->fd);
	    }
	} else
	{
	  if ( fd->fd >= 0 )
	    close(fd->fd);
	}
      fd->data = NULL;
      fd->fd = -1;
      fd->events = 0;
      fd->ops = NULL;
    }
}
void cbpoll_ref(struct cbpoll_ctx *ctx, int index)
{
  struct cbpoll_fd *fd;
  assert((size_t)index < ctx->max_fd);
  fd = &ctx->fdata[index];
  assert(fd->refcnt);
  fd->refcnt++;
}

void cbpoll_close_fd(struct cbpoll_ctx *ctx, int index)
{
  struct cbpoll_fd *fd = &ctx->fdata[index];
  if ( fd->fd >= 0 && (fd->flags & CBPOLLFD_FLAG_REMOVED) == 0 )
    {
      epoll_ctl(ctx->epfd,
		EPOLL_CTL_DEL,
		fd->fd,
		NULL);
    }
  fd->flags |= CBPOLLFD_FLAG_REMOVED;
  cbpoll_unref(ctx, index);
}

/*
  The idea is to be able to add events for virtual file descriptors while dispatching.  This will
  work because virtual file descriptors won't ever be set by epoll_wait() and there is guaranteed to
  be a slot available for each file descriptor.  The real file descriptor that triggers the event
  will always be lower than the first unused event slot so any new events will always be handled.
*/
int32_t cbpoll_get_dispatch_list(struct cbpoll_ctx *ctx, int32_t **count, struct epoll_event **events)
{
  int32_t ret;
  if ( ctx->dispatch_count >= 0 )
    {
      *events = ctx->events;
      *count = &ctx->dispatch_count;
      ret = 0;
    } else
    {
      ret = -EAGAIN;
    }
  return ret;
}

static void *cbpoll_thread(void *p)
{
  struct cbpoll_ctx *ctx = p;
  int ret;
  void *result = NULL;
  int i;
  struct epoll_event *ev;
  int32_t fd, idx, f;
  uint32_t len;
  struct cbpoll_fd *fdata;
  if ( ! ctx->name )
    set_thread_name("dspdcbpoll");
  else
    set_thread_name(ctx->name);
  while ( AO_load(&ctx->abort) == 0 )
    {
      ctx->dispatch_count = -1;
      if ( ctx->sleep )
	ctx->sleep(ctx->arg, ctx);
      ret = epoll_wait(ctx->epfd, ctx->events, ctx->max_fd, -1);
      if ( ret < 0 )
	{
	  if ( errno != EINTR )
	    {
	      result = (void*)(intptr_t)errno;
	      break;
	    } else
	    {
	      continue;
	    }
	}
      ctx->wq.overflow.fd = -1;
      ctx->dispatch_count = ret;
      for ( i = 0; i < ctx->dispatch_count; i++ )
	{
	  ev = &ctx->events[i];
	  fd = ev->data.u64 & 0xFFFFFFFF;
	  idx = ev->data.u64 >> 32;
	  fdata = &ctx->fdata[idx];
	  if ( fdata->refcnt )
	    {
	      if ( fdata->ops->fd_event(fdata->data,
					ctx,
					idx,
					fd,
					ev->events) < 0 )
		{
		  cbpoll_close_fd(ctx, idx);
		} else if ( ctx->wq.overflow.fd >= 0 )
		{
		  ctx->wq.overflow.callback(ctx,
					    fdata->data,
					    ctx->wq.overflow.arg,
					    ctx->wq.overflow.index,
					    ctx->wq.overflow.fd);
		  if ( ctx->wq.overflow.msg == CBPOLL_PIPE_MSG_DEFERRED_WORK )
		    cbpoll_unref(ctx, idx);
		  ctx->wq.overflow.fd = -1;
		}
	    }
	}
      if ( dspd_fifo_len(ctx->wq.fifo, &len) == 0 )
	{
	  if ( len > 0 )
	    {
	      dspd_mutex_lock(&ctx->wq.lock);
	      dspd_cond_signal(&ctx->wq.cond);
	      dspd_mutex_unlock(&ctx->wq.lock);
	    }
	}
    }
  dspd_thread_join(&ctx->wq.thread, NULL);
  for ( i = 0; i < ctx->max_fd; i++ )
    {
      fdata = &ctx->fdata[i];
      if ( fdata->refcnt )
	{
	  f = fdata->fd;
	  if ( fdata->fd >= 0 )
	    {
	      epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, fdata->fd, NULL);
	      close(fdata->fd);
	      fdata->fd = -1;
	    }
	  if ( fdata->ops->destructor )
	    fdata->ops->destructor(fdata->data,
				   ctx,
				   i,
				   f);
	}
    }
  return result;
}



void *cbpoll_get_extra_data(struct cbpoll_ctx *ctx)
{
  return ctx->wq.extra_data;
}

static void *async_work_thread(void *p)
{
  struct cbpoll_ctx *ctx = p;
  struct cbpoll_work *work;
  struct cbpoll_fd *fd;
  uint32_t len;
  if ( ctx->name )
    {
      char buf[256];
      sprintf(buf, "%s-wq", ctx->name);
      set_thread_name(buf);
    } else
    {
      set_thread_name("dspdcbpoll-wq");
    }

    
  while ( AO_load(&ctx->abort) == 0 )
    {
      //Wait for incoming data
      dspd_mutex_lock(&ctx->wq.lock);
      while ( AO_load(&ctx->abort) == 0 )
	{
	  if ( dspd_fifo_len(ctx->wq.fifo, &len) != 0 )
	    break;
	  if ( len != 0 )
	    break;
	  dspd_cond_wait(&ctx->wq.cond, &ctx->wq.lock);
	}
      dspd_mutex_unlock(&ctx->wq.lock);

      //Read events
      while ( dspd_fifo_riov(ctx->wq.fifo, (void**)&work, &len) == 0 )
	{
	  if ( len == 0 )
	    break;
	  assert(work->index < ctx->max_fd);
	  fd = &ctx->fdata[work->index];
	  assert(fd->fd == work->fd);
	  ctx->wq.extra_data = work->extra_data;
	  work->callback(ctx,
			 fd->data,
			 work->arg,
			 work->index,
			 work->fd);
	  dspd_fifo_rcommit(ctx->wq.fifo, 1);
	}
    }
  return NULL;
}

int32_t cbpoll_send_event(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *evt)
{
  ssize_t ret, err = 0;
  while ( (ret = write(ctx->event_pipe[1], evt, sizeof(*evt))) != sizeof(*evt))
    {
      if ( ret == 0 )
	{
	  err = -EPIPE;
	  break;
	}
      assert(ret < 0);
      if ( errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK )
	{
	  err = -errno;
	  break;
	}
    }
  return err;
}

void cbpoll_queue_work(struct cbpoll_ctx *ctx, struct cbpoll_work *wrk)
{
  int32_t written;
  struct cbpoll_fd *fd;
  written = dspd_fifo_write(ctx->wq.fifo, wrk, 1);
  assert(written >= 0);
  if ( written == 0 )
    {
      if ( ctx->wq.overflow.fd < 0 )
	{
	  memcpy(&ctx->wq.overflow, wrk, sizeof(*wrk));
	} else
	{
	  fd = &ctx->fdata[wrk->index];
	  wrk->callback(ctx,
			fd->data,
			wrk->arg,
			wrk->index,
			wrk->fd);
	  if ( wrk->msg == CBPOLL_PIPE_MSG_DEFERRED_WORK )
	    cbpoll_unref(ctx, wrk->index);
	}
    }
}


int32_t cbpoll_get_events(struct cbpoll_ctx *ctx, int32_t index)
{
  struct cbpoll_fd *f = &ctx->fdata[index];
  assert(index < ctx->max_fd);
  assert(f->refcnt);
  return f->events;
}

int32_t cbpoll_set_events(struct cbpoll_ctx *ctx, 
			  int32_t index,
			  int32_t events)
{
  struct cbpoll_fd *f = &ctx->fdata[index];
  int32_t ret;
  struct epoll_event evt;
  assert(index < ctx->max_fd);
  if ( f->events != events || 
       (f->events & EPOLLONESHOT) || 
       (events & EPOLLONESHOT) )
    {
      assert(f->refcnt);
      if ( f->fd >= 0 )
	{
	  evt.events = events;
	  if ( evt.events & (EPOLLIN|EPOLLOUT) )
	    evt.events |= EPOLLRDHUP;
	  evt.data.u64 = index;
	  evt.data.u64 <<= 32;
	  evt.data.u64 |= f->fd;
	  ret = epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, f->fd, &evt);
	  if ( ret < 0 )
	    ret = -errno;
	  else
	    f->events = events;
	} else
	{
	  f->events = events;
	  ret = 0;
	}
    } else
    {
      ret = 0;
    }
  return ret;
}

int32_t cbpoll_add_fd(struct cbpoll_ctx *ctx, 
		      int32_t fd,
		      int32_t events,
		      const struct cbpoll_fd_ops *ops,
		      void *arg)
{
  int32_t index = -EINVAL, ret;
  size_t i;
  struct cbpoll_fd *f;
  struct epoll_event evt;
  for ( i = 0; i < ctx->max_fd; i++ )
    {
      f = &ctx->fdata[i];
      if ( f->refcnt == 0 )
	{
	  evt.events = events;
	  if ( fd >= 0 )
	    {
	      evt.data.u64 = i;
	      evt.data.u64 <<= 32;
	      evt.data.u64 |= fd;
	      ret = epoll_ctl(ctx->epfd, 
			      EPOLL_CTL_ADD,
			      fd,
			      &evt);
	    } else
	    {
	      ret = 0;
	    }
	  if ( ret == 0 )
	    {
	      index = i;
	      f->fd = fd;
	      f->events = events;
	      f->refcnt = 1;
	      f->ops = ops;
	      f->flags = 0;
	      f->data = arg;
	    } else
	    {
	      index = -errno;
	    }
	  break;
	}
    }
  return index;
}

void cbpoll_queue_deferred_work(struct cbpoll_ctx *ctx,
				int32_t index,
				int64_t arg,
				void (*callback)(struct cbpoll_ctx *ctx,
						 void *data,
						 int64_t arg,
						 int32_t index,
						 int32_t fd))
{
  struct cbpoll_work work;
  work.index = index;
  work.fd = ctx->fdata[index].fd;
  work.arg = arg;
  work.msg = CBPOLL_PIPE_MSG_DEFERRED_WORK;
  work.callback = callback;
  memset(work.extra_data, 0, sizeof(work.extra_data));
  cbpoll_ref(ctx, index);
  cbpoll_queue_work(ctx, &work);
}

void cbpoll_deferred_work_complete(struct cbpoll_ctx *ctx,
				   int32_t index,
				   int64_t arg)
{
  struct cbpoll_pipe_event evt;
  struct cbpoll_fd *fd = &ctx->fdata[index];
  evt.fd = fd->fd;
  evt.index = index;
  evt.stream = -1;
  evt.msg = CBPOLL_PIPE_MSG_DEFERRED_WORK;
  evt.arg = arg;
  cbpoll_send_event(ctx, &evt);
}



int32_t cbpoll_init(struct cbpoll_ctx *ctx, 
		    int32_t  flags,
		    uint32_t max_fds)
{
  int32_t ret;
  memset(ctx, 0, sizeof(*ctx));
  ctx->epfd = -1;
  ctx->event_pipe[0] = -1;
  ctx->event_pipe[1] = -1;
  max_fds++; //add 1 for pipe
  ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
  if ( ctx->epfd < 0 )
    {
      ret = -errno;
      goto out;
    }
  
  ctx->events = calloc(max_fds, sizeof(*ctx->events));
  if ( ! ctx->events )
    {
      ret = -errno;
      goto out;
    }
  ctx->fdata = calloc(max_fds, sizeof(*ctx->fdata));
  if ( ! ctx->fdata )
    {
      ret = -errno;
      goto out;
    }

  if ( pipe2(ctx->event_pipe, O_CLOEXEC) < 0 )
    {
      ret = -errno;
      goto out;
    }

  fcntl(ctx->event_pipe[0], F_SETFL, fcntl(ctx->event_pipe[0], F_GETFL) | O_NONBLOCK);
  ret = dspd_mutex_init(&ctx->wq.lock, NULL);
  if ( ret )
    goto out;
  ret = dspd_cond_init(&ctx->wq.cond, NULL);
  if ( ret )
    goto out;
  ret = dspd_fifo_new(&ctx->wq.fifo, 
		      max_fds,
		      sizeof(struct cbpoll_work),
		      NULL);
  if ( ret )
    goto out;
  ctx->max_fd = max_fds;
  ret = cbpoll_add_fd(ctx, ctx->event_pipe[0], EPOLLIN, &msgpipe_fd_ops, NULL);
  if ( ret < 0 )
    goto out;
  
  

  return 0;

 out:
  dspd_mutex_destroy(&ctx->wq.lock);
  dspd_cond_destroy(&ctx->wq.cond);
  close(ctx->epfd);
  close(ctx->event_pipe[0]);
  close(ctx->event_pipe[1]);
  if ( ctx->wq.fifo )
    dspd_fifo_delete(ctx->wq.fifo);
  free(ctx->fdata);
  free(ctx->events);
  
  return ret;
}

int32_t cbpoll_start(struct cbpoll_ctx *ctx)
{
  int32_t ret;
  ret = dspd_thread_create(&ctx->wq.thread, 
			   NULL,
			   async_work_thread,
			   ctx);
  if ( ret == 0 )
    {
      ret = dspd_thread_create(&ctx->thread,
			       NULL,
			       cbpoll_thread,
			       ctx);
      if ( ret != 0 )
	{
	  AO_store(&ctx->abort, 1);
	  dspd_mutex_lock(&ctx->wq.lock);
	  dspd_cond_signal(&ctx->wq.cond);
	  dspd_mutex_unlock(&ctx->wq.lock);
	  dspd_thread_join(&ctx->wq.thread, NULL);
	  AO_store(&ctx->abort, 0);
	}
    }
  return ret * -1;
}
int32_t cbpoll_run(struct cbpoll_ctx *ctx)
{
  int32_t ret;
  ret = dspd_thread_create(&ctx->wq.thread, 
			   NULL,
			   async_work_thread,
			   ctx);
  if ( ret == 0 )
    {
      ctx->thread.thread = pthread_self();
      ctx->thread.init = true;
      ret = (intptr_t)cbpoll_thread(ctx);
      ctx->thread.init = false;
    }
  return ret * -1;
}


void cbpoll_destroy(struct cbpoll_ctx *ctx)
{
  struct cbpoll_pipe_event evt;
  AO_store(&ctx->abort, 1);
  evt.fd = -1;
  evt.index = -1;
  evt.stream = -1;
  evt.msg = 0;
  evt.arg = 0;
  cbpoll_send_event(ctx, &evt);
}

void cbpoll_set_callbacks(struct cbpoll_ctx *ctx,
			  void *arg,
			  void (*sleep)(void *arg, struct cbpoll_ctx *context),
			  void (*wake)(void *arg, struct cbpoll_ctx *context))
{
  ctx->sleep = sleep;
  ctx->wake = wake;
  ctx->arg = arg;
}

uint32_t cbpoll_refcnt(struct cbpoll_ctx *ctx, int index)
{
  uint32_t ret;
  if ( index < 0 || index > ctx->max_fd )
    ret = 0;
  else
    ret = ctx->fdata[index].refcnt;
  return ret;
}

int32_t cbpoll_set_name(struct cbpoll_ctx *ctx, const char *threadname)
{
  free(ctx->name);
  ctx->name = strdup(threadname);
  if ( ! ctx->name )
    return -ENOMEM;
  return 0;
}
