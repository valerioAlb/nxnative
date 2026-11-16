// Sockets: the host's networking (LDN / the loopback of local play, hle_*socket*.c) needs BSD sockets with file descriptors, which Winsock does not give.
// Until that is ported every socket call fails (EAFNOSUPPORT): the games see no network, as with networking turned off on Horizon
#pragma once
#include "dm_windows.h"
typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __ss_pad[126]; };
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct msghdr { void *msg_name; socklen_t msg_namelen; struct iovec *msg_iov; int msg_iovlen; void *msg_control; socklen_t msg_controllen; int msg_flags; };
#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 23
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 4
#define SO_BROADCAST 0x20
#define SO_RCVBUF 0x1002
#define SO_SNDBUF 0x1001
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define SO_ERROR 0x1007
#define SO_KEEPALIVE 8
#define SO_NOSIGPIPE 0x1022
#define SO_REUSEPORT 0x200
#define SO_LINGER 0x80
#define SO_TYPE 0x1008
#define MSG_DONTWAIT 0x40
#define MSG_PEEK 2
#define MSG_NOSIGNAL 0x4000
#define MSG_WAITALL 8
#define MSG_TRUNC 0x20
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define INADDR_ANY ((in_addr_t)0)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define TCP_NODELAY 1
int socket(int d, int t, int p);
int socketpair(int d, int t, int p, int sv[2]);
int bind(int s, const struct sockaddr *a, socklen_t n);
int connect(int s, const struct sockaddr *a, socklen_t n);
int listen(int s, int n);
int accept(int s, struct sockaddr *a, socklen_t *n);
ssize_t send(int s, const void *b, size_t n, int f);
ssize_t recv(int s, void *b, size_t n, int f);
ssize_t sendto(int s, const void *b, size_t n, int f, const struct sockaddr *a, socklen_t an);
ssize_t recvfrom(int s, void *b, size_t n, int f, struct sockaddr *a, socklen_t *an);
ssize_t sendmsg(int s, const struct msghdr *m, int f);
ssize_t recvmsg(int s, struct msghdr *m, int f);
int setsockopt(int s, int l, int o, const void *v, socklen_t n);
int getsockopt(int s, int l, int o, void *v, socklen_t *n);
int getsockname(int s, struct sockaddr *a, socklen_t *n);
int getpeername(int s, struct sockaddr *a, socklen_t *n);
int shutdown(int s, int how);
static inline uint16_t dm_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
#define htons(v) dm_bswap16((uint16_t)(v))
#define ntohs(v) dm_bswap16((uint16_t)(v))
#define htonl(v) __builtin_bswap32((uint32_t)(v))
#define ntohl(v) __builtin_bswap32((uint32_t)(v))
struct pollfd { int fd; short events, revents; };
typedef unsigned long nfds_t;
#define POLLIN 1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
#define POLLRDNORM 64
#define POLLWRNORM 256
int poll(struct pollfd *f, nfds_t n, int timeout);
struct cmsghdr { socklen_t cmsg_len; int cmsg_level, cmsg_type; };
#define SCM_RIGHTS 1
#define MSG_CMSG_CLOEXEC 0x40000000
#define CMSG_ALIGN(n) (((n) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))
#define CMSG_LEN(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_DATA(c) ((unsigned char *)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(m) ((m)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr *)(m)->msg_control : (struct cmsghdr *)0)
#define CMSG_NXTHDR(m, c) ((struct cmsghdr *)0)
