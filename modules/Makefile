include ../config.makefile
include ../rules.makefile

MLIBS=$(LIBS) -ldspdc -ldspds


all: $(MODULES)

clean:
	-rm -f *.o *.so

distclean:
	-rm -f *.o *.so \#* *~

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

udev:
	$(CC) $(CFLAGS) $(MLIBS) -lasound -ludev -shared -o mod_udev.so mod_udev.c




install:
	mkdir -p -m755 $(MODDIR)
	install -m555 *.so $(MODDIR)

uninstall:
	rm -R -f -- $(MODDIR)/

