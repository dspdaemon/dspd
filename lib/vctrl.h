#ifndef _DSPD_VCTRL_H_
#define _DSPD_VCTRL_H_
#define DSPD_VCTRL_DEVICE (1<<2)
#define DSPD_VCTRL_CLIENT (1<<3)
struct dspd_vctrl_list;
int32_t dspd_vctrl_list_new(struct dspd_vctrl_list **list);
void dspd_vctrl_list_delete(struct dspd_vctrl_list *list);
int32_t dspd_vctrl_stream_ctl(struct dspd_rctx *rctx,
			      uint32_t          req,
			      const void       *inbuf,
			      size_t            inbufsize,
			      void             *outbuf,
			      size_t            outbufsize);
void dspd_daemon_vctrl_set_value(uint32_t stream, 
				 uint32_t sbits,
				 float value,
				 const char *name);
int32_t dspd_daemon_vctrl_register(int32_t playback,
				   int32_t capture,
				   int32_t type,
				   float value,
				   const char *name);
int32_t dspd_daemon_vctrl_unregister(int32_t playback,
				     int32_t capture);

#endif
