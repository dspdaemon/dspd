#ifndef _MOD_OSSCUSE_H_
#define _MOD_OSSCUSE_H_
#include <sys/ioctl.h>
#include "soundcard.h"
enum {
  IORP_OK,        //Queued
  IORP_CANCELED,  //Canceled (deferred)
  IORP_COMPLETED, //Completed and freed
};
/*
  IO request packet.  This gets sent to a worker thread (one per client for /dev/dsp).
  All of the fuse requests except for open start with the 64 bit identifier so the
  dispatcher should be able to copy the request to another memory queue or malloc()
  a buffer.
*/
struct iorp {
  void          *addr;      //Address of the payload (fuse_in_header+data)
  void          *alloc_ctx; //Allocator (might be NULL, pass to dspd_rtalloc_free())
  volatile AO_t  canceled;  //Set to 1 if canceled (writer needs to wake thread.  op may
                            //complete partially, fully, or not at all).
  uint64_t       unique;    //Unique id from fuse_in_header
};

struct oss_cdev_client;

struct oss_cdev_ops {
  void (*read)(struct oss_cdev_client *cli, size_t size, off_t off, int flags);
  void (*write)(struct oss_cdev_client *cli, const char *buf, size_t size, off_t off, int flags);
  void (*flush)(struct oss_cdev_client *cli);
  void (*release)(struct oss_cdev_client *cli);
  void (*fsync)(struct oss_cdev_client *cli, int datasync);
  void (*ioctl)(struct oss_cdev_client *cli,
		int cmd,
		void *arg,
		int flags,
		const void *in_buf,
		size_t in_bufsz,
		size_t out_bufsz);
  void (*poll)(struct oss_cdev_client *cli, uint64_t ph);

  void (*free)(struct oss_cdev_client *cli);
};

struct oss_dsp_cdev;

struct snd_mixer_oss_assign_table {
  int32_t      oss_id;
  const char  *name;
  int32_t      index;
  uint64_t     flags;
};

struct dspd_oss_server {
  /*This is the buffer used when memory is low*/
  char    *inbuf;
  size_t   inbufsize;
  struct dspd_rtalloc *alloc;
  pthread_mutex_t lock;
  struct cbpoll_ctx cbpoll, ctl_cbpoll;
  struct rtcuse_cdev_params dsp_params;
  pthread_attr_t client_threadattr;
  pthread_condattr_t client_condattr;

  pthread_rwlock_t    devtable_lock;
  struct oss_dsp_cdev *dsp_table[DSPD_MAX_OBJECTS];
  struct oss_dsp_cdev *ctl_table[DSPD_MAX_OBJECTS];
  struct oss_dsp_cdev *v4_mixer;
  bool enable_v4mixer, enable_legacymixer;
  bool persistent_devnodes; //Don't remove dead device nodes and reuse them later.
  const char *devnode_prefix;
  struct dspd_dict *config;
  const struct snd_mixer_oss_assign_table *ctl_assignments;

  int32_t cuse_helper_fd;
  int32_t helper;

};




struct dsp_data {
  struct dspd_rclient    rclient;
  uint32_t rate;
  uint32_t channels;
  int32_t  format;
  uint32_t buffer_bytes;
  uint32_t frag_bytes;
  uint32_t frame_bytes;
  uint32_t low_water;
  int      trigger;
  int      started;
  uint32_t subdivision;
  int32_t  profile;
  bool                    params_set;
  bool                    force_raw;
  bool                    cooked;
  struct dspd_cli_params  params;
  struct dspd_device_stat devinfo;
  int32_t                 policy;
  uint32_t                fflags;
  char                   *readbuf;
  size_t                  readlen;
  uint64_t                capture_read, playback_written;
  size_t                  max_write, max_read;
  unsigned long long      channelmap;
};
struct oss_legacy_mixer_table;
struct oss_cdev_client {
  struct dspd_rtalloc     *alloc;
  struct dspd_fifo_header *eventq;
  int32_t client_index;
  int32_t device_index;
  int32_t cdev_slot;
  int32_t error; //If nonzero then automatically reply to all requests with an error.
  /*These can be used for timing and for waiting on input*/
  dspd_mutex_t lock;
  dspd_cond_t  event;
  volatile AO_TS_t wakeup;
  struct oss_dsp_cdev *cdev;

  struct iorp        *current_iorp;
  struct rtcuse_ipkt *current_pkt;
  uint32_t            current_count;

  uint64_t            pollhandle;
  bool                poll_ok;
  bool                poll_armed;

  //void               *pkt_alloc;

  //This can be taken with pthread_mutex_trylock() in the cbpoll thread
  //to try to complete requests without deferring.
  //It should be used for poll, read, and write.
  //It must only be used when nothing is pending.
  //That means the worker thread should free the request
  //before replying.  It can't be used for ioctl because that
  //makes everything too messy.
  //pthread_mutex_t data_lock;
  pthread_t thread;
  
  uint64_t unique;
  uint64_t fh;
  uint32_t flags;
  uint32_t mode;
  void *client_ptr;

  struct dsp_data dsp;

  volatile int op_error;

  const struct oss_cdev_ops *ops;
  struct oss_legacy_mixer_table *elements;
};

struct oss_dsp_cdev {
  bool                      is_mixer;
  struct dspd_rtalloc      *alloc;
  int32_t                   playback_index, capture_index;
  struct rtcuse_cdev       *cdev;
  int32_t                   cdev_index;
  uint32_t                  cbpoll_index;

  struct rtcuse_ipkt       *ctlpkt;

  /*
    The client context should be released by sending a command back to the cbpoll thread
    which will do whatever needs done and then queue some operations on another thread.
   */
  struct oss_cdev_client   *clients[DSPD_MAX_OBJECTS];
  volatile int error;
  dspd_mutex_t              lock;
  bool dead;
};

struct new_client_req {
  uint32_t             flags;
  struct oss_dsp_cdev *dev;
};

struct oss_mix_elem {
  uint64_t tstamp;
  uint64_t flags;
  int32_t  type;
  int32_t  elem_index;
  int32_t  channels;
  //Table of oss to dspd conversions.
  int32_t *enumerations;
  int32_t *controls;
  size_t   enum_count;
};

struct oss_legacy_mixer_table {
  int32_t               count;
  struct oss_mix_elem   *elements; //[SOUND_MIXER_NONE+1];
};


typedef int32_t (*cdev_callback_t)(struct oss_cdev_client *cli);

int oss_reply_write(struct oss_cdev_client *cli, size_t count);
int oss_reply_error(struct oss_cdev_client *cli, int32_t error);
int dspd_cdev_client_sleep(struct oss_cdev_client *cli, dspd_time_t *abstime, bool alertable);
int oss_reply_ioctl(struct oss_cdev_client *cli, uint32_t result, const void *buf, size_t size);

int oss_req_interrupted(struct oss_cdev_client *cli);
bool dsp_check_poll(struct oss_cdev_client *cli);
int oss_reply_poll(struct oss_cdev_client *cli, uint32_t revents);
int oss_reply_buf(struct oss_cdev_client *cli, const char *buf, size_t size);
uint32_t dsp_check_revents(struct oss_cdev_client *cli);
void osscuse_get_sysinfo(oss_sysinfo *info);
extern const struct oss_cdev_ops osscuse_dsp_ops;
extern const struct oss_cdev_ops osscuse_legacy_ops;
extern const struct oss_cdev_ops osscuse_mixer_ops;
size_t oss_mixer_count(void);
struct oss_dsp_cdev *oss_find_cdev(int dspnum);
void oss_unlock_cdev(struct oss_dsp_cdev *dev);
struct oss_dsp_cdev *oss_lock_cdev(int dspnum);
int32_t osscuse_get_audio_engine(int32_t num, int32_t *card, int32_t *next);
#define OSS_CARDINFO_FULLDUPLEX 1

int osscuse_get_cardinfo(int dev, struct dspd_device_stat *info);

const struct snd_mixer_oss_assign_table *oss_get_mixer_assignment(const struct dspd_mix_info *info);

int oss_new_legacy_mixer_assignments(int32_t device, struct oss_legacy_mixer_table **table);
void oss_delete_legacy_mixer_assignments(struct oss_legacy_mixer_table *table);
#define OSSCUSE_ENABLE_HELPER 1
#define OSSCUSE_DISABLE_HELPER 0
#define OSSCUSE_HELPER_AUTO -1
struct osscuse_open_req {
#define DEVTYPE_MIXER 1
#define DEVTYPE_DSP   2
  int32_t devtype;
  int32_t devnum;
  struct rtcuse_cdev_params params;
};

#endif
