#include "fdc.h"
#include "cpc.h"
#include <stdlib.h>
#include <string.h>
#include "ser.h"

#define REV_US 200000

enum { S_IDLE, S_CMD, S_WAIT, S_READ, S_WRITE, S_RESULT };

struct FDC {
    CPC *cpc;
    int state;
    uint8_t cmd[9]; int cmd_len, cmd_pos;
    uint8_t res[8]; int res_len, res_pos;
    uint8_t pcn[4];
    uint8_t int_st0[4]; bool int_pending[4]; int int_order[4], int_count;
    int seek_timer[4], seek_target[4]; bool seeking[4];
    bool motor;
    int rot_us;
    CPCDisc *disc[4];
    int unit, head;
    /* execution */
    int timer;                    /* wait before next byte/sector */
    uint8_t *buf; int buf_len, buf_pos;
    CPCSector *cur; CPCTrack *curtrk;
    bool rqm, dio, exec;
    int cur_r, eot, mt, sk, deleted_cmd, phys_track, cmd_c, cmd_h, cmd_n;
    uint8_t st0, st1, st2;
    int fmt_count, fmt_filler, fmt_gap, fmt_n, fmt_sc; uint8_t fmt_ids[4]; int fmt_id_pos;
    CPCTrack *fmt_track;
    bool write_mode;
    int specify[2];
};

FDC *fdc_create(CPC *cpc) { FDC *f = calloc(1, sizeof *f); f->cpc = cpc; fdc_reset(f); return f; }
void fdc_destroy(FDC *f) { if (f) { free(f->buf); free(f); } }

void fdc_reset(FDC *f) {
    f->state = S_IDLE; f->cmd_pos = f->cmd_len = 0; f->res_len = f->res_pos = 0;
    memset(f->pcn, 0, sizeof f->pcn); memset(f->int_pending, 0, sizeof f->int_pending);
    memset(f->seeking, 0, sizeof f->seeking); f->int_count = 0;
    f->rqm = true; f->dio = false; f->exec = false; f->motor = false;
}

void fdc_insert(FDC *f, int drive, CPCDisc *d) { f->disc[drive & 3] = d; }
CPCDisc *fdc_disc(FDC *f, int drive) { return f->disc[drive & 3]; }
int fdc_track(FDC *f, int drive) { return f->pcn[drive & 3]; }
bool fdc_motor(FDC *f) { return f->motor; }
bool fdc_busy(FDC *f) { return f->state != S_IDLE; }
void fdc_set_motor(FDC *f, bool on) { f->motor = on; }

uint8_t fdc_read_status(FDC *f) {
    uint8_t v = 0;
    if (f->rqm) v |= 0x80;
    if (f->dio) v |= 0x40;
    if (f->exec) v |= 0x20;
    if (f->state != S_IDLE) v |= 0x10;
    for (int i = 0; i < 4; i++) if (f->seeking[i]) v |= 1 << i;
    return v;
}

static int cmd_length(uint8_t c) {
    switch (c & 0x1F) {
    case 0x03: return 3; case 0x04: return 2; case 0x07: return 2; case 0x08: return 1; case 0x0F: return 3;
    case 0x0A: return 2; case 0x06: case 0x0C: case 0x05: case 0x09: case 0x02: return 9;
    case 0x0D: return 6; case 0x11: case 0x19: case 0x1D: return 9;
    default: return 1;
    }
}

static CPCTrack *cur_track(FDC *f, int unit, int head) {
    CPCDisc *d = f->disc[unit]; if (!d) return NULL;
    int t = f->pcn[unit]; if (t >= DSK_MAX_TRACKS) return NULL;
    if (d->sides < 1) return NULL;
    if (head >= d->sides) { if (!f->cpc->single_sided_read[unit]) return NULL; head = 0; }
    return &d->trk[t][head];
}


static void begin_result(FDC *f, int len) {
    f->state = S_RESULT; f->res_len = len; f->res_pos = 0; f->rqm = true; f->dio = true; f->exec = false;
}

static void end_rw(FDC *f, uint8_t st0, uint8_t st1, uint8_t st2, uint8_t c, uint8_t h, uint8_t r, uint8_t n) {
    f->res[0] = st0 | f->unit | (f->head << 2); f->res[1] = st1; f->res[2] = st2;
    f->res[3] = c; f->res[4] = h; f->res[5] = r; f->res[6] = n;
    begin_result(f, 7);
}

static void finish_idle(FDC *f) { f->state = S_IDLE; f->rqm = true; f->dio = false; f->exec = false; }

/* Time until the ID of sector `idx` reaches the head */
static int sector_wait(FDC *f, CPCTrack *t, int idx) {
    if (t->nsectors == 0) return 0;
    int slot = REV_US / t->nsectors;
    int start = idx * slot;
    int d = start - f->rot_us; if (d < 0) d += REV_US;
    return f->cpc->cfg.fast_disc ? d / 8 : d;
}

static int find_sector(CPCTrack *t, int c, int h, int r, int n, bool use_n, int *wrong_c) {
    int found = -1;
    for (int i = 0; i < t->nsectors; i++) {
        CPCSector *s = &t->sec[i];
        if (s->r == r && s->h == h && (!use_n || s->n == n)) {
            if (s->c == c) return i;
            if (found < 0) { found = i; *wrong_c = 1; }
        }
    }
    return -1;
}

static void start_exec_sector(FDC *f);

static void start_read_write(FDC *f, bool write, bool deleted) {
    uint8_t *c = f->cmd;
    f->unit = c[1] & 3; f->head = (c[1] >> 2) & 1;
    f->mt = (c[0] >> 7) & 1; f->sk = (c[0] >> 5) & 1; f->deleted_cmd = deleted;
    f->cmd_c = c[2]; f->cmd_h = c[3]; f->cur_r = c[4]; f->cmd_n = c[5]; f->eot = c[6];
    f->write_mode = write;
    if (!f->disc[f->unit] || !f->motor) {
        end_rw(f, 0x48, 0, 0, f->cmd_c, f->cmd_h, f->cur_r, f->cmd_n); return;
    }
    if (write && f->disc[f->unit]->write_protect && !f->disc[f->unit]->temp_writes) {
        end_rw(f, 0x40, 0x02, 0, f->cmd_c, f->cmd_h, f->cur_r, f->cmd_n); return;
    }
    start_exec_sector(f);
}

static void start_exec_sector(FDC *f) {
    CPCTrack *t = cur_track(f, f->unit, f->head);
    if (!t || t->nsectors == 0) {
        end_rw(f, 0x40, 0x05, 0, f->cmd_c, f->cmd_h, f->cur_r, f->cmd_n); return;
    }
    int wrong = 0;
    int idx = find_sector(t, f->cmd_c, f->cmd_h, f->cur_r, f->cmd_n, true, &wrong);
    if (idx < 0) {
        uint8_t st2 = wrong ? 0x10 : 0;
        end_rw(f, 0x40, 0x04, st2, f->cmd_c, f->cmd_h, f->cur_r, f->cmd_n); return;
    }
    f->curtrk = t; f->cur = &t->sec[idx];
    f->st1 = 0; f->st2 = 0;
    f->timer = sector_wait(f, t, idx) + 16;
    f->state = S_WAIT; f->rqm = false; f->exec = true; f->dio = !f->write_mode;
}

static void begin_transfer(FDC *f) {
    CPCSector *s = f->cur;
    int len = 128 << (f->cmd_n & 7);
    if (f->cmd_n == 0) len = f->cmd[8] < 128 ? (f->cmd[8] ? f->cmd[8] : 128) : 128;
    if (len > s->len && !f->write_mode) len = s->len;
    free(f->buf); f->buf = calloc(1, len ? len : 1); f->buf_len = len; f->buf_pos = 0;
    if (!f->write_mode) {
        memcpy(f->buf, s->data, len < s->len ? len : s->len);
        bool del = (s->st2 & 0x40) != 0;
        if (del != f->deleted_cmd) {
            f->st2 |= 0x40;
            if (f->sk) { /* skip sector */ f->state = S_IDLE; }
        }
        if (s->st1 & 0x20) { f->st1 |= 0x20; f->st2 |= (s->st2 & 0x20); }
        f->state = S_READ; f->timer = f->cpc->cfg.fast_disc ? 4 : 32; f->rqm = false;
    } else {
        f->state = S_WRITE; f->rqm = true; f->timer = 0;
    }
}

static void sector_done(FDC *f) {
    uint8_t st0 = 0;
    CPCSector *s = f->cur;
    bool stop = (f->st1 & 0x20) || (f->st2 & 0x40);
    if (f->write_mode) {
        dsk_touch(f->disc[f->unit], f->curtrk);
        free(s->data); int len = f->buf_len; s->data = malloc(len); memcpy(s->data, f->buf, len); s->len = len;
        if (f->deleted_cmd) s->st2 |= 0x40; else s->st2 &= ~0x40;
        f->disc[f->unit]->dirty = true; f->disc[f->unit]->writes++;
    }
    if (stop) { st0 = 0x40; end_rw(f, st0, f->st1, f->st2, s->c, s->h, s->r, s->n); return; }
    if (f->cur_r == f->eot) {
        if (f->mt && f->head == 0 && f->disc[f->unit]->sides > 1) {
            f->head = 1; f->cmd_h ^= 1; f->cur_r = 1; start_exec_sector(f); return;
        }
        end_rw(f, 0x40, f->st1 | 0x80, f->st2, s->c, s->h, s->r + 1, s->n); return;
    }
    f->cur_r++;
    start_exec_sector(f);
}

static void do_command(FDC *f) {
    uint8_t *c = f->cmd;
    int op = c[0] & 0x1F;
    switch (op) {
    case 0x03: f->specify[0] = c[1]; f->specify[1] = c[2]; finish_idle(f); break;
    case 0x04: {
        int u = c[1] & 3, h = (c[1] >> 2) & 1;
        CPCDisc *d = f->disc[u];
        uint8_t st3 = u | (h << 2);
        if (d && d->sides > 1) st3 |= 0x08;
        if (f->pcn[u] == 0) st3 |= 0x10;
        if (d && f->motor) st3 |= 0x20;
        if (d && d->write_protect) st3 |= 0x40;
        f->res[0] = st3; begin_result(f, 1); break; }
    case 0x07: {
        int u = c[1] & 3;
        f->seeking[u] = true; f->seek_target[u] = 0; f->seek_timer[u] = f->cpc->cfg.fast_disc ? 2000 : 12000 * (f->pcn[u] ? f->pcn[u] : 1);
        finish_idle(f); break; }
    case 0x0F: {
        int u = c[1] & 3;
        f->seeking[u] = true; f->seek_target[u] = c[2];
        int dist = abs((int)c[2] - f->pcn[u]);
        f->seek_timer[u] = f->cpc->cfg.fast_disc ? 400 * (dist + 1) : 12000 * (dist + 1);
        finish_idle(f); break; }
    case 0x08: {
        for (int i = 0; i < 4; i++) if (f->int_pending[i]) {
            f->res[0] = f->int_st0[i]; f->res[1] = f->pcn[i]; f->int_pending[i] = false; begin_result(f, 2); return;
        }
        f->res[0] = 0x80; begin_result(f, 1); break; }
    case 0x0A: {
        int u = c[1] & 3, h = (c[1] >> 2) & 1; f->unit = u; f->head = h;
        CPCTrack *t = cur_track(f, u, h);
        if (!f->disc[u] || !f->motor || !t || t->nsectors == 0) { f->res[0] = 0x40 | 0x08 * (f->disc[u] == NULL) | u | (h << 2); f->res[1] = 0x01; f->res[2] = 0; memset(f->res + 3, 0, 4); begin_result(f, 7); break; }
        int idx = 0;
        /* next sector ID to pass under the head */
        int slot = REV_US / t->nsectors; idx = ((f->rot_us + slot - 1) / slot) % t->nsectors;
        CPCSector *s = &t->sec[idx];
        f->cur = s; f->curtrk = t; f->timer = sector_wait(f, t, idx) + 20; f->state = S_WAIT; f->exec = true; f->rqm = false;
        f->cmd[0] = 0x0A | 0x80; /* mark for completion */
        break; }
    case 0x06: start_read_write(f, false, false); break;
    case 0x0C: start_read_write(f, false, true); break;
    case 0x05: start_read_write(f, true, false); break;
    case 0x09: start_read_write(f, true, true); break;
    case 0x02: start_read_write(f, false, false); break;
    case 0x0D: {
        f->unit = c[1] & 3; f->head = (c[1] >> 2) & 1;
        f->fmt_n = c[2]; f->fmt_sc = c[3]; f->fmt_gap = c[4]; f->fmt_filler = c[5];
        CPCDisc *d = f->disc[f->unit];
        if (!d || !f->motor) { end_rw(f, 0x48, 0, 0, 0, 0, 0, f->fmt_n); break; }
        if (d->write_protect && !d->temp_writes) { end_rw(f, 0x40, 0x02, 0, 0, 0, 0, f->fmt_n); break; }
        CPCTrack *t = cur_track(f, f->unit, f->head);
        if (!t) { end_rw(f, 0x40, 0x01, 0, 0, 0, 0, f->fmt_n); break; }
        dsk_touch(d, t);
        dsk_track_clear(t);
        t->size_code = f->fmt_n; t->gap3 = f->fmt_gap; t->filler = f->fmt_filler; t->nsectors = f->fmt_sc > 32 ? 32 : f->fmt_sc;
        f->fmt_track = t; f->fmt_count = 0; f->fmt_id_pos = 0;
        f->state = S_WRITE; f->write_mode = true; f->rqm = true; f->dio = false; f->exec = true; f->timer = 0;
        f->cmd[0] = 0x0D | 0x80;
        if (d->sides < f->head + 1) d->sides = f->head + 1;
        if (d->tracks < f->pcn[f->unit] + 1) d->tracks = f->pcn[f->unit] + 1;
        break; }
    default:
        f->res[0] = 0x80; begin_result(f, 1);
    }
}

uint8_t fdc_read_data(FDC *f) {
    if (f->state == S_RESULT) {
        uint8_t v = f->res[f->res_pos++];
        if (f->res_pos >= f->res_len) finish_idle(f);
        return v;
    }
    if (f->state == S_READ && f->rqm) {
        uint8_t v = f->buf[f->buf_pos++];
        f->rqm = false; f->timer = f->cpc->cfg.fast_disc ? 4 : 32;
        if (f->buf_pos >= f->buf_len) { f->timer = 0; f->rqm = false; sector_done(f); }
        return v;
    }
    return 0xFF;
}

void fdc_write_data(FDC *f, uint8_t v) {
    if (f->state == S_IDLE || f->state == S_CMD) {
        if (f->state == S_IDLE) { f->cmd_pos = 0; f->cmd_len = cmd_length(v); f->state = S_CMD; f->dio = false; }
        f->cmd[f->cmd_pos++] = v;
        if (f->cmd_pos >= f->cmd_len) { f->state = S_IDLE; do_command(f); }
        return;
    }
    if (f->state == S_WRITE && f->rqm) {
        if (f->cmd[0] == (0x0D | 0x80)) {
            f->fmt_ids[f->fmt_id_pos++] = v;
            if (f->fmt_id_pos == 4) {
                f->fmt_id_pos = 0;
                CPCTrack *t = f->fmt_track; int i = f->fmt_count++;
                if (i < t->nsectors) {
                    CPCSector *s = &t->sec[i];
                    s->c = f->fmt_ids[0]; s->h = f->fmt_ids[1]; s->r = f->fmt_ids[2]; s->n = f->fmt_ids[3];
                    /* free first: a track restored mid-format from a savestate already has sector buffers */
                    s->len = 128 << (s->n & 7); free(s->data); s->data = malloc(s->len); memset(s->data, f->fmt_filler, s->len);
                }
                f->rqm = false; f->timer = f->cpc->cfg.fast_disc ? 200 : 6000;
                if (f->fmt_count >= f->fmt_sc) {
                    f->disc[f->unit]->dirty = true; f->disc[f->unit]->writes++;
                    f->timer = f->cpc->cfg.fast_disc ? 400 : 12000; f->fmt_count = 1000;
                }
            }
            return;
        }
        f->buf[f->buf_pos++] = v; f->rqm = false; f->timer = f->cpc->cfg.fast_disc ? 4 : 32;
        if (f->buf_pos >= f->buf_len) { f->timer = 0; sector_done(f); }
    }
}

static void fdc_tick_one(FDC *f, int us) {
    f->rot_us = (f->rot_us + us) % REV_US;
    for (int i = 0; i < 4; i++) if (f->seeking[i]) {
        if ((f->seek_timer[i] -= us) <= 0) {
            f->seeking[i] = false; f->pcn[i] = f->seek_target[i];
            f->int_st0[i] = 0x20 | i | (f->disc[i] ? 0 : 0x08);
            f->int_pending[i] = true;
        }
    }
    if (f->state == S_WAIT) {
        if ((f->timer -= us) <= 0) {
            if (f->cmd[0] == (0x0A | 0x80)) {
                CPCSector *s = f->cur;
                f->res[0] = f->unit | (f->head << 2); f->res[1] = 0; f->res[2] = 0;
                f->res[3] = s->c; f->res[4] = s->h; f->res[5] = s->r; f->res[6] = s->n; begin_result(f, 7);
            } else begin_transfer(f);
        }
    } else if (f->state == S_READ && !f->rqm) {
        if ((f->timer -= us) <= 0) f->rqm = true;
    } else if (f->state == S_WRITE && f->cmd[0] == (0x0D | 0x80)) {
        if (!f->rqm && (f->timer -= us) <= 0) {
            if (f->fmt_count >= 1000) {
                f->res[0] = f->unit | (f->head << 2); f->res[1] = 0; f->res[2] = 0;
                f->res[3] = 0; f->res[4] = 0; f->res[5] = 0; f->res[6] = f->fmt_n; begin_result(f, 7);
            } else f->rqm = true;
        }
    } else if (f->state == S_WRITE && !f->rqm) {
        if ((f->timer -= us) <= 0) f->rqm = true;
    }
}

/* Advance long idle spans without repeating the controller's four seek checks and state
 * dispatch for every CPU T-state. Once a command or seek is active, keep the original
 * one-microsecond transitions so crossings and newly-started timers remain byte-for-byte equal. */
void fdc_tick(FDC *f, int us) {
    if (!f || us <= 0) return;
    for (int drive = 0; drive < 4; drive++) if (f->seeking[drive]) goto active;
    if (f->state == S_IDLE) { f->rot_us = (f->rot_us + us) % REV_US; return; }
active:
    while (us-- > 0) {
        bool seeking = false;
        for (int drive = 0; drive < 4; drive++) seeking |= f->seeking[drive];
        if (f->state == S_IDLE && !seeking) { f->rot_us = (f->rot_us + us + 1) % REV_US; return; }
        fdc_tick_one(f, 1);
    }
}

/* ---------------- state snapshot ---------------- */
/* Discs themselves are media, not state. Pointers into them (cur, curtrk, fmt_track) are stored as
 * (unit, track index, sector index); unit -1 means none. */
typedef struct { int32_t unit, trk, sec; } FdcRef;

static FdcRef ref_of(const FDC *f, const CPCTrack *t, const CPCSector *s) {
    FdcRef r = {-1, 0, 0};
    if (!t) return r;
    for (int u = 0; u < 4; u++) {
        const CPCDisc *d = f->disc[u];
        if (d && (const char *)t >= (const char *)d && (const char *)t < (const char *)(d + 1)) {
            r.unit = u; r.trk = (int32_t)(t - &d->trk[0][0]); r.sec = s ? (int32_t)(s - t->sec) : 0; return r;
        }
    }
    return r;
}
static CPCTrack *track_of(const FDC *f, FdcRef r) {
    if (r.unit < 0 || r.unit > 3 || !f->disc[r.unit] || r.trk < 0 || r.trk >= DSK_MAX_TRACKS * 2) return NULL;
    return &f->disc[r.unit]->trk[0][0] + r.trk;
}
static void ser_ref(Ser *s, FdcRef *r) { ser_i32(s, &r->unit); ser_i32(s, &r->trk); ser_i32(s, &r->sec); }

/* What the controller code relies on: it indexes cmd/res/fmt_ids/buf by their positions and dereferences
 * cur, curtrk, fmt_track and disc[unit] according to the state. A state that breaks any of these (it can come
 * from an API client) is rejected, so restoring it cannot make a later byte transfer crash. */
static bool fdc_consistent(const FDC *f, FdcRef rc, bool has_buf) {
    if (f->state < S_IDLE || f->state > S_RESULT || f->unit < 0 || f->unit > 3 || f->head < 0 || f->head > 1) return false;
    if (f->cmd_len < 0 || f->cmd_len > 9 || f->cmd_pos < 0 || f->cmd_pos > 9) return false;
    if (f->res_len < 0 || f->res_len > 8 || f->res_pos < 0 || f->res_pos > 8) return false;
    if (f->int_count < 0 || f->int_count > 4 || f->fmt_id_pos < 0 || f->fmt_id_pos > 3) return false;
    /* fmt_count indexes fmt_track->sec[] (bounded above by nsectors, not below): a negative value would write
     * into the neighbouring track inside the same CPCDisc, which no sanitizer sees. 1000 marks "all written". */
    if ((f->fmt_count < 0 || f->fmt_count > 255) && f->fmt_count != 1000) return false;
    /* Counters the tick adds to or subtracts from: the disc angle stays below one revolution, timers stay
     * far from int overflow (real ones are at most a few revolutions; they go negative by one tick at most). */
    if (f->rot_us < 0 || f->rot_us >= REV_US || f->timer < -10 * REV_US || f->timer > 10 * REV_US) return false;
    for (int i = 0; i < 4; i++) if (f->seek_timer[i] < -10 * REV_US || f->seek_timer[i] > 10 * REV_US) return false;
    if (f->state == S_CMD && f->cmd_pos >= f->cmd_len) return false;          /* the next byte goes to cmd[cmd_pos] */
    if (f->state == S_RESULT && f->res_pos >= f->res_len) return false;
    bool format = f->cmd[0] == (0x0D | 0x80);
    bool sector = f->state == S_WAIT || f->state == S_READ || (f->state == S_WRITE && !format);
    if (sector && (!f->cur || !f->curtrk || rc.unit != f->unit || !f->disc[f->unit])) return false;
    if ((f->state == S_READ || (f->state == S_WRITE && !format)) && (!has_buf || f->buf_pos >= f->buf_len)) return false;
    if (f->state == S_WRITE && format && (!f->fmt_track || !f->disc[f->unit])) return false;
    return true;
}

static void fdc_fields(Ser *s, FDC *f) {
    ser_int(s, &f->state);
    ser_bytes(s, f->cmd, sizeof f->cmd); ser_int(s, &f->cmd_len); ser_int(s, &f->cmd_pos);
    ser_bytes(s, f->res, sizeof f->res); ser_int(s, &f->res_len); ser_int(s, &f->res_pos);
    ser_bytes(s, f->pcn, sizeof f->pcn);
    ser_bytes(s, f->int_st0, sizeof f->int_st0); ser_bools(s, f->int_pending, 4); ser_ints(s, f->int_order, 4); ser_int(s, &f->int_count);
    ser_ints(s, f->seek_timer, 4); ser_ints(s, f->seek_target, 4); ser_bools(s, f->seeking, 4);
    ser_bool(s, &f->motor);
    ser_int(s, &f->rot_us);
    ser_int(s, &f->unit); ser_int(s, &f->head);
    ser_int(s, &f->timer);
    ser_bool(s, &f->rqm); ser_bool(s, &f->dio); ser_bool(s, &f->exec);
    ser_int(s, &f->cur_r); ser_int(s, &f->eot); ser_int(s, &f->mt); ser_int(s, &f->sk); ser_int(s, &f->deleted_cmd);
    ser_int(s, &f->phys_track); ser_int(s, &f->cmd_c); ser_int(s, &f->cmd_h); ser_int(s, &f->cmd_n);
    ser_u8(s, &f->st0); ser_u8(s, &f->st1); ser_u8(s, &f->st2);
    ser_int(s, &f->fmt_count); ser_int(s, &f->fmt_filler); ser_int(s, &f->fmt_gap); ser_int(s, &f->fmt_n); ser_int(s, &f->fmt_sc);
    ser_bytes(s, f->fmt_ids, sizeof f->fmt_ids); ser_int(s, &f->fmt_id_pos);
    ser_bool(s, &f->write_mode);
    ser_ints(s, f->specify, 2);
}

void fdc_state_visit(Ser *s, FDC *f) {
    ser_tag(s, "FDC0");
    /* Reading (CHECK or READ) parses into a scratch copy so the invariants can be checked on the values the
     * stream holds before anything is applied; READ commits the copy at the end. */
    FDC tmp = *f; FDC *d = ser_reading(s) ? &tmp : f;
    SerMode mode = s->mode;
    if (ser_reading(s)) s->mode = SER_READ;                 /* the scratch copy may be written in CHECK too */
    fdc_fields(s, d);
    FdcRef rc = ref_of(f, f->curtrk, f->cur), rf = ref_of(f, f->fmt_track, NULL);
    ser_ref(s, &rc); ser_ref(s, &rf);
    s->mode = mode;
    /* The sector buffer: present flag, length (the allocation is max(len, 1)), position, bytes. */
    bool has = ser_flag(s, f->buf != NULL);
    uint32_t n = ser_count(s, (uint32_t)f->buf_len, 1u << 20);
    uint32_t pos = ser_count(s, (uint32_t)f->buf_pos, 1u << 20);
    uint8_t *nb = NULL;
    if (ser_storing(s) && has) { nb = calloc(1, n ? n : 1); if (!nb) { s->bad = true; return; } }
    ser_bytes(s, ser_storing(s) ? nb : f->buf, has ? n : 0);
    if (ser_reading(s) && !s->bad) {
        d->buf_len = (int)n; d->buf_pos = (int)pos; d->buf = NULL;
        d->curtrk = track_of(f, rc); d->cur = d->curtrk && rc.sec >= 0 && rc.sec < DSK_MAX_SECTORS ? &d->curtrk->sec[rc.sec] : NULL;
        d->fmt_track = track_of(f, rf);
        if (!fdc_consistent(d, rc, has)) s->bad = true;
    }
    if (ser_storing(s) && !s->bad) { free(f->buf); *f = tmp; f->buf = nb; }
    else free(nb);
}
