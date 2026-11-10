#ifndef BLUEPIXEL_CPC_SERVER_H
#define BLUEPIXEL_CPC_SERVER_H
#include <stdio.h>
#include "cpc_api.h"
int cpc_server_stdio(CPCApiSession *session, FILE *input, FILE *output);
typedef struct CPCServer CPCServer;
/* Called serially on the transport worker. Return malloc'd JSON; the server frees it.
 * Desktop implementations must forward this callback to the emulation thread. */
typedef char *(*CPCServerDispatch)(void *context, const char *request);
typedef struct {
    const char *unix_path;          /* NULL disables Unix sockets; existing paths are never removed at start */
    int websocket_port;            /* -1 disables TCP; 0 chooses a free loopback port */
    const char *token;             /* exactly 64 lowercase hex characters */
    const char *allowed_origins;   /* comma-separated exact Origins; NULL rejects all browser Origins */
    CPCServerDispatch dispatch;
    void *context;
    void (*cancel)(void *context); /* optional: interrupt an in-flight long run.* during stop */
} CPCServerConfig;
CPCServer *cpc_server_start(const CPCServerConfig *config, char *error, size_t error_size);
void cpc_server_stop(CPCServer *server); /* joins worker; caller keeps dispatch context alive until return */
int cpc_server_websocket_port(const CPCServer *server);
/* Create a random token file (0600), or read an existing owner-only file without following symlinks. */
int cpc_server_token_file(const char *path, char token[65], char *error, size_t error_size);
#endif
