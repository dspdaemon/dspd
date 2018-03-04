#ifndef _DSPD_PCMCS_H_
#define _DSPD_PCMCS_H_


enum dspd_pcmcli_stream_states {
  PCMCS_STATE_ALLOC = 0,
  PCMCS_STATE_INIT,
  PCMCS_STATE_BOUND,
  PCMCS_STATE_PAUSED,
  PCMCS_STATE_PREPARED,
  PCMCS_STATE_RUNNING,
  PCMCS_STATE_ERROR,
};





struct dspd_pcmcli_stream {
  int32_t                         state;
  int32_t                         trigger;
  int32_t                         stream_flags;
  int32_t                         error;

  bool                            no_xrun;
  uint32_t                        xrun_threshold;


  
  struct dspd_client_shm   shm;
  struct dspd_cli_params   params;
  struct dspd_fifo_header  fifo;
  struct dspd_mbx_header   mbx;
  dspd_tofloat32_t         playback_conv;
  dspd_fromfloat32_t       capture_conv;
  size_t                   framesize;
  uint32_t                 sample_time;
  uint64_t                 appl_ptr;
  uint64_t                 hw_ptr;
  uint32_t                 last_hw_ptr;
  struct dspd_pcm_status    status;
  bool                      got_status;

  dspd_time_t               trigger_tstamp;
  bool                      got_tstamp;
  bool                      constant_latency;
  uint64_t                  hw_pause_ptr;
  struct dspd_intrp         intrp;
  uint64_t                  hw_iptr;
  dspd_time_t               next_wakeup;
  size_t                    write_size;
};

int32_t dspd_pcmcli_stream_init(struct dspd_pcmcli_stream *stream, int32_t sbit);
void dspd_pcmcli_stream_destroy(struct dspd_pcmcli_stream *stream);
int32_t dspd_pcmcli_stream_new(struct dspd_pcmcli_stream **stream, int32_t sbit);
void dspd_pcmcli_stream_delete(struct dspd_pcmcli_stream *stream);
size_t dspd_pcmcli_stream_sizeof(void);
int32_t dspd_pcmcli_stream_attach(struct dspd_pcmcli_stream *stream,
			     const struct dspd_cli_params *hwparams,
			     const struct dspd_shm_map *map);
void dspd_pcmcli_stream_detach(struct dspd_pcmcli_stream *stream);

int32_t dspd_pcmcli_stream_set_paused(struct dspd_pcmcli_stream *stream, bool paused);
int32_t dspd_pcmcli_stream_set_running(struct dspd_pcmcli_stream *stream, bool running);
int32_t dspd_pcmcli_stream_set_constant_latency(struct dspd_pcmcli_stream *stream, bool enable);

int32_t dspd_pcmcli_stream_reset(struct dspd_pcmcli_stream *stream);
int32_t dspd_pcmcli_stream_set_trigger_tstamp(struct dspd_pcmcli_stream *stream, dspd_time_t tstamp);

int32_t dspd_pcmcli_stream_status(struct dspd_pcmcli_stream *cliemt,
			     struct dspd_pcmcli_status *status,
			     bool hwsync);



ssize_t dspd_pcmcli_stream_write(struct dspd_pcmcli_stream *stream,
			    const void           *data,
			    size_t                len);

ssize_t dspd_pcmcli_stream_read(struct dspd_pcmcli_stream *stream,
			   void                 *data,
			   size_t                len);


int32_t dspd_pcmcli_stream_set_pointer(struct dspd_pcmcli_stream *stream, bool relative, uint64_t ptr);
int32_t dspd_pcmcli_stream_rewind(struct dspd_pcmcli_stream *stream, uint64_t *frames);
int32_t dspd_pcmcli_stream_forward(struct dspd_pcmcli_stream *stream, uint64_t *frames);
int32_t dspd_pcmcli_stream_avail(struct dspd_pcmcli_stream *stream, uint64_t *hwptr, uint64_t *appl_ptr);



int32_t dspd_pcmcli_stream_check_xrun(struct dspd_pcmcli_stream *stream);

int32_t dspd_pcmcli_stream_state(struct dspd_pcmcli_stream *stream);


#define PCMCS_WAKEUP_DELAYED 0
#define PCMCS_WAKEUP_NOW     1
#define PCMCS_WAKEUP_NONE    2

int32_t dspd_pcmcli_stream_get_next_wakeup(struct dspd_pcmcli_stream *stream, const struct dspd_pcmcli_status *status, size_t avail, dspd_time_t *next);
int32_t dspd_pcmcli_stream_cl_avail(struct dspd_pcmcli_stream *stream);

#endif /*_DSPD_PCMCS_H_*/
