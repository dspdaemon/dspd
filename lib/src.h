#ifndef _DSPD_SRC_H_
#define _DSPD_SRC_H_
typedef void *dspd_src_t;
struct dspd_src_ops;
struct dspd_src_info {
  char     name[32];
  uint32_t max_quality;
  uint32_t min_quality;
  uint32_t step; //Recommended step.  Resamplers will round according to this.
};

uint64_t dspd_src_get_frame_count(uint64_t rate_in, uint64_t rate_out, uint64_t frames_in);

int32_t dspd_src_set_rates(dspd_src_t src, int32_t in, int32_t out);

int32_t dspd_src_reset(dspd_src_t src);

int32_t dspd_src_process(dspd_src_t   src,
			 bool         eof,
			 const float *inbuf,
			 size_t      *frames_in,
			 float       *outbuf,
			 size_t      *frames_out);
int dspd_src_init(const struct dspd_src_ops *ops);
int32_t dspd_src_new(dspd_src_t *newsrc, int quality, int channels);
int32_t dspd_src_delete(dspd_src_t src);
void dspd_src_info(struct dspd_src_info *info);

void dspd_src_get_params(dspd_src_t src, 
			 uint32_t *quality,
			 uint32_t *rate_in,
			 uint32_t *rate_out);
int dspd_src_get_default_quality(void);
void dspd_src_set_default_quality(int q);

struct dspd_src_ops {
  int32_t (*init)(void);
  int32_t (*set_rates)(dspd_src_t src,
		       uint32_t in,
		       uint32_t out);
  int32_t (*reset)(dspd_src_t src);
  int32_t (*process)(dspd_src_t   src,
		     bool         eof,
		     const float * __restrict inbuf,
		     size_t      * __restrict frames_in,
		     float       * __restrict outbuf,
		     size_t      * __restrict frames_out);
  int32_t (*newsrc)(dspd_src_t *src,
		    int quality, 
		    int channels);
  int32_t (*freesrc)(dspd_src_t src);
  void (*info)(struct dspd_src_info *info);
  void (*get_params)(dspd_src_t src, 
		     uint32_t *quality,
		     uint32_t *rate_in,
		     uint32_t *rate_out);
  void (*set_default_quality)(int q);
  int (*get_default_quality)(void);
};


#endif
