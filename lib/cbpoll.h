#ifndef _DSPD_CBPOLL_H_
#define _DSPD_CBPOLL_H_
#include <sys/epoll.h>
#include <stdint.h>
struct cbpoll_ctx;

#define CBPOLL_PIPE_MSG_DEFERRED_WORK 1
#define CBPOLL_PIPE_MSG_CALLBACK      2
#define CBPOLL_PIPE_MSG_USER          1000
struct cbpoll_pipe_event {
  int32_t fd;
  int32_t index;
  int32_t stream;
  int32_t msg;
  int64_t arg;
  void (*callback)(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *evt);
};

struct cbpoll_work {
  uint32_t index;
  int32_t  fd;
  int32_t  msg;
  int64_t  arg;
  void (*callback)(struct cbpoll_ctx *ctx,
		   void *data,
		   int64_t arg,
		   int32_t index,
		   int32_t fd,
		   int32_t msg,
		   bool async);
  char     extra_data[32];
};

struct cbpoll_wq {
  struct dspd_fifo_header  *fifo;
  dspd_cond_t               cond;
  dspd_mutex_t              lock;
  dspd_thread_t             thread;
  struct cbpoll_work        overflow;
  void                     *extra_data;
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
		      const struct cbpoll_pipe_event *event);
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
int32_t cbpoll_send_event(struct cbpoll_ctx *ctx, struct cbpoll_pipe_event *evt);
void cbpoll_queue_work(struct cbpoll_ctx *ctx, struct cbpoll_work *wrk);

int32_t cbpoll_set_events(struct cbpoll_ctx *ctx, 
			  int32_t index,
			  int32_t events);
int32_t cbpoll_get_events(struct cbpoll_ctx *ctx, int32_t index);
int32_t cbpoll_add_fd(struct cbpoll_ctx *ctx, 
		      int32_t fd,
		      int32_t events,
		      const struct cbpoll_fd_ops *ops,
		      void *arg);
void *cbpoll_get_extra_data(struct cbpoll_ctx *ctx);
void cbpoll_queue_deferred_work(struct cbpoll_ctx *ctx,
				int32_t index,
				int64_t arg,
				void (*callback)(struct cbpoll_ctx *ctx,
						 void *data,
						 int64_t arg,
						 int32_t index,
						 int32_t fd,
						 int32_t msg,
						 bool async));
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
  struct cbpoll_client_hdr *(*create)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr, int64_t arg);
  //async client destruction (usually called in work thread)
  void (*destroy)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr);

  //Completion callbacks

  //client success (called in event loop thread)
  bool (*success)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr);
  //client fail (usually called in event loop thread)
  void (*fail)(struct cbpoll_ctx *ctx, struct cbpoll_client_list *list, int32_t cli, int32_t index, int32_t fd);

  void (*work_complete)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr);
  void (*do_work)(struct cbpoll_ctx *ctx, struct cbpoll_client_hdr *hdr);
};

//header for client struct (make this the first member)
struct cbpoll_client_hdr {
  int32_t fd;
  ssize_t index;
  ssize_t fd_index;
  struct cbpoll_client_list *list;
  struct cbpoll_client_ops *ops;
  dspd_ts_t busy;
};

struct cbpoll_client_list {
  struct cbpoll_client_ops *ops;
  struct cbpoll_client_hdr **clients;
  size_t max_clients;
  void *data;
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

#endif
