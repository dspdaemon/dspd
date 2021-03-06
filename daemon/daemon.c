#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"


static void sighup_handler(int signal, siginfo_t *info, void *context)
{
  dspd_daemon_abort(&dspd_dctx);
}



int main(int argc, char **argv)
{
  char **names, **files;
  size_t i;
  int ret = 0;
  char *buf, *p;
  struct sigaction act = {
    .sa_sigaction = sighup_handler,
  };
  //Find the absolute path so module path lookups relative to the executable location
  //will work.
  if ( argv[0][0] != '/' )
    {
      buf = calloc(1, PATH_MAX+1UL);
      assert(buf != NULL);
      if ( buf )
	{
	  if ( readlink("/proc/self/exe", buf, PATH_MAX) > 0 )
	    {
	      p = strdup(buf);
	      assert(p != NULL);
	      if ( p )
		argv[0] = p;
	    }
	  free(buf);
	}
    }
  
  signal(SIGPIPE, SIG_IGN);
  if ( (ret = dspd_daemon_init(&dspd_dctx, argc, argv)) < 0 )
    {
      fprintf(stderr, "Error %d while initializing daemon\n", ret);
      return 1;
    }
  sigaction(SIGHUP, &act, NULL);
  dspd_time_init();
  dspd_enable_assert_log();

  struct dspd_dict *modules = dspd_dict_find_section(dspd_dctx.config, "MODULES");

  if ( modules )
    {
      files = calloc(modules->count, sizeof(char*));
      names = calloc(modules->count, sizeof(char*));
      for ( i = 0; i < modules->count; i++ )
	{
	  names[i] = modules->list[i].key;
	  files[i] = modules->list[i].value;
	}
      ret = dspd_load_modules(&dspd_dctx.modules, 
			      &dspd_dctx, 
			      (const char**)files,
			      (const char**)names,
			      modules->count);
      
    }
  if ( ret == 0 )
    dspd_daemon_run();
  dspd_daemon_destroy(&dspd_dctx);
  return ret == 0;
}
