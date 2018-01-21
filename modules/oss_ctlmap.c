/*
 *   OSS_CTLMAP - Map ALSA style controls to an OSSv4 compatible layout
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */



#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "../lib/sslib.h"
#include "../lib/req.h"
#include "../lib/daemon.h"

struct dspd_ctlm {
  size_t                real_element;
  struct dspd_mix_info  info;
  struct dspd_mix_range range;
  uint32_t              scale_max;
};

struct dspd_ctl_map {
  int32_t           device;
  void             *context;
  struct dspd_ctlm *controls;
  size_t            control_count;
  size_t            max_control_count;
  struct dspd_ctlm *real_controls;
  size_t            real_control_count;
  ssize_t           refcnt;
  bool              dead;
  uint64_t          tstamp;
  struct dspd_rctx  rctx;
  pthread_mutex_t   lock;
};




/*

  The ossxmix utility has some problems with control ranges that include negative numbers and OSS
  can't understand multi channel controls.  OSS tends to expose controls intended for a GUI and
  ALSA tends to expose low level hardware controls.  The information that the OSS API expects is similar
  to what alsamixer generates internally for its user interface.  For that reason, this code is based
  on the way alsamixer works.  If a device doesn't show up correctly in ossxmix, then the best thing to
  do is find out what alsamixer is doing and try to duplicate that here.

*/

//mapped value to native control
static int32_t m2ctl(const struct dspd_ctlm *ctl, int32_t mval)
{
  int32_t ret;
  uint32_t r, n;
  if ( ctl->scale_max > 0 )
    {
      if ( ctl->range.min < ctl->range.max )
	r = ctl->range.max - ctl->range.min;
      else
	r = ctl->range.min - ctl->range.max;
      
      if ( mval <= 0 )
	return ctl->range.min;
      else if ( mval >= ctl->scale_max )
	return ctl->range.max;
      
      if ( r > ctl->scale_max )
	{
	  n = r / ctl->scale_max;
	  mval *= n;
	} else
	{
	  n = ctl->scale_max / r;
	  mval /= n;
	}
    }
  if ( (ctl->range.min < 0 || ctl->range.max < 0) && ctl->range.min != ctl->range.max )
    {
      if ( ctl->range.min < ctl->range.max )
	ret = mval + ctl->range.min;
      else
	ret = ctl->range.min - mval;
    } else
    {
      ret = mval;
    }
  return ret;
}

//native control to mapped value
static int32_t ctl2m(const struct dspd_ctlm *ctl, int32_t cval)
{
  int32_t ret, r, n, orig_val = cval;
  if ( (ctl->range.min < 0 || ctl->range.max < 0) && ctl->range.min != ctl->range.max )
    {
      if ( ctl->range.min < ctl->range.max )
	ret = cval - ctl->range.min;
      else
	ret = ctl->range.min - cval;
    } else
    {
      ret = cval;
    }
  if ( ctl->scale_max > 0 )
    {
      if ( ctl->range.min < ctl->range.max )
	r = ctl->range.max - ctl->range.min;
      else
	r = ctl->range.min - ctl->range.max;
      if ( orig_val == ctl->range.max )
	{
	  ret = ctl->scale_max;
	} else if ( orig_val == ctl->range.min )
	{
	  ret = 0;
	} else
	{
	  if ( r > ctl->scale_max )
	    {
	      n = r / ctl->scale_max;
	      ret /= n;
	    } else
	    {
	      n = ctl->scale_max / r;
	      ret *= n;
	    }
	}
    }
  return ret;
}


static void free_ctl(struct dspd_ctl_map *map)
{
  free(map->controls);
  map->controls = NULL;
  free(map->real_controls);
  map->real_controls = NULL;
  map->real_control_count = 0;
  map->max_control_count = 0;
  map->control_count = 0;
}

static struct dspd_ctlm *add_ctl(struct dspd_ctl_map *map, const struct dspd_ctlm *tmpl, int newtype)
{
  size_t count = map->control_count + 1;
  void *p;
  struct dspd_ctlm *ret = NULL;
  size_t br;
  struct dspd_mix_val val;
  if ( map->max_control_count < count )
    {
      count *= 2;
      p = dspd_reallocz(map->controls, 
			sizeof(*map->controls) * count,
			sizeof(*map->controls) * map->control_count,
			false);
      if ( p )
	{
	  map->controls = p;
	  ret = &map->controls[map->control_count];
	  map->control_count++;
	  map->max_control_count = count;
	}
    } else
    {
      ret = &map->controls[map->control_count];
      map->control_count = count;
    }
  if ( ret && tmpl )
    {
      *ret = *tmpl;
      memset(&val, 0, sizeof(val));
      val.index = tmpl->real_element;
      val.type = newtype;
      (void)dspd_stream_ctl(map->context,
			    map->device,
			    DSPD_SCTL_SERVER_MIXER_GETRANGE,
			    &val,
			    sizeof(val),
			    &ret->range,
			    sizeof(ret->range),
			    &br);
      ret->info.ctl_index = map->control_count - 1;
    }
  assert(ret);
  return ret;
}

static int32_t add_enum(struct dspd_ctl_map *map, const struct dspd_ctlm *rctl)
{
  struct dspd_ctlm *ctl;
  int32_t ret = 0;
  if ( rctl->info.flags & DSPD_MIXF_ENUM )
    {
      ctl = add_ctl(map, rctl, DSPD_MIXF_ENUM);
      if ( ctl )
	ctl->info.flags = rctl->info.flags;
      else
	ret = -ENOMEM;
    }
  return ret;
}
static int32_t add_switch(struct dspd_ctl_map *map, const struct dspd_ctlm *rctl)
{
  int32_t ret = 0;
  struct dspd_ctlm *ctl;
  if ( rctl->info.flags & DSPD_MIXF_PSWITCH )
    {
      ctl = add_ctl(map, rctl, DSPD_MIXF_PSWITCH);
      if ( ctl )
	ctl->info.flags = rctl->info.flags & (DSPD_MIXF_PSWITCH|DSPD_MIXF_PSWJOINED|DSPD_MIXF_COMMSWITCH);
      else
	ret = -ENOMEM;
    }
  if ( ret == 0 && (rctl->info.flags & DSPD_MIXF_CSWITCH) )
    {
      ctl = add_ctl(map, rctl, DSPD_MIXF_CSWITCH);
      if ( ctl )
	ctl->info.flags = rctl->info.flags & (DSPD_MIXF_CSWITCH|DSPD_MIXF_CSWJOINED|DSPD_MIXF_COMMSWITCH|DSPD_MIXF_CSWEXCL);
      else
	ret = -ENOMEM;
    }
  return ret;
}

struct ctl_chpair {
  int32_t     left;
  int32_t     right;
  const char *name;
};
static const char *is_paired(int32_t mask, uint32_t firstbit)
{
  static const struct ctl_chpair ctl_pairs[] = {
    {
      .left = DSPD_MIXER_CHN_FRONT_LEFT,
      .right = DSPD_MIXER_CHN_FRONT_RIGHT,
      .name = "Front"
    },
    {
      .left = DSPD_MIXER_CHN_REAR_LEFT, 
      .right = DSPD_MIXER_CHN_REAR_RIGHT,
      .name = "Rear"
    },
    {
      .left = DSPD_MIXER_CHN_SIDE_LEFT, 
      .right = DSPD_MIXER_CHN_SIDE_RIGHT,
      .name = "Side"
    },
    {
      .left = DSPD_MIXER_CHN_FRONT_CENTER,
      .right = DSPD_MIXER_CHN_REAR_CENTER,
      .name = "Center"
    },
    {
      .left = DSPD_MIXER_CHN_FRONT_CENTER,
      .right = DSPD_MIXER_CHN_REAR_CENTER,
      .name = "Center"
    },
  };

  size_t i;
  const char *ret = NULL;
  const struct ctl_chpair *p;
  uint32_t nextbit = firstbit + 1;
  if ( nextbit < 32U && ((1U << nextbit) & mask) && ((1U << firstbit) & mask) )
    {
      for ( i = 0; i < ARRAY_SIZE(ctl_pairs); i++ )
	{
	  p = &ctl_pairs[i];
	  if ( p->left == firstbit && p->right == nextbit )
	    {
	      ret = p->name;
	      break;
	    }
	}
    }
  return ret;
}
static const char *is_single(int32_t mask, uint32_t bit)
{
  static const int32_t channels[] = {
    DSPD_MIXER_CHN_MONO,
    DSPD_MIXER_CHN_FRONT_RIGHT,
    DSPD_MIXER_CHN_REAR_LEFT,
    DSPD_MIXER_CHN_REAR_RIGHT,
    DSPD_MIXER_CHN_FRONT_CENTER,
    DSPD_MIXER_CHN_WOOFER,
    DSPD_MIXER_CHN_SIDE_LEFT,
    DSPD_MIXER_CHN_SIDE_RIGHT,
    DSPD_MIXER_CHN_REAR_CENTER,
  };
  static char *desc[] = {
    "Mono",
    "Right",
    "Rear Left",
    "Rear Right",
    "Front Center",
    "LFE",
    "Side Left",
    "Side Right",
    "Rear Center",
  };
  const char *ret = NULL;
  size_t i;
  if ( (1U << bit) & mask )
    {
      for ( i = 0; i < ARRAY_SIZE(channels); i++ )
	{
	  if ( channels[i] == bit )
	    {
	      ret = desc[i];
	      break;
	    }
	}
    }
  return ret;
}


static void set_range(struct dspd_ctlm *ctl)
{
  int32_t r;
  if ( ctl->range.min != ctl->range.max )
    {
      if ( ctl->range.min < ctl->range.max )
	r = ctl->range.max - ctl->range.min;
      else
	r = ctl->range.min - ctl->range.max;
      if ( r > UINT8_MAX )
	ctl->scale_max = INT16_MAX;
      else
	ctl->scale_max = UINT8_MAX;
    }
}

static int32_t add_voldb(struct dspd_ctl_map *map, const struct dspd_ctlm *rctl)
{
  int32_t ret = 0;
  struct dspd_ctlm *ctl;
  uint32_t chmask, m;
  size_t i;
  const char *name;
  
  if ( rctl->info.flags & (DSPD_MIXF_PVOL|DSPD_MIXF_PDB) )
    {
      chmask = rctl->info.pchan_mask;
      for ( i = 0; i < DSPD_MIXER_CHN_LAST; i++ )
	{
	 
	  if ( (name = is_paired(chmask, i)) )
	    {
	      ctl = add_ctl(map, rctl, DSPD_MIXF_PVOL);
	      if ( ctl )
		{
		  ctl->info.flags = rctl->info.flags & (DSPD_MIXF_PVOL|DSPD_MIXF_PDB|DSPD_MIXF_PMONO|DSPD_MIXF_PVJOINED|DSPD_MIXF_COMMVOL);
		  m = (1U << i) | (1U << (i+1));
		  ctl->info.pchan_mask = m;
		  chmask &= ~m;
		  set_range(ctl);
		  strlcpy(ctl->info.name, name, sizeof(ctl->info.name));
		} else
		{
		  ret = -ENOMEM;
		  break;
		}
	    }
	}
      if ( chmask )
	{
	  for ( i = 0; i < DSPD_MIXER_CHN_LAST; i++ )
	    {
	      if ( (name = is_single(chmask, i)) )
		{
		  m = 1U << i;
		  ctl = add_ctl(map, rctl, DSPD_MIXF_PVOL);
		  if ( ctl )
		    {
		      ctl->info.flags = rctl->info.flags & (DSPD_MIXF_PVOL|DSPD_MIXF_PDB|DSPD_MIXF_PMONO|DSPD_MIXF_PVJOINED|DSPD_MIXF_COMMVOL);
		      ctl->info.pchan_mask = m;
		      chmask &= ~m;
		      if ( strcmp(name, "Mono") != 0 )
			strlcpy(ctl->info.name, name, sizeof(ctl->info.name));
		      if ( chmask != ctl->info.pchan_mask )
			set_range(ctl);
		    } else
		    {
		      ret = -ENOMEM;
		      break;
		    }
		}
	    }
	}
      
    }
  if ( rctl->info.flags & (DSPD_MIXF_CVOL|DSPD_MIXF_CDB) )
    {
      ctl = add_ctl(map, rctl, DSPD_MIXF_CVOL);
      if ( ctl )
	ctl->info.flags = rctl->info.flags & (DSPD_MIXF_CVOL|DSPD_MIXF_CDB|DSPD_MIXF_CMONO|DSPD_MIXF_CVJOINED|DSPD_MIXF_COMMVOL);
      else
	ret = -ENOMEM;
    }
  return ret;
}


static int32_t add_element(struct dspd_ctl_map *map, struct dspd_ctlm *rctl)
{
  int32_t ret;
  ret = add_enum(map, rctl);
  if ( ret == 0 )
    ret = add_switch(map, rctl);
  if ( ret == 0 )
    ret = add_voldb(map, rctl);
  return ret;
}

static int32_t refresh_ctl(struct dspd_ctl_map *map, size_t count)
{
  uint32_t i;
  struct dspd_ctlm *ctl;
  size_t br;
  int32_t ret;
  struct dspd_mix_info info;
  free_ctl(map);
  map->real_controls = calloc(count, sizeof(*map->real_controls));
  if ( ! map->real_controls )
    return -ENOMEM;
  i = UINT32_MAX;
  ret = dspd_stream_ctl(map->context,
			map->device,
			DSPD_SCTL_SERVER_MIXER_ELEM_INFO,
			&i,
			sizeof(i),
			&info,
			sizeof(info),
			&br);
  
  if ( ret == 0 )
    {
      map->tstamp = info.tstamp;
      for ( i = 0; i < count; i++ )
	{
	  ctl = &map->real_controls[i];
	  ctl->real_element = i;
	  ret = dspd_stream_ctl(map->context,
				map->device,
				DSPD_SCTL_SERVER_MIXER_ELEM_INFO,
				&i,
				sizeof(i),
				&ctl->info,
				sizeof(ctl->info),
				&br);
	  if ( ret == 0 )
	    {
	      if ( br != sizeof(ctl->info) )
		{
		  ret = -EPROTO;
		  break;
		}
	      ret = add_element(map, ctl);
	      if ( ret < 0 )
		break;
	    } else if ( ret < 0 )
	    {
	      if ( ret == -EINVAL )
		ret = -EAGAIN;
	      break;
	    }
	}
    }
  if ( ret == 0 )
    map->real_control_count = count;
  else
    map->control_count = 0;
  return ret;
}

static int32_t find_ctl(struct dspd_ctl_map *map,
			struct dspd_ctlm **c,
			uint32_t index,
			uint64_t tstamp,
			uint64_t type,
			uint32_t flags)
{
  struct dspd_ctlm *ctl;
  int32_t ret = -EINVAL;
  uint64_t ts;
  if ( index < map->control_count )
    {
      ctl = &map->controls[index];
      
      if ( flags & DSPD_CTRLF_TSTAMP_32BIT )
	ts = (ctl->info.tstamp / 1000000ULL) % UINT32_MAX;
      else
	ts = tstamp;
      if ( tstamp != 0 && ts != tstamp )
	{
	  ret = -EIDRM;
	} else if ( ctl->info.flags & type )
	{
	  *c = ctl;
	  ret = 0;
	}
    } else
    {
      ret = -EIDRM;
    }
  return ret;
}




static int32_t ctrlm_mixer_elem_count(struct dspd_rctx *rctx,
				      uint32_t          req,
				      const void       *inbuf,
				      size_t            inbufsize,
				      void             *outbuf,
				      size_t            outbufsize)
{
  struct dspd_ctl_map *map = dspd_req_userdata(rctx);
  int32_t ret;
  uint32_t count = map->real_control_count;
  uint64_t c;
  size_t br;
  struct dspd_mix_val cmd, info;
  bool refresh = false;
  ret = dspd_stream_ctl(map->context,
			map->device,
			req,
			NULL,
			0,
			&count,
			sizeof(count),
			&br);
  if ( ret == 0 && br == sizeof(count) )
    {
      if ( count == map->real_control_count )
	{
	  memset(&cmd, 0, sizeof(cmd));
	  cmd.index = UINT32_MAX;
	  ret = dspd_stream_ctl(map->context,
				map->device,
				DSPD_SCTL_SERVER_MIXER_GETVAL,
				&cmd,
				sizeof(cmd),
				&info,
				sizeof(info),
				&br);
	  if ( ret == 0 && br == sizeof(info) )
	    {
	      if ( map->tstamp != info.tstamp )
		refresh = true;
	    }
	} else
	{
	  refresh = true;
	}
    }
  if ( ret == 0 && refresh == true )
    {
      while ( (ret = refresh_ctl(map, count)) == -EAGAIN )
	{
	  ret = dspd_stream_ctl(map->context,
				map->device,
				req,
				NULL,
				0,
				&count,
				sizeof(count),
				&br);
	  if ( ret == 0 && br != sizeof(count) )
	    ret = -EPROTO;
	  if ( ret )
	    break;
	}
    }
  if ( ret == 0 )
    {
      count = map->control_count;
      if ( outbufsize >= sizeof(c) )
	{
	  c = map->max_control_count;
	  c <<= 32U;
	  c |= count;
	  ret = dspd_req_reply_buf(rctx, 0, &c, sizeof(c));
	} else
	{
	  ret = dspd_req_reply_buf(rctx, 0, &count, sizeof(count));
	}
    } else
    {
      ret = dspd_req_reply_err(rctx, 0, ret);
    }
  return ret;
}

static int32_t ctrlm_mixer_elem_info(struct dspd_rctx *rctx,
				     uint32_t          req,
				     const void       *inbuf,
				     size_t            inbufsize,
				     void             *outbuf,
				     size_t            outbufsize)
{
  struct dspd_ctl_map *map = dspd_req_userdata(rctx);
  uint32_t idx = *(uint32_t*)inbuf, newidx;
  struct dspd_mix_info info;
  int32_t ret = -EINVAL;
  struct dspd_ctlm *ctl;
  size_t br;
  if ( idx < map->control_count )
    {
      ctl = &map->controls[idx];
      newidx = ctl->real_element;
      ret = dspd_stream_ctl(map->context,
			    map->device,
			    req,
			    &newidx,
			    sizeof(newidx),
			    &info,
			    sizeof(info),
			    &br);
      if ( ret == 0 )
	{
	  info.flags = ctl->info.flags;
	  info.ctl_index = idx;
	  info.tstamp = ctl->info.tstamp;
	  info.pchan_mask = ctl->info.pchan_mask;
	  info.cchan_mask = ctl->info.cchan_mask;
	  strlcpy(info.name, ctl->info.name, sizeof(info.name));
	}
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(rctx, 0, &info, br);
  else
    ret = dspd_req_reply_err(rctx, 0, ret);
  return ret;
}

static int32_t ctrlm_mixer_enum_info(struct dspd_rctx *rctx,
				     uint32_t          req,
				     const void       *inbuf,
				     size_t            inbufsize,
				     void             *outbuf,
				     size_t            outbufsize)
{
  struct dspd_ctl_map *map = dspd_req_userdata(rctx);
  const struct dspd_mix_enum_idx *idx = inbuf;
  struct dspd_mix_enum_idx newidx;
  struct dspd_ctlm *ctl;
  int32_t ret = -EINVAL;
  size_t br = 0;
  if ( idx->elem_idx < map->control_count )
    {
      ctl = &map->controls[idx->elem_idx];
      newidx.elem_idx = ctl->real_element;
      newidx.enum_idx = idx->enum_idx;
      ret = dspd_stream_ctl(map->context,
			    map->device,
			    req,
			    &newidx,
			    sizeof(newidx),
			    outbuf,
			    outbufsize,
			    &br);
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(rctx, 0, outbuf, br);
  else
    ret = dspd_req_reply_err(rctx, 0, ret);
  return ret;
}

static int32_t ctrlm_mixer_elem_getval(struct dspd_rctx *rctx,
				       uint32_t          req,
				       const void       *inbuf,
				       size_t            inbufsize,
				       void             *outbuf,
				       size_t            outbufsize)

{
  struct dspd_ctl_map *map = dspd_req_userdata(rctx);
  const struct dspd_mix_val *cmd = inbuf;
  struct dspd_mix_val newcmd, out;
  struct dspd_ctlm *ctl;
  int32_t ret;
  size_t br = 0;
  if ( cmd->index == UINT32_MAX )
    {
      ret = dspd_stream_ctl(map->context,
			    map->device,
			    req,
			    inbuf,
			    inbufsize,
			    outbuf,
			    outbufsize,
			    &br);
    } else
    {
      ret = find_ctl(map, &ctl, cmd->index, cmd->tstamp, cmd->type, cmd->flags);
      if ( ret == 0 )
	{
	  newcmd = *cmd;
	  newcmd.index = ctl->real_element;
	  newcmd.tstamp = map->real_controls[ctl->real_element].info.tstamp;
	  ret = dspd_stream_ctl(map->context,
				map->device,
				req,
				&newcmd,
				sizeof(newcmd),
				&out,
				sizeof(out),
				&br);
	  if ( ret == 0 )
	    {
	      out.index = ctl->info.ctl_index;
	      out.tstamp = ctl->info.tstamp;
	      out.value = ctl2m(ctl, out.value);
	      outbuf = &out;
	    }
	}
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(rctx, 0, outbuf, br);
  else
    ret = dspd_req_reply_err(rctx, 0, ret);
  return ret;
}

static int32_t ctrlm_mixer_elem_setval(struct dspd_rctx *rctx,
				       uint32_t          req,
				       const void       *inbuf,
				       size_t            inbufsize,
				       void             *outbuf,
				       size_t            outbufsize)

{  
  struct dspd_ctl_map *map = dspd_req_userdata(rctx);
  const struct dspd_mix_val *cmd = inbuf;
  struct dspd_mix_val newcmd, out;
  struct dspd_ctlm *ctl;
  int32_t ret;
  size_t br = 0;
  ret = find_ctl(map, &ctl, cmd->index, cmd->tstamp, cmd->type, cmd->flags);
  if ( ret == 0 )
    {
      newcmd = *cmd;
      newcmd.value = m2ctl(ctl, cmd->value);
      newcmd.index = ctl->real_element;
      newcmd.tstamp = map->real_controls[ctl->real_element].info.tstamp;
      newcmd.flags = cmd->flags;
      if ( newcmd.flags & DSPD_CTRLF_TSTAMP_32BIT )
	newcmd.tstamp = (newcmd.tstamp / 1000000ULL) % UINT32_MAX;
      if ( outbuf )
	outbuf = &out;

      
      
      ret = dspd_stream_ctl(map->context,
			    map->device,
			    req,
			    &newcmd,
			    sizeof(newcmd),
			    &out,
			    sizeof(out),
			    &br);
      if ( ret == 0 && outbuf != NULL )
	{
	  out.index = ctl->info.ctl_index;

	  out.value = ctl2m(ctl, out.value);

	  if ( newcmd.flags & DSPD_CTRLF_TSTAMP_32BIT )
	    out.tstamp = (ctl->info.tstamp / 1000000ULL) % UINT32_MAX;
	  else
	    out.tstamp = ctl->info.tstamp;
	}
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(rctx, 0, outbuf, br);
  else
    ret = dspd_req_reply_err(rctx, 0, ret);
  return ret;
}

static int32_t ctrlm_mixer_elem_getrange(struct dspd_rctx *rctx,
					 uint32_t          req,
					 const void       *inbuf,
					 size_t            inbufsize,
					 void             *outbuf,
					 size_t            outbufsize)
{
  struct dspd_ctl_map *map = dspd_req_userdata(rctx);
  const struct dspd_mix_val *cmd = inbuf; struct dspd_mix_val newcmd;
  struct dspd_ctlm *ctl;
  int32_t ret;
  struct dspd_mix_range *r;
  size_t br = 0;
  ret = find_ctl(map, &ctl, cmd->index, cmd->tstamp, cmd->type, cmd->flags);
  if ( ret == 0 )
    {
      if ( ((ctl->range.min >= 0 && ctl->range.max >= 0) || ctl->range.min == ctl->range.max) && ctl->scale_max == 0 )
	{
	  newcmd = *cmd;
	  newcmd.index = ctl->real_element;
	  newcmd.tstamp = map->real_controls[ctl->real_element].info.tstamp;
	  ret = dspd_stream_ctl(map->context,
				map->device,
				req,
				&newcmd,
				sizeof(newcmd),
				outbuf,
				outbufsize,
				&br);
	} else
	{
	  r = outbuf;
	  if ( ctl->scale_max > 0 )
	    {
	      r->min = 0;
	      r->max = ctl->scale_max;
	    } else if ( ctl->range.min < 0 || ctl->range.max < 0 )
	    {
	      br = sizeof(ctl->range);
	      r->min = 0;
	      if ( ctl->range.min < ctl->range.max )
		r->max = ctl->range.max - ctl->range.min;
	      else
		r->max = ctl->range.min - ctl->range.max;
	    } else
	    {
	      *r = ctl->range;
	    }
	}
      r = outbuf;
    }
  if ( ret == 0 && br > 0 )
    ret = dspd_req_reply_buf(rctx, 0, outbuf, br);
  else
    ret = dspd_req_reply_err(rctx, 0, ret);
  return ret;
}

static int32_t ctrlm_mixer_set_cb(struct dspd_rctx *rctx,
				  uint32_t          req,
				  const void       *inbuf,
				  size_t            inbufsize,
				  void             *outbuf,
				  size_t            outbufsize)
{
  return dspd_req_reply_err(rctx, 0, EINVAL);
}

#define OSS_MIXER_CMDN(cmd) (DSPD_MIXER_CMDN(cmd)+1UL)

static struct dspd_req_handler mixer_handlers[] = {
  [OSS_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ELEM_COUNT)] = {
    .handler = ctrlm_mixer_elem_count,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = 0,
    .outbufsize = sizeof(uint32_t),
  },
  [OSS_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ELEM_INFO)] = {
    .handler = ctrlm_mixer_elem_info,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(uint32_t),
    .outbufsize = sizeof(struct dspd_mix_info),
  },
  [OSS_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_ENUM_INFO)] = {
    .handler = ctrlm_mixer_enum_info,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_enum_idx),
    .outbufsize = sizeof(struct dspd_mix_info),
  },
  [OSS_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_GETVAL)] = {
    .handler = ctrlm_mixer_elem_getval,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = sizeof(struct dspd_mix_val),
  },
  [OSS_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_SETVAL)] = {
    .handler = ctrlm_mixer_elem_setval,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = 0,
  },
  [OSS_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_GETRANGE)] = {
    .handler = ctrlm_mixer_elem_getrange,
    .xflags = DSPD_REQ_FLAG_CMSG_FD,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mix_val),
    .outbufsize = sizeof(struct dspd_mix_range),
  },
  [OSS_MIXER_CMDN(DSPD_SCTL_SERVER_MIXER_SETCB)] = { 
    .handler = ctrlm_mixer_set_cb,
    .xflags = DSPD_REQ_FLAG_CMSG_FD | DSPD_REQ_FLAG_REMOTE,
    .rflags = 0,
    .inbufsize = sizeof(struct dspd_mixer_cbinfo),
    .outbufsize = 0,
  },
};
static int32_t ctlm_reply_buf(struct dspd_rctx *arg, 
				int32_t flags, 
				const void *buf, 
				size_t len)
{
  if ( buf != arg->outbuf )
    memcpy(arg->outbuf, buf, len);
  arg->bytes_returned = len;
  return 0;
}
static int32_t ctlm_reply_fd(struct dspd_rctx *arg, 
				int32_t flags, 
				const void *buf, 
				size_t len, 
				int32_t fd)
{
  return -EPROTO; //Not implemented, so trying it is an error.
}

static int32_t ctlm_reply_err(struct dspd_rctx *arg, 
				int32_t flags, 
				int32_t err)
{
  if ( err > 0 )
    err *= -1;
  return err;
}

static const struct dspd_rcb ctlm_rcb = {
  .reply_buf = ctlm_reply_buf,
  .reply_err = ctlm_reply_err,
  .reply_fd = ctlm_reply_fd,
};


static int32_t dspd_ctlm_ctl(struct dspd_ctl_map *map,
			     int32_t              stream,
			     int32_t              cmd,
			     const void          *inbuf,
			     size_t               inbufsize,
			     void                *outbuf,
			     size_t               outbufsize,
			     size_t              *bytes_returned)
{
  int32_t ret;
  size_t idx;
  idx = OSS_MIXER_CMDN(cmd);
  if ( idx < ARRAY_SIZE(mixer_handlers) )
    {
      map->rctx.outbuf = outbuf;
      map->rctx.outbufsize = outbufsize;
      ret = dspd_daemon_dispatch_ctl(&map->rctx, 
				     mixer_handlers,
				     ARRAY_SIZE(mixer_handlers),
				     (uint64_t)OSS_MIXER_CMDN(cmd) | (uint64_t)cmd << 32U,
				     inbuf,
				     inbufsize,
				     outbuf,
				     outbufsize);
      if ( ret == 0 && bytes_returned != NULL )
	*bytes_returned = map->rctx.bytes_returned;
    } else
    {
      ret = dspd_stream_ctl(&dspd_dctx, stream, cmd, inbuf, inbufsize, outbuf, outbufsize, bytes_returned);
    }
  return ret;
}

static int32_t dspd_ctlm_new(struct dspd_ctl_map **map, int32_t device, void *context)
{
  struct dspd_ctl_map *m;
  int32_t ret;
  ret = dspd_daemon_ref(device, DSPD_DCTL_ENUM_TYPE_SERVER);
  if ( ret == 0 )
    {
      m = calloc(1, sizeof(*m));
      if ( m )
	{
	  m->rctx.ops = &ctlm_rcb;
	  m->rctx.user_data = m;
	  m->device = device;
	  m->refcnt = 1;
	  m->context = context;
	  ret = pthread_mutex_init(&m->lock, NULL);
	  if ( ret )
	    {
	      ret *= -1;
	      free(m);
	    } else
	    {
	      *map = m;
	    }
	} else
	{
	  ret = -ENOMEM;
	}
      if ( ret < 0 )
	dspd_daemon_unref(device);
    }
  return ret;
}

static void dspd_ctlm_ref(struct dspd_ctl_map *map)
{
  pthread_mutex_lock(&map->lock);
  assert(map->refcnt > 0);
  map->refcnt++;
  pthread_mutex_unlock(&map->lock);
}


static void dspd_ctlm_unref(struct dspd_ctl_map *map, bool locked)
{
  bool dead;
  if ( ! locked )
    pthread_mutex_lock(&map->lock);
  assert(map->refcnt > 0);
  map->refcnt--;
  dead = ! map->refcnt;
  pthread_mutex_unlock(&map->lock);
  if ( dead )
    {
      dspd_daemon_unref(map->device);
      free_ctl(map);
      pthread_mutex_destroy(&map->lock);
      free(map);
    }
}

struct dspd_ctlm_list {
  struct dspd_ctl_map *maps[DSPD_MAX_OBJECTS];
  pthread_mutex_t      lock;
};

static struct dspd_ctlm_list ctl_list = {
  .maps = { NULL },
  .lock = PTHREAD_MUTEX_INITIALIZER
};


int32_t oss_mixer_ctl(void                *context,
		      int32_t              stream,
		      int32_t              cmd,
		      const void          *inbuf,
		      size_t               inbufsize,
		      void                *outbuf,
		      size_t               outbufsize,
		      size_t              *bytes_returned)

{
  int32_t ret = -EINVAL;
  struct dspd_ctl_map *map;
  if ( stream >= 0 && stream < DSPD_MAX_OBJECTS )
    {
      pthread_mutex_lock(&ctl_list.lock);
      map = ctl_list.maps[stream];
      if ( map )
	{
	  dspd_ctlm_ref(map);
	  pthread_mutex_unlock(&ctl_list.lock);
	  pthread_mutex_lock(&map->lock);
	  ret = dspd_ctlm_ctl(map, stream, cmd, inbuf, inbufsize, outbuf, outbufsize, bytes_returned);
	  dspd_ctlm_unref(map, true);
	} else
	{
	  pthread_mutex_unlock(&ctl_list.lock);
	  ret = dspd_stream_ctl(context, stream, cmd, inbuf, inbufsize, outbuf, outbufsize, bytes_returned);
	}
    }
  return ret;
}

int32_t oss_mixer_add_devmap(int32_t device, void *context)
{
  struct dspd_ctl_map *oldmap;
  int32_t ret;
  assert(device >= 0 && device < DSPD_MAX_OBJECTS);
  pthread_mutex_lock(&ctl_list.lock);
  oldmap = ctl_list.maps[device];
  ctl_list.maps[device] = NULL;
  ret = dspd_ctlm_new(&ctl_list.maps[device], device, context);
  pthread_mutex_unlock(&ctl_list.lock);
  if ( oldmap )
    dspd_ctlm_unref(oldmap, false);
  return ret;
}

void oss_mixer_remove_devmap(int32_t device)
{
  struct dspd_ctl_map *map;
  assert(device >= 0 && device < DSPD_MAX_OBJECTS);
  pthread_mutex_lock(&ctl_list.lock);
  map = ctl_list.maps[device];
  ctl_list.maps[device] = NULL;
  pthread_mutex_unlock(&ctl_list.lock);
  if ( map )
    dspd_ctlm_unref(map, false);
}
