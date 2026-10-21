#ifndef CPC_REPLAY_H
#define CPC_REPLAY_H
#include "cpc.h"
#include "state.h"

/* Session recording and replay.
 *
 * A recording is the full machine state at the start plus every host input stamped with the emulated
 * microsecond it happened at. The core is deterministic, so replaying the inputs reproduces the session
 * exactly; a hash of the whole state every CPC_CHECK_FRAMES frames detects any divergence ("desync").
 * Seeking restores the nearest keyframe (taken while replaying) and re-simulates up to the target frame,
 * which is cheap because the core runs about 50x faster than real time.
 *
 * Inputs recorded: keys, joystick, AMX mouse, auto-type, reset, Multiface Stop. Anything else that changes the
 * machine (media, configuration, memory/register edits) cannot be reproduced and "taints" the recording. */

#define CPC_CHECK_FRAMES 50        /* checkpoint hash + keyframe spacing (1 s at 50 Hz) */

typedef enum { CPC_EV_KEY = 1, CPC_EV_JOY, CPC_EV_AMX, CPC_EV_AUTOTYPE, CPC_EV_AUTOTYPE_CANCEL, CPC_EV_RESET, CPC_EV_MF_STOP } CPCEventType;

typedef struct CPCRecording CPCRecording;
typedef struct CPCPlayer CPCPlayer;

/* --- host input gate (called by cpc_key & co): false = ignore (replay running); logs when recording --- */
bool replay_input(CPC *c, int type, int a, int b, int d, const char *text);
void replay_apply_due(CPC *c);             /* replayer: apply events due before the next instruction */
void replay_frame_done(CPC *c);            /* instruction boundary right after a frame flyback */

/* --- recording --- */
bool cpc_record_start(CPC *c);             /* captures the current state and starts logging (false: already recording/replaying) */
CPCRecording *cpc_record_stop(CPC *c);     /* caller owns the result (cpc_recording_free); also collects one cut short by a taint */
bool cpc_recording_active(const CPC *c);
void cpc_record_taint(CPC *c, const char *reason);          /* something replay cannot reproduce is about to happen: ends the recording here */
const char *cpc_recording_taint(const CPCRecording *r);     /* why it ended early; NULL when stopped normally */
const char *cpc_record_cut_reason(const CPC *c);            /* non-NULL: a recording was cut short and waits for cpc_record_stop */
void cpc_recording_set_meta(CPCRecording *r, const char *meta);
const char *cpc_recording_meta(const CPCRecording *r);
/* Is `d` the disc that was in drive `unit` when recording began (always true when the drive was empty)? Tracks the session wrote to
 * are compared as they were at the start, so a disc saved after the session still matches. */
bool cpc_recording_disc_matches(const CPCRecording *r, int unit, const CPCDisc *d);
void cpc_recording_free(CPCRecording *r);
uint64_t cpc_recording_frames(const CPCRecording *r);       /* length in frames */
uint64_t cpc_recording_events(const CPCRecording *r);
uint8_t *cpc_recording_serialize(const CPCRecording *r, size_t *size);   /* caller frees; build-specific */
CPCRecording *cpc_recording_deserialize(const uint8_t *data, size_t size);

/* --- playback --- */
CPCPlayer *cpc_player_create(CPCRecording *r);              /* does not own r */
void cpc_player_free(CPCPlayer *p);
bool cpc_player_begin(CPC *c, CPCPlayer *p, unsigned *hazards);   /* restores the start state; the machine now replays */
void cpc_player_end(CPC *c);                                /* leave replay: the machine continues live from here */
bool cpc_player_active(const CPC *c);
bool cpc_player_seek(CPC *c, uint64_t frame);               /* frame relative to the start; silent (debugger detached) */
uint64_t cpc_player_frame(const CPC *c);
uint64_t cpc_player_length(const CPC *c);
bool cpc_player_finished(const CPC *c);                     /* reached the end of the recording */
bool cpc_player_desync(const CPC *c, uint64_t *frame);      /* true once a checkpoint hash did not match */
size_t cpc_player_keyframes(const CPC *c);                  /* keyframes cached so far */
uint64_t cpc_player_indexed(const CPC *c);                  /* frames up to which seeking is fast (contiguous keyframes) */

/* Pictures: frames from+1 .. from+count as displayed (frame_done after each flyback), for reverse playback.
 * Leaves the machine at the last captured frame. Returns the number captured. */
int cpc_player_capture(CPC *c, uint64_t from, int count, uint32_t *out);

/* Background indexing: a second machine with the same hardware and media replays the whole recording on another
 * thread, and its keyframes are moved into the visible machine's player so that seeking anywhere is fast. */
CPC *cpc_clone_machine(const CPC *src);                     /* same config, ROMs, cartridge and (copies of) media; no state */
void cpc_clone_free(CPC *c);
size_t cpc_player_adopt_keyframes(CPC *c, CPCPlayer *src);  /* moves keyframes c's player lacks; call with c's lock held */
size_t cpc_player_keyframe_bytes(const CPC *c);

/* --- reverse debugging (needs a replay; positions are emulated microseconds, c->cycles) --- */
bool cpc_player_goto_cycles(CPC *c, uint64_t cycles, bool rebuild_history);   /* exact position; refills the execution history */
bool cpc_player_step_back(CPC *c);                                           /* state before the last executed step; false at the start */

typedef enum { CPC_REV_BREAKPOINTS, CPC_REV_WRITE, CPC_REV_READ, CPC_REV_CODE } CPCReverseMode;
typedef struct {
    bool found;
    uint64_t cycles;          /* position just before the instruction that triggered */
    uint16_t pc;              /* address of that instruction */
    int kind;                 /* 1 code, 2 memory read, 3 memory write, 4 I/O */
    int bp_index;
    uint16_t addr; uint8_t value, previous;   /* memory events */
} CPCReverseHit;
typedef struct CPCReverseScan CPCReverseScan;

/* Search backwards for the latest hit before the current position: breakpoints as the user set them (conditions
 * honoured, pass counts and logpoints ignored), the last write/read of memory lo..hi, or the last time PC was at lo.
 * The scan works one keyframe interval per slice so a frontend can keep its UI alive and offer cancel. */
CPCReverseScan *cpc_reverse_begin(CPC *c, CPCReverseMode mode, uint16_t lo, uint16_t hi);   /* NULL: not replaying / no free breakpoint */
bool cpc_reverse_slice(CPC *c, CPCReverseScan *s);                  /* true when the scan is over */
double cpc_reverse_progress(const CPCReverseScan *s);               /* 0..1 of the way back to the start */
uint64_t cpc_reverse_cycles(const CPCReverseScan *s);               /* position being scanned */
bool cpc_reverse_end(CPC *c, CPCReverseScan *s, bool keep, CPCReverseHit *hit);   /* restores the debugger, positions the machine, frees s */
#endif
