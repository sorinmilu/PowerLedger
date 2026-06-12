#ifndef IPC_SOCKET_H
#define IPC_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#include "ledger_format.h"
#include "sysfs_poll.h"

typedef struct IpcSocket IpcSocket;

int ipc_socket_init(IpcSocket **out);
int ipc_socket_refresh_live(IpcSocket *ipc);
void ipc_socket_update_cache(IpcSocket *ipc, const struct PowerLedgerEvent *event,
                             const struct SysfsSample *sample, int advance_session);
void ipc_socket_restore_session(IpcSocket *ipc, const char *ledger_path);
int ipc_socket_accept_pending(IpcSocket *ipc, int epfd);
void ipc_socket_shutdown(IpcSocket *ipc, int epfd);
int ipc_socket_get_listen_fd(const IpcSocket *ipc);
const char *ipc_socket_path(void);

#ifdef TEST_MODE
int ipc_socket_test_build_json(IpcSocket *ipc, char *buf, size_t buflen);
int ipc_socket_test_build_json_all(IpcSocket *ipc, char *buf, size_t buflen);
int ipc_socket_test_build_ascii(IpcSocket *ipc, char *buf, size_t buflen);
int ipc_socket_test_build_ascii_all(IpcSocket *ipc, char *buf, size_t buflen);
#endif

#endif /* IPC_SOCKET_H */
