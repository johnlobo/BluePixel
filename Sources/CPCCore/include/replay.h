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
CPCRecording *cpc_record_stop(CPC *c);     /* caller owns the result (cpc_recording_free) */
bool cpc_recording_active(const CPC *c);
void cpc_record_taint(CPC *c, const char *reason);          /* something replay cannot reproduce happened */
const char *cpc_recording_taint(const CPCRecording *r);     /* NULL when clean */
const char *cpc_record_taint_now(const CPC *c);             /* same, for the recording in progress */
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
size_t cpc_player_keyframe_bytes(const CPC *c);
#endif
