#include "debug.h"
#include <stdlib.h>
#include <string.h>

/* Memory access map and function profiler. Both only observe: they never change what the machine does, and they
 * stay silent while the debugger re-simulates (seeking, reverse debugging). */

/* ---------------- access map ---------------- */
void dbg_access_enable(CPC *c, bool on) {
    Debugger *d = dbg_attach(c);
    if (on && !d->acc_store) d->acc_store = calloc(1, sizeof(DbgAccess));
    d->acc = on ? d->acc_store : NULL;
    dbg_rebuild_watch(d);
}
void dbg_access_clear(CPC *c) { Debugger *d = c->dbg; if (d && d->acc_store) memset(d->acc_store, 0, sizeof(DbgAccess)); }
void dbg_access_copy(const Debugger *d, int kind, uint32_t *out) {
    const DbgAccess *a = d->acc_store;
    if (!a) { memset(out, 0, 65536 * sizeof *out); return; }
    memcpy(out, kind == 0 ? a->r : kind == 1 ? a->w : a->x, 65536 * sizeof *out);
}
void dbg_access_fetch(Debugger *d, uint16_t addr) { if (!d->quiet && !d->scanning) d->acc->x[addr]++; }

/* ---------------- profiler ---------------- */
#define PROF_KEYS (DBG_PROF_HALT + 1)
#define PROF_DEPTH 256
typedef struct { uint16_t callee, ret; uint64_t t0; } Frame;
struct DbgProfile {
    uint64_t total, frames, last_frames;
    uint64_t self[PROF_KEYS], incl[PROF_KEYS]; uint32_t calls[PROF_KEYS];
    uint32_t hot[65536];
    Frame st[PROF_DEPTH]; int depth;
};

void dbg_prof_enable(CPC *c, bool on) {
    Debugger *d = dbg_attach(c);
    if (on && !d->prof_store) d->prof_store = calloc(1, sizeof(struct DbgProfile));
    if (on) { d->prof_store->last_frames = c->frames; d->prof_store->depth = 0; }   /* the shadow stack is not valid after a pause */
    d->prof = on ? d->prof_store : NULL;
}
void dbg_prof_clear(CPC *c) {
    Debugger *d = c->dbg; if (!d || !d->prof_store) return;
    memset(d->prof_store, 0, sizeof *d->prof_store); d->prof_store->last_frames = c->frames;
}
bool dbg_prof_running(const Debugger *d) { return d->prof != NULL; }
uint64_t dbg_prof_total(const Debugger *d) { return d->prof_store ? d->prof_store->total : 0; }
uint64_t dbg_prof_frames(const Debugger *d) { return d->prof_store ? d->prof_store->frames : 0; }

static bool is_call(uint8_t op) { return op == 0xCD || (op & 0xC7) == 0xC4; }
static bool is_rst(uint8_t op) { return (op & 0xC7) == 0xC7; }
static bool is_ret(uint8_t op, uint8_t op2) { return op == 0xC9 || (op & 0xC7) == 0xC0 || (op == 0xED && (op2 & 0xC7) == 0x45); }

void dbg_prof_before(CPC *c, DbgProfPre *pre) {
    const Z80 *z = &c->z80;
    pre->pc = z->pc; pre->sp = z->sp.w; pre->halted = z->halted; pre->nmi = z->nmi_pending;
    pre->op = cpc_peek(c, z->pc); pre->op2 = cpc_peek(c, (uint16_t)(z->pc + 1));
}

static void push_frame(struct DbgProfile *p, uint16_t callee, uint16_t ret) {
    if (p->depth == PROF_DEPTH) { memmove(&p->st[0], &p->st[1], (PROF_DEPTH - 1) * sizeof(Frame)); p->depth--; }
    p->st[p->depth++] = (Frame){callee, ret, p->total};
    p->calls[callee]++;
}

void dbg_prof_after(CPC *c, const DbgProfPre *pre, int us) {
    struct DbgProfile *p = c->dbg->prof; const Z80 *z = &c->z80;
    p->total += (uint64_t)us;
    if (c->frames > p->last_frames) p->frames += c->frames - p->last_frames;
    p->last_frames = c->frames;
    int key = pre->halted ? DBG_PROF_HALT : p->depth ? p->st[p->depth - 1].callee : DBG_PROF_TOP;
    p->self[key] += (uint64_t)us;
    if (!pre->halted) p->hot[pre->pc] += (uint32_t)us;
    bool pushed = z->sp.w == (uint16_t)(pre->sp - 2), popped = z->sp.w == (uint16_t)(pre->sp + 2);
    if (z->int_ack || (pre->nmi && z->pc == 0x66)) { if (pushed) push_frame(p, z->pc, pre->pc); return; }   /* an interrupt is a call */
    if (pre->halted) return;
    if (pushed && (is_call(pre->op) || is_rst(pre->op))) { push_frame(p, z->pc, (uint16_t)(pre->pc + (is_rst(pre->op) ? 1 : 3))); return; }
    if (popped && is_ret(pre->op, pre->op2)) {
        int lowest = p->depth > 8 ? p->depth - 8 : 0;
        for (int k = p->depth - 1; k >= lowest; k--) {
            if (p->st[k].ret != z->pc) continue;
            for (int j = p->depth - 1; j >= k; j--) p->incl[p->st[j].callee] += p->total - p->st[j].t0;   /* closes the frames it unwound too */
            p->depth = k; return;
        }
    }
}

int dbg_prof_entries(const Debugger *d, DbgProfEntry *out, int max) {
    const struct DbgProfile *p = d->prof_store; if (!p) return 0;
    int n = 0;
    for (int k = 0; k < PROF_KEYS && n < max; k++) {
        if (!p->self[k] && !p->incl[k] && !p->calls[k]) continue;
        out[n++] = (DbgProfEntry){k, p->calls[k], p->self[k], p->incl[k]};
    }
    for (int j = 0; j < p->depth; j++) {                                    /* calls still open: count their time so far */
        uint64_t dt = p->total - p->st[j].t0; int i = 0;
        while (i < n && out[i].addr != p->st[j].callee) i++;
        if (i == n) { if (n >= max) continue; out[n++] = (DbgProfEntry){p->st[j].callee, p->calls[p->st[j].callee], 0, 0}; }
        out[i].incl += dt;
    }
    return n;
}

static const uint32_t *hot_key;
static int hot_cmp(const void *a, const void *b) { uint32_t x = hot_key[*(const uint16_t *)a], y = hot_key[*(const uint16_t *)b]; return x < y ? 1 : x > y ? -1 : 0; }
int dbg_prof_hotspots(const Debugger *d, uint16_t *addr, uint32_t *own, int max) {
    const struct DbgProfile *p = d->prof_store; if (!p) return 0;
    uint16_t *idx = malloc(65536 * sizeof *idx); int n = 0;
    for (int a = 0; a < 65536; a++) if (p->hot[a]) idx[n++] = (uint16_t)a;
    hot_key = p->hot; qsort(idx, (size_t)n, sizeof *idx, hot_cmp);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) { addr[i] = idx[i]; own[i] = p->hot[idx[i]]; }
    free(idx); return n;
}
