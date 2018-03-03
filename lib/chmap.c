/*
 *  CHMAP - Channel maps
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
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "sslib.h"



/*
  Find a named position
 */
int32_t dspd_chmap_index(const struct dspd_chmap *map, 
			 uint32_t pos)
{
  uint8_t i;
  int32_t ret = -1;
  for ( i = 0; i < map->channels; i++ )
    {
      if ( map->pos[i] == pos )
	{
	  ret = i;
	  break;
	}
    }
  return ret;
}

/*
  Generate a map to convert from input to output.
  It is technically possible to mix dynamically from two
  different channel maps if they are compatible but there
  is really no need to do it since the output generated here
  won't often change.
*/
bool dspd_chmap_getconv(const struct dspd_chmap *from,
			const struct dspd_chmap *to,
			struct dspd_chmap *cmap)
{
  uint8_t i;
  bool ret = true;
  int32_t pos;
  cmap->channels = from->channels;
  for ( i = 0; i < from->channels; i++ )
    {
      pos = dspd_chmap_index(to, from->pos[i]);
      if ( pos < 0 )
	{
	  ret = false;
	  break;
	}
      /*
	The input (pos[i]) actually goes to pos.
	
	That means:
	output[cmap->pos[i]] += input[i];
	

       */
      cmap->pos[i] = pos;
    }
  

  return ret;
}

void dspd_chmap_getdefault(struct dspd_chmap *map, unsigned int channels)
{
  unsigned int i;
  if ( channels == 1 )
    {
      map->channels = 1;
      map->pos[0] = DSPD_CHMAP_MONO;
    } else
    {
      for ( i = 0; i < channels; i++ )
	map->pos[i] = i + DSPD_CHMAP_FL;
      map->channels = channels;
    }
}

void dspd_chmap_add_route(struct dspd_chmap *map, uint32_t in, uint32_t out)
{
  size_t offset;
  if ( map->stream & DSPD_CHMAP_MULTI )
    {
      offset = map->channels * 2;
      map->pos[offset] = in;
      map->pos[offset+1] = out;
      map->channels++;
    } else
    {
      map->pos[in] = out;
    }
}

/*
  Get a generic map from a device map.  The client should set
  climap->channels to whatever it wants and allocate enough
  space to hold all of devmap.  The result may have a different
  number of channels, but it will still correspond to whatever
  was requested.  This is because a mono input may need to be
  copied to multiple outputs.
  
  The results are undefined if the caller does not initialize climap to all 0,
  except for climap->channels.

*/
int dspd_chmap_create_generic(const struct dspd_chmap *devmap, 
			      struct dspd_chmap *climap)

{
  
  unsigned int channels = climap->channels, i, ret = 0;
  int32_t idx, r, l;
  if ( channels == 1 )
    {
      if ( devmap->channels > channels )
	{
	  climap->stream |= DSPD_CHMAP_MULTI;
	  climap->channels = 0;
	  if ( (idx = dspd_chmap_index(devmap, DSPD_CHMAP_FL)) >= 0 )
	    dspd_chmap_add_route(climap, 0, idx);
	  if ( (idx = dspd_chmap_index(devmap, DSPD_CHMAP_FR)) >= 0 )
	    dspd_chmap_add_route(climap, 0, idx);
	  if ( (idx = dspd_chmap_index(devmap, DSPD_CHMAP_FC)) >= 0 )
	    dspd_chmap_add_route(climap, 0, idx);
	  if ( climap->channels == 0 )
	    {
	      if ( (idx = dspd_chmap_index(devmap, DSPD_CHMAP_MONO)) >= 0 )
		dspd_chmap_add_route(climap, 0, idx);
	    }
	  if ( climap->channels == 0 )
	    {
	      //Don't know what to do, so copy to each channel
	      for ( i = 0; i < devmap->channels; i++ )
		dspd_chmap_add_route(climap, 0, i);
	    }
	  if ( devmap->stream & DSPD_PCM_SBIT_CAPTURE )
	    {
	      for ( i = 0; i < (climap->channels * 2); i += 2 )
		{
		  unsigned int n = climap->pos[i];
		  climap->pos[i] = climap->pos[i+1];
		  climap->pos[i+1] = n;
		}
	    }
	} else
	{
	  //Only one channel (mono output) so copy to that one.
	  climap->pos[0] = 0;
	  climap->stream &= ~DSPD_CHMAP_MULTI;
	}
    } else if ( channels == 2 )
    {
      climap->stream &= ~DSPD_CHMAP_MULTI;
      if ( devmap->channels < channels )
	{
	  climap->pos[0] = 0;
	  climap->pos[1] = 0;
	} else
	{
	  r = dspd_chmap_index(devmap, DSPD_CHMAP_FL);
	  l = dspd_chmap_index(devmap, DSPD_CHMAP_FR);
	  if ( r >= 0 && l >= 0 )
	    {
	      climap->pos[0] = r;
	      climap->pos[1] = l;
	    } else
	    {
	      climap->pos[0] = devmap->pos[0];
	      climap->pos[1] = devmap->pos[1];
	    }
	}
    } else if ( devmap->channels >= channels )
    {
      climap->stream &= ~DSPD_CHMAP_MULTI;
      for ( i = 0; i < channels; i++ )
	climap->pos[i] = devmap->pos[i];
    } else
    {
      ret = -EDOM;
    }
  return ret;
}

static bool dspd_chmap_test_multi(const struct dspd_chmap *srv,
				  const struct dspd_chmap *cli,
				  uint32_t actual_channels)

{
  size_t i, n = cli->channels * 2;
  uint8_t in, out;
  bool ret = true;
  if ( srv->stream & DSPD_PCM_SBIT_CAPTURE )
    {
      for ( i = 0; i < n; i += 2 )
	{
	  in = cli->pos[i];
	  out = cli->pos[i+1];
	  if ( out >= actual_channels ||
	       in >= srv->channels )
	    {
	      ret = false;
	      break;
	    }
	}
    } else
    {
      for ( i = 0; i < n; i += 2 )
	{
	  in = cli->pos[i];
	  out = cli->pos[i+1];
	  if ( in >= actual_channels ||
	       out >= srv->channels )
	    {
	      ret = false;
	      break;
	    }
	}
    }
  return ret;
}

bool dspd_chmap_test(const struct dspd_chmap *srv,
		     const struct dspd_chmap *cli,
		     uint32_t actual_channels)
{
  size_t i;
  bool ret = true;
  if ( (srv->stream & DSPD_CHMAP_MULTI) ||
       srv->channels > DSPD_CHMAP_LAST ||
       cli->channels > DSPD_CHMAP_LAST ||
       srv->channels == 0 ||
       cli->channels == 0 ||
       ((cli->stream & DSPD_CHMAP_MULTI) == 0 && actual_channels < cli->channels))
    {
      ret = false;
    } else if ( cli->stream & DSPD_CHMAP_MULTI )
    {
      ret = dspd_chmap_test_multi(srv, cli, actual_channels);
    } else
    {
      for ( i = 0; i < cli->channels; i++ )
	{
	  if ( cli->pos[i] >= srv->channels )
	    {
	      ret = false;
	      break;
	    }
	}
    }
  return ret;
  
}

void dspd_chmap_dump(const struct dspd_chmap *map)
{
  unsigned int i;
  if ( map->stream & DSPD_CHMAP_MULTI )
    {
      for ( i = 0; i < map->channels * 2; i += 2 )
	{
	  fprintf(stderr, "CH[%d]: %d=>%d\n", i / 2, (int)map->pos[i], (int)map->pos[i+1]);
	}
    } else
    {
       for ( i = 0; i < map->channels; i++ )
	 fprintf(stderr, "CH[%d]: %d\n", i, (int)map->pos[i]);
    }
}



size_t dspd_chmap_bufsize(uint8_t channels, uint8_t stream)
{
  size_t len;
  if ( channels > DSPD_CHMAP_LAST )
    {
      len = 0; //Bad channel map
    } else
    {
      len = sizeof(struct dspd_chmap);
      if ( stream & DSPD_CHMAP_MULTI )
	channels *= 2;
      len += channels;
    }
  return len;
}

size_t dspd_chmap_sizeof(const struct dspd_chmap *map)
{
  return dspd_chmap_bufsize(map->channels, map->stream);
}

size_t dspd_fchmap_sizeof(const struct dspd_fchmap *map)
{
  return dspd_chmap_sizeof(&map->map);
}

int32_t dspd_fchmap_parse(const char *str, struct dspd_fchmap *map)
{
  char buf[16];
  const char *saveptr, *tok;
  size_t len, pos = 0;
  int32_t ret = 0;
  char *p;
  uint8_t src, dest;
  bool have_multi = false;
  memset(map, 0, sizeof(*map));
  for ( tok = dspd_strtok_c(str, ",", &saveptr, &len); 
	tok; 
	tok = dspd_strtok_c(NULL, ",", &saveptr, &len) )
    {
     
      if ( pos == DSPD_CHMAP_LAST )
	{
	  ret = -ECHRNG;
	  break;
	}
      
      if ( len >= sizeof(buf) )
	{
	  ret = -E2BIG;
	  break;
	}
      memcpy(buf, tok, len);
      buf[len] = 0;

      p = strchr(buf, '=');
      if ( ! p )
	{
	  //The format is 'A,B,C...'
	  if ( have_multi )
	    {
	      ret = -EINVAL;
	      break;
	    }
	  ret = dspd_strtou8(buf, &src, 0);
	  if ( ret < 0 )
	    break;
	  map->map.pos[pos] = src;
	} else
	{
	  //The format is 'A=B,C=D,...'
	  if ( pos > 0 && have_multi == false )
	    {
	      ret = -EINVAL;
	      break;
	    }
	  have_multi = true;

	  *p = 0;
	  p++;
	  ret = dspd_strtou8(buf, &src, 0);
	  if ( ret < 0 )
	    break;
	  ret = dspd_strtou8(p, &dest, 0);
	  if ( ret < 0 )
	    break;

	  map->map.pos[pos*2] = src;
	  map->map.pos[(pos*2)+1UL] = dest;
	}
      pos++;
    }
  map->map.channels = pos;
  if ( have_multi )
    map->map.stream |= DSPD_CHMAP_MULTI;
  return ret;
}
