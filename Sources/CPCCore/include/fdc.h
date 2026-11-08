#ifndef CPC_FDC_H
#define CPC_FDC_H
#include <stdint.h>
#include <stdbool.h>
#include "dsk.h"

typedef struct CPC CPC;
typedef struct FDC FDC;

FDC *fdc_create(CPC *cpc);
/* Canonical state of the controller (savestates / replay / hash; see ser.h). Pointers into discs are stored as indices. */
struct Ser;
void fdc_state_visit(struct Ser *s, FDC *f);
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
