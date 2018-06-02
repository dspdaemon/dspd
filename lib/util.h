#ifndef _DSPD_UTIL_H_
#define _DSPD_UTIL_H_
#include <stdio.h>
#include <stdbool.h>
#include <sys/uio.h>
#include <stdarg.h>
#include <errno.h>
struct dspd_ll {
  void           *pointer;
  struct dspd_ll *prev, *next;
};
struct dspd_ll *dspd_ll_new(void *pointer);
struct dspd_ll *dspd_ll_append(struct dspd_ll *head, void *pointer);
struct dspd_ll *dspd_ll_tail(struct dspd_ll *head);
struct dspd_cli_params;
void dspd_translate_parameters(const struct dspd_cli_params *input,
			       struct dspd_cli_params *output);

void dspd_fullduplex_parameters(const struct dspd_cli_params *playback,
				const struct dspd_cli_params *capture,
				struct dspd_cli_params *result);

#ifdef MIN
#undef MIN
#endif
#ifdef MAX
#undef MAX
#endif

/*
  Nice definitions that don't do unexpected stuff.  It is safe
  to pass expressions as an argument since the expression is
  evaluated inside the parenthesis before the comparison happens.
*/
#define MIN(_a,_b)  ((_a)<(_b)?(_a):(_b))
#define MAX(_a,_b)  ((_a)>(_b)?(_a):(_b))

static inline int get_hpo2(int x)
{
  return (sizeof(x) * 8) - __builtin_clz(x - 1);
}
static inline int get_lpo2(int x)
{
  return (sizeof(x) * 8) - (__builtin_clz(x - 1) + 1);
}


#ifndef ARRAY_SIZE
#define ARRAY_SIZE(_a) (sizeof(_a)/sizeof(_a[0]))
#endif
void dspd_dump_params(const struct dspd_cli_params *params, FILE *fp);

struct dspd_dict *dspd_parse_args(int argc, char **argv);
void dspd_dump_kv(const struct dspd_dict *kv, FILE *fp);
uint32_t dspd_get_fragsize(const struct dspd_cli_params *devparams,
			   int32_t rate,
			   int32_t frames);
size_t strlcpy(char * __restrict dst, 
	       const char * __restrict src, 
	       size_t size);

void *dspd_resizebuf(void *ptr, size_t newsize, size_t oldsize);
void *dspd_reallocz(void *ptr, size_t newsize, size_t oldsize, bool zero_all);

void *dspd_memdup(const void *ptr, size_t len);

#define DSPD_IOV_EOF INT32_MIN
#define DSPD_IOV_COMPLETE 0
ssize_t dspd_writev(int fd, const struct iovec *iov, int niov, size_t *offset);
ssize_t dspd_readv(int fd, const struct iovec *iov, int niov, size_t *offset);
int set_thread_name(const char *name);

/*
  The default value can be any value.  The recommended use default
  value is 0, 1, or -1.  If -1, then the string is invalid.  If 
  0 or 1, then the caller doesn't care and just wants a valid
  boolean value.  The means the return values are 0, 1, or 
  defaultvalue.
  
*/
#define DSPD_SYNTAX_ERROR -1
int32_t dspd_strtob(int32_t defaultvalue, const char *opt);

int32_t dspd_vparse_opt(int32_t defaultvalue, 
			const char *opt, va_list opts);

//varargs are: value, intval, ..., NULL
//example: "something", 123, NULL
int32_t dspd_parse_opt(int32_t defaultvalue, 
		       const char *opt, ...);
size_t dspd_strlen_safe(const char *str);

const char *dspd_strtok_c(const char *str, const char *delim, const char **saveptr, size_t *length);

static inline bool dspd_tmperr(int err)
{
  if ( err > 0 )
    err *= -1;
  return err == -EAGAIN || 
    err == -EWOULDBLOCK || 
    err == -EINTR || 
    err == -EBUSY || 
    err == -EINVAL;
}

#define dspd_fatal_err(_e) (!dspd_tmperr(_e))
bool dspd_devname_cmp(const char *devname, const char *str);
#endif
