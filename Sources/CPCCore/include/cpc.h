#ifndef CPC_H
#define CPC_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "z80.h"

/* Visible frame buffer (hi-res pixels; every CPC scanline is one row, frontends may double it). */
#define CPC_SCREEN_W 768
#define CPC_SCREEN_H 270

/* Keyboard matrix position helper: line 0..9, bit 0..7 */
#define CPC_KEY(line, bit) ((line) * 8 + (bit))

typedef enum { CPC_MODEL_464, CPC_MODEL_664, CPC_MODEL_6128, CPC_MODEL_464PLUS, CPC_MODEL_6128PLUS } CPCModel;
typedef enum { CPC_MONITOR_COLOUR, CPC_MONITOR_GREEN, CPC_MONITOR_GREY } CPCMonitor;

typedef struct CPCDisc CPCDisc;
typedef struct CPCTape CPCTape;
typedef struct FDC FDC;
struct Debugger;

typedef struct {
    uint8_t r[32];
    uint8_t sel;
    uint16_t hcc, vcc, vlc, vtac, hsc, vsc;
    uint16_t ma, ma_row, ma_start;
    bool in_adjust, vsync, hsync, hde, vde, last_row;
    uint8_t type;
} CPCCrtc;

typedef struct {
    uint8_t regs[16];
    uint8_t sel;
    uint16_t tone_cnt[3], tone_per[3];
    uint8_t tone_out[3];
    uint16_t noise_cnt;
    uint32_t lfsr;
    uint8_t noise_out;
    uint16_t env_cnt;
    int8_t env_step;
    uint8_t env_vol;
    bool env_hold, env_up;
    uint8_t sub;
    float level[3];
} CPCPsg;

typedef struct {
    bool unlocked; int unlock_pos;
    bool page_visible;            /* ASIC registers mapped at 0x4000-0x7FFF */
    uint8_t ram[16384];
    uint8_t rmr2;                 /* last RMR2 write */
    uint8_t lower_base;           /* 0: 0000, 1: 4000, 2: 8000 */
    uint8_t cart_bank;            /* cartridge bank mapped as lower ROM */
    uint8_t pri, splt, ivr, sscr, dcsr, icsr;
    uint16_t ssa;
    struct { uint16_t addr, loop_addr; uint8_t prescale; uint16_t pause, loop_cnt; int tick; bool stopped; } dma[3];
    uint32_t pal_rgb[17];         /* current display colours for pens 0..15 and border */
    uint32_t spal_rgb[16];
    uint16_t pal12[17];
} CPCAsic;

typedef struct CPC CPC;

typedef struct {
    uint8_t type;          /* CRTC type 0..4 */
    bool plus;             /* Amstrad Plus features */
    bool plus_ppi;
    int ram_kb;            /* 64, 128, 320, 576 ... */
    bool silicon_disc;
    bool fast_disc;
    bool turbo;
    bool four_drives;
    bool amdrum, digiblaster;
    bool joystick_enabled;
    CPCMonitor monitor;
    bool linear_palette;
    int sample_rate;       /* 22050 / 44100 */
    bool stereo;
    float volume;          /* 0..1 */
    bool disc_sound, tape_sound;
    bool enable_32_roms;
    bool refresh_50hz;
} CPCConfig;

struct CPC {
    Z80 z80;
    CPCConfig cfg;
    CPCModel model;

    /* memory */
    uint8_t *ram; size_t ram_size;
    uint8_t *rom_lower;            /* 16K or NULL */
    uint8_t *rom_upper[32];        /* 16K each or NULL */
    uint8_t *rom_multiface;
    uint8_t *cartridge;            /* Plus cartridge banks (32 x 16K) */
    int cartridge_banks;
    uint8_t *rd_map[4], *wr_map[4];
    bool lower_rom_en, upper_rom_en;
    uint8_t upper_rom_sel, ram_config;
    uint8_t dummy_rom[16384];

    CPCAsic asic;
    /* Gate array */
    uint8_t ga_pen, ga_mode, ga_ram;
    uint8_t ga_palette[17];        /* hardware colour 0..31 */
    int ga_irq_counter, ga_vsync_delay;
    bool irq_line;
    uint8_t ga_hsync_cnt;
    uint32_t hw_rgb[32];

    /* CRTC / beam */
    CPCCrtc crtc;
    int beam_x, beam_y;            /* pixel / line within visible area */
    int frame_line, disp_x0;       /* CRTC line since frame start; beam x of display char 0 */
    uint32_t scroll_carry[16]; bool scroll_valid;
    int beam_line;                 /* signed line counter since vsync */
    bool beam_vsync;
    int vsync_lines;
    uint32_t *frame;               /* CPC_SCREEN_W * CPC_SCREEN_H, 0xAARRGGBB */
    uint32_t *frame_done;          /* last completed frame */
    bool frame_ready;
    uint64_t frames;

    /* PPI */
    bool single_sided_read[4];
    uint8_t ppi_a, ppi_b, ppi_c, ppi_ctl;
    uint8_t kbd[16];               /* active-low rows */
    bool tape_motor, tape_write_bit;

    /* PSG */
    CPCPsg psg;
    uint8_t psg_ctl;
    uint8_t amdrum;

    /* audio ring buffer (interleaved L/R float) */
    float *audio; int audio_cap; int audio_w, audio_r;
    float audio_acc[2]; int audio_acc_n; int audio_phase;

    /* FDC + discs */
    FDC *fdc;
    CPCDisc *disc[4];
    bool motor;

    /* tape */
    CPCTape *tape;
    uint8_t tape_in;
    bool rec_active; uint8_t *rec_buf; size_t rec_len, rec_cap; int rec_phase;

    uint64_t cycles;               /* elapsed microseconds */
    uint64_t t_counter;            /* debugger T counter */

    /* debugger hooks */
    struct Debugger *dbg;
};

CPC *cpc_create(CPCModel model);
void cpc_destroy(CPC *c);
void cpc_configure(CPC *c, const CPCConfig *cfg);
void cpc_default_config(CPCConfig *cfg, CPCModel model);
void cpc_reset(CPC *c, bool cold);

/* ROM management: slot -1 = lower ROM, 0..31 = upper ROMs. Data is copied. */
bool cpc_load_rom(CPC *c, int slot, const uint8_t *data, size_t size);
void cpc_clear_roms(CPC *c);
bool cpc_load_cartridge(CPC *c, const uint8_t *data, size_t size);

/* Emulation */
int cpc_step(CPC *c);                       /* run a single instruction, returns NOPs */
void cpc_run_frame(CPC *c);                 /* run until the next frame flyback (or ~2 frames of time) */
void cpc_key(CPC *c, int key, bool down);
void cpc_joystick(CPC *c, int bit, bool down);
const uint32_t *cpc_frame(const CPC *c);
/* Post-process a frame to 768x540 (or 384x270). `dst` must hold the full output. */
void cpc_render(const uint32_t *src, uint32_t *dst, bool pal, bool render_both, bool half_size, int brightness);

/* Tape recording (cassette output to WAV bytes) */
void cpc_tape_record_start(CPC *c);
uint8_t *cpc_tape_record_stop(CPC *c, size_t *size);   /* caller frees */

/* Audio: returns frames written (stereo interleaved floats) */
int cpc_audio_read(CPC *c, float *out, int frames);
int cpc_audio_available(const CPC *c);

/* bus helpers for debugger/tests */
uint8_t cpc_peek(CPC *c, uint16_t addr);
void cpc_poke(CPC *c, uint16_t addr, uint8_t v);
uint8_t cpc_mem_read(void *ctx, uint16_t addr);
void cpc_mem_write(void *ctx, uint16_t addr, uint8_t v);

/* PSG */
void psg_reset(CPCPsg *p);
void psg_write_reg(CPCPsg *p, uint8_t reg, uint8_t v);
void psg_tick(CPCPsg *p);
void psg_mix(const CPCPsg *p, float *l, float *r, bool stereo);

/* ASIC (Amstrad Plus) */
void asic_reset(CPC *c);
void asic_unlock_feed(CPC *c, uint8_t v);
void asic_rmr2(CPC *c, uint8_t v);
uint8_t asic_read(CPC *c, uint16_t off);
void asic_write(CPC *c, uint16_t off, uint8_t v);
void asic_ga_palette(CPC *c, int pen, uint8_t hw);
void asic_line(CPC *c);        /* once per scanline (hsync end) */

void cpc_remove_cartridge(CPC *c);

/* internal */
void cpc_update_mapping(CPC *c);
void cpc_compute_mapping(CPC *c, uint8_t ram_cfg, bool lower_en, bool upper_en, uint8_t upper_sel, uint8_t lower_base, uint8_t cart_bank, uint8_t **rd, uint8_t **wr);
extern int cpc_trace_io;
bool cpc_dbg_break(const CPC *c);
void cpc_video_tick(CPC *c);
void cpc_io_write(void *ctx, uint16_t port, uint8_t v);
uint8_t cpc_io_read(void *ctx, uint16_t port);
void crtc_reset(CPCCrtc *c, uint8_t type);

#endif
