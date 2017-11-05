#ifndef _DSPD_SSCLIENT_H_
#define _DSPD_SSCLIENT_H_
#ifndef EUNKNOWN
#define EUNKNOWN 1143426888
#endif
#define DSPD_POLLERR (POLLNVAL|POLLHUP|POLLRDHUP|POLLERR)
#define SS_MAX_PAYLOAD 4096

struct dspd_conn {
  uint32_t magic;
  struct dspd_req header_in;
  struct dspd_req header_out;

  char   data_in[SS_MAX_PAYLOAD];
  size_t offset_in;
  bool   async_input_pending;
  dspd_mutex_t lock;
  int sock_fd;
  int fd_in;
  int timeout;
  struct pollfd pfd;
  int revents;
  
  uint32_t event_flags;
  void (*event_flags_changed)(void *arg, uint32_t *flags);
  void *arg;

  bool event_processed;

};

enum dspd_ssctl_req {
  DSPD_SOCKSRV_REQ_QUIT = 1,
  DSPD_SOCKSRV_REQ_NEWCLI,
  DSPD_SOCKSRV_REQ_DELCLI,
  DSPD_SOCKSRV_REQ_REFSRV,
  DSPD_SOCKSRV_REQ_UNREFSRV,
  DSPD_SOCKSRV_REQ_NMCLI,
  DSPD_SOCKSRV_REQ_DMCLI,
  DSPD_SOCKSRV_REQ_RMSRV,
  DSPD_SOCKSRV_REQ_UMSRV,
 };

int32_t dspd_conn_recv_fd(struct dspd_conn *conn);

int dspd_conn_new(const char *addr, struct dspd_conn **ptr);
void dspd_conn_delete(struct dspd_conn *conn);

uint32_t dspd_conn_revents(struct dspd_conn *conn);


int dspd_conn_ctl(struct dspd_conn *conn,
		  uint32_t stream,
		  uint32_t req,
		  const void          *inbuf,
		  size_t        inbufsize,
		  void         *outbuf,
		  size_t        outbufsize,
		  size_t       *bytes_returned);

int dspd_ipc_process_messages(struct dspd_conn *ssc, int timeout);

void dspd_conn_set_event_flag_cb(struct dspd_conn *conn, 
				 void (*event_flags_changed)(void *arg, uint32_t *flags),
				 void *arg);
uint32_t dspd_conn_get_event_flags(struct dspd_conn *conn, bool clear);

#endif
