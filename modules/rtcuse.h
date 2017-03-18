#ifndef _RTCUSE_H_
#define _RTCUSE_H_
struct rtcuse_cdev_params {
  char        devname[NAME_MAX];
  int         flags;
  int32_t     maxwrite;
  int32_t     maxread;
  size_t      pktlen;
  uint32_t    major;
  uint32_t    minor;
};

struct rtcuse_cdev {
  struct rtcuse_cdev_params params;
  int fd;
};

struct rtcuse_ipkt {
  struct fuse_in_header header;
  char                  data[0];
};
bool rtcuse_get_fh(struct fuse_in_header *hdr, void *buf, uint64_t *fh);
int rtcuse_create_cdev(const char *device, 
		       int oflags, 
		       struct rtcuse_cdev_params *devp,
		       struct rtcuse_cdev **dev);
void rtcuse_destroy_cdev(struct rtcuse_cdev *dev);
ssize_t rtcuse_readv(struct rtcuse_cdev *dev, const struct iovec *iov, int iovcnt);
ssize_t rtcuse_readv_block(struct rtcuse_cdev *dev, const struct iovec *iov, int iovcnt);
ssize_t rtcuse_writev_block(struct rtcuse_cdev *dev, const struct iovec *iov, int iovcnt);
ssize_t rtcuse_writev(struct rtcuse_cdev *dev, 
		      const struct iovec *iov, 
		      int iovcnt);
int rtcuse_reply_open(struct rtcuse_cdev *dev, uint64_t unique, uint64_t fh, uint32_t flags);
#endif
