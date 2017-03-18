/*
  Some ideas about how to split controls:

  1.  Find channel numbers that are consecutive and join them into
  a stereo control.

  2.  switch and switch-joined together means that channels are muted
  as all or nothing.

  3.  All numerical ranges should be normalized.  So, -123 to 0 would become
  0 to 123.  It looks like volume ranges are best looked at using abs()
  because what really matters isn't the absolute value, but where it is
  between min and max.  Treating it as an unsigned int would just as
  easily work.

  Get: 0 - currentval
  Set: min+newval
  


  4.  This should be done with the idea of mapping directly to alsamixer
  user interface.

  5.  Playback+capture in a single control should be split out.

  6.  These controls should take an index range starting at 65536 or
  another set of ioctls.
  
  7.  Possible ranges (everything else is mapped):
  0-INT32_MAX mono
  0-INT16_MAX mono
  0-INT16_MAX stereo
  0-UINT8_MAX mono
  0-UINT8_MAX stereo

  8.  In some cases a mapped value will have to "top out" or "bottom out"
  at a slightly different value.  Basically, if the input is max or min,
  then don't apply math.  If the input is in the middle then use math.

  9.  The entry point could be something like reserving 2 bits of the
  element id.  1 bit will be EXPANDED and another is for SIMPLIFIED.
  I think using an input argument to the ELEM_COUNT command might
  make it possible to find out control values.  This whole thing
  could even put simplified and expaned controls into the same
  address range then mark where each begins and only one bit
  will be used.  So, use the sign bit (negative) to redirect
  to the mapped controls.  The return value of ELEM_COUNT would
  be 2 int16_t that represent the start addresses.  If -1 then
  does not exist.

  10.  Should be able to get the real element by setting the sign bit,
  so element 1 becomes -1.

 */


#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sched.h>
#define _DSPD_CTL_MACROS
#include "sslib.h"
#include "daemon.h"

struct ctlmap_elem {
  uint32_t  element; //Index of real element
  uint32_t  index;   //Index of this element
  char      name[32]; //Generated name
  uint32_t  flags;
  uint32_t  type;
  uint32_t  chmask;
  uint32_t  maxval;
  struct dspd_mix_range real_range;
  struct dspd_mix_info info;
  struct dspd_mix_val  val;
  struct ctlmap_elem *next;
};



struct dspd_ctlmap {
  int32_t (*ioctl)(void *arg,
		   int32_t req,
		   const void *inbuf,
		   size_t inbufsize,
		   void *outbuf,
		   size_t outbufsize,
		   size_t *bytes_returned);
  void *arg;
  pthread_mutex_lock lock;
  struct dspd_mix_val mixval;
  struct ctlmap_elem *elements;
  uint32_t elements_count;

};

