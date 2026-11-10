#include "cpc.h"
#include <string.h>

static const float vol_table[16] = {
    0.0f, 0.0100f, 0.0145f, 0.0211f, 0.0307f, 0.0455f, 0.0645f, 0.1073f,
    0.1266f, 0.2050f, 0.2922f, 0.3728f, 0.4986f, 0.6353f, 0.8055f, 1.0f
};

void psg_reset(CPCPsg *p) {
    memset(p, 0, sizeof *p);
    p->lfsr = 1;
    for (int i = 0; i < 3; i++) p->tone_per[i] = 1;
    p->regs[7] = 0xFF;
}

static void env_reset(CPCPsg *p) {
    uint8_t shape = p->regs[13] & 15;
    p->env_step = 0; p->env_hold = false; p->env_cnt = 0;
    p->env_up = (shape & 4) != 0;              /* attack */
    p->env_vol = p->env_up ? 0 : 15;
}

void psg_write_reg(CPCPsg *p, uint8_t reg, uint8_t v) {
    reg &= 15;
    static const uint8_t mask[16] = {255, 15, 255, 15, 255, 15, 31, 255, 31, 31, 31, 255, 255, 15, 255, 255};
    p->regs[reg] = v & mask[reg];
    p->lvl_clean = false;
    if (reg < 6) { int ch = reg >> 1; p->tone_per[ch] = (uint16_t)(p->regs[ch * 2] | (p->regs[ch * 2 + 1] << 8)); }
    if (reg == 13) env_reset(p);
}

static void env_step(CPCPsg *p) {
    if (p->env_hold) return;
    uint8_t shape = p->regs[13] & 15;
    if (p->env_up) { if (p->env_vol < 15) p->env_vol++; } else if (p->env_vol > 0) p->env_vol--;
    if (++p->env_step >= 16) {
        p->env_step = 0;
        if (!(shape & 8)) { p->env_hold = true; p->env_vol = 0; }
        else if (shape & 1) { p->env_hold = true; p->env_vol = (((shape & 4) != 0) ^ ((shape & 2) != 0)) ? 15 : 0; }
        else if (shape & 2) p->env_up = !p->env_up;
        else p->env_vol = p->env_up ? 0 : 15;
    }
}

/* Volume index a channel plays at right now (the envelope when bit 4 of its register is set). */
static inline uint8_t channel_amp(const CPCPsg *p, int i) {
    uint8_t a = p->regs[8 + i];
    return (a & 16) ? p->env_vol : (a & 15);
}

/* A level is (tone && noise) ? vol_table[amp] : 0 and vol_table[0] is 0, so a channel at amplitude 0 stays at 0 whatever its
 * oscillators do: flips only count for channels that can be heard. The levels are recomputed only when the formula's result
 * can differ from what is stored, which keeps them bit-identical to recomputing every tick. */
bool psg_tick_slow(CPCPsg *p) {
    bool changed = !p->lvl_clean;
    if ((p->sub & 7) == 0) {
        for (int i = 0; i < 3; i++) {
            if (++p->tone_cnt[i] >= (p->tone_per[i] ? p->tone_per[i] : 1)) { p->tone_cnt[i] = 0; p->tone_out[i] ^= 1; if (channel_amp(p, i)) changed = true; }
        }
    }
    if ((p->sub & 15) == 0) {
        uint16_t np = p->regs[6] & 31; if (!np) np = 1;
        if (++p->noise_cnt >= np) {
            p->noise_cnt = 0;
            uint32_t bit = (p->lfsr ^ (p->lfsr >> 3)) & 1;
            p->lfsr = (p->lfsr >> 1) | (bit << 16);
            uint8_t before = p->noise_out;
            p->noise_out = p->lfsr & 1;
            if (p->noise_out != before && (channel_amp(p, 0) | channel_amp(p, 1) | channel_amp(p, 2))) changed = true;
        }
        uint16_t ep = (uint16_t)(p->regs[11] | (p->regs[12] << 8)); if (!ep) ep = 1;
        if (++p->env_cnt >= ep) { p->env_cnt = 0; uint8_t before = p->env_vol; env_step(p); if (p->env_vol != before) changed = true; }
    }
    if (!changed) return false;
    uint8_t mix = p->regs[7];
    for (int i = 0; i < 3; i++) {
        bool t = (mix >> i) & 1 ? true : p->tone_out[i];
        bool n = (mix >> (i + 3)) & 1 ? true : p->noise_out;
        uint8_t a = p->regs[8 + i];
        uint8_t amp = (a & 16) ? p->env_vol : (a & 15);
        p->level[i] = (t && n) ? vol_table[amp] : 0.0f;
    }
    p->lvl_clean = true;
    return true;
}

void psg_mix(const CPCPsg *p, float *l, float *r, bool stereo) {
    float a = p->level[0], b = p->level[1], c = p->level[2];
    if (stereo) { *l = (a + 0.5f * b) * 0.5f; *r = (c + 0.5f * b) * 0.5f; }
    else { float m = (a + b + c) / 3.0f; *l = *r = m; }
}
