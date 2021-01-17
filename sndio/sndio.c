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



//Enable full duplex.  Clients used to intermittently lock up on xrun but that seems to be fixed.  The real sndio 1.0.1
//client and server seem to also have this problem.
#define ENABLE_FULLDUPLEX

//Enable SIO_SYNC
//#ifdef ENABLE_XRUN_POLICIES


#define MAX_CLIENTS 32

#define ENABLE_CTL 

//#define DEBUG

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
  int32_t pclient_idx;
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
  size_t      p_datamax, c_datamax;
  size_t      p_offset;
  size_t      p_len;
  size_t      p_max, p_total;
  char        p_data[AMSG_DATAMAX];
  size_t      pxfer_offset;

  uint8_t xrun_policy;
  bool par_set;

  bool   running;
  uint64_t frames_written;
  size_t start_threshold;
  

  int32_t  last_delay;
  uint32_t last_fill;
  uint32_t max_send;
  uint64_t appl_ptr;
  uint64_t hw_ptr;

  dspd_time_t tstamp;

  int32_t sample_time;
  uint32_t delta;
  uint32_t fragtime;
  bool draining;

    
  bool vol_changed;
  bool vol_pending;
  bool vol_update;
  uint32_t volume;
  int32_t vol_elem;

  struct dspd_aio_ctx *aio;
  struct dspd_pcmcli  *pcm;
  struct dspd_cli_params params;

  volatile bool io_ready; //io is pending
  struct dspd_async_op op;

  union {
    struct {
      struct socksrv_open_reply oreply;
      struct socksrv_open_req   oreq;
#define HELLO_STATE_IDLE 0
#define HELLO_STATE_OPENING 1
#define HELLO_STATE_SETINFO 2
      uint32_t                  hello_state;
    } hello;
    struct dspd_stream_volume vol;
    bool setpar_state;
  } data;

  

  struct dspd_cli_info info;
  int32_t pdelta, cdelta;

  /*
    This is similar to the real sndiod.  The difference in
    implementation is that tickpending may sometimes not be set if the
    total pointer adjustment is too small.  This is because dspd is
    not real hardware so the block size used internally may vary dynamically.

  */
  uint32_t fillpending;
  uint32_t tickpending;
  /*
    This is not always the reciprocal of the fill level since the latency of
    the server may vary depending on what clients are connected and how the OS schedules
    the io thread.
  */
  uint32_t last_avail;

  int32_t xrun, xrun_override;
  size_t rdsil;
};


#define SERVER_MSG_ADDCLI (CBPOLL_PIPE_MSG_USER+1)
#define SERVER_MSG_DELCLI (CBPOLL_PIPE_MSG_USER+2)



struct sndio_ctx {
  struct cbpoll_client_list list;
  size_t nclients;

  struct cbpoll_ctx *cbpoll;
  int fd;
  int cbidx;
  uint8_t cookie[AMSG_COOKIELEN];
  size_t  sessrefs;


  struct dspd_daemon_ctx             *daemon;
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
  pid_t                pid;
  uid_t                uid;
  gid_t                gid;
};

struct amsg_handler {
  int pstate;
  /*
    Handle an incoming packet.  The called function must reply or change the cbpoll events as needed.
    If an operation is pending then return -EINPROGRESS or 0.
  */
  int32_t (*handler)(struct sndio_client *cli);

  /*
    Process pending handler io.  Return -EINPROGRESS to try again (usually part of a chain
    of async ops), 0 to stop, and -1 for a fatal error.
   */
  int32_t (*process)(struct sndio_client *cli, void *context, struct dspd_async_op *op);
};
int cli_procmsg(struct sndio_client *cli, void *context, struct dspd_async_op *op);
static int par2cli(const struct dspd_device_stat *info, 
		   int mode,
		   const struct amsg_par *par, 
		   struct dspd_cli_params *clp);
/*static int cli2par(int mode,
		   const struct dspd_cli_params *clp,
		   struct sio_par *par);*/
static int client_pollout(struct sndio_client *cli);
static int send_pkt(struct sndio_client *cli, size_t len, bool async);
static int client_check_buffers(struct sndio_client *cli);
static int client_buildmsg(struct sndio_client *cli, bool async);
static int send_ack(struct sndio_client *cli);
static bool client_cbtimer_event(struct cbpoll_ctx *ctx, 
				 struct dspd_cbtimer *timer,
				 void *arg, 
				 dspd_time_t timeout);

static int client_wxfer(struct sndio_client *cli);

static int32_t set_io_ready(struct sndio_client *cli, int32_t ret)
{
  DSPD_ASSERT(cli->io_ready == false);
  if ( cli->header.reserved_slot >= 0 && (ret == -EINPROGRESS || ret == 0) )
    {
      cli->io_ready = true;
      ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, 0);
    } else
    {
      ret = -EIO;
    }
  return ret;
}
static int32_t set_io_retry(struct sndio_client *cli)
{
  DSPD_ASSERT(cli->io_ready == true);
  cli->io_ready = false;
  return 0;
}

static int32_t io_sync_process(struct sndio_client *cli, int32_t ret)
{
  if ( ret < 0 )
    {
      cbpoll_ref(cli->server->cbpoll, cli->header.reserved_slot);
      dspd_aio_shutdown(cli->aio);
    }
  return ret;
}

static void set_io_complete(struct sndio_client *cli)
{
  assert(cli->io_ready == true);
  cli->io_ready = false;
}

void async_complete_cb(void *context, struct dspd_async_op *op)
{
  struct sndio_client *cli = op->data;
  set_io_complete(cli);
}

void complete_cb(void *context, struct dspd_async_op *op)
{
  struct sndio_client *cli = op->data;
  int32_t ret = cli_procmsg(cli, context, op);
  if ( ret < 0 && ret != -EINPROGRESS )
    {
      set_io_complete(cli);
      shutdown(cli->fd, SHUT_RDWR);
    } else if ( ret == 0 )
    {
      cli->imsg.cmd = UINT32_MAX;
      set_io_complete(cli);
      cli->offset_in = 0;
    }
}

static int32_t sndio_async_ctl(struct sndio_client *cli,
			       uint32_t stream,
			       uint32_t req,
			       const void          *inbuf,
			       size_t        inbufsize,
			       void         *outbuf,
			       size_t        outbufsize)
{
  int32_t ret;
  DSPD_ASSERT(cli->op.error <= 0);
  memset(&cli->op, 0, sizeof(cli->op));
  cli->op.stream = stream;
  cli->op.req = req;
  cli->op.inbuf = inbuf;
  cli->op.inbufsize = inbufsize;
  cli->op.outbuf = outbuf;
  cli->op.outbufsize = outbufsize;
  cli->op.complete = complete_cb;
  cli->op.data = cli;
  ret = dspd_aio_submit(cli->aio, &cli->op);
  return set_io_ready(cli, ret);
}



static void client_async_destructor(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr)
{
  struct sndio_client *cli = (struct sndio_client*)hdr;
  dspd_pcmcli_destroy(cli->pcm);
  free(cli->pcm);
  dspd_aio_delete(cli->aio);
  close(cli->fd);
  free(cli);
}

static void get_client_info(struct sndio_client *cli)
{
  struct ucred cred;
  socklen_t len = sizeof(cred);
  char path[PATH_MAX];
  cli->info.stream = -1;
  if ( getsockopt(cli->fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 )
    {
      if ( snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", cred.pid, cred.pid) < sizeof(path) )
	{
	  int fd = open(path, O_RDONLY), ret;
	  if ( fd >= 0 )
	    {
	      while ( (ret = read(fd, cli->info.name, sizeof(cli->info.name) - 1UL)) < 0 )
		{
		  ret = errno;
		  if ( ret != EINTR && ret != EWOULDBLOCK && ret != EAGAIN )
		    break;
		}
	      close(fd);
	      char *p = strchr(cli->info.name, '\n');
	      if ( p )
		*p = 0;
	    }
	}
      
      //If this is not running inside the daemon then the client pid can't be used.
      if ( cli->server->pid < 0 )
	{
	  cli->info.pid = cred.pid;
	  cli->info.uid = cred.uid;
	  cli->info.gid = cred.gid;
	} else
	{
	  cli->info.pid = cli->server->pid;
	  cli->info.gid = cli->server->gid;
	  cli->info.uid = cli->server->uid;
	}
    }
}

static void client_shutdown_cb(struct dspd_aio_ctx *aio, void *arg)
{
  struct sndio_client *cli = arg;
  cbpoll_remove_aio(cli->server->cbpoll, aio);
  cbpoll_unref(cli->server->cbpoll, cli->header.reserved_slot);
}

static struct cbpoll_client_hdr *client_create(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr, void *arg)
{
  struct sndio_ctx *srv = (struct sndio_ctx*)hdr->list;
  struct sndio_client *cli = calloc(1, sizeof(struct sndio_client));
  
  if ( cli != NULL )
    {
      cli->header = *hdr;
      cli->server = srv;
      cli->fd = hdr->fd;
      cli->pcm = calloc(1, dspd_pcmcli_sizeof());
      cli->pclient_idx = -1;
      get_client_info(cli);
      if ( cli->pcm == NULL ||
	   cbpoll_aio_new(srv->cbpoll, &cli->aio, NULL, cli->server->daemon, client_shutdown_cb, cli) < 0 )
	{
	  client_async_destructor(ctx, &cli->header);
	  cli = NULL;
	}
    }
  return (struct cbpoll_client_hdr*)cli;
}




static bool client_success(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr)
{
  struct sndio_client *cli = (struct sndio_client*)hdr;
  bool ret = false;
  if ( cbpoll_add_aio(ctx, cli->aio, hdr->reserved_slot) >= 0 )
    {
      cli->timer = dspd_cbtimer_new(ctx, client_cbtimer_event, cli);
      if ( cli->timer )
	{
	  if ( hdr->list_index >= 0 && hdr->list_index >= cli->server->nclients )
	    cli->server->nclients = hdr->list_index + 1UL;
	  ret = true;
	}
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
  if ( cli->pstate > 0 )
    cli->server->sessrefs--;
  if ( cli->pclient_idx >= 0 )
    cli->server->cli_map[cli->pclient_idx] = -1;
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
  int32_t ret;
  bool result = true;
  uint64_t hw, appl;
  ret = client_wxfer(cli);
  if ( ret == 0 )
    {
      if ( cli->draining )
	{
	  ret = dspd_pcmcli_avail(cli->pcm, DSPD_PCM_SBIT_PLAYBACK, &hw, &appl);
	  if ( (ret == -EPIPE || hw == appl) && cli->io_ready == false && cli->offset_in == 0 && cli->offset_out == cli->len_out )
	    {
	      ret = dspd_pcmcli_stop(cli->pcm, DSPD_PCM_SBIT_PLAYBACK, async_complete_cb, cli);
	      if ( ret < 0 || send_ack(cli) < 0 )
		shutdown(cli->fd, SHUT_RDWR);
	      cli->draining = false;
	      result = false;
	    } else if ( ret < 0 )
	    {
	      shutdown(cli->fd, SHUT_RDWR);
	      result = false;
	    }
	} else if ( ! cli->running )
	{
	  result = false;
	} else if ( client_check_buffers(cli) < 0 ||
		    client_buildmsg(cli, false) < 0 )
	{
	  shutdown(cli->fd, SHUT_RDWR);
	  result = false;
	} 
    } else
    {
      result = false;
    }
  return result;
}

static void client_reset(struct sndio_client *cli)
{
   cli->running = false;
   cli->tickpending = 0;
   cli->frames_written = 0;
   cli->delta = 0;
   cli->last_delay = 0;
   cli->last_fill = 0;
   cli->appl_ptr = 0;
   cli->hw_ptr = 0;
   cli->rdsil = 0;
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
void prepare_complete_cb(void *context, struct dspd_async_op *op)
{
  struct sndio_client *cli = op->data;
  cli->running = false;
  set_io_complete(cli);
  if ( cli->imsg.cmd != UINT32_MAX && cli->offset_in == 0 )
    if ( cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLIN) < 0 )
      shutdown(cli->fd, SHUT_RDWR);
}


static int32_t client_pdelta(struct sndio_client *cli, int32_t avail)
{
  uint32_t delay = cli->params.bufsize - avail;
  int32_t delta = 0, mdelta;
  if ( avail > cli->last_avail )
    {
      cli->fillpending += avail - cli->last_avail;
      if ( cli->fillpending >= cli->params.fragsize || (cli->xrun & DSPD_PCM_SBIT_PLAYBACK) )
	{
	  cli->tickpending++;
	  cli->xrun &= ~DSPD_PCM_SBIT_PLAYBACK;
	}
      cli->last_avail = avail;
    }
  if ( delay < cli->last_delay )
    {
      delta = cli->last_delay - delay;
      cli->last_delay = delay;
      mdelta = cli->appl_ptr - cli->hw_ptr;
      if ( delta > mdelta )
	delta = mdelta;
      cli->hw_ptr += delta;
    }
  return delta;
}
static int32_t client_cdelta(struct sndio_client *cli, int32_t avail)
{
  int32_t delta = 0;
  if ( avail > cli->last_fill )
    {
      delta = avail - cli->last_fill;
      cli->last_fill = avail;
    }
  return delta;
}
static int32_t playback_avail(struct sndio_client *cli, int32_t *avail)
{
  uint64_t hw, appl;
  int32_t ret = dspd_pcmcli_avail(cli->pcm, DSPD_PCM_SBIT_PLAYBACK, &hw, &appl);
  if ( ret >= 0 )
    *avail = ret;
  else
    *avail = 0;
  if ( hw == appl && (cli->xrun_override & DSPD_PCM_SBIT_PLAYBACK) == 0 )
    ret = -EPIPE;
  return ret;
}

static int32_t capture_avail(struct sndio_client *cli, int32_t *avail)
{
  int32_t ret = dspd_pcmcli_avail(cli->pcm, DSPD_PCM_SBIT_CAPTURE, NULL, NULL);
  if ( ret >= 0 )
    *avail = ret;
  else
    *avail = 0;
  if ( ret >= cli->params.bufsize && (cli->xrun_override & DSPD_PCM_SBIT_CAPTURE) == 0 )
    ret = -EPIPE;
  if ( (cli->streams & DSPD_PCM_SBIT_PLAYBACK) == 0 )
    cli->max_send = ret;
  return ret;
}

static int buffer_xrun(struct sndio_client *cli, int32_t streams, int32_t p_avail, int32_t c_avail)
{
  int32_t ret = 0;
  if ( cli->xrun_policy == SIO_ERROR )
    {
      ret = shutdown(cli->fd, SHUT_RDWR);
    } else if ( cli->xrun_policy == SIO_IGNORE )
    {
      if ( cli->streams == DSPD_PCM_SBIT_FULLDUPLEX )
	{
	  cli->xrun |= streams;
	}
      /*
	There is nothing special to do for half duplex streams since pausing the pointer
	already occurs.  The problem with full duplex streams is that there are two pointers
	and one stream will usually xrun (or just be caught) before the other one.
      */
      
    } else if ( cli->xrun_policy == SIO_SYNC )
    {
      //TODO: Let the client see how far the hardware pointer has moved since the xrun
    }
  return ret;
}

int client_check_buffers(struct sndio_client *cli)
{
  int32_t p_avail = 0, c_avail = 0, p_delta = 0, c_delta = 0, delta, sbits;
  int32_t ret = 0, p = 0, c = 0, xrun = 0;
  dspd_time_t nextwakeup = 0;
  if ( cli->running == true )
    {
      if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	p = playback_avail(cli, &p_avail);
      if ( cli->streams & DSPD_PCM_SBIT_CAPTURE )
	c = capture_avail(cli, &c_avail);
      if ( p == -EPIPE || c == -EPIPE )
	{
	  if ( p == -EPIPE )
	    xrun |= DSPD_PCM_SBIT_PLAYBACK;
	  if ( c == -EPIPE )
	    xrun |= DSPD_PCM_SBIT_CAPTURE;
	  ret = buffer_xrun(cli, xrun, p_avail, c_avail);
	} else if ( p < 0 )
	{
	  ret = p;
	} else if ( c < 0 )
	{
	  ret = c;
	} else
	{
	  cli->xrun_override = 0;
	  cli->xrun = 0;
	  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	    {
	      p_delta = client_pdelta(cli, p_avail);
	      cli->pdelta += p_delta;
	    }
	  if ( cli->streams & DSPD_PCM_SBIT_CAPTURE )
	    {
	      c_delta = client_cdelta(cli, c_avail);
	      cli->cdelta += c_delta;
	    }
	  delta = MAX(cli->pdelta, cli->cdelta);
	  cli->delta += delta;
	  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	    cli->pdelta -= delta;
	  if ( cli->streams & DSPD_PCM_SBIT_CAPTURE )
	    cli->cdelta -= delta;
	  
	  sbits = cli->streams;
	  ret = dspd_pcmcli_get_next_wakeup(cli->pcm, NULL, &sbits, &nextwakeup);
	  if ( ret == 0 && nextwakeup > 0 && p_avail == 0 && c_avail == 0 ) 
	    set_client_wakeup(cli, &nextwakeup, &cli->fragtime);
	}
    }
  return ret;
}

static ssize_t client_read_frames(struct sndio_client *cli, char *buf, size_t len)
{
  ssize_t total = 0, ret = 0, n;
  uint64_t frames;
  if ( cli->rdsil > 0 )
    {
      n = MIN(cli->rdsil, len);
      frames = n;
      ret = dspd_pcmcli_forward(cli->pcm, DSPD_PCM_SBIT_CAPTURE, &frames);
      if ( ret == 0 )
	{
	  n = frames;
	  ret = dspd_pcm_fill_silence(cli->params.format, buf, n * DSPD_CLI_CCHAN(cli->params.channels));
	  DSPD_ASSERT(ret > 0);
	  total += ret;
	  cli->rdsil -= ret;
	  ret = 0;
	}
    }
  if ( ret == 0 && total < len && cli->rdsil == 0 )
    {
      n = len - total;
      ret = dspd_pcmcli_read_frames(cli->pcm, 
				    buf+(cli->cframe_bytes * total),
				    n);
      if ( ret > 0 )
	total += ret;
    }
  if ( total > 0 )
    ret = total;
  return ret;
}

static int client_buildmsg(struct sndio_client *cli, bool async)
{
  int ret = 0;
  size_t max_read;
  if ( cli->running == false || cli->cstate != CLIENT_STATE_IDLE || cli->io_ready == true )
    return 0;
  if ( cli->tickpending )
    {
      if ( cli->fillpending > 0 )
	{
	  AMSG_INIT(&cli->opkt.msg);
	  cli->opkt.msg.cmd = htonl(AMSG_FLOWCTL);
	  cli->opkt.msg.u.ts.delta = htonl(cli->fillpending);
	  cli->fillpending = 0;
	  cli->tickpending = 0;
	  ret = send_pkt(cli, sizeof(struct amsg), async);
	}
    } else if ( cli->delta > 0 )
    {
      cli->opkt.msg.cmd = htonl(AMSG_MOVE);
      cli->opkt.msg.u.ts.delta = htonl(cli->delta);
      cli->delta = 0;
      ret = send_pkt(cli, sizeof(struct amsg), async);
    } else if ( cli->last_fill > 0 && cli->max_send > 0 && 
		((cli->xrun & DSPD_PCM_SBIT_CAPTURE) == 0 || (cli->xrun_override & DSPD_PCM_SBIT_CAPTURE)))
    {
      AMSG_INIT(&cli->opkt.msg);
      cli->opkt.msg.cmd = htonl(AMSG_DATA);
      //cli->max_send is the maximum amount that can be sent if the buffer
      //actually has that much data
      uint32_t max_send = MIN(cli->last_fill, cli->max_send);
      max_read = MIN(max_send, cli->c_datamax);
      ret = client_read_frames(cli, cli->opkt.buf, max_read);
      if ( ret > 0 )
	{
	  cli->last_fill -= ret;
	  cli->max_send -= ret;
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



void trigger_complete_cb(void *context, struct dspd_async_op *op)
{
  int32_t ret;
  int32_t sbits;
  dspd_time_t tstamp;
  struct sndio_client *cli = op->data;
  if ( op->error == 0 )
    {
      sbits = cli->streams;
      ret = dspd_pcmcli_get_next_wakeup(cli->pcm, NULL, &sbits, &tstamp);
      if ( ret == 0 )
	{
	  tstamp += cli->fragtime;
	  set_client_wakeup(cli, &tstamp, &cli->fragtime);
	  cli->running = true;
	} else
	{
	  op->error = ret;
	}
    }
  complete_cb(context, op);
}

int client_trigger(struct sndio_client *cli, int32_t sbits)
{
  int32_t err;
  err = dspd_pcmcli_settrigger(cli->pcm, sbits, trigger_complete_cb, cli);
  return set_io_ready(cli, err);
}

static ssize_t client_recv(int fd, char *buf, size_t len)
{
  int32_t ret = 0;
  if ( len > 0 )
    {
      ret = read(fd, buf, len);
      if ( ret < 0 )
	{
	  ret = -errno;
	  if ( ret == -EWOULDBLOCK || ret == -EAGAIN || ret == -EINTR )
	    ret = 0;
	} else if ( ret == 0 )
	{
	  ret = -ECONNABORTED;
	}
    }
  return ret;
}

static int client_wxfer(struct sndio_client *cli)
{
  ssize_t ret;
  size_t fr;
  if ( cli->cstate != CLIENT_STATE_RXDATA )
    return 0;
  ret = client_recv(cli->fd, &cli->p_data[cli->p_offset], cli->p_max - cli->p_offset);
  if ( ret >= 0 )
    {
      cli->p_offset += ret;
      fr = (cli->p_offset - cli->pxfer_offset) / cli->pframe_bytes;
      if ( fr > 0 )
	{
	  if ( (cli->xrun & DSPD_PCM_SBIT_PLAYBACK) != 0 && (cli->xrun_override & DSPD_PCM_SBIT_PLAYBACK) == 0 )
	    {
	      int32_t avail = dspd_pcmcli_avail(cli->pcm, DSPD_PCM_SBIT_PLAYBACK, NULL, NULL);
	      if ( avail > cli->last_avail )
		{
		  int32_t wsil = avail - cli->last_avail;
		  ret = dspd_pcmcli_write_frames(cli->pcm, NULL, wsil);
		  if ( ret > 0 )
		    {
		      cli->last_avail += wsil - ret;
		      cli->xrun_override |= DSPD_PCM_SBIT_PLAYBACK;
		    }
		}
	    }

	  ret = dspd_pcmcli_write_frames(cli->pcm, &cli->p_data[cli->pxfer_offset], fr);
	  if ( ret == -EAGAIN )
	    {
	      ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, 0);
	    } else if ( ret > 0 )
	    {
	      fr = ret;
	      cli->pxfer_offset += ret * cli->pframe_bytes;
	      cli->frames_written += fr;
	      cli->appl_ptr += fr;
	      cli->last_avail -= fr;
	      cli->last_delay += fr;
	      cli->max_send += fr;

	      if ( cli->xrun & DSPD_PCM_SBIT_CAPTURE )
		{
		  int32_t avail = dspd_pcmcli_avail(cli->pcm, DSPD_PCM_SBIT_CAPTURE, NULL, NULL);
		  if ( avail > cli->last_fill )
		    {
		      uint64_t f = avail - cli->last_fill, f2 = f;
		      if ( dspd_pcmcli_forward(cli->pcm, DSPD_PCM_SBIT_CAPTURE, &f) == 0 )
			{
			  cli->last_fill -= f2 - f;
			  cli->rdsil = cli->last_fill;
			  cli->xrun &= ~DSPD_PCM_SBIT_CAPTURE;
			  cli->xrun_override |= DSPD_PCM_SBIT_CAPTURE;
			}
		    } else
		    {
		      cli->xrun_override |= DSPD_PCM_SBIT_CAPTURE;
		    }
		}
	      if ( cli->p_offset == cli->p_max && cli->pxfer_offset == cli->p_offset )
		{
		  cli->p_max = 0;
		  cli->cstate = CLIENT_STATE_IDLE;
		  ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLIN);
		} else
		{
		  ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, 0);
		}
	      if ( cli->running == false && cli->frames_written >= cli->start_threshold )
		ret = client_trigger(cli, cli->streams);
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
	ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLOUT);
      else
	cli->cstate = CLIENT_STATE_IDLE;
    } else
    {
      ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLOUT);
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
static int send_none(struct sndio_client *cli, int ret, bool ready)
{
  cli->cstate = CLIENT_STATE_IDLE;
  cli->offset_in = 0;
  if ( ready == true && ret == 0 )
    ret = cbpoll_enable_events(cli->server->cbpoll, cli->header.reserved_slot, POLLIN);
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
	  ret = -EACCES;
	}
    }
  return send_none(cli, ret, false);
}

/*static void route_changed(int32_t dev, int32_t index, void *client, int32_t err, void *arg)
{
  //Nothing to do since the device isn't accessed directly.
  return;
  }*/


static int amsg_hello_process(struct sndio_client *cli, void *context, struct dspd_async_op *op)
{
  struct socksrv_open_reply *oreply = op->outbuf;
  int32_t ret = -EINVAL;
  struct dspd_pcmcli_bindparams params = { 0 };
  if ( op->error < 0 )
    {
      ret = op->error;
    } else if ( cli->data.hello.hello_state == HELLO_STATE_OPENING )
    {
      if ( op->outbufsize != sizeof(*oreply) )
	{
	  ret = -EPROTO;
	} else
	{
	  ret = dspd_pcmcli_init(cli->pcm, cli->streams, DSPD_PCMCLI_NOTIMER | DSPD_PCMCLI_NONBLOCK | DSPD_PCMCLI_CONSTANT_LATENCY);
	  if ( ret == 0 )
	    ret = dspd_pcmcli_set_no_xrun(cli->pcm, true);
	  if ( ret == 0 )
	    {
	      params.playback_stream = oreply->playback_stream;
	      params.capture_stream = oreply->capture_stream;
	      params.playback_device = oreply->playback_device;
	      params.capture_stream = oreply->capture_device;
	      params.context = cli->aio;
	      params.playback_info = &oreply->playback_info;
	      params.capture_info = &oreply->capture_info;
	      ret = dspd_pcmcli_bind(cli->pcm, &params, DSPD_PCMCLI_BIND_CONNECTED, NULL, NULL);
	      if ( ret == 0 )
		{
		  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
		    {
		      //TODO: Routing callbacks
		      cli->pclient_idx = params.playback_stream;
		      if ( cli->pclient_idx >= 0 )
			cli->server->cli_map[cli->pclient_idx] = cli->header.list_index;
		    }
		  ret = set_io_retry(cli);
		  if ( ret == 0 )
		    {
		      ret = set_io_ready(cli, dspd_aio_set_info(cli->aio, &cli->info, complete_cb, cli));
		      if ( ret == 0 )
			{
			  cli->data.hello.hello_state = HELLO_STATE_SETINFO;
			  ret = -EINPROGRESS;
			}
		    }
		}
	    }
	}
    } else if ( cli->data.hello.hello_state == HELLO_STATE_SETINFO )
    {
      cli->pstate = PROTO_INIT;
      cli->data.hello.hello_state = HELLO_STATE_IDLE;
      ret = send_ack(cli);
    }
  return ret;
}

static int amsg_hello(struct sndio_client *cli)
{
  const struct amsg_hello *h = &cli->imsg.u.hello;
  struct socksrv_open_req *oreq = &cli->data.hello.oreq;
  uint16_t mode = ntohs(h->mode);
  int ret = -1;
  memset(&cli->data.hello, 0, sizeof(cli->data.hello));
#ifndef ENABLE_FULLDUPLEX
  if ( mode == DSPD_PCM_SBIT_FULLDUPLEX )
    return -EINVAL;
#endif

  if ( ! ((mode & ~(DSPD_PCM_SBIT_PLAYBACK|DSPD_PCM_SBIT_CAPTURE)) != 0 ||
	  h->version != AMSG_VERSION || mode == 0 ))
    {
      oreq->sbits = mode;
      oreq->flags = 0;
      snprintf(oreq->name, sizeof(oreq->name), "hw:%hhd", h->devnum);
      cli->streams = mode;

      ret = sndio_async_ctl(cli, 
			    -1, 
			    DSPD_SOCKSRV_REQ_OPEN_BY_NAME,
			    &cli->data.hello.oreq,
			    sizeof(cli->data.hello.oreq),
			    &cli->data.hello.oreply,
			    sizeof(cli->data.hello.oreply));
      if ( ret == 0 )
	cli->data.hello.hello_state = HELLO_STATE_OPENING;
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
  pparams->xflags |= DSPD_CLI_XFLAG_FULLDUPLEX_CHANNELS;
  pparams->channels = DSPD_CLI_FDCHAN(pparams->channels, cparams->channels);
}


static int amsg_setpar_process(struct sndio_client *cli, void *context, struct dspd_async_op *op)
{
  struct amsg_par *par = &cli->imsg.u.par;
  int ret;
  if ( op->error < 0 )
    return op->error;
  if ( cli->data.setpar_state == false )
    {
      ret = dspd_pcmcli_get_hwparams(cli->pcm, &cli->params);
      if ( ret == 0 )
	{
	  cli->data.setpar_state = true;
	  ret = dspd_pcmcli_prepare(cli->pcm, complete_cb, cli);
	}
    } else
    {
      cli->pframe_bytes = dspd_pcmcli_frames_to_bytes(cli->pcm, NULL, DSPD_PCM_SBIT_PLAYBACK, 1UL);
      if ( cli->pframe_bytes > 0 )
	cli->p_datamax = AMSG_DATAMAX / cli->pframe_bytes;

      cli->cframe_bytes = dspd_pcmcli_frames_to_bytes(cli->pcm, NULL, DSPD_PCM_SBIT_CAPTURE, 1UL);
      if ( cli->cframe_bytes > 0 )
	cli->c_datamax = AMSG_DATAMAX / cli->cframe_bytes;
      

      cli->start_threshold = cli->params.bufsize;
      if ( cli->params.rate )
	cli->sample_time = 1000000000 / cli->params.rate;
      cli->fragtime = cli->sample_time * cli->params.fragsize;
      cli->par_set = true;
      cli->pstate = PROTO_CONFIGURED;
#ifdef ENABLE_XRUN_POLICIES
      if ( AMSG_ISSET(par->xrun) )
	cli->xrun_policy = par->xrun;
      else
	cli->xrun_policy = SIO_IGNORE;
#else
      if ( AMSG_ISSET(par->xrun) && par->xrun == SIO_ERROR )
	cli->xrun_policy = SIO_ERROR;
      else
	cli->xrun_policy = SIO_IGNORE;
	
#endif

      ret = send_none(cli, 0, true);
    }
  return ret;
}

static int amsg_setpar(struct sndio_client *cli)
{
  struct dspd_cli_params pparams = { 0 }, cparams = { 0 };
  struct dspd_cli_params *params;
  int32_t ret = 0;
  struct amsg_par *par = &cli->imsg.u.par;
  if ( cli->pstate != PROTO_INIT && cli->pstate != PROTO_CONFIGURED )
    return -EBADFD;
  cli->data.setpar_state = false;
  par2cpu(par);
 
  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      
      ret = par2cli(dspd_pcmcli_device_info(cli->pcm, DSPD_PCM_SBIT_PLAYBACK),
		    DSPD_PCM_SBIT_PLAYBACK,
		    par,
		    &pparams);
    } else
    {
      ret = 0;
    }
  if ( ret == 0 && (cli->streams & DSPD_PCM_SBIT_CAPTURE) )
    {
      ret = par2cli(dspd_pcmcli_device_info(cli->pcm, DSPD_PCM_SBIT_CAPTURE),
		    DSPD_PCM_SBIT_CAPTURE,
		    par,
		    &cparams);
    }
  if ( ret == 0 )
    {
      if ( cli->streams == DSPD_PCM_SBIT_FULLDUPLEX )
	combine_params(&pparams, &cparams);
      if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
	params = &pparams;
      else
	params = &cparams;
      if ( ! cli->server->daemon )
	params->flags |= DSPD_CLI_FLAG_SHM;
      params->stream = cli->streams;
#ifdef DEBUG
      dspd_dump_params(params, stdout);
#endif
      ret = set_io_ready(cli, dspd_pcmcli_set_hwparams_async(cli->pcm, params, complete_cb, cli));
    }
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
      ret = -EBADFD;
    } else if ( cli->par_set )
    {
      
      
      fmt = cli->params.format;
      clp = &cli->params;
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
	    par->pchan = ntohs(dspd_pcmcli_hw_params_get_channels(cli->pcm, NULL, DSPD_PCM_SBIT_PLAYBACK));
	  if ( cli->streams & DSPD_PCM_SBIT_CAPTURE )
	    par->rchan = ntohs(dspd_pcmcli_hw_params_get_channels(cli->pcm, NULL, DSPD_PCM_SBIT_CAPTURE));
	  ret = send_rmsg(cli, AMSG_GETPAR);
	} else
	{
	  ret = -EINVAL;
	}

    } else
    {
      //TODO: Fake config from default par (memset 0xFF)
      ret = -EINVAL;
    }
  return ret;
}

static int amsg_start_process(struct sndio_client *cli, void *context, struct dspd_async_op *op)
{
  int32_t ret = op->error;
  if ( ret == 0 )
    {
      cli->pstate = PROTO_TRIGGERED;
      if ( cli->streams == DSPD_PCM_SBIT_CAPTURE )
	ret = send_none(cli, 0, false);
      else
	ret = send_pkt(cli, sizeof(cli->opkt.msg), false);
    }
  return ret;
}

static int amsg_start(struct sndio_client *cli)
{
  int32_t ret;
  cli->frames_written = 0;
  cli->pdelta = 0;
  cli->cdelta = 0;
  cli->fillpending = cli->params.bufsize - cli->params.fragsize;
  cli->last_avail = cli->fillpending;
  cli->start_threshold = cli->fillpending;

  AMSG_INIT(&cli->opkt.msg);
  cli->opkt.msg.cmd = htonl(AMSG_FLOWCTL);
  cli->opkt.msg.u.ts.delta = htonl(cli->fillpending);
  if ( cli->streams == DSPD_PCM_SBIT_CAPTURE )
    {
      ret = client_trigger(cli, DSPD_PCM_SBIT_CAPTURE);
    } else
    {
      cli->pstate = PROTO_TRIGGERED;
      ret = send_pkt(cli, sizeof(cli->opkt.msg), false);
    }
  cli->fillpending = 0;
  return ret;
}



static int amsg_stop_process(struct sndio_client *cli, void *context, struct dspd_async_op *op)
{
  int ret;
  if ( ! (cli->streams & DSPD_PCM_SBIT_PLAYBACK) )
    client_reset(cli);
  cli->running = false;
  if ( op->error == 0 && cli->draining == false )
    ret = send_ack(cli);
  else
    ret = send_none(cli, 0, false);
  return ret;
}

static int amsg_stop(struct sndio_client *cli)
{
  int ret = 0;
  int32_t sbits = 0;
  cli->pstate = PROTO_CONFIGURED;
  if ( (cli->streams & DSPD_PCM_SBIT_PLAYBACK) && ret == 0 )
    {
      if ( cli->running == false && cli->frames_written > 0 )
	{
	  cli->draining = true;
	  ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, 0);
	  sbits |= DSPD_PCM_SBIT_PLAYBACK;
	} 
    }
  if ( ret == 0 )
    ret = client_trigger(cli, sbits);
  return ret;
}


static int amsg_data_process(struct sndio_client *cli, void *context, struct dspd_async_op *op)
{
  int32_t ret = client_check_buffers(cli);
  if ( ret == 0 )
    ret = client_wxfer(cli);
  return ret;
}

static int amsg_data(struct sndio_client *cli)
{
  size_t size = ntohl(cli->imsg.u.data.size);
  int ret = -EBADFD;
  if ( ! ((cli->streams & DSPD_PCM_SBIT_PLAYBACK) == 0 ||
	  (size % cli->pframe_bytes) != 0 ||
	  size > AMSG_DATAMAX) )
    {
      cli->p_max = size;
      cli->p_total = 0;
      cli->p_len = 0;
      cli->p_offset = 0;
      cli->cstate = CLIENT_STATE_RXDATA;
      cli->pxfer_offset = 0;
      cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLIN);
      ret = client_wxfer(cli);
    }
  return ret;
}

static int amsg_setvol_process(struct sndio_client *cli, void *context, struct dspd_async_op *op)
{
  cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLIN);
  return op->error;
}

static int amsg_setvol(struct sndio_client *cli)
{
  /*
    Set the volume.  No need to send any replies.  Do not trigger any feedback events since
    the sender knows what they did.
   */
  uint32_t v = ntohl(cli->imsg.u.vol.ctl);
  int ret;
  struct dspd_stream_volume *sv = &cli->data.vol;
  if ( v > SIO_MAXVOL )
    v = SIO_MAXVOL;
  if ( cli->streams & DSPD_PCM_SBIT_PLAYBACK )
    {
      memset(sv, 0, sizeof(*sv));
      sv->stream = DSPD_PCM_SBIT_PLAYBACK;
      sv->volume = (float)v / (float)SIO_MAXVOL;
      //If the op fails then the client will be disconnected.  If the op succeeds
      //then there will be no volume event for this operation.
      cli->volume = v;
      ret = sndio_async_ctl(cli,
			    -1,
			    DSPD_SCTL_CLIENT_SETVOLUME,
			    sv,
			    sizeof(sv),
			    NULL,
			    0);
    } else
    {
      ret = 0;
    }
  return ret;
}

static int amsg_bye(struct sndio_client *cli)
{
  shutdown(cli->fd, SHUT_RDWR);
  return -ESHUTDOWN;
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
    .process = amsg_hello_process,
  },
  [AMSG_SETPAR] = {
    .pstate = -1,
    .handler = amsg_setpar,
    .process = amsg_setpar_process,
  },
  [AMSG_GETPAR] = {
    .pstate = -1,
    .handler = amsg_getpar,
  },
  [AMSG_START] = {
    .pstate = PROTO_CONFIGURED,
    .handler = amsg_start,
    .process = amsg_start_process,
  },
  [AMSG_STOP] = {
    .pstate = PROTO_TRIGGERED,
    .handler = amsg_stop,
    .process = amsg_stop_process,
  },
  [AMSG_DATA] = {
    .pstate = PROTO_TRIGGERED,
    .handler = amsg_data,
    .process = amsg_data_process,
  },
  [AMSG_SETVOL] = {
    .pstate = -1,
    .handler = amsg_setvol,
    .process = amsg_setvol_process,
  },
  [AMSG_BYE] = {
    .pstate = -1,
    .handler = amsg_bye,
  },
};

int cli_startmsg(struct sndio_client *cli)
{
  size_t cmd = (size_t)ntohl(cli->imsg.cmd);
  struct amsg_handler *h;
  int ret = -1;
  if ( cmd < _AMSG_COUNT )
    {
      h = &amsg_handlers[cmd];
      if ( h->handler != NULL && 
	   ((cli->pstate == h->pstate) || (h->pstate == -1 && cli->pstate >= PROTO_INIT)))
	{
	  ret = h->handler(cli);
	  if ( ret == -EINPROGRESS )
	    ret = 0;
	}
    }
  cli->offset_in = 0;
  return ret;
}

int cli_procmsg(struct sndio_client *cli, void *context, struct dspd_async_op *op)
{
  size_t cmd = (size_t)ntohl(cli->imsg.cmd);
  struct amsg_handler *h = NULL;
  int ret = -1;
  if ( cmd < _AMSG_COUNT )
    {
      h = &amsg_handlers[cmd];
      if ( h->process != NULL && 
	   ((cli->pstate == h->pstate) || (h->pstate == -1 && cli->pstate >= PROTO_INIT)))
	{
	  ret = h->process(cli, context, op);
	}
    }
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
	    ret = cli_startmsg(cli);
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
      ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLOUT);
      break;
    }
  return ret;
}

static int client_pollout(struct sndio_client *cli)
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
		  ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLIN);
	    } else
	    {
	      ret = 0;
	    }
	}
    } else
    {
      ret = cbpoll_set_events(cli->server->cbpoll, cli->header.reserved_slot, POLLIN);
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
  DSPD_ASSERT(cli->timer != NULL);
  if ( revents & (POLLHUP|POLLRDHUP|POLLNVAL|POLLERR) )
    ret = -EIO;
  else if ( revents & POLLIN )
    ret = client_pollin(cli);
  else if ( revents & POLLOUT )
    ret = client_pollout(cli);
  else
    ret = 0;
  DSPD_ASSERT(cli->timer != NULL);
  ret = io_sync_process(cli, ret);
#ifdef DEBUG
  if ( ret < 0 )
    fprintf(stderr, "Client error %d\n", ret);
#endif
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
	      ctx->cli_vol_index = sc->header.list_index;
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
		  if ( (sc->streams & DSPD_PCM_SBIT_PLAYBACK) && sc->pclient_idx == stream )
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
  if ( ctx->daemon )
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
  sctx->pid = -1;
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
  sctx->daemon = params->context;
    

  if ( params->net_addrs )
    {
      for ( tok = strchr(params->net_addrs, ','); tok; tok = strchr(&tok[1], ',') )
	fd_count++;
    }

  if ( ! sctx->daemon )
    {
      sctx->cbpoll = calloc(1, sizeof(*sctx->cbpoll));
      if ( ! sctx->cbpoll )
	{
	  ret = -errno;
	  goto out;
	}
      ret = cbpoll_init(sctx->cbpoll, CBPOLL_FLAG_TIMER|CBPOLL_FLAG_CBTIMER, MAX_CLIENTS+3+fd_count);
      if ( ret < 0 )
	goto out;
    } else
    {
      sctx->cbpoll = sctx->daemon->main_thread_loop_context;
    }



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
      sctx->cbidx = cbpoll_add_fd(sctx->cbpoll, sctx->fd, EPOLLIN|EPOLLONESHOT, &sndio_listen_ops, sctx);
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
		  ret = cbpoll_add_fd(sctx->cbpoll, fd, EPOLLIN|EPOLLONESHOT, &sndio_listen_ops, sctx);
		  if ( ret < 0 )
		    goto out;
		  sctx->tcp_fds[sctx->tcp_nfds] = fd;
		  sctx->tcp_nfds++;
		}
	    }
	}
    }

#ifdef ENABLE_CTL
  if ( sctx->daemon )
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
  if ( sctx->daemon )
    ret = dspd_aio_connect(sctx->aio, NULL, sctx->daemon, &dspd_aio_fifo_eventfd_ops, &sctx->efd);
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
  
  if ( sctx->daemon )
    ret = cbpoll_add_fd(sctx->cbpoll, sctx->efd.fd, EPOLLIN, &sndio_ctl_ops, sctx);
  else
    ret = cbpoll_add_fd(sctx->cbpoll, dspd_aio_get_iofd(sctx->aio), dspd_aio_block_directions(sctx->aio), &sndio_ctl_ops, sctx);
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
    dspd_sndio_delete(sctx);
  return ret;
}

static void set_cred(struct sndio_ctx *ctx)
{
  ctx->uid = getuid();
  ctx->gid = getgid();
  ctx->pid = getpid();
}

int32_t dspd_sndio_start(struct sndio_ctx *ctx)
{
  int32_t ret = 0;
  size_t i;
  if ( ! ctx->daemon )
    {
      ret = cbpoll_set_name(ctx->cbpoll, "dspd-sndiod");
      if ( ret < 0 )
	return ret;
      set_cred(ctx);
    }
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
  if ( ! ctx->daemon )
    {
      ret = cbpoll_start(ctx->cbpoll);
      if ( ret == 0 )
	ctx->started = true;
    } else if ( ret == 0 )
    {
      ctx->started = true;
    }
  return ret;
}

int32_t dspd_sndio_run(struct sndio_ctx *ctx)
{
  int32_t ret;
  size_t i;
  if ( ! ctx->daemon )
    set_cred(ctx);
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
  ret = cbpoll_run(ctx->cbpoll);
  ctx->started = false;
  return ret;
}

void dspd_sndio_delete(struct sndio_ctx *ctx)
{
  size_t i;
  if ( ctx->started && ctx->daemon == NULL )
    {
      cbpoll_destroy(ctx->cbpoll);
      free(ctx->cbpoll);
      ctx->cbpoll = NULL;
    }
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


