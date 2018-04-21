#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <assert.h>
#define _DSPD_HAVE_UCRED
#include "sslib.h"
#define err_exit(err) {fprintf(stderr, "Error %d %s:%d\n", err, __FUNCTION__, __LINE__); exit(1);}
#define FILENAME "/dev/zero"


void test_playback(struct dspd_pcmcli *cli, const struct dspd_cli_params *params, FILE *fp, size_t duration)
{
  float buf[512];
  size_t len, offset;
  ssize_t wr;
  bool started = false;
  int32_t ret;
  size_t xfer = 0;
  struct dspd_pcmcli_status status;
  uint32_t min_fill = UINT32_MAX, max_fill = 0, min_delay = UINT32_MAX, max_delay = 0, fill, avgfill = 0, avgdelay = 0, count = 0;
  while ( (len = fread(buf, sizeof(float) * params->channels, 256, fp)) && xfer < duration )
    {
      offset = 0;
      while ( offset < len )
	{
	  wr = dspd_pcmcli_write_frames(cli,
					  &buf[offset * params->channels],
					  len - offset);
	  if ( wr == -EAGAIN && started == false )
	    {
	      ret = dspd_pcmcli_start(cli, DSPD_PCM_SBIT_PLAYBACK, NULL, NULL);
	      if ( ret < 0 )
		err_exit(ret);
	      wr = 0;
	      started = true;
	    }
	  if ( wr < 0 && wr != -EAGAIN )
	    err_exit((int)wr);
	  if ( wr > 0 )
	    {
	      offset += wr;
	      xfer += wr;
	    }
	  
	  ret = dspd_pcmcli_get_status(cli, DSPD_PCM_SBIT_PLAYBACK, true, &status);
	  if ( ret < 0 )
	    break;
	  if ( started )
	    {
	      fill = status.appl_ptr - status.hw_ptr;
	      if ( min_delay > status.delay )
		min_delay = status.delay;
	      if ( max_delay < status.delay )
		max_delay = status.delay;
	      if ( min_fill > fill )
		min_fill = fill;
	      if ( max_fill < fill )
		max_fill = fill;
	     
	      avgfill += fill;
	      avgdelay += status.delay;
	      count++;
	    }
	}
    }
  if ( count )
    {
      avgdelay /= count;
      avgfill /= count;
    }
  printf("TEST: duration=%lu min_delay=%u max_delay=%u min_fill=%u max_fill=%u avgfill=%u avgdelay=%u\n",
	 (long)duration,
	 min_delay, 
	 max_delay,
	 min_fill,
	 max_fill,
	 avgfill,
	 avgdelay);
  assert(min_delay > 0);
  assert(max_delay > 0);
  assert(min_fill > 0);
  assert(max_fill > 0);
  assert(max_delay <= (params->bufsize*2));
  assert(max_fill <= params->bufsize);

}

void set_info_complete(void *context, struct dspd_async_op *op)
{
  bool *done = op->data;
  *done = true;
  if ( op->error )
    err_exit(op->error);
}

int main(int argc, char **argv)
{
  int32_t ret;
  struct dspd_pcmcli *cli = NULL;
  struct dspd_cli_params params = {
    .format = DSPD_PCM_FORMAT_FLOAT_LE,
    .channels = 2,
    .rate = 48000,
    .bufsize = 1024,
    .fragsize = 256,
    .latency = 256,
    .flags = DSPD_CLI_FLAG_SHM,
    .stream = DSPD_PCM_SBIT_PLAYBACK,
  };
  struct dspd_async_op op;
  struct dspd_cli_info info;
  bool done = false;
  const struct dspd_device_stat *devinfo;
  memset(&info, 0, sizeof(info));
  dspd_time_init();
  printf("Opening context for playback\n");
  printf(" format:   %d\n"
	 " channels: %d\n"
	 " rate:     %d\n"
	 " bufsize:  %d\n"
	 " fragsize: %d\n"
	 " latency:  %d\n"  
	 " flags:    0x%x\n"
	 " stream:   0x%x\n",
	 params.format,
	 params.channels,
	 params.rate,
	 params.bufsize,
	 params.fragsize,
	 params.latency,
	 params.flags,
	 params.stream);
  ret = dspd_pcmcli_new(&cli, DSPD_PCM_SBIT_PLAYBACK, 0);
  if ( ret < 0 )
    err_exit(ret);
  assert(cli != NULL);
  memset(&op, 0, sizeof(op));



  printf("Connecting to device...");
  ret = dspd_pcmcli_open_device(cli, 
				NULL, 
				dspd_pcmcli_select_byname_cb,
				"hw:0");
  if ( ret < 0 )
    err_exit(ret);
  
  devinfo = dspd_pcmcli_device_info(cli, DSPD_PCM_SBIT_PLAYBACK);
  assert(devinfo != NULL);
  printf("%s\n", devinfo->desc);

  printf("Setting info...");
  memset(&info, 0, sizeof(info));
  info.stream = DSPD_PCM_SBIT_PLAYBACK;
  info.pid = getpid();
  info.gid = getgid();
  info.uid = getuid();
  strcpy(info.name, "playback test");
  ret = dspd_pcmcli_set_info(cli, &info, set_info_complete, &done);
  if ( ret < 0 )
    err_exit(ret);
  while ( ! done )
    {
      ret = dspd_pcmcli_process_io(cli, 0, -1);
      if ( ret < 0 && ret != -EINPROGRESS )
	err_exit(ret);
    }
  printf("done\n");

  printf("Setting hwparams...");
  ret = dspd_pcmcli_set_hwparams(cli,
				 &params,
				 NULL,
				 NULL,
				 true);
  if ( ret < 0 )
    err_exit(ret);
  printf("done\n");

  printf("Preparing (state %d)\n", dspd_pcmcli_get_state(cli));
  ret = dspd_pcmcli_prepare(cli, NULL, NULL);
  if ( ret < 0 )
    err_exit(ret);


  FILE *fp = fopen(FILENAME, "r");
  assert(fp);

  printf("Testing playback:\n");
  test_playback(cli, &params, fp, params.rate);
  printf(" Restarting...\n");
  ret = dspd_pcmcli_prepare(cli, NULL, NULL);
  if ( ret < 0 )
    err_exit(ret);
  sleep(1);
  test_playback(cli, &params, fp, params.rate);
  

  printf("Testing pause:\n");
  ret = dspd_pcmcli_prepare(cli, NULL, NULL);
  if ( ret < 0 )
    err_exit(ret);
  printf(" Starting...\n");
  test_playback(cli, &params, fp, params.rate);
  printf(" Pausing...\n");
  ret = dspd_pcmcli_pause(cli, true, NULL, NULL);
  if ( ret < 0 )
    err_exit(ret);
  sleep(1);
  printf(" Resuming...\n");
  ret = dspd_pcmcli_pause(cli, false, NULL, NULL);
  if ( ret < 0 )
    err_exit(ret);
  test_playback(cli, &params, fp, params.rate);

 

  printf("Testing constant latency...\n");
  ret = dspd_pcmcli_prepare(cli, NULL, NULL);
  if ( ret < 0 )
    err_exit(ret);
  ret = dspd_pcmcli_set_constant_latency(cli, true);
  if ( ret < 0 )
    err_exit(ret);
  test_playback(cli, &params, fp, params.rate * 5);

  fclose(fp);
  dspd_pcmcli_delete(cli);

  return 0;
}
