#ifndef _DSPD_CLIENT_H_
#define _DSPD_CLIENT_H_

struct dspd_syncgroup;
enum dspd_shm_sections {
  DSPD_CLIENT_SECTION_INVALID,
  DSPD_CLIENT_SECTION_MBX,
  DSPD_CLIENT_SECTION_FIFO,
};

struct dspd_client_trigger_tstamp {
  uint64_t playback_tstamp;
  uint64_t capture_tstamp;
  int32_t  streams;
};

struct dspd_client_trigger_state {
  uint32_t    stream;
  bool        valid;
  dspd_time_t trigger_tstamp;
  dspd_time_t preferred_start;
};



struct dspd_client_stream {
  struct dspd_fifo_header       fifo;
  struct dspd_mbx_header        mbx;
  struct dspd_shm_map           shm;
  struct dspd_cli_params        params;
  struct dspd_pcm_status        status;
  //This is used to determine if a stream is new
  uint64_t                      start_count; 
  //This is used to synchronize with the device position
  uint64_t                      dev_appl_ptr;
  uint64_t                      cli_appl_ptr;
  uint32_t                      sample_time;
  bool                          enabled;
  bool                          ready;
  bool                          prepared;
  uint64_t                      trigger_tstamp;
  union dspd_atomic_float32     volume;
  dspd_time_t                   last_hw_tstamp;
  size_t                        frame_size;
  bool                          started;
  
  
};
struct dspd_pcmcli_status {
  uint64_t appl_ptr;
  uint64_t hw_ptr;
  uint64_t tstamp;
  uint64_t trigger_tstamp;
  uint64_t delay_tstamp;
  uint32_t avail;
  int32_t  delay;
  int32_t  error;
  int32_t  reserved;
};


struct dspd_pcmcli_ops {

  int32_t (*get_playback_status)(void     *dev,
				 void     *client,      
				 uint64_t *pointer,
				 uint64_t *start_count,
				 uint32_t *latency,
				 uintptr_t  frames,
				 const struct dspd_pcm_status *status);
  
  void (*playback_xfer)(void                            *dev,
			void                            *client,
			double                          *buf,
			uintptr_t                        frames,
			uint64_t                         start_count,
			const struct dspd_pcm_status *status);

  int32_t (*get_capture_status)(void *dev,
				void *client,
				uint64_t *pointer,
				uint64_t *start_count,
				uint32_t *latency,
				const struct dspd_pcm_status *status);
  void (*capture_xfer)(void           *dev,
		       void           *client,
		       const float    *buf,
		       uintptr_t       frames,
		       const struct dspd_pcm_status *status);

  void (*error)(void *dev, int32_t index, void *client, int32_t err);
  
  
};

struct dspd_client_src {
  dspd_src_t  src;
  float      *buf;
  size_t      nsamples;
  uint32_t    rate;
  uint32_t    channels;
};



int32_t dspd_client_new(struct dspd_slist *list,
			void **client);
void dspd_client_delete(void *client);
int32_t dspd_client_get_index(void *client);

#endif

