#pragma once
#include "dm_windows.h"
struct iovec { void *iov_base; size_t iov_len; };
ssize_t readv(int fd, const struct iovec *v, int n);
ssize_t writev(int fd, const struct iovec *v, int n);
ssize_t preadv(int fd, const struct iovec *v, int n, off_t off);
ssize_t pwritev(int fd, const struct iovec *v, int n, off_t off);
