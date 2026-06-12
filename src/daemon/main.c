#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "binary_io.h"
#include "sysfs_poll.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

enum {
    FD_KIND_TIMER = 1,
    FD_KIND_AC = 2,
};

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-f ledger_path]\n", prog);
}

static int add_epoll_fd(int epfd, int fd, int kind)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = (uint32_t)kind;

    if (kind == FD_KIND_AC) {
        ev.events |= EPOLLET;
    }

    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static int setup_timerfd(int epfd)
{
    struct itimerspec its;
    int tfd;

    tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        return -1;
    }

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 60;
    its.it_interval.tv_sec = 60;

    if (timerfd_settime(tfd, 0, &its, NULL) != 0) {
        (void)close(tfd);
        return -1;
    }

    if (add_epoll_fd(epfd, tfd, FD_KIND_TIMER) != 0) {
        (void)close(tfd);
        return -1;
    }

    return tfd;
}

static void drain_fd(int fd)
{
    uint64_t buf;
    ssize_t nread;

    do {
        nread = read(fd, &buf, sizeof(buf));
    } while (nread > 0);
}

static int log_event(BinaryIo *io, ledger_event_t type)
{
    struct SysfsSample sample;
    struct PowerLedgerEvent event;

    if (sysfs_poll_sample(&sample) != 0) {
        return -1;
    }

    sysfs_poll_build_event(type, &sample, &event);
    return binary_io_append(io, &event);
}

static void handle_timer(BinaryIo *io, int tfd)
{
    drain_fd(tfd);
    if (log_event(io, EV_TICK) != 0) {
        perror("log_event EV_TICK");
    }
}

static void handle_ac(BinaryIo *io, int ac_fd)
{
    int online;
    ledger_event_t ev;

    drain_fd(ac_fd);

    online = sysfs_poll_read_ac_online(ac_fd);
    if (online < 0) {
        perror("sysfs_poll_read_ac_online");
        return;
    }

    ev = (online != 0) ? EV_PLUG : EV_UNPLUG;
    if (!sysfs_poll_ac_debounce_accept(ev)) {
        return;
    }

    if (log_event(io, ev) != 0) {
        perror("log_event AC");
    }
}

int main(int argc, char **argv)
{
    const char *ledger_path = NULL;
    BinaryIo *io = NULL;
    int epfd = -1;
    int tfd = -1;
    int ac_fd = -1;
    int opt;
    int running = 1;

    while ((opt = getopt(argc, argv, "f:h")) != -1) {
        switch (opt) {
        case 'f':
            ledger_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (binary_io_open(&io, ledger_path) != 0) {
        perror("binary_io_open");
        return EXIT_FAILURE;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        binary_io_close(io);
        return EXIT_FAILURE;
    }

    tfd = setup_timerfd(epfd);
    if (tfd < 0) {
        perror("setup_timerfd");
        binary_io_close(io);
        (void)close(epfd);
        return EXIT_FAILURE;
    }

    ac_fd = sysfs_poll_open_ac_fd();
    if (ac_fd < 0) {
        perror("sysfs_poll_open_ac_fd");
        binary_io_close(io);
        (void)close(tfd);
        (void)close(epfd);
        return EXIT_FAILURE;
    }

    if (add_epoll_fd(epfd, ac_fd, FD_KIND_AC) != 0) {
        perror("epoll_ctl AC");
        binary_io_close(io);
        (void)close(ac_fd);
        (void)close(tfd);
        (void)close(epfd);
        return EXIT_FAILURE;
    }

    while (running) {
        struct epoll_event events[8];
        int nready;
        int i;

        nready = epoll_wait(epfd, events, 8, -1);
        if (nready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (i = 0; i < nready; i++) {
            switch (events[i].data.u32) {
            case FD_KIND_TIMER:
                handle_timer(io, tfd);
                break;
            case FD_KIND_AC:
                handle_ac(io, ac_fd);
                break;
            default:
                break;
            }
        }
    }

    binary_io_close(io);
    (void)close(ac_fd);
    (void)close(tfd);
    (void)close(epfd);
    return EXIT_FAILURE;
}
