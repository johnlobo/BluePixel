/* Canonical-state ABI probe, driven by test_state_abi.py.
 *
 *   state_dump run  <os> <basic> <amsdos> <dsk> <frames> <out_prefix>
 *       cold boot, RUN"NAVE", run to <frames>; writes <out_prefix>.canon (canonical stream) and
 *       <out_prefix>.state (state file bytes); prints "hash <h>"; then runs <frames> more and prints "hash2 <h>".
 *   state_dump load <os> <basic> <amsdos> <dsk> <statefile> <frames>
 *       restores a state file written by any build into a fresh machine, runs <frames>, prints "hash2 <h>".
 *   state_dump every <os> <basic> <amsdos> <dsk> <frames> -
 *       cold boot, RUN"NAVE"; at every frame (and a varying number of instructions into it, so many saves land
 *       inside FDC transfers) saves, restores into a second machine and compares hashes. Prints counts; a
 *       legitimate state that restore rejects, or that restores differently, is a failure.
 *
 * Built twice by the test, once normally and once with every struct packed (-fpack-struct=1): the layouts
 * differ, so equal streams and cross-loading states show that the format does not depend on the ABI.
 * Uses only core APIs and stdio, no host layer. */
#include "cpc.h"
#include "cpc_keys.h"
#include "dsk.h"
#include "fdc.h"
#include "state.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz); if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "short read %s\n", p); exit(2); }
    fclose(f); *n = (size_t)sz; return b;
}
static void spit(const char *p, const uint8_t *b, size_t n) {
    FILE *f = fopen(p, "wb"); if (!f || fwrite(b, 1, n, f) != n) { fprintf(stderr, "cannot write %s\n", p); exit(2); }
    fclose(f);
}

static CPC *boot(char **rom, const char *dsk) {
    CPC *c = cpc_create(CPC_MODEL_6128);
    CPCConfig cfg; cpc_default_config(&cfg, CPC_MODEL_6128); cpc_configure(c, &cfg);
    size_t n; uint8_t *b;
    b = slurp(rom[0], &n); cpc_load_rom(c, -1, b, n); free(b);
    b = slurp(rom[1], &n); cpc_load_rom(c, 0, b, n); free(b);
    b = slurp(rom[2], &n); cpc_load_rom(c, 7, b, n); free(b);
    b = slurp(dsk, &n); CPCDisc *d = dsk_load(b, n, dsk); free(b);
    if (!d) { fprintf(stderr, "bad dsk\n"); exit(2); }
    fdc_insert(c->fdc, 0, d);
    cpc_reset(c, true);
    return c;
}

int main(int argc, char **argv) {
    if (argc != 8) { fprintf(stderr, "usage: see header\n"); return 2; }
    CPC *c = boot(argv + 2, argv[5]);
    int frames = atoi(strcmp(argv[1], "load") ? argv[6] : argv[7]);
    if (!strcmp(argv[1], "run")) {
        for (int i = 0; i < frames; i++) {
            if (i == 150) cpc_autotype(c, "run\"nave\n");
            if (i >= 400 && i < 430) cpc_key(c, CPCK_RIGHT, true);   /* some input through the matrix */
            if (i == 430) cpc_key(c, CPCK_RIGHT, false);
            cpc_run_frame(c);
        }
        size_t n; uint8_t *canon = cpc_state_canonical(c, &n);
        char p[1024]; snprintf(p, sizeof p, "%s.canon", argv[7]); spit(p, canon, n); free(canon);
        CPCState *st = cpc_state_save(c); const uint8_t *sb = cpc_state_bytes(st, &n);
        snprintf(p, sizeof p, "%s.state", argv[7]); spit(p, sb, n); cpc_state_free(st);
        printf("hash %016" PRIx64 "\nlayout sizeof(CPC)=%zu\n", cpc_state_hash(c), sizeof(CPC));
        for (int i = 0; i < frames; i++) cpc_run_frame(c);
        printf("hash2 %016" PRIx64 "\n", cpc_state_hash(c));
    } else if (!strcmp(argv[1], "every")) {
        CPC *b = boot(argv + 2, argv[5]);
        int saves = 0, busy = 0, rejected = 0, differ = 0;
        for (int i = 0; i < frames; i++) {
            if (i == 150) cpc_autotype(c, "run\"nave\n");
            cpc_run_frame(c);
            for (int k = (i * 97) % 3000; k > 0; k--) cpc_step(c);
            CPCState *st = cpc_state_save(c); saves++;
            if (fdc_busy(c->fdc)) busy++;
            if (!st || !cpc_state_restore(b, st, NULL)) rejected++;
            else if (cpc_state_hash(b) != cpc_state_hash(c)) differ++;
            cpc_state_free(st);
        }
        printf("saves %d\nfdc_busy %d\nrejected %d\ndiffer %d\n", saves, busy, rejected, differ);
    } else {
        size_t n; uint8_t *b = slurp(argv[6], &n);
        CPCState *st = cpc_state_from_bytes(b, n); free(b);
        unsigned hz = 0;
        if (!st || !cpc_state_restore(c, st, &hz)) { printf("restore failed\n"); return 1; }
        cpc_state_free(st);
        for (int i = 0; i < frames; i++) cpc_run_frame(c);
        printf("hazards %u\nhash2 %016" PRIx64 "\n", hz, cpc_state_hash(c));
    }
    return 0;
}
