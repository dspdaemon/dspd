#ifndef _DSPD_PCMCLI_H_
#define _DSPD_PCMCLI_H_

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "sslib.h"
#include "pcmcli_stream.h"



struct dspd_pcmcli_status;
struct dspd_pcmcli;
typedef struct _dspd_pcmcli_timer dspd_pcmcli_timer_t;

typedef ssize_t (*dspd_pcmcli_io_cb_t)(struct dspd_pcmcli *cli, struct dspd_pcmcli_status *status, void *arg);

typedef int (*dspd_pcmcli_timer_fire_t)(dspd_pcmcli_timer_t *tmr, bool latch);
typedef int (*dspd_pcmcli_timer_reset_t)(dspd_pcmcli_timer_t *tmr);
typedef int (*dspd_pcmcli_timer_get_t)(dspd_pcmcli_timer_t *tmr, dspd_time_t *abstime, uint32_t *per);
typedef int (*dspd_pcmcli_timer_set_t)(dspd_pcmcli_timer_t *tmr, uint64_t abstime, uint32_t per);
typedef int (*dspd_pcmcli_timer_getpollfd_t)(dspd_pcmcli_timer_t *tmr, struct pollfd *pfd);
typedef void (*dspd_pcmcli_timer_destroy_t)(dspd_pcmcli_timer_t *tmr);



struct dspd_pcmcli_timer_ops {
  dspd_pcmcli_timer_fire_t      fire;
  dspd_pcmcli_timer_reset_t     reset;
  dspd_pcmcli_timer_get_t       get;
  dspd_pcmcli_timer_set_t       set;
  dspd_pcmcli_timer_getpollfd_t getpollfd;
  dspd_pcmcli_timer_destroy_t   destroy;
};

struct dspd_pcmcli;

struct dspd_pcmcli_bindparams {
  int32_t  playback_stream;
  int32_t  playback_device;
  int32_t  capture_stream;
  int32_t  capture_device;
  struct dspd_aio_ctx *context;
  struct dspd_device_stat *playback_info;
  struct dspd_device_stat *capture_info;
};

enum {
  /*Allocated (should be zeroed memory)*/
  PCMCLI_STATE_ALLOC = 0,
  /*Initialized*/
  PCMCLI_STATE_INIT,
  /* Open */
  PCMCLI_STATE_OPEN,
  /* Setup installed */ 
  PCMCLI_STATE_SETUP,
  /* Ready to start */
  PCMCLI_STATE_PREPARED,
  /* Running */
  PCMCLI_STATE_RUNNING,
  /* Stopped: underrun (playback) or overrun (capture) detected */
  PCMCLI_STATE_XRUN,
  /* Draining: running (playback) */
  PCMCLI_STATE_DRAINING,
  /* Paused */
  PCMCLI_STATE_PAUSED,
  /* Hardware is suspended */
  PCMCLI_STATE_SUSPENDED,
  /* Hardware is disconnected */
  PCMCLI_STATE_DISCONNECTED,
  PCMCLI_STATE_LAST = PCMCLI_STATE_DISCONNECTED

};
int32_t dspd_pcmcli_get_next_wakeup(struct dspd_pcmcli *client, const uint32_t *avail, int32_t *streams, dspd_time_t *next);
int32_t dspd_pcmcli_wait(struct dspd_pcmcli *client, int32_t streams, uint32_t avail, bool async);
void dspd_pcmcli_restore_wait(struct dspd_pcmcli *client);

int32_t dspd_pcmcli_pollfd_revents(struct dspd_pcmcli *client, const struct pollfd *pfds, size_t nfds, int32_t *revents);
int32_t dspd_pcmcli_get_pollfd(struct dspd_pcmcli *client, struct pollfd *pfds, size_t nfds, int32_t events);
int32_t dspd_pcmcli_pollfd_count(struct dspd_pcmcli *client);

ssize_t dspd_pcmcli_write_frames(struct dspd_pcmcli *client, const void *data, size_t frames);
ssize_t dspd_pcmcli_write_bytes(struct dspd_pcmcli *client, const void *data, size_t bytes);
ssize_t dspd_pcmcli_read_frames(struct dspd_pcmcli *client, void *data, size_t frames);
ssize_t dspd_pcmcli_read_bytes(struct dspd_pcmcli *client, void *data, size_t bytes);


int32_t dspd_pcmcli_get_status(struct dspd_pcmcli *client, int32_t stream, bool hwsync, struct dspd_pcmcli_status *status);
int32_t dspd_pcmcli_avail(struct dspd_pcmcli *client, int32_t stream, uint64_t *hw_ptr, uint64_t *appl_ptr);

//Nonblocking (can be changed later)
#define DSPD_PCMCLI_NONBLOCK 1
//Don't open timer fd.  Blocking will not work in this mode.
#define DSPD_PCMCLI_NOTIMER  2
//Emulate a ring buffer with delay instead of reporting client buffer directly.
#define DSPD_PCMCLI_CONSTANT_LATENCY 4

//Allow partial frame reads+writes
#define DSPD_PCMCLI_BYTE_MODE 8
int32_t dspd_pcmcli_init(struct dspd_pcmcli *client, int32_t streams, int32_t flags);
size_t dspd_pcmcli_sizeof(void);
void dspd_pcmcli_destroy(struct dspd_pcmcli *client);
int32_t dspd_pcmcli_new(struct dspd_pcmcli **client, int32_t streams, int32_t flags);
void dspd_pcmcli_delete(struct dspd_pcmcli *client);
int32_t dspd_pcmcli_set_constant_latency(struct dspd_pcmcli *client, bool enable);
int32_t dspd_pcmcli_set_nonblocking(struct dspd_pcmcli *client, bool nonblocking);
int32_t dspd_pcmcli_set_no_xrun(struct dspd_pcmcli *client, bool no_xrun);

int32_t dspd_pcmcli_set_poll_threshold(struct dspd_pcmcli *client, size_t frames);

#define DSPD_PCMCLI_BIND_AUTOCLOSE 1
#define DSPD_PCMCLI_BIND_CONNECTED 2
int32_t dspd_pcmcli_bind(struct dspd_pcmcli *client, const struct dspd_pcmcli_bindparams *params, int32_t flags, dspd_aio_ccb_t complete, void *data);
void dspd_pcmcli_unbind(struct dspd_pcmcli *client);

int32_t dspd_pcmcli_set_hwparams(struct dspd_pcmcli *client, 
				 const struct dspd_cli_params *hwparams, 
				 const struct dspd_client_shm *playback_shm,
				 const struct dspd_client_shm *capture_shm,
				 bool sync);
int32_t dspd_pcmcli_get_hwparams(struct dspd_pcmcli *client, struct dspd_cli_params *hwparams);
int32_t dspd_pcmcli_set_swparams(struct dspd_pcmcli *client, const struct dspd_rclient_swparams *swparams, bool sync, dspd_aio_ccb_t complete, void *data);
int32_t dspd_pcmcli_get_swparams(struct dspd_pcmcli *client, struct dspd_rclient_swparams *swparams);
int32_t dspd_pcmcli_set_channelmap(struct dspd_pcmcli *client, const struct dspd_pcm_chmap *chmap, bool sync, dspd_aio_ccb_t complete, void *data);

int32_t dspd_pcmcli_get_channelmap(struct dspd_pcmcli *client, 
				   int32_t stream, 
				   struct dspd_pcm_chmap *chmap,
				   size_t chmap_bufsize,
				   dspd_aio_ccb_t complete,
				   void *data);

int32_t dspd_pcmcli_ctl(struct dspd_pcmcli *client,
			  int32_t stream,
			  int32_t req,
			  const void *inbuf,
			  size_t inbufsize,
			  void *outbuf,
			  size_t outbufsize,
			  size_t *bytes_returned);

int32_t dspd_pcmcli_select_byname_cb(void *arg, int32_t streams, int32_t index, const struct dspd_device_stat *info, struct dspd_pcmcli *client);
int32_t dspd_pcmcli_open_device(struct dspd_pcmcli *client, 
				  const char *server,
				int32_t (*select_device)(void *arg, int32_t streams, int32_t index, const struct dspd_device_stat *info, struct dspd_pcmcli *client),
				  void *arg);
int32_t dspd_pcmcli_prepare(struct dspd_pcmcli *client, dspd_aio_ccb_t complete, void *data);

int32_t dspd_pcmcli_start(struct dspd_pcmcli *client, int32_t sbits, dspd_aio_ccb_t complete, void *data);
int32_t dspd_pcmcli_stop(struct dspd_pcmcli *client, int32_t sbits, dspd_aio_ccb_t complete, void *data);
int32_t dspd_pcmcli_settrigger(struct dspd_pcmcli *client, int32_t sbits, dspd_aio_ccb_t complete, void *data);
int32_t dspd_pcmcli_pause(struct dspd_pcmcli *client, bool paused, dspd_aio_ccb_t complete, void *data);

int32_t dspd_pcmcli_get_client_index(const struct dspd_pcmcli *client, int32_t sbit);
int32_t dspd_pcmcli_get_device_index(const struct dspd_pcmcli *client, int32_t sbit);
int32_t dspd_pcmcli_process_io(struct dspd_pcmcli *client, int32_t revents, int32_t timeout);
int32_t dspd_pcmcli_cancel_io(struct dspd_pcmcli *client);


int32_t dspd_pcmcli_rewind(struct dspd_pcmcli *client, int32_t sbits, uint64_t *frames);
int32_t dspd_pcmcli_forward(struct dspd_pcmcli *client, int32_t sbits, uint64_t *frames);
int32_t dspd_pcmcli_set_appl_pointer(struct dspd_pcmcli *client, int32_t sbits, bool relative, uint64_t frames);
int32_t dspd_pcmcli_delay(struct dspd_pcmcli *client, int32_t sbit, int64_t *frames);
int32_t dspd_pcmcli_drain(struct dspd_pcmcli *client);
ssize_t dspd_pcmcli_frames_to_bytes(struct dspd_pcmcli *client, 
				    const struct dspd_cli_params *params,
				    int32_t stream, 
				    size_t frames);
ssize_t dspd_pcmcli_bytes_to_frames(struct dspd_pcmcli *client, 
				    const struct dspd_cli_params *params,
				    int32_t stream, 
				    size_t bytes);
int32_t dspd_pcmcli_hw_params_default(struct dspd_pcmcli *client, 
				      struct dspd_cli_params *params);
int32_t dspd_pcmcli_hw_params_set_channels(struct dspd_pcmcli *client, 
					   struct dspd_cli_params *params,
					   int32_t stream, 
					   int32_t channels);
int32_t dspd_pcmcli_hw_params_set_rate(struct dspd_pcmcli *client, 
				       struct dspd_cli_params *params,
				       int32_t rate);
int32_t dspd_pcmcli_hw_params_set_bufsize(struct dspd_pcmcli *client, 
					  struct dspd_cli_params *params,
					  int32_t size);
int32_t dspd_pcmcli_hw_params_set_fragsize(struct dspd_pcmcli *client, 
					   struct dspd_cli_params *params,
					   int32_t size);
int32_t dspd_pcmcli_hw_params_set_latency(struct dspd_pcmcli *client, 
					  struct dspd_cli_params *params,
					  int32_t frames);
int32_t dspd_pcmcli_hw_params_set_format(struct dspd_pcmcli *cli,
					 struct dspd_cli_params *params,
					 int32_t format);

int32_t dspd_pcmcli_hw_params_get_channels(struct dspd_pcmcli *cli, struct dspd_cli_params *params, int32_t stream);

const struct dspd_device_stat *dspd_pcmcli_device_info(struct dspd_pcmcli *client, int32_t sbit);


int32_t dspd_pcmcli_set_info(struct dspd_pcmcli *client, 
			     const struct dspd_cli_info *info,
			     dspd_aio_ccb_t complete,
			     void *arg);

void dspd_pcmcli_set_timer_callbacks(struct dspd_pcmcli *client, const struct dspd_pcmcli_timer_ops *ops, void *arg);

struct dspd_aio_ctx *dspd_pcmcli_get_aio_ctx(struct dspd_pcmcli *client);

int32_t dspd_pcmcli_get_stream_index(struct dspd_pcmcli *cli, int32_t sbit);

int32_t dspd_pcmcli_get_state(struct dspd_pcmcli *cli);

void dspd_pcmcli_set_pcm_io_callbacks(struct dspd_pcmcli *cli,
				      dspd_pcmcli_io_cb_t playback_cb,
				      dspd_pcmcli_io_cb_t capture_cb,
				      void *arg);
				

#endif /* ifdef _DSPD_PCMCLI_H_ */
