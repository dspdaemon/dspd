#ifndef _DSPD_DEVICE_H_
#define _DSPD_DEVICE_H_
#include "thread.h"
#include <poll.h>
#include <setjmp.h>
struct dspd_pcmcli_status;
struct dspd_pcmcli_ops;

#include "atomic.h"
#define DSPD_MAX_OBJECTS 256
#define DSPD_MASK_SIZE ((DSPD_MAX_OBJECTS/8)+1)
struct dspd_pcm_status {
  uint64_t appl_ptr;
  uint64_t hw_ptr;
  uint64_t tstamp;
  uint32_t fill;
  uint32_t space;
  int32_t  delay;
  int32_t  error;
};

struct dspd_drv_params {
  char     *desc;
  char     *name;
  char     *bus;
  char     *addr;
  int32_t   format;
  uint32_t  channels;
  uint32_t  rate;
  uint32_t  fragsize;
  uint32_t  bufsize;
  int32_t   stream;

  uint32_t  min_latency;
  uint32_t  max_latency;
  uint32_t  min_dma;
};

struct dspd_pcmdrv_ops {
  /*
    Map a buffer area.
    handle:  Driver data
    buf:     Buffer section.  Must be readable and writeable.  Must be filled with silence
             if appropriate (section is uninitialized and not being rewritten).
    offset:  Offset to start of area
    frames:  Number requested on input and actual number on output.
             Will be <= to number requested.
   */
  int (*mmap_begin)(void *handle,
		    void **buf,
		    uintptr_t *offset,
		    uintptr_t *frames);


			 
   /*
    Send data to the device.  Unlike ALSA, this must be called or the device
    may (probably) not see the data.
   */
  intptr_t (*mmap_commit)(void *handle,
			  uintptr_t offset,
			  uintptr_t frames);
  /*
    Recover from errors.  Should only return an error if a fatal error occurs.
  */
  int (*recover)(void *handle);

  /*
    Start playing or recording.
   */
  int (*start)(void *handle);
  
  /*
    Stop playing and discard pending samples.
   */
  int (*drop)(void *handle);

  /*
    Prepare device to begin io operations.
   */
  int (*prepare)(void *handle);

  
  /*
    Get device status.  Includes timestamp, buffer levels, and delays.
   */
  int (*status)(void *handle, const struct dspd_pcm_status **status);


  /*
    Rewind the application pointer.  Returns amount actually changed or an error.
   */
  intptr_t (*rewind)(void *handle, uintptr_t frames);

  /*
    Move the application pointer forward.  Returns amount actually changed or an error.
  */
  intptr_t (*forward)(void *handle, uintptr_t frames);

  /*
    Get amount that is possible to safely rewind.
  */
  intptr_t (*rewindable)(void *handle);

  /*Set volume*/
  void (*set_volume)(void *handle, double volume);

  /*
    Set latency hint.  Return value is actual buffer size.
    It is up to the driver as to whether this takes effect
    immediately or even uses a hardware function.  The
    recommended action is to change the stored volume and
    then apply it during the next io operation and optionally
    rewind down to as low as 10ms to apply the change.
   */
  uintptr_t (*set_latency)(void *handle, uintptr_t buffer_size, uintptr_t latency);

  int32_t (*poll_descriptors_count)(void *handle);
  int32_t (*poll_descriptors)(void *handle, 
			      struct pollfd *pfds,
			      uint32_t space);
  int32_t (*poll_revents)(void *handle,
			  struct pollfd *pfds,
			  uint32_t nfds,
			  uint16_t *revents);

  int32_t (*get_params)(void *handle, struct dspd_drv_params *params);

  void (*destructor)(void *handle);

  /*
    The idea is that a full duplex device will be created by creating two
    half duplex devices and joining them.  This callback would be able to
    return its own handle or the other half depending on the value passed in the
    stream argument.
   */
  void *(*get_handle)(void *fullduplex_handle, int stream);

  /*
    Link two handles for full duplex.
   */
  void *(*fdlink)(void *handle, void *handle2);

  /*
    This callback should return -ENOSYS
    without doing anything else if a feature is not
    supported.  Anything else will be considered a result.
    
  */
  int32_t (*ioctl)(struct dspd_rctx *rctx,
		   uint32_t          req,
		   const void       *inbuf,
		   size_t            inbufsize,
		   void             *outbuf,
		   size_t            outbufsize);

  int32_t (*get_error)(void *handle);

  void (*set_stream_index)(void *handle, int32_t stream);


  
  int32_t (*get_chmap)(void *handle, struct dspd_chmap *map);
  int32_t (*translate_chmap)(void *handle, 
			     const struct dspd_chmap *map_in, 
			     struct dspd_chmap *map_out);
  int32_t (*create_chmap)(void *handle,
			  int32_t channels,
			  struct dspd_chmap *map_out);
  
  /*
    Perform fast pointer adjustments.  This function should be
    called before manipulating buffer contents and after manipulating
    buffer contents with an adjustment in the other direction.  This
    needs to be done even if the handle is in an error state.
    
    It is acceptible for this function to just do standard rewind
    and forward.  The idea is to adjust the status buffer contents
    if a client needs to start in the middle of the current buffer
    section.

   */
  intptr_t (*adjust_pointer)(void *handle, intptr_t frames);
  
};



struct dspd_dev_reg {
  //5 bits latency (shift bit left), 9 bits playback_idx+1, 9 bits capture_idx+1
  //The indexes are just like for select().  That means 
  //it is possible to do {for ( i = 0; i < idx; i++ ){} } and the register value
  //becomes nonzero when streams are present.
  volatile AO_t              config;
  //These each need updated atomically, but not together.
  union dspd_atomic_float32  playback_volume;
  union dspd_atomic_float32  capture_volume;
  //Mask of client numbers.  Bit 0 is reserved.
  volatile uint8_t           client_mask[(DSPD_MAX_OBJECTS*2)/8];
};

struct dspd_io_cycle {
  void      *addr;
  uintptr_t  len;
  uintptr_t  offset;
  //This is how many times the device has been prepared and recovered.
  //It can be used to mark clients so a client is new to the current
  //instance if it does not have the same start count.
  //This counter is always nonzero when a client is called.
  uint64_t   start_count;
};

struct dspd_pcm_status;
struct dspd_pcmdev_stream {
  struct dspd_drv_params           params;
  const struct dspd_pcmdrv_ops    *ops;
  const struct dspd_pcm_status    *status;
  void                            *handle;
  struct dspd_io_cycle             cycle;
  bool                             started;
  size_t                           latency;
  size_t                           streams;
  float                            volume;
  uint8_t                          changes;
  size_t                           stop_threshold;
  size_t                           stop_count;
  bool                             running;
  struct pollfd                   *pfds;
  uint32_t                         nfds;
  uint32_t                         fds_set;
  bool                             latency_changed;
  uint64_t                         sample_time;
  struct dspd_intrp                intrp;
  uintptr_t                        last_hw;
  bool                             check_status;
  dspd_time_t                      next_wakeup;
  bool                             glitch;
  size_t                           glitch_threshold;
  size_t                           requested_latency;
  dspd_time_t                      early_cycle;
};

#define DSPD_IOC_LOCK    1
#define DSPD_IOC_RELEASE 2
#define DSPD_IOC_BOTH    3

struct dspd_pcm_device {
  struct dspd_pcmdev_stream        playback;
  struct dspd_pcmdev_stream        capture;
  struct dspd_scheduler  *sched;
  dspd_mutex_t            reg_lock;
  struct dspd_dev_reg     reg;
  uint32_t                current_config;
  void (*process_data)(void *arg, struct dspd_pcm_device *device);

  uint8_t lock_mask[DSPD_MAX_OBJECTS/8];

  //This basically indicates that an IRQ occurred.  It is up to the client
  //to decide what to do with it.
  void (*process_client)(struct dspd_pcm_device *device,
			 uint32_t client, //client number
			 uint32_t ioc,    //io cycle type (lock,release,both)
			 uint32_t key);    //Identifier for this device


  void  *arg;
  uintptr_t current_latency;
  bool trigger;
  dspd_thread_t iothread;
  struct dspd_slist *list;
 
#define DSPD_CBIT_PRESENT (1<<2)
#define DSPD_CBIT_LATENCY (0xF8)

  //8 bits per client: 3-7=latency, 2=present, 1=capture, 0=playback
  uint8_t client_configs[DSPD_MAX_OBJECTS];

  volatile intptr_t current_client;
  int32_t  key;

  sigjmp_buf sbh_env, sbh_except;
  volatile int current_exception, exc_client;
  volatile AO_t error;

  volatile AO_t irq_count, ack_count;

};




/*
  The realtime client<>server interface should use these callbacks.
  The other side of it should use iorp for most stuff.
*/
struct dspd_pcmdev_ops {

  /*
    Connect a client object to a server objects.  If streams is 0 and client == -1, then
    it will increase the reference count without registering a client.
    A device may support a limited number of clients and reject a client with -ECONNREFUSED.
    It should also refuse if the device is dead.
  */
  int32_t (*connect)(void *dev, int32_t client);
  /*Does not really fail.  Just like close().  It can return an error, but
    after returning the client is not connected to this device.*/
  int32_t (*disconnect)(void *dev, int32_t client);

  /*
    Get and set latency for this client.  The server will figure out what the
    real latency is and return it.
   */
  int32_t (*set_latency)(void *dev, uint32_t client, uint32_t latency);
  int32_t (*get_latency)(void *dev, uint32_t client);

  /*
    Trigger a client.  The server may already be running.  If not then it will
    wake up.  Can return -EINVAL or -ENODEV.
   */
  int32_t (*trigger)(void *dev, uint32_t client, uint32_t streams);

  /*
    Get device parameters (rate, channels, etc).  May fail if the device is dead.
   */
  int32_t (*getparams)(void *dev, int32_t stream, struct dspd_drv_params *params);

  //Unused for now.
  intptr_t (*control)(void         *ctx,
		      const void   *buf_in,
		      size_t        len_in,
		      void         *buf_out,
		      size_t        len_out);
  

};

struct dspd_cli_params {
  int32_t format;
  int32_t channels; //0 if disabled.

  int32_t rate;
  int32_t bufsize;

  int32_t fragsize;
  int32_t stream;

  int32_t latency;
#define DSPD_CLI_FLAG_SHM 1
  int32_t flags;

  //Server only.  Latencies in frames
  uint32_t min_latency;
  uint32_t max_latency;

  int32_t src_quality;

  /*
    Buffer and fragment sizes are in bytes.  If latency is 0
    then fill it in, otherwise it is also in bytes.
  */
#define DSPD_CLI_XFLAG_BYTES      1
  /*
    Convert sample rates and channel maps.
  */
#define DSPD_CLI_XFLAG_COOKEDMODE 2

  /*
    Size are in nanoseconds.
  */
#define DSPD_CLI_XFLAG_NANOSECONDS 4
  int32_t xflags; //translation flags

  /*
    Use exact fragment sizes
   */
#define DSPD_CLI_XFLAG_EXACTSIZE   8

};
struct dspd_cli_stat {
  char    dest[128];
  int32_t streams;
  int32_t flags;
  int32_t error;
  int32_t pid;
  uint64_t slot_id;
  struct dspd_cli_params playback;
  struct dspd_cli_params capture;
};
struct dspd_device_stat {
  char name[64]; //Driver specific device name (hw:0, etc)
  char bus[64];  //Hardware bus
  char addr[64]; //Hardware address
  char desc[128];
  int32_t streams;
  int32_t flags;
  int32_t error;
  int32_t reserved;
  uint64_t slot_id;
  struct dspd_cli_params playback;
  struct dspd_cli_params capture;
};



struct dspd_cli_params;

struct dspd_pcmcli_io_ops {
  int (*mmap_begin)(void *handle,
		    float **buf,
		    uintptr_t *offset,
		    uintptr_t *frames);
  
  intptr_t (*mmap_commit)(void *handle,
			  uintptr_t offset,
			  uintptr_t frames);
  int (*recover)(void *handle);
  int (*start)(void *handle);
  int (*drop)(void *handle);
  int (*prepare)(void *handle);
  int (*status)(void *handle, struct dspd_pcmcli_status *status);
  intptr_t (*rewind)(void *handle, uintptr_t frames);
  intptr_t (*forward)(void *handle, uintptr_t frames);
  intptr_t (*rewindable)(void *handle);
  void (*set_volume)(void *handle, double volume);

  double (*get_volume)(void *handle);
  int32_t (*get_params)(void *handle, struct dspd_cli_params *params);
  int32_t (*set_params)(void *handle, const struct dspd_cli_params *params);
};









int32_t dspd_dev_get_slot(void *dev);

















void dspd_pcm_device_delete(struct dspd_pcm_device *dev);
struct dspd_pcmdev_params {
  int32_t stream;
  const struct dspd_pcmdrv_ops *ops[2];
  void **driver_handles;
  void *arg;
};
int32_t dspd_pcm_device_new(void **dev,
			    const struct dspd_pcmdev_params *params,
			    struct dspd_slist *list);


void *dspd_pcm_device_get_driver_handle(struct dspd_pcm_device *dev, uint32_t stream);
#endif