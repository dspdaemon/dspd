/*
 *  SHM - Shared memory API
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


#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <assert.h>
#include <stdbool.h>
#include "shm.h"
static int dspd_verify_section(const struct dspd_shm_map *map,
			       const struct dspd_shm_section *sect,
			       struct dspd_shm_addr *addr);

//This has one instance per program.  The idea is that only
//one named object gets created so it will only leave one object
//in memory if it crashes.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
//static uid_t shm_user;
#define SHM_NAME "dspd"
#define SHM_ALIGN 16
static int dspd_shm_open(int flags)
{
  char name[33];
  int ret, f = O_EXCL | O_CREAT, e = 0;
  if ( (flags & (DSPD_SHM_FLAG_WRITE | DSPD_SHM_FLAG_READ)) ==
       (DSPD_SHM_FLAG_WRITE | DSPD_SHM_FLAG_READ))
    {
      f |= O_RDWR;
    } else if ( flags & DSPD_SHM_FLAG_WRITE )
    {
      f |= O_WRONLY;
    } else if ( flags & DSPD_SHM_FLAG_READ )
    {
      f |= O_RDONLY;
    }
       
  sprintf(name, "/"SHM_NAME"-%d", getuid());
  pthread_mutex_lock(&lock);
  //Try to create a section that can't be accessed by
  //any external program.
  while ( (ret = shm_open(name, f, 0)) < 0 )
    {
      if ( errno == EEXIST )
	{
	  //Remove it if it fails.  Any other instance
	  //should be ready to do the same thing so it
	  //won't be a problem if something else does it first.
	  if ( shm_unlink(name) < 0 )
	    if ( errno != ENOENT )
	      {
		e = errno;
		break;
	      }
	} else
	{
	  e = errno;
	  break;
	}
    }
  /*
    Remove it so if the program crashes it will be deleted by the kernel.
    Also prevents other programs from accessing it.  If something else
    did open it then it already has enough permissions to attach a debugger
    so there probably isn't anything to worry about.
  */
  if ( ret >= 0 )
    shm_unlink(name);
  pthread_mutex_unlock(&lock);
  if ( ret < 0 )
    {
      errno = e;
      ret = -e;
    }
  return ret;
}

//Returns 0 if arguments are invalid
static size_t calculate_size(const struct dspd_shm_addr *sect,
			     uint32_t nsect)

{
  uint64_t ret = sizeof(struct dspd_shm_header);
  uint32_t i, s;
  const struct dspd_shm_addr *sptr;
  size_t p = sysconf(_SC_PAGESIZE);
  ret += (sizeof(struct dspd_shm_section) * nsect);
  for ( i = 0; i < nsect; i++ )
    {
      sptr = &sect[i];
      s = sptr->length / SHM_ALIGN;
      if ( s % SHM_ALIGN )
	s++;
      ret += (s * SHM_ALIGN);
    }
  if ( ret % p )
    ret = ((ret / p) + 1) * p;
  if ( ret > UINT32_MAX )
    ret = 0;
  return ret;
}

static void assign_sections(struct dspd_shm_header *hdr,
			    const struct dspd_shm_addr *addr,
			    uint32_t naddr)
{
  uint32_t i, offset, l;
  const struct dspd_shm_addr *a;
  struct dspd_shm_section *o;
  offset = sizeof(struct dspd_shm_header) + (naddr * sizeof(struct dspd_shm_section));
  char *buf = (char*)hdr;
  for ( i = 0; i < naddr; i++ )
    {
      a = &addr[i];
      o = &hdr->sections[i];
      o->length = a->length;
      o->offset = offset;
      o->section_id = a->section_id;

      o->reserved = 0;
      if ( a->addr )
	memcpy(&buf[offset], a->addr, a->length);

      l = o->length / SHM_ALIGN;
      if ( o->length % SHM_ALIGN )
	l++;
      offset += (l * SHM_ALIGN);
    }
  // for ( i = 0; i < naddr; i++ )
  //  DSPD_ASSERT(hdr->sections[i].section_id == addr[i].section_id);
}

/*
  Create a private or shared memory section.  A private section
  is normally not visible to other processes because it is allocated
  with malloc() or something similar.

  

*/

int dspd_shm_create(struct dspd_shm_map *map,
		    const struct dspd_shm_addr *sect,
		    uint32_t nsect)
{
  size_t len;
  void *addr = NULL;
  int fd = -1, flags, p, ret;
  len = calculate_size(sect, nsect);
  if ( len == 0 )
    return -EINVAL;
  if ( map->flags & (DSPD_SHM_FLAG_MMAP | DSPD_SHM_FLAG_SVIPC) )
    return -EINVAL; //Can't specify SHM type
  flags = map->flags;
  if ( map->flags & DSPD_SHM_FLAG_PRIVATE )
    {
      addr = calloc(1, len);
      if ( ! addr )
	return errno;
      fd = -1;
      flags = 0;
    } else
    {
      flags |= DSPD_SHM_FLAG_MMAP;
      fd = dspd_shm_open(map->flags);
      if ( fd >= 0 )
	{
	  if ( ftruncate(fd, len) == 0 )
	    {
	      p = 0;
	      if ( map->flags & DSPD_SHM_FLAG_READ )
		p |= PROT_READ;
	      if ( map->flags & DSPD_SHM_FLAG_WRITE )
		p |= PROT_WRITE;
	      addr = mmap(NULL, 
			  len, 
			  p, 
			  MAP_LOCKED | MAP_SHARED,
			  fd,
			  0);
	      if ( addr == (void*)-1L )
		addr = mmap(NULL, 
			    len, 
			    p, 
			    MAP_SHARED,
			    fd,
			    0);
	      if ( addr == (void*)-1L )
		addr = NULL;
	      
	      if ( addr )
		memset(addr, 0, len);
		
	    }
	}
    }
  if ( addr )
    {
      //fprintf(stderr, "SHM OK %ld\n", addr);
      assign_sections(addr, sect, nsect);
      map->addr = addr;
      map->addr->version = 0;
      map->addr->length = len;
      map->addr->section_count = nsect;
      map->arg = fd;
      map->key = 0;
      map->flags |= flags;
      map->length = len;
      map->section_count = nsect;
      //fprintf(stderr, "MAP ADDR %p\n", addr);
      ret = 0;
    } else
    {
      ret = -errno;

      if ( fd >= 0 )
	close(fd);
    }
  return ret;
}

static int dspd_shm_attach_mmap(struct dspd_shm_map *map)
{
  void *addr;
  struct stat fi;
  int ret;
  int p = 0;
  if ( map->flags & DSPD_SHM_FLAG_WRITE )
    p |= PROT_WRITE;
  if ( map->flags & DSPD_SHM_FLAG_READ )
    p |= PROT_READ;
  if ( fstat(map->arg, &fi) == 0 )
    {
      if ( fi.st_size >= map->length )
	{
	  addr = mmap(NULL, 
		      map->length,
		      p,
		      MAP_LOCKED | MAP_SHARED,
		      map->arg,
		      0);
	  if ( addr == (void*)-1L )
	    addr = mmap(NULL, 
			map->length,
			p,
			MAP_SHARED,
			map->arg,
			0);
	  if ( addr != (void*)-1L )
	    {
	      map->addr = addr;
	      ret = 0;
	    } else
	    {
	      ret = -errno;
	    }
	} else
	{
	  ret = -errno;
	}
    } else
    {
      ret = -errno;
    }
  return ret;
}

static int dspd_shm_attach_private(struct dspd_shm_map *map)
{
  if ( ! map->addr )
    return -EINVAL;
  return 0;
}

/*
  Attach to shared memory location.
*/
int dspd_shm_attach(struct dspd_shm_map *map)
{
  int ret = -EINVAL, t;
  uint32_t l;
  if ( map->length <= UINT32_MAX &&
       map->section_count < UINT32_MAX )
    {
      l = (map->section_count * sizeof(struct dspd_shm_section)) + 
	sizeof(struct dspd_shm_header);
      if ( l <= map->length )
	{
	  t = map->flags & (DSPD_SHM_FLAG_MMAP|DSPD_SHM_FLAG_SVIPC|DSPD_SHM_FLAG_PRIVATE);
	  if ( t == DSPD_SHM_FLAG_MMAP )
	    {
	      ret = dspd_shm_attach_mmap(map);
	    } else if ( t == DSPD_SHM_FLAG_PRIVATE )
	    {
	      ret = dspd_shm_attach_private(map);
	    } 
	}
    }
  return ret;
}

/*
  Detach from memory location.
*/
void dspd_shm_close2(struct dspd_shm_map *map, bool unmap)
{
  if ( map->flags & DSPD_SHM_FLAG_MMAP )
    {
      if ( unmap )
	{
	  munmap(map->addr, map->length);
	  map->flags &= ~DSPD_SHM_FLAG_MMAP;
	}
      if ( map->arg >= 0 )
	{
	  close(map->arg);
	  map->arg = -1;
	}
    } else if ( map->flags & DSPD_SHM_FLAG_PRIVATE )
    {
      if ( unmap )
	{
	  map->flags &= ~DSPD_SHM_FLAG_PRIVATE;
	  free(map->addr);
	}
    }
  if ( unmap )
    {
      map->length = 0;
      map->addr = NULL;
    }
}

static int dspd_verify_section(const struct dspd_shm_map *map,
			       const struct dspd_shm_section *sect,
			       struct dspd_shm_addr *addr)
{
  uint32_t minoff, sectlen = sect->length, sectoff = sect->offset;
  uint32_t o = sectoff + sectlen;
  int ret;
  char *p = (char*)map->addr;
  minoff = sizeof(struct dspd_shm_header) + 
    (sizeof(struct dspd_shm_section) * map->section_count);
  if ( sectoff < map->length &&
       sectoff < map->length &&
       sectlen < UINT32_MAX &&
       sectoff < UINT32_MAX &&
       o > minoff )
    {
      if ( addr )
	{
	  addr->length = sectlen;
	  addr->addr = &p[sectoff];
	}
      ret = 0;
    } else
    {
      ret = -EBADFD;
    }
  return ret;
}

/*
  Get the address of a section and do some sanity checks so it 
  won't point outside of the buffer.
*/
int dspd_shm_get_addr(const struct dspd_shm_map *map,
		      struct dspd_shm_addr *addr)
{
  uint32_t i, sectid;
  int ret = -ENOENT;
  const struct dspd_shm_section *sect;
  for ( i = 0; i < map->section_count; i++ )
    {
      sect = &map->addr->sections[i];
      sectid = sect->section_id;
      if ( sectid == addr->section_id )
	{
	  ret = dspd_verify_section(map, sect, addr);
	  if ( ret == 0 )
	    addr->section_id = sectid;
	  break;
	}
    }
  return ret;
}



