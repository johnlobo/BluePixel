#ifndef CPC_DEBUG_H
#define CPC_DEBUG_H
#include "cpc.h"

typedef enum { BP_CODE, BP_MEM_READ, BP_MEM_WRITE, BP_IO } BpType;

typedef struct {
    BpType type;
    bool enabled, used;
    /* code & memory breakpoints: 16K block identity + offset range inside it */
    const uint8_t *block;          /* 16K block base the address belongs to (NULL: any mapping) */
    uint16_t start, end;           /* CPU address range (code: start only) */
    /* I/O breakpoints */
    uint16_t port, mask;
    bool on_in, on_out;
    char cond[160];
    int pass_count, hits, count;   /* hits = times valid, count = times condition true */
} DbgBreakpoint;

typedef struct { uint16_t start, end; bool words, labels, used; } DbgDataArea;
typedef struct { char name[48]; int32_t value; } DbgSymbol;
typedef struct { uint16_t pc, af, bc, de, hl, ix, iy, sp; } DbgHistory;
#define DBG_HISTORY 4096
typedef struct { int id; uint64_t started; bool running; uint32_t count, last, min, max; uint64_t total; bool used; } DbgTimer;

#define DBG_MAX_BP 256
#define DBG_MAX_AREAS 64
#define DBG_MAX_TIMERS 32

typedef struct Debugger {
    bool active;                   /* master breakpoints enable */
    bool break_instructions;       /* ED FF = BRK */
    bool skip_once;                /* skip code-breakpoint check at the current PC once (after resuming) */
    DbgBreakpoint bp[DBG_MAX_BP];
    DbgDataArea areas[DBG_MAX_AREAS];
    DbgSymbol *syms; int nsyms, syms_cap;
    DbgTimer timers[DBG_MAX_TIMERS];
    int temp_bp;                   /* run-to address, -1 when unused */
    bool step_out; uint16_t step_out_sp;
    bool hist_on; int hist_pos, hist_count;  /* execution history ring (last DBG_HISTORY instructions) */
    DbgHistory hist[DBG_HISTORY];   /* stop after a RET that leaves the frame entered at step_out_sp */
    uint8_t watch[65536];          /* bit0 = read watch, bit1 = write watch */
    int io_bps;
    bool hit; int hit_index;       /* last breakpoint that stopped the machine */
    uint16_t ev_addr; uint8_t ev_value, ev_prev; bool ev_valid;
    uint64_t reset_cycles_base;
    char last_error[128];
} Debugger;

Debugger *dbg_attach(CPC *c);
void dbg_detach(CPC *c);
void dbg_rebuild_watch(Debugger *d);
static inline uint8_t dbg_watch_bits(const Debugger *d, uint16_t addr) { return d->watch[addr] & 3; }   /* 1 read, 2 write */

int dbg_add_code_bp(CPC *c, uint16_t addr);                    /* returns index or -1 */
int dbg_add_mem_bp(CPC *c, uint16_t start, uint16_t end, bool read, bool write);
int dbg_add_io_bp(CPC *c, uint16_t port, uint16_t mask, bool in, bool out);
void dbg_remove_bp(CPC *c, int index);
void dbg_clear_bps(CPC *c);
void dbg_toggle_code_bp(CPC *c, uint16_t addr);
bool dbg_set_condition(CPC *c, int index, const char *cond, int pass_count);

/* Evaluate a debugger expression (registers, variables, functions, symbols). */
bool dbg_eval(CPC *c, const char *expr, int32_t *out, char *err, size_t errsize);

/* Execution control. Return true when the machine stopped because of a debugger event. */
bool dbg_check_code(CPC *c);                       /* called before each instruction */
void dbg_mem_event(CPC *c, uint16_t addr, uint8_t value, bool write);
void dbg_io_event(CPC *c, uint16_t port, uint8_t value, bool write);
void dbg_step_into(CPC *c);
bool dbg_step_over(CPC *c);                        /* true when the machine must keep running until the next instruction */
void dbg_run_to(CPC *c, uint16_t addr);
void dbg_step_out(CPC *c);
void dbg_history_record(CPC *c);
int dbg_history_count(const Debugger *d);
DbgHistory dbg_history_get(const Debugger *d, int back);   /* back = 0: most recent instruction */                         /* run until the current function returns */

/* Symbols & data areas */
void dbg_symbol_set(CPC *c, const char *name, int32_t value);
void dbg_symbol_clear(CPC *c);
bool dbg_symbol_lookup(const Debugger *d, const char *name, int32_t *v);
const char *dbg_symbol_for_address(const Debugger *d, uint16_t addr);
int dbg_symbols_load(CPC *c, const char *path);
void dbg_area_add(CPC *c, uint16_t start, uint16_t end, bool words);
void dbg_area_remove(CPC *c, uint16_t addr);
const DbgDataArea *dbg_area_at(const Debugger *d, uint16_t addr);

/* Memory views */
typedef struct { int mode; bool lower, upper; int upper_rom; uint8_t ram_cfg; uint8_t lower_base, cart_bank; } DbgView; /* mode 0=read 1=write 2=any; lower_base/cart_bank: Plus secondary ROM mapping */
uint8_t dbg_view_read(CPC *c, const DbgView *v, uint16_t addr);
void dbg_view_write(CPC *c, const DbgView *v, uint16_t addr, uint8_t value);
void dbg_view_current(CPC *c, DbgView *v);

/* Disassemble one line honouring data areas and symbols. Returns instruction length. */
int dbg_disasm(CPC *c, const DbgView *v, uint16_t addr, char *out, size_t size);
void dbg_describe_address(CPC *c, uint16_t addr, char *out, size_t size);   /* e.g. "0006:Upper 7" */

/* Search / fill / compare helpers */
int dbg_find(CPC *c, const DbgView *v, uint16_t from, const uint8_t *pat, const bool *wild, int len, bool forward);
void dbg_fill(CPC *c, const DbgView *v, uint16_t start, uint16_t end, const uint8_t *pat, int len);
#endif
