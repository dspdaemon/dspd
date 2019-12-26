#ifndef _DSPD_THREAD_H_
#define _DSPD_THREAD_H_
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
/*
  This is all wrappers for pthread stuff.  Each object should
  be structured so it can be cast to a pthread type and be easily
  used with pthreads.  The only reason for this is that pthread stuff
  cannot be checked for initialization.  There is no portable "invalid
  mutex", etc.

  Each of these objects is considered uninitialized if zeroed with memset, calloc, etc.
  It is safe to cast pointers to them to pthread objects since the underlying
  pthread object is the first member of the struct.  Various dspd_ macros and inlines
  are defined, but accessing the struct directly should be encouraged since this is not
  meant to be a big bloated internal thread API.

*/
struct dspd_mutex {
  pthread_mutex_t mutex;
  uint8_t         init;
};
struct dspd_cond {
  pthread_cond_t  cond;
  uint8_t         init;
};
struct dspd_thread {
  pthread_t thread;
  uint8_t   init;
};
struct dspd_rwlock {
  pthread_rwlock_t lock;
  uint8_t          init;
};
struct dspd_threadattr {
  pthread_attr_t   attr;
  uint8_t          init;
};
typedef struct dspd_mutex  dspd_mutex_t;
typedef struct dspd_cond   dspd_cond_t;
typedef struct dspd_thread dspd_thread_t;
typedef struct dspd_rwlock dspd_rwlock_t;
typedef struct dspd_threadattr dspd_threadattr_t;
static inline int dspd_mutex_init(dspd_mutex_t *mut, pthread_mutexattr_t *attr)
{
  int ret = EBUSY;
  if ( ! mut->init )
    {
      ret = pthread_mutex_init(&mut->mutex, attr);
      if ( ret == 0 )
	mut->init = 1;
    }
  return ret;
}
static inline int dspd_mutex_destroy(dspd_mutex_t *mut)
{
  int ret = EINVAL;
  if ( mut->init )
    if ( (ret = pthread_mutex_destroy(&mut->mutex)) == 0 )
      mut->init = 0;
  return ret;
}
#define dspd_mutex_lock(_m)    pthread_mutex_lock(&(_m)->mutex)
#define dspd_mutex_unlock(_m)  pthread_mutex_unlock(&(_m)->mutex)
#define dspd_mutex_trylock(_m) pthread_mutex_trylock(&(_m)->mutex)
#define dspd_mutex_timedlock(_m,_t) pthread_mutex_timedlock(&(_m)->mutex, (_t))
#define DSPD_MUTEX_INITIALIZER {PTHREAD_MUTEX_INITIALIZER,1}

static inline int dspd_cond_init(dspd_cond_t *cond, pthread_condattr_t *attr)
{
  int ret = EBUSY;
  if ( ! cond->init )
    {
      ret = pthread_cond_init(&cond->cond, attr);
      if ( ret == 0 )
	cond->init = 1;
    }
  return ret;
}
static inline int dspd_cond_destroy(dspd_cond_t *cond)
{
  int ret = EINVAL;
  if ( cond->init )
    {
      ret = pthread_cond_destroy(&cond->cond);
      if ( ret == 0 )
	cond->init = 0;
    }
  return ret;
}
#define dspd_cond_wait(_c,_m) pthread_cond_wait(&(_c)->cond, &(_m)->mutex)
#define dspd_cond_timedwait(_c,_m,_t) pthread_cond_timedwait(&(_c)->cond,&(_m)->mutex,(_t))
#define dspd_cond_broadcast(_c) pthread_cond_broadcast(&(_c)->cond)
#define dspd_cond_signal(_c) pthread_cond_signal(&(_c)->cond)
#define DSPD_COND_INITIALIZER {PTHREAD_COND_INITIALIZER,1}
static inline int dspd_threadattr_init(dspd_threadattr_t *attr)
{
  int ret = EBUSY;
  if ( ! attr->init )
    {
      ret = pthread_attr_init(&attr->attr);
      if ( ret == 0 )
	attr->init = 1;
    }
  return ret;
}

static inline int dspd_threadattr_destroy(dspd_threadattr_t *attr)
{
  int ret = EINVAL;
  if ( attr->init )
    {
      if ( (ret = pthread_attr_destroy(&attr->attr)) == 0 )
	attr->init = 0;
    }
  return ret;
}

static inline int dspd_rwlock_init(dspd_rwlock_t *mut, pthread_rwlockattr_t *attr)
{
  int ret = EBUSY;
  if ( ! mut->init )
    {
      ret = pthread_rwlock_init(&mut->lock, attr);
      if ( ret == 0 )
	mut->init = 1;
    }
  return ret;
}
static inline int dspd_rwlock_destroy(dspd_rwlock_t *mut)
{
  int ret = EINVAL;
  if ( mut->init )
    if ( (ret = pthread_rwlock_destroy(&mut->lock)) == 0 )
      mut->init = 0;
  return ret;
}
#define dspd_rwlock_rdlock(_m)    pthread_rwlock_rdlock(&(_m)->lock)
#define dspd_rwlock_unlock(_m)  pthread_rwlock_unlock(&(_m)->lock)
#define dspd_rwlock_tryrdlock(_m) pthread_rwlock_tryrdlock(&(_m)->lock)
#define dspd_rwlock_timedrdlock(_m,_t) pthread_mutex_timedrdlock(&(_m)->lock, (_t))
#define dspd_rwlock_trywrlock(_m) pthread_rwlock_trywrlock(&(_m)->lock)
#define dspd_rwlock_timedwrlock(_m,_t) pthread_mutex_timedwrlock(&(_m)->lock, (_t))
#define dspd_rwlock_wrlock(_m)    pthread_rwlock_wrlock(&(_m)->lock)
#define DSPD_RWLOCK_INITIALIZER {PTHREAD_RWLOCK_INITIALIZER,1}



static inline int dspd_thread_create(dspd_thread_t *thread, const dspd_threadattr_t *attr,
				     void *(*start_routine) (void *), void *arg)
{
  int ret = EINVAL;
  if ( ! thread->init )
    {
      ret = pthread_create(&thread->thread, &attr->attr, start_routine, arg);
      if ( ret == 0 )
	thread->init = 1;
    }
  return ret;
}

#define dspd_thread_equal(_t1,_t2) pthread_equal((_t1).thread,(_t2).thread)
static inline int dspd_thread_join(dspd_thread_t *thread, void **retval)
{
  int ret = EINVAL;
  if ( thread->init )
    {
      ret = pthread_join(thread->thread, retval);
      if ( ret == 0 )
	thread->init = 0;
    }
  return ret;
}

int dspd_gettid(void);

#endif
