#include <stdlib.h>
#include <string.h>
#include "../lib/sslib.h"
static int print_usage(const char *name)
{
  fprintf(stderr, "Usage: %s [-i INTERVAL] [-a ADDRESS]\n"
	  "-i     Poll interval in seconds (default is 5)\n"
	  "       Run once and exit if the interval is 0\n"
	  "-a     Server address\n",
	  name);
  return 1;
}

int main(int argc, char **argv)
{
  int err;
  struct dspd_conn *conn;
  unsigned char *mask = NULL;
  size_t i;
  size_t br, bits;
  uint32_t mask_size, mtype = DSPD_DCTL_ENUM_TYPE_SERVER, dev;
  struct dspd_device_stat devinfo;
  uint32_t us1, us2;
  const char *addr = NULL;
  int interval = 5;
  int nextarg;
  for ( i = 1; i < (size_t)argc; i++ )
    {
      nextarg = i + 1;
      if ( strcmp(argv[i], "-?") == 0 ||
	   strcmp(argv[i], "--help") == 0 ||
	   strcmp(argv[i], "-h") == 0 )
	{
	  return print_usage(argv[0]);
	} else if ( strcmp(argv[i], "-a") == 0 )
	{
	  if ( nextarg != argc )
	    addr = argv[nextarg];
	  else
	    return print_usage(argv[0]);
	} else if ( strcmp(argv[i], "-i") == 0 )
	{
	  if ( nextarg != argc )
	    interval = atoi(argv[nextarg]);
	  else
	    return print_usage(argv[0]);
	}
    }


  dspd_time_init();
  err = dspd_conn_new(addr, &conn);
  if ( err )
    {
      fprintf(stderr, "Could not connect to server (%s)\n", addr);
      goto out;
    }
  while (1)
    {
      err = dspd_stream_ctl(conn,
			    0,
			    DSPD_DCTL_GET_OBJMASK_SIZE,
			    NULL,
			    0,
			    &mask_size,
			    sizeof(mask_size),
			    &br);
      if ( err )
	goto out;
      mask = realloc(mask, mask_size);
      if ( ! mask )
	{
	  err = errno;
	  goto out;
	}
      memset(mask, 0, mask_size);
      err = dspd_stream_ctl(conn,
			    0,
			    DSPD_DCTL_ENUMERATE_OBJECTS,
			    &mtype,
			    sizeof(mtype),
			    mask,
			    mask_size,
			    &br);
      if ( err )
	goto out;


      bits = mask_size * 8;
      for ( i = 0; i < bits; i++ )
	{
	  if ( dspd_test_bit(mask, i) )
	    {
	      dev = i;
	      err = dspd_stream_ctl(conn,
				    -1,
				    DSPD_SOCKSRV_REQ_REFSRV,
				    &dev,
				    sizeof(dev),
				    &devinfo,
				    sizeof(devinfo),
				    &br);
	      if ( err == 0 && br == sizeof(devinfo) )
		{
		  if ( devinfo.playback.latency )
		    us1 = (1000000000 / devinfo.playback.rate) * devinfo.playback.latency;
		  else
		    us1 = 0;
		  if ( devinfo.capture.latency )
		    us2 = (1000000000 / devinfo.capture.rate) * devinfo.capture.latency;
		  else
		    us2 = 0;
		  us1 /= 1000;
		  us2 /= 1000;
		  fprintf(stderr, "DEVICE[%d]: NAME=%s STREAMS=0x%x PLAYBACK=%uus CAPTURE=%uus REFCOUNT=%u EID=%llu\n",
			  dev, 
			  devinfo.name,
			  devinfo.streams,
			  us1,
			  us2,
			  devinfo.refcount,
			  (long long)devinfo.hotplug_event_id);
			  
		}
	    }
	}
      if ( interval == 0 )
	break;
      sleep(interval);
    }

 out:
  if ( err )
    {
      fprintf(stderr, "Error %d\n", err);
      return 1;
    }
  return 0;
}
