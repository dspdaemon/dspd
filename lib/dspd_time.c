/*
 *  DSPD_TIME - Timers
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


#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <stdio.h>
#include <atomic_ops.h>
#include <stdbool.h>
#include "dspd_time.h"

static volatile AO_TS_t lock = AO_TS_INITIALIZER;
static volatile AO_t init = 0;
static clockid_t clockid = -1;
static time_t clockres = -1;
static time_t tick = -1;

int dspd_time_init(void)
{
  struct timespec ts, ts2;
  (void)ts2;
  if ( AO_load(&init) )
    return 0;
  while ( AO_test_and_set(&lock) == AO_TS_SET )
    usleep(1);
  /*
    The coarse timers always seem to be CONFIG_HZ.
  */  
  if ( clock_getres(CLOCK_MONOTONIC, &ts) == 0 )
    {
      clockid = CLOCK_MONOTONIC;
#ifdef CLOCK_MONOTONIC_COARSE
      if ( clock_getres(CLOCK_MONOTONIC_COARSE, &ts2) == 0 )
	{
	  if ( ts2.tv_nsec < ts.tv_nsec )
	    tick = ts.tv_nsec;
	  else
	    tick = ts2.tv_nsec;
	}
#endif
    } else if ( clock_getres(CLOCK_REALTIME, &ts) == 0 )
    {
      clockid = CLOCK_REALTIME;
#ifdef CLOCK_REALTIME_COARSE
      if ( clock_getres(CLOCK_REALTIME_COARSE, &ts2) == 0 )
	{
	  if ( ts2.tv_nsec < ts.tv_nsec )
	    tick = ts.tv_nsec;
	  else
	    tick = ts2.tv_nsec;
	}
#endif
    } else
    {
      return -errno;
    }
  //Try to guess CONFIG_HZ.  Should normally be 100-1000Hz, but maybe this system
  //is different?
  if ( tick <= 100000 )
    {
      if ( ts.tv_nsec > 100000 )
	tick = ts.tv_nsec;
      else
	tick = 1000000;
    }
  clockres = ts.tv_nsec;
  AO_store(&init, 1);
  AO_CLEAR(&lock);
  return 0;
}

time_t dspd_get_tick(void)
{
  return tick;
}

time_t dspd_get_clockres(void)
{
  return clockres;
}

time_t dspd_get_clockid(void)
{
  return clockid;
}

dspd_time_t dspd_get_time(void)
{
  struct timespec ts;
  if ( clockid == -1 )
    if ( dspd_time_init() < 0 )
      return 0;
  if ( clock_gettime(clockid, &ts) != 0 )
    return 0;
  return (dspd_time_t)(ts.tv_sec * 1000000000ULL) + ts.tv_nsec;
}

dspd_time_t dspd_timespec_to_dspdtime(struct timespec *ts)
{
  return (dspd_time_t)(ts->tv_sec * 1000000000ULL) + ts->tv_nsec;
}

void dspd_time_to_timespec(dspd_time_t t, struct timespec *ts)
{
  ts->tv_sec = t / 1000000000ULL;
  ts->tv_nsec = t % 1000000000ULL;
}

int dspd_timer_init(struct dspd_timer *tmr)
{
  int fd, ret;
  if ( clockid == -1 )
    if ( (ret = dspd_time_init()) != 0 )
      return ret;
  fd = timerfd_create(clockid, TFD_CLOEXEC | TFD_NONBLOCK);
  if ( fd < 0 )
    return -errno;
  tmr->fd = fd;
  return 0;
}

int dspd_timer_new(struct dspd_timer **tmr)
{
  int ret;
  assert(tmr);
  *tmr = malloc(sizeof(struct dspd_timer));
  if ( ! *tmr )
    return -errno;
  ret = dspd_timer_init(*tmr);
  if ( ret )
    { free(*tmr); *tmr = NULL; }
  return ret;
}

int dspd_timer_set(struct dspd_timer *tmr, dspd_time_t abstime, uint32_t per)
{
  struct itimerspec new_val, old_val;
  int err = 0;
  if ( abstime == tmr->oneshot_next && per == tmr->interval && tmr->unlatch == false )
    return 0;
  if ( ! tmr->latched )
    {
      if ( per > 0 )
	{
	  new_val.it_interval.tv_sec = per / 1000000000;
	  new_val.it_interval.tv_nsec = per % 1000000000;
	} else
	{
	  new_val.it_interval.tv_sec = 0;
	  new_val.it_interval.tv_nsec = 0;
	}
      new_val.it_value.tv_sec = abstime / 1000000000;
      new_val.it_value.tv_nsec = abstime % 1000000000;
      tmr->oneshot_next = abstime;
      tmr->interval = per;
      err = timerfd_settime(tmr->fd, 
			    TFD_TIMER_ABSTIME,
			    &new_val,
			    &old_val);
      if ( err < 0 )
	return -errno;
    }
  tmr->oneshot_next = abstime;
  tmr->interval = per;
  tmr->trigger = false;
  return 0;
}

int dspd_timer_get(struct dspd_timer *tmr, dspd_time_t *abstime, uint32_t *per)
{
  *abstime = tmr->oneshot_next;
  *per = tmr->interval;
  return 0;
}

int dspd_timer_getexp(struct dspd_timer *tmr, dspd_time_t *exp)
{
  int ret;
  while ( (ret = read(tmr->fd, exp, sizeof(*exp))) != sizeof(*exp))
    {
      if ( ret == 0 || (ret < 0 && errno == EAGAIN) )
	{
	  *exp = 0;
	  return 0;
	}
      if ( ret < 0 && errno != EINTR )
	return -errno;
    }
  return 0;
}

int dspd_timer_getpollfd(struct dspd_timer *tmr, struct pollfd *pfd)
{
  assert(tmr);
  if ( tmr->fd <= 0 )
    return -EINVAL;
  pfd->fd = tmr->fd;
  pfd->events = POLLIN;
  pfd->revents = 0;
  return 0;
}

void dspd_timer_destroy(struct dspd_timer *tmr)
{
  close(tmr->fd);
  tmr->fd = -1;
}

void dspd_timer_delete(struct dspd_timer *tmr)
{
  dspd_timer_destroy(tmr);
  free(tmr);
}

int dspd_timer_fire(struct dspd_timer *tmr, bool latch)
{
  int ret = 0;
  uint64_t t;
  if ( tmr->trigger == false )
    {
      t = tmr->oneshot_next;
      ret = dspd_timer_set(tmr, 1, tmr->interval);
      if ( ret == 0 )
	{
	  if ( latch == true )
	    tmr->latched = true;
	  tmr->trigger = true;
	}
      tmr->oneshot_next = t;
    }
  return ret;
}

int dspd_timer_ack(struct dspd_timer *tmr)
{
  uint64_t val;
  int32_t ret = 0;
  if ( read(tmr->fd, &val, sizeof(val)) == sizeof(val) )
    {
      if ( tmr->latched )
	{
	  tmr->unlatch = true;
	  tmr->latched = false;
	  ret = dspd_timer_set(tmr, tmr->oneshot_next, tmr->interval);
	  tmr->unlatch = false;
	}
      tmr->trigger = false;
    }
  return ret;
}

bool dspd_timer_triggered(struct dspd_timer *tmr)
{
  return tmr->trigger;
}


int dspd_sleep(dspd_time_t abstime, dspd_time_t *waketime)
{
  uint64_t n;
  struct timespec ts, rm = { 0, 0 };
  ts.tv_sec = abstime / 1000000000;
  ts.tv_nsec = abstime % 1000000000;
  if ( clock_nanosleep(clockid, TIMER_ABSTIME, &ts, &rm) < 0 )
    {
      if ( errno != EINTR )
	return -errno;
      n = dspd_timespec_to_dspdtime(&rm);
      *waketime = abstime - n;
      return -EINTR;
    }
  n = dspd_timespec_to_dspdtime(&rm);
  *waketime = abstime - n;
  return 0;
}




void dspd_intrp_reset(struct dspd_intrp *i)
{
  i->last_tstamp = 0;
  i->diff = 0;
  i->have_tstamp = false;
}
		      
int64_t dspd_intrp_set(struct dspd_intrp *i, 
		       dspd_time_t tstamp, 
		       uint64_t ptr)
{
  uint64_t diff = i->last_tstamp - tstamp;
  dspd_intrp_update(i, tstamp, diff);
  if ( i->have_tstamp )
    return i->diff;
  i->have_tstamp = true;
  return 0;
}


void dspd_intrp_update(struct dspd_intrp *i, 
		       dspd_time_t tstamp, 
		       dspd_time_t diff)
{
  uint64_t tdiff = tstamp - i->last_tstamp;
  uint64_t pdiff = tdiff / i->sample_time;
  int64_t d = (pdiff - diff) * i->sample_time; //ns diff
  int64_t val;
  uint64_t lt;
 
  lt = i->last_tstamp;
  i->last_tstamp = tstamp;
  if ( diff == 0 )
    {
      if ( tstamp != lt )
	i->diff /= 2;
      return;
    }
  
  

  //Limit to some value
  val = llabs(d) / diff; //ns per frame 
  if ( val > i->maxdiff )
    val = i->maxdiff;

  if ( d < 0 )
    d = val * -1;
  else
    d = val;
  //Compute weighted average
  i->diff += d;
  i->diff /= 2;


}

uint64_t dspd_intrp_used(struct dspd_intrp *i, dspd_time_t time)
{
  int64_t f = (time / i->sample_time) * i->diff;
  int64_t ret;
  if ( llabs(f) >= time )
    ret = time;
  else
    ret = time - f;
  ret /= i->sample_time;
  return ret;
}

dspd_time_t dspd_intrp_frames(struct dspd_intrp *i, int64_t frames)
{
  int64_t f, ret;
  f = frames * i->diff;

  ret = ((frames * i->sample_time) + f) / i->sample_time;
  if ( ret <= 0 )
    ret = frames;
  return ret;
}

dspd_time_t dspd_intrp_time(struct dspd_intrp *i, dspd_time_t time)
{
  uint64_t ret;
  ret = dspd_intrp_frames(i, time / i->sample_time);
  return ret * i->sample_time;
}


void dspd_intrp_reset2(struct dspd_intrp *i, uint32_t rate)
{
  dspd_intrp_reset(i);
  if ( rate )
    i->sample_time = 1000000000 / rate;
}
