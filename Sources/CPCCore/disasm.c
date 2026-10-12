#include "z80.h"
#include <stdio.h>
#include <string.h>

typedef struct { uint16_t pc; uint8_t (*peek)(void *, uint16_t); void *ctx; } Rd;
static uint8_t b8(Rd *r) { return r->peek(r->ctx, r->pc++); }
static uint16_t b16(Rd *r) { uint8_t l = b8(r); uint8_t h = b8(r); return (uint16_t)(l | (h << 8)); }

static const char *R8[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
static const char *RP[4] = {"BC", "DE", "HL", "SP"};
static const char *RP2[4] = {"BC", "DE", "HL", "AF"};
static const char *CC[8] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
static const char *ALU[8] = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
static const char *ROT[8] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};

int z80_disassemble(uint16_t pc, uint8_t (*peek)(void *, uint16_t), void *ctx, char *out, int outsize) {
    Rd r = {pc, peek, ctx};
    char s[48]; s[0] = 0;
    const char *ixn = NULL;                       /* "IX"/"IY" if prefixed */
    uint8_t op = b8(&r);
    while (op == 0xDD || op == 0xFD) {
        ixn = op == 0xDD ? "IX" : "IY";
        uint8_t nx = peek(ctx, r.pc);
        if (nx == 0xDD || nx == 0xFD) { snprintf(s, sizeof s, "DEFB #%02X", op); goto done; }
        op = b8(&r);
    }
    char hlr[8], mem[24], rn[8][12];
    snprintf(hlr, sizeof hlr, "%s", ixn ? ixn : "HL");
    for (int i = 0; i < 8; i++) snprintf(rn[i], sizeof rn[i], "%s", R8[i]);
    if (ixn) { snprintf(rn[4], sizeof rn[4], "%sH", ixn); snprintf(rn[5], sizeof rn[5], "%sL", ixn); }
    mem[0] = 0;
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    #define MEMOP() do { if (ixn) { int8_t d = (int8_t)b8(&r); snprintf(mem, sizeof mem, "(%s%c#%02X)", ixn, d < 0 ? '-' : '+', d < 0 ? -d : d); } else snprintf(mem, sizeof mem, "(HL)"); } while (0)
    #define REG(i) ((i) == 6 ? mem : rn[i])
    const char *rp[4] = {"BC", "DE", NULL, "SP"}; rp[2] = hlr;
    const char *rp2[4] = {"BC", "DE", NULL, "AF"}; rp2[2] = hlr;
    if (op == 0xCB) {
        if (ixn) {
            int8_t d = (int8_t)b8(&r); uint8_t cb = b8(&r);
            int cx = cb >> 6, cy = (cb >> 3) & 7, cz = cb & 7;
            char m[24]; snprintf(m, sizeof m, "(%s%c#%02X)", ixn, d < 0 ? '-' : '+', d < 0 ? -d : d);
            if (cx == 0) { if (cz == 6) snprintf(s, sizeof s, "%s %s", ROT[cy], m); else snprintf(s, sizeof s, "%s %s,%s", ROT[cy], m, R8[cz]); }
            else if (cx == 1) snprintf(s, sizeof s, "BIT %d,%s", cy, m);
            else if (cz == 6) snprintf(s, sizeof s, "%s %d,%s", cx == 2 ? "RES" : "SET", cy, m);
            else snprintf(s, sizeof s, "%s %d,%s,%s", cx == 2 ? "RES" : "SET", cy, m, R8[cz]);
        } else {
            uint8_t cb = b8(&r); int cx = cb >> 6, cy = (cb >> 3) & 7, cz = cb & 7;
            if (cx == 0) snprintf(s, sizeof s, "%s %s", ROT[cy], R8[cz]);
            else snprintf(s, sizeof s, "%s %d,%s", cx == 1 ? "BIT" : cx == 2 ? "RES" : "SET", cy, R8[cz]);
        }
        goto done;
    }
    if (op == 0xED) {
        uint8_t e = b8(&r); int ex = e >> 6, ey = (e >> 3) & 7, ez = e & 7, ep = ey >> 1, eq = ey & 1;
        if (ex == 1) {
            switch (ez) {
            case 0: if (ey == 6) snprintf(s, sizeof s, "IN F,(C)"); else snprintf(s, sizeof s, "IN %s,(C)", R8[ey]); break;
            case 1: if (ey == 6) snprintf(s, sizeof s, "OUT (C),0"); else snprintf(s, sizeof s, "OUT (C),%s", R8[ey]); break;
            case 2: snprintf(s, sizeof s, "%s HL,%s", eq ? "ADC" : "SBC", RP[ep]); break;
            case 3: { uint16_t a = b16(&r); if (eq) snprintf(s, sizeof s, "LD %s,(#%04X)", RP[ep], a); else snprintf(s, sizeof s, "LD (#%04X),%s", a, RP[ep]); break; }
            case 4: snprintf(s, sizeof s, "NEG"); break;
            case 5: snprintf(s, sizeof s, ey == 1 ? "RETI" : "RETN"); break;
            case 6: snprintf(s, sizeof s, "IM %d", (ey & 3) < 2 ? 0 : (ey & 3) - 1); break;
            default: { const char *t[8] = {"LD I,A", "LD R,A", "LD A,I", "LD A,R", "RRD", "RLD", "NOP", "NOP"}; snprintf(s, sizeof s, "%s", t[ey]); }
            }
        } else if (ex == 2 && ey >= 4 && ez < 4) {
            static const char *bl[4][4] = {{"LDI", "CPI", "INI", "OUTI"}, {"LDD", "CPD", "IND", "OUTD"}, {"LDIR", "CPIR", "INIR", "OTIR"}, {"LDDR", "CPDR", "INDR", "OTDR"}};
            snprintf(s, sizeof s, "%s", bl[ey - 4][ez]);
        } else if (e == 0xFF) snprintf(s, sizeof s, "BRK");
        else snprintf(s, sizeof s, "DEFB #ED,#%02X", e);
        goto done;
    }
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 0) snprintf(s, sizeof s, "NOP");
            else if (y == 1) snprintf(s, sizeof s, "EX AF,AF'");
            else { int8_t d = (int8_t)b8(&r); uint16_t t = r.pc + d;
                if (y == 2) snprintf(s, sizeof s, "DJNZ #%04X", t); else if (y == 3) snprintf(s, sizeof s, "JR #%04X", t); else snprintf(s, sizeof s, "JR %s,#%04X", CC[y - 4], t); }
            break;
        case 1: if (!q) { uint16_t n = b16(&r); snprintf(s, sizeof s, "LD %s,#%04X", rp[p], n); } else snprintf(s, sizeof s, "ADD %s,%s", hlr, rp[p]); break;
        case 2:
            switch (y) {
            case 0: snprintf(s, sizeof s, "LD (BC),A"); break; case 1: snprintf(s, sizeof s, "LD A,(BC)"); break;
            case 2: snprintf(s, sizeof s, "LD (DE),A"); break; case 3: snprintf(s, sizeof s, "LD A,(DE)"); break;
            case 4: { uint16_t a = b16(&r); snprintf(s, sizeof s, "LD (#%04X),%s", a, hlr); break; }
            case 5: { uint16_t a = b16(&r); snprintf(s, sizeof s, "LD %s,(#%04X)", hlr, a); break; }
            case 6: { uint16_t a = b16(&r); snprintf(s, sizeof s, "LD (#%04X),A", a); break; }
            default: { uint16_t a = b16(&r); snprintf(s, sizeof s, "LD A,(#%04X)", a); break; }
            }
            break;
        case 3: snprintf(s, sizeof s, "%s %s", q ? "DEC" : "INC", rp[p]); break;
        case 4: case 5: MEMOP(); snprintf(s, sizeof s, "%s %s", z == 4 ? "INC" : "DEC", REG(y)); break;
        case 6: { MEMOP(); uint8_t n = b8(&r); snprintf(s, sizeof s, "LD %s,#%02X", REG(y), n); break; }
        default: { const char *t[8] = {"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"}; snprintf(s, sizeof s, "%s", t[y]); }
        }
        break;
    case 1:
        if (op == 0x76) snprintf(s, sizeof s, "HALT");
        else if (y == 6 || z == 6) { MEMOP(); snprintf(s, sizeof s, "LD %s,%s", y == 6 ? mem : R8[y], z == 6 ? mem : R8[z]); }
        else snprintf(s, sizeof s, "LD %s,%s", rn[y], rn[z]);
        break;
    case 2: MEMOP(); snprintf(s, sizeof s, "%s%s", ALU[y], REG(z)); break;
    default:
        switch (z) {
        case 0: snprintf(s, sizeof s, "RET %s", CC[y]); break;
        case 1:
            if (!q) snprintf(s, sizeof s, "POP %s", rp2[p]);
            else { const char *t[4] = {"RET", "EXX", NULL, "LD SP,"}; if (p == 2) snprintf(s, sizeof s, "JP (%s)", hlr); else if (p == 3) snprintf(s, sizeof s, "LD SP,%s", hlr); else snprintf(s, sizeof s, "%s", t[p]); }
            break;
        case 2: { uint16_t a = b16(&r); snprintf(s, sizeof s, "JP %s,#%04X", CC[y], a); break; }
        case 3:
            switch (y) {
            case 0: { uint16_t a = b16(&r); snprintf(s, sizeof s, "JP #%04X", a); break; }
            case 2: { uint8_t n = b8(&r); snprintf(s, sizeof s, "OUT (#%02X),A", n); break; }
            case 3: { uint8_t n = b8(&r); snprintf(s, sizeof s, "IN A,(#%02X)", n); break; }
            case 4: snprintf(s, sizeof s, "EX (SP),%s", hlr); break;
            case 5: snprintf(s, sizeof s, "EX DE,HL"); break;
            case 6: snprintf(s, sizeof s, "DI"); break;
            default: snprintf(s, sizeof s, "EI");
            }
            break;
        case 4: { uint16_t a = b16(&r); snprintf(s, sizeof s, "CALL %s,#%04X", CC[y], a); break; }
        case 5: if (!q) snprintf(s, sizeof s, "PUSH %s", rp2[p]); else { uint16_t a = b16(&r); snprintf(s, sizeof s, "CALL #%04X", a); } break;
        case 6: { uint8_t n = b8(&r); snprintf(s, sizeof s, "%s#%02X", ALU[y], n); break; }
        default: snprintf(s, sizeof s, "RST #%02X", y * 8);
        }
    }
done:
    snprintf(out, outsize, "%s", s);
    return (uint16_t)(r.pc - pc);
}
