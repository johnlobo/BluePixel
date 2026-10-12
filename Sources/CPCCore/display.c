#include "cpc.h"

#define SW CPC_SCREEN_W
#define SH CPC_SCREEN_H

static inline uint32_t scale(uint32_t p, int num, int den) {
    uint32_t r = ((p >> 16) & 255) * num / den, g = ((p >> 8) & 255) * num / den, b = (p & 255) * num / den;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
static inline uint32_t blend3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = (((a >> 16) & 255) + 2 * ((b >> 16) & 255) + ((c >> 16) & 255)) >> 2;
    uint32_t g = (((a >> 8) & 255) + 2 * ((b >> 8) & 255) + ((c >> 8) & 255)) >> 2;
    uint32_t bl = ((a & 255) + 2 * (b & 255) + (c & 255)) >> 2;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

/* Output size: 768x540 normally, 384x270 when half_size. */
void cpc_render(const uint32_t *src, uint32_t *dst, bool pal, bool render_both, bool half_size, int brightness) {
    int ow = half_size ? SW / 2 : SW, oh = half_size ? SH : SH * 2;
    for (int y = 0; y < SH; y++) {
        const uint32_t *s = src + (size_t)y * SW;
        if (half_size) {
            uint32_t *d = dst + (size_t)y * ow;
            for (int x = 0; x < ow; x++) {
                uint32_t a = s[x * 2], b = s[x * 2 + 1];
                uint32_t r = (((a >> 16) & 255) + ((b >> 16) & 255)) >> 1, g = (((a >> 8) & 255) + ((b >> 8) & 255)) >> 1, bl = ((a & 255) + (b & 255)) >> 1;
                d[x] = 0xFF000000u | (r << 16) | (g << 8) | bl;
            }
            continue;
        }
        uint32_t *d0 = dst + (size_t)(2 * y) * ow, *d1 = d0 + ow;
        for (int x = 0; x < SW; x++) {
            uint32_t p = s[x];
            if (pal) p = blend3(s[x > 0 ? x - 1 : x], p, s[x < SW - 1 ? x + 1 : x]);
            d0[x] = p;
            d1[x] = render_both ? (pal ? scale(p, 1, 2) : p) : 0xFF000000u;
        }
    }
    (void)oh;
    if (brightness) {
        int num = 100 + brightness, n = ow * (half_size ? SH : SH * 2);
        for (int i = 0; i < n; i++) {
            uint32_t p = dst[i];
            int r = ((p >> 16) & 255) * num / 100, g = ((p >> 8) & 255) * num / 100, b = (p & 255) * num / 100;
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            dst[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}
