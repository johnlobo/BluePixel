#include "cpc.h"
#include "snapshot.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static uint8_t *block64(CPC *c, int i) { return c->ram + (size_t)i * 65536; }
static int blocks_available(CPC *c) { return (int)(c->ram_size / 65536); }

/* ---- RLE (0xE5 escape) ---- */
static size_t rle_decode(const uint8_t *s, size_t n, uint8_t *out, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n && o < cap) {
        uint8_t b = s[i++];
        if (b != 0xE5) { out[o++] = b; continue; }
        if (i >= n) break;
        uint8_t cnt = s[i++];
        if (cnt == 0) { out[o++] = 0xE5; continue; }
        if (i >= n) break;
        uint8_t v = s[i++];
        while (cnt-- && o < cap) out[o++] = v;
    }
    return o;
}

static size_t rle_encode(const uint8_t *s, size_t n, uint8_t *out) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t v = s[i]; size_t run = 1;
        while (i + run < n && s[i + run] == v && run < 255) run++;
        if (run >= 3 || v == 0xE5) {
            if (v == 0xE5 && run < 3) { for (size_t k = 0; k < run; k++) { out[o++] = 0xE5; out[o++] = 0; } }
            else { out[o++] = 0xE5; out[o++] = (uint8_t)run; out[o++] = v; }
            i += run;
        } else { for (size_t k = 0; k < run; k++) out[o++] = v; i += run; }
    }
    return o;
}

bool cpc_snapshot_load(CPC *c, const uint8_t *d, size_t n) {
    if (n < 0x100 || memcmp(d, "MV - SNA", 8)) return false;
    int ver = d[0x10];
    Z80 *z = &c->z80;
    z->af.l = d[0x11]; z->af.h = d[0x12]; z->bc.l = d[0x13]; z->bc.h = d[0x14]; z->de.l = d[0x15]; z->de.h = d[0x16];
    z->hl.l = d[0x17]; z->hl.h = d[0x18]; z->r = d[0x19]; z->i = d[0x1A]; z->iff1 = d[0x1B] & 1; z->iff2 = d[0x1C] & 1;
    z->ix.l = d[0x1D]; z->ix.h = d[0x1E]; z->iy.l = d[0x1F]; z->iy.h = d[0x20];
    z->sp.l = d[0x21]; z->sp.h = d[0x22]; z->pc = d[0x23] | (d[0x24] << 8); z->im = d[0x25];
    z->af2.l = d[0x26]; z->af2.h = d[0x27]; z->bc2.l = d[0x28]; z->bc2.h = d[0x29]; z->de2.l = d[0x2A]; z->de2.h = d[0x2B];
    z->hl2.l = d[0x2C]; z->hl2.h = d[0x2D]; z->halted = false; z->ei_delay = 0;
    c->ga_pen = (d[0x2E] & 0x10) ? 16 : (d[0x2E] & 0x0F);   /* as the gate array decodes it: 16 = border; never past ga_palette[16] */
    for (int i = 0; i < 17; i++) c->ga_palette[i] = d[0x2F + i] & 0x1F;
    c->ga_mode = d[0x40] & 3; c->lower_rom_en = !(d[0x40] & 4); c->upper_rom_en = !(d[0x40] & 8);
    c->ga_ram = d[0x41];
    c->crtc.sel = d[0x42] & 31;
    for (int i = 0; i < 18; i++) c->crtc.r[i] = d[0x43 + i];
    c->upper_rom_sel = d[0x55];
    c->ppi_a = d[0x56]; c->ppi_b = d[0x57]; c->ppi_c = d[0x58]; c->ppi_ctl = d[0x59];
    c->tape_motor = (c->ppi_c & 0x10) != 0;
    c->psg.sel = d[0x5A] & 15;
    psg_reset(&c->psg);
    for (int i = 0; i < 16; i++) psg_write_reg(&c->psg, i, d[0x5B + i]);
    c->psg.sel = d[0x5A] & 15;
    c->psg_ctl = (c->ppi_c >> 6) & 3;
    int kb = d[0x6B] | (d[0x6C] << 8);
    memset(c->ram, 0, c->ram_size);
    size_t pos = 0x100;
    if (ver < 3 || kb) {
        size_t bytes = (size_t)kb * 1024;
        if (pos + bytes > n) bytes = n - pos;
        if (bytes > c->ram_size) bytes = c->ram_size;
        memcpy(c->ram, d + pos, bytes);
        pos += bytes;
    }
    if (ver >= 3) {
        /* header continuation */
        c->crtc.type = n > 0xA4 ? d[0xA4] : c->crtc.type;
        if (n > 0xB4) {
            c->crtc.hcc = d[0xA9]; c->crtc.vlc = d[0xAB]; c->crtc.vcc = d[0xAC] & 127; c->crtc.vtac = d[0xAD];
            c->ga_vsync_delay = d[0xB2]; c->ga_irq_counter = d[0xB3]; c->irq_line = d[0xB4] != 0;
        }
        while (pos + 8 <= n) {
            const uint8_t *ch = d + pos; uint32_t sz = rd32(ch + 4); pos += 8;
            if (pos + sz > n) sz = (uint32_t)(n - pos);
            if (!memcmp(ch, "MEM", 3) && ch[3] >= '0' && ch[3] <= '8') {
                int blk = ch[3] - '0';
                if (blk < blocks_available(c)) {
                    uint8_t *dst = block64(c, blk);
                    if (sz == 65536) memcpy(dst, d + pos, 65536); else rle_decode(d + pos, sz, dst, 65536);
                }
            }
            pos += sz;
        }
    }
    c->crtc.ma_row = c->crtc.ma_start = ((c->crtc.r[12] << 8) | c->crtc.r[13]) & 0x3FFF;
    c->crtc.hde = true; c->crtc.vde = c->crtc.vcc < c->crtc.r[6];
    c->crtc.vsync = false; c->crtc.hsync = false; c->crtc.in_adjust = false;
    c->beam_x = 0; c->beam_y = -37;
    cpc_update_mapping(c);
    return true;
}

uint8_t *cpc_snapshot_save(CPC *c, int version, int mem_kb, bool compress, size_t *out_size) {
    if (version < 1 || version > 3) version = 3;
    size_t cap = 0x100 + c->ram_size * 2 + 4096;
    uint8_t *o = calloc(1, cap);
    memcpy(o, "MV - SNA", 8);
    o[0x10] = (uint8_t)version;
    Z80 *z = &c->z80;
    o[0x11] = z->af.l; o[0x12] = z->af.h; o[0x13] = z->bc.l; o[0x14] = z->bc.h; o[0x15] = z->de.l; o[0x16] = z->de.h;
    o[0x17] = z->hl.l; o[0x18] = z->hl.h; o[0x19] = z->r; o[0x1A] = z->i; o[0x1B] = z->iff1; o[0x1C] = z->iff2;
    o[0x1D] = z->ix.l; o[0x1E] = z->ix.h; o[0x1F] = z->iy.l; o[0x20] = z->iy.h;
    o[0x21] = z->sp.l; o[0x22] = z->sp.h; uint16_t pc = z->pc; o[0x23] = pc & 255; o[0x24] = pc >> 8; o[0x25] = z->im;
    o[0x26] = z->af2.l; o[0x27] = z->af2.h; o[0x28] = z->bc2.l; o[0x29] = z->bc2.h; o[0x2A] = z->de2.l; o[0x2B] = z->de2.h;
    o[0x2C] = z->hl2.l; o[0x2D] = z->hl2.h;
    o[0x2E] = c->ga_pen;
    for (int i = 0; i < 17; i++) o[0x2F + i] = c->ga_palette[i] | 0x40;
    o[0x40] = (c->ga_mode & 3) | (c->lower_rom_en ? 0 : 4) | (c->upper_rom_en ? 0 : 8) | 0x80;
    o[0x41] = c->ga_ram;
    o[0x42] = c->crtc.sel;
    for (int i = 0; i < 18; i++) o[0x43 + i] = c->crtc.r[i];
    o[0x55] = c->upper_rom_sel;
    o[0x56] = c->ppi_a; o[0x57] = c->ppi_b; o[0x58] = c->ppi_c; o[0x59] = c->ppi_ctl;
    o[0x5A] = c->psg.sel;
    for (int i = 0; i < 16; i++) o[0x5B + i] = c->psg.regs[i];
    int total_blocks = blocks_available(c);
    size_t pos = 0x100;
    if (version < 3) {
        int kb = mem_kb == 128 ? 128 : 64;
        o[0x6B] = kb & 255; o[0x6C] = kb >> 8;
        memcpy(o + pos, c->ram, (size_t)kb * 1024); pos += (size_t)kb * 1024;
    } else {
        int kb = 0; o[0x6B] = 0; o[0x6C] = 0; (void)kb;
        for (int b = 0; b < total_blocks; b++) {
            if (mem_kb && (b + 1) * 64 > (mem_kb > 64 ? mem_kb : 64) && b > 0) break;
            uint8_t *blk = block64(c, b);
            bool any = false; for (int i = 0; i < 65536 && !any; i++) any = blk[i] != 0;
            if (b > 1 && !any) continue;
            memcpy(o + pos, "MEM0", 4); o[pos + 3] = '0' + b;
            if (compress) {
                uint8_t *tmp = malloc(65536 * 2);
                size_t cs = rle_encode(blk, 65536, tmp);
                if (cs < 65536) { wr32(o + pos + 4, (uint32_t)cs); memcpy(o + pos + 8, tmp, cs); pos += 8 + cs; free(tmp); continue; }
                free(tmp);
            }
            wr32(o + pos + 4, 65536); memcpy(o + pos + 8, blk, 65536); pos += 8 + 65536;
        }
    }
    o[0x6D] = c->cfg.plus ? 4 : (c->model == CPC_MODEL_464 ? 0 : c->model == CPC_MODEL_664 ? 1 : 2);
    if (version >= 3) {
        o[0xA4] = c->crtc.type;
        o[0xA9] = (uint8_t)c->crtc.hcc; o[0xAB] = (uint8_t)c->crtc.vlc; o[0xAC] = (uint8_t)c->crtc.vcc; o[0xAD] = (uint8_t)c->crtc.vtac;
        o[0xB2] = (uint8_t)c->ga_vsync_delay; o[0xB3] = (uint8_t)c->ga_irq_counter; o[0xB4] = c->irq_line;
        /* 0x100 header is followed by chunks (already laid out) */
    }
    *out_size = pos;
    return o;
}
