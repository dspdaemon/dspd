/*
 *  MBX - Memory Buffer eXchange  (lock free atomic memory blocks)
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include "mbx.h"

/*
  Calculate the size of a MBX data buffer.
*/
uint32_t dspd_mbx_bufsize(uint32_t blocksize)
{
  return (blocksize * DSPD_MBX_BLOCKS) + sizeof(struct dspd_mbx_data);
}


/*
  Initialize an MBX header and possibly a data section.  The data
  section may be either contiguous with the header or located
  elsewhere specified by addr.
*/
int dspd_mbx_init(struct dspd_mbx_header *mbx, 
		  uint32_t blocksize,
		  void *addr)
{
  int i;
  memset(mbx, 0, sizeof(*mbx));
  if ( addr == NULL ) //Assume inline data
    mbx->data = (struct dspd_mbx_data*)&mbx->ibytes[0];
  else
    mbx->data = addr;
  mbx->blocksize = blocksize;
  assert(mbx->blocksize);
  mbx->flags |= DSPD_MBX_FLAG_INIT;
  for ( i = 0; i < DSPD_MBX_BLOCKS; i++ )
    dspd_ts_clear(&mbx->data->locks[i].lock);
  assert(mbx->blocksize);
  dspd_mbx_reset(mbx);
  assert(mbx->blocksize);
  return 0;
}

/*
  Destroy mbx header.
 */
void dspd_mbx_destroy(struct dspd_mbx_header *mbx)
{
  if ( mbx->flags & DSPD_MBX_FLAG_INIT )
    {
      mbx->flags = 0;
      mbx->blocksize = 0;
      mbx->data = NULL;
      mbx->flags = 0;
    }
}

/*
  Allocate a new mbx and initialize it.  A data section may be specified,
  otherwise a new one will be allocated.
*/
int dspd_mbx_new(struct dspd_mbx_header **mbx,
		 uint32_t blocksize, 
		 void *addr)
{
  struct dspd_mbx_header *hdr = NULL;
  int err = 0;
  uintptr_t s;
  if ( addr == NULL )
    s = dspd_mbx_bufsize(blocksize);
  else
    s = 0;
  hdr = malloc(sizeof(*hdr)+s);
  if ( ! hdr )
    {
      err = -errno;
      goto out;
    }
  err = dspd_mbx_init(hdr, blocksize, addr);
  if ( err )
    goto out;
  *mbx = hdr;
  
 out:
  if ( err )
    free(hdr);
  return err;
}

/*
  Free mbx.
*/
void dspd_mbx_delete(struct dspd_mbx_header *mbx)
{
  if ( mbx != NULL )
    {
      if ( mbx->flags & DSPD_MBX_FLAG_INIT )
	{
	  dspd_mbx_destroy(mbx);
	  free(mbx);
	}
    }
}

/*
  Lock mbx for reading.  Return value is address of the memory
  block.  The lock argument should normally be 1 (true) otherwise
  the buffer may be overwritten while it is being read.  The reason
  this option exists is because the writer may need to read its own
  memory blocks.
*/
void *dspd_mbx_acquire_read(struct dspd_mbx_header *mbx, int lock)
{
  int32_t idx, i;
  void *addr = NULL;
  for ( i = 0; i < DSPD_MBX_BLOCKS; i++ )
    {
      idx = dspd_load_uint32((uint32_t*)&mbx->data->index);
      if ( idx >= 0 )
	{
	  idx %= 3; //Prevent out of bounds access
	  if ( ! lock )
	    {
	      addr = &mbx->data->data[mbx->blocksize * idx];
	    } else if ( dspd_test_and_set(&mbx->data->locks[idx].lock) != DSPD_TS_SET )
	    {
	      addr = &mbx->data->data[mbx->blocksize * idx];
	      break;
	    }
	}
    }
  return addr;
}

/*
  Release read buffer.  The 2nd argument (ptr) is the address
  returned by dspd_mbx_acquire_read().
*/

void dspd_mbx_release_read(struct dspd_mbx_header *mbx, void *ptr)
{
  uintptr_t idx = ((uintptr_t)ptr - (uintptr_t)mbx->data) / mbx->blocksize;
  dspd_ts_clear(&mbx->data->locks[idx].lock);
}

/*
  Lock buffer for writing.
*/
void *dspd_mbx_acquire_write(struct dspd_mbx_header *mbx)
{
  int32_t idx, i;
  void *addr = NULL;
  idx = (int32_t)dspd_load_uint32((uint32_t*)&mbx->data->index);
  if ( idx < -1 ) //Prevent reading out of bounds
    idx = 0;
  for ( i = 0; i < (DSPD_MBX_BLOCKS*2); i++ )
    {
      idx++; 
      idx %= DSPD_MBX_BLOCKS;

      //Non-atomic read if possible
      if ( dspd_ts_read(&mbx->data->locks[idx].lock) != DSPD_TS_SET )
	{
	  //Seems ok, so try test and set
	  if ( dspd_test_and_set(&mbx->data->locks[idx].lock) != DSPD_TS_SET )
	    {
	      addr = &mbx->data->data[mbx->blocksize*idx];
	      break;
	    }
	}
    }
  return addr;
}

/*
  Release buffer locked for writing.
 */
void dspd_mbx_release_write(struct dspd_mbx_header *mbx, void *ptr)
{
  uintptr_t idx = ((uintptr_t)ptr - (uintptr_t)mbx->data) / mbx->blocksize;
  dspd_ts_clear(&mbx->data->locks[idx].lock);
  dspd_store_uint32((uint32_t*)&mbx->data->index, (uint32_t)idx);
}

/*
  Mark mbx as having no data available for reading.
*/
void dspd_mbx_reset(struct dspd_mbx_header *mbx)
{
  size_t complete, tries = 0, i;
  struct dspd_mbx_data *d = mbx->data;
  void *addr;
  do {
    if ( tries > 2 )
      usleep(1);
    complete = 1;
    dspd_store_uint32((uint32_t*)&mbx->data->index, (uint32_t)-1);
    for ( i = 0; i < DSPD_MBX_BLOCKS; i++ )
      {
	
	if ( dspd_test_and_set(&d->locks[i].lock) != DSPD_TS_SET )
	  {
	    addr = &mbx->data->data[mbx->blocksize*i];
	    memset(addr, 0, mbx->blocksize);
	    dspd_ts_clear(&d->locks[i].lock);
	  } else
	  {
	    complete = 0;
	    tries++;
	  }
      }
  } while ( complete == 0 );
}
