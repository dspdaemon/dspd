#ifndef _DSPDAIO_H_
#define _DSPDAIO_H_
struct ucred;
struct dspd_async_op;
struct dspd_aio_ctx;
typedef void (*dspd_aio_ccb_t)(void *context, struct dspd_async_op *op);
struct dspd_async_op {
  uint32_t     stream;
  uint32_t     req;
  const void  *inbuf;
  size_t       inbufsize;
  void        *outbuf;
  size_t       outbufsize;
  size_t       xfer;
  //Negative errno (real error), 0 (success), or positive errno (internal use)
  int32_t      error;
  uint32_t     tag;
  void        *data;
  uint32_t     flags;
  uint64_t     reserved;
  dspd_aio_ccb_t complete;
};

struct dspd_aio_ops {
  ssize_t (*writev)(void *arg, struct iovec *iov, size_t count);
  ssize_t (*write)(void *arg, const void *buf, size_t count);
  ssize_t (*read)(void *arg, void *buf, size_t length);
  int32_t (*recvfd)(void *arg, struct iovec *iov);
  int32_t (*sendfd)(void *arg, int32_t fd, struct iovec *data);
  int32_t (*poll)(void *arg, int32_t events, int32_t *revents, int32_t timeout);
  int32_t (*set_nonblocking)(void *arg, bool nonblocking);
  ssize_t (*send_cred)(void *arg, const struct ucred *uc, const void *data, size_t length);
  ssize_t (*recv_cred)(void *arg, struct ucred *uc, void *data, size_t length);
  void    (*close)(void *arg);
  void    (*poll_async)(void *arg, uint32_t events);
};

typedef void (*dspd_aio_event_cb_t)(struct dspd_aio_ctx     *context,
				    void                    *arg,
				    uint32_t                 req,
				    int32_t                  stream,
				    int32_t                  flags,
				    const struct dspd_async_event *evt,
				    const void              *buf,
				    size_t                   len);

#define DSPD_AIO_SYNC    -1
#define DSPD_AIO_DEFAULT  0

struct dspd_aio_ctx {
  uint32_t               magic;
#define DSPD_AIO_TYPE_SOCKET 1
#define DSPD_AIO_TYPE_FIFO   2
  int32_t                io_type;
  struct dspd_async_op **pending_ops;
  size_t                 max_ops;
  size_t                 user_max_ops;
  ssize_t                pending_ops_count;
  size_t                 position;
  ssize_t                current_op;
  bool                   cancel_pending;
  int32_t                error;
  void                  *ops_arg;
  const struct dspd_aio_ops *ops;
  int32_t                iofd;
  bool                   local;
  int32_t                slot;
  void (*io_ready)(struct dspd_aio_ctx *ctx, void *arg);
  void  *io_ready_arg;


  void (*io_dead)(struct dspd_aio_ctx *ctx, void *arg, bool closing);
  void  *io_dead_arg;
  
  struct iovec              iov_out[2];
  size_t                    cnt_out;
  size_t                    len_out;
  size_t                    off_out;
  struct dspd_req           req_out;
  uint16_t                  seq_out;
  bool                      output_pending;
  struct dspd_req_pointers  ptrs_out;

  int32_t                last_fd;
  int32_t                uid, gid, pid;
  struct dspd_req        req_in;
  size_t                 off_in;
  char                   buf_in[SS_MAX_PAYLOAD];
  ssize_t                op_in;
 
  
  void  *async_event_arg;
  dspd_aio_event_cb_t async_event;
  

  uint32_t event_flags;
  void (*event_flags_changed)(void *arg, uint32_t *flags);
  void *event_flags_changed_arg;
  
  uint32_t revents;

  //Data for convenience functions, such as dspd_aio_set_info().
  //Any dspd_aio_ function that can complete asynchronously without
  //a dspd_async_op will use this buffer and for that reason, convenience
  //functions must be run one at a time.
  char   data[256];
  size_t datalen;
  bool dead;

  void (*io_submitted)(struct dspd_aio_ctx *ctx, 
		       const struct dspd_async_op *op,
		       void *arg);
  void (*io_completed)(struct dspd_aio_ctx *ctx, 
		       const struct dspd_async_op *op,
		       void *arg);
  void *io_arg;

  void (*shutdown_cb)(struct dspd_aio_ctx *ctx, void *arg);
  void *shutdown_arg;

  int32_t aio_index;
};

int32_t dspd_aio_sock_new(intptr_t sv[2], ssize_t max_req, int32_t flags, bool local);
extern struct dspd_aio_ops dspd_aio_sock_ops;

void dspd_aio_set_dead_cb(struct dspd_aio_ctx *ctx, 
			  void (*io_dead)(struct dspd_aio_ctx *ctx, void *arg, bool closing),
			  void  *arg);
void dspd_aio_set_ready_cb(struct dspd_aio_ctx *ctx, 
			   void (*io_ready)(struct dspd_aio_ctx *ctx, void *arg),
			   void  *arg);
int32_t dspd_aio_submit(struct dspd_aio_ctx *ctx, struct dspd_async_op *op);
int32_t dspd_aio_cancel(struct dspd_aio_ctx *ctx, struct dspd_async_op *op, bool async);
int32_t dspd_aio_process(struct dspd_aio_ctx *ctx, int32_t revents, int32_t timeout);
int dspd_aio_sync_ctl(struct dspd_aio_ctx *ctx,
		      uint32_t stream,
		      uint32_t req,
		      const void          *inbuf,
		      size_t        inbufsize,
		      void         *outbuf,
		      size_t        outbufsize,
		      size_t       *bytes_returned);
int32_t dspd_aio_send(struct dspd_aio_ctx *ctx);
int32_t dspd_aio_recv(struct dspd_aio_ctx *ctx);
int32_t dspd_aio_block_directions(struct dspd_aio_ctx *ctx);
int32_t dspd_aio_recv_fd(struct dspd_aio_ctx *ctx);
int32_t dspd_aio_new(struct dspd_aio_ctx **ctxp, ssize_t max_req);
void dspd_aio_destroy(struct dspd_aio_ctx *ctx);
void dspd_aio_delete(struct dspd_aio_ctx *ctx);
void dspd_aio_shutdown(struct dspd_aio_ctx *ctx, 
		       void (*shutdown_cb)(struct dspd_aio_ctx *ctx, void *arg),
		       void *arg);
int32_t dspd_aio_get_iofd(struct dspd_aio_ctx *aio);
bool dspd_aio_is_local(struct dspd_aio_ctx *aio);
struct dspd_aio_fifo_ops;
int32_t dspd_aio_connect(struct dspd_aio_ctx *ctx, const char *addr, void *context, const struct dspd_aio_fifo_ops *ops, void *arg);
void dspd_aio_set_event_cb(struct dspd_aio_ctx *ctx, 
			   dspd_aio_event_cb_t async_event,
			   void  *async_event_arg);
void dspd_aio_get_event_cb(struct dspd_aio_ctx *ctx, dspd_aio_event_cb_t *async_event, void **arg);


void dspd_aio_set_event_flag_cb(struct dspd_aio_ctx *ctx, 
				void (*event_flags_changed)(void *arg, uint32_t *flags),
				void *arg);

uint32_t dspd_aio_get_event_flags(struct dspd_aio_ctx *ctx, bool clear);
uint32_t dspd_aio_revents(struct dspd_aio_ctx *ctx);

int32_t dspd_aio_set_info(struct dspd_aio_ctx *ctx, 
			  const struct dspd_cli_info *info,
			  dspd_aio_ccb_t complete,
			  void *arg);

struct dspd_aio_fifo_master;
struct dspd_aio_fifo_ctx;

struct dspd_aio_fifo_ops {
  int32_t (*wake)(struct dspd_aio_fifo_ctx *ctx, void *arg);
  int32_t (*wait)(struct dspd_aio_fifo_ctx *ctx, void *arg, int32_t timeout);
  int32_t (*reset)(struct dspd_aio_fifo_ctx *ctx, void *arg);
};

struct dspd_aio_fifo_eventfd {
  int                fd;
  volatile dspd_ts_t tsval;
};
struct dspd_aio_fifo_ptevent {
  pthread_mutex_t    *lock;
  pthread_cond_t     *cond;
  volatile dspd_ts_t  tsval;
};
extern struct dspd_aio_fifo_ops dspd_aio_fifo_ptevent_ops;
extern struct dspd_aio_fifo_ops dspd_aio_fifo_eventfd_ops;


//The client and server have rx and tx reversed from each other.
struct dspd_aio_fifo_ctx {
  struct dspd_fifo_header        *rx, *tx;
  struct dspd_fifo_header        *rxoob, *txoob;
  struct dspd_aio_fifo_master    *master;
  const struct dspd_aio_fifo_ops *ops;
  void                           *arg;
  bool                            nonblocking;
  struct dspd_aio_fifo_ctx       *peer;
  volatile dspd_ts_t              lock;
  volatile AO_t                   poll_events;

};

struct dspd_aio_fifo_oob_msg {
  int32_t fd;
};



struct dspd_aio_fifo_master {
  struct dspd_fifo_header           *rx, *tx;
  //Out of band messages.  This should work because the ordering will be the same as
  //the in band messages.
  struct dspd_fifo_header           *txoob, *rxoob;
  pthread_mutex_t                    lock;
  volatile struct dspd_aio_fifo_ctx *client, *server;
  struct dspd_aio_fifo_ctx           ctx[2];
  int32_t                            slot;
  bool                               remote;
};

int32_t dspd_aio_fifo_new(struct dspd_aio_fifo_ctx *ctx[2], 
			  ssize_t max_req,
			  bool    local,
			  const struct dspd_aio_fifo_ops *client_ops, 
			  void *client_arg,
			  const struct dspd_aio_fifo_ops *server_ops,
			  void *server_arg);
ssize_t dspd_aio_fifo_read(void *arg, void *buf, size_t len);
ssize_t dspd_aio_fifo_write(void *arg, const void *buf, size_t len);
int32_t dspd_aio_fifo_cmsg_recvfd(void *arg, struct iovec *iov);
ssize_t dspd_aio_fifo_readv(void *arg, struct iovec *iov, size_t iovcnt);
ssize_t dspd_aio_fifo_writev(void *arg, struct iovec *iov, size_t iovcnt);
int32_t dspd_aio_fifo_cmsg_sendfd(void *arg, int32_t fd, struct iovec *data);
void dspd_aio_fifo_close(void *arg);
int32_t dspd_aio_fifo_signal(struct dspd_aio_fifo_ctx *ctx, int32_t events);
int32_t dspd_aio_fifo_wait(struct dspd_aio_fifo_ctx *ctx, int32_t events, int32_t timeout);
int32_t dspd_aio_fifo_test_events(struct dspd_aio_fifo_ctx *ctx, int32_t events);
extern struct dspd_aio_ops dspd_aio_fifo_ctx_ops;
#endif /*ifdef _DSPDAIO_H_*/
