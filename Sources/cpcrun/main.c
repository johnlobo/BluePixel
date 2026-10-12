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

static int run_boot(int argc, char **argv) {
    CPCModel m = CPC_MODEL_6128; const char *roms = "roms", *out = "frame.bmp"; int frames = 150, wait = 100; const char *disc = NULL, *typed = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) { const char *v = argv[++i]; m = !strcmp(v, "464") ? CPC_MODEL_464 : !strcmp(v, "664") ? CPC_MODEL_664 : CPC_MODEL_6128; }
        else if (!strcmp(argv[i], "--roms") && i + 1 < argc) roms = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--type") && i + 1 < argc) typed = argv[++i];
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    }
    CPC *c = cpc_create(m); load_roms(c, roms, m); cpc_reset(c, true);
    if (disc) { CPCDisc *d = dsk_load_file(disc); if (!d) fprintf(stderr, "cannot load %s\n", disc); else { fdc_insert(c->fdc, 0, d); } }
    for (int i = 0; i < frames; i++) {
        if (typed && i == wait) { char *t = strdup(typed); for (char *p = t; *p; p++) if (*p == '~') *p = '\n'; cpc_autotype(c, t); free(t); }
        cpc_run_frame(c);
    }
    write_bmp(out, cpc_frame(c), CPC_SCREEN_W, CPC_SCREEN_H);
    printf("frames=%llu pc=%04x cycles=%llu\n", (unsigned long long)c->frames, c->z80.pc, (unsigned long long)c->cycles);
    cpc_destroy(c); return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "newdisc")) { CPCDisc *d = dsk_new(); dsk_format(d, DSK_FORMAT_DATA, 40, 1); d->extended = false; bool ok = dsk_save_file(d, argv[2]); dsk_free(d); return ok ? 0 : 1; }
    if (argc >= 4 && !strcmp(argv[1], "tapetest")) return run_tapetest(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "snaptest")) return run_snaptest(argv[2], argv[3]);
    if (argc >= 2 && !strcmp(argv[1], "boot")) return run_boot(argc - 2, argv + 2);
    if (argc >= 3 && !strcmp(argv[1], "zex")) return run_zex(argv[2]);
    fprintf(stderr, "usage: cpcrun zex <file.com>\n");
    return 1;
}
