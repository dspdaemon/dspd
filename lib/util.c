/*
 *  UTIL - Various utility routines
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as
 *   published by the Free Software Foundation; either version 2.1 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <syscall.h>
#include <stdarg.h>
#include "sslib.h"



struct dspd_ll *dspd_ll_new(void *pointer)
{
  struct dspd_ll *ret;
  ret = calloc(1, sizeof(*ret));
  if ( ret )
    ret->pointer = pointer;
  return ret;
}

struct dspd_ll *dspd_ll_append(struct dspd_ll *head, void *pointer)
{
  struct dspd_ll *curr, *prev, *ret;
  if ( head == NULL )
    {
      ret = dspd_ll_new(pointer);
    } else
    {
      for ( curr = head; curr; curr = curr->next )
	prev = curr;
      
      prev->next = dspd_ll_new(pointer);
      if ( prev->next )
	prev->next->prev = prev;
      ret = prev->next;
    }
  return ret;
}

struct dspd_ll *dspd_ll_tail(struct dspd_ll *head)
{
  struct dspd_ll *curr, *prev = NULL;
  for ( curr = head; curr; curr = curr->next )
    prev = curr;
  return prev;
}


void dspd_translate_parameters(const struct dspd_cli_params *input,
			       struct dspd_cli_params *output)
{
  struct dspd_cli_params out;
  size_t frame_bytes;
  uint64_t sample_time, sample_time_input, t1, t2, n;
  
  if ( output->format < 0 )
    out.format = DSPD_PCM_FORMAT_FLOAT_LE;
  else
    out.format = output->format;
  
  if ( output->xflags & DSPD_CLI_XFLAG_COOKEDMODE )
    {
      if ( output->channels == 0 || output->channels > input->channels )
	out.channels = input->channels;
      else
	out.channels = output->channels;

      if ( output->rate == 0 )
	out.rate = input->rate;
      else
	out.rate = output->rate;
    } else
    {
      out.channels = input->channels;
      out.rate = input->rate;
    }
  frame_bytes = dspd_get_pcm_format_size(out.format);
  if ( frame_bytes == 0 )
    {
      out.format = DSPD_PCM_FORMAT_FLOAT_LE;
      frame_bytes = dspd_get_pcm_format_size(out.format);
    }
  frame_bytes *= out.channels;
  DSPD_ASSERT(frame_bytes);
  sample_time = 1000000000 / out.rate;
  sample_time_input = 1000000000 / out.rate;
  if ( output->xflags & DSPD_CLI_XFLAG_BYTES )
    {
      out.fragsize = output->fragsize / frame_bytes;
      out.bufsize = output->bufsize / frame_bytes;
      if ( output->latency == 0 )
	out.latency = out.fragsize;
      else
	out.latency = output->latency / frame_bytes;
    } else if ( output->xflags & DSPD_CLI_XFLAG_NANOSECONDS )
    {
      out.fragsize = output->fragsize / sample_time;
      out.bufsize = output->bufsize / sample_time;
      if ( output->latency == 0 )
	out.latency = out.fragsize;
      else
	out.latency = output->latency / sample_time;
    } else
    {
      out.fragsize = output->fragsize;
      out.bufsize = output->bufsize;
      if ( output->latency == 0 )
	out.latency = out.fragsize;
      else
	out.latency = output->latency;
    }

  t1 = sample_time_input * input->min_latency;
  t2 = sample_time * out.latency;
  if ( t2 < t1 )
    out.latency = t1 / sample_time;
  
  t1 = sample_time_input * input->max_latency;
  t2 = sample_time * out.latency;
  if ( t2 > t1 )
    out.latency = t1 / sample_time;

  if ( out.latency > out.fragsize )
    out.fragsize = out.latency;
  if ( out.fragsize > (out.bufsize / 3) )
    out.bufsize = out.fragsize * 3;
  
  if ( output->xflags & DSPD_CLI_XFLAG_EXACTSIZE )
    {
      t1 = out.latency * sample_time;
      t2 = input->min_latency * sample_time_input;
      n = t1 / t2;
      DSPD_ASSERT(n > 0);
      out.latency = ((n * input->min_latency) * sample_time_input) / sample_time;
      n = out.fragsize / out.latency;
      out.fragsize = n * out.latency;
    }


  output->format = out.format;
  output->channels = out.channels;
  output->rate = out.rate;
  

  if ( output->xflags & DSPD_CLI_XFLAG_BYTES )
    {
      output->fragsize = out.fragsize * frame_bytes;
      output->bufsize = out.bufsize * frame_bytes;
      output->latency = out.latency * frame_bytes;
    } else if ( output->xflags & DSPD_CLI_XFLAG_NANOSECONDS )
    {
      output->fragsize = out.fragsize * sample_time;
      output->bufsize = out.bufsize * sample_time;
      output->latency = out.latency * sample_time;
    } else
    {
      output->fragsize = out.fragsize;
      output->bufsize = out.bufsize;
      output->latency = out.latency;
    }

}


void dspd_fullduplex_parameters(const struct dspd_cli_params *playback,
				const struct dspd_cli_params *capture,
				struct dspd_cli_params *result)
{
  memset(result, 0, sizeof(*result));
  //Floating point always works.
  result->format = DSPD_PCM_FORMAT_FLOAT_LE;

  //This applies to OSS but it may be ok to use different channel
  //counts for other APIs.
  result->channels = MIN(playback->channels, capture->channels);

  //These two are probably always going to be the same
  result->rate = MIN(playback->rate, capture->rate);

  result->bufsize = MAX(playback->bufsize, capture->bufsize);

  result->fragsize = MAX(playback->fragsize, capture->fragsize);

  result->stream = DSPD_PCM_SBIT_PLAYBACK | DSPD_PCM_SBIT_CAPTURE;

  result->min_latency = MAX(playback->min_latency, capture->min_latency);
  result->max_latency = MAX(playback->max_latency, capture->max_latency);
  
  
}

void dspd_dump_params(const struct dspd_cli_params *params, FILE *fp)
{
  fprintf(fp,
	  "PARAMS:      %p\n"
	  "FORMAT:      %d\n"
	  "CHANNELS:    %d\n"
	  "RATE:        %d\n"
	  "BUFSIZE:     %d\n"
	  "FRAGSIZE:    %d\n"
	  "STREAM:      %d\n"
	  "LATENCY:     %d\n"
	  "FLAGS:       %d\n"
	  "MIN_LATENCY: %d\n"
	  "MAX_LATENCY: %d\n"
	  "SRC_QUALITY: %d\n"
	  "XFLAGS:      %d\n",
	  params,
	  params->format,
	  params->channels,
	  params->rate,
	  params->bufsize,
	  params->fragsize,
	  params->stream,
	  params->latency,
	  params->flags,
	  params->min_latency,
	  params->max_latency,
	  params->src_quality,
	  params->xflags);
}

/*
  The arguments are in the format of:

  -csomething
  -c something
  -c-dash-something
  --longname=longval

  This is generally not going to work:
  
  -k -val (2 different keys, use -k-val)
  

  The rules are:

  

  1.  If it starts with '-' then it must be 1 character.  Any more
  is considered a value.

  2.  Ambiguous stuff isn't allowed.

  3.  If it starts with '--' then it is delimited by =, not the next arg.

  

 */

struct dspd_dict *dspd_parse_args(int argc, char **argv)
{
  int i;
  struct dspd_dict *kvs = dspd_dict_new(argv[0]);
  const char *a;
  const char *key = NULL;
  char str[3];
  char *ptr, *p;
  uintptr_t pos;
  if ( ! kvs )
    return NULL;
  for ( i = 1; i <= argc; i++ )
    {
      
      a = argv[i];
 
      if ( a != NULL && (a[0] == '-' && a[1] != '-') )
	{
	  if ( key )
	    {
	      //Previous value is key
	      if ( ! dspd_dict_set_value(kvs, key, NULL, true) )
		goto error;
	      key = NULL;
	    }

	  if ( strlen(a) > 2 )
	    {
	      //Key and value are one "-kv"
	      key = NULL;
	      memcpy(str, a, 2);
	      str[2] = 0;
	      if ( ! dspd_dict_set_value(kvs, str, &a[2], true) )
		goto error;
	    } else
	    {
	      //Key is -k
	      key = a;
	    }
	  
	} else if ( a != NULL && strncmp(a, "--", 2) == 0 )
	{
	  //--key=value or --arg
	  key = NULL;
	  p = strchr(a, '=');
	  if ( p )
	    {
	      pos = (uintptr_t)p - (uintptr_t)a;
	      ptr = strndup(a, pos);
	      if ( ! ptr )
		goto error;
	      if ( ! dspd_dict_set_value(kvs, ptr, &a[pos+1], true) )
		{
		  free(ptr);
		  goto error;
		}
	      free(ptr);
	      ptr = NULL;
	    } else
	    {
	      if ( ! dspd_dict_set_value(kvs, a, NULL, true) )
		goto error;
		
	      
	    }
	} else if ( key )
	{
	  //Have key, so this is a value
	  if ( ! dspd_dict_set_value(kvs, key, a, true) )
	    goto error;
	  key = NULL;
	} else if ( a )
	{
	  //Bare arg
	  if ( ! dspd_dict_set_value(kvs, a, NULL, true) )
	    goto error;
	}
    }

  return kvs;

 error:
  dspd_dict_free(kvs);
  return NULL;
}

void dspd_dump_kv(const struct dspd_dict *kv, FILE *fp)
{
  size_t i;
  fprintf(fp, "[%s]\n", kv->name);
  for ( i = 0; i < kv->count; i++ )
    fprintf(fp, "%s=%s\n", kv->list[i].key, kv->list[i].value);
}

uint32_t dspd_get_fragsize(const struct dspd_cli_params *devparams,
			   int32_t rate,
			   int32_t frames)
{
  uint64_t sti = 1000000000ULL / rate, sto = 1000000000ULL / devparams->rate;
  uint64_t ft = sti * frames;
  uint64_t mt = sto * devparams->min_latency;
  if ( mt > ft )
    return mt / sti;
  return frames;
}

size_t strlcpy(char * __restrict dst, 
	       const char * __restrict src, 
	       size_t size)
{
  size_t i = 0, n = size - 1;
  while ( i < n && src[i] != 0 )
    {
      dst[i] = src[i];
      i++;
    }
  n = i;
  while ( n < size )
    {
      dst[n] = 0;
      n++;
    }
  while ( src[i] )
    i++;
  return i;
}

void *dspd_resizebuf(void *ptr, size_t newsize, size_t oldsize)
{
  void *ret;
  if ( newsize > 0 && newsize != oldsize )
    {
      ret = realloc(ptr, newsize);
      if ( ret == NULL )
	{
	  if ( newsize < oldsize )
	    ret = ptr;
	}
    } else if ( newsize == 0 )
    {
      ret = NULL;
      free(ptr);
    } else 
    {
      ret = ptr;
    }
  return ret;
}

void *dspd_reallocz(void *ptr, size_t newsize, size_t oldsize, bool zero_all)
{
  void *ret;
  ret = dspd_resizebuf(ptr, newsize, oldsize);
  if ( ret )
    {
      if ( zero_all && newsize )
	memset(ret, 0, newsize);
      else if ( newsize > oldsize )
	memset((char*)ret + oldsize, 0, newsize - oldsize);
    }
  return ret;
}


void *dspd_memdup(const void *ptr, size_t len)
{
  void *ret = malloc(len);
  if ( ret )
    memcpy(ret, ptr, len);
  return ret;
}


/*
  The dspd_writev() function will keep track of the offset into the buffer list.  The
  offset argument should be initialized to 0 before the first call.
  Return values:
  dspd_writev normally returns the number of bytes written or 0 if there is no more work to do.

  -errno        An error has occurred (including non fatal ones such as EINTR)
  DSPD_IOV_EOF  A negative number that indicates end of file (operation returned 0)
  
 */
ssize_t dspd_writev(int fd, const struct iovec *iov, int niov, size_t *offset)
{
  int i;
  size_t total = 0, n;
  ssize_t ret;
  for ( i = 0; i < niov; i++ )
    {
      total += iov[i].iov_len;
      if ( total > *offset )
	{
	  n = total - *offset;
	  if ( n < iov[i].iov_len )
	    ret = write(fd, (const char*)iov[i].iov_base + n, iov[i].iov_len - n);
	  else
	    ret = writev(fd, &iov[i], niov - i);
	  if ( ret < 0 )
	    return -errno;
	  else if ( ret == 0 )
	    return DSPD_IOV_EOF;
	  (*offset) += ret;
	  return ret;
	}
    }
  return DSPD_IOV_COMPLETE;
}

ssize_t dspd_readv(int fd, const struct iovec *iov, int niov, size_t *offset)
{
  int i;
  size_t total = 0, n;
  ssize_t ret;
  for ( i = 0; i < niov; i++ )
    {
      total += iov[i].iov_len;
      if ( total > *offset )
	{
	  n = total - *offset;
	  if ( n < iov[i].iov_len )
	    ret = read(fd, (char*)iov[i].iov_base + n, iov[i].iov_len - n);
	  else
	    ret = readv(fd, &iov[i], niov - i);
	  if ( ret < 0 )
	    return -errno;
	  else if ( ret == 0 )
	    return DSPD_IOV_EOF;
	  (*offset) += ret;
	  return ret;
	}
    }
  return DSPD_IOV_COMPLETE;
}

int set_thread_name(const char *name)
{
  int ret = prctl(PR_SET_NAME, name, 0, 0, 0);
  if ( ret == -1 )
    ret = -errno;
  return ret;
}

int dspd_gettid(void)
{
  return syscall(SYS_gettid);
}

int32_t dspd_strtob(int32_t defaultvalue, const char *opt)
{
  int32_t ret = defaultvalue;
  if ( strcasecmp(opt, "on") == 0 ||
       strcasecmp(opt, "true") == 0 ||
       strcasecmp(opt, "1") == 0 ||
       strcasecmp(opt, "yes") == 0 )
    {
      ret = 1;
    } else if ( strcasecmp(opt, "off") == 0 ||
		strcasecmp(opt, "true") == 0 ||
		strcasecmp(opt, "0") == 0 ||
		strcasecmp(opt, "no") == 0 )
    {
      ret = 0;
    }
  return ret;
}

int32_t dspd_vparse_opt(int32_t defaultvalue, 
			const char *opt, va_list opts)
{
  const char *val;
  int r;
  int32_t ret = defaultvalue, n;
  while ( (val = va_arg(opts, const char*)) )
    {
      if ( ! val )
	break;
      n = va_arg(opts, int32_t);
      r = strcasecmp(opt, val);
      if ( r == 0 )
	{
	  ret = n;
	  break;
	}
    }
  return ret;
}


int32_t dspd_parse_opt(int32_t defaultvalue, 
		       const char *opt, ...)
{
  va_list opts;
  int32_t ret;
  va_start(opts, opt);
  ret = dspd_vparse_opt(defaultvalue, opt, opts);
  va_end(opts);
  return ret;
}

size_t dspd_strlen_safe(const char *str)
{
  if ( str )
    return strlen(str);
  return 0;
}

/*
  This is like strtok_r(), but it doesn't modify the input string.  The return value is a
  pointer to the next token, if any, and the length is the number of characters in the
  string.  The caller could turn in into a string:

  if ( length < sizeof(tmpbuf) )
  {
     memcpy(tmpbuf, tok, length);
     tmpbuf[length] = 0;
   } else 
   { 
     //ERROR: Too big
   }
*/
const char *dspd_strtok_c(const char *str, const char *delim, const char **saveptr, size_t *length)
{
  const char *ret = NULL;
  if ( ! str )
    str = *saveptr;
  else
    *saveptr = str;
  const char *d;
  size_t dlen = strlen(delim);
  do {
    d = strstr(str, delim);
    if ( d )
      {
	*length = (size_t)d - (size_t)str;
	*saveptr = &str[(*length)+dlen];
	ret = str;
      } else if ( str[0] == 0 )
      {
	*length = 0;
      } else
      {
	*length = strlen(str);
	if ( *length > 0 )
	  {
	    ret = str;
	    *saveptr = &str[*length];
	  }
      }
    str = *saveptr;
  } while ( *length == 0 && ret != NULL );
  return ret;
}

bool dspd_devname_cmp(const char *devname, const char *str)
{
  const char *p;
  bool ret = false;
  if ( str != NULL )
    {
      if ( str[0] == ':' )
	{
	  p = strchr(devname, ':');
	  if ( p != NULL )
	    ret = strcmp(p, str) == 0;
	} else
	{
	  ret = strcmp(devname, str) == 0;
	}
    } else
    {
      ret = true;
    }
  return ret;
}

static bool enable_assert_log = false;
void dspd_enable_assert_log(void)
{
  enable_assert_log = true;
}
void _dspd_assert(const char *expr, const char *file, unsigned int line)
{
  char buf[1024UL];
  int len, offset = 0, ret;
  int fd = -1;
  snprintf(buf, sizeof(buf), "/tmp/dspd-fail-%d.log", getpid());
  if ( enable_assert_log )
    fd = creat(buf, 0644);
  len = snprintf(buf, sizeof(buf), "%s:%u: failed assertion `%s'\n", file, line, expr);
  if ( fd >= 0 )
    {
      while ( offset < len )
	{
	  ret = write(fd, &buf[offset], len - offset);
	  if ( ret <= 0 )
	    break;
	  offset += ret;
	}
    }
  offset = 0;
  while ( offset < len )
    {
      ret = write(2, &buf[offset], len - offset);
      if ( ret <= 0 )
        break;
      offset += ret;
    }
  if ( fd != 2 )
    close(fd);
  abort();
}
