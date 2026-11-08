#include "tape.h"
#include <stdlib.h>
#include <string.h>
#include "ser.h"
#include <math.h>
#include <stdio.h>

#define TSTATE_US (1.0 / 3.5)      /* TZX/CDT timings are in 3.5 MHz T-states */

typedef struct { uint8_t type; uint32_t off, len; char info[80]; } TapeBlock;

enum { KIND_TZX, KIND_AUDIO };

struct CPCTape {
    uint8_t *data; size_t size; int kind;
    TapeBlock *blocks; int nblocks;
    int cur;                         /* current block */
    uint32_t *pulses; int npulses, pcap, pidx;
    double rem;                      /* remaining time of current pulse/pause (us) */
    double pause_rem;
    bool level, playing, motor, ended;
    int loop_start, loop_count;
    int call_stack[16], call_sp;
    double elapsed_us;
    /* audio (VOC/WAV) */
    int16_t *samples; size_t nsamples; double rate; double pos; bool aud_level;
    char name[256];
};

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd24(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static void add_pulse(CPCTape *t, uint32_t len) {
    if (t->npulses == t->pcap) { t->pcap = t->pcap ? t->pcap * 2 : 4096; t->pulses = realloc(t->pulses, t->pcap * sizeof(uint32_t)); }
    t->pulses[t->npulses++] = len;
}

static void add_data(CPCTape *t, const uint8_t *d, uint32_t len, uint32_t b0, uint32_t b1, int used_last) {
    for (uint32_t i = 0; i < len; i++) {
        int bits = (i == len - 1) ? used_last : 8;
        for (int b = 7; b > 8 - bits - 1 && b >= 8 - bits; b--) {
            uint32_t p = (d[i] >> b) & 1 ? b1 : b0;
            add_pulse(t, p); add_pulse(t, p);
        }
    }
}

static const char *type_name(uint8_t id) {
    switch (id) {
    case 0x10: return "Standard Speed Data"; case 0x11: return "Turbo Speed Data"; case 0x12: return "Pure Tone";
    case 0x13: return "Pulse Sequence"; case 0x14: return "Pure Data"; case 0x15: return "Direct Recording";
    case 0x20: return "Pause"; case 0x21: return "Group Start"; case 0x22: return "Group End"; case 0x23: return "Jump";
    case 0x24: return "Loop Start"; case 0x25: return "Loop End"; case 0x26: return "Call Sequence"; case 0x27: return "Return";
    case 0x28: return "Select"; case 0x2A: return "Stop if 48K"; case 0x2B: return "Set Signal Level"; case 0x30: return "Text";
    case 0x31: return "Message"; case 0x32: return "Archive Info"; case 0x33: return "Hardware Type"; case 0x35: return "Custom Info";
    case 0x5A: return "Glue"; default: return "Unknown";
    }
}

static void describe(CPCTape *t, TapeBlock *b) {
    const uint8_t *d = t->data + b->off;
    const uint8_t *payload = NULL; uint32_t plen = 0;
    if (b->type == 0x10 && b->len >= 5) { payload = d + 5; plen = rd16(d + 3); }
    else if (b->type == 0x11 && b->len >= 19) { payload = d + 19; plen = rd24(d + 16); }
    else if (b->type == 0x14 && b->len >= 11) { payload = d + 11; plen = rd24(d + 8); }
    if (payload && plen >= 18 && payload[0] == 0x2C) {
        char nm[17]; memcpy(nm, payload + 1, 16); nm[16] = 0;
        for (int i = 0; i < 16; i++) if (nm[i] < 32 || nm[i] > 126) nm[i] = ' ';
        snprintf(b->info, sizeof b->info, "%s: \"%s\" block %d", type_name(b->type), nm, payload[17]);
    } else if (b->type == 0x30 && b->len >= 2) {
        int n = d[0]; if (n > 60) n = 60; snprintf(b->info, sizeof b->info, "Text: %.*s", n, d + 1);
    } else if (payload) snprintf(b->info, sizeof b->info, "%s (%u bytes)", type_name(b->type), plen);
    else snprintf(b->info, sizeof b->info, "%s", type_name(b->type));
}

static bool parse_tzx(CPCTape *t) {
    const uint8_t *d = t->data; size_t n = t->size, p = 10;
    if (n < 10) return false;
    int cap = 64; t->blocks = calloc(cap, sizeof(TapeBlock));
    while (p < n) {
        uint8_t id = d[p++]; uint32_t len = 0;
        switch (id) {
        case 0x10: if (p + 4 > n) goto done; len = 4 + rd16(d + p + 2); break;
        case 0x11: if (p + 18 > n) goto done; len = 18 + rd24(d + p + 15); break;
        case 0x12: len = 4; break;
        case 0x13: if (p + 1 > n) goto done; len = 1 + d[p] * 2; break;
        case 0x14: if (p + 10 > n) goto done; len = 10 + rd24(d + p + 7); break;
        case 0x15: if (p + 8 > n) goto done; len = 8 + rd24(d + p + 5); break;
        case 0x18: case 0x19: if (p + 4 > n) goto done; len = 4 + rd32(d + p); break;
        case 0x20: len = 2; break;
        case 0x21: if (p + 1 > n) goto done; len = 1 + d[p]; break;
        case 0x22: len = 0; break;
        case 0x23: len = 2; break;
        case 0x24: len = 2; break;
        case 0x25: len = 0; break;
        case 0x26: if (p + 2 > n) goto done; len = 2 + rd16(d + p) * 2; break;
        case 0x27: len = 0; break;
        case 0x28: if (p + 2 > n) goto done; len = 2 + rd16(d + p); break;
        case 0x2A: len = 4; break;
        case 0x2B: len = 5; break;
        case 0x30: if (p + 1 > n) goto done; len = 1 + d[p]; break;
        case 0x31: if (p + 2 > n) goto done; len = 2 + d[p + 1]; break;
        case 0x32: if (p + 2 > n) goto done; len = 2 + rd16(d + p); break;
        case 0x33: if (p + 1 > n) goto done; len = 1 + d[p] * 3; break;
        case 0x35: if (p + 20 > n) goto done; len = 20 + 4 + rd32(d + p + 16); break;
        case 0x5A: len = 9; break;
        default: if (p + 4 > n) goto done; len = 4 + rd32(d + p); break;
        }
        if (p + len > n) goto done;
        if (t->nblocks == cap) { cap *= 2; t->blocks = realloc(t->blocks, cap * sizeof(TapeBlock)); }
        TapeBlock *b = &t->blocks[t->nblocks++];
        memset(b, 0, sizeof *b); b->type = id; b->off = (uint32_t)p; b->len = len;
        describe(t, b);
        p += len;
    }
done:
    return t->nblocks > 0;
}

static void load_block(CPCTape *t) {
    t->npulses = 0; t->pidx = 0; t->rem = 0; t->pause_rem = 0;
    if (t->cur >= t->nblocks) { t->ended = true; t->playing = false; return; }
    TapeBlock *b = &t->blocks[t->cur];
    const uint8_t *d = t->data + b->off;
    switch (b->type) {
    case 0x10: {
        uint32_t pause = rd16(d), len = rd16(d + 2); const uint8_t *data = d + 4;
        int pilot = (len && data[0] < 128) ? 8063 : 3223;
        for (int i = 0; i < pilot; i++) add_pulse(t, 2168);
        add_pulse(t, 667); add_pulse(t, 735);
        add_data(t, data, len, 855, 1710, 8);
        t->pause_rem = pause * 1000.0; break; }
    case 0x11: {
        uint32_t pilot = rd16(d), s1 = rd16(d + 2), s2 = rd16(d + 4), b0 = rd16(d + 6), b1 = rd16(d + 8), pc = rd16(d + 10);
        int used = d[12]; uint32_t pause = rd16(d + 13), len = rd24(d + 15);
        for (uint32_t i = 0; i < pc; i++) add_pulse(t, pilot);
        add_pulse(t, s1); add_pulse(t, s2);
        add_data(t, d + 18, len, b0, b1, used ? used : 8);
        t->pause_rem = pause * 1000.0; break; }
    case 0x12: { uint32_t l = rd16(d), c = rd16(d + 2); for (uint32_t i = 0; i < c; i++) add_pulse(t, l); break; }
    case 0x13: { int n = d[0]; for (int i = 0; i < n; i++) add_pulse(t, rd16(d + 1 + i * 2)); break; }
    case 0x14: {
        uint32_t b0 = rd16(d), b1 = rd16(d + 2); int used = d[4]; uint32_t pause = rd16(d + 5), len = rd24(d + 7);
        add_data(t, d + 10, len, b0, b1, used ? used : 8);
        t->pause_rem = pause * 1000.0; break; }
    case 0x15: {
        uint32_t per = rd16(d); uint32_t pause = rd16(d + 2); int used = d[4]; uint32_t len = rd24(d + 5);
        const uint8_t *s = d + 8; int last = -1; uint32_t run = 0;
        for (uint32_t i = 0; i < len; i++) {
            int bits = (i == len - 1) ? (used ? used : 8) : 8;
            for (int k = 0; k < bits; k++) {
                int v = (s[i] >> (7 - k)) & 1;
                if (v == last) run += per; else { if (last >= 0) add_pulse(t, run); last = v; run = per; }
            }
        }
        if (run) add_pulse(t, run);
        t->pause_rem = pause * 1000.0; break; }
    case 0x20: {
        uint32_t ms = rd16(d);
        if (ms == 0) { t->playing = false; t->cur++; return; }
        t->pause_rem = ms * 1000.0; break; }
    case 0x2B: t->level = d[4] != 0; break;
    default: break;
    }
}

static void next_block(CPCTape *t) {
    TapeBlock *b = t->cur < t->nblocks ? &t->blocks[t->cur] : NULL;
    (void)b;
    t->cur++;
    /* handle control blocks */
    while (t->cur < t->nblocks) {
        TapeBlock *c = &t->blocks[t->cur]; const uint8_t *d = t->data + c->off;
        if (c->type == 0x23) { int16_t off = (int16_t)rd16(d); t->cur += off; if (t->cur < 0) t->cur = 0; continue; }
        if (c->type == 0x24) { t->loop_start = t->cur + 1; t->loop_count = rd16(d); t->cur++; continue; }
        if (c->type == 0x25) { if (--t->loop_count > 0) t->cur = t->loop_start; else t->cur++; continue; }
        if (c->type == 0x26 && t->call_sp < 16 && rd16(d) > 0) { t->call_stack[t->call_sp++] = t->cur + 1; t->cur += (int16_t)rd16(d + 2); continue; }
        if (c->type == 0x27 && t->call_sp > 0) { t->cur = t->call_stack[--t->call_sp]; continue; }
        break;
    }
    load_block(t);
}

/* ---- audio containers ---- */
static bool parse_wav(CPCTape *t) {
    const uint8_t *d = t->data; size_t n = t->size;
    if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
    size_t p = 12; int ch = 1, bits = 8; uint32_t rate = 44100; const uint8_t *pcm = NULL; size_t plen = 0;
    while (p + 8 <= n) {
        uint32_t sz = rd32(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && p + 24 <= n) { ch = rd16(d + p + 10); rate = rd32(d + p + 12); bits = rd16(d + p + 22); }
        else if (!memcmp(d + p, "data", 4)) { pcm = d + p + 8; plen = (p + 8 + sz > n) ? n - p - 8 : sz; break; }
        p += 8 + sz + (sz & 1);
    }
    if (!pcm || (bits != 8 && bits != 16) || ch < 1) return false;
    size_t frame = (size_t)ch * (bits / 8); t->nsamples = plen / frame;
    t->samples = malloc(t->nsamples * sizeof(int16_t));
    for (size_t i = 0; i < t->nsamples; i++) {
        const uint8_t *s = pcm + i * frame;
        t->samples[i] = bits == 8 ? (int16_t)((s[0] - 128) << 8) : (int16_t)(s[0] | (s[1] << 8));
    }
    t->rate = rate; t->kind = KIND_AUDIO; return true;
}

static bool parse_voc(CPCTape *t) {
    const uint8_t *d = t->data; size_t n = t->size;
    if (n < 26 || memcmp(d, "Creative Voice File", 19)) return false;
    size_t p = rd16(d + 20); size_t cap = 0; t->nsamples = 0; t->samples = NULL; double rate = 0;
    while (p < n) {
        uint8_t type = d[p]; if (type == 0) break;
        if (p + 4 > n) break;
        uint32_t len = rd24(d + p + 1); p += 4;
        if (p + len > n) len = (uint32_t)(n - p);
        if (type == 1 || type == 2) {
            const uint8_t *s = d + p; uint32_t cnt = len;
            if (type == 1) { if (len < 2) break; rate = 1000000.0 / (256 - s[0]); s += 2; cnt -= 2; }
            if (t->nsamples + cnt > cap) { cap = (t->nsamples + cnt) * 2; t->samples = realloc(t->samples, cap * sizeof(int16_t)); }
            for (uint32_t i = 0; i < cnt; i++) t->samples[t->nsamples++] = (int16_t)((s[i] - 128) << 8);
        } else if (type == 3 && len >= 3) {
            uint32_t cnt = rd16(d + p) + 1; if (t->nsamples + cnt > cap) { cap = (t->nsamples + cnt) * 2; t->samples = realloc(t->samples, cap * sizeof(int16_t)); }
            for (uint32_t i = 0; i < cnt; i++) t->samples[t->nsamples++] = 0;
        }
        p += len;
    }
    if (!t->nsamples || rate <= 0) return false;
    t->rate = rate; t->kind = KIND_AUDIO; return true;
}

CPCTape *tape_load(const uint8_t *data, size_t size, const char *name) {
    CPCTape *t = calloc(1, sizeof *t);
    t->data = malloc(size); memcpy(t->data, data, size); t->size = size;
    if (name) strncpy(t->name, name, sizeof t->name - 1);
    bool ok = false;
    if (size > 10 && !memcmp(data, "ZXTape!\x1a", 8)) { t->kind = KIND_TZX; ok = parse_tzx(t); }
    else if (size > 8 && !memcmp(data, "Creative", 8)) ok = parse_voc(t);
    else if (size > 12 && !memcmp(data, "RIFF", 4)) ok = parse_wav(t);
    if (!ok) { tape_free(t); return NULL; }
    tape_rewind(t);
    return t;
}

void tape_free(CPCTape *t) {
    if (!t) return;
    free(t->data); free(t->blocks); free(t->pulses); free(t->samples); free(t);
}

void tape_rewind(CPCTape *t) {
    if (!t) return;
    t->cur = 0; t->ended = false; t->level = false; t->pos = 0; t->elapsed_us = 0; t->call_sp = 0; t->aud_level = false;
    t->playing = false;
    if (t->kind == KIND_TZX) { load_block(t); }
}

void tape_set_motor(CPCTape *t, bool on) { if (t) t->motor = on; }
void tape_play(CPCTape *t, bool on) { if (t) { t->playing = on; if (on && t->ended) tape_rewind(t), t->playing = true; } }
bool tape_playing(const CPCTape *t) { return t && t->playing; }
bool tape_read_level(CPCTape *t) { return t ? (t->kind == KIND_AUDIO ? t->aud_level : t->level) : false; }
int tape_block(const CPCTape *t) { return t ? (t->cur < t->nblocks ? t->cur : t->nblocks) : 0; }
int tape_block_count(const CPCTape *t) { return t ? t->nblocks : 0; }
const char *tape_block_info(const CPCTape *t, int i) { return (t && i >= 0 && i < t->nblocks) ? t->blocks[i].info : ""; }
const char *tape_name(const CPCTape *t) { return t ? t->name : ""; }
double tape_position_seconds(const CPCTape *t) { return t ? t->elapsed_us / 1e6 : 0; }
bool tape_is_audio(const CPCTape *t) { return t && t->kind == KIND_AUDIO; }
double tape_length_seconds(const CPCTape *t) { return (t && t->kind == KIND_AUDIO) ? t->nsamples / t->rate : 0; }

void tape_seek_block(CPCTape *t, int block) {
    if (!t || t->kind != KIND_TZX) return;
    if (block < 0) block = 0;
    if (block >= t->nblocks) block = t->nblocks - 1;
    t->cur = block; t->ended = false; t->call_sp = 0; load_block(t);
}

void tape_tick(CPCTape *t, int us) {
    if (!t || !t->playing || !t->motor) return;
    t->elapsed_us += us;
    if (t->kind == KIND_AUDIO) {
        t->pos += us * t->rate / 1e6;
        size_t i = (size_t)t->pos;
        if (i >= t->nsamples) { t->playing = false; t->ended = true; return; }
        int16_t v = t->samples[i];
        if (v > 3000) t->aud_level = true; else if (v < -3000) t->aud_level = false;
        return;
    }
    double budget = us;
    while (budget > 0 && t->playing) {
        if (t->pidx < t->npulses) {
            if (t->rem <= 0) t->rem = t->pulses[t->pidx] * TSTATE_US;
            if (t->rem > budget) { t->rem -= budget; return; }
            budget -= t->rem; t->rem = 0; t->pidx++; t->level = !t->level;
        } else if (t->pause_rem > 0) {
            t->level = false;
            if (t->pause_rem > budget) { t->pause_rem -= budget; return; }
            budget -= t->pause_rem; t->pause_rem = 0;
        } else {
            next_block(t);
            if (t->ended) return;
        }
    }
}

/* ---------------- state snapshot ---------------- */
/* Playback position only; the tape itself is media. `size` and `nblocks` identify the tape the state was
 * taken with. Returns false (READ mode) when the inserted tape is a different one; the stream still parses. */
static bool finite_nonneg(double v) { return isfinite(v) && v >= 0; }

/* What playback relies on: block indices (cur, loop_start, call stack) inside the tape, pidx inside the pulse
 * list of the current block, finite non-negative times. A state that breaks one (it can come from an API
 * client) is rejected. The pulse count of block `cur` is found on a clone, so checking changes nothing. */
static bool tape_consistent(const CPCTape *t, const CPCTape *v) {
    if (v->cur < 0 || v->cur > t->nblocks || v->loop_start < 0 || v->loop_start > t->nblocks) return false;
    for (int i = 0; i < 16; i++) if (v->call_stack[i] < 0 || v->call_stack[i] > t->nblocks) return false;
    if (!finite_nonneg(v->rem) || !finite_nonneg(v->pause_rem) || !finite_nonneg(v->elapsed_us) || !finite_nonneg(v->pos)) return false;
    if (t->kind != KIND_TZX) return true;
    CPCTape *c = tape_clone(t); if (!c) return false;
    c->cur = v->cur; load_block(c);
    bool ok = v->pidx >= 0 && v->pidx <= c->npulses;
    tape_free(c);
    return ok;
}

bool tape_state_visit(Ser *s, CPCTape *t) {
    CPCTape none; memset(&none, 0, sizeof none);
    bool real = t != NULL; if (!real) t = &none;          /* NULL: parse and discard (no tape inserted) */
    ser_tag(s, "TAP0");
    uint64_t size = ser_le(s, (uint64_t)t->size, 8);
    int32_t nblocks = (int32_t)(uint32_t)ser_le(s, (uint32_t)t->nblocks, 4);
    bool same = real && size == (uint64_t)t->size && nblocks == t->nblocks;
    /* Reading parses into a scratch copy (also in CHECK mode), validated before READ applies it. */
    CPCTape tmp = *t, *d = ser_reading(s) ? &tmp : t;
    SerMode mode = s->mode;
    if (ser_reading(s)) s->mode = SER_READ;
    ser_int(s, &d->cur); ser_int(s, &d->pidx); ser_int(s, &d->loop_start); ser_int(s, &d->loop_count);
    ser_ints(s, d->call_stack, 16); ser_int_in(s, &d->call_sp, 0, 16);
    ser_f64(s, &d->rem); ser_f64(s, &d->pause_rem); ser_f64(s, &d->elapsed_us); ser_f64(s, &d->pos);
    ser_bool(s, &d->level); ser_bool(s, &d->playing); ser_bool(s, &d->motor); ser_bool(s, &d->ended); ser_bool(s, &d->aud_level);
    s->mode = mode;
    /* A different tape's position is parsed and dropped (reported as a hazard), so it is not validated. */
    if (ser_reading(s) && same && !s->bad && !tape_consistent(t, &tmp)) s->bad = true;
    if (ser_storing(s) && same && !s->bad) {
        t->cur = tmp.cur; t->loop_start = tmp.loop_start; t->loop_count = tmp.loop_count; t->call_sp = tmp.call_sp;
        memcpy(t->call_stack, tmp.call_stack, sizeof t->call_stack);
        t->elapsed_us = tmp.elapsed_us; t->pos = tmp.pos; t->motor = tmp.motor; t->aud_level = tmp.aud_level;
        if (t->kind == KIND_TZX) load_block(t);           /* pulses are derived from the current block */
        t->pidx = tmp.pidx; t->rem = tmp.rem; t->pause_rem = tmp.pause_rem;
        t->level = tmp.level; t->playing = tmp.playing; t->ended = tmp.ended;   /* load_block may have changed them */
    }
    return same;
}

CPCTape *tape_clone(const CPCTape *t) { return t ? tape_load(t->data, t->size, t->name) : NULL; }
