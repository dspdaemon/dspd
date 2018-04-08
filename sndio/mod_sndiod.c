#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../sndio/dspd_sndio.h"
static int sndiod_init(void *daemon, void **context)
{
  struct sndio_ctx *ctx;
  struct dspd_sndio_params params;
  struct dspd_dict *cfg = dspd_read_config("sndiod", true);
  char *val;
  int32_t i;
  memset(&params, 0, sizeof(params));

  if ( cfg )
    {
      if ( dspd_dict_find_value(cfg, "unit", &val) )
	{
	  i = 0;
	  dspd_strtoi32(val, &i, 0);
	  params.unit_number = i;
	}
      if ( dspd_dict_find_value(cfg, "address", &val) )
	{
	  params.net_addrs = strdup(val);
	}
      if ( dspd_dict_find_value(cfg, "disable_unix_socket", &val) )
	{
	  i = 0;
	  dspd_strtoi32(val, &i, 0);
	  params.disable_unix_socket = i;
	}
      if ( dspd_dict_find_value(cfg, "systemwide", &val) )
	{
	  i = 0;
	  dspd_strtoi32(val, &i, 0);
	  params.system_server = i;
	}
      dspd_dict_free(cfg);
    }


  params.context = daemon;
  int ret = dspd_sndio_new(&ctx, &params);
  if ( ret == 0 )
    {
      ret = dspd_sndio_start(ctx);
      if ( ret == 0 )
	*context = ctx;
      else
	dspd_sndio_delete(ctx);
    }
  return 0;
}
static void sndiod_close(void *daemon, void **context)
{
  if ( *context )
    {
      dspd_sndio_delete(*context);
      *context = NULL;
    }
}



struct dspd_mod_cb dspd_mod_sndiod = {
  .init_priority = DSPD_MOD_INIT_PRIO_EXTSVC,
  .desc = "sndio server",
  .init = sndiod_init,
  .close = sndiod_close,
};
