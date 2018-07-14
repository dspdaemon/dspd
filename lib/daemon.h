#ifndef _DSPD_DAEMON_H_
#define _DSPD_DAEMON_H_
#include "wq.h"
#include "vctrl.h"

//pci,usb,etc.
#define DSPD_HOTPLUG_BUSNAME "bus"
//Device name such as hw:0
#define DSPD_HOTPLUG_DEVNAME "name"
//Bus address such as 9-2:1.0 (usb)
#define DSPD_HOTPLUG_ADDRESS  "address"
//Human readable description "HDA ATI SB"
#define DSPD_HOTPLUG_DESC    "description"
//add, remove
#define DSPD_HOTPLUG_EVENT   "event_type"
//usually module name
#define DSPD_HOTPLUG_SENDER  "sender"
//alsa, etc.
#define DSPD_HOTPLUG_DEVTYPE  "device_type"
//snd_hda_intel, etc
#define DSPD_HOTPLUG_KDRIVER "kernel_driver"
//used internally
#define DSPD_HOTPLUG_SLOT    "slot"
//fullduplex,playback,capture
#define DSPD_HOTPLUG_STREAM  "stream"
//Bus specific chip id such as 041e:3040
#define DSPD_HOTPLUG_HWID    "hwid"

#define DSPD_HOTPLUG_MODALIAS "modalias"

#define DSPD_HOTPLUG_EVENT_ID "event_id"

/*
  Glitch correction.

  If a device has an xrun or is likely to have one then do something about it.  The
  idea is that if the server is having glitches then limit the latency to 10-20ms
  but rewind as needed for clients wanting a lower latency.  This uses more CPU
  but it should generally work.
*/
//Disable glitch correction.  Use the minimum latency specified by clients connected
//to a device.
#define DSPD_GHCN_OFF      0
//Always on.  The buffer will be padded with silence or the data of higher latency
//clients and low latency clients will have to rewind.
#define DSPD_GHCN_ON       1
//Turn on and stay on after the first glitch.  This is the default for servers with
//normal priority.
#define DSPD_GHCN_LATCH    2
//Turn on after the first glitch and reset when the device goes idle again.
//Assume that glitches are transient (maybe the user tries a game and then listens
//to music for the rest of the day).  This is the default for any server that can
//get higher than normal priority (nice,rtprio,iso).
#define DSPD_GHCN_AUTO     3



struct dspd_hotplug_cb {
  /*
    Return values are 0-255.  0=unsupported
    The module with the highest score gets to add the device.
   */
  int  (*score)(void *arg, const struct dspd_dict *device);
  /*
    A device was added to the system.  This callback is where a device gets
    added to the daemon if it is supported.  Return value is negative error code or slot number.
  */
  int  (*add)(void *arg, const struct dspd_dict *device);
  /*
    Device is being removed.  Return values: 0=removed, -EBUSY=can't 
    remove right now, -ENODEV=not my device.
  */
  int  (*remove)(void *arg, const struct dspd_dict *device);

  void (*init_device)(void *arg, const struct dspd_dict *device);

};

struct dspd_hotplug_handler {
  const struct dspd_hotplug_cb *callbacks;
  void                   *arg;
 
};

struct dspd_hotplug_devname;
struct dspd_hotplug_devname {
  char      hwname[64];
  int32_t   slot;
  int32_t   sbits;
  uint64_t  event_id;
  const struct dspd_dict *info;
  struct dspd_hotplug_devname *next;
};

struct dspd_hotplug {
  pthread_mutex_t     lock;
  uint32_t            device_count;
  struct dspd_dict *devices;
  struct dspd_ll     *handlers;
  int32_t                 default_playback;
  int32_t                 default_capture;
  struct dspd_hotplug_devname *names;
  struct dspd_dict *playback_search, *capture_search;

};

struct dspd_startup_callback {
  void (*callback)(void *arg);
  void *arg;
};
enum dspd_obj_type {
  DSPD_OBJ_TYPE_INVALID,
  DSPD_OBJ_TYPE_DAEMON_CTX,
  DSPD_OBJ_TYPE_IPC,
  DSPD_OBJ_TYPE_AIO,
};
struct dspd_rcb;
struct dspd_aio_ctx;
struct dspd_aio_fifo_ops;
struct dspd_vctrl_list;
struct cbpoll_ctx;
struct dspd_daemon_ctx {
  uint32_t magic;
  struct dspd_module_list *modules;
  int argc;
  char **argv;
  char  *path;

  struct dspd_hotplug      hotplug;
  struct dspd_slist      *objects;

  struct dspd_ll         *startup_callbacks;       
  const struct dspd_rcb  *ctl_ops;
  struct dspd_dict     *args_list;
  struct dspd_dict     *config;

  int32_t rtio_policy;
  int32_t rtio_priority;
  int32_t rtsvc_policy;
  int32_t rtsvc_priority;
  int32_t priority;
  //struct dspd_kvpair default_device;
  //struct dspd_dict *default_dev_info;

  int glitch_correction;

  char *modules_dir;
  

  struct dspd_sglist *syncgroups;

  int debug;
  uid_t uid;
  gid_t gid;
  char *user;
  mode_t ipc_mode;

  void *aio_handler_ctx;
  int32_t (*new_aio_ctx)(struct dspd_aio_ctx       **aio,  //Pointer to NULL (make new context) or existing context
			 //Ops for the new context if *aio!=NULL
			 const struct dspd_aio_fifo_ops  *ops,
			 void                       *arg,
			 //Optional sockets: {client,server}, {-1,server}
			 int32_t                     sockets[2], 
			 //Minimum value for max pending requests (0 for default)
			 ssize_t                     max_req,
			 //Remote context has extra error checking and restrictions
			 bool                        remote);
  
  struct dspd_vctrl_list *vctrl;

  struct cbpoll_ctx *main_thread_loop_context;
  
};


extern struct dspd_daemon_ctx dspd_dctx;
int dspd_daemon_init(int argc, char **argv);

int dspd_daemon_hotplug_register(const struct dspd_hotplug_cb *callbacks,
				 void *arg);
int dspd_daemon_hotplug_unregister(const struct dspd_hotplug_cb *callbacks,
				   void *arg);
uint64_t dspd_daemon_hotplug_event_id(char buf[32UL]);
int dspd_daemon_hotplug_add(const struct dspd_dict *dict);
int dspd_daemon_hotplug_remove(const struct dspd_dict *dict, const char *name);
int dspd_hotplug_delete(const struct dspd_dict *dict);
int dspd_daemon_get_config(const struct dspd_dict *sect,
			   struct dspd_drv_params *params);
int dspd_daemon_add_device(void **handles, 
			   int32_t stream,
			   uint64_t hotplug_event_id,
			   const struct dspd_pcmdrv_ops *playback_ops,
			   const struct dspd_pcmdrv_ops *capture_ops);

int dspd_daemon_register_startup(void (*callback)(void *arg),
				 void *arg);
int dspd_daemon_run(void);

int32_t dspd_daemon_dispatch_ctl(struct dspd_rctx *rctx,
				 const struct dspd_req_handler *handlers,
				 uint32_t count,
				 uint64_t             req,
				 const void          *inbuf,
				 size_t        inbufsize,
				 void         *outbuf,
				 size_t        outbufsize);

struct dspd_dispatch_ctl2_info {
  uint32_t                       min_req;
  uint32_t                       handlers_count;
  uint32_t                       req;
  const struct dspd_req_handler *handlers;
};

int32_t dspd_daemon_dispatch_ctl2(struct dspd_rctx *rctx,
				  struct dspd_dispatch_ctl2_info *info,
				  const void       *inbuf,
				  size_t            inbufsize,
				  void             *outbuf,
				  size_t            outbufsize);

int32_t dspd_daemon_ref(uint32_t stream, uint32_t flags);
void dspd_daemon_unref(uint32_t stream);
int32_t dspd_daemon_ref_by_name(const char *hwname, int32_t sbits);

#define DSPD_THREADATTR_RTIO     1
#define DSPD_THREADATTR_RTSVC    2
#define DSPD_THREADATTR_DETACHED 4
int dspd_daemon_threadattr_init(void *attr, size_t size, int flags);

struct dspd_dict *dspd_read_config(const char *module_name, bool exec_ok);
const char *dspd_get_config_dir(void);
dspd_time_t dspd_get_min_latency(void);
const char *dspd_get_modules_dir(void);
int32_t dspd_get_glitch_correction(void);

int dspd_daemon_set_ipc_perm(const char *path);
int dspd_daemon_set_ipc_perm_fd(int fd);
int dspd_daemon_set_thread_nice(int tid, int thread_type);
void dspd_daemon_set_thread_schedparam(int tid, int thread_type);
#endif
