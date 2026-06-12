#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "binary_io.h"
#include "dbus_handler.h"
#include "ipc_socket.h"
#include "sysfs_poll.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

enum {
    FD_KIND_TIMER = 1,
    FD_KIND_AC = 2,
    FD_KIND_DBUS = 3,
    FD_KIND_IPC = 4,
};

static volatile sig_atomic_t g_stop_requested;

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-f ledger_path]\n", prog);
}

static void on_signal(int signo)
{
    (void)signo;
    g_stop_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }

    return 0;
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

static int log_event(BinaryIo *io, IpcSocket *ipc, ledger_event_t type)
{
    struct SysfsSample sample;
    struct PowerLedgerEvent event;

    if (sysfs_poll_sample(&sample) != 0) {
        return -1;
    }

    sysfs_poll_build_event(type, &sample, &event);
    if (binary_io_append(io, &event) != 0) {
        return -1;
    }

    if (ipc != NULL) {
        ipc_socket_update_cache(ipc, &event);
    }

    return 0;
}

static void seed_ipc_cache(IpcSocket *ipc)
{
    struct SysfsSample sample;
    struct PowerLedgerEvent event;

    if (ipc == NULL) {
        return;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        return;
    }

    sysfs_poll_build_event(EV_TICK, &sample, &event);
    ipc_socket_update_cache(ipc, &event);
}

static void handle_timer(BinaryIo *io, IpcSocket *ipc, int tfd)
{
    drain_fd(tfd);
    if (log_event(io, ipc, EV_TICK) != 0) {
        perror("log_event EV_TICK");
    }
}

static void handle_dbus(DbusHandler *dbus, BinaryIo *io, IpcSocket *ipc)
{
    if (dbus_handler_dispatch(dbus, io, ipc) != 0) {
        perror("dbus_handler_dispatch");
    }
}

static void handle_ac(BinaryIo *io, IpcSocket *ipc, int ac_fd)
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

    if (log_event(io, ipc, ev) != 0) {
        perror("log_event AC");
    }
}

static void handle_ipc(IpcSocket *ipc)
{
    if (ipc_socket_dispatch(ipc) != 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("ipc_socket_dispatch");
    }
}

int main(int argc, char **argv)
{
    const char *ledger_path = NULL;
    BinaryIo *io = NULL;
    DbusHandler *dbus = NULL;
    IpcSocket *ipc = NULL;
    int epfd = -1;
    int tfd = -1;
    int ac_fd = -1;
    int dbus_fd = -1;
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

    if (install_signal_handlers() != 0) {
        perror("install_signal_handlers");
        return EXIT_FAILURE;
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

    if (dbus_handler_init(&dbus) == 0) {
        dbus_fd = dbus_handler_get_fd(dbus);
        if (dbus_fd < 0 || add_epoll_fd(epfd, dbus_fd, FD_KIND_DBUS) != 0) {
            perror("epoll_ctl DBUS");
            dbus_handler_shutdown(dbus, epfd);
            dbus = NULL;
            dbus_fd = -1;
        }
    } else {
        fprintf(stderr, "warning: D-Bus sleep tracking disabled\n");
    }

    if (ipc_socket_init(&ipc) == 0) {
        int listen_fd = ipc_socket_get_listen_fd(ipc);

        seed_ipc_cache(ipc);
        if (listen_fd < 0 || add_epoll_fd(epfd, listen_fd, FD_KIND_IPC) != 0) {
            perror("epoll_ctl IPC");
            ipc_socket_shutdown(ipc, epfd);
            ipc = NULL;
        }
    } else {
        fprintf(stderr, "warning: IPC socket disabled (%s)\n", strerror(errno));
    }

    while (running) {
        struct epoll_event events[8];
        int nready;
        int i;

        if (g_stop_requested) {
            break;
        }

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
                handle_timer(io, ipc, tfd);
                break;
            case FD_KIND_AC:
                handle_ac(io, ipc, ac_fd);
                break;
            case FD_KIND_DBUS:
                if (dbus != NULL) {
                    handle_dbus(dbus, io, ipc);
                }
                break;
            case FD_KIND_IPC:
                if (ipc != NULL) {
                    handle_ipc(ipc);
                }
                break;
            default:
                break;
            }
        }
    }

    if (ipc != NULL) {
        ipc_socket_shutdown(ipc, epfd);
        ipc = NULL;
    }

    if (dbus != NULL) {
        dbus_handler_shutdown(dbus, epfd);
    }

    binary_io_close(io);
    (void)close(ac_fd);
    (void)close(tfd);
    (void)close(epfd);
    return EXIT_SUCCESS;
}
