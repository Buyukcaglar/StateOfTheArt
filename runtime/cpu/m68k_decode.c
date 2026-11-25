/* 68000 instruction decoder with static cycle costs (Motorola 68000 UM tables). */
#include "m68k.h"
#include <string.h>

typedef struct { uint32_t pc0, pos; bool bad; Insn *i; } Dec;

static uint16_t next16(Dec *d)
{
    uint16_t w = (uint16_t)mem_fetch16(d->pos);
    int k = (int)(d->pos - d->pc0) - 2;
    if (k >= 0 && k + 1 < (int)sizeof d->i->ext) { d->i->ext[k] = w >> 8; d->i->ext[k + 1] = w & 0xff; }
    d->pos += 2;
    return w;
}

/* EA categories */
#define C_DATA    1   /* all but An */
#define C_MEM     2   /* all but Dn, An */
#define C_CTRL    4   /* (An), d16, idx, abs, pc modes */
#define C_ALT     8   /* no pc-relative, no imm */
#define C_ANY     16

static bool decode_ea(Dec *d, int mode, int reg, int size, EA *ea, int cat)
{
    memset(ea, 0, sizeof *ea);
    ea->reg = (uint8_t)reg;
    switch (mode) {
    case 0: ea->mode = EA_DREG; break;
    case 1: ea->mode = EA_AREG; if (size == 1 && !(cat & C_ANY)) {} break;
    case 2: ea->mode = EA_AIND; break;
    case 3: ea->mode = EA_POSTINC; break;
    case 4: ea->mode = EA_PREDEC; break;
    case 5: ea->mode = EA_ADISP; ea->disp = (int16_t)next16(d); break;
    case 6: {
        uint16_t e = next16(d);
        ea->mode = EA_AIDX; ea->xreg = (e >> 12) & 15; ea->xlong = (e >> 11) & 1;
        ea->disp = (int8_t)(e & 0xff);
        break;
    }
    case 7:
        switch (reg) {
        case 0: ea->mode = EA_ABSW; ea->val = (uint32_t)(int32_t)(int16_t)next16(d); break;
        case 1: { uint32_t h = next16(d); ea->mode = EA_ABSL; ea->val = (h << 16) | next16(d); break; }
        case 2: { uint32_t base = d->pos; ea->mode = EA_PCDISP; ea->val = base + (int16_t)next16(d); break; }
        case 3: {
            uint32_t base = d->pos; uint16_t e = next16(d);
            ea->mode = EA_PCIDX; ea->xreg = (e >> 12) & 15; ea->xlong = (e >> 11) & 1;
            ea->val = base + (int8_t)(e & 0xff);
            break;
        }
        case 4:
            ea->mode = EA_IMM;
            if (size == 4) { uint32_t h = next16(d); ea->val = (h << 16) | next16(d); }
            else if (size == 1) ea->val = next16(d) & 0xff;
            else ea->val = next16(d);
            break;
        default: return false;
        }
        break;
    }
    /* category checks */
    int m = ea->mode;
    if ((cat & C_DATA) && m == EA_AREG) return false;
    if ((cat & C_MEM) && (m == EA_DREG || m == EA_AREG)) return false;
    if ((cat & C_CTRL) && (m == EA_DREG || m == EA_AREG || m == EA_POSTINC || m == EA_PREDEC || m == EA_IMM)) return false;
    if ((cat & C_ALT) && (m == EA_PCDISP || m == EA_PCIDX || m == EA_IMM)) return false;
    return true;
}

/* effective address calculation time */
static int eat(const EA *e, int size)
{
    bool l = size == 4;
    switch (e->mode) {
    case EA_AIND: case EA_POSTINC: return l ? 8 : 4;
    case EA_PREDEC: return l ? 10 : 6;
    case EA_ADISP: case EA_PCDISP: case EA_ABSW: return l ? 12 : 8;
    case EA_AIDX: case EA_PCIDX: return l ? 14 : 10;
    case EA_ABSL: return l ? 16 : 12;
    case EA_IMM: return l ? 8 : 4;
    default: return 0;
    }
}
/* move destination time */
static int move_dst(const EA *e, int size)
{
    bool l = size == 4;
    switch (e->mode) {
    case EA_AIND: case EA_POSTINC: case EA_PREDEC: return l ? 8 : 4;
    case EA_ADISP: case EA_ABSW: return l ? 12 : 8;
    case EA_AIDX: return l ? 14 : 10;
    case EA_ABSL: return l ? 16 : 12;
    default: return 0;
    }
}
/* control-mode times for LEA/PEA/JMP/JSR */
static int ctrl_t(const EA *e, int an, int d16, int idx, int aw, int al)
{
    switch (e->mode) {
    case EA_AIND: return an;
    case EA_ADISP: case EA_PCDISP: return d16;
    case EA_AIDX: case EA_PCIDX: return idx;
    case EA_ABSW: return aw;
    case EA_ABSL: return al;
    default: return 0;
    }
}

static const int size_bits[4] = { 1, 2, 4, 0 };

#define BAD() do { d->bad = true; return; } while (0)
#define EA_OR_BAD(mode, reg, size, ea, cat) do { if (!decode_ea(d, mode, reg, size, ea, cat)) BAD(); } while (0)

static void dec0(Dec *d, Insn *i, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    if (op & 0x100) {
        if (mode == 1) { /* MOVEP */
            int sz = (op & 0x40) ? 4 : 2;
            i->size = sz;
            i->src.mode = EA_DREG; i->src.reg = (op >> 9) & 7;
            i->dst.mode = EA_ADISP; i->dst.reg = reg; i->dst.disp = (int16_t)next16(d);
            i->kind = (op & 0x80) ? OP_MOVEP_RM : OP_MOVEP_MR;
            i->cycles = sz == 4 ? 24 : 16;
            return;
        }
        /* dynamic bit op */
        static const int k[4] = { OP_BTST, OP_BCHG, OP_BCLR, OP_BSET };
        i->kind = k[(op >> 6) & 3];
        i->src.mode = EA_DREG; i->src.reg = (op >> 9) & 7;
        int cat = i->kind == OP_BTST ? C_DATA : (C_DATA | C_ALT);
        if (i->kind == OP_BTST && mode == 7 && reg == 4) { /* btst dn,#imm */
            EA_OR_BAD(mode, reg, 1, &i->dst, C_DATA);
        } else EA_OR_BAD(mode, reg, 1, &i->dst, cat);
        i->size = i->dst.mode == EA_DREG ? 4 : 1;
        if (i->dst.mode == EA_DREG)
            i->cycles = i->kind == OP_BTST ? 6 : i->kind == OP_BCLR ? 10 : 8;
        else i->cycles = (i->kind == OP_BTST ? 4 : 8) + eat(&i->dst, 1);
        return;
    }
    int top = (op >> 9) & 7;
    int szf = (op >> 6) & 3;
    if (top == 4) { /* static bit op */
        static const int k[4] = { OP_BTST, OP_BCHG, OP_BCLR, OP_BSET };
        i->kind = k[szf];
        i->src.mode = EA_IMM; i->src.val = next16(d) & 0xff;
        int cat = i->kind == OP_BTST ? C_DATA : (C_DATA | C_ALT);
        if (i->kind == OP_BTST && mode == 7 && reg == 4) BAD();
        EA_OR_BAD(mode, reg, 1, &i->dst, cat);
        i->size = i->dst.mode == EA_DREG ? 4 : 1;
        if (i->dst.mode == EA_DREG)
            i->cycles = i->kind == OP_BTST ? 10 : i->kind == OP_BCLR ? 14 : 12;
        else i->cycles = (i->kind == OP_BTST ? 8 : 12) + eat(&i->dst, 1);
        return;
    }
    static const int imm_ops[8] = { OP_ORI, OP_ANDI, OP_SUBI, OP_ADDI, -1, OP_EORI, OP_CMPI, -1 };
    if (imm_ops[top] < 0 || szf == 3) BAD();
    /* to CCR / SR */
    if (mode == 7 && reg == 4 && (top == 0 || top == 1 || top == 5)) {
        uint16_t v = next16(d);
        if (szf == 0) { i->kind = top == 0 ? OP_ORI_CCR : top == 1 ? OP_ANDI_CCR : OP_EORI_CCR; i->imm = v & 0xff; }
        else if (szf == 1) { i->kind = top == 0 ? OP_ORI_SR : top == 1 ? OP_ANDI_SR : OP_EORI_SR; i->imm = v; }
        else BAD();
        i->cycles = 20;
        return;
    }
    int sz = size_bits[szf];
    i->kind = imm_ops[top];
    i->size = sz;
    i->src.mode = EA_IMM;
    if (sz == 4) { uint32_t h = next16(d); i->src.val = (h << 16) | next16(d); }
    else i->src.val = next16(d) & (sz == 1 ? 0xff : 0xffff);
    EA_OR_BAD(mode, reg, sz, &i->dst, i->kind == OP_CMPI ? (C_DATA | C_ALT) : (C_DATA | C_ALT));
    if (i->dst.mode == EA_DREG) i->cycles = i->kind == OP_CMPI ? (sz == 4 ? 14 : 8) : (sz == 4 ? 16 : 8);
    else i->cycles = (i->kind == OP_CMPI ? (sz == 4 ? 12 : 8) : (sz == 4 ? 20 : 12)) + eat(&i->dst, sz);
}

static void dec_move(Dec *d, Insn *i, uint16_t op)
{
    int t = op >> 12;
    int sz = t == 1 ? 1 : t == 3 ? 2 : 4;
    i->size = sz;
    EA_OR_BAD((op >> 3) & 7, op & 7, sz, &i->src, C_ANY);
    if (sz == 1 && i->src.mode == EA_AREG) BAD();
    int dm = (op >> 6) & 7, dr = (op >> 9) & 7;
    if (dm == 1) {
        if (sz == 1) BAD();
        i->kind = OP_MOVEA; i->dst.mode = EA_AREG; i->dst.reg = dr;
        i->cycles = 4 + eat(&i->src, sz);
        return;
    }
    i->kind = OP_MOVE;
    EA_OR_BAD(dm, dr, sz, &i->dst, C_DATA | C_ALT);
    i->cycles = 4 + eat(&i->src, sz) + move_dst(&i->dst, sz);
}

static void dec4(Dec *d, Insn *i, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    int szf = (op >> 6) & 3;
    if (op & 0x100) {
        if ((op & 0x1c0) == 0x1c0) { /* LEA */
            i->kind = OP_LEA; i->size = 4;
            EA_OR_BAD(mode, reg, 4, &i->src, C_CTRL);
            i->dst.mode = EA_AREG; i->dst.reg = (op >> 9) & 7;
            i->cycles = ctrl_t(&i->src, 4, 8, 12, 8, 12);
            return;
        }
        if ((op & 0x1c0) == 0x180) { /* CHK.w */
            i->kind = OP_CHK; i->size = 2;
            EA_OR_BAD(mode, reg, 2, &i->src, C_DATA);
            i->dst.mode = EA_DREG; i->dst.reg = (op >> 9) & 7;
            i->cycles = 10 + eat(&i->src, 2);
            i->flow = FLOW_TRAP;
            return;
        }
        BAD();
    }
    switch ((op >> 8) & 0xf) {
    case 0x0: /* NEGX / MOVE from SR */
    case 0x2: /* CLR */
    case 0x4: /* NEG / MOVE to CCR */
    case 0x6: /* NOT / MOVE to SR */
    {
        int g = (op >> 8) & 0xf;
        if (szf == 3) {
            if (g == 0) { i->kind = OP_MOVE_FROM_SR; i->size = 2; EA_OR_BAD(mode, reg, 2, &i->dst, C_DATA | C_ALT);
                i->cycles = i->dst.mode == EA_DREG ? 6 : 8 + eat(&i->dst, 2); return; }
            if (g == 4) { i->kind = OP_MOVE_TO_CCR; i->size = 2; EA_OR_BAD(mode, reg, 2, &i->src, C_DATA);
                i->cycles = 12 + eat(&i->src, 2); return; }
            if (g == 6) { i->kind = OP_MOVE_TO_SR; i->size = 2; EA_OR_BAD(mode, reg, 2, &i->src, C_DATA);
                i->cycles = 12 + eat(&i->src, 2); return; }
            BAD();
        }
        int sz = size_bits[szf];
        i->size = sz;
        i->kind = g == 0 ? OP_NEGX : g == 2 ? OP_CLR : g == 4 ? OP_NEG : OP_NOT;
        EA_OR_BAD(mode, reg, sz, &i->dst, C_DATA | C_ALT);
        if (i->dst.mode == EA_DREG) i->cycles = sz == 4 ? 6 : 4;
        else i->cycles = (sz == 4 ? 12 : 8) + eat(&i->dst, sz);
        return;
    }
    case 0x8:
        if (szf == 0) { i->kind = OP_NBCD; i->size = 1; EA_OR_BAD(mode, reg, 1, &i->dst, C_DATA | C_ALT);
            i->cycles = i->dst.mode == EA_DREG ? 6 : 8 + eat(&i->dst, 1); return; }
        if (szf == 1) {
            if (mode == 0) { i->kind = OP_SWAP; i->size = 4; i->dst.mode = EA_DREG; i->dst.reg = reg; i->cycles = 4; return; }
            i->kind = OP_PEA; i->size = 4; EA_OR_BAD(mode, reg, 4, &i->src, C_CTRL);
            i->cycles = ctrl_t(&i->src, 12, 16, 20, 16, 20); return;
        }
        if (mode == 0) { i->kind = OP_EXT; i->size = szf == 2 ? 2 : 4; i->dst.mode = EA_DREG; i->dst.reg = reg; i->cycles = 4; return; }
        /* MOVEM reg->mem */
        i->kind = OP_MOVEM_RM; i->size = szf == 2 ? 2 : 4;
        i->imm = next16(d);
        if (mode == 4) { i->dst.mode = EA_PREDEC; i->dst.reg = reg; }
        else EA_OR_BAD(mode, reg, i->size, &i->dst, C_CTRL | C_ALT);
        i->cycles = ctrl_t(&i->dst, 8, 12, 14, 12, 16);
        if (i->dst.mode == EA_PREDEC) i->cycles = 8;
        return;
    case 0xa:
        if (szf == 3) {
            if (op == 0x4afc) { i->kind = OP_ILLEGAL; i->flow = FLOW_TRAP; i->cycles = 34; return; }
            i->kind = OP_TAS; i->size = 1; EA_OR_BAD(mode, reg, 1, &i->dst, C_DATA | C_ALT);
            i->cycles = i->dst.mode == EA_DREG ? 4 : 14 + eat(&i->dst, 1); return;
        }
        i->kind = OP_TST; i->size = size_bits[szf];
        EA_OR_BAD(mode, reg, i->size, &i->dst, C_DATA | C_ALT);
        i->cycles = 4 + eat(&i->dst, i->size);
        return;
    case 0xc:
        if (szf == 2 || szf == 3) {
            i->kind = OP_MOVEM_MR; i->size = szf == 2 ? 2 : 4;
            i->imm = next16(d);
            if (mode == 3) { i->src.mode = EA_POSTINC; i->src.reg = reg; }
            else EA_OR_BAD(mode, reg, i->size, &i->src, C_CTRL);
            i->cycles = ctrl_t(&i->src, 12, 16, 18, 16, 20);
            if (i->src.mode == EA_POSTINC) i->cycles = 12;
            return;
        }
        BAD();
    case 0xe:
        if (szf == 1) {
            int lo = op & 0x3f;
            if (lo < 0x10) { i->kind = OP_TRAP; i->imm = 32 + (lo & 15); i->flow = FLOW_TRAP; i->cycles = 34; return; }
            if (lo < 0x18) { i->kind = OP_LINK; i->dst.mode = EA_AREG; i->dst.reg = reg; i->imm = (uint32_t)(int32_t)(int16_t)next16(d); i->cycles = 16; return; }
            if (lo < 0x20) { i->kind = OP_UNLK; i->dst.mode = EA_AREG; i->dst.reg = reg; i->cycles = 12; return; }
            if (lo < 0x28) { i->kind = OP_MOVE_USP_TO; i->src.mode = EA_AREG; i->src.reg = reg; i->cycles = 4; return; }
            if (lo < 0x30) { i->kind = OP_MOVE_USP_FROM; i->dst.mode = EA_AREG; i->dst.reg = reg; i->cycles = 4; return; }
            switch (op) {
            case 0x4e70: i->kind = OP_RESET; i->cycles = 132; return;
            case 0x4e71: i->kind = OP_NOP; i->cycles = 4; return;
            case 0x4e72: i->kind = OP_STOP; i->imm = next16(d); i->flow = FLOW_STOP; i->cycles = 4; return;
            case 0x4e73: i->kind = OP_RTE; i->flow = FLOW_RET; i->cycles = 20; return;
            case 0x4e75: i->kind = OP_RTS; i->flow = FLOW_RET; i->cycles = 16; return;
            case 0x4e76: i->kind = OP_TRAPV; i->flow = FLOW_TRAP; i->cycles = 4; return;
            case 0x4e77: i->kind = OP_RTR; i->flow = FLOW_RET; i->cycles = 20; return;
            }
            BAD();
        }
        if (szf == 2) {
            i->kind = OP_JSR; EA_OR_BAD(mode, reg, 4, &i->src, C_CTRL);
            i->cycles = ctrl_t(&i->src, 16, 18, 22, 18, 20);
            if (i->src.mode == EA_ABSW || i->src.mode == EA_ABSL || i->src.mode == EA_PCDISP) { i->flow = FLOW_CALL; i->target = i->src.mode == EA_PCDISP ? i->src.val : i->src.val; }
            else i->flow = FLOW_CALL_IND;
            return;
        }
        if (szf == 3) {
            i->kind = OP_JMP; EA_OR_BAD(mode, reg, 4, &i->src, C_CTRL);
            i->cycles = ctrl_t(&i->src, 8, 10, 14, 10, 12);
            if (i->src.mode == EA_ABSW || i->src.mode == EA_ABSL || i->src.mode == EA_PCDISP) { i->flow = FLOW_BRANCH; i->target = i->src.val; }
            else i->flow = FLOW_JUMP_IND;
            return;
        }
        BAD();
    default: BAD();
    }
}

static void dec5(Dec *d, Insn *i, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7, szf = (op >> 6) & 3;
    if (szf == 3) {
        i->cond = (op >> 8) & 15;
        if (mode == 1) {
            i->kind = OP_DBCC; i->dst.mode = EA_DREG; i->dst.reg = reg;
            uint32_t base = d->pos; i->target = base + (int16_t)next16(d);
            i->flow = FLOW_COND; i->cycles = 10;
            return;
        }
        i->kind = OP_SCC; i->size = 1;
        EA_OR_BAD(mode, reg, 1, &i->dst, C_DATA | C_ALT);
        i->cycles = i->dst.mode == EA_DREG ? 4 : 8 + eat(&i->dst, 1);
        return;
    }
    int sz = size_bits[szf];
    i->kind = (op & 0x100) ? OP_SUBQ : OP_ADDQ;
    i->size = sz;
    i->src.mode = EA_IMM; i->src.val = ((op >> 9) & 7) ? ((op >> 9) & 7) : 8;
    EA_OR_BAD(mode, reg, sz, &i->dst, C_ALT);
    if (i->dst.mode == EA_AREG) { if (sz == 1) BAD(); i->cycles = 8; }
    else if (i->dst.mode == EA_DREG) i->cycles = sz == 4 ? 8 : 4;
    else i->cycles = (sz == 4 ? 12 : 8) + eat(&i->dst, sz);
}

static void dec6(Dec *d, Insn *i, uint16_t op)
{
    int c = (op >> 8) & 15;
    uint32_t base = d->pos;
    int32_t disp = (int8_t)(op & 0xff);
    bool word = disp == 0;
    if (word) disp = (int16_t)next16(d);
    i->target = base + disp;
    i->size = word ? 2 : 1;
    if (c == 0) { i->kind = OP_BRA; i->flow = FLOW_BRANCH; i->cycles = 10; }
    else if (c == 1) { i->kind = OP_BSR; i->flow = FLOW_CALL; i->cycles = 18; }
    else { i->kind = OP_BCC; i->cond = c; i->flow = FLOW_COND; i->cycles = 10; }
}

/* OR / AND / SUB / ADD / CMP / EOR style */
static void dec_arith(Dec *d, Insn *i, uint16_t op, int kind_ea_dn, int kind_dn_ea, bool allow_an_src)
{
    int mode = (op >> 3) & 7, reg = op & 7, szf = (op >> 6) & 3, dn = (op >> 9) & 7;
    int sz = size_bits[szf];
    i->size = sz;
    if (!(op & 0x100)) { /* <ea>,Dn */
        i->kind = kind_ea_dn;
        EA_OR_BAD(mode, reg, sz, &i->src, allow_an_src ? C_ANY : C_DATA);
        if (i->src.mode == EA_AREG && sz == 1) BAD();
        i->dst.mode = EA_DREG; i->dst.reg = dn;
        if (sz == 4) {
            bool rd = i->src.mode == EA_DREG || i->src.mode == EA_AREG || i->src.mode == EA_IMM;
            i->cycles = (kind_ea_dn == OP_CMP) ? 6 + eat(&i->src, 4) : (rd ? 8 : 6) + eat(&i->src, 4);
        } else i->cycles = 4 + eat(&i->src, sz);
    } else { /* Dn,<ea> */
        i->kind = kind_dn_ea;
        i->src.mode = EA_DREG; i->src.reg = dn;
        if (kind_dn_ea == OP_EOR) {
            EA_OR_BAD(mode, reg, sz, &i->dst, C_DATA | C_ALT);
            if (i->dst.mode == EA_DREG) { i->cycles = sz == 4 ? 8 : 4; return; }
        } else EA_OR_BAD(mode, reg, sz, &i->dst, C_MEM | C_ALT);
        i->cycles = (sz == 4 ? 12 : 8) + eat(&i->dst, sz);
    }
}

static void dec_x(Dec *d, Insn *i, uint16_t op, int kind)
{
    (void)d;
    int szf = (op >> 6) & 3;
    i->kind = kind; i->size = kind == OP_ABCD || kind == OP_SBCD ? 1 : size_bits[szf];
    if (op & 8) { i->src.mode = EA_PREDEC; i->dst.mode = EA_PREDEC; }
    else { i->src.mode = EA_DREG; i->dst.mode = EA_DREG; }
    i->src.reg = op & 7; i->dst.reg = (op >> 9) & 7;
    bool mem = op & 8;
    if (kind == OP_ABCD || kind == OP_SBCD) i->cycles = mem ? 18 : 6;
    else i->cycles = mem ? (i->size == 4 ? 30 : 18) : (i->size == 4 ? 8 : 4);
}

static void dec8(Dec *d, Insn *i, uint16_t op)
{
    int szf = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
    if (szf == 3) {
        i->kind = (op & 0x100) ? OP_DIVS : OP_DIVU; i->size = 2;
        EA_OR_BAD(mode, reg, 2, &i->src, C_DATA);
        i->dst.mode = EA_DREG; i->dst.reg = (op >> 9) & 7;
        i->cycles = (i->kind == OP_DIVS ? 158 : 140) + eat(&i->src, 2);
        return;
    }
    if ((op & 0x1f0) == 0x100) { dec_x(d, i, op, OP_SBCD); return; }
    dec_arith(d, i, op, OP_OR, OP_OR, false);
}

static void dec9d(Dec *d, Insn *i, uint16_t op, bool add)
{
    int szf = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
    if (szf == 3) {
        i->kind = add ? OP_ADDA : OP_SUBA;
        i->size = (op & 0x100) ? 4 : 2;
        EA_OR_BAD(mode, reg, i->size, &i->src, C_ANY);
        i->dst.mode = EA_AREG; i->dst.reg = (op >> 9) & 7;
        if (i->size == 4) {
            bool rd = i->src.mode == EA_DREG || i->src.mode == EA_AREG || i->src.mode == EA_IMM;
            i->cycles = (rd ? 8 : 6) + eat(&i->src, 4);
        } else i->cycles = 8 + eat(&i->src, 2);
        return;
    }
    if ((op & 0x130) == 0x100) { dec_x(d, i, op, add ? OP_ADDX : OP_SUBX); return; }
    dec_arith(d, i, op, add ? OP_ADD : OP_SUB, add ? OP_ADD : OP_SUB, true);
}

static void decB(Dec *d, Insn *i, uint16_t op)
{
    int szf = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
    if (szf == 3) {
        i->kind = OP_CMPA; i->size = (op & 0x100) ? 4 : 2;
        EA_OR_BAD(mode, reg, i->size, &i->src, C_ANY);
        i->dst.mode = EA_AREG; i->dst.reg = (op >> 9) & 7;
        i->cycles = 6 + eat(&i->src, i->size);
        return;
    }
    if (op & 0x100) {
        if (mode == 1) { /* CMPM */
            i->kind = OP_CMPM; i->size = size_bits[szf];
            i->src.mode = EA_POSTINC; i->src.reg = reg;
            i->dst.mode = EA_POSTINC; i->dst.reg = (op >> 9) & 7;
            i->cycles = i->size == 4 ? 20 : 12;
            return;
        }
        dec_arith(d, i, op, OP_CMP, OP_EOR, true);
        return;
    }
    dec_arith(d, i, op, OP_CMP, OP_CMP, true);
}

static void decC(Dec *d, Insn *i, uint16_t op)
{
    int szf = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
    if (szf == 3) {
        i->kind = (op & 0x100) ? OP_MULS : OP_MULU; i->size = 2;
        EA_OR_BAD(mode, reg, 2, &i->src, C_DATA);
        i->dst.mode = EA_DREG; i->dst.reg = (op >> 9) & 7;
        i->cycles = 38 + eat(&i->src, 2);
        return;
    }
    if ((op & 0x1f0) == 0x100) { dec_x(d, i, op, OP_ABCD); return; }
    if ((op & 0x1f8) == 0x140 || (op & 0x1f8) == 0x148 || (op & 0x1f8) == 0x188) {
        i->kind = OP_EXG; i->size = 4; i->cycles = 6;
        int rx = (op >> 9) & 7, ry = op & 7;
        int m = (op >> 3) & 0x1f;
        if (m == 0x08) { i->src.mode = EA_DREG; i->dst.mode = EA_DREG; }
        else if (m == 0x09) { i->src.mode = EA_AREG; i->dst.mode = EA_AREG; }
        else { i->src.mode = EA_DREG; i->dst.mode = EA_AREG; }
        i->src.reg = rx; i->dst.reg = ry;
        return;
    }
    dec_arith(d, i, op, OP_AND, OP_AND, false);
}

static void decE(Dec *d, Insn *i, uint16_t op)
{
    int szf = (op >> 6) & 3;
    static const int kinds[4][2] = { { OP_ASR, OP_ASL }, { OP_LSR, OP_LSL }, { OP_ROXR, OP_ROXL }, { OP_ROR, OP_ROL } };
    bool left = op & 0x100;
    if (szf == 3) {
        int t = (op >> 9) & 7;
        if (t > 3) BAD();
        i->kind = kinds[t][left]; i->size = 2; i->regmem = 2;
        EA_OR_BAD((op >> 3) & 7, op & 7, 2, &i->dst, C_MEM | C_ALT);
        i->cycles = 8 + eat(&i->dst, 2);
        return;
    }
    int t = (op >> 3) & 3;
    i->kind = kinds[t][left]; i->size = size_bits[szf];
    i->dst.mode = EA_DREG; i->dst.reg = op & 7;
    int cnt = (op >> 9) & 7;
    if (op & 0x20) { i->regmem = 1; i->src.mode = EA_DREG; i->src.reg = cnt; }
    else { i->regmem = 0; i->imm = cnt ? cnt : 8; }
    i->cycles = i->size == 4 ? 8 : 6;
    if (!i->regmem) i->cycles += 2 * i->imm;
}

int m68k_decode(uint32_t pc, Insn *i)
{
    memset(i, 0, sizeof *i);
    Dec d = { pc, pc, false, i };
    i->pc = pc;
    uint16_t op = next16(&d);
    i->op = op;
    switch (op >> 12) {
    case 0x0: dec0(&d, i, op); break;
    case 0x1: case 0x2: case 0x3: dec_move(&d, i, op); break;
    case 0x4: dec4(&d, i, op); break;
    case 0x5: dec5(&d, i, op); break;
    case 0x6: dec6(&d, i, op); break;
    case 0x7:
        if (op & 0x100) d.bad = true;
        else { i->kind = OP_MOVEQ; i->size = 4; i->imm = (uint32_t)(int32_t)(int8_t)(op & 0xff);
               i->dst.mode = EA_DREG; i->dst.reg = (op >> 9) & 7; i->cycles = 4; }
        break;
    case 0x8: dec8(&d, i, op); break;
    case 0x9: dec9d(&d, i, op, false); break;
    case 0xa: i->kind = OP_LINEA; i->flow = FLOW_TRAP; i->cycles = 34; break;
    case 0xb: decB(&d, i, op); break;
    case 0xc: decC(&d, i, op); break;
    case 0xd: dec9d(&d, i, op, true); break;
    case 0xe: decE(&d, i, op); break;
    case 0xf: i->kind = OP_LINEF; i->flow = FLOW_TRAP; i->cycles = 34; break;
    }
    if (d.bad) {
        memset(i, 0, sizeof *i);
        i->pc = pc; i->op = op; i->kind = OP_ILLEGAL; i->flow = FLOW_TRAP; i->cycles = 34;
        d.pos = pc + 2;
    }
    i->len = (uint8_t)(d.pos - pc);
    return i->len;
}

/* Block boundary rule shared by the interpreter, coverage and the lifter. */
bool m68k_ends_block(const Insn *i)
{
    if (i->flow != FLOW_NONE) return true;
    switch (i->kind) {
    case OP_MOVE_TO_SR: case OP_ORI_SR: case OP_ANDI_SR: case OP_EORI_SR:
    case OP_RESET: case OP_ILLEGAL: case OP_LINEA: case OP_LINEF:
    case OP_DIVU: case OP_DIVS:
        return true;
    default:
        return false;
    }
}
