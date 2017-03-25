/*
 *  SOCKET - Socket helpers
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as
 *   published by the Free Software Foundation; either version 2.1 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <ctype.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdbool.h>
#include "socket.h"
#include "cfgread.h"
int dspd_unix_sock_create(const char *addr, int flags)
{
  struct sockaddr_un local;
  int fd, len, err;
  memset(&local, 0, sizeof(local));
  if ( (fd = socket(AF_UNIX, SOCK_STREAM | flags, 0)) < 0 )
    return -errno;
  local.sun_family = AF_UNIX;
  strcpy(local.sun_path, addr);
  unlink(local.sun_path);
  len = strlen(local.sun_path) + sizeof(local.sun_family);
  if (bind(fd, (struct sockaddr *)&local, len) == -1)
    { err = -errno; close(fd); return err; }
  return fd;
}

int dspd_tcp_sock_create(const char *addr, int flags)
{
  union { 
    struct sockaddr_in6 in6;
    struct sockaddr_in  in4;
  } sock;
  int fd, len, err, on = 1;
  uint16_t port;
  char str[INET6_ADDRSTRLEN*2];
  char *p, *a;
  if ( strlen(addr) >= sizeof(str) )
    return -EINVAL;
  strcpy(str, addr);
  memset(&sock, 0, sizeof(sock));
  if ( strchr(str, '.') == NULL )
    {
      p = strchr(str, '[');
      if ( p == NULL )
	return -EINVAL;
      a = &p[1];
      p = strchr(str, ']');
      if ( ! p )
	return -EINVAL;
      *p = 0;
      p++;
      if ( *p != ':' )
	return -EINVAL;
      p++;
      if ( dspd_strtou16(p, &port, 10) < 0 )
	return -EINVAL;
      if ( ! inet_pton(AF_INET6, a, &sock.in6.sin6_addr) )
	return -errno;
      if ( (fd = socket(AF_INET6, SOCK_STREAM | flags, 0)) < 0 )
	return -errno;
      sock.in6.sin6_family = AF_INET6;
      sock.in6.sin6_port = htons(port);
      len = sizeof(sock);
    } else
    {
      p = strchr(str, ':');
      if ( ! p )
	return -EINVAL;
      *p = 0;
      p++;
      if ( dspd_strtou16(p, &port, 10) < 0 )
	return -EINVAL;
      if ( ! inet_pton(AF_INET, str, &sock.in4.sin_addr.s_addr) )
	return -errno;
      if ( (fd = socket(AF_INET, SOCK_STREAM | flags, 0)) < 0 )
	return -errno;
      sock.in4.sin_family = AF_INET;
      sock.in4.sin_port = htons(port);
      len = sizeof(sock);
    }
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) < 0)
    {
      err = -errno;
      close(fd);
      return err;
    }
  if (bind(fd, (struct sockaddr *)&sock, len) == -1)
    { err = -errno; close(fd); return err; }
  return fd;
}


int dspd_unix_sock_connect(const char *addr, int flags)
{
  int s, len, err;
  struct sockaddr_un remote;
  if ((s = socket(AF_UNIX, SOCK_STREAM | flags, 0)) == -1) {
    return -errno;
  }
  memset(&remote, 0, sizeof(remote));
  remote.sun_family = AF_UNIX;
  strcpy(remote.sun_path, addr);
  len = strlen(remote.sun_path) + sizeof(remote.sun_family);
  if (connect(s, (struct sockaddr *)&remote, len) == -1) {
    err = -errno;
    close(s);
    return err;
  }
  return s;
}

