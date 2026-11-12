#include "api_internal.h"
#include "cpc.h"
#include "dsk.h"
#include "fdc.h"
#include "snapshot.h"
#include "state.h"
#include "cpc_keys.h"
#include "debug.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CPCApiSession *cpc_api_create(void) {
    CPCApiSession *s = calloc(1, sizeof *s);
    if (!s) {
        return NULL;
    }
    s->c = cpc_create(CPC_MODEL_6128);
    if (!s->c) {
        free(s);
        return NULL;
    }
#ifdef __EMSCRIPTEN__
    s->transports = CPC_API_WASM;
#else
    s->transports = CPC_API_STDIO;
#endif
    atomic_init(&s->cancelled, false);
    return s;
}

void cpc_api_set_transports(CPCApiSession *s, unsigned transports) {
    if (s) {
        s->transports = transports;
    }
}

void cpc_api_cancel(CPCApiSession *s) {
    if (s) {
        atomic_store(&s->cancelled, true);
    }
}

void cpc_api_set_yield(CPCApiSession *s, void (*callback)(void *), void *context) {
    if (s) {
        s->host_yield = callback;
        s->yield_context = context;
    }
}

CPCApiSession *cpc_api_borrow(CPC *c) {
    if (!c) {
        return NULL;
    }
    CPCApiSession *s = calloc(1, sizeof *s);
    if (s) {
        s->c = c;
        s->borrowed = true;
        atomic_init(&s->cancelled, false);
        s->rom_os = c->rom_lower != NULL;
        s->rom_basic = c->rom_upper[0] != NULL;
        s->rom_amsdos = c->rom_upper[7] != NULL;
    }
    return s;
}

void cpc_api_destroy(CPCApiSession *s) {
    if (s) {
        if (s->c && !s->borrowed) {
            for (int i = 0; i < 4; i++) {
                dsk_free(fdc_disc(s->c->fdc, i));
            }
            cpc_destroy(s->c);
        }
        free(s);
    }
}

static char *response(const char *id, const char *result, const char *error) {
    size_t n = strlen(id) + strlen(result ? result : "null") + (error ? strlen(error) + 80 : 48);
    char *r = malloc(n);
    if (!r) {
        return NULL;
    }
    if (error) {
        int code = !strcmp(error, "invalid JSON")       ? -32700
                   : !strcmp(error, "method not found") ? -32601
                   : !strcmp(error, "missing method")   ? -32600
                                                        : -32000;
        snprintf(r, n, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}", id, code, error);
    } else {
        snprintf(r, n, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result ? result : "null");
    }
    return r;
}

static char *dispatch_inner(CPCApiSession *s, const char *request, Parser *pp) {
    if (!s || !request) {
        return NULL;
    }
    pp->s = request;
    pp->n = 0;
    int root = api_parse(pp);
    if (!root || pp->t[root].type != '{') {
        return response("null", NULL, "invalid JSON");
    }
    int version = api_field(pp, root, "jsonrpc");
    if (!api_eq(pp, version, "2.0")) {
        return response("null", NULL, "invalid JSON-RPC version");
    }
    int id = api_field(pp, root, "id");
    char idbuf[64] = "null";
    if (id) {
        int n = pp->t[id].end - pp->t[id].start;
        if (n < 63) {
            memcpy(idbuf, request + pp->t[id].start, (size_t)n);
            idbuf[n] = 0;
        }
    }
    char *m = api_str(pp, api_field(pp, root, "method"));
    int a = api_field(pp, root, "params");
    CPC *c = s->c;
    char out[1024];
    const char *err = NULL;
    if (!m) {
        err = "missing method";
    } else if (!strcmp(m, "session.capabilities")) {
        char transports[96] = "";
        const char *names[] = {"stdio", "unix", "websocket", "wasm"};
        for (unsigned i = 0; i < 4; i++) {
            if (s->transports & (1u << i)) {
                if (*transports) {
                    strcat(transports, ",");
                }
                strcat(transports, "\"");
                strcat(transports, names[i]);
                strcat(transports, "\"");
            }
        }
        snprintf(out, sizeof out,
                 "{\"api_version\":\"1.4\",\"state_format\":1,\"machine\":\"CPC 6128\","
                 "\"roms_loaded\":{\"os\":%s,\"basic\":%s,\"amsdos\":%s},"
                 "\"capabilities\":[\"run\",\"headless\",\"capture\",\"memory\",\"input\",\"audio\",\"tape\",\"disc\","
                 "\"display\",\"debugger\"],"
                 "\"transports\":[%s]}",
                 c->rom_lower ? "true" : "false", c->rom_upper[0] ? "true" : "false",
                 c->rom_upper[7] ? "true" : "false", transports);
    } else if (!strcmp(m, "rom.load")) {
        char *role = api_str(pp, api_field(pp, a, "role"));
        char *payload = api_str(pp, api_field(pp, a, "data_base64"));
        size_t n = 0;
        unsigned char *b = payload ? api_unb64(payload, &n) : NULL;
        int slot = -99;
        bool *loaded = NULL;
        if (role && !strcmp(role, "os")) {
            slot = -1;
            loaded = &s->rom_os;
        } else if (role && !strcmp(role, "basic")) {
            slot = 0;
            loaded = &s->rom_basic;
        } else if (role && !strcmp(role, "amsdos")) {
            slot = 7;
            loaded = &s->rom_amsdos;
        }
        if (!loaded) {
            err = "role must be os, basic, or amsdos";
        } else if (!b || n != 16384) {
            err = "ROM must contain exactly 16384 bytes";
        } else if (!cpc_load_rom(c, slot, b, n)) {
            err = "ROM load failed";
        } else {
            *loaded = true;
            strcpy(out, "true");
        }
        free(role);
        free(payload);
        free(b);
    } else if (!strcmp(m, "machine.reset")) {
        cpc_reset(c, true);
        if (c->dbg) {
            c->dbg->hit = false;
            c->dbg->skip_once = true;
            c->dbg->temp_bp = -1;
            c->dbg->step_out = false;
        }
        strcpy(out, "true");
    } else if (!strcmp(m, "run.frames")) {
        long n = api_num(pp, api_field(pp, a, "count"), 1);
        if (n < 0 || n > 1000000) {
            err = "count out of range";
        } else {
            long ran = 0;
            bool brk = false;
            for (long i = 0; i < n && !brk && !atomic_load(&s->cancelled); i++) {
                cpc_run_frame(c);
                ran++;
                brk = cpc_dbg_break(c);
                if (s->host_yield) {
                    s->host_yield(s->yield_context);
                }
            }
            snprintf(out, sizeof out, "{\"frames\":%" PRIu64 ",\"pc\":%u,\"ran\":%ld,\"break\":%s}", c->frames,
                     c->z80.pc, ran, brk ? "true" : "false");
        }
    } else if (!strcmp(m, "run.instructions") || !strcmp(m, "run.nops")) {
        long count = api_num(pp, api_field(pp, a, "count"), 1);
        if (count < 0 || count > 100000000) {
            err = "count out of range";
        } else {
            long steps = 0, nops = 0;
            while (!atomic_load(&s->cancelled) &&
                   ((!strcmp(m, "run.instructions") && steps < count) || (!strcmp(m, "run.nops") && nops < count))) {
                int n = cpc_step(c);
                if (n <= 0) {
                    n = 1;
                }
                steps++;
                nops += n;
                if (s->host_yield && !(steps & 255)) {
                    s->host_yield(s->yield_context);
                }
            }
            snprintf(out, sizeof out,
                     "{\"requested\":%ld,\"executed_instructions\":%ld,\"advanced_nops\":%ld,\"pc\":%u,\"t_states_"
                     "aligned\":%" PRIu64 "}",
                     count, steps, nops, c->z80.pc, c->t_counter * 4);
        }
    } else if (!strcmp(m, "cpu.registers")) {
        snprintf(out, sizeof out,
                 "{\"pc\":%u,\"sp\":%u,\"af\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u,\"ix\":%u,\"iy\":%u,\"af_alt\":%u,\"bc_"
                 "alt\":%u,\"de_alt\":%u,\"hl_alt\":%u,\"i\":%u,\"r\":%u,\"im\":%u,\"iff1\":%s,\"iff2\":%s,\"halted\":%"
                 "s,\"frames\":%" PRIu64 ",\"nops\":%" PRIu64 "}",
                 c->z80.pc, c->z80.sp.w, c->z80.af.w, c->z80.bc.w, c->z80.de.w, c->z80.hl.w, c->z80.ix.w, c->z80.iy.w,
                 c->z80.af2.w, c->z80.bc2.w, c->z80.de2.w, c->z80.hl2.w, c->z80.i, c->z80.r, c->z80.im,
                 c->z80.iff1 ? "true" : "false", c->z80.iff2 ? "true" : "false", c->z80.halted ? "true" : "false",
                 c->frames, c->t_counter);
    } else if (!strcmp(m, "mem.read")) {
        long addr = api_address_value(pp, c, api_field(pp, a, "address")),
             len = api_num(pp, api_field(pp, a, "length"), 1);
        if (addr < 0 || addr > 65535 || len < 1 || len > 4096 || addr + len > 65536) {
            err = "invalid memory range or symbol";
        } else {
            size_t cap = (size_t)len * 2 + 48;
            char *r = malloc(cap);
            if (!r) {
                err = "out of memory";
            } else {
                int z = snprintf(r, cap, "{\"address\":%ld,\"hex\":\"", addr);
                for (long i = 0; i < len; i++) {
                    z += snprintf(r + z, cap - (size_t)z, "%02x", cpc_peek(c, (uint16_t)(addr + i)));
                }
                snprintf(r + z, cap - (size_t)z, "\"}");
                char *ret = response(idbuf, r, NULL);
                free(r);
                free(m);
                return ret;
            }
        }
    } else if (!strcmp(m, "mem.write")) {
        long addr = api_address_value(pp, c, api_field(pp, a, "address")),
             v = api_num(pp, api_field(pp, a, "value"), -1);
        if (addr < 0 || addr > 65535 || v < 0 || v > 255) {
            err = "invalid address or byte";
        } else {
            cpc_poke(c, (uint16_t)addr, (uint8_t)v);
            strcpy(out, "true");
        }
    } else if (!strcmp(m, "state.hash")) {
        snprintf(out, sizeof out, "{\"hash\":\"%016" PRIx64 "\"}", cpc_state_hash(c));
    } else if (!strcmp(m, "state.save")) {
        CPCState *st = cpc_state_save(c);
        if (!st) {
            err = "state save failed";
        } else {
            size_t n = 0;
            const uint8_t *b = cpc_state_bytes(st, &n);
            char *data = api_b64(b, n);
            size_t cap = strlen(data) + 64;
            char *r = malloc(cap);
            snprintf(r, cap, "{\"data_base64\":\"%s\"}", data);
            char *ret = response(idbuf, r, NULL);
            free(r);
            free(data);
            cpc_state_free(st);
            free(m);
            return ret;
        }
    } else if (!strcmp(m, "state.restore")) {
        char *payload = api_str(pp, api_field(pp, a, "data_base64"));
        size_t n = 0;
        unsigned char *b = payload ? api_unb64(payload, &n) : NULL;
        CPCState *st = b ? cpc_state_from_bytes(b, n) : NULL;
        if (!st) {
            err = "invalid state image";
        } else {
            unsigned hazards = 0;
            if (!cpc_state_restore(c, st, &hazards)) {
                err = "state restore failed";
            } else {
                snprintf(out, sizeof out, "{\"restored\":true,\"hazards\":%u}", hazards);
            }
        }
        cpc_state_free(st);
        free(payload);
        free(b);
    } else if (!strcmp(m, "snapshot.load") || !strcmp(m, "disc.load")) {
        char *payload = api_str(pp, api_field(pp, a, "data_base64"));
        long drive = api_num(pp, api_field(pp, a, "drive"), 0);
        size_t n = 0;
        char *dname = api_str(pp, api_field(pp, a, "name"));
        unsigned char *b = payload ? api_unb64(payload, &n) : NULL;
        if (!b) {
            err = "missing or invalid data_base64";
        } else if (!strcmp(m, "snapshot.load")) {
            if (!cpc_snapshot_load(c, b, n)) {
                err = "snapshot load failed";
            } else {
                strcpy(out, "true");
            }
        } else if (drive < 0 || drive > 1) {
            err = "drive must be 0 or 1";
        } else {
            CPCDisc *d = dsk_load(b, n, dname && *dname ? dname : "<api>");
            if (!d) {
                err = "invalid DSK";
            } else {
                CPCDisc *old = fdc_disc(c->fdc, (int)drive);
                fdc_insert(c->fdc, (int)drive, d);
                dsk_free(old);
                strcpy(out, "true");
            }
        }
        free(dname);
        free(payload);
        free(b);
    } else if (!strcmp(m, "snapshot.save")) {
        long snap_version = api_num(pp, api_field(pp, a, "version"), 3);
        size_t n = 0;
        uint8_t *b = cpc_snapshot_save(c, (int)snap_version, 0, true, &n);
        if (!b) {
            err = "snapshot save failed";
        } else {
            char *data = api_b64(b, n);
            size_t cap = strlen(data) + 64;
            char *r = malloc(cap);
            snprintf(r, cap, "{\"data_base64\":\"%s\"}", data);
            char *ret = response(idbuf, r, NULL);
            free(b);
            free(data);
            free(r);
            free(m);
            return ret;
        }
    } else if (!strcmp(m, "input.text")) {
        char *t = api_str(pp, api_field(pp, a, "text"));
        if (!t) {
            err = "missing text";
        } else {
            cpc_autotype(c, t);
            free(t);
            strcpy(out, "true");
        }
    } else if (!strcmp(m, "input.matrix")) {
        long row = api_num(pp, api_field(pp, a, "row"), -1), mask = api_num(pp, api_field(pp, a, "mask"), -1);
        bool pressed = api_boolean(pp, api_field(pp, a, "pressed"), false);
        if (row < 0 || row > 9 || mask < 0 || mask > 255 || mask == 255) {
            err = "row must be 0..9 and mask must select at least one key";
        } else {
            for (int bit = 0; bit < 8; bit++) {
                if (!(mask & (1 << bit))) {
                    if (pressed) {
                        c->kbd[row] &= (uint8_t)~(1 << bit);
                    } else {
                        c->kbd[row] |= (uint8_t)(1 << bit);
                    }
                }
            }
            strcpy(out, "true");
        }
    } else if (!strcmp(m, "symbols.load")) {
        char *payload = api_str(pp, api_field(pp, a, "data_base64"));
        size_t n = 0;
        unsigned char *b = payload ? api_unb64(payload, &n) : NULL;
        if (!b) {
            err = "missing or invalid symbols data_base64";
        } else {
            Debugger *d = dbg_attach(c);
            d->hist_on = false;
            d->active = false;
            int count = dbg_symbols_parse(c, (char *)b, n);
            if (count < 0) {
                err = "symbol parsing failed";
            } else {
                snprintf(out, sizeof out, "{\"loaded\":%d}", count);
            }
        }
        free(payload);
        free(b);
    } else if (!strcmp(m, "boot_ready.status")) {
        bool drained = !cpc_autotype_active(c);
        for (int row = 0; row < 16; row++) {
            if (c->kbd[row] != 0xFF) {
                drained = false;
            }
        }
        snprintf(out, sizeof out, "{\"ready\":%s,\"pc\":%u,\"keyboard_drained\":%s,\"target\":%d}",
                 s->boot_configured && c->z80.pc == s->boot_target && drained ? "true" : "false", c->z80.pc,
                 drained ? "true" : "false", s->boot_target);
    } else if (!strcmp(m, "screen.capture")) {
        size_t pixels = (size_t)CPC_SCREEN_W * CPC_SCREEN_H, n = pixels * 4;
        const uint32_t *frame = cpc_frame(c);
        if (!frame) {
            frame = c->frame;
        }
        uint8_t *rgba = malloc(n);
        if (!rgba) {
            err = "out of memory";
        } else {
            for (size_t i = 0; i < pixels; i++) {
                uint32_t px = frame[i];
                rgba[i * 4] = (uint8_t)(px >> 16);
                rgba[i * 4 + 1] = (uint8_t)(px >> 8);
                rgba[i * 4 + 2] = (uint8_t)px;
                rgba[i * 4 + 3] = (uint8_t)(px >> 24);
            }
            char *data = api_b64(rgba, n);
            if (!data) {
                free(rgba);
                err = "out of memory";
            } else {
                bool with_metadata = api_boolean(pp, api_field(pp, a, "metadata"), false);
                char meta[768] = "";
                if (with_metadata) {
                    size_t z = 0;
                    z += (size_t)snprintf(meta + z, sizeof meta - z,
                                          "\"metadata\":{\"screen_mode\":%u,\"crtc_type\":%u,\"crtc_registers\":[",
                                          c->ga_mode, c->crtc.type);
                    for (int i = 0; i < 18 && z < sizeof meta; i++) {
                        z += (size_t)snprintf(meta + z, sizeof meta - z, "%s%u", i ? "," : "", c->crtc.r[i]);
                    }
                    z += (size_t)snprintf(meta + z, sizeof meta - z, "],\"palette_hw\":[");
                    for (int i = 0; i < 17 && z < sizeof meta; i++) {
                        z += (size_t)snprintf(meta + z, sizeof meta - z, "%s%u", i ? "," : "", c->ga_palette[i]);
                    }
                    z += (size_t)snprintf(meta + z, sizeof meta - z, "],\"palette_argb\":[");
                    for (int i = 0; i < 17 && z < sizeof meta; i++) {
                        z += (size_t)snprintf(meta + z, sizeof meta - z, "%s\"%08" PRIx32 "\"", i ? "," : "",
                                              cpc_pen_rgb(c, i));
                    }
                    (void)snprintf(meta + z, sizeof meta - z, "]},");
                }
                size_t cap = strlen(data) + sizeof meta + 192;
                char *r = malloc(cap);
                if (!r) {
                    free(data);
                    free(rgba);
                    err = "out of memory";
                } else {
                    snprintf(r, cap,
                             "{\"frame\":%" PRIu64
                             ",\"width\":%d,\"height\":%d,\"format\":\"rgba8\",%s\"data_base64\":\"%s\"}",
                             c->frames, CPC_SCREEN_W, CPC_SCREEN_H, meta[0] ? meta : "", data);
                    char *ret = response(idbuf, r, NULL);
                    free(r);
                    free(data);
                    free(rgba);
                    free(m);
                    return ret;
                }
            }
        }
    } else if (!strcmp(m, "run.reset_tstates")) {
        c->t_counter = 0;
        strcpy(out, "true");
    } else {
        char *dyn = NULL;
        const char *e = NULL;
        if (api_media_dispatch(s, pp, a, m, &dyn, &e) || api_debug_dispatch(s, pp, a, m, &dyn, &e)) {
            if (e) {
                err = e;
                free(dyn);
            } else {
                char *ret = response(idbuf, dyn, NULL);
                free(dyn);
                free(m);
                return ret;
            }
        } else {
            err = "method not found";
        }
    }
    char *ret = response(idbuf, out, err);
    free(m);
    return ret;
}

/* Evaluating a symbolic address (dbg_eval) attaches a Debugger with the execution history ring on. The ring costs
 * time on every instruction and no method exposes it, so no call leaves it running, whatever path attached the
 * debugger. */
char *cpc_api_dispatch(CPCApiSession *s, const char *request) {
    Parser *parser =
        malloc(sizeof *parser); /* 130 KB of tokens: on the heap so a small WebAssembly stack is never at risk */
    if (!parser) {
        return response("null", NULL, "out of memory");
    }
    char *reply = dispatch_inner(s, request, parser);
    free(parser);
    if (s && s->c && s->c->dbg) {
        s->c->dbg->hist_on = false;
    }
    return reply;
}
