/*
 *  SCHEDULER - An event scheduler
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

#include <stdint.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include "scheduler.h"




void dspd_sched_trigger(struct dspd_scheduler *sch)
{
  uint64_t val;
  int ret;
  //Multiple triggers can trigger one single wakeup
  if ( AO_test_and_set(&sch->eventfd_triggered) != AO_TS_SET )
    {
      val = 1;
      while ( (ret = write(sch->eventfd, &val, sizeof(val))) < (int)sizeof(val) )
	{
	  if  ( ret >= 0 )
	    {
	      assert(ret == sizeof(val));
	    }
	  if ( errno != EINTR )
	    {
	      assert(errno == EAGAIN);
	      break;
	    }
	}
    }
}


void dspd_scheduler_delete(struct dspd_scheduler *sch)
{
  close(sch->epfd);
  close(sch->timerfd);
  close(sch->eventfd);
  free(sch->evts);
  free(sch->fds);
  free(sch);
}

static void timer_callback(void *udata, int32_t fd, void *fdata, uint32_t events)
{
  struct dspd_scheduler *sch = fdata;
  sch->ops->timer_event(sch->udata);
}

static void event_callback(void *udata, int32_t fd, void *fdata, uint32_t events)
{
  struct dspd_scheduler *sch = fdata;
  uint64_t val;
  int ret;
  assert((events & (EPOLLERR|EPOLLRDHUP|EPOLLHUP)) == 0);
  //Clear trigger value before reading.
  //This makes the race condition settle out after 1 spurious wakeup.
  //In most cases it results in a single wakeup from a single trigger
  //and is likely to cause a single wakeup from multiple triggers (good).
  if ( events & EPOLLIN )
    {
      ret = read(fd, &val, sizeof(val));
      if ( ret < 0 )
	{
	  assert(errno == EAGAIN || errno == EINTR);
	} else if ( ret == sizeof(val) )
	{
	  AO_CLEAR(&sch->eventfd_triggered);
	}
      if ( sch->ops->trigger_event )
	sch->ops->trigger_event(sch->udata);
    }
}

struct dspd_scheduler *dspd_scheduler_new(const struct dspd_scheduler_ops *ops, void *udata, int32_t maxfds)
{
  struct dspd_scheduler *sch = calloc(1, sizeof(struct dspd_scheduler));
  int err;
  if ( ! sch )
    return NULL;

  sch->timerfd = -1;
  sch->epfd = -1;
  sch->eventfd = -1;
  sch->maxfds = maxfds+2;
  sch->fds = calloc(sch->maxfds, sizeof(*sch->fds));
  sch->ops = ops;
  if ( ! sch->fds )
    goto out;
  
  sch->evts = calloc(sch->maxfds, sizeof(*sch->evts));
  if ( ! sch->evts )
    goto out;
  
  sch->epfd = epoll_create(sch->maxfds);
  if ( sch->epfd < 0 )
    goto out;
    
  sch->eventfd = eventfd(0, 0);
  if ( sch->eventfd < 0 )
    goto out;
  fcntl(sch->eventfd, F_SETFL, fcntl(sch->eventfd, F_GETFL) | O_NONBLOCK);
  if ( dspd_scheduler_add_fd(sch, sch->eventfd, EPOLLIN, sch, event_callback) < 0 )
    goto out;


  sch->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if ( sch->timerfd < 0 )
    goto out;
  assert(sch->timerfd > 0 );
  if ( sch->ops->timer_event )
    {
      if ( dspd_scheduler_add_fd(sch, sch->timerfd, EPOLLIN, sch, timer_callback) < 0 )
	goto out;
    } else
    {
      if ( dspd_scheduler_add_fd(sch, sch->timerfd, EPOLLIN, NULL, NULL) < 0 )
	goto out;
    }
  sch->udata = udata;

  assert(sch->ops);
  assert(sch->ops->wake);
  assert(sch->ops->sleep);

  return sch;

 out:
  err = errno;
  close(sch->timerfd);
  close(sch->epfd);
  free(sch->evts);
  free(sch->fds);
  free(sch);
  errno = err;
  return NULL;
}

int dspd_scheduler_add_fd(struct dspd_scheduler *sch, 
			  int32_t fd, 
			  int32_t events, 
			  void *data,
			  dspd_sch_callback_t cb)
{
  struct dspd_scheduler_fd *f;
  struct epoll_event evt;
  int32_t ret;
  memset(&evt, 0, sizeof(evt));
  if ( sch->nfds < sch->maxfds )
    {
      f = &sch->fds[sch->nfds];
      f->ptr = data;
      f->fd = fd;
      f->callback = cb;
      evt.data.u32 = sch->nfds;
      evt.events = events;
      ret = epoll_ctl(sch->epfd, EPOLL_CTL_ADD, fd, &evt);
      if ( ret == 0 )
	sch->nfds++;
    } else
    {
      ret = -1;
      errno = EINVAL;
    }
  return ret;
}


static void process_fds(struct dspd_scheduler *sch, int32_t nevents)
{
  int i;
  struct epoll_event *evt;
  struct dspd_scheduler_fd *sfd;
  for ( i = 0; i < nevents; i++ )
    {
      evt = &sch->evts[i];
      assert(evt->data.u32 < sch->nfds);
      sfd = &sch->fds[evt->data.u32];
      if ( sfd->callback )
	sfd->callback(sch->udata, sfd->fd, sfd->ptr, evt->events);
    }
}

static int stop_timer(struct dspd_scheduler *sch)
{
  struct itimerspec its;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 0;
  its.it_value.tv_sec = 0;
  its.it_value.tv_nsec = 0;
  return timerfd_settime(sch->timerfd, TFD_TIMER_ABSTIME, &its, NULL);
}

static int program_timer(struct dspd_scheduler *sch, uint64_t abstime)
{
  /*
    NOTE:
    If the timerfd fires (caught by poll or not caught) then calling timerfd_settime() will
    reset the expiration count and cause another read() to block until the new expiration occurs.

    That means resetting it in any way requires a system call.  Might as well do a oneshot.

  */
  struct itimerspec its;
  its.it_value.tv_sec = abstime / 1000000000;
  its.it_value.tv_nsec = abstime % 1000000000;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 0;
  return timerfd_settime(sch->timerfd, TFD_TIMER_ABSTIME, &its, NULL);
}

void *dspd_scheduler_run(void *arg)
{
  struct dspd_scheduler *sch = arg;
  int ret = ECANCELED;
  uint64_t abstime;
  int32_t reltime;
  if ( sch->ops->loop_started )
    sch->ops->loop_started(sch->udata);
  while ( sch->abort == 0 )
    {
      if ( sch->ops->sleep(sch->udata, &abstime, &reltime) )
	{
	  if ( reltime == DSPD_SCHED_STOP )
	    {
	      ret = stop_timer(sch);
	      reltime = -1;
	    } else
	    {
	      ret = program_timer(sch, abstime);
	    }
	  if ( ret < 0 )
	    break;
	  while ( (ret = epoll_wait(sch->epfd, sch->evts, sch->nfds, reltime)) <= 0 )
	    {
	      if ( reltime >= 0 && ret == 0 )
		break;
	      if ( ret < 0 && errno != EINTR )
		break;
	    }
	} else
	{
	  ret = 0;
	}
      if ( ret < 0 )
	break;
      process_fds(sch, ret);
      assert(sch->ops);
      assert(sch->ops->wake);
      sch->ops->wake(sch->udata);
    }
  if ( sch->abort )
    ret = ECANCELED;
  else
    ret = errno;
  if ( sch->ops->abort )
    sch->ops->abort(sch->udata, ret);
  return NULL;
}

void dspd_scheduler_abort(struct dspd_scheduler *sch)
{
  sch->abort = 1;
}


