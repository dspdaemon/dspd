include ../config.makefile
include ../rules.makefile


OBJECTS=modules.o util.o cfgread.o objlist.o mbx.o shm.o fifo.o \
	pcm.o scheduler.o device.o client.o dspd_time.o log.o req.o \
	daemon.o rclient.o cbpoll.o socket.o ssclient.o chmap.o src.o \
	rtalloc.o wq.o syncgroup.o mixer.o dspdaio.o

DSPDS_OBJ=client.o daemon.o device.o log.o modules.o \
	rtalloc.o syncgroup.o wq.o scheduler.o

DSPDC_OBJ=util.o cfgread.o mbx.o shm.o fifo.o \
	pcm.o dspd_time.o req.o rclient.o cbpoll.o socket.o ssclient.o \
	chmap.o objlist.o src.o mixer.o dspdaio.o

all: $(OBJECTS) solib

dspdc:
	$(CC) $(LIBS) $(CFLAGS) -shared -o libdspdc.so $(DSPDC_OBJ)
dspds:
	$(CC) $(LIBS) $(CFLAGS) -shared -o libdspds.so $(DSPDS_OBJ)

solib: dspdc dspds
#	$(CC) $(LIBS) $(OBJECTS) $(CFLAGS) -shared -o libsoundserver.so

clean:
	-rm -f *.so *.o

distclean:
	-rm -f *.so *.o *~ \#*

install:
	mkdir -p $(REAL_LIBDIR)
	install -m555 libdspds.so $(REAL_LIBDIR)/libdspds.so
	install -m555 libdspdc.so $(REAL_LIBDIR)/libdspdc.so

uninstall:
	rm -f $(REAL_LIBDIR)/libsoundserver.so
