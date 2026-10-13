#include "cpc.h"
#include "fdc.h"
#include "tape.h"
#include "cpc_keys.h"
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_MAX (576 * 1024)
#define AUDIO_CAP 16384
#define SPRITE_PIXEL_W 4

/* Hardware colour number (0..31) to RGB */
static const uint32_t hw_colours[32] = {
    0x808080, 0x808080, 0x00FF80, 0xFFFF80, 0x000080, 0xFF0080, 0x008080, 0xFF8080,
    0xFF0080, 0xFFFF80, 0xFFFF00, 0xFFFFFF, 0xFF0000, 0xFF00FF, 0xFF8000, 0xFF80FF,
    0x000080, 0x00FF80, 0x00FF00, 0x00FFFF, 0x000000, 0x0000FF, 0x008000, 0x0080FF,
    0x800080, 0x80FF80, 0x80FF00, 0x80FFFF, 0x800000, 0x8000FF, 0x808000, 0x8080FF,
};

static const uint8_t ram_cfg_table[8][4] = {{0, 1, 2, 3}, {0, 1, 2, 7}, {4, 5, 6, 7}, {0, 3, 2, 7},
                                      {0, 4, 2, 3}, {0, 5, 2, 3}, {0, 6, 2, 3}, {0, 7, 2, 3}};

void cpc_default_config(CPCConfig *cfg, CPCModel model) {
    memset(cfg, 0, sizeof *cfg);
    cfg->type = 0; cfg->plus = model >= CPC_MODEL_464PLUS; cfg->plus_ppi = cfg->plus;
    if (cfg->plus) cfg->type = 3;
    cfg->ram_kb = (model == CPC_MODEL_6128 || model == CPC_MODEL_6128PLUS) ? 128 : 64;
    cfg->fast_disc = true; cfg->joystick_enabled = true; cfg->monitor = CPC_MONITOR_COLOUR;
    cfg->linear_palette = false; cfg->sample_rate = 44100; cfg->stereo = true; cfg->volume = 0.75f;
    cfg->disc_sound = false; cfg->tape_sound = true; cfg->refresh_50hz = true;
}

/* 4-bit intensity to 8-bit, from WinApe.exe (0x5CCA14 linear, 0x5CCA44 non-linear). The gate array's
 * three levels are intensities 0, 6 and 15, so half-tones are #66 (linear, like the Plus) or #80. */
static const uint8_t level8[2][16] = {
    {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x00, 0x18, 0x30, 0x47, 0x5F, 0x72, 0x80, 0x8E, 0x9C, 0xAA, 0xB9, 0xC7, 0xD5, 0xE3, 0xF1, 0xFF},
};
uint8_t cpc_level8(const CPC *c, int intensity) { return level8[c->cfg.linear_palette ? 0 : 1][intensity & 15]; }

static void build_palette(CPC *c) {
    for (int i = 0; i < 32; i++) {
        uint32_t rgb = hw_colours[i];
        #define LVL(v) cpc_level8(c, (v) == 0xFF ? 15 : (v) ? 6 : 0)
        int r = LVL((rgb >> 16) & 255), g = LVL((rgb >> 8) & 255), b = LVL(rgb & 255);
        #undef LVL
        if (c->cfg.monitor != CPC_MONITOR_COLOUR) {
            int l = (int)(0.299 * r + 0.587 * g + 0.114 * b);
            if (c->cfg.monitor == CPC_MONITOR_GREEN) { r = l / 4; g = l; b = l / 4; } else { r = g = b = l; }
        }
        c->hw_rgb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

CPC *cpc_create(CPCModel model) {
    CPC *c = calloc(1, sizeof *c);
    c->model = model;
    cpc_default_config(&c->cfg, model);
    c->ram_size = RAM_MAX;
    c->ram = calloc(1, RAM_MAX);
    c->frame = calloc(CPC_SCREEN_W * CPC_SCREEN_H, 4);
    c->frame_done = calloc(CPC_SCREEN_W * CPC_SCREEN_H, 4);
    c->audio = calloc(AUDIO_CAP * 2, sizeof(float));
    c->fdc = fdc_create(c);
    c->z80.ctx = c;
    c->z80.rd = cpc_mem_read; c->z80.wr = cpc_mem_write;
    c->z80.in = cpc_io_read; c->z80.out = cpc_io_write;
    build_palette(c);
    cpc_reset(c, true);
    return c;
}

void cpc_destroy(CPC *c) {
    if (!c) return;
    for (int i = 0; i < 32; i++) free(c->rom_upper[i]);
    free(c->rom_lower); free(c->rom_multiface); free(c->cartridge);
    free(c->rec_buf); tape_free(c->tape); free(c->ram); free(c->frame); free(c->frame_done); free(c->audio);
    fdc_destroy(c->fdc);
    free(c);
}

void cpc_configure(CPC *c, const CPCConfig *cfg) {
    c->cfg = *cfg;
    c->crtc.type = cfg->type;
    c->z80.turbo = cfg->turbo;
    build_palette(c); asic_refresh_palette(c);
    cpc_update_mapping(c);
}

bool cpc_load_rom(CPC *c, int slot, const uint8_t *data, size_t size) {
    if (slot < -1 || slot > 31) return false;
    uint8_t **dst = slot < 0 ? &c->rom_lower : &c->rom_upper[slot];
    free(*dst); *dst = NULL;
    if (!data || !size) { cpc_update_mapping(c); return true; }
    *dst = calloc(1, 16384);
    memcpy(*dst, data, size > 16384 ? 16384 : size);
    cpc_update_mapping(c);
    return true;
}

void cpc_clear_roms(CPC *c) {
    for (int i = -1; i < 32; i++) cpc_load_rom(c, i, NULL, 0);
}

void cpc_compute_mapping(CPC *c, uint8_t ram_cfg, bool lower_en, bool upper_en, uint8_t upper_sel, uint8_t lower_base, uint8_t cart_bank, uint8_t **rd, uint8_t **wr) {
    uint8_t cfgbits = ram_cfg & 7;
    int bank = (ram_cfg >> 3) & 7;
    if (c->cfg.ram_kb <= 64) { cfgbits = 0; bank = 0; }
    for (int i = 0; i < 4; i++) {
        int blk = ram_cfg_table[cfgbits][i];
        size_t phys = blk < 4 ? (size_t)blk * 16384 : 65536 + (size_t)bank * 65536 + (size_t)(blk - 4) * 16384;
        if (phys + 16384 > c->ram_size) phys %= c->ram_size;
        wr[i] = rd[i] = c->ram + phys;
    }
    bool cart = c->cfg.plus && c->cartridge;
    if (lower_en) {
        if (cart) { if (cart_bank < 32) rd[lower_base] = c->cartridge + (size_t)cart_bank * 16384; }
        else if (c->rom_lower) rd[0] = c->rom_lower;
    }
    if (upper_en) {
        uint8_t sel = upper_sel;
        uint8_t *u = sel < 32 ? c->rom_upper[sel] : NULL;
        if (!u && cart) { int b = sel >= 0x80 ? (sel & 0x1F) : (sel == 7 ? 3 : 1); u = c->cartridge + (size_t)b * 16384; }
        if (!u) u = c->rom_upper[0];
        if (u) rd[3] = u;
    }
}

void cpc_update_mapping(CPC *c) {
    cpc_compute_mapping(c, c->ga_ram, c->lower_rom_en, c->upper_rom_en, c->upper_rom_sel, c->asic.lower_base, c->asic.cart_bank, c->rd_map, c->wr_map);
    /* Pages are 16K, so writes to the ROM half (0000-1FFF) also land in the Multiface buffer. */
    if (c->mf_active && c->rom_multiface) c->rd_map[0] = c->wr_map[0] = c->rom_multiface;
}

bool cpc_load_multiface(CPC *c, const uint8_t *data, size_t size) {
    free(c->rom_multiface); c->rom_multiface = NULL; c->mf_active = c->mf_running = false;
    if (data && size) {
        c->rom_multiface = calloc(1, 16384);
        memcpy(c->rom_multiface, data, size < 8192 ? size : 8192);
    }
    cpc_update_mapping(c);
    return c->rom_multiface != NULL;
}

bool cpc_multiface_stop(CPC *c) {
    if (!c->cfg.multiface || !c->rom_multiface || c->mf_running) return false;
    c->mf_running = c->mf_active = true; cpc_update_mapping(c);
    c->z80.nmi_pending = true;
    return true;
}

/* The Multiface snoops writes to the gate array, CRTC, ROM select and PPI control into its RAM
 * so that its software can restore the hardware state (same locations as the real interface). */
static void mf_snoop(CPC *c, uint16_t port, uint8_t v) {
    uint8_t *m = c->rom_multiface;
    if ((port & 0xC000) == 0x4000) {
        switch (v >> 6) {
        case 0: m[0x3FCF] = v; break;
        case 1: { uint8_t pen = m[0x3FCF]; m[0x3F90 | ((pen & 0x10) << 2) | (pen & 0x0F)] = v; break; }
        case 2: m[0x3FEF] = v; break;
        default: m[0x3FFF] = v; break;
        }
    }
    if (!(port & 0x4000)) {
        int f = (port >> 8) & 3;
        if (f == 0) m[0x3CFF] = v; else if (f == 1) m[0x3DB0 | (m[0x3CFF] & 0x0F)] = v;
    }
    if (!(port & 0x2000)) m[0x3AAC] = v;
    if (!(port & 0x0800) && ((port >> 8) & 3) == 3) m[0x37FF] = v;
}

void cpc_reset(CPC *c, bool cold) {
    if (cold) memset(c->ram, 0, c->ram_size);
    z80_reset(&c->z80);
    c->z80.turbo = c->cfg.turbo;
    c->mf_active = c->mf_running = false;
    c->z80.brk_enabled = c->dbg && c->dbg->break_instructions;
    c->lower_rom_en = c->upper_rom_en = true;
    c->upper_rom_sel = 0; c->ga_ram = 0; c->ga_mode = 1; c->ga_pen = 0;
    for (int i = 0; i < 17; i++) c->ga_palette[i] = 20;
    c->ga_irq_counter = 0; c->ga_vsync_delay = 0; c->irq_line = false;
    crtc_reset(&c->crtc, c->cfg.type);
    asic_reset(c);
    c->beam_x = 0; c->beam_y = -37; c->beam_vsync = false;
    c->ppi_a = c->ppi_b = c->ppi_c = 0; c->ppi_ctl = 0x9B; c->psg_ctl = 0;
    memset(c->kbd, 0xFF, sizeof c->kbd); c->amx_row = 0xFF; c->amx_dx = c->amx_dy = 0;
    psg_reset(&c->psg);
    c->tape_motor = false; c->motor = false;
    fdc_reset(c->fdc);
    cpc_update_mapping(c);
}

uint8_t cpc_mem_read(void *ctx, uint16_t a) {
    CPC *c = ctx;
    if (__builtin_expect(c->asic.page_visible, 0) && (a & 0xC000) == 0x4000) return asic_read(c, a & 0x3FFF);
    uint8_t v = c->rd_map[a >> 14][a & 0x3FFF];
    if (__builtin_expect(c->dbg != NULL, 0) && (c->dbg->watch[a] & 1)) dbg_mem_event(c, a, v, false);
    return v;
}
void cpc_mem_write(void *ctx, uint16_t a, uint8_t v) {
    CPC *c = ctx;
    if (__builtin_expect(c->dbg != NULL, 0) && (c->dbg->watch[a] & 2)) dbg_mem_event(c, a, v, true);
    if (__builtin_expect(c->asic.page_visible, 0) && (a & 0xC000) == 0x4000) { asic_write(c, a & 0x3FFF, v); return; }
    c->wr_map[a >> 14][a & 0x3FFF] = v;
}
uint8_t cpc_peek(CPC *c, uint16_t a) { return cpc_mem_read(c, a); }
void cpc_poke(CPC *c, uint16_t a, uint8_t v) { cpc_mem_write(c, a, v); }

/* RAM access through an arbitrary gate array RAM configuration (0xC0-0xFF), ignoring ROMs. */
static uint8_t *cpc_bank_ptr(CPC *c, uint8_t ram_cfg, uint16_t a) {
    uint8_t *rd[4], *wr[4];
    cpc_compute_mapping(c, ram_cfg, false, false, 0, 0, 0xff, rd, wr);
    return wr[a >> 14] + (a & 0x3fff);
}
uint8_t cpc_bank_peek(CPC *c, uint8_t ram_cfg, uint16_t a) { return *cpc_bank_ptr(c, ram_cfg, a); }
void cpc_bank_poke(CPC *c, uint8_t ram_cfg, uint16_t a, uint8_t v) { *cpc_bank_ptr(c, ram_cfg, a) = v; }

/* ---------------- CRTC ---------------- */
void crtc_reset(CPCCrtc *k, uint8_t type) {
    memset(k, 0, sizeof *k);
    k->type = type;
    k->hde = true; k->vde = true;
}

static uint8_t crtc_read(CPC *c, int func) {
    CPCCrtc *k = &c->crtc;
    if (func == 2) { /* status */
        if (k->type == 1) return (k->vsync ? 0x20 : 0) | 0x00;
        return 0;
    }
    if (func == 3) {
        int r = k->sel & 31;
        if (r >= 12 && r <= 17) return k->r[r];
        if (k->type == 1 && r < 12) return 0;
        return 0;
    }
    return 0xFF;
}

static void crtc_write(CPC *c, int func, uint8_t v) {
    CPCCrtc *k = &c->crtc;
    if (func == 0) { k->sel = v & 31; asic_unlock_feed(c, v); }
    else if (func == 1) {
        static const uint8_t mask[18] = {255, 255, 255, 255, 127, 31, 127, 127, 255, 31, 127, 31, 63, 255, 63, 255, 63, 255};
        int r = k->sel;
        if (r < 18) k->r[r] = v & mask[r];
    }
}

/* ---------------- Gate array ---------------- */
static void ga_write(CPC *c, uint8_t v) {
    switch (v >> 6) {
    case 0: c->ga_pen = (v & 0x10) ? 16 : (v & 0x0F); break;
    case 1: c->ga_palette[c->ga_pen] = v & 0x1F; if (c->cfg.plus) asic_ga_palette(c, c->ga_pen, v & 0x1F); break;
    case 2:
        if ((v & 0xE0) == 0xA0 && c->cfg.plus && c->asic.unlocked) { asic_rmr2(c, v); break; }
        c->ga_mode = v & 3;
        c->lower_rom_en = !(v & 4); c->upper_rom_en = !(v & 8);
        if (v & 0x10) { c->ga_irq_counter = 0; c->irq_line = false; }
        cpc_update_mapping(c);
        break;
    default:
        c->ga_ram = v; cpc_update_mapping(c); break;
    }
}

/* ---------------- PSG / PPI ---------------- */
/* The AY follows its data bus while BDIR/BC1 are held: in latch-address mode every change of PPI port A
 * reselects the register, and in write mode it is written again (players such as Arkos AKG rely on
 * this). An address with the upper nibble set deselects the chip, as on the real AY-3-8912. */
static void psg_ctrl_update(CPC *c) {
    uint8_t ctl = (c->ppi_c >> 6) & 3;      /* bit1 = BDIR, bit0 = BC1 */
    if (ctl == 3) c->psg.sel = c->ppi_a;
    else if (ctl == 2 && c->psg.sel < 16) psg_write_reg(&c->psg, c->psg.sel, c->ppi_a);
    c->psg_ctl = ctl;
}

static void ppi_write_c(CPC *c, uint8_t v) {
    c->ppi_c = v;
    c->tape_motor = (v & 0x10) != 0;
    tape_set_motor(c->tape, c->tape_motor);
    c->tape_write_bit = (v & 0x20) != 0;
    psg_ctrl_update(c);
}

static uint8_t ppi_read(CPC *c, int func) {
    switch (func) {
    case 0:
        if (c->ppi_ctl & 0x10) {
            if (c->psg_ctl == 1) {
                if (c->psg.sel > 15) return 0xFF;
                if (c->psg.sel == 14) { int row = c->ppi_c & 15; return (!(c->psg.regs[7] & 0x40) && row < 10) ? (row == 9 ? c->kbd[9] & c->amx_row : c->kbd[row]) : 0xFF; }
                return c->psg.regs[c->psg.sel];
            }
            return 0xFF;
        }
        return c->ppi_a;
    case 1:
        if (c->ppi_ctl & 2) {
            uint8_t v = 0x5E | (c->crtc.vsync ? 1 : 0);
            if (!c->cfg.refresh_50hz) v &= ~0x10;
            if (tape_read_level(c->tape)) v |= 0x80;
            return v;
        }
        return c->ppi_b;
    case 2: return c->ppi_c;
    default: return c->ppi_ctl;
    }
}

static void ppi_write(CPC *c, int func, uint8_t v) {
    switch (func) {
    case 0: c->ppi_a = v; if (!(c->ppi_ctl & 0x10) && c->psg_ctl >= 2) psg_ctrl_update(c); break;
    case 1: c->ppi_b = v; break;
    case 2: ppi_write_c(c, v); break;
    default:
        if (v & 0x80) { c->ppi_ctl = v; c->ppi_a = c->ppi_b = 0; ppi_write_c(c, 0); }
        else { int bit = (v >> 1) & 7; uint8_t nc = (v & 1) ? (c->ppi_c | (1 << bit)) : (c->ppi_c & ~(1 << bit)); ppi_write_c(c, nc); }
    }
}

/* ---------------- I/O decode ---------------- */
int cpc_trace_io;
void cpc_io_write(void *ctx, uint16_t port, uint8_t v) {
    CPC *c = ctx;
    if (cpc_trace_io) fprintf(stderr, "OUT %04X,%02X  pc=%04X\n", port, v, c->z80.pc);
    if (c->dbg && c->dbg->io_bps) dbg_io_event(c, port, v, true);
    if (c->cfg.multiface && c->rom_multiface) {
        if (port == 0xFEE8 && !c->mf_active) { c->mf_active = true; cpc_update_mapping(c); }
        else if (port == 0xFEEA && c->mf_active) { c->mf_active = false; cpc_update_mapping(c); }
        mf_snoop(c, port, v);
    }
    if ((port & 0xC000) == 0x4000) ga_write(c, v);
    if (!(port & 0x8000) && (v & 0xC0) == 0xC0 && (port & 0xC000) != 0x4000) { c->ga_ram = v; cpc_update_mapping(c); }
    else if (!(port & 0x8000) && (v & 0xC0) == 0xC0) { c->ga_ram = v; cpc_update_mapping(c); }
    if (!(port & 0x4000)) crtc_write(c, (port >> 8) & 3, v);
    if (!(port & 0x2000)) { c->upper_rom_sel = v; cpc_update_mapping(c); }
    if (!(port & 0x0800)) ppi_write(c, (port >> 8) & 3, v);
    if (!(port & 0x0400)) {
        if (!(port & 0x0080)) {
            if (!(port & 0x0100)) { c->motor = v & 1; fdc_set_motor(c->fdc, c->motor); }
            else if (port & 1) fdc_write_data(c->fdc, v);
        }
    }
    if ((port & 0xFF00) == 0xFF00 && c->cfg.amdrum) c->amdrum = v;
    if (!(port & 0x1000) && c->cfg.digiblaster) c->amdrum = v;
}

uint8_t cpc_io_read(void *ctx, uint16_t port) {
    CPC *c = ctx;
    uint8_t v = 0xFF;
    if (c->dbg && c->dbg->io_bps) dbg_io_event(c, port, 0xFF, false);
    if (!(port & 0x4000)) { int f = (port >> 8) & 3; if (f >= 2) v = crtc_read(c, f); }
    if (!(port & 0x0800)) v = ppi_read(c, (port >> 8) & 3);
    if (!(port & 0x0400) && !(port & 0x0080) && (port & 0x0100)) v = (port & 1) ? fdc_read_data(c->fdc) : fdc_read_status(c->fdc);
    return v;
}

/* ---------------- Keyboard ---------------- */
void cpc_key(CPC *c, int key, bool down) {
    int line = key >> 3, bit = key & 7;
    if (line > 9) return;
    if (down) c->kbd[line] &= ~(1 << bit); else c->kbd[line] |= 1 << bit;
}

void cpc_amx_mouse(CPC *c, int dx, int dy, uint8_t buttons) { c->amx_dx += dx; c->amx_dy += dy; c->amx_buttons = buttons; }

/* Like WinAPE: once per frame a direction line is held low if the mouse moved that way at all. */
static void amx_frame(CPC *c) {
    uint8_t r = 0xFF;
    if (c->amx_enabled) {
        if (c->amx_dx < 0) r &= ~0x04; else if (c->amx_dx > 0) r &= ~0x08;
        if (c->amx_dy < 0) r &= ~0x01; else if (c->amx_dy > 0) r &= ~0x02;
        if (c->amx_buttons & 1) r &= ~0x10;
        if (c->amx_buttons & 2) r &= ~0x20;
    }
    c->amx_dx = c->amx_dy = 0; c->amx_row = r;
}

void cpc_joystick(CPC *c, int bit, bool down) {
    if (!c->cfg.joystick_enabled) return;
    if (down) c->kbd[9] &= ~(1 << bit); else c->kbd[9] |= 1 << bit;
}

/* ---------------- Video ---------------- */
static inline void put_row16(CPC *c, const uint32_t *px) {
    int y = c->beam_y, x = c->beam_x;
    if (y >= 0 && y < CPC_SCREEN_H) {
        uint32_t *row = c->frame + (size_t)y * CPC_SCREEN_W;
        for (int i = 0; i < 16; i++, x++) if ((unsigned)x < CPC_SCREEN_W) row[x] = px[i];
    }
    c->beam_x += 16;
}

static inline uint32_t pen_rgb(CPC *c, int pen) { return c->cfg.plus ? c->asic.pal_rgb[pen] : c->hw_rgb[c->ga_palette[pen] & 31]; }

static void emit_char(CPC *c, bool de, uint16_t ma, uint16_t ra) {
    uint32_t px[16];
    CPCAsic *a = &c->asic;
    bool plus = c->cfg.plus && a->unlocked;
    if (c->crtc.hcc == 0) c->disp_x0 = c->beam_x;
    if (c->beam_x >= CPC_SCREEN_W + 16 || c->beam_y < 0 || c->beam_y >= CPC_SCREEN_H) { c->beam_x += 16; if (!de) c->scroll_valid = false; return; }
    if (!de) {
        uint32_t rgb = pen_rgb(c, 16);
        for (int i = 0; i < 16; i++) px[i] = rgb;
        c->scroll_valid = false;
        put_row16(c, px);
        return;
    }
    int ra_eff = ra; uint16_t ma_eff = ma;
    if (plus && (a->sscr & 0x70)) {
        ra_eff += (a->sscr >> 4) & 7;
        if (ra_eff >= 8) { ra_eff -= 8; ma_eff = (uint16_t)(ma + c->crtc.r[1]); }
    }
    uint32_t addr = ((ma_eff & 0x3000) << 2) | ((ra_eff & 7) << 11) | ((ma_eff & 0x3FF) << 1);
    uint8_t bytes[2] = {c->ram[addr], c->ram[addr + 1]};
    int o = 0;
    for (int k = 0; k < 2; k++) {
        uint8_t b = bytes[k];
        switch (c->ga_mode) {
        case 0: case 3:
            for (int p = 0; p < 2; p++) {
                /* pixel 0: bit7 = pen bit0, bit3 = bit1, bit5 = bit2, bit1 = bit3 (pixel 1 one bit lower) */
                int pen = p == 0 ? (((b >> 7) & 1) | (((b >> 3) & 1) << 1) | (((b >> 5) & 1) << 2) | (((b >> 1) & 1) << 3))
                                 : (((b >> 6) & 1) | (((b >> 2) & 1) << 1) | (((b >> 4) & 1) << 2) | (((b >> 0) & 1) << 3));
                uint32_t rgb = pen_rgb(c, pen); for (int i = 0; i < 4; i++) px[o++] = rgb;
            }
            break;
        case 1:
            for (int p = 0; p < 4; p++) {
                int pen = ((b >> (7 - p)) & 1) | (((b >> (3 - p)) & 1) << 1);
                uint32_t rgb = pen_rgb(c, pen); px[o++] = rgb; px[o++] = rgb;
            }
            break;
        default:
            for (int p = 0; p < 8; p++) px[o++] = pen_rgb(c, (b >> (7 - p)) & 1);
        }
    }
    int hs = plus ? (a->sscr & 15) : 0;
    if (hs) {
        uint32_t out[16];
        for (int i = 0; i < hs; i++) out[i] = c->scroll_valid ? c->scroll_carry[16 - hs + i] : pen_rgb(c, 16);
        for (int i = hs; i < 16; i++) out[i] = px[i - hs];
        for (int i = 0; i < hs; i++) c->scroll_carry[16 - hs + i] = px[16 - hs + i];
        c->scroll_valid = true;
        put_row16(c, out);
    } else put_row16(c, px);
}

/* Hardware sprites, composited over the finished scanline `line` (CRTC line since frame start) into buffer row y. */
static void draw_sprites_line(CPC *c, int line, int y) {
    CPCAsic *a = &c->asic;
    if (y < 0 || y >= CPC_SCREEN_H) return;
    uint32_t *row = c->frame + (size_t)y * CPC_SCREEN_W;
    for (int n = 15; n >= 0; n--) {
        const uint8_t *at = a->ram + 0x2000 + n * 8;
        int magx = at[4] & 3, magy = (at[4] >> 2) & 3;
        if (!magx || !magy) continue;
        int sx = (int16_t)(at[0] | (at[1] << 8)), sy = (int16_t)(at[2] | (at[3] << 8));
        int mx = magx == 3 ? 4 : magx, my = magy == 3 ? 4 : magy;
        int dy = line - sy;
        if (dy < 0 || dy >= 16 * my) continue;
        const uint8_t *pix = a->ram + n * 256 + (dy / my) * 16;
        for (int px = 0; px < 16; px++) {
            int pen = pix[px] & 15;
            if (!pen) continue;
            int x0 = c->disp_x0 + (sx + px * mx) * SPRITE_PIXEL_W;
            for (int k = 0; k < mx * SPRITE_PIXEL_W; k++) { int x = x0 + k; if ((unsigned)x < CPC_SCREEN_W) row[x] = a->spal_rgb[pen]; }
        }
    }
}

static void ga_hsync_end(CPC *c) {
    c->ga_irq_counter++;
    if (c->ga_vsync_delay > 0) {
        if (--c->ga_vsync_delay == 0) {
            if (c->ga_irq_counter >= 32) c->irq_line = true;
            c->ga_irq_counter = 0;
        }
    } else if (c->ga_irq_counter >= 52) {
        c->ga_irq_counter = 0; c->irq_line = true;
    }
    if (c->irq_line && c->cfg.plus) c->z80.int_vector = c->asic.unlocked ? ((c->asic.ivr & 0xF8) | 6) : 0xFF;
    if (c->cfg.plus && c->asic.unlocked) draw_sprites_line(c, c->frame_line, c->beam_y);
    c->beam_x = 0;
    c->beam_y++;
    asic_line(c);
}

static void frame_flyback(CPC *c) {
    uint32_t *t = c->frame_done; c->frame_done = c->frame; c->frame = t;
    c->frame_ready = true; c->frames++;
    c->beam_y = -37; c->ga_vsync_delay = 2;
}

void cpc_video_tick(CPC *c) {
    CPCCrtc *k = &c->crtc;
    uint16_t ma = (k->ma_row + k->hcc) & 0x3FFF;
    emit_char(c, k->hde && k->vde, ma, k->vlc);

    if (k->hsync) {
        uint8_t w = k->r[3] & 15; if (!w) w = 16;
        if (++k->hsc >= w) { k->hsync = false; ga_hsync_end(c); }
    }
    if (k->hcc == k->r[0]) {
        /* end of scanline */
        k->hcc = 0; k->hde = k->r[1] > 0;
        bool new_frame = false;
        if (k->in_adjust) {
            k->vlc = (k->vlc + 1) & 31;
            if (++k->vtac >= k->r[5]) new_frame = true;
        } else if (k->vlc == k->r[9]) {
            if (k->vcc == k->r[4]) {
                if (k->r[5]) { k->in_adjust = true; k->vtac = 0; k->vlc = (k->vlc + 1) & 31; }
                else new_frame = true;
            } else { k->vlc = 0; k->vcc = (k->vcc + 1) & 127; k->ma_row = (k->ma_row + k->r[1]) & 0x3FFF; }
        } else k->vlc = (k->vlc + 1) & 31;
        if (new_frame) {
            k->vcc = 0; k->vlc = 0; k->in_adjust = false;
            k->ma_start = ((k->r[12] << 8) | k->r[13]) & 0x3FFF; k->ma_row = k->ma_start;
        }
        c->frame_line = new_frame ? 0 : c->frame_line + 1;
        if (c->cfg.plus && c->asic.unlocked && c->asic.splt && !k->in_adjust && (k->vlc & 7) == 0 && k->vcc * (k->r[9] + 1) + k->vlc == c->asic.splt)
            k->ma_row = c->asic.ssa & 0x3FFF;
        k->vde = !k->in_adjust && k->vcc < k->r[6];
        if (k->vsync) {
            uint8_t vw = k->r[3] >> 4; if (!vw) vw = 16;
            if (++k->vsc >= vw) k->vsync = false;
        } else if (k->vcc == k->r[7] && k->vlc == 0 && !k->in_adjust) {
            k->vsync = true; k->vsc = 0;
            frame_flyback(c);
        }
    } else {
        k->hcc++;
        if (k->hcc == k->r[1]) k->hde = false;
    }
    if (k->hcc == k->r[2] && !k->hsync) { k->hsync = true; k->hsc = 0; }
}

/* ---------------- Audio ---------------- */
static void audio_tick(CPC *c) {
    psg_tick(&c->psg);
    float l, r;
    psg_mix(&c->psg, &l, &r, c->cfg.stereo);
    if (c->cfg.amdrum || c->cfg.digiblaster) { float d = (c->amdrum / 255.0f) * 0.5f; l = r = (l + r) * 0.5f + d; }
    if (c->cfg.tape_sound && c->tape_motor && tape_read_level(c->tape)) { l += 0.02f; r += 0.02f; }
    c->audio_acc[0] += l; c->audio_acc[1] += r; c->audio_acc_n++;
    c->audio_phase += c->cfg.sample_rate;
    if (c->audio_phase >= 1000000) {
        c->audio_phase -= 1000000;
        int n = c->audio_acc_n ? c->audio_acc_n : 1;
        int w = __atomic_load_n(&c->audio_w, __ATOMIC_RELAXED);
        int next = (w + 1) % AUDIO_CAP;
        if (next != __atomic_load_n(&c->audio_r, __ATOMIC_ACQUIRE)) {
            c->audio[w * 2] = c->audio_acc[0] / n * c->cfg.volume;
            c->audio[w * 2 + 1] = c->audio_acc[1] / n * c->cfg.volume;
            __atomic_store_n(&c->audio_w, next, __ATOMIC_RELEASE);
        }
        c->audio_acc[0] = c->audio_acc[1] = 0; c->audio_acc_n = 0;
    }
}

int cpc_audio_available(const CPC *c) {
    int w = __atomic_load_n(&c->audio_w, __ATOMIC_ACQUIRE), r = __atomic_load_n(&c->audio_r, __ATOMIC_ACQUIRE);
    return (w - r + AUDIO_CAP) % AUDIO_CAP;
}

int cpc_audio_read(CPC *c, float *out, int frames) {
    int n = 0;
    int r = __atomic_load_n(&c->audio_r, __ATOMIC_RELAXED);
    while (n < frames && r != __atomic_load_n(&c->audio_w, __ATOMIC_ACQUIRE)) {
        out[n * 2] = c->audio[r * 2]; out[n * 2 + 1] = c->audio[r * 2 + 1];
        r = (r + 1) % AUDIO_CAP; n++;
    }
    __atomic_store_n(&c->audio_r, r, __ATOMIC_RELEASE);
    return n;
}

/* ---------------- Tape recording ---------------- */
void cpc_tape_record_start(CPC *c) { free(c->rec_buf); c->rec_buf = NULL; c->rec_len = c->rec_cap = 0; c->rec_phase = 0; c->rec_active = true; }

static void rec_tick(CPC *c) {
    if (!c->rec_active || !c->tape_motor) return;
    c->rec_phase += 44100;
    if (c->rec_phase < 1000000) return;
    c->rec_phase -= 1000000;
    if (c->rec_len == c->rec_cap) { c->rec_cap = c->rec_cap ? c->rec_cap * 2 : 1 << 20; c->rec_buf = realloc(c->rec_buf, c->rec_cap); }
    c->rec_buf[c->rec_len++] = c->tape_write_bit ? 0xC0 : 0x40;
}

uint8_t *cpc_tape_record_stop(CPC *c, size_t *size) {
    c->rec_active = false;
    size_t n = c->rec_len, total = 44 + n;
    uint8_t *o = malloc(total);
    memcpy(o, "RIFF", 4); uint32_t v = (uint32_t)(total - 8); memcpy(o + 4, &v, 4);
    memcpy(o + 8, "WAVEfmt ", 8); v = 16; memcpy(o + 16, &v, 4);
    uint16_t h = 1; memcpy(o + 20, &h, 2); memcpy(o + 22, &h, 2);
    v = 44100; memcpy(o + 24, &v, 4); memcpy(o + 28, &v, 4);
    h = 1; memcpy(o + 32, &h, 2); h = 8; memcpy(o + 34, &h, 2);
    memcpy(o + 36, "data", 4); v = (uint32_t)n; memcpy(o + 40, &v, 4);
    memcpy(o + 44, c->rec_buf, n);
    free(c->rec_buf); c->rec_buf = NULL; c->rec_len = c->rec_cap = 0;
    *size = total; return o;
}

/* ---------------- Main loop ---------------- */
static void advance(CPC *c, int nops) {
    for (int i = 0; i < nops; i++) {
        cpc_video_tick(c);
        audio_tick(c);
        fdc_tick(c->fdc, 1);
        tape_tick(c->tape, 1);
        rec_tick(c);
        c->cycles++;
    }
    c->t_counter += nops;
}

int cpc_step(CPC *c) {
    if (c->dbg) { c->z80.brk_enabled = c->dbg->break_instructions; if (dbg_check_code(c)) return 0; }
    int n = z80_step(&c->z80, c->irq_line);
    if (c->z80.int_ack) { c->irq_line = false; c->ga_irq_counter &= 0x1F; }
    if (c->z80.retn_hit && c->mf_running) c->mf_running = false;
    if (c->z80.brk_hit) { if (c->dbg) { c->dbg->hit = true; c->dbg->hit_index = -2; } return n; }
    advance(c, n);
    return n;
}

void cpc_run_frame(CPC *c) {
    uint64_t start = c->cycles;
    c->frame_ready = false;
    cpc_autotype_tick(c);
    amx_frame(c);
    while (!c->frame_ready && c->cycles - start < 40000) {
        cpc_step(c);
        if (c->z80.brk_hit || (c->dbg && c->dbg->hit)) return;
    }
}

const uint32_t *cpc_frame(const CPC *c) { return c->frame_done; }
