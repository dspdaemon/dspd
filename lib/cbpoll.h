#ifndef _DSPD_CBPOLL_H_
#define _DSPD_CBPOLL_H_
#include <sys/epoll.h>
#include <stdint.h>
struct cbpoll_ctx;

#define CBPOLL_ERRMASK (EPOLLRDHUP|EPOLLHUP|EPOLLERR)

#define CBPOLL_PIPE_MSG_DEFERRED_WORK 1
#define CBPOLL_PIPE_MSG_CALLBACK      2
#define CBPOLL_PIPE_MSG_USER          1000

#define CBPOLL_MAX_FDS INT16_MAX

struct cbpoll_msg {
  uint16_t len;
#define CBPOLL_MSG_EVENT_THREAD 1
#define CBPOLL_MSG_WORK_THREAD 2
  uint16_t flags;
  int32_t  fd;
  int16_t  index;
  int16_t  stream;
  int32_t  msg;
  int64_t  arg;
  int64_t  arg2;
  void (*callback)(struct cbpoll_ctx *ctx, struct cbpoll_msg *msg, void *data);
};

struct cbpoll_msg_ex {
  struct cbpoll_msg msg;
  uint8_t           extra_data[32];
};


struct cbpoll_wq {
  struct dspd_fifo_header  *fifo;
  dspd_cond_t               cond;
  dspd_mutex_t              lock;
  dspd_thread_t             thread;
  struct cbpoll_msg_ex      overflow;
};

struct cbpoll_fd_ops {
  int   (*fd_event)(void *data, 
		    struct cbpoll_ctx *context,
		    int index,
		    int fd,
		    int revents);
  int   (*pipe_event)(void *data, 
		      struct cbpoll_ctx *context,
		      int index,
		      int fd,
		      const struct cbpoll_msg *event);
  bool (*destructor)(void *data,
		     struct cbpoll_ctx *context,
		     int index,
		     int fd);
  int  (*set_events)(void *data, 
		     struct cbpoll_ctx *context,
		     int index,
		     int fd,
		     int events);

  void (*timer_event)(void *data,
		      struct cbpoll_ctx *context,
		      int index,
		      int fd,
		      dspd_time_t timeout);
  void (*refcnt_changed)(void *data, 
			 struct cbpoll_ctx *context,
			 int index,
			 int fd,
			 uint32_t refcnt);

};

struct cbpoll_fd {
  int       fd;
  int       events;
  void     *data;

  /*
    The idea is to increment it before sending it off to the
    other thread.  The other thread sends an event to the event
    pipe.  The event causes the reference count to decrease which
    may free the context.  This means that a slot may still be used
    even after being removed from the epoll fd.

    The helper thread must not try to send anything.  It will buffer
    the outgoing data without sending.  The return event will get the
    outgoing data flowing again or release the slot if an error occurred.

  */
  uint32_t  refcnt;
#define CBPOLLFD_FLAG_REMOVED 1
#define CBPOLLFD_FLAG_EVENTS_CHANGED 2
#define CBPOLLFD_FLAG_RESERVED 4
  uint32_t  flags;
  const struct cbpoll_fd_ops *ops;

  int32_t associated_context;
};

struct dspd_cbtimer;
typedef bool (*dspd_cbtimer_cb_t)(struct cbpoll_ctx *ctx, 
				  struct dspd_cbtimer *timer,
				  void *arg, 
				  dspd_time_t timeout);

struct dspd_cbtimer {
  dspd_cbtimer_cb_t callback;
  void *arg;
  struct cbpoll_ctx *cbpoll;
  dspd_time_t timeout, period;
  struct dspd_cbtimer *prev, *next;
};

struct dspd_aio_ctx;
struct cbpoll_ctx {
  int epfd;
  int event_pipe[2];
  void (*sleep)(void *arg, struct cbpoll_ctx *context);
  void (*wake)(void *arg, struct cbpoll_ctx *context);
  size_t max_fd;
  struct cbpoll_fd   *fdata;
  ssize_t             fdata_idx;
  struct epoll_event *events;
  volatile AO_t       abort;
  void *arg;
  struct cbpoll_wq    wq;
  dspd_thread_t thread;
  char *name;
  int32_t dispatch_count;

  struct dspd_timer timer;
  dspd_time_t  *pending_timers;
  dspd_time_t   next_timeout;
  bool          timeout_changed;
  size_t        timer_idx;

  struct dspd_aio_ctx **aio_list;
  size_t               aio_idx;
  struct dspd_aio_fifo_eventfd eventfd;
  bool wake_self;

  struct dspd_cbtimer *cbtimer_objects;
  struct dspd_cbtimer *pending_cbtimer_list;
  struct dspd_cbtimer **cbtimer_dispatch_list;
  dspd_mutex_t loop_lock;
  dspd_mutex_t work_lock;
  dspd_time_t last_time;

};

int32_t cbpoll_get_dispatch_list(struct cbpoll_ctx *ctx, int32_t **count, struct epoll_event **events);


uint32_t cbpoll_unref(struct cbpoll_ctx *ctx, int index);
uint32_t cbpoll_ref(struct cbpoll_ctx *ctx, int index);
uint32_t cbpoll_refcnt(struct cbpoll_ctx *ctx, int index);
int32_t cbpoll_send_event(struct cbpoll_ctx *ctx, const struct cbpoll_msg *evt);
void cbpoll_queue_work(struct cbpoll_ctx *ctx, const struct cbpoll_msg *wrk);
void cbpoll_reply_work(struct cbpoll_ctx *ctx,
		       const struct cbpoll_msg *prev,
		       struct cbpoll_msg *reply);
int32_t cbpoll_set_events(struct cbpoll_ctx *ctx, 
			  int32_t index,
			  int32_t events);
int32_t cbpoll_disable_events(struct cbpoll_ctx *ctx, 
			      int32_t index,
			      int32_t events);
int32_t cbpoll_enable_events(struct cbpoll_ctx *ctx, 
			     int32_t index,
			     int32_t events);

int32_t cbpoll_get_events(struct cbpoll_ctx *ctx, int32_t index);
int32_t cbpoll_add_fd(struct cbpoll_ctx *ctx, 
		      int32_t fd,
		      int32_t events,
		      const struct cbpoll_fd_ops *ops,
		      void *arg);

void cbpoll_queue_deferred_work(struct cbpoll_ctx *ctx,
				int32_t index,
				int64_t arg,
				void (*callback)(struct cbpoll_ctx *ctx,
						 struct cbpoll_msg *wrk,
						 void *data));
void cbpoll_deferred_work_complete(struct cbpoll_ctx *ctx,
				   int32_t index,
				   int64_t arg);
#define CBPOLL_FLAG_TIMER 1
#define CBPOLL_FLAG_AIO_FIFO 2
#define CBPOLL_FLAG_CBTIMER 4
int32_t cbpoll_init(struct cbpoll_ctx *ctx, 
		    int32_t  flags,
		    uint32_t max_fds);
int32_t cbpoll_set_name(struct cbpoll_ctx *ctx, const char *threadname);
int32_t cbpoll_start(struct cbpoll_ctx *ctx);
int32_t cbpoll_run(struct cbpoll_ctx *ctx);
void cbpoll_destroy(struct cbpoll_ctx *ctx);
void cbpoll_close_fd(struct cbpoll_ctx *ctx, int index);
void cbpoll_set_callbacks(struct cbpoll_ctx *ctx,
			  void *arg,
			  void (*sleep)(void *arg, struct cbpoll_ctx *context),
			  void (*wake)(void *arg, struct cbpoll_ctx *context));

struct cbpoll_fd *cbpoll_get_fdata(struct cbpoll_ctx *ctx, int32_t index);
void cbpoll_set_next_timeout(struct cbpoll_ctx *ctx, int32_t timeout);
void cbpoll_cancel_timer(struct cbpoll_ctx *ctx, size_t index);
void cbpoll_set_timer(struct cbpoll_ctx *ctx, size_t index, dspd_time_t timeout);

int32_t cbpoll_add_aio(struct cbpoll_ctx *context, struct dspd_aio_ctx *aio, int32_t associated_context);
void cbpoll_remove_aio(struct cbpoll_ctx *context, struct dspd_aio_ctx *aio);
int32_t cbpoll_aio_new(struct cbpoll_ctx *cbpoll, 
		       struct dspd_aio_ctx **aio,
		       const char *addr,
		       void *context,
		       void (*shutdown_cb)(struct dspd_aio_ctx *ctx, void *arg),
		       void *shutdown_arg);

dspd_time_t dspd_cbtimer_get_timeout(struct dspd_cbtimer *t);
void dspd_cbtimer_set(struct dspd_cbtimer *timer, dspd_time_t timeout, dspd_time_t period);
void dspd_cbtimer_delete(struct dspd_cbtimer *timer);
void dspd_cbtimer_cancel(struct dspd_cbtimer *timer);
struct dspd_cbtimer *dspd_cbtimer_new(struct cbpoll_ctx *ctx, 
				      dspd_cbtimer_cb_t callback,
				      void *arg);

void dspd_cbtimer_fire(struct dspd_cbtimer *timer);

//Client management for servers
//The general pattern is something like: get incoming client, shut off listening fd, accept in create(),
//then activate the listening fd again in success().

struct cbpoll_client_list;
struct cbpoll_client_hdr;
struct cbpoll_client_ops {
  //async client creation (usually called in work thread)
  struct cbpoll_client_hdr *(*create)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr, void *arg);
  //async client destruction (usually called in work thread)
  void (*destroy)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr);

  //Completion callbacks

  //client success (called in event loop thread).  return true to confirm.
  bool (*success)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr);
  //client fail (usually called in event loop thread)
  void (*fail)(struct cbpoll_ctx *ctx, struct cbpoll_client_list *list, int32_t cli, int32_t index, int32_t fd);

  void (*work_complete)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr);
  void (*do_work)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr);
};

//header for client struct (make this the first member)
struct cbpoll_client_hdr {
  int32_t fd;
  ssize_t list_index;
  ssize_t fd_index;
  ssize_t reserved_slot;
  struct cbpoll_client_list *list;
  const struct cbpoll_client_ops *ops;
  dspd_ts_t busy;
};

struct cbpoll_client_list {
  const struct cbpoll_client_ops *ops;
  struct cbpoll_client_hdr **clients;
  const struct cbpoll_fd_ops *fd_ops;
  size_t max_clients;
  void *data;
#define CBPOLL_CLIENT_LIST_LISTENFD 1
  uint32_t flags;
  
};

//accept a new fd
int32_t cbpoll_accept(struct cbpoll_ctx *ctx, 
		      struct cbpoll_client_list *list,
		      int32_t fd, //usually fd of listening socket or accepted socket
		      size_t index, //index of listening fd
		      int64_t arg); //arbitrary data

//generic destructor that runs asynchronously
bool cbpoll_async_destructor_cb(void *data,
				struct cbpoll_ctx *context,
				int index,
				int fd);
int32_t cbpoll_queue_client_work(struct cbpoll_ctx *ctx, size_t index);
void cbpoll_link(struct cbpoll_ctx *ctx, int index1, int index2);
void cbpoll_unlink(struct cbpoll_ctx *ctx, int index);
int cbpoll_listening_fd_event_cb(void *data, 
				 struct cbpoll_ctx *context,
				 int index,
				 int fd,
				 int revents);

#endif
