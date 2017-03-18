/*
 *  WQ - Work queue
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
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include "wq.h"

int dspd_wq_new(struct dspd_wq **wq)
{
  struct dspd_wq *w = calloc(1, sizeof(struct dspd_wq));
  int e;
  if ( ! w )
    return -errno;
  if ( pipe(w->pipe) < 0 )
    {
      e = errno;
      free(w);
      return -e;
    }
  *wq = w;
  return 0;
}

void dspd_wq_delete(struct dspd_wq *wq)
{
  close(wq->pipe[0]);
  close(wq->pipe[1]);
  free(wq);
}

bool dspd_wq_process(struct dspd_wq *wq)
{
  ssize_t ret;
  int e;
  bool result = true;
  size_t offset, len;
  char *p;
  ret = read(wq->pipe[0], &wq->buf, sizeof(wq->buf.item));
  if ( ret <= 0 )
    {
      e = errno;
      if ( (ret == 0) || (ret < 0 && e != EAGAIN && e != EINTR && e != EWOULDBLOCK) )
	result = false;
    } else if ( ret < sizeof(wq->buf.item.len) )
    {
      result = false;
    } else if ( ret < wq->buf.item.len )
    {
      offset = ret;
      p = (char*)&wq->buf;
      while ( offset < wq->buf.item.len )
	{
	  ret = read(wq->pipe[1], &p[offset], wq->buf.item.len - offset);
	  if ( ret <= 0 )
	    {
	      e = errno;
	      if ( (ret == 0) || (ret < 0 && e != EAGAIN && e != EINTR && e != EWOULDBLOCK) )
		{
		  result = false;
		  break;
		}
	    } else
	    {
	      offset += ret;
	    }
	}
    }
  if ( result && wq->buf.item.callback )
    {
      len = wq->buf.item.len - sizeof(wq->buf.item);
      if ( len == 0 )
	p = NULL;
      else
	p = wq->buf.data;
      result = wq->buf.item.callback(wq->buf.item.arg, p, len);
    }
  return result;
}

bool dspd_queue_work(struct dspd_wq *wq, const struct dspd_wq_item *item)
{
  ssize_t ret;
  bool result = true;
  int e;
  assert(item->len >= sizeof(*item) && item->len <= DSPD_WQ_ITEM_MAX_DATA);
  while ( (ret = write(wq->pipe[1], item, item->len)) != item->len )
    {
      e = errno;
      assert(ret <= 0);
      if ( (ret == 0) ||
	   (ret < 0 && e != EINTR && e != EAGAIN && e != EWOULDBLOCK) )
	{
	  result = false;
	  break;
	}
    }
  return result;
}
