#ifndef FLAMES_READY_PLATFORM_H
#define FLAMES_READY_PLATFORM_H

#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <process.h>
#  include <io.h>
#  include <time.h>
#  include <limits.h>

typedef SOCKET fr_socket_t;
#  define FR_INVALID_SOCKET   INVALID_SOCKET
#  define fr_close_socket(s)  closesocket(s)
#  define fr_socket_err()     WSAGetLastError()
#  define FR_EAGAIN           WSAEWOULDBLOCK
#  define FR_EINTR            WSAEINTR
#  define FR_EWOULDBLOCK      WSAEWOULDBLOCK

#  define fr_select_nfds(s)   0

static inline int fr_set_nonblocking(fr_socket_t s, int enable)
{
    u_long v = (u_long)(enable ? 1 : 0);
    return ioctlsocket(s, FIONBIO, &v) == 0 ? 0 : -1;
}

static inline ssize_t fr_sock_read(fr_socket_t fd, void *buf, size_t n)
{
    int r = recv(fd, (char *)buf, (int)n, 0);
    return r < 0 ? -1 : (ssize_t)r;
}
static inline ssize_t fr_sock_write(fr_socket_t fd, const void *buf, size_t n)
{
    int w = send(fd, (const char *)buf, (int)n, 0);
    return w < 0 ? -1 : (ssize_t)w;
}

#  define fr_sleep_s(s)       Sleep((DWORD)(s) * 1000)
#  define fr_usleep_10ms()    Sleep(10)
#  define fr_getpid()         ((int)GetCurrentProcessId())
#  define fr_unlink(p)        _unlink(p)
#  define fr_strncasecmp(a,b,n)   _strnicmp((a),(b),(n))
#  define FR_IGNORE_SIGPIPE()     ((void)0)
#  define FR_NO_FORK 1
#  define FR_SHM_FAILED        NULL
#  define fr_shm_unmap(p,sz)   UnmapViewOfFile(p)

#  ifndef ssize_t
     typedef SSIZE_T ssize_t;
#  endif

#  ifndef PATH_MAX
#    define PATH_MAX MAX_PATH
#  endif

#else

#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <sys/mman.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <sys/stat.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <errno.h>
#  include <time.h>
#  include <limits.h>

typedef int fr_socket_t;
#  define FR_INVALID_SOCKET   (-1)
#  define fr_close_socket(s)  close(s)
#  define fr_socket_err()     errno
#  define FR_EAGAIN           EAGAIN
#  define FR_EINTR            EINTR
#  define FR_EWOULDBLOCK      EWOULDBLOCK
#  define fr_select_nfds(s)   ((s) + 1)

static inline int fr_set_nonblocking(fr_socket_t s, int enable)
{
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(s, F_SETFL, enable ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

static inline ssize_t fr_sock_read(fr_socket_t fd, void *buf, size_t n)
{
    return read(fd, buf, n);
}
static inline ssize_t fr_sock_write(fr_socket_t fd, const void *buf, size_t n)
{
    return write(fd, buf, n);
}

#  define fr_sleep_s(s)       sleep((unsigned)(s))
#  define fr_usleep_10ms()    usleep(10000)
#  define fr_getpid()         ((int)getpid())
#  define fr_unlink(p)        unlink(p)
#  define fr_strncasecmp(a,b,n)   strncasecmp((a),(b),(n))
#  define FR_IGNORE_SIGPIPE()     signal(SIGPIPE, SIG_IGN)
#  define FR_NO_FORK              0
#  define FR_SHM_FAILED           ((void *)MAP_FAILED)
#  define fr_shm_unmap(p,sz)      munmap((p),(sz))

#endif

#endif
