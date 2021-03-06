#define _GNU_SOURCE
#include <unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#define _DSPD_CTL_MACROS
#include "../lib/sslib.h"
#include "../lib/daemon.h"
#include "../lib/cbpoll.h"
#define TEST_COUNT 1
static void test_aio_ops(struct dspd_aio_ctx *ctx)
{
  char inbuf[512], outbuf[512];
  int32_t ret;
  size_t len;
  size_t br;
  strcpy(inbuf, "test");
  len = strlen(inbuf) + 1;
  ret = dspd_stream_ctl(ctx,
			-1,
			DSPD_SOCKSRV_REQ_ECHO,
			inbuf,
			len,
			outbuf,
			len,
			&br);
  if ( ret < 0 )
    {
      dspd_log(0, "dspd_stream_ctl: error %d", ret);
      return;
    }
  assert(br == len);
  if ( strcmp(inbuf, outbuf) == 0 )
    {
      dspd_log(0, "Echo request succeeded");
    } else
    {
      dspd_log(0, "Echo request failed");
      abort();
    }
  
			
}

static void aio_test(void *p)
{
  struct dspd_aio_ctx *ctx;
  int32_t ret;
  struct dspd_aio_fifo_ptevent pte;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_condattr_t attr;
  ret = pthread_condattr_init(&attr);
  assert(ret == 0);
  ret = pthread_condattr_setclock(&attr, dspd_get_clockid());
  assert(ret == 0);
  ret = pthread_cond_init(&cond, &attr);
  assert(ret == 0);
  pthread_condattr_destroy(&attr);
  ret = pthread_mutex_init(&lock, NULL);
  assert(ret == 0);
  dspd_ts_clear(&pte.tsval);
  pte.cond = &cond;
  pte.lock = &lock;

  
  dspd_log(0, "Testing async io...");

  dspd_log(0, "Testing pthread io");
  ret = dspd_aio_new(&ctx, DSPD_AIO_DEFAULT);
  if ( ret < 0 )
    {
      dspd_log(0, "Error creating aio context");
      abort();
    }
  ret = dspd_aio_connect(ctx, NULL, p, &dspd_aio_fifo_ptevent_ops, &pte);
  if ( ret < 0 )
    {
      dspd_log(0, "Error connecting context");
      abort();
    }
  test_aio_ops(ctx);
  dspd_aio_delete(ctx);
  pthread_mutex_destroy(&lock);
  pthread_cond_destroy(&cond);

  dspd_log(0, "Testing eventfd io");
  struct dspd_aio_fifo_eventfd efd;
  efd.fd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
  assert(efd.fd >= 0);
  dspd_ts_clear(&efd.tsval);
  
  ret = dspd_aio_new(&ctx, DSPD_AIO_DEFAULT);
  if ( ret < 0 )
    {
      dspd_log(0, "Error creating aio context");
      abort();
    }
  ret = dspd_aio_connect(ctx, NULL, p, &dspd_aio_fifo_eventfd_ops, &efd);
  if ( ret < 0 )
    {
      dspd_log(0, "Error connecting context");
      abort();
    }
  
  test_aio_ops(ctx);
  dspd_aio_delete(ctx);
  close(efd.fd);

  
  return;
}

static void *aiotest_thread(void *p)
{
  size_t i;
  for ( i = 0; i < TEST_COUNT; i++ )
    {
      dspd_log(0, "Test %ld of %ld", (long)i, (long)TEST_COUNT);
      aio_test(p);
    }
  dspd_log(0, "Test complete");
  return NULL;
}

static pthread_t aiothread;
static int aiotest_init(struct dspd_daemon_ctx *daemon, void **context)
{
  pthread_create(&aiothread, NULL, aiotest_thread, daemon);
  return 0;
}

static void aiotest_close(struct dspd_daemon_ctx *daemon, void **context)
{
  
}



struct dspd_mod_cb dspd_mod_aiotest = {
  .init_priority = 0,
  .desc = "Async io test module",
  .init = aiotest_init,
  .close = aiotest_close,
};
