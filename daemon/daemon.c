#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"



static int mainthread_loop(int argc,
			   char **argv,
			   struct dspd_daemon_ctx *ctx)
{
  while (1)
    {
      if ( ! dspd_wq_process(ctx->wq) )
	break;
    }
  return 0;
}

int main(int argc, char **argv)
{
  struct dspd_dict *sect;
  char **names, **files;
  size_t i;
  int ret = 0;
  signal(SIGPIPE, SIG_IGN);
  if ( (ret = dspd_daemon_init(argc, argv)) < 0 )
    {
      fprintf(stderr, "Error %d while initializing daemon\n", ret);
      return 1;
    }
  dspd_time_init();

  //sect = dict_read(fp);

  sect = dspd_dctx.config;

  struct dspd_dict *modules = dspd_dict_find_section(sect, "MODULES");

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
    {
      dspd_daemon_register_mainthread_loop(mainthread_loop);
      assert(dspd_dctx.objects != NULL);
      dspd_daemon_run();
    }
  return ret == 0;
}
