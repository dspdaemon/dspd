include ../config.makefile
include ../rules.makefile

OBJECTS=sndio.o sndiod.o
SONAME=libdsndio.so.0.0
LIBS += -ldspdc

all: $(OBJECTS) solib daemon module
#Symlink for testing purposes
	-rm -f ../modules/mod_sndiod.so
	ln -s ../sndio/mod_sndiod.so ../modules/mod_sndiod.so

solib: 
	$(CC) $(LIBS) sndio.o $(CFLAGS) -shared -o $(SONAME) -L../lib
	-rm -f libdsndio.so
	ln -s $(SONAME) libdsndio.so

module:
	$(CC) $(CFLAGS) $(MLIBS) -ldsndio -L../sndio -shared -o mod_sndiod.so mod_sndiod.c

daemon:
	$(CC) $(LIBS) -ldsndio sndiod.o $(CFLAGS) -L. -o dspd-sndiod

clean:
	-rm -f *.so *.o dspd-sndiod $(SONAME)

distclean:
	-rm -f *.so *.o *~ \#* dspd-sndiod $(SONAME)

install:
	mkdir -p $(REAL_LIBDIR)
	install -m555 $(SONAME) $(REAL_LIBDIR)/$(SONAME)
	-rm -f $(REAL_LIBDIR)/libdsndio.so
	ln -s $(SONAME) $(REAL_LIBDIR)/libdsndio.so
	mkdir -p -m755 $(MODDIR)
	install -m555 mod_sndiod.so $(MODDIR)

uninstall:
	-rm -f $(REAL_LIBDIR)/libdsndio.so $(REAL_LIBDIR)/$(SONAME)
	-rm -f $(REAL_BINDIR)/dspd-sndiod
	-rm -f $(MODDIR)/mod_sndiod.so

