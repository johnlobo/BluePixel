#include "debug.h"
#include "replay.h"
#include "fdc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { W_READ = 1, W_WRITE = 2, W_CODE = 4 };

Debugger *dbg_attach(CPC *c) {
    if (c->dbg) return c->dbg;
    Debugger *d = calloc(1, sizeof *d);
    d->active = true; d->temp_bp = -1; d->hist_on = true;
    c->dbg = d;
    c->z80.brk_enabled = d->break_instructions;
    return d;
}

void dbg_detach(CPC *c) {
    if (!c->dbg) return;
    free(c->dbg->syms); free(c->dbg->prof_store); free(c->dbg->acc_store); free(c->dbg); c->dbg = NULL; c->z80.brk_enabled = false;
}

bool cpc_dbg_break(const CPC *c) { return c->dbg && c->dbg->hit; }

void dbg_rebuild_watch(Debugger *d) {
    memset(d->watch, d->acc ? W_READ | W_WRITE : 0, sizeof d->watch);     /* the access map sees every access */
    d->io_bps = 0; d->nchange = 0; d->read_bps = 0;
    for (int i = 0; i < DBG_MAX_BP; i++) {
        DbgBreakpoint *b = &d->bp[i];
        if (!b->used || !b->enabled) continue;
        if (b->type == BP_CODE) d->watch[b->start] |= W_CODE;
        else if (b->type == BP_MEM_READ || b->type == BP_MEM_WRITE) {
            if (b->type == BP_MEM_READ) d->read_bps++;
            for (uint32_t a = b->start; a <= b->end; a++) d->watch[a] |= b->type == BP_MEM_READ ? W_READ : W_WRITE;
        } else if (b->type == BP_CHANGE) d->nchange++;
        else d->io_bps++;
    }
}

static int alloc_bp(Debugger *d) { for (int i = 0; i < DBG_MAX_BP; i++) if (!d->bp[i].used) return i; return -1; }

int dbg_add_code_bp(CPC *c, uint16_t addr) {
    Debugger *d = dbg_attach(c);
    int i = alloc_bp(d); if (i < 0) return -1;
    DbgBreakpoint *b = &d->bp[i]; memset(b, 0, sizeof *b);
    b->used = b->enabled = true; b->type = BP_CODE; b->start = b->end = addr;
    b->block = c->rd_map[addr >> 14];
    dbg_rebuild_watch(d); return i;
}

int dbg_add_mem_bp(CPC *c, uint16_t start, uint16_t end, bool read, bool write) {
    Debugger *d = dbg_attach(c);
    int first = -1;
    for (int pass = 0; pass < 2; pass++) {
        if (!(pass == 0 ? read : write)) continue;
        int i = alloc_bp(d); if (i < 0) break;
        DbgBreakpoint *b = &d->bp[i]; memset(b, 0, sizeof *b);
        b->used = b->enabled = true; b->type = pass == 0 ? BP_MEM_READ : BP_MEM_WRITE; b->start = start; b->end = end;
        if (first < 0) first = i;
    }
    dbg_rebuild_watch(d); return first;
}

int dbg_add_io_bp(CPC *c, uint16_t port, uint16_t mask, bool in, bool out) {
    Debugger *d = dbg_attach(c);
    int i = alloc_bp(d); if (i < 0) return -1;
    DbgBreakpoint *b = &d->bp[i]; memset(b, 0, sizeof *b);
    b->used = b->enabled = true; b->type = BP_IO; b->port = port & mask; b->mask = mask; b->on_in = in; b->on_out = out;
    dbg_rebuild_watch(d); return i;
}

void dbg_remove_bp(CPC *c, int i) { if (c->dbg && i >= 0 && i < DBG_MAX_BP) { c->dbg->bp[i].used = false; dbg_rebuild_watch(c->dbg); } }
void dbg_clear_bps(CPC *c) { if (c->dbg) { memset(c->dbg->bp, 0, sizeof c->dbg->bp); dbg_rebuild_watch(c->dbg); } }

void dbg_toggle_code_bp(CPC *c, uint16_t addr) {
    Debugger *d = dbg_attach(c);
    const uint8_t *blk = c->rd_map[addr >> 14];
    for (int i = 0; i < DBG_MAX_BP; i++) {
        DbgBreakpoint *b = &d->bp[i];
        if (b->used && b->type == BP_CODE && b->start == addr && b->block == blk) { dbg_remove_bp(c, i); return; }
    }
    dbg_add_code_bp(c, addr);
}

bool dbg_set_condition(CPC *c, int i, const char *cond, int pass) {
    if (!c->dbg || i < 0 || i >= DBG_MAX_BP || !c->dbg->bp[i].used) return false;
    if (cond && *cond) { int32_t v; char err[128]; Debugger *d = c->dbg; bool sv = d->ev_valid; d->ev_valid = true; bool ok = dbg_eval(c, cond, &v, err, sizeof err); d->ev_valid = sv; if (!ok) { snprintf(d->last_error, sizeof d->last_error, "%s", err); return false; } }
    snprintf(c->dbg->bp[i].cond, sizeof c->dbg->bp[i].cond, "%s", cond ? cond : "");
    c->dbg->bp[i].pass_count = pass; c->dbg->bp[i].hits = 0;
    return true;
}

/* ---------------- Expression evaluator ---------------- */
/* Parentheses, function arguments and chained unary operators recurse: an expression typed or sent by a client must not be able
 * to exhaust the stack (a WASM build has a small one), so nesting is capped. */
#define DBG_EVAL_MAX_DEPTH 64
typedef struct { CPC *c; Debugger *d; const char *s; char *err; size_t errsize; bool failed; int depth; } P;

static void fail(P *p, const char *msg) { if (!p->failed) { p->failed = true; snprintf(p->err, p->errsize, "%s", msg); } }
static void skipws(P *p) { while (*p->s == ' ' || *p->s == '\t') p->s++; }
static int32_t parse_or(P *p);

static bool is_id_start(int ch) { return isalpha(ch) || ch == '_' || ch == '.'; }
static bool is_id(int ch) { return isalnum(ch) || ch == '_' || ch == '\'' || ch == '.'; }

static bool reg_lookup(CPC *c, const char *n, int32_t *v) {
    Z80 *z = &c->z80;
    struct { const char *n; int32_t v; } t[] = {
        {"a", z->af.h}, {"f", z->af.l}, {"b", z->bc.h}, {"c", z->bc.l}, {"d", z->de.h}, {"e", z->de.l}, {"h", z->hl.h}, {"l", z->hl.l},
        {"af", z->af.w}, {"bc", z->bc.w}, {"de", z->de.w}, {"hl", z->hl.w}, {"ix", z->ix.w}, {"iy", z->iy.w}, {"sp", z->sp.w}, {"pc", z->pc},
        {"ixh", z->ix.h}, {"hx", z->ix.h}, {"ixl", z->ix.l}, {"lx", z->ix.l}, {"iyh", z->iy.h}, {"hy", z->iy.h}, {"iyl", z->iy.l}, {"ly", z->iy.l},
        {"af'", z->af2.w}, {"bc'", z->bc2.w}, {"de'", z->de2.w}, {"hl'", z->hl2.w}, {"i", z->i}, {"r", z->r}, {"im", z->im},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcasecmp(n, t[i].n)) { *v = t[i].v; return true; }
    return false;
}

static bool var_lookup(P *p, const char *n, int32_t *v) {
    CPC *c = p->c; Debugger *d = p->d;
    struct { const char *n; int32_t v; } t[] = {
        {"address", d->ev_addr}, {"value", d->ev_value}, {"previous", d->ev_prev}, {"ay_select", c->psg.sel}, {"psg_select", c->psg.sel},
        {"cartridge_bank", c->asic.cart_bank}, {"crtc_select", c->crtc.sel}, {"false", 0}, {"true", -1}, {"fdc_motor", c->motor ? -1 : 0},
        {"lower_enabled", c->lower_rom_en ? -1 : 0}, {"upper_enabled", c->upper_rom_en ? -1 : 0}, {"mode", c->ga_mode},
        {"mouse_buttons", 0}, {"mouse_wheel", 0}, {"mouse_x", 0}, {"mouse_y", 0}, {"palette_select", c->ga_pen},
        {"ppi_a", c->ppi_a}, {"ppi_b", c->ppi_b}, {"ppi_c", c->ppi_c}, {"ppi_control", c->ppi_ctl}, {"ram_bank", c->ga_ram},
        {"rtc_select", 0}, {"secondary_rom", c->asic.rmr2}, {"tape_motor", c->tape_motor ? -1 : 0}, {"upper_rom", c->upper_rom_sel},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcasecmp(n, t[i].n)) { *v = t[i].v; return true; }
    return false;
}

static DbgTimer *timer_get(Debugger *d, int id, bool create) {
    for (int i = 0; i < DBG_MAX_TIMERS; i++) if (d->timers[i].used && d->timers[i].id == id) return &d->timers[i];
    if (!create) return NULL;
    for (int i = 0; i < DBG_MAX_TIMERS; i++) if (!d->timers[i].used) { memset(&d->timers[i], 0, sizeof(DbgTimer)); d->timers[i].used = true; d->timers[i].id = id; d->timers[i].min = 0xFFFFFFFF; return &d->timers[i]; }
    return NULL;
}

static int32_t call_function(P *p, const char *name, int32_t *a, int n) {
    CPC *c = p->c; Debugger *d = p->d;
    #define IS(x) (!strcasecmp(name, x))
    if (IS("byte")) return n ? a[0] & 0xFF : 0;
    if (IS("word")) return n ? a[0] & 0xFFFF : 0;
    if (IS("hibyte")) return n ? (a[0] >> 8) & 0xFF : 0;
    if (IS("hiword")) return n ? (int32_t)(((uint32_t)a[0] >> 16) & 0xFFFF) : 0;
    if (IS("peek")) return n ? cpc_peek(c, (uint16_t)a[0]) : 0;
    if (IS("poke")) { for (int i = 1; i < n; i++) { DbgView v = {1, 0, 0, 0, 0, 0, 0}; dbg_view_current(c, &v); v.mode = 1; dbg_view_write(c, &v, (uint16_t)(a[0] + i - 1), (uint8_t)a[i]); } return 0; }
    if (IS("ay") || IS("psg")) return c->psg.regs[(n ? a[0] : c->psg.sel) & 15];
    if (IS("crtc")) return c->crtc.r[(n ? a[0] : c->crtc.sel) & 31];
    if (IS("rtc")) return 0;
    if (IS("ga_palette")) { int pen = (n ? a[0] : c->ga_pen) & 31; if (pen > 16) pen = 16; if (n > 1) c->ga_palette[pen] = a[1] & 31; return c->ga_palette[pen]; }
    if (IS("palette")) { int pen = n ? a[0] & 31 : c->ga_pen; if (pen > 16) pen = 16; if (n > 1) { asic_write(c, 0x2400 + pen * 2, (uint8_t)(((a[1] >> 4) & 15) << 4 | (a[1] & 15))); asic_write(c, 0x2401 + pen * 2, (uint8_t)((a[1] >> 8) & 15)); } return c->asic.pal12[pen]; }
    if (IS("reset_cycles")) { c->t_counter = n ? (uint64_t)a[0] : 0; return 0; }
    if (IS("timer_start")) { DbgTimer *t = timer_get(d, n ? a[0] : 0, true); if (t) { t->started = c->cycles; t->running = true; } return 0; }
    if (IS("timer_stop")) {
        DbgTimer *t = timer_get(d, n ? a[0] : 0, true); if (!t) return 1;
        uint64_t el = t->running ? c->cycles - t->started : 0; t->running = false;
        uint32_t e = el > 0xFFFF ? 0xFFFF : (uint32_t)el; if (e == 0) e = 1;
        t->count++; t->last = e; if (e < t->min) t->min = e; if (e > t->max) t->max = e; t->total += e;
        return (int32_t)e;
    }
    snprintf(p->err, p->errsize, "unknown function '%s'", name); p->failed = true;
    return 0;
    #undef IS
}

static int32_t parse_primary_impl(P *p) {
    skipws(p);
    const char *s = p->s;
    if (*s == '(') { p->s++; int32_t v = parse_or(p); skipws(p); if (*p->s != ')') fail(p, "missing ')'"); else p->s++; return v; }
    if (*s == '#' || *s == '&' || *s == '$' || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
        s += (s[0] == '0') ? 2 : 1; char *e; long v = strtol(s, &e, 16); if (e == s) { fail(p, "bad hex number"); return 0; } p->s = e; return (int32_t)v;
    }
    if (*s == '%') { s++; char *e; long v = strtol(s, &e, 2); if (e == s) { fail(p, "bad binary number"); return 0; } p->s = e; return (int32_t)v; }
    if (isdigit((unsigned char)*s)) {
        char *e; long v = strtol(s, &e, 10);
        if ((*e == 'h' || *e == 'H') && !is_id(e[1])) { v = strtol(s, &e, 16); e++; }
        p->s = e; return (int32_t)v;
    }
    if (is_id_start((unsigned char)*s)) {
        char name[64]; int n = 0;
        while (is_id((unsigned char)*s) && n < 63) name[n++] = *s++;
        name[n] = 0; p->s = s; skipws(p);
        if (*p->s == '(') {
            p->s++; int32_t args[8]; int na = 0; skipws(p);
            if (*p->s != ')') { for (;;) { args[na++ < 7 ? na - 1 : 7] = parse_or(p); skipws(p); if (*p->s == ',') { p->s++; continue; } break; } }
            if (*p->s != ')') fail(p, "missing ')'"); else p->s++;
            return call_function(p, name, args, na > 8 ? 8 : na);
        }
        int32_t v;
        if (var_lookup(p, name, &v) || reg_lookup(p->c, name, &v)) return v;
        if (dbg_symbol_lookup(p->d, name, &v)) return v;
        snprintf(p->err, p->errsize, "unknown name '%s'", name); p->failed = true; return 0;
    }
    fail(p, "unexpected character"); return 0;
}

static int32_t parse_primary(P *p) {
    if (p->depth >= DBG_EVAL_MAX_DEPTH) { fail(p, "expression nested too deeply"); return 0; }
    p->depth++;
    int32_t v = parse_primary_impl(p);
    p->depth--;
    return v;
}

static int32_t parse_unary(P *p);
static int32_t parse_unary_impl(P *p) {
    skipws(p);
    if (*p->s == '-') { p->s++; return -parse_unary(p); }
    if (*p->s == '+') { p->s++; return parse_unary(p); }
    if (!strncasecmp(p->s, "not", 3) && !is_id(p->s[3])) { p->s += 3; return ~parse_unary(p); }
    return parse_primary(p);
}
static int32_t parse_unary(P *p) {
    if (p->depth >= DBG_EVAL_MAX_DEPTH) { fail(p, "expression nested too deeply"); return 0; }
    p->depth++;
    int32_t v = parse_unary_impl(p);
    p->depth--;
    return v;
}
static int32_t parse_mul(P *p) {
    int32_t v = parse_unary(p);
    for (;;) {
        skipws(p);
        if (*p->s == '*') { p->s++; v *= parse_unary(p); }
        else if (*p->s == '/') { p->s++; int32_t r = parse_unary(p); if (!r) { fail(p, "division by zero"); return 0; } v /= r; }
        else if (!strncasecmp(p->s, "mod", 3) && !is_id(p->s[3])) { p->s += 3; int32_t r = parse_unary(p); if (!r) { fail(p, "division by zero"); return 0; } v %= r; }
        else return v;
    }
}
static int32_t parse_add(P *p) {
    int32_t v = parse_mul(p);
    for (;;) { skipws(p); if (*p->s == '+') { p->s++; v += parse_mul(p); } else if (*p->s == '-') { p->s++; v -= parse_mul(p); } else return v; }
}
static int32_t parse_cmp(P *p) {
    int32_t v = parse_add(p);
    for (;;) {
        skipws(p);
        if (!strncmp(p->s, "<>", 2)) { p->s += 2; v = v != parse_add(p) ? -1 : 0; }
        else if (!strncmp(p->s, "<=", 2)) { p->s += 2; v = v <= parse_add(p) ? -1 : 0; }
        else if (!strncmp(p->s, ">=", 2)) { p->s += 2; v = v >= parse_add(p) ? -1 : 0; }
        else if (*p->s == '<') { p->s++; v = v < parse_add(p) ? -1 : 0; }
        else if (*p->s == '>') { p->s++; v = v > parse_add(p) ? -1 : 0; }
        else if (*p->s == '=') { p->s++; if (*p->s == '=') p->s++; v = v == parse_add(p) ? -1 : 0; }
        else return v;
    }
}
static int32_t parse_and(P *p) {
    int32_t v = parse_cmp(p);
    for (;;) { skipws(p); if (!strncasecmp(p->s, "and", 3) && !is_id(p->s[3])) { p->s += 3; v &= parse_cmp(p); } else return v; }
}
static int32_t parse_or(P *p) {
    int32_t v = parse_and(p);
    for (;;) {
        skipws(p);
        if (!strncasecmp(p->s, "or", 2) && !is_id(p->s[2])) { p->s += 2; v |= parse_and(p); }
        else if (!strncasecmp(p->s, "xor", 3) && !is_id(p->s[3])) { p->s += 3; v ^= parse_and(p); }
        else return v;
    }
}

bool dbg_eval(CPC *c, const char *expr, int32_t *out, char *err, size_t errsize) {
    Debugger *d = dbg_attach(c);
    P p = {c, d, expr, err, errsize, false, 0};
    if (err && errsize) err[0] = 0;
    int32_t v = parse_or(&p);
    skipws(&p);
    if (!p.failed && *p.s) { snprintf(err, errsize, "unexpected '%s'", p.s); p.failed = true; }
    if (out) *out = v;
    return !p.failed;
}

/* ---------------- Break checks ---------------- */
void dbg_log(CPC *c, const char *line) {
    Debugger *d = dbg_attach(c);
    snprintf(d->log_lines[d->log_total % DBG_LOG_LINES], sizeof d->log_lines[0], "%s", line);
    d->log_total++;
}
uint32_t dbg_log_total(const Debugger *d) { return d->log_total; }
const char *dbg_log_line(const Debugger *d, uint32_t n) {
    if (n >= d->log_total || d->log_total - n > DBG_LOG_LINES) return NULL;
    return d->log_lines[n % DBG_LOG_LINES];
}
void dbg_log_clear(Debugger *d) { d->log_total = 0; }
void dbg_set_log(CPC *c, int index, const char *message) {
    Debugger *d = dbg_attach(c);
    if (index < 0 || index >= DBG_MAX_BP) return;
    snprintf(d->bp[index].log, sizeof d->bp[index].log, "%s", message ? message : "");
}

/* Expands {expr} / {expr:d} in a logpoint message and logs it with the frame number and PC. */
static void logpoint(CPC *c, const char *msg) {
    char out[192]; int o = snprintf(out, sizeof out, "[%llu] %04X  ", (unsigned long long)c->frames, c->z80.pc);
    for (const char *p = msg; *p && o < (int)sizeof out - 1; ) {
        if (*p != '{') { out[o++] = *p++; continue; }
        const char *e = strchr(p, '}');
        if (!e) { out[o++] = *p++; continue; }
        char expr[128]; int n = (int)(e - p - 1); if (n > 127) n = 127;
        memcpy(expr, p + 1, n); expr[n] = 0;
        bool dec = n > 2 && !strcmp(expr + n - 2, ":d"); if (dec) expr[n - 2] = 0;
        int32_t v; char err[64];
        if (!dbg_eval(c, expr, &v, err, sizeof err)) o += snprintf(out + o, sizeof out - o, "<%s?>", expr);
        else if (dec) o += snprintf(out + o, sizeof out - o, "%d", v);
        else o += snprintf(out + o, sizeof out - o, (v & ~0xFF) ? "#%04X" : "#%02X", v & 0xFFFF);
        p = e + 1;
    }
    out[o < (int)sizeof out ? o : (int)sizeof out - 1] = 0;
    dbg_log(c, out);
}

static bool bp_fires(CPC *c, Debugger *d, int i) {
    DbgBreakpoint *b = &d->bp[i];
    if (b->cond[0]) {
        int32_t v; char err[64];
        if (!dbg_eval(c, b->cond, &v, err, sizeof err) || v == 0) return false;
    }
    if (d->scanning) {                                    /* re-simulation: leave the user's counters and log alone */
        if (b->log[0]) return false;
        d->hit = true; d->hit_index = i; return true;
    }
    b->count++;
    if (b->pass_count > 0) { if (++b->hits < b->pass_count) return false; b->hits = 0; }
    if (b->log[0]) { logpoint(c, b->log); return false; }
    d->hit = true; d->hit_index = i;
    return true;
}

/* ---------------- Break on change ---------------- */
static bool reg_ref(CPC *c, const char *n, uint16_t *where, uint8_t *size) {
    Z80 *z = &c->z80;
    struct { const char *n; const void *p; uint8_t sz; } t[] = {
        {"a", &z->af.h, 1}, {"f", &z->af.l, 1}, {"b", &z->bc.h, 1}, {"c", &z->bc.l, 1}, {"d", &z->de.h, 1}, {"e", &z->de.l, 1}, {"h", &z->hl.h, 1}, {"l", &z->hl.l, 1},
        {"af", &z->af.w, 2}, {"bc", &z->bc.w, 2}, {"de", &z->de.w, 2}, {"hl", &z->hl.w, 2}, {"ix", &z->ix.w, 2}, {"iy", &z->iy.w, 2}, {"sp", &z->sp.w, 2}, {"pc", &z->pc, 2},
        {"ixh", &z->ix.h, 1}, {"hx", &z->ix.h, 1}, {"ixl", &z->ix.l, 1}, {"lx", &z->ix.l, 1}, {"iyh", &z->iy.h, 1}, {"hy", &z->iy.h, 1}, {"iyl", &z->iy.l, 1}, {"ly", &z->iy.l, 1},
        {"af'", &z->af2.w, 2}, {"bc'", &z->bc2.w, 2}, {"de'", &z->de2.w, 2}, {"hl'", &z->hl2.w, 2}, {"i", &z->i, 1}, {"r", &z->r, 1}, {"im", &z->im, 1},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcasecmp(n, t[i].n)) { *where = (uint16_t)((const uint8_t *)t[i].p - (const uint8_t *)z); *size = t[i].sz; return true; }
    return false;
}

/* Is the text only numbers, operators and symbols (no registers, variables or functions)? Then it has one fixed value. */
static bool constant_text(CPC *c, const char *s) {
    for (const char *p = s; *p; ) {
        if (is_id_start((unsigned char)*p) && !(p > s && isalnum((unsigned char)p[-1]))) {
            char id[64]; size_t n = 0; while (is_id((unsigned char)*p) && n < sizeof id - 1) id[n++] = *p++; id[n] = 0;
            const char *q = p; while (*q == ' ') q++;
            int32_t v; uint16_t w; uint8_t sz;
            if (*q == '(' || reg_ref(c, id, &w, &sz) || !dbg_symbol_lookup(c->dbg, id, &v)) return false;
        } else if (isdigit((unsigned char)*p) || *p == '#' || *p == '&' || *p == '$' || *p == '%') {
            while (isalnum((unsigned char)*p) || *p == '#' || *p == '&' || *p == '$' || *p == '%') p++;      /* a number, including 4000h and 0x4000 */
        } else p++;
    }
    return true;
}

static void change_classify(CPC *c, DbgBreakpoint *b) {
    char t[160]; size_t n = 0; for (const char *p = b->expr; *p && n < sizeof t - 1; p++) if (*p != ' ' && *p != '\t') t[n++] = *p; t[n] = 0;
    b->ckind = 0; b->have_last = false;
    uint16_t w; uint8_t sz;
    if (reg_ref(c, t, &w, &sz)) { b->ckind = 1; b->cwhere = w; b->csize = sz; return; }
    bool pk = !strncasecmp(t, "peek(", 5), wd = !strncasecmp(t, "word(", 5);
    if ((pk || wd) && n > 6 && t[n - 1] == ')') {
        char inner[160]; memcpy(inner, t + 5, n - 6); inner[n - 6] = 0;
        int32_t v; char err[64];
        if (constant_text(c, inner) && dbg_eval(c, inner, &v, err, sizeof err)) { b->ckind = pk ? 2 : 3; b->cwhere = (uint16_t)v; }
    }
}

int dbg_add_change_bp(CPC *c, const char *expr, char *err, size_t errsize) {
    Debugger *d = dbg_attach(c);
    int32_t v; char e[128]; bool sv = d->ev_valid; d->ev_valid = true;
    bool ok = expr && *expr && dbg_eval(c, expr, &v, e, sizeof e); d->ev_valid = sv;
    if (!ok) { if (err && errsize) snprintf(err, errsize, "%s", expr && *expr ? e : "empty expression"); return -1; }
    int i = alloc_bp(d); if (i < 0) { if (err && errsize) snprintf(err, errsize, "too many breakpoints"); return -1; }
    DbgBreakpoint *b = &d->bp[i]; memset(b, 0, sizeof *b);
    b->used = b->enabled = true; b->type = BP_CHANGE; snprintf(b->expr, sizeof b->expr, "%s", expr);
    change_classify(c, b);
    dbg_rebuild_watch(d); return i;
}

static bool change_value(CPC *c, DbgBreakpoint *b, int32_t *v) {
    switch (b->ckind) {
    case 1: { const uint8_t *p = (const uint8_t *)&c->z80 + b->cwhere; *v = b->csize == 1 ? *p : (int32_t)(p[0] | (p[1] << 8)); return true; }
    case 2: *v = cpc_peek(c, b->cwhere); return true;
    case 3: *v = cpc_peek(c, b->cwhere) | (cpc_peek(c, (uint16_t)(b->cwhere + 1)) << 8); return true;
    default: { char err[64]; return dbg_eval(c, b->expr, v, err, sizeof err); }
    }
}

void dbg_changes_rebase(CPC *c) {
    Debugger *d = c->dbg; if (!d) return;
    for (int i = 0; i < DBG_MAX_BP; i++) if (d->bp[i].used && d->bp[i].type == BP_CHANGE) d->bp[i].have_last = false;
}

static bool check_changes(CPC *c, Debugger *d) {
    for (int i = 0; i < DBG_MAX_BP; i++) {
        DbgBreakpoint *b = &d->bp[i];
        if (!b->used || !b->enabled || b->type != BP_CHANGE) continue;
        int32_t v; if (!change_value(c, b, &v)) continue;
        if (!b->have_last) { b->last = v; b->have_last = true; continue; }
        if (v == b->last) continue;
        d->ev_addr = c->z80.pc; d->ev_value = (uint8_t)v; d->ev_prev = (uint8_t)b->last; d->ev_valid = true;
        b->last = v;
        if (bp_fires(c, d, i)) return true;
    }
    return false;
}

bool dbg_check_code(CPC *c) {
    Debugger *d = c->dbg;
    if (!d) return false;
    uint16_t pc = c->z80.pc;
    if (d->skip_once) { d->skip_once = false; return false; }
    if (d->temp_bp >= 0 && pc == d->temp_bp) { d->temp_bp = -1; d->hit = true; d->hit_index = -1; return true; }
    if (d->nchange && d->active && !d->scanning && !d->quiet && check_changes(c, d)) return true;
    if (!d->active || !(d->watch[pc] & W_CODE)) return false;
    d->ev_addr = pc; d->ev_valid = true;
    for (int i = 0; i < DBG_MAX_BP; i++) {
        DbgBreakpoint *b = &d->bp[i];
        if (!b->used || !b->enabled || b->type != BP_CODE || b->start != pc) continue;
        if (b->block && c->rd_map[pc >> 14] != b->block) continue;
        if (bp_fires(c, d, i)) return true;
    }
    return false;
}

void dbg_mem_event(CPC *c, uint16_t addr, uint8_t value, bool write) {
    Debugger *d = c->dbg;
    if (d && d->acc && !d->fetching && !d->scanning && !d->quiet) { if (write) d->acc->w[addr]++; else d->acc->r[addr]++; }
    if (!d || !d->active || d->hit) return;
    d->ev_addr = addr; d->ev_value = value; d->ev_prev = write ? c->wr_map[addr >> 14][addr & 0x3FFF] : value; d->ev_valid = true;
    for (int i = 0; i < DBG_MAX_BP; i++) {
        DbgBreakpoint *b = &d->bp[i];
        if (!b->used || !b->enabled || b->type != (write ? BP_MEM_WRITE : BP_MEM_READ)) continue;
        if (addr < b->start || addr > b->end) continue;
        if (bp_fires(c, d, i)) return;
    }
}

void dbg_io_event(CPC *c, uint16_t port, uint8_t value, bool write) {
    Debugger *d = c->dbg;
    if (!d || !d->active || !d->io_bps || d->hit) return;
    d->ev_addr = port; d->ev_value = value; d->ev_prev = value; d->ev_valid = true;
    for (int i = 0; i < DBG_MAX_BP; i++) {
        DbgBreakpoint *b = &d->bp[i];
        if (!b->used || !b->enabled || b->type != BP_IO) continue;
        if (write ? !b->on_out : !b->on_in) continue;
        if ((port & b->mask) != b->port) continue;
        if (bp_fires(c, d, i)) return;
    }
}

/* ---------------- Stepping ---------------- */
void dbg_step_into(CPC *c) {
    Debugger *d = dbg_attach(c);
    d->hit = false; d->skip_once = true;
    cpc_step(c);
    /* A halted Z80 only runs NOPs until an interrupt: complete the HALT in one step instead of
     * leaving PC unchanged for thousands of steps (bounded to a couple of frames). */
    for (int n = 0; c->z80.halted && n < 80000; n++) { d->skip_once = true; cpc_step(c); }
    d->hit = false;
}

static int insn_len(CPC *c, uint16_t pc) { char t[64]; return z80_disassemble(pc, (uint8_t (*)(void *, uint16_t))cpc_mem_peek, c, t, sizeof t); }

bool dbg_step_over(CPC *c) {
    Debugger *d = dbg_attach(c);
    uint16_t pc = c->z80.pc;
    uint8_t op = cpc_peek(c, pc), op2 = cpc_peek(c, pc + 1);
    bool call = op == 0xCD || (op & 0xC7) == 0xC4 || (op & 0xC7) == 0xC7;
    bool rep = op == 0xED && op2 >= 0xB0 && op2 <= 0xBB && (op2 & 4) == 0 && (op2 & 3) < 4 && op2 != 0xB4 && op2 != 0xB5 && op2 != 0xB6 && op2 != 0xB7;
    if (!call && !rep) { dbg_step_into(c); return false; }
    uint16_t next = (uint16_t)(pc + insn_len(c, pc));
    d->hit = false; d->skip_once = true; d->temp_bp = next;
    return true;
}

void dbg_history_record(CPC *c) {
    Debugger *d = c->dbg; Z80 *z = &c->z80;
    if (z->halted) return;              /* a HALT loop would flood the history */
    d->hist[d->hist_pos] = (DbgHistory){z->pc, z->af.w, z->bc.w, z->de.w, z->hl.w, z->ix.w, z->iy.w, z->sp.w};
    d->hist_pos = (d->hist_pos + 1) % DBG_HISTORY;
    if (d->hist_count < DBG_HISTORY) d->hist_count++;
}
int dbg_history_count(const Debugger *d) { return d->hist_count; }
DbgHistory dbg_history_get(const Debugger *d, int back) { return d->hist[(d->hist_pos - 1 - back + 2 * DBG_HISTORY) % DBG_HISTORY]; }

void dbg_step_out(CPC *c) { Debugger *d = dbg_attach(c); d->hit = false; d->skip_once = true; d->step_out = true; d->step_out_sp = c->z80.sp.w; }

void dbg_run_to(CPC *c, uint16_t addr) { Debugger *d = dbg_attach(c); d->hit = false; d->skip_once = true; d->temp_bp = addr; }

/* ---------------- Symbols & data areas ---------------- */
void dbg_symbol_set(CPC *c, const char *name, int32_t value) {
    Debugger *d = dbg_attach(c);
    for (int i = 0; i < d->nsyms; i++) if (!strcasecmp(d->syms[i].name, name)) { d->syms[i].value = value; return; }
    if (d->nsyms == d->syms_cap) { d->syms_cap = d->syms_cap ? d->syms_cap * 2 : 256; d->syms = realloc(d->syms, d->syms_cap * sizeof(DbgSymbol)); }
    snprintf(d->syms[d->nsyms].name, sizeof d->syms[0].name, "%s", name); d->syms[d->nsyms++].value = value;
}
void dbg_symbol_clear(CPC *c) { if (c->dbg) c->dbg->nsyms = 0; }
bool dbg_symbol_lookup(const Debugger *d, const char *name, int32_t *v) {
    for (int i = 0; i < d->nsyms; i++) if (!strcasecmp(d->syms[i].name, name)) { *v = d->syms[i].value; return true; }
    return false;
}
const char *dbg_symbol_for_address(const Debugger *d, uint16_t addr) {
    for (int i = 0; i < d->nsyms; i++) if ((uint16_t)d->syms[i].value == addr && d->syms[i].value >= 0 && d->syms[i].value <= 0xFFFF) return d->syms[i].name;
    return NULL;
}

static bool parse_num(const char *s, int32_t *v) {
    if (*s == '#' || *s == '&' || *s == '$') { char *e; *v = (int32_t)strtol(s + 1, &e, 16); return e != s + 1; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { char *e; *v = (int32_t)strtol(s + 2, &e, 16); return e != s + 2; }
    if (*s == '%') { char *e; *v = (int32_t)strtol(s + 1, &e, 2); return e != s + 1; }
    char *e; long n = strtol(s, &e, 10); if (e == s) return false;
    if (*e == 'h' || *e == 'H') n = strtol(s, &e, 16);
    *v = (int32_t)n; return true;
}

int dbg_symbols_parse(CPC *c, const char *text, size_t size) {
    char line[256]; int count = 0;
    const char *p = text, *end = text + size;
    while (p < end) {
        const char *e = p; while (e < end && *e != '\n' && *e != '\r') e++;
        size_t len = (size_t)(e - p); if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = 0;
        p = e; while (p < end && (*p == '\n' || *p == '\r')) p++;
        char *tok[6]; int nt = 0; char *sv;
        for (char *t = strtok_r(line, " \t\r\n=", &sv); t && nt < 6; t = strtok_r(NULL, " \t\r\n=", &sv)) tok[nt++] = t;
        if (nt < 2) continue;
        int32_t v; char name[64];
        if (!strcasecmp(tok[0], "DEF") && nt >= 3) {        /* NoICE (.noi, SDCC/CPCtelera): DEF name 0xADDR */
            if (!strncmp(tok[1], "l__", 3) || !strncmp(tok[1], "s__", 3) || !strncmp(tok[1], ".__.", 4) || !parse_num(tok[2], &v)) continue;   /* skip linker section lengths/starts */
            snprintf(name, sizeof name, "%s", tok[1]);
        }
        else if (!strcasecmp(tok[0], "al") && nt >= 3) { if (!parse_num(tok[1], &v)) { char *e; v = (int32_t)strtol(tok[1], &e, 16); if (e == tok[1]) continue; } snprintf(name, sizeof name, "%s", tok[2][0] == '.' ? tok[2] + 1 : tok[2]); }
        else if (strlen(tok[0]) == 8 && strspn(tok[0], "0123456789ABCDEFabcdef") == 8) {   /* SDCC/ASxxxx .map: 0000ADDR name module */
            if (!strncmp(tok[1], "l__", 3) || !strncmp(tok[1], "s__", 3) || !strncmp(tok[1], ".__.", 4)) continue;
            v = (int32_t)strtol(tok[0], NULL, 16); snprintf(name, sizeof name, "%s", tok[1]);
        }
        else {
            /* "name value", "name: EQU value" (sjasmplus), "name #addr B0" (RASM), "name = value":
             * the value is the first numeric token after the name */
            snprintf(name, sizeof name, "%s", tok[0]); size_t l = strlen(name); if (l && name[l - 1] == ':') name[l - 1] = 0;
            bool found = false, area = false;
            for (int k = 1; k < nt; k++) if (!strcasecmp(tok[k], "bytes")) area = true;   /* linker area summary line */
            for (int k = 1; k < nt && !found && !area; k++) {
                if (!strcasecmp(tok[k], "equ")) continue;
                const char *t = tok[k]; size_t tl = strlen(t);
                /* whole token must be a number: #1234 &1234 $1234 0x1234 1234h %1010 or decimal */
                if (strspn(t + (strchr("#&$%", t[0]) ? 1 : 0), "0123456789ABCDEFabcdefxXhH") != tl - (strchr("#&$%", t[0]) ? 1 : 0)) continue;
                found = parse_num(t, &v);
            }
            if (!found) continue;
        }
        if (name[0] == ';' || !name[0]) continue;
        dbg_symbol_set(c, name, v); count++;
    }
    return count;
}

void dbg_area_add(CPC *c, uint16_t start, uint16_t end, bool words) {
    Debugger *d = dbg_attach(c);
    for (int i = 0; i < DBG_MAX_AREAS; i++) if (!d->areas[i].used) { d->areas[i] = (DbgDataArea){start, end, words, false, true}; return; }
}
void dbg_area_remove(CPC *c, uint16_t addr) { Debugger *d = c->dbg; if (!d) return; for (int i = 0; i < DBG_MAX_AREAS; i++) if (d->areas[i].used && addr >= d->areas[i].start && addr <= d->areas[i].end) d->areas[i].used = false; }
const DbgDataArea *dbg_area_at(const Debugger *d, uint16_t addr) {
    for (int i = 0; i < DBG_MAX_AREAS; i++) if (d->areas[i].used && addr >= d->areas[i].start && addr <= d->areas[i].end) return &d->areas[i];
    return NULL;
}

/* ---------------- Memory views ---------------- */
void dbg_view_current(CPC *c, DbgView *v) { v->mode = 0; v->lower = c->lower_rom_en; v->upper = c->upper_rom_en; v->upper_rom = c->upper_rom_sel; v->ram_cfg = c->ga_ram; v->lower_base = c->asic.lower_base; v->cart_bank = c->asic.cart_bank; }

static void view_maps(CPC *c, const DbgView *v, uint8_t **rd, uint8_t **wr) {
    cpc_compute_mapping(c, v->ram_cfg, v->mode == 1 ? false : v->lower, v->mode == 1 ? false : v->upper, (uint8_t)v->upper_rom, v->lower_base, v->cart_bank, rd, wr);
}

uint8_t dbg_view_read(CPC *c, const DbgView *v, uint16_t addr) {
    uint8_t *rd[4], *wr[4]; view_maps(c, v, rd, wr);
    return (v->mode == 1 ? wr : rd)[addr >> 14][addr & 0x3FFF];
}
void dbg_view_write(CPC *c, const DbgView *v, uint16_t addr, uint8_t value) {
    if (c->recorder) cpc_record_taint(c, "memory was edited");
    dbg_changes_rebase(c);
    uint8_t *rd[4], *wr[4]; view_maps(c, v, rd, wr);
    wr[addr >> 14][addr & 0x3FFF] = value;
}

typedef struct { CPC *c; const DbgView *v; } ViewCtx;
static uint8_t view_peek(void *ctx, uint16_t a) { ViewCtx *x = ctx; return dbg_view_read(x->c, x->v, a); }

int dbg_disasm(CPC *c, const DbgView *v, uint16_t addr, char *out, size_t size) {
    Debugger *d = dbg_attach(c);
    const DbgDataArea *a = dbg_area_at(d, addr);
    ViewCtx x = {c, v};
    if (a) {
        if (a->words && addr < a->end) { uint16_t w = (uint16_t)(view_peek(&x, addr) | (view_peek(&x, addr + 1) << 8)); snprintf(out, size, "DW #%04X", w); return 2; }
        snprintf(out, size, "DB #%02X", view_peek(&x, addr)); return 1;
    }
    char t[64]; int n = z80_disassemble(addr, view_peek, &x, t, sizeof t);
    snprintf(out, size, "%s", t);
    return n;
}

void dbg_describe_address(CPC *c, uint16_t addr, char *out, size_t size) {
    const uint8_t *blk = c->rd_map[addr >> 14];
    if (blk == c->rom_lower) { snprintf(out, size, "%04X:Lower", addr & 0x3FFF); return; }
    for (int i = 0; i < 32; i++) if (c->rom_upper[i] && blk == c->rom_upper[i]) { snprintf(out, size, "%04X:Upper %d", addr & 0x3FFF, i); return; }
    snprintf(out, size, "%04X", addr);
}

int dbg_find(CPC *c, const DbgView *v, uint16_t from, const uint8_t *pat, const bool *wild, int len, bool forward) {
    if (len <= 0) return -1;
    for (int k = 1; k <= 65536; k++) {
        uint16_t a = (uint16_t)(forward ? from + k : from - k);
        bool ok = true;
        for (int i = 0; i < len && ok; i++) if (!(wild && wild[i]) && dbg_view_read(c, v, (uint16_t)(a + i)) != pat[i]) ok = false;
        if (ok) return a;
    }
    return -1;
}

void dbg_fill(CPC *c, const DbgView *v, uint16_t start, uint16_t end, const uint8_t *pat, int len) {
    if (len <= 0) return;
    for (uint32_t a = start, i = 0; a <= end; a++, i++) dbg_view_write(c, v, (uint16_t)a, pat[i % len]);
}
