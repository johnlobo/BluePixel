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
    const char *wav = NULL; int space_at = -1;
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
        printf("pal12:"); for (int k = 0; k < 17; k++) printf(" %03X", c->asic.pal12[k]); printf("\n");
        printf("ga_pal:"); for (int k = 0; k < 17; k++) printf(" %02X", c->ga_palette[k]); printf("\n");
        printf("pri=%d splt=%d ssa=%04X sscr=%02X ivr=%02X dcsr=%02X\n", c->asic.pri, c->asic.splt, c->asic.ssa, c->asic.sscr, c->asic.ivr, c->asic.dcsr);
        printf("asic ram 6400:"); for (int k = 0; k < 40; k++) printf(" %02X", c->asic.ram[0x2400 + k]); printf("\n");
    }
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
    if (argc >= 3 && !strcmp(argv[1], "amxtest")) return run_amxtest(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "symtest")) return run_symtest(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "inttest")) return run_inttest(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "boot")) return run_boot(argc - 2, argv + 2);
    if (argc >= 3 && !strcmp(argv[1], "zex")) return run_zex(argv[2]);
    fprintf(stderr, "usage: cpcrun zex <file.com>\n");
    return 1;
}
