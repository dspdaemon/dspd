/*
 *  CBPOLL - epoll event loop
 *
 *   Copyright (c) 2018 Tim Smith <dspdaemon _AT_ yandex.com>
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
#include <sys/eventfd.h>
#include "sslib.h"
#include "cbpoll.h"

void cbpoll_close_fd(struct cbpoll_ctx *ctx, int index);
static int eventfd_event(void *data, 
			 struct cbpoll_ctx *context,
			 int index,
			 int fd,
			 int revents);
static int timer_fd_event(void *data, 
			  struct cbpoll_ctx *context,
			  int index,
			  int fd,
			  int revents);
bool timer_fd_destructor(void *data,
			 struct cbpoll_ctx *context,
			 int index,
			 int fd);
static bool eventfd_destructor(void *data,
			       struct cbpoll_ctx *context,
			       int index,
			       int fd);
static void cbtimer_dispatch(struct cbpoll_ctx *ctx, dspd_time_t timeout);

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


static uint32_t _cbpoll_unref(struct cbpoll_ctx *ctx, int index, bool recursive)
{
  struct cbpoll_fd *fd;
  DSPD_ASSERT((size_t)index < ctx->max_fd);
  fd = &ctx->fdata[index];
  DSPD_ASSERT(fd->refcnt);
  fd->refcnt--;
  if ( fd->ops->refcnt_changed )
    fd->ops->refcnt_changed(fd->data, ctx, index, fd->fd, fd->refcnt);
  if ( recursive == true && fd->associated_context >= 0 )
    if ( _cbpoll_unref(ctx, fd->associated_context, false) == 0 )
      fd->associated_context = -1;
    
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
      if ( ctx->pending_timers != NULL )
	cbpoll_cancel_timer(ctx, index);
      if ( fd->associated_context >= 0 && fd->associated_context == index )
	ctx->fdata[fd->associated_context].associated_context = -1;
      fd->associated_context = -1;
    }
  return fd->refcnt;
}

uint32_t cbpoll_unref(struct cbpoll_ctx *ctx, int index)
{
  return _cbpoll_unref(ctx, index, true);
}

static uint32_t _cbpoll_ref(struct cbpoll_ctx *ctx, int index, bool recursive)
{
  struct cbpoll_fd *fd;
  uint32_t refcnt;
  DSPD_ASSERT((size_t)index < ctx->max_fd);
  fd = &ctx->fdata[index];
  DSPD_ASSERT(fd->refcnt);
  fd->refcnt++;
  if ( fd->ops->refcnt_changed )
    fd->ops->refcnt_changed(fd->data, ctx, index, fd->fd, fd->refcnt);
  if ( recursive == true && fd->associated_context >= 0 )
    {
      refcnt = _cbpoll_ref(ctx, fd->associated_context, false);
      (void)refcnt;
      DSPD_ASSERT(refcnt > 1U);
    }
  return fd->refcnt;
}

uint32_t cbpoll_ref(struct cbpoll_ctx *ctx, int index)
{
  return _cbpoll_ref(ctx, index, true);
}

void cbpoll_link(struct cbpoll_ctx *ctx, int index1, int index2)
{
  struct cbpoll_fd *fd1, *fd2;
  DSPD_ASSERT(index1 >= 0 && index2 >= 0);
  DSPD_ASSERT(index1 <= (int)ctx->max_fd && index2 <= (int)ctx->max_fd);
  fd1 = &ctx->fdata[index1];
  fd2 = &ctx->fdata[index2];
  DSPD_ASSERT(fd1->associated_context < 0 && fd2->associated_context < 0);
  DSPD_ASSERT(fd1->refcnt > 0 && fd2->refcnt > 0);
  fd1->associated_context = index2;
  fd2->associated_context = index1;
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
  fd->flags &= ~CBPOLLFD_FLAG_EVENTS_CHANGED;
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
  else if ( strlen(ctx->name) > 0 )
    set_thread_name(ctx->name);

  while ( AO_load(&ctx->abort) == 0 )
    {
      dspd_mutex_lock(&ctx->loop_lock);
      ctx->dispatch_count = -1;
      ctx->fdata_idx = -1;
      if ( ctx->sleep )
	ctx->sleep(ctx->arg, ctx);

      if ( ctx->timeout_changed == true )
	{
	  dspd_timer_set(&ctx->timer, ctx->next_timeout, 0);
	  ctx->timeout_changed = false;
	}
      dspd_mutex_unlock(&ctx->loop_lock);

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
      dspd_mutex_lock(&ctx->loop_lock);
      ctx->dispatch_count = ret;
      ctx->wq.overflow.callback = NULL;
      ctx->fdata_idx = -1;
      for ( i = 0; i < ctx->dispatch_count; i++ )
	{
	  ev = &ctx->events[i];
	  fd = ev->data.u64 & 0xFFFFFFFF;
	  idx = ev->data.u64 >> 32;
	  fdata = &ctx->fdata[idx];
	  if ( fdata->refcnt )
	    {
	      //Set the index so that cbpoll_set_events() caches the values for this context.  Setting
	      //another context passes through as it always did.
	      ctx->fdata_idx = idx;
	      if ( fdata->ops->fd_event(fdata->data,
					ctx,
					idx,
					fd,
					ev->events) < 0 )
		{
		  cbpoll_close_fd(ctx, idx);
		} else if ( ctx->wq.overflow.callback )
		{
		  ctx->wq.overflow.callback(ctx,
					    fdata->data,
					    &ctx->wq.overflow,
					    false);
		  if ( ctx->wq.overflow.msg == CBPOLL_PIPE_MSG_DEFERRED_WORK )
		    cbpoll_unref(ctx, idx);
		  ctx->wq.overflow.callback = NULL;
		}
	      //The idea is to free the user of the callback from caring about system calls made in
	      //setting the epoll events to wait for.  So, a POLLIN might be requested in on function
	      //and cancelled in another without making 2 system calls.  A POLLOUT might be needed in
	      //yet another function so the two will not need to coordinate or cause extra syscalls.
	      if ( fdata->flags & CBPOLLFD_FLAG_EVENTS_CHANGED )
		{
		  ctx->fdata_idx = -1; //make invalid index to commit changes now
		  cbpoll_set_events(ctx, idx, fdata->events);
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
      dspd_mutex_unlock(&ctx->loop_lock);
    }
  dspd_thread_join(&ctx->wq.thread, NULL);


  dspd_mutex_lock(&ctx->loop_lock);
  for ( i = 0; i < ctx->max_fd; i++ )
    {
      fdata = &ctx->fdata[i];
      ctx->fdata_idx = i;
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
  dspd_mutex_unlock(&ctx->loop_lock);
  return result;
}




static void *async_work_thread(void *p)
{
  struct cbpoll_ctx *ctx = p;
  struct cbpoll_work *work;
  struct cbpoll_fd *fd;
  uint32_t len;
  if ( ctx->name != NULL && strlen(ctx->name) > 0 )
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
	  DSPD_ASSERT(work->index < ctx->max_fd);
	  fd = &ctx->fdata[work->index];
	  work->callback(ctx,
			 fd->data,
			 work,
			 true);
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
      DSPD_ASSERT(ret < 0);
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
  DSPD_ASSERT(written >= 0);
  if ( written == 0 )
    {
      if ( ctx->wq.overflow.callback == NULL )
	{
	  memcpy(&ctx->wq.overflow, wrk, sizeof(*wrk));
	} else
	{
	  fd = &ctx->fdata[wrk->index];
	  wrk->callback(ctx,
			fd->data,
			wrk,
			false);
	  if ( wrk->msg == CBPOLL_PIPE_MSG_DEFERRED_WORK )
	    cbpoll_unref(ctx, wrk->index);
	}
    }
}


int32_t cbpoll_get_events(struct cbpoll_ctx *ctx, int32_t index)
{
  struct cbpoll_fd *f = &ctx->fdata[index];
  DSPD_ASSERT(index < ctx->max_fd);
  DSPD_ASSERT(f->refcnt);
  return f->events;
}

int32_t cbpoll_disable_events(struct cbpoll_ctx *ctx, 
			      int32_t index,
			      int32_t events)
{
  struct cbpoll_fd *f = &ctx->fdata[index];
  return cbpoll_set_events(ctx, index, f->events & ~events);
}

int32_t cbpoll_enable_events(struct cbpoll_ctx *ctx, 
			     int32_t index,
			     int32_t events)
{
  struct cbpoll_fd *f = &ctx->fdata[index];
  return cbpoll_set_events(ctx, index, f->events | events);
}

int32_t cbpoll_set_events(struct cbpoll_ctx *ctx, 
			  int32_t index,
			  int32_t events)
{
  struct cbpoll_fd *f;
  int32_t ret = 0;
  struct epoll_event evt;
  f = &ctx->fdata[index];
  DSPD_ASSERT(index < ctx->max_fd);
  if ( f->events != events || 
       (f->flags & CBPOLLFD_FLAG_EVENTS_CHANGED) ||
       (f->events & EPOLLONESHOT) || 
       (events & EPOLLONESHOT) )
    {
      DSPD_ASSERT(f->refcnt);
      if ( f->fd >= 0 )
	{
	  if ( index == ctx->fdata_idx )
	    {
	      f->events = events;
	      f->flags |= CBPOLLFD_FLAG_EVENTS_CHANGED;
	    } else
	    {
	      evt.events = events;
	      if ( evt.events & (EPOLLIN|EPOLLOUT) )
		evt.events |= EPOLLRDHUP;
	  
	      evt.data.u64 = index;
	      evt.data.u64 <<= 32;
	      evt.data.u64 |= f->fd;
	      ret = epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, f->fd, &evt);
	      if ( ret < 0 )
		{
		  ret = -errno;
		} else
		{
		  f->events = events;
		  f->flags &= ~CBPOLLFD_FLAG_EVENTS_CHANGED;
		}
	    }
	} else
	{
	  if ( f->ops->set_events )
	    {
	      ret = f->ops->set_events(f->data, ctx, index, f->fd, events);
	      if ( ret == 0 )
		f->events = events;
	    } else
	    {
	      f->events = events;
	      ret = 0;
	    }
	  f->flags &= ~CBPOLLFD_FLAG_EVENTS_CHANGED;
	}
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
	      if ( ops->set_events )
		ret = ops->set_events(arg, ctx, i, fd, events);
	      else
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
						 struct cbpoll_work *wrk,
						 bool async))
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

static const struct cbpoll_fd_ops timerfd_ops = {
  .fd_event = timer_fd_event,
  .destructor = timer_fd_destructor
};

static const struct cbpoll_fd_ops eventfd_ops = {
  .fd_event = eventfd_event,
  .destructor = eventfd_destructor,
};

static void destroy(struct cbpoll_ctx *ctx)
{
  dspd_mutex_destroy(&ctx->wq.lock);
  dspd_cond_destroy(&ctx->wq.cond);
  dspd_mutex_destroy(&ctx->loop_lock);
  dspd_mutex_destroy(&ctx->work_lock);
  close(ctx->epfd);
  ctx->epfd = -1;
  close(ctx->event_pipe[0]);
  close(ctx->event_pipe[1]);
  ctx->event_pipe[0] = -1;
  ctx->event_pipe[1] = -1;
  if ( ctx->wq.fifo )
    dspd_fifo_delete(ctx->wq.fifo);
  free(ctx->fdata);
  ctx->fdata = NULL;
  free(ctx->events);
  ctx->events = NULL;
  if ( ctx->timer.fd >= 0 )
    dspd_timer_destroy(&ctx->timer);
  free(ctx->pending_timers);
  ctx->pending_timers = NULL;
  close(ctx->eventfd.fd);
  free(ctx->aio_list);
  ctx->aio_list = NULL;
  free(ctx->cbtimer_objects);
  ctx->cbtimer_objects = NULL;
  free(ctx->pending_cbtimer_list);
  ctx->pending_cbtimer_list = NULL;
  free(ctx->cbtimer_dispatch_list);
  ctx->cbtimer_dispatch_list = NULL;
}

int32_t cbpoll_init(struct cbpoll_ctx *ctx, 
		    int32_t  flags,
		    uint32_t max_fds)
{
  int32_t ret;
  struct pollfd pfd;
  size_t i;
  memset(ctx, 0, sizeof(*ctx));
  ctx->epfd = -1;
  ctx->event_pipe[0] = -1;
  ctx->event_pipe[1] = -1;
  ctx->timer.fd = -1;
  ctx->eventfd.fd = -1;
  ctx->fdata_idx = -1;
  max_fds++; //add 1 for pipe
  if ( flags & CBPOLL_FLAG_TIMER )
    max_fds++;
  if ( flags & CBPOLL_FLAG_AIO_FIFO )
    max_fds++;

  ret = dspd_mutex_init(&ctx->loop_lock, NULL);
  if ( ret > 0 )
    {
      ret *= -1;
      goto out;
    }
  ret = dspd_mutex_init(&ctx->work_lock, NULL);
  if ( ret > 0 )
    {
      ret *= -1;
      goto out;
    }

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
  for ( i = 0; i < max_fds; i++ )
    ctx->fdata[i].associated_context = -1;
  

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
		      max_fds * 2UL,
		      sizeof(struct cbpoll_work),
		      NULL);
  if ( ret )
    goto out;
  ctx->max_fd = max_fds;
  ret = cbpoll_add_fd(ctx, ctx->event_pipe[0], EPOLLIN, &msgpipe_fd_ops, NULL);
  if ( ret < 0 )
    goto out;
  
  if ( flags & CBPOLL_FLAG_TIMER )
    {
      ret = dspd_timer_init(&ctx->timer);
      if ( ret < 0 )
	goto out;
      ret = dspd_timer_getpollfd(&ctx->timer, &pfd);
      if ( ret < 0 )
	goto out;
      ret = cbpoll_add_fd(ctx, pfd.fd, pfd.events, &timerfd_ops, NULL);
      if ( ret < 0 )
	goto out;

      ctx->pending_timers = calloc(ctx->max_fd, sizeof(*ctx->pending_timers));
      if ( ! ctx->pending_timers )
	{
	  ret = -ENOMEM;
	  goto out;
	}
    }
  if ( flags & CBPOLL_FLAG_CBTIMER )
    {
      ctx->pending_cbtimer_list = calloc(ctx->max_fd, sizeof(ctx->pending_cbtimer_list[0]));
      ctx->cbtimer_objects = calloc(ctx->max_fd, sizeof(ctx->cbtimer_objects[0]));
      ctx->cbtimer_dispatch_list = calloc(ctx->max_fd, sizeof(ctx->cbtimer_dispatch_list[0]));
      if ( ! (ctx->pending_cbtimer_list && 
	      ctx->cbtimer_objects &&
	      ctx->cbtimer_dispatch_list) )
	{
	  ret = -ENOMEM;
	  goto out;
	}
    }
  if ( flags & CBPOLL_FLAG_AIO_FIFO )
    {
      ctx->eventfd.fd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
      if ( ctx->eventfd.fd < 0 )
	goto out;
      dspd_ts_clear(&ctx->eventfd.tsval);
      ctx->aio_list = calloc(ctx->max_fd, sizeof(*ctx->aio_list));
      if ( ! ctx->aio_list )
	goto out;
    }
  ret = 0;

 out:
  if ( ret < 0 )
    destroy(ctx);
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
  dspd_thread_join(&ctx->thread, NULL);
  destroy(ctx);
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
  if ( index < 0 || index >= ctx->max_fd )
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

struct cbpoll_fd *cbpoll_get_fdata(struct cbpoll_ctx *ctx, int32_t index)
{
  struct cbpoll_fd *ret = NULL;
  if ( index >= 0 && index < ctx->max_fd )
    ret = &ctx->fdata[index];
  return ret;
}

void cbpoll_set_timer(struct cbpoll_ctx *ctx, size_t index, dspd_time_t timeout)
{
  DSPD_ASSERT(index < ctx->max_fd && ctx->pending_timers != NULL);
  DSPD_ASSERT(ctx->fdata[index].ops->timer_event);
  if ( ctx->pending_timers[index] != timeout )
    {
      if ( ctx->next_timeout > timeout || ctx->next_timeout == 0 )
	{
	  ctx->next_timeout = timeout;
	  ctx->timeout_changed = true;
	}
      if ( ctx->timer_idx <= index )
	ctx->timer_idx = index + 1;
      ctx->pending_timers[index] = timeout;
    }
}

void cbpoll_cancel_timer(struct cbpoll_ctx *ctx, size_t index)
{
  DSPD_ASSERT(index < ctx->max_fd && ctx->pending_timers != NULL);
  //The timer may still fire one time.  After that it will be canceled for real.
  ctx->pending_timers[index] = 0;
}

bool timer_fd_destructor(void *data,
			 struct cbpoll_ctx *context,
			 int index,
			 int fd)
{
  free(context->pending_timers);
  context->pending_timers = NULL;
  return true;
}

static int timer_fd_event(void *data, 
			  struct cbpoll_ctx *context,
			  int index,
			  int fd,
			  int revents)
{
  size_t i;
  struct cbpoll_fd *f;
  dspd_time_t t, next, newtimeout = UINT64_MAX;
  ssize_t timer_index;
  if ( revents & POLLIN )
    {
      timer_index = -1;
      next = context->next_timeout;
      for ( i = 0; i < context->timer_idx; i++ )
	{
	  t = context->pending_timers[i];
	  if ( t )
	    {
	      if ( t <= next )
		{
		  //This timer expired.
		  f = &context->fdata[i];
		  //All timers are oneshot
		  context->pending_timers[i] = 0;
		  f->ops->timer_event(f->data,
				      context,
				      i,
				      f->fd,
				      t);
		}
	      t = context->pending_timers[i];
	      if ( t >= 0 )	      
		{
		  //This timer is set.
		  timer_index = i;
		  if ( t < newtimeout )
		    newtimeout = t;
		}
	    }	    
	}
      if ( context->pending_cbtimer_list )
	cbtimer_dispatch(context, dspd_get_time());
      if ( newtimeout != context->next_timeout )
	{
	  //Timeout changed
	  context->timer_idx = timer_index + 1;
	  if ( timer_index < 0 )
	    context->next_timeout = 0; //No more timeouts
	  else
	    context->next_timeout = newtimeout; //New timeout value
	  //Timeout needs changed later.  Not now because the value could change
	  //between now and the next epoll_wait().
	  context->timeout_changed = true;
	}
    }
  return 0;
}





static int aiofd_event(void *data, 
		       struct cbpoll_ctx *context,
		       int index,
		       int fd,
		       int revents)
{
  int32_t ret;
  struct dspd_aio_ctx *aio = data;
  ret = dspd_aio_process(aio, revents, 0);
  if ( ret == 0 || ret == -EINPROGRESS )
    ret = cbpoll_set_events(context, index, dspd_aio_block_directions(aio));
  return ret;
}
static const struct cbpoll_fd_ops aiofd_ops = {
  .fd_event = aiofd_event,
};

static void aio_fd_ready(struct dspd_aio_ctx *ctx, void *arg)
{
  cbpoll_set_events(arg, ctx->slot, dspd_aio_block_directions(ctx));
}

static void aio_fifo_ready(struct dspd_aio_ctx *ctx, void *arg)
{
  struct cbpoll_ctx *cbpoll = arg;
  uint64_t val = 1;
  ssize_t err;
  if ( cbpoll->wake_self == false )
    {
      if ( dspd_aio_fifo_test_events(ctx->ops_arg, dspd_aio_block_directions(ctx)) )
	{
	  if ( dspd_test_and_set(&cbpoll->eventfd.tsval) != DSPD_TS_SET )
	    {
	      while ( write(cbpoll->eventfd.fd, &val, sizeof(val)) < 0 )
		{
		  err = errno;
		  if ( err != EINTR && err != EWOULDBLOCK && err != EAGAIN )
		    break;
		}
	    }
	  cbpoll->wake_self = true;
	}
    }
}

static void io_submitted_cb(struct dspd_aio_ctx *aio, 
			    const struct dspd_async_op *op,
			    void *arg)
{
  struct cbpoll_ctx *cbctx = arg;
  cbpoll_ref(cbctx, aio->slot);
  (void)cbpoll_set_events(cbctx, aio->slot, dspd_aio_block_directions(aio));
}

static void io_completed_cb(struct dspd_aio_ctx *aio, 
			    const struct dspd_async_op *op,
			    void *arg)
{
  struct cbpoll_ctx *cbctx = arg;
  cbpoll_unref(cbctx, aio->slot);
  (void)cbpoll_set_events(cbctx, aio->slot, dspd_aio_block_directions(aio));
}

static void io_submitted_cb2(struct dspd_aio_ctx *aio, 
			     const struct dspd_async_op *op,
			     void *arg)
{
  struct cbpoll_ctx *cbctx = arg;
  cbpoll_ref(cbctx, aio->slot);
}

static void io_completed_cb2(struct dspd_aio_ctx *aio, 
			     const struct dspd_async_op *op,
			     void *arg)
{
  struct cbpoll_ctx *cbctx = arg;
  cbpoll_unref(cbctx, aio->slot);
}

int32_t cbpoll_aio_new(struct cbpoll_ctx *cbpoll, 
		       struct dspd_aio_ctx **aio,
		       const char *addr,
		       void *context)
{
  struct dspd_aio_ctx *c;
  int32_t ret;
  ret = dspd_aio_new(&c, DSPD_AIO_DEFAULT);
  if ( ret == 0 )
    {
      if ( context )
	ret = dspd_aio_connect(c, NULL, context, &dspd_aio_fifo_eventfd_ops, &cbpoll->eventfd);
      else
	ret = dspd_aio_connect(c, addr, context, NULL, NULL);
      if ( ret < 0 )
	dspd_aio_delete(c);
      else
	*aio = c;
    }
  return ret;
}



int32_t cbpoll_add_aio(struct cbpoll_ctx *context, struct dspd_aio_ctx *aio, int32_t associated_context)
{
  int32_t fd, ret = -EINVAL;
  size_t i;
  fd = dspd_aio_get_iofd(aio);
  if ( fd >= 0 )
    {
      ret = cbpoll_add_fd(context, fd, dspd_aio_block_directions(aio), &aiofd_ops, aio); 
      if ( ret == 0 )
	{
	  dspd_aio_set_ready_cb(aio, aio_fd_ready, context);
	  aio->slot = ret;
	  if ( associated_context >= 0 )
	    cbpoll_link(context, aio->slot, associated_context);
	  aio->io_submitted = io_submitted_cb;
	  aio->io_completed = io_completed_cb;
	  aio->io_arg = context;
	}
    } else
    {
      if ( context->aio_list )
	{
	  for ( i = 0; i < context->max_fd; i++ )
	    {
	      if ( context->aio_list[i] == NULL )
		{
		  context->aio_list[i] = aio;
		  if ( context->aio_idx <= i )
		    context->aio_idx = i + 1;
		  dspd_aio_set_ready_cb(aio, aio_fifo_ready, context);
		  aio->aio_index = i;
		  if ( associated_context >= 0 )
		    {
		      aio->slot = associated_context;
		      aio->io_submitted = io_submitted_cb2;
		      aio->io_completed = io_completed_cb2;
		      aio->io_arg = context;
		      cbpoll_ref(context, aio->slot);
		    } else
		    {
		      aio->slot = -1;
		    }
		  ret = i;
		  break;
		}
	    }
	}
    }
  return ret;
}

void cbpoll_remove_aio(struct cbpoll_ctx *context, struct dspd_aio_ctx *aio)
{
  if ( context->aio_list != NULL && aio->aio_index >= 0 )
    {
      context->aio_list[aio->aio_index] = NULL;
      aio->aio_index = -1;
    }
  if ( aio->slot >= 0 )
    {
      cbpoll_unref(context, aio->slot);
      aio->slot = -1;
    }
}

static bool eventfd_destructor(void *data,
			       struct cbpoll_ctx *context,
			       int index,
			       int fd)
{
  free(context->aio_list);
  context->aio_list = NULL;
  return true;
}

static int eventfd_event(void *data, 
			 struct cbpoll_ctx *context,
			 int index,
			 int fd,
			 int revents)
{
  uint64_t val;
  size_t i;
  ssize_t aio_idx = -1;
  struct dspd_aio_ctx *aio;
  int32_t ret;
  if ( revents & POLLIN )
    {
      context->wake_self = false;
      if ( read(fd, &val, sizeof(val)) == sizeof(val) )
	dspd_ts_clear(&context->eventfd.tsval);
    }
  for ( i = 0; i < context->aio_idx; i++ )
    {
      aio = context->aio_list[i];
      if ( aio )
	{
	  ret = dspd_aio_process(aio, 0, 0);
	  if ( ret < 0 )
	    context->aio_list[i] = NULL;
	  if ( context->aio_list[i] )
	    aio_idx = i;
	}
    }
  context->aio_idx = aio_idx + 1;
  return 0;
}

struct dspd_cbtimer *dspd_cbtimer_new(struct cbpoll_ctx *ctx, 
				      dspd_cbtimer_cb_t callback,
				      void *arg)
{
  struct dspd_cbtimer *ret = NULL;
  size_t i;
  if ( ctx->cbtimer_objects )
    {
      for ( i = 0; i < ctx->max_fd; i++ )
	{
	  if ( ctx->cbtimer_objects[i].callback == NULL )
	    {
	      ret = &ctx->cbtimer_objects[i];
	      ret->callback = callback;
	      ret->arg = arg;
	      ret->cbpoll = ctx;
	      break;
	    }
	}
    }
  return ret;
}

void dspd_cbtimer_cancel(struct dspd_cbtimer *timer)
{
  if ( timer->prev )
    timer->prev->next = timer->next;
  else if ( timer == timer->cbpoll->pending_cbtimer_list )
    timer->cbpoll->pending_cbtimer_list = timer->next;
  if ( timer->next )
    timer->next->prev = timer->prev;
  timer->timeout = 0;
  timer->period = 0;
  timer->next = NULL;
  timer->prev = NULL;
}

void dspd_cbtimer_delete(struct dspd_cbtimer *timer)
{
  dspd_cbtimer_cancel(timer);
  memset(timer, 0, sizeof(*timer));
}

void dspd_cbtimer_set(struct dspd_cbtimer *timer, dspd_time_t timeout, dspd_time_t period)
{
  struct dspd_cbtimer *t, **prev = &timer->cbpoll->pending_cbtimer_list;
  timer->next = NULL;
  timer->prev = NULL;
  timer->timeout = timeout;
  timer->period = period;
  for ( t = timer->cbpoll->pending_cbtimer_list; t; t = t->next )
    {
      if ( t->timeout >= timer->timeout )
	{
	  timer->next = t;
	  timer->prev = t->prev;
	  t->prev = timer;
	  break;
	}
      prev = &t->next;
    }
  *prev = timer;
  if ( timer->cbpoll->next_timeout < timeout || timer->cbpoll->next_timeout == 0 )
    {
      timer->cbpoll->next_timeout = timeout;
      timer->cbpoll->timeout_changed = true;
    }
}

void dspd_cbtimer_fire(struct dspd_cbtimer *timer)
{
  timer->timeout = timer->cbpoll->last_time;
  timer->cbpoll->next_timeout = timer->cbpoll->last_time;
  timer->cbpoll->timeout_changed = true;
  if ( timer->cbpoll->pending_cbtimer_list == timer )
    return;
  if ( timer->prev )
    timer->prev->next = timer->next;
  if ( timer->next )
    timer->next->prev = timer->prev;
  timer->next = timer->cbpoll->pending_cbtimer_list;
  timer->cbpoll->pending_cbtimer_list = timer;
}

dspd_time_t dspd_cbtimer_get_timeout(struct dspd_cbtimer *t)
{
  return t->timeout;
}

static void cbtimer_dispatch(struct cbpoll_ctx *ctx, dspd_time_t timeout)
{
  size_t count = 0, i;
  struct dspd_cbtimer *t;
  ctx->last_time = timeout;
  for ( t = ctx->pending_cbtimer_list; t; t = t->next )
    {
      if ( t->timeout <= timeout )
	{
	  DSPD_ASSERT(t->callback != NULL);
	  ctx->cbtimer_dispatch_list[count] = t;
	  count++;
	} else
	{
	  break;
	}
    }
  for ( i = 0; i < count; i++ )
    {
      t = ctx->cbtimer_dispatch_list[i];
      if ( t->callback(ctx, t, t->arg, timeout) )
	{
	  t->timeout += t->period;
	  dspd_cbtimer_set(t, t->timeout, t->period);
	}
    }
}




struct accept_data {
  struct cbpoll_client_list *list;
  size_t index;
  size_t fd_index;
};

static void insert_cb(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *evt)
{
  struct cbpoll_client_hdr *hdr = (struct cbpoll_client_hdr*)(uintptr_t)evt->arg;
  size_t i = hdr->index;
  size_t fdi = hdr->fd_index;
  assert(hdr->list->clients[i] == (struct cbpoll_client_hdr*)UINTPTR_MAX);
  hdr->list->clients[i] = hdr;
  if ( ! hdr->list->ops->success(ctx, hdr) )
    hdr->list->clients[i] = NULL;
  cbpoll_unref(ctx, fdi);
}

static void fail_cb(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *evt)
{
  struct cbpoll_client_list *list = (struct cbpoll_client_list*)(uintptr_t)evt->arg;
  assert(evt->stream >= 0);
  assert(list->clients[evt->stream] == (struct cbpoll_client_hdr*)UINTPTR_MAX);
  list->clients[evt->stream] = NULL;
  list->ops->fail(ctx, list, evt->stream, evt->index, evt->fd);
  cbpoll_unref(ctx, evt->index);
}

static void accept_cb(struct cbpoll_ctx *ctx,
		      void *data,
		      struct cbpoll_work *wrk,
		      bool async)
{
  struct cbpoll_pipe_event evt;
  struct accept_data *d = (struct accept_data*)wrk->extra_data;
  struct cbpoll_client_hdr *ptr;
  struct cbpoll_client_hdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  memset(&evt, 0, sizeof(evt));
  hdr.fd = wrk->fd;
  hdr.index = d->index;
  hdr.fd_index = d->fd_index;
  hdr.list = d->list;
  hdr.ops = d->list->ops;
  dspd_ts_clear(&hdr.busy);
  ptr = d->list->ops->create(ctx, &hdr, wrk->arg);
  evt.fd = wrk->fd;
  evt.index = d->fd_index;
  evt.stream = d->index;
  evt.msg = CBPOLL_PIPE_MSG_CALLBACK;

  if ( ptr == NULL )
    {
      evt.callback = fail_cb;
      evt.arg = (uintptr_t)d->list;
    } else
    {
      evt.callback = insert_cb;
      evt.arg = (uintptr_t)ptr;
    }
  if ( ! async )
    {
      evt.callback(ctx, &evt);
    } else if ( cbpoll_send_event(ctx, &evt) < 0 )
    {
      if ( ptr )
	d->list->ops->destroy(ctx, ptr);
      dspd_mutex_lock(&ctx->loop_lock);
      evt.arg = (uintptr_t)d->list;
      fail_cb(ctx, &evt);
      dspd_mutex_unlock(&ctx->loop_lock);
    }
}

int32_t cbpoll_accept(struct cbpoll_ctx *ctx, 
		      struct cbpoll_client_list *list,
		      int32_t fd,
		      size_t index, 
		      int64_t arg)
{
  struct cbpoll_work wrk;
  size_t i;
  struct accept_data *d = (struct accept_data*)wrk.extra_data;
  int32_t ret = -EMFILE;
  memset(&wrk, 0, sizeof(wrk));
  for ( i = 0; i < list->max_clients; i++ )
    {
      if ( list->clients[i] == NULL )
	{
	  list->clients[i] = (void*)UINTPTR_MAX;
	  d->index = i;
	  break;
	}
    }
  if ( i < list->max_clients )
    {
      wrk.index = index;
      wrk.fd = fd;
      wrk.msg = CBPOLL_PIPE_MSG_DEFERRED_WORK;
      wrk.arg = arg;
      wrk.callback = accept_cb;
      d->list = list;
      d->fd_index = index;
      cbpoll_ref(ctx, index);
      cbpoll_queue_work(ctx, &wrk);
      ret = 0;
    }
  return ret;
}


static void destroy_cb(struct cbpoll_ctx *ctx,
		       void *data,
		       struct cbpoll_work *wrk,
		       bool async)
{
  struct cbpoll_client_hdr *hdr = (struct cbpoll_client_hdr*)(uintptr_t)wrk->arg;
  hdr->ops->destroy(ctx, hdr);
}

bool cbpoll_async_destructor_cb(void *data,
				struct cbpoll_ctx *context,
				int index,
				int fd)
{
  struct cbpoll_client_hdr *hdr = data;
  struct cbpoll_work wrk;
  struct cbpoll_fd *f = &context->fdata[index], *f2;
  if ( f->associated_context >= 0 )
    {
      f2 = &context->fdata[f->associated_context];
      DSPD_ASSERT(f2->refcnt > 0);
      if ( f2->data == data )
	return false;
    }
  memset(&wrk, 0, sizeof(wrk));
  wrk.index = index;
  wrk.fd = fd;
  wrk.msg = CBPOLL_PIPE_MSG_CALLBACK;
  wrk.arg = (uintptr_t)hdr;
  wrk.callback = destroy_cb;
  if ( hdr->list )
    hdr->list->clients[hdr->index] = NULL;
  cbpoll_queue_work(context, &wrk);
  return false; //Do not close fd
}

static void work_complete_cb(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *evt)
{
  struct cbpoll_client_hdr *hdr = (struct cbpoll_client_hdr*)(uintptr_t)evt->arg;
  hdr->ops->work_complete(ctx, hdr);
  cbpoll_unref(ctx, hdr->fd_index);
}

static void work_cb(struct cbpoll_ctx *ctx,
		    void *data,
		    struct cbpoll_work *wrk,
		    bool async)
{
  struct cbpoll_client_hdr *hdr = (struct cbpoll_client_hdr*)(uintptr_t)wrk->arg;
  struct cbpoll_pipe_event evt;
  memset(&evt, 0, sizeof(evt));
  dspd_ts_clear(&hdr->busy);
  dspd_mutex_lock(&ctx->work_lock);
  hdr->ops->do_work(ctx, hdr);
  dspd_mutex_unlock(&ctx->work_lock);
  evt.fd = wrk->fd;
  evt.index = wrk->index;
  evt.stream = -1;
  evt.msg = CBPOLL_PIPE_MSG_CALLBACK;
  evt.arg = (uintptr_t)wrk->arg;
  evt.callback = work_complete_cb;
  if ( async )
    {
      if ( cbpoll_send_event(ctx, &evt) < 0 )
	{
	  dspd_mutex_lock(&ctx->loop_lock);
	  evt.callback(ctx, &evt);
	  dspd_mutex_unlock(&ctx->loop_lock);
	}
    } else
    {
      evt.callback(ctx, &evt);
    }
}

int32_t cbpoll_queue_client_work(struct cbpoll_ctx *ctx, size_t index)
{
  struct cbpoll_fd *fd = &ctx->fdata[index];
  struct cbpoll_work wrk;
  struct cbpoll_client_hdr *hdr = fd->data;
  int32_t ret = -EAGAIN;
  memset(&wrk, 0, sizeof(wrk));
  if ( dspd_test_and_set(&hdr->busy) != DSPD_TS_SET )
    {
      wrk.index = index;
      wrk.fd = fd->fd;
      wrk.msg = CBPOLL_PIPE_MSG_CALLBACK;
      wrk.arg = (uintptr_t)hdr;
      wrk.callback = work_cb;
      cbpoll_ref(ctx, index);
      cbpoll_queue_work(ctx, &wrk);
      ret = 0;
    }
  return ret;
}

struct _dspd_pcmcli_timer {
  struct dspd_cbtimer timer;
};

static int pcmcli_timer_fire(dspd_pcmcli_timer_t *tmr, bool latch)
{
  dspd_cbtimer_fire(&tmr->timer);
  return 0;
}

static int pcmcli_timer_reset(dspd_pcmcli_timer_t *tmr)
{
  dspd_cbtimer_cancel(&tmr->timer);
  return 0;
}

static int pcmcli_timer_get(dspd_pcmcli_timer_t *tmr, dspd_time_t *abstime, uint32_t *per)
{
  *abstime = tmr->timer.timeout;
  *per = tmr->timer.period;
  return 0;
}

static int pcmcli_timer_set(dspd_pcmcli_timer_t *tmr, uint64_t abstime, uint32_t per)
{
  dspd_cbtimer_set(&tmr->timer, abstime, per);
  return 0;
}

static int pcmcli_timer_getpollfd(dspd_pcmcli_timer_t *tmr, struct pollfd *pfd)
{
  return -ENOSYS;
}

static void pcmcli_timer_destroy(dspd_pcmcli_timer_t *tmr)
{
  dspd_cbtimer_delete(&tmr->timer);
}

static struct dspd_pcmcli_timer_ops pcmcli_cbtimer_ops = {
  .fire = pcmcli_timer_fire,
  .reset = pcmcli_timer_reset,
  .get = pcmcli_timer_get,
  .set = pcmcli_timer_set,
  .getpollfd = pcmcli_timer_getpollfd,
  .destroy = pcmcli_timer_destroy
};

static bool pcmcli_timer_cb(struct cbpoll_ctx *ctx, 
			    struct dspd_cbtimer *timer,
			    void *arg, 
			    dspd_time_t timeout)
{
  (void) dspd_pcmcli_process_io(arg, POLLMSG, 0);
  return false;
}

int32_t cbpoll_set_pcmcli_timer_callbacks(struct cbpoll_ctx *cbpoll, 
					  struct dspd_pcmcli *pcm)
{
  struct dspd_cbtimer *cbt = dspd_cbtimer_new(cbpoll, pcmcli_timer_cb, pcm);
  if ( ! cbt )
    return -ENOMEM;
  dspd_pcmcli_set_timer_callbacks(pcm, &pcmcli_cbtimer_ops, cbt);
  return 0;
}

