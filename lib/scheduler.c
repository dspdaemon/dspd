/*
 *  SCHEDULER - An event scheduler
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
#include <linux/unistd.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "sslib.h"
#include "daemon.h"
static bool dspd_sched_run_slave_once(struct dspd_scheduler *sch);

#define INVALID_SCHEDULER (struct dspd_scheduler*)UINTPTR_MAX
#define INVALID_FD -1
#define RESERVED_FD -2

static void update_dl_latency(struct dspd_scheduler *sch, bool idle);


static struct dspd_scheduler *get_slave(struct dspd_scheduler *sch, uintptr_t idx)
{
  volatile struct dspd_scheduler *ret;
  ret = sch->slaves[idx];
  if ( ret == (struct dspd_scheduler*)UINTPTR_MAX )
    ret = NULL;
  return (struct dspd_scheduler*)ret;
}


bool dspd_sched_return(struct dspd_scheduler *sch, 
		       dspd_sch_work_t callback,
		       void *arg,
		       uint64_t data)
{
  struct dspd_scheduler_work wrk;
  bool ret = false;
  memset(&wrk, 0, sizeof(wrk));
  wrk.callback = callback;
  wrk.arg = arg;
  wrk.data = data;
  if ( sch->retq )
    {
      if ( dspd_fifo_write(sch->retq, &wrk, 1) == 1 )
	{
	  ret = true;
	  dspd_sched_trigger(sch);	      
	}
    }
  return ret;
}

bool dspd_sched_queue_work(struct dspd_scheduler *sch, 
			   dspd_sch_work_t callback,
			   void *arg,
			   uint64_t data)
{
  struct dspd_scheduler_work wrk;
  bool ret = false;
  struct dspd_scheduler *s;
  if ( sch && sch->workq )
    {
      memset(&wrk, 0, sizeof(wrk));
      wrk.callback = callback;
      wrk.arg = arg;
      wrk.data = data;
      wrk.context = sch;
      if ( dspd_fifo_write(sch->workq, &wrk, 1) == 1 )
	{
	  ret = true;
	  if ( sch->flags & DSPD_SCHED_SLAVE )
	    s = sch->master;
	  else
	    s = sch;
	  if ( s )
	    {
	      if ( dspd_test_and_set(&s->workq_tsval) != DSPD_TS_SET )
		{
		  dspd_mutex_lock(&s->workq_lock);
		  dspd_cond_signal(&s->workq_event);
		  dspd_mutex_unlock(&s->workq_lock);
		}
	    }
	}
    }
  return ret;
}

void dspd_sched_trigger(struct dspd_scheduler *sch)
{
  uint64_t val;
  int ret;
  if ( ! sch )
    return;
  if ( sch->flags & DSPD_SCHED_SLAVE )
    {
      if ( sch->master )
	{
	  if ( dspd_test_and_set(&sch->eventfd_triggered) != DSPD_TS_SET )
	    dspd_sched_trigger(sch->master);
	}
    } else
    {
      //Multiple triggers can trigger one single wakeup
      if ( dspd_test_and_set(&sch->eventfd_triggered) != DSPD_TS_SET )
	{
	  val = 1;
	  while ( (ret = write(sch->eventfd, &val, sizeof(val))) < (int)sizeof(val) )
	    {
	      if  ( ret >= 0 )
		{
		  DSPD_ASSERT(ret == sizeof(val));
		}
	      if ( errno != EINTR )
		{
		  DSPD_ASSERT(errno == EAGAIN);
		  break;
		}
	    }
	}
    }
}

static void add_slave_cb(struct dspd_scheduler *master, void *arg, uint64_t data)
{
  struct dspd_scheduler *slave = arg;
  if ( dspd_sched_add_slave(master, slave) < 0 )
    {
      if ( ! slave->abort )
	{
	  dspd_sched_abort(slave);
	  dspd_sched_run_slave_once(slave);
	}
    }
}

int32_t dspd_sched_send_slave(struct dspd_scheduler *master, struct dspd_scheduler *slave)
{
  int32_t ret = -EINVAL;
  struct dspd_scheduler_work wrk;
  if ( master->flags & DSPD_SCHED_MASTER )
    {
      memset(&wrk, 0, sizeof(wrk));
      wrk.callback = add_slave_cb;
      wrk.arg = slave;
      wrk.context = master;
      dspd_mutex_lock(&master->control_lock);
      if ( dspd_fifo_write(master->controlq, &wrk, 1) == 1 )
	{
	  dspd_sched_trigger(master);
	  ret = 0;
	} else
	{
	  ret = -EBUSY;
	}
      dspd_mutex_unlock(&master->control_lock);
    }
  return ret;
}

static void thread_exit_cb(struct dspd_scheduler *sch, void *arg, uint64_t data)
{
  pthread_exit(NULL);
}

void dspd_sched_stop_workq(struct dspd_scheduler *sch)
{
  if ( sch->workq_thread.init )
    {
      while ( ! dspd_sched_queue_work(sch, thread_exit_cb, NULL, 0ULL) )
	usleep(1000);
      dspd_thread_join(&sch->workq_thread, NULL);
      sch->workq_thread.init = false;
    }
}

void dspd_sched_delete(struct dspd_scheduler *sch)
{
  if ( sch )
    {
      dspd_sched_stop_workq(sch);
      if ( ! (sch->flags & DSPD_SCHED_SLAVE) )
	{
	  close(sch->epfd);
	  close(sch->timerfd);
	  close(sch->eventfd);
	}
      free(sch->thread_name);
      free(sch->slaves);
      free(sch->evts);
      free(sch->fds);
      dspd_dtimer_delete(sch->dtimer);
      dspd_dtimer_delete(sch->slave_dispatch);
      dspd_fifo_delete(sch->workq);
      dspd_fifo_delete(sch->retq);
      dspd_fifo_delete(sch->controlq);
      dspd_cond_destroy(&sch->workq_event);
      dspd_mutex_destroy(&sch->workq_lock);
      dspd_mutex_destroy(&sch->control_lock);
      free(sch);
    }
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
  struct dspd_scheduler_work wrk;
  DSPD_ASSERT((events & (EPOLLERR|EPOLLRDHUP|EPOLLHUP)) == 0);
  //Clear trigger value before reading.
  //This makes the race condition settle out after 1 spurious wakeup.
  //In most cases it results in a single wakeup from a single trigger
  //and is likely to cause a single wakeup from multiple triggers (good).
  if ( events & EPOLLIN )
    {
      if ( ! (sch->flags & DSPD_SCHED_SLAVE) )
	{
	  ret = read(fd, &val, sizeof(val));
	  if ( ret < 0 )
	    {
	      DSPD_ASSERT(errno == EAGAIN || errno == EINTR);
	    } else if ( ret == sizeof(val) )
	    {
	      AO_CLEAR(&sch->eventfd_triggered);
	    }
	}
      if ( sch->retq )
	{
	  while ( dspd_fifo_read(sch->retq, &wrk, 1) )
	    {
	      if ( wrk.callback )
		wrk.callback(wrk.context, wrk.arg, wrk.data);
	    }
	}
      if ( sch->controlq )
	{
	  while ( dspd_fifo_read(sch->controlq, &wrk, 1) )
	    {
	      if ( wrk.callback )
		wrk.callback(wrk.context, wrk.arg, wrk.data);
	    }
	}
      if ( sch->ops->trigger_event )
	sch->ops->trigger_event(sch->udata);
    }
}

static void activate_slave(struct dspd_scheduler *sch, int32_t flag)
{
  int32_t oldf = sch->activate_flags;
  sch->activate_flags |= flag;
  if ( oldf == 0 )
    dspd_dtimer_insert(sch->master->slave_dispatch, &sch->slave_event);
}

static void slave_timer_cb(struct dspd_dtimer *timer, struct dspd_dtimer_event *event)
{
  struct dspd_scheduler *sch = event->user_data;
  struct epoll_event *evt;
  activate_slave(sch, DSPD_SCHED_TIMER);
  evt = &sch->evts[sch->injected_events];
  memset(evt, 0, sizeof(*evt));
  evt->data.u32 = sch->timerfd_index;
  evt->events = POLLIN;
  sch->injected_events++;
}

static void master_timer_event(void *data)
{
  struct dspd_scheduler *sch = data;
  if ( dspd_dtimer_set_time(sch->dtimer, sch->dtimer->timeout) )
    dspd_dtimer_dispatch(sch->dtimer);
}

static void master_trigger_event(void *data)
{
  struct dspd_scheduler *sch = data, *s;
  size_t i;
  struct epoll_event *evt;
  for ( i = 0; i < sch->nslaves; i++ )
    {
      s = (struct dspd_scheduler*)sch->slaves[i];
      if ( s )
	{
	  if ( dspd_ts_load(&s->eventfd_triggered) == DSPD_TS_SET )
	    {
	      dspd_ts_clear(&s->eventfd_triggered);
	      activate_slave(s, DSPD_SCHED_EVENTFD);
	      evt = &s->evts[s->injected_events];
	      memset(evt, 0, sizeof(*evt));
	      evt->data.u32 = s->eventfd_index;
	      evt->events = POLLIN;
	      s->injected_events++;
	    }
	}
    }
}

static void slave_removed_cb(struct dspd_scheduler *sch, void *arg, uint64_t data)
{
  struct dspd_scheduler *slave = arg;
  slave->master = NULL;
  if ( slave->ops->destructor )
    slave->ops->destructor(slave, slave->udata);
}

static void slave_dispatch_cb(struct dspd_dtimer *timer, struct dspd_dtimer_event *event)
{
  struct dspd_scheduler *sch = event->user_data, *master;
  bool success;
  ssize_t i;
  master = sch->master;
  AO_store(&master->dispatch_slave, sch->slave_index);
  success = dspd_sched_run_slave_once(sch);
  AO_store(&master->dispatch_slave, DSPD_SCHED_INVALID_SLAVE);
  if ( ! success )
    {
      dspd_dtimer_remove(&sch->timer_event);
      dspd_dtimer_remove(&sch->slave_event);
      sch->master->slaves[sch->slave_index] = NULL;
      if ( sch->slave_index == (sch->master->nslaves-1UL) )
	{
	  i = sch->master->nslaves;
	  while ( ! sch->master->slaves[i] )
	    {
	      i--;
	      if ( i == -1 )
		break;
	    }
	  sch->master->nslaves = i + 1L;
	}
      while ( ! dspd_sched_queue_work(master, slave_removed_cb, sch, 0ULL) )
	usleep(100);
    }
}

static void master_wake(void *data)
{
  struct dspd_scheduler *sch = data;
  if ( dspd_dtimer_set_time(sch->slave_dispatch, UINT64_MAX) )
    dspd_dtimer_dispatch(sch->slave_dispatch);
}

static void master_abort(void *data, int error)
{
  struct dspd_scheduler *sch = data, *s;
  size_t i;
   for ( i = 0; i < sch->nslaves; i++ )
    {
      s = get_slave(sch, i);
      if ( s )
	{
	  if ( s->ops->abort )
	    s->ops->abort(s->udata, error);
	  sch->slaves[i] = NULL;
	}
    }
   sch->nslaves = 0;
}

static bool master_sleep(void *user_data, uint64_t *abstime, uint64_t *deadline, int32_t *reltime)
{
  struct dspd_scheduler *sch = user_data;
  *deadline = 0;
  *abstime = sch->dtimer->timeout;
  if ( sch->slave_dispatch->pending )
    *reltime = DSPD_SCHED_SPIN; //slaves need to run immediately
  else if ( sch->dtimer->pending )
    *reltime = DSPD_SCHED_WAIT; //timeouts are pending
  else
    *reltime = DSPD_SCHED_STOP; //no work to do
  return true;
}

#define SEGFAULT() while(1){ raise(SIGSEGV); abort(); }

static void master_sigbus(void *user_data)
{
  struct dspd_scheduler *sch = user_data, *s;
  intptr_t idx;
  idx = AO_load(&sch->dispatch_slave);
  if ( idx < 0 ) //No slave.  Something is horribly wrong.
    SEGFAULT();
  s = get_slave(sch, idx);
  if ( s == NULL ) //Slave somehow went away (memory corruption?)
    SEGFAULT();
  if ( ! s->ops->sigbus ) //Did not define handler, so there is nothing to do.
    SEGFAULT();
  //One of the statements above will segfault if the handler
  //generates another bus error.
  AO_store(&sch->dispatch_slave, DSPD_SCHED_INVALID_SLAVE);
  s->ops->sigbus(s->udata);
}

static struct dspd_scheduler_ops master_ops = {
  .started = NULL,
  .timer_event = master_timer_event,
  .trigger_event = master_trigger_event,
  .wake = master_wake,
  .sleep = master_sleep,
  .abort = master_abort,
  .sigbus = master_sigbus,
};


static bool workq_poll(struct dspd_scheduler *sch, struct dspd_scheduler_work *wrk, uintptr_t *counter)
{
  bool ret = false;
  uintptr_t c, i, n, idx;
  struct dspd_scheduler *s;
  if ( sch->flags & DSPD_SCHED_MASTER )
    {
      c = *counter;
      n = sch->max_slaves + 1UL;
      for ( i = 0; i < n; i++ )
	{
	  idx = (i + c) % n;
	  if ( idx == sch->max_slaves )
	    {
	      if ( dspd_fifo_read(sch->workq, wrk, 1) == 1 )
		{
		  ret = true;
		  break;
		}
	    } else
	    {
	      s = get_slave(sch, idx);
	      if ( s )
		{
		  if ( dspd_fifo_read(s->workq, wrk, 1) == 1 )
		    {
		      ret = true;
		      i++;
		      break;
		    }
		}
	    }
	}
      *counter = c + i;
    } else
    {
      ret = dspd_fifo_read(sch->workq, wrk, 1) == 1;
    }
  return ret;
}

static void *workq_thread(void *arg)
{
  struct dspd_scheduler *sch = arg;
  struct dspd_scheduler_work wrk;
  uintptr_t counter = 0;
  char buf[32];
  if ( sch->thread_name )
    {
      if ( (size_t)snprintf(buf, sizeof(buf), "%s-wq", sch->thread_name) < sizeof(buf) )
	set_thread_name(buf);
    }

  while ( 1 )
    {
      wrk.callback = NULL;
      if ( ! workq_poll(sch, &wrk, &counter) )
	{
	  if ( dspd_ts_load(&sch->workq_tsval) != DSPD_TS_SET )
	    {
	      dspd_mutex_lock(&sch->workq_lock);
	      while ( workq_poll(sch, &wrk, &counter) == false )
		dspd_cond_wait(&sch->workq_event, &sch->workq_lock);
	      dspd_mutex_unlock(&sch->workq_lock);
	      dspd_ts_clear(&sch->workq_tsval);
	    } else
	    {
	      dspd_ts_clear(&sch->workq_tsval);
	      if ( workq_poll(sch, &wrk, &counter) == false )
		{
		  usleep(1);
		  continue;
		}
	    }
	}
      if ( wrk.callback )
	wrk.callback(wrk.context, wrk.arg, wrk.data);
    }
  return NULL;
}

struct dspd_scheduler *dspd_sched_new(const struct dspd_scheduler_ops *ops, 
				      void *udata, 
				      const struct dspd_sched_params *params)
{
  struct dspd_scheduler *sch = calloc(1, sizeof(struct dspd_scheduler));
  int err = 0;
  size_t i = 0;
  if ( ! sch )
    return NULL;
  sch->timebase = 1; //1ns
  sch->timerfd = -1;
  sch->epfd = -1;
  sch->eventfd = -1;
  sch->maxfds = 2 + params->nfds;
  sch->flags = params->flags;
  sch->latency = UINT64_MAX;
  dspd_ts_clear(&sch->workq_tsval);
  if ( params->thread_name )
    {
      sch->thread_name = strdup(params->thread_name);
      if ( ! sch->thread_name )
	goto out;
    }
  if ( sch->flags & DSPD_SCHED_MASTER )
    {
      sch->udata = sch;
      if ( ops )
	{
	  errno = EINVAL;
	  goto out;
	}
      sch->ops = &master_ops;
      sch->maxfds += params->nslaves;
      sch->max_slaves = params->nslaves;

      err = dspd_dtimer_new(&sch->dtimer, dspd_get_time());
      if ( err < 0 )
	{
	  errno = -err;
	  goto out;
	}
      err = dspd_dtimer_new(&sch->slave_dispatch, UINT64_MAX);
      if ( err < 0 )
	{
	  errno = -err;
	  goto out;
	}

    } else
    {
      sch->udata = udata;
      sch->ops = ops;
    }

  if ( sch->max_slaves > 0 )
    {
      sch->slaves = calloc(sch->max_slaves, sizeof(*sch->slaves));
      if ( ! sch->slaves )
	goto out;
    }

  sch->fds = calloc(sch->maxfds, sizeof(*sch->fds));
  if ( ! sch->fds )
    goto out;
  
  sch->evts = calloc(sch->maxfds, sizeof(*sch->evts));
  if ( ! sch->evts )
    goto out;
  
  for ( i = 0; i < sch->maxfds; i++ )
    sch->fds[i].fd = INVALID_FD;



  if ( sch->flags & DSPD_SCHED_WORKQ )
    {
      uint32_t n;
      n = params->nmsgs + 2U; //add some space for exit command and at least one regular command


      err = dspd_fifo_new(&sch->workq, n, sizeof(struct dspd_scheduler_work), NULL);
      if ( err < 0 )
	{
	  errno = -err;
	  goto out;
	}
      err = dspd_fifo_new(&sch->retq, n, sizeof(struct dspd_scheduler_work), NULL);
      if ( err < 0 )
	{
	  errno = -err;
	  goto out;
	}

      if ( ! (sch->flags & DSPD_SCHED_SLAVE) )
	{
	  err = dspd_cond_init(&sch->workq_event, NULL);
	  if ( err )
	    {
	      errno = err;
	      goto out;
	    }
	  err = dspd_mutex_init(&sch->workq_lock, NULL);
	  if ( err )
	    {
	      errno = err;
	      goto out;
	    }
	      
	  dspd_threadattr_t attr = { .init = 0 };
	  err = dspd_daemon_threadattr_init(&attr, sizeof(attr), 0);
	  if ( err != 0 )
	    {
	      errno = err;
	      goto out;
	    }
	  err = dspd_thread_create(&sch->workq_thread, &attr, workq_thread, sch);
	  dspd_threadattr_destroy(&attr);
	  if ( err != 0 )
	    {
	      errno = err;
	      goto out;
	    }
	}
    }


  if ( ! (sch->flags & DSPD_SCHED_SLAVE) )
    {
      sch->epfd = epoll_create(sch->maxfds);
      if ( sch->epfd < 0 )
	goto out;
      sch->eventfd = eventfd(0, 0);
      if ( sch->eventfd < 0 )
	goto out;
      fcntl(sch->eventfd, F_SETFL, fcntl(sch->eventfd, F_GETFL) | O_NONBLOCK);
      if ( dspd_sched_add_fd(sch, sch->eventfd, EPOLLIN, sch, event_callback) < 0 )
	goto out;
      sch->eventfd_index = 0;

      sch->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
      if ( sch->timerfd < 0 )
	goto out;
      DSPD_ASSERT(sch->timerfd > 0 );
      if ( sch->ops->timer_event )
	{
	  if ( dspd_sched_add_fd(sch, sch->timerfd, EPOLLIN, sch, timer_callback) < 0 )
	    goto out;
	} else
	{
	  if ( dspd_sched_add_fd(sch, sch->timerfd, EPOLLIN, NULL, NULL) < 0 )
	    goto out;
	}
      sch->timerfd_index = 1;

      if ( sch->flags & DSPD_SCHED_MASTER )
	{
	  uint32_t n;
	  n = (params->nmsgs + 2U + sch->maxfds) * 2;
	  err = dspd_fifo_new(&sch->controlq, n, sizeof(struct dspd_scheduler_work), NULL);
	  if ( err < 0 )
	    {
	      errno = -err;
	      goto out;
	    }
	  err = dspd_mutex_init(&sch->control_lock, NULL);
	  if ( err )
	    {
	      errno = err;
	      goto out;
	    }
	}

     

    } else
    {
      
      struct dspd_scheduler_fd *f;
      f = &sch->fds[sch->nfds];
      f->fd = RESERVED_FD;
      f->ptr = sch;
      if ( sch->ops->timer_event )
	f->callback = timer_callback;
      sch->timerfd_index = sch->nfds;
      sch->nfds++;

      f = &sch->fds[sch->nfds];
      f->fd = RESERVED_FD;
      f->ptr = sch;
      f->callback = event_callback;
      sch->eventfd_index = sch->nfds;
      sch->nfds++;

      sch->timer_event.callback = slave_timer_cb;
      sch->timer_event.user_data = sch;

      sch->slave_event.callback = slave_dispatch_cb;
      sch->slave_event.user_data = sch;
      

    }
  

  DSPD_ASSERT(sch->ops);
  if ( sch->ops )
    {
      DSPD_ASSERT(sch->ops->wake);
      DSPD_ASSERT(sch->ops->sleep);
    }

  
  return sch;

 out:
  err = errno;
  dspd_sched_delete(sch);
  errno = err;
  return NULL;
}

int dspd_sched_set_fd_event(struct dspd_scheduler *sch, 
			    int32_t fd,
			    int32_t events)
{
  struct epoll_event evt;
  int32_t ret;
  size_t i;
  for ( i = 0; i < sch->maxfds; i++ )
    {
      if ( sch->fds[i].fd == fd )
	break;
   }
  if ( i == sch->maxfds )
    {
      errno = EINVAL;
      ret = -1;
    } else
    {
      memset(&evt, 0, sizeof(evt));
      evt.events = events;
      evt.data.u32 = i;
      if ( sch->flags & DSPD_SCHED_SLAVE )
	{
	  evt.data.u32 |= (sch->slave_index << 16U) & 0xFFFF0000U;
	  if ( sch->master )
	    {
	      ret = epoll_ctl(sch->master->epfd, EPOLL_CTL_MOD, fd, &evt);
	    } else
	    {
	      errno = EINVAL;
	      ret = -1;
	    }
	} else
	{
	  ret = epoll_ctl(sch->epfd, EPOLL_CTL_MOD, fd, &evt);
	}
    }
  return ret;
}

static ssize_t find_free_fd(struct dspd_scheduler *sch)
{
  size_t i;
  ssize_t ret = -1L;
  for ( i = 0; i < sch->maxfds; i++ )
    {
      if ( i != sch->timerfd_index && i != sch->slave_index && sch->fds[i].fd == INVALID_FD )
	{
	  ret = i;
	  break;
	}
    }
  return ret;
}

int dspd_sched_add_fd(struct dspd_scheduler *sch, 
		      int32_t fd, 
		      int32_t events, 
		      void *data,
		      dspd_sch_callback_t cb)
{
  struct dspd_scheduler_fd *f;
  struct epoll_event evt;
  ssize_t index;
  int32_t ret;
  memset(&evt, 0, sizeof(evt));
  index = find_free_fd(sch);
  if ( index >= 0 )
    {
      f = &sch->fds[index];
      f->ptr = data;
      f->fd = fd;
      f->callback = cb;
      evt.data.u32 = index;
      evt.events = events;
      if ( sch->flags & DSPD_SCHED_SLAVE )
	{
	  evt.data.u32 |= (sch->slave_index << 16U) & 0xFFFF0000U;
	  if ( sch->master )
	    ret = epoll_ctl(sch->master->epfd, EPOLL_CTL_ADD, fd, &evt);
	  else
	    ret = 0;
	} else
	{
	  ret = epoll_ctl(sch->epfd, EPOLL_CTL_ADD, fd, &evt);
	}
      if ( ret == 0 && index >= sch->nfds )
	sch->nfds = index + 1L;
    } else
    {
      ret = -ENOSPC;
      errno = ENOSPC;
    }
  return ret;
}

void dspd_sched_remove_fd(struct dspd_scheduler *sch, int32_t fd)
{
  size_t i;
  ssize_t idx = -1L;
  struct dspd_scheduler_fd *f;
  struct epoll_event evt;
  for ( i = 0; i < sch->nfds; i++ )
    {
      f = &sch->fds[i];
      if ( f->fd == fd )
	{
	  memset(f, 0, sizeof(*f));
	  f->fd = INVALID_FD;
	  memset(&evt, 0, sizeof(evt));
	  if ( sch->master )
	    epoll_ctl(sch->master->epfd, EPOLL_CTL_DEL, fd, &evt);
	  else
	    epoll_ctl(sch->epfd, EPOLL_CTL_DEL, fd, &evt);
	} else if ( f->fd >= 0 )
	{
	  idx = i;
	}
    }
  sch->nfds = idx + 1L;
}

static void process_fds(struct dspd_scheduler *sch, int32_t nevents)
{
  int i;
  struct epoll_event *evt, *e;
  struct dspd_scheduler_fd *sfd;
  struct dspd_scheduler *s;
  uint32_t sidx, fidx;
  for ( i = 0; i < nevents; i++ )
    {
      evt = &sch->evts[i];
      fidx = evt->data.u32 & 0xFFFFU;
      sidx = (evt->data.u32 & 0xFFFF0000U) >> 16U;
      DSPD_ASSERT(fidx < sch->nfds);
      if ( sidx )
	{
	  sidx--;
	  s = (struct dspd_scheduler*)sch->slaves[sidx];
	  if ( s != NULL && sidx < s->maxfds )
	    {
	      sfd = &s->fds[fidx];
	      if ( sfd->fd >= 0 )
		{
		  e = &s->evts[s->injected_events];
		  s->injected_events++;
		  memset(e, 0, sizeof(*e));
		  e->data.u32 = fidx;
		  e->events = evt->events;
		  activate_slave(s, DSPD_SCHED_FDEVENT);
		}
	    }
	} else
	{
	  sfd = &sch->fds[fidx];
	  if ( sfd->callback )
	    sfd->callback(sch->udata, sfd->fd, sfd->ptr, evt->events);
	}
    }
}

static int stop_timer(struct dspd_scheduler *sch)
{
  struct itimerspec its;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 0;
  its.it_value.tv_sec = 0;
  its.it_value.tv_nsec = 0;
  if ( sch->sigxcpu_handled )
    {
      pthread_setschedparam(pthread_self(), sch->sched_policy, &sch->sched_param);
      sch->sigxcpu_handled = false;
      usleep(1); //just in case the indefinite wait doesn't work
    }
  return timerfd_settime(sch->timerfd, TFD_TIMER_ABSTIME, &its, NULL);
}

static int program_timer(struct dspd_scheduler *sch, uint64_t abstime)
{
  /*
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




static bool dspd_sched_run_slave_once(struct dspd_scheduler *sch)
{
  int32_t ret = 0;
  dspd_time_t abstime, deadline;
  if ( sch->dead )
    return true;
  if ( sch->activate_flags & DSPD_SCHED_LOOPSTART )
    {
      sch->activate_flags &= ~DSPD_SCHED_LOOPSTART;
      if ( sch->ops->started )
	sch->ops->started(sch->udata);
      goto sleep_now;
    }
  if ( ! AO_load(&sch->abort) )
    {
      sch->activate_flags = 0;
      process_fds(sch, sch->injected_events);
      sch->injected_events = 0;
      DSPD_ASSERT(sch->ops);
      DSPD_ASSERT(sch->ops->wake);
      sch->ops->wake(sch->udata);

    sleep_now:
      if ( ! sch->ops->sleep(sch->udata, &abstime, &deadline, &sch->reltime) )
	sch->reltime = DSPD_SCHED_SPIN;

      //keep priority for dispatch
      sch->slave_event.deadline = deadline;
      sch->slave_event.timeout = abstime;

      if ( sch->reltime == DSPD_SCHED_STOP )
	{
	  dspd_dtimer_remove(&sch->timer_event);
	} else
	{
	  sch->timer_event.deadline = deadline;
	  sch->timer_event.timeout = abstime;
	  dspd_dtimer_insert(sch->master->dtimer, &sch->timer_event);
	  if ( sch->reltime == DSPD_SCHED_SPIN )
	    activate_slave(sch, DSPD_SCHED_RETRY);
	}
    } else
    {
      ret = -1;
    }
  if ( ret < 0 )
    {
      AO_store(&sch->abort, 1);
      if ( sch->ops->abort )
	sch->ops->abort(sch->udata, ECANCELED);
      sch->dead = true;
    }
  return AO_load(&sch->abort) == 0;
}

static void sigbus_handler(int sig, siginfo_t *signinfo, void *context)
{
  struct dspd_scheduler *sch = dspdtls_get();
  if ( ! sch )
    raise(SIGSEGV);
  else
    siglongjmp(sch->sigbus_env, 1);
}

static void sigxcpu_handler(int sig, siginfo_t *signinfo, void *context)
{
  //Drop realtime priority
  struct dspd_scheduler *sch = dspdtls_get();
  struct sched_param param = { 0 };
  if ( sch )
    {
      sch->sigxcpu_handled = true;
      sched_setscheduler(sch->tid, SCHED_OTHER, &param);
    } else
    {
      //The signal might arrive at wrong thread or maybe while stopping the thread.
      sched_setscheduler(dspd_gettid(), SCHED_OTHER, &param);
    }
  return;
}

void *dspd_sched_run(void *arg)
{
  struct dspd_scheduler *sch = arg;
  int ret = ECANCELED, err;
  uint64_t abstime, deadline;
  int32_t reltime;
  struct sigaction act;
  dspd_sched_deadline_init(sch);

  sch->tid = dspd_gettid();
 
  if ( sch->thread_name )
    set_thread_name(sch->thread_name);
    

  err = dspdtls_set(sch);
  if ( err )
    {
      errno = -err;
      AO_store(&sch->abort, 1);
    }
  if ( sch->flags & DSPD_SCHED_SCHEDDL )
    update_dl_latency(sch, true);
  

  if ( pthread_getschedparam(pthread_self(), &sch->sched_policy, &sch->sched_param) != 0 )
    {
      sch->sched_policy = SCHED_OTHER;
      sch->sched_param.sched_priority = 0;
    }
  
  if ( sch->flags & DSPD_SCHED_SIGBUS )
    {
      memset(&act, 0, sizeof(act));
      act.sa_sigaction = sigbus_handler;
      act.sa_flags = SA_SIGINFO;
      sigaction(SIGBUS, &act, NULL);
      if ( sigsetjmp(sch->sigbus_env, SIGBUS) == 1 )
	{
	  if ( sch->ops->sigbus == NULL )
	    SEGFAULT();
	  sch->ops->sigbus(sch->udata);
	}
    }
  if ( (sch->flags & (DSPD_SCHED_SIGXCPU|DSPD_SCHED_SLAVE)) == DSPD_SCHED_SIGXCPU )
    {
      memset(&act, 0, sizeof(act));
      struct rlimit rl, old;
      act.sa_sigaction = sigxcpu_handler;
      act.sa_flags = SA_SIGINFO;
      sigaction(SIGXCPU, &act, NULL);

      rl.rlim_cur = 50000;
      rl.rlim_max = 100000;
      prlimit(sch->tid, RLIMIT_RTTIME, &rl, &old);
    }
  
  if ( sch->ops->started != NULL && (sch->activate_flags & DSPD_SCHED_LOOPSTART) )
    {
      sch->flags &= ~DSPD_SCHED_LOOPSTART;
      sch->ops->started(sch->udata);
    }

  while ( AO_load(&sch->abort) == 0 )
    {
      if ( sch->ops->sleep(sch->udata, &abstime, &deadline, &reltime) )
	{
	  if ( sch->sched_policy == SCHED_DEADLINE && sch->dl_latency != sch->latency )
	    update_dl_latency(sch, reltime == DSPD_SCHED_STOP);

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
  if ( AO_load(&sch->abort) )
    ret = ECANCELED;
  else
    ret = errno;
  if ( sch->ops->abort )
    sch->ops->abort(sch->udata, ret);

  dspdtls_clear();

  sch->dead = true;
  if ( sch->ops->destructor )
    sch->ops->destructor(sch, sch->udata);

  return NULL;
}

void dspd_sched_abort(struct dspd_scheduler *sch)
{
  if ( sch )
    AO_store(&sch->abort, 1);
}

int32_t dspd_sched_add_slave(struct dspd_scheduler *master, struct dspd_scheduler *slave)
{
  size_t i;
  int32_t ret = ENOSPC;
  struct dspd_scheduler_fd *fd;
  struct epoll_event evt;
  for ( i = 0; i < master->max_slaves; i++ )
    {
      if ( master->slaves[i] == NULL )
	{
	  master->slaves[i] = slave;
	  slave->master = master;
	  if ( i <= master->nslaves )
	    master->nslaves = i + 1UL;
	  slave->fds[slave->eventfd_index].fd = master->eventfd;
	  slave->fds[slave->timerfd_index].fd = master->timerfd;
	  if ( (size_t)slave->timerfd_index >= slave->nfds )
	    slave->nfds = slave->timerfd_index + 1L;
	  if ( (size_t)slave->eventfd_index >= slave->nfds )
	    slave->nfds = slave->eventfd_index + 1L;
	  slave->slave_index = i;
	  slave->sched_param = master->sched_param;
	  slave->sched_policy = master->sched_policy;
	  activate_slave(slave, DSPD_SCHED_LOOPSTART);
	  for ( i = 0; i < slave->nfds; i++ )
	    {
	      if ( i != master->timerfd_index && i != master->eventfd_index )
		{
		  fd = &slave->fds[i];
		  if ( fd->fd >= 0 )
		    {
		      memset(&evt, 0, sizeof(evt));
		      evt.data.u32 = slave->slave_index;
		      evt.data.u32 <<= 16U;
		      evt.data.u32 |= i & 0xFFFF;
		      if ( epoll_ctl(master->epfd, EPOLL_CTL_ADD, fd->fd, &evt) < 0 )
			{
			  ret = errno;
			  dspd_sched_abort(slave);
			  dspd_sched_run_slave_once(slave);
			  goto out;
			}
		    }
		}
	    }
	  ret = 0;
	  break;
	}
    }

 out:
  errno = ret;
  return -ret;
}

void dspd_sched_remove_slave(struct dspd_scheduler *slave)
{
  size_t i;
  ssize_t idx;
  struct dspd_scheduler *s;
  if ( slave->master )
    {
      for ( i = 0; i < slave->nfds; i++ )
	{
	  if ( i != slave->eventfd_index && 
	       i != slave->timerfd_index && 
	       slave->fds[i].fd >= 0 )
	    dspd_sched_remove_fd(slave, slave->fds[i].fd);
	}
      //It is ok if the work queue thread follows this pointer as long
      //as the slave gets released from the work queue thread.
      slave->master->slaves[slave->slave_index] = NULL;
      idx = -1L;
      for ( i = 0; i < slave->master->max_slaves; i++ )
	{
	  s = get_slave(slave->master, i);
	  if ( s != NULL )
	    idx = i;
	}
      slave->nslaves = idx + 1L;
    }
}



void dspd_sched_get_deadline_hint(struct dspd_scheduler *sch,
				  int32_t *avail_min,
				  int32_t *buffer_time)
{
  *avail_min = sch->avail_min;
  *buffer_time = sch->buffer_time;
}

struct dspd_scheduler *dspd_sched_get(void)
{
  struct dspd_scheduler *sched = dspdtls_get();
  struct dspd_scheduler *ret = NULL;
  if ( sched->flags & DSPD_SCHED_MASTER )
    {
      if ( sched->dispatch_slave >= 0 )
	{
	  ret = get_slave(sched, sched->dispatch_slave);
	} else
	{
	  ret = sched;
	}
    } else
    {
      ret = sched;
    }
  return ret;
}

static void set_slave_latency(struct dspd_scheduler *sched)
{
  struct dspd_scheduler *s;
  size_t i;
  for ( i = 0; i < sched->nslaves; i++ )
    {
      s = (struct dspd_scheduler*)sched->slaves[i];
      if ( s && s->ops->setlatency )
	s->ops->setlatency(s->udata, sched->latency);
    }
}

void dspd_sched_set_latency(struct dspd_scheduler *sched, dspd_time_t latency)
{
  struct dspd_scheduler *s;
  size_t i;
  dspd_time_t l;
  
  if ( sched->master )
    {
      sched->latency = latency;
      if ( sched->master->latency > latency )
	{
	  sched->master->latency = latency;
	  set_slave_latency(sched->master);
	} else if ( sched->master->latency < latency )
	{
	  l = UINT64_MAX;
	  for ( i = 0; i < sched->master->nslaves; i++ )
	    {
	      s = (struct dspd_scheduler*)sched->master->slaves[i];
	      if ( s )
		{
		  if ( s->latency < l )
		    l = s->latency;
		}
	    }
	  if ( sched->master->latency != l )
	    {
	      sched->master->latency = l;
	      set_slave_latency(sched->master);
	    }
	}
    } else if ( sched->latency != latency )
    {
      sched->latency = latency;
      if ( sched->ops->setlatency )
	sched->ops->setlatency(sched->udata, sched->latency);
    }
}





#ifdef _USE_DSPD_SCHED_DEADLINE


/* XXX use the proper syscall numbers */
#ifdef __x86_64__
#if ! (defined(__NR_sched_setattr) && defined(__NR_sched_getattr))
#define __NR_sched_setattr		314
#define __NR_sched_getattr		315
#endif
#define HAVE_SCHED_DEADLINE
#endif

#ifdef __i386__
#if ! (defined(__NR_sched_setattr) && defined(__NR_sched_getattr))
#define __NR_sched_setattr		351
#define __NR_sched_getattr		352
#endif
#define HAVE_SCHED_DEADLINE
#endif

#ifdef __arm__
#if ! (defined(__NR_sched_setattr) && defined(__NR_sched_getattr))
#define __NR_sched_setattr		380
#define __NR_sched_getattr		381
#endif
#define HAVE_SCHED_DEADLINE
#endif

struct sched_attr {
  __u32 size;

  __u32 sched_policy;
  __u64 sched_flags;

  /* SCHED_NORMAL, SCHED_BATCH */
  __s32 sched_nice;

  /* SCHED_FIFO, SCHED_RR */
  __u32 sched_priority;

  /* SCHED_DEADLINE (nsec) */
  __u64 sched_runtime;
  __u64 sched_deadline;
  __u64 sched_period;
};

#else
#define HAVE_SCHED_DEADLINE
#endif

static int dspd_sched_setattr(pid_t pid,
			      const struct sched_attr *attr,
			      unsigned int flags)
{
#ifdef HAVE_SCHED_DEADLINE
  int ret;
  ret = syscall(__NR_sched_setattr, pid, attr, flags);
  if ( ret < 0 )
    ret = -errno;
  return ret;
#else
  return -ENOSYS;
#endif
}

/*static int dspd_sched_getattr(pid_t pid,
			      struct sched_attr *attr,
			      unsigned int size,
			      unsigned int flags)
{
#ifdef HAVE_SCHED_DEADLINE
  int ret;
  ret = syscall(__NR_sched_getattr, pid, attr, size, flags);
  if ( ret < 0 )
    ret = -errno;
  return ret;
#else
  return -ENOSYS;
#endif
}*/

bool dspd_sched_enable_deadline(struct dspd_scheduler *sch)
{
  sch->have_sched_deadline = dspd_daemon_have_sched(SCHED_DEADLINE);
  return sch->have_sched_deadline;
}

int32_t dspd_sched_set_deadline_hint(struct dspd_scheduler *sch, 
				     int32_t avail_min,
				     int32_t buffer_time)
{
  int32_t ret = 0;
  if ( sch->have_sched_deadline &&
       (avail_min != sch->avail_min ||
	buffer_time != sch->buffer_time) )
    {
      struct sched_attr attr;
      memset(&attr, 0, sizeof(attr));
      attr.sched_flags = 0;
      attr.sched_nice = 0;
      attr.sched_priority = 0;
      attr.sched_policy = SCHED_DEADLINE;
      attr.sched_runtime = avail_min / 2;
      attr.sched_period = buffer_time / 2;
      attr.sched_deadline = avail_min;
      if ( attr.sched_period < attr.sched_deadline )
	attr.sched_period = attr.sched_deadline;
      attr.sched_runtime *= sch->timebase;
      attr.sched_period *= sch->timebase;
      attr.sched_deadline *= sch->timebase;
      ret = dspd_sched_setattr(sch->tid, &attr, 0);
      if ( ret == 0 )
	{
	  sch->avail_min = avail_min;
	  sch->buffer_time = buffer_time;
	}
    }
  return ret;
}

bool dspd_sched_deadline_init(struct dspd_scheduler *sch)
{
  struct sched_attr attr;
  int ret;
  if ( sch->have_sched_deadline && sch->tid < 0 )
    {
      sch->tid = dspd_gettid();
      memset(&attr, 0, sizeof(attr));
      attr.sched_flags = 0;
      attr.sched_nice = 0;
      attr.sched_priority = 0;
      attr.sched_policy = SCHED_DEADLINE;
      attr.sched_runtime = 2500000;
      attr.sched_period = attr.sched_deadline = 10000000;
      attr.sched_deadline = attr.sched_period;
      ret = dspd_sched_setattr(sch->tid, &attr, 0);
      sch->have_sched_deadline = ret < 0;

       
    }
  return sch->have_sched_deadline;
}
void dspd_sched_set_timebase(struct dspd_scheduler *sched, int32_t t)
{
  sched->timebase = t;
}

static void update_dl_latency(struct dspd_scheduler *sch, bool idle)
{
  struct sched_attr attr;
  dspd_time_t l, l2;
  memset(&attr, 0, sizeof(attr));
  attr.size = sizeof(attr);
  attr.sched_flags = 0;
  attr.sched_nice = 0;
  attr.sched_priority = 0;
  attr.sched_policy = SCHED_DEADLINE;
  if ( ! idle )
    {
      l = sch->latency / 4ULL;
      if ( l < 1024 )
	l = 1024;
      else if ( l > 250000000ULL )
	l = 250000000ULL;
      attr.sched_runtime = l * 2ULL;
      attr.sched_deadline = l * 3ULL;
      attr.sched_period = l * 4ULL;
      l2 = sch->latency;
    } else
    {
      l = 1000000ULL; //1ms
      attr.sched_period = l * 10UL; //10ms period
      attr.sched_runtime = l;  //1ms run time
      attr.sched_deadline = l * 2ULL; //2ms deadline
      l2 = UINT64_MAX;
    }
  if ( dspd_sched_setattr(sch->tid, &attr, 0) == 0 )
    sch->dl_latency = l2;
}
