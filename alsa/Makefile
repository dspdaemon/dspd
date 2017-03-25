include ../config.makefile
include ../rules.makefile
LIBS :=-lrt -lasound
#CC=gcc
#CFLAGS=-ggdb -O0 -pthread $(XCFLAGS)
APCFLAGS=-Wall -fPIC -DPIC $(CFLAGS)
APLDFLAGS=-shared -lm -ldl -lpthread -lasound -lrt \
	-module -avoid-version -export-dynamic -ldspdc -L../lib
MAKEOBJ=$(CC) $(APCFLAGS) -c


all: devel dspd

skel:
	$(MAKEOBJ) pcm_skel.c
	$(CC) $(APLDFLAGS) $(APCFLAGS) -o libasound_module_pcm_skel.so \
	pcm_skel.o

devel:
	$(MAKEOBJ) pcm_devel.c
	$(CC) -rdynamic -shared $(CFLAGS) $(LIBS) -pthread -o libasound_module_pcm_devel.so \
	pcm_devel.o



dspd:
	$(MAKEOBJ) pcm_dspd.c
	$(CC) $(APCFLAGS) $(LIBS) -rdynamic -shared -ldspdc -L../lib -o libasound_module_pcm_dspd.so \
	pcm_dspd.o

install:
	mkdir -p $(REAL_LIBDIR)/alsa-lib
	install -m555 libasound_module_pcm_dspd.so $(REAL_LIBDIR)/alsa-lib/

uninstall:
	-rm -f $(REAL_LIBDIR)/alsa-lib/libasound_module_pcm_dspd.so

clean:
	-rm -f *.o *.so