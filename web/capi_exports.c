/* WebAssembly entry points for CPCApi. The whole machine is driven through JSON-RPC (see api/schema.json); these three
 * functions are all the browser needs besides malloc/free. Build with tools/build-wasm-capi.sh. */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE   /* lets the native build compile-check this file */
#endif
#include "cpc_api.h"

EMSCRIPTEN_KEEPALIVE CPCApiSession *bpapi_create(void) { return cpc_api_create(); }
EMSCRIPTEN_KEEPALIVE void bpapi_destroy(CPCApiSession *session) { cpc_api_destroy(session); }
/* NUL-terminated request in, malloc'd NUL-terminated response out: release it with free() (exported as _free). */
EMSCRIPTEN_KEEPALIVE char *bpapi_dispatch(CPCApiSession *session, const char *request) { return cpc_api_dispatch(session, request); }
