#ifndef DBUS_HANDLER_H
#define DBUS_HANDLER_H

#include "binary_io.h"

typedef struct DbusHandler DbusHandler;
typedef struct IpcSocket IpcSocket;

int dbus_handler_init(DbusHandler **out);
int dbus_handler_get_fd(const DbusHandler *handler);
int dbus_handler_dispatch(DbusHandler *handler, BinaryIo *io, IpcSocket *ipc);
void dbus_handler_shutdown(DbusHandler *handler, int epfd);

#endif /* DBUS_HANDLER_H */
