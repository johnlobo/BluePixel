#include "cpc.h"
#include "dsk.h"
#include "fdc.h"
#include "snapshot.h"
#include "cpc_keys.h"
#include <emscripten.h>
#include <stdlib.h>

static CPC *machine;
static CPCDisc *drives[2];
static uint32_t *display_frame;
static uint8_t *disc_export;
static size_t disc_export_size;

EMSCRIPTEN_KEEPALIVE int bp_init(void) {
    if (machine) cpc_destroy(machine);
    drives[0] = drives[1] = NULL;
    machine = cpc_create(CPC_MODEL_6128);
    if (!machine) return 0;
    CPCConfig cfg;
    cpc_default_config(&cfg, CPC_MODEL_6128);
    cfg.ram_kb = 128;
    cfg.sample_rate = 44100;
    cfg.stereo = true;
    cpc_configure(machine, &cfg);
    cpc_reset(machine, true);
    display_frame = malloc(CPC_SCREEN_W * CPC_SCREEN_H * 2 * sizeof *display_frame);
    if (!display_frame) {
        cpc_destroy(machine);
        machine = NULL;
        return 0;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE void bp_destroy(void) {
    if (!machine) return;
    for (int drive = 0; drive < 2; drive++) {
        fdc_insert(machine->fdc, drive, NULL);
        machine->disc[drive] = NULL;
        dsk_free(drives[drive]);
        drives[drive] = NULL;
    }
    cpc_destroy(machine);
    machine = NULL;
    free(display_frame);
    display_frame = NULL;
    free(disc_export);
    disc_export = NULL;
    disc_export_size = 0;
}

EMSCRIPTEN_KEEPALIVE int bp_load_rom(int slot, const uint8_t *data, size_t size) {
    return machine && cpc_load_rom(machine, slot, data, size);
}

EMSCRIPTEN_KEEPALIVE int bp_load_disc(int drive, const uint8_t *data, size_t size) {
    if (!machine || drive < 0 || drive > 1) return 0;
    CPCDisc *disc = dsk_load(data, size, drive == 0 ? "browser-a.dsk" : "browser-b.dsk");
    if (!disc) return 0;
    fdc_insert(machine->fdc, drive, NULL);
    dsk_free(drives[drive]);
    drives[drive] = disc;
    machine->disc[drive] = drives[drive];
    fdc_insert(machine->fdc, drive, drives[drive]);
    return 1;
}

EMSCRIPTEN_KEEPALIVE int bp_load_snapshot(const uint8_t *data, size_t size) {
    return machine && cpc_snapshot_load(machine, data, size);
}

EMSCRIPTEN_KEEPALIVE uint8_t *bp_save_snapshot(size_t *size) {
    if (!machine) { if (size) *size = 0; return NULL; }
    return cpc_snapshot_save(machine, 3, 0, true, size);
}

EMSCRIPTEN_KEEPALIVE void bp_run_frame(void) { if (machine) cpc_run_frame(machine); }
EMSCRIPTEN_KEEPALIVE void bp_reset(void) { if (machine) cpc_reset(machine, true); }
EMSCRIPTEN_KEEPALIVE void bp_type(const char *text) { if (machine && text) cpc_autotype(machine, text); }
EMSCRIPTEN_KEEPALIVE void bp_key(int key, int down) { if (machine) cpc_key(machine, key, down != 0); }
EMSCRIPTEN_KEEPALIVE void bp_joystick(int bit, int down) { if (machine) cpc_joystick(machine, bit, down != 0); }
EMSCRIPTEN_KEEPALIVE const uint32_t *bp_frame(void) {
    if (!machine || !display_frame) return NULL;
    cpc_render(cpc_frame(machine), display_frame, true, true, false, 0);
    return display_frame;
}

EMSCRIPTEN_KEEPALIVE int bp_read_audio(float *out, int frames) {
    return machine ? cpc_audio_read(machine, out, frames) : 0;
}

EMSCRIPTEN_KEEPALIVE const char *bp_disc_name(int drive) {
    return drive >= 0 && drive < 2 && drives[drive] ? drives[drive]->name : "";
}

EMSCRIPTEN_KEEPALIVE const uint8_t *bp_export_disc(int drive, size_t *size) {
    free(disc_export);
    disc_export = NULL;
    disc_export_size = 0;
    if (drive >= 0 && drive < 2 && drives[drive]) disc_export = dsk_serialize(drives[drive], &disc_export_size);
    if (size) *size = disc_export_size;
    return disc_export;
}

EMSCRIPTEN_KEEPALIVE void bp_release_disc_export(void) {
    free(disc_export);
    disc_export = NULL;
    disc_export_size = 0;
}

EMSCRIPTEN_KEEPALIVE void bp_free(void *p) { free(p); }
