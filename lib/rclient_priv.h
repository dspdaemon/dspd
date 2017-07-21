#ifndef _DSPD_RCLIENT_PRIV_H_
#define _DSPD_RCLIENT_PRIV_H_
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
int32_t dspd_rclient_init(struct dspd_rclient *client, int32_t stream);
void dspd_rclient_destroy(struct dspd_rclient *client);
#endif
