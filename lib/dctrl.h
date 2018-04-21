#ifndef _DSPD_DCTRL_H_
#define _DSPD_DCTRL_H_
#define DSPD_DCTL_ENUM_TYPE_SERVER 1
#define DSPD_DCTL_ENUM_TYPE_CLIENT 2
#define DSPD_DCTL_ENUM_TYPE_ANY 0

enum dspd_client_cb_idx {
  DSPD_CLIENT_CB_ERROR,
};
struct dspd_client_cb {
  enum dspd_client_cb_idx  index;
  void                    *callback;
  void                    *arg;
};


enum dspd_dctl_req {
  //Daemon control
  DSPD_DCTL_MIN = 0, //Make this reserved for now
  DSPD_DCTL_GET_OBJMASK_SIZE,
  DSPD_DCTL_ENUMERATE_OBJECTS,
  DSPD_DCTL_GET_MODULE_COUNT,
  DSPD_DCTL_GET_MODULE_NAME,
  /*
    TODO: Add support for new client.  Needs to be local context
    only.  A server, such as mod_socketserver, would use stream -1
    to handle this in a safe way.  Maybe it could even hook
    stream 0 and check for success or that could be done in
    dspd_stream_ctl().
  */
  DSPD_DCTL_NEW_CLIENT,
  DSPD_DCTL_DELETE_CLIENT,

  DSPD_DCTL_GET_DEFAULTDEV,
  DSPD_DCTL_ASYNC_RESERVED,
  DSPD_DCTL_SYNCSTART,
  DSPD_DCTL_SYNCSTOP,
  DSPD_DCTL_LAST = DSPD_DCTL_SYNCSTOP,
  DSPD_DCTL_MAX = 4095,

  //Stream object control
  DSPD_SCTL_MIN = 4096,
  DSPD_SCTL_CLIENT_MIN = DSPD_SCTL_MIN,
  //These take an argument to get the stream (capture or playback)
  DSPD_SCTL_CLIENT_START,
  DSPD_SCTL_CLIENT_STOP,
  DSPD_SCTL_CLIENT_STATUS,
  DSPD_SCTL_CLIENT_AVAIL,
  DSPD_SCTL_CLIENT_READ,
  DSPD_SCTL_CLIENT_WRITE,
  DSPD_SCTL_CLIENT_GETPARAMS,
  DSPD_SCTL_CLIENT_SETPARAMS,
  DSPD_SCTL_CLIENT_SETVOLUME,
  DSPD_SCTL_CLIENT_GETVOLUME,
  DSPD_SCTL_CLIENT_CONNECT, //Connect: client=>server
  DSPD_SCTL_CLIENT_DISCONNECT,
  DSPD_SCTL_CLIENT_RAWPARAMS, //Configure raw params for server
  DSPD_SCTL_CLIENT_GETTRIGGER,
  DSPD_SCTL_CLIENT_STAT,
  DSPD_SCTL_CLIENT_MAPBUF,
  DSPD_SCTL_CLIENT_GETCHANNELMAP, //Get channel map
  DSPD_SCTL_CLIENT_SETCHANNELMAP, //Set channel map
  DSPD_SCTL_CLIENT_SETCB,         //Set callback
  DSPD_SCTL_CLIENT_RESERVE,
  DSPD_SCTL_CLIENT_SETTRIGGER,
  DSPD_SCTL_CLIENT_SYNCGROUP,
  DSPD_SCTL_CLIENT_SYNCCMD,
  DSPD_SCTL_CLIENT_LOCK,
  DSPD_SCTL_CLIENT_SWPARAMS,
  DSPD_SCTL_CLIENT_PAUSE,
  DSPD_SCTL_CLIENT_SETINFO,
  DSPD_SCTL_CLIENT_MAX = 6144,
  DSPD_SCTL_SERVER_MIN,
  DSPD_SCTL_SERVER_CONNECT, //Connect: server<=client
  DSPD_SCTL_SERVER_DISCONNECT,
  DSPD_SCTL_SERVER_SETTRIGGER, //Takes client and streams
  DSPD_SCTL_SERVER_SETLATENCY,
  DSPD_SCTL_SERVER_GETPARAMS,
  DSPD_SCTL_SERVER_SETVOLUME,
  DSPD_SCTL_SERVER_GETVOLUME,
  DSPD_SCTL_SERVER_STAT,
  DSPD_SCTL_SERVER_GETCHANNELMAP,  //Get channel map (input is stream)
  DSPD_SCTL_SERVER_GETLATENCY,
  DSPD_SCTL_SERVER_RESERVE, //Reserve a slot
  DSPD_SCTL_SERVER_CONVERT_CHMAP, //Convert a client specification into a compatible map.
  DSPD_SCTL_SERVER_PCM_LAST = DSPD_SCTL_SERVER_MIN + 256,

  DSPD_SCTL_SERVER_MIXER_ELEM_COUNT,
#define DSPD_SCTL_SERVER_MIXER_FIRST DSPD_SCTL_SERVER_MIXER_ELEM_COUNT
#define DSPD_MIXER_CMDN(_n) (_n-DSPD_SCTL_SERVER_MIXER_ELEM_COUNT)

  DSPD_SCTL_SERVER_MIXER_ELEM_INFO,
  DSPD_SCTL_SERVER_MIXER_ENUM_INFO,
  DSPD_SCTL_SERVER_MIXER_GETVAL,
  DSPD_SCTL_SERVER_MIXER_SETVAL,
  DSPD_SCTL_SERVER_MIXER_GETRANGE,
  DSPD_SCTL_SERVER_MIXER_HWCMD, //Hardware specific command
  DSPD_SCTL_SERVER_IRQINFO,
  DSPD_SCTL_SERVER_MIXER_SETCB,
  DSPD_SCTL_SERVER_LOCK,
  DSPD_DCTL_ASYNC_EVENT,
  DSPD_SCTL_SERVER_REMOVE,
  DSPD_SCTL_MAX = 8191,
  DSPD_SCTL_SERVER_MAX = DSPD_SCTL_MAX,

  //User defined commands
  DSPD_UCTL_MIN = 8192,
  DSPD_UCTL_MAX = 16384,
  
  //The rest is reserved for now
  
};

struct dspd_sg_info {
  uint32_t sgid;
  uint32_t sbits;
};



#define DSPD_REQ(_n) ((_n)&(~(DSPD_REQ_FLAG_CMSG_FD|DSPD_REQ_FLAG_REMOTE)))
#define DSPD_REMOTE(_n) (!!((_n)&DSPD_REQ_FLAG_REMOTE))
#define DSPD_FD(_n) (!!((_n)&DSPD_REQ_FLAG_CMSG_FD))

#ifdef _DSPD_CTL_MACROS
#define DIDX(_n) ((_n)-DSPD_DCTL_MIN)
#define DSPD_DCTL_

#define SIDX(_n) ((_n)-DSPD_SCTL_MIN)
#define DSPD_SCTL_COUNT (DSPD_SCTL_MAX-DSPD_SCTL_MIN)

#define CLIDX(_n) ((_n)-DSPD_SCTL_CLIENT_MIN)
#define DSPD_SCTL_CLIENT_COUNT (DSPD_SCTL_CLIENT_MAX-DSPD_SCTL_CLIENT_MIN)

#define SRVIDX(_n) ((_n)-DSPD_SCTL_SERVER_MIN)
#define DSPD_SCTL_SERVER_COUNT (DSPD_SCTL_SERVER_MAX-DSPD_SCTL_SERVER_MIN)


#define UIDX(_n) ((_n)-DSPD_UCTL_MIN)
#define DSPD_UCTL_COUNT (DSPD_UCTL_MAX-DSPD_UCTL_MAX)
#endif

struct dspd_stream_volume {
  int32_t stream;
  float   volume;
};

#define DSPD_CTL_EVENT_MASK_REMOVE 	(~0U)
#define DSPD_CTL_EVENT_MASK_VALUE	(1<<0)
#define DSPD_CTL_EVENT_MASK_INFO	(1<<1)
#define DSPD_CTL_EVENT_MASK_ADD		(1<<2)
#define DSPD_CTL_EVENT_MASK_TLV		(1<<3)
#define DSPD_CTL_EVENT_MASK_CHANGED     (1<<31)
typedef void (*dspd_mixer_callback)(int32_t card,
				    int32_t elem,
				    uint32_t mask,
				    void *arg);
struct dspd_mixer_cbinfo {
  bool                remove;
  dspd_mixer_callback callback;
  void               *arg;
};

#define DSPD_MIXF_PVOL     (1<<0)
#define DSPD_MIXF_CVOL     (1<<1)
#define DSPD_MIXF_PMONO    (1<<2)
#define DSPD_MIXF_CMONO    (1<<3)
#define DSPD_MIXF_PSWITCH  (1<<4)
#define DSPD_MIXF_CSWITCH  (1<<5)
#define DSPD_MIXF_CDB      (1<<6)
#define DSPD_MIXF_PDB      (1<<7)
#define DSPD_MIXF_COMMSWITCH (1<<8)
#define DSPD_MIXF_COMMVOL    (1<<9)

#define DSPD_MIXF_PVJOINED (1<<10)
#define DSPD_MIXF_CVJOINED (1<<11)
#define DSPD_MIXF_PSWJOINED (1<<12)
#define DSPD_MIXF_CSWJOINED (1<<13)
#define DSPD_MIXF_CSWEXCL (1<<14)
//Device has enumerated values.  An example would
//be "input source".  That may have values assigned an index
//and a name ("Front Mic", etc).
#define DSPD_MIXF_ENUM  (1<<15)
#define DSPD_MIXF_ENUMP (1<<16)
#define DSPD_MIXF_ENUMC (1<<17)
//Software volume control
#define DSPD_MIXF_VIRTUAL (1<<18)

#define DSPD_MIXF_PLAYBACK (DSPD_MIXF_PVOL|DSPD_MIXF_PDB|DSPD_MIXF_ENUMP|DSPD_MIXF_PSWITCH)
#define DSPD_MIXF_CAPTURE (DSPD_MIXF_CVOL|DSPD_MIXF_CDB|DSPD_MIXF_ENUMC|DSPD_MIXF_CSWITCH)

#define DSPD_MIXF_VOL (DSPD_MIXF_PVOL|DSPD_MIXF_CVOL|DSPD_MIXF_PDB|DSPD_MIXF_CDB)



struct dspd_mix_range {
  int32_t min, max;
};

struct dspd_mix_enum_idx {
  uint32_t elem_idx;
  uint32_t enum_idx;
};


struct dspd_mix_val {
  uint32_t  index;
  uint32_t reserved;
  uint64_t type;
  int32_t value;
#define DSPD_MIX_CONVERT -1
  int32_t channel;
  int32_t dir;
#define DSPD_CTRLF_TSTAMP_32BIT 1
#define DSPD_CTRLF_SCALE_PCT 2
  int32_t flags;
  //Device specific information.
  uint64_t hwinfo;
  uint64_t tstamp;
  uint64_t update_count;
};

struct dspd_mix_info {
  uint64_t flags;
  uint64_t hwinfo;
  uint64_t tstamp;
  uint64_t update_count;
  uint32_t pchan_mask;
  uint32_t cchan_mask;
  uint32_t ctl_index;
  int32_t  vol_index; //Enum only
  char     name[32];
};

typedef enum _dspd_mixer_elem_channel_id {
  /** Unknown */
  DSPD_MIXER_CHN_UNKNOWN = -1,
  /** Front left */
  DSPD_MIXER_CHN_FRONT_LEFT = 0,
  /** Front right */
  DSPD_MIXER_CHN_FRONT_RIGHT,
  /** Rear left */
  DSPD_MIXER_CHN_REAR_LEFT,
  /** Rear right */
  DSPD_MIXER_CHN_REAR_RIGHT,
  /** Front center */
  DSPD_MIXER_CHN_FRONT_CENTER,
  /** Woofer */
  DSPD_MIXER_CHN_WOOFER,
  /** Side Left */
  DSPD_MIXER_CHN_SIDE_LEFT,
  /** Side Right */
  DSPD_MIXER_CHN_SIDE_RIGHT,
  /** Rear Center */
  DSPD_MIXER_CHN_REAR_CENTER,
  DSPD_MIXER_CHN_LAST = 31,
  /** Mono (Front left alias) */
  DSPD_MIXER_CHN_MONO = DSPD_MIXER_CHN_FRONT_LEFT
} dspd_mixer_elem_channel_id_t;


struct dspd_mix_irqinfo {
  uint32_t irq_count;
  uint32_t ack_count;
};
struct dspd_mix_irqinfo64 {
  uint64_t irq_count;
  uint64_t ack_count;
};

struct dspd_sync_cmd {
  uint32_t streams;
#define SGCMD_START 1
#define SGCMD_STOP  2
#define SGCMD_STARTALL 3
  uint32_t cmd;
  uint32_t sgid;
  uint32_t reserved;
  uint64_t tstamp;
};



size_t dspd_mixf_getname(size_t index, char *name, size_t len);

void dspd_mixf_dump(uint64_t mask);



/*
  These events are sent asynchronously when the client is subscribed.
  They mean that something most likely changed.  It is possible that
  something changed and changed back by the time the client polled the
  object and it is possible that race conditions (the type that resolve
  after a short time) cause spurious events to be sent.  The server
  should never send so many events that it causes a performance problem.
*/
#define DSPD_EVENT_HOTPLUG   1
#define DSPD_EVENT_CONTROL   2
#define DSPD_EVENT_RESETFLAGS 3
#define DSPD_EVENT_SETFLAGS 4

#define DSPD_EVENT_FLAG_HOTPLUG 1
#define DSPD_EVENT_FLAG_CONTROL 2
#define DSPD_EVENT_FLAG_VCTRL   4

struct dspd_async_event {
  uint32_t event;
  uint32_t flags;
  uint32_t arg1;
  uint32_t arg2;
};


#ifdef _DSPD_HAVE_UCRED
struct dspd_cli_info_pkt {
  union {
    struct ucred cred;
    int32_t      pad[4];
  } cred;
  char    name[32];
};
#endif

//The -1 special makes Linux fill in the default credentials.
//Otherwise, it is valid to use a thread id or some privileged processes may
//fill in arbitrary credentials.
#define DSPD_CLI_INFO_DEFAULT -1
struct dspd_cli_info {
  int32_t stream;
#define DSPD_CLI_INFO_TID -2
  int32_t pid;
  int32_t uid;
  int32_t gid;
  char    name[32];
};

#endif
