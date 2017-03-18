#ifndef _DSPD_SOCKET_H_
#define _DSPD_SOCKET_H_
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
int dspd_unix_sock_create(const char *addr, int flags);
int dspd_unix_sock_connect(const char *addr, int flags);

#endif
