#ifndef _DSPD_SCHEDULER_H_
#define _DSPD_SCHEDULER_H_
#include <stdint.h>
#include <sys/epoll.h>
#include <atomic_ops.h>
typedef void (*dspd_sch_callback_t)(void *udata, int32_t fd, void *fdata, uint32_t events);

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

struct dspd_scheduler_ops {
  void (*loop_started)(void *user_data);

  //Optional: call this when it wakes up due to the timer expiring.
  void (*timer_event)(void *user_data);

  //Optional: call this when the eventfd is set
  void (*trigger_event)(void *user_data);

  //This is called on each loop iteration after fd callbacks  and timer_event 
  void (*wake)(void *user_data);
  //This is called before sleeping.  If *reltime is set to -2 then the timer
  //is disabled.  If *reltime is 0 then it will poll without sleeping.  If *reltime > 0
  //then the results are undefined.  The current implementation passes the value to epoll().
  bool (*sleep)(void *user_data, uint64_t *abstime, int32_t *reltime);
  //This is called when aborting.
  void (*abort)(void *user_data, int error);
};

struct dspd_scheduler {
  int                   abort;
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
};

//Create a new scheduler
struct dspd_scheduler *dspd_scheduler_new(const struct dspd_scheduler_ops *ops, void *udata, int32_t maxfds);
//Free the scheduler
void dspd_scheduler_delete(struct dspd_scheduler *sch);
//Add a file descriptor.
int dspd_scheduler_add_fd(struct dspd_scheduler *sch, int32_t fd, int32_t events, void *data, dspd_sch_callback_t cb);
//The the schedule loop.  Does not exit until the loop aborts.
void *dspd_scheduler_run(void *arg);

//Make the scheduler abort at the end of the loop.
void dspd_scheduler_abort(struct dspd_scheduler *sch);
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
#endif
