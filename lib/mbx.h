#ifndef _DSPD_MBX_H_
#define _DSPD_MBX_H_
#include "atomic.h"
#define DSPD_MBX_BLOCKS 3
struct dspd_mbx_data;
struct dspd_mbx_header {
  uint32_t             blocksize; //Size of blocks in dspd_mbx_data
#define DSPD_MBX_FLAG_INIT 1
  uint32_t             flags;
  struct dspd_mbx_data *data; //Pointer to data (often ibytes)
  char                  ibytes[]; //Inline data
};

struct dspd_mbx_data {
  volatile int32_t     index; //Index of last write
  union {
    volatile dspd_ts_t lock;  //Actual lock (usually 1 byte)
    volatile int32_t pad;     //Pad (make alignment the same for multilib)
  } locks[DSPD_MBX_BLOCKS];
  char                 data[]; //Data blocks
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

void *dspd_mbx_acquire_read(struct dspd_mbx_header *mbx, int lock);
void dspd_mbx_release_read(struct dspd_mbx_header *mbx, void *ptr);
void *dspd_mbx_acquire_write(struct dspd_mbx_header *mbx);
void dspd_mbx_release_write(struct dspd_mbx_header *mbx, void *ptr);
//Reset block index
void dspd_mbx_reset(struct dspd_mbx_header *mbx);

#endif
