CC ?= cc
CFLAGS ?=-Wall -O0 -ggdb -fPIC $(XCFLAGS) -L../lib -DLIBSUFFIX=\"\" -std=gnu99
MAKE_OBJ=$(CC) $(CFLAGS) $(XCFLAGS) -c
LIBS ?=-lrt -pthread -lm -ldl $(XLIBS)
ifeq ($(DESTDIR),)
SYSCONFDIR=/etc
else
SYSCONFDIR=$(DESTDIR)/etc
endif

INSTALL_TARGETS ?= install-bin install-lib
MODULES ?= alsahw alsacfgfile socketserver speex libsamplerate osscuse udev
#DESTDIR ?= /
PREFIX ?= usr
LIBDIR ?= lib$(LIBSUFFIX)
BINDIR ?= bin
REAL_LIBDIR=$(DESTDIR)/$(PREFIX)/$(LIBDIR)
REAL_BINDIR=$(DESTDIR)/$(PREFIX)/$(BINDIR)
MODDIR=$(REAL_LIBDIR)/dspd
#SYSCONFDIR=$(DESTDIR)/etc

#.SUFFIXES: .h .c .o
#.c:
#	$(MAKE_OBJ) $@.c -o $@.o

