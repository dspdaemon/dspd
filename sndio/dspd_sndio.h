#ifndef _DSPD_SNDIO_H
#define _DSPD_SNDIO_H
struct sndio_ctx;
struct dspd_daemon_ctx;
struct dspd_sndio_params {
  const char             *net_addrs;
  bool                    system_server;
  bool                    disable_unix_socket;
  int32_t                 unit_number;
  struct dspd_daemon_ctx *context;
  const char             *server_addr;
};
int32_t dspd_sndio_new(struct sndio_ctx **ctx, struct dspd_sndio_params *params);
int32_t dspd_sndio_start(struct sndio_ctx *ctx);
int32_t dspd_sndio_run(struct sndio_ctx *ctx);
void dspd_sndio_delete(struct sndio_ctx *ctx);
#endif
