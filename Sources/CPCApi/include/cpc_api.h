#ifndef BLUEPIXEL_CPC_API_H
#define BLUEPIXEL_CPC_API_H
#include <stddef.h>
typedef struct CPCApiSession CPCApiSession;
struct CPC;
CPCApiSession *cpc_api_create(void);
/* Borrow an existing desktop machine. The caller serializes dispatch with the machine's owner;
 * destroying this session never destroys the machine or its mounted media. */
CPCApiSession *cpc_api_borrow(struct CPC *machine);
#define CPC_API_STDIO 1u
#define CPC_API_UNIX 2u
#define CPC_API_WEBSOCKET 4u
#define CPC_API_WASM 8u
/* Host metadata only; call before accepting requests. */
void cpc_api_set_transports(CPCApiSession *session, unsigned transports);
/* Thread-safe, terminal cancellation for host shutdown. run.* stops at an instruction/frame boundary. */
void cpc_api_cancel(CPCApiSession *session);
/* Optional owner-thread hook between frames / instruction batches, for desktop UI lock handoff. */
void cpc_api_set_yield(CPCApiSession *session, void (*callback)(void *), void *context);
void cpc_api_destroy(CPCApiSession *session);
/* Dispatch one JSON-RPC 2.0 request. The returned JSON string is heap allocated. */
char *cpc_api_dispatch(CPCApiSession *session, const char *request);
#endif
