#ifndef CPC_FDC_H
#define CPC_FDC_H
#include <stdint.h>
#include <stdbool.h>
#include "dsk.h"

typedef struct CPC CPC;
typedef struct FDC FDC;

FDC *fdc_create(CPC *cpc);
/* Machine-state snapshot of the controller (for savestates / replay). Pointers into discs are stored as indices. */
uint8_t *fdc_state_save(const FDC *f, size_t *size);          /* malloc'd; caller frees */
bool fdc_state_restore(FDC *f, const uint8_t *data, size_t size);
void fdc_destroy(FDC *f);
void fdc_reset(FDC *f);
uint8_t fdc_read_status(FDC *f);
uint8_t fdc_read_data(FDC *f);
void fdc_write_data(FDC *f, uint8_t v);
void fdc_set_motor(FDC *f, bool on);
void fdc_tick(FDC *f, int us);
/* Drive/disc management (disc ownership stays with the caller unless noted) */
void fdc_insert(FDC *f, int drive, CPCDisc *d);
CPCDisc *fdc_disc(FDC *f, int drive);
int fdc_track(FDC *f, int drive);
bool fdc_motor(FDC *f);
bool fdc_busy(FDC *f);
#endif
