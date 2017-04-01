LIBSUFFIX=64
MODULES=socketserver udev alsahw alsacfgfile libsamplerate speex osscuse
XBINDIRS= sndio
PREFIX=/usr
CFLAGS=-Wall -O2 -ggdb -fPIC -L../lib -pthread   -msse3 yes -DLIBSUFFIX=\"64\"
