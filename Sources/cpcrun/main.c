/* cpcrun: headless test runner. `cpcrun zex <file.com>` runs a CP/M .COM exerciser (ZEXDOC/ZEXALL). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"

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

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "zex")) return run_zex(argv[2]);
    fprintf(stderr, "usage: cpcrun zex <file.com>\n");
    return 1;
}
