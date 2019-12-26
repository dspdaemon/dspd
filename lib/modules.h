#ifndef _DSPD_MODULES_H_
#define _DSPD_MODULES_H_
#include <stdint.h>
#include <pthread.h>
#define DSPD_MODULE_NAME_MAX 32

//Priorities for module initialization.  Highest goes first.
#define DSPD_MOD_INIT_PRIO_STEP 10000U
#define _MAKEPRIO(_n) (DSPD_MOD_INIT_PRIO_STEP*_n)

//sample rate conversion
#define DSPD_MOD_INIT_PRIO_SRC     _MAKEPRIO(5U)
//internal services
#define DSPD_MOD_INIT_PRIO_INTSVC  _MAKEPRIO(4U)
//external services
#define DSPD_MOD_INIT_PRIO_EXTSVC  _MAKEPRIO(3U)
//hardware drivers
#define DSPD_MOD_INIT_PRIO_HWDRV   _MAKEPRIO(2U)
//hotplug event generators
#define DSPD_MOD_INIT_PRIO_HOTPLUG _MAKEPRIO(1U)

struct dspd_daemon_ctx;

struct dspd_mod_cb {
  uint32_t    compat_version;
  uint32_t    mod_version;
  //Initialization priority (highest first).  If two modules have the same
  //priority then they are loaded in the order they appear in the module list.
  uint32_t    init_priority;
  const char *desc;
  int (*init)(struct dspd_daemon_ctx *daemon, void **context);
  void (*close)(struct dspd_daemon_ctx *daemon, void **context);
	       
};
struct dspd_ll;
struct dspd_module {
  const struct dspd_mod_cb *callbacks;
  void                     *dl_handle;
  char                     *file;
  char                     *name;
  void                     *context;
};


struct dspd_module_list {
  pthread_rwlock_t  lock;
  uint32_t          count;
  struct dspd_ll   *modules;
  struct dspd_daemon_ctx *daemon_ctx;
  void             *lastinit;
};
void dspd_module_list_delete(struct dspd_module_list *list);
int dspd_load_modules(struct dspd_module_list **l,
		      void *context,
		      const char **files,
		      const char **names,
		      size_t       count);

#endif
