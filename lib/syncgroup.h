#ifndef _DSPD_SYNCGROUP_H
#define _DSPD_SYNCGROUP_H
struct dspd_syncgroup;
struct dspd_sglist;
int32_t dspd_sg_new(struct dspd_sglist *sgl, struct dspd_syncgroup **sg, uint32_t streams);
struct dspd_syncgroup *dspd_sg_get(struct dspd_sglist *sgl, uint32_t sgid);
void dspd_sg_put(struct dspd_sglist *sgl, uint32_t sgid);
void dspd_sg_add(struct dspd_syncgroup *sg, uint32_t idx);
void dspd_sg_remove(struct dspd_syncgroup *sg, uint32_t idx);
dspd_time_t dspd_sg_start(struct dspd_syncgroup *sg, const uint32_t *streams);
void dspd_sg_stop(struct dspd_syncgroup *sg, const uint32_t *streams);
uint32_t dspd_sg_id(struct dspd_syncgroup *sg);
uint32_t dspd_sg_streams(struct dspd_syncgroup *sg);
void dspd_sglist_delete(struct dspd_sglist *sgl);
int32_t dspd_sglist_new(struct dspd_sglist **sgl);
#endif
