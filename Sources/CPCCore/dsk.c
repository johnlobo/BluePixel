#include "dsk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

CPCDisc *dsk_new(void) { return calloc(1, sizeof(CPCDisc)); }

void dsk_track_clear(CPCTrack *t) {
    for (int i = 0; i < t->nsectors; i++) { free(t->sec[i].data); t->sec[i].data = NULL; }
    memset(t, 0, sizeof *t);
}

void dsk_track_copy(CPCTrack *dst, const CPCTrack *src) {
    dsk_track_clear(dst);
    *dst = *src;
    for (int s = 0; s < DSK_MAX_SECTORS; s++) {
        CPCSector *sec = &dst->sec[s];
        if (s < src->nsectors && src->sec[s].data) { sec->data = malloc(sec->len ? sec->len : 1); memcpy(sec->data, src->sec[s].data, sec->len); }
        else sec->data = NULL;
    }
}

void dsk_touch(CPCDisc *d, const CPCTrack *t) {
    ptrdiff_t i = t - &d->trk[0][0];
    if (i < 0 || i >= DSK_MAX_TRACKS * 2) return;
    CPCTrack **o = &d->orig[0][0] + i;
    if (*o) return;
    *o = calloc(1, sizeof **o); dsk_track_copy(*o, t);
}

void dsk_forget_origins(CPCDisc *d) {
    for (int i = 0; i < DSK_MAX_TRACKS * 2; i++) {
        CPCTrack **o = &d->orig[0][0] + i;
        if (*o) { dsk_track_clear(*o); free(*o); *o = NULL; }
    }
}

uint32_t dsk_content_crc(const CPCDisc *d) {
    uLong crc = crc32(0L, Z_NULL, 0);
    uint8_t g[2] = {(uint8_t)d->tracks, (uint8_t)d->sides}; crc = crc32(crc, g, 2);
    for (int i = 0; i < DSK_MAX_TRACKS * 2; i++) {
        const CPCTrack *t = &d->trk[0][0] + i;
        if (!t->nsectors) continue;
        uint8_t h[5] = {(uint8_t)i, t->size_code, t->gap3, t->filler, (uint8_t)t->nsectors}; crc = crc32(crc, h, 5);
        for (int s = 0; s < t->nsectors; s++) {
            const CPCSector *c = &t->sec[s];
            uint8_t m[8] = {c->c, c->h, c->r, c->n, c->st1, c->st2, (uint8_t)c->len, (uint8_t)(c->len >> 8)};
            crc = crc32(crc, m, 8);
            if (c->data && c->len) crc = crc32(crc, c->data, c->len);
        }
    }
    return (uint32_t)crc;
}

size_t dsk_track_pack_size(const CPCTrack *t) {
    size_t n = 4;
    for (int s = 0; s < t->nsectors; s++) n += 8 + (t->sec[s].data ? t->sec[s].len : 0);
    return n;
}
uint8_t *dsk_track_pack(const CPCTrack *t, uint8_t *o) {
    *o++ = t->size_code; *o++ = t->gap3; *o++ = t->filler; *o++ = (uint8_t)t->nsectors;
    for (int s = 0; s < t->nsectors; s++) {
        const CPCSector *c = &t->sec[s];
        uint16_t len = c->data ? c->len : 0;               /* a sector without data is stored empty */
        *o++ = c->c; *o++ = c->h; *o++ = c->r; *o++ = c->n; *o++ = c->st1; *o++ = c->st2; *o++ = (uint8_t)len; *o++ = (uint8_t)(len >> 8);
        if (len) { memcpy(o, c->data, len); o += len; }
    }
    return o;
}
const uint8_t *dsk_track_unpack(CPCTrack *t, const uint8_t *p, const uint8_t *end) {
    if (end - p < 4 || p[3] > DSK_MAX_SECTORS) return NULL;
    t->size_code = p[0]; t->gap3 = p[1]; t->filler = p[2]; int ns = p[3]; p += 4;
    for (int s = 0; s < ns; s++) {
        if (end - p < 8) { dsk_track_clear(t); return NULL; }
        CPCSector *c = &t->sec[s];
        c->c = p[0]; c->h = p[1]; c->r = p[2]; c->n = p[3]; c->st1 = p[4]; c->st2 = p[5]; c->len = (uint16_t)(p[6] | (p[7] << 8)); p += 8;
        if ((size_t)(end - p) < c->len) { t->nsectors = s; dsk_track_clear(t); return NULL; }
        c->data = malloc(c->len ? c->len : 1); memcpy(c->data, p, c->len); p += c->len;
        t->nsectors = s + 1;
    }
    return p;
}

void dsk_free(CPCDisc *d) {
    if (!d) return;
    dsk_forget_origins(d);
    for (int t = 0; t < DSK_MAX_TRACKS; t++) for (int h = 0; h < 2; h++) dsk_track_clear(&d->trk[t][h]);
    free(d);
}

int dsk_sector_size(const CPCSector *s) { return s->len; }

static bool starts_with(const uint8_t *d, size_t n, const char *s) { size_t l = strlen(s); return n >= l && !memcmp(d, s, l); }

CPCDisc *dsk_load(const uint8_t *d, size_t size, const char *path) {
    if (size < 0x100) return NULL;
    bool ext = starts_with(d, size, "EXTENDED");
    if (!ext && !starts_with(d, size, "MV - CPC")) return NULL;
    CPCDisc *disc = dsk_new();
    disc->extended = ext;
    disc->tracks = d[0x30]; disc->sides = d[0x31];
    if (disc->tracks > DSK_MAX_TRACKS || disc->sides < 1 || disc->sides > 2) { free(disc); return NULL; }
    if (path) { strncpy(disc->path, path, sizeof disc->path - 1); const char *s = strrchr(path, '/'); strncpy(disc->name, s ? s + 1 : path, sizeof disc->name - 1); }
    size_t pos = 0x100;
    uint16_t std_size = d[0x32] | (d[0x33] << 8);
    for (int t = 0; t < disc->tracks; t++) for (int h = 0; h < disc->sides; h++) {
        size_t tsize = ext ? (size_t)d[0x34 + t * disc->sides + h] * 256 : std_size;
        if (tsize == 0) continue;
        if (pos + 0x100 > size) goto done;
        const uint8_t *ti = d + pos;
        if (!starts_with(ti, size - pos, "Track-Info")) goto done;
        CPCTrack *tr = &disc->trk[t][h];
        tr->size_code = ti[0x14]; tr->nsectors = ti[0x15] > DSK_MAX_SECTORS ? DSK_MAX_SECTORS : ti[0x15];
        tr->gap3 = ti[0x16]; tr->filler = ti[0x17];
        size_t dp = pos + 0x100;
        for (int s = 0; s < tr->nsectors; s++) {
            const uint8_t *si = ti + 0x18 + s * 8;
            CPCSector *sec = &tr->sec[s];
            sec->c = si[0]; sec->h = si[1]; sec->r = si[2]; sec->n = si[3]; sec->st1 = si[4]; sec->st2 = si[5];
            size_t len = ext ? (si[6] | (si[7] << 8)) : (size_t)(128 << (sec->n & 7));
            if (!ext && sec->n > 6) len = 0x2000;
            sec->len = (uint16_t)len;
            sec->data = calloc(1, len ? len : 1);
            if (dp + len <= size) memcpy(sec->data, d + dp, len); else if (dp < size) memcpy(sec->data, d + dp, size - dp);
            /* extended weak-sector copies: keep first copy only */
            size_t nominal = (size_t)(128 << (sec->n & 7));
            if (ext && len > nominal && nominal) sec->len = (uint16_t)nominal;
            dp += len;
        }
        pos += tsize;
    }
done:
    return disc;
}

CPCDisc *dsk_load_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    CPCDisc *d = dsk_load(b, n, path); free(b); return d;
}

uint8_t *dsk_serialize(const CPCDisc *d, size_t *out_size) {
    size_t cap = 0x100 + (size_t)DSK_MAX_TRACKS * 2 * (0x100 + 32 * 0x2000);
    uint8_t *o = calloc(1, cap); size_t pos = 0x100;
    bool ext = d->extended;
    memcpy(o, ext ? "EXTENDED CPC DSK File\r\nDisk-Info\r\n" : "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", ext ? 34 : 34);
    memcpy(o + 0x22, "BluePx", 6);
    o[0x30] = d->tracks; o[0x31] = d->sides;
    size_t max_track = 0;
    for (int t = 0; t < d->tracks; t++) for (int h = 0; h < d->sides; h++) {
        const CPCTrack *tr = &d->trk[t][h];
        if (tr->nsectors == 0) { if (ext) o[0x34 + t * d->sides + h] = 0; continue; }
        size_t data = 0;
        for (int s = 0; s < tr->nsectors; s++) data += ext ? tr->sec[s].len : (size_t)(128 << (tr->sec[s].n & 7));
        size_t tsize = 0x100 + data; tsize = (tsize + 255) & ~255u;
        if (tsize > max_track) max_track = tsize;
        uint8_t *ti = o + pos;
        memcpy(ti, "Track-Info\r\n", 12);
        ti[0x10] = t; ti[0x11] = h; ti[0x14] = tr->size_code; ti[0x15] = tr->nsectors; ti[0x16] = tr->gap3; ti[0x17] = tr->filler;
        size_t dp = pos + 0x100;
        for (int s = 0; s < tr->nsectors; s++) {
            const CPCSector *sec = &tr->sec[s]; uint8_t *si = ti + 0x18 + s * 8;
            size_t len = ext ? sec->len : (size_t)(128 << (sec->n & 7));
            si[0] = sec->c; si[1] = sec->h; si[2] = sec->r; si[3] = sec->n; si[4] = sec->st1; si[5] = sec->st2;
            if (ext) { si[6] = len & 255; si[7] = len >> 8; }
            memcpy(o + dp, sec->data, len < sec->len ? len : sec->len);
            dp += len;
        }
        if (ext) o[0x34 + t * d->sides + h] = tsize / 256;
        pos += tsize;
    }
    if (!ext) { o[0x32] = max_track & 255; o[0x33] = max_track >> 8; }
    *out_size = pos;
    return o;
}

bool dsk_save_file(CPCDisc *d, const char *path) {
    size_t n; uint8_t *b = dsk_serialize(d, &n);
    FILE *f = fopen(path, "wb"); if (!f) { free(b); return false; }
    bool ok = fwrite(b, 1, n, f) == n; fclose(f); free(b);
    if (ok) { d->dirty = false; if (path != d->path) strncpy(d->path, path, sizeof d->path - 1); }
    return ok;
}

void dsk_format(CPCDisc *d, DskFormat f, int tracks, int sides) {
    for (int t = 0; t < DSK_MAX_TRACKS; t++) for (int h = 0; h < 2; h++) dsk_track_clear(&d->trk[t][h]);
    d->tracks = tracks; d->sides = sides;
    uint8_t first = f == DSK_FORMAT_DATA ? 0xC1 : f == DSK_FORMAT_SYSTEM ? 0x41 : 0x01;
    for (int t = 0; t < tracks; t++) for (int h = 0; h < sides; h++) {
        CPCTrack *tr = &d->trk[t][h];
        tr->nsectors = 9; tr->size_code = 2; tr->gap3 = 0x4E; tr->filler = 0xE5;
        for (int s = 0; s < 9; s++) {
            CPCSector *sec = &tr->sec[s];
            sec->c = t; sec->h = h; sec->r = first + s; sec->n = 2; sec->len = 512;
            sec->data = malloc(512); memset(sec->data, 0xE5, 512);
        }
    }
    d->dirty = true;
}

static const CPCSector *find_sector(const CPCDisc *d, int track, int id) {
    if (track >= d->tracks) return NULL;
    const CPCTrack *t = &d->trk[track][0];
    for (int i = 0; i < t->nsectors; i++) if (t->sec[i].r == id && t->sec[i].data) return &t->sec[i];
    return NULL;
}

int dsk_catalogue(const CPCDisc *d, DskFile *out, int max) {
    /* Directory: first four 512-byte sectors of the data (C1, track 0), system (41, track 2) or IBM (01, track 1) format. */
    static const int first_id[3] = {0xC1, 0x41, 0x01}, dir_track[3] = {0, 2, 1};
    for (int f = 0; f < 3; f++) {
        if (!find_sector(d, dir_track[f], first_id[f])) continue;
        int n = 0;
        for (int s = 0; s < 4; s++) {
            const CPCSector *sec = find_sector(d, dir_track[f], first_id[f] + s);
            if (!sec || sec->len < 512) break;
            for (int e = 0; e < 16; e++) {
                const uint8_t *en = sec->data + e * 32;
                if (en[0] > 15 || en[12] != 0) continue;          /* deleted/unused, or not the first extent */
                if (n >= max) return n;
                DskFile *o = &out[n++];
                o->user = en[0]; o->system = (en[10] & 0x80) != 0; o->read_only = (en[9] & 0x80) != 0;
                int k = 0;
                for (int i = 1; i <= 8; i++) { char ch = en[i] & 0x7F; if (ch != ' ') o->name[k++] = ch; }
                o->name[k] = 0; k = 0;
                for (int i = 9; i <= 11; i++) { char ch = en[i] & 0x7F; if (ch != ' ') o->ext[k++] = ch; }
                o->ext[k] = 0;
            }
        }
        return n;
    }
    return -1;
}

bool dsk_is_cpm_boot(const CPCDisc *d) { return find_sector(d, 0, 0x41) != NULL; }

CPCDisc *dsk_clone(const CPCDisc *d) {
    CPCDisc *c = malloc(sizeof *c); if (!c) return NULL;
    memcpy(c, d, sizeof *c);
    for (int i = 0; i < DSK_MAX_TRACKS * 2; i++) {
        CPCTrack **o = &c->orig[0][0] + i;
        if (*o) { const CPCTrack *src = *o; *o = calloc(1, sizeof **o); dsk_track_copy(*o, src); }
    }
    for (int t = 0; t < DSK_MAX_TRACKS; t++) for (int h = 0; h < 2; h++) {
        CPCTrack *k = &c->trk[t][h];
        for (int s = 0; s < DSK_MAX_SECTORS; s++) {
            CPCSector *sec = &k->sec[s];
            if (s < k->nsectors && sec->data) { uint8_t *copy = malloc(sec->len ? sec->len : 1); memcpy(copy, sec->data, sec->len); sec->data = copy; }
            else sec->data = NULL;
        }
    }
    return c;
}
