#include "state.h"
#include "fdc.h"
#include "tape.h"
#include "dsk.h"
#include "ser.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- file form ----
 * Header, 32 bytes little endian: "BPST", u32 format, u64 frames, u64 cycles, u32 raw size, u32 zlib size;
 * then the zlib-compressed canonical stream. Nothing in it depends on the build: a state saved by one build
 * loads in any other build of the same format, and equal machines hash equal everywhere.
 * Format 1 is the first canonical one; the earlier struct-image states ("H1 format", version 3) are not read. */
#define STATE_MAGIC "BPST"
#define STATE_FORMAT 1u
#define HEADER_BYTES 32u
#define RAW_MAX (64u << 20)

struct CPCState {
    uint64_t frames, cycles;
    size_t size;                     /* bytes[] length: header + compressed stream */
    uint8_t bytes[];
};

static size_t ram_used(const CPC *c) {
    size_t n = (size_t)(c->cfg.ram_kb <= 64 ? 64 : c->cfg.ram_kb) * 1024;
    return n > c->ram_size ? c->ram_size : n;
}
#define FRAME_PIXELS ((size_t)CPC_SCREEN_W * CPC_SCREEN_H)

/* ---- canonical stream: one visit function per part of the machine ----
 * The order of calls is the format. Fields of struct CPC that are not visited are not machine state: live
 * pointers (ram, roms, maps, fdc, tape, frame buffers as pointers), the configuration (compared, not
 * restored), host presentation (hw_rgb, asic.pal_rgb/spal_rgb, recomputed after restore), the audio queue,
 * debugger/recorder hooks and frame_ready. Tests/headless/check_state_coverage.py proves, from the debug
 * information, that every other byte of struct CPC reaches the stream. */

typedef struct {                       /* hardware configuration the state depends on */
    int model; uint8_t type; bool plus, plus_ppi, silicon_disc, fast_disc, turbo, four_drives, amdrum, digiblaster, multiface,
            joystick_enabled, enable_32_roms, refresh_50hz;
    int ram_kb;
} StateCfg;

static StateCfg cfg_of(const CPC *c) {
    StateCfg s; memset(&s, 0, sizeof s);
    s.model = (int)c->model; s.type = c->cfg.type; s.plus = c->cfg.plus; s.plus_ppi = c->cfg.plus_ppi; s.ram_kb = c->cfg.ram_kb;
    s.silicon_disc = c->cfg.silicon_disc; s.fast_disc = c->cfg.fast_disc; s.turbo = c->cfg.turbo; s.four_drives = c->cfg.four_drives;
    s.amdrum = c->cfg.amdrum; s.digiblaster = c->cfg.digiblaster; s.multiface = c->cfg.multiface;
    s.joystick_enabled = c->cfg.joystick_enabled; s.enable_32_roms = c->cfg.enable_32_roms; s.refresh_50hz = c->cfg.refresh_50hz;
    return s;
}
static void visit_cfg(Ser *s, StateCfg *g) {
    ser_tag(s, "CFG0");
    ser_int(s, &g->model); ser_u8(s, &g->type); ser_bool(s, &g->plus); ser_bool(s, &g->plus_ppi); ser_bool(s, &g->silicon_disc); ser_bool(s, &g->fast_disc);
    ser_bool(s, &g->turbo); ser_bool(s, &g->four_drives); ser_bool(s, &g->amdrum); ser_bool(s, &g->digiblaster); ser_bool(s, &g->multiface);
    ser_bool(s, &g->joystick_enabled); ser_bool(s, &g->enable_32_roms); ser_bool(s, &g->refresh_50hz); ser_int(s, &g->ram_kb);
}

static void visit_z80(Ser *s, Z80 *z) {
    ser_tag(s, "Z80 ");
    Z80Pair *pairs[] = {&z->af, &z->bc, &z->de, &z->hl, &z->af2, &z->bc2, &z->de2, &z->hl2, &z->ix, &z->iy, &z->sp, &z->wz};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) ser_u16(s, &pairs[i]->w);
    ser_u16(s, &z->pc); ser_u8(s, &z->i); ser_u8(s, &z->r);
    ser_bool(s, &z->iff1); ser_bool(s, &z->iff2); ser_bool(s, &z->halted); ser_u8_in(s, &z->im, 2);
    ser_int(s, &z->ei_delay); ser_bool(s, &z->int_ack); ser_u8(s, &z->int_vector);
    ser_bool(s, &z->nmi_pending); ser_bool(s, &z->retn_hit); ser_u64(s, &z->instructions);
}

static void visit_mem(Ser *s, CPC *c) {
    ser_tag(s, "MEM0");
    uint32_t ru = ser_count(s, (uint32_t)ram_used(c), RAW_MAX);
    if (ser_reading(s) && ru != ram_used(c)) s->bad = true;        /* RAM size is configuration: no partial restore */
    ser_bytes(s, c->ram, s->bad ? 0 : ru);
    ser_bool(s, &c->mf_active); ser_bool(s, &c->mf_running);
    ser_bool(s, &c->lower_rom_en); ser_bool(s, &c->upper_rom_en); ser_u8_in(s, &c->upper_rom_sel, 255); ser_u8(s, &c->ram_config);
    /* Multiface II: its 16K block holds 8K of RAM the program can change. Kept only if the machine has one. */
    bool mf = ser_flag(s, c->rom_multiface != NULL);
    static uint8_t discard[16384];
    ser_bytes(s, mf && c->rom_multiface ? c->rom_multiface : discard, mf ? 16384 : 0);
}

static void visit_asic(Ser *s, CPCAsic *a) {
    ser_tag(s, "ASIC");
    ser_bool(s, &a->unlocked); ser_int_in(s, &a->unlock_pos, 0, 16);   /* indexes the 17-byte unlock sequence */ ser_bool(s, &a->page_visible);
    ser_bytes(s, a->ram, sizeof a->ram);
    ser_u8(s, &a->rmr2); ser_u8_in(s, &a->lower_base, 2); ser_u8_in(s, &a->cart_bank, 31);
    ser_u8(s, &a->pri); ser_u8(s, &a->splt); ser_u8(s, &a->ivr); ser_u8(s, &a->sscr); ser_u8(s, &a->dcsr); ser_u8(s, &a->icsr);
    ser_u16(s, &a->ssa);
    for (int i = 0; i < 3; i++) {
        CPCDma *d = &a->dma[i];
        ser_u16(s, &d->addr); ser_u16(s, &d->loop_addr); ser_u8(s, &d->prescale); ser_u16(s, &d->pause); ser_u16(s, &d->loop_cnt);
        ser_int(s, &d->tick); ser_bool(s, &d->stopped);
    }
    ser_u16s(s, a->pal12, 17);
}

static void visit_video(Ser *s, CPC *c) {
    ser_tag(s, "GA  ");
    ser_u8_in(s, &c->ga_pen, 16);                  /* pens 0..15 and border (16): indexes ga_palette[17] */ ser_u8(s, &c->ga_mode); ser_u8(s, &c->ga_ram);
    for (int i = 0; i < 17; i++) ser_u8_in(s, &c->ga_palette[i], 31);
    ser_int(s, &c->ga_irq_counter); ser_int(s, &c->ga_vsync_delay); ser_bool(s, &c->irq_line); ser_u8(s, &c->ga_hsync_cnt);

    ser_tag(s, "CRTC");
    CPCCrtc *r = &c->crtc;
    ser_bytes(s, r->r, sizeof r->r); ser_u8_in(s, &r->sel, 31);
    ser_u16(s, &r->hcc); ser_u16(s, &r->vcc); ser_u16(s, &r->vlc); ser_u16(s, &r->vtac); ser_u16(s, &r->hsc); ser_u16(s, &r->vsc);
    ser_u16(s, &r->ma); ser_u16(s, &r->ma_row); ser_u16(s, &r->ma_start);
    ser_bool(s, &r->in_adjust); ser_bool(s, &r->vsync); ser_bool(s, &r->hsync); ser_bool(s, &r->hde); ser_bool(s, &r->vde); ser_bool(s, &r->last_row);
    ser_u8_in(s, &r->type, 4);

    ser_tag(s, "BEAM");
    ser_int_in(s, &c->beam_x, -1024, 4096); ser_int_in(s, &c->beam_y, -1024, 4096);   /* used to index the frame buffer */
    ser_int(s, &c->frame_line); ser_int(s, &c->disp_x0);
    ser_u32s(s, c->scroll_carry, 16); ser_bool(s, &c->scroll_valid);
    ser_int(s, &c->beam_line); ser_bool(s, &c->beam_vsync); ser_int(s, &c->vsync_lines);
    ser_u64(s, &c->frames);
    /* Both frame buffers: the picture being drawn and the last completed one (captures read it). */
    ser_u32s(s, c->frame, FRAME_PIXELS); ser_u32s(s, c->frame_done, FRAME_PIXELS);
}

static void visit_io(Ser *s, CPC *c) {
    ser_tag(s, "PPI ");
    ser_bools(s, c->single_sided_read, 4);
    ser_u8(s, &c->ppi_a); ser_u8(s, &c->ppi_b); ser_u8(s, &c->ppi_c); ser_u8(s, &c->ppi_ctl);
    ser_bytes(s, c->kbd, sizeof c->kbd);
    ser_bool(s, &c->amx_enabled); ser_int(s, &c->amx_dx); ser_int(s, &c->amx_dy); ser_u8(s, &c->amx_buttons); ser_u8(s, &c->amx_row);
    ser_bool(s, &c->tape_motor); ser_bool(s, &c->tape_write_bit);

    ser_tag(s, "PSG ");
    CPCPsg *p = &c->psg;
    ser_bytes(s, p->regs, sizeof p->regs); ser_u8_in(s, &p->sel, 255);
    ser_u16s(s, p->tone_cnt, 3); ser_u16s(s, p->tone_per, 3); ser_bytes(s, p->tone_out, sizeof p->tone_out);
    ser_u16(s, &p->noise_cnt); ser_u32(s, &p->lfsr); ser_u8(s, &p->noise_out);
    ser_u16(s, &p->env_cnt); ser_i8_in(s, &p->env_step, 0, 15); ser_u8_in(s, &p->env_vol, 15);   /* env_vol indexes vol_table[16] */ ser_bool(s, &p->env_hold); ser_bool(s, &p->env_up);
    ser_u8(s, &p->sub); ser_f32s(s, p->level, 3);
    p->lvl_clean = false; p->mix_valid = false;      /* derived caches: recompute from the state just read */
    ser_u8(s, &c->psg_ctl); ser_u8(s, &c->amdrum);
    /* Sample generation phase: decides where the next output sample falls, so it is state, not presentation. */
    ser_f32s(s, c->audio_acc, 2); ser_int(s, &c->audio_acc_n); ser_int(s, &c->audio_phase);

    ser_tag(s, "MISC");
    ser_bool(s, &c->motor); ser_u8(s, &c->tape_in); ser_u64(s, &c->cycles);
}

/* Discs: geometry and every track the emulated FDC has changed since the reference point (CPCDisc.orig), so a
 * state can be restored after later disc writes and a hash notices a difference in what was written.
 * Writing/hashing goes straight from the discs; reading parses into DiscImg and is applied only once the whole
 * stream has been validated (restoring changes the discs, which must not happen for a bad stream). */
typedef struct { bool present; uint32_t tracks, sides; uint32_t n; uint32_t idx[DSK_MAX_TRACKS * 2]; CPCTrack *trk; } DiscImg;

static void free_discs(DiscImg d[4]) {
    for (int i = 0; i < 4; i++) { for (uint32_t k = 0; k < d[i].n && d[i].trk; k++) dsk_track_clear(&d[i].trk[k]); free(d[i].trk); }
    memset(d, 0, 4 * sizeof *d);
}

static void visit_discs(Ser *s, CPC *c, DiscImg out[4]) {
    ser_tag(s, "DSK0");
    if (!ser_reading(s)) {
        for (int i = 0; i < 4; i++) {
            const CPCDisc *d = fdc_disc(c->fdc, i);
            ser_flag(s, d != NULL); if (!d) continue;
            ser_count(s, (uint32_t)d->tracks, 0xFFFFFFFFu); ser_count(s, (uint32_t)d->sides, 0xFFFFFFFFu);
            uint32_t n = 0; for (int k = 0; k < DSK_MAX_TRACKS * 2; k++) if (*(&d->orig[0][0] + k)) n++;
            ser_count(s, n, n);
            for (int k = 0; k < DSK_MAX_TRACKS * 2; k++) {
                if (!*(&d->orig[0][0] + k)) continue;
                const CPCTrack *t = &d->trk[0][0] + k;
                size_t sz = dsk_track_pack_size(t); uint8_t *b = malloc(sz ? sz : 1);
                if (!b) { s->bad = true; return; }
                dsk_track_pack(t, b);
                ser_count(s, (uint32_t)k, (uint32_t)k); ser_count(s, (uint32_t)sz, (uint32_t)sz); ser_bytes(s, b, sz); free(b);
            }
        }
        return;
    }
    memset(out, 0, 4 * sizeof *out);
    for (int i = 0; i < 4 && !s->bad; i++) {
        out[i].present = ser_flag(s, false); if (!out[i].present) continue;
        out[i].tracks = ser_count(s, 0, DSK_MAX_TRACKS); out[i].sides = ser_count(s, 0, 2);
        uint32_t n = ser_count(s, 0, DSK_MAX_TRACKS * 2);
        out[i].trk = calloc(n ? n : 1, sizeof(CPCTrack)); if (!out[i].trk) { s->bad = true; break; }
        for (uint32_t k = 0; k < n && !s->bad; k++) {
            uint32_t idx = ser_count(s, 0, DSK_MAX_TRACKS * 2 - 1), sz = ser_count(s, 0, RAW_MAX);
            if (s->bad || sz > s->in_len - s->pos) { s->bad = true; break; }
            const uint8_t *b = s->in + s->pos;
            if (dsk_track_unpack(&out[i].trk[k], b, b + sz) != b + sz) { s->bad = true; break; }
            s->pos += sz; out[i].idx[k] = idx; out[i].n = k + 1;
        }
    }
    if (s->bad) free_discs(out);
}

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

/* The whole machine, in format order. `cfg` is visited as a value: written from the machine, read back for
 * comparison only. Returns the tape match (false: the state's tape is not the inserted one). */
static bool visit_machine(Ser *s, CPC *c, StateCfg *cfg, DiscImg discs[4], bool *tape_in_state) {
    visit_cfg(s, cfg);
    visit_z80(s, &c->z80);
    visit_mem(s, c);
    visit_asic(s, &c->asic);
    visit_video(s, c);
    visit_io(s, c);
    fdc_state_visit(s, c->fdc);
    bool has_tape = ser_flag(s, c->tape != NULL), tape_ok = true;
    if (has_tape) tape_ok = tape_state_visit(s, c->tape);
    if (tape_in_state) *tape_in_state = has_tape;
    cpc_autotype_state_visit(s, c);
    visit_discs(s, c, discs);
    ser_tag(s, "END ");
    return tape_ok;
}

/* Canonical stream of the live machine (WRITE or HASH). */
static void emit(Ser *s, CPC *c) { StateCfg g = cfg_of(c); visit_machine(s, c, &g, NULL, NULL); }

static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t rd32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }

uint8_t *cpc_state_canonical(CPC *c, size_t *size) {
    cpc_settle(c);
    Ser s = {.mode = SER_WRITE};
    emit(&s, c);
    if (s.bad) { free(s.buf); return NULL; }
    *size = s.len; return s.buf;
}

CPCState *cpc_state_save(CPC *c) {
    cpc_settle(c);
    size_t n; uint8_t *raw = cpc_state_canonical(c, &n); if (!raw) return NULL;
    uLongf zcap = compressBound((uLong)n);
    CPCState *st = malloc(sizeof *st + HEADER_BYTES + zcap); if (!st) { free(raw); return NULL; }
    if (compress2(st->bytes + HEADER_BYTES, &zcap, raw, (uLong)n, 1) != Z_OK) { free(raw); free(st); return NULL; }
    free(raw);
    uint8_t *h = st->bytes;
    memcpy(h, STATE_MAGIC, 4); le32(h + 4, STATE_FORMAT); le64(h + 8, c->frames); le64(h + 16, c->cycles);
    le32(h + 24, (uint32_t)n); le32(h + 28, (uint32_t)zcap);
    st->frames = c->frames; st->cycles = c->cycles; st->size = HEADER_BYTES + zcap;
    CPCState *shrunk = realloc(st, sizeof *st + st->size);
    return shrunk ? shrunk : st;
}

uint64_t cpc_state_hash(CPC *c) {
    cpc_settle(c);
    Ser s = {.mode = SER_HASH, .h = SER_FNV_OFFSET};
    emit(&s, c);
    /* Media is external to CPCState, but the hash must tell apart machines that would read different bytes
     * next: mounted disc contents (even before the first write) and the loaded ROM / cartridge set. */
    ser_tag(&s, "MED0");
    for (int i = 0; i < 4; i++) {
        const CPCDisc *d = fdc_disc(c->fdc, i);
        ser_flag(&s, d != NULL);
        if (d) ser_le(&s, dsk_content_crc(d), 4);
    }
    for (int i = -1; i < 32; i++) {
        uint8_t *rom = i < 0 ? c->rom_lower : c->rom_upper[i];
        ser_flag(&s, rom != NULL);
        if (rom) ser_bytes(&s, rom, 16384);
    }
    ser_flag(&s, c->rom_multiface != NULL);
    if (c->rom_multiface) ser_bytes(&s, c->rom_multiface, 16384);
    ser_le(&s, (uint32_t)c->cartridge_banks, 4);
    if (c->cartridge && c->cartridge_banks > 0) ser_bytes(&s, c->cartridge, (size_t)c->cartridge_banks * 16384);
    return s.h;
}

void cpc_state_free(CPCState *s) { free(s); }
uint64_t cpc_state_frames(const CPCState *s) { return s->frames; }
uint64_t cpc_state_cycles(const CPCState *s) { return s->cycles; }
size_t cpc_state_size(const CPCState *s) { return s->size; }
const uint8_t *cpc_state_bytes(const CPCState *s, size_t *size) { *size = s->size; return s->bytes; }

CPCState *cpc_state_from_bytes(const uint8_t *data, size_t size) {
    if (size < HEADER_BYTES || memcmp(data, STATE_MAGIC, 4) != 0 || rd32(data + 4) != STATE_FORMAT) return NULL;
    if (rd32(data + 24) > RAW_MAX || (size_t)rd32(data + 28) != size - HEADER_BYTES) return NULL;
    CPCState *s = malloc(sizeof *s + size); if (!s) return NULL;
    memcpy(s->bytes, data, size); s->size = size; s->frames = rd64(data + 8); s->cycles = rd64(data + 16);
    return s;
}

bool cpc_state_restore(CPC *c, const CPCState *st, unsigned *hazards) {
    cpc_settle(c);      /* pending time belongs to the machine being replaced */
    unsigned hz = 0;
    if (hazards) *hazards = 0;
    if (st->size < HEADER_BYTES) return false;
    uint32_t raw_size = rd32(st->bytes + 24);
    if (raw_size > RAW_MAX) return false;
    uint8_t *raw = malloc(raw_size ? raw_size : 1); if (!raw) return false;
    uLongf rn = raw_size;
    if (uncompress(raw, &rn, st->bytes + HEADER_BYTES, (uLong)(st->size - HEADER_BYTES)) != Z_OK || rn != raw_size) { free(raw); return false; }

    /* Pass 1: validate the whole stream without touching the machine. */
    DiscImg discs[4];
    StateCfg saved = cfg_of(c);
    Ser chk = {.mode = SER_CHECK, .in = raw, .in_len = raw_size};
    visit_machine(&chk, c, &saved, discs, NULL);
    free_discs(discs);
    if (chk.bad || chk.pos != raw_size) { free(raw); return false; }

    /* Pass 2: the same visit, storing. It cannot fail now (validated above; only allocation can). */
    StateCfg live = cfg_of(c);
    Ser rd = {.mode = SER_READ, .in = raw, .in_len = raw_size};
    bool had_tape = false;
    bool tape_ok = visit_machine(&rd, c, &saved, discs, &had_tape);
    if (memcmp(&saved, &live, sizeof live) != 0) hz |= CPC_STATE_HAZARD_CONFIG;
    for (int i = 0; i < 4; i++) if ((fdc_disc(c->fdc, i) != NULL) != discs[i].present) hz |= CPC_STATE_HAZARD_DISC;
    if (had_tape != (c->tape != NULL) || !tape_ok) hz |= CPC_STATE_HAZARD_TAPE;
    bool ok = !rd.bad;
    if (ok) apply_discs(c, discs);
    free_discs(discs);
    asic_refresh_palette(c);
    cpc_update_mapping(c);
    free(raw);
    if (hazards) *hazards = hz;
    return ok;
}

uint32_t cpc_crc32(const uint8_t *data, size_t size) { return (uint32_t)crc32(0L, data, (uInt)size); }
