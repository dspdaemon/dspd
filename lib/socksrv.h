
struct dspd_socket_server {
  int epfd;
};


struct dspd_sockclient_cb {
  int32_t (*connected)(int32_t fd, 
		       int32_t index,
		       int32_t domain,
		       int32_t type,
		       int32_t protocol,
		       struct dspd_socket_server *server, 
		       void **context);
  int32_t (*read)(int32_t fd, 
		  struct dspd_socket_server *server,
		  void *context);
  int32_t (*write)(int32_t fd, 
		   struct dspd_socket_server *server,
		   void *context);
  int32_t (*error)(int32_t fd, 
		   struct dspd_socket_server *server,
		   void *context);
  void (*destructor)(int32_t fd, 
		     struct dspd_socket_server *server,
		     void *arg);
};

struct dspd_sockclient {
  int32_t fd;
  void   *context;
  const struct dspd_sockclient_cb *cb;
};

//Listen on an address
struct dspd_socket_server *dspd_socksrv_new(const char *addr, const struct dspd_sockclient_cb *cb);
