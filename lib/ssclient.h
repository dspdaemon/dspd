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
  DSPD_SOCKSRV_REQ_SETSRV,
  DSPD_SOCKSRV_REQ_ALLOCZ,
  DSPD_SOCKSRV_REQ_FREE,
  DSPD_SOCKSRV_REQ_ECHO,
  DSPD_SOCKSRV_REQ_EVENT,
  DSPD_SOCKSRV_REQ_DEFAULTDEV,
  DSPD_SOCKSRV_REQ_CTLADDRMODE,
#define DSPD_SOCKSRV_CTLADDR_RAW    0
#define DSPD_SOCKSRV_CTLADDR_SIMPLE 1
 };

struct socksrv_ctl_event {
  int32_t  card;
#define SS_DEV_ADD    -1
#define SS_DEV_REMOVE -2
  int32_t  elem;
  uint32_t mask;
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

#define SELECT_DEV_OK     1
#define SELECT_DEV_REJECT 0
#define SELECT_DEV_ABORT -1
#define SELECT_DEV_OK_ABORT -2
int32_t dspd_select_device(struct dspd_conn *ssc, 
			   int32_t streams,
			   int32_t (*select_device)(void *arg, int32_t streams, int32_t index, const struct dspd_device_stat *info),
			   void *arg);
			   
			   
			   
int dspd_conn_get_socket(struct dspd_conn *conn);
#endif
