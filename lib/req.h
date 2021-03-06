#ifndef _DSPD_REQ_H_
#define _DSPD_REQ_H_


#define DSPD_REQ_FLAG_ERROR   1
#define DSPD_REQ_FLAG_POLLIN  2
#define DSPD_REQ_FLAG_POLLOUT 4
//Probably a timer event for network support
#define DSPD_REQ_FLAG_POLLPRI 8
//Device is dead
#define DSPD_REQ_FLAG_POLLHUP 16
#define DSPD_REQ_FLAG_EVENT   32
//Event queue overflowed.  The client should
//poll all elements and devices for changes since
//some events were dropped.
#define DSPD_REQ_FLAG_OVERFLOW 64

#define DSPD_REQ_FLAG_ROUTE_CHANGED 128

#define DSPD_REQ_FLAG_NONBLOCK 256

struct dspd_req {
  uint32_t len;    //Length of entire packet
  uint32_t flags;  //Flags, such as error flag or events
  uint32_t cmd;    //Command.  The server turns this into a 32 bit integer
                   //that contains 2 flags
  int32_t  stream; //Stream number or -1 for socket server
  int32_t  bytes_returned; //Bytes returned from a pointer request
  union {
    uint32_t rlen;
    int32_t  err;
  } rdata;
  uint64_t tag;    //User defined value to be sent with reply
  char pdata[0];   //Payload
};

struct dspd_req_pointers {
  const void   *inbuf;
  size_t        inbufsize;
  void         *outbuf;
  size_t        outbufsize;
};

struct dspd_rcb {
  int32_t (*reply_buf)(struct dspd_rctx *arg, 
		       int32_t flags, 
		       const void *buf, 
		       size_t len);
  int32_t (*reply_fd)(struct dspd_rctx *arg, 
		      int32_t flags, 
		      const void *buf, 
		      size_t len, 
		      int32_t fd);
  int32_t (*reply_err)(struct dspd_rctx *arg, 
		       int32_t flags, 
		       int32_t err);
};



struct dspd_rctx_retry_data;
struct dspd_rctx {
  const struct dspd_rcb *ops;
  void *user_data;
  void *ops_arg;
  size_t bytes_returned;
  size_t outbufsize;
  void *outbuf;
  int32_t fd;
  int32_t index;
  int32_t flags;

  struct dspd_rctx_retry_data *retry;
};


#ifdef DSPD_ENABLE_REQ_RETRY
struct dspd_rctx_retry_data {
  void                          *alloc_buf; //buffer for temporary variables
  size_t                         alloc_max; //maximum size for temp. vars.
  size_t                         alloc_len; //length currently allocated
  int32_t                        state;     //if <=0 then completed, otherwise busy
  const struct dspd_req_handler *handler;   //last handler (saved for next invocation)
  const void                    *label;     //place to resume within handler->handler
  const void                    *inbuf;     //last input buffer
  size_t                         inbufsize; //last input buffer size
  int32_t                        req;       //raw request number (not index or otherwise decoded)
};



//Make sure a computed goto is always executed so that clang won't refuse to compile it.  With
//clang 3.3 it is possible to do "(void)&&begin" to fool it, but it is possible a future version
//might not be fooled so it is probably best to do a computed goto within the function every time.
//It is arguably a bug in clang since it can't prove that rctx->label is going to be non-NULL in
//a callback function with no computed gotos.
#define _DSPD_RCTX_RESUME(rctx) do {		\
  __label__ begin;				\
  if ( ! rctx->label )				\
    rctx->label = &&begin;			\
  goto *rctx->label;				\
begin:						\
  rctx->label = NULL;				\
  break;					\
  } while(0);

//clean and easy syntax for local variables: $.var = value;
#define $ (*$__local)

//declare local variables with an anonymous struct since the stack won't be restored
#define _DSPD_RCTX_LOCALVARS(rctx, ...) struct __VA_ARGS__ *$__local=dspd_rctx_alloc(rctx,sizeof(*$__local))

//define most (preferably all) of the function body: DSPD_RCTX_PROC(rctx,{ int x; long y; }){ /*code here*/ }
#define dspd_req_proc(rctx, ...) _DSPD_RCTX_LOCALVARS(rctx, __VA_ARGS__); _DSPD_RCTX_RESUME

//This macro returns a value which is set with dspd_req_retry()
#define dspd_req_await(rctx, _val) ({ __label__ _awlbl; int _ret; static const void *_awaddr = &&_awlbl; \
      return _dspd_rctx_await_fcn(rctx, _awaddr, &_val);		\
    _awlbl:								\
      _ret = rctx->retry->state;	_ret;				\
    })
int32_t _dspd_rctx_await_fcn(struct dspd_rctx *rctx, const void *label, int32_t val);
bool dspd_req_retry(struct dspd_rctx *rctx, int32_t result, int32_t *error);
void *dspd_rctx_alloc(struct dspd_rctx *rctx, size_t len);
//set pending to allow resuming manually (does not require dspd_req_proc or any computed goto)
#define dspd_req_set_pending(rctx) _dspd_rctx_await_fcn(rctx, NULL, -EINPROGRESS)
void dspd_rctx_reset(struct dspd_rctx *rctx);

#endif


int32_t dspd_req_flags(const struct dspd_rctx *rctx);

int32_t dspd_req_reply_buf(struct dspd_rctx *rctx, 
			   int32_t flags, 
			   const void *buf, 
			   size_t len);
//Close file descriptor after sending over socket
#define DSPD_REPLY_FLAG_CLOSEFD 1
#define DSPD_REPLY_FLAG_EVENT   2
int32_t dspd_req_reply_fd(struct dspd_rctx *rctx, 
			  int32_t flags, 
			  const void *buf, 
			  size_t len,
			  int32_t fd);
int32_t dspd_req_reply_err(struct dspd_rctx *rctx,
			   int32_t flags,
			   int32_t err);
//Get user data context pointer
void *dspd_req_userdata(struct dspd_rctx *rctx);
//Reap file descriptor
int32_t dspd_req_get_fd(struct dspd_rctx *rctx);
int32_t dspd_req_index(struct dspd_rctx *rctx);


#define DSPD_REQ_FLAG_CMSG_FD      (1<<31)
#define DSPD_REQ_FLAG_REMOTE       (1<<30)
#define DSPD_REQ_FLAG_UNIX_FAST_IOCTL   (1<<29)
#define DSPD_REQ_FLAG_UNIX_IOCTL (1<<28)
//Input and output are pointers.
#define DSPD_REQ_FLAG_POINTER (1<<27)
#define DSPD_REQ_FLAG_CMSG_CRED (1<<26)

#define DSPD_REQ_DEFAULT_XFLAGS (DSPD_REQ_FLAG_UNIX_IOCTL|DSPD_REQ_FLAG_UNIX_FAST_IOCTL|DSPD_REQ_FLAG_CMSG_FD|DSPD_REQ_FLAG_CMSG_CRED)

typedef int32_t (*dspd_req_callback_t)(struct dspd_rctx *context,
				       uint32_t      req,
				       const void   *inbuf,
				       size_t        inbufsize,
				       void         *outbuf,
				       size_t        outbufsize);

struct dspd_req_handler {
  dspd_req_callback_t handler;
  int32_t xflags; //excluded flags
  int32_t rflags; //required flags
  //Minimum sizes
  int32_t inbufsize;
  int32_t outbufsize;
  int32_t data;
  /*
    GCC 4.8.2 will apparently miscompile some stuff if this isn't padded.
    I haven't looked to deeply into it but gdb shows functions looping
    around executing mathematical statements multiple times (no loop
    in the source - WTF?) while skipping function calls in between and at 
    some point calls into dynamically linked functions go to the wrong 
    address and it segfaults.  This issue doesn't show up with clang 3.3.

  */

  int32_t reserved;
};				       
struct dspd_rpkt;



/*int dspd_dispatch_request(const struct dspd_req_handler *handlers,
			  size_t   count,
			  void    *context,
			  uint32_t req,
			  const void   *inbuf,
			  size_t        inbufsize,
			  void         *outbuf,
			  size_t        outbufsize,
			  size_t       *bytes_returned);*/


/*
  Generic packet format.  Each packet is prefixed with 4 bytes.  The last bit is
  indicates whether the message contains a file descriptor.  The first 31 bites
  are the size of the entire packet.

  Any packet that contains a file descriptor should declare the first 4 bytes of
  the payload to be int32_t.  The file descriptor number will be copied to the
  first 4 bytes of the payload so it can be included inline.  The caller that
  reaps the packet should check for PKT_CMSG_FD and reject the packet if
  a file descriptor was not expected.

  Any packets that are sent with a file descriptor must include it in the first 4 bytes
  of the packet and set PKT_CMSG_FD.
  
*/
#define PKT_CMSG_FD (1<<31)
struct dspd_rpkt {
  uint32_t len;
  uint32_t flags;
  //char barf[16];
  char     buf[0];
};

struct dspd_pktstat {
  bool     isfd; //Contains a file descriptor
  bool     iscred; //Contains credentials
  bool     started; //Has the operation been successfully started
  uint32_t len;     //Length of data to be transferred
  uint32_t offset;  //Offset (progress).  Done when len==offset&&len>0.
};

struct dspd_aio_ops;
struct dspd_req_ctx {
  //Callbacks for io.  Need to return -errno, positive result, or EOF (0).
  //ssize_t (*read)(void *arg, void *buf, size_t len);
  //ssize_t (*write)(void *arg, const void *buf, size_t len);
  //int     (*set_nonblocking)(void *arg, bool nonblock);
  //int     (*poll)(void *arg, int events);
  //If getfd returns -1 then rx/tx of file descriptors is not supported.
  //int (*getfd)(void *arg);
  const struct dspd_aio_ops *ops;
  void   *arg;
  
  size_t  hdrlen; //Size of packet header.  At least 4 bytes.
  size_t  rxmax, txmax; //Maximum packet sizes
  int recvfd; //Last received file descriptor
  //Packet buffers
  struct dspd_req *rxpkt, *txpkt;
  //States of packets.
  struct dspd_pktstat      rxstat, txstat;
  int32_t fd_out;
  int32_t pid, gid, uid;
};
void dspd_req_ctx_delete(struct dspd_req_ctx *ctx);
struct dspd_req_ctx *dspd_req_ctx_new(size_t pktlen,
				      size_t hdrlen,
				      const struct dspd_aio_ops *ops,
				      void *arg);
//Receive the packet
int32_t dspd_req_recv(struct dspd_req_ctx *ctx);
//Send the packet
int32_t dspd_req_send(struct dspd_req_ctx *ctx, int32_t fd);
//Reap the received packet.
int32_t dspd_req_reap(struct dspd_req_ctx *ctx, void **buf, size_t *len, int32_t *fd);
//Get write buffer.
ssize_t dspd_req_get_wbuf(struct dspd_req_ctx *ctx, void **buf, size_t len);


int dspd_stream_ctl(void    *context, //DSPD object, such as dspd_dctx
		    uint32_t stream,  //Slot in object list (0 is for daemon)
		    uint32_t req,     //16 bit req + 2 bits flags + 14 bits reserved
		    const void          *inbuf,
		    size_t        inbufsize,
		    void         *outbuf,
		    size_t        outbufsize,
		    size_t       *bytes_returned);

/*
  This makes it possible to use named arguments since there are too many.  I think composing a whole
  struct every time isn't any better and other options require lots of function calls.

  Now it can be invoked like this (params in any order, default value is 0):

  int32_t err = dspd_stream_npctl({.context=clientptr, .stream=123, .request=SOMEREQ, .inbuf=&somestruct, .inbufsize=sizeof(somestruct), .bytes_returned=&br})

  This is probably a little bit slower so it might be best only for calls that are not frequently made.

*/
struct _dspd_stream_ctlparams {
  void *context;
  uint32_t    stream;
  uint32_t    request;
  const void *inbuf;
  size_t      inbufsize;
  void       *outbuf;
  size_t      outbufsize;
  size_t     *bytes_returned;
};


static inline int32_t _dspd_stream_npctl_fcn(struct _dspd_stream_ctlparams params)
{
  return dspd_stream_ctl(params.context,
			 params.stream,
			 params.request,
			 params.inbuf,
			 params.inbufsize,
			 params.outbuf,
			 params.outbufsize,
			 params.bytes_returned);
}
#define dspd_stream_npctl(...) _dspd_stream_npctl_fcn((struct _dspd_stream_ctlparams)__VA_ARGS__)


ssize_t dspd_req_read_cb(void *arg, void *buf, size_t len);
ssize_t dspd_req_write_cb(void *arg, const void *buf, size_t len);
int dspd_req_getfd_cb(void *arg);

int dspd_ctx_get_fd(void *context);

bool dspd_req_input_pending(struct dspd_req_ctx *ctx);
bool dspd_req_output_pending(struct dspd_req_ctx *ctx);
struct iovec;
int dspd_cmsg_sendfd(int s, int fd, struct iovec *data);
int dspd_cmsg_recvfd(int s, struct iovec *iov);
#endif
