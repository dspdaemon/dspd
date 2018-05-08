/*
  PCM_DEVEL - ALSA PCM IOPLUG DEVELOPMENT MODULE
  
  This module loads another pcm io plugin from an arbitrary location so
  that a module can be quickly built and tested.

  The following environment variables affect the operation of this plugin:

  SND_PCM_DEVEL_LOAD:  The path and name of the library to load with dlopen()
  SND_PCM_DEVEL_DEBUG: Enable debugging to stderr
  

*/

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



#define _GNU_SOURCE

#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
SND_PCM_PLUGIN_DEFINE_FUNC(devel)
{
  const char *solib = getenv("SND_PCM_DEVEL_LOAD");
  int (*fptr)(snd_pcm_t **pcmp, const char *name,		       
	      snd_config_t *root, snd_config_t *conf,			
	      snd_pcm_stream_t stream, int mode);
  const char *ptr;
  char *p;
  char str[256];
  char fname[256];
  static void *handle = NULL;
  int debug = 0;
  long ul;
  snd_config_iterator_t cfgi, next;
  p = getenv("SND_PCM_DEVEL_DEBUG");
  if ( p )
    debug = atoi(p);
  if ( debug )
    fprintf(stderr, "pcm_devel: Loaded dspd pcm development plugin\n");
  
  snd_config_for_each(cfgi, next, conf) 
    {
      snd_config_t *it = snd_config_iterator_entry(cfgi);
      const char *key;
      if (snd_config_get_id(it, &key) < 0)
	continue;
      if ( strcmp(key, "lib") == 0 && solib == NULL )
	{
	  snd_config_get_string(it, &solib);
	} else if ( strcmp(key, "debug") == 0 )
	{
	  if ( snd_config_get_integer(it, &ul) == 0 )
	    debug = ul;
	}
    }

  if ( ! solib )
    {
      fprintf(stderr, "pcm_devel: No library specified\n");
      return -EINVAL;
    }

  if ( ! handle )
    {
#if SND_LIB_MINOR == 0 
      handle = snd_dlopen(solib, RTLD_NOW);
      if ( ! handle )
	{
	  if ( debug )
	    fprintf(stderr, "pcm_devel: dlopen: %s\n", dlerror());
	  return -ELIBACC;
	}
#else 
      /*
	This is possibly incorrect for some 1.1.x versions.
      */
      char ebuf[256] = { 0 };
      handle = snd_dlopen(solib, RTLD_NOW, ebuf, sizeof(ebuf));
      if ( ! handle )
	{
	  if ( debug )
	    fprintf(stderr, "pcm_devel: dlopen: %s\n", ebuf);
	  return -ELIBACC;
	}
#endif
    }

  const char soname[] = "libasound_module_pcm_";
  if ( strncmp(solib, soname, sizeof(soname)-1) == 0 )
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
	    fprintf(stderr, "pcm_devel: Could not parse library location\n");
	  return -EINVAL;
	}
    } else
    {
      if ( debug )
	fprintf(stderr, "pcm_devel: Invalid library name\n");
      return -EINVAL;
    }
  strcpy(fname, &str[21]);
  p = strchr(fname, '.');
  if ( ! p )
    {
      if ( debug )
	fprintf(stderr, "pcm_devel: Could not determine symbol name for library\n");
      return -EINVAL;
    }
  *p = 0;
  strcpy(str, fname);
  sprintf(fname, "_snd_pcm_%s_open", str);
  fptr = dlsym(handle, fname);
  if ( ! fptr )
    {
      if ( debug )
	fprintf(stderr, "pcm_devel: dlopen: %s\n", dlerror());
      return -ENOSYS;
    }
  return fptr(pcmp, name, root, conf, stream, mode);
}

SND_PCM_PLUGIN_SYMBOL(devel);
