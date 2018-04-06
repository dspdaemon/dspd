include ../config.makefile
include ../rules.makefile

all: daemon latency

daemon:
	$(CC) $(LIBS) -ldspdc -ldspds -Wl,-rpath=../lib$(LIBSUFFIX) $(CFLAGS) daemon.c -o dspd

latency:
	$(CC) $(LIBS) -ldspdc -Wl,-rpath=../libs$(LIBSUFFIX) $(CFLAGS) latency.c -o dspd-latency-monitor

clean:
	-rm -f dspd *.o dspd-latency-monitor

distclean:
	-rm -f *.o *~ \#*

install:
	mkdir -p $(REAL_BINDIR)
	install -m555 dspd $(REAL_BINDIR)/dspd
