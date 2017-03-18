#include <string.h>
#include "sslib.h"


static const char *dspd_mixf_names[] = {
  "Playback",
  "Capture",
  "Playback mono",
  "Capture mono",
  "Playback switch",
  "Capture switch",
  "Capture dB",
  "Playback dB",
  "Common switch",
  "Common volume",
  "Playback volume joined",
  "Capture volume joined",
  "Playback switch joined",
  "Capture switch joined",
  "Capture switch exclusive",
  "Enumerated",
  "Playback enumerated",
  "Capture enumerated",
};

void dspd_mixf_dump(uint64_t mask)
{
  static const size_t count = sizeof(dspd_mixf_names) / sizeof(dspd_mixf_names[0]);
  size_t maxidx = count - 1;
  size_t i, n = 0;
  for ( i = 0; i < 64; i++ )
    {
      if ( mask & (1ULL<<(uint64_t)i) )
	{
	  if ( n > 0 )
	    fputc(',', stderr);
	  if ( i > maxidx )
	    fprintf(stderr, "bit%ld", (long)i);
	  else
	    fputs(dspd_mixf_names[i], stderr);
	  n++;
	}
    }
  fprintf(stderr, "\n");
}

size_t dspd_mixf_getname(size_t index, char *name, size_t len)
{
  size_t ret;
  if ( index >= ARRAY_SIZE(dspd_mixf_names) )
    {
      ret = snprintf(name, len, "bit%ld", (long)index);
    } else
    {
      strlcpy(name, dspd_mixf_names[index], len);
      ret = strlen(dspd_mixf_names[index]);
    }
  return ret;
}
