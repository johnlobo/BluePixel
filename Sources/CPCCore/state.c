#include "state.h"
#include "fdc.h"
#include "tape.h"
#include "dsk.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define STATE_MAGIC 0x53435043u        /* "CPCS" */
#define STATE_VERSION 3u

struct CPCState {
    uint32_t magic, version, cpc_size, raw_size, zsize;
    uint64_t frames, cycles;
    uint8_t z[];
};

/* Fields of CPC that are NOT machine state: live pointers, host-side settings and debugger/recorder hooks.
 * They are zeroed in the saved image and kept from the live machine on restore. One list drives both. */
#define LIVE_FIELDS(X) \
    X(z80.ctx) X(z80.rd) X(z80.fetch) X(z80.wr) X(z80.in) X(z80.out) X(z80.brk_enabled) X(z80.brk_hit) X(z80.turbo) \
    X(cfg) X(model) X(hw_rgb) \
    X(ram) X(ram_size) X(rom_lower) X(rom_upper) X(rom_multiface) X(cartridge) X(cartridge_banks) X(rd_map) X(wr_map) \
    X(frame) X(frame_done) X(frame_ready) X(audio) X(audio_cap) X(audio_w) X(audio_r) \
    X(fdc) X(tape) X(rec_active) X(rec_buf) X(rec_len) X(rec_cap) X(rec_phase) X(t_counter) X(dbg) X(recorder) X(stopped_recording) X(player) X(trace_io) X(trace_io_ctx) \
    X(asic.pal_rgb) X(asic.spal_rgb)

typedef struct {                       /* hardware configuration the state depends on */
    uint8_t type, plus, plus_ppi, silicon_disc, fast_disc, turbo, four_drives, amdrum, digiblaster, multiface,
            joystick_enabled, enable_32_roms, refresh_50hz, amx_enabled, single_sided_read[4], pad[3];
    int32_t ram_kb;
} StateCfg;

static StateCfg cfg_of(const CPC *c) {
    StateCfg s; memset(&s, 0, sizeof s);
    s.type = c->cfg.type; s.plus = c->cfg.plus; s.plus_ppi = c->cfg.plus_ppi; s.ram_kb = c->cfg.ram_kb;
    s.silicon_disc = c->cfg.silicon_disc; s.fast_disc = c->cfg.fast_disc; s.turbo = c->cfg.turbo; s.four_drives = c->cfg.four_drives;
    s.amdrum = c->cfg.amdrum; s.digiblaster = c->cfg.digiblaster; s.multiface = c->cfg.multiface;
    s.joystick_enabled = c->cfg.joystick_enabled; s.enable_32_roms = c->cfg.enable_32_roms; s.refresh_50hz = c->cfg.refresh_50hz;
    s.amx_enabled = c->amx_enabled; for (int i = 0; i < 4; i++) s.single_sided_read[i] = c->single_sided_read[i];
    return s;
}

static size_t ram_used(const CPC *c) {
    size_t n = (size_t)(c->cfg.ram_kb <= 64 ? 64 : c->cfg.ram_kb) * 1024;
    return n > c->ram_size ? c->ram_size : n;
}
#define FRAME_BYTES ((size_t)CPC_SCREEN_W * CPC_SCREEN_H * 4)

/* ---- byte sink: either collects the raw image or just hashes it ---- */
typedef struct { uint8_t *p; size_t n, cap; uint64_t h; bool hash_only; } Sink;

static void put(Sink *s, const void *d, size_t n) {
    const uint8_t *b = d;
    if (s->hash_only) { uint64_t h = s->h; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; } s->h = h; return; }
    if (s->n + n > s->cap) { s->cap = (s->n + n) * 2; s->p = realloc(s->p, s->cap); }
    memcpy(s->p + s->n, b, n); s->n += n;
}
static void put_u32(Sink *s, uint32_t v) { put(s, &v, 4); }

static void scrub(CPC *img) {
#define X(f) memset(&img->f, 0, sizeof img->f);
    LIVE_FIELDS(X)
#undef X
}
static void keep_live(CPC *dst, const CPC *live) {
#define X(f) memcpy(&dst->f, &live->f, sizeof dst->f);
    LIVE_FIELDS(X)
#undef X
}

/* Discs: geometry and every track the emulated FDC has changed since the reference point (CPCDisc.orig), so that
 * a state can be restored after later disc writes, and a hash notices a difference in what was written. */
static void put_discs(Sink *s, CPC *c) {
    for (int i = 0; i < 4; i++) {
        const CPCDisc *d = fdc_disc(c->fdc, i);
        put_u32(s, d ? 1 : 0); if (!d) continue;
        put_u32(s, (uint32_t)d->tracks); put_u32(s, (uint32_t)d->sides);
        uint32_t n = 0; for (int k = 0; k < DSK_MAX_TRACKS * 2; k++) if (*(&d->orig[0][0] + k)) n++;
        put_u32(s, n);
        for (int k = 0; k < DSK_MAX_TRACKS * 2; k++) {
            if (!*(&d->orig[0][0] + k)) continue;
            const CPCTrack *t = &d->trk[0][0] + k;
            size_t sz = dsk_track_pack_size(t); uint8_t *b = malloc(sz);
            dsk_track_pack(t, b); put_u32(s, (uint32_t)k); put_u32(s, (uint32_t)sz); put(s, b, sz); free(b);
        }
    }
}

/* Writes the whole state image. Order is the file format; restore reads it back the same way. */
static bool write_image(CPC *c, Sink *s) {
    StateCfg cfg = cfg_of(c); put(s, &cfg, sizeof cfg);
    CPC *img = malloc(sizeof *img); if (!img) return false;
    memcpy(img, c, sizeof *img); scrub(img); put(s, img, sizeof *img); free(img);
    uint32_t ru = (uint32_t)ram_used(c); put_u32(s, ru); put(s, c->ram, ru);
    put(s, c->frame, FRAME_BYTES); put(s, c->frame_done, FRAME_BYTES);
    if (c->rom_multiface) { put_u32(s, 16384); put(s, c->rom_multiface, 16384); } else put_u32(s, 0);
    size_t n; uint8_t *b = fdc_state_save(c->fdc, &n); put_u32(s, (uint32_t)n); put(s, b, n); free(b);
    if (c->tape) { uint8_t t[256]; size_t tn = tape_state_size(); tape_state_save(c->tape, t); put_u32(s, (uint32_t)tn); put(s, t, tn); } else put_u32(s, 0);
    b = cpc_autotype_state_save(c, &n); put_u32(s, (uint32_t)n); put(s, b, n); free(b);
    put_discs(s, c);
    return true;
}

CPCState *cpc_state_save(CPC *c) {
    Sink s = {0}; if (!write_image(c, &s)) return NULL;
    uLongf zcap = compressBound((uLong)s.n);
    CPCState *st = malloc(sizeof *st + zcap); if (!st) { free(s.p); return NULL; }
    if (compress2(st->z, &zcap, s.p, (uLong)s.n, 1) != Z_OK) { free(s.p); free(st); return NULL; }
    st->magic = STATE_MAGIC; st->version = STATE_VERSION; st->cpc_size = (uint32_t)sizeof(CPC);
    st->raw_size = (uint32_t)s.n; st->zsize = (uint32_t)zcap; st->frames = c->frames; st->cycles = c->cycles;
    free(s.p);
    return realloc(st, sizeof *st + zcap);
}

uint64_t cpc_state_hash(CPC *c) {
    Sink s = {0}; s.hash_only = true; s.h = 1469598103934665603ULL;
    write_image(c, &s);
    /* Mounted media affects future reads. It is intentionally external to CPCState, but hashes
     * must identify it even before the first disk write. */
    for (int i = 0; i < 4; i++) {
        const CPCDisc *d = fdc_disc(c->fdc, i);
        put_u32(&s, d ? 1u : 0u);
        if (d) put_u32(&s, dsk_content_crc(d));
    }
    /* ROM bytes are external to CPCState, like the base disk image. Include the ROM set so two
     * otherwise equal machines with different ROM contents cannot report the same state hash. */
    for (int i = -1; i < 32; i++) {
        const uint8_t *rom = i < 0 ? c->rom_lower : c->rom_upper[i];
        put_u32(&s, rom ? 1u : 0u);
        if (rom) put(&s, rom, 16384);
    }
    put_u32(&s, c->rom_multiface ? 1u : 0u);
    if (c->rom_multiface) put(&s, c->rom_multiface, 16384);
    put_u32(&s, (uint32_t)c->cartridge_banks);
    if (c->cartridge && c->cartridge_banks > 0) put(&s, c->cartridge, (size_t)c->cartridge_banks * 16384);
    return s.h;
}

void cpc_state_free(CPCState *s) { free(s); }
uint64_t cpc_state_frames(const CPCState *s) { return s->frames; }
uint64_t cpc_state_cycles(const CPCState *s) { return s->cycles; }
size_t cpc_state_size(const CPCState *s) { return sizeof *s + s->zsize; }
const uint8_t *cpc_state_bytes(const CPCState *s, size_t *size) { *size = cpc_state_size(s); return (const uint8_t *)s; }

CPCState *cpc_state_from_bytes(const uint8_t *data, size_t size) {
    if (size < sizeof(CPCState)) return NULL;
    const CPCState *h = (const CPCState *)data;
    if (h->magic != STATE_MAGIC || h->version != STATE_VERSION || h->cpc_size != sizeof(CPC)) return NULL;
    if (sizeof *h + (size_t)h->zsize != size) return NULL;
    CPCState *s = malloc(size); if (!s) return NULL;
    memcpy(s, data, size); return s;
}

/* ---- reader ---- */
typedef struct { const uint8_t *p; size_t n, pos; bool bad; } Src;
static const uint8_t *take(Src *r, size_t n) { if (r->bad || n > r->n - r->pos) { r->bad = true; return NULL; } const uint8_t *p = r->p + r->pos; r->pos += n; return p; }
static uint32_t take_u32(Src *r) { const uint8_t *p = take(r, 4); uint32_t v = 0; if (p) memcpy(&v, p, 4); return v; }

typedef struct { int present; uint32_t tracks, sides; uint32_t n; uint32_t idx[DSK_MAX_TRACKS * 2]; CPCTrack *trk; } DiscImg;

static void free_discs(DiscImg d[4]) { for (int i = 0; i < 4; i++) { for (uint32_t k = 0; k < d[i].n && d[i].trk; k++) dsk_track_clear(&d[i].trk[k]); free(d[i].trk); } }

/* Puts the discs where the state was taken: tracks changed since then go back to their original, the saved ones are set. */
static void apply_discs(CPC *c, DiscImg img[4]) {
    for (int i = 0; i < 4; i++) {
        CPCDisc *d = fdc_disc(c->fdc, i); if (!d) continue;
        bool in[DSK_MAX_TRACKS * 2] = {false};
        for (uint32_t k = 0; k < img[i].n; k++) in[img[i].idx[k]] = true;
        for (int k = 0; k < DSK_MAX_TRACKS * 2; k++) {
            CPCTrack **o = &d->orig[0][0] + k;
            if (*o && !in[k]) { dsk_track_copy(&d->trk[0][0] + k, *o); dsk_track_clear(*o); free(*o); *o = NULL; }
        }
        if (!img[i].present) continue;
        d->tracks = (int)img[i].tracks; d->sides = (int)img[i].sides;
        for (uint32_t k = 0; k < img[i].n; k++) {
            CPCTrack *dst = &d->trk[0][0] + img[i].idx[k];
            dsk_touch(d, dst);
            dsk_track_clear(dst); *dst = img[i].trk[k]; memset(&img[i].trk[k], 0, sizeof img[i].trk[k]);
        }
    }
}


/* Parses the disc section; false when malformed (nothing is left allocated). */
static bool get_discs(Src *r, DiscImg out[4]) {
    memset(out, 0, 4 * sizeof *out);
    for (int i = 0; i < 4; i++) {
        out[i].present = take_u32(r) != 0; if (!out[i].present) continue;
        out[i].tracks = take_u32(r); out[i].sides = take_u32(r); uint32_t n = take_u32(r);
        if (r->bad || n > DSK_MAX_TRACKS * 2) { r->bad = true; break; }
        out[i].trk = calloc(n ? n : 1, sizeof(CPCTrack));
        for (uint32_t k = 0; k < n; k++) {
            uint32_t idx = take_u32(r), sz = take_u32(r); const uint8_t *b = take(r, sz);
            if (r->bad || idx >= DSK_MAX_TRACKS * 2) { r->bad = true; break; }
            if (!dsk_track_unpack(&out[i].trk[k], b, b + sz)) { r->bad = true; break; }
            out[i].idx[k] = idx; out[i].n = k + 1;
        }
        if (r->bad) break;
    }
    if (r->bad) { free_discs(out); memset(out, 0, 4 * sizeof *out); return false; }
    return true;
}

bool cpc_state_restore(CPC *c, const CPCState *st, unsigned *hazards) {
    unsigned hz = 0;
    if (hazards) *hazards = 0;
    if (st->magic != STATE_MAGIC || st->cpc_size != sizeof(CPC)) return false;
    uint8_t *raw = malloc(st->raw_size); if (!raw) return false;
    uLongf rn = st->raw_size;
    if (uncompress(raw, &rn, st->z, st->zsize) != Z_OK || rn != st->raw_size) { free(raw); return false; }
    Src r = {raw, st->raw_size, 0, false};

    const uint8_t *p = take(&r, sizeof(StateCfg)); StateCfg saved; if (p) memcpy(&saved, p, sizeof saved);
    const uint8_t *img = take(&r, sizeof(CPC));
    uint32_t ru = take_u32(&r); const uint8_t *ram = take(&r, ru);
    const uint8_t *f0 = take(&r, FRAME_BYTES), *f1 = take(&r, FRAME_BYTES);
    uint32_t mfn = take_u32(&r); const uint8_t *mf = take(&r, mfn);
    uint32_t fdn = take_u32(&r); const uint8_t *fd = take(&r, fdn);
    uint32_t tn = take_u32(&r); const uint8_t *tp = take(&r, tn);
    uint32_t atn = take_u32(&r); const uint8_t *at = take(&r, atn);
    DiscImg discs[4]; bool discs_ok = get_discs(&r, discs);
    bool ok = !r.bad && discs_ok && ru == ram_used(c);
    if (ok) {
        StateCfg live = cfg_of(c), cmp = saved;
        cmp.amx_enabled = live.amx_enabled; memcpy(cmp.single_sided_read, live.single_sided_read, 4);   /* applied below, not a mismatch */
        if (memcmp(&cmp, &live, sizeof live) != 0) hz |= CPC_STATE_HAZARD_CONFIG;
        for (int i = 0; i < 4; i++) if ((fdc_disc(c->fdc, i) != NULL) != (discs[i].present != 0)) hz |= CPC_STATE_HAZARD_DISC;
        apply_discs(c, discs);
        CPC *keep = malloc(sizeof *keep); memcpy(keep, c, sizeof *keep);
        memcpy(c, img, sizeof *c); keep_live(c, keep); free(keep);
        memcpy(c->ram, ram, ru); memcpy(c->frame, f0, FRAME_BYTES); memcpy(c->frame_done, f1, FRAME_BYTES);
        if (mfn && c->rom_multiface) memcpy(c->rom_multiface, mf, mfn < 16384 ? mfn : 16384);
        if (fdn) ok = fdc_state_restore(c->fdc, fd, fdn) && ok;
        if (tn) { if (!c->tape || tn != tape_state_size() || !tape_state_restore(c->tape, tp)) hz |= CPC_STATE_HAZARD_TAPE; }
        else if (c->tape) hz |= CPC_STATE_HAZARD_TAPE;
        if (atn) ok = cpc_autotype_state_restore(c, at, atn) && ok;
        c->amx_enabled = saved.amx_enabled; for (int i = 0; i < 4; i++) c->single_sided_read[i] = saved.single_sided_read[i];
        asic_refresh_palette(c);
        cpc_update_mapping(c);
    }
    free_discs(discs);
    free(raw);
    if (hazards) *hazards = hz;
    return ok;
}

uint32_t cpc_crc32(const uint8_t *data, size_t size) { return (uint32_t)crc32(0L, data, (uInt)size); }
