#define _POSIX_C_SOURCE 200809L

#include "dbus_handler.h"
#include "ipc_socket.h"
#include "sysfs_poll.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define DBUS_MATCH_RULE \
    "type='signal',sender='org.freedesktop.login1'," \
    "interface='org.freedesktop.login1.Manager'," \
    "member='PrepareForSleep',path='/org/freedesktop/login1'"

struct DbusHandler {
    DBusConnection *conn;
    int fd;
    BinaryIo *pending_io;
    IpcSocket *pending_ipc;
};

static int log_lifecycle_event(BinaryIo *io, IpcSocket *ipc, ledger_event_t type)
{
    struct SysfsSample sample;
    struct PowerLedgerEvent event;

    if (io == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (sysfs_poll_sample(&sample) != 0) {
        return -1;
    }

    sysfs_poll_build_event(type, &sample, &event);
    if (binary_io_append(io, &event) != 0) {
        return -1;
    }

    if (ipc != NULL) {
        ipc_socket_update_cache(ipc, &event, &sample, 1);
    }

    return 0;
}

static DBusHandlerResult prepare_for_sleep_filter(DBusConnection *conn,
                                                  DBusMessage *msg,
                                                  void *userdata)
{
    DbusHandler *handler = userdata;
    dbus_bool_t sleeping;

    (void)conn;

    if (handler == NULL || handler->pending_io == NULL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (!dbus_message_is_signal(msg, "org.freedesktop.login1.Manager",
                                "PrepareForSleep")) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &sleeping,
                               DBUS_TYPE_INVALID)) {
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (sleeping) {
        if (log_lifecycle_event(handler->pending_io, handler->pending_ipc, EV_SLEEP) != 0) {
            perror("dbus_handler EV_SLEEP");
        }
    } else if (log_lifecycle_event(handler->pending_io, handler->pending_ipc, EV_WAKE) != 0) {
        perror("dbus_handler EV_WAKE");
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

int dbus_handler_init(DbusHandler **out)
{
    DBusError error;
    DbusHandler *handler;
    int fd = -1;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    handler = calloc(1, sizeof(*handler));
    if (handler == NULL) {
        return -1;
    }

    handler->fd = -1;
    dbus_error_init(&error);

    handler->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (handler->conn == NULL) {
        fprintf(stderr, "dbus_handler_init: dbus_bus_get failed: %s\n",
                error.message);
        dbus_error_free(&error);
        free(handler);
        return -1;
    }

    dbus_connection_set_exit_on_disconnect(handler->conn, FALSE);

    dbus_bus_add_match(handler->conn, DBUS_MATCH_RULE, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "dbus_handler_init: dbus_bus_add_match failed: %s\n",
                error.message);
        dbus_error_free(&error);
        dbus_connection_unref(handler->conn);
        free(handler);
        return -1;
    }

    if (!dbus_connection_add_filter(handler->conn, prepare_for_sleep_filter,
                                    handler, NULL)) {
        fprintf(stderr, "dbus_handler_init: dbus_connection_add_filter failed\n");
        dbus_error_init(&error);
        (void)dbus_bus_remove_match(handler->conn, DBUS_MATCH_RULE, &error);
        dbus_error_free(&error);
        dbus_connection_unref(handler->conn);
        free(handler);
        return -1;
    }

    if (!dbus_connection_get_unix_fd(handler->conn, &fd) || fd < 0) {
        fprintf(stderr, "dbus_handler_init: dbus_connection_get_unix_fd failed\n");
        dbus_connection_remove_filter(handler->conn, prepare_for_sleep_filter,
                                      handler);
        dbus_error_init(&error);
        (void)dbus_bus_remove_match(handler->conn, DBUS_MATCH_RULE, &error);
        dbus_error_free(&error);
        dbus_connection_unref(handler->conn);
        free(handler);
        return -1;
    }

    handler->fd = fd;

    {
        int flags = fcntl(handler->fd, F_GETFL);
        if (flags < 0 || fcntl(handler->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            fprintf(stderr, "dbus_handler_init: fcntl O_NONBLOCK failed\n");
            dbus_handler_shutdown(handler, -1);
            return -1;
        }
    }

    dbus_connection_flush(handler->conn);

    *out = handler;
    return 0;
}

int dbus_handler_get_fd(const DbusHandler *handler)
{
    if (handler == NULL) {
        errno = EINVAL;
        return -1;
    }

    return handler->fd;
}

int dbus_handler_dispatch(DbusHandler *handler, BinaryIo *io, IpcSocket *ipc)
{
    if (handler == NULL || handler->conn == NULL || io == NULL) {
        errno = EINVAL;
        return -1;
    }

    handler->pending_io = io;
    handler->pending_ipc = ipc;
    while (dbus_connection_read_write_dispatch(handler->conn, 0)) {
        /* drain all pending dbus traffic without blocking */
    }
    handler->pending_io = NULL;
    handler->pending_ipc = NULL;
    return 0;
}

void dbus_handler_shutdown(DbusHandler *handler, int epfd)
{
    DBusError error;

    if (handler == NULL) {
        return;
    }

    if (handler->fd >= 0 && epfd >= 0) {
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, handler->fd, NULL);
    }

    if (handler->conn != NULL) {
        dbus_connection_remove_filter(handler->conn, prepare_for_sleep_filter,
                                      handler);
        dbus_error_init(&error);
        dbus_bus_remove_match(handler->conn, DBUS_MATCH_RULE, &error);
        if (dbus_error_is_set(&error)) {
            fprintf(stderr, "dbus_handler_shutdown: dbus_bus_remove_match: %s\n",
                    error.message);
            dbus_error_free(&error);
        }
        dbus_connection_unref(handler->conn);
        handler->conn = NULL;
    }

    handler->fd = -1;
    free(handler);
}
