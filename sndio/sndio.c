/*
 *   DSPD sndio protocol server
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

/*This module is compatible with sndio-1.0.1*/

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#define _DSPD_HAVE_UCRED
#include "../lib/socket.h"
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../lib/cbpoll.h"
#include "sndio.h"
#include "amsg.h"
#include "defs.h"
#include "dspd_sndio.h"

#define USE_CBTIMER

#define MAX_CLIENTS 32

#define ENABLE_CTL 

union sockaddr_gen {
  struct sockaddr_un un;
  struct sockaddr_in in;
  struct sockaddr_in6 in6;
};

struct sndio_pkt {
  struct amsg msg;
  char        buf[AMSG_DATAMAX+sizeof(struct amsg)];
};


void dspd_sndio_delete(struct sndio_ctx *ctx);

#define CLIENT_MSG_ERROR    (CBPOLL_PIPE_MSG_USER+1)
#define CLIENT_MSG_COMPLETE (CBPOLL_PIPE_MSG_USER+2)

struct sndio_client {
  struct cbpoll_client_hdr header;
  struct dspd_rclient *pclient, *cclient;
  int32_t pclient_idx;
  struct cbpoll_ctx  *cbctx;
  struct sndio_ctx   *server;
  struct dspd_cbtimer *timer;


  //No work to do (waiting on POLLIN)
#define CLIENT_STATE_IDLE   0
  //Receiving packet (struct amsg)
#define CLIENT_STATE_RXPKT  1

  //Receiving PCM data.  Note that sending more than the server is ready for
  //is a fatal error.
#define CLIENT_STATE_RXDATA 2
  //Transmitting packet (any type)
#define CLIENT_STATE_TXPKT  3

#define CLIENT_STATE_BUSY   4

  int32_t               cstate;
  int32_t               sio_idx;
  int32_t               fd;
  int32_t               streams;

  //New connection
#define PROTO_OPEN 0
  //Authenticated
#define PROTO_AUTH 1
  //HELLO OK
#define PROTO_INIT 2
  //Setpar was called (maybe stopped, maybe not started)
#define PROTO_CONFIGURED 3
  //Playback or capture started
#define PROTO_TRIGGERED  4
  int32_t               pstate;

  struct amsg           imsg;
  size_t                offset_in;
  
  struct sndio_pkt      opkt;
  size_t                offset_out, len_out;

  size_t      pframe_bytes, cframe_bytes;
  size_t      p_offset;
  size_t      p_len;
  size_t      p_max, p_total;
  char        p_data[AMSG_DATAMAX];
  size_t      p_avail;

  struct dspd_cli_params pparams, cparams;
  uint8_t xrun_policy;
  bool par_set;
  struct dspd_sg_info syncgroup;
  bool   running;
  size_t frames_written;
  size_t start_threshold;
  
  uint32_t write_window;
  int32_t  last_delay;
  uint32_t last_fill;
  uint64_t appl_ptr;
  uint64_t hw_ptr;

  dspd_time_t tstamp;
  size_t      flow_control_pending;
  int32_t sample_time;
  uint32_t delta;
  
  bool draining;

    
  bool vol_changed;
  bool vol_pending;
  bool vol_update;
  uint32_t volume;
  int32_t vol_elem;
};


#define SERVER_MSG_ADDCLI (CBPOLL_PIPE_MSG_USER+1)
#define SERVER_MSG_DELCLI (CBPOLL_PIPE_MSG_USER+2)



struct sndio_ctx {
  struct cbpoll_client_list list;
  size_t nclients;

  struct cbpoll_ctx cbctx;
  int fd;
  int cbidx;
  uint8_t cookie[AMSG_COOKIELEN];
  size_t  sessrefs;


  void             *ctx;
  char             *server_addr;
  int              *tcp_fds;
  size_t            tcp_nfds;
  bool              started;

  struct dspd_aio_ctx    *aio;
  struct dspd_ctl_client *ctl;
  struct dspd_aio_fifo_eventfd efd;

  ssize_t              cli_vol_index; //current index for getting new volume
  size_t               cli_vol_pos; //schedule position
  struct sndio_client *cli_vol_ptr; //pointer to current client for new volume
  int32_t              cli_vol;     //new volume value
  int16_t              cli_map[MAX_CLIENTS]; //map of dspd stream # to sndio client #
  int32_t              cli_vol_elem;
};

struct amsg_handler {
  int pstate;
  int32_t (*handler)(struct sndio_client *cli);
};
static int par2cli(const struct dspd_device_stat *info, 
		   int mode,
		   const struct amsg_par *par, 
		   struct dspd_cli_params *clp);
/*static int cli2par(int mode,
		   const struct dspd_cli_params *clp,
		   struct sio_par *par);*/
int client_pollout(struct sndio_client *cli);
int client_check_io(struct sndio_client *cli);
int client_check_io_fast(struct sndio_client *cli, dspd_time_t *nextwakeup);
static int send_pkt(struct sndio_client *cli, size_t len, bool async);
int client_check_buffers(struct sndio_client *cli);
int client_buildmsg(struct sndio_client *cli, bool async);
static int send_ack(struct sndio_client *cli);
static bool client_cbtimer_event(struct cbpoll_ctx *ctx, 
				 struct dspd_cbtimer *timer,
				 void *arg, 
				 dspd_time_t timeout);


static struct cbpoll_client_hdr *client_create(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr, void *arg)
{
  struct sndio_ctx *srv = (struct sndio_ctx*)hdr->list;
  struct sndio_client *cli = calloc(1, sizeof(struct sndio_client));
  if ( cli != NULL )
    {
      cli->header = *hdr;
      cli->cbctx = ctx;
      cli->server = srv;
      cli->fd = hdr->fd;
      cli->sio_idx = hdr->reserved_slot;
    }
  return (struct cbpoll_client_hdr*)cli;
}

static void client_async_destructor(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr)
{
  struct sndio_client *cli = (struct sndio_client*)hdr;
  if ( cli->pclient )
    dspd_rclient_delete(cli->pclient);
  if ( cli->cclient != NULL && cli->cclient != cli->pclient )
    dspd_rclient_delete(cli->cclient);
  free(cli);
}

static bool client_success(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr)
{
  struct sndio_client *cli = (struct sndio_client*)hdr;
  bool ret = false;
  cli->timer = dspd_cbtimer_new(ctx, client_cbtimer_event, cli);
  if ( cli->timer )
    {
      if ( hdr->list_index >= 0 && hdr->list_index >= cli->server->nclients )
	cli->server->nclients = hdr->list_index + 1UL;
      ret = true;
    }
  return ret;
}

struct cbpoll_client_ops client_list_ops = {
  .create = client_create,
  .destroy = client_async_destructor,
  .success = client_success,
};


static bool client_destructor(void *data,
			      struct cbpoll_ctx *context,
			      int index,
			      int fd)
{
  struct sndio_client *cli = data;
  size_t i; ssize_t c;
  if ( cli->pstate > 0 )
    cli->server->sessrefs--;
  if ( cli->header.list_index == (cli->server->nclients - 1UL) )
    {
      c = -1L;
      for ( i = 0; i < cli->server->nclients; i++ )
	{
	  if ( cli->server->list.clients[i] != NULL &&
	       cli->server->list.clients[i] != (struct cbpoll_client_hdr*)UINTPTR_MAX )
	    c = i;
	}
      cli->server->nclients = (size_t)(c + 1L);
    }
  if ( cli->timer )
    {
      dspd_cbtimer_delete(cli->timer);
      cli->timer = NULL;
    }
  shutdown(fd, SHUT_RDWR);
  return cbpoll_async_destructor_cb(data, context, index, fd);
}



static bool client_cbtimer_event(struct cbpoll_ctx *ctx, 
				 struct dspd_cbtimer *timer,
				 void *arg, 
				 dspd_time_t timeout)
{
  struct sndio_client *cli = arg;
  int32_t ret, s;
  size_t br;
  if ( cli->draining )
    {
      ret = dspd_rclient_avail(cli->pclient, DSPD_PCM_SBIT_PLAYBACK);
      if ( ret == -EPIPE )
	{
	  s = DSPD_PCM_SBIT_PLAYBACK;
	  ret = dspd_rclient_ctl(cli->pclient, 
				 DSPD_SCTL_CLIENT_STOP,
				 &s,
				 sizeof(s),
				 NULL,
				 0,
				 &br);
	  if ( ret < 0 || send_ack(cli) < 0 )
	    shutdown(cli->fd, SHUT_RDWR);
	  cli->draining = false;
	  return false;
	} else if ( ret < 0 )
	{
	  shutdown(cli->fd, SHUT_RDWR);
	  return false;
	}
      return true;
    }

  if ( ! cli->running )
    return false;
  if ( client_check_buffers(cli) < 0 ||
       client_buildmsg(cli, false) < 0 )
    {
      shutdown(cli->fd, SHUT_RDWR);
      return false;
    } 
  return true;
}

static void client_reset(struct sndio_client *cli)
{
   cli->running = false;
   cli->flow_control_pending = 0;
   cli->frames_written = 0;
   cli->delta = 0;
   cli->last_delay = 0;
   cli->last_fill = 0;
   cli->appl_ptr = 0;
   cli->hw_ptr = 0;
   
}

static int client_xrun(struct sndio_client *cli)
{
  int ret = 0;
  if ( cli->running == true && cli->draining == false )
    {
      if ( cli->xrun_policy == SIO_ERROR )
	{
	  shutdown(cli->fd, SHUT_RDWR);
	  ret = -1;
	} else if ( cli->xrun_policy == SIO_SYNC )
	{
	  //TODO: Implement this
	} else
	{
	  if ( cli->write_window == 0 && cli->pclient != NULL )
	    cli->flow_control_pending = cli->pparams.bufsize - cli->pparams.fragsize;
	}
    }
  return ret;
}


static void set_client_wakeup(struct sndio_client *cli, dspd_time_t *next, uint32_t *per)
{
  dspd_time_t n = 0, p = 0;
  if ( next )
    n = *next;
  if ( per )
    p = *per;
  if ( n == 0 && p == 0 )
    dspd_cbtimer_cancel(cli->timer);
  else
    dspd_cbtimer_set(cli->timer, n, p);
}






int client_check_buffers(struct sndio_client *cli)
{
  int ret = 0;
  uint32_t avail_min = 1;
  int32_t delay;
  uint32_t pdelta = 0, cdelta = 0, mdelta;
  dspd_time_t pnext = 0, cnext = 0, nextwakeup;
  int32_t avail = -1;
  if ( cli->running == true )
    {
      if ( cli->pclient )
	{
	  ret = dspd_rclient_avail_cl(cli->pclient, &avail_min, &delay);
	  if ( ret == 0 )
	    {
	      ret = dspd_rclient_get_next_wakeup_avail(cli->pclient, 
						       DSPD_PCM_SBIT_PLAYBACK,
						       avail_min, 
						       &pnext);
	      if ( ret < 0 )
		{
	
		  if ( ret == -EPIPE )
		    ret = client_xrun(cli);
		    
		}
	    } else if ( ret < 0 )
	    {
	      if ( ret == -EPIPE )
		ret = client_xrun(cli);
	      
	    } else
	    {
	      avail = ret;
	      if ( cli->streams == DSPD_PCM_SBIT_FULLDUPLEX )
		{
		  if ( ret > cli->write_window )
		    {
		      
		      //This way works best because full duplex connections need buffer feedback to read
		      //if the playback data runs out.
		      cli->flow_control_pending = ret - cli->write_window;
		    }
		} else
		{
		  /*
		    This is more efficient and prevents some clients, such as xine, from spinning.
		   */
		  if ( cli->write_window == 0 )
		    cli->flow_control_pending = ret;
		}
	      if ( delay < cli->last_delay && delay > 0 )
		{
		  pdelta = cli->last_delay - delay;

		  //This always seems to work and also tends to be more efficient.
		  if ( ret == 0 )
		    pdelta = 0;
		  else
		    cli->last_delay = delay;

		  mdelta = cli->appl_ptr - cli->hw_ptr;
		  if ( pdelta > mdelta )
		    pdelta = mdelta;
		  cli->hw_ptr += pdelta;

		}
	      ret = 0;
	    }
	}
      if ( cli->cclient && ret == 0 )
	{
	  ret = dspd_rclient_avail(cli->cclient, DSPD_PCM_SBIT_CAPTURE);
	  if ( ret == 0 )
	    {
	      ret = dspd_rclient_get_next_wakeup_avail(cli->cclient, 
						       DSPD_PCM_SBIT_CAPTURE,
						       cli->cparams.fragsize,
						       &cnext);
	      if ( ret < 0 )
		{
		  if ( ret == -EPIPE )
		    ret = client_xrun(cli);
		}
	    } else if ( ret < 0 )
	    {
	      if ( ret == -EPIPE )
		ret = client_xrun(cli);
	    } else
	    {
	      if ( cli->cparams.fragsize > avail_min )
		avail_min = cli->cparams.fragsize;
	      if ( ret > avail )
		avail = ret;
	      if ( ret > cli->last_fill )
		{
		  cdelta = ret - cli->last_fill;
		  cli->last_fill = ret;
		  ret = 0;
		}
	    }
	}
      cli->delta += MAX(pdelta, cdelta);
      if ( pnext != 0 && cnext != 0 )
	nextwakeup = MIN(pnext, cnext);
      else if ( pnext )
	nextwakeup = pnext;
      else
	nextwakeup = cnext;
      /*
	Since the server synchronized the hardware with the system timer, set_client_wakeup() 
	never gets called.  The periodic wakeup is enough to prevent xruns.
      */
      if ( nextwakeup > 0 && avail < avail_min )
	set_client_wakeup(cli, &nextwakeup, NULL);
    }
  return ret;
}

int client_buildmsg(struct sndio_client *cli, bool async)
{
  int ret = 0;
  size_t max_read;
  if ( cli->running == false || cli->cstate != CLIENT_STATE_IDLE )
    return 0;
  if ( cli->flow_control_pending )
    {
      AMSG_INIT(&cli->opkt.msg);
      cli->opkt.msg.cmd = htonl(AMSG_FLOWCTL);
      cli->write_window += cli->flow_control_pending;
      cli->opkt.msg.u.ts.delta = htonl(cli->write_window);
      cli->flow_control_pending = 0;
      ret = send_pkt(cli, sizeof(struct amsg), async);
    } else if ( cli->delta > 0 )
    {
      cli->opkt.msg.cmd = htonl(AMSG_MOVE);
      cli->opkt.msg.u.ts.delta = htonl(cli->delta);
      cli->delta = 0;
      ret = send_pkt(cli, sizeof(struct amsg), async);
    } else if ( cli->last_fill )
    {
      AMSG_INIT(&cli->opkt.msg);
      cli->opkt.msg.cmd = htonl(AMSG_DATA);
      max_read = AMSG_DATAMAX / cli->cframe_bytes;
      if ( max_read > cli->last_fill )
	max_read = cli->last_fill;
      ret = dspd_rclient_read(cli->cclient, cli->opkt.buf, max_read);
      if ( ret > 0 )
	{
	  cli->last_fill -= ret;
	  ret *= cli->cframe_bytes;
	  cli->opkt.msg.u.data.size = htonl(ret);
	  ret = send_pkt(cli, sizeof(struct amsg) + ret, async);
	}
    } else if ( cli->vol_changed )
    {
      cli->opkt.msg.cmd = htonl(AMSG_SETVOL);
      cli->opkt.msg.u.vol.ctl = htonl(cli->volume);
      cli->vol_changed = false;
      ret = send_pkt(cli, sizeof(struct amsg), async);
    } else
    {
      if ( cli->cstate == CLIENT_STATE_TXPKT )
	ret = client_pollout(cli);
    }
  return ret;
}


int client_trigger(struct sndio_client *cli)
{
  int ret;
  struct dspd_sync_cmd scmd;
  struct dspd_sync_cmd info;
  size_t br;
  uint32_t len = 0;
  dspd_time_t tstamps[2] = { 0, 0 };
  if ( cli->streams == DSPD_PCM_SBIT_FULLDUPLEX &&
       cli->pclient != cli->cclient )
    {
      
      memset(&scmd, 0, sizeof(scmd));
      scmd.cmd = SGCMD_STARTALL;
      scmd.streams = cli->streams;
      scmd.sgid = cli->syncgroup.sgid;
      ret = dspd_rclient_ctl(cli->pclient, 
			     DSPD_SCTL_CLIENT_SYNCCMD,
			     &scmd,
			     sizeof(scmd),
			     &info,
			     sizeof(info),
			     &br);
      if ( ret == 0 )
	{
	  cli->tstamp = info.tstamp;
	  len = cli->pparams.fragsize;
	}
    } else if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      ret = dspd_rclient_ctl(cli->pclient,
			     DSPD_SCTL_CLIENT_START,
			     &cli->streams,
			     sizeof(cli->streams),
			     &tstamps,
			     sizeof(tstamps),
			     &br);
      if ( ret == 0 )
	{
	  cli->tstamp = tstamps[0];
	  len = cli->pparams.fragsize;
	}
    } else
    {
      ret = dspd_rclient_ctl(cli->cclient,
			     DSPD_SCTL_CLIENT_START,
			     &cli->streams,
			     sizeof(cli->streams),
			     &tstamps,
			     sizeof(tstamps),
			     &br);
      if ( ret == 0 )
	{
	  cli->tstamp = tstamps[1];
	  len = cli->cparams.fragsize;
	}
    }
  dspd_time_t t = cli->tstamp + (len * cli->sample_time);
  uint32_t p = len * cli->sample_time;
  set_client_wakeup(cli, &t, &p);
  cli->running = true;
  return ret;
}


int client_wxfer(struct sndio_client *cli)
{
  if ( cli->cstate != CLIENT_STATE_RXDATA )
      return 0;

  ssize_t ret;
  size_t r, m, fr, offset;
  int e;
  r = cli->p_max - cli->p_total;
  m = AMSG_DATAMAX - cli->p_offset;
  if ( r > m )
    r = m;
  ret = read(cli->fd, &cli->p_data[cli->p_offset], r);


  if ( ret < 0 )
    {
      e = errno;
      if ( e == EWOULDBLOCK || e == EAGAIN || e == EINTR )
	ret = 0;
    } else if ( ret == 0 )
    {
      ret = -1;
    } else
    {
      cli->p_offset += ret;
      cli->p_total += ret;
      fr = cli->p_offset / cli->pframe_bytes;
      if ( fr > 0 )
	{
	  ret = dspd_rclient_write(cli->pclient, cli->p_data, fr);
	  if ( ret < 0 )
	    {
	      ret = -1;
	    } else if ( ret > 0 )
	    {
	      offset = ret * cli->pframe_bytes;
	      cli->p_avail -= ret;
	      if ( offset < cli->p_offset )
		memmove(cli->p_data, &cli->p_data[offset], cli->p_offset - offset);
	      cli->p_offset -= offset;
	      cli->frames_written += fr;
	      cli->appl_ptr += fr;
	      cli->write_window -= fr;
	      cli->last_delay += fr;
	      if ( cli->p_total == cli->p_max )
		{
		  cli->p_total = 0;
		  cli->p_max = 0;
		  cli->cstate = CLIENT_STATE_IDLE;
		}
	      if ( cli->running == false )
		{
		  if ( cli->frames_written >= cli->start_threshold )
		      ret = client_trigger(cli);
		} else
		{
		  ret = 0;
		}
	    }
	} else
	{
	  ret = 0;
	}
    }
  return ret;
}

static int send_pkt(struct sndio_client *cli, size_t len, bool async)
{
  int ret;
  cli->offset_out = 0;
  cli->len_out = len;
  cli->cstate = CLIENT_STATE_TXPKT;
  if ( ! async )
    {
      ret = client_pollout(cli);
      if ( ret == 0 && cli->cstate == CLIENT_STATE_TXPKT )
	ret = cbpoll_set_events(cli->cbctx, cli->header.reserved_slot, POLLOUT);
      else
	cli->cstate = CLIENT_STATE_IDLE;
    } else
    {
      ret = cbpoll_set_events(cli->cbctx, cli->header.reserved_slot, POLLOUT);
    }
  return ret;
}

static int send_ack(struct sndio_client *cli)
{
  AMSG_INIT(&cli->opkt.msg);
  cli->opkt.msg.cmd = htonl(AMSG_ACK);
  return send_pkt(cli, sizeof(cli->opkt.msg), false);
}
static int send_rmsg(struct sndio_client *cli, uint32_t cmd)
{
  cli->opkt.msg.cmd = htonl(cmd);
  return send_pkt(cli, sizeof(cli->opkt.msg), false);
}
static int send_none(struct sndio_client *cli, int ret)
{
  cli->cstate = CLIENT_STATE_IDLE;
  cli->offset_in = 0;
  return ret;
}
static int amsg_auth(struct sndio_client *cli)
{
  int ret;
  if ( cli->server->sessrefs == 0 )
    {
      memcpy(cli->server->cookie, cli->imsg.u.auth.cookie, sizeof(cli->server->cookie));
      cli->pstate = PROTO_AUTH;
      cli->server->sessrefs = 1;
      ret = 0;
    } else
    {
      if ( memcmp(cli->server->cookie, cli->imsg.u.auth.cookie, sizeof(cli->server->cookie)) == 0 )
	{
	  ret = 0;
	  cli->pstate = PROTO_AUTH;
	  cli->server->sessrefs++;
	} else
	{
	  ret = -1;
	}
    }
  return send_none(cli, ret);
}

static void route_changed(int32_t dev, int32_t index, void *client, int32_t err, void *arg)
{
  //Nothing to do since the device isn't accessed directly.
  return;
}

static int amsg_hello(struct sndio_client *cli)
{
  int ret;
  struct amsg_hello *h = &cli->imsg.u.hello;
  size_t br;
  uint16_t mode = ntohs(h->mode);
  char buf[32];
  struct dspd_sg_info sg;
  struct dspd_cli_info_pkt info;
  socklen_t len;
  char path[1024];
  struct dspd_client_cb ccb;
  if ( (mode & ~(DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) != 0 ||
       h->version != AMSG_VERSION )
    return -1; //Unsupported mask
  sprintf(buf, "hw:%hhd", h->devnum);
  if ( mode == DSPD_PCM_SBIT_FULLDUPLEX )
    {
      ret = dspd_rclient_open(cli->server->ctx, cli->server->server_addr, buf, mode, &cli->pclient);
      if ( ret == 0 )
	{
	  cli->cclient = cli->pclient;
	} else
	{
	  ret = dspd_rclient_open(cli->server->ctx, cli->server->server_addr, buf, DSPD_PCM_SBIT_PLAYBACK, &cli->pclient);
	  if ( ret == 0 )
	    {
	      ret = dspd_rclient_open(cli->server->ctx, cli->server->server_addr, buf, DSPD_PCM_SBIT_CAPTURE, &cli->cclient);
	      if ( ret == 0 )
		{
		  memset(&sg, 0, sizeof(sg));
		  sg.sbits = DSPD_PCM_SBIT_FULLDUPLEX;
		  ret = dspd_rclient_ctl(cli->pclient, 
					 DSPD_SCTL_CLIENT_SYNCGROUP,
					 &sg,
					 sizeof(sg),
					 &cli->syncgroup,
					 sizeof(cli->syncgroup),
					 &br);
		  if ( ret == 0 )
		    ret = dspd_rclient_ctl(cli->cclient, 
					   DSPD_SCTL_CLIENT_SYNCGROUP,
					   &cli->syncgroup,
					   sizeof(cli->syncgroup),
					   NULL,
					   0,
					   &br);
		}
	    }
	}
    } else if ( mode == DSPD_PCM_SBIT_PLAYBACK )
    {
      ret = dspd_rclient_open(cli->server->ctx, cli->server->server_addr, buf, mode, &cli->pclient);
    } else if ( mode == DSPD_PCM_SBIT_CAPTURE )
    {
      ret = dspd_rclient_open(cli->server->ctx, cli->server->server_addr, buf, mode, &cli->cclient);
    } else
    {
      ret = -1;
    }
  if ( mode & DSPD_PCM_SBIT_PLAYBACK )
    {
      memset(&info, 0, sizeof(info));
      len = sizeof(info.cred.cred);
      if ( getsockopt(cli->fd, SOL_SOCKET, SO_PEERCRED, &info.cred.cred, &len) == 0 )
	{
	  if ( snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", info.cred.cred.pid, info.cred.cred.pid) < sizeof(path) )
	    {
	      int fd = open(path, O_RDONLY), ret;
	      memset(&info, 0, sizeof(info));
	      if ( fd >= 0 )
		{
		  while ( (ret = read(fd, info.name, sizeof(info.name) - 1UL)) < 0 )
		    {
		      ret = errno;
		      if ( ret != EINTR && ret != EWOULDBLOCK && ret != EAGAIN )
			break;
		    }
		  close(fd);
		  char *p = strchr(info.name, '\n');
		  if ( p )
		    *p = 0;
		}
	      if ( info.name[0] == 0 )
		sprintf(info.name, "Client #%d (pid %d)", dspd_rclient_client_index(cli->pclient), info.cred.cred.pid);
	    }
	  //This will fail if sndio is running in a separate process.
	  (void)dspd_rclient_ctl(cli->pclient,
				 DSPD_SCTL_CLIENT_SETINFO,
				 &info,
				 sizeof(info),
				 NULL,
				 0,
				 &br);
	}
    }
  if ( ret == 0 )
    {
      cli->streams = mode; //Same as PCM_SBIT
      cli->pstate = PROTO_INIT;
      if ( cli->pclient != NULL )
	{
	  cli->pclient_idx = dspd_rclient_client_index(cli->pclient);
	  if ( cli->pclient_idx > 0 )
	    {
	      cli->server->cli_map[cli->pclient_idx] = cli->sio_idx;
	      if ( cli->server->ctx != NULL )
		{
		  memset(&ccb, 0, sizeof(ccb));
		  ccb.index = cli->pclient_idx;
		  ccb.callback.route_changed = route_changed;
		  ccb.arg = cli;
		  (void)dspd_rclient_ctl(cli->pclient,
					 DSPD_SCTL_CLIENT_SETCB,
					 &ccb,
					 sizeof(ccb),
					 NULL,
					 0,
					 &br);
		}
	    }
	}
      ret = send_ack(cli);
    } else
    {
      ret = -1;
    }
  return ret;
}

static void par2cpu(struct amsg_par *par)
{
  par->pchan = ntohs(par->pchan);
  par->rchan = ntohs(par->rchan);
  par->rate = ntohl(par->rate);
  par->bufsz = ntohl(par->bufsz);
  par->round = ntohl(par->round);
  par->appbufsz = ntohl(par->appbufsz);
}
/*static void cpu2par(struct amsg_par *par)
{
  par->pchan = htons(par->pchan);
  par->rchan = htons(par->rchan);
  par->rate = htonl(par->rate);
  par->bufsz = htonl(par->bufsz);
  par->round = htonl(par->round);
  par->appbufsz = htonl(par->appbufsz);

  }*/

static void adjust_param(int32_t *val1, int32_t *val2)
{
  *val1 = MAX(*val1, *val2);
  *val2 = *val2;
}

static void combine_params(struct dspd_cli_params *pparams, struct dspd_cli_params *cparams)
{
  int32_t n;
  if ( pparams->rate != cparams->rate )
    {
      n = MAX(pparams->rate, cparams->rate);
      pparams->rate = n;
      cparams->rate = n;
    }
  cparams->format = pparams->format;
  adjust_param(&pparams->bufsize, &cparams->bufsize);
  adjust_param(&pparams->fragsize, &cparams->fragsize);
  adjust_param(&pparams->latency, &cparams->latency);
}

static int _amsg_setpar(struct sndio_client *cli)
{
  struct amsg_par *par = &cli->imsg.u.par;
  int ret;
  struct dspd_cli_params pparams, cparams;
  const struct dspd_cli_params *p;
  struct dspd_rclient_hwparams hwparams;
  
  if ( cli->pstate != PROTO_INIT && cli->pstate != PROTO_CONFIGURED )
    return -1;
  par2cpu(par);
  memset(&pparams, 0x00, sizeof(pparams));
  memset(&cparams, 0x00, sizeof(cparams));
  memset(&hwparams, 0x00, sizeof(hwparams));
  if ( ! cli->server->ctx )
    {
      pparams.flags = DSPD_CLI_FLAG_SHM;
      cparams.flags = DSPD_CLI_FLAG_SHM;
    }
  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      ret = par2cli(dspd_rclient_devinfo(cli->pclient),
		    DSPD_PCM_SBIT_PLAYBACK,
		    par,
		    &pparams);
    } else
    {
      ret = 0;
    }
  if ( ret == 0 && (cli->streams & DSPD_PCM_SBIT_CAPTURE) )
    {
      ret = par2cli(dspd_rclient_devinfo(cli->cclient),
		    DSPD_PCM_SBIT_CAPTURE,
		    par,
		    &cparams);
    }
  if ( ret == 0 )
    {
      if ( cli->streams == DSPD_PCM_SBIT_FULLDUPLEX && cli->cclient == cli->pclient )
	{
	  combine_params(&pparams, &cparams);
	  hwparams.playback_params = &pparams;
	  hwparams.capture_params = &cparams;
	  ret = dspd_rclient_set_hw_params(cli->pclient, &hwparams);
	  if ( ret == 0 )
	    {
	      p = dspd_rclient_get_hw_params(cli->pclient, DSPD_PCM_SBIT_PLAYBACK);
	      assert(p);
	      cli->pparams = *p;

	      p = dspd_rclient_get_hw_params(cli->cclient, DSPD_PCM_SBIT_CAPTURE);
	      assert(p);
	      cli->cparams = *p;

	      cli->pframe_bytes = dspd_get_pcm_format_size(cli->pparams.format) * cli->pparams.channels;
	      cli->cframe_bytes = dspd_get_pcm_format_size(cli->cparams.format) * cli->cparams.channels;
	      cli->start_threshold = cli->pparams.bufsize;
	    }
	} else 
	{
	  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      hwparams.playback_params = &pparams;
	      ret = dspd_rclient_set_hw_params(cli->pclient, &hwparams);
	      if ( ret == 0 )
		{
		  p = dspd_rclient_get_hw_params(cli->pclient, DSPD_PCM_SBIT_PLAYBACK);
		  assert(p);
		  cli->pparams = *p;
		  cli->start_threshold = cli->pparams.bufsize;
		  cli->pframe_bytes = dspd_get_pcm_format_size(cli->pparams.format) * cli->pparams.channels;
		  
		}
	    }
	  if ( ret == 0 && (cli->streams & DSPD_PCM_SBIT_CAPTURE) )
	    {
	      hwparams.playback_params = NULL;
	      hwparams.capture_params = &cparams;
	      ret = dspd_rclient_set_hw_params(cli->cclient, &hwparams);
	      if ( ret == 0 )
		{
		  p = dspd_rclient_get_hw_params(cli->cclient, DSPD_PCM_SBIT_CAPTURE);
		  assert(p);
		  cli->cparams = *p;
		  cli->cframe_bytes = dspd_get_pcm_format_size(cli->cparams.format) * cli->cparams.channels;
		  
		}
	    }
	}
    }
  if ( ret == 0 )
    {
      if ( cli->pparams.rate )
	cli->sample_time = 1000000000 / cli->pparams.rate;
      else if ( cli->cparams.rate )
	cli->sample_time = 1000000000 / cli->cparams.rate;
      cli->par_set = true;
      cli->pstate = PROTO_CONFIGURED;
      if ( AMSG_ISSET(par->xrun) )
	cli->xrun_policy = par->xrun;
      else
	cli->xrun_policy = SIO_IGNORE;
    }
  //This one has no reply.  The client should normally send
  //AMSG_GETPAR next which replies with AMSG_GETPAR.
  //return send_none(cli, ret);
  return ret;
}

static void amsg_setpar_cb(struct cbpoll_ctx *ctx,
			   struct cbpoll_msg *wrk,
			   void *data)
{
  struct sndio_client *cli = data;
  int ret = _amsg_setpar(cli);
  struct cbpoll_msg evt = { .len = sizeof(struct cbpoll_msg) };
  if ( ret == 0 )
    evt.arg = CLIENT_MSG_COMPLETE;
  else
    evt.arg = CLIENT_MSG_ERROR;
  evt.msg = CBPOLL_PIPE_MSG_DEFERRED_WORK;
  evt.fd = wrk->fd;
  evt.index = wrk->index;
  if ( cbpoll_send_event(ctx, &evt) < 0 )
    shutdown(wrk->fd, SHUT_RDWR);
}

static int amsg_setpar(struct sndio_client *cli)
{
  int ret;
  cbpoll_queue_deferred_work(cli->cbctx, cli->header.reserved_slot, 0, amsg_setpar_cb);
  ret = send_none(cli, 0);
  cli->cstate = CLIENT_STATE_BUSY;
  cbpoll_set_events(cli->cbctx, cli->header.reserved_slot, 0);
  return ret;
}

static int amsg_getpar(struct sndio_client *cli)
{
  int32_t fmt;
  struct amsg_par *par = &cli->opkt.msg.u.par;
  const struct dspd_cli_params *clp;
  unsigned int bits, len, usig, be;
  int ret = 0;
  bool f;
  AMSG_INIT(&cli->opkt.msg);
  if ( cli->pstate != PROTO_INIT && cli->pstate != PROTO_CONFIGURED )
    {
      ret = -1;
    } else if ( cli->par_set )
    {
      
      
      if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	{
	  fmt = cli->pparams.format;
	  clp = &cli->pparams;
	} else
	{
	  fmt = cli->cparams.format;
	  clp = &cli->cparams;
	}
      if ( dspd_pcm_format_info(fmt, &bits, &len, &usig, &be, &f) )
	{
	  par->bps = len;
	  par->bits = bits;
	  par->sig = ! usig;
	  if ( len > 1 )
	    par->le = ! be;
	  else
	    par->le = 0;

      
	  par->rate = ntohl(clp->rate);
	  par->bufsz = ntohl(clp->bufsize);
	  par->appbufsz = par->bufsz;
	  par->xrun = cli->xrun_policy;
	  par->round = ntohl(clp->fragsize);
	  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	    par->pchan = ntohs(cli->pparams.channels);
	  if ( cli->streams & DSPD_PCM_SBIT_CAPTURE )
	    par->rchan = ntohs(cli->cparams.channels);
	  ret = send_rmsg(cli, AMSG_GETPAR);
	} else
	{
	  ret = -1;
	}

    } else
    {
      //TODO: Fake config from default par (memset 0xFF)
      ret = -1;
    }
  return ret;
}

static int amsg_start(struct sndio_client *cli)
{
  cli->pstate = PROTO_TRIGGERED;
  cli->frames_written = 0;
  cli->write_window = cli->pparams.bufsize - cli->pparams.fragsize;
  cli->start_threshold = cli->write_window;
  AMSG_INIT(&cli->opkt.msg);
  cli->opkt.msg.cmd = htonl(AMSG_FLOWCTL);
  cli->opkt.msg.u.ts.delta = htonl(cli->write_window);

  if ( cli->streams == DSPD_PCM_SBIT_CAPTURE )
    {
      if ( client_trigger(cli) < 0 )
	return -1;
      return send_none(cli, 0);
    }
  return send_pkt(cli, sizeof(cli->opkt.msg), false);
}

static int amsg_stop(struct sndio_client *cli)
{
  int ret = 0;
  int32_t s;
  size_t br;
  cli->pstate = PROTO_CONFIGURED;
  if ( cli->pclient && ret == 0 )
    {
      if ( cli->running == false && cli->frames_written > 0 )
	{
	  client_trigger(cli);
	  cli->draining = true;
	  cbpoll_set_events(cli->cbctx, cli->header.reserved_slot, 0);
	  ret = send_none(cli, 0);
	} 
    }

  if ( cli->cclient )
    {
      s = DSPD_PCM_SBIT_CAPTURE;
      ret = dspd_rclient_ctl(cli->cclient, 
			     DSPD_SCTL_CLIENT_STOP,
			     &s,
			     sizeof(s),
			     NULL,
			     0,
			     &br);
    }
  if ( ! cli->pclient )
    client_reset(cli);
  cli->running = false;
  if ( ret == 0 && cli->draining == false )
    ret = send_ack(cli);
  return ret;
}

static int amsg_data(struct sndio_client *cli)
{
  size_t size = ntohl(cli->imsg.u.data.size);
  if ( (cli->streams & DSPD_PCM_SBIT_PLAYBACK) == 0 ||
       (size % cli->pframe_bytes) != 0 ||
       (size / cli->pframe_bytes) > cli->write_window )
    return -1;
       
  if ( dspd_rclient_test_xrun(cli->pclient, cli->streams) )
    if ( client_xrun(cli) < 0 )
      return -1;
  cli->p_max = size;
  cli->p_total = 0;
  cli->p_len = 0;
  cli->p_offset = 0;
  cli->cstate = CLIENT_STATE_RXDATA;
  cbpoll_set_events(cli->cbctx, cli->header.reserved_slot, POLLIN);
  return client_wxfer(cli);
}

static int amsg_setvol(struct sndio_client *cli)
{
  /*
    Set the volume.  No need to send any replies.  Do not trigger any feedback events since
    the sender knows what they did.
   */
  uint32_t v = ntohl(cli->imsg.u.vol.ctl);
  int ret;
  struct dspd_stream_volume sv;
  if ( v > SIO_MAXVOL )
    v = SIO_MAXVOL;
  if ( cli->pclient )
    {
      sv.stream = DSPD_PCM_SBIT_PLAYBACK;
      sv.volume = (float)v / (float)SIO_MAXVOL;
      ret = dspd_rclient_ctl(cli->pclient,
			     DSPD_SCTL_CLIENT_SETVOLUME,
			     &sv,
			     sizeof(sv),
			     NULL,
			     0,
			     NULL);
      if ( ret == 0 )
	cli->volume = v;
    } else
    {
      ret = 0;
    }
  return ret;
}

static int amsg_bye(struct sndio_client *cli)
{
  shutdown(cli->fd, SHUT_RDWR);
  return -1;
}

static int amsg_ack(struct sndio_client *cli)
{
  return 0;
}

static struct amsg_handler amsg_handlers[_AMSG_COUNT] = {
  [AMSG_ACK] = {
    .pstate = -1,
    .handler = amsg_ack,
  },
  [AMSG_AUTH] = {
    .pstate = PROTO_OPEN,
    .handler = amsg_auth,
  },
  [AMSG_HELLO] = {
    .pstate = PROTO_AUTH,
    .handler = amsg_hello,
  },
  [AMSG_SETPAR] = {
    .pstate = -1,
    .handler = amsg_setpar,
  },
  [AMSG_GETPAR] = {
    .pstate = -1,
    .handler = amsg_getpar,
  },
  [AMSG_START] = {
    .pstate = PROTO_CONFIGURED,
    .handler = amsg_start,
  },
  [AMSG_STOP] = {
    .pstate = PROTO_TRIGGERED,
    .handler = amsg_stop,
  },
  [AMSG_DATA] = {
    .pstate = PROTO_TRIGGERED,
    .handler = amsg_data,
  },
  [AMSG_SETVOL] = {
    .pstate = -1,
    .handler = amsg_setvol,
  },
  [AMSG_BYE] = {
    .pstate = -1,
    .handler = amsg_bye,
  },
};

int cli_execmsg(struct sndio_client *cli)
{
  size_t cmd = (size_t)ntohl(cli->imsg.cmd);
  struct amsg_handler *h;
  int ret = -1;
  if ( cmd < _AMSG_COUNT )
    {
      h = &amsg_handlers[cmd];
      if ( h->handler != NULL && 
	   ((cli->pstate == h->pstate) || (h->pstate == -1 && cli->pstate >= PROTO_INIT)))
	ret = h->handler(cli);
    }
  cli->offset_in = 0;
  return ret;
}




int client_pollin(struct sndio_client *cli)
{
  int ret = 0, e;
  char *ptr;
  switch(cli->cstate)
    {
    case CLIENT_STATE_IDLE:
      cli->offset_in = 0;
      cli->cstate = CLIENT_STATE_RXPKT;
    case CLIENT_STATE_RXPKT:
      //Incoming packet header
      ptr = (char*)&cli->imsg;
      ret = read(cli->fd, &ptr[cli->offset_in], sizeof(cli->imsg) - cli->offset_in);
      if ( ret < 0 )
	{
	  e = errno;
	  if ( e != EINTR && e != EAGAIN && e != EWOULDBLOCK )
	    ret = -1;
	  else
	    ret = 0;
	} else if ( ret == 0 )
	{
	  ret = -1;
	} else
	{
	  cli->offset_in += ret;
	  if ( cli->offset_in == sizeof(cli->imsg) )
	    ret = cli_execmsg(cli);
	  else
	    ret = 0;
	}
      break;
    case CLIENT_STATE_RXDATA:
      ret = client_wxfer(cli);
      break;
    
    case CLIENT_STATE_TXPKT:
      //This may rarely happen if a packet was started during a timer event and
      //there was a POLLIN event at the same time.
      ret = cbpoll_set_events(cli->cbctx, cli->header.reserved_slot, POLLOUT);
      break;
    }
  return ret;
}

int client_pollout(struct sndio_client *cli)
{
  int ret, e;
  const char *ptr;
  if ( cli->cstate == CLIENT_STATE_TXPKT )
    {
      
      ptr = (const char*)&cli->opkt;
      ret = write(cli->fd, &ptr[cli->offset_out], cli->len_out - cli->offset_out);
      if ( ret < 0 )
	{
	  e = errno;
	  if ( e != EINTR && e != EWOULDBLOCK && e != EAGAIN )
	    ret = -1;
	  else
	    ret = 0;
	} else if ( ret == 0 )
	{
	  ret = -1;
	} else
	{
	  cli->offset_out += ret;
	  if ( cli->offset_out == cli->len_out )
	    {
	      cli->offset_out = 0;
	      cli->len_out = 0;
	      cli->cstate = CLIENT_STATE_IDLE;
	      
	      ret = client_buildmsg(cli, true);
	      if ( cli->cstate == CLIENT_STATE_IDLE )
		  ret = cbpoll_set_events(cli->cbctx, cli->header.reserved_slot, POLLIN);
	    } else
	    {
	      ret = 0;
	    }
	}
    } else
    {
      ret = cbpoll_set_events(cli->cbctx, cli->header.reserved_slot, POLLIN);
    }
  
  return ret;
}

static int client_fd_event(void *data, 
			   struct cbpoll_ctx *context,
			   int index,
			   int fd,
			   int revents)
{
  struct sndio_client *cli = data;
  int32_t ret;
 
  if ( revents & (POLLHUP|POLLRDHUP|POLLNVAL|POLLERR) )
    ret = -1;
  else if ( revents & POLLIN )
    ret = client_pollin(cli);
  else if ( revents & POLLOUT )
    ret = client_pollout(cli);
  else
    ret = 0;
  return ret;
}






static int client_pipe_event(void *data, 
			     struct cbpoll_ctx *context,
			     int index,
			     int fd,
			     const struct cbpoll_msg *event)
{
  int ret = 0;
  struct sndio_client *cli = data;
  switch(event->arg)
    {
    case CLIENT_MSG_ERROR:
      shutdown(fd, SHUT_RDWR);
      break;
    case CLIENT_MSG_COMPLETE:
      cli->cstate = CLIENT_STATE_IDLE;
      ret = cbpoll_set_events(context, index, POLLIN);
      break;
    }
  return ret;

}

static const struct cbpoll_fd_ops sndio_client_ops = {
  .fd_event = client_fd_event,
  .pipe_event = client_pipe_event,
  .destructor = client_destructor,
};



static bool server_destructor(void *data,
			      struct cbpoll_ctx *context,
			      int index,
			      int fd)
{
  fprintf(stderr, "Closing sndio socket %d\n", fd);
  return true;
}


static const struct cbpoll_fd_ops sndio_listen_ops = {
  .fd_event = cbpoll_listening_fd_event_cb,
  .destructor = server_destructor,
};

static void set_cur_vol(struct sndio_ctx *ctx, uint32_t index, uint32_t value)
{
  struct sndio_client *sc;
  if ( ctx->cli_vol_elem != index || ctx->cli_vol_ptr == NULL || ctx->cli_vol_index < 0 )
    return;
  sc = (struct sndio_client*)ctx->list.clients[ctx->cli_vol_index];
  if ( sc != ctx->cli_vol_ptr )
    return;
  if ( sc->vol_pending == false || sc->vol_elem != index )
    return;
  if ( sc->volume != value )
    {
      sc->volume = value;
      sc->vol_changed = true;
    }
  sc->vol_pending = false;
}
static void ctl_get_cb(struct dspd_ctl_client *cli, void *arg, struct dspd_async_op *op, uint32_t index, int32_t value);
static void start_next_vol(struct dspd_ctl_client *cli, struct sndio_ctx *ctx)
{
  size_t i, idx;
  struct sndio_client *sc;
  int32_t ret;
  for ( i = 1; i <= ctx->list.max_clients; i++ )
    {
      idx = i + ctx->cli_vol_pos;
      sc = (struct sndio_client*)ctx->list.clients[idx % ctx->list.max_clients];
      if ( sc == NULL || sc == (struct sndio_client*)UINTPTR_MAX )
	continue;
      if ( sc->vol_update == true && sc->vol_elem >= 0 )
	{
	  ret = dspd_ctlcli_elem_get_int32(cli, sc->vol_elem, -1, &ctx->cli_vol, ctl_get_cb, ctx);
	  if ( ret == -EINPROGRESS )
	    {
	      ctx->cli_vol_index = sc->sio_idx;
	      ctx->cli_vol_ptr = sc;
	      ctx->cli_vol_pos = idx;
	      ctx->cli_vol_elem = sc->vol_elem;
	      sc->vol_pending = true;
	      sc->vol_update = false;
	      return;
	    }
	}
    }
  ctx->cli_vol_index = -1;
  ctx->cli_vol_ptr = NULL;
  ctx->cli_vol_elem = -1;
}

#define RANGE_DIV (VCTRL_RANGE_MAX / SIO_MAXVOL)
static void ctl_get_cb(struct dspd_ctl_client *cli, void *arg, struct dspd_async_op *op, uint32_t index, int32_t value)
{
  if ( op->error == 0 )
    {
      if ( value == VCTRL_RANGE_MAX )
	value = SIO_MAXVOL;
      else
	value /= RANGE_DIV;
      set_cur_vol(arg, index, value);
    }
  start_next_vol(cli, arg); 
}

static void ctl_change_cb(struct dspd_ctl_client *cli, void *arg, int32_t err, uint32_t elem, int32_t evt, const struct dspd_mix_info *info)
{
  struct sndio_client *sc;
  struct sndio_ctx *server = arg;
  int32_t ret;
  if ( evt != DSPD_CTL_EVENT_MASK_REMOVE && (evt & DSPD_CTL_EVENT_MASK_VALUE) != 0 )
    {
      size_t stream = info->hwinfo & 0xFFFFU;
      if ( stream > 0 && stream < ARRAY_SIZE(server->cli_map) )
	{
	  ssize_t idx = server->cli_map[stream];
	  if ( idx >= 0 )
	    {
	      sc = (struct sndio_client*)server->list.clients[idx];
	      if ( sc != NULL && sc != (struct sndio_client*)UINTPTR_MAX )
		{
		  if ( sc->pclient != NULL && sc->pclient_idx == stream )
		    {
		      if ( server->cli_vol_index < 0 )
			{
			  ret = dspd_ctlcli_elem_get_int32(cli, elem, -1, &server->cli_vol, ctl_get_cb, server);
			  if ( ret == -EINPROGRESS )
			    {
			      server->cli_vol_index = idx;
			      server->cli_vol_ptr = sc;
			      server->cli_vol_pos = idx;
			      server->cli_vol_elem = elem;
			      sc->vol_elem = elem;
			      sc->vol_pending = true;
			      sc->vol_update = false;
			    }
			} else
			{
			  sc->vol_update = true;
			  sc->vol_elem = elem;
			}
		    }
		}
	    }
	}
    }
}



static int ctl_fd_event(void *data, 
			struct cbpoll_ctx *context,
			int index,
			int fd,
			int revents)
{
  int ret;
  struct sndio_ctx *ctx = data;
  if ( revents & (POLLRDHUP|POLLHUP|POLLERR|POLLNVAL) )
    return -1;
  if ( ctx->ctx )
    {
      ret = dspd_aio_process(ctx->aio, 0, 0);
      if ( ret == -EINPROGRESS )
	ret = 0;
    } else
    {
      ret = dspd_aio_process(ctx->aio, revents, 0);
      if ( ret == 0 || ret == -EINPROGRESS )
	ret = cbpoll_set_events(context, index, dspd_aio_block_directions(ctx->aio));
    }
  return ret;
}
static bool ctl_destructor(void *data,
			   struct cbpoll_ctx *context,
			   int index,
			   int fd)
{
  struct sndio_ctx *ctx = data;
  dspd_ctlcli_delete(ctx->ctl);
  ctx->ctl = NULL;
  dspd_aio_delete(ctx->aio);
  ctx->aio = NULL;
  ctx->efd.fd = -1;
  return true; //close fd
}
static const struct cbpoll_fd_ops sndio_ctl_ops = {
  .fd_event = ctl_fd_event,
  .pipe_event = NULL,
  .destructor = ctl_destructor,
};

static int par2cli(const struct dspd_device_stat *info, 
		   int mode,
		   const struct amsg_par *par, 
		   struct dspd_cli_params *clp)
{
  const struct dspd_cli_params *devpar;
  if ( mode & DSPD_PCM_SBIT_PLAYBACK )
    devpar = &info->playback;
  else
    devpar = &info->capture;
  if ( devpar->rate == 0 )
    return -1;
  clp->stream = mode;
  clp->format = dspd_pcm_build_format(par->bits,
				      par->bps,
				      ! par->sig,
				      ! par->le,
				      false);
  if ( clp->format < 0 )
    clp->format = DSPD_PCM_FORMAT_S16_NE;
  if ( mode & DSPD_PCM_SBIT_PLAYBACK )
    {
      if ( AMSG_ISSET(par->pchan) )
	{
	  if ( par->pchan < 1 )
	    clp->channels = 1;
	  else if ( par->pchan > NCHAN_MAX )
	    clp->channels = NCHAN_MAX;
	  else if ( par->pchan > 2 && par->pchan > devpar->channels )
	    clp->channels = devpar->channels;
	  else
	    clp->channels = par->pchan;
	} else
	{
	  clp->channels = devpar->channels;
	}
    }
  if ( mode & DSPD_PCM_SBIT_CAPTURE )
    {
      if ( AMSG_ISSET(par->rchan) && par->rchan > 0 )
	{
	  if ( par->rchan < 1 )
	    clp->channels = 1;
	  else if ( par->rchan > NCHAN_MAX )
	    clp->channels = NCHAN_MAX;
	  else if ( par->rchan > 2 && par->rchan > devpar->channels )
	    clp->channels = devpar->channels;
	  else
	    clp->channels = par->rchan;
	} else
	{
	  clp->channels = devpar->channels;
	}
    }
  if ( AMSG_ISSET(par->rate) )
    {
      if ( par->rate < RATE_MIN )
	clp->rate = RATE_MIN;
      else if ( par->rate > RATE_MAX )
	clp->rate = RATE_MAX;
      else
	clp->rate = par->rate;
    } else
    {
	clp->rate = devpar->rate;
    }
  
  if ( AMSG_ISSET(par->appbufsz) && par->appbufsz > 0 )
    clp->bufsize = par->appbufsz;
  else
    clp->bufsize = devpar->bufsize / 2;
  if ( AMSG_ISSET(par->round) && par->round > 0 )
    clp->fragsize = clp->bufsize / par->round;
  else
    clp->fragsize = clp->bufsize / 4;
  clp->latency = clp->fragsize;
  

  

  dspd_time_t sample_time = 1000000000 / devpar->rate;
  dspd_time_t min_time = sample_time * devpar->min_latency;
  dspd_time_t max_time = sample_time * devpar->max_latency;
  dspd_time_t t;
  dspd_time_t par_time = (1000000000 / clp->rate) * clp->latency;
  for ( t = min_time; t < max_time; t *= 2 )
    {
      if ( t >= par_time )
	break;
    }
  if ( t > max_time )
    par_time = max_time;
  else
    par_time = t;
  clp->latency = par_time / (1000000000 / clp->rate);
  clp->fragsize = clp->latency;
  clp->bufsize = MAX((clp->fragsize * 4), clp->bufsize);
  

  return 0;
}
/*static int cli2par(int mode,
		   const struct dspd_cli_params *clp,
		   struct sio_par *par)
{
  unsigned int bits, len, usig, be;
  bool f;
  if ( ! dspd_pcm_format_info(clp->format, &bits, &len, &usig, &be, &f) )
    return -1;
  par->bps = len;
  par->bits = bits;
  par->sig = ! usig;
  if ( len > 1 )
    par->le = ! be;
  else
    par->le = 0;
  if ( mode & DSPD_PCM_SBIT_PLAYBACK )
    par->pchan = clp->channels;
  else
    par->rchan = clp->channels;
  par->rate = clp->rate;
  par->bufsz = clp->bufsize;
  par->appbufsz = par->bufsz;
  if ( clp->fragsize <= 0 )
    return -1; //Should not happen
  par->round = clp->fragsize;
  return 0;
  }*/







int32_t dspd_sndio_new(struct sndio_ctx **ctx, struct dspd_sndio_params *params)
{
  char sockpath[PATH_MAX] = { 0 };
  int32_t uid;
  mode_t mask;
  size_t fd_count = 0, len, offset;
  struct stat fi;
  char *tmp = NULL;
  int32_t ret = 0;
  char *saveptr = NULL;
  char *tok;
  int fd, port;
  len = sizeof(struct sndio_ctx);
  if ( len % sizeof(uintptr_t) )
    {
      offset = sizeof(uintptr_t) - (len % sizeof(uintptr_t));
      len += sizeof(uintptr_t);
    } else
    {
      offset = 0;
    }
  len += MAX_CLIENTS * sizeof(struct cbpoll_client_hdr*);

  struct sndio_ctx *sctx = calloc(1, len);
  if ( ! sctx )
    return -ENOMEM;
  sctx->list.clients = (struct cbpoll_client_hdr**)(((char*)sctx) + sizeof(struct sndio_ctx) + offset);
  sctx->list.max_clients = MAX_CLIENTS;
  sctx->list.flags = CBPOLL_CLIENT_LIST_LISTENFD | CBPOLL_CLIENT_LIST_AUTO_POLLIN;
  sctx->list.ops = &client_list_ops;
  sctx->list.fd_ops = &sndio_client_ops;
  sctx->fd = -1;
  sctx->cbidx = -1;
  sctx->cli_vol_index = -1;
  if ( params->server_addr )
    {
      sctx->server_addr = strdup(params->server_addr);
      if ( ! sctx->server_addr )
	{
	  ret = -ENOMEM;
	  goto out;
	}
    }
  sctx->ctx = params->context;
    

  if ( params->net_addrs )
    {
      for ( tok = strchr(params->net_addrs, ','); tok; tok = strchr(&tok[1], ',') )
	fd_count++;
    }
  ret = cbpoll_init(&sctx->cbctx, CBPOLL_FLAG_TIMER|CBPOLL_FLAG_CBTIMER, MAX_CLIENTS+3+fd_count);
  if ( ret < 0 )
    goto out;



  if ( ! params->disable_unix_socket )
    {
      uid = getuid();
      if ( params->system_server || uid == 0 )
	{
	  strcpy(sockpath, "/tmp/aucat");
	  mask = 022;
	} else
	{
	  mask = 077;
	  sprintf(sockpath, "/tmp/aucat-%d", uid);
	}
      if ( mkdir(sockpath, 0777 & ~mask) < 0 )
	{
	  if ( errno != EEXIST )
	    {
	      ret = -errno;
	      goto out;
	    }
	}
     

      if ( stat(sockpath, &fi) < 0 )
	{
	  ret = -errno;
	  goto out;
	}
      if ( fi.st_uid != uid || (fi.st_mode & mask) != 0 )
	{
	  ret = -EPERM;
	  goto out;
	}
      len = strlen(sockpath);
      sprintf(&sockpath[len], "/aucat%d", params->unit_number);
      unlink(sockpath);
     


      sctx->fd = dspd_unix_sock_create(sockpath, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if ( sctx->fd < 0 )
	{
	  ret = -errno;
	  goto out;
	}

      if ( params->context )
	{
	  if ( params->context->uid > 0 && 
	       params->context->gid > 0 &&
	       params->context->ipc_mode != 0 )
	    {
	      chown(sockpath, params->context->uid, params->context->gid);
	    }
	  if ( params->context->ipc_mode != 0 )
	    chmod(sockpath, params->context->ipc_mode);
	} else
	{
	  chmod(sockpath, 0777);
	}
      sctx->cbidx = cbpoll_add_fd(&sctx->cbctx, sctx->fd, EPOLLIN|EPOLLONESHOT, &sndio_listen_ops, sctx);
      if ( sctx->cbidx < 0 )
	{
	  ret = -errno;
	  goto out;
	}
    }
  
  if ( params->net_addrs )
    {
      tmp = strdup(params->net_addrs);
      if ( ! tmp )
	{
	  ret = -ENOMEM;
	  goto out;
	}
      if ( fd_count > 0 )
	{
	  sctx->tcp_fds = calloc(fd_count, sizeof(*sctx->tcp_fds));
	  port = AUCAT_PORT + params->unit_number;
	  for ( tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr) )
	    {
	      if ( strlen(tok) >= (sizeof(sockpath)-6) )
		{
		  ret = -EINVAL;
		  goto out;
		}
	      if ( strstr(tok, "]:") == NULL || (strstr(tok, "]") == NULL && strstr(tok, ":") == NULL) )
		sprintf(sockpath, "%s:%d", tok, port);
	      else
		strcpy(sockpath, tok);


	      fd = dspd_tcp_sock_create(sockpath, SOCK_CLOEXEC | SOCK_NONBLOCK);
	      if ( fd >= 0 )
		{
		  ret = cbpoll_add_fd(&sctx->cbctx, fd, EPOLLIN|EPOLLONESHOT, &sndio_listen_ops, sctx);
		  if ( ret < 0 )
		    goto out;
		  sctx->tcp_fds[sctx->tcp_nfds] = fd;
		  sctx->tcp_nfds++;
		}
	    }
	}
    }

#ifdef ENABLE_CTL
  if ( sctx->ctx )
    {
      sctx->efd.fd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
      if ( sctx->efd.fd < 0 )
	{
	  ret = -errno;
	  goto out;
	}
      dspd_ts_clear(&sctx->efd.tsval);
    }

  ret = dspd_aio_new(&sctx->aio, DSPD_AIO_SYNC); //synchronous nonblocking io
  if ( ret < 0 )
    goto out;
  if ( sctx->ctx )
    ret = dspd_aio_connect(sctx->aio, NULL, sctx->ctx, &dspd_aio_fifo_eventfd_ops, &sctx->efd);
  else
    ret = dspd_aio_connect(sctx->aio, NULL, NULL, NULL, NULL);
  if ( ret < 0 )
    goto out;
    
  ret = dspd_ctlcli_new(&sctx->ctl, DSPD_CC_IO_SYNC, DSPD_MAX_OBJECTS);
  if ( ret < 0 )
    goto out;
  dspd_ctlcli_bind(sctx->ctl, sctx->aio, 0);
  uint32_t count;
 
  dspd_ctlcli_set_event_cb(sctx->ctl, ctl_change_cb, sctx);
  ret = dspd_ctlcli_subscribe(sctx->ctl, true, &count, NULL, NULL);
  if ( ret < 0 )
    goto out;

  ret = dspd_ctlcli_refresh_count(sctx->ctl, &count, NULL, NULL);
  if ( ret < 0 )
    goto out;
  
  if ( sctx->ctx )
    ret = cbpoll_add_fd(&sctx->cbctx, sctx->efd.fd, EPOLLIN, &sndio_ctl_ops, sctx);
  else
    ret = cbpoll_add_fd(&sctx->cbctx, dspd_aio_get_iofd(sctx->aio), dspd_aio_block_directions(sctx->aio), &sndio_ctl_ops, sctx);
  if ( ret < 0 )
    goto out;
#else
  (void)ctl_change_cb;
  (void)sndio_ctl_ops;
#endif

  ret = 0;
  *ctx = sctx;
  
 out:
  free(tmp);
  if ( ret < 0 )
    {
      dspd_sndio_delete(sctx);
      fprintf(stderr, "Error %d while creating sndio server", ret);
    }
  return ret;
}


int32_t dspd_sndio_start(struct sndio_ctx *ctx)
{
  int32_t ret;
  size_t i;
  ret = cbpoll_set_name(&ctx->cbctx, "dspd-sndiod");
  if ( ret < 0 )
    return ret;
  if ( ctx->fd >= 0 )
    {
      ret = listen(ctx->fd, SOMAXCONN);
      if ( ret < 0 )
	{
	  ret = -errno;
	  perror("listen");
	  return ret;
	}
    }
  for ( i = 0; i < ctx->tcp_nfds; i++ )
    {
      ret = listen(ctx->tcp_fds[i], SOMAXCONN);
      if ( ret < 0 )
	{
	  ret = -errno;
	  perror("listen");
	  return ret;
	}
    }
  ret = cbpoll_start(&ctx->cbctx);
  if ( ret == 0 )
    ctx->started = true;
  return ret;
}

int32_t dspd_sndio_run(struct sndio_ctx *ctx)
{
  int32_t ret;
  size_t i;
  if ( ctx->fd >= 0 )
    {
      ret = listen(ctx->fd, SOMAXCONN);
      if ( ret < 0 )
	return -errno;
    }
  for ( i = 0; i < ctx->tcp_nfds; i++ )
    {
      ret = listen(ctx->tcp_fds[i], SOMAXCONN);
      if ( ret < 0 )
	return -errno;
    }
  ctx->started = true;
  ret = cbpoll_run(&ctx->cbctx);
  ctx->started = false;
  return ret;
}

void dspd_sndio_delete(struct sndio_ctx *ctx)
{
  size_t i;
  if ( ctx->started )
    cbpoll_destroy(&ctx->cbctx);
  free(ctx->server_addr);
  if ( ! ctx->started )
    {
      for ( i = 0; i < ctx->tcp_nfds; i++ )
	close(ctx->tcp_fds[i]);
    }
  free(ctx->tcp_fds);
  if ( ctx->ctl )
    dspd_ctlcli_delete(ctx->ctl);
  if ( ctx->aio )
    dspd_aio_delete(ctx->aio);
  if ( ctx->efd.fd >= 0 )
    close(ctx->efd.fd);
  free(ctx);
}


