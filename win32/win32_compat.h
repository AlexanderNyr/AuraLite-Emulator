/* win32/win32_compat.h -- POSIX socket/poll/memstream shim for MinGW-w64 */
#ifndef AURALITE_WIN32_COMPAT_H
#define AURALITE_WIN32_COMPAT_H

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#undef far
#undef near
#undef FAR
#undef NEAR
#include <io.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef POLLIN
#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
struct pollfd {
    int   fd;
    short events;
    short revents;
};
#endif

typedef unsigned long nfds_t;

int     w32_socket(int domain, int type, int protocol);
int     w32_bind(int s, const struct sockaddr *addr, socklen_t namelen);
int     w32_listen(int s, int backlog);
int     w32_accept(int s, struct sockaddr *addr, socklen_t *addrlen);
int     w32_connect(int s, const struct sockaddr *name, socklen_t namelen);
int     w32_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
ssize_t w32_send(int s, const void *buf, size_t len, int flags);
ssize_t w32_recv(int s, void *buf, size_t len, int flags);
int     w32_close(int fd);
int     w32_fcntl(int fd, int cmd, ...);
int     w32_poll(struct pollfd *fds, nfds_t nfds, int timeout);
FILE   *w32_open_memstream(char **ptr, size_t *sizeloc);
FILE   *w32_fopen(const char *path, const char *mode);
int     w32_fclose(FILE *stream);
int     w32_remove(const char *path);

#ifndef AURALITE_WIN32_COMPAT_IMPL
#define socket         w32_socket
#define bind           w32_bind
#define listen         w32_listen
#define accept         w32_accept
#define connect        w32_connect
#define setsockopt     w32_setsockopt
#define send           w32_send
#define recv           w32_recv
#define close          w32_close
#define fcntl          w32_fcntl
#define poll           w32_poll
#define open_memstream w32_open_memstream
#define fopen          w32_fopen
#define fclose         w32_fclose
#define remove         w32_remove
#endif

#endif /* _WIN32 */
#endif /* AURALITE_WIN32_COMPAT_H */
