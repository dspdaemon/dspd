/*
 *  ALSACFGFILE - Simple config file for ALSA support
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

#include <unistd.h>
#include <alsa/asoundlib.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"

static struct dspd_dict *config_sections;
static int get_next_card(int it)
{
  int ret = snd_card_next(&it);
  if ( ret == 0 )
    ret = it;
  else
    ret = -1;
  return ret;
}
static char *find_card(const char *desc)
{
  int i = -1;
  snd_ctl_card_info_t *info;
  char hw[16];
  snd_ctl_t *ctl;
  const char *p;
  char *ret = NULL;
  if ( snd_ctl_card_info_malloc(&info) != 0 )
    return NULL;
  for ( i = get_next_card(i); i >= 0; i = get_next_card(i) )
    {
      sprintf(hw, "hw:%d", i);
      if ( snd_ctl_open(&ctl, hw, 0) == 0 )
	{
	  if ( snd_ctl_card_info(ctl, info) == 0 )
	    {
	      p = snd_ctl_card_info_get_name(info);
	      if ( p )
		{
		  if ( strcmp(p, desc) == 0 )
		    {
		      ret = strdup(hw);
		      snd_ctl_close(ctl);
		      break;
		    }
		}
	    }
	  snd_ctl_close(ctl);
	}
    }
  return ret;
}

static void trigger_hotplug_events(void *arg)
{
  struct dspd_dict *curr;
  int ret;
  char *p;
  char *val;
  bool c;
  char eid[32UL], *e;
  dspd_log(0, "mod_alsacfg: Triggering hotplug events");
  for ( curr = config_sections; curr; curr = curr->next )
    {
      if ( ! dspd_dict_find_value(curr, DSPD_HOTPLUG_EVENT_ID, &e) )
	{
	  dspd_daemon_hotplug_event_id(eid);
	  if ( ! dspd_dict_insert_value(curr, DSPD_HOTPLUG_EVENT_ID, eid) )
	    continue;
	}
      if ( ! dspd_dict_find_value(curr, DSPD_HOTPLUG_DEVNAME, &p) )
	continue;
      if ( ! p )
	continue;
      if ( strcmp(p, "${description}") == 0 )
	{
	  if ( ! dspd_dict_find_value(curr, DSPD_HOTPLUG_DESC, &p) )
	    continue;
	  if ( ! p )
	    continue;
	  val = find_card(p);
	  if ( val )
	    {
	      c = dspd_dict_set_value(curr, DSPD_HOTPLUG_DEVNAME, val, true);
	      free(val);
	      if ( ! c )
		continue;
	    }
	}

      
      char *desc = NULL;
      dspd_dict_find_value(curr, DSPD_HOTPLUG_DESC, &desc);
      dspd_log(0, "mod_alsacfg: Adding '%s'...", desc);
      ret = dspd_daemon_hotplug_add(curr);
      dspd_log(0, "mod_alsacfg: result is %d.", ret);
    }
}

static int cfgfile_init(struct dspd_daemon_ctx *daemon, void **context)
{

  config_sections = dspd_read_config("mod_alsacfgfile", true);
  
  if ( config_sections )
    dspd_log(0, "mod_alsacfg: Parsed sections for alsa devices");
  else
    dspd_log(0, "mod_alsacfg: No sections found for alsa");
  if ( config_sections )
    dspd_daemon_register_startup(trigger_hotplug_events, NULL);

  return 0;
}

static void cfgfile_close(struct dspd_daemon_ctx *daemon, void **context)
{
  
}


struct dspd_mod_cb dspd_mod_alsacfgfile = {
  .init_priority = DSPD_MOD_INIT_PRIO_HOTPLUG,
  .desc = "ALSA PCM Simple Config File",
  .init = cfgfile_init,
  .close = cfgfile_close,
};
