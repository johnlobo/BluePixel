#include "replay.h"
#include "cpc_keys.h"
#include "fdc.h"
#include "tape.h"
#include "dsk.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t cycles; uint32_t type; int32_t a, b, d; uint32_t text; } Event;   /* text: pool offset + 1, 0 = none */
typedef struct { uint32_t frame; uint64_t hash; } Check;

struct CPCRecording {
    CPCState *start;
    uint64_t start_frames, start_cycles, end_frames, end_cycles;
    Event *ev; size_t nev, ev_cap;
    char *pool; size_t pool_n, pool_cap;
    Check *chk; size_t nchk, chk_cap;
    char taint[96];
    char *meta;                          /* free-form description of the media/config (set by the frontend) */
};

typedef struct { CPCState *s; size_t next; bool owned; } Keyframe;

struct CPCPlayer {
    CPCRecording *rec;
    size_t next;                         /* next unapplied event */
    Keyframe *keys; size_t nkeys;        /* index = frame / CPC_CHECK_FRAMES */
    size_t key_bytes;
    bool desync; uint64_t desync_frame;
};

/* ---------------- recording ---------------- */
static uint32_t pool_add(CPCRecording *r, const char *text) {
    if (!text) return 0;
    size_t n = strlen(text) + 1;
    if (r->pool_n + n > r->pool_cap) { r->pool_cap = (r->pool_n + n) * 2; r->pool = realloc(r->pool, r->pool_cap); }
    memcpy(r->pool + r->pool_n, text, n); uint32_t off = (uint32_t)r->pool_n + 1; r->pool_n += n; return off;
}

bool replay_input(CPC *c, int type, int a, int b, int d, const char *text) {
    if (c->player) return false;                       /* the replay owns the inputs */
    CPCRecording *r = c->recorder;
    if (r) {
        if (r->nev == r->ev_cap) { r->ev_cap = r->ev_cap ? r->ev_cap * 2 : 256; r->ev = realloc(r->ev, r->ev_cap * sizeof *r->ev); }
        r->ev[r->nev++] = (Event){c->cycles, (uint32_t)type, a, b, d, pool_add(r, text)};
    }
    return true;
}

bool cpc_record_start(CPC *c) {
    if (c->recorder || c->player) return false;
    CPCRecording *r = calloc(1, sizeof *r);
    r->start = cpc_state_save(c);
    if (!r->start) { free(r); return false; }
    r->start_frames = c->frames; r->start_cycles = c->cycles;
    c->recorder = r;
    return true;
}

CPCRecording *cpc_record_stop(CPC *c) {
    CPCRecording *r = c->recorder;
    if (!r) { r = c->stopped_recording; c->stopped_recording = NULL; return r; }   /* cut short by a taint */
    r->end_frames = c->frames; r->end_cycles = c->cycles; c->recorder = NULL;
    return r;
}
const char *cpc_record_cut_reason(const CPC *c) { return c->stopped_recording ? c->stopped_recording->taint : NULL; }
void cpc_recording_set_meta(CPCRecording *r, const char *meta) { free(r->meta); r->meta = meta ? strdup(meta) : NULL; }
const char *cpc_recording_meta(const CPCRecording *r) { return r->meta; }

bool cpc_recording_active(const CPC *c) { return c->recorder != NULL; }
/* Called before an action that replay cannot reproduce: the recording ends here (everything before is valid). */
void cpc_record_taint(CPC *c, const char *reason) {
    CPCRecording *r = c->recorder; if (!r) return;
    snprintf(r->taint, sizeof r->taint, "%s", reason);
    r->end_frames = c->frames; r->end_cycles = c->cycles;
    c->recorder = NULL;
    cpc_recording_free(c->stopped_recording);              /* an earlier uncollected one is dropped */
    c->stopped_recording = r;
}
const char *cpc_recording_taint(const CPCRecording *r) { return r->taint[0] ? r->taint : NULL; }
uint64_t cpc_recording_frames(const CPCRecording *r) { return r->end_frames - r->start_frames; }
uint64_t cpc_recording_events(const CPCRecording *r) { return r->nev; }

void cpc_recording_free(CPCRecording *r) {
    if (!r) return;
    cpc_state_free(r->start); free(r->ev); free(r->pool); free(r->chk); free(r->meta); free(r);
}

/* ---------------- replay ---------------- */
static void apply(CPC *c, const CPCRecording *r, const Event *e) {
    switch (e->type) {
    case CPC_EV_KEY: cpc_key_raw(c, e->a, e->b != 0); break;
    case CPC_EV_JOY: cpc_joystick_raw(c, e->a, e->b != 0); break;
    case CPC_EV_AMX: cpc_amx_mouse_raw(c, e->a, e->b, (uint8_t)e->d); break;
    case CPC_EV_AUTOTYPE: if (e->text) cpc_autotype_raw(c, r->pool + e->text - 1); break;
    case CPC_EV_AUTOTYPE_CANCEL: cpc_autotype_cancel_raw(c); break;
    case CPC_EV_RESET: cpc_reset_raw(c, e->a != 0); break;
    case CPC_EV_MF_STOP: cpc_multiface_stop_raw(c); break;
    }
}

void replay_apply_due(CPC *c) {
    CPCPlayer *p = c->player; const CPCRecording *r = p->rec;
    while (p->next < r->nev && r->ev[p->next].cycles <= c->cycles) apply(c, r, &r->ev[p->next++]);
}


static void add_check(CPCRecording *r, uint32_t frame, uint64_t hash) {
    if (r->nchk == r->chk_cap) { r->chk_cap = r->chk_cap ? r->chk_cap * 2 : 64; r->chk = realloc(r->chk, r->chk_cap * sizeof *r->chk); }
    r->chk[r->nchk++] = (Check){frame, hash};
}
static const Check *find_check(const CPCRecording *r, uint32_t frame) {
    size_t lo = 0, hi = r->nchk;                        /* sorted by frame */
    while (lo < hi) { size_t m = (lo + hi) / 2; if (r->chk[m].frame < frame) lo = m + 1; else hi = m; }
    return lo < r->nchk && r->chk[lo].frame == frame ? &r->chk[lo] : NULL;
}

void replay_frame_done(CPC *c) {
    if (c->recorder) {
        CPCRecording *r = c->recorder;
        uint64_t f = c->frames - r->start_frames;
        if (f > 0 && f % CPC_CHECK_FRAMES == 0) add_check(r, (uint32_t)f, cpc_state_hash(c));
    }
    if (c->player) {
        CPCPlayer *p = c->player; const CPCRecording *r = p->rec;
        uint64_t f = c->frames - r->start_frames;
        if (f % CPC_CHECK_FRAMES == 0) {
            const Check *ck = find_check(r, (uint32_t)f);
            if (ck && !p->desync && cpc_state_hash(c) != ck->hash) { p->desync = true; p->desync_frame = f; }
            size_t k = (size_t)(f / CPC_CHECK_FRAMES);
            if (k >= p->nkeys) {
                size_t n = k + 16; p->keys = realloc(p->keys, n * sizeof *p->keys);
                memset(p->keys + p->nkeys, 0, (n - p->nkeys) * sizeof *p->keys); p->nkeys = n;
            }
            if (!p->keys[k].s && !p->desync) {          /* keyframes taken after a desync would be wrong */
                CPCState *s = cpc_state_save(c);
                if (s) { p->keys[k] = (Keyframe){s, p->next, true}; p->key_bytes += cpc_state_size(s); }
            }
        }
    }
}

/* ---------------- player ---------------- */
CPCPlayer *cpc_player_create(CPCRecording *r) { CPCPlayer *p = calloc(1, sizeof *p); p->rec = r; return p; }

static void keys_clear(CPCPlayer *p) {
    for (size_t i = 0; i < p->nkeys; i++) if (p->keys[i].s && p->keys[i].owned) cpc_state_free(p->keys[i].s);
    free(p->keys); p->keys = NULL; p->nkeys = 0; p->key_bytes = 0;
}
void cpc_player_free(CPCPlayer *p) { if (!p) return; keys_clear(p); free(p); }

bool cpc_player_begin(CPC *c, CPCPlayer *p, unsigned *hazards) {
    if (c->recorder || !p->rec->start) return false;
    c->player = NULL;
    if (!cpc_state_restore(c, p->rec->start, hazards)) return false;
    keys_clear(p);
    p->nkeys = 16; p->keys = calloc(p->nkeys, sizeof *p->keys);
    p->keys[0] = (Keyframe){p->rec->start, 0, false};
    p->next = 0; p->desync = false; p->desync_frame = 0;
    c->player = p;
    return true;
}

void cpc_player_end(CPC *c) { c->player = NULL; }
bool cpc_player_active(const CPC *c) { return c->player != NULL; }
uint64_t cpc_player_frame(const CPC *c) { return c->player ? c->frames - c->player->rec->start_frames : 0; }
uint64_t cpc_player_length(const CPC *c) { return c->player ? cpc_recording_frames(c->player->rec) : 0; }
bool cpc_player_finished(const CPC *c) { return c->player && cpc_player_frame(c) >= cpc_player_length(c); }
bool cpc_player_desync(const CPC *c, uint64_t *frame) {
    if (!c->player || !c->player->desync) return false;
    if (frame) *frame = c->player->desync_frame; return true;
}
size_t cpc_player_keyframes(const CPC *c) {
    size_t n = 0; if (!c->player) return 0;
    for (size_t i = 0; i < c->player->nkeys; i++) if (c->player->keys[i].s) n++;
    return n;
}
size_t cpc_player_keyframe_bytes(const CPC *c) { return c->player ? c->player->key_bytes : 0; }

bool cpc_player_seek(CPC *c, uint64_t frame) {
    CPCPlayer *p = c->player; if (!p) return false;
    uint64_t len = cpc_recording_frames(p->rec);
    if (frame > len) frame = len;
    uint64_t base = p->rec->start_frames, target = base + frame, cur = c->frames - base;
    size_t k = (size_t)(frame / CPC_CHECK_FRAMES);
    if (k >= p->nkeys) k = p->nkeys ? p->nkeys - 1 : 0;
    while (k > 0 && !p->keys[k].s) k--;
    /* already between the keyframe and the target, strictly before it: just run on. Otherwise restore, which
     * also lands exactly on a frame boundary when the machine was stopped in the middle of a frame. */
    bool run_on = cur < frame && cur >= (uint64_t)k * CPC_CHECK_FRAMES;
    struct Debugger *dbg = c->dbg; c->dbg = NULL;           /* seeking is silent: no breakpoints, logpoints or history */
    bool ok = true;
    if (!run_on) {
        ok = p->keys[k].s && cpc_state_restore(c, p->keys[k].s, NULL);
        p->next = p->keys[k].next;
    }
    uint64_t guard = (target - c->frames) * 4 + 1000;
    while (ok && c->frames < target) { cpc_run_frame(c); if (guard-- == 0) ok = false; }
    c->dbg = dbg;
    return ok;
}

uint64_t cpc_player_indexed(const CPC *c) {
    const CPCPlayer *p = c->player; if (!p) return 0;
    uint64_t len = cpc_recording_frames(p->rec), last = len / CPC_CHECK_FRAMES;
    uint64_t k = 0;
    while (k <= last && k < p->nkeys && p->keys[k].s) k++;
    if (k > last) return len;                         /* every keyframe up to the end exists */
    return k == 0 ? 0 : (k - 1) * CPC_CHECK_FRAMES;
}

int cpc_player_capture(CPC *c, uint64_t from, int count, uint32_t *out) {
    CPCPlayer *p = c->player; if (!p || !cpc_player_seek(c, from)) return 0;
    uint64_t len = cpc_recording_frames(p->rec);
    struct Debugger *dbg = c->dbg; c->dbg = NULL;
    int n = 0;
    while (n < count && cpc_player_frame(c) < len) {
        uint64_t before = c->frames; int guard = 4000;
        while (c->frames == before && guard-- > 0) cpc_run_frame(c);
        if (c->frames == before) break;
        memcpy(out + (size_t)n * CPC_SCREEN_W * CPC_SCREEN_H, c->frame_done, (size_t)CPC_SCREEN_W * CPC_SCREEN_H * 4);
        n++;
    }
    c->dbg = dbg;
    return n;
}

CPC *cpc_clone_machine(const CPC *src) {
    CPC *c = cpc_create(src->model);
    cpc_configure(c, &src->cfg);
    if (src->rom_lower) cpc_load_rom(c, -1, src->rom_lower, 16384);
    for (int i = 0; i < 32; i++) if (src->rom_upper[i]) cpc_load_rom(c, i, src->rom_upper[i], 16384);
    if (src->rom_multiface) cpc_load_multiface(c, src->rom_multiface, 8192);
    if (src->cartridge) { c->cartridge = malloc((size_t)32 * 16384); memcpy(c->cartridge, src->cartridge, (size_t)32 * 16384); c->cartridge_banks = src->cartridge_banks; }
    c->amx_enabled = src->amx_enabled; memcpy(c->single_sided_read, src->single_sided_read, sizeof c->single_sided_read);
    for (int i = 0; i < 4; i++) { CPCDisc *d = fdc_disc(src->fdc, i); if (d) fdc_insert(c->fdc, i, dsk_clone(d)); }
    if (src->tape) c->tape = tape_clone(src->tape);
    cpc_update_mapping(c);
    return c;
}

void cpc_clone_free(CPC *c) {
    if (!c) return;
    for (int i = 0; i < 4; i++) dsk_free(fdc_disc(c->fdc, i));
    cpc_destroy(c);
}

size_t cpc_player_adopt_keyframes(CPC *c, CPCPlayer *src) {
    CPCPlayer *p = c->player;
    if (!p || p->rec != src->rec) return 0;
    size_t moved = 0;
    for (size_t k = 1; k < src->nkeys; k++) {
        if (!src->keys[k].s || !src->keys[k].owned) continue;
        if (k >= p->nkeys) {
            size_t n = k + 16; p->keys = realloc(p->keys, n * sizeof *p->keys);
            memset(p->keys + p->nkeys, 0, (n - p->nkeys) * sizeof *p->keys); p->nkeys = n;
        }
        if (p->keys[k].s) continue;
        p->keys[k] = src->keys[k]; p->key_bytes += cpc_state_size(src->keys[k].s);
        src->keys[k].s = NULL; moved++;
    }
    return moved;
}

/* ---------------- reverse debugging ---------------- */
/* Everything works by restoring the nearest keyframe before the target and re-simulating forwards: the core is
 * deterministic and about 50x faster than real time. Time is c->cycles (emulated microseconds), which is unique
 * per step. */
static int key_before(const CPCPlayer *p, uint64_t cycles) {          /* latest keyframe strictly before `cycles` */
    for (int k = (int)p->nkeys - 1; k >= 0; k--) if (p->keys[k].s && cpc_state_cycles(p->keys[k].s) < cycles) return k;
    return -1;
}
static bool restore_key(CPC *c, CPCPlayer *p, int k) {
    if (!cpc_state_restore(c, p->keys[k].s, NULL)) return false;
    p->next = p->keys[k].next; return true;
}

bool cpc_player_goto_cycles(CPC *c, uint64_t target, bool history) {
    CPCPlayer *p = c->player; if (!p) return false;
    int k = key_before(p, target + 1);                                 /* a keyframe exactly at target is fine too */
    if (history) { int earlier = key_before(p, target); if (earlier >= 0) k = earlier; }   /* leave steps to record */
    if (k < 0 || !restore_key(c, p, k)) return false;
    Debugger *d = c->dbg;
    bool active = false, brk = false, hist = false; int temp = -1;
    if (d) {
        active = d->active; brk = d->break_instructions; hist = d->hist_on; temp = d->temp_bp;
        d->active = false; d->break_instructions = false; d->temp_bp = -1; d->hist_on = history; d->skip_once = false;
        if (history) { d->hist_count = 0; d->hist_pos = 0; }
    }
    uint64_t guard = 20000000;
    while (c->cycles < target && guard--) cpc_step(c);
    if (d) { d->active = active; d->break_instructions = brk; d->hist_on = hist; d->temp_bp = temp; d->hit = false; d->skip_once = false; }
    return c->cycles == target;
}

bool cpc_player_step_back(CPC *c) {
    CPCPlayer *p = c->player; if (!p) return false;
    uint64_t here = c->cycles;
    int k = key_before(p, here);
    if (k < 0 || !restore_key(c, p, k)) return false;
    Debugger *d = c->dbg; c->dbg = NULL;                               /* silent pass: find where the last step began */
    uint64_t prev = c->cycles, guard = 20000000;
    while (c->cycles < here && guard--) { prev = c->cycles; cpc_step(c); }
    c->dbg = d;
    if (c->cycles != here) { cpc_player_goto_cycles(c, here, true); return false; }
    return cpc_player_goto_cycles(c, prev, true);
}

struct CPCReverseScan {
    uint64_t origin, upper, lowest;
    bool finished, found, was_active, was_brk, was_hist; int temp_bp, was_temp;
    CPCReverseHit hit;
    DbgBreakpoint *saved;
};

CPCReverseScan *cpc_reverse_begin(CPC *c, CPCReverseMode mode, uint16_t lo, uint16_t hi) {
    CPCPlayer *p = c->player; if (!p) return NULL;
    Debugger *d = dbg_attach(c);
    CPCReverseScan *s = calloc(1, sizeof *s);
    s->saved = malloc(sizeof d->bp); memcpy(s->saved, d->bp, sizeof d->bp);
    s->origin = s->upper = c->cycles; s->lowest = p->rec->start_cycles; s->temp_bp = -1;
    s->was_active = d->active; s->was_brk = d->break_instructions; s->was_hist = d->hist_on; s->was_temp = d->temp_bp;
    if (mode != CPC_REV_BREAKPOINTS) {
        for (int i = 0; i < DBG_MAX_BP; i++) d->bp[i].enabled = false;                 /* only the temporary watch */
        s->temp_bp = mode == CPC_REV_CODE ? dbg_add_code_bp(c, lo) : dbg_add_mem_bp(c, lo, hi, mode == CPC_REV_READ, mode == CPC_REV_WRITE);
        if (s->temp_bp < 0) { memcpy(d->bp, s->saved, sizeof d->bp); dbg_rebuild_watch(d); free(s->saved); free(s); return NULL; }
        if (mode == CPC_REV_CODE) d->bp[s->temp_bp].block = NULL;                       /* any memory mapping */
    }
    d->active = true; d->break_instructions = false; d->hist_on = false; d->temp_bp = -1; d->scanning = true;
    dbg_rebuild_watch(d);
    return s;
}

bool cpc_reverse_slice(CPC *c, CPCReverseScan *s) {
    CPCPlayer *p = c->player; Debugger *d = c->dbg;
    if (s->finished) return true;
    int k = key_before(p, s->upper);
    if (k < 0 || !restore_key(c, p, k)) { s->finished = true; return true; }
    d->hit = false; d->skip_once = false;
    CPCReverseHit last = {0};
    uint64_t upper = s->upper, guard = 40000000;
    while (c->cycles < upper && guard--) {
        uint64_t t0 = c->cycles; uint16_t pc0 = c->z80.pc;
        int n = cpc_step(c);
        if (d->hit) {
            int bi = d->hit_index;
            if (bi >= 0 && bi < DBG_MAX_BP) {
                const DbgBreakpoint *b = &d->bp[bi];
                last.found = true; last.cycles = t0; last.pc = pc0; last.bp_index = bi;
                last.kind = b->type == BP_CODE ? 1 : b->type == BP_MEM_READ ? 2 : b->type == BP_MEM_WRITE ? 3 : 4;
                last.addr = d->ev_addr; last.value = d->ev_value; last.previous = d->ev_prev;
            }
            d->hit = false;
            if (n == 0) d->skip_once = true;                          /* stopped before executing: let this instruction run */
        }
    }
    if (last.found) { s->hit = last; s->found = true; s->finished = true; return true; }
    s->upper = cpc_state_cycles(p->keys[k].s);
    if (s->upper <= s->lowest) s->finished = true;
    return s->finished;
}

double cpc_reverse_progress(const CPCReverseScan *s) {
    if (s->origin <= s->lowest) return 1;
    return (double)(s->origin - s->upper) / (double)(s->origin - s->lowest);
}
uint64_t cpc_reverse_cycles(const CPCReverseScan *s) { return s->upper; }

bool cpc_reverse_end(CPC *c, CPCReverseScan *s, bool keep, CPCReverseHit *hit) {
    Debugger *d = c->dbg;
    memcpy(d->bp, s->saved, sizeof d->bp);                            /* also restores the user's counters */
    d->active = s->was_active; d->break_instructions = s->was_brk; d->hist_on = s->was_hist; d->temp_bp = s->was_temp;
    d->scanning = false; d->hit = false; d->skip_once = false;
    dbg_rebuild_watch(d);
    bool found = s->found;
    if (hit) *hit = s->hit;
    cpc_player_goto_cycles(c, keep && found ? s->hit.cycles : s->origin, true);
    free(s->saved); free(s);
    return found;
}

/* ---------------- file format ---------------- */
typedef struct { uint8_t *p; size_t n, cap; } Out;
static void o_put(Out *o, const void *d, size_t n) { if (o->n + n > o->cap) { o->cap = (o->n + n) * 2; o->p = realloc(o->p, o->cap); } memcpy(o->p + o->n, d, n); o->n += n; }
static void o_u32(Out *o, uint32_t v) { o_put(o, &v, 4); }
static void o_u64(Out *o, uint64_t v) { o_put(o, &v, 8); }

#define REC_MAGIC 0x52435043u        /* "CPCR" */
#define REC_VERSION 1u

uint8_t *cpc_recording_serialize(const CPCRecording *r, size_t *size) {
    Out o = {0};
    o_u32(&o, REC_MAGIC); o_u32(&o, REC_VERSION); o_u32(&o, (uint32_t)sizeof(CPC));
    o_u64(&o, r->start_frames); o_u64(&o, r->start_cycles); o_u64(&o, r->end_frames); o_u64(&o, r->end_cycles);
    uint32_t tl = (uint32_t)strlen(r->taint); o_u32(&o, tl); o_put(&o, r->taint, tl);
    uint32_t ml = r->meta ? (uint32_t)strlen(r->meta) : 0; o_u32(&o, ml); o_put(&o, r->meta, ml);
    size_t sn; const uint8_t *sb = cpc_state_bytes(r->start, &sn); o_u32(&o, (uint32_t)sn); o_put(&o, sb, sn);
    o_u32(&o, (uint32_t)r->nev);
    for (size_t i = 0; i < r->nev; i++) { const Event *e = &r->ev[i]; o_u64(&o, e->cycles); o_u32(&o, e->type); o_u32(&o, (uint32_t)e->a); o_u32(&o, (uint32_t)e->b); o_u32(&o, (uint32_t)e->d); o_u32(&o, e->text); }
    o_u32(&o, (uint32_t)r->pool_n); o_put(&o, r->pool, r->pool_n);
    o_u32(&o, (uint32_t)r->nchk);
    for (size_t i = 0; i < r->nchk; i++) { o_u32(&o, r->chk[i].frame); o_u64(&o, r->chk[i].hash); }
    *size = o.n; return o.p;
}

typedef struct { const uint8_t *p; size_t n, pos; bool bad; } In;
static const uint8_t *i_take(In *i, size_t n) { if (i->bad || n > i->n - i->pos) { i->bad = true; return NULL; } const uint8_t *p = i->p + i->pos; i->pos += n; return p; }
static uint32_t i_u32(In *i) { const uint8_t *p = i_take(i, 4); uint32_t v = 0; if (p) memcpy(&v, p, 4); return v; }
static uint64_t i_u64(In *i) { const uint8_t *p = i_take(i, 8); uint64_t v = 0; if (p) memcpy(&v, p, 8); return v; }

CPCRecording *cpc_recording_deserialize(const uint8_t *data, size_t size) {
    In in = {data, size, 0, false};
    if (i_u32(&in) != REC_MAGIC || i_u32(&in) != REC_VERSION || i_u32(&in) != sizeof(CPC)) return NULL;
    CPCRecording *r = calloc(1, sizeof *r);
    r->start_frames = i_u64(&in); r->start_cycles = i_u64(&in); r->end_frames = i_u64(&in); r->end_cycles = i_u64(&in);
    uint32_t tl = i_u32(&in); const uint8_t *tp = i_take(&in, tl);
    if (tp && tl && tl < sizeof r->taint) memcpy(r->taint, tp, tl);
    uint32_t ml = i_u32(&in); const uint8_t *mp = i_take(&in, ml);
    if (mp && ml) { r->meta = malloc(ml + 1); memcpy(r->meta, mp, ml); r->meta[ml] = 0; }
    uint32_t sn = i_u32(&in); const uint8_t *sb = i_take(&in, sn);
    if (sb) r->start = cpc_state_from_bytes(sb, sn);
    uint32_t nev = i_u32(&in);
    if (!in.bad && (size_t)nev <= (in.n - in.pos) / 28) {
        r->ev = malloc((nev ? nev : 1) * sizeof *r->ev); r->ev_cap = nev ? nev : 1;
        for (uint32_t k = 0; k < nev; k++) { Event *e = &r->ev[r->nev++]; e->cycles = i_u64(&in); e->type = i_u32(&in); e->a = (int32_t)i_u32(&in); e->b = (int32_t)i_u32(&in); e->d = (int32_t)i_u32(&in); e->text = i_u32(&in); }
    } else in.bad = true;
    uint32_t pn = i_u32(&in); const uint8_t *pp = i_take(&in, pn);
    if (pp) { r->pool = malloc(pn ? pn : 1); memcpy(r->pool, pp, pn); r->pool_n = r->pool_cap = pn; }
    uint32_t nc = i_u32(&in);
    if (!in.bad && (size_t)nc <= (in.n - in.pos) / 12) {
        r->chk = malloc((nc ? nc : 1) * sizeof *r->chk); r->chk_cap = nc ? nc : 1;
        for (uint32_t k = 0; k < nc; k++) { r->chk[r->nchk].frame = i_u32(&in); r->chk[r->nchk].hash = i_u64(&in); r->nchk++; }
    } else in.bad = true;
    /* event text offsets must point inside the pool */
    for (size_t k = 0; !in.bad && k < r->nev; k++) if (r->ev[k].text && (r->ev[k].text > r->pool_n || r->pool[r->pool_n - 1] != 0)) in.bad = true;
    if (in.bad || !r->start) { cpc_recording_free(r); return NULL; }
    return r;
}
