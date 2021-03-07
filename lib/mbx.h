#ifndef _DSPD_MBX_H_
#define _DSPD_MBX_H_
#include <stdbool.h>
#include "atomic.h"
#define DSPD_MBX_BLOCKS 4
struct dspd_mbx_data;

struct dspd_seqlock32 {
  volatile uint32_t seq;
  volatile uint32_t overflow;
};

bool dspd_seqlock32_read_begin(const struct dspd_seqlock32 *lock, uint64_t *context);
bool dspd_seqlock32_read_complete(const struct dspd_seqlock32 *lock, uint64_t context);
void dspd_seqlock32_write_lock(struct dspd_seqlock32 *lock);
void dspd_seqlock32_write_unlock(struct dspd_seqlock32 *lock);
void dspd_seqlock32_init(struct dspd_seqlock32 *lock);

struct dspd_mbx_header {
  uint32_t             blocksize; //Size of blocks in dspd_mbx_data
#define DSPD_MBX_FLAG_INIT 1
  uint32_t             flags;
  struct dspd_mbx_data *data; //Pointer to data (often ibytes)
  char                  ibytes[]; //Inline data
};

struct dspd_mbx_data {
  volatile int32_t      index; //Index of last write
  volatile int32_t      pad;
  struct dspd_seqlock32 locks[DSPD_MBX_BLOCKS];
  char                  data[]; //Data blocks
};

//Get size of struct dspd_mbx_data
uint32_t dspd_mbx_bufsize(uint32_t blocksize);

//Initialize preallocated buffer with external buffer or inline buffer
int dspd_mbx_init(struct dspd_mbx_header *mbx, 
		  uint32_t blocksize,
		  void *addr);

//Destroy preallocated buffer
void dspd_mbx_destroy(struct dspd_mbx_header *mbx);

//Allocate a buffer and possibly the data section
int dspd_mbx_new(struct dspd_mbx_header **mbx,
		 uint32_t blocksize, 
		 void *addr);
void dspd_mbx_delete(struct dspd_mbx_header *mbx);


//Reset block index
void dspd_mbx_reset(struct dspd_mbx_header *mbx);

void *dspd_mbx_read(struct dspd_mbx_header *mbx, void *buf, size_t len);
void *dspd_mbx_write_lock(struct dspd_mbx_header *mbx, int32_t *idx);
void dspd_mbx_write_unlock(struct dspd_mbx_header *mbx, int32_t idx);


#endif
