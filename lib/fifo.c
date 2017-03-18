/*
 *  FIFO - Generic lock free FIFOs
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

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "fifo.h"
#include "atomic.h"

uint32_t dspd_fifo_peek(const struct dspd_fifo_header *fifo,
			uint32_t offset,
			void **addr)
{
  uint32_t in, out, l, p, n;
  in = dspd_load_uint32(&fifo->data->obj_in);
  out = dspd_load_uint32(&fifo->data->obj_out) + offset;
  l = in - out;
  if ( l > fifo->max_obj )
    return 0;
  p = out % fifo->max_obj;
  n = fifo->max_obj - p;
  if ( n > l )
    n = l;
  *addr = &fifo->data->data[fifo->obj_size*p];
  return n;
}

/*
  Get raw pointers
 */
uint32_t dspd_fifo_optr(const struct dspd_fifo_header *fifo)
{
  return dspd_load_uint32(&fifo->data->obj_out);
}
uint32_t dspd_fifo_iptr(const struct dspd_fifo_header *fifo)
{
  return dspd_load_uint32(&fifo->data->obj_in);
}

/*Reset pointers so fifo has 0 bytes of data*/
void dspd_fifo_reset(struct dspd_fifo_header *fifo)
{
  dspd_store_uint32(&fifo->data->obj_in, 0);
  dspd_store_uint32(&fifo->data->obj_out, 0);
}

/*
  Get objects in buffer and pointers.
*/
int dspd_fifo_len_ptrs(const struct dspd_fifo_header *fifo, 
		       uint32_t *len,
		       uint32_t *in,
		       uint32_t *out)
{
  uint32_t l;
  int ret;
  *in = dspd_load_uint32(&fifo->data->obj_in);
  *out = dspd_load_uint32(&fifo->data->obj_out);
  l = *in - *out;
  if ( l > fifo->max_obj )
    {
      ret = EIO;
    } else
    {
      *len = l;
      ret = 0;
    }
  return ret;
}

/*
  Get length
 */
int dspd_fifo_len(const struct dspd_fifo_header *fifo, 
		  uint32_t *len)
{
  uint32_t i, o;
  return dspd_fifo_len_ptrs(fifo, len, &i, &o);
}

/*
  Get free space in buffer and pointers.
 */
int dspd_fifo_space_ptrs(const struct dspd_fifo_header *fifo, uint32_t *len,
			 uint32_t *in, uint32_t *out)
{
  int ret;
  uint32_t l;
  ret = dspd_fifo_len_ptrs(fifo, &l, in, out);
  if ( ret == 0 )
    *len = fifo->max_obj - l;
  return ret;
}

/*
  Get free space for writing.
*/
int dspd_fifo_space(const struct dspd_fifo_header *fifo, uint32_t *len)
{
  uint32_t i, o;
  return dspd_fifo_space_ptrs(fifo, len, &i, &o);
}

/*
  Commit write operation.
*/
void dspd_fifo_wcommit(struct dspd_fifo_header *fifo, uint32_t len)
{
  uint32_t l;
  l = dspd_load_uint32(&fifo->data->obj_in);
  l += len;
  dspd_store_uint32(&fifo->data->obj_in, l);
}

/*
  Commit read operation.
*/
void dspd_fifo_rcommit(struct dspd_fifo_header *fifo, uint32_t len)
{
  uint32_t l;
  l = dspd_load_uint32(&fifo->data->obj_out);
  l += len;
  dspd_store_uint32(&fifo->data->obj_out, l);
}

//Get whatever chunk is at the current write pointer.
//Gets the whole thing (the caller does not specify the size
//because there is only one contiguous chunk).
int dspd_fifo_wiov(struct dspd_fifo_header *fifo,
		   void **ptr,
		   uint32_t *len)
{
  int ret;
  uint32_t space, o, l, p, in, out;
  ret = dspd_fifo_space_ptrs(fifo, &space, &in, &out);
  if ( ret == 0 )
    {
      if ( space == 0 )
	{
	  *len = 0;
	  *ptr = fifo->data->data;
	} else
	{
	  p = in;
	  o = p % fifo->max_obj;
	  *ptr = &fifo->data->data[o * fifo->obj_size];
	  l = fifo->max_obj - o;
	  if ( l < space )
	    *len = l;
	  else
	    *len = space;
	}
    }
  return ret;
}

int dspd_fifo_wiov_ex(struct dspd_fifo_header *fifo,
		      void **ptr,
		      uint32_t *offset,
		      uint32_t *len)
{
  int ret;
  uint32_t space, o, l, p, in, out;
  ret = dspd_fifo_space_ptrs(fifo, &space, &in, &out);
  if ( ret == 0 )
    {
      if ( space == 0 )
	{
	  *len = 0;
	  *ptr = fifo->data->data;
	} else
	{
	  p = in;
	  o = p % fifo->max_obj;
	  *ptr = fifo->data->data;
	  *offset = o;
	  l = fifo->max_obj - o;
	  if ( l < space )
	    space = l;
	  if ( space < *len )
	    *len = space;
	}
    }
  return ret;
}


//Just like dspd_fifo_wiov(), but for reading.
int dspd_fifo_riov(struct dspd_fifo_header *fifo,
		   void **ptr,
		   uint32_t *len)
{
  int ret;
  uint32_t avail, o, l, p, in, out;
  ret = dspd_fifo_len_ptrs(fifo, &avail, &in, &out);
  if ( ret == 0 )
    {
      if ( avail == 0 )
	{
	  *len = 0;
	  *ptr = fifo->data->data;
	} else
	{
	  p = out;
	  o = p % fifo->max_obj;
	  *ptr = &fifo->data->data[o * fifo->obj_size];
	  l = fifo->max_obj - o;
	  if ( l < avail )
	    *len = l;
	  else
	    *len = avail;
	}
    }
  return ret;
}


int dspd_fifo_riov_ex(struct dspd_fifo_header *fifo,
		      void **ptr,
		      uint32_t *offset,
		      uint32_t *len)
{
  int ret;
  uint32_t avail, o, l, p, in, out;
  ret = dspd_fifo_len_ptrs(fifo, &avail, &in, &out);
  if ( ret == 0 )
    {
      if ( avail == 0 )
	{
	  *len = 0;
	  *ptr = fifo->data->data;
	  *offset = 0;
	} else
	{
	  p = out;
	  o = p % fifo->max_obj;
	  *ptr = fifo->data->data;
	  l = fifo->max_obj - o;
	  if ( l < avail )
	    avail = l;
	  if ( avail < *len )
	    *len = avail;
	  *offset = o;
	}
    }
  return ret;
}

/*
  Calculate the data section size.
*/
uint32_t dspd_fifo_bufsize(uint32_t nmemb, uint32_t size)
{
  return (nmemb*size) + sizeof(struct dspd_fifo_data);
}

/*
  Initialize a fifo header.  If addr is NULL then assume that
  hdr points to a buffer with enough space for the header and
  data sections.
*/
int dspd_fifo_init(struct dspd_fifo_header *hdr, 
		   uint32_t nmemb,
		   uint32_t size,
		   void *addr)
{
  memset(hdr, 0, sizeof(*hdr));
  if ( addr == NULL )
    hdr->data = (struct dspd_fifo_data*)&hdr->bytes[0];
  else
    hdr->data = addr;
  hdr->max_obj = nmemb;
  hdr->obj_size = size;
  hdr->flags |= DSPD_FIFO_FLAG_INIT;
  hdr->bufsize = hdr->max_obj * hdr->obj_size;
  memset(&hdr->data[0], 0, dspd_fifo_bufsize(nmemb, size));
  return 0;
}

/*
  Mark fifo as uninitialized.
 */
void dspd_fifo_destroy(struct dspd_fifo_header *hdr)
{
  if ( hdr->flags & DSPD_FIFO_FLAG_INIT )
    {
      hdr->flags = 0;
      hdr->max_obj = 0;
      hdr->obj_size = 0;
      hdr->data = NULL;
    }
}


size_t dspd_fifo_size(uint32_t nmemb, uint32_t size)
{
  return dspd_fifo_bufsize(nmemb, size);
}

/*
  Allocate a fifo and initialize it.  Optionally use external buffer
  as data section.
*/
int dspd_fifo_new(struct dspd_fifo_header **fifo,
		  uint32_t nmemb,
		  uint32_t size,
		  void *addr)
{
  struct dspd_fifo_header *hdr = NULL;
  int err = 0;
  uintptr_t s;
  char *a;
  if ( addr == NULL )
    s = dspd_fifo_bufsize(nmemb, size);
  else
    s = 0;
  a = malloc(sizeof(*hdr)+s);
  if ( ! a )
    {
      err = errno;
      goto out;
    }
  hdr = (struct dspd_fifo_header*)a;
  err = dspd_fifo_init(hdr, nmemb, size, addr);
  if ( err )
    goto out;
  *fifo = hdr;
  return 0;

 out:
  free(hdr);
  return err;
}

/*
  Destroy and free a fifo buffer.
 */
void dspd_fifo_delete(struct dspd_fifo_header *fifo)
{
  if ( fifo->flags & DSPD_FIFO_FLAG_INIT )
    {
      dspd_fifo_destroy(fifo);
      free(fifo);
    }
}

/*
  Write len objects at addr to fifo.
*/
int32_t dspd_fifo_write(struct dspd_fifo_header *fifo,
			const void *addr,
			uint32_t len)
{
  uint32_t l, n;
  int32_t written = 0;
  void *ptr;
  int32_t ret;
  ret = dspd_fifo_wiov(fifo, &ptr, &l);
  if ( ret == 0 )
    {
      if ( l > len )
	l = len;
      written += l;
      memcpy(ptr, addr, l*fifo->obj_size);
      dspd_fifo_wcommit(fifo, l);
      if ( written < len )
	{
	  ret = dspd_fifo_wiov(fifo, &ptr, &l);
	  if ( ret == 0 )
	    {
	      n = len - written;
	      if ( l > n )
		l = n;
	      memcpy(ptr, addr+(written*fifo->obj_size), l*fifo->obj_size);
	      written += l;
	      dspd_fifo_wcommit(fifo, l);
	    }
	}
    }
  if ( written > 0 )
    return written;
  return ret;
}


/*
  Copy len objects to addr.
*/
int32_t dspd_fifo_read(struct dspd_fifo_header *fifo,
		       void *addr,
		       uint32_t len)
{
  uint32_t l, n;
  int32_t rd = 0;
  void *ptr;
  int32_t ret;
  ret = dspd_fifo_riov(fifo, &ptr, &l);
  if ( ret == 0 )
    {
      if ( l > len )
	l = len;
      rd += l;
      memcpy(addr, ptr, l*fifo->obj_size);
      dspd_fifo_rcommit(fifo, l);
      if ( rd < len )
	{
	  ret = dspd_fifo_riov(fifo, &ptr, &l);
	  if ( ret == 0 )
	    {
	      n = len - rd;
	      if ( l > n )
		l = n;
	      memcpy(addr+(rd*fifo->obj_size), ptr, l*fifo->obj_size);
	      rd += l;
	      dspd_fifo_rcommit(fifo, l);
	    }
	}
    }
  if ( rd > 0 )
    return rd;
  return ret;
}

int32_t dspd_fifo_get_error(const struct dspd_fifo_header *fifo)
{
  return (int32_t)dspd_load_uint32(&fifo->data->error);
}

void dspd_fifo_set_error(const struct dspd_fifo_header *fifo, int32_t error)
{
  if ( fifo->data )
    dspd_store_uint32(&fifo->data->error, error);
}

