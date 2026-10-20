#ifndef CPC_KEYS_H
#define CPC_KEYS_H
#include "cpc.h"

/* CPC keyboard matrix positions: CPC_KEY(line, bit) */
enum {
    CPCK_UP = CPC_KEY(0,0), CPCK_RIGHT = CPC_KEY(0,1), CPCK_DOWN = CPC_KEY(0,2), CPCK_F9 = CPC_KEY(0,3),
    CPCK_F6 = CPC_KEY(0,4), CPCK_F3 = CPC_KEY(0,5), CPCK_ENTER = CPC_KEY(0,6), CPCK_FDOT = CPC_KEY(0,7),
    CPCK_LEFT = CPC_KEY(1,0), CPCK_COPY = CPC_KEY(1,1), CPCK_F7 = CPC_KEY(1,2), CPCK_F8 = CPC_KEY(1,3),
    CPCK_F5 = CPC_KEY(1,4), CPCK_F1 = CPC_KEY(1,5), CPCK_F2 = CPC_KEY(1,6), CPCK_F0 = CPC_KEY(1,7),
    CPCK_CLR = CPC_KEY(2,0), CPCK_LBRACKET = CPC_KEY(2,1), CPCK_RETURN = CPC_KEY(2,2), CPCK_RBRACKET = CPC_KEY(2,3),
    CPCK_F4 = CPC_KEY(2,4), CPCK_SHIFT = CPC_KEY(2,5), CPCK_BACKSLASH = CPC_KEY(2,6), CPCK_CTRL = CPC_KEY(2,7),
    CPCK_CARET = CPC_KEY(3,0), CPCK_MINUS = CPC_KEY(3,1), CPCK_AT = CPC_KEY(3,2), CPCK_P = CPC_KEY(3,3),
    CPCK_SEMICOLON = CPC_KEY(3,4), CPCK_COLON = CPC_KEY(3,5), CPCK_SLASH = CPC_KEY(3,6), CPCK_DOT = CPC_KEY(3,7),
    CPCK_0 = CPC_KEY(4,0), CPCK_9 = CPC_KEY(4,1), CPCK_O = CPC_KEY(4,2), CPCK_I = CPC_KEY(4,3),
    CPCK_L = CPC_KEY(4,4), CPCK_K = CPC_KEY(4,5), CPCK_M = CPC_KEY(4,6), CPCK_COMMA = CPC_KEY(4,7),
    CPCK_8 = CPC_KEY(5,0), CPCK_7 = CPC_KEY(5,1), CPCK_U = CPC_KEY(5,2), CPCK_Y = CPC_KEY(5,3),
    CPCK_H = CPC_KEY(5,4), CPCK_J = CPC_KEY(5,5), CPCK_N = CPC_KEY(5,6), CPCK_SPACE = CPC_KEY(5,7),
    CPCK_6 = CPC_KEY(6,0), CPCK_5 = CPC_KEY(6,1), CPCK_R = CPC_KEY(6,2), CPCK_T = CPC_KEY(6,3),
    CPCK_G = CPC_KEY(6,4), CPCK_F = CPC_KEY(6,5), CPCK_B = CPC_KEY(6,6), CPCK_V = CPC_KEY(6,7),
    CPCK_4 = CPC_KEY(7,0), CPCK_3 = CPC_KEY(7,1), CPCK_E = CPC_KEY(7,2), CPCK_W = CPC_KEY(7,3),
    CPCK_S = CPC_KEY(7,4), CPCK_D = CPC_KEY(7,5), CPCK_C = CPC_KEY(7,6), CPCK_X = CPC_KEY(7,7),
    CPCK_1 = CPC_KEY(8,0), CPCK_2 = CPC_KEY(8,1), CPCK_ESC = CPC_KEY(8,2), CPCK_Q = CPC_KEY(8,3),
    CPCK_TAB = CPC_KEY(8,4), CPCK_A = CPC_KEY(8,5), CPCK_CAPS = CPC_KEY(8,6), CPCK_Z = CPC_KEY(8,7),
    CPCK_JOY_UP = CPC_KEY(9,0), CPCK_JOY_DOWN = CPC_KEY(9,1), CPCK_JOY_LEFT = CPC_KEY(9,2), CPCK_JOY_RIGHT = CPC_KEY(9,3),
    CPCK_JOY_FIRE1 = CPC_KEY(9,4), CPCK_JOY_FIRE2 = CPC_KEY(9,5), CPCK_JOY_FIRE3 = CPC_KEY(9,6), CPCK_DEL = CPC_KEY(9,7),
    CPCK_NONE = -1
};

/* Maps an ASCII character to a key (and whether SHIFT/CTRL are required). Returns CPCK_NONE if unmapped. */
int cpc_key_for_char(int ch, bool *shift);
const char *cpc_key_name(int key);

/* Auto-type: queues text; '\n' = RETURN. Escape sequences: "\\x01".. are not used; use cpc_autotype_key for specials. */
void cpc_autotype(CPC *c, const char *text);
bool cpc_autotype_active(const CPC *c);
void cpc_autotype_cancel(CPC *c);
void cpc_autotype_raw(CPC *c, const char *text);
void cpc_autotype_cancel_raw(CPC *c);
void cpc_autotype_tick(CPC *c);   /* called once per frame by cpc_run_frame */
#endif
