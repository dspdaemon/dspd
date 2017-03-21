
/*
 *  SYNCGROUP - Synchronization groups
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






#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <assert.h>
#include "sslib.h"
#include "daemon.h"
#include "syncgroup.h"


struct dspd_syncgroup {
  uint32_t sgid;
  volatile uintptr_t refcnt;
  uint32_t streams;
  dspd_mutex_t lock;
  uint8_t  mask[DSPD_MAX_OBJECTS/8];
};

struct dspd_sglist {
  uint32_t          last_id;
  dspd_rwlock_t     lock;
  struct dspd_syncgroup *spare;
  struct dspd_syncgroup *groups[DSPD_MAX_OBJECTS];
};
static void dspd_sg_delete(struct dspd_syncgroup *sg);
static int32_t sg_new(struct dspd_syncgroup **sg);
void dspd_sglist_delete(struct dspd_sglist *sgl)
{
  size_t i;
  for ( i = 0; i < ARRAY_SIZE(sgl->groups); i++ )
    {
      if ( sgl->groups[i] )
	dspd_sg_delete(sgl->groups[i]);
    }
  if ( sgl->spare )
    dspd_sg_delete(sgl->spare);
  dspd_rwlock_destroy(&sgl->lock);
  free(sgl);
}

int32_t dspd_sglist_new(struct dspd_sglist **sgl)
{
  struct dspd_sglist *sglp;
  int ret;
  sglp = calloc(1, sizeof(*sglp));
  if ( sglp )
    {
      ret = dspd_rwlock_init(&sglp->lock, NULL);
      if ( ret == 0 )
	ret = sg_new(&sglp->spare);
      if ( ret )
	dspd_sglist_delete(sglp);
      else
	*sgl = sglp;
    } else
    {
      ret = -ENOMEM;
    }
  return ret;
}

static struct dspd_syncgroup *sg_find(struct dspd_sglist *sgl, uint32_t sgid)
{
  uint32_t idx = sgid & 0xFF;
  struct dspd_syncgroup *ret = NULL;

  if ( sgl->groups[idx] )
    if ( sgl->groups[idx]->sgid == sgid )
      ret = sgl->groups[idx];
  return ret;
}

static uint32_t sg_newid(struct dspd_sglist *sgl)
{
  size_t i;
  uint32_t ret = 0;
  for ( i = 0; i < ARRAY_SIZE(sgl->groups); i++ )
    {
      if ( sgl->groups[i] == NULL )
	{
	  sgl->last_id++;
	  sgl->last_id %= UINT24_MAX;
	  if ( sgl->last_id == 0 )
	    sgl->last_id++;
	  ret = sgl->last_id << 8;
	  ret |= i;
	  break;
	}
    }
  return ret;
}

static void dspd_sg_delete(struct dspd_syncgroup *sg)
{
  dspd_mutex_destroy(&sg->lock);
  free(sg);
}

static int32_t sg_new(struct dspd_syncgroup **sg)
{
  struct dspd_syncgroup *sgp;
  int32_t ret = ENOMEM;
  sgp = calloc(1, sizeof(*sgp));
  if ( sgp )
    {
      ret = dspd_mutex_init(&sgp->lock, NULL);
      if ( ret )
	free(sgp);
      else
	*sg = sgp;
    }
  return ret;
}


int32_t dspd_sg_new(struct dspd_sglist *sgl, struct dspd_syncgroup **sg, uint32_t streams)
{
  int32_t ret = -EMFILE;
  struct dspd_syncgroup *sgp;
  uint32_t sgid;
  
  if ( sgl->spare )
    {
      sgp = NULL;
    } else
    {
      ret = sg_new(&sgp);
      if ( ret )
	return -ret;
    }

  dspd_rwlock_wrlock(&sgl->lock);
  sgid = sg_newid(sgl);

  if ( sgid )
    {
      
      if ( sgp == NULL )
	{
	  if ( sgl->spare )
	    {
	      sgp = sgl->spare;
	      sgl->spare = NULL;
	      ret = 0;
	    } else
	    {
	      ret = sg_new(&sgp);
	    }
	  if ( ret == 0 )
	    {
	      sgp->sgid = sgid;
	      sgp->refcnt = 1;
	      sgp->streams = streams;
	      sgl->groups[sgid&0xFFU] = sgp;
	      *sg = sgp;
	    }
	} else
	{
	  sgp->sgid = sgid;
	  sgp->refcnt = 1;
	  sgp->streams = streams;
	  sgl->groups[sgid&0xFFU] = sgp;
	  if ( sgp == sgl->spare )
	    sgl->spare = NULL;
	  *sg = sgp;
	  ret = 0;
	}
    }
  dspd_rwlock_unlock(&sgl->lock);
  if ( ret && sgp )
    dspd_sg_delete(sgp);

  return ret;
}

struct dspd_syncgroup *dspd_sg_get(struct dspd_sglist *sgl, uint32_t sgid)
{
  struct dspd_syncgroup *ret;
  dspd_rwlock_rdlock(&sgl->lock);
  ret = sg_find(sgl, sgid);
  if ( ret )
    {
#ifdef DSPD_HAVE_ATOMIC_INCDEC
      dspd_atomic_inc(&ret->refcnt);
#else
      dspd_mutex_lock(&ret->lock);
      ret->refcnt++;
      dspd_mutex_unlock(&ret->lock);
#endif
    }
  dspd_rwlock_unlock(&sgl->lock);

  return ret;
}

void dspd_sg_put(struct dspd_sglist *sgl, uint32_t sgid)
{
  uintptr_t rc;
  struct dspd_syncgroup *sg;
  dspd_rwlock_wrlock(&sgl->lock);
  sg = sg_find(sgl, sgid);
  if ( sg )
    {
#ifdef DSPD_HAVE_ATOMIC_INCDEC
      rc = dspd_atomic_dec(&sg->refcnt);
#else
      dspd_mutex_lock(&sg->lock);
      sg->refcnt--;
      rc = sg->refcnt;
      dspd_mutex_unlock(&sg->lock);
#endif
      if ( rc == 0 )
	{
	  if ( sgl->spare == NULL )
	    {
	      sg->sgid = 0;
	      sg->streams = 0;
	      memset(sg->mask, 0, sizeof(sg->mask));
	      sgl->spare = sg;
	    } else
	    {
	      dspd_sg_delete(sg);
	    }
	  sgl->groups[sgid & 0xFF] = NULL;
	}
    }
  dspd_rwlock_unlock(&sgl->lock);
}

void dspd_sg_add(struct dspd_syncgroup *sg, uint32_t idx)
{
  dspd_mutex_lock(&sg->lock);
  dspd_set_bit(sg->mask, idx);
  dspd_mutex_unlock(&sg->lock);
}

void dspd_sg_remove(struct dspd_syncgroup *sg, uint32_t idx)
{
  dspd_mutex_lock(&sg->lock);
  dspd_clr_bit(sg->mask, idx);
  dspd_mutex_unlock(&sg->lock);
}

dspd_time_t dspd_sg_start(struct dspd_syncgroup *sg, const uint32_t *streams)
{
  size_t i, br;
  struct dspd_sync_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  dspd_mutex_lock(&sg->lock);
  if ( streams )
    cmd.streams = *streams;
  else
    cmd.streams = sg->streams;
  cmd.cmd = SGCMD_START;
  cmd.tstamp = dspd_get_time();
  for ( i = 0; i < ARRAY_SIZE(sg->mask); i++ )
    {
      if ( dspd_test_bit(sg->mask, i) )
	(void)dspd_stream_ctl(&dspd_dctx,
			      i,
			      DSPD_SCTL_CLIENT_SYNCCMD,
			      &cmd,
			      sizeof(cmd),
			      NULL,
			      0,
			      &br);
      
    }
  dspd_mutex_unlock(&sg->lock);
  return cmd.tstamp;
}

void dspd_sg_stop(struct dspd_syncgroup *sg, const uint32_t *streams)
{
  size_t i, br;
  struct dspd_sync_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  dspd_mutex_lock(&sg->lock);
  if ( streams )
    cmd.streams = *streams;
  else
    cmd.streams = sg->streams;
  cmd.cmd = SGCMD_STOP;
  for ( i = 0; i < ARRAY_SIZE(sg->mask); i++ )
    {
      if ( dspd_test_bit(sg->mask, i) )
	(void)dspd_stream_ctl(&dspd_dctx,
			      i,
			      DSPD_SCTL_CLIENT_SYNCCMD,
			      &cmd,
			      sizeof(cmd),
			      NULL,
			      0,
			      &br);
      
    }
  dspd_mutex_unlock(&sg->lock);
}

uint32_t dspd_sg_id(struct dspd_syncgroup *sg)
{
  return sg->sgid;
}

uint32_t dspd_sg_streams(struct dspd_syncgroup *sg)
{
  return sg->streams;
}
