#ifndef CPC_STATE_H
#define CPC_STATE_H
#include "cpc.h"

/* Complete, exact machine state: everything that can influence what the emulated machine does next
 * (CPU, gate array, CRTC incl. beam, PPI, PSG, ASIC, RAM, both frame buffers, FDC, tape position, auto-type,
 * Multiface RAM, keyboard matrix). Not included: media (discs, tapes, ROMs), configuration that only affects
 * presentation (monitor, volume), audio queue and debugger state. Restoring is exact only into a machine
 * configured and loaded with the same media; mismatches are reported as hazards. */
typedef struct CPCState CPCState;

enum {
    CPC_STATE_HAZARD_DISC = 1,     /* a drive has a disc now that had none when the state was taken, or the other way round */
    CPC_STATE_HAZARD_TAPE = 2,     /* the inserted tape is not the one the state was taken with */
    CPC_STATE_HAZARD_CONFIG = 4,   /* hardware configuration (model, RAM, CRTC type...) differs */
};

CPCState *cpc_state_save(CPC *c);                                       /* NULL on failure */
bool cpc_state_restore(CPC *c, const CPCState *s, unsigned *hazards);   /* false: unusable (different build/RAM size) */
void cpc_state_free(CPCState *s);
uint32_t cpc_crc32(const uint8_t *data, size_t size);                  /* for identifying media in recordings */
uint64_t cpc_state_hash(CPC *c);                                        /* hash of the same content, for sync checks */
uint64_t cpc_state_frames(const CPCState *s);                           /* c->frames when taken */
uint64_t cpc_state_cycles(const CPCState *s);
size_t cpc_state_size(const CPCState *s);                               /* bytes in memory (compressed) */

/* Byte form for files: version- and build-specific (it embeds the CPC struct layout). */
const uint8_t *cpc_state_bytes(const CPCState *s, size_t *size);
CPCState *cpc_state_from_bytes(const uint8_t *data, size_t size);       /* validates; copies */
#endif
