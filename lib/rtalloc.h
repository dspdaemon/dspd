#ifndef _DSPD_RTALLOC_H_
#define _DSPD_RTALLOC_H_
struct pageinfo {
  volatile AO_TS_t  lock;
  volatile uint16_t count; //Number of additional pages allocated
};

struct dspd_rtalloc {
  struct pageinfo *pages;
  char            *membase;
  uintptr_t        boundary;
  size_t           pagecount;
  size_t           pagesize;
};
struct dspd_rtalloc *dspd_rtalloc_new(size_t npages, size_t pagesize);
void dspd_rtalloc_delete(struct dspd_rtalloc *alloc);
void *dspd_rtalloc_getpages(struct dspd_rtalloc *alloc, size_t npages);
void *dspd_rtalloc_malloc(struct dspd_rtalloc *alloc, size_t len);
void *dspd_rtalloc_calloc(struct dspd_rtalloc *alloc, size_t nmemb, size_t size);
void dspd_rtalloc_free(struct dspd_rtalloc *alloc, void *addr);
void dspd_rtalloc_shrink(struct dspd_rtalloc *alloc, void *addr, size_t new_size);
bool rtalloc_check_buffer(struct dspd_rtalloc *alloc, void *addr);
#endif
