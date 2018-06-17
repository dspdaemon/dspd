/*
 *  RTALLOC - Realtime memory allocator
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

#include <atomic_ops.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include "rtalloc.h"
#include "util.h"

bool rtalloc_check_buffer(struct dspd_rtalloc *alloc, void *addr)
{
  bool ret;
  if ( alloc != NULL )
    {
      uintptr_t first = ((uintptr_t)addr - (uintptr_t)alloc->membase) / alloc->pagesize;
      ret = (first < alloc->pagecount);
    } else
    {
      ret = true;
    }
  return ret;
}


static inline size_t realsize(size_t s)
{
  size_t n = s / 16;
  if ( s % 16 )
    n++;
  return n * 16;
}

void dspd_rtalloc_shrink(struct dspd_rtalloc *alloc, void *addr, size_t new_size)
{
  uintptr_t npages;
  uintptr_t first = ((uintptr_t)addr - (uintptr_t)alloc->membase) / alloc->pagesize, i, last, diff;
  struct pageinfo *p;

  if ( alloc == NULL )
    return;
  DSPD_ASSERT((uintptr_t)addr < alloc->boundary);
 
  DSPD_ASSERT(first < alloc->pagecount);
  npages = new_size / alloc->pagesize;
  if ( new_size % alloc->pagesize )
    npages++;
  npages--; //Additional pages, not total pages.
  p = &alloc->pages[first];
  if ( npages < p->count )
    {
      diff = p->count - npages;
      p->count = npages;
      first += npages + 1;
      last = first + diff;
      for ( i = first; i < last; i++ )
	{
	  p = &alloc->pages[i];
	  AO_CLEAR(&p->lock);
	}
    }
}

struct dspd_rtalloc *dspd_rtalloc_new(size_t npages, size_t pagesize)
{
  size_t slen, pgbuf, data, total, i;
  struct dspd_rtalloc *a;
  char *buf;
  slen = realsize(sizeof(*a));
  pgbuf = realsize(sizeof(struct pageinfo) * npages);
  data = npages * pagesize;
  total = data + pgbuf + slen;
  buf = calloc(1, total);

  if ( buf )
    {
      a = (struct dspd_rtalloc*)buf;
      a->pages = (struct pageinfo*)&buf[slen];
      a->membase = &buf[pgbuf+slen];
      a->pagecount = npages;
      a->pagesize = pagesize;
      a->boundary = (size_t)a->membase + (npages * pagesize);
      for ( i = 0; i < a->pagecount; i++ )
	AO_CLEAR(&a->pages[i].lock);
    } else
    {
      a = NULL;
    }
  return a;
}

void dspd_rtalloc_delete(struct dspd_rtalloc *alloc)
{
  free(alloc);
}

static void allocator_unwind(struct pageinfo *pglist, size_t count)
{
  size_t i;
  for ( i = 0; i < count; i++ )
    AO_CLEAR(&pglist[i].lock);
}


/*
  FIXME: The code below works but it isn't very good.  Need to continue
  searching after the last known used fragment.  The current method
  may rarely be useful if some buffer space is cleared.
*/
void *dspd_rtalloc_getpages(struct dspd_rtalloc *alloc, size_t npages)
{
  size_t i, j, count, last;
  struct pageinfo *pg;
  void *ret = NULL;
  for ( i = 0; i < alloc->pagecount; i++ )
    {

      //Count all of the free pages
      last = i + npages;
      if ( last >= alloc->pagecount )
	break; //Not enough pages
      for ( j = i; j < last; j++ )
	{
	  pg = &alloc->pages[j];
	  if ( pg->lock == AO_TS_SET )
	    break;
	}
      count = j - i;

      //Have enough pages been found?
      if ( count == npages )
	{
	  //Try allocating all of the pages that were found
	  for ( j = i; j < last; j++ )
	    {
	      pg = &alloc->pages[j];
	      if ( pg->lock == AO_TS_SET || AO_test_and_set(&pg->lock) == AO_TS_SET )
		{
		  allocator_unwind(&alloc->pages[i], j - i);
		  break;
		} else
		{
		  AO_short_store(&pg->count, 0);
		}
	    }
	  count = j - i;
	  if ( count == npages )
	    {
	      alloc->pages[i].count = npages - 1;
	      ret = &alloc->membase[i*alloc->pagesize];
	      break;
	    }
	} else
	{
	  i = j;
	}
    }
 
  return ret;
}

void *dspd_rtalloc_malloc(struct dspd_rtalloc *alloc, size_t len)
{
  size_t npages;
  void *ret;
  npages = len / alloc->pagesize;
  if ( len % alloc->pagesize )
    npages++;
  if ( npages > alloc->pagecount )
    {
      ret = malloc(len);
    } else
    {
      ret = dspd_rtalloc_getpages(alloc, npages);
    }
  return ret;
}

void *dspd_rtalloc_calloc(struct dspd_rtalloc *alloc, size_t nmemb, size_t size)
{
  void *ret;
  size_t s = nmemb * size;
  ret = dspd_rtalloc_malloc(alloc, s);
  if ( ret )
    memset(ret, 0, s);
  return ret;
}

void dspd_rtalloc_free(struct dspd_rtalloc *alloc, void *addr)
{
  uintptr_t first, i, count, last;
  struct pageinfo *pg;
  if ( alloc != NULL && ((uintptr_t)addr >= (uintptr_t)alloc->membase &&
			 (uintptr_t)addr < (uintptr_t)alloc->boundary))
    {
      first = ((uintptr_t)addr - (uintptr_t)alloc->membase) / alloc->pagesize;
      pg = &alloc->pages[first];
      count = AO_short_load(&pg->count);
      last = first + 1 + count;
      DSPD_ASSERT(last <= alloc->pagecount);
      AO_CLEAR(&pg->lock);
      for ( i = first + 1; i < last; i++ )
	{
	  DSPD_ASSERT(alloc->pages[i].lock == AO_TS_SET);
	  AO_CLEAR(&alloc->pages[i].lock);
	}
    } else
    {
      free(addr);
    }
}

