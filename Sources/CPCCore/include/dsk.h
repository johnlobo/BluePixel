#ifndef CPC_DSK_H
#define CPC_DSK_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DSK_MAX_TRACKS 84
#define DSK_MAX_SECTORS 32

typedef struct {
    uint8_t c, h, r, n, st1, st2;
    uint16_t len;          /* stored data length */
    uint8_t *data;
} CPCSector;

typedef struct {
    int nsectors;
    uint8_t gap3, filler, size_code;
    CPCSector sec[DSK_MAX_SECTORS];
} CPCTrack;

typedef struct CPCDisc {
    char path[1024];
    char name[256];
    int tracks, sides;
    bool extended, dirty, write_protect, temp_writes;
    uint32_t writes;       /* sectors written/formatted by the emulated FDC since the image was loaded (never reset) */
    CPCTrack trk[DSK_MAX_TRACKS][2];
    /* Copy-on-write originals: the first time the emulated FDC changes a track, its previous content is kept here.
     * Savestates embed the tracks that have one, so a session can go back and forth across disc writes. */
    CPCTrack *orig[DSK_MAX_TRACKS][2];
} CPCDisc;

typedef enum { DSK_FORMAT_DATA, DSK_FORMAT_SYSTEM, DSK_FORMAT_IBM } DskFormat;

CPCDisc *dsk_new(void);
CPCDisc *dsk_load(const uint8_t *data, size_t size, const char *path);
CPCDisc *dsk_load_file(const char *path);
uint8_t *dsk_serialize(const CPCDisc *d, size_t *out_size);
bool dsk_save_file(CPCDisc *d, const char *path);
void dsk_free(CPCDisc *d);
CPCDisc *dsk_clone(const CPCDisc *d);        /* deep copy (sector data included) */
void dsk_format(CPCDisc *d, DskFormat f, int tracks, int sides);
void dsk_track_clear(CPCTrack *t);
void dsk_track_copy(CPCTrack *dst, const CPCTrack *src);   /* deep copy; dst is cleared first */
void dsk_touch(CPCDisc *d, const CPCTrack *t);              /* t belongs to d: remember its content before it changes */
void dsk_forget_origins(CPCDisc *d);                        /* the current content becomes the reference */
uint32_t dsk_content_crc(const CPCDisc *d);                 /* geometry + sector ids and data; independent of the file format */
/* Compact byte form of one track, for savestates and recordings. */
size_t dsk_track_pack_size(const CPCTrack *t);
uint8_t *dsk_track_pack(const CPCTrack *t, uint8_t *out);   /* returns the end of what it wrote */
const uint8_t *dsk_track_unpack(CPCTrack *t, const uint8_t *p, const uint8_t *end);   /* t must be empty; NULL: malformed (t left empty) */
int dsk_sector_size(const CPCSector *s);

typedef struct { char name[9], ext[4]; uint8_t user; bool system, read_only; } DskFile;
/* AMSDOS directory (first extent of each file); -1 when no known format is found. */
int dsk_catalogue(const CPCDisc *d, DskFile *out, int max);
/* System-format disc with a CP/M boot sector (track 0, sector &41). */
bool dsk_is_cpm_boot(const CPCDisc *d);
#endif
