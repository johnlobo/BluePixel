#ifndef CPC_TAPE_H
#define CPC_TAPE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct CPCTape CPCTape;

CPCTape *tape_load(const uint8_t *data, size_t size, const char *name);   /* CDT/TZX/VOC/WAV/CSV */
void tape_free(CPCTape *t);
void tape_set_motor(CPCTape *t, bool on);
void tape_play(CPCTape *t, bool on);
void tape_rewind(CPCTape *t);
bool tape_read_level(CPCTape *t);
void tape_tick(CPCTape *t, int us);
int tape_block(const CPCTape *t);
int tape_block_count(const CPCTape *t);
void tape_seek_block(CPCTape *t, int block);
bool tape_playing(const CPCTape *t);
double tape_position_seconds(const CPCTape *t);
#endif
