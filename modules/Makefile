include ../config.makefile
include ../rules.makefile

MLIBS=$(LIBS) -ldspdc -ldspds


all: $(MODULES)

clean:
	-rm -f *.o *.so osscuse_cdev_helper

distclean:
	-rm -f *.o *.so \#* *~ osscuse_cdev_helper

none:
	/bin/true

alsahw:
	$(CC) $(CFLAGS) $(MLIBS) -lasound -shared -o mod_alsahw.so mod_alsahw.c alsahw_notify.c


alsacfgfile:
	$(CC) $(CFLAGS) $(MLIBS) -lasound -shared -o mod_alsacfgfile.so mod_alsacfgfile.c

socketserver:
	$(CC) $(CFLAGS) $(MLIBS) -shared -o mod_socketserver.so mod_socketserver.c

speex:
	$(CC) $(CFLAGS) $(MLIBS) -lspeexdsp -shared -o mod_speexsrc.so mod_speexsrc.c

libsamplerate:
	$(CC) $(CFLAGS) $(MLIBS) -lsamplerate -shared -o mod_libsamplerate.so mod_libsamplerate.c

osscuse:
	$(CC) $(CFLAGS) $(MLIBS) -shared -o mod_osscuse.so mod_osscuse.c io_osscuse.c rtcuse.c
	$(CC) $(CFLAGS) $(MLIBS) -o osscuse_cdev_helper osscuse_cdev_helper.c rtcuse.c
udev:
	$(CC) $(CFLAGS) $(MLIBS) -lasound -ludev -shared -o mod_udev.so mod_udev.c

aiotest:
	$(CC) $(CFLAGS) $(MLIBS) -shared -o mod_aiotest.so mod_aiotest.c


install:
	mkdir -p -m755 $(MODDIR)
	install -m555 *.so $(MODDIR)
	test -e osscuse_cdev_helper && install -m555 osscuse_cdev_helper $(MODDIR)

uninstall:
	-rm -R -f -- $(MODDIR)/

