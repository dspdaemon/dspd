/*
  TODO: Add support for SIO_SYNC and default params.  The second one doesn't seem to
  be used in practice but the real sndiod seems to support it.
 

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

#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../lib/cbpoll.h"
#include "sndio.h"
#include "amsg.h"
#include "defs.h"
#include "dspd_sndio.h"
#define MAX_CLIENTS 32



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
  struct dspd_rclient *pclient, *cclient;

  struct cbpoll_ctx  *cbctx;
  struct sndio_ctx   *server;
  bool                timer_active;
  dspd_time_t         next_wakeup;
  uint32_t            periodic_wakeup;


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
  int32_t               cbidx;
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
};


#define SERVER_MSG_ADDCLI (CBPOLL_PIPE_MSG_USER+1)
#define SERVER_MSG_DELCLI (CBPOLL_PIPE_MSG_USER+2)

struct sndio_timer {
  struct dspd_timer timer;
  dspd_time_t       next_wakeup;
  uint32_t          periodic_wakeup;
  bool              active;
  bool              reset;
  
};


struct sndio_ctx {
  struct sndio_client *clients[MAX_CLIENTS];
  size_t nclients;
  struct cbpoll_ctx cbctx;
  int fd;
  int cbidx;
  uint8_t cookie[AMSG_COOKIELEN];
  size_t  sessrefs;

  struct sndio_timer tmr;
  void             *ctx;
  char             *server_addr;
  int              *tcp_fds;
  size_t            tcp_nfds;
  bool              started;
};

struct amsg_handler {
  int pstate;
  int32_t (*handler)(struct sndio_client *cli);
};
static int par2cli(const struct dspd_device_stat *info, 
		   int mode,
		   const struct amsg_par *par, 
		   struct dspd_cli_params *clp);
static int cli2par(int mode,
		   const struct dspd_cli_params *clp,
		   struct sio_par *par);
int client_pollout(struct sndio_client *cli);
int client_check_io(struct sndio_client *cli);
int client_check_io_fast(struct sndio_client *cli, dspd_time_t *nextwakeup);
static int send_pkt(struct sndio_client *cli, size_t len, bool async);
int client_check_buffers(struct sndio_client *cli);
int client_buildmsg(struct sndio_client *cli, bool async);
static int send_ack(struct sndio_client *cli);
static bool client_timer_event2(struct sndio_client *cli)
{
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
	}
    }
  return ret;
}

static void loop_sleep(void *arg, struct cbpoll_ctx *context)
{
  struct sndio_ctx *ctx = arg;
  uint64_t val;
  if ( ctx->tmr.reset )
    {
      read(ctx->tmr.timer.fd, &val, sizeof(val));
      ctx->tmr.reset = false;
    }
  dspd_timer_set(&ctx->tmr.timer, ctx->tmr.next_wakeup, ctx->tmr.periodic_wakeup);
}

static void set_client_wakeup(struct sndio_client *cli, dspd_time_t *next, uint32_t *per)
{
  if ( next )
    cli->next_wakeup = *next;
  if ( per )
    cli->periodic_wakeup = *per;
  if ( cli->next_wakeup > 0 && (cli->server->tmr.next_wakeup > cli->next_wakeup || cli->server->tmr.next_wakeup == 0) )
    {
      cli->server->tmr.next_wakeup = cli->next_wakeup;
      cli->server->tmr.reset = true;
    }
  if ( cli->periodic_wakeup > 0 && (cli->server->tmr.periodic_wakeup > cli->periodic_wakeup || cli->server->tmr.periodic_wakeup == 0))
    {
      cli->server->tmr.next_wakeup = cli->next_wakeup;
      cli->server->tmr.reset = true;
    }
  cli->timer_active = ( cli->periodic_wakeup || cli->next_wakeup );
}




static int timer_fd_event(void *data, 
			  struct cbpoll_ctx *context,
			  int index,
			  int fd,
			  int revents)
{
  struct sndio_ctx *ctx = data;
  size_t i;
  struct sndio_client *cli;
  dspd_time_t w = ctx->tmr.next_wakeup;
  uint32_t p = ctx->tmr.periodic_wakeup;
  ctx->tmr.next_wakeup = 0;
  ctx->tmr.periodic_wakeup = 0;
  for ( i = 0; i < ctx->nclients; i++ )
    {
      cli = ctx->clients[i];
      if ( cli != NULL && cli != (struct sndio_client*)UINTPTR_MAX && cli->timer_active )
	{
	  if ( client_timer_event2(cli) )
	    {
	      if ( ctx->tmr.next_wakeup == 0 || 
		   (ctx->tmr.next_wakeup < cli->next_wakeup && cli->next_wakeup != 0) )
		ctx->tmr.next_wakeup = cli->next_wakeup;
	      if ( ctx->tmr.periodic_wakeup == 0 || 
		   (ctx->tmr.periodic_wakeup < cli->periodic_wakeup && cli->periodic_wakeup != 0) )
		ctx->tmr.periodic_wakeup = cli->periodic_wakeup;
	    }
	}
    }
  if ( ctx->tmr.next_wakeup == w && ctx->tmr.periodic_wakeup == p )
    ctx->tmr.reset = true;
  return 0;
}


int client_check_buffers(struct sndio_client *cli)
{
  int ret = 0;
  uint32_t avail_min = 0xdeadbeef;
  int32_t delay;
  uint32_t pdelta = 0, cdelta = 0, mdelta;
  dspd_time_t pnext = 0, cnext = 0, nextwakeup;

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
      if ( nextwakeup )
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
	ret = cbpoll_set_events(cli->cbctx, cli->cbidx, POLLOUT);
      else
	cli->cstate = CLIENT_STATE_IDLE;
    } else
    {
      ret = cbpoll_set_events(cli->cbctx, cli->cbidx, POLLOUT);
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

static int amsg_hello(struct sndio_client *cli)
{
  int ret;
  struct amsg_hello *h = &cli->imsg.u.hello;
  size_t br;
  uint16_t mode = ntohs(h->mode);
  char buf[32];
  struct dspd_sg_info sg;
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
  if ( ret == 0 )
    {
      cli->streams = mode; //Same as PCM_SBIT
      cli->pstate = PROTO_INIT;
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
static void cpu2par(struct amsg_par *par)
{
  par->pchan = htons(par->pchan);
  par->rchan = htons(par->rchan);
  par->rate = htonl(par->rate);
  par->bufsz = htonl(par->bufsz);
  par->round = htonl(par->round);
  par->appbufsz = htonl(par->appbufsz);

}

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
  size_t br;
  if ( cli->pstate != PROTO_INIT && cli->pstate != PROTO_CONFIGURED )
    return -1;
  par2cpu(par);
  memset(&pparams, 0x00, sizeof(pparams));
  memset(&cparams, 0x00, sizeof(cparams));
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
      if ( cli->streams == DSPD_PCM_SBIT_FULLDUPLEX )
	combine_params(&pparams, &cparams);
      
      if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	{
	  ret = dspd_rclient_ctl(cli->pclient,
				 DSPD_SCTL_CLIENT_SETPARAMS,
				 &pparams,
				 sizeof(pparams),
				 &cli->pparams,
				 sizeof(cli->pparams),
				 &br);
	  cli->pframe_bytes = dspd_get_pcm_format_size(cli->pparams.format) * cli->pparams.channels;
	  if ( ret == 0 && cli->cclient != cli->pclient )
	    {
	      cli->start_threshold = cli->pparams.bufsize; //FIXME: Figure out the best size
	      ret = dspd_rclient_connect(cli->pclient, NULL, NULL, NULL, NULL, -1, -1);
	    }
	}
	  
      if ( ret == 0 && (cli->streams & DSPD_PCM_SBIT_CAPTURE) )
	{
	  ret = dspd_rclient_ctl(cli->cclient,
				 DSPD_SCTL_CLIENT_SETPARAMS,
				 &cparams,
				 sizeof(cparams),
				 &cli->cparams,
				 sizeof(cli->cparams),
				 &br);
	  cli->cframe_bytes = dspd_get_pcm_format_size(cli->cparams.format) * cli->cparams.channels;
	  if ( ret == 0 )
	    {
	      ret = dspd_rclient_connect(cli->cclient, NULL, NULL, NULL, NULL, -1, -1);
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
			   void *data,
			   int64_t arg,
			   int32_t index,
			   int32_t fd)
{
  struct sndio_client *cli = data;
  int ret = _amsg_setpar(cli);
  struct cbpoll_pipe_event evt;
  memset(&evt, 0, sizeof(evt));
  if ( ret == 0 )
    evt.arg = CLIENT_MSG_COMPLETE;
  else
    evt.arg = CLIENT_MSG_ERROR;
  evt.msg = CBPOLL_PIPE_MSG_DEFERRED_WORK;
  evt.fd = fd;
  evt.index = index;
  if ( cbpoll_send_event(ctx, &evt) < 0 )
    shutdown(fd, SHUT_RDWR);
}

static int amsg_setpar(struct sndio_client *cli)
{
  int ret;
  cbpoll_queue_deferred_work(cli->cbctx, cli->cbidx, 0, amsg_setpar_cb);
  ret = send_none(cli, 0);
  cli->cstate = CLIENT_STATE_BUSY;
  cbpoll_set_events(cli->cbctx, cli->cbidx, 0);
  return ret;
}

static int amsg_getpar(struct sndio_client *cli)
{
  int32_t fmt;
  struct amsg_par *par = &cli->opkt.msg.u.par;
  const struct dspd_cli_params *clp;
  unsigned int bits, len, usig, be;
  int ret = 0;
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
      if ( dspd_pcm_format_info(fmt, &bits, &len, &usig, &be) )
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
	  cbpoll_set_events(cli->cbctx, cli->cbidx, 0);
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
  cbpoll_set_events(cli->cbctx, cli->cbidx, POLLIN);
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
      ret = cbpoll_set_events(cli->cbctx, cli->cbidx, POLLOUT);
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
		  ret = cbpoll_set_events(cli->cbctx, cli->cbidx, POLLIN);
	    } else
	    {
	      ret = 0;
	    }
	}
    } else
    {
      ret = cbpoll_set_events(cli->cbctx, cli->cbidx, POLLIN);
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
static void free_client_cb(struct cbpoll_ctx *ctx,
			   void *data,
			   int64_t arg,
			   int32_t index,
			   int32_t fd)
{
  struct sndio_client *cli = (struct sndio_client*)(intptr_t)arg;
  if ( cli->pclient )
    dspd_rclient_delete(cli->pclient);
  if ( cli->cclient != NULL && cli->cclient != cli->pclient )
    dspd_rclient_delete(cli->cclient);
  free(cli);
}


static bool client_destructor(void *data,
			      struct cbpoll_ctx *context,
			      int index,
			      int fd)
{
  ssize_t i;
  struct sndio_client *cli = data;
  struct cbpoll_work wrk;
  for ( i = 0; i < MAX_CLIENTS; i++ )
    {
      if ( cli->server->clients[i] == cli )
	{
	  cli->server->clients[i] = NULL;
	  if ( i == (cli->server->nclients-1) )
	    cli->server->nclients = i;
	}
    }
  if ( cli->pstate > 0 )
    cli->server->sessrefs--;
  shutdown(fd, SHUT_RDWR);
  memset(&wrk, 0, sizeof(wrk));
  wrk.index = cli->server->cbidx;
  wrk.msg = 0;
  wrk.arg = (intptr_t)cli;
  wrk.fd = cli->server->fd;
  wrk.callback = free_client_cb;
  cbpoll_queue_work(context, &wrk);

  return true;
}

static int client_pipe_event(void *data, 
			     struct cbpoll_ctx *context,
			     int index,
			     int fd,
			     const struct cbpoll_pipe_event *event)
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



static int listen_pipe_event(void *data, 
			     struct cbpoll_ctx *context,
			     int index,
			     int fd,
			     const struct cbpoll_pipe_event *event)
{
  struct sndio_ctx *ctx = data;
  struct sndio_client *cli;
  int ret;
  switch(event->msg)
    {
    case SERVER_MSG_ADDCLI:
      cli = (struct sndio_client*)(uintptr_t)event->arg;
      ret = cbpoll_add_fd(&ctx->cbctx, cli->fd, EPOLLIN, &sndio_client_ops, cli);
      if ( ret < 0 )
	{
	  ctx->clients[cli->sio_idx] = NULL;
	  close(cli->fd);
	  free(cli);
	} else
	{
	  cli->cbidx = ret;
	  if ( cli->sio_idx >= ctx->nclients )
	    ctx->nclients = cli->sio_idx + 1;
	  ctx->clients[cli->sio_idx] = cli;
	}
      break;
    case SERVER_MSG_DELCLI:
      ctx->clients[event->arg] = NULL;
      break;
    }
  return 0;
}

static void create_client_cb(struct cbpoll_ctx *ctx,
			     void *data,
			     int64_t arg,
			     int32_t index,
			     int32_t fd)
{
  struct sndio_ctx *srv = data;
  struct sndio_client *cli = calloc(1, sizeof(struct sndio_client));
  int32_t newfd = arg & 0xFFFFFFFF;
  int32_t sio_idx = arg >> 32U;
  struct cbpoll_pipe_event evt;
  memset(&evt, 0, sizeof(evt));
  evt.fd = fd;
  evt.index = index;

  if ( cli != NULL )
    {
      cli->cbctx = ctx;
      cli->server = srv;
      cli->fd = newfd;
      cli->sio_idx = sio_idx;
      evt.msg = SERVER_MSG_ADDCLI;
      evt.arg = (uintptr_t)cli;
    } else
    {
      evt.msg = SERVER_MSG_DELCLI;
      evt.arg = sio_idx;
      close(newfd);
      newfd = -1;
    }
  if ( cbpoll_send_event(ctx, &evt) < 0 )
    {
      close(newfd);
      free(cli);
      srv->clients[sio_idx] = NULL;
    }
  //This isn't done automatically because the reply is not CBPOLL_PIPE_MSG_DEFERRED_WORK
  cbpoll_unref(ctx, srv->cbidx);
}


static int listen_fd_event(void *data, 
			   struct cbpoll_ctx *context,
			   int index,
			   int fd,
			   int revents)
{

  union sockaddr_gen addr;
  socklen_t len = sizeof(addr);
  struct sndio_ctx *ctx = data;
  ssize_t slot = -1;
  ssize_t i;
  uint64_t arg;
  int newfd = accept4(fd, (struct sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if ( newfd >= 0 )
    {
      for ( i = 0; i < MAX_CLIENTS; i++ )
	{
	  if ( ctx->clients[i] == NULL && ctx->clients[i] != (struct sndio_client*)UINTPTR_MAX )
	    {
	      slot = i;
	      break;
	    }
	}
      if ( slot >= 0 )
	{
	  ctx->clients[slot] = (struct sndio_client*)UINTPTR_MAX;
	  arg = slot;
	  arg <<= 32U;
	  arg |= newfd;
	  cbpoll_queue_deferred_work(context, index, arg, create_client_cb);
	} else if ( slot == -1 )
	{
	  close(newfd);
	}
    }
  return 0;
}




static const struct cbpoll_fd_ops sndio_listen_ops = {
  .fd_event = listen_fd_event,
  .pipe_event = listen_pipe_event,
  .destructor = NULL,
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
				      ! par->le);
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
static int cli2par(int mode,
		   const struct dspd_cli_params *clp,
		   struct sio_par *par)
{
  unsigned int bits, len, usig, be;
  if ( ! dspd_pcm_format_info(clp->format, &bits, &len, &usig, &be) )
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
}


static bool timer_destructor(void *data,
			     struct cbpoll_ctx *context,
			     int index,
			     int fd)
{
  struct sndio_ctx *ctx = data;
  dspd_timer_destroy(&ctx->tmr.timer);
  return false; //Do not close (already done)
}

static const struct cbpoll_fd_ops sndio_timer_ops = {
  .fd_event = timer_fd_event,
  .pipe_event = NULL,
  .destructor = timer_destructor,
};



int32_t dspd_sndio_new(struct sndio_ctx **ctx, struct dspd_sndio_params *params)
{
  char sockpath[PATH_MAX] = { 0 };
  int32_t uid;
  mode_t mask;
  size_t fd_count = 0, len;
  struct stat fi;
  char *tmp = NULL;
  int32_t ret = 0;
  char *saveptr = NULL;
  char *tok;
  int fd, port;
  struct sndio_ctx *sctx = calloc(1, sizeof(struct sndio_ctx));
  if ( ! sctx )
    return -ENOMEM;
  sctx->fd = -1;
  sctx->tmr.timer.fd = -1;
  sctx->cbidx = -1;

  if ( params->server_addr )
    {
      sctx->server_addr = strdup(params->server_addr);
      if ( ! sctx->server_addr )
	goto error;
    }
  sctx->ctx = params->context;
    
  ret = dspd_timer_init(&sctx->tmr.timer);
  if ( ret < 0 )
    goto error;

  if ( params->net_addrs )
    {
      for ( tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr) )
	fd_count++;
    }
  ret = cbpoll_init(&sctx->cbctx, 0, MAX_CLIENTS+2+fd_count);
  if ( ret < 0 )
    goto error;

  ret = cbpoll_add_fd(&sctx->cbctx, sctx->tmr.timer.fd, EPOLLIN, &sndio_timer_ops, sctx);
  if ( ret < 0 )
    goto error;

  cbpoll_set_callbacks(&sctx->cbctx, sctx, loop_sleep, NULL);
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
	      goto error;
	    }
	}
      if ( stat(sockpath, &fi) < 0 )
	{
	  ret = -errno;
	  goto error;
	}
      if ( fi.st_uid != uid || (fi.st_mode & mask) != 0 )
	{
	  ret = -EPERM;
	  goto error;
	}
      len = strlen(sockpath);
      sprintf(&sockpath[len], "/aucat%d", params->unit_number);
      unlink(sockpath);
      sctx->fd = dspd_unix_sock_create(sockpath, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if ( sctx->fd < 0 )
	{
	  ret = -errno;
	  goto error;
	}
      chmod(sockpath, 0777);
      sctx->cbidx = cbpoll_add_fd(&sctx->cbctx, sctx->fd, EPOLLIN, &sndio_listen_ops, sctx);
      if ( sctx->cbidx < 0 )
	{
	  ret = -errno;
	  goto error;
	}
    }
  
  if ( params->net_addrs )
    {
      tmp = strdup(params->net_addrs);
      if ( ! tmp )
	{
	  ret = -ENOMEM;
	  goto error;
	}
      if ( fd_count > 0 )
	{
	  strcpy(tmp, params->net_addrs);
	  sctx->tcp_fds = calloc(fd_count, sizeof(*sctx->tcp_fds));
	  port = AUCAT_PORT + params->unit_number;
	  for ( tok = strtok_r(tmp, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr) )
	    {
	      if ( strlen(tok) >= (sizeof(sockpath)-6) )
		{
		  ret = -EINVAL;
		  goto error;
		}
	      if ( strstr(tok, "]:") == NULL || (strstr(tok, "]") == NULL && strstr(tok, ":") == NULL) )
		sprintf(sockpath, "%s:%d", tok, port);
	      else
		strcpy(sockpath, tok);


	      fd = dspd_tcp_sock_create(sockpath, SOCK_CLOEXEC | SOCK_NONBLOCK);
	      if ( fd >= 0 )
		{
		  ret = cbpoll_add_fd(&sctx->cbctx, fd, EPOLLIN, &sndio_listen_ops, sctx);
		  if ( ret < 0 )
		    goto error;
		  sctx->tcp_fds[sctx->tcp_nfds] = fd;
		  sctx->tcp_nfds++;
		}
	    }
	}
    }
  ret = 0;
  *ctx = sctx;
  
 error:
  free(tmp);
  if ( ret < 0 )
    dspd_sndio_delete(sctx);
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
      ret = listen(ctx->fd, MAX_CLIENTS);
      if ( ret < 0 )
	return -errno;
    }
  for ( i = 0; i < ctx->tcp_nfds; i++ )
    {
      ret = listen(ctx->tcp_fds[i], MAX_CLIENTS);
      if ( ret < 0 )
	return -errno;
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
      ret = listen(ctx->fd, MAX_CLIENTS);
      if ( ret < 0 )
	return -errno;
    }
  for ( i = 0; i < ctx->tcp_nfds; i++ )
    {
      ret = listen(ctx->tcp_fds[i], MAX_CLIENTS);
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
      dspd_timer_destroy(&ctx->tmr.timer);
      for ( i = 0; i < ctx->tcp_nfds; i++ )
	close(ctx->tcp_fds[i]);
    }
  free(ctx->tcp_fds);
}


