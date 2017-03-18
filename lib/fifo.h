#ifndef _DSPD_FIFO_H_
#define _DSPD_FIFO_H_
#include <stdint.h>
#include "atomic.h"
struct dspd_fifo_data {
  volatile uint32_t obj_in;
  volatile uint32_t obj_out;
  volatile uint32_t rate;
  volatile uint32_t error;
  char              data[0];
};

struct dspd_fifo_header {
  uint32_t                   max_obj;
  uint32_t                   obj_size;
  uint32_t                   bufsize;
#define DSPD_FIFO_FLAG_INIT 1
  uint32_t                   flags;
  struct dspd_fifo_data     *data;
  char                       bytes[0];
};

struct dspd_block_header {
  uint32_t length;
  uint32_t offset;
  uint32_t rate;
  union {
    volatile uint32_t  pad;
    volatile dspd_ts_t lock;
  } lock;
  char     data[];
};
struct dspd_block_footer {
  uint32_t length;
  uint32_t pad;
};
#define DSPD_BLOCK_HFLEN (sizeof(struct dspd_block_footer)+sizeof(struct dspd_block_header))

int32_t dspd_fifo_get_error(const struct dspd_fifo_header *fifo);
void dspd_fifo_set_error(const struct dspd_fifo_header *fifo, int32_t err);
uint32_t dspd_fifo_optr(const struct dspd_fifo_header *fifo);
uint32_t dspd_fifo_iptr(const struct dspd_fifo_header *fifo);
int dspd_fifo_len_ptrs(const struct dspd_fifo_header *fifo, 
		       uint32_t *len,
		       uint32_t *in,
		       uint32_t *out);
int dspd_fifo_len(const struct dspd_fifo_header *fifo, uint32_t *len);
int dspd_fifo_space(const struct dspd_fifo_header *fifo, uint32_t *len);
void dspd_fifo_wcommit(struct dspd_fifo_header *fifo, uint32_t len);
void dspd_fifo_rcommit(struct dspd_fifo_header *fifo, uint32_t len);
void dspd_fifo_reset(struct dspd_fifo_header *fifo);
int dspd_fifo_wiov(struct dspd_fifo_header *fifo,
		   void **ptr,
		   uint32_t *len);
int dspd_fifo_riov(struct dspd_fifo_header *fifo,
		   void **ptr,
		   uint32_t *len);

//Works like snd_pcm_mmap_begin()
int dspd_fifo_wiov_ex(struct dspd_fifo_header *fifo,
		      void **ptr,
		      uint32_t *offset,
		      uint32_t *len);
int dspd_fifo_riov_ex(struct dspd_fifo_header *fifo,
		      void **ptr,
		      uint32_t *offset,
		      uint32_t *len);


int dspd_fifo_init(struct dspd_fifo_header *hdr, 
		   uint32_t nmemb,
		   uint32_t size,
		   void *addr);
void dspd_fifo_destroy(struct dspd_fifo_header *hdr);
size_t dspd_fifo_size(uint32_t nmemb, uint32_t size);
int dspd_fifo_new(struct dspd_fifo_header **fifo,
		  uint32_t nmemb,
		  uint32_t size,
		  void *addr);
void dspd_fifo_delete(struct dspd_fifo_header *fifo);
int32_t dspd_fifo_write(struct dspd_fifo_header *fifo,
			const void *addr,
			uint32_t len);
int32_t dspd_fifo_read(struct dspd_fifo_header *fifo,
		       void *addr,
		       uint32_t len);


uint32_t dspd_fifo_peek(const struct dspd_fifo_header *fifo,
			uint32_t offset,
			void **addr);



#endif
