/* Coverage probe for the canonical state serializer, driven by check_state_coverage.py.
 *
 * stdin: one line per field to probe, "<object> <offset> <size> <restore> <name>", where object is
 * cpc | fdc | tape | autotype, offset/size come from the debug information and restore is 1 when the field must also
 * survive save -> restore into another machine (0 for configuration, which is compared but not restored).
 * For each field it flips the low bit of each probed byte and checks that the canonical stream changes;
 * then, once per field, that a full save/restore carries the flipped value into a second machine.
 * stdout: "MISS <name> <byte>" / "NORESTORE <name>" per failure, then "probed N fields".
 * No ROMs or media files are needed: the probe only serializes, it never runs the machine. */
#include "cpc.h"
#include "state.h"
#include "tape.h"
#include "cpc_keys.h"
#include "dsk.h"
#include "fdc.h"
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A two-block TZX (pure tone, then a pause): enough for tape_load to return a tape whose position state can be probed. */
static const uint8_t empty_tzx[] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20,
                                    0x12, 0x00, 0x08, 0x10, 0x00,    /* pure tone: 16 pulses of 2048 T */
                                    0x20, 0xE8, 0x03};               /* pause 1000 ms */

static CPC *machine_of(CPCModel m) {
    CPC *c = cpc_create(m);
    CPCConfig cfg; cpc_default_config(&cfg, m); cpc_configure(c, &cfg);
    cpc_reset(c, true);
    c->tape = tape_load(empty_tzx, sizeof empty_tzx, "probe");
    cpc_autotype(c, "probe");                        /* allocates the auto-type queue so it can be probed */
    return c;
}
static CPC *machine(void) { return machine_of(CPC_MODEL_6128); }
/* Everything is freed, so LeakSanitizer reports only leaks in the core (e.g. on a restore path). */
static void free_machine(CPC *c) { for (int i = 0; i < 4; i++) dsk_free(fdc_disc(c->fdc, i)); cpc_destroy(c); }

static uint8_t *base_of(CPC *c, const char *obj) {
    if (!strcmp(obj, "cpc")) return (uint8_t *)c;
    if (!strcmp(obj, "fdc")) return (uint8_t *)c->fdc;
    if (!strcmp(obj, "tape")) return (uint8_t *)c->tape;
    if (!strcmp(obj, "autotype")) return (uint8_t *)c->autotype;
    return NULL;
}

static bool same_stream(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) { return an == bn && !memcmp(a, b, an); }

static long find_tag(const uint8_t *p, size_t n, const char *tag) {
    for (size_t i = 0; i + 4 <= n; i++) if (!memcmp(p + i, tag, 4)) return (long)i;
    return -1;
}

/* State file bytes for an arbitrary raw stream: the header of a real save with the payload replaced. */
static CPCState *state_with(const CPCState *real, const uint8_t *raw, size_t n) {
    size_t hn; const uint8_t *h = cpc_state_bytes(real, &hn);
    uLongf zcap = compressBound((uLong)n);
    uint8_t *f = malloc(32 + zcap); memcpy(f, h, 32);
    compress2(f + 32, &zcap, raw, (uLong)n, 1);
    for (int i = 0; i < 4; i++) { f[24 + i] = (uint8_t)(n >> (8 * i)); f[28 + i] = (uint8_t)(zcap >> (8 * i)); }
    CPCState *st = cpc_state_from_bytes(f, 32 + zcap); free(f);
    return st;
}

/* Untrusted input: restore must reject or accept every corrupted stream without crashing (the coverage script
 * builds this with ASan/UBSan), an accepted one must be safe to run, and a rejected one must leave the target machine untouched. Corruption is
 * aimed at the small sections (header, video registers, PPI onward incl. FDC, tape, auto-type, discs);
 * flips inside RAM or pixels are always valid and teach nothing. */
static int fuzz(CPC *a, CPC *b, int iterations) {
    CPCDisc *d = dsk_new(); dsk_format(d, DSK_FORMAT_DATA, 40, 1);
    dsk_touch(d, &d->trk[0][0]);                      /* a track with an original: the disc section has content */
    fdc_insert(a->fdc, 0, d); fdc_insert(b->fdc, 0, dsk_clone(d));
    cpc_autotype(a, "fuzz\n");
    size_t n; uint8_t *raw = cpc_state_canonical(a, &n);
    CPCState *real = cpc_state_save(a);
    long ga = find_tag(raw, n, "GA  "), ppi = find_tag(raw, n, "PPI ");
    if (ga < 0 || ppi < 0) { printf("ERROR tags not found\n"); return 1; }
    uint32_t rng = 12345; int fails = 0, accepted = 0, rejected = 0;
    uint8_t *m = malloc(n);
    for (int it = 0; it < iterations; it++) {
        memcpy(m, raw, n); size_t mn = n;
        int kind = it % 4;
        for (int f = 0; f < 1 + (it % 3); f++) {
            rng = rng * 1103515245u + 12345u; uint32_t r = rng >> 8;
            size_t pos = r % 3 == 0 ? r % 64 : r % 3 == 1 ? (size_t)ga + r % 320 : (size_t)ppi + r % (n - (size_t)ppi);
            if (kind == 0) m[pos] ^= (uint8_t)(1u << (r % 8));          /* bit flip */
            else if (kind == 1) m[pos] = (uint8_t)(r >> 3);              /* random byte */
            else if (kind == 2) m[pos] = 0xFF;                           /* large values for counts and lengths */
            else mn = pos;                                               /* truncation */
        }
        size_t bn; uint8_t *before = cpc_state_canonical(b, &bn);
        CPCState *st = state_with(real, m, mn);
        bool ok = st && cpc_state_restore(b, st, NULL);
        size_t an; uint8_t *after = cpc_state_canonical(b, &an);
        if (ok) { accepted++; cpc_run_frame(b); cpc_run_frame(b); }   /* accepted values must also be safe to run */
        else { rejected++; if (!same_stream(before, bn, after, an)) { printf("FUZZ rejected stream changed the machine (iteration %d)\n", it); fails++; } }
        free(before); free(after); cpc_state_free(st);
    }
    free(m); free(raw); cpc_state_free(real);
    printf("fuzzed %d corrupted streams: %d accepted, %d rejected, %d failure(s)\n", iterations, accepted, rejected, fails);
    return fails;
}

/* Hostile values, field by field. A client can send any stream that passes the format checks, so the values
 * themselves are the attack: 1 to 4 random state fields of machine A get extreme or index-like values, A is
 * saved and restored into B, and B is then driven hard (FDC byte transfers and ticks, gate array pen/colour,
 * CRTC select/data incl. the Plus unlock sequence, PPI/PSG, tape playback, whole frames). Under ASan/UBSan any
 * value that is accepted but unsafe crashes here; it must instead be rejected by restore. */
typedef struct { int obj; unsigned long off, size; bool is_bool; char name[96]; } Leaf;
static Leaf leaves[1024]; static int nleaves;

static void exercise(CPC *c) {
    /* First touch every device with the restored values as they are, before any OUT below resets an index
     * (selecting a pen or CRTC register would overwrite a hostile ga_pen or crtc.sel before it is used). */
    cpc_io_write(c, 0x7F00, 0x40 | 0x14);            /* colour write to the restored pen */
    cpc_io_write(c, 0xBD00, 0x55);                    /* CRTC data write to the restored register */
    (void)cpc_io_read(c, 0xBF00); (void)cpc_io_read(c, 0xF4FF);
    fdc_write_data(c->fdc, 0x46); (void)fdc_read_data(c->fdc); fdc_tick(c->fdc, 50000);
    for (int i = 0; i < 48; i++) {
        uint8_t v = (uint8_t)(i * 37 + 11);
        fdc_write_data(c->fdc, v); (void)fdc_read_data(c->fdc); fdc_tick(c->fdc, 2500);
        cpc_io_write(c, 0x7F00, (uint8_t)(i & 0x1F)); cpc_io_write(c, 0x7F00, (uint8_t)(0x40 | (i & 0x1F)));
        cpc_io_write(c, 0xBC00, (uint8_t)i); cpc_io_write(c, 0xBD00, v);
        cpc_io_write(c, 0xBC00, (uint8_t)(0xFF - i));
        cpc_io_write(c, 0xF400, v); cpc_io_write(c, 0xF600, (uint8_t)(0xC0 | (i & 15))); cpc_io_write(c, 0xF600, (uint8_t)(0x40 | (i & 15)));
        (void)cpc_io_read(c, 0xF4FF); (void)cpc_io_read(c, 0xF5FF); (void)cpc_io_read(c, 0xFB7F); (void)cpc_io_read(c, 0xBF00);
        cpc_io_write(c, 0xFB7F, v); cpc_io_write(c, 0xFA7E, (uint8_t)(i & 1));
    }
    static const uint8_t unlock[17] = {0xFF, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62, 0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD, 0xEE};
    for (int i = 0; i < 17; i++) cpc_io_write(c, 0xBC00, unlock[i]);
    if (c->tape) { tape_play(c->tape, true); tape_set_motor(c->tape, true); tape_tick(c->tape, 400000); }
    cpc_run_frame(c); cpc_run_frame(c);
}

static void put_le(uint8_t *p, unsigned long size, uint64_t v) { for (unsigned long i = 0; i < size && i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static const char *const objs[] = {"cpc", "fdc", "tape", "autotype"};
static uint8_t *leaf_ptr(CPC *c, const Leaf *l) { return base_of(c, objs[l->obj]) + l->off; }

static void fdc_cmd(CPC *c, const uint8_t *b, int n) { for (int i = 0; i < n; i++) fdc_write_data(c->fdc, b[i]); }

static int hostile(CPCModel model, int iterations) {
    static const uint64_t pool[] = {0, 1, 2, 3, 4, 5, 8, 9, 15, 16, 17, 31, 32, 33, 0x45, 0x46, 0x4D, 0x7F, 0x80, 0x8A, 0x8D, 0xFF,
                                    0x100, 0x7FFF, 0x8000, 0xFFFF, 100000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu,
                                    0xFFFFFFFFFFFFFFFFull, 0x7FF0000000000000ull /* +inf as double */, 0x7FF8000000000000ull /* NaN */};
    /* Three starting points for machine A, so hostile values also land inside the FDC's transfer paths (they
     * need real track/sector pointers, which no value in a stream can invent): idle, a FORMAT TRACK waiting
     * for its sector ids, and a READ DATA waiting for its sector. */
    CPCDisc *d = dsk_new(); dsk_format(d, DSK_FORMAT_DATA, 40, 1);
    CPC *b = machine_of(model); fdc_insert(b->fdc, 0, dsk_clone(d));
    CPC *as[3];
    for (int i = 0; i < 3; i++) { as[i] = machine_of(model); fdc_insert(as[i]->fdc, 0, dsk_clone(d)); }
    static const uint8_t format[] = {0x4D, 0x00, 0x02, 0x09, 0x2A, 0xE5};
    static const uint8_t read[] = {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF};
    for (int i = 1; i < 3; i++) fdc_set_motor(as[i]->fdc, true);   /* both commands fail with the motor off */
    fdc_cmd(as[1], format, sizeof format); fdc_cmd(as[2], read, sizeof read);
    /* The starting points must really be mid-command, or the transfer paths go untested. */
    bool mid = fdc_busy(as[1]->fdc) && fdc_busy(as[2]->fdc);
    dsk_free(d);
    CPCState *base = cpc_state_save(b);
    uint32_t rng = 777u + (uint32_t)model; int accepted = 0, rejected = 0, base_ok = 0;
    for (int i = 0; i < 3; i++) { CPCState *t = cpc_state_save(as[i]); base_ok += t && cpc_state_restore(b, t, NULL); cpc_state_free(t); }
    cpc_state_restore(b, base, NULL);
    for (int it = 0; it < iterations; it++) {
        CPC *a = as[it % 3];
        int k = 1 + it % 4; Leaf *pick[4]; uint8_t *saved[4];
        for (int j = 0; j < k; j++) {
            rng = rng * 1103515245u + 12345u; Leaf *l = pick[j] = &leaves[(rng >> 8) % (unsigned)nleaves];
            uint8_t *p = leaf_ptr(a, l);
            saved[j] = malloc(l->size); memcpy(saved[j], p, l->size);
            rng = rng * 1103515245u + 12345u; uint64_t v = pool[(rng >> 8) % (sizeof pool / sizeof pool[0])];
            if (l->is_bool) {                        /* bool or bool array: one element, 0 or 1 */
                rng = rng * 1103515245u + 12345u; p[(rng >> 8) % l->size] = (uint8_t)(v & 1);
            } else {
                unsigned long el = l->size <= 8 ? l->size : (l->size % 4 ? 1 : 4);   /* arrays: one element */
                if (l->size > 8) { rng = rng * 1103515245u + 12345u; p += ((rng >> 8) % (l->size / el)) * el; }
                put_le(p, el, v);
            }
        }
        CPCState *st = cpc_state_save(a);
        bool ok = st && cpc_state_restore(b, st, NULL);
        if (ok) { accepted++; exercise(b); } else rejected++;
        cpc_state_free(st);
        cpc_state_restore(b, base, NULL);
        for (int j = k - 1; j >= 0; j--) { memcpy(leaf_ptr(a, pick[j]), saved[j], pick[j]->size); free(saved[j]); }   /* reverse: picks may repeat */
    }
    cpc_state_free(base);
    free_machine(b); for (int i = 0; i < 3; i++) free_machine(as[i]);
    printf("hostile values (%s): %d states from 3 starting points (%d of 3 restore cleanly, FDC mid-command: %s), %d accepted and exercised, %d rejected\n",
           model == CPC_MODEL_6128PLUS ? "6128 Plus" : "6128", iterations, base_ok, mid ? "yes" : "NO", accepted, rejected);
    return base_ok == 3 && mid ? 0 : 1;   /* any unsafe accepted value aborts under the sanitizers */
}

int main(void) {
    CPC *a = machine(), *b = machine();
    if (!a->tape) { printf("ERROR tape_load failed\n"); return 2; }
    size_t n0; uint8_t *s0 = cpc_state_canonical(a, &n0);
    if (!s0) { printf("ERROR canonical failed\n"); return 2; }
    char obj[16], name[256]; unsigned long off, size; int restore; int fields = 0, fails = 0, rejected = 0;
    while (scanf("%15s %lu %lu %d %255s", obj, &off, &size, &restore, name) == 5) {
        uint8_t *base = base_of(a, obj);
        if (!base) { printf("ERROR unknown object %s\n", obj); return 2; }
        fields++;
        if (restore && nleaves < (int)(sizeof leaves / sizeof leaves[0])) {
            Leaf *l = &leaves[nleaves++]; l->obj = !strcmp(obj, "cpc") ? 0 : !strcmp(obj, "fdc") ? 1 : !strcmp(obj, "tape") ? 2 : 3;
            l->off = off; l->size = size; l->is_bool = strstr(name, ":bool") != NULL; snprintf(l->name, sizeof l->name, "%s", name);
        }
        /* Large byte arrays (RAM-like) are sampled at both ends and the middle; everything else byte by byte. */
        unsigned long picks[3] = {0, size / 2, size - 1}, np = size > 64 ? 3 : size;
        for (unsigned long k = 0; k < np; k++) {
            unsigned long j = size > 64 ? picks[k] : k;
            base[off + j] ^= 1;                      /* low bit: a bool stays 0/1 */
            size_t n1; uint8_t *s1 = cpc_state_canonical(a, &n1);
            bool missed = !s1 || same_stream(s0, n0, s1, n1);
            free(s1);
            if (missed) { printf("MISS %s %s+%lu byte %lu\n", name, obj, off, j); fails++; }
            /* Restore check on the first byte: the flipped value must reach the other machine. A reader that
             * rejects the value (it fails validation, e.g. an index past its buffer) has also seen it; then the
             * other machine must be left exactly as it was, since restore validates before it changes anything. */
            if (!missed && k == 0 && restore) {
                size_t na, nbefore; uint8_t *sa = cpc_state_canonical(a, &na), *before = cpc_state_canonical(b, &nbefore);
                CPCState *st = cpc_state_save(a);
                bool ok = st && cpc_state_restore(b, st, NULL);
                size_t nb; uint8_t *sb = cpc_state_canonical(b, &nb);
                bool good = ok ? same_stream(sa, na, sb, nb) : same_stream(before, nbefore, sb, nb);
                if (!good) { printf("NORESTORE %s (restore ok=%d)\n", name, ok); fails++; }
                else if (!ok) rejected++;
                free(sa); free(sb); free(before); cpc_state_free(st);
            }
            base[off + j] ^= 1;
        }
    }
    size_t n2; uint8_t *s2 = cpc_state_canonical(a, &n2);
    if (!s2 || !same_stream(s0, n0, s2, n2)) { printf("ERROR probe did not leave the machine as it found it\n"); fails++; }
    free(s0); free(s2);
    printf("probed %d fields, %d rejected as invalid on restore (machine untouched), %d failure(s)\n", fields, rejected, fails);
    fails += fuzz(a, b, 600);
    int hn = getenv("HOSTILE_ITERATIONS") ? atoi(getenv("HOSTILE_ITERATIONS")) : 3000;   /* longer one-off runs */
    fails += hostile(CPC_MODEL_6128, hn);
    fails += hostile(CPC_MODEL_6128PLUS, hn);
    free_machine(a); free_machine(b);
    return fails ? 1 : 0;
}
