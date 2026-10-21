/* cpcrun: headless test runner. `cpcrun zex <file.com>` runs a CP/M .COM exerciser (ZEXDOC/ZEXALL). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"
#include "cpc.h"
#include "cpc_keys.h"
#include "dsk.h"
#include "fdc.h"
#include "tape.h"
#include "snapshot.h"
#include "state.h"
#include "replay.h"
#include <time.h>
#include "debug.h"

static uint8_t mem[65536];
static int done;

static uint8_t t_rd(void *c, uint16_t a) { (void)c; return mem[a]; }
static void t_wr(void *c, uint16_t a, uint8_t v) { (void)c; mem[a] = v; }
static uint8_t t_in(void *c, uint16_t p) { (void)c; (void)p; return 0xFF; }
static void t_out(void *c, uint16_t p, uint8_t v) { (void)c; (void)p; (void)v; }

static int run_zex(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    size_t n = fread(mem + 0x100, 1, 0xFE00, f);
    fclose(f);
    (void)n;
    Z80 z; memset(&z, 0, sizeof z);
    z.rd = t_rd; z.wr = t_wr; z.in = t_in; z.out = t_out;
    z80_reset(&z);
    z.pc = 0x100; z.sp.w = 0xF000;
    mem[0] = 0x76; /* warm boot: HALT */
    mem[5] = 0xC9; /* BDOS entry: RET (intercepted below) */
    while (!done) {
        if (z.pc == 5) {
            if (z.bc.l == 2) { putchar(z.de.l); fflush(stdout); }
            else if (z.bc.l == 9) { for (uint16_t a = z.de.w; mem[a] != '$'; a++) putchar(mem[a]); fflush(stdout); }
        }
        if (z.pc == 0) { done = 1; break; }
        z80_step(&z, false);
    }
    printf("\n");
    return 0;
}


static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}

static void write_bmp(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    int row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h * 2;
    uint8_t hdr[54] = {'B', 'M'};
    *(uint32_t *)(hdr + 2) = size; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = w; *(int32_t *)(hdr + 22) = h * 2; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
    fwrite(hdr, 1, 54, f);
    for (int y = h * 2 - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) { uint32_t p = px[(y / 2) * w + x]; uint8_t bgr[3] = {p & 255, (p >> 8) & 255, (p >> 16) & 255}; fwrite(bgr, 1, 3, f); }
        for (int i = 0; i < pad; i++) fputc(0, f);
    }
    fclose(f);
}

static void load_roms(CPC *c, const char *dir, CPCModel m) {
    char p[1024]; size_t n; uint8_t *b;
    const char *os = m == CPC_MODEL_464 ? "OS464.ROM" : m == CPC_MODEL_664 ? "OS664.ROM" : "OS6128.ROM";
    const char *basic = m == CPC_MODEL_464 ? "BASIC1-0.ROM" : m == CPC_MODEL_664 ? "BASIC664.ROM" : "BASIC1-1.ROM";
    snprintf(p, sizeof p, "%s/%s", dir, os); if ((b = slurp(p, &n))) { cpc_load_rom(c, -1, b, n); free(b); } else fprintf(stderr, "missing %s\n", p);
    snprintf(p, sizeof p, "%s/%s", dir, basic); if ((b = slurp(p, &n))) { cpc_load_rom(c, 0, b, n); free(b); } else fprintf(stderr, "missing %s\n", p);
    if (m != CPC_MODEL_464) { snprintf(p, sizeof p, "%s/AMSDOS.ROM", dir); if ((b = slurp(p, &n))) { cpc_load_rom(c, 7, b, n); free(b); } }
}

static int run_tapetest(const char *roms, const char *dir) {
    char wav[1024]; snprintf(wav, sizeof wav, "%s/tape.wav", dir);
    CPC *c = cpc_create(CPC_MODEL_464); load_roms(c, roms, CPC_MODEL_464); cpc_reset(c, true);
    for (int i = 0; i < 120; i++) cpc_run_frame(c);
    cpc_autotype(c, "10 print \"hello from tape\"\nsave \"t\"\n");
    cpc_tape_record_start(c);
    for (int i = 0; i < 1500; i++) { cpc_run_frame(c); if (i == 400) cpc_autotype(c, "\n"); }
    size_t n; uint8_t *w = cpc_tape_record_stop(c, &n);
    FILE *f = fopen(wav, "wb"); fwrite(w, 1, n, f); fclose(f);
    printf("recorded %zu bytes\n", n);
    cpc_destroy(c);
    c = cpc_create(CPC_MODEL_464); load_roms(c, roms, CPC_MODEL_464); cpc_reset(c, true);
    c->tape = tape_load(w, n, "tape.wav"); free(w);
    if (!c->tape) { printf("tape load failed\n"); return 1; }
    for (int i = 0; i < 120; i++) cpc_run_frame(c);
    cpc_autotype(c, "load \"t\"\n");
    tape_play(c->tape, true);
    for (int i = 0; i < 1500; i++) { cpc_run_frame(c); if (i == 300) cpc_autotype(c, "\n"); }
    cpc_autotype(c, "run\n");
    for (int i = 0; i < 100; i++) cpc_run_frame(c);
    char out[1024]; snprintf(out, sizeof out, "%s/tape.bmp", dir);
    write_bmp(out, cpc_frame(c), CPC_SCREEN_W, CPC_SCREEN_H);
    cpc_destroy(c); return 0;
}

static int run_snaptest(const char *roms, const char *dir) {
    CPC *c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    for (int i = 0; i < 120; i++) cpc_run_frame(c);
    cpc_autotype(c, "mode 0:for i=0 to 15:pen i,i+1:print i;:next:a$=\"snap ok\"\n");
    for (int i = 0; i < 600; i++) cpc_run_frame(c);
    size_t n; uint8_t *snap = cpc_snapshot_save(c, 3, 128, true, &n);
    printf("snapshot %zu bytes\n", n);
    cpc_destroy(c);
    c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    if (!cpc_snapshot_load(c, snap, n)) { printf("load failed\n"); return 1; }
    free(snap);
    cpc_autotype(c, "print a$\n");
    for (int i = 0; i < 200; i++) cpc_run_frame(c);
    char out[1024]; snprintf(out, sizeof out, "%s/snap.bmp", dir);
    write_bmp(out, cpc_frame(c), CPC_SCREEN_W, CPC_SCREEN_H);
    cpc_destroy(c); return 0;
}

static uint8_t dis_peek(void *ctx, uint16_t a) { return ((uint8_t *)ctx)[a]; }
static int run_dis(const char *file, uint32_t off, uint32_t base, int count) {
    size_t n; uint8_t *b = slurp(file, &n); if (!b) return 1;
    uint8_t *buf = calloc(1, 65536 + 4); memcpy(buf + base, b + off, n - off > 65536 - base ? 65536 - base : n - off);
    uint16_t pc = (uint16_t)base;
    for (int i = 0; i < count; i++) { char t[64]; int l = z80_disassemble(pc, dis_peek, buf, t, sizeof t); printf("%04X  ", pc); for (int k = 0; k < 4; k++) if (k < l) printf("%02X ", buf[(uint16_t)(pc + k)]); else printf("   "); printf(" %s\n", t); pc += l; }
    return 0;
}

static int run_dbgtest(const char *roms) {
    int fails = 0;
    #define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)
    CPC *c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    Debugger *d = dbg_attach(c);
    int32_t v; char err[128];
    c->z80.hl.w = 0x1234; c->z80.bc.w = 0x00FF; c->z80.af.h = 0x80;
    CHECK(dbg_eval(c, "hl+1", &v, err, sizeof err) && v == 0x1235, "hl+1");
    CHECK(dbg_eval(c, "(b and #f) = 0 and c = 255", &v, err, sizeof err) && v == -1, "bitwise/compare");
    CHECK(dbg_eval(c, "#FF + &10 + %101 + 10", &v, err, sizeof err) && v == 255 + 16 + 5 + 10, "number formats");
    CHECK(dbg_eval(c, "hibyte(hl) = #12 and byte(hl)=#34", &v, err, sizeof err) && v == -1, "functions");
    CHECK(!dbg_eval(c, "foo + 1", &v, err, sizeof err), "unknown name rejected");
    dbg_symbol_set(c, "start", 0x4000);
    CHECK(dbg_eval(c, "start + 2", &v, err, sizeof err) && v == 0x4002, "symbols");
    /* code breakpoint on the interrupt handler */
    int bp = dbg_add_code_bp(c, 0x0038);
    CHECK(dbg_set_condition(c, bp, "hl >= 0", 0), "condition accepted");
    for (int i = 0; i < 300 && !d->hit; i++) cpc_run_frame(c);
    CHECK(d->hit && c->z80.pc == 0x0038, "code breakpoint at 0038 hit");
    d->hit = false; d->skip_once = true;
    dbg_step_into(c);
    CHECK(c->z80.pc == 0xB941 || c->z80.pc != 0x0038, "step into moves");
    /* IO breakpoint on gate array writes */
    dbg_clear_bps(c);
    dbg_add_io_bp(c, 0x7F00, 0xC000, false, true);
    d->hit = false; d->skip_once = true;
    for (int i = 0; i < 300 && !d->hit; i++) cpc_run_frame(c);
    CHECK(d->hit, "io breakpoint hit");
    /* memory write breakpoint */
    dbg_clear_bps(c);
    dbg_add_mem_bp(c, 0xB8B4, 0xB8B4, false, true);
    d->hit = false; d->skip_once = true;
    for (int i = 0; i < 300 && !d->hit; i++) cpc_run_frame(c);
    CHECK(d->hit && d->ev_addr == 0xB8B4, "memory write breakpoint hit");
    /* step over a CALL */
    dbg_clear_bps(c); d->hit = false;
    c->z80.pc = 0x0100; cpc_poke(c, 0x4000, 0xCD); cpc_poke(c, 0x4001, 0x00); cpc_poke(c, 0x4002, 0x50); cpc_poke(c, 0x5000, 0xC9);
    c->z80.pc = 0x4000; c->z80.sp.w = 0x9000;
    CHECK(dbg_step_over(c), "step over CALL requests run");
    for (int i = 0; i < 100000 && !d->hit; i++) cpc_step(c);
    CHECK(d->hit && c->z80.pc == 0x4003, "returned after CALL");
    cpc_destroy(c);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}

static int run_mftest(const char *roms) {
    int fails = 0;
    CPC *c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128);
    CPCConfig cfg = c->cfg; cfg.multiface = true; cpc_configure(c, &cfg);
    uint8_t rom[8192] = {0};
    /* NMI handler: LD A,42h / LD (2000h),A / RETN */
    const uint8_t h[] = {0x3E, 0x42, 0x32, 0x00, 0x20, 0xED, 0x45};
    memcpy(rom + 0x66, h, sizeof h); rom[0] = 0xA5;
    CHECK(cpc_load_multiface(c, rom, sizeof rom), "load multiface");
    cpc_reset(c, true);
    for (int i = 0; i < 100; i++) cpc_run_frame(c);
    uint8_t lower0 = cpc_mem_read(c, 0);
    CHECK(c->rom_multiface[0x3FFF] == c->ga_ram || (c->rom_multiface[0x3FFF] & 0xC0) == 0xC0, "RAM config snooped");
    CHECK(c->rom_multiface[0x3CFF] <= 17, "CRTC select snooped");
    uint16_t pc = c->z80.pc;
    CHECK(cpc_multiface_stop(c), "stop accepted");
    CHECK(!cpc_multiface_stop(c), "second stop ignored while running");
    CHECK(cpc_mem_read(c, 0) == 0xA5, "MF ROM paged in");
    cpc_step(c); CHECK(c->z80.pc == 0x66, "NMI vector");
    for (int i = 0; i < 3; i++) cpc_step(c);
    CHECK(c->z80.pc == pc, "RETN back to stop address");
    CHECK(!c->mf_running, "running cleared by RETN");
    CHECK(cpc_mem_read(c, 0x2000) == 0x42, "MF RAM written");
    cpc_io_write(c, 0xFEEA, 0); CHECK(cpc_mem_read(c, 0) == lower0, "FEEA pages out");
    cpc_io_write(c, 0xFEE8, 0); CHECK(cpc_mem_read(c, 0) == 0xA5, "FEE8 pages in");
    cpc_io_write(c, 0xFEEA, 0);
    for (int i = 0; i < 50; i++) cpc_run_frame(c);
    CHECK(cpc_mem_read(c, 0x2000) != 0x42 || lower0 == 0x42, "machine runs after stop");
    cpc_destroy(c);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}

static int run_inttest(const char *roms) {
    CPC *c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    for (int i = 0; i < 100; i++) cpc_run_frame(c);
    uint64_t last = 0, f0 = c->cycles; int n = 0; bool vs = false;
    while (c->cycles - f0 < 19968 * 4) {
        cpc_step(c);
        if (c->crtc.vsync != vs) { vs = c->crtc.vsync; if (vs) printf("  VSYNC at %llu\n", (unsigned long long)(c->cycles - f0)); }
        if (c->z80.int_ack) { printf("INT at %llu (+%llu)\n", (unsigned long long)(c->cycles - f0), (unsigned long long)(c->cycles - last)); last = c->cycles; n++; }
    }
    printf("%d interrupts in 4 frames\n", n);
    cpc_destroy(c); return 0;
}

static int run_symtest(const char *path, const char *name) {
    CPC *c = cpc_create(CPC_MODEL_6128); dbg_attach(c);
    int n = dbg_symbols_load(c, path); int32_t v = -1;
    bool ok = dbg_symbol_lookup(c->dbg, name, &v);
    const char *back = ok ? dbg_symbol_for_address(c->dbg, (uint16_t)v) : NULL;
    printf("%d symbols; %s = %s%04X; address -> %s\n", n, name, ok ? "" : "(missing) ", v & 0xFFFF, back ? back : "(none)");
    cpc_destroy(c); return ok ? 0 : 1;
}

static int run_steptest(const char *roms, const char *disc) {
    int fails = 0;
    CPC *c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    for (int i = 0; i < 100; i++) cpc_run_frame(c);
    Debugger *d = dbg_attach(c);
    /* HALT: EI / HALT / JR -3 at #4000 */
    cpc_poke(c, 0x4000, 0xFB); cpc_poke(c, 0x4001, 0x76); cpc_poke(c, 0x4002, 0x18); cpc_poke(c, 0x4003, 0xFC);
    c->z80.pc = 0x4000; c->z80.halted = false;
    dbg_step_into(c); dbg_step_into(c);   /* EI, then HALT completes into the interrupt */
    CHECK(!c->z80.halted && c->z80.pc != 0x4001 && c->z80.pc != 0x4002, "step over HALT reaches the interrupt");
    printf("  after HALT pc=%04X\n", c->z80.pc);
    cpc_destroy(c);
    /* F8 x300 on a running game: PC must change unless the instruction loops on itself */
    c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    CPCDisc *dk = disc ? dsk_load_file(disc) : NULL; if (dk) fdc_insert(c->fdc, 0, dk);
    for (int i = 0; i < 100; i++) cpc_run_frame(c);
    if (dk) { cpc_autotype(c, "run\"boop\n"); for (int i = 0; i < 700; i++) cpc_run_frame(c); }
    d = dbg_attach(c);
    int stuck = 0, runs = 0;
    for (int i = 0; i < 300; i++) {
        uint16_t pc = c->z80.pc; uint8_t op = cpc_mem_read(c, pc), op2 = cpc_mem_read(c, pc + 1);
        bool self_loop = (op == 0x18 && op2 == 0xFE) || (op == 0x10 && op2 == 0xFE) || (op == 0xED && (op2 & 0xF0) == 0xB0);
        if (dbg_step_over(c)) { runs++; d->hit = false; for (int f = 0; f < 500 && !d->hit; f++) cpc_run_frame(c); }
        if (c->z80.pc == pc && !self_loop) { stuck++; printf("  stuck at %04X op %02X %02X\n", pc, op, op2); }
    }
    CHECK(stuck == 0, "F8 always advances");
    /* Step Out: step into a CALL, then run until the routine returns */
    int found = 0;
    for (int i = 0; i < 2000 && !found; i++) {
        uint16_t pc = c->z80.pc; uint8_t op = cpc_mem_read(c, pc);
        if (op == 0xCD) {
            uint16_t ret = (uint16_t)(pc + 3), sp = c->z80.sp.w;
            dbg_step_into(c);
            dbg_step_out(c); d->hit = false;
            for (int f = 0; f < 500 && !d->hit; f++) cpc_run_frame(c);
            CHECK(c->z80.pc == ret && c->z80.sp.w == sp, "step out returns after the CALL");
            printf("  CALL at %04X -> step out at %04X (expected %04X)\n", pc, c->z80.pc, ret);
            found = 1;
        } else dbg_step_into(c);
    }
    printf("  %d call/block step-overs, final pc=%04X\n", runs, c->z80.pc);
    cpc_destroy(c);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}

static int run_logtest(const char *roms, const char *disc) {
    int fails = 0;
    CPC *c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    CPCDisc *dk = dsk_load_file(disc); fdc_insert(c->fdc, 0, dk);
    for (int i = 0; i < 100; i++) cpc_run_frame(c);
    cpc_autotype(c, "run\"boop\n"); for (int i = 0; i < 700; i++) cpc_run_frame(c);
    Debugger *d = dbg_attach(c);
    int i = dbg_add_code_bp(c, 0x6BB9);
    dbg_set_log(c, i, "scan A={A} HL={HL} frames={peek(#B8B4):d}");
    for (int f = 0; f < 50; f++) cpc_run_frame(c);
    CHECK(!d->hit, "logpoint does not stop the machine");
    CHECK(dbg_log_total(d) >= 40, "logpoint logged every frame");
    printf("  %u lines, last: %s\n", dbg_log_total(d), dbg_log_line(d, dbg_log_total(d) - 1));
    dbg_set_log(c, i, "");
    for (int f = 0; f < 5 && !d->hit; f++) cpc_run_frame(c);
    CHECK(d->hit, "clearing the message makes it a normal breakpoint again");
    cpc_destroy(c);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}

/* ---- shared scripted session used by the state and replay tests (boop: load, menu, start) ---- */
static CPC *script_boot(const char *roms, const char *disc) {
    CPC *c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    CPCDisc *dk = dsk_load_file(disc); fdc_insert(c->fdc, 0, dk); return c;
}
static void script_frame(CPC *c, int i) {
    if (i == 100) cpc_autotype(c, "run\"boop\n");
    if (i == 900) cpc_key(c, CPCK_SPACE, true); if (i == 905) cpc_key(c, CPCK_SPACE, false);
    if (i == 1000) cpc_key(c, CPCK_RETURN, true); if (i == 1005) cpc_key(c, CPCK_RETURN, false);
    if (i == 1100) cpc_key(c, CPCK_DOWN, true); if (i == 1104) cpc_key(c, CPCK_DOWN, false);
    if (i == 1200) cpc_key(c, CPCK_RETURN, true); if (i == 1205) cpc_key(c, CPCK_RETURN, false);
    cpc_run_frame(c);
}

static int run_statetest(const char *roms, const char *disc) {
    int fails = 0;
    #define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)
    int points[] = {150, 180, 260, 400, 700, 905, 1010, 1102, 1203};
    for (size_t pi = 0; pi < sizeof points / sizeof *points; pi++) {
        for (int mid = 0; mid < 2; mid++) {
            int K = points[pi];
            CPC *a = script_boot(roms, disc);
            for (int i = 0; i < K; i++) script_frame(a, i);
            if (mid) for (int n = 0; n < 4321; n++) cpc_step(a);        /* stop inside a frame, between instructions */
            CPCState *s = cpc_state_save(a); uint64_t h0 = cpc_state_hash(a);
            /* a machine that has done something else entirely */
            CPC *b = script_boot(roms, disc);
            for (int i = 0; i < K + 37; i++) { if (i == 120) cpc_key(b, CPCK_A, true); if (i == 130) cpc_key(b, CPCK_A, false); cpc_run_frame(b); }
            unsigned hz = 99; bool ok = cpc_state_restore(b, s, &hz);
            bool same0 = cpc_state_hash(b) == h0;
            int bad = -1;
            for (int i = K; i < K + 200 && bad < 0; i++) {
                if (!mid || i > K) { script_frame(a, i); script_frame(b, i); } else { cpc_run_frame(a); cpc_run_frame(b); }
                if (cpc_state_hash(a) != cpc_state_hash(b)) bad = i - K;
            }
            char msg[160]; snprintf(msg, sizeof msg, "frame %d%s: restore ok=%d hazards=%u identical-at-restore=%d, identical for 200 frames=%s (state %zu bytes)",
                                    K, mid ? "+4321 instr" : "", ok, hz, same0, bad < 0 ? "yes" : "NO", cpc_state_size(s));
            CHECK(ok && hz == 0 && same0 && bad < 0, msg);
            if (bad >= 0) printf("  diverged %d frames after restore\n", bad);
            /* byte form round trip */
            size_t bn; const uint8_t *bytes = cpc_state_bytes(s, &bn); CPCState *s2 = cpc_state_from_bytes(bytes, bn);
            CHECK(s2 && cpc_state_restore(b, s2, NULL) && cpc_state_hash(b) == h0, "byte round trip restores the same state");
            cpc_state_free(s2); cpc_state_free(s); cpc_destroy(a); cpc_destroy(b);
            if (mid == 0 && K != 150 && K != 1010) break;      /* mid-frame variants only at a few points */
        }
    }
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static void rec_frame(CPC *c, int i) {          /* script_frame plus inputs that exercise every event type */
    if (i == 1300) cpc_joystick(c, 4, true);
    if (i == 1310) cpc_joystick(c, 4, false);
    if (i == 1320) cpc_amx_mouse(c, 5, -3, 1);
    if (i == 1330) cpc_amx_mouse(c, 0, 0, 0);
    if (i == 1400) cpc_reset(c, false);
    if (i == 1440) cpc_autotype(c, "PRINT 1+1\n");
    script_frame(c, i);
}

static int run_rectest(const char *roms, const char *disc) {
    int fails = 0;
    #define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)
    /* 0. two machines that never shared state hash the same (no padding/pointer noise in the hash) */
    { CPC *a = script_boot(roms, disc), *b = script_boot(roms, disc); int bad = 0;
      for (int i = 0; i < 400; i++) { script_frame(a, i); script_frame(b, i); if (i % 50 == 0 && cpc_state_hash(a) != cpc_state_hash(b)) bad++; }
      CHECK(!bad, "independent machines running the same inputs have identical state hashes"); cpc_destroy(a); cpc_destroy(b); }
    /* 1. record from the middle of the disc load (auto-type still typing) */
    const int START = 105, END = 1500;
    CPC *a = script_boot(roms, disc);
    for (int i = 0; i < START; i++) rec_frame(a, i);
    CHECK(cpc_record_start(a), "recording starts");
    uint64_t base = a->frames; static uint64_t ref[3000];
    ref[0] = cpc_state_hash(a);
    for (int i = START; i < END; i++) { rec_frame(a, i); ref[a->frames - base] = cpc_state_hash(a); }
    CPCRecording *rec = cpc_record_stop(a);
    printf("  recorded %llu frames, %llu input events, taint=%s\n", (unsigned long long)cpc_recording_frames(rec), (unsigned long long)cpc_recording_events(rec), cpc_recording_taint(rec) ? cpc_recording_taint(rec) : "none");
    CHECK(!cpc_recording_taint(rec) && cpc_recording_frames(rec) == (uint64_t)(END - START), "recording is clean and has the expected length");
    size_t fn; uint8_t *file = cpc_recording_serialize(rec, &fn); CPCRecording *rec2 = cpc_recording_deserialize(file, fn);
    printf("  file: %zu bytes\n", fn);
    CHECK(rec2 && cpc_recording_frames(rec2) == cpc_recording_frames(rec) && cpc_recording_events(rec2) == cpc_recording_events(rec), "file round trip");
    free(file); cpc_recording_free(rec); rec = rec2;
    CHECK(cpc_recording_deserialize((const uint8_t *)"garbage", 7) == NULL, "garbage is rejected");

    /* 2. replay in a machine that was somewhere else entirely, host input ignored */
    CPC *b = script_boot(roms, disc);
    for (int i = 0; i < 700; i++) { if (i == 50) cpc_key(b, CPCK_Z, true); cpc_run_frame(b); }
    CPCPlayer *p = cpc_player_create(rec); unsigned hz = 99;
    CHECK(cpc_player_begin(b, p, &hz) && hz == 0, "replay begins, no hazards");
    int bad = -1, n = 0;
    while (!cpc_player_finished(b)) {
        if (n == 300) cpc_key(b, CPCK_Q, true);             /* must be ignored */
        cpc_run_frame(b); n++;
        if (cpc_state_hash(b) != ref[b->frames - base] && bad < 0) bad = n;
    }
    CHECK(bad < 0, "forward replay matches the recording at every frame");
    if (bad >= 0) printf("  first mismatch at frame %d\n", bad);
    uint64_t df; CHECK(!cpc_player_desync(b, &df), "no desync detected by the checkpoints");
    printf("  keyframes cached: %zu (%zu KB)\n", cpc_player_keyframes(b), cpc_player_keyframe_bytes(b) / 1024);

    /* 3. seeking: random targets, forward and back, and from the middle of a frame */
    unsigned long long seed = 12345; int bads = 0; double worst = 0, total = 0; int seeks = 80;
    for (int k = 0; k < seeks; k++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t f = (seed >> 33) % (cpc_recording_frames(rec) + 1);
        double t0 = now_ms(); bool ok = cpc_player_seek(b, f); double dt = now_ms() - t0; total += dt; if (dt > worst) worst = dt;
        if (!ok || cpc_state_hash(b) != ref[f] || cpc_player_frame(b) != f) { bads++; printf("  seek to %llu failed (ok=%d)\n", (unsigned long long)f, ok); }
        if (k % 10 == 3) { for (int s = 0; s < 1234; s++) cpc_step(b); }          /* leave it mid-frame, then seek to the same frame */
        if (k % 10 == 3) { ok = cpc_player_seek(b, f); if (!ok || cpc_state_hash(b) != ref[f]) { bads++; printf("  re-seek mid-frame to %llu failed\n", (unsigned long long)f); } }
    }
    CHECK(!bads, "80 random seeks land on the exact recorded state");
    printf("  seek time: avg %.2f ms, worst %.2f ms\n", total / seeks, worst);
    CHECK(!cpc_player_desync(b, &df), "still no desync after seeking");
    /* play on from a seek and reach the end exactly */
    cpc_player_seek(b, 700); while (!cpc_player_finished(b)) cpc_run_frame(b);
    CHECK(cpc_state_hash(b) == ref[cpc_recording_frames(rec)], "playing on from a seek ends in the recorded final state");
    /* negative control: an input the recording does not contain must be caught as a desync */
    { CPC *x = script_boot(roms, disc); CPCPlayer *q = cpc_player_create(rec); cpc_player_begin(x, q, NULL); int hit = -1;
      for (int i = 0; !cpc_player_finished(x); i++) { if (i == 420) cpc_key_raw(x, CPCK_X, true); cpc_run_frame(x); if (hit < 0 && cpc_state_hash(x) != ref[x->frames - base]) hit = i; }
      uint64_t where; bool ds = cpc_player_desync(x, &where);
      CHECK(hit >= 420 && ds, "an injected input is detected (hash mismatch and desync flag)");
      printf("  injected at frame 420: first differing hash at %d, checkpoint desync reported at frame %llu\n", hit, (unsigned long long)where);
      cpc_player_free(q); cpc_destroy(x); }
    /* an action replay cannot reproduce ends the recording there; what came before is still exact */
    { CPC *t = script_boot(roms, disc); for (int i = 0; i < 200; i++) script_frame(t, i);
      cpc_record_start(t); uint64_t tb = t->frames; static uint64_t th[400]; th[0] = cpc_state_hash(t);
      int i = 200; for (; i < 300; i++) { script_frame(t, i); th[t->frames - tb] = cpc_state_hash(t); }
      CHECK(cpc_recording_active(t) && !cpc_record_cut_reason(t), "recording is active before the edit");
      cpc_poke(t, 0x8000, 0x42);
      CHECK(!cpc_recording_active(t) && cpc_record_cut_reason(t), "editing memory cuts the recording");
      for (; i < 330; i++) script_frame(t, i);
      CPCRecording *cut = cpc_record_stop(t);
      CHECK(cut && cpc_recording_frames(cut) == 100 && cpc_recording_taint(cut), "the cut recording keeps everything before the edit");
      CPC *u = script_boot(roms, disc); CPCPlayer *up = cpc_player_create(cut); cpc_player_begin(u, up, NULL); int ok = 1;
      while (!cpc_player_finished(u)) { cpc_run_frame(u); if (cpc_state_hash(u) != th[u->frames - tb]) ok = 0; }
      CHECK(ok, "the cut recording replays exactly");
      cpc_player_free(up); cpc_recording_free(cut); cpc_destroy(t); cpc_destroy(u); }
    /* pictures for reverse playback match the displayed frames */
    { int bad2 = 0; uint64_t froms[] = {0, 50, 123, 1000, cpc_recording_frames(rec) - 40}; static uint32_t pics[50 * CPC_SCREEN_W * CPC_SCREEN_H];
      for (size_t q = 0; q < sizeof froms / sizeof *froms; q++) {
          int got = cpc_player_capture(b, froms[q], 50, pics);
          for (int i = 0; i < got; i += 7) { cpc_player_seek(b, froms[q] + 1 + i); if (memcmp(pics + (size_t)i * CPC_SCREEN_W * CPC_SCREEN_H, b->frame_done, (size_t)CPC_SCREEN_W * CPC_SCREEN_H * 4)) bad2++; }
          if (got < 1) bad2++;
      }
      CHECK(!bad2, "captured pictures equal the frames the player displays"); }
    /* background indexing: a clone replays everything and hands its keyframes to a player that had none */
    { CPC *d = script_boot(roms, disc); CPCPlayer *dp = cpc_player_create(rec); cpc_player_begin(d, dp, NULL);
      CPC *cl = cpc_clone_machine(b); CPCPlayer *cp = cpc_player_create(rec); unsigned chz = 99;
      CHECK(cpc_player_begin(cl, cp, &chz) && chz == 0, "a clone of the machine starts the replay with no hazards");
      CHECK(cpc_player_indexed(d) == 0, "nothing is indexed before the clone has run");
      double t0 = now_ms(); while (!cpc_player_finished(cl)) cpc_run_frame(cl);
      double indexing = now_ms() - t0; size_t moved = cpc_player_adopt_keyframes(d, cp);
      CHECK(!cpc_player_desync(cl, &df) && cpc_state_hash(cl) == ref[cpc_recording_frames(rec)], "the clone ends in the recorded final state");
      printf("  indexing %llu frames took %.0f ms, %zu keyframes adopted\n", (unsigned long long)cpc_recording_frames(rec), indexing, moved);
      CHECK(cpc_player_indexed(d) == cpc_recording_frames(rec), "the whole recording is indexed");
      int badx = 0; double worstx = 0; seed = 99;
      for (int k = 0; k < 60; k++) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; uint64_t f = (seed >> 33) % (cpc_recording_frames(rec) + 1);
          double s0 = now_ms(); bool ok = cpc_player_seek(d, f); double dt = now_ms() - s0; if (dt > worstx) worstx = dt;
          if (!ok || cpc_state_hash(d) != ref[f]) badx++; }
      CHECK(!badx, "seeking on the indexed player is exact everywhere");
      printf("  worst seek on the indexed player: %.1f ms\n", worstx);
      /* ---- reverse debugging on the indexed player ---- */
      Debugger *dd = dbg_attach(d); int badr = 0; unsigned long long rs = 777;
      for (int k = 0; k < 50; k++) {
          rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
          uint64_t f = k < 8 ? (uint64_t)(k * 150 + 50) : 5 + (rs >> 33) % (cpc_recording_frames(rec) - 10);    /* first ones sit exactly on keyframes */
          cpc_player_seek(d, f);
          int extra = k < 8 ? k % 3 : (int)((rs >> 20) % 30000); for (int s = 0; s < extra; s++) cpc_step(d);
          uint64_t h0 = cpc_state_hash(d), t0 = d->cycles;
          bool ok = cpc_player_step_back(d); uint64_t tb = d->cycles;
          if (extra == 0 && f == 0) continue;
          cpc_step(d);
          if (!ok || !(tb < t0) || cpc_state_hash(d) != h0) { badr++; printf("  step back failed at frame %llu +%d (ok=%d, %llu -> %llu)\n", (unsigned long long)f, extra, ok, (unsigned long long)t0, (unsigned long long)tb); }
      }
      CHECK(!badr, "stepping forward after stepping back restores the exact state (50 positions, keyframe boundaries included)");
      { uint64_t f0 = 900; cpc_player_seek(d, f0); size_t before = dbg_history_count(dd); (void)before;
        cpc_player_goto_cycles(d, d->cycles + 5000, true);
        CHECK(dbg_history_count(dd) > 100 && dbg_history_get(dd, 0).pc != 0, "positioning refills the execution history"); }
      /* find bytes of game RAM that change often, to use as watch targets */
      cpc_player_seek(d, 900); static uint8_t snap[65536]; static int changes[65536];
      for (int f = 0; f < 80; f++) { for (int a = 0; a < 65536; a++) snap[a] = cpc_peek(d, (uint16_t)a); cpc_run_frame(d);
                                     for (int a = 0x4000; a < 0xB000; a++) if (cpc_peek(d, (uint16_t)a) != snap[a]) changes[a]++; }
      int targets[3], nt = 0; for (int a = 0x4000; a < 0xB000 && nt < 3; a += 1) if (changes[a] >= 3 && changes[a] <= 40) { targets[nt++] = a; a += 97; }
      printf("  watch targets:"); for (int i = 0; i < nt; i++) printf(" %04X(%d changes)", targets[i], changes[targets[i]]); printf("\n");
      int badw = 0, usedw = 0;
      for (int i = 0; i < nt; i++) for (int mode = 0; mode < 2; mode++) for (int mid = 0; mid < 2; mid++) {
          uint16_t a = (uint16_t)targets[i]; bool want_read = mode == 1;
          /* ground truth: forward from frame 900 with a real watch, last hit before X */
          cpc_player_seek(d, 900);
          int bp = dbg_add_mem_bp(d, a, a, want_read, !want_read); CPCReverseHit truth = {0}; dd->active = true; dd->hit = false;
          uint64_t X = 0; int steps_after = mid ? 1234 : 0;
          for (int f = 0; f < 80; f++) { cpc_run_frame(d); dd->hit = false; }       /* run 80 frames, then stop on a frame boundary */
          cpc_player_seek(d, 900);
          { uint64_t end_frame = d->frames + 80; dd->hit = false;
            while (d->frames < end_frame) { uint64_t t = d->cycles; uint16_t pc = d->z80.pc; cpc_step(d);
                if (dd->hit) { truth = (CPCReverseHit){true, t, pc, 0, bp, dd->ev_addr, dd->ev_value, dd->ev_prev}; dd->hit = false; } }
            for (int s = 0; s < steps_after; s++) { uint64_t t = d->cycles; uint16_t pc = d->z80.pc; cpc_step(d);
                if (dd->hit) { truth = (CPCReverseHit){true, t, pc, 0, bp, dd->ev_addr, dd->ev_value, dd->ev_prev}; dd->hit = false; } }
            X = d->cycles; }
          dbg_remove_bp(d, bp); dd->hit = false; dd->active = true;
          uint64_t hx = cpc_state_hash(d);
          if (!truth.found) continue;
          usedw++;
          CPCReverseScan *sc = cpc_reverse_begin(d, mode == 1 ? CPC_REV_READ : CPC_REV_WRITE, a, a); CPCReverseHit got = {0};
          int slices = 0; while (!cpc_reverse_slice(d, sc)) slices++;
          bool found = cpc_reverse_end(d, sc, true, &got);
          bool same = found && got.cycles == truth.cycles && got.pc == truth.pc && got.addr == a && d->cycles == truth.cycles && d->z80.pc == truth.pc;
          if (!same) { badw++; printf("  watch %04X %s: expected t=%llu pc=%04X, got found=%d t=%llu pc=%04X (machine at %llu pc=%04X)\n", a, want_read ? "read" : "write", (unsigned long long)truth.cycles, truth.pc, found, (unsigned long long)got.cycles, got.pc, (unsigned long long)d->cycles, d->z80.pc); }
          /* cancel path: back to where it started, exactly */
          sc = cpc_reverse_begin(d, mode == 1 ? CPC_REV_READ : CPC_REV_WRITE, a, a); cpc_reverse_slice(d, sc); cpc_reverse_end(d, sc, false, NULL);
          (void)hx; (void)slices;
      }
      CHECK(!badw && usedw >= 4, "reverse watch finds the latest write/read exactly where a forward watch says (and stops before the instruction)");
      /* breakpoints: user counters, pass counts and logpoints are left alone */
      { cpc_player_seek(d, 1000); for (int f = 0; f < 60; f++) cpc_run_frame(d);
        int bp = dbg_add_code_bp(d, 0x6BB9); dd->bp[bp].pass_count = 7; dd->bp[bp].count = 3; dd->bp[bp].hits = 2;
        int lg = dbg_add_code_bp(d, 0x6BB9 + 3); dbg_set_log(d, lg, "nope"); uint32_t logs0 = dbg_log_total(dd);
        uint64_t X = d->cycles, hx = cpc_state_hash(d);
        /* truth: where was PC at 6BB9 last? */
        cpc_player_seek(d, 1000); dd->active = true; CPCReverseHit truth = {0};
        int tb = dbg_add_code_bp(d, 0x6BB9); dd->bp[tb].block = NULL; dd->scanning = true;
        while (d->cycles < X) { uint64_t t = d->cycles; uint16_t pc = d->z80.pc; int n = cpc_step(d); if (dd->hit) { truth = (CPCReverseHit){true, t, pc, 1, tb}; } dd->hit = false; if (n == 0) dd->skip_once = true; }
        dd->scanning = false; dbg_remove_bp(d, tb); dbg_set_log(d, tb, ""); dd->skip_once = false;
        cpc_player_goto_cycles(d, X, true);
        CHECK(cpc_state_hash(d) == hx, "returned to the starting position");
        CPCReverseScan *sc = cpc_reverse_begin(d, CPC_REV_BREAKPOINTS, 0, 0); CPCReverseHit got = {0}; while (!cpc_reverse_slice(d, sc)) {}
        bool found = cpc_reverse_end(d, sc, true, &got);
        CHECK(found && truth.found && got.cycles == truth.cycles && got.kind == 1 && d->z80.pc == 0x6BB9, "reverse continue finds the last time a code breakpoint was reached");
        CHECK(dd->bp[bp].pass_count == 7 && dd->bp[bp].count == 3 && dd->bp[bp].hits == 2 && dbg_log_total(dd) == logs0, "the user's counters, pass counts and logpoints are untouched");
        /* nothing earlier: stays put */
        cpc_player_seek(d, 1000); for (int f = 0; f < 30; f++) cpc_run_frame(d); uint64_t hy = cpc_state_hash(d);
        for (int i = 0; i < DBG_MAX_BP; i++) dd->bp[i].used = false; dbg_rebuild_watch(dd);
        uint64_t co0 = d->cycles; int nsl = 0;
        sc = cpc_reverse_begin(d, CPC_REV_CODE, 0x0010, 0); while (!cpc_reverse_slice(d, sc)) nsl++;
        found = cpc_reverse_end(d, sc, true, NULL);


        CHECK(!found && cpc_state_hash(d) == hy, "no earlier hit: the machine stays where it was"); }
      cpc_player_free(cp); cpc_clone_free(cl); cpc_player_free(dp); cpc_destroy(d); }
    /* take control: the machine continues live */
    cpc_player_end(b); cpc_key(b, CPCK_A, true); cpc_run_frame(b);
    CHECK(!cpc_player_active(b) && b->kbd[CPCK_A >> 3] != 0xFF, "after leaving the replay the machine takes input again");
    cpc_player_free(p); cpc_recording_free(rec); cpc_destroy(a); cpc_destroy(b);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}


/* Sessions that write to the disc: the written tracks travel with the states, replay starts from the recorded disc. */
static int run_disctest(const char *roms, const char *disc) {
    int fails = 0;
    #define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)
    enum { START = 120, N = 700 };
    static uint64_t href[N + 1], cref[N + 1];
    CPC *a = script_boot(roms, disc);
    for (int i = 0; i < START; i++) cpc_run_frame(a);
    CPCDisc *da = fdc_disc(a->fdc, 0);
    uint32_t crc0 = dsk_content_crc(da);
    CHECK(cpc_record_start(a), "recording starts");
    uint64_t base = a->frames; href[0] = cpc_state_hash(a); cref[0] = dsk_content_crc(da);
    cpc_autotype(a, "openout\"t.tmp\":print#9,\"hello disc\":closeout\n");
    for (int i = 0; i < N; i++) { cpc_run_frame(a); href[i + 1] = cpc_state_hash(a); cref[i + 1] = dsk_content_crc(da); }
    CPCRecording *rec = cpc_record_stop(a);
    CHECK(rec && !cpc_recording_taint(rec), "the recording is clean although the disc was written");
    uint32_t crc1 = dsk_content_crc(da);
    CHECK(da->writes > 0 && crc1 != crc0, "the program wrote a file on the disc");
    CHECK(cpc_recording_disc_matches(rec, 0, da), "the disc after the session still matches the recording");
    size_t fn; uint8_t *file = cpc_recording_serialize(rec, &fn); CPCRecording *rec2 = cpc_recording_deserialize(file, fn); free(file);
    CHECK(rec2 != NULL, "a recording with disc pre-images survives the file round trip");
    cpc_recording_free(rec); rec = rec2;
    /* replay on the machine whose disc already holds the written file */
    CPCPlayer *p = cpc_player_create(rec); unsigned hz = 99;
    CHECK(cpc_player_begin(a, p, &hz) && hz == 0, "replay begins on the already written disc");
    CHECK(dsk_content_crc(da) == crc0, "the disc is put back as it was when recording began");
    int bad = -1;
    for (int n = 1; n <= N && bad < 0; n++) { cpc_run_frame(a); if (cpc_state_hash(a) != href[n] || dsk_content_crc(da) != cref[n]) bad = n; }
    CHECK(bad < 0, "forward replay reproduces the state and the disc at every frame");
    if (bad >= 0) printf("  first mismatch at frame %d\n", bad);
    uint64_t df; CHECK(!cpc_player_desync(a, &df), "no desync reported");
    /* seeks across the write, forwards and backwards */
    srand(7); int sbad = 0;
    for (int k = 0; k < 40; k++) {
        int f = rand() % (N + 1);
        cpc_player_seek(a, (uint64_t)f);
        if (cpc_state_hash(a) != href[f] || dsk_content_crc(da) != cref[f]) { sbad++; printf("  seek %d: hash %s disc %s\n", f, cpc_state_hash(a) == href[f] ? "ok" : "BAD", dsk_content_crc(da) == cref[f] ? "ok" : "BAD"); }
    }
    CHECK(!sbad, "40 random seeks restore the exact state and the exact disc");
    cpc_player_seek(a, 0);
    CHECK(dsk_content_crc(da) == crc0, "seeking to the start undoes the disc writes");
    /* the clone used for indexing ends with the same disc */
    { CPC *cl = cpc_clone_machine(a); CPCPlayer *cp = cpc_player_create(rec); unsigned h2;
      CHECK(cpc_player_begin(cl, cp, &h2), "a clone starts the replay");
      while (!cpc_player_finished(cl)) cpc_run_frame(cl);
      CHECK(cpc_state_hash(cl) == href[N] && dsk_content_crc(fdc_disc(cl->fdc, 0)) == cref[N], "the clone reaches the same state and disc");
      cpc_player_free(cp); cpc_clone_free(cl); }
    /* a pristine copy of the disc matches too and replays the same */
    { CPC *b = script_boot(roms, disc);
      CHECK(cpc_recording_disc_matches(rec, 0, fdc_disc(b->fdc, 0)), "the pristine disc matches the recording");
      for (int i = 0; i < 300; i++) cpc_run_frame(b);
      CPCPlayer *pb = cpc_player_create(rec);
      CHECK(cpc_player_begin(b, pb, &hz), "replay begins on a fresh machine");
      while (!cpc_player_finished(b)) cpc_run_frame(b);
      CHECK(cpc_state_hash(b) == href[N], "a fresh machine ends in the same state");
      cpc_player_free(pb); cpc_destroy(b); }
    /* taking control keeps the disc as it is at that point */
    cpc_player_seek(a, 400); uint32_t cut = dsk_content_crc(da); cpc_player_end(a);
    CHECK(dsk_content_crc(da) == cut, "leaving the replay leaves the disc as it was");
    cpc_player_free(p); cpc_recording_free(rec); cpc_destroy(a);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}


/* Reverse Step Over on a small program: calls (nested, longer than a keyframe interval), a block instruction, plain steps. */
static int run_sotest(const char *roms) {
    int fails = 0;
    #define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)
    static const uint8_t main_code[] = {0xF3, 0x31,0x00,0x80, 0x00, 0xCD,0x00,0x41, 0x00, 0x21,0x00,0x60, 0x11,0x00,0x61, 0x01,0x00,0x04,
                                        0xED,0xB0, 0x3E,0x05, 0xF5, 0xC1, 0x18,0xFE};
    static const uint8_t sub1[] = {0x00, 0xC5, 0xCD,0x00,0x42, 0xC1, 0xC9};
    static const uint8_t sub2[] = {0x16,0x04, 0x01,0x00,0x00, 0x0B, 0x78, 0xB1, 0x20,0xFB, 0x15, 0x20,0xF5, 0xC9};
    CPC *a = cpc_create(CPC_MODEL_6128); load_roms(a, roms, CPC_MODEL_6128); cpc_reset(a, true);
    for (size_t i = 0; i < sizeof main_code; i++) cpc_poke(a, 0x4000 + i, main_code[i]);
    for (size_t i = 0; i < sizeof sub1; i++) cpc_poke(a, 0x4100 + i, sub1[i]);
    for (size_t i = 0; i < sizeof sub2; i++) cpc_poke(a, 0x4200 + i, sub2[i]);
    a->z80.pc = 0x4000; a->z80.iff1 = a->z80.iff2 = false;
    CHECK(cpc_record_start(a), "recording starts");
    for (int i = 0; i < 300; i++) cpc_run_frame(a);
    CPCRecording *rec = cpc_record_stop(a);
    CPCPlayer *p = cpc_player_create(rec);
    CHECK(cpc_player_begin(a, p, NULL), "replay begins");
    for (int i = 0; i < 300; i++) cpc_run_frame(a);                     /* index every keyframe */
    cpc_player_seek(a, 0);
    struct { uint16_t at; uint16_t sp; uint16_t expect; const char *what; } t[] = {
        {0x4001, 0, 0x4000, "plain: after DI"},
        {0x4017, 0, 0x4016, "plain: after PUSH AF"},
        {0x4018, 0, 0x4017, "plain: after POP BC"},
        {0x4008, 0x8000, 0x4005, "after RET: back to the CALL"},
        {0x4105, 0x7FFC, 0x4102, "after a nested RET that ran for more than a keyframe interval: back to its CALL"},
        {0x4014, 0, 0x4012, "after LDIR: back to the first iteration"},
    };
    for (size_t k = 0; k < sizeof t / sizeof *t; k++) {
        cpc_player_seek(a, 0);
        uint64_t guard = 4000000; bool seen = false;
        while (guard--) {
            cpc_step(a);
            if (a->z80.pc == t[k].at && (!t[k].sp || a->z80.sp.w == t[k].sp)) { seen = true; break; }
        }
        uint64_t here = a->cycles;
        CPCReverseScan *sc = cpc_reverse_begin(a, CPC_REV_STEPOVER, 0, 0); int slices = 0;
        while (sc && !cpc_reverse_slice(a, sc)) slices++;
        CPCReverseHit h; bool found = sc && cpc_reverse_end(a, sc, true, &h);
        char msg[200]; snprintf(msg, sizeof msg, "%s (%04X -> %04X, %d slices)", t[k].what, t[k].at, a->z80.pc, slices);
        CHECK(seen && found && a->z80.pc == t[k].expect && a->cycles < here, msg);
        if (t[k].expect == 0x4012) CHECK(a->z80.bc.w == 0x0400 && a->z80.hl.w == 0x6000, "the block instruction is back at its first iteration");
    }
    /* nothing before the first instruction */
    cpc_player_seek(a, 0);
    { CPCReverseScan *sc = cpc_reverse_begin(a, CPC_REV_STEPOVER, 0, 0); while (sc && !cpc_reverse_slice(a, sc)) {}
      CHECK(sc && !cpc_reverse_end(a, sc, true, NULL) && a->z80.pc == 0x4000, "at the start there is nothing to go back to"); }
    cpc_player_free(p); cpc_recording_free(rec); cpc_destroy(a);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}

static int run_amxtest(const char *roms) {
    int fails = 0;
    CPC *c = cpc_create(CPC_MODEL_6128); load_roms(c, roms, CPC_MODEL_6128); cpc_reset(c, true);
    cpc_run_frame(c); CHECK(c->amx_row == 0xFF, "idle when disabled");
    c->amx_enabled = true;
    cpc_amx_mouse(c, 5, -3, 1); cpc_run_frame(c);
    CHECK(c->amx_row == (uint8_t)~(0x08 | 0x01 | 0x10), "right+up+left button");
    cpc_amx_mouse(c, 0, 0, 1); cpc_run_frame(c);
    CHECK(c->amx_row == (uint8_t)~0x10, "no movement keeps only the button");
    cpc_amx_mouse(c, -1, 2, 2); cpc_run_frame(c);
    CHECK(c->amx_row == (uint8_t)~(0x04 | 0x02 | 0x20), "left+down+right button");
    cpc_destroy(c);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}

static int run_boot(int argc, char **argv) {
    const char *wav = NULL; int space_at = -1, return_at = -1;
    CPCModel m = CPC_MODEL_6128; const char *roms = "roms", *out = "frame.bmp"; int frames = 150, wait = 100; const char *disc = NULL, *typed = NULL, *cart = NULL; int fkey = -1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) { const char *v = argv[++i]; m = !strcmp(v, "464") ? CPC_MODEL_464 : !strcmp(v, "664") ? CPC_MODEL_664 : CPC_MODEL_6128; }
        else if (!strcmp(argv[i], "--roms") && i + 1 < argc) roms = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--fkey") && i + 1 < argc) fkey = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cart") && i + 1 < argc) cart = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--type") && i + 1 < argc) typed = argv[++i];
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--space") && i + 1 < argc) space_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--return") && i + 1 < argc) return_at = atoi(argv[++i]);
    }
    FILE *wf = wav ? fopen(wav, "wb") : NULL; uint32_t wav_frames = 0;
    if (wf) fseek(wf, 44, SEEK_SET);
    CPC *c = cpc_create(cart ? CPC_MODEL_6128PLUS : m);
    if (cart) { CPCConfig cfg; cpc_default_config(&cfg, CPC_MODEL_6128PLUS); cfg.ram_kb = 128; cpc_configure(c, &cfg); size_t n; uint8_t *b = slurp(cart, &n); if (!b || !cpc_load_cartridge(c, b, n)) { fprintf(stderr, "bad cartridge\n"); return 1; } free(b); }
    else load_roms(c, roms, m);
    cpc_reset(c, true);
    if (disc) { CPCDisc *d = dsk_load_file(disc); if (!d) fprintf(stderr, "cannot load %s\n", disc); else { fdc_insert(c->fdc, 0, d); } }
    for (int i = 0; i < frames; i++) {
        if (typed && i == wait) { char *t = strdup(typed); for (char *p = t; *p; p++) if (*p == '~') *p = '\n'; cpc_autotype(c, t); free(t); }
        if (getenv("IOLOG") && i == (getenv("IOFROM") ? atoi(getenv("IOFROM")) : wait - 1)) { extern int cpc_trace_io; cpc_trace_io = 1; }
        if (i == space_at) cpc_key(c, CPCK_SPACE, true);
        if (i == space_at + 5) cpc_key(c, CPCK_SPACE, false);
        if (i == return_at) cpc_key(c, CPCK_RETURN, true);
        if (i == return_at + 5) cpc_key(c, CPCK_RETURN, false);
        if (fkey >= 0) { static const int fk[10] = {CPCK_F0, CPCK_F1, CPCK_F2, CPCK_F3, CPCK_F4, CPCK_F5, CPCK_F6, CPCK_F7, CPCK_F8, CPCK_F9};
            if (i == wait) cpc_key(c, fk[fkey], true); if (i == wait + 8) cpc_key(c, fk[fkey], false); }
        if (getenv("TRACEAT") && i >= wait) {
            unsigned at = strtoul(getenv("TRACEAT"), 0, 16); int n = getenv("TRACEN") ? atoi(getenv("TRACEN")) : 200; uint64_t start = c->cycles; bool on = false;
            while (c->cycles - start < 19968 * 60) {
                if (!on && c->z80.pc == at) on = true;
                if (on) { char t[64]; z80_disassemble(c->z80.pc, (uint8_t (*)(void *, uint16_t))cpc_mem_read, c, t, sizeof t); printf("%04X  %-24s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X U=%d/%02X L=%d unl=%d\n", c->z80.pc, t, c->z80.af.w, c->z80.bc.w, c->z80.de.w, c->z80.hl.w, c->z80.sp.w, c->upper_rom_en, c->upper_rom_sel, c->lower_rom_en, c->asic.unlocked); if (--n <= 0) return 0; }
                cpc_step(c);
            }
            printf("never reached\n"); return 0;
        }
        if (getenv("TRACE0") && i >= wait) {
            static uint16_t ring[16384]; int rp = 0; uint64_t start = c->cycles;
            while (c->cycles - start < 19968 * 40) {
                ring[rp++ & 16383] = c->z80.pc;
                cpc_step(c);
                if (c->z80.pc == 0 && c->lower_rom_en) {
                    for (int k = 0; k < 16384; k++) { uint16_t pc = ring[(rp + k) & 16383]; char t[64]; z80_disassemble(pc, (uint8_t (*)(void *, uint16_t))cpc_mem_read, c, t, sizeof t); printf("%04X  %s\n", pc, t); }
                    printf("RESET at cycle %llu\n", (unsigned long long)(c->cycles - start)); return 0;
                }
            }
            printf("no reset\n"); return 0;
        }
        cpc_run_frame(c);
        if (wf) { float buf[4096 * 2]; int n; while ((n = cpc_audio_read(c, buf, 4096)) > 0) {
            for (int k = 0; k < n * 2; k++) { int v = (int)(buf[k] * 32767); int16_t s16 = v > 32767 ? 32767 : v < -32768 ? -32768 : v; fwrite(&s16, 2, 1, wf); }
            wav_frames += n; } }
    }
    if (wf) {
        uint32_t rate = c->cfg.sample_rate, bytes = wav_frames * 4, v; uint16_t h;
        fseek(wf, 0, SEEK_SET); fwrite("RIFF", 1, 4, wf); v = 36 + bytes; fwrite(&v, 4, 1, wf); fwrite("WAVEfmt ", 1, 8, wf);
        v = 16; fwrite(&v, 4, 1, wf); h = 1; fwrite(&h, 2, 1, wf); h = 2; fwrite(&h, 2, 1, wf); fwrite(&rate, 4, 1, wf);
        v = rate * 4; fwrite(&v, 4, 1, wf); h = 4; fwrite(&h, 2, 1, wf); h = 16; fwrite(&h, 2, 1, wf); fwrite("data", 1, 4, wf); fwrite(&bytes, 4, 1, wf);
        fclose(wf);
    }
    if (getenv("DUMP")) {
        printf("mode=%d ram=%02X lower=%d upper=%d sel=%02X unl=%d vis=%d rmr2=%02X\n", c->ga_mode, c->ga_ram, c->lower_rom_en, c->upper_rom_en, c->upper_rom_sel, c->asic.unlocked, c->asic.page_visible, c->asic.rmr2);
        printf("psg:"); for (int k = 0; k < 16; k++) printf(" %02X", c->psg.regs[k]); printf("  sel=%02X\n", c->psg.sel);
        printf("crtc:"); for (int k = 0; k < 18; k++) printf(" %02X", c->crtc.r[k]); printf("\n");
        printf("beam_y=%d beam_x=%d frame_line=%d mode=%d\n", c->beam_y, c->beam_x, c->frame_line, c->ga_mode);
        printf("pal12:"); for (int k = 0; k < 17; k++) printf(" %03X", c->asic.pal12[k]); printf("\n");
        printf("ga_pal:"); for (int k = 0; k < 17; k++) printf(" %02X", c->ga_palette[k]); printf("\n");
        printf("pri=%d splt=%d ssa=%04X sscr=%02X ivr=%02X dcsr=%02X\n", c->asic.pri, c->asic.splt, c->asic.ssa, c->asic.sscr, c->asic.ivr, c->asic.dcsr);
        printf("asic ram 6400:"); for (int k = 0; k < 40; k++) printf(" %02X", c->asic.ram[0x2400 + k]); printf("\n");
    }
    if (getenv("MIDFRAME")) { uint64_t s0 = c->cycles; while (c->cycles - s0 < (uint64_t)atoi(getenv("MIDFRAME"))) cpc_step(c); }
    if (getenv("VIEW")) {
        static uint32_t v[CPC_SCREEN_W * CPC_SCREEN_H];
        if (!strcmp(getenv("VIEW"), "raster")) cpc_render_raster(c, v); else cpc_render_vram(c, v);
        write_bmp(out, v, CPC_SCREEN_W, CPC_SCREEN_H);
    } else
    write_bmp(out, cpc_frame(c), CPC_SCREEN_W, CPC_SCREEN_H);
    printf("frames=%llu pc=%04x cycles=%llu\n", (unsigned long long)c->frames, c->z80.pc, (unsigned long long)c->cycles);
    cpc_destroy(c); return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "newdisc")) { CPCDisc *d = dsk_new(); dsk_format(d, DSK_FORMAT_DATA, 40, 1); d->extended = false; bool ok = dsk_save_file(d, argv[2]); dsk_free(d); return ok ? 0 : 1; }
    if (argc >= 4 && !strcmp(argv[1], "tapetest")) return run_tapetest(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "snaptest")) return run_snaptest(argv[2], argv[3]);
    if (argc >= 6 && !strcmp(argv[1], "dis")) return run_dis(argv[2], strtoul(argv[3], 0, 0), strtoul(argv[4], 0, 0), atoi(argv[5]));
    if (argc >= 3 && !strcmp(argv[1], "dbgtest")) return run_dbgtest(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "mftest")) return run_mftest(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "steptest")) return run_steptest(argv[2], argc >= 4 ? argv[3] : NULL);
    if (argc >= 4 && !strcmp(argv[1], "logtest")) return run_logtest(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "sotest")) return run_sotest(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "disctest")) return run_disctest(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "rectest")) return run_rectest(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "statetest")) return run_statetest(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "amxtest")) return run_amxtest(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "symtest")) return run_symtest(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "inttest")) return run_inttest(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "boot")) return run_boot(argc - 2, argv + 2);
    if (argc >= 3 && !strcmp(argv[1], "zex")) return run_zex(argv[2]);
    fprintf(stderr, "usage: cpcrun zex <file.com>\n");
    return 1;
}
