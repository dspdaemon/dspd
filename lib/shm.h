#ifndef _DSPD_SHM_H_
#define _DSPD_SHM_H_
#include <stdint.h>
struct dspd_client_shm {
  int32_t  arg;      //Pointer (low), fd, or shmid
  int32_t  reserved; //Pointer (high) or unused
  int32_t  flags;    //SHM flags
  int32_t  key;      //svipc key or object list slot
  uint32_t len;      //size of shm
  uint32_t section_count;
};
struct dspd_shm_section {
  uint32_t length;
  uint32_t offset;
  uint32_t section_id;
  uint32_t reserved;
};

struct dspd_shm_addr {
  /*Size of memory block*/
  uint32_t   length;
  /*This is a nonzero identifier that is unique within a memory section*/
  uint32_t   section_id;
  /*
    Address of memory.  Memory to be copied when creating a new memory block
    or the address of a memory block that was attached after being created.
   */
  void      *addr;
};

struct dspd_shm_header {
  uint32_t                length;
  uint32_t                version;
  uint32_t                section_count;
  struct dspd_shm_section sections[];
};

//Type of SHM.  The only thing that will be supported
//is FLAG_PRIVATE.  The other types are chosen by the implementation.
#define DSPD_SHM_FLAG_MMAP    1
#define DSPD_SHM_FLAG_SVIPC   2
#define DSPD_SHM_FLAG_PRIVATE 4
#define DSPD_SHM_FLAG_WRITE   8
#define DSPD_SHM_FLAG_READ    16
#define DSPD_SHM_FLAG_MEMFD   32

struct dspd_shm_map {
  int32_t                 arg;
  int32_t                 key;
  int32_t                 flags;
  struct dspd_shm_header *addr;
  size_t                  length;
  uint32_t                section_count;
};

int dspd_shm_get_addr(const struct dspd_shm_map *map,
		      struct dspd_shm_addr *addr);
void dspd_shm_close(struct dspd_shm_map *map);
int dspd_shm_attach(struct dspd_shm_map *map);
int dspd_shm_create(struct dspd_shm_map *map,
		    const struct dspd_shm_addr *sect,
		    uint32_t nsect);

#endif
