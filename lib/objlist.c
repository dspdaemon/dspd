/*
 *  OBJLIST - a mostly nonblocking object list
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

#include <pthread.h>
#include <stdint.h>
#include <atomic_ops.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "objlist.h"
#include "atomic.h"
#include "req.h"

#define KL_FUTEX
//#define KL_PTHREADS
#ifdef KL_FUTEX
/*
  FUTEX based keyed lock

  This version has no race condition.  It is simply unable to take the lock
  if the wrong key is used.  The pthread version may very rarely take the lock
  and release it because the key is wrong.  That could theoretically cause
  a glitch if another thread wanted to take the lock and failed.  Even so,
  it would only be a glitch for one client stream and not for the entire
  device since realtime device io threads do not sleep for a lock that
  is unavailable.

*/
#include <linux/futex.h>
#include <sys/time.h>
struct keyed_lock {
  volatile uint32_t futex;
  uint32_t key;
  volatile AO_t     wcnt;
};

static inline int sys_futex(volatile uint32_t *uaddr, 
			    int op, 
			    int val, 
			    struct timespec *timeout)
{
  return syscall(__NR_futex, uaddr, op, val, timeout);
}

/*
  Compare value at *addr with old and replace *old with new_val if
  they match.  This is from atomic_ops/sysdeps/gcc/x86.h.

  This needs rewritten for each CPU type.  It works on x86 and x86_64.
*/
static inline int compare_and_swap_u32(volatile uint32_t *addr,
				       uint32_t old, uint32_t new_val) 
{
  char result;
  __asm__ __volatile__("lock; cmpxchgl %3, %0; setz %1"
	    	       : "=m"(*addr), "=q"(result)
		       : "m"(*addr), "r" (new_val), "a"(old) : "memory");
  return (int) result; //1=success, 0=failure
}
static inline void wait_for(volatile uint32_t *addr, uint32_t value)
{
  int v;
  while ((v = *addr) != value)
    sys_futex(addr, FUTEX_WAIT, v, NULL);
}

static bool kl_trylock_keyed(struct keyed_lock *kl, uint32_t key)
{
  bool ret;
  if ( dspd_load_uint32(&kl->futex) == key )
    ret = compare_and_swap_u32(&kl->futex, key, UINT32_MAX);
  else
    ret = false;
  return ret;
}
static inline void kl_set_key(struct keyed_lock *kl, uint32_t key)
{
  dspd_store_uint32(&kl->futex, key);
  kl->key = key;
}
static inline uint32_t kl_get_key(struct keyed_lock *kl)
{
  return kl->key;
}
static void kl_destroy(struct keyed_lock *kl)
{
  kl->key = 0;
  kl->futex = 0;
}
static bool kl_init(struct keyed_lock *kl)
{
  kl->wcnt = 0;
  kl->key = 0;
  return true;
}

static bool kl_lock_keyed(struct keyed_lock *kl, uint32_t key)
{
  if ( ! kl_trylock_keyed(kl, key) )
    {
      AO_fetch_and_add1(&kl->wcnt);
      while ( kl_trylock_keyed(kl, key) == 0 )
	wait_for(&kl->futex, key);
      AO_fetch_and_sub1(&kl->wcnt);
    }
  return true;
}
static inline bool kl_lock(struct keyed_lock *kl)
{
  return kl_lock_keyed(kl, kl->key);
}

static void kl_unlock(struct keyed_lock *kl)
{
  dspd_store_uint32(&kl->futex, kl->key);
  if ( AO_load(&kl->wcnt) > 0 )
    sys_futex(&kl->futex, FUTEX_WAKE, 1, NULL);
}
#endif

#ifdef KL_PTHREADS
struct keyed_lock {
  pthread_mutex_t    lock;
  volatile AO_t      key;
};

bool kl_init(struct keyed_lock *kl)
{
  bool ret;
  if ( pthread_mutex_init(&kl->lock, NULL) == 0 )
    {
      kl->key = 0;
      ret = 1;
    } else
    {
      ret = 0;
    }
  return ret;
}

void kl_destroy(struct keyed_lock *kl)
{
  pthread_mutex_destroy(&kl->lock);
  kl->key = 0;
}

void kl_lock(struct keyed_lock *kl)
{
  pthread_mutex_lock(&kl->lock);
}

void kl_unlock(struct keyed_lock *kl)
{
  pthread_mutex_unlock(&kl->lock);
}

bool kl_trylock(struct keyed_lock *kl)
{
  return pthread_mutex_trylock(&kl->lock) == 0;
}

bool kl_trylock_keyed(struct keyed_lock *kl, uint32_t key)
{
  bool ret;
  if ( AO_load(&kl->key) == key )
    {
      ret = (pthread_mutex_trylock(&kl->lock) == 0);
      if ( ret )
	{
	  if ( AO_load(&kl->key) != key )
	    {
	      pthread_mutex_unlock(&kl->lock);
	      ret = false;
	    }
	}
    } else
    {
      ret = false;
    }
  return ret;
}

bool kl_lock_keyed(struct keyed_lock *kl, uint32_t key)
{
  bool ret;
  if ( AO_load(&kl->key) == key )
    {
      ret = (pthread_mutex_lock(&kl->lock) == 0);
      if ( ret )
	{
	  if ( AO_load(&kl->key) != key )
	    {
	      pthread_mutex_unlock(&kl->lock);
	      ret = false;
	    }
	}
    } else
    {
      ret = false;
    }
  return ret;
}

void kl_set_key(struct keyed_lock *kl, uint32_t key)
{
  kl->key = key;
}

uint32_t kl_get_key(struct keyed_lock *kl)
{
  return kl->key;
}
#endif
struct dspd_slist_entry {
  bool  used;
  void *data;
  void *server_ops;
  void *client_ops;
  void (*destructor)(void *data);
  int32_t (*ctl)(struct dspd_rctx *rctx,
		 uint32_t             req,
		 const void          *inbuf,
		 size_t        inbufsize,
		 void         *outbuf,
		 size_t        outbufsize);
  uint32_t refcnt;
  uint64_t slot_id;
  struct keyed_lock lock;
  pthread_rwlock_t  rwlock;
};

struct dspd_slist {
  pthread_rwlock_t   lock;
  uintptr_t          count;
  pthread_mutex_t    idlock;
  uint64_t           last_id;
  struct dspd_slist_entry *entries;
};

uintptr_t dspd_slist_get_object_mask(struct dspd_slist *list,
				     uint8_t *mask, 
				     size_t   mask_size,
				     bool server, 
				     bool client)
{
  uintptr_t i, maxidx = mask_size * 8, count = 0;
  const struct dspd_slist_entry *e;
  if ( maxidx > list->count )
    maxidx = list->count;
  memset(mask, 0, mask_size);
  pthread_rwlock_rdlock(&list->lock);
  for ( i = 0; i < list->count; i++ )
    {
      e = &list->entries[i];
      if ( e->used && ((e->server_ops && server) || (e->client_ops && client)) )
	{
	  if ( i < maxidx )
	    dspd_set_bit(mask, i);
	  count++;
	}
    }
  pthread_rwlock_unlock(&list->lock);
  return count;
}


static void unwind(struct dspd_slist *l)
{
  uintptr_t i;
  struct dspd_slist_entry *e;
  for ( i = 0; i < l->count; i++ )
    {
      e = &l->entries[i];
      pthread_rwlock_destroy(&e->rwlock);
      kl_destroy(&e->lock);
    }
}

struct dspd_slist *dspd_slist_new(uintptr_t entries)
{
  uintptr_t i;
  struct dspd_slist_entry *e;
  struct dspd_slist *l = calloc(1, sizeof(struct dspd_slist));
  if ( ! l )
    return NULL;
  l->entries = calloc(entries, sizeof(l->entries[0]));
  if ( ! l->entries )
    {
      free(l);
      return NULL;
    }
  if ( pthread_rwlock_init(&l->lock, NULL) != 0 )
    {
      free(l->entries);
      free(l);
      return NULL;
    }
  if ( pthread_mutex_init(&l->idlock, NULL) != 0 )
    {
      free(l->entries);
      free(l);
      pthread_rwlock_destroy(&l->lock);
      return NULL;
    }
  
  l->count = entries;
  for ( i = 0; i < l->count; i++ )
    {
      e = &l->entries[i];
      if ( ! kl_init(&e->lock) )
	{
	  l->count = i - 1;
	  unwind(l);
	  goto out;
	}
      if ( pthread_rwlock_init(&e->rwlock, NULL) != 0 )
	{
	  kl_destroy(&e->lock);
	  l->count = i - 1;
	  unwind(l);
	  goto out;
	}
    }
  return l;

 out:
  pthread_rwlock_destroy(&l->lock);
  pthread_mutex_destroy(&l->idlock);
  free(l->entries);
  free(l);
  return NULL;
}

void dspd_slist_delete(struct dspd_slist *l)
{
  unwind(l);
  pthread_rwlock_destroy(&l->lock);
  pthread_mutex_destroy(&l->idlock);
  free(l->entries);
  free(l);
}

intptr_t dspd_slist_get_free(struct dspd_slist *list, int32_t whence)
{
  uintptr_t i, idx;
  struct dspd_slist_entry *e;
  for ( i = 0; i < list->count; i++ )
    {
      if ( whence < 0 )
	idx = i;
      else
	idx = list->count - (i + 1);
      e = &list->entries[idx];
      if ( ! e->used )
	{
	  pthread_rwlock_wrlock(&e->rwlock);
	  kl_lock(&e->lock);
	  return idx;
	}
    }
  return -1;
}

void dspd_slist_entry_get_pointers(struct dspd_slist *list, uintptr_t entry, void **data, void **server_ops, void **client_ops)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  *data = e->data;
  *server_ops = e->server_ops;
  *client_ops = e->client_ops;
}

void dspd_slist_entry_set_pointers(struct dspd_slist *list, uintptr_t entry, void *data, void *server_ops, void *client_ops)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  e->data = data;
  e->server_ops = server_ops;
  e->client_ops = client_ops;
}
uint64_t dspd_slist_id(struct dspd_slist *list, uintptr_t entry)
{
  return list->entries[entry].slot_id;
}
void dspd_slist_entry_set_used(struct dspd_slist *list, uintptr_t entry, bool used)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  e->used = used;
  if ( ! used )
    {
      e->server_ops = NULL;
      e->client_ops = NULL;
      e->data = NULL;
      kl_set_key(&e->lock, 0);
    } else
    {
      pthread_mutex_lock(&list->idlock);
      list->last_id++;
      e->slot_id = list->last_id;
      pthread_mutex_unlock(&list->idlock);
    }
}

void dspd_slist_entry_srvlock(struct dspd_slist *list, uintptr_t entry)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  kl_lock(&e->lock);
}

void dspd_slist_entry_set_key(struct dspd_slist *list, uintptr_t entry, uint32_t key)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  kl_set_key(&e->lock, key);
}

uint32_t dspd_slist_entry_get_key(struct dspd_slist *list, uintptr_t entry)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  return kl_get_key(&e->lock);
}

void dspd_slist_entry_srvunlock(struct dspd_slist *list, uintptr_t entry)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  kl_unlock(&e->lock);
}

void dspd_slist_entry_wrlock(struct dspd_slist *list, uintptr_t entry)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  pthread_rwlock_wrlock(&e->rwlock);
}

void dspd_slist_entry_rdlock(struct dspd_slist *list, uintptr_t entry)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  pthread_rwlock_rdlock(&e->rwlock);
}

void dspd_slist_entry_rw_unlock(struct dspd_slist *list, uintptr_t entry)
{
  struct dspd_slist_entry *e = &list->entries[entry];
  pthread_rwlock_unlock(&e->rwlock);
}




void dspd_slist_rdlock(struct dspd_slist *list)
{
  pthread_rwlock_rdlock(&list->lock);
}

void dspd_slist_wrlock(struct dspd_slist *list)
{
  pthread_rwlock_wrlock(&list->lock);
}

void dspd_slist_unlock(struct dspd_slist *list)
{
  pthread_rwlock_unlock(&list->lock);
}




bool dspd_client_srv_lock(struct dspd_slist *list, uintptr_t index, uintptr_t key)
{
  struct dspd_slist_entry *e = &list->entries[index];
  bool ret;
  if ( e->used )
    {
      ret = kl_lock_keyed(&e->lock, key);
      if ( ret )
	{
	  ret = e->used;
	  if ( ! ret )
	    kl_unlock(&e->lock);
	}
    } else
    {
      ret = false;
    }
  return ret;
}

bool dspd_client_srv_trylock(struct dspd_slist *list, uintptr_t index, uintptr_t key)
{
  struct dspd_slist_entry *e = &list->entries[index];
  bool ret;
  if ( e->used )
    {
      ret = kl_trylock_keyed(&e->lock, key);
      if ( ret )
	{
	  ret = e->used;
	  if ( ! ret )
	    kl_unlock(&e->lock);
	}
    } else
    {
      ret = false;
    }
  return ret;
}

void dspd_client_srv_unlock(struct dspd_slist *list, uintptr_t index)
{
  struct dspd_slist_entry *e = &list->entries[index];
  kl_unlock(&e->lock);
}

//Must have the rwlock
uint32_t dspd_slist_ref(struct dspd_slist *list, uintptr_t index)
{
  uint32_t ret = 0;
  if ( index < list->count )
    {
      if ( list->entries[index].used )
	{
	  list->entries[index].refcnt++;
	  ret = list->entries[index].refcnt;
	}
    }
  return ret;
}

//Must have the rwlock
uint32_t dspd_slist_unref(struct dspd_slist *list, uintptr_t index)
{
  uint32_t ret = 0;
  if ( index < list->count )
    {
      if ( list->entries[index].used )
	{
	  list->entries[index].refcnt--;
	  ret = list->entries[index].refcnt;
	  if ( ret == 0 )
	    {
	      if ( list->entries[index].destructor )
		list->entries[index].destructor(list->entries[index].data);
	      dspd_slist_entry_set_used(list, index, 0);
	    }
	}
    }
  return ret;
}

uint32_t dspd_slist_refcnt(struct dspd_slist *list, uintptr_t index)
{
  uint32_t ret = 0;
  if ( index < list->count )
    {
      if ( list->entries[index].used )
	ret = list->entries[index].refcnt;
    }
  return ret;
}

void dspd_slist_set_destructor(struct dspd_slist *list,
			       uintptr_t index,
			       void (*destructor)(void *data))
{
  list->entries[index].destructor = destructor;
}

void dspd_slist_set_ctl(struct dspd_slist *list,
			uintptr_t object,
			int32_t (*ctl)(struct dspd_rctx *rctx,
				       uint32_t             req,
				       const void          *inbuf,
				       size_t        inbufsize,
				       void         *outbuf,
				       size_t        outbufsize))
{
  if ( object < list->count )
    list->entries[object].ctl = ctl;
}

int32_t dspd_slist_ctl(struct dspd_slist *list,
		       struct dspd_rctx *rctx,
		       uint32_t             req,
		       const void          *inbuf,
		       size_t        inbufsize,
		       void         *outbuf,
		       size_t        outbufsize)
{
  struct dspd_slist_entry *e;
  int32_t ret = -EINVAL;
  int handled = 0;
  uintptr_t object = (uintptr_t)rctx->index;
  if ( object < list->count )
    {
      e = &list->entries[object];
      if ( e->used && e->ctl )
	{
	  rctx->user_data = e->data;
	  handled = 1;
	  ret = e->ctl(rctx,
		       req,
		       inbuf,
		       inbufsize,
		       outbuf,
		       outbufsize);
	}
    }
  if ( ! handled )
    ret = rctx->ops->reply_err(rctx, 0, EINVAL);
  return ret;
}
