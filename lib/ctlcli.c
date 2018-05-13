/*
 *   DSPD Asynchronous Mixer Control
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

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "sslib.h"


struct dspd_cc_elem {
  struct dspd_mix_info info;        
  int32_t              events;
  bool                 changed;
  bool                 removed;
  bool                 init;
  ssize_t              real_element;
};

struct dspd_cc_aio_op {
  struct dspd_async_op    op;
  bool                    busy;
  union {
    dspd_cc_elem_getset_cb_t    getset;
    dspd_cc_elem_getrange_cb_t  getrange;
    dspd_cc_subscribe_cb_t      subscribed;
    dspd_cc_enum_info_cb_t      enum_info;
    void                       *addr;
  } callback;
  void                   *arg;
  struct dspd_ctl_client *cli;
  void                   *user_buf;
  union {
    struct dspd_mix_info      info;
    struct dspd_mix_enum_idx  idx;
    struct dspd_mix_val       val;
    uint32_t                  u32;
    struct dspd_async_event   evt;
  } in;
  union {
    struct dspd_mix_info      info;
    struct dspd_mix_val       val;
    struct dspd_mix_range     range;
    uint32_t                  u32;
  } out;
};

union dspd_cc_alloc {
  struct dspd_cc_elem   elem;
  //struct dspd_cc_aio_op op;
};
#define UPDATE_SERIAL
struct dspd_ctl_client {
  int32_t                  device;
  int32_t                  error;

  bool                     element_map;


  //Elements
  struct dspd_cc_elem    **element_list;
  size_t                   element_count;
  size_t                   element_list_max;


  struct dspd_aio_ctx     *ioctx;
 

  dspd_cc_elem_change_cb_t   change_cb;
  void                      *change_arg;

    

  struct dspd_cc_aio_op  **pending_ops;
  size_t                   pending_ops_count;
  size_t                   max_pending_ops;
  
  struct dspd_cc_elem       pending_element;
  uint32_t                  pending_elements_count;
  uint32_t                  pending_elements_offset;
  int32_t                   pending_element_pos;
#define PENDING_ELEMENT_STATE_IDLE 0
#define PENDING_ELEMENT_STATE_GETCOUNT 1
#define PENDING_ELEMENT_STATE_GETLIST 2
  volatile uint32_t         pending_elements_state;
  dspd_cc_list_cb_t         get_list_complete;
  void                     *get_list_complete_arg;
  struct dspd_async_op      pending_elements_op;
  uint32_t                 *pending_elements_userbuf;
  bool                      retry_values;
  bool                      retry_refresh;

  dspd_aio_event_cb_t       prevcb;
  void                     *prevarg;

  union dspd_cc_alloc      *alloc_list;
  size_t                    alloc_count;
  uint8_t                  *alloc_mask;
  bool                      scale_pct;
};

static void *cc_alloc(struct dspd_ctl_client *cli)
{
  size_t i;
  for ( i = 0; i < cli->alloc_count; i++ )
    {
      if ( dspd_test_bit(cli->alloc_mask, i) == 0 )
	{
	  dspd_set_bit(cli->alloc_mask, i);
	  return &cli->alloc_list[i];
	}
    }
  return calloc(1, sizeof(union dspd_cc_alloc));
}
static void cc_free(struct dspd_ctl_client *cli, void *ptr)
{
  size_t idx, max_addr = (size_t)&cli->alloc_list[cli->alloc_count];
  if ( (size_t)ptr >= (size_t)cli->alloc_list && (size_t)ptr < max_addr )
    {
      idx = ((size_t)ptr - (size_t)cli->alloc_list) / sizeof(cli->alloc_list[0]);
      assert(idx < cli->alloc_count);
      assert(dspd_test_bit(cli->alloc_mask, idx) != 0);
      dspd_clr_bit(cli->alloc_mask, idx);
    } else
    {
      free(ptr);
    }
}

static void do_callback(struct dspd_ctl_client *cli, uint32_t err, size_t elem, int32_t evt, const struct dspd_mix_info *info)
{
  if ( cli->change_cb )
    {
      if ( cli->error < 0 && err == 0 )
	err = cli->error;
      cli->change_cb(cli, cli->change_arg, err, elem, evt, info);
    }
}



static int32_t get_type(struct dspd_cc_elem *e)
{
  size_t i;
  int32_t ret = 0;
  for ( i = 0; i < (sizeof(e->info.flags) * 8UL); i++ )
    {
      ret = e->info.flags & (1U << i);
      if ( ret )
	break;
    }
  return ret;
}

static struct dspd_cc_aio_op *alloc_op(struct dspd_ctl_client *cli)
{
  size_t i;
  struct dspd_cc_aio_op *ret = NULL;
  if ( cli->pending_ops_count < cli->max_pending_ops )
    {
      for ( i = 0; i < cli->max_pending_ops; i++ )
	{
	  if ( cli->pending_ops[i] == NULL )
	    {
	      cli->pending_ops[i] = calloc(1, sizeof(struct dspd_cc_aio_op));
	      if ( ! cli->pending_ops[i] )
		break;
	    }
	  if ( cli->pending_ops[i]->busy == false )
	    {
	      ret = cli->pending_ops[i];
	      memset(ret, 0, sizeof(*ret));
	      ret->busy = true;
	      ret->cli = cli;
	      ret->op.stream = cli->device;
	      ret->op.data = ret;
	      ret->op.inbuf = &ret->in;
	      ret->op.outbuf = &ret->out;
	      cli->pending_ops_count++;
	      break;
	    }
	}
    }
  return ret;
}

static void free_op(struct dspd_cc_aio_op *op)
{
  op->busy = false;
  op->cli->pending_ops_count--;
}

static struct dspd_cc_elem *find_elem(struct dspd_ctl_client *cli, size_t index)
{
  struct dspd_cc_elem *e = NULL;
  if ( index < cli->element_count )
    {
      e = cli->element_list[index];
      if ( e->removed )
	e = NULL;
    }
  return e;
}

static int32_t submit_io(struct dspd_ctl_client *cli, struct dspd_cc_aio_op *op)
{
  int32_t ret;
  assert(op->op.complete != NULL);
  ret = dspd_aio_submit(cli->ioctx, &op->op);
  //The op is freed here if completing synchronously due to no callback or an error
  //starting the operation.  Otherwise, the op should be freed in the callback.
  if ( ret < 0 )
    {
      free_op(op);
    } else if ( ret == 0 )
    {
      if ( ! op->callback.addr )
	{
	  //No callback, so complete synchronously.
	  while ( op->op.error > 0 )
	    {
	      ret = dspd_aio_process(cli->ioctx, 0, -1);
	      if ( ret < 0 && ret != -EINPROGRESS )
		{
		  if ( op->op.error > 0 )
		    (void)dspd_aio_cancel(cli->ioctx, &op->op, false);
		  break;
		}
	    }
	  if ( ret == 0 || ret == -EINPROGRESS )
	    ret = op->op.error;
	  assert(op->op.error <= 0);
	  free_op(op);
	} else
	{
	  ret = -EINPROGRESS;
	}
    }
  return ret;
}



static void remove_device(struct dspd_ctl_client *cli)
{
  cli->error = -ENODEV;
  do_callback(cli, cli->error, -1, DSPD_CTL_EVENT_MASK_REMOVE, NULL);
}



static void internal_refresh_cb(struct dspd_ctl_client *cli, void *arg, int32_t error, uint32_t count)
{
  return; //Dummy function to allow asynchronous operations for internal use
}
static void dspd_ctlcli_async_event(struct dspd_aio_ctx    *context,
				    void                   *arg,
				    uint32_t                req,
				    int32_t                 stream,
				    int32_t                 flags,
				    const struct dspd_async_event *evt,
				    const void             *buf,
				    size_t                  len)
{
  struct dspd_ctl_client *cli = arg;
  const struct socksrv_ctl_event *ctl;
  int32_t ret = -EBUSY;
  struct dspd_cc_elem *e;
  bool do_refresh = false;

  if ( cli->prevcb )
    cli->prevcb(context, cli->prevarg, req, stream, flags, evt, buf, len);

  //the flags arg will indicate overflow
  do_refresh = !!(flags & DSPD_REQ_FLAG_OVERFLOW);

  if ( req == DSPD_DCTL_ASYNC_EVENT && len == sizeof(*ctl) )
    {
      
      ctl = buf;
      
      switch(evt->event)
	{
	case DSPD_EVENT_HOTPLUG:
	  if ( ctl->card == cli->device )
	    remove_device(cli);
	  break;
	case DSPD_EVENT_CONTROL:
	  if ( ctl->card == cli->device )
	    {
	      if ( ctl->elem == SS_DEV_REMOVE )
		{
		  //The object being watched was removed
		  remove_device(cli);
		} else if ( ctl->elem >= 0 )
		{
		  //DSPD_CTL_EVENT_MASK_x changed
		  //TODO: Handle INFO change
		  if ( ctl->mask == DSPD_CTL_EVENT_MASK_REMOVE || (ctl->mask & DSPD_CTL_EVENT_MASK_ADD) || (ctl->mask & DSPD_CTL_EVENT_MASK_OVERFLOW) )
		    {
		      if ( (ctl->mask & DSPD_CTL_EVENT_MASK_OVERFLOW) && ctl->mask != DSPD_CTL_EVENT_MASK_REMOVE )
			cli->retry_values = true;
		      do_refresh = true;
		    } else
		    {
		      e = find_elem(cli, ctl->elem);
		      if ( e )
			do_callback(cli, 0, ctl->elem, ctl->mask, &e->info);
		    }
		}
	    }
	  break;
	}
    }
  if ( do_refresh )
    {
      if ( cli->pending_elements_state != PENDING_ELEMENT_STATE_IDLE )
	{
	  cli->retry_refresh = true;
	} else
	{
	  ret = dspd_ctlcli_refresh_count(cli, NULL, internal_refresh_cb, cli);
	  if ( ret < 0 )
	    {
	      if ( ret == -EAGAIN )
		cli->error = -EIO;
	      else if ( ret != -EINPROGRESS )
		cli->error = ret;
	    }
	}
    }

}




int32_t dspd_ctlcli_set_event_cb(struct dspd_ctl_client *cli, dspd_cc_elem_change_cb_t callback, void *arg)
{
  cli->change_cb = callback;
  cli->change_arg = arg;
  return 0;
}

static void subscribe_complete(void *context, struct dspd_async_op *op)
{
  struct dspd_cc_aio_op *o = op->data;
  uint32_t qlen = 0;

  if ( op->error == 0 )
    {
      if ( op->xfer < sizeof(qlen) )
	{
	  op->error = -EPROTO;
	} else
	{
	  qlen = *(uint32_t*)op->outbuf;
	  if ( o->user_buf )
	    memcpy(o->user_buf, &qlen, sizeof(qlen));
	}
    }
 
  if ( o->callback.subscribed )
    {
      o->callback.subscribed(o->cli, o->arg, op, qlen);
      free_op(o);
    }
}

int32_t dspd_ctlcli_subscribe(struct dspd_ctl_client *cli, bool subscribe, uint32_t *qlen, dspd_cc_subscribe_cb_t complete, void *arg)
{
  struct dspd_cc_aio_op *op;
  int32_t ret = -EAGAIN;
  if ( cli->error < 0 )
    {
      ret = cli->error;
    } else if ( qlen == NULL && complete == NULL )
    {
      ret = -EINVAL;
    } else
    {
      op = alloc_op(cli);
      if ( op )
	{
	  op->callback.subscribed = complete;
	  op->arg = arg;
	  op->user_buf = qlen;
	  op->in.evt.event = DSPD_EVENT_SETFLAGS;
	  if ( subscribe )
	    {
	      op->in.evt.flags = DSPD_EVENT_FLAG_CONTROL;
	      if ( cli->device == 0 )
		op->in.evt.flags |= DSPD_EVENT_FLAG_VCTRL;
	    } else
	    {
	      op->in.evt.flags = 0;
	    }
	  op->op.stream = DSPD_STREAM_SOCKSRV;
	  op->op.req = DSPD_SOCKSRV_REQ_EVENT;
	  op->op.inbufsize = sizeof(op->in.evt);
	  op->op.outbufsize = sizeof(op->out.u32);
	  op->op.complete = subscribe_complete;
	  ret = submit_io(cli, op);
	}
    }
  return ret;
}

static struct dspd_cc_elem *alloc_element(struct dspd_ctl_client *cli)
{
  return cc_alloc(cli);
}
static void free_element(struct dspd_ctl_client *cli, struct dspd_cc_elem *elem)
{
  cc_free(cli, elem);
}

static void free_list(struct dspd_ctl_client *cli, struct dspd_cc_elem **list, size_t count)
{
  size_t i;
  if ( list && count )
    {
      for ( i = 0; i < count; i++ )
	free_element(cli, list[i]);
      free(list);
    }
}



static int32_t realloc_element_list(struct dspd_ctl_client *cli, size_t count)
{
  void *p;
  size_t c, elem_count = cli->element_count;
  int32_t ret = 0;
  if ( (count > cli->element_list_max) || 
       (count < elem_count && cli->element_list_max > (DSPD_MAX_OBJECTS*4UL) &&
	count < (cli->element_list_max / 2UL)) )
    {
      c = count * 2UL;
      p = dspd_reallocz(cli->element_list,
			c * sizeof(cli->element_list[0]),
			cli->element_count * sizeof(cli->element_list[0]),
			false);
      if ( p )
	{
	  cli->element_list = p;
	  cli->element_list_max = c;
	} else
	{
	  ret = -ENOMEM;
	}
    }
  return ret;
}

static int32_t sync_one_element(struct dspd_ctl_client *cli, const struct dspd_cc_elem *elem, size_t index)
{
  struct dspd_cc_elem *e, *old;
  int32_t ret = 0;
  if ( index >= cli->element_count )
    {
      ret = realloc_element_list(cli, cli->element_count+1UL);
      if ( ret == 0 )
	{
	  e = alloc_element(cli);
	  if ( e != NULL )
	    {
	      *e = *elem;
	      cli->element_list[index] = e;
	      cli->element_count++;
	      e->init = true;
	      if ( cli->element_map == false )
		e->real_element = cli->pending_element_pos;
	      else
		e->real_element = e->info.hwinfo;
	      do_callback(cli, 0, index, DSPD_CTL_EVENT_MASK_ADD, &e->info);
	    }
	}
    } else 
    {
      old = cli->element_list[index];
      if ( old->info.tstamp == elem->info.tstamp && 
	   old->info.flags == elem->info.flags &&
	   strcmp(old->info.name, elem->info.name) == 0 )
	{
	  if ( cli->retry_values == true || old->info.update_count != elem->info.update_count )
	    {
	      old->info.update_count = elem->info.update_count;
	      do_callback(cli, 0, index, DSPD_CTL_EVENT_MASK_VALUE, &old->info);
	    }
	} else
	{
	  old->removed = true;
	  do_callback(cli, 0, index, DSPD_CTL_EVENT_MASK_REMOVE, &old->info);
	  *old = *elem;
	  old->init = true;
	  if ( cli->element_map == false )
	    old->real_element = cli->pending_element_pos;
	  else
	    old->real_element = old->info.hwinfo;
	  do_callback(cli, 0, index, DSPD_CTL_EVENT_MASK_ADD, &old->info);
	}
    }
  return ret;
}

static int32_t remove_elements(struct dspd_ctl_client *cli, size_t newcount)
{
  size_t i, c = cli->element_count;
  struct dspd_cc_elem *e;
  int32_t ret;
  for ( i = c; i > newcount; i-- )
    {
      e = cli->element_list[i-1UL];
      e->removed = true;
      cli->element_count--;
      do_callback(cli, 0, i, DSPD_CTL_EVENT_MASK_REMOVE, &e->info);
      free_element(cli, e);
      cli->element_list[i-1UL] = NULL;
    }
  cli->element_count = c;
  ret = realloc_element_list(cli, newcount);
  cli->element_count = newcount;
  return ret;
}


static void refresh_list_cb(void *context, struct dspd_async_op *op);
static int32_t get_next_element(struct dspd_ctl_client *cli, struct dspd_async_op *op)
{
  memset(op, 0, sizeof(*op));
  cli->pending_element_pos = cli->pending_elements_offset;
  op->stream = cli->device;
  op->req = DSPD_SCTL_SERVER_MIXER_ELEM_INFO;
  op->inbuf = &cli->pending_element_pos;
  op->inbufsize = sizeof(cli->pending_element_pos);
  op->outbuf = &cli->pending_element;
  op->outbufsize = sizeof(cli->pending_element);
  op->data = cli;
  op->complete = refresh_list_cb;
  return dspd_aio_submit(cli->ioctx, op);
}


static void refresh_list_cb(void *context, struct dspd_async_op *op)
{
  struct dspd_ctl_client *cli = op->data;
  int32_t ret;

  switch(cli->pending_elements_state)
    {
    case PENDING_ELEMENT_STATE_IDLE:
      return;
    case PENDING_ELEMENT_STATE_GETCOUNT:
      if ( op->error < 0 )
	{
	  cli->pending_elements_state = PENDING_ELEMENT_STATE_IDLE;
	} else
	{
	  if ( cli->pending_elements_count < cli->element_count )
	    op->error = remove_elements(cli, cli->pending_elements_count);
	  cli->pending_elements_offset = 0;
	  if ( op->error < 0 )
	    {
	      //Aborted
	      cli->pending_elements_count = 0;
	      cli->pending_elements_state = PENDING_ELEMENT_STATE_IDLE;
	      break;
	    } else
	    {
	      if ( cli->pending_elements_count == 0 )
		{
		  //No elements.  Elements might be added later but none exist right now.
		  cli->pending_elements_state = PENDING_ELEMENT_STATE_IDLE;
		  break;
		}

	      cli->pending_elements_state = PENDING_ELEMENT_STATE_GETLIST;
	      ret = get_next_element(cli, op);
	      if ( ret < 0 )
		{
		  //Aborted
		  if ( op->error != -EIDRM )
		    op->error = ret;
		  cli->pending_elements_state = PENDING_ELEMENT_STATE_IDLE;
		}
	    }
	}
      break;
    case PENDING_ELEMENT_STATE_GETLIST:
      if ( op->error == 0 )
	{
	  if ( cli->pending_element_pos >= 0 )
	    {
	      op->error = sync_one_element(cli, &cli->pending_element, cli->pending_element_pos);
	      if ( op->error < 0 )
		{
		  cli->pending_elements_state = PENDING_ELEMENT_STATE_IDLE; //Aborted
		  break;
		}
	    }
	    
	  if ( cli->pending_elements_offset < cli->pending_elements_count )
	    {
	      ret = get_next_element(cli, op);
	      if ( ret < 0 )
		{
		  //Aborted
		  if ( op->error != -EIDRM )
		    op->error = ret;
		  cli->pending_elements_state = PENDING_ELEMENT_STATE_IDLE;
		  break;
		}
	      cli->pending_elements_offset++;
	    } else if ( cli->pending_elements_offset == cli->pending_elements_count )
	    {
	      cli->pending_elements_state = PENDING_ELEMENT_STATE_IDLE; //Done
	    }
	} else
	{
	  cli->pending_elements_state = PENDING_ELEMENT_STATE_IDLE; //Aborted
	}
      break;
    }

  if ( cli->pending_elements_state == PENDING_ELEMENT_STATE_IDLE )
    {
      if ( op->error == 0 )
	{
	  cli->retry_values = false;
	  if ( cli->error == 0 )
	    {
	      if ( cli->pending_elements_userbuf )
		{
		  *cli->pending_elements_userbuf = cli->element_count;
		  cli->pending_elements_userbuf = NULL;
		}
	    }
	}
      if ( op->error == -EIDRM || cli->retry_refresh == true )
	{
	  cli->pending_elements_count = 0;
	  cli->retry_refresh = false;
	  if ( cli->get_list_complete != NULL )
	    {
	      ret = dspd_ctlcli_refresh_count(cli, NULL, cli->get_list_complete, cli->get_list_complete_arg);
	      if ( ret < 0 && ret != -EINPROGRESS )
		op->error = ret;
	      else
		return;
	    } else
	    {
	      op->error = -EAGAIN;
	      return;
	    }
	}
      if ( cli->get_list_complete )
	{
	  cli->get_list_complete(cli, cli->get_list_complete_arg, op->error, cli->element_count);
	  cli->get_list_complete = NULL;
	}
      cli->pending_elements_count = 0;
    }
}


int32_t dspd_ctlcli_refresh_count(struct dspd_ctl_client *cli, uint32_t *count, dspd_cc_list_cb_t complete, void *arg)
{
  struct dspd_async_op *op = &cli->pending_elements_op;
  int32_t ret;
  if ( cli->error < 0 )
    {
      ret = cli->error;
    } else if ( op->error > 0 || cli->pending_elements_state != PENDING_ELEMENT_STATE_IDLE )
    {
      ret = -EAGAIN;
    } else
    {
      assert(op->error <= 0);
      memset(op, 0, sizeof(*op));
      cli->retry_refresh = false;
      op->stream = cli->device;
      op->req = DSPD_SCTL_SERVER_MIXER_ELEM_COUNT;
      cli->pending_elements_count = 0;
      cli->pending_elements_state = PENDING_ELEMENT_STATE_GETCOUNT;
      cli->pending_element_pos = -1;
      cli->pending_elements_userbuf = count;
      op->outbuf = &cli->pending_elements_count;
      op->outbufsize = sizeof(cli->pending_elements_count);
      op->data = cli;
      op->complete = refresh_list_cb;
      cli->get_list_complete = complete;
      cli->get_list_complete_arg = arg;
      ret = dspd_aio_submit(cli->ioctx, op);
      if ( ret == 0 )
	{
	  if ( complete == NULL )
	    {
	      while ( cli->pending_elements_state != PENDING_ELEMENT_STATE_IDLE )
		{
		  ret = dspd_aio_process(cli->ioctx, 0, -1);
		  if ( ret < 0 && ret != -EINPROGRESS )
		    {
		      (void)dspd_aio_cancel(cli->ioctx, op, false);
		      break;
		    }
		}
	    } else
	    {
	      ret = -EINPROGRESS;
	    }
	}
    }
  return ret;
}

int32_t dspd_ctlcli_elem_count(struct dspd_ctl_client *cli)
{
  int32_t ret = cli->error;
  if ( ret == 0 )
    ret = cli->element_count;
  return ret;
}

int32_t dspd_ctlcli_elem_get_info(struct dspd_ctl_client *cli, uint32_t index, struct dspd_mix_info *info)
{
  //Get info if element is valid and not removed.  May return -EIDRM.
  int32_t ret = -EIDRM;
  const struct dspd_cc_elem *e;
  if ( cli->error < 0 )
    {
      ret = cli->error;
    } else if ( index < cli->element_count )
    {
      e = cli->element_list[index];
      if ( e->init == true )
	{
	  memcpy(info, e, sizeof(*info));
	  ret = 0;
	}
    }
  return ret;
}


static void getset_int32_complete(void *context, struct dspd_async_op *op)
{
  struct dspd_cc_aio_op *o = op->data;
  struct dspd_mix_val *val = op->outbuf;
  const struct dspd_mix_val *in = op->inbuf;
  struct dspd_cc_elem *e;
  if ( op->error == 0 )
    {
      if ( op->req == DSPD_SCTL_SERVER_MIXER_GETVAL && op->xfer != sizeof(*val) )
	op->error = -EPROTO;
      if ( op->error == 0 && op->xfer == sizeof(*val) )
	{
	  e = find_elem(o->cli, val->index);
	  if ( e != NULL && e->info.tstamp == val->tstamp )
	    e->info.update_count = val->update_count;

	  if ( o->user_buf != NULL )
	    memcpy(o->user_buf, &val->value, sizeof(val->value));
	}
    }
 
  if ( o->callback.getset )
    {
      if ( op->error < 0 )
	o->callback.getset(o->cli, o->arg, op, in->index, 0);
      else
	o->callback.getset(o->cli, o->arg, op, val->index, val->value);
      free_op(o);
    }
}

void dspd_ctlcli_set_scale_pct(struct dspd_ctl_client *cli, bool en)
{
  cli->scale_pct = en;
}
bool dspd_ctlcli_get_scale_pct(struct dspd_ctl_client *cli)
{
  return cli->scale_pct;
}


int32_t dspd_ctlcli_elem_set_int32(struct dspd_ctl_client *cli, uint32_t index, int32_t channel, int32_t in, int32_t *out, dspd_cc_elem_getset_cb_t complete, void *arg)
{
  struct dspd_cc_aio_op *op;
  int32_t ret = -EAGAIN;
  struct dspd_cc_elem *e;
  if ( cli->error < 0 )
    {
      ret = cli->error;
    } else if ( (e = find_elem(cli, index)) == NULL )
    {
      //Probably removed, maybe just wrong (should be -EINVAL?)
      ret = -EIDRM;
    } else if ( e->init == false )
    {
      //Temporarily unvailable because the element hasn't been loaded (probably refreshing the list)
      ret = -EAGAIN;
    } else
    {
      op = alloc_op(cli);
      if ( op )
	{
	  op->callback.getset = complete;
	  op->arg = arg;
	  op->user_buf = out;
	  op->in.val.index = index;
	  op->in.val.type = get_type(e);
	  if ( (op->in.val.type & (DSPD_MIXF_PVOL|DSPD_MIXF_CVOL)) && cli->scale_pct )
	    op->in.val.flags = DSPD_CTRLF_SCALE_PCT;
	  op->in.val.value = in;
	  op->in.val.channel = channel;
	  op->in.val.dir = 0;
	  op->in.val.tstamp = e->info.tstamp;
	  op->op.req = DSPD_SCTL_SERVER_MIXER_SETVAL;
	  op->op.inbufsize = sizeof(op->in.val);
	  if ( op->user_buf )
	    op->op.outbufsize = sizeof(op->out.val);
	  else
	    op->op.outbuf = NULL;
	  op->op.complete = getset_int32_complete;
	  ret = submit_io(cli, op);
	}
    }
  return ret;
}





int32_t dspd_ctlcli_elem_get_int32(struct dspd_ctl_client *cli, 
				   uint32_t index, 
				   int32_t channel, 
				   int32_t *val, 
				   dspd_cc_elem_getset_cb_t complete, 
				   void *arg)
{
  struct dspd_cc_aio_op *op;
  int32_t ret = -EAGAIN;
  struct dspd_cc_elem *e;
  if ( cli->error < 0 )
    {
      ret = cli->error;
    } else if ( val == NULL && complete == NULL )
    {
      ret = -EINVAL;
    } else if ( (e = find_elem(cli, index)) == NULL )
    {
      //Probably removed, maybe just wrong (should be -EINVAL?)
      ret = -EIDRM;
    } else if ( e->init == false )
    {
      //Temporarily unvailable because it hasn't been loaded (probably refreshing the list)
      ret = -EAGAIN;
    } else
    {
      op = alloc_op(cli);
      if ( op )
	{
	  op->callback.getset = complete;
	  op->arg = arg;
	  op->user_buf = val;
	  op->in.val.index = index;
	  op->in.val.type = get_type(e);
	  if ( (op->in.val.type & (DSPD_MIXF_PVOL|DSPD_MIXF_CVOL)) && cli->scale_pct )
	    op->in.val.flags = DSPD_CTRLF_SCALE_PCT;
	  op->in.val.value = 0;
	  op->in.val.channel = channel;
	  op->in.val.dir = 0;
	  op->in.val.tstamp = e->info.tstamp;
	  op->op.req = DSPD_SCTL_SERVER_MIXER_GETVAL;
	  op->op.inbufsize = sizeof(op->in.val);
	  op->op.outbufsize = sizeof(op->out.val);
	  op->op.complete = getset_int32_complete;
	  ret = submit_io(cli, op);
	}
    }
  return ret;
}
static void getrange_complete(void *context, struct dspd_async_op *op)
{
  struct dspd_cc_aio_op *o = op->data;
  struct dspd_mix_range *r = NULL;
  if ( op->error == 0 )
    {
      if ( op->xfer >= sizeof(*r) )
	{
	  r = op->outbuf;
	  if ( o->user_buf )
	    memcpy(o->user_buf, r, sizeof(*r));
	} else
	{
	  op->error = -EPROTO;
	}
    }
  if ( o->callback.getrange )
    {
      o->callback.getrange(o->cli, o->arg, op, r);
      free_op(o);
    }
}


int32_t dspd_ctlcli_elem_get_range(struct dspd_ctl_client *cli, 
				   uint32_t index, 
				   struct dspd_mix_range *range, 
				   dspd_cc_elem_getrange_cb_t complete, 
				   void *arg)
{
  struct dspd_cc_aio_op *op;
  int32_t ret = -EAGAIN;
  struct dspd_cc_elem *e;
  if ( cli->error < 0 )
    {
      ret = cli->error;
    } else if ( range == NULL && complete == NULL )
    {
      ret = -EINVAL;
    } else if ( (e = find_elem(cli, index)) == NULL )
    {
      //Probably removed, maybe just wrong (should be -EINVAL?)
      ret = -EIDRM;
    } else if ( e->init == false )
    {
      //Temporarily unvailable because it hasn't been loaded (probably refreshing the list)
      ret = -EAGAIN;
    } else
    {
      op = alloc_op(cli);
      if ( op )
	{
	  op->callback.getrange = complete;
	  op->arg = arg;
	  op->user_buf = range;
	  op->in.val.index = index;
	  op->in.val.type = get_type(e);
	  op->in.val.tstamp = e->info.tstamp;
	  op->op.req = DSPD_SCTL_SERVER_MIXER_GETRANGE;
	  op->op.inbufsize = sizeof(op->in.val);
	  op->op.outbufsize = sizeof(op->out.range);
	  op->op.complete = getrange_complete;
	  ret = submit_io(cli, op);
	}
    }
  return ret;
}

static void enum_info_complete(void *context, struct dspd_async_op *op)
{
  struct dspd_cc_aio_op *o = op->data;
  struct dspd_mix_info *info = NULL;
  if ( op->error == 0 )
    {
      if ( op->xfer == sizeof(*info) )
	{
	  info = op->outbuf;
	  if ( o->user_buf )
	    memcpy(o->user_buf, info, sizeof(*info));
	} else
	{
	  op->error = -EPROTO;
	}
    }
  if ( o->callback.enum_info )
    {
      o->callback.enum_info(o->cli, o->arg, op, info);
      free_op(o);
    }
}

int32_t dspd_ctlcli_elem_get_enum_info(struct dspd_ctl_client *cli, 
				       uint32_t elem_index, 
				       uint32_t enum_index,
				       struct dspd_mix_info *info,
				       dspd_cc_elem_getrange_cb_t complete, 
				       void *arg)
{
  struct dspd_cc_aio_op *op;
  int32_t ret = -EAGAIN;
  struct dspd_cc_elem *e;
  if ( cli->error < 0 )
    {
      ret = cli->error;
    } else if ( info == NULL && complete == NULL )
    {
      ret = -EINVAL;
    } else if ( (e = find_elem(cli, elem_index)) == NULL )
    {
      //Probably removed, maybe just wrong (should be -EINVAL?)
      ret = -EIDRM;
    } else if ( e->init == false )
    {
      //Temporarily unvailable because it hasn't been loaded (probably refreshing the list)
      ret = -EAGAIN;
    } else
    {
      op = alloc_op(cli);
      if ( op )
	{
	  op->callback.getrange = complete;
	  op->arg = arg;
	  op->user_buf = info;
	  op->in.idx.elem_idx = elem_index;
	  op->in.idx.enum_idx = enum_index;
	  op->op.req = DSPD_SCTL_SERVER_MIXER_GETRANGE;
	  op->op.inbufsize = sizeof(op->in.idx);
	  op->op.outbufsize = sizeof(op->out.info);
	  op->op.complete = enum_info_complete;
	  ret = submit_io(cli, op);
	}
    }
  return ret;
}

int32_t dspd_ctlcli_init(struct dspd_ctl_client *cli, 
			 ssize_t iocount_hint,
			 size_t max_elem_hint)
{
  int32_t ret = 0;
  size_t i;
  memset(cli, 0, sizeof(*cli));
  if ( iocount_hint == DSPD_CC_IO_SYNC )
    {
      cli->max_pending_ops = 4UL;
    } else if ( iocount_hint == DSPD_CC_IO_DEFAULT )
    {
      cli->max_pending_ops = DSPD_MAX_OBJECTS + 1UL;
    } else
    {
      if ( iocount_hint < 4L )
	cli->max_pending_ops = 4L;
      else
	cli->max_pending_ops = iocount_hint;
    }
  cli->device = 1;
  cli->pending_ops = calloc(cli->max_pending_ops, sizeof(struct dspd_cc_aio_op*));

  if ( max_elem_hint == 0 )
    max_elem_hint = DSPD_MAX_OBJECTS;
  max_elem_hint = 1 << get_hpo2(max_elem_hint);
  max_elem_hint *= 2UL;
  if ( max_elem_hint > 32768UL )
    max_elem_hint = 32768UL;
  cli->element_list_max = max_elem_hint;
  cli->element_list = calloc(cli->element_list_max, sizeof(struct dspd_cc_elem*));
  cli->alloc_list = calloc(max_elem_hint, sizeof(cli->alloc_list[0]));
  cli->alloc_mask = calloc(max_elem_hint / 8UL, sizeof(cli->alloc_mask[0]));
  if ( ! (cli->pending_ops && cli->element_list && cli->alloc_list && cli->alloc_mask) )
    {
      dspd_ctlcli_destroy(cli);
      ret = -ENOMEM;
    } else
    {
      for ( i = 0; i < cli->max_pending_ops; i++ )
	cli->pending_ops[i] = calloc(1, sizeof(struct dspd_cc_aio_op));
    }
  return ret;
}

void dspd_ctlcli_bind(struct dspd_ctl_client *cli, struct dspd_aio_ctx *aio, int32_t device)
{
  assert(cli->ioctx == NULL);
  cli->device = device;
  cli->ioctx = aio;
  dspd_aio_get_event_cb(aio, &cli->prevcb, &cli->prevarg);
  dspd_aio_set_event_cb(aio, dspd_ctlcli_async_event, cli);
}


size_t dspd_ctlcli_sizeof(void)
{
  return sizeof(struct dspd_ctl_client);
}

int32_t dspd_ctlcli_new(struct dspd_ctl_client **cli, 
			ssize_t max_io_hint,
			size_t max_elem_hint)
{
  struct dspd_ctl_client *c;
  int32_t ret;
  c = malloc(dspd_ctlcli_sizeof());
  if ( c )
    {
      ret = dspd_ctlcli_init(c, max_io_hint, max_elem_hint);
      if ( ret < 0 )
	free(c);
      else
	*cli = c;
    } else
    {
      ret = -errno;
    }
  return ret;
}



void dspd_ctlcli_delete(struct dspd_ctl_client *cli)
{
  dspd_ctlcli_destroy(cli);
  free(cli);
}

void dspd_ctlcli_destroy(struct dspd_ctl_client *cli)
{
  size_t i;
  if ( cli )
    {
      for ( i = 0; i < cli->max_pending_ops; i++ )
	{
	  if ( cli->pending_ops[i] )
	    {
	      if ( cli->pending_ops[i]->busy )
		dspd_aio_cancel(cli->ioctx, &cli->pending_ops[i]->op, false);
	      assert(cli->pending_ops[i]->busy == false);
	      free(cli->pending_ops[i]);
	    }
	}
      free(cli->pending_ops);
      cli->pending_ops = NULL;
      if ( cli->ioctx )
	{
	  dspd_aio_set_event_cb(cli->ioctx, cli->prevcb, cli->prevarg);
	  cli->ioctx = NULL;
	}
      free_list(cli, cli->element_list, cli->element_count);
      cli->element_list = NULL;
      free(cli->alloc_list);
      cli->alloc_list = NULL;
      free(cli->alloc_mask);
      cli->alloc_mask = NULL;
    }
}




