include config.makefile
include rules.makefile
LIBDIRS=lib alsa
BINDIRS=modules daemon $(XBINDIRS)
SUBDIRS=$(LIBDIRS) $(BINDIRS) $(M32DIR) sndio

all:
	sh -c 'OWD="$$PWD";for f in $(SUBDIRS); do cd $$OWD/$$f && make all || exit 1; done'

distclean:
	sh -c 'OWD="$$PWD";for f in $(SUBDIRS); do cd $$OWD/$$f && make distclean; done'
	sh -c '>config.makefile'
	-rm -f config.log

clean:
	sh -c 'OWD="$$PWD";for f in $(SUBDIRS); do cd $$OWD/$$f && make clean; done'
	-rm -f config.log

#Install the daemon and required libraries (just modules for now)
install-bin:
	sh -c 'OWD="$$PWD";for f in $(BINDIRS); do cd $$OWD/$$f && make install || exit 1; done'

#Install client libraries
install-lib:
	sh -c 'OWD="$$PWD";for f in $(LIBDIRS); do cd $$OWD/$$f && make install || exit 1; done'

#Install everything.  For multilib, use this once and then do install-lib
#for the secondary arch (usually 32 bit).
install: $(INSTALL_TARGETS)

install-config:
	mkdir -p $(DESTDIR)/etc/dspd
	install -m 0644 -t $(DESTDIR)/etc/dspd configs/dspd/*.conf
	mkdir -p $(DESTDIR)/var/run/dspd
	chown "nobody:audio" $(DESTDIR)/var/run/dspd
	chmod 0770 /var/run/dspd

uninstall:
	sh -c 'OWD="$$PWD";for f in $(SUBDIRS); do cd $$OWD/$$f && make uninstall || exit 1; done'
	rm -R -f /etc/dspd /var/run/dspd

install32:
	sh -c 'cd m32 && make DESTDIR=$(DESTDIR) install-lib || exit 1'
