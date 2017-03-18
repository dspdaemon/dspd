/*
 *  
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

/*
  ALSA PCM IOPLUG DEVELOPMENT MODULE
  
  This module loads another pcm io plugin from an arbitrary location so
  that a module can be quickly built and tested.
*/

#define _GNU_SOURCE

#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <samplerate.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <dlfcn.h>

SND_PCM_PLUGIN_DEFINE_FUNC(devel)
{
  const char *solib = getenv("SND_PCM_DEVEL_LOAD");
  int (*fptr)(snd_pcm_t **pcmp, const char *name,		       
	      snd_config_t *root, snd_config_t *conf,			
	      snd_pcm_stream_t stream, int mode);
  const char *ptr;
  char *p;
  if ( ! solib )
    return -EINVAL;
  char str[256];
  char fname[256];
  static void *handle = NULL;
  int debug = 0;
  p = getenv("SND_PCM_DEVEL_DEBUG");
  if ( p )
    debug = atoi(p);
  if ( debug )
    fprintf(stderr, "Loaded dspd pcm development plugin\n");
  if ( ! handle )
    {
      //fprintf(stderr, "dlopen(%s)\n", solib);
      handle = dlopen(solib, RTLD_NOW);
      if ( ! handle )
	{
	  if ( debug )
	    fprintf(stderr, "DLOPEN: %s\n", dlerror());
	  return -ELIBACC;
	}
    }

  if ( strcmp(solib, "libasound_module_pcm_") == 0 )
    {
      strcpy(str, solib);
    } else if ( (ptr = strstr(solib, "/libasound_module_pcm_")) )
    {
      if ( strchr(&ptr[1], '/') == NULL && strstr(ptr, ".so") != NULL )
	{
	  strcpy(str, &ptr[1]);
	} else
	{
	  if ( debug )
	    fprintf(stderr, "Could not parse library location\n");
	  return -EINVAL;
	}
    } else
    {
      if ( debug )
	fprintf(stderr, "Invalid library name\n");
      return -EINVAL;
    }
  strcpy(fname, &str[21]);
  p = strchr(fname, '.');
  if ( ! p )
    {
      if ( debug )
	fprintf(stderr, "Could not determine symbol name for library\n");
      return -EINVAL;
    }
  *p = 0;
  strcpy(str, fname);
  sprintf(fname, "_snd_pcm_%s_open", str);
  fptr = dlsym(handle, fname);
  if ( ! fptr )
    {
      if ( debug )
	fprintf(stderr, "DLOPEN: %s\n", dlerror());
      return -ENOSYS;
    }
  return fptr(pcmp, name, root, conf, stream, mode);
}

SND_PCM_PLUGIN_SYMBOL(devel);
