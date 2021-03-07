/*
 *  MBX - Memory Buffer eXchange  (lock free atomic memory blocks)
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include "mbx.h"
#include "util.h"

#define SL_BUSY 1U

bool dspd_seqlock32_read_begin(const struct dspd_seqlock32 *lock, uint64_t *context)
{
  bool ret = false;
  uint32_t seq = dspd_load_uint32(&lock->seq);
  uint32_t ovl;
  if ( ! (seq & SL_BUSY) )
    {
      ovl = dspd_load_uint32(&lock->overflow);
      if ( dspd_load_uint32(&lock->seq) == seq )
	{
	  if ( dspd_load_uint32(&lock->overflow) == ovl )
	    {
	      (*context) = ovl;
	      (*context) <<= 32U;
	      (*context) |= seq;
	      ret = true;
	    }
	}
    }
  return ret;
}

bool dspd_seqlock32_read_complete(const struct dspd_seqlock32 *lock, uint64_t context)
{
  uint64_t ctx;
  bool ret = dspd_seqlock32_read_begin(lock, &ctx);
  if ( ret == true )
    ret = context == ctx;
  return ret;
}

void dspd_seqlock32_write_lock(struct dspd_seqlock32 *lock)
{
  uint32_t seq = dspd_load_uint32(&lock->seq);
  seq++;
  if ( ! (seq & SL_BUSY) )
    seq++; //the lock might be shared and the other side might have corrupted the memory
  dspd_store_uint32(&lock->seq, seq);
}

void dspd_seqlock32_write_unlock(struct dspd_seqlock32 *lock)
{
  uint32_t seq = dspd_load_uint32(&lock->seq), o;
  seq++;
  if ( seq & SL_BUSY )
    seq++; //fix corruption
  if ( seq == 0U )
    {
      o = dspd_load_uint32(&lock->overflow);
      o++;
      dspd_store_uint32(&lock->overflow, o);
    }
  dspd_store_uint32(&lock->seq, seq);
}

void dspd_seqlock32_init(struct dspd_seqlock32 *lock)
{
  dspd_store_uint32(&lock->seq, UINT32_MAX);
  dspd_store_uint32(&lock->overflow, 0U);
  dspd_store_uint32(&lock->seq, 0U);
}

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
  DSPD_ASSERT(mbx->blocksize);
  mbx->flags |= DSPD_MBX_FLAG_INIT;
  for ( i = 0; i < DSPD_MBX_BLOCKS; i++ )
    dspd_seqlock32_init(&mbx->data->locks[i]);
  DSPD_ASSERT(mbx->blocksize);
  dspd_mbx_reset(mbx);
  DSPD_ASSERT(mbx->blocksize);
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
  hdr = calloc(1UL, sizeof(*hdr)+s);
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
  Lock buffer for writing.
*/
void *dspd_mbx_write_lock(struct dspd_mbx_header *mbx, int32_t *idx)
{
  int32_t i;
  i = (int32_t)dspd_load_uint32((uint32_t*)&mbx->data->index);
  if ( i < -1 ) //Prevent reading out of bounds
    i = 0;
  else
    i = (unsigned)(i + 1) % DSPD_MBX_BLOCKS;
  *idx = i;
  dspd_seqlock32_write_lock(&mbx->data->locks[i]);
  return &mbx->data->data[i * mbx->blocksize];
}

/*
  Release buffer locked for writing.
 */
void dspd_mbx_write_unlock(struct dspd_mbx_header *mbx, int32_t idx)
{
  dspd_seqlock32_write_unlock(&mbx->data->locks[idx]);
  dspd_store_uint32((uint32_t*)&mbx->data->index, (uint32_t)idx);
}

void *dspd_mbx_read(struct dspd_mbx_header *mbx, void *buf, size_t len)
{
  int32_t idx;
  uint64_t ctx;
  void *ret = NULL;
  if ( len > 0UL )
    {
      if ( len > mbx->blocksize )
	len = mbx->blocksize;
      while ( (idx = dspd_load_uint32((uint32_t*)&mbx->data->index)) >= 0 )
	{
	  idx %= DSPD_MBX_BLOCKS;
	  if ( dspd_seqlock32_read_begin(&mbx->data->locks[idx], &ctx) )
	    {
	      memcpy(buf, &mbx->data->data[idx * mbx->blocksize], len);
	      if ( dspd_seqlock32_read_complete(&mbx->data->locks[idx], ctx) )
		{
		  ret = buf;
		  break;
		}
	    }
	}
    }
  return ret;
}


/*
  Mark mbx as having no data available for reading.
*/
void dspd_mbx_reset(struct dspd_mbx_header *mbx)
{
  dspd_store_uint32((uint32_t*)&mbx->data->index, (uint32_t)-1);
}
