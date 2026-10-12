#include "tape.h"
#include <stdlib.h>
/* Minimal placeholder; full implementation follows. */
CPCTape *tape_load(const uint8_t *d, size_t s, const char *n) { (void)d; (void)s; (void)n; return NULL; }
void tape_free(CPCTape *t) { (void)t; }
void tape_set_motor(CPCTape *t, bool on) { (void)t; (void)on; }
void tape_play(CPCTape *t, bool on) { (void)t; (void)on; }
void tape_rewind(CPCTape *t) { (void)t; }
bool tape_read_level(CPCTape *t) { (void)t; return false; }
void tape_tick(CPCTape *t, int us) { (void)t; (void)us; }
int tape_block(const CPCTape *t) { (void)t; return 0; }
int tape_block_count(const CPCTape *t) { (void)t; return 0; }
void tape_seek_block(CPCTape *t, int b) { (void)t; (void)b; }
bool tape_playing(const CPCTape *t) { (void)t; return false; }
double tape_position_seconds(const CPCTape *t) { (void)t; return 0; }
