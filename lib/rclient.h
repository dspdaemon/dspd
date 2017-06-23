#ifndef _DSPD_RCLIENT_H_
#define _DSPD_RCLIENT_H_

struct dspd_rclient_bindparams {
  int32_t  device;
  int32_t  client;
  void    *conn;
};

struct dspd_rclient_swparams {
  uint32_t avail_min;
  uint32_t start_threshold;
  uint32_t stop_threshold;
};

struct dspd_rclient_hwparams {
  const struct dspd_cli_params *playback_params;
  const struct dspd_chmap      *playback_chmap;
  const struct dspd_cli_params *capture_params;
  const struct dspd_chmap      *capture_chmap;
  void    *context;
  int32_t  stream;
  int32_t  device;
};

struct dspd_rclient {
  struct dspd_client_stream playback;
  struct dspd_client_shm    playback_shm;
  struct dspd_client_stream capture;
  struct dspd_client_shm    capture_shm;
  dspd_tofloat32_t   playback_conv;
  size_t             playback_framesize;
  dspd_fromfloat32_t capture_conv;
  size_t             capture_framesize;
  int                eventfd;
  bool               eventfd_set;
  struct dspd_timer  timer;
  
  struct dspd_rclient_swparams swparams;

  struct dspd_rclient_bindparams bparams;

  uint32_t           trigger;
  bool               playback_xfer;
  uint32_t           forced_events;
  uint32_t           last_events;
  
  uint32_t           stream_poll_events;

  bool               autoclose; //Automatically close connection
  struct dspd_device_stat devinfo;
  struct dspd_intrp playback_intrp, capture_intrp;

  dspd_time_t playback_next_wakeup, capture_next_wakeup;
  uint32_t wakeup_streams;

  uint32_t streams;
  bool init;
  int32_t mq_fd;
  struct dspd_mq_notification notification;
  size_t mq_msgsize;
};
void dspd_rclient_detach(struct dspd_rclient *client, int32_t stream);
int32_t dspd_rclient_init(struct dspd_rclient *client, int32_t stream);
void dspd_rclient_destroy(struct dspd_rclient *client);



int32_t dspd_rclient_attach(struct dspd_rclient *client,
			    const struct dspd_client_shm *cshm,
			    const struct dspd_cli_params *params,
			    const struct dspd_rclient_bindparams *bparams);

int32_t dspd_rclient_write_rewind(struct dspd_rclient *client, uint32_t len);
int32_t dspd_rclient_set_write_ptr(struct dspd_rclient *client, uintptr_t ptr);
int32_t dspd_rclient_write(struct dspd_rclient *client, 
			   const void          *buf,
			   uint32_t             length);
int32_t dspd_rclient_read(struct dspd_rclient *client, 
			  void                *buf,
			  uint32_t             length);
int32_t dspd_rclient_set_read_ptr(struct dspd_rclient *client, uintptr_t ptr);


int32_t dspd_rclient_get_next_wakeup(struct dspd_rclient *client, 
				     int32_t sbits, 
				     dspd_time_t *waketime);
int32_t dspd_rclient_get_next_wakeup_avail(struct dspd_rclient *client, 
					   int32_t sbits, 
					   uint32_t avail_min,
					   dspd_time_t *waketime);
int32_t dspd_rclient_ctl(struct dspd_rclient *client,
			 uint32_t    req,
			 const void *inbuf,
			 size_t      inbufsize,
			 void       *outbuf,
			 size_t      outbufsize,
			 size_t     *br);
int32_t dspd_update_timer(struct dspd_rclient *client, int streams);
int32_t dspd_force_poll_events(struct dspd_rclient *client, uint32_t events);
int32_t dspd_rclient_poll_revents(struct dspd_rclient *client, struct pollfd *pfd, int32_t count);

int32_t dspd_poll_ack(struct dspd_rclient *client);
int32_t dspd_rclient_enable_pollfd(struct dspd_rclient *client, bool enable);
int32_t dspd_rclient_pollfd_count(struct dspd_rclient *client);
int32_t dspd_rclient_pollfd(struct dspd_rclient *client, uint32_t count, struct pollfd *pfd);
int32_t dspd_rclient_wait(struct dspd_rclient *client, int32_t sbits);
int32_t dspd_rclient_get_next_wakeup(struct dspd_rclient *client, 
				     int32_t sbits, 
				     dspd_time_t *waketime);
int32_t dspd_rclient_status(struct dspd_rclient *client, 
			    int32_t stream, 
			    struct dspd_pcmcli_status *status);
int32_t dspd_rclient_get_error(struct dspd_rclient *client, int32_t stream);
int32_t dspd_rclient_get_hw_ptr(struct dspd_rclient *client, int32_t stream, uint32_t *ptr);

void dspd_rclient_poll_notify(struct dspd_rclient *client, uint32_t sbits);
void dspd_rclient_poll_clear(struct dspd_rclient *client, uint32_t sbits);
int32_t dspd_rclient_avail(struct dspd_rclient *client, int32_t stream);
int32_t dspd_rclient_drain(struct dspd_rclient *client);

int32_t dspd_rclient_reset(struct dspd_rclient *client, int32_t stream);

struct _dspd_rclient_ctlparams {
  struct dspd_rclient *client;
  uint32_t    request;
  const void *inbuf;
  size_t      inbufsize;
  void       *outbuf;
  size_t      outbufsize;
  size_t     *bytes_returned;
};
static inline int32_t _dspd_rclient_npctl_fcn(struct _dspd_rclient_ctlparams params)
{
  return dspd_rclient_ctl(params.client,
			  params.request,
			  params.inbuf,
			  params.inbufsize,
			  params.outbuf,
			  params.outbufsize,
			  params.bytes_returned);
}

#define dspd_rclient_npctl(...) _dspd_rclient_npctl_fcn((struct _dspd_rclient_ctlparams)__VA_ARGS__)


int32_t dspd_rclient_get_streams(struct dspd_rclient *client);



int32_t dspd_rclient_connect(struct dspd_rclient *client, 
			     const struct dspd_cli_params *params, //required, may be full duplex
			     const struct dspd_chmap *playback_map, //optional
			     const struct dspd_chmap *capture_map, //optional
			     void *context, //connection or dspd_dctx
			     int32_t stream, //client stream
			     int32_t device);

int32_t dspd_rclient_disconnect(struct dspd_rclient *client, bool reserve);
int32_t dspd_rclient_avail_cl(struct dspd_rclient *client, uint32_t *avail_min, int32_t *delay);
int32_t dspd_rclient_set_avail_min(struct dspd_rclient *client, uint32_t avail_min);

int dspd_rclient_update_pollfd(struct dspd_rclient *client, uint32_t sbits, bool constant_latency);

//Bind client, server, device, and stream.  This supposed to
//allow passing the rclient to represent all of these things before
//it is attached.
int32_t dspd_rclient_bind(struct dspd_rclient *client,
			  struct dspd_rclient_bindparams *bparams);
int32_t dspd_rclient_fast_status(struct dspd_rclient *client, 
				 int32_t stream, 
				 struct dspd_pcmcli_status *status);

int32_t dspd_rclient_new(struct dspd_rclient **client, int32_t streams);
void dspd_rclient_delete(struct dspd_rclient *client);
int32_t dspd_rclient_open_dev(struct dspd_rclient *client, 
			      const char *name, 
			      int32_t stream,
			      struct dspd_device_stat *info);


int dspd_rclient_open(void *context,
		      const char *addr, 
		      const char *name,
		      int stream,
		      struct dspd_rclient **client);
const struct dspd_device_stat *dspd_rclient_devinfo(const struct dspd_rclient *client);

bool dspd_rclient_test_xrun(struct dspd_rclient *client, int sbits);


int32_t dspd_rclient_set_hw_params(struct dspd_rclient *cli, 
				   const struct dspd_rclient_hwparams *hwp);

const struct dspd_cli_params *dspd_rclient_get_hw_params(const struct dspd_rclient *client, int32_t sbit);
int32_t dspd_rclient_set_excl(struct dspd_rclient *client, int32_t flags);
#endif
