#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define IPC_HEADER_JSON 0x4AU
#define IPC_RESP_BUF    512

static int ipc_query_once(const char *sock_path)
{
    struct sockaddr_un addr;
    unsigned char req = IPC_HEADER_JSON;
    char buf[IPC_RESP_BUF];
    ssize_t nread;
    ssize_t nwritten;
    int fd;

    if (sock_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path) >=
        (int)sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        (void)close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        (void)close(fd);
        return -1;
    }

    nwritten = write(fd, &req, 1);
    if (nwritten != 1) {
        (void)close(fd);
        return -1;
    }

    nread = read(fd, buf, sizeof(buf) - 1U);
    if (nread <= 0) {
        (void)close(fd);
        return -1;
    }

    buf[nread] = '\0';
    (void)close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    int count = 50;
    const char *sock_path = "./tmp/power_ledger.sock";
    int i;

    if (argc > 1) {
        count = atoi(argv[1]);
        if (count <= 0) {
            fprintf(stderr, "ipc_stress: invalid count\n");
            return EXIT_FAILURE;
        }
    }

    if (argc > 2) {
        sock_path = argv[2];
    }

    for (i = 0; i < count; i++) {
        if (ipc_query_once(sock_path) != 0) {
            perror("ipc_query_once");
            fprintf(stderr, "ipc_stress failed on iteration %d/%d\n", i + 1, count);
            return EXIT_FAILURE;
        }
    }

    printf("ipc_stress passed: %d queries on %s\n", count, sock_path);
    return EXIT_SUCCESS;
}
