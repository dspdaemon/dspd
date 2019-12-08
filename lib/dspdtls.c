/*
 *  DSPDTLS - signal safe thread local storage
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

/*

  This is for signal handlers running within threads.  The idea is to
  be able to find the argument that a pthread was started with, but it
  could be used with any other pointer or pointer sized data.  The
  __thread attribute is not guaranteed to be signal safe, even for
  reading, or even exist on all platforms.

  This TLS has the following considerations:

  1.  A slot can be reused (released), but it can't be freed since all pointers
      must be safe to follow.
  2.  It is not safe to set or clear a slot inside of a signal handler.
      It is safe to get the pointer value and modify the buffer that it
      points to.
  3.  The slot must be manually released before returning from the thread
      function or calling pthread_exit()
  4.  It only works with a full pointer sized compare_and_swap function.
  5.  Only a single value, such as the pthread argument, can be used.
      Regular pthread tls and __thread tls don't have this restriction.
  

*/

#include "sslib.h"

#ifdef AO_HAVE_compare_and_swap_full
#define SLOT_EMPTY (intptr_t)-1

struct dspdtls_slot {
  void *ptr;
  AO_t  tid;
  struct dspdtls_slot *next;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct dspdtls_slot *slots;

void *dspdtls_get(void)
{
  struct dspdtls_slot *s;
  void *ret = NULL;
  pid_t tid = dspd_gettid();
  for ( s = slots; s; s = s->next )
    {
      if ( (pid_t)AO_load(&s->tid) == tid )
	{
	  ret = s->ptr;
	  break;
	}
    }
  return ret;
}

int32_t dspdtls_set(void *ptr)
{
  struct dspdtls_slot *s, **prev = &slots;
  pid_t tid = dspd_gettid();
  int32_t ret = -ENOMEM;
  pthread_mutex_lock(&lock);
  for ( s = slots; s; s = s->next )
    {
      //Try to claim an empty slot.
      if ( AO_compare_and_swap_full(&s->tid, SLOT_EMPTY, tid) )
	{
	  s->ptr = ptr;
	  prev = NULL;
	  ret = 0;
	  break;
	}
      prev = &s->next;
    }
  if ( prev )
    {
      //No empty slot was found.
      s = calloc(1, sizeof(*s));
      if ( s )
	{
	  s->ptr = ptr;
	  s->tid = tid;
	  *prev = s;
	  ret = 0;
	}
    }
  pthread_mutex_unlock(&lock);
  return ret;
}

void dspdtls_clear(void)
{
  struct dspdtls_slot *s;
  pid_t tid = dspd_gettid();
  pthread_mutex_lock(&lock);
  for ( s = slots; s; s = s->next )
    {
      //Find the slot with the current tid and mark it as empty.
      if ( AO_compare_and_swap_full(&s->tid, tid, SLOT_EMPTY) )
	{
	  s->ptr = NULL;
	  break;
	}
    }
  pthread_mutex_unlock(&lock);
}
#else
/*
  This works on most systems.
*/
static __thread void *tlsptr;
void *dspdtls_get(void)
{
  return tlsptr;
}
void dspdtls_clear(void)
{
  tlsptr = NULL;
}
int32_t dspdtls_set(void *arg)
{
  tlsptr = arg;
  return 0;
}
#endif

