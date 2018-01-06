#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#define _DSPD_CTL_MACROS
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../lib/cbpoll.h"
#include "ss_eventq.h"


ssize_t socksrv_eq_realloc(struct socksrv_ctl_eq *eq, size_t min_events, size_t max_events, size_t curr_events)
{
  struct socksrv_ctl_event *ev;
  size_t len = eq->in - eq->out, i, new_size;
  ssize_t ret = 0;
  if ( curr_events == 0 && min_events == 0 && max_events == 0 )
    {
      //Free memory
      free(eq->events);
      eq->events = NULL;
      eq->in = 0;
      eq->out = 0;
      eq->max_events = 0;
      eq->min_events = 0;
      eq->event_count = 0;
    } else if ( len == 0 )
    {
      if ( curr_events < 2 )
	curr_events = 2;
      if ( max_events < 2 )
	max_events = 2;
      if ( min_events < 2 )
	min_events = 2;
      //Change buffer size with empty buffer
      if ( curr_events < min_events )
	curr_events = min_events;
      if ( curr_events != eq->event_count )
	ev = realloc(eq->events, sizeof(*ev) * curr_events);
      else
	ev = eq->events;
      if ( ev )
	{
	  eq->events = ev;
	  eq->in = 0;
	  eq->out = 0;
	  eq->event_count = curr_events;
	  eq->max_events = max_events;
	  eq->min_events = min_events;
	  ret = curr_events;
	} else
	{
	  ret = -ENOMEM;
	}
    } else if ( max_events < len )
    {
      //Too much data in buffer.
      ret = -EBUSY;
    } else
    {
      new_size = MIN(len, curr_events);
      if ( new_size < 2 )
	new_size = 2;
      if ( max_events < 2 )
	max_events = 2;
      if ( min_events < 2 )
	min_events = 2;
      ev = malloc(sizeof(*ev) * new_size);
      if ( ev )
	{
	  for ( i = 0; i < len; i++ )
	    ev[i] = eq->events[(eq->in + i) % eq->event_count];
	  free(eq->events);
	  eq->events = ev;
	  eq->in = len;
	  eq->out = 0;
	  eq->event_count = new_size;
	  eq->max_events = max_events;
	  eq->min_events = min_events;
	  ret = new_size;
	} else
	{
	  ret = -ENOMEM;
	}
    }
  return ret;
}



bool socksrv_eq_push(struct socksrv_ctl_eq *eq, 
		     const struct socksrv_ctl_event *evt)
{
  size_t i, pos, fill, n;
  struct socksrv_ctl_event *e;
  ssize_t ret;
  for ( i = eq->in; i < eq->out; i++ )
    {
      pos = i % eq->event_count;
      e = &eq->events[pos];
      if ( e->card == evt->card && evt->elem == evt->elem )
	{
	  e->mask |= evt->mask;
	  return true;
	}
    }
  fill = eq->in - eq->out;
  if ( fill == eq->event_count )
    {
      //The buffer is full.  Try to expand it.
      n = fill * 2;
      if ( n > eq->max_events )
	n = eq->max_events; //Don't go over the buffer size limit
      if ( n == eq->event_count ) //Reached buffer size limit.
	return false;
      ret = socksrv_eq_realloc(eq, eq->min_events, eq->max_events, n);
      if ( ret < 0 ) //Out of memory
	return false;
    }
  eq->events[eq->in % eq->event_count] = *evt;
  eq->in++;
  return true;
}

bool socksrv_eq_pop(struct socksrv_ctl_eq *eq, 
		    struct socksrv_ctl_event *evt)
{
  size_t len;
  bool ret;
  len = eq->in - eq->out;
  ret = !!len;
  if ( len )
    {
      *evt = eq->events[eq->out%eq->event_count];
      eq->out++;
      len--;
      if ( len == 0 && eq->event_count > eq->min_events )
	socksrv_eq_realloc(eq, eq->min_events, eq->max_events, eq->min_events);
    }
  return ret;
}

void socksrv_eq_reset(struct socksrv_ctl_eq *eq)
{
  eq->in = 0;
  eq->out = 0;
  if ( eq->event_count > 0 )
    socksrv_eq_realloc(eq, eq->min_events, eq->max_events, eq->min_events);
}
