#ifndef CPC_Z80_H
#define CPC_Z80_H
#include <stdint.h>
#include <stdbool.h>

typedef union { struct { uint8_t l, h; }; uint16_t w; } Z80Pair;

typedef struct Z80 Z80;
struct Z80 {
    Z80Pair af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, wz;
    uint16_t pc;
    uint8_t i, r;
    bool iff1, iff2, halted;
    uint8_t im;
    int ei_delay;          /* instructions to wait before interrupts are accepted (after EI) */
    bool turbo;            /* "Turbo mode": 1 NOP per instruction, 2 for OUT */
    bool brk_enabled;      /* ED FF is a debugger BRK instruction */
    bool int_ack;          /* set when a maskable interrupt was accepted by the last step */
    uint8_t int_vector;    /* data-bus byte supplied for IM2 (default 0xFF) */
    bool brk_hit;          /* set when BRK executed; core does not advance PC */
    bool nmi_pending;      /* edge-triggered NMI, accepted before the next instruction */
    bool retn_hit;         /* set when the last step executed RETN/RETI */
    uint64_t instructions; /* retired instruction counter */
    void *ctx;
    uint8_t (*rd)(void *ctx, uint16_t addr);
    void (*wr)(void *ctx, uint16_t addr, uint8_t v);
    uint8_t (*in)(void *ctx, uint16_t port);
    void (*out)(void *ctx, uint16_t port, uint8_t v);
};

#define Z80_A(z) ((z)->af.h)
#define Z80_F(z) ((z)->af.l)

void z80_reset(Z80 *z);
/* Executes one instruction (or accepts a pending interrupt when `irq` is true).
 * Returns elapsed CPC NOPs (microseconds). `prefix_repeat` is set when a block instruction repeats. */
int z80_step(Z80 *z, bool irq);

/* Disassembler (shared with the debugger). Returns instruction length. */
int z80_disassemble(uint16_t pc, uint8_t (*peek)(void *ctx, uint16_t), void *ctx, char *out, int outsize);

#endif
