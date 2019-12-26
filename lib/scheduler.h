#ifndef _DSPD_SCHEDULER_H_
#define _DSPD_SCHEDULER_H_
#include <stdint.h>
#include <sys/epoll.h>
#include <atomic_ops.h>
#include <sched.h>
#include <pthread.h>
#include <setjmp.h>
#include "thread.h"
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#define _USE_DSPD_SCHED_DEADLINE
#endif
struct dspd_scheduler;
typedef void (*dspd_sch_callback_t)(void *udata, int32_t fd, void *fdata, uint32_t events);
typedef void (*dspd_sch_work_t)(struct dspd_scheduler *sch, void *arg, uint64_t data);
//Stop timer
#define DSPD_SCHED_STOP -2
//Wait for FD events
#define DSPD_SCHED_WAIT -1
//Poll without sleeping
#define DSPD_SCHED_SPIN  0

struct dspd_scheduler_fd {
  int32_t  fd;
  void    *ptr;
  dspd_sch_callback_t callback;
};

struct dspd_scheduler_work {
  dspd_sch_work_t        callback;
  void                  *arg;
  uint64_t               data;
  struct dspd_scheduler *context;
};

struct dspd_scheduler_ops {
  void (*started)(void *user_data);

  //Optional: call this when it wakes up due to the timer expiring.
  void (*timer_event)(void *user_data);

  //Optional: call this when the eventfd is set
  void (*trigger_event)(void *user_data);

  //This is called on each loop iteration after fd callbacks  and timer_event 
  void (*wake)(void *user_data);

  //This is called before sleeping.  If *reltime is set to -2 then the timer
  //is disabled.  If *reltime is 0 then it will poll without sleeping.  If *reltime > 0
  //then the results are undefined.  The current implementation passes the value to epoll().
  //The deadline argument is relative to any other slave schedulers.  As long as they all
  //agree, it may be either the absolute latest time to run, or a relative timeout or a priority.
  bool (*sleep)(void *user_data, uint64_t *abstime, uint64_t *deadline, int32_t *reltime);

  //This is called when aborting.
  void (*abort)(void *user_data, int error);

  //Sigbus handler to be called in a normal thread context (not in the signal handler)
  void (*sigbus)(void *user_data);

  //This is the place to safely delete the scheduler or unlink a slave scheduler
  void (*destructor)(struct dspd_scheduler *sch, void *user_data);

  void (*setlatency)(void *user_data, uint64_t latency);
};

struct dspd_scheduler {
  volatile AO_t         abort;
  int                   epfd;
  int                   eventfd;
  volatile AO_TS_t      eventfd_triggered;
  int                   nfds, maxfds;
  int                   timerfd;
  struct dspd_scheduler_fd        *fds;
  struct epoll_event              *evts;
  const struct dspd_scheduler_ops *ops;
  void                            *udata;
  int32_t                          avail_min, buffer_time;
  bool                             have_sched_deadline;
  int                              tid;
  int32_t                          timebase;


  struct dspd_dtimer              *dtimer, *slave_dispatch;
  
#define DSPD_SCHED_TIMER     1
#define DSPD_SCHED_EVENTFD   2
#define DSPD_SCHED_LOOPSTART 4
#define DSPD_SCHED_RETRY     8
#define DSPD_SCHED_FDEVENT   16
  int32_t                          activate_flags;
  struct dspd_dtimer_event         timer_event, slave_event;
  size_t                           injected_events;
  int32_t                          reltime;
  struct dspd_scheduler           *master;
  size_t                           timerfd_index;
  size_t                           eventfd_index;
  int32_t                          slave_index;

  volatile struct dspd_scheduler **slaves;
  size_t                           nslaves;
  size_t                           max_slaves;
#define DSPD_SCHED_INVALID_SLAVE (AO_t)-1L
  volatile AO_t                    dispatch_slave;
  volatile AO_t                    current_exception;
  sigjmp_buf                       sigbus_env, sigbus_except;

  //This scheduler is a master
#define DSPD_SCHED_MASTER 1
  //This scheduler is a slave
#define DSPD_SCHED_SLAVE  2
  //Enable work queue with helper thread
#define DSPD_SCHED_WORKQ  4
  //Enable sigbus handler
#define DSPD_SCHED_SIGBUS 8
  //Enable SIGXCPU handling.
  //This keeps runaway RT threads from locking up the system.
#define DSPD_SCHED_SIGXCPU 16
  //Use SCHED_DEADLINE
#define DSPD_SCHED_SCHEDDL 32
  int32_t                          flags;

  //The work queue runs in a separate thread.  Sometimes results may need returned, so
  //another work queue of the same capacity is available for returning results to the 
  //scheduler thread.
  struct dspd_fifo_header         *workq, *retq; 
  dspd_cond_t                      workq_event;
  dspd_mutex_t                     workq_lock;
  dspd_thread_t                    workq_thread;
  dspd_ts_t                        workq_tsval;

  //Control queue for sending slave schedulers to the master.  It might be used for
  //something else in the future.
  dspd_mutex_t                     control_lock;
  struct dspd_fifo_header         *controlq;

  volatile bool                    sigxcpu_handled;
  int32_t                          sched_policy;
  struct sched_param               sched_param;
  bool                             dead;
  char                            *thread_name;

  dspd_time_t                      latency;

  dspd_time_t                      dl_latency;
};

struct dspd_sched_params {
  int32_t     flags;
  int32_t     nslaves;
  int32_t     nfds;
  uint32_t    nmsgs;
  const char *thread_name;
};

//Create a new scheduler
struct dspd_scheduler *dspd_sched_new(const struct dspd_scheduler_ops *ops, 
				      void *udata, 
				      const struct dspd_sched_params *params);
//Free the scheduler
void dspd_sched_delete(struct dspd_scheduler *sch);
//Add a file descriptor.
int dspd_sched_add_fd(struct dspd_scheduler *sch, int32_t fd, int32_t events, void *data, dspd_sch_callback_t cb);
int dspd_sched_set_fd_event(struct dspd_scheduler *sch, 
			    int32_t fd,
			    int32_t events);

void dspd_sched_remove_fd(struct dspd_scheduler *sch, int32_t fd);

int32_t dspd_sched_add_slave(struct dspd_scheduler *master, struct dspd_scheduler *slave);
void dspd_sched_remove_slave(struct dspd_scheduler *slave);

//The the schedule loop.  Does not exit until the loop aborts.
void *dspd_sched_run(void *arg);

//Make the scheduler abort at the end of the loop.
void dspd_sched_abort(struct dspd_scheduler *sch);
void dspd_sched_trigger(struct dspd_scheduler *sch);

int32_t dspd_sched_set_deadline_hint(struct dspd_scheduler *sch, 
				     int32_t avail_min,
				     int32_t buffer_time);
void dspd_sched_get_deadline_hint(struct dspd_scheduler *sch,
				  int32_t *avail_min,
				  int32_t *buffer_time);

bool dspd_sched_enable_deadline(struct dspd_scheduler *sch);
bool dspd_sched_deadline_init(struct dspd_scheduler *sch);
void dspd_sched_set_timebase(struct dspd_scheduler *sched, int32_t t);

int32_t dspd_sched_send_slave(struct dspd_scheduler *master, struct dspd_scheduler *slave);
bool dspd_sched_return(struct dspd_scheduler *sch, 
		       dspd_sch_work_t callback,
		       void *arg,
		       uint64_t data);
bool dspd_sched_queue_work(struct dspd_scheduler *sch, 
			   dspd_sch_work_t callback,
			   void *arg,
			   uint64_t data);

struct dspd_scheduler *dspd_sched_get(void);
void dspd_sched_stop_workq(struct dspd_scheduler *sch);

void dspd_sched_set_latency(struct dspd_scheduler *sched, dspd_time_t latency);




#endif
