#include "z80.h"
#include "z80_timing.h"
#include <string.h>

enum { FS = 0x80, FZ = 0x40, FY = 0x20, FH = 0x10, FX = 0x08, FP = 0x04, FN = 0x02, FC = 0x01 };

static uint8_t sz53[256], sz53p[256], parity_tab[256];
static bool tables_ready;

static void init_tables(void) {
    if (tables_ready) return;
    for (int i = 0; i < 256; i++) {
        int p = 0, v = i;
        while (v) { p ^= v & 1; v >>= 1; }
        parity_tab[i] = p ? 0 : FP;
        sz53[i] = (i & (FS | FY | FX)) | (i == 0 ? FZ : 0);
        sz53p[i] = sz53[i] | parity_tab[i];
    }
    tables_ready = true;
}

void z80_reset(Z80 *z) {
    init_tables();
    uint8_t brk = z->brk_enabled, turbo = z->turbo;
    void *ctx = z->ctx;
    typeof(z->rd) rd = z->rd; typeof(z->fetch) fetch = z->fetch; typeof(z->wr) wr = z->wr; typeof(z->in) in = z->in; typeof(z->out) out = z->out;
    memset(z, 0, sizeof *z);
    z->ctx = ctx; z->rd = rd; z->fetch = fetch; z->wr = wr; z->in = in; z->out = out;
    z->brk_enabled = brk; z->turbo = turbo;
    z->af.w = 0xFFFF; z->sp.w = 0xFFFF; z->wz.w = 0; z->int_vector = 0xFF;
}

#define RD(a) (z->rd(z->ctx, (uint16_t)(a)))
#define WR(a, v) (z->wr(z->ctx, (uint16_t)(a), (uint8_t)(v)))
#define A z->af.h
#define F z->af.l

static inline uint8_t fetch8(Z80 *z) {
    uint8_t v = __builtin_expect(z->fast_map != NULL, 1) ? z->fast_map[z->pc >> 14][z->pc & 0x3FFF] : z->fetch(z->ctx, z->pc);
    z->pc++;
    return v;
}
static inline uint16_t fetch16(Z80 *z) { uint8_t l = fetch8(z); uint8_t h = fetch8(z); return (uint16_t)(l | (h << 8)); }
static inline uint16_t rd16(Z80 *z, uint16_t a) { uint8_t l = RD(a); uint8_t h = RD(a + 1); return (uint16_t)(l | (h << 8)); }
static inline void wr16(Z80 *z, uint16_t a, uint16_t v) { WR(a, v & 0xFF); WR(a + 1, v >> 8); }
static inline void push16(Z80 *z, uint16_t v) { z->sp.w--; WR(z->sp.w, v >> 8); z->sp.w--; WR(z->sp.w, v & 0xFF); }
static inline uint16_t pop16(Z80 *z) { uint16_t v = rd16(z, z->sp.w); z->sp.w += 2; return v; }

/* ---- ALU ---- */
static void alu_add(Z80 *z, uint8_t v, int c) {
    int r = A + v + c;
    uint8_t res = (uint8_t)r;
    F = sz53[res] | (((A ^ v ^ res) & 0x10) ? FH : 0) | ((~(A ^ v) & (A ^ res) & 0x80) ? FP : 0) | (r > 0xFF ? FC : 0);
    A = res;
}
static uint8_t alu_sub_f(Z80 *z, uint8_t v, int c) {
    int r = A - v - c;
    uint8_t res = (uint8_t)r;
    F = sz53[res] | (((A ^ v ^ res) & 0x10) ? FH : 0) | (((A ^ v) & (A ^ res) & 0x80) ? FP : 0) | FN | (r < 0 ? FC : 0);
    return res;
}
static void alu(Z80 *z, int op, uint8_t v) {
    switch (op) {
    case 0: alu_add(z, v, 0); break;
    case 1: alu_add(z, v, F & FC); break;
    case 2: A = alu_sub_f(z, v, 0); break;
    case 3: A = alu_sub_f(z, v, F & FC); break;
    case 4: A &= v; F = sz53p[A] | FH; break;
    case 5: A ^= v; F = sz53p[A]; break;
    case 6: A |= v; F = sz53p[A]; break;
    case 7: { alu_sub_f(z, v, 0); F = (F & ~(FY | FX)) | (v & (FY | FX)); break; }
    }
}
static uint8_t inc8(Z80 *z, uint8_t v) {
    uint8_t r = v + 1;
    F = (F & FC) | sz53[r] | ((r & 0x0F) == 0 ? FH : 0) | (v == 0x7F ? FP : 0);
    return r;
}
static uint8_t dec8(Z80 *z, uint8_t v) {
    uint8_t r = v - 1;
    F = (F & FC) | sz53[r] | ((v & 0x0F) == 0 ? FH : 0) | (v == 0x80 ? FP : 0) | FN;
    return r;
}
static uint16_t add16(Z80 *z, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + b;
    z->wz.w = a + 1;
    F = (F & (FS | FZ | FP)) | ((r >> 8) & (FY | FX)) | ((((a ^ b ^ r) >> 8) & 0x10) ? FH : 0) | (r > 0xFFFF ? FC : 0);
    return (uint16_t)r;
}
static uint16_t adc16(Z80 *z, uint16_t a, uint16_t b) {
    uint32_t c = F & FC, r = (uint32_t)a + b + c;
    uint16_t res = (uint16_t)r;
    z->wz.w = a + 1;
    F = ((res >> 8) & (FS | FY | FX)) | (res == 0 ? FZ : 0) | ((((a ^ b ^ res) >> 8) & 0x10) ? FH : 0) |
        ((~(a ^ b) & (a ^ res) & 0x8000) ? FP : 0) | (r > 0xFFFF ? FC : 0);
    return res;
}
static uint16_t sbc16(Z80 *z, uint16_t a, uint16_t b) {
    uint32_t c = F & FC, r = (uint32_t)a - b - c;
    uint16_t res = (uint16_t)r;
    z->wz.w = a + 1;
    F = ((res >> 8) & (FS | FY | FX)) | (res == 0 ? FZ : 0) | ((((a ^ b ^ res) >> 8) & 0x10) ? FH : 0) |
        (((a ^ b) & (a ^ res) & 0x8000) ? FP : 0) | FN | (r > 0xFFFF ? FC : 0);
    return res;
}

static uint8_t rot_shift(Z80 *z, int op, uint8_t v) {
    uint8_t c, r;
    switch (op) {
    case 0: c = v >> 7; r = (v << 1) | c; break;                      /* RLC */
    case 1: c = v & 1; r = (v >> 1) | (c << 7); break;                /* RRC */
    case 2: c = v >> 7; r = (v << 1) | (F & FC); break;               /* RL */
    case 3: c = v & 1; r = (v >> 1) | ((F & FC) << 7); break;         /* RR */
    case 4: c = v >> 7; r = v << 1; break;                            /* SLA */
    case 5: c = v & 1; r = (v >> 1) | (v & 0x80); break;              /* SRA */
    case 6: c = v >> 7; r = (v << 1) | 1; break;                      /* SLL (undocumented) */
    default: c = v & 1; r = v >> 1; break;                            /* SRL */
    }
    F = sz53p[r] | c;
    return r;
}

static uint8_t daa(Z80 *z) {
    uint8_t a = A, add = 0, carry = F & FC;
    if ((F & FH) || (a & 0x0F) > 9) add |= 0x06;
    if (carry || a > 0x99) { add |= 0x60; carry = FC; }
    uint8_t res = (F & FN) ? a - add : a + add;
    uint8_t h = (F & FN) ? ((F & FH) && (a & 0x0F) < 6 ? FH : 0) : ((a & 0x0F) > 9 ? FH : 0);
    F = sz53p[res] | (F & FN) | h | carry;
    return res;
}

static bool cond(Z80 *z, int cc) {
    switch (cc) {
    case 0: return !(F & FZ); case 1: return F & FZ;
    case 2: return !(F & FC); case 3: return F & FC;
    case 4: return !(F & FP); case 5: return F & FP;
    case 6: return !(F & FS); default: return F & FS;
    }
}

static inline uint8_t *reg8(Z80 *z, int r) {
    switch (r) { case 0: return &z->bc.h; case 1: return &z->bc.l; case 2: return &z->de.h; case 3: return &z->de.l;
                 case 4: return &z->hl.h; case 5: return &z->hl.l; default: return &z->af.h; }
}
static inline uint8_t *reg8x(Z80 *z, int r, Z80Pair *ixy) {
    if (ixy && r == 4) return &ixy->h;
    if (ixy && r == 5) return &ixy->l;
    return reg8(z, r);
}
static inline Z80Pair *rp(Z80 *z, int p, Z80Pair *hlp) {
    switch (p) { case 0: return &z->bc; case 1: return &z->de; case 2: return hlp; default: return &z->sp; }
}
static inline Z80Pair *rp2(Z80 *z, int p, Z80Pair *hlp) {
    switch (p) { case 0: return &z->bc; case 1: return &z->de; case 2: return hlp; default: return &z->af; }
}

static uint8_t bit_op(Z80 *z, int y, uint8_t v, uint8_t xy_src) {
    F = (F & FC) | FH | (v & (1 << y) ? 0 : (FZ | FP)) | (xy_src & (FY | FX)) | ((y == 7 && (v & 0x80)) ? FS : 0);
    return v;
}

static int exec_cb(Z80 *z, bool indexed, uint16_t addr, uint8_t op) {
    int x = op >> 6, y = (op >> 3) & 7, r = op & 7;
    if (!indexed && r != 6) {
        uint8_t *p = reg8(z, r);
        if (x == 0) *p = rot_shift(z, y, *p);
        else if (x == 1) bit_op(z, y, *p, *p);
        else if (x == 2) *p &= ~(1 << y);
        else *p |= 1 << y;
        return z80_nops_cb[op];
    }
    uint8_t v = RD(addr);
    if (x == 1) { bit_op(z, y, v, indexed ? addr >> 8 : z->wz.h); return z80_nops_cb[op]; }
    if (x == 0) v = rot_shift(z, y, v); else if (x == 2) v &= ~(1 << y); else v |= 1 << y;
    WR(addr, v);
    if (indexed && r != 6) *reg8(z, r) = v;
    return z80_nops_cb[op];
}

static void block_in_out_flags(Z80 *z, uint8_t val, int k, uint8_t b) {
    F = sz53[b] | ((val & 0x80) ? FN : 0) | (k > 255 ? (FH | FC) : 0) | (parity_tab[(k & 7) ^ b] );
}

static int exec_ed(Z80 *z, uint8_t op) {
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    int nops = z80_nops_ed[op];
    if (x == 1) {
        switch (zz) {
        case 0: { uint8_t v = z->in(z->ctx, z->bc.w); z->wz.w = z->bc.w + 1; F = (F & FC) | sz53p[v]; if (y != 6) *reg8(z, y) = v; break; }
        case 1: z->out(z->ctx, z->bc.w, y == 6 ? 0 : *reg8(z, y)); z->wz.w = z->bc.w + 1; break;
        case 2: {
            Z80Pair *r = rp(z, p, &z->hl);
            z->hl.w = q ? adc16(z, z->hl.w, r->w) : sbc16(z, z->hl.w, r->w);
        } break;
        case 3: { uint16_t a = fetch16(z); Z80Pair *r = rp(z, p, &z->hl);
            if (q) r->w = rd16(z, a); else wr16(z, a, r->w);
            z->wz.w = a + 1; break; }
        case 4: { uint8_t a = A; A = 0; A = alu_sub_f(z, a, 0); break; }
        case 5: z->iff1 = z->iff2; z->pc = pop16(z); z->wz.w = z->pc; z->retn_hit = true; break;
        case 6: z->im = (y & 3) < 2 ? 0 : (y & 3) - 1; break;
        case 7:
            switch (y) {
            case 0: z->i = A; break;
            case 1: z->r = A; break;
            case 2: A = z->i; F = (F & FC) | sz53[A] | (z->iff2 ? FP : 0); break;
            case 3: A = z->r; F = (F & FC) | sz53[A] | (z->iff2 ? FP : 0); break;
            case 4: { /* RRD */ uint8_t m = RD(z->hl.w); uint8_t n = (A << 4) | (m >> 4); A = (A & 0xF0) | (m & 0x0F); WR(z->hl.w, n); F = (F & FC) | sz53p[A]; z->wz.w = z->hl.w + 1; break; }
            case 5: { /* RLD */ uint8_t m = RD(z->hl.w); uint8_t n = (m << 4) | (A & 0x0F); A = (A & 0xF0) | (m >> 4); WR(z->hl.w, n); F = (F & FC) | sz53p[A]; z->wz.w = z->hl.w + 1; break; }
            default: break;
            }
            break;
        }
        return nops;
    }
    if (x == 2 && y >= 4 && zz < 4) {
        bool inc = !(y & 1), rep = y >= 6;
        switch (zz) {
        case 0: { /* LDI/LDD */
            uint8_t v = RD(z->hl.w); WR(z->de.w, v);
            z->hl.w += inc ? 1 : -1; z->de.w += inc ? 1 : -1; z->bc.w--;
            uint8_t n = v + A;
            F = (F & (FS | FZ | FC)) | ((n & 2) ? FY : 0) | (n & FX) | (z->bc.w ? FP : 0);
            if (rep && z->bc.w) { z->pc -= 2; z->wz.w = z->pc + 1; nops++; }
            break; }
        case 1: { /* CPI/CPD */
            uint8_t v = RD(z->hl.w); uint8_t res = A - v; uint8_t h = (A ^ v ^ res) & 0x10;
            z->hl.w += inc ? 1 : -1; z->bc.w--; z->wz.w += inc ? 1 : -1;
            uint8_t n = res - (h ? 1 : 0);
            F = (F & FC) | (res & FS) | (res == 0 ? FZ : 0) | (h ? FH : 0) | ((n & 2) ? FY : 0) | (n & FX) | (z->bc.w ? FP : 0) | FN;
            if (rep && z->bc.w && res) { z->pc -= 2; z->wz.w = z->pc + 1; nops++; }
            break; }
        case 2: { /* INI/IND */
            uint8_t v = z->in(z->ctx, z->bc.w); WR(z->hl.w, v);
            z->bc.h--; int k = v + ((z->bc.l + (inc ? 1 : -1)) & 0xFF);
            z->hl.w += inc ? 1 : -1;
            block_in_out_flags(z, v, k, z->bc.h);
            if (rep && z->bc.h) { z->pc -= 2; nops++; }
            break; }
        default: { /* OUTI/OUTD */
            uint8_t v = RD(z->hl.w); z->bc.h--;
            z->out(z->ctx, z->bc.w, v);
            z->hl.w += inc ? 1 : -1; int k = v + z->hl.l;
            block_in_out_flags(z, v, k, z->bc.h);
            if (rep && z->bc.h) { z->pc -= 2; nops++; }
            break; }
        }
    }
    return nops;
}

int z80_step(Z80 *z, bool irq) {
    if (!tables_ready) init_tables();
    z->brk_hit = false; z->int_ack = false; z->retn_hit = false;
    if (z->nmi_pending) {
        z->nmi_pending = false; z->halted = false; z->iff1 = false; z->r = (z->r & 0x80) | ((z->r + 1) & 0x7F);
        push16(z, z->pc); z->pc = 0x66; z->wz.w = 0x66;
        return z->turbo ? 1 : 3;
    }
    if (irq && z->iff1 && z->ei_delay == 0) {
        z->int_ack = true; z->halted = false; z->iff1 = z->iff2 = false; z->r = (z->r & 0x80) | ((z->r + 1) & 0x7F);
        if (z->im < 2) { push16(z, z->pc); z->pc = 0x38; z->wz.w = 0x38; return z->turbo ? 1 : 5; }
        push16(z, z->pc);
        uint16_t v = rd16(z, (z->i << 8) | z->int_vector);
        z->pc = v; z->wz.w = v;
        return z->turbo ? 1 : 6;
    }
    int delay = z->ei_delay;
    if (z->halted) { z->r = (z->r & 0x80) | ((z->r + 1) & 0x7F); if (delay) z->ei_delay--; return 1; }

    uint16_t start_pc = z->pc;
    z->r = (z->r & 0x80) | ((z->r + 1) & 0x7F);
    uint8_t op = fetch8(z);
    Z80Pair *ixy = NULL;
    int nops = 0;
    bool is_out = false;
    while (op == 0xDD || op == 0xFD) {
        ixy = op == 0xDD ? &z->ix : &z->iy;
        nops++;
        z->r = (z->r & 0x80) | ((z->r + 1) & 0x7F);
        op = fetch8(z);
    }
    Z80Pair *hlp = ixy ? ixy : &z->hl;
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;

    if (op == 0xCB) {
        if (ixy) {
            int8_t d = (int8_t)fetch8(z); uint8_t cbop = fetch8(z);
            uint16_t addr = ixy->w + d; z->wz.w = addr;
            exec_cb(z, true, addr, cbop);
            nops = ((cbop >> 6) == 1) ? 6 : 7;
        } else {
            z->r = (z->r & 0x80) | ((z->r + 1) & 0x7F);
            uint8_t cbop = fetch8(z);
            nops += 1 + exec_cb(z, false, z->hl.w, cbop);
        }
        goto done;
    }
    if (op == 0xED) {
        z->r = (z->r & 0x80) | ((z->r + 1) & 0x7F);
        uint8_t edop = fetch8(z);
        if (z->brk_enabled && edop == 0xFF) { z->pc = start_pc; z->brk_hit = true; return 1; }
        if (edop >= 0x40 && edop < 0x80 && (edop & 7) == 1) is_out = true;
        nops = 1 + exec_ed(z, edop);
        if (edop >= 0xA0 && (edop & 7) == 3 && edop >= 0xA3) is_out = true;
        goto done;
    }

    nops += z80_nops_main[op];
    switch (x) {
    case 0:
        switch (zz) {
        case 0:
            switch (y) {
            case 0: break;
            case 1: { Z80Pair t = z->af; z->af = z->af2; z->af2 = t; break; }
            case 2: { int8_t d = (int8_t)fetch8(z); z->bc.h--; if (z->bc.h) { z->pc += d; z->wz.w = z->pc; nops++; } break; }
            case 3: { int8_t d = (int8_t)fetch8(z); z->pc += d; z->wz.w = z->pc; nops++; break; }
            default: { int8_t d = (int8_t)fetch8(z); if (cond(z, y - 4)) { z->pc += d; z->wz.w = z->pc; nops++; } break; }
            }
            break;
        case 1:
            if (!q) rp(z, p, hlp)->w = fetch16(z);
            else hlp->w = add16(z, hlp->w, rp(z, p, hlp)->w);
            break;
        case 2:
            switch (y) {
            case 0: WR(z->bc.w, A); z->wz.l = z->bc.l + 1; z->wz.h = A; break;
            case 1: A = RD(z->bc.w); z->wz.w = z->bc.w + 1; break;
            case 2: WR(z->de.w, A); z->wz.l = z->de.l + 1; z->wz.h = A; break;
            case 3: A = RD(z->de.w); z->wz.w = z->de.w + 1; break;
            case 4: { uint16_t a = fetch16(z); wr16(z, a, hlp->w); z->wz.w = a + 1; break; }
            case 5: { uint16_t a = fetch16(z); hlp->w = rd16(z, a); z->wz.w = a + 1; break; }
            case 6: { uint16_t a = fetch16(z); WR(a, A); z->wz.l = a + 1; z->wz.h = A; break; }
            default: { uint16_t a = fetch16(z); A = RD(a); z->wz.w = a + 1; break; }
            }
            break;
        case 3: { Z80Pair *r = rp(z, p, hlp); r->w += q ? -1 : 1; break; }
        case 4: case 5:
            if (y == 6) {
                uint16_t a = ixy ? (uint16_t)(ixy->w + (int8_t)fetch8(z)) : z->hl.w;
                if (ixy) { nops += 2; z->wz.w = a; }
                uint8_t v = RD(a); WR(a, zz == 4 ? inc8(z, v) : dec8(z, v));
            } else {
                uint8_t *r = reg8x(z, y, ixy);
                *r = zz == 4 ? inc8(z, *r) : dec8(z, *r);
            }
            break;
        case 6:
            if (y == 6) {
                uint16_t a = ixy ? (uint16_t)(ixy->w + (int8_t)fetch8(z)) : z->hl.w;
                if (ixy) { nops += 2; z->wz.w = a; }
                WR(a, fetch8(z));
            } else *reg8x(z, y, ixy) = fetch8(z);
            break;
        default:
            switch (y) {
            case 0: A = (A << 1) | (A >> 7); F = (F & (FS | FZ | FP)) | (A & (FY | FX)) | (A & 1); break;
            case 1: F = (F & (FS | FZ | FP)) | (A & 1); A = (A >> 1) | (A << 7); F |= A & (FY | FX); break;
            case 2: { uint8_t c = A >> 7; A = (A << 1) | (F & FC); F = (F & (FS | FZ | FP)) | (A & (FY | FX)) | c; break; }
            case 3: { uint8_t c = A & 1; A = (A >> 1) | ((F & FC) << 7); F = (F & (FS | FZ | FP)) | (A & (FY | FX)) | c; break; }
            case 4: A = daa(z); break;
            case 5: A = ~A; F = (F & (FS | FZ | FP | FC)) | (A & (FY | FX)) | FH | FN; break;
            case 6: F = (F & (FS | FZ | FP)) | (A & (FY | FX)) | FC; break;
            default: F = (F & (FS | FZ | FP)) | ((F & FC) ? FH : FC) | (A & (FY | FX)); break;
            }
        }
        break;
    case 1:
        if (op == 0x76) { z->halted = true; break; }
        if (y == 6 || zz == 6) {
            uint16_t a = z->hl.w;
            if (ixy) { a = ixy->w + (int8_t)fetch8(z); nops += 2; z->wz.w = a; }
            if (y == 6) WR(a, *reg8(z, zz)); else *reg8(z, y) = RD(a);
        } else *reg8x(z, y, ixy) = *reg8x(z, zz, ixy);
        break;
    case 2:
        if (zz == 6) {
            uint16_t a = z->hl.w;
            if (ixy) { a = ixy->w + (int8_t)fetch8(z); nops += 2; z->wz.w = a; }
            alu(z, y, RD(a));
        } else alu(z, y, *reg8x(z, zz, ixy));
        break;
    default:
        switch (zz) {
        case 0: if (cond(z, y)) { z->pc = pop16(z); z->wz.w = z->pc; nops += 2; } break;
        case 1:
            if (!q) { Z80Pair *r = rp2(z, p, hlp); r->w = pop16(z); }
            else switch (p) {
                case 0: z->pc = pop16(z); z->wz.w = z->pc; break;
                case 1: { Z80Pair t = z->bc; z->bc = z->bc2; z->bc2 = t; t = z->de; z->de = z->de2; z->de2 = t; t = z->hl; z->hl = z->hl2; z->hl2 = t; break; }
                case 2: z->pc = hlp->w; break;
                default: z->sp.w = hlp->w; break;
            }
            break;
        case 2: { uint16_t a = fetch16(z); z->wz.w = a; if (cond(z, y)) z->pc = a; break; }
        case 3:
            switch (y) {
            case 0: z->pc = fetch16(z); z->wz.w = z->pc; break;
            case 2: { uint8_t n = fetch8(z); z->out(z->ctx, (A << 8) | n, A); z->wz.w = ((A) << 8) | ((n + 1) & 0xFF); is_out = true; break; }
            case 3: { uint8_t n = fetch8(z); uint16_t port = (A << 8) | n; A = z->in(z->ctx, port); z->wz.w = port + 1; break; }
            case 4: { uint16_t t = rd16(z, z->sp.w); wr16(z, z->sp.w, hlp->w); hlp->w = t; z->wz.w = t; break; }
            case 5: { Z80Pair t = z->de; z->de = z->hl; z->hl = t; break; }
            case 6: z->iff1 = z->iff2 = false; break;
            case 7: z->iff1 = z->iff2 = true; z->ei_delay = 1; break;
            default: break; /* CB handled above */
            }
            break;
        case 4: { uint16_t a = fetch16(z); z->wz.w = a; if (cond(z, y)) { push16(z, z->pc); z->pc = a; nops += 2; } break; }
        case 5:
            if (!q) push16(z, rp2(z, p, hlp)->w);
            else if (p == 0) { uint16_t a = fetch16(z); push16(z, z->pc); z->pc = a; z->wz.w = a; }
            break;
        case 6: alu(z, y, fetch8(z)); break;
        default: push16(z, z->pc); z->pc = y * 8; z->wz.w = z->pc; break;
        }
    }
done:
    z->instructions++;
    if (delay) z->ei_delay--;
    if (z->turbo) return is_out ? 2 : 1;
    return nops;
}
