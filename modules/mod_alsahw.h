#ifndef _ALSADRIVER_H_
#define _ALSADRIVER_H_
#include <stdint.h>
#include <alsa/asoundlib.h>
#include <poll.h>

int32_t alsahw_pcm_mmap_begin(void *handle,
			       void **buf,
			       uintptr_t *offset,
			       uintptr_t *frames);
intptr_t alsahw_pcm_mmap_commit(void *handle,
				 uintptr_t offset,
				 uintptr_t frames);
int32_t alsahw_pcm_recover(void *handle);
int32_t alsahw_pcm_start(void *handle);
int32_t alsahw_pcm_drop(void *handle);
int32_t alsahw_pcm_prepare(void *handle);
int32_t alsahw_pcm_status(void *handle, const struct dspd_pcm_status **status, bool hwsync);
intptr_t alsahw_pcm_rewind(void *handle, uintptr_t frames);
intptr_t alsahw_pcm_forward(void *handle, uintptr_t frames);
intptr_t alsahw_pcm_rewindable(void *handle);
void alsahw_set_volume(void *handle, double volume);
int32_t alsahw_pcm_write_begin(void *handle,
				void **buf,
				uintptr_t *offset,
				uintptr_t *frames);
intptr_t alsahw_pcm_write_commit(void *handle,
				  uintptr_t offset,
				  uintptr_t frames);







struct alsahw_mix_elem {
  snd_mixer_selem_id_t *sid;
  snd_mixer_elem_t     *elem;
  int32_t               index;
  uint64_t              tstamp;
  uint64_t              flags;
  uint64_t              update_count;
  uint32_t              pchan_mask, cchan_mask;
  uint32_t              pchan_count, cchan_count;
};

struct alsahw_handle {
  snd_pcm_t                *handle;
  union {
    dspd_fromfloat64wv_t    fromdouble;
    dspd_tofloat32wv_t      tofloat;
  } convert;
  union { 
    double                   *addr64;
    float                    *addr32;
    void                     *addr;
  } buffer;
  struct dspd_pcm_status status;
  uintptr_t                 erase_ptr;
  int32_t                   err;
  int32_t                   sample_time;
  uintptr_t                 min_dma;
  uintptr_t                 min_dma_bytes;
  uintptr_t                 frame_size;
  uintptr_t                 channels;
  double                    volume;
  snd_pcm_status_t         *alsa_status;
  void                     *hw_addr;
  snd_pcm_sw_params_t      *swparams;
  snd_pcm_hw_params_t      *hwparams;
  struct dspd_drv_params    params;

  /*
    Can be up to buffer_size.  Beyond that, there is no need to
    increment because there is no way to get older data than one buffer length.
  */
  uintptr_t                 samples_read;
  uint64_t                  saved_appl;
  bool                      is_rewound;
  
  int                       stream;
 
  uintptr_t                 vbufsize; //virtual buffer size is <= device buffer size
  uintptr_t                 hlatency;  //latency hint for clock interpolation
  int                       got_tstamp;
  uintptr_t                 xfer;
  uintptr_t                 started;

  struct dspd_pcm_chmap    *channel_map;

  struct alsahw_mix_elem   *elements;
  size_t                     elements_count, max_elements;
  snd_mixer_t               *mixer;
  dspd_mutex_t               mixer_lock;
  uint64_t                   mixer_update_count;
  uint64_t                   mixer_tstamp;

  int32_t                    stream_index;

  

  void *other_handle;
  uint64_t hotplug_event_id;
 
  /*
    Interpolation is not perfectly accurate so sometimes the server thread
    will need to do more expensive position register reads instead of
    interpolating.
  */
  /*virtual buffer size interpolation threshold*/
  uintptr_t i_vbuf_threshold;
  /*hardware buffer size interpolation threshold*/
  uintptr_t i_hbuf_threshold;
  

  struct dspd_scheduler *sched;
  volatile int32_t       recover_result;
  volatile int32_t       prepare_result;

};

struct alsahw_ctldata;

//See SND_CTL_EVENT_MASK_ in <alsa/control.h> for how to do callbacks
#define ALSAHW_CTL_EVENT_MASK_CHANGED (1<<31)
typedef void (*alsahw_mixer_callback)(snd_mixer_t *ctl,
				      struct alsahw_ctldata *data,
				      uint32_t mask,
				      snd_mixer_elem_t *elem,
				      void *arg);
struct alsahw_cb {
  struct alsahw_cb *next;
  dspd_mixer_callback callback;
  void *arg;
};

struct alsahw_ctldata {
  struct alsahw_ctldata *next;
  struct pollfd         *pfds;
  size_t                 nfds;
  snd_mixer_t           *mixer;
  bool                   ready;
  struct alsahw_cb      *callbacks;
  pthread_mutex_t       *lock;
  alsahw_mixer_callback  alsa_cb;
  void                  *arg;
};

struct alsahw_notifier {
  dspd_mutex_t           poll_lock;
  dspd_mutex_t           notify_lock;
  struct alsahw_ctldata *ctl_list;
  struct pollfd         *pfds;
  size_t                 nfds;
  int                    efd;
  dspd_thread_t          thread;
  //Memory pool so registrations are less likely to block
  struct alsahw_cb       callbacks[DSPD_MAX_OBJECTS];
};


int alsahw_init_notifier(struct alsahw_notifier **notifier);
int alsahw_register_mixer_callback(struct alsahw_notifier *n,
				   snd_mixer_t *mixer,
				   dspd_mixer_callback cb,
				   void *arg);
int alsahw_unregister_mixer_callback(struct alsahw_notifier *n,
				     snd_mixer_t *mixer,
				     dspd_mixer_callback cb,
				     void *arg);
int alsahw_register_mixer(struct alsahw_notifier *notifier,
			  snd_mixer_t *mixer,
			  pthread_mutex_t *lock,
			  alsahw_mixer_callback cb,
			  void *arg);
int alsahw_unregister_mixer(struct alsahw_notifier *notifier,
			    snd_mixer_t *mixer);

void alsahw_mixer_event_notify(struct alsahw_ctldata *data,
			       int32_t card,
			       int32_t elem,
			       int32_t mask);

#endif
