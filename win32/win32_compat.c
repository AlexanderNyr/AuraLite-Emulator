/* win32/win32_compat.c -- Winsock2, poll, and open_memstream shim for Windows */
#ifdef _WIN32
#define AURALITE_WIN32_COMPAT_IMPL
#include "win32_compat.h"
#undef socket
#undef bind
#undef listen
#undef accept
#undef connect
#undef setsockopt
#undef send
#undef recv
#undef close
#undef fcntl
#undef poll
#undef open_memstream
#undef fopen
#undef fclose
#undef remove
#include <conio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static int g_wsa_ready = 0;

static void w32_ensure_wsa(void) {
    if (!g_wsa_ready) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
            g_wsa_ready = 1;
    }
}

static void w32_map_wsa_error(void) {
    int err = WSAGetLastError();
    switch (err) {
    case WSAEWOULDBLOCK:  errno = EAGAIN; break;
    case WSAEINTR:        errno = EINTR; break;
    case WSAECONNREFUSED: errno = ECONNREFUSED; break;
    case WSAECONNRESET:   errno = ECONNRESET; break;
    case WSAEADDRINUSE:   errno = EADDRINUSE; break;
    case WSAEINVAL:       errno = EINVAL; break;
    default:              errno = EIO; break;
    }
}

/* Track non-blocking state per socket handle (small linear table) */
#define MAX_TRACKED_SOCKS 64
static struct {
    int fd;
    int nonblock;
    int used;
} g_socks[MAX_TRACKED_SOCKS];

static int sock_get_nb(int fd) {
    for (int i = 0; i < MAX_TRACKED_SOCKS; i++)
        if (g_socks[i].used && g_socks[i].fd == fd)
            return g_socks[i].nonblock;
    return 0;
}

static void sock_set_nb(int fd, int nb) {
    for (int i = 0; i < MAX_TRACKED_SOCKS; i++) {
        if (g_socks[i].used && g_socks[i].fd == fd) {
            g_socks[i].nonblock = nb;
            return;
        }
    }
    for (int i = 0; i < MAX_TRACKED_SOCKS; i++) {
        if (!g_socks[i].used) {
            g_socks[i].used = 1;
            g_socks[i].fd = fd;
            g_socks[i].nonblock = nb;
            return;
        }
    }
}

static void sock_forget(int fd) {
    for (int i = 0; i < MAX_TRACKED_SOCKS; i++)
        if (g_socks[i].used && g_socks[i].fd == fd)
            g_socks[i].used = 0;
}

int w32_socket(int domain, int type, int protocol) {
    w32_ensure_wsa();
    SOCKET s = socket(domain, type, protocol);
    if (s == INVALID_SOCKET) {
        w32_map_wsa_error();
        return -1;
    }
    int fd = (int)s;
    sock_set_nb(fd, 0);
    return fd;
}

int w32_bind(int s, const struct sockaddr *addr, socklen_t namelen) {
    if (bind((SOCKET)s, addr, namelen) == SOCKET_ERROR) {
        w32_map_wsa_error();
        return -1;
    }
    return 0;
}

int w32_listen(int s, int backlog) {
    if (listen((SOCKET)s, backlog) == SOCKET_ERROR) {
        w32_map_wsa_error();
        return -1;
    }
    return 0;
}

int w32_accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
    SOCKET c = accept((SOCKET)s, addr, addrlen);
    if (c == INVALID_SOCKET) {
        w32_map_wsa_error();
        return -1;
    }
    int fd = (int)c;
    sock_set_nb(fd, 0);
    return fd;
}

int w32_connect(int s, const struct sockaddr *name, socklen_t namelen) {
    if (connect((SOCKET)s, name, namelen) == SOCKET_ERROR) {
        w32_map_wsa_error();
        return -1;
    }
    return 0;
}

int w32_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen) {
    if (setsockopt((SOCKET)s, level, optname, (const char *)optval, optlen) == SOCKET_ERROR) {
        w32_map_wsa_error();
        return -1;
    }
    return 0;
}

ssize_t w32_send(int s, const void *buf, size_t len, int flags) {
    int temp_nb = ((flags & MSG_DONTWAIT) != 0) && !sock_get_nb(s);
    if (temp_nb) {
        u_long one = 1;
        ioctlsocket((SOCKET)s, FIONBIO, &one);
    }
    int n = send((SOCKET)s, (const char *)buf, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
    if (temp_nb) {
        u_long zero = 0;
        ioctlsocket((SOCKET)s, FIONBIO, &zero);
    }
    if (n == SOCKET_ERROR) {
        w32_map_wsa_error();
        return -1;
    }
    return (ssize_t)n;
}

ssize_t w32_recv(int s, void *buf, size_t len, int flags) {
    int temp_nb = ((flags & MSG_DONTWAIT) != 0) && !sock_get_nb(s);
    if (temp_nb) {
        u_long one = 1;
        ioctlsocket((SOCKET)s, FIONBIO, &one);
    }
    int n = recv((SOCKET)s, (char *)buf, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
    if (temp_nb) {
        u_long zero = 0;
        ioctlsocket((SOCKET)s, FIONBIO, &zero);
    }
    if (n == SOCKET_ERROR) {
        w32_map_wsa_error();
        return -1;
    }
    return (ssize_t)n;
}

int w32_close(int fd) {
    sock_forget(fd);
    if (closesocket((SOCKET)fd) == 0)
        return 0;
    return _close(fd);
}

int w32_fcntl(int fd, int cmd, ...) {
    if (cmd == F_GETFL)
        return sock_get_nb(fd) ? O_NONBLOCK : 0;
    if (cmd == F_SETFL) {
        va_list ap;
        va_start(ap, cmd);
        int fl = va_arg(ap, int);
        va_end(ap);
        u_long nb = (fl & O_NONBLOCK) ? 1UL : 0UL;
        if (ioctlsocket((SOCKET)fd, FIONBIO, &nb) == SOCKET_ERROR) {
            w32_map_wsa_error();
            return -1;
        }
        sock_set_nb(fd, (int)nb);
        return 0;
    }
    return 0;
}

int w32_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    w32_ensure_wsa();
    int ready = 0;
    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    int has_sock = 0;

    for (nfds_t i = 0; i < nfds; i++) {
        int fd = (int)fds[i].fd;
        fds[i].revents = 0;
        if (fd < 0) continue;
        if (fd == 0) {
            /* stdin fd */
            if (_kbhit() || WaitForSingleObject(GetStdHandle(STD_INPUT_HANDLE), 0) == WAIT_OBJECT_0) {
                fds[i].revents = POLLIN;
                ready++;
            }
            continue;
        }
        SOCKET s = (SOCKET)fds[i].fd;
        if (fds[i].events & POLLIN)  FD_SET(s, &rfds);
        if (fds[i].events & POLLOUT) FD_SET(s, &wfds);
        FD_SET(s, &efds);
        has_sock = 1;
    }

    if (!has_sock) {
        if (!ready && timeout > 0) Sleep((DWORD)(timeout > 50 ? 50 : timeout));
        return ready;
    }

    struct timeval tv;
    tv.tv_sec  = (timeout >= 0) ? (timeout / 1000) : 0;
    tv.tv_usec = (timeout >= 0) ? ((timeout % 1000) * 1000) : 0;
    int sr = select(0, &rfds, &wfds, &efds, (timeout >= 0) ? &tv : NULL);
    if (sr > 0) {
        for (nfds_t i = 0; i < nfds; i++) {
            int fd = (int)fds[i].fd;
            if (fd <= 0) continue;
            SOCKET s = (SOCKET)fds[i].fd;
            short rev = 0;
            if (FD_ISSET(s, &rfds)) rev |= POLLIN;
            if (FD_ISSET(s, &wfds)) rev |= POLLOUT;
            if (FD_ISSET(s, &efds)) rev |= POLLERR;
            if (rev) {
                fds[i].revents = rev;
                ready++;
            }
        }
    }
    return ready;
}

/* open_memstream implementation backed by a temporary file */
#define MAX_MEMSTREAMS 8
static struct {
    FILE   *fp;
    char  **ptr;
    size_t *sizeloc;
    char    tmppath[MAX_PATH];
    int     used;
} g_memstreams[MAX_MEMSTREAMS];

FILE *w32_open_memstream(char **ptr, size_t *sizeloc) {
    if (!ptr || !sizeloc) return NULL;
    *ptr = NULL;
    *sizeloc = 0;
    int slot = -1;
    for (int i = 0; i < MAX_MEMSTREAMS; i++) {
        if (!g_memstreams[i].used) { slot = i; break; }
    }
    if (slot < 0) return NULL;

    char dir[MAX_PATH];
    char path[MAX_PATH];
    DWORD dlen = GetTempPathA(sizeof dir, dir);
    if (!dlen || dlen >= sizeof dir || !GetTempFileNameA(dir, "ams", 0, path))
        return NULL;

    FILE *fp = fopen(path, "w+b");
    if (!fp) {
        DeleteFileA(path);
        return NULL;
    }
    g_memstreams[slot].used = 1;
    g_memstreams[slot].fp = fp;
    g_memstreams[slot].ptr = ptr;
    g_memstreams[slot].sizeloc = sizeloc;
    memcpy(g_memstreams[slot].tmppath, path, MAX_PATH);
    g_memstreams[slot].tmppath[MAX_PATH - 1] = '\0';
    return fp;
}

static const char *w32_translate_path(const char *path, char buf[MAX_PATH]) {
    if (!path) return NULL;
    if (strcmp(path, "/dev/null") == 0)
        return "NUL";
    if (strncmp(path, "/tmp/", 5) == 0) {
        DWORD dlen = GetTempPathA(MAX_PATH - 64, buf);
        if (dlen > 0 && dlen < MAX_PATH - 64) {
            snprintf(buf + dlen, MAX_PATH - dlen, "%s", path + 5);
            return buf;
        }
    }
    return path;
}

FILE *w32_fopen(const char *path, const char *mode) {
    char buf[MAX_PATH];
    return fopen(w32_translate_path(path, buf), mode);
}

int w32_remove(const char *path) {
    char buf[MAX_PATH];
    return remove(w32_translate_path(path, buf));
}

int w32_fclose(FILE *stream) {
    for (int i = 0; i < MAX_MEMSTREAMS; i++) {
        if (g_memstreams[i].used && g_memstreams[i].fp == stream) {
            fflush(stream);
            long sz = ftell(stream);
            if (sz < 0) sz = 0;
            rewind(stream);
            char *buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t got = sz ? fread(buf, 1, (size_t)sz, stream) : 0;
                buf[got] = '\0';
                *g_memstreams[i].ptr = buf;
                *g_memstreams[i].sizeloc = got;
            }
            int rc = fclose(stream);
            DeleteFileA(g_memstreams[i].tmppath);
            g_memstreams[i].used = 0;
            return buf ? rc : -1;
        }
    }
    return fclose(stream);
}

#endif /* _WIN32 */
