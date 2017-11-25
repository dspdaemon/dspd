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
#endif
