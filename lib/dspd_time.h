#ifndef _DSPD_TIME_H_
#define _DSPD_TIME_H_
#include <poll.h>
#include <stdint.h>


typedef uint64_t dspd_time_t;

int dspd_time_init(void);
time_t dspd_get_clockres(void);
time_t dspd_get_clockid(void);
time_t dspd_get_tick(void);
dspd_time_t dspd_get_time(void);
dspd_time_t dspd_timespec_to_dspdtime(struct timespec *ts);
void dspd_time_to_timespec(dspd_time_t t, struct timespec *ts);
int dspd_sleep(dspd_time_t abstime, dspd_time_t *waketime);

struct dspd_timer {
  int      fd;
  uint64_t oneshot_next;
  uint32_t interval;
  bool     latched;
  bool     unlatch;
  bool     trigger;
};
#define DSPD_EVENT_DISABLE ((dspd_time_t*)UINTPTR_MAX)
int dspd_timer_init(struct dspd_timer *tmr);
void dspd_timer_destroy(struct dspd_timer *tmr);

int dspd_timer_new(struct dspd_timer **tmr);
void dspd_timer_delete(struct dspd_timer *tmr);

int dspd_timer_set(struct dspd_timer *tmr, uint64_t abstime, uint32_t per);
int dspd_timer_getexp(struct dspd_timer *tmr, uint64_t *exp);
int dspd_timer_getpollfd(struct dspd_timer *tmr, struct pollfd *pfd);
void dspd_timer_destroy(struct dspd_timer *tmr);
int dspd_timer_fire(struct dspd_timer *tmr, bool latch);
int dspd_timer_ack(struct dspd_timer *tmr);
int dspd_timer_get(struct dspd_timer *tmr, dspd_time_t *abstime, uint32_t *per);
#define dspd_timer_reset(_t) dspd_timer_set(_t, 0, 0)

struct dspd_intrp {
  uint64_t last_tstamp;
  int64_t  sample_time;
  int64_t  diff;
  int64_t  maxdiff;
  bool     have_tstamp;
};

void dspd_intrp_reset(struct dspd_intrp *i);
void dspd_intrp_reset2(struct dspd_intrp *i, uint32_t rate);
void dspd_intrp_update(struct dspd_intrp *i, 
		       dspd_time_t tstamp, 
		       dspd_time_t diff);

int64_t dspd_intrp_set(struct dspd_intrp *i, 
		       dspd_time_t tstamp, 
		       dspd_time_t ptr);

dspd_time_t dspd_intrp_frames(struct dspd_intrp *i, int64_t frames);
dspd_time_t dspd_intrp_time(struct dspd_intrp *i, dspd_time_t time);
uint64_t dspd_intrp_used(struct dspd_intrp *i, dspd_time_t time);

struct dspd_dtimer;
struct dspd_dtimer_event;
typedef void (*dspd_dtimer_cb_t)(struct dspd_dtimer *timer, struct dspd_dtimer_event *event);
struct dspd_dtimer_event {
  //Callback for when timer expires.  It is safe to free the event or insert it again.
  //Inserted events will run during the next timer tick at the earliest.
  dspd_dtimer_cb_t callback;
  void *user_data;
  struct dspd_dtimer *timer;
  dspd_time_t timeout;   //Time when timer should fire
  dspd_time_t deadline;  //Latest time when timer should fire (set to timeout if not sure)
                         //This is more of a priority than a real deadline.
  uint64_t    tag;
  struct dspd_dtimer_event *prev, *next;
};

struct dspd_dtimer {
  struct dspd_dtimer_event *pending;
  struct dspd_dtimer_event *added;
  dspd_time_t                 timeout;
  dspd_time_t                 now;
  bool                        dispatch;
};

void dspd_dtimer_remove(struct dspd_dtimer_event *evt);
void dspd_dtimer_dispatch(struct dspd_dtimer *timer);
void dspd_dtimer_insert(struct dspd_dtimer *timer, struct dspd_dtimer_event *evt);
int32_t dspd_dtimer_fire(struct dspd_dtimer_event *evt);
bool dspd_dtimer_set_time(struct dspd_dtimer *timer, dspd_time_t now);
int32_t dspd_dtimer_new(struct dspd_dtimer **tmr, dspd_time_t now);
void dspd_dtimer_delete(struct dspd_dtimer *tmr);

void dspd_dtimer_remove_tag(struct dspd_dtimer *tmr, uint64_t tag);


#endif
