#include "cpc_keys.h"
#include "replay.h"
#include <stdlib.h>
#include <string.h>
#include "ser.h"

typedef struct { char ch; int key; bool shift; } CharMap;

static const CharMap map[] = {
    {'1', CPCK_1, 0}, {'!', CPCK_1, 1}, {'2', CPCK_2, 0}, {'"', CPCK_2, 1}, {'3', CPCK_3, 0}, {'#', CPCK_3, 1},
    {'4', CPCK_4, 0}, {'$', CPCK_4, 1}, {'5', CPCK_5, 0}, {'%', CPCK_5, 1}, {'6', CPCK_6, 0}, {'&', CPCK_6, 1},
    {'7', CPCK_7, 0}, {'\'', CPCK_7, 1}, {'8', CPCK_8, 0}, {'(', CPCK_8, 1}, {'9', CPCK_9, 0}, {')', CPCK_9, 1},
    {'0', CPCK_0, 0}, {'_', CPCK_0, 1}, {'-', CPCK_MINUS, 0}, {'=', CPCK_MINUS, 1}, {'^', CPCK_CARET, 0},
    {'@', CPCK_AT, 0}, {'|', CPCK_AT, 1}, {'[', CPCK_LBRACKET, 0}, {'{', CPCK_LBRACKET, 1},
    {']', CPCK_RBRACKET, 0}, {'}', CPCK_RBRACKET, 1}, {'\\', CPCK_BACKSLASH, 0}, {'`', CPCK_BACKSLASH, 1},
    {';', CPCK_SEMICOLON, 0}, {'+', CPCK_SEMICOLON, 1}, {':', CPCK_COLON, 0}, {'*', CPCK_COLON, 1},
    {',', CPCK_COMMA, 0}, {'<', CPCK_COMMA, 1}, {'.', CPCK_DOT, 0}, {'>', CPCK_DOT, 1},
    {'/', CPCK_SLASH, 0}, {'?', CPCK_SLASH, 1}, {' ', CPCK_SPACE, 0}, {'\n', CPCK_RETURN, 0}, {'\r', CPCK_RETURN, 0},
    {'\t', CPCK_TAB, 0},
};

static const int letter_keys[26] = {
    CPCK_A, CPCK_B, CPCK_C, CPCK_D, CPCK_E, CPCK_F, CPCK_G, CPCK_H, CPCK_I, CPCK_J, CPCK_K, CPCK_L, CPCK_M,
    CPCK_N, CPCK_O, CPCK_P, CPCK_Q, CPCK_R, CPCK_S, CPCK_T, CPCK_U, CPCK_V, CPCK_W, CPCK_X, CPCK_Y, CPCK_Z,
};

int cpc_key_for_char(int ch, bool *shift) {
    *shift = false;
    if (ch >= 'a' && ch <= 'z') return letter_keys[ch - 'a'];
    if (ch >= 'A' && ch <= 'Z') { *shift = true; return letter_keys[ch - 'A']; }
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i].ch == ch) { *shift = map[i].shift; return map[i].key; }
    return CPCK_NONE;
}

struct CPCAutoType { char *text; size_t pos; int phase; int hold; int key; bool shift; bool active; };
typedef struct CPCAutoType AutoType;
/* Owned by the machine (c->autotype), created on first use, freed by cpc_destroy. It used to live in a static
 * table of 8 slots keyed by the CPC pointer: a ninth machine (the session indexer clones machines) shared slot 0
 * with another, and a new machine allocated at a freed one's address inherited its queue. */
static AutoType *at_of(CPC *c) {
    if (!c->autotype) { c->autotype = calloc(1, sizeof *c->autotype); if (!c->autotype) abort(); }
    return c->autotype;
}
void cpc_autotype_free(CPC *c) {
    if (!c->autotype) return;
    free(c->autotype->text); free(c->autotype); c->autotype = NULL;
}

void cpc_autotype_raw(CPC *c, const char *text);
void cpc_autotype(CPC *c, const char *text) {
    if (!replay_input(c, CPC_EV_AUTOTYPE, 0, 0, 0, text)) return;
    cpc_autotype_raw(c, text);
}
void cpc_autotype_raw(CPC *c, const char *text) {
    AutoType *a = at_of(c);
    free(a->text); a->text = strdup(text); a->pos = 0; a->phase = 0; a->hold = 0; a->active = true;
}
bool cpc_autotype_active(const CPC *c) { return at_of((CPC *)c)->active; }
void cpc_autotype_cancel_raw(CPC *c);
void cpc_autotype_cancel(CPC *c) {
    if (!replay_input(c, CPC_EV_AUTOTYPE_CANCEL, 0, 0, 0, NULL)) return;
    cpc_autotype_cancel_raw(c);
}
void cpc_autotype_cancel_raw(CPC *c) {
    AutoType *a = at_of(c);
    if (a->key >= 0 && a->hold) { cpc_key_raw(c, a->key, false); if (a->shift) cpc_key_raw(c, CPCK_SHIFT, false); }
    a->active = false; a->hold = 0;
}

void cpc_autotype_tick(CPC *c) {
    AutoType *a = at_of(c);
    if (!a->active) return;
    if (a->hold > 0) {
        if (--a->hold == 0) {
            cpc_key_raw(c, a->key, false); if (a->shift) cpc_key_raw(c, CPCK_SHIFT, false);
            a->hold = -2;                                  /* release gap */
        }
        return;
    }
    if (a->hold < 0) { a->hold++; return; }
    while (a->text[a->pos]) {
        int k = cpc_key_for_char((unsigned char)a->text[a->pos++], &a->shift);
        if (k == CPCK_NONE) continue;
        a->key = k;
        if (a->shift) cpc_key_raw(c, CPCK_SHIFT, true);
        cpc_key_raw(c, k, true); a->hold = 3; return;
    }
    a->active = false;
}

/* ---------------- state snapshot ---------------- */
void cpc_autotype_state_visit(Ser *s, CPC *c) {
    AutoType *a = at_of(c);
    ser_tag(s, "TYP0");
    bool has = ser_flag(s, a->text != NULL);
    uint32_t tl = ser_count(s, a->text ? (uint32_t)strlen(a->text) : 0, 1u << 20);
    char *nt = NULL;
    if (ser_storing(s) && has) { nt = malloc(tl + 1); if (!nt) { s->bad = true; return; } nt[tl] = 0; }
    ser_bytes(s, ser_storing(s) ? nt : a->text, has ? tl : 0);
    uint64_t pos = ser_le(s, (uint64_t)a->pos, 8);
    if (ser_reading(s) && pos > tl) s->bad = true;           /* pos indexes the text */
    ser_int(s, &a->phase); ser_int(s, &a->hold); ser_int(s, &a->key); ser_bool(s, &a->shift); ser_bool(s, &a->active);
    if (ser_storing(s) && !s->bad) {
        free(a->text); a->text = nt; a->pos = (size_t)pos;
    } else free(nt);
}
