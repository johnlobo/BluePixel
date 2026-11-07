#ifndef BLUEPIXEL_CPC_HOST_H
#define BLUEPIXEL_CPC_HOST_H
#include "dsk.h"
#include "cpc.h"
#include "cpc_api.h"
#include <stddef.h>
CPCDisc *dsk_load_file(const char *path);
bool dsk_save_file(CPCDisc *disc, const char *path);
int dbg_symbols_load(CPC *cpc, const char *path);
bool bp_roms_load(CPCApiSession *session, const char *rom_dir, char *error, size_t error_size);
#endif
