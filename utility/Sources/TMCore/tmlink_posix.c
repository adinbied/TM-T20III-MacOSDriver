/*
 * tmlink_posix.c - fd and TCP (port 9100) transports.
 */
#include "tmcore.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { int rfd, wfd, owned; } fd_ctx;

static int fd_write(void *c, const uint8_t *b, size_t n)
{
    fd_ctx *x = c;
    while (n) {
        ssize_t w = write(x->wfd, b, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int fd_read(void *c, uint8_t *b, size_t cap, int timeout_ms)
{
    fd_ctx *x = c;
    struct pollfd p = {x->rfd, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;
    ssize_t n = read(x->rfd, b, cap);
    if (n == 0)
        return -1;                       /* peer closed */
    return n < 0 ? (errno == EINTR || errno == EAGAIN ? 0 : -1) : (int)n;
}

static void fd_close(void *c)
{
    fd_ctx *x = c;
    if (x->owned) {
        close(x->rfd);
        if (x->wfd != x->rfd)
            close(x->wfd);
    }
    free(x);
}

static int fd_link(tm_link *l, int rfd, int wfd, int owned)
{
    fd_ctx *x = calloc(1, sizeof *x);
    if (!x)
        return TM_ERR_IO;
    x->rfd = rfd;
    x->wfd = wfd;
    x->owned = owned;
    memset(l, 0, sizeof *l);
    l->ctx = x;
    l->write = fd_write;
    l->read = fd_read;
    l->close = fd_close;
    return TM_OK;
}

int tm_link_fd(tm_link *l, int rfd, int wfd)
{
    return fd_link(l, rfd, wfd, 0);
}

int tm_link_tcp(tm_link *l, const char *host, int port)
{
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port > 0 ? port : 9100);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0)
        return TM_ERR_NODEV;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return TM_ERR_NODEV;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd_link(l, fd, fd, 1);
}

#ifndef __APPLE__
int tm_link_usb(tm_link *l)
{
    (void)l;
    return TM_ERR_NODEV;                 /* USB transport is macOS-only */
}
#endif
