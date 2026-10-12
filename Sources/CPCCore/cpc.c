#include "cpc.h"
#include "fdc.h"
#include "tape.h"
#include <stdlib.h>
#include <string.h>

#define RAM_MAX (576 * 1024)
#define AUDIO_CAP 16384

/* Hardware colour number (0..31) to RGB */
static const uint32_t hw_colours[32] = {
    0x808080, 0x808080, 0x00FF80, 0xFFFF80, 0x000080, 0xFF0080, 0x008080, 0xFF8080,
    0xFF0080, 0xFFFF80, 0xFFFF00, 0xFFFFFF, 0xFF0000, 0xFF00FF, 0xFF8000, 0xFF80FF,
    0x000080, 0x00FF80, 0x00FF00, 0x00FFFF, 0x000000, 0x0000FF, 0x008000, 0x0080FF,
    0x800080, 0x80FF80, 0x80FF00, 0x80FFFF, 0x800000, 0x8000FF, 0x808000, 0x8080FF,
};

static const uint8_t ram_cfg[8][4] = {{0, 1, 2, 3}, {0, 1, 2, 7}, {4, 5, 6, 7}, {0, 3, 2, 7},
                                      {0, 4, 2, 3}, {0, 5, 2, 3}, {0, 6, 2, 3}, {0, 7, 2, 3}};

void cpc_default_config(CPCConfig *cfg, CPCModel model) {
    memset(cfg, 0, sizeof *cfg);
    cfg->type = 0; cfg->plus = model >= CPC_MODEL_464PLUS; cfg->plus_ppi = cfg->plus;
    if (cfg->plus) cfg->type = 3;
    cfg->ram_kb = (model == CPC_MODEL_6128 || model == CPC_MODEL_6128PLUS) ? 128 : 64;
    cfg->fast_disc = true; cfg->joystick_enabled = true; cfg->monitor = CPC_MONITOR_COLOUR;
    cfg->linear_palette = true; cfg->sample_rate = 44100; cfg->stereo = true; cfg->volume = 0.75f;
    cfg->disc_sound = false; cfg->tape_sound = true; cfg->refresh_50hz = true;
}

static void build_palette(CPC *c) {
    for (int i = 0; i < 32; i++) {
        uint32_t rgb = hw_colours[i];
        int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
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
    free(c->ram); free(c->frame); free(c->frame_done); free(c->audio);
    fdc_destroy(c->fdc);
    free(c);
}

void cpc_configure(CPC *c, const CPCConfig *cfg) {
    c->cfg = *cfg;
    c->crtc.type = cfg->type;
    c->z80.turbo = cfg->turbo;
    build_palette(c);
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

void cpc_update_mapping(CPC *c) {
    uint8_t cfgbits = c->ga_ram & 7;
    int bank = (c->ga_ram >> 3) & 7;
    if (c->cfg.ram_kb <= 64) { cfgbits = 0; bank = 0; }
    for (int i = 0; i < 4; i++) {
        int blk = ram_cfg[cfgbits][i];
        size_t phys = blk < 4 ? (size_t)blk * 16384 : 65536 + (size_t)bank * 65536 + (size_t)(blk - 4) * 16384;
        if (phys + 16384 > c->ram_size) phys %= c->ram_size;
        c->wr_map[i] = c->rd_map[i] = c->ram + phys;
    }
    if (c->lower_rom_en && c->rom_lower) c->rd_map[0] = c->rom_lower;
    if (c->upper_rom_en) {
        uint8_t *u = c->rom_upper[c->upper_rom_sel & 31];
        if (!u) u = c->rom_upper[0];
        if (u) c->rd_map[3] = u;
    }
}

void cpc_reset(CPC *c, bool cold) {
    if (cold) memset(c->ram, 0, c->ram_size);
    z80_reset(&c->z80);
    c->z80.turbo = c->cfg.turbo;
    c->z80.brk_enabled = c->dbg != NULL;
    c->lower_rom_en = c->upper_rom_en = true;
    c->upper_rom_sel = 0; c->ga_ram = 0; c->ga_mode = 1; c->ga_pen = 0;
    for (int i = 0; i < 17; i++) c->ga_palette[i] = 20;
    c->ga_irq_counter = 0; c->ga_vsync_delay = 0; c->irq_line = false;
    crtc_reset(&c->crtc, c->cfg.type);
    c->beam_x = 0; c->beam_y = -37; c->beam_vsync = false;
    c->ppi_a = c->ppi_b = c->ppi_c = 0; c->ppi_ctl = 0x9B; c->psg_ctl = 0;
    memset(c->kbd, 0xFF, sizeof c->kbd);
    psg_reset(&c->psg);
    c->tape_motor = false; c->motor = false;
    fdc_reset(c->fdc);
    cpc_update_mapping(c);
}

uint8_t cpc_mem_read(void *ctx, uint16_t a) { CPC *c = ctx; return c->rd_map[a >> 14][a & 0x3FFF]; }
void cpc_mem_write(void *ctx, uint16_t a, uint8_t v) { CPC *c = ctx; c->wr_map[a >> 14][a & 0x3FFF] = v; }
uint8_t cpc_peek(CPC *c, uint16_t a) { return cpc_mem_read(c, a); }
void cpc_poke(CPC *c, uint16_t a, uint8_t v) { cpc_mem_write(c, a, v); }

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
    if (func == 0) k->sel = v & 31;
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
    case 1: c->ga_palette[c->ga_pen] = v & 0x1F; break;
    case 2:
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
static void psg_ctrl_update(CPC *c) {
    uint8_t ctl = (c->ppi_c >> 6) & 3;      /* bit1 = BDIR, bit0 = BC1 */
    if (ctl == 3) c->psg.sel = c->ppi_a & 15;
    else if (ctl == 2) psg_write_reg(&c->psg, c->psg.sel, c->ppi_a);
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
                if (c->psg.sel == 14) { int row = c->ppi_c & 15; return (!(c->psg.regs[7] & 0x40) && row < 10) ? c->kbd[row] : 0xFF; }
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
    case 0: c->ppi_a = v; break;
    case 1: c->ppi_b = v; break;
    case 2: ppi_write_c(c, v); break;
    default:
        if (v & 0x80) { c->ppi_ctl = v; c->ppi_a = c->ppi_b = 0; ppi_write_c(c, 0); }
        else { int bit = (v >> 1) & 7; uint8_t nc = (v & 1) ? (c->ppi_c | (1 << bit)) : (c->ppi_c & ~(1 << bit)); ppi_write_c(c, nc); }
    }
}

/* ---------------- I/O decode ---------------- */
void cpc_io_write(void *ctx, uint16_t port, uint8_t v) {
    CPC *c = ctx;
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

void cpc_joystick(CPC *c, int bit, bool down) {
    if (!c->cfg.joystick_enabled) return;
    if (down) c->kbd[9] &= ~(1 << bit); else c->kbd[9] |= 1 << bit;
}

/* ---------------- Video ---------------- */
static inline void put_px(CPC *c, int n, uint32_t rgb) {
    int x = c->beam_x, y = c->beam_y;
    if (y < 0 || y >= CPC_SCREEN_H) return;
    uint32_t *row = c->frame + (size_t)y * CPC_SCREEN_W;
    for (int i = 0; i < n; i++, x++) if ((unsigned)x < CPC_SCREEN_W) row[x] = rgb;
}

static void emit_char(CPC *c, bool de, uint16_t ma, uint16_t ra) {
    int y = c->beam_y;
    if (y < 0 || y >= CPC_SCREEN_H || c->beam_x >= CPC_SCREEN_W) { c->beam_x += 16; return; }
    if (!de) {
        uint32_t rgb = c->hw_rgb[c->ga_palette[16] & 31];
        put_px(c, 16, rgb);
        c->beam_x += 16;
        return;
    }
    uint32_t addr = ((ma & 0x3000) << 2) | ((ra & 7) << 11) | ((ma & 0x3FF) << 1);
    uint8_t b0 = c->ram[addr], b1 = c->ram[addr + 1];
    uint8_t bytes[2] = {b0, b1};
    for (int k = 0; k < 2; k++) {
        uint8_t b = bytes[k];
        switch (c->ga_mode) {
        case 0: case 3:
            for (int p = 0; p < 2; p++) {
                int pen = p == 0 ? (((b >> 7) & 1) | (((b >> 5) & 1) << 1) | (((b >> 3) & 1) << 2) | (((b >> 1) & 1) << 3))
                                 : (((b >> 6) & 1) | (((b >> 4) & 1) << 1) | (((b >> 2) & 1) << 2) | (((b >> 0) & 1) << 3));
                put_px(c, 4, c->hw_rgb[c->ga_palette[pen] & 31]); c->beam_x += 4;
            }
            break;
        case 1:
            for (int p = 0; p < 4; p++) {
                int pen = ((b >> (7 - p)) & 1) | (((b >> (3 - p)) & 1) << 1);
                put_px(c, 2, c->hw_rgb[c->ga_palette[pen] & 31]); c->beam_x += 2;
            }
            break;
        default:
            for (int p = 0; p < 8; p++) {
                int pen = (b >> (7 - p)) & 1;
                put_px(c, 1, c->hw_rgb[c->ga_palette[pen] & 31]); c->beam_x += 1;
            }
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
    c->beam_x = 0;
    c->beam_y++;
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
        int w = c->audio_w;
        int next = (w + 1) % AUDIO_CAP;
        if (next != c->audio_r) {
            c->audio[w * 2] = c->audio_acc[0] / n * c->cfg.volume;
            c->audio[w * 2 + 1] = c->audio_acc[1] / n * c->cfg.volume;
            c->audio_w = next;
        }
        c->audio_acc[0] = c->audio_acc[1] = 0; c->audio_acc_n = 0;
    }
}

int cpc_audio_available(const CPC *c) { return (c->audio_w - c->audio_r + AUDIO_CAP) % AUDIO_CAP; }

int cpc_audio_read(CPC *c, float *out, int frames) {
    int n = 0;
    while (n < frames && c->audio_r != c->audio_w) {
        out[n * 2] = c->audio[c->audio_r * 2]; out[n * 2 + 1] = c->audio[c->audio_r * 2 + 1];
        c->audio_r = (c->audio_r + 1) % AUDIO_CAP; n++;
    }
    return n;
}

/* ---------------- Main loop ---------------- */
static void advance(CPC *c, int nops) {
    for (int i = 0; i < nops; i++) {
        cpc_video_tick(c);
        audio_tick(c);
        fdc_tick(c->fdc, 1);
        tape_tick(c->tape, 1);
        c->cycles++;
    }
    c->t_counter += nops;
}

int cpc_step(CPC *c) {
    int n = z80_step(&c->z80, c->irq_line);
    if (c->z80.int_ack) { c->irq_line = false; c->ga_irq_counter &= 0x1F; }
    if (!c->z80.brk_hit) advance(c, n);
    return n;
}

void cpc_run_frame(CPC *c) {
    uint64_t start = c->cycles;
    c->frame_ready = false;
    while (!c->frame_ready && c->cycles - start < 40000) {
        cpc_step(c);
        if (c->z80.brk_hit) return;
    }
}

const uint32_t *cpc_frame(const CPC *c) { return c->frame_done; }
