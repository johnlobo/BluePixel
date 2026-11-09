/* CPCApi debugger methods (debug.*). They drive the same Debugger the desktop app uses. Execution control follows the
 * core's model: debug.step_into / debug.step_out arm the debugger, and the machine only advances on run.* calls;
 * run.frames stops at the first frame in which a breakpoint fires and reports "break": true. */
#include "api_internal.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>

#define DISASM_MAX_COUNT 256
#define MEM_MAX_LENGTH 4096
#define MAX_SYMBOLS 16384     /* each call is a linear scan; stops a runaway client from growing the table without bound */

static const char *bp_type_name(BpType t) {
    switch (t) {
        case BP_CODE: return "code";
        case BP_MEM_READ: return "mem_read";
        case BP_MEM_WRITE: return "mem_write";
        case BP_IO: return "io";
        case BP_CHANGE: return "change";
    }
    return "unknown";
}

/* The execution history ring costs time on every instruction and the API does not expose it: never turn it on. */
static Debugger *attach(CPC *c) {
    bool fresh = !c->dbg;
    Debugger *d = dbg_attach(c);
    if (fresh) d->hist_on = false;
    return d;
}

static char *finish(ApiBuf *b, const char **error) {
    char *r = apibuf_take(b);
    if (!r) *error = "out of memory";
    return r;
}

static void status(CPC *c, ApiBuf *b) {
    Debugger *d = c->dbg;
    int bps = 0;
    if (d) for (int i = 0; i < DBG_MAX_BP; i++) if (d->bp[i].used) bps++;
    apibuf_printf(b, "{\"attached\":%s,\"enabled\":%s,\"history\":%s,\"break\":%s,\"break_index\":%d,\"pc\":%u,\"symbols\":%d,\"breakpoints\":%d}",
                  d ? "true" : "false", d && d->active ? "true" : "false", d && d->hist_on ? "true" : "false", d && d->hit ? "true" : "false",
                  d && d->hit ? d->hit_index : -1, c->z80.pc, d ? d->nsyms : 0, bps);
}

static void breakpoints_list(CPC *c, ApiBuf *b) {
    Debugger *d = c->dbg;
    apibuf_printf(b, "{\"breakpoints\":[");
    bool first = true;
    for (int i = 0; d && i < DBG_MAX_BP; i++) {
        const DbgBreakpoint *bp = &d->bp[i];
        if (!bp->used) continue;
        apibuf_printf(b, "%s{\"id\":%d,\"type\":\"%s\",\"enabled\":%s,\"start\":%u,\"end\":%u,\"pass_count\":%d,\"hits\":%d,\"condition\":",
                      first ? "" : ",", i, bp_type_name(bp->type), bp->enabled ? "true" : "false", bp->start, bp->end, bp->pass_count, bp->hits);
        apibuf_json_string(b, bp->cond);
        apibuf_printf(b, "}");
        first = false;
    }
    apibuf_printf(b, "]}");
}

static const char *bp_methods(CPCApiSession *s, Parser *p, int a, const char *m, ApiBuf *b, char **plain) {
    CPC *c = s->c;
    if (!strcmp(m, "debug.breakpoints.list")) { breakpoints_list(c, b); return NULL; }
    if (!strcmp(m, "debug.breakpoints.toggle_code")) {
        long addr = api_address_value(p, c, api_field(p, a, "address"));
        if (addr < 0 || addr > 0xFFFF) return "invalid address or symbol";
        dbg_toggle_code_bp(c, (uint16_t)addr);
        bool has = false;
        for (int i = 0; i < DBG_MAX_BP; i++) {
            const DbgBreakpoint *bp = &c->dbg->bp[i];
            if (bp->used && bp->enabled && bp->type == BP_CODE && bp->start == (uint16_t)addr) has = true;
        }
        apibuf_printf(b, "{\"address\":%ld,\"set\":%s}", addr, has ? "true" : "false");
        return NULL;
    }
    if (!strcmp(m, "debug.breakpoints.add_memory")) {
        long start = api_address_value(p, c, api_field(p, a, "start")), end = api_address_value(p, c, api_field(p, a, "end"));
        int access_field = api_field(p, a, "access");
        char *access = api_str(p, access_field);
        const char *acc = access ? access : "write";
        if (access_field && !access) return "access must be read, write or readwrite";
        bool rd = !strcmp(acc, "read") || !strcmp(acc, "readwrite"), wr = !strcmp(acc, "write") || !strcmp(acc, "readwrite");
        const char *err = NULL;
        int free_slots = 0;
        attach(c);
        for (int i = 0; i < DBG_MAX_BP; i++) if (!c->dbg->bp[i].used) free_slots++;
        if (start < 0 || end < 0 || start > end || end > 0xFFFF) err = "invalid memory range or symbol";
        else if (!rd && !wr) err = "access must be read, write or readwrite";
        else if (free_slots < (rd && wr ? 2 : 1)) err = "breakpoint table is full";     /* all or nothing: never half a readwrite pair */
        else {
            /* readwrite is two breakpoints (read, write); report both ids so the caller can remove both */
            int before[DBG_MAX_BP];
            for (int i = 0; i < DBG_MAX_BP; i++) before[i] = c->dbg->bp[i].used;
            int id = dbg_add_mem_bp(c, (uint16_t)start, (uint16_t)end, rd, wr);
            apibuf_printf(b, "{\"id\":%d,\"ids\":[", id);
            bool first = true;
            for (int i = 0; i < DBG_MAX_BP; i++)
                if (c->dbg->bp[i].used && !before[i]) { apibuf_printf(b, "%s%d", first ? "" : ",", i); first = false; }
            apibuf_printf(b, "]}");
        }
        free(access);
        return err;
    }
    if (!strcmp(m, "debug.breakpoints.set_condition")) {
        long id, pass;
        int cond_field = api_field(p, a, "condition");
        char *cond = api_str(p, cond_field);
        if (!api_req_int(p, api_field(p, a, "id"), &id) || !api_opt_int(p, api_field(p, a, "pass_count"), 0, &pass) || (cond_field && !cond)) {
            free(cond);
            return "id, pass_count (integers) and condition (string) are required in that form";
        }
        if (id < 0 || id >= DBG_MAX_BP || !c->dbg || !c->dbg->bp[id].used) { free(cond); return "no breakpoint with that id"; }
        if (pass < 0 || pass > 1000000) { free(cond); return "pass_count out of range"; }
        if (cond && strlen(cond) >= sizeof c->dbg->bp[0].cond) { free(cond); return "condition too long (max 159 bytes)"; }
        bool ok = dbg_set_condition(c, (int)id, cond ? cond : "", (int)pass);
        apibuf_printf(b, "{\"set\":%s,\"error\":", ok ? "true" : "false");
        apibuf_json_string(b, ok ? "" : c->dbg->last_error);
        apibuf_printf(b, "}");
        free(cond);
        return NULL;
    }
    /* debug.breakpoints.remove */
    long id;
    if (!api_req_int(p, api_field(p, a, "id"), &id)) return "id must be an integer";
    if (id < 0 || id >= DBG_MAX_BP || !c->dbg || !c->dbg->bp[id].used) return "no breakpoint with that id";
    dbg_remove_bp(c, (int)id);
    *plain = strdup("true");
    return NULL;
}

static const char *view_methods(CPCApiSession *s, Parser *p, int a, const char *m, ApiBuf *b) {
    CPC *c = s->c;
    DbgView view;
    dbg_view_current(c, &view);
    if (!strcmp(m, "debug.disassemble")) {
        long addr = api_address_value(p, c, api_field(p, a, "address")), count;
        if (!api_opt_int(p, api_field(p, a, "count"), 1, &count)) return "count must be an integer";
        if (addr < 0 || addr > 0xFFFF || count < 1 || count > DISASM_MAX_COUNT) return "invalid address or count";
        apibuf_printf(b, "{\"instructions\":[");
        for (long i = 0; i < count; i++) {
            char text[160] = "";
            int len = dbg_disasm(c, &view, (uint16_t)addr, text, sizeof text);
            if (len < 1) len = 1;
            apibuf_printf(b, "%s{\"address\":%ld,\"length\":%d,\"text\":", i ? "," : "", addr, len);
            apibuf_json_string(b, text);
            apibuf_printf(b, "}");
            addr = (addr + len) & 0xFFFF;
        }
        apibuf_printf(b, "]}");
        return NULL;
    }
    if (!strcmp(m, "debug.mem.read")) {
        long addr = api_address_value(p, c, api_field(p, a, "address")), len;
        if (!api_opt_int(p, api_field(p, a, "length"), 1, &len)) return "length must be an integer";
        if (addr < 0 || addr > 0xFFFF || len < 1 || len > MEM_MAX_LENGTH || addr + len > 0x10000) return "invalid memory range or symbol";
        apibuf_printf(b, "{\"address\":%ld,\"hex\":\"", addr);
        for (long i = 0; i < len; i++) apibuf_printf(b, "%02x", dbg_view_read(c, &view, (uint16_t)(addr + i)));
        apibuf_printf(b, "\"}");
        return NULL;
    }
    /* debug.mem.write: through the current paging, so it can reach RAM under a ROM */
    long addr = api_address_value(p, c, api_field(p, a, "address")), value;
    if (!api_req_int(p, api_field(p, a, "value"), &value) || addr < 0 || addr > 0xFFFF || value < 0 || value > 255) return "invalid address or byte";
    attach(c);       /* dbg_view_write rebases the change breakpoints of the debugger */
    dbg_view_write(c, &view, (uint16_t)addr, (uint8_t)value);
    apibuf_printf(b, "true");
    return NULL;
}

static const char *symbol_methods(CPCApiSession *s, Parser *p, int a, const char *m, ApiBuf *b) {
    CPC *c = s->c;
    if (!strcmp(m, "debug.symbols.clear")) { dbg_symbol_clear(c); apibuf_printf(b, "true"); return NULL; }
    if (!strcmp(m, "debug.symbols.set")) {
        char *name = api_str(p, api_field(p, a, "name"));
        long value = 0;
        const char *err = NULL;
        if (!name || !*name) err = "missing symbol name";
        else if (!api_req_int(p, api_field(p, a, "value"), &value)) err = "value must be an integer";
        else if (strlen(name) >= sizeof((DbgSymbol *)0)->name) err = "symbol name too long (max 47 bytes)";
        else if (value < -0x80000000L || value > 0x7FFFFFFFL) err = "value out of range";
        else if (c->dbg->nsyms >= MAX_SYMBOLS && !dbg_symbol_lookup(c->dbg, name, &(int32_t){0})) err = "symbol table is full";
        else { dbg_symbol_set(c, name, (int32_t)value); apibuf_printf(b, "true"); }
        free(name);
        return err;
    }
    /* debug.symbols.list */
    Debugger *d = c->dbg;
    apibuf_printf(b, "{\"symbols\":[");
    for (int i = 0; d && i < d->nsyms; i++) {
        apibuf_printf(b, "%s{\"name\":", i ? "," : "");
        apibuf_json_string(b, d->syms[i].name);
        apibuf_printf(b, ",\"value\":%d}", d->syms[i].value);
    }
    apibuf_printf(b, "]}");
    return NULL;
}

int api_debug_dispatch(CPCApiSession *s, Parser *p, int a, const char *m, char **result, const char **error) {
    CPC *c = s->c;
    *result = NULL;
    *error = NULL;
    if (strncmp(m, "debug.", 6)) return 0;
    ApiBuf b = {0};
    char *plain = NULL;
    const char *err = NULL;
    if (!strcmp(m, "debug.attach")) {
        attach(c)->active = true;  /* symbols.load leaves breakpoints disabled; attaching turns them on */
        status(c, &b);
    } else if (!strcmp(m, "debug.detach")) {
        dbg_detach(c);
        status(c, &b);
    } else if (!strcmp(m, "debug.status")) {
        status(c, &b);
    } else if (!strcmp(m, "debug.resume")) {
        Debugger *d = attach(c);
        d->hit = false; d->skip_once = true; d->temp_bp = -1; d->step_out = false;
        plain = strdup("true");
    } else if (!strcmp(m, "debug.pause")) {
        if (c->dbg) { c->dbg->temp_bp = -1; c->dbg->step_out = false; }
        plain = strdup("true");
    } else if (!strcmp(m, "debug.step_into")) {
        dbg_step_into(c);
        apibuf_printf(&b, "{\"pc\":%u}", c->z80.pc);
    } else if (!strcmp(m, "debug.step_out")) {
        dbg_step_out(c);
        plain = strdup("true");
    } else if (!strcmp(m, "debug.run_to")) {
        long addr = api_address_value(p, c, api_field(p, a, "address"));
        if (addr < 0 || addr > 0xFFFF) err = "invalid address or symbol";
        else { dbg_run_to(c, (uint16_t)addr); plain = strdup("true"); }
    } else if (!strcmp(m, "debug.data.mark")) {
        long start = api_address_value(p, c, api_field(p, a, "start")), end = api_address_value(p, c, api_field(p, a, "end"));
        if (start < 0 || end < start || end > 0xFFFF) err = "invalid memory range or symbol";
        else { dbg_area_add(c, (uint16_t)start, (uint16_t)end, api_boolean(p, api_field(p, a, "words"), false)); plain = strdup("true"); }
    } else if (!strcmp(m, "debug.data.clear")) {
        long addr = api_address_value(p, c, api_field(p, a, "address"));
        if (addr < 0 || addr > 0xFFFF) err = "invalid address or symbol";
        else { dbg_area_remove(c, (uint16_t)addr); plain = strdup("true"); }
    } else if (!strcmp(m, "debug.breakpoints.list") || !strcmp(m, "debug.breakpoints.toggle_code") || !strcmp(m, "debug.breakpoints.add_memory") ||
               !strcmp(m, "debug.breakpoints.set_condition") || !strcmp(m, "debug.breakpoints.remove")) {
        err = bp_methods(s, p, a, m, &b, &plain);
    } else if (!strcmp(m, "debug.disassemble") || !strcmp(m, "debug.mem.read") || !strcmp(m, "debug.mem.write")) {
        err = view_methods(s, p, a, m, &b);
    } else if (!strcmp(m, "debug.symbols.set") || !strcmp(m, "debug.symbols.clear") || !strcmp(m, "debug.symbols.list")) {
        err = symbol_methods(s, p, a, m, &b);
    } else {
        return 0;
    }
    if (err) { free(b.buf); free(plain); *error = err; return 1; }
    *result = plain ? plain : finish(&b, error);
    if (plain) free(b.buf);
    if (!*error && !*result) *error = "out of memory";
    return 1;
}
