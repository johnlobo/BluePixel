#include "cpc.h"
#include "dsk.h"
#include "fdc.h"
#include "snapshot.h"
#include "tape.h"
#include "cpc_keys.h"
#include "debug.h"
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

EMSCRIPTEN_KEEPALIVE int bp_load_tape(const uint8_t *data, size_t size, const char *name) {
    if (!machine || !data || !size) return 0;
    CPCTape *tape = tape_load(data, size, name ? name : "tape.cdt");
    if (!tape) return 0;
    tape_free(machine->tape);
    machine->tape = tape;
    tape_set_motor(tape, machine->tape_motor);
    return 1;
}

EMSCRIPTEN_KEEPALIVE void bp_tape_play(int play) { if (machine) tape_play(machine->tape, play != 0); }
EMSCRIPTEN_KEEPALIVE void bp_tape_rewind(void) { if (machine) tape_rewind(machine->tape); }
EMSCRIPTEN_KEEPALIVE void bp_tape_remove(void) {
    if (!machine) return;
    tape_free(machine->tape);
    machine->tape = NULL;
}
EMSCRIPTEN_KEEPALIVE int bp_tape_loaded(void) { return machine && machine->tape != NULL; }
EMSCRIPTEN_KEEPALIVE int bp_tape_playing(void) { return machine && tape_playing(machine->tape); }
EMSCRIPTEN_KEEPALIVE const char *bp_tape_name(void) { return machine ? tape_name(machine->tape) : ""; }

EMSCRIPTEN_KEEPALIVE int bp_load_snapshot(const uint8_t *data, size_t size) {
    return machine && cpc_snapshot_load(machine, data, size);
}

EMSCRIPTEN_KEEPALIVE uint8_t *bp_save_snapshot(size_t *size) {
    if (!machine) { if (size) *size = 0; return NULL; }
    return cpc_snapshot_save(machine, 3, 0, true, size);
}

EMSCRIPTEN_KEEPALIVE int bp_run_frame(void) {
    if (!machine) return 0;
    cpc_run_frame(machine);
    return cpc_dbg_break(machine) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE void bp_reset(void) {
    if (!machine) return;
    cpc_reset(machine, true);
    if (machine->dbg) { machine->dbg->hit = false; machine->dbg->skip_once = true; machine->dbg->temp_bp = -1; }
}
EMSCRIPTEN_KEEPALIVE void bp_set_monitor(int monitor) {
    if (!machine || monitor < CPC_MONITOR_COLOUR || monitor > CPC_MONITOR_GREY) return;
    CPCConfig cfg = machine->cfg;
    cfg.monitor = (CPCMonitor)monitor;
    cpc_configure(machine, &cfg);
}
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

EMSCRIPTEN_KEEPALIVE void bp_debug_attach(void) { if (machine) dbg_attach(machine); }
EMSCRIPTEN_KEEPALIVE int bp_debug_attached(void) { return machine && machine->dbg != NULL; }
EMSCRIPTEN_KEEPALIVE void bp_debug_resume(void) {
    if (!machine) return;
    Debugger *d = dbg_attach(machine);
    d->hit = false; d->skip_once = true; d->temp_bp = -1; d->step_out = false;
}
EMSCRIPTEN_KEEPALIVE void bp_debug_pause(void) {
    if (!machine || !machine->dbg) return;
    machine->dbg->temp_bp = -1; machine->dbg->step_out = false;
}
EMSCRIPTEN_KEEPALIVE void bp_debug_step_into(void) { if (machine) dbg_step_into(machine); }
EMSCRIPTEN_KEEPALIVE unsigned int bp_debug_register(int id) {
    if (!machine) return 0;
    Z80 *z = &machine->z80;
    switch (id) {
        case 0: return z->af.w; case 1: return z->bc.w; case 2: return z->de.w; case 3: return z->hl.w;
        case 4: return z->ix.w; case 5: return z->iy.w; case 6: return z->sp.w; case 7: return z->pc;
        case 8: return z->im; case 9: return (z->iff1 ? 1 : 0) | (z->iff2 ? 2 : 0) | (z->halted ? 4 : 0);
        case 10: return (unsigned int)machine->t_counter;
        default: return 0;
    }
}
EMSCRIPTEN_KEEPALIVE int bp_debug_disassemble(unsigned int addr, char *out, int size) {
    if (!machine || !out || size <= 0) return 0;
    DbgView view; dbg_view_current(machine, &view);
    return dbg_disasm(machine, &view, (uint16_t)addr, out, (size_t)size);
}
EMSCRIPTEN_KEEPALIVE int bp_debug_instruction_length(unsigned int addr) {
    char out[160];
    return bp_debug_disassemble(addr, out, sizeof out);
}
EMSCRIPTEN_KEEPALIVE int bp_debug_toggle_code_bp(unsigned int addr) {
    if (!machine) return 0;
    dbg_toggle_code_bp(machine, (uint16_t)addr);
    return 1;
}
EMSCRIPTEN_KEEPALIVE int bp_debug_has_code_bp(unsigned int addr) {
    if (!machine || !machine->dbg) return 0;
    for (int i = 0; i < DBG_MAX_BP; i++) {
        DbgBreakpoint *bp = &machine->dbg->bp[i];
        if (bp->used && bp->enabled && bp->type == BP_CODE && bp->start == (uint16_t)addr) return 1;
    }
    return 0;
}
EMSCRIPTEN_KEEPALIVE int bp_debug_breakpoint_count(void) {
    if (!machine || !machine->dbg) return 0;
    int count = 0;
    for (int i = 0; i < DBG_MAX_BP; i++) if (machine->dbg->bp[i].used && machine->dbg->bp[i].type == BP_CODE) count++;
    return count;
}
EMSCRIPTEN_KEEPALIVE int bp_debug_breakpoint_address(int ordinal) {
    if (!machine || !machine->dbg || ordinal < 0) return -1;
    for (int i = 0, n = 0; i < DBG_MAX_BP; i++) {
        DbgBreakpoint *bp = &machine->dbg->bp[i];
        if (bp->used && bp->type == BP_CODE && n++ == ordinal) return bp->start;
    }
    return -1;
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
