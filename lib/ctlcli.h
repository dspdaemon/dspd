#ifndef _DSPD_CTLCLI_H_
#define _DSPD_CTLCLI_H_
struct dspd_ctl_client;
typedef void (*dspd_cc_elem_change_cb_t)(struct dspd_ctl_client *cli, void *arg, int32_t err, uint32_t elem, int32_t evt, const struct dspd_mix_info *info);
typedef void (*dspd_cc_elem_getset_cb_t)(struct dspd_ctl_client *cli, void *arg, struct dspd_async_op *op, uint32_t index, int32_t value);
typedef void (*dspd_cc_elem_getrange_cb_t)(struct dspd_ctl_client *cli, void *arg, struct dspd_async_op *op, struct dspd_mix_range *range);
typedef void (*dspd_cc_subscribe_cb_t)(struct dspd_ctl_client *cli, void *arg, struct dspd_async_op *op, uint32_t qlen);
typedef void (*dspd_cc_enum_info_cb_t)(struct dspd_ctl_client *cli, void *arg, struct dspd_async_op *op, struct dspd_mix_info *info);
typedef void (*dspd_cc_list_cb_t)(struct dspd_ctl_client *cli, void *arg, int32_t error, uint32_t count);

#define DSPD_CC_IO_SYNC  DSPD_AIO_SYNC
#define DSPD_CC_IO_DEFAULT DSPD_AIO_DEFAULT

struct dspd_ctl_client;




int32_t dspd_ctlcli_set_event_cb(struct dspd_ctl_client *cli, dspd_cc_elem_change_cb_t callback, void *arg);
int32_t dspd_ctlcli_subscribe(struct dspd_ctl_client *cli, bool subscribe, uint32_t *qlen, dspd_cc_subscribe_cb_t complete, void *arg);
int32_t dspd_ctlcli_refresh_count(struct dspd_ctl_client *cli, uint32_t *count, dspd_cc_list_cb_t complete, void *arg);
int32_t dspd_ctlcli_elem_count(struct dspd_ctl_client *cli);
int32_t dspd_ctlcli_elem_get_info(struct dspd_ctl_client *cli, uint32_t index, struct dspd_mix_info *info);
int32_t dspd_ctlcli_elem_set_int32(struct dspd_ctl_client *cli, 
				   uint32_t index, 
				   int32_t channel, 
				   int32_t in, 
				   int32_t *out, 
				   dspd_cc_elem_getset_cb_t complete, 
				   void *arg);
int32_t dspd_ctlcli_elem_get_int32(struct dspd_ctl_client *cli, 
				   uint32_t index, 
				   int32_t channel, 
				   int32_t *val, 
				   dspd_cc_elem_getset_cb_t complete, 
				   void *arg);
int32_t dspd_ctlcli_elem_get_range(struct dspd_ctl_client *cli, 
				   uint32_t index, 
				   struct dspd_mix_range *range, 
				   dspd_cc_elem_getrange_cb_t complete, 
				   void *arg);
int32_t dspd_ctlcli_elem_get_enum_info(struct dspd_ctl_client *cli, 
				       uint32_t elem_index, 
				       uint32_t enum_index,
				       struct dspd_mix_info *info,
				       dspd_cc_elem_getrange_cb_t complete, 
				       void *arg);

int32_t dspd_ctlcli_init(struct dspd_ctl_client *cli, 
			 ssize_t max_io_hint,
			 size_t max_elem_hint);

size_t dspd_ctlcli_sizeof(void);
int32_t dspd_ctlcli_new(struct dspd_ctl_client **cli, 
			ssize_t max_io_hint,
			size_t max_elem_hint);
void dspd_ctlcli_bind(struct dspd_ctl_client *cli, struct dspd_aio_ctx *aio, int32_t device);
void dspd_ctlcli_delete(struct dspd_ctl_client *cli);
void dspd_ctlcli_destroy(struct dspd_ctl_client *cli);

void dspd_ctlcli_set_scale_pct(struct dspd_ctl_client *cli, bool en);
bool dspd_ctlcli_get_scale_pct(struct dspd_ctl_client *cli);

#endif
