#ifndef _SS_EVENTQ_H_
#define _SS_EVENTQ_H_


struct socksrv_ctl_eq {
  size_t in;
  size_t out;
  size_t max_events;
  size_t min_events;
  size_t event_count;
  struct socksrv_ctl_event *events; 
};

ssize_t socksrv_eq_realloc(struct socksrv_ctl_eq *eq, size_t min_events, size_t max_events, size_t curr_events);
bool socksrv_eq_push(struct socksrv_ctl_eq *eq, 
		     const struct socksrv_ctl_event *evt);
bool socksrv_eq_pop(struct socksrv_ctl_eq *eq, 
		    struct socksrv_ctl_event *evt);
void socksrv_eq_reset(struct socksrv_ctl_eq *eq);
#endif /*_SS_EVENTQ_H*/
