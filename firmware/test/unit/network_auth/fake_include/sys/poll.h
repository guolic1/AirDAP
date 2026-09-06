#pragma once

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN 0x01
#define POLLOUT 0x02
#define POLLERR 0x04
#define POLLHUP 0x08
#define POLLNVAL 0x10

int poll(struct pollfd *descriptors, nfds_t count, int timeout_ms);
