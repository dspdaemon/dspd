#ifndef _DSPD_WQ_H_
#define _DSPD_WQ_H_
#include <limits.h>
struct dspd_wq_item {
  uint16_t len;
  bool (*callback)(void *arg, void *data, size_t len);
  void *arg;
};

#define DSPD_WQ_ITEM_MAX_DATA (PIPE_BUF-sizeof(struct dspd_wq_item))

struct dspd_wq_buf {
  struct dspd_wq_item item;
  char data[DSPD_WQ_ITEM_MAX_DATA];
};

struct dspd_wq {
  int pipe[2];
  struct dspd_wq_buf buf;
};
int dspd_wq_new(struct dspd_wq **wq);

void dspd_wq_delete(struct dspd_wq *wq);
bool dspd_wq_process(struct dspd_wq *wq);
bool dspd_queue_work(struct dspd_wq *wq, const struct dspd_wq_item *item);
#endif
