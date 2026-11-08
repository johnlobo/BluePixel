#ifndef CPC_SER_H
#define CPC_SER_H
/* Canonical state serializer (internal to CPCCore).
 *
 * Every piece of machine state is described once, by a visit function that calls the ser_* primitives
 * below on each field in a fixed order. The same function writes, hashes, validates and restores, so the
 * save and restore paths cannot drift apart. The byte stream is little endian with no padding and does not
 * depend on struct layout, pointer size or host byte order: equal machines produce equal bytes on every
 * build (native, wasm32, any ABI). Floats are stored as their IEEE-754 bit patterns.
 *
 * Modes:
 *   SER_WRITE  append the canonical bytes to a growable buffer
 *   SER_HASH   feed the same bytes to FNV-1a 64 without storing them
 *   SER_CHECK  parse a stream and validate it, storing nothing (restore runs this first, so a malformed
 *              stream never leaves the machine half-restored)
 *   SER_READ   parse a stream and store every field
 * In CHECK mode the primitives never write through their pointer. Values that decide the shape of what
 * follows (lengths, counts, presence flags) must therefore be read with ser_count()/ser_flag(), which
 * return the value in every mode, instead of being taken back from the struct. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SER_WRITE, SER_HASH, SER_CHECK, SER_READ } SerMode;

typedef struct Ser {
    SerMode mode;
    uint8_t *buf; size_t len, cap;          /* WRITE */
    uint64_t h;                             /* HASH */
    const uint8_t *in; size_t in_len, pos;  /* CHECK / READ */
    bool bad;                               /* allocation failure or malformed input; sticky */
} Ser;

#define SER_FNV_OFFSET 1469598103934665603ULL
#define SER_FNV_PRIME 1099511628211ULL

static inline bool ser_reading(const Ser *s) { return s->mode == SER_CHECK || s->mode == SER_READ; }
static inline bool ser_storing(const Ser *s) { return s->mode == SER_READ; }

/* Moves n bytes between `p` and the stream; the only function that touches it. `p` is written only in
 * READ mode, or in CHECK mode when `scratch` says it is a local temporary rather than a machine field. */
static inline void ser_xfer(Ser *s, void *p, size_t n, bool scratch) {
    if (s->bad || n == 0) return;
    switch (s->mode) {
    case SER_WRITE:
        if (n > s->cap - s->len) {
            size_t cap = s->cap ? s->cap : 4096;
            while (n > cap - s->len) cap *= 2;
            uint8_t *nb = realloc(s->buf, cap);
            if (!nb) { s->bad = true; return; }
            s->buf = nb; s->cap = cap;
        }
        memcpy(s->buf + s->len, p, n); s->len += n; break;
    case SER_HASH: {
        const uint8_t *b = p; uint64_t h = s->h;
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= SER_FNV_PRIME; }
        s->h = h; break; }
    case SER_CHECK:
    case SER_READ:
        if (n > s->in_len - s->pos) { s->bad = true; return; }
        if (s->mode == SER_READ || scratch) memcpy(p, s->in + s->pos, n);
        s->pos += n; break;
    }
}

/* Value-returning forms: write `v` (WRITE/HASH) or return the value in the stream (CHECK/READ, 0 once bad). */
static inline uint64_t ser_le(Ser *s, uint64_t v, int nbytes) {
    uint8_t b[8]; for (int i = 0; i < nbytes; i++) b[i] = (uint8_t)(v >> (8 * i));
    ser_xfer(s, b, (size_t)nbytes, true);
    if (!ser_reading(s)) return v;
    if (s->bad) return 0;
    uint64_t x = 0; for (int i = 0; i < nbytes; i++) x |= (uint64_t)b[i] << (8 * i);
    return x;
}

/* Fixed-width little-endian fields. Each takes a pointer to the field; only READ mode writes it. */
#define SER_FIELD(name, type, nbytes) \
    static inline void name(Ser *s, type *v) { uint64_t x = ser_le(s, (uint64_t)*v, nbytes); if (ser_storing(s) && !s->bad) *v = (type)x; }
SER_FIELD(ser_u8, uint8_t, 1)
SER_FIELD(ser_u16, uint16_t, 2)
SER_FIELD(ser_u32, uint32_t, 4)
SER_FIELD(ser_u64, uint64_t, 8)
#undef SER_FIELD
static inline void ser_i8(Ser *s, int8_t *v) {
    uint64_t x = ser_le(s, (uint8_t)*v, 1); if (ser_storing(s) && !s->bad) *v = (int8_t)(uint8_t)x;
}
/* `int` is stored as 32 bits whatever its host width. Every int in the machine is a small counter, timer or
 * coordinate (the largest, timers and the audio phase, stay near 10^6), so a value read beyond +-2^24 is
 * malformed: accepting it would let a crafted state overflow a counter (undefined behaviour) on the next tick. */
#define SER_INT_LIMIT (1 << 24)
static inline void ser_int(Ser *s, int *v) {
    uint64_t x = ser_le(s, (uint32_t)*v, 4); int y = (int)(int32_t)(uint32_t)x;
    if (ser_reading(s) && !s->bad && (y > SER_INT_LIMIT || y < -SER_INT_LIMIT)) s->bad = true;
    if (ser_storing(s) && !s->bad) *v = y;
}
static inline void ser_i32(Ser *s, int32_t *v) {
    uint64_t x = ser_le(s, (uint32_t)*v, 4); if (ser_storing(s) && !s->bad) *v = (int32_t)(uint32_t)x;
}
/* An int used as an index or count: out-of-range values mark untrusted input malformed instead of being
 * stored, so a crafted state cannot make the core index outside its arrays. */
static inline void ser_int_in(Ser *s, int *v, int lo, int hi) {
    uint64_t x = ser_le(s, (uint32_t)*v, 4); int y = (int)(int32_t)(uint32_t)x;
    if (ser_reading(s) && !s->bad && (y < lo || y > hi)) s->bad = true;
    if (ser_storing(s) && !s->bad) *v = y;
}
static inline void ser_i8_in(Ser *s, int8_t *v, int8_t lo, int8_t hi) {
    uint64_t x = ser_le(s, (uint8_t)*v, 1); int8_t y = (int8_t)(uint8_t)x;
    if (ser_reading(s) && !s->bad && (y < lo || y > hi)) s->bad = true;
    if (ser_storing(s) && !s->bad) *v = y;
}
static inline void ser_u8_in(Ser *s, uint8_t *v, uint8_t hi) {
    uint64_t x = ser_le(s, *v, 1);
    if (ser_reading(s) && !s->bad && x > hi) s->bad = true;
    if (ser_storing(s) && !s->bad) *v = (uint8_t)x;
}
/* bool is one byte, 0 or 1; anything else is malformed. */
static inline void ser_bool(Ser *s, bool *v) {
    uint64_t x = ser_le(s, *v ? 1 : 0, 1);
    if (ser_reading(s) && x > 1) s->bad = true;
    if (ser_storing(s) && !s->bad) *v = x != 0;
}
/* Floats travel as their IEEE-754 bits. A NaN or infinity is never machine state; reading one is malformed
 * (it would reach float-to-int conversions, which are undefined for such values). */
static inline void ser_f32(Ser *s, float *v) {
    uint32_t u; memcpy(&u, v, 4); uint64_t x = ser_le(s, u, 4);
    u = (uint32_t)x; float f; memcpy(&f, &u, 4);
    if (ser_reading(s) && !s->bad && !isfinite(f)) s->bad = true;
    if (ser_storing(s) && !s->bad) *v = f;
}
static inline void ser_f64(Ser *s, double *v) {
    uint64_t u; memcpy(&u, v, 8); uint64_t x = ser_le(s, u, 8);
    double f; memcpy(&f, &x, 8);
    if (ser_reading(s) && !s->bad && !isfinite(f)) s->bad = true;
    if (ser_storing(s) && !s->bad) *v = f;
}

/* Arrays of the primitives above (fixed length known to both sides). */
static inline void ser_bytes(Ser *s, void *p, size_t n) { ser_xfer(s, p, n, false); }
static inline void ser_bools(Ser *s, bool *v, size_t n) { for (size_t i = 0; i < n; i++) ser_bool(s, &v[i]); }
/* On little-endian hosts the in-memory array already is the canonical form: move it in one go (the frame
 * buffers are 200K pixels each). Big-endian hosts take the per-element path. */
static inline bool ser_host_le(void) { const uint16_t one = 1; uint8_t b; memcpy(&b, &one, 1); return b == 1; }
static inline void ser_u16s(Ser *s, uint16_t *v, size_t n) {
    if (ser_host_le()) { ser_xfer(s, v, n * 2, false); return; }
    for (size_t i = 0; i < n; i++) ser_u16(s, &v[i]);
}
static inline void ser_u32s(Ser *s, uint32_t *v, size_t n) {
    if (ser_host_le()) { ser_xfer(s, v, n * 4, false); return; }
    for (size_t i = 0; i < n; i++) ser_u32(s, &v[i]);
}
static inline void ser_ints(Ser *s, int *v, size_t n) { for (size_t i = 0; i < n; i++) ser_int(s, &v[i]); }
static inline void ser_f32s(Ser *s, float *v, size_t n) { for (size_t i = 0; i < n; i++) ser_f32(s, &v[i]); }

/* Shape-deciding values: write `v`, or return what the stream holds, in every mode. A count above `max`
 * marks the stream malformed and returns 0, so callers can loop on the result safely. */
static inline uint32_t ser_count(Ser *s, uint32_t v, uint32_t max) {
    uint32_t x = (uint32_t)ser_le(s, v, 4);
    if (ser_reading(s) && x > max) { s->bad = true; return 0; }
    return s->bad ? 0 : x;
}
static inline bool ser_flag(Ser *s, bool v) {
    uint64_t x = ser_le(s, v ? 1 : 0, 1);
    if (ser_reading(s) && x > 1) s->bad = true;
    return !s->bad && x != 0;
}

/* Section tag: four ASCII bytes that make the stream self-describing and catch any reader/writer mismatch. */
static inline void ser_tag(Ser *s, const char tag[4]) {
    uint8_t b[4]; memcpy(b, tag, 4);
    ser_xfer(s, b, 4, true);
    if (ser_reading(s) && !s->bad && memcmp(b, tag, 4) != 0) s->bad = true;
}
#endif
