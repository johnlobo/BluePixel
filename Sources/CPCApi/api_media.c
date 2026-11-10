/* CPCApi methods for input devices, display, audio, tape and disc media. All results are JSON built with ApiBuf. */
#include "api_internal.h"
#include "cpc_keys.h"
#include <ctype.h>
#include <stdio.h>
#include "dsk.h"
#include "fdc.h"
#include "tape.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_MAX_FRAMES 8192

static char *finish(ApiBuf *b, const char **error) {
    char *r = apibuf_take(b);
    if (!r) *error = "out of memory";
    return r;
}

static char *bool_result(bool v) { return strdup(v ? "true" : "false"); }

static void tape_status(CPC *c, ApiBuf *b) {
    CPCTape *t = c->tape;
    apibuf_printf(b, "{\"loaded\":%s,\"playing\":%s,\"name\":", t ? "true" : "false", t && tape_playing(t) ? "true" : "false");
    apibuf_json_string(b, t ? tape_name(t) : "");
    apibuf_printf(b, ",\"blocks\":%d,\"block\":%d,\"position_seconds\":%.3f,\"length_seconds\":%.3f}",
                  t ? tape_block_count(t) : 0, t ? tape_block(t) : 0, t ? tape_position_seconds(t) : 0.0, t ? tape_length_seconds(t) : 0.0);
}

static const char *tape_methods(CPCApiSession *s, Parser *p, int a, const char *m, char **result) {
    CPC *c = s->c;
    if (!strcmp(m, "tape.load")) {
        char *payload = api_str(p, api_field(p, a, "data_base64")), *name = api_str(p, api_field(p, a, "name"));
        size_t n = 0;
        unsigned char *b = payload ? api_unb64(payload, &n) : NULL;
        const char *err = NULL;
        if (!b || n == 0) err = "missing or invalid data_base64";
        else {
            CPCTape *t = tape_load(b, n, name && *name ? name : "tape.cdt");
            if (!t) err = "unsupported or invalid tape image";
            else {
                tape_free(c->tape);
                c->tape = t;
                tape_set_motor(t, c->tape_motor);
                *result = strdup("true");
            }
        }
        free(payload); free(name); free(b);
        return err;
    }
    if (!strcmp(m, "tape.play")) {
        if (!c->tape) return "no tape loaded";
        tape_play(c->tape, api_boolean(p, api_field(p, a, "play"), true));
        *result = strdup("true");
    } else if (!strcmp(m, "tape.rewind")) {
        if (!c->tape) return "no tape loaded";
        tape_rewind(c->tape);
        *result = strdup("true");
    } else if (!strcmp(m, "tape.remove")) {
        tape_free(c->tape);
        c->tape = NULL;
        *result = strdup("true");
    } else {   /* tape.status */
        ApiBuf b = {0};
        tape_status(c, &b);
        const char *e = NULL;
        *result = finish(&b, &e);
        return e;
    }
    return *result ? NULL : "out of memory";
}

static const char *disc_methods(CPCApiSession *s, Parser *p, int a, const char *m, char **result) {
    CPC *c = s->c;
    long drive;
    if (!api_opt_int(p, api_field(p, a, "drive"), 0, &drive) || drive < 0 || drive > 1) return "drive must be 0 or 1";
    CPCDisc *d = fdc_disc(c->fdc, (int)drive);
    ApiBuf b = {0};
    if (!strcmp(m, "disc.info")) {
        apibuf_printf(&b, "{\"drive\":%ld,\"loaded\":%s,\"name\":", drive, d ? "true" : "false");
        apibuf_json_string(&b, d ? d->name : "");
        apibuf_printf(&b, "}");
    } else {   /* disc.export */
        if (!d) return "no disc in that drive";
        size_t n = 0;
        uint8_t *bytes = dsk_serialize(d, &n);
        char *data = bytes ? api_b64(bytes, n) : NULL;
        free(bytes);
        if (!data) return "disc export failed";
        apibuf_printf(&b, "{\"drive\":%ld,\"name\":", drive);
        apibuf_json_string(&b, d->name);
        apibuf_printf(&b, ",\"data_base64\":\"%s\"}", data);
        free(data);
    }
    const char *e = NULL;
    *result = finish(&b, &e);
    return e;
}

static const char *audio_read(CPCApiSession *s, Parser *p, int a, char **result) {
    CPC *c = s->c;
    long want;
    if (!api_opt_int(p, api_field(p, a, "frames"), -1, &want) || want < -1) return "frames must be a non-negative integer";
    int avail = cpc_audio_available(c);
    if (want < 0) want = avail;
    if (want > AUDIO_MAX_FRAMES) want = AUDIO_MAX_FRAMES;
    float *samples = malloc((size_t)(want ? want : 1) * 2 * sizeof *samples);
    unsigned char *bytes = malloc((size_t)(want ? want : 1) * 8);
    if (!samples || !bytes) { free(samples); free(bytes); return "out of memory"; }
    int got = cpc_audio_read(c, samples, (int)want);
    /* little-endian IEEE-754 float32, interleaved L/R, regardless of host byte order */
    for (int i = 0; i < got * 2; i++) {
        uint32_t bits;
        memcpy(&bits, &samples[i], sizeof bits);
        for (int k = 0; k < 4; k++) bytes[i * 4 + k] = (unsigned char)(bits >> (8 * k));
    }
    char *data = api_b64(bytes, (size_t)got * 8);
    free(samples); free(bytes);
    if (!data) return "out of memory";
    ApiBuf b = {0};
    apibuf_printf(&b, "{\"frames\":%d,\"remaining\":%d,\"sample_rate\":%d,\"channels\":%d,\"format\":\"f32le\",\"data_base64\":\"%s\"}",
                  got, cpc_audio_available(c), c->cfg.sample_rate, 2, data);
    free(data);
    const char *e = NULL;
    *result = finish(&b, &e);
    return e;
}

static const char *screen_render(CPCApiSession *s, Parser *p, int a, char **result) {
    CPC *c = s->c;
    bool half = api_boolean(p, api_field(p, a, "half_size"), false);
    bool pal = api_boolean(p, api_field(p, a, "pal"), true);
    bool both = api_boolean(p, api_field(p, a, "scanlines"), true);
    long brightness = api_num(p, api_field(p, a, "brightness"), 0);
    if (brightness < -100 || brightness > 100) return "brightness must be -100..100";
    int w = half ? CPC_SCREEN_W / 2 : CPC_SCREEN_W, h = half ? CPC_SCREEN_H : CPC_SCREEN_H * 2;
    size_t pixels = (size_t)w * (size_t)h;
    uint32_t *dst = malloc(pixels * sizeof *dst);
    unsigned char *rgba = malloc(pixels * 4);
    if (!dst || !rgba) { free(dst); free(rgba); return "out of memory"; }
    const uint32_t *frame = cpc_frame(c);
    if (!frame) frame = c->frame;
    cpc_render(frame, dst, pal, both, half, (int)brightness);
    for (size_t i = 0; i < pixels; i++) {
        uint32_t px = dst[i];
        rgba[i * 4] = (unsigned char)(px >> 16); rgba[i * 4 + 1] = (unsigned char)(px >> 8);
        rgba[i * 4 + 2] = (unsigned char)px; rgba[i * 4 + 3] = (unsigned char)(px >> 24);
    }
    char *data = api_b64(rgba, pixels * 4);
    free(dst); free(rgba);
    if (!data) return "out of memory";
    ApiBuf b = {0};
    apibuf_printf(&b, "{\"frame\":%" PRIu64 ",\"width\":%d,\"height\":%d,\"format\":\"rgba8\",\"data_base64\":\"%s\"}", c->frames, w, h, data);
    free(data);
    const char *e = NULL;
    *result = finish(&b, &e);
    return e;
}

/* ---- boot_ready.configure: the entry address, given or read from the AMSDOS header of a file on a mounted disc ----
 * AMSDOS header (first 128 bytes of the file's data on the disc): 0x12 type (bit 0 = protected, so 2 and 3 are binaries), 0x15 load
 * address, 0x18 logical length, 0x1A entry address, 0x40 real length (24 bits), 0x43 checksum (16 bits, the sum of bytes 0x00..0x42). */
#define AMSDOS_HEADER_BYTES 128
#define AMSDOS_TYPE_BINARY 2

static const char *amsdos_entry(CPC *c, long drive, const char *file, long *address, char *exact, size_t exact_size) {
    CPCDisc *disc = fdc_disc(c->fdc, (int)drive);
    if (!disc) return "no disc in that drive";
    char name[16] = {0}, ext[8] = {0};
    const char *dot = strrchr(file, '.');
    size_t name_len = dot ? (size_t)(dot - file) : strlen(file), ext_len = dot ? strlen(dot + 1) : 0;
    if (!name_len || name_len > 8 || ext_len > 3) return "file must be NAME or NAME.EXT (up to 8 and 3 characters)";
    for (size_t i = 0; i < name_len; i++) name[i] = (char)toupper((unsigned char)file[i]);
    for (size_t i = 0; i < ext_len; i++) ext[i] = (char)toupper((unsigned char)dot[1 + i]);

    DskFile files[64];
    int count = dsk_catalogue(disc, files, 64);
    if (count < 0) return "the disc has no AMSDOS directory";
    int found = 0, pick = -1;
    for (int i = 0; i < count; i++) {
        if (files[i].user != 0 || strcmp(files[i].name, name) || (dot && strcmp(files[i].ext, ext))) continue;
        found++;
        pick = i;
    }
    if (!found) return "file not found on that disc (user 0)";
    if (found > 1) return "several files have that name: add the extension";

    uint8_t head[AMSDOS_HEADER_BYTES];
    if (dsk_file_head(disc, files[pick].name, files[pick].ext, 0, head, sizeof head) != AMSDOS_HEADER_BYTES) return "the file could not be read from the disc";
    unsigned sum = 0;
    for (int i = 0; i <= 0x42; i++) sum += head[i];
    if ((sum & 0xFFFF) != (unsigned)(head[0x43] | head[0x44] << 8)) return "the file has no AMSDOS header (checksum mismatch): pass address instead";
    if ((head[0x12] & 0xFE) != AMSDOS_TYPE_BINARY) return "the file is not a binary (AMSDOS type): it has no entry address, pass address instead";
    long entry = head[0x1A] | head[0x1B] << 8;
    if (!entry) return "the AMSDOS header has no entry address (zero): pass address instead";
    *address = entry;
    snprintf(exact, exact_size, "%s%s%s", files[pick].name, files[pick].ext[0] ? "." : "", files[pick].ext);
    return NULL;
}

static const char *boot_ready_configure(CPCApiSession *s, Parser *p, int a, char **result) {
    int address_field = api_field(p, a, "address"), file_field = api_field(p, a, "file");
    /* As before this method learned `file`: an address that is not an integer in 0..65535 simply leaves the boot target unset. */
    long address = address_field ? api_num(p, address_field, -1) : -1;
    bool address_valid = address >= 0 && address <= 65535;
    char *file = api_str(p, file_field);
    if (file_field && !file) return "file must be a string";
    char exact[16] = "";
    const char *source = address_valid ? "explicit" : "none";
    if (file) {
        if (address_field && !address_valid) { free(file); return "address must be an integer in 0..65535"; }
        long drive;
        if (!api_opt_int(p, api_field(p, a, "drive"), 0, &drive) || drive < 0 || drive > 1) { free(file); return "drive must be 0 or 1"; }
        long detected = -1;
        const char *err = amsdos_entry(s->c, drive, file, &detected, exact, sizeof exact);
        free(file);
        if (err) return err;
        if (address_field && address != detected) return "address differs from the entry address in the file's AMSDOS header";
        address = detected;
        address_valid = true;
        source = "amsdos_header";
    }
    if (!address_valid) address = -1;
    s->boot_target = (int)address;
    s->boot_configured = address_valid;
    ApiBuf b = {0};
    apibuf_printf(&b, "{\"configured\":%s,\"address\":%d,\"source\":\"%s\"", s->boot_configured ? "true" : "false", s->boot_target, source);
    if (exact[0]) { apibuf_printf(&b, ",\"file\":"); apibuf_json_string(&b, exact); }
    apibuf_printf(&b, "}");
    const char *e = NULL;
    *result = finish(&b, &e);
    return e;
}

/* rom.unload: the web UI lets the user remove the disc ROM again, which rom.load (exactly 16K) cannot express. */
static const char *rom_unload(CPCApiSession *s, Parser *p, int a) {
    char *role = api_str(p, api_field(p, a, "role"));
    int slot = -99;
    bool *loaded = NULL;
    if (role && !strcmp(role, "os")) { slot = -1; loaded = &s->rom_os; }
    else if (role && !strcmp(role, "basic")) { slot = 0; loaded = &s->rom_basic; }
    else if (role && !strcmp(role, "amsdos")) { slot = 7; loaded = &s->rom_amsdos; }
    free(role);
    if (!loaded) return "role must be os, basic, or amsdos";
    if (!cpc_load_rom(s->c, slot, NULL, 0)) return "ROM unload failed";
    *loaded = false;
    return NULL;
}

int api_media_dispatch(CPCApiSession *s, Parser *p, int a, const char *m, char **result, const char **error) {
    CPC *c = s->c;
    *result = NULL;
    *error = NULL;
    if (!strncmp(m, "tape.", 5) && (!strcmp(m, "tape.load") || !strcmp(m, "tape.play") || !strcmp(m, "tape.rewind") ||
                                    !strcmp(m, "tape.remove") || !strcmp(m, "tape.status"))) {
        *error = tape_methods(s, p, a, m, result);
    } else if (!strcmp(m, "boot_ready.configure")) {
        *error = boot_ready_configure(s, p, a, result);
    } else if (!strcmp(m, "rom.unload")) {
        *error = rom_unload(s, p, a);
        if (!*error) *result = bool_result(true);
    } else if (!strcmp(m, "disc.info") || !strcmp(m, "disc.export")) {
        *error = disc_methods(s, p, a, m, result);
    } else if (!strcmp(m, "audio.read")) {
        *error = audio_read(s, p, a, result);
    } else if (!strcmp(m, "audio.status")) {
        ApiBuf b = {0};
        apibuf_printf(&b, "{\"available\":%d,\"sample_rate\":%d,\"channels\":2,\"format\":\"f32le\"}", cpc_audio_available(c), c->cfg.sample_rate);
        *result = finish(&b, error);
    } else if (!strcmp(m, "screen.render")) {
        *error = screen_render(s, p, a, result);
    } else if (!strcmp(m, "display.set_monitor")) {
        long mon = api_num(p, api_field(p, a, "monitor"), -1);
        if (mon < CPC_MONITOR_COLOUR || mon > CPC_MONITOR_GREY) *error = "monitor must be 0 (colour), 1 (green) or 2 (grey)";
        else {
            CPCConfig cfg = c->cfg;
            cfg.monitor = (CPCMonitor)mon;
            cpc_configure(c, &cfg);
            *result = bool_result(true);
        }
    } else if (!strcmp(m, "input.key")) {
        long key = api_num(p, api_field(p, a, "key"), -1);
        if (key < 0 || key > 255 || (key >> 3) > 9) *error = "key must be a matrix position (row * 8 + bit, row 0..9)";
        else {
            cpc_key(c, (int)key, api_boolean(p, api_field(p, a, "down"), true));
            *result = bool_result(true);
        }
    } else if (!strcmp(m, "input.joystick")) {
        long bit = api_num(p, api_field(p, a, "bit"), -1);
        if (bit < 0 || bit > 7) *error = "bit must be 0..7";
        else {
            cpc_joystick(c, (int)bit, api_boolean(p, api_field(p, a, "down"), true));
            *result = bool_result(true);
        }
    } else {
        return 0;
    }
    if (!*error && !*result) *error = "out of memory";
    return 1;
}
