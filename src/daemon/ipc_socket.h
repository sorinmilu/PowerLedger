#ifndef IPC_SOCKET_H
#define IPC_SOCKET_H

#include "ledger_format.h"

typedef struct IpcSocket IpcSocket;

int ipc_socket_init(IpcSocket **out);
void ipc_socket_update_cache(IpcSocket *ipc, const struct PowerLedgerEvent *event);
int ipc_socket_dispatch(IpcSocket *ipc);
void ipc_socket_shutdown(IpcSocket *ipc, int epfd);
int ipc_socket_get_listen_fd(const IpcSocket *ipc);
const char *ipc_socket_path(void);

#endif /* IPC_SOCKET_H */
