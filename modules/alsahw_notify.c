#define _GNU_SOURCE
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "mod_alsahw.h"





static void disable_descriptor_list(struct alsahw_notifier *notifier,
				    struct alsahw_ctldata *ctl);

static int element_callback(snd_mixer_elem_t *elem,
			    unsigned int mask);

static int mixer_callback(snd_mixer_t *mixer, 
			  unsigned int mask,
			  snd_mixer_elem_t *elem)
{
  struct alsahw_ctldata *data = snd_mixer_get_callback_private(mixer);
  if ( (mask & SND_CTL_EVENT_MASK_ADD) && elem )
    {
      snd_mixer_elem_set_callback(elem, element_callback);
      snd_mixer_elem_set_callback_private(elem, mixer);
    }
  if ( data->alsa_cb )
    data->alsa_cb(mixer,
		  data,
		  mask,
		  elem,
		  data->arg);
  return 0;
}

void alsahw_mixer_event_notify(struct alsahw_ctldata *data,
			       int32_t card,
			       int32_t elem,
			       int32_t mask)
{
  struct alsahw_cb *curr;
  for ( curr = data->callbacks; curr; curr = curr->next )
    {
      curr->callback(card,
		     elem,
		     mask,
		     curr->arg);
    }
}

static struct alsahw_cb *new_cb(struct alsahw_notifier *n)
{
  size_t i;
  for ( i = 0; i < ARRAY_SIZE(n->callbacks); i++ )
    {
      if ( n->callbacks[i].callback == NULL )
	return &n->callbacks[i];
    }
  return calloc(1, sizeof(struct alsahw_cb));
}
static void delete_cb(struct alsahw_notifier *n, struct alsahw_cb *cb)
{
  uintptr_t base = (uintptr_t)n->callbacks;
  uintptr_t maxaddr = (uintptr_t)&n->callbacks[ARRAY_SIZE(n->callbacks)-1];
  uintptr_t addr = (uintptr_t)cb;
  if ( addr >= base && addr <= maxaddr )
    {
      memset(cb, 0, sizeof(*cb));
    } else
    {
      free(cb);
    }
}

static int element_callback(snd_mixer_elem_t *elem,
			    unsigned int mask)
{
  return mixer_callback(snd_mixer_elem_get_callback_private(elem),
			mask | ALSAHW_CTL_EVENT_MASK_CHANGED,
			elem);
}




static void lock_notifier(struct alsahw_notifier *n)
{
  static const uint64_t val = 1;
  /*
    The thread is either sleeping in poll() or going to finish notifying
    soon.
   */
  dspd_mutex_lock(&n->notify_lock);
  //Wake the thread.  It  should sleep on notify_lock once it wakes up.
  write(n->efd, &val, sizeof(val));
  dspd_mutex_lock(&n->poll_lock);
}

static void unlock_notifier(struct alsahw_notifier *n)
{
  dspd_mutex_unlock(&n->poll_lock);
  dspd_mutex_unlock(&n->notify_lock);
}

static struct alsahw_ctldata *ctl_for_mixer(struct alsahw_notifier *n,
				     snd_mixer_t *mixer)
{
  struct alsahw_ctldata *curr;
  for ( curr = n->ctl_list; curr; curr = curr->next )
    {
      if ( curr->mixer == mixer )
	break;
    }
  return curr;
}

int alsahw_register_mixer_callback(struct alsahw_notifier *n,
				   snd_mixer_t *mixer,
				   dspd_mixer_callback cb,
				   void *arg)
{
  int ret = -ENOENT;
  struct alsahw_ctldata *ctl;
  struct alsahw_cb *curr, **next;
  dspd_mutex_lock(&n->notify_lock);
  ctl = ctl_for_mixer(n, mixer);
  if ( ctl )
    {
      next = &ctl->callbacks;
      for ( curr = ctl->callbacks; curr; curr = curr->next )
	{
	  if ( curr->callback == cb && curr->arg == arg )
	    {
	      ret = -EEXIST;
	      goto out;
	    } 
	  next = &curr->next;
	}
      *next = new_cb(n);
      if ( *next )
	{
	  (*next)->callback = cb;
	  (*next)->arg = arg;
	  ret = 0;
	} else
	{
	  ret = -ENOMEM;
	}
    }
 out:
  dspd_mutex_unlock(&n->notify_lock);
  return ret;
}


int alsahw_unregister_mixer_callback(struct alsahw_notifier *n,
				     snd_mixer_t *mixer,
				     dspd_mixer_callback cb,
				     void *arg)
{
  int ret = -ENOENT;
  struct alsahw_ctldata *ctl;
  struct alsahw_cb *curr, **prev;
  dspd_mutex_lock(&n->notify_lock);
  ctl = ctl_for_mixer(n, mixer);
  if ( ctl )
    {
      prev = &ctl->callbacks;
      for ( curr = ctl->callbacks; curr; curr = curr->next )
	{
	  if ( curr->callback == cb && curr->arg == arg )
	    {
	      ret = 0;
	      *prev = curr->next;
	      delete_cb(n, curr);
	      break;
	    }
	  prev = &curr->next;
	}
    }
  dspd_mutex_unlock(&n->notify_lock);
  return ret;
}

static int insert_fds(struct alsahw_notifier *notifier, 
		      struct pollfd *pfd,
		      size_t nfds)
{
  void *ptr;
  size_t nc = nfds + notifier->nfds;
  ptr = realloc(notifier->pfds, nc * sizeof(notifier->pfds[0]));
  if ( ! ptr )
    return -ENOMEM;
  notifier->pfds = ptr;
  memcpy(&notifier->pfds[notifier->nfds], 
	 pfd,
	 nfds * sizeof(*pfd));
  notifier->nfds = nc;
  return 0;
}

int alsahw_register_mixer(struct alsahw_notifier *notifier,
			  snd_mixer_t *mixer,
			  pthread_mutex_t *lock,
			  alsahw_mixer_callback cb,
			  void *arg)
{
  int ret;
  struct alsahw_ctldata *ctl, *curr, **prev;
  ctl = calloc(1, sizeof(*ctl));
  if ( ! ctl )
    return -ENOMEM;
  ctl->alsa_cb = cb;
  ctl->arg = arg;
  ctl->lock = lock;
  ctl->mixer = mixer;
  snd_mixer_set_callback(mixer, mixer_callback);
  snd_mixer_set_callback_private(mixer, ctl);
  ret = snd_mixer_poll_descriptors_count(mixer);
  if ( ret < 0 )
    goto out;
  if ( ret == 0 )
    {
      ret = -EINVAL;
      goto out;
    }
  ctl->pfds = calloc(ret, sizeof(*ctl->pfds));
  if ( ! ctl->pfds )
    {
      ret = -EINVAL;
      goto out;
    }

  ret = snd_mixer_poll_descriptors(mixer, ctl->pfds, ret);
  if ( ret < 0 )
    goto out;
  if ( ret == 0 )
    {
      ret = -EINVAL;
      goto out;
    }
  ctl->nfds = ret;

  lock_notifier(notifier);
  
  prev = &notifier->ctl_list;
  for ( curr = notifier->ctl_list; curr; curr = curr->next )
    {
      prev = &curr->next;
      if ( curr->mixer == mixer )
	{
	  ret = -EEXIST;
	  break;
	}
    }
  if ( ret != -EEXIST )
    {
      ret = insert_fds(notifier, ctl->pfds, ctl->nfds);
      if ( ret == 0 )
	{
	  *prev = ctl;
	  snd_mixer_elem_t *curr;
	  for ( curr = snd_mixer_first_elem(mixer);
		curr;
		curr = snd_mixer_elem_next(curr) )
	    {
	      snd_mixer_elem_set_callback(curr, element_callback);
	      snd_mixer_elem_set_callback_private(curr, mixer);
	    }
	}
    }




  unlock_notifier(notifier);

 out:
  if ( ret )
    {
      
      snd_mixer_set_callback(mixer, NULL);
      snd_mixer_set_callback_private(mixer, NULL);
      free(ctl->pfds);
      free(ctl);
    }
  return ret;
}

static void remove_callbacks(struct alsahw_notifier *n, struct alsahw_ctldata *ctl, snd_mixer_t *mixer)
{
  struct alsahw_cb *curr, *prev = NULL;
  for ( curr = ctl->callbacks; curr; curr = curr->next )
    {
      delete_cb(n, prev);
      prev = curr;
    }
  delete_cb(n, prev);
}

static bool checkfd(int fd, const struct pollfd *pfds, size_t nfds)
{
  size_t i;
  for ( i = 0; i < nfds; i++ )
    {
      if ( pfds[i].fd == fd )
	return true;
    }
  return false;
}

static void remove_pfds(struct alsahw_notifier *n, const struct pollfd *pfds, size_t nfds)
{
  size_t i, o = 1;
  for ( i = 1; i < n->nfds; i++ )
    {
      if ( ! checkfd(n->pfds[i].fd, pfds, nfds) )
	{
	  memmove(&n->pfds[o], &n->pfds[i], sizeof(struct pollfd));
	  o++;
	}
    }
  n->nfds = o;
}

int alsahw_unregister_mixer(struct alsahw_notifier *notifier,
			    snd_mixer_t *mixer)
{
  int ret = -ENOENT;
  struct alsahw_ctldata *curr, **prev = &notifier->ctl_list;
  lock_notifier(notifier);
  for ( curr = notifier->ctl_list; curr; curr = curr->next )
    {
      if ( curr->mixer == mixer )
	{

	  snd_mixer_elem_t *c;
	  for ( c = snd_mixer_first_elem(mixer);
		c;
		c = snd_mixer_elem_next(c) )
	    {
	      snd_mixer_elem_set_callback(c, NULL);
	      snd_mixer_elem_set_callback_private(c, NULL);
	    }

	  *prev = curr->next;
	  ret = 0;
	  remove_callbacks(notifier, curr, mixer);
	  snd_mixer_set_callback(mixer, NULL);
	  snd_mixer_set_callback_private(mixer, NULL);
	  remove_pfds(notifier, curr->pfds, curr->nfds);
	  free(curr->pfds);
	  free(curr);
	  break;
	}
      prev = &curr->next;
    }

  unlock_notifier(notifier);
  return ret;
}

static void set_event_for_fd(struct alsahw_notifier *notifier, 
			     struct pollfd *pfd)
{
  struct alsahw_ctldata *curr;
  size_t i;
  for ( curr = notifier->ctl_list; curr; curr = curr->next )
    {
      for ( i = 0; i < curr->nfds; i++ )
	{
	  if ( curr->pfds[i].fd == pfd->fd )
	    {
	      curr->pfds[i].revents = pfd->revents;
	      curr->ready = 1;
	      return;
	    }
	}
    }
}

static void disable_descriptor_list(struct alsahw_notifier *notifier,
				    struct alsahw_ctldata *ctl)
{
  size_t i, j;
  for ( i = 0; i < ctl->nfds; i++ )
    {
      for ( j = 1; j < notifier->nfds; j++ )
	{
	  if ( ctl->pfds[i].fd == notifier->pfds[j].fd )
	    {
	      notifier->pfds[j].fd = -1;
	      break;
	    }
	}
    }
  j = 1;
  for ( i = 1; i < notifier->nfds; i++ )
    {
      if ( notifier->pfds[i].fd != -1 )
	{
	  memmove(&notifier->pfds[j], 
		  &notifier->pfds[i], 
		  sizeof(struct pollfd));
	  j++;
	}
    }
  notifier->nfds = j;
}

static int check_event(struct alsahw_ctldata *ctl)
{
  int ret;
  unsigned short ev = 0;
  if ( ctl->lock )
    pthread_mutex_lock(ctl->lock);
  ctl->ready = 0;
  ret = snd_mixer_poll_descriptors_revents(ctl->mixer,
					   ctl->pfds,
					   ctl->nfds,
					   &ev);
  if ( ev )
    {
      if ( snd_mixer_handle_events(ctl->mixer) < 0 )
	ret = -1;
      else if ( ev & (POLLERR|POLLRDHUP|POLLHUP|POLLNVAL) )
	ret = -1;
    }
  if ( ctl->lock )
    pthread_mutex_unlock(ctl->lock);
  return ret;
}

static void dispatch_events(struct alsahw_notifier *notifier)
{
  size_t i;
  struct pollfd *pfd;
  uint64_t val;
  struct alsahw_ctldata *curr;
  if ( notifier->pfds[0].revents & POLLIN )
    read(notifier->pfds[0].fd, &val, sizeof(val));
  for ( i = 1; i < notifier->nfds; i++ )
    {
      pfd = &notifier->pfds[i];
      if ( pfd->revents )
	{
	  set_event_for_fd(notifier, pfd);
	  pfd->revents = 0;
	}
    }
  for ( curr = notifier->ctl_list; curr; curr = curr->next )
    {
      if ( curr->ready )
	if ( check_event(curr) < 0 )
	  disable_descriptor_list(notifier, curr);
    }
}


void *notify_thread(void *p)
{
  int ret;
  struct alsahw_notifier *n = p;
  int e;
  prctl(PR_SET_NAME, "alsahw_notify", 0, 0, 0);
  while(1)
    {
      dspd_mutex_lock(&n->poll_lock);
      ret = poll(n->pfds, n->nfds, -1);
      dspd_mutex_unlock(&n->poll_lock);
      
      if ( ret < 0 )
	{
	  e = errno;
	  if ( e == EAGAIN || e == EINTR )
	    {
	      usleep(1);
	      continue;
	    } else
	    {
	      break;
	    }
	}

      //Having the lock dropped like this means that
      //anything modifying epfds must be sure to preserve revents.
      dspd_mutex_lock(&n->notify_lock);
      dispatch_events(n);
      dspd_mutex_unlock(&n->notify_lock);
      
    }
  return NULL;
}

int alsahw_init_notifier(struct alsahw_notifier **notifier)
{
  int ret;
  struct alsahw_notifier *n = calloc(1, sizeof(struct alsahw_notifier));
  if ( ! n )
    return -ENOMEM;
  n->efd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
  if ( n->efd < 0 )
    {
      ret = -errno;
      goto out;
    }
  n->pfds = calloc(1, sizeof(struct pollfd));
  if ( ! n->pfds )
    {
      ret = -ENOMEM;
      goto out;
    }
  n->pfds[0].fd = n->efd;
  n->pfds[0].events = POLLIN;
  n->nfds = 1;

  ret = dspd_thread_create(&n->thread, NULL, notify_thread, n);
  if ( ret != 0 )
    ret *= -1;
 out:
  if ( ret )
    {
      dspd_mutex_destroy(&n->poll_lock);
      dspd_mutex_destroy(&n->notify_lock);
      close(n->efd);
      free(n->pfds);
      free(n);
    } else
    {
      *notifier = n;
    }
  return ret;
}
