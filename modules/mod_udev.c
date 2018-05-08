/*
 *   UDEV - udev hotplug support for ALSA
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
#include <libudev.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <atomic_ops.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include "../lib/sslib.h"
#include "../lib/daemon.h"

/*STREAMS=0x3
GOT DEVICE 0x23a1db0
ACTION:    add
CARD:      card2
SUBSYSTEM: 9-2:1.0
MODALIAS:  usb:v041Ep3040d0100dc00dsc00dp00ic01isc01ip00in00
DRIVER:    snd-usb-audio
DEVICE:    (null)
DEVICE ID: usb@041e:3040

STREAMS=0x3
GOT DEVICE 0x23ba6e0
ACTION:    add
CARD:      card0
SUBSYSTEM: 0000:00:14.2
MODALIAS:  pci:v00001002d00004383sv00001849sd00007892bc04sc03i00
DRIVER:    snd_hda_intel
DEVICE:    0x4383
DEVICE ID: pci@0x1849:0x7892

STREAMS=0x3
GOT DEVICE 0x23bd250
ACTION:    add
CARD:      card1
SUBSYSTEM: 0000:07:06.0
MODALIAS:  pci:v0000125Dd00001988sv0000125Dsd00001988bc04sc01i00
DRIVER:    snd_maestro3
DEVICE:    0x1988
DEVICE ID: pci@0x125d:0x1988


STREAMS=0x0
GOT DEVICE 0x23b1d70
ACTION:    remove
CARD:      card2
SUBSYSTEM: usb9
MODALIAS:  (null)
DRIVER:    usb
DEVICE:    (null)
DEVICE ID: unknown@unknown


STREAMS=0x3
GOT DEVICE 0x23b1850
ACTION:    add
CARD:      card2
SUBSYSTEM: 9-2:1.0
MODALIAS:  usb:v041Ep3040d0100dc00dsc00dp00ic01isc01ip00in00
DRIVER:    snd-usb-audio
DEVICE:    (null)
DEVICE ID: usb@041e:3040*/


struct hotplug_ctx {
  bool (*device_event)(struct hotplug_ctx *ctx, struct udev_device *dev, bool newdev);
  bool (*uevent)(struct hotplug_ctx *ctx);
  bool (*connect)(struct hotplug_ctx *ctx);
  void (*disconnect)(struct hotplug_ctx *ctx);
  volatile AO_t abort;
  bool connected;
  struct udev *udev;
  struct udev_monitor *mon;
  pthread_t thread;
  int thread_result;
  bool nousb;
  bool fullduplex;
  char *pcmprefix;
  char *ctlprefix;
};


static bool get_usb_id(struct udev_device *dev, char dev_id[64])
{
  const char *str, *vendor, *product;
  dev = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");
  if ( ! dev )
    return false;
  str = udev_device_get_sysattr_value(dev, "device");
  if ( str )
    {
      strlcpy(dev_id, str, 64);
      return false;
    }
  vendor = udev_device_get_sysattr_value(dev, "idVendor");
  product = udev_device_get_sysattr_value(dev, "idProduct");
  if ( ! (vendor && product) )
    {
      dev = udev_device_get_parent(dev);
      if ( ! dev )
	return false;
      dev = udev_device_get_parent(dev);
      if ( ! dev )
	return false;
      vendor = udev_device_get_sysattr_value(dev, "idVendor");
      product = udev_device_get_sysattr_value(dev, "idProduct");
    }
  if ( vendor && product )
    {
      snprintf(dev_id, 64, "%s:%s", vendor, product);
      return true;
    }
  return false;
}

static bool get_pci_id(struct udev_device *dev, char dev_id[64])
{
  const char *vendor, *device;
  bool ret = false;
  vendor = udev_device_get_sysattr_value(dev, "subsystem_vendor");
  device = udev_device_get_sysattr_value(dev, "subsystem_device");
  if ( vendor && device )
    {
      snprintf(dev_id, 64, "%s:%s", vendor, device);
      ret = true;
    } 
  return ret;
}

static void get_device_id(struct udev_device *dev, char bus[16], char dev_id[64])
{
  const char *modalias, *p;
  modalias = udev_device_get_sysattr_value(dev, "modalias");
  if ( modalias )
    {
      p = strchr(modalias, ':');
      if ( p != NULL && ((size_t)p - (size_t)modalias) < 16 )
	{
	  strlcpy(bus, modalias, ((size_t)p - (size_t)modalias) + 1);
	} else
	{
	  modalias = NULL;
	}
    }
  if ( get_pci_id(dev, dev_id) )
    {
      if ( ! modalias )
	strcpy(bus, "pci");
    } else if ( get_usb_id(dev, dev_id) )
    {
      if ( ! modalias )
	strcpy(bus, "usb");
    } else
    {
      if ( ! modalias )
	strcpy(bus, "none");
      strcpy(dev_id, "unknown");
    }
}

/*
  This isn't the most correct thing.  All it does is see what subdevice 0 is capable of.
  Devices can be more complicated than this.
*/
static int get_info(const char *card, char desc[128], const char *prefix)
{
  char hwname[16];
  snd_ctl_t *handle = NULL;
  int streams = 0;
  snd_pcm_info_t *info;
  snd_ctl_card_info_t *cinfo;
  int dev = -1, err;
  const char *name;
  snd_pcm_info_alloca(&info);
  snd_ctl_card_info_alloca(&cinfo);
  if ( snprintf(hwname, sizeof(hwname), "%s:%s", prefix, &card[4]) >= sizeof(hwname) )
    goto out;

  if ( snd_ctl_open(&handle, hwname, 0) < 0 )
    goto out;
  if ( snd_ctl_card_info(handle, cinfo) < 0 )
    goto out;
  name = snd_ctl_card_info_get_name(cinfo);
  if ( ! name )
    goto out;
  
  if ( snd_ctl_pcm_next_device(handle, &dev) < 0 )
    goto out;
  if ( dev < 0 )
    goto out;



  snd_pcm_info_set_device(info, dev);
  snd_pcm_info_set_subdevice(info, 0);
  snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);
  err = snd_ctl_pcm_info(handle, info);
  if ( err == 0 )
    streams |= DSPD_PCM_SBIT_PLAYBACK;
  

  snd_pcm_info_set_device(info, dev);
  snd_pcm_info_set_subdevice(info, 0);
  snd_pcm_info_set_stream(info, SND_PCM_STREAM_CAPTURE);
  err = snd_ctl_pcm_info(handle, info);
  if ( err == 0 )
    streams |= DSPD_PCM_SBIT_CAPTURE;
  strlcpy(desc, name, 128);

 out:
  if ( handle )
    snd_ctl_close(handle);
  return streams;
}

static bool insert_value(struct dspd_dict *dict, const char *key, const char *value)
{
  bool ret;
  if ( value )
    ret = dspd_dict_insert_value(dict, key, value);
  else
    ret = false;
  if ( ret == true )
    {
      assert(value);
    }
  return ret;
}

static const char *stream_name(int s)
{
  const char *ret;
  if ( s == DSPD_PCM_SBIT_FULLDUPLEX )
    ret = "fullduplex";
  else if ( s == DSPD_PCM_SBIT_PLAYBACK )
    ret = "playback";
  else if ( s == DSPD_PCM_SBIT_CAPTURE )
    ret = "capture";
  else
    ret = NULL;
  return ret;
}

static bool udev_device_event(struct hotplug_ctx *ctx, struct udev_device *dev, bool newdev)
{
  const char *str, *action, *card, *modalias, *driver;
  struct udev_device *pdev = udev_device_get_parent(dev);
  int streams, ret;
  char hwname[32];
  char desc[128];
  struct dspd_dict *dict;
  char busid[16] = { 0 }, hwid[64] = { 0 };
  char eid[32UL];
  int s;
  if ( ! pdev )
    {
      str = udev_device_get_sysname(dev);
      if ( strncmp(str, "card", 4) == 0 )
	pdev = dev;
      else
	return true;
    }
  if ( newdev )
    {
      action = udev_device_get_action(dev);
      if ( ! action )
	return true;
      if ( strcmp(action, "change") == 0 )
	{
	  action = "add";
	} else if ( strcmp(action, "remove") != 0 )
	{
	  return true;
	}
      str = udev_device_get_sysname(dev);
      if ( ! str )
	return true;
      if ( strncmp(str, "card", 4) != 0 )
	return true;
      card = str;
    } else
    {
      if ( udev_device_get_sysattr_value(pdev, "modalias") == NULL && pdev != dev )
	return true;
      str = udev_device_get_sysname(dev);
      if ( ! str )
	return true;
      if ( strncmp(str, "card", 4) != 0 )
	return true;
      card = str;
      action = "add";
    }
  if ( snprintf(hwname, sizeof(hwname), "%s:%s", ctx->pcmprefix, &card[4]) >= sizeof(hwname) )
    return true;
  dspd_log(0, "Got event '%s' for '%s", action, card);
  if ( strcmp(action, "remove") != 0 )
    {
      streams = get_info(card, desc, ctx->ctlprefix);
      if ( streams == 0 )
	return true;
      
    } else
    {
      ret = dspd_daemon_hotplug_remove(NULL, hwname);
      if ( ret == 0 )
	dspd_log(0, "Removed device %s", hwname);
      else
	dspd_log(0, "Could not remove device %s: error %d", hwname, ret);
      return true;
    }
  

  get_device_id(pdev, busid, hwid);
  if ( ctx->nousb && strcmp(busid, "usb") == 0 )
    return true;
  
  dict = dspd_dict_new("DEVICE");
  if ( ! dict )
    return true;

  if (  ctx->fullduplex == false && (streams & DSPD_PCM_SBIT_FULLDUPLEX) )
    s = DSPD_PCM_SBIT_PLAYBACK;
  else
    s = streams;

  //This may not be entirely correct.  Some devices seem to be missing sysfs attributes, but
  //they still exist in ALSA.
  modalias = udev_device_get_sysattr_value(pdev, "modalias");
  if ( modalias == NULL && pdev == dev )
    modalias = "none";
  driver = udev_device_get_driver(pdev);
  if ( driver == NULL && pdev == dev )
    driver = "builtin";

  (void)dspd_daemon_hotplug_event_id(eid);

  if ( (insert_value(dict, DSPD_HOTPLUG_EVENT_ID, eid) &&
	insert_value(dict, DSPD_HOTPLUG_EVENT, action) &&
	insert_value(dict, DSPD_HOTPLUG_BUSNAME, busid) &&
	insert_value(dict, DSPD_HOTPLUG_DEVTYPE, "alsa") &&
	insert_value(dict, DSPD_HOTPLUG_SENDER, "mod_udev") &&
	insert_value(dict, DSPD_HOTPLUG_DESC, desc) &&
	insert_value(dict, DSPD_HOTPLUG_DEVNAME, hwname) &&
	insert_value(dict, DSPD_HOTPLUG_ADDRESS, udev_device_get_sysname(pdev)) &&
	insert_value(dict, DSPD_HOTPLUG_KDRIVER, driver) &&
	insert_value(dict, DSPD_HOTPLUG_HWID, hwid) && 
	insert_value(dict, DSPD_HOTPLUG_MODALIAS, modalias) &&
	insert_value(dict, DSPD_HOTPLUG_STREAM, stream_name(s))))
    {
      ret = dspd_daemon_hotplug_add(dict);
      if ( ret > 0 )
	dspd_log(0, "mod_udev: Added %s (%s) for stream %s,0x%x to slot %d", hwname, desc, stream_name(s), streams, ret);
      else
	dspd_log(0, "mod_udev: Failed to add %s (%s): error %d", hwname, desc, ret);
      
    } else
    {
      dspd_log(0, "mod_udev: Missing data for event %s device %s (%s)", action, hwname, desc);
    }
  if (  ctx->fullduplex == false && (streams & DSPD_PCM_SBIT_FULLDUPLEX) )
    {
      if ( dspd_dict_set_value(dict, DSPD_HOTPLUG_STREAM, stream_name(DSPD_PCM_SBIT_CAPTURE), false) )
	{
	  ret = dspd_daemon_hotplug_add(dict);
	  if ( ret > 0 )
	    dspd_log(0, "mod_udev: Added %s (%s) for stream %s,0x%x to slot %d", hwname, desc, stream_name(DSPD_PCM_SBIT_CAPTURE), streams, ret);
	  else
	    dspd_log(0, "mod_udev: Failed to add %s (%s): error %d", hwname, desc, ret);
	}
    }
  

  dspd_dict_free(dict);

  return true;
}


static void udev_disconnect(struct hotplug_ctx *ctx)
{
  if ( ctx->udev )
    {
      udev_unref(ctx->udev);
      ctx->udev = NULL;
    }
  if ( ctx->mon )
    {
      udev_monitor_unref(ctx->mon);
      ctx->mon = NULL;
    }
  ctx->connected = false;
}

static bool process_entry(struct hotplug_ctx *ctx, struct udev_list_entry *le)
{
  const char *path;
  struct udev_device *dev;
  bool ret = true;
  path = udev_list_entry_get_name(le);
  if ( path )
    {
      dev = udev_device_new_from_syspath(ctx->udev, path);
      if ( dev )
	ret = ctx->device_event(ctx, dev, false);
    }
  return ret;
}

static bool udev_connect(struct hotplug_ctx *ctx)
{
  struct udev_enumerate *enumerate = NULL;
  struct udev_list_entry *devices, *dev_list_entry;
  ctx->udev = udev_new();
  if ( ! ctx->udev )
    goto error;

  enumerate = udev_enumerate_new(ctx->udev);
  if ( ! enumerate )
    goto error;

  if ( udev_enumerate_add_match_subsystem(enumerate, "sound") < 0 )
    goto error;
  if ( udev_enumerate_scan_devices(enumerate) < 0 )
    goto error;

  devices = udev_enumerate_get_list_entry(enumerate);
  if ( devices )
    {
      udev_list_entry_foreach(dev_list_entry, devices) 
	{
	  if ( ! process_entry(ctx, dev_list_entry) )
	    goto error;
	}
    }
  

  udev_enumerate_unref(enumerate);
  enumerate = NULL;

  ctx->mon = udev_monitor_new_from_netlink(ctx->udev, "udev");
  if ( ! ctx->mon )
    goto error;
  if ( udev_monitor_filter_add_match_subsystem_devtype(ctx->mon, "sound", NULL) < 0 )
    goto error;
  if ( udev_monitor_enable_receiving(ctx->mon) < 0 )
    goto error;
  
  return true;
 error:
  if ( enumerate )
    udev_enumerate_unref(enumerate);
  ctx->disconnect(ctx);
  return false;
}

static bool udev_uevent(struct hotplug_ctx *ctx)
{
  struct udev_device *dev = udev_monitor_receive_device(ctx->mon);
  bool ret;
  if ( dev )
    {
      ret = ctx->device_event(ctx, dev, true);
      udev_device_unref(dev);
    } else
    {
      ret = true;
    }
  return ret;
}

static void *udev_thread(void *p)
{
  struct hotplug_ctx *ctx = p;
  struct pollfd pfd;
  int ret;
  while ( ! AO_load(&ctx->abort) )
    {
      
      if ( ! ctx->connected )
	{
	  ctx->connected = ctx->connect(ctx);
	  if ( ! ctx->connected )
	    {
	      if ( ! AO_load(&ctx->abort) )
		usleep(250000);
	      continue;
	    } else
	    {
	      pfd.fd = udev_monitor_get_fd(ctx->mon);
	      pfd.events = POLLIN;
	      pfd.revents = 0;
	      if ( pfd.fd < 0 )
		ctx->disconnect(ctx);
	    }
	}
      if ( ctx->connected )
	{
	  ret = poll(&pfd, 1, -1);
	 
	  if ( ret < 0 && errno != EAGAIN && errno != EINTR )
	    {
	      ctx->disconnect(ctx);
	    } else if ( ret > 0 && (pfd.revents & POLLIN) )
	    {
	      if ( ! ctx->uevent(ctx) )
		ctx->disconnect(ctx);
	      else if ( udev_monitor_enable_receiving(ctx->mon) < 0 )
		ctx->disconnect(ctx);
	      //fprintf(stderr, "FDS %d %d\n", pfd.fd, udev_monitor_get_fd(ctx->mon));
	    }
	  //fprintf(stderr, "REVENTS %d\n", pfd.revents);
	}
    }
  ctx->disconnect(ctx);
  return NULL;
}

static struct hotplug_ctx hpctx = {
  .device_event = udev_device_event,
  .uevent = udev_uevent,
  .connect = udev_connect,
  .disconnect = udev_disconnect,
  
};
static void start_udev_thread(void *arg)
{
  struct hotplug_ctx *ctx = arg;
  ctx->thread_result = pthread_create(&ctx->thread, NULL, udev_thread, arg);
}

static int udev_init(void *daemon, void **context)
{
  struct dspd_dict *cfg;
  char *p;
  hpctx.thread_result = -1;

  cfg = dspd_read_config("mod_udev", true);

  if ( cfg )
    {
      if ( dspd_dict_find_value(cfg, "nousb", &p) )
	hpctx.nousb = !! atoi(p);
	
      if ( dspd_dict_find_value(cfg, "fullduplex", &p) )
	hpctx.fullduplex = !!atoi(p);

      if ( dspd_dict_find_value(cfg, "pcmprefix", &p) )
	hpctx.pcmprefix = strdup(p);
	
      
      if ( dspd_dict_find_value(cfg, "ctlprefix", &p) )
	hpctx.ctlprefix = strdup(p);
	
      dspd_dict_free(cfg);
    }
  if ( hpctx.ctlprefix == NULL )
    hpctx.ctlprefix = strdup("hw");
  if ( hpctx.pcmprefix == NULL )
    hpctx.pcmprefix = strdup("hw");
  

  dspd_daemon_register_startup(start_udev_thread, &hpctx);
  return 0;
}

static void udev_close(void *daemon, void **context)
{
  if ( hpctx.thread_result == 0 )
    {
      AO_store(&hpctx.abort, 1);
      pthread_join(hpctx.thread, NULL);
    }
}


struct dspd_mod_cb dspd_mod_udev = {
  .init_priority = DSPD_MOD_INIT_PRIO_HOTPLUG + 1U,
  .desc = "ALSA PCM UDEV Hotplug",
  .init = udev_init,
  .close = udev_close,
};
