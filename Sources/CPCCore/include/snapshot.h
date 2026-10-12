#ifndef CPC_SNAPSHOT_H
#define CPC_SNAPSHOT_H
#include "cpc.h"
bool cpc_snapshot_load(CPC *c, const uint8_t *data, size_t size);
/* version 1..3; mem_kb 64/128 for v1-2; for v3 0 = all populated banks. Caller frees. */
uint8_t *cpc_snapshot_save(CPC *c, int version, int mem_kb, bool compress, size_t *out_size);
#endif
