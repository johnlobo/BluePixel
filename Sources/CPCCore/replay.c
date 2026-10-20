#include "replay.h"
#include "cpc_keys.h"
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
    char taint[96]; bool tainted;
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
    CPCRecording *r = c->recorder; if (!r) return NULL;
    r->end_frames = c->frames; r->end_cycles = c->cycles; c->recorder = NULL;
    return r;
}

bool cpc_recording_active(const CPC *c) { return c->recorder != NULL; }
void cpc_record_taint(CPC *c, const char *reason) {
    CPCRecording *r = c->recorder; if (!r || r->tainted) return;
    r->tainted = true; snprintf(r->taint, sizeof r->taint, "%s", reason);
}
const char *cpc_recording_taint(const CPCRecording *r) { return r->tainted ? r->taint : NULL; }
const char *cpc_record_taint_now(const CPC *c) { return c->recorder && c->recorder->tainted ? c->recorder->taint : NULL; }
uint64_t cpc_recording_frames(const CPCRecording *r) { return r->end_frames - r->start_frames; }
uint64_t cpc_recording_events(const CPCRecording *r) { return r->nev; }

void cpc_recording_free(CPCRecording *r) {
    if (!r) return;
    cpc_state_free(r->start); free(r->ev); free(r->pool); free(r->chk); free(r);
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
    uint32_t tl = r->tainted ? (uint32_t)strlen(r->taint) : 0; o_u32(&o, tl); o_put(&o, r->taint, tl);
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
    if (tp && tl && tl < sizeof r->taint) { memcpy(r->taint, tp, tl); r->tainted = true; }
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
