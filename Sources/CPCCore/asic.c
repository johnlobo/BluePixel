#include "cpc.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t unlock_seq[17] = {0xFF, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62, 0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD, 0xEE};

static uint32_t rgb12(uint16_t w) {      /* word 0x0GRB */
    uint32_t g = ((w >> 8) & 15) * 17, r = ((w >> 4) & 15) * 17, b = (w & 15) * 17;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void apply_monitor(CPC *c, uint32_t *rgb) {
    if (c->cfg.monitor == CPC_MONITOR_COLOUR) return;
    int r = (*rgb >> 16) & 255, g = (*rgb >> 8) & 255, b = *rgb & 255;
    int l = (int)(0.299 * r + 0.587 * g + 0.114 * b);
    *rgb = 0xFF000000u | (c->cfg.monitor == CPC_MONITOR_GREEN ? ((l / 4) << 16 | l << 8 | l / 4) : (l << 16 | l << 8 | l));
}

void asic_reset(CPC *c) {
    CPCAsic *a = &c->asic;
    memset(a, 0, sizeof *a);
    for (int i = 0; i < 17; i++) { a->pal12[i] = 0; a->pal_rgb[i] = 0xFF000000u; }
}

void asic_unlock_feed(CPC *c, uint8_t v) {
    CPCAsic *a = &c->asic;
    if (!c->cfg.plus) return;
    if (a->unlocked) {
        /* ordinary CRTC register selects keep working; 0xA5 re-locks the ASIC */
        if (v == 0xA5) { a->unlocked = false; a->unlock_pos = 0; a->page_visible = false; cpc_update_mapping(c); }
        return;
    }
    if (v == unlock_seq[a->unlock_pos]) { if (++a->unlock_pos == 17) a->unlocked = true; }
    else a->unlock_pos = (v == 0xFF) ? 1 : 0;
}

void asic_rmr2(CPC *c, uint8_t v) {
    CPCAsic *a = &c->asic;
    a->rmr2 = v;
    int map = (v >> 3) & 3;
    a->cart_bank = v & 7;
    a->page_visible = (map == 3);
    a->lower_base = map == 3 ? 0 : map;
    cpc_update_mapping(c);
}

void asic_ga_palette(CPC *c, int pen, uint8_t hw) {
    CPCAsic *a = &c->asic;
    static const uint8_t lvl[3] = {0, 6, 15};
    static const uint8_t rgbtab[32][3] = {                       /* R,G,B level index for hardware colour */
        {1,1,1}, {1,1,1}, {0,2,1}, {2,2,1}, {0,0,1}, {2,0,1}, {0,1,1}, {2,1,1}, {2,0,1}, {2,2,1}, {2,2,0}, {2,2,2}, {2,0,0}, {2,0,2}, {2,1,0}, {2,1,2},
        {0,0,1}, {0,2,1}, {0,2,0}, {0,2,2}, {0,0,0}, {0,0,2}, {0,1,0}, {0,1,2}, {1,0,1}, {1,2,1}, {1,2,0}, {1,2,2}, {1,0,0}, {1,0,2}, {1,1,0}, {1,1,2}};
    const uint8_t *t = rgbtab[hw & 31];
    uint16_t w = (uint16_t)((lvl[t[1]] << 8) | (lvl[t[0]] << 4) | lvl[t[2]]);
    a->pal12[pen] = w; a->pal_rgb[pen] = rgb12(w); apply_monitor(c, &a->pal_rgb[pen]);
    a->ram[0x2400 + pen * 2] = (uint8_t)w; a->ram[0x2400 + pen * 2 + 1] = (uint8_t)(w >> 8);
}

uint8_t asic_read(CPC *c, uint16_t off) {
    CPCAsic *a = &c->asic;
    if (off >= 0x2808 && off < 0x2810) return 0x3F;
    if (off == 0x2C0F) return a->dcsr | a->icsr;
    return a->ram[off & 0x3FFF];
}

void asic_write(CPC *c, uint16_t off, uint8_t v) {
    CPCAsic *a = &c->asic;
    off &= 0x3FFF;
    a->ram[off] = v;
    if (off >= 0x2400 && off < 0x2422) {
        int i = (off - 0x2400) >> 1; uint16_t w = (uint16_t)(a->ram[0x2400 + i * 2] | ((a->ram[0x2400 + i * 2 + 1] & 15) << 8));
        a->pal12[i] = w; a->pal_rgb[i] = rgb12(w); apply_monitor(c, &a->pal_rgb[i]);
    } else if (off >= 0x2420 && off < 0x2440) {
        int i = (off - 0x2420) >> 1; uint16_t w = (uint16_t)(a->ram[0x2420 + i * 2] | ((a->ram[0x2420 + i * 2 + 1] & 15) << 8));
        a->spal_rgb[i] = rgb12(w); apply_monitor(c, &a->spal_rgb[i]);
    } else switch (off) {
    case 0x2800: a->pri = v; break;
    case 0x2801: a->splt = v; break;
    case 0x2802: a->ssa = (uint16_t)((a->ssa & 0x00FF) | (v << 8)); break;
    case 0x2803: a->ssa = (uint16_t)((a->ssa & 0xFF00) | v); break;
    case 0x2804: a->sscr = v; break;
    case 0x2805: a->ivr = v; break;
    case 0x2C00: case 0x2C04: case 0x2C08: { int ch = (off - 0x2C00) >> 2; a->dma[ch].addr = (uint16_t)((a->dma[ch].addr & 0xFF00) | v); break; }
    case 0x2C01: case 0x2C05: case 0x2C09: { int ch = (off - 0x2C00) >> 2; a->dma[ch].addr = (uint16_t)((a->dma[ch].addr & 0x00FF) | (v << 8)); break; }
    case 0x2C02: case 0x2C06: case 0x2C0A: a->dma[(off - 0x2C00) >> 2].prescale = v; break;
    case 0x2C0F:
        a->icsr &= ~(v & 0x70);                       /* writing 1 to an interrupt bit clears it */
        a->dcsr = v & 7;
        for (int i = 0; i < 3; i++) { if (v & (1 << i)) { a->dma[i].stopped = false; a->dma[i].pause = 0; a->dma[i].tick = 0; } }
        break;
    default: break;
    }
}

void cpc_remove_cartridge(CPC *c) { free(c->cartridge); c->cartridge = NULL; c->cartridge_banks = 0; cpc_update_mapping(c); }

bool cpc_load_cartridge(CPC *c, const uint8_t *d, size_t n) {
    uint8_t *banks = calloc(32, 16384); int nb = 0;
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "AMS!", 4)) {
        size_t p = 12;
        while (p + 8 <= n) {
            uint32_t sz = d[p + 4] | (d[p + 5] << 8) | (d[p + 6] << 16) | ((uint32_t)d[p + 7] << 24);
            if (!memcmp(d + p, "cb", 2) && d[p + 2] >= '0' && d[p + 2] <= '9' && d[p + 3] >= '0' && d[p + 3] <= '9') {
                int b = (d[p + 2] - '0') * 10 + (d[p + 3] - '0');
                if (b < 32) { size_t cp = sz > 16384 ? 16384 : sz; if (p + 8 + cp <= n) memcpy(banks + (size_t)b * 16384, d + p + 8, cp); if (b + 1 > nb) nb = b + 1; }
            }
            p += 8 + sz + (sz & 1);
        }
    } else if (n >= 16384 && n <= 32 * 16384) {              /* .CRT: raw banks */
        memcpy(banks, d, n); nb = (int)((n + 16383) / 16384);
    }
    if (!nb) { free(banks); return false; }
    free(c->cartridge); c->cartridge = banks; c->cartridge_banks = nb;
    return true;
}

/* ---- per-scanline processing: DMA sound and programmable raster interrupt ---- */
static void dma_channel(CPC *c, int ch) {
    CPCAsic *a = &c->asic;
    if (!(a->dcsr & (1 << ch)) || a->dma[ch].stopped) return;
    if (a->dma[ch].pause > 0) { if (++a->dma[ch].tick > a->dma[ch].prescale) { a->dma[ch].tick = 0; a->dma[ch].pause--; } return; }
    for (int guard = 0; guard < 8; guard++) {
        uint16_t ad = a->dma[ch].addr;
        uint16_t ins = (uint16_t)(c->ram[ad] | (c->ram[(uint16_t)(ad + 1)] << 8));
        a->dma[ch].addr += 2;
        switch (ins >> 12) {
        case 0: psg_write_reg(&c->psg, (ins >> 8) & 15, ins & 255); break;
        case 1: a->dma[ch].pause = ins & 0xFFF; a->dma[ch].tick = 0; return;
        case 2: a->dma[ch].loop_addr = a->dma[ch].addr; a->dma[ch].loop_cnt = ins & 0xFFF; break;
        case 4:
            if (ins & 1) { if (a->dma[ch].loop_cnt > 0) { a->dma[ch].loop_cnt--; if (a->dma[ch].loop_cnt) a->dma[ch].addr = a->dma[ch].loop_addr; } }
            if (ins & 0x10) { a->icsr |= 0x10 << ch; c->irq_line = true; c->z80.int_vector = (a->ivr & 0xF8) | (4 - ch * 2); }
            if (ins & 0x20) { a->dma[ch].stopped = true; return; }
            break;
        default: break;
        }
    }
}

void asic_line(CPC *c) {
    CPCAsic *a = &c->asic;
    if (!c->cfg.plus || !a->unlocked) return;
    for (int i = 0; i < 3; i++) dma_channel(c, i);
    if (a->pri) {
        int line = c->crtc.vcc * (c->crtc.r[9] + 1) + c->crtc.vlc;
        if (line == a->pri) { a->icsr |= 0x80; c->irq_line = true; c->z80.int_vector = (a->ivr & 0xF8) | 6; }
    }
}
