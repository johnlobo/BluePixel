#include "cpc.h"
#include <string.h>

static inline uint32_t dim(uint32_t p) { return 0xFF000000u | ((p >> 2) & 0x3F3F3F); }   /* 25% brightness */

void cpc_render_raster(CPC *c, uint32_t *out) {
    int by = c->beam_y, bx = c->beam_x;
    for (int y = 0; y < CPC_SCREEN_H; y++) {
        const uint32_t *cur = c->frame + (size_t)y * CPC_SCREEN_W, *old = c->frame_done + (size_t)y * CPC_SCREEN_W;
        uint32_t *o = out + (size_t)y * CPC_SCREEN_W;
        for (int x = 0; x < CPC_SCREEN_W; x++) {
            bool drawn = y < by || (y == by && x < bx);
            o[x] = drawn ? cur[x] : dim(old[x]);
        }
    }
    /* beam: a red line across the current row and a marker at the beam's horizontal position */
    if (by >= 0 && by < CPC_SCREEN_H) {
        uint32_t *o = out + (size_t)by * CPC_SCREEN_W;
        for (int x = 0; x < CPC_SCREEN_W; x++) o[x] = (x >= bx - 2 && x <= bx + 2) ? 0xFFFFFF00u : 0xFFFF2020u;
    } else {
        /* beam outside the visible area (vertical border/flyback): mark the top edge */
        for (int x = 0; x < CPC_SCREEN_W; x++) out[x] = 0xFFFF2020u;
    }
}

/* Pens for one screen byte in the given mode (pixels per byte: 2, 4 or 8). */
static int byte_pens(uint8_t b, int mode, int *pen) {
    switch (mode) {
    case 0: case 3:
        pen[0] = ((b >> 7) & 1) | (((b >> 3) & 1) << 1) | (((b >> 5) & 1) << 2) | (((b >> 1) & 1) << 3);
        pen[1] = ((b >> 6) & 1) | (((b >> 2) & 1) << 1) | (((b >> 4) & 1) << 2) | ((b & 1) << 3);
        return 2;
    case 1:
        for (int p = 0; p < 4; p++) pen[p] = ((b >> (7 - p)) & 1) | (((b >> (3 - p)) & 1) << 1);
        return 4;
    default:
        for (int p = 0; p < 8; p++) pen[p] = (b >> (7 - p)) & 1;
        return 8;
    }
}

void cpc_render_vram(CPC *c, uint32_t *out) {
    const uint8_t *r = c->crtc.r;
    int cols = r[1], rows = r[6], lines_per_row = (r[9] & 31) + 1;
    uint16_t start = (uint16_t)(((r[12] << 8) | r[13]) & 0x3FFF);
    uint32_t border = cpc_pen_rgb(c, 16);
    for (int i = 0; i < CPC_SCREEN_W * CPC_SCREEN_H; i++) out[i] = border;
    int w = cols * 16, h = rows * lines_per_row;
    if (w > CPC_SCREEN_W) w = CPC_SCREEN_W;
    if (h > CPC_SCREEN_H) h = CPC_SCREEN_H;
    int ox = (CPC_SCREEN_W - w) / 2, oy = (CPC_SCREEN_H - h) / 2;
    int pen[8];
    for (int y = 0; y < h; y++) {
        int row = y / lines_per_row, ra = y % lines_per_row;
        uint32_t *o = out + (size_t)(oy + y) * CPC_SCREEN_W + ox;
        for (int col = 0; col * 16 < w; col++) {
            uint16_t ma = (uint16_t)(start + row * cols + col);
            uint32_t addr = ((ma & 0x3000) << 2) | ((ra & 7) << 11) | ((ma & 0x3FF) << 1);
            for (int k = 0; k < 2; k++) {
                int n = byte_pens(c->ram[(addr + k) & 0xFFFF], c->ga_mode, pen), pw = 8 / n;
                for (int p = 0; p < n; p++) {
                    uint32_t rgb = cpc_pen_rgb(c, pen[p]);
                    for (int i = 0; i < pw; i++) { int x = col * 16 + k * 8 + p * pw + i; if (x < w) o[x] = rgb; }
                }
            }
        }
    }
}
