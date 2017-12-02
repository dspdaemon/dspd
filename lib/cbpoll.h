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
		   int32_t fd);
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
};

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
};

int32_t cbpoll_get_dispatch_list(struct cbpoll_ctx *ctx, int32_t **count, struct epoll_event **events);


void cbpoll_unref(struct cbpoll_ctx *ctx, int index);
void cbpoll_ref(struct cbpoll_ctx *ctx, int index);
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
						 int32_t fd));
void cbpoll_deferred_work_complete(struct cbpoll_ctx *ctx,
				   int32_t index,
				   int64_t arg);
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

#endif
