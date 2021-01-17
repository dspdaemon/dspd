#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "amsg.h"
#include "dspd_sndio.h"
#include "../lib/sslib.h"
int print_usage(const char *self)
{
  fprintf(stderr, "Usage: %s [-U unit] [-L net_addrs] [-D dspd_opts] [-d] [-?]\n"
	  "-U   Unit number.  Default listen port is %d+unit.\n"
	  "-L   Network addresses: [ipv6]:port,[ipv6],ipv4:port,ipv4,...\n"
	  "-d   Stay in forgeground for debugging.\n"
	  "-D   DSPD options (disable_unix_socket,systemwide_server,server_address)\n"
	  "     server_address:      /path/to/dspd.sock,default\n"
	  "     systemwide_server:   1=true,0=false,default\n"
	  "     disable_unix_socket: 1=true,0=false,default\n"
	  "-?   Print help\n", self, AUCAT_PORT);
  return 1;
}

bool parse_dspd_args(char *str, struct dspd_sndio_params *params)
{
  char *tok, *saveptr;
  size_t i = 0;
  int32_t val;
  for ( tok = strtok_r(str, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr) )
    {
      if ( i > 2 )
	{
	  fprintf(stderr, "-D requires 1-3 arguments\n");
	  return false;
	}

      if ( strcmp(tok, "default") != 0 )
	{
	  switch(i)
	    {
	    case 0:
	      free((void*)params->server_addr);
	      params->server_addr = strdup(tok);
	      if ( ! params->server_addr )
		{
		  perror("strdup");
		  exit(1);
		}
	      break;
	    case 1:
	      if ( dspd_strtoi32(tok, &val, 0) < 0 )
		return false;
	      params->system_server = !!val;
	      break;
	    case 2:
	      if ( dspd_strtoi32(tok, &val, 0) < 0 )
		return false;
	      params->disable_unix_socket = !!val;
	      break;
	    }
	}
      i++;
    }

  return true;
}

int main(int argc, char *argv[])
{
  int c;
  struct dspd_sndio_params params;
  bool debug = false;
  int32_t val;
  char *tmp = NULL;
  int32_t ret = 0;
  struct sndio_ctx *server;
  memset(&params, 0, sizeof(params));
  
  while ((c = getopt(argc, argv, "U:L:D:de:?e:")) != -1) 
    {
      switch(c)
	{
	  
	case 'U': //Unit
	  if ( dspd_strtoi32(optarg, &val, 0) < 0 )
	    {
	      fprintf(stderr, "Option 'U' requires an integer argument\n");
	      ret = 1;
	      goto out;
	    }
	  params.unit_number = val;
	  break;
	case 'L': //Net addrs
	  if ( params.net_addrs )
	    {
	      fprintf(stderr, "Option 'L' should only be used once\n");
	      ret = 1;
	      goto out;
	    }
	  free((void*)params.net_addrs);
	  params.net_addrs = strdup(optarg);
	  break;
	case 'D': //DSPD opts (server addr, disable unix, sys_server)
	  
	  free(tmp);
	  tmp = strdup(optarg);
	  if ( ! tmp )
	    {
	      fprintf(stderr, "Could not allocate memory.\n");
	      ret = 1;
	      goto out;
	    }
	  if ( ! parse_dspd_args(tmp, &params) )
	    {
	      ret = print_usage(argv[0]);
	      goto out;
	    }
	  break;
	case 'd': //Debug
	  debug = true;
	  break;
	case '?':
	  ret = print_usage(argv[0]);
	  goto out;
	  break;
	}
    }
  

  ret = dspd_sndio_new(&server, &params);
  
  if ( ret == 0 )
    {
      if ( ! debug )
	{
	  ret = daemon(0, 0);
	  if ( ret < 0 )
	    {
	      perror("daemon");
	      goto out;
	    }
	}
      ret = dspd_sndio_run(server);
    }
  if ( ret != 0 )
    fprintf(stderr, "Could not start server: error %d\n", ret);
 out:
  free((void*)params.net_addrs);
  free((void*)params.server_addr);
  free(tmp);
  return !!ret;
}
