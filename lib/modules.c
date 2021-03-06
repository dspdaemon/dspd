/*
 *  MODULES - solib module handler
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

#include <dlfcn.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include "util.h"
#include "modules.h"
#include "log.h"
#include "sslib.h"
#include "daemon.h"

static int dspd_module_list_new(struct dspd_module_list **list)
{
  struct dspd_module_list *ml;
  int ret;
  ml = calloc(1, sizeof(*ml));
  if ( ml )
    {
      ret = pthread_rwlock_init(&ml->lock, NULL);
    } else
    {
      ret = errno;
    }
  if ( ret != 0 )
    {
      free(ml);
    } else
    {
      *list = ml;
    }
  return ret;
}

static struct dspd_ll *find_highest(struct dspd_ll **list)
{
  struct dspd_ll *curr, *item = NULL;
  uint32_t p = 0;
  struct dspd_module *m;
  for ( curr = *list; curr; curr = curr->next )
    {
      m = curr->pointer;
      if ( m->callbacks->init_priority > p )
	{
	  item = curr;
	  p = m->callbacks->init_priority;
	}
    }
  if ( item != NULL )
    {
      if ( item->prev != NULL )
	item->prev->next = item->next;
      else
	*list = item->next;
      if ( item->next != NULL )
	item->next->prev = item->prev;
      item->next = NULL;
      item->prev = NULL;
    }
  return item;
}

int dspd_load_modules(struct dspd_module_list **l,
		      void *context,
		      const char **files,
		      const char **names,
		      size_t       count)
{
  size_t i;
  struct dspd_module *m = NULL;
  struct dspd_ll *curr;
  int ret = 0;
  char name[33];
  struct dspd_module_list *list;
  char *tmp; const char *p;
  bool fail_ok;
  ret = dspd_module_list_new(&list);
  if ( ret )
    return ret;
  tmp = calloc(1UL, PATH_MAX);
  if ( ! tmp )
    {
      ret = -errno;
      goto out;
    }
  list->daemon_ctx = context;
  for ( i = 0; i < count; i++ )
    {
      m = calloc(1, sizeof(*m));
      if ( ! m )
	{
	  ret = errno;
	  goto out;
	}
      p = files[i];
      if ( *p == '?' )
	{
	  fail_ok = true;
	  p++;
	} else
	{
	  fail_ok = false;
	}
      if ( p[0] == '@' )
	{
	  if ( strncmp(p, "@executable_path/", 17) == 0 )
	    {
	      if ( snprintf(tmp, PATH_MAX, "%s/%s", list->daemon_ctx->path, &p[17]) >= PATH_MAX )
		{
		  ret = ENAMETOOLONG;
		  goto out;
		}
	    } else if ( strncmp(p, "@modules_path/", 13) == 0 )
	    {
	      p = &p[13];
	      //Matches '@modules_path/modulename.so'
	      if ( snprintf(tmp, PATH_MAX, "%s/%s", dspd_get_modules_dir(), p) >= PATH_MAX )
		{
		  ret = ENAMETOOLONG;
		  goto out;
		}
	    } else if ( strncmp(p, "@loader_path/", 13) == 0 ) //Use ld.so.conf
	    {
	      strlcpy(tmp, p, PATH_MAX);
	    }
	} else if ( p[0] != '/' )
	{
	  //Use DSPD modules path by default for a relative path other than ./mod_*.so
	  if ( snprintf(tmp, PATH_MAX, "%s/%s", dspd_get_modules_dir(), p) >= PATH_MAX )
	    {
	      ret = ENAMETOOLONG;
	      goto out;
	    }
	} else
	{
	  //Use loader path because the location could not be parsed.
	  strlcpy(tmp, p, PATH_MAX);
	}
      m->dl_handle = dlopen(tmp, RTLD_NOW);
      if ( ! m->dl_handle )
	{
	  dspd_log(0, "Error loading module '%s': %s\n",
		   p, dlerror());
	  if ( ! fail_ok )
	    {
	      ret = ELIBACC;
	      goto out;
	    } else
	    {
	      continue;
	    }
	}
      sprintf(name, "dspd_mod_%s", names[i]);
      m->callbacks = dlsym(m->dl_handle, name);
      if ( ! m->callbacks )
	{
	  ret = EINVAL;
	  goto out;
	}
      m->name = strdup(names[i]);
      if ( ! m->name )
	{
	  ret = errno;
	  goto out;
	}
      m->file = strdup(files[i]);
      if ( ! m->file )
	{
	  ret = errno;
	  goto out;
	}
      if ( ! list->modules )
	{
	  list->modules = dspd_ll_new(m);
	  if ( ! list->modules )
	    goto out;
	} else if ( ! dspd_ll_append(list->modules, m) )
	{
	  goto out;
	}
      list->count++;
      m = NULL;
    }
  
  struct dspd_ll *head = NULL, *tail = NULL;
  while ( (curr = find_highest(&list->modules)) )
    {
      if ( tail )
	{
	  tail->next = curr;
	  curr->prev = tail;
	  tail = curr;
	} else
	{
	  head = curr;
	  tail = curr;
	}
    }
  if ( tail )
    {
      if ( list->modules != NULL )
	{
	  DSPD_ASSERT(tail->next == NULL);
	  tail->next = list->modules;
	  DSPD_ASSERT(list->modules->prev == NULL);
	  list->modules->prev = tail;
	}
      list->modules = head;
    } else
    {
      DSPD_ASSERT(list->modules == NULL);
    }
  

  for ( curr = list->modules; curr; curr = curr->next )
    {      
      m = curr->pointer;
      if ( m->callbacks->init )
	{
	  dspd_log(0, "Initializing module: %s", m->callbacks->desc);
	  ret = m->callbacks->init(list->daemon_ctx, &m->context);
	  if ( ret )
	    {
	      dspd_log(0, "Error %d while initializing %s", ret, m->callbacks->desc);
	      m = NULL;
	      goto out;
	    }
	  m = NULL;
	  list->lastinit = curr;
	}
    }
  
  free(tmp);
  *l = list;
  return 0;

 out:
  if ( m )
    {
      free(m->name);
      free(m->file);
      free(m);
    }
  free(tmp);
  dspd_module_list_delete(list);
  return ret;
}


void dspd_module_list_delete(struct dspd_module_list *list)
{
  struct dspd_ll *curr, *prev = NULL;
  struct dspd_module *m;
  for ( curr = list->lastinit; curr != NULL; curr = curr->prev )
    {
      m = curr->pointer;
      if ( m->callbacks->close )
	m->callbacks->close(list->daemon_ctx, &m->context);
    }
  for ( curr = list->modules; curr; curr = curr->next )
    {
      m = curr->pointer;
      if ( m )
	{
	  if ( m->dl_handle )
	    dlclose(m->dl_handle);
	  free(m->name);
	  free(m->file);
	}
      DSPD_ASSERT(prev != curr);
      free(prev);
      prev = curr;
    }
  free(prev);
  pthread_rwlock_destroy(&list->lock);
  free(list);
}
