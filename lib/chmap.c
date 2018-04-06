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




static ssize_t find_channel(const struct dspd_pcm_chmap *map, uint16_t channel)
{
  size_t i;
  ssize_t ret = -1;
  for ( i = 0; i < map->count; i++ )
    {
      if ( map->pos[i] == channel )
	{
	  ret = i;
	  break;
	}
    }
  return ret;
}
static int32_t stereo_to_mono(const struct dspd_pcm_chmap *in, 
			      const struct dspd_pcm_chmap *out,
			      struct dspd_pcm_chmap *map)
{
  size_t i;
  map->flags = DSPD_CHMAP_MATRIX;
  for ( i = 0; i < in->count; i++ )
    map->pos[i] = 0;
  map->ichan = in->count;
  map->ochan = out->count;
  map->count = map->ichan;
  return 0;
}
static int32_t mono_to_stereo(const struct dspd_pcm_chmap *in, 
			      const struct dspd_pcm_chmap *out,
			      struct dspd_pcm_chmap *map)
{
  ssize_t ch1, ch2;
  size_t i, o;
  (void)in;
  ch1 = find_channel(out, DSPD_CHMAP_FL);
  ch2 = find_channel(out, DSPD_CHMAP_FR);
  if ( ch1 >= 0 && ch2 >= 0 )
    {
      map->pos[0] = 0;
      map->pos[1] = ch1;
      map->pos[2] = 0;
      map->pos[3] = ch2;
      map->count = 4U;
    } else
    {
      for ( i = 0, o = 0; i < out->count; i++ )
	{
	  if ( out->pos[i] > DSPD_CHMAP_MONO )
	    {
	      map->pos[o] = 0;
	      o++;
	      map->pos[o] = i;
	      o++;
	    }
	}
      if ( map->count < 1U )
	return -EINVAL;
      map->count = o;
    }
  map->ochan = out->count;
  map->ichan = 1U;
  map->flags = DSPD_CHMAP_MULTI | DSPD_CHMAP_MATRIX;
  return 0;
}
static int32_t translate_map(const struct dspd_pcm_chmap *in, 
			     const struct dspd_pcm_chmap *out,
			     struct dspd_pcm_chmap *map)
{
  size_t i, o;
  ssize_t ch;
  int32_t ret = 0;
  map->flags = DSPD_CHMAP_MATRIX;

  if ( out->flags & DSPD_PCM_SBIT_CAPTURE )
    {
      for ( i = 0, o = 0; i < out->count && o < map->count; i++ )
	{
	  ch = find_channel(in, out->pos[i]);
	  if ( ch < 0 )
	    {
	      ret = -EINVAL;
	      break;
	    }
	  map->pos[o] = ch;
	  o++;
	  map->pos[o] = i;
	  o++;
	}
      map->count = o;
      map->ichan = in->ichan;
      map->ochan = out->ichan;
      map->flags |= DSPD_CHMAP_MULTI;
    } else
    {
      map->ichan = in->count;
      map->ochan = out->count;
      for ( i = 0; i < in->count; i++ )
	{
	  ch = find_channel(out, in->pos[i]);
	  if ( ch < 0 )
	    {
	      ret = -EINVAL;
	      break;
	    }
	  map->pos[i] = ch;
	}	
      map->count = i;
    }
  return ret;
}

int32_t dspd_pcm_chmap_translate(const struct dspd_pcm_chmap *in, 
				 const struct dspd_pcm_chmap *out,
				 struct dspd_pcm_chmap *map)
{
  int32_t ret = -EINVAL;
  size_t i;
  if ( in == NULL )
    {
      if ( map->count <= out->ichan )
	{
	  for ( i = 0; i < out->ichan; i++ )
	    map->pos[i] = i;
	  map->ichan = map->count;
	  map->ochan = map->count;
	  ret = 0;
	}
    } else if ( (in->flags & DSPD_CHMAP_MATRIX) == 0 )
    {
      if ( in->count > 0 && out->count == 1 && out->pos[0] == DSPD_CHMAP_MONO )
	{
	  ret = stereo_to_mono(in, out, map);
	} else if ( in->count == 1 && out->count > 1 && in->pos[0] == DSPD_CHMAP_MONO )
	{
	  ret = mono_to_stereo(in, out, map);
	} else if ( in->count <= out->count )
	{
	  ret = translate_map(in, out, map);
	}
    }
  return ret;
}

const struct dspd_pcm_chmap *dspd_pcm_chmap_get_default(size_t channels)
{
  static const struct dspd_pcm_chmap_container maps[] = {
    { .map = { .count = 1U, .flags = 0 }, .pos = { 
	DSPD_CHMAP_MONO  
      }
    },
    { .map = { .count = 2U, .flags = 0 }, .pos = { 
	DSPD_CHMAP_FL,  
	DSPD_CHMAP_FR,  
      }
    },
    { .map = { .count = 3U, .flags = 0 }, .pos = { 
	DSPD_CHMAP_FL,  
	DSPD_CHMAP_FR, 
	DSPD_CHMAP_FC, 
      } 
    },
    { .map = { .count = 4U, .flags = 0 }, .pos = { 
	DSPD_CHMAP_FL,  
	DSPD_CHMAP_FR, 
	DSPD_CHMAP_FC, 
	DSPD_CHMAP_LFE, 
      } 
    },
    { .map = { .count = 6U, .flags = 0 }, .pos = { 
	DSPD_CHMAP_FL,  
	DSPD_CHMAP_FR, 
	DSPD_CHMAP_FC, 
	DSPD_CHMAP_LFE, 
	DSPD_CHMAP_RL,  
	DSPD_CHMAP_RR,
      } 
    },
    { .map = { .count = 8U, .flags = 0 }, .pos = { 
	DSPD_CHMAP_FL,  
	DSPD_CHMAP_FR, 
	DSPD_CHMAP_FC, 
	DSPD_CHMAP_LFE, 
	DSPD_CHMAP_RL,  
	DSPD_CHMAP_RR,
	DSPD_CHMAP_SL,  
	DSPD_CHMAP_SR,
      } 
    },
  };
  size_t i;
  const struct dspd_pcm_chmap *ret = NULL;
  for ( i = 0; i < ARRAY_SIZE(maps); i++ )
    {
      if ( maps[i].map.count == channels )
	{
	  ret = &maps[i].map;
	  break;
	}
    }
  return ret;
}


int32_t dspd_pcm_chmap_test_channels(const struct dspd_pcm_chmap *map, size_t channels_in, size_t channels_out)
{
  size_t i, inp, outp;
  int32_t ret = 0;
  if ( map->flags & DSPD_CHMAP_MATRIX )
    {
      if ( (map->count % 2U) == 0 )
	{
	  if ( map->flags & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      if ( channels_out == 0 )
		channels_out = UINTPTR_MAX;
	      if ( channels_out != map->ichan )
		return -EINVAL;
	    } else if ( map->flags & DSPD_PCM_SBIT_CAPTURE )
	    {
	      if ( channels_in == 0 )
		channels_in = UINTPTR_MAX;
	      if ( channels_out != map->ochan )
		return -EINVAL;
	    }
	  for ( i = 0; i < map->count; i += 2UL )
	    {
	      inp = map->pos[i];
	      outp = map->pos[i+1UL];
	      if ( inp >= channels_in || outp >= channels_out )
		{
		  ret = -ECHRNG;
		  break;
		}
	    }
	} else
	{
	  ret = -EINVAL;
	}
    } else
    {
      if ( (map->flags & DSPD_PCM_SBIT_PLAYBACK) && 
	   (channels_in > map->count || (channels_out != 0 && map->count > channels_out)))
	{
	  ret = -EDOM;
	} else if ( (map->flags & DSPD_PCM_SBIT_CAPTURE) && 
		    (channels_out > map->count || (channels_in != 0 && map->count > channels_in)))
	{
	  ret = -EDOM;
	} else
	{
	  for ( i = 0; i < map->count; i++ )
	    {
	      if ( map->pos[i] == DSPD_CHMAP_MONO && map->count > 1U )
		{
		  ret = -ECHRNG;
		  break;
		} else if ( map->pos[i] < DSPD_CHMAP_MONO || map->pos[i] > DSPD_CHMAP_LAST )
		{
		  ret = -ECHRNG;
		  break;
		}
	    }
	}
    }
  return ret;
}


//The second argument is the returned map (arg #3) from dspd_pcm_chmap_translate() or
//the input map (arg #1).
int32_t dspd_pcm_chmap_test(const struct dspd_pcm_chmap *in,  
			    const struct dspd_pcm_chmap *out)
{
  size_t i;
  int32_t ret = 1;
  ssize_t c;
  bool sub = true;
  if ( out != NULL && out->count == 0 )
    {
      ret = -EINVAL;
    } else if ( in->flags & DSPD_CHMAP_MATRIX )
    {
      //Analyze a conversion matrix
      if ( (in->count == 0 || in->ichan == 0 || in->ochan == 0) || (out != NULL && in->ochan > out->count) )
	{
	  ret = -EINVAL;
	} else if ( in->flags & DSPD_CHMAP_MULTI )
	{
	  for ( i = 0; i < in->count; i += 2 )
	    {
	      if ( in->pos[i] >= in->ichan ||
		   in->pos[i+1UL] >= in->ochan ||
		   (out != NULL && in->pos[i+1UL] >= out->count) )
		{
		  ret = -ECHRNG;
		  break;
		}
	    }
	} else
	{
	  for ( i = 0; i < in->count; i++ )
	    {
	      if ( out != NULL )
		{
		  if ( in->pos[i] >= out->count )
		    {
		      ret = -ECHRNG;
		      break;
		    }
		}
	      if ( in->pos[i] >= in->ichan || in->pos[i] >= in->ochan )
		{
		  ret = -ECHRNG;
		  break;
		}
	    }
	}
    } else
    {
      //Analyze an enumerated map
      if ( in->count == 0 || out == NULL )
	{
	  ret = -EINVAL;
	} else
	{
	  for ( i = 0; i < in->count; i++ )
	    {
	      c = find_channel(out, in->pos[i]);
	      if ( c < 0 )
		{
		  ret = -EBADSLT;
		  break;
		} else if ( c != (ssize_t)i )
		{
		  sub = false;
		}
	    }
	  //If the input map is a strict subset of the output map then 
	  //a channel map is not required.
	  if ( ret == 1 && sub == true )
	    ret = 0;
	}
    } 
  return ret;
}



size_t dspd_pcm_chmap_sizeof(size_t count, int32_t flags)
{
  size_t ret;
  if ( count <= DSPD_CHMAP_LAST )
    {
      ret = sizeof(uint32_t) * count;
      if ( flags & DSPD_CHMAP_MULTI )
	ret *= 2;
      ret += sizeof(struct dspd_pcm_chmap);
    } else
    {
      ret = 0;
    }
  return ret;
}

static const char *channel_names[] = {
  "UNKNOWN", "unspecified",
  "NA", "N/A, silent",
  "MONO", "mono stream",
  "FL", "front left",
  "FR", "front right",
  "RL", "rear left",
  "RR", "rear right",
  "FC", "front center",
  "LFE", "subwoofer",
  "SL", "side left",
  "SR", "side right",
  "RC", "rear center",
  "FLC", "front left center",
  "FRC", "front right center",
  "RLC", "rear left center",
  "RRC", "rear right center",
  "FLW", "front left wide",
  "FRW", "front right wide",
  "FLH", "front left high",
  "FCH", "front center high",
  "FRH", "front right high",
  "TC", "top center",
  "TFL", "top front left",
  "TFR", "top front right",
  "TFC", "top front center",
  "TRL", "top rear left",
  "TRR", "top rear right",
  "TRC", "top rear center",
  "TFLC", "top front left center",
  "TFRC", "top front right center",
  "TSL", "top side left",
  "TSR", "top side right",
  "LLFE", "left subwoofer",
  "RLFE", "right subwoofer",
  "BC", "bottom center",
  "BLC", "bottom left center",
  "BRC", "bottom right center",
};

const char *dspd_pcm_chmap_channel_name(size_t channel, bool abbrev)
{
  size_t n = ARRAY_SIZE(channel_names) / 2UL, i;
  const char *ret = NULL;
  if ( channel < n )
    {
      i = channel * 2UL;
      if ( ! abbrev )
	i++;
      ret = channel_names[i];
    }
  return ret;
}

ssize_t dspd_pcm_chmap_index(const char *name)
{
  unsigned long l;
  ssize_t ret = -EINVAL;
  size_t i;
  if ( dspd_strtoul(name, &l, 0) == 0 )
    {
      if ( l <= DSPD_CHMAP_LAST )
	ret = l;
    } else
    {
      for ( i = 0; i < ARRAY_SIZE(channel_names); i += 2 )
	{
	  if ( strcasecmp(name, channel_names[i]) == 0 )
	    {
	      ret = i / 2;
	      break;
	    }
	}
    }
  return ret;
}




int32_t dspd_pcm_chmap_from_string(const char *str, struct dspd_pcm_chmap_container *map)
{
  char buf[16];
  const char *saveptr, *tok;
  size_t len, pos = 0;
  int32_t ret = 0, src, dest;
  char *p;
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

      p = strstr(buf, "=>");
      if ( ! p )
	{
	  //The format is 'A,B,C...'
	  if ( have_multi )
	    {
	      ret = -EINVAL;
	      break;
	    }
	  ret = dspd_pcm_chmap_index(buf);
	  if ( ret < 0 )
	    break;
	  map->map.pos[pos] = ret;
	} else
	{
	  //The format is 'A=>B,C=>D,...'
	  if ( pos > 0 && have_multi == false )
	    {
	      ret = -EINVAL;
	      break;
	    }
	  have_multi = true;

	  *p = 0;
	  p += 2UL;
	  ret = dspd_strtoi32(buf, &src, 0);
	  if ( ret < 0 )
	    break;
	  ret = dspd_strtoi32(p, &dest, 0);
	  if ( ret < 0 )
	    break;

	  if ( map->map.ichan < src )
	    map->map.ichan = src;
	  map->map.pos[pos*2UL] = src;
	  
	  if ( map->map.ochan < dest )
	    map->map.ochan = dest;
	  map->map.pos[(pos*2UL)+1UL] = dest;
	}
      pos++;
    }
  map->map.count = pos;
  if ( have_multi )
    {
      map->map.flags |= DSPD_CHMAP_MULTI;
    }
  map->map.ichan = 0;
  map->map.ochan = 0;
  return ret;
}

static ssize_t chmap_matrix_to_string(const struct dspd_pcm_chmap *map, char *buf, size_t len)
{
  size_t i, o = 0;
  size_t offset = 0;
  ssize_t ret = 0;
  int n;
  if ( map->flags & DSPD_CHMAP_MULTI )
    {
      for ( i = 0; i < map->count; i += 2 )
	{
	  if ( o > 0 )
	    n = snprintf(&buf[offset], len - offset, "%u=>%u", map->pos[i], map->pos[i+1UL]);
	  else
	    n = snprintf(&buf[offset], len - offset, ",%u=>%u", map->pos[i], map->pos[i+1UL]);
	  offset += n;
	  if ( offset >= len )
	    {
	      ret = -ENOSPC;
	      break;
	    }
	}
    } else
    {
      for ( i = 0; i < map->count; i++ )
	{
	  if ( o > 0 )
	    n = snprintf(&buf[offset], len - offset, "%u", map->pos[i]);
	  else
	    n = snprintf(&buf[offset], len - offset, ",%u", map->pos[i]);
	  offset += n;
	  if ( offset >= len )
	    {
	      ret = -ENOSPC;
	      break;
	    }
	}
    }
  if ( ret == 0 && offset > 0 )
    ret = offset;
  return ret;
}

static ssize_t chmap_to_string(const struct dspd_pcm_chmap *map, char *buf, size_t len)
{
  size_t offset = 0;
  size_t i;
  const char *c;
  ssize_t ret = 0;
  int n;
  char tmp[32];
  for ( i = 0; i < map->count; i++ )
    {
      c = dspd_pcm_chmap_channel_name(map->pos[i], true);
      if ( c == NULL || strcmp(c, "NA") == 0 || strcmp(c, "UNKNOWN") == 0 )
	{
	  n = map->pos[i] & DSPD_CHMAP_POSITION_MASK;
	  if ( n > DSPD_CHMAP_UNKNOWN && n <= DSPD_CHMAP_LAST )
	    {
	      sprintf(tmp, "%u", n);
	      c = tmp;
	    } else
	    {
	      ret = -EINVAL;
	      break;
	    }
	}
      if ( i > 0 )
	n = snprintf(&buf[offset], len - offset, ",%s", c);
      else
	n = snprintf(&buf[offset], len - offset, "%s", c);
      offset += n;
      if ( offset > len )
	{
	  ret = -ENOSPC;
	  break;
	}
    }
  if ( ret == 0 && offset > 0 )
    ret = offset;
  return ret;
}

ssize_t dspd_pcm_chmap_to_string(const struct dspd_pcm_chmap *map, char *buf, size_t len)
{
  ssize_t ret;
  if ( map->flags & DSPD_CHMAP_MATRIX )
    {
      if ( (map->flags & DSPD_CHMAP_MATRIX) && (map->count % 2U) )
	ret = -EINVAL;
      else
	ret = chmap_matrix_to_string(map, buf, len);
    } else
    {
      ret = chmap_to_string(map, buf, len);
    }
  return ret;
}

int32_t dspd_pcm_chmap_any(const struct dspd_pcm_chmap *map, struct dspd_pcm_chmap *result)
{
  const struct dspd_pcm_chmap *m;
  size_t i, o;
  uint32_t c, f = result->flags & DSPD_PCM_SBIT_FULLDUPLEX;
  int32_t ret = -EINVAL;
  if ( map == NULL || result->count == 1 )
    {
      if ( result->count > 0 && result->count <= DSPD_CHMAP_LAST )
	{
	  m = dspd_pcm_chmap_get_default(result->count);
	  if ( m )
	    {
	      memcpy(result, m, dspd_pcm_chmap_sizeof(m->count, m->flags));
	      ret = 0;
	    } else
	    {
	      if ( result->count == 1U )
		{
		  result->pos[0] = DSPD_CHMAP_MONO;
		} else
		{
		  for ( i = 0; i < result->count; i++ )
		    result->pos[i] = DSPD_CHMAP_FL+i;
		}
	      ret = 0;
	      result->ichan = 0;
	      result->ochan = 0;
	    }
	}
    } else
    {
      if ( ! (map->flags & DSPD_CHMAP_MATRIX) )
	{
	  m = dspd_pcm_chmap_get_default(result->count);
	  if ( m )
	    {
	      if ( dspd_pcm_chmap_test(m, map) >= 0 )
		{
		  memcpy(result, m, dspd_pcm_chmap_sizeof(m->count, m->flags));
		  result->flags |= f;
		  return 0;
		}
	    }
	  for ( i = 0, o = 0; i < map->count && o < result->count; i++ )
	    {
	      c = map->pos[i] & DSPD_CHMAP_POSITION_MASK;
	      if ( c >= DSPD_CHMAP_MONO && c <= DSPD_CHMAP_LAST )
		{
		  result->pos[o] = map->pos[i];
		  o++;
		}
	    }
	  if ( o > 0 )
	    {
	      ret = 0;
	      result->ichan = 0;
	      result->ochan = 0;
	      result->count = o;
	    }
	}
    }
  result->flags |= f;
  return ret;
}

void dspd_pcm_chmap_write_buf(const struct dspd_pcm_chmap * __restrict map, 
			      const float                 * __restrict inbuf,
			      double                      * __restrict outbuf,
			      size_t                                   frames,
			      double                                   volume)
{
  size_t i, j;
  for ( i = 0; i < frames; i++ )
    {
      for ( j = 0; j < map->count; j++ )
	outbuf[map->pos[j]] += (inbuf[j] * volume); 
      inbuf = &inbuf[map->ichan];
      outbuf = &outbuf[map->ochan];
    }
}


void dspd_pcm_chmap_write_buf_multi(const struct dspd_pcm_chmap * __restrict map, 
				    const float                 * __restrict inbuf,
				    double                      * __restrict outbuf,
				    size_t                                   frames,
				    double                                   volume)
{
  size_t i, j;
  for ( i = 0; i < frames; i++ )
    {
      for ( j = 0; j < map->count; j += 2UL )
	outbuf[map->pos[j+1UL]] += (inbuf[map->pos[j]] * volume);
      inbuf = &inbuf[map->ichan];
      outbuf = &outbuf[map->ochan];
    }
}

void dspd_pcm_chmap_write_buf_simple(const struct dspd_pcm_chmap * __restrict map, 
				     const float                 * __restrict inbuf,
				     double                      * __restrict outbuf,
				     size_t                                   frames,
				     double                                   volume)
{
  size_t i, j;
  for ( i = 0; i < frames; i++ )
    {
      for ( j = 0; j < map->count; j++ )
	outbuf[j] += (inbuf[j] * volume);
      inbuf = &inbuf[map->ichan];
      outbuf = &outbuf[map->ochan];
    }
}


void dspd_pcm_chmap_read_buf(const struct dspd_pcm_chmap * __restrict map, 
			     const float                 * __restrict inbuf,
			     float                       * __restrict outbuf,
			     size_t                                   frames,
			     float                                    volume)
{
  size_t i, j;
  for ( i = 0; i < frames; i++ )
    {
      for ( j = 0; j < map->count; j++ )
	outbuf[map->pos[j]] += (inbuf[j] * volume);
      inbuf = &inbuf[map->ichan];
      outbuf = &outbuf[map->ochan];
    }
}


void dspd_pcm_chmap_read_buf_multi(const struct dspd_pcm_chmap * __restrict map, 
				   const float                 * __restrict inbuf,
				   float                       * __restrict outbuf,
				   size_t                                   frames,
				   float                                    volume)
{
  size_t i, j;
  for ( i = 0; i < frames; i++ )
    {
      for ( j = 0; j < map->count; j += 2UL )
	outbuf[map->pos[j+1UL]] += (inbuf[map->pos[j]] * volume);
      inbuf = &inbuf[map->ichan];
      outbuf = &outbuf[map->ochan];
    }
}

void dspd_pcm_chmap_read_buf_simple(const struct dspd_pcm_chmap * __restrict map, 
				    const float                 * __restrict inbuf,
				    float                       * __restrict outbuf,
				    size_t                                   frames,
				    float                                    volume)
{
  size_t i, j;
  for ( i = 0; i < frames; i++ )
    {
      for ( j = 0; j < map->count; j++ )
	outbuf[j] += (inbuf[j] * volume);
      inbuf = &inbuf[map->ichan];
      outbuf = &outbuf[map->ochan];
    }
}
