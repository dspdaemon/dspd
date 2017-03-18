#ifndef _DSPD_MODULES_H_
#define _DSPD_MODULES_H_
#include <stdint.h>
#include <pthread.h>
#define DSPD_MODULE_NAME_MAX 32
struct dspd_mod_cb {
  uint32_t    compat_version;
  uint32_t    mod_version;
  const char *desc;
  int (*init)(void *daemon, void **context);
  void (*close)(void *daemon, void **context);
  int (*ioctl)(void         *daemon, 
	       void         *context,
	       int32_t       req,
	       const void   *inbuf,
	       size_t        inbufsize,
	       void         *outbuf,
	       size_t        outbufsize,
	       size_t       *bytes_returned);
	       
};
struct dspd_ll;
struct dspd_module {
  const struct dspd_mod_cb *callbacks;
  void                     *dl_handle;
  char                     *file;
  char                     *name;
  void                     *context;
};

struct dspd_daemon_ctx;
struct dspd_module_list {
  pthread_rwlock_t  lock;
  uint32_t          count;
  struct dspd_ll   *modules;
  struct dspd_daemon_ctx *daemon_ctx;
  void             *lastinit;
};
void dspd_module_list_destroy(struct dspd_module_list *list);
int dspd_load_modules(struct dspd_module_list **l,
		      void *context,
		      const char **files,
		      const char **names,
		      size_t       count);

#endif
