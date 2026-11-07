#ifndef BLUEPIXEL_CPC_API_H
#define BLUEPIXEL_CPC_API_H
#include <stddef.h>
typedef struct CPCApiSession CPCApiSession;
CPCApiSession *cpc_api_create(void);
void cpc_api_destroy(CPCApiSession *session);
/* Dispatch one JSON-RPC 2.0 request. The returned JSON string is heap allocated. */
char *cpc_api_dispatch(CPCApiSession *session, const char *request);
#endif
