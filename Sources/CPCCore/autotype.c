#include "cpc_keys.h"
#include <stdlib.h>
#include <string.h>

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

typedef struct { char *text; size_t pos; int phase; int hold; int key; bool shift; bool active; } AutoType;
static AutoType *at_of(CPC *c) {
    /* the autotype state lives in a side table keyed by CPC pointer (single instance is typical) */
    static AutoType states[8]; static CPC *owners[8];
    for (int i = 0; i < 8; i++) if (owners[i] == c) return &states[i];
    for (int i = 0; i < 8; i++) if (!owners[i]) { owners[i] = c; return &states[i]; }
    return &states[0];
}

void cpc_autotype(CPC *c, const char *text) {
    AutoType *a = at_of(c);
    free(a->text); a->text = strdup(text); a->pos = 0; a->phase = 0; a->hold = 0; a->active = true;
}
bool cpc_autotype_active(const CPC *c) { return at_of((CPC *)c)->active; }
void cpc_autotype_cancel(CPC *c) {
    AutoType *a = at_of(c);
    if (a->key >= 0 && a->hold) { cpc_key(c, a->key, false); if (a->shift) cpc_key(c, CPCK_SHIFT, false); }
    a->active = false; a->hold = 0;
}

void cpc_autotype_tick(CPC *c) {
    AutoType *a = at_of(c);
    if (!a->active) return;
    if (a->hold > 0) {
        if (--a->hold == 0) {
            cpc_key(c, a->key, false); if (a->shift) cpc_key(c, CPCK_SHIFT, false);
            a->hold = -2;                                  /* release gap */
        }
        return;
    }
    if (a->hold < 0) { a->hold++; return; }
    while (a->text[a->pos]) {
        int k = cpc_key_for_char((unsigned char)a->text[a->pos++], &a->shift);
        if (k == CPCK_NONE) continue;
        a->key = k;
        if (a->shift) cpc_key(c, CPCK_SHIFT, true);
        cpc_key(c, k, true); a->hold = 3; return;
    }
    a->active = false;
}
