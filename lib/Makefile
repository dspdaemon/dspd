include ../config.makefile
include ../rules.makefile



MAKEBIN=$(CC) $(LIBS) $(CFLAGS) -L. -ldspds -ldspdc -Wl,-rpath=./

%.bin: %.c
	$(MAKEBIN) -o $@ $<

TESTPROGS=test_chmap.bin test_playback.bin

OBJECTS=modules.o util.o cfgread.o objlist.o mbx.o shm.o fifo.o \
	pcm.o scheduler.o device.o client.o dspd_time.o log.o req.o \
	daemon.o rclient.o cbpoll.o socket.o ssclient.o chmap.o src.o \
	rtalloc.o wq.o syncgroup.o mixer.o dspdaio.o vctrl.o pcmcli_stream.o \
	pcmcli.o ctlcli.o

DSPDS_OBJ=client.o daemon.o device.o log.o modules.o \
	rtalloc.o syncgroup.o wq.o scheduler.o vctrl.o

DSPDC_OBJ=util.o cfgread.o mbx.o shm.o fifo.o \
	pcm.o dspd_time.o req.o rclient.o cbpoll.o socket.o ssclient.o \
	chmap.o objlist.o src.o mixer.o dspdaio.o pcmcli_stream.o pcmcli.o \
	ctlcli.o

all: $(OBJECTS) solib

dspdc:
	$(CC) $(LIBS) $(CFLAGS) -shared -o libdspdc.so $(DSPDC_OBJ)
dspds:
	$(CC) $(LIBS) $(CFLAGS) -L. -ldspdc -shared -o libdspds.so $(DSPDS_OBJ)

runtest:
	$(foreach f,$(TESTPROGS), ./$(f)${\n}; )

testobj: $(TESTPROGS)

test: testobj runtest


solib: dspdc dspds
#	$(CC) $(LIBS) $(OBJECTS) $(CFLAGS) -shared -o libsoundserver.so

clean:
	-rm -f *.so *.o *.bin

distclean:
	-rm -f *.so *.o *~ \#* *.bin

install:
	mkdir -p $(REAL_LIBDIR)
	install -m555 libdspds.so $(REAL_LIBDIR)/libdspds.so
	install -m555 libdspdc.so $(REAL_LIBDIR)/libdspdc.so

uninstall:
	rm -f $(REAL_LIBDIR)/libdspds.so $(REAL_LIBDIR)/libdspdc.so

