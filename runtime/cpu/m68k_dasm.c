#include "m68k.h"
#include <stdio.h>
#include <string.h>

static const char *names[OP_COUNT] = {
    [OP_ILLEGAL] = "illegal", [OP_LINEA] = "linea", [OP_LINEF] = "linef",
    [OP_ORI] = "ori", [OP_ANDI] = "andi", [OP_SUBI] = "subi", [OP_ADDI] = "addi", [OP_EORI] = "eori", [OP_CMPI] = "cmpi",
    [OP_ORI_CCR] = "ori", [OP_ORI_SR] = "ori", [OP_ANDI_CCR] = "andi", [OP_ANDI_SR] = "andi", [OP_EORI_CCR] = "eori", [OP_EORI_SR] = "eori",
    [OP_BTST] = "btst", [OP_BCHG] = "bchg", [OP_BCLR] = "bclr", [OP_BSET] = "bset", [OP_MOVEP_RM] = "movep", [OP_MOVEP_MR] = "movep",
    [OP_MOVE] = "move", [OP_MOVEA] = "movea",
    [OP_NEGX] = "negx", [OP_CLR] = "clr", [OP_NEG] = "neg", [OP_NOT] = "not", [OP_TST] = "tst", [OP_TAS] = "tas", [OP_NBCD] = "nbcd",
    [OP_MOVE_FROM_SR] = "move", [OP_MOVE_TO_CCR] = "move", [OP_MOVE_TO_SR] = "move",
    [OP_EXT] = "ext", [OP_SWAP] = "swap", [OP_PEA] = "pea", [OP_LEA] = "lea", [OP_MOVEM_RM] = "movem", [OP_MOVEM_MR] = "movem",
    [OP_TRAP] = "trap", [OP_LINK] = "link", [OP_UNLK] = "unlk", [OP_MOVE_USP_TO] = "move", [OP_MOVE_USP_FROM] = "move",
    [OP_RESET] = "reset", [OP_NOP] = "nop", [OP_STOP] = "stop", [OP_RTE] = "rte", [OP_RTS] = "rts", [OP_TRAPV] = "trapv", [OP_RTR] = "rtr",
    [OP_JSR] = "jsr", [OP_JMP] = "jmp", [OP_CHK] = "chk",
    [OP_ADDQ] = "addq", [OP_SUBQ] = "subq", [OP_SCC] = "s", [OP_DBCC] = "db",
    [OP_BRA] = "bra", [OP_BSR] = "bsr", [OP_BCC] = "b",
    [OP_MOVEQ] = "moveq",
    [OP_OR] = "or", [OP_DIVU] = "divu", [OP_DIVS] = "divs", [OP_SBCD] = "sbcd",
    [OP_SUB] = "sub", [OP_SUBA] = "suba", [OP_SUBX] = "subx",
    [OP_CMP] = "cmp", [OP_CMPA] = "cmpa", [OP_CMPM] = "cmpm", [OP_EOR] = "eor",
    [OP_AND] = "and", [OP_MULU] = "mulu", [OP_MULS] = "muls", [OP_ABCD] = "abcd", [OP_EXG] = "exg",
    [OP_ADD] = "add", [OP_ADDA] = "adda", [OP_ADDX] = "addx",
    [OP_ASL] = "asl", [OP_ASR] = "asr", [OP_LSL] = "lsl", [OP_LSR] = "lsr",
    [OP_ROXL] = "roxl", [OP_ROXR] = "roxr", [OP_ROL] = "rol", [OP_ROR] = "ror",
};
static const char *cc[16] = { "t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le" };

const char *m68k_opname(int kind) { return kind >= 0 && kind < OP_COUNT && names[kind] ? names[kind] : "?"; }

static void ea_str(const EA *e, char *b, int n)
{
    char x[8] = "";
    if (e->mode == EA_AIDX || e->mode == EA_PCIDX)
        snprintf(x, sizeof x, "%c%d.%c", e->xreg < 8 ? 'd' : 'a', e->xreg & 7, e->xlong ? 'l' : 'w');
    switch (e->mode) {
    case EA_DREG: snprintf(b, n, "d%d", e->reg); break;
    case EA_AREG: snprintf(b, n, e->reg == 7 ? "sp" : "a%d", e->reg); break;
    case EA_AIND: snprintf(b, n, "(a%d)", e->reg); break;
    case EA_POSTINC: snprintf(b, n, "(a%d)+", e->reg); break;
    case EA_PREDEC: snprintf(b, n, "-(a%d)", e->reg); break;
    case EA_ADISP: snprintf(b, n, "%d(a%d)", e->disp, e->reg); break;
    case EA_AIDX: snprintf(b, n, "%d(a%d,%s)", e->disp, e->reg, x); break;
    case EA_ABSW: snprintf(b, n, "$%x.w", e->val); break;
    case EA_ABSL: snprintf(b, n, "$%x", e->val); break;
    case EA_PCDISP: snprintf(b, n, "$%x(pc)", e->val); break;
    case EA_PCIDX: snprintf(b, n, "$%x(pc,%s)", e->val, x); break;
    case EA_IMM: snprintf(b, n, "#$%x", e->val); break;
    case EA_SR: snprintf(b, n, "sr"); break;
    case EA_CCR: snprintf(b, n, "ccr"); break;
    case EA_USP: snprintf(b, n, "usp"); break;
    default: b[0] = 0; break;
    }
}

static void movem_list(uint32_t mask, bool predec, char *b, int n)
{
    int p = 0; b[0] = 0;
    for (int r = 0; r < 16; r++) {
        int bit = predec ? 15 - r : r;
        if (mask & (1u << bit)) p += snprintf(b + p, n - p, "%s%c%d", p ? "/" : "", r < 8 ? 'd' : 'a', r & 7);
        if (p >= n) break;
    }
}

void m68k_disasm(const Insn *i, char *buf, int n)
{
    char s[48], d[48], sfx[4] = "";
    ea_str(&i->src, s, sizeof s);
    ea_str(&i->dst, d, sizeof d);
    if (i->size == 1) strcpy(sfx, ".b"); else if (i->size == 2) strcpy(sfx, ".w"); else if (i->size == 4) strcpy(sfx, ".l");
    const char *nm = m68k_opname(i->kind);
    switch (i->kind) {
    case OP_BCC: snprintf(buf, n, "b%s $%x", cc[i->cond], i->target); break;
    case OP_BRA: case OP_BSR: snprintf(buf, n, "%s $%x", nm, i->target); break;
    case OP_DBCC: snprintf(buf, n, "db%s %s,$%x", cc[i->cond], d, i->target); break;
    case OP_SCC: snprintf(buf, n, "s%s %s", cc[i->cond], d); break;
    case OP_MOVEQ: snprintf(buf, n, "moveq #%d,%s", (int32_t)i->imm, d); break;
    case OP_ADDQ: case OP_SUBQ: snprintf(buf, n, "%s%s #%u,%s", nm, sfx, i->src.val, d); break;
    case OP_ORI_CCR: case OP_ANDI_CCR: case OP_EORI_CCR: snprintf(buf, n, "%s #$%x,ccr", nm, i->imm); break;
    case OP_ORI_SR: case OP_ANDI_SR: case OP_EORI_SR: snprintf(buf, n, "%s #$%x,sr", nm, i->imm); break;
    case OP_MOVE_FROM_SR: snprintf(buf, n, "move sr,%s", d); break;
    case OP_MOVE_TO_CCR: snprintf(buf, n, "move %s,ccr", s); break;
    case OP_MOVE_TO_SR: snprintf(buf, n, "move %s,sr", s); break;
    case OP_MOVE_USP_TO: snprintf(buf, n, "move %s,usp", s); break;
    case OP_MOVE_USP_FROM: snprintf(buf, n, "move usp,%s", d); break;
    case OP_MOVEM_RM: { char l[64]; movem_list(i->imm, i->dst.mode == EA_PREDEC, l, sizeof l); snprintf(buf, n, "movem%s %s,%s", sfx, l, d); break; }
    case OP_MOVEM_MR: { char l[64]; movem_list(i->imm, false, l, sizeof l); snprintf(buf, n, "movem%s %s,%s", sfx, s, l); break; }
    case OP_TRAP: snprintf(buf, n, "trap #%u", i->imm - 32); break;
    case OP_LINK: snprintf(buf, n, "link %s,#%d", d, (int32_t)i->imm); break;
    case OP_STOP: snprintf(buf, n, "stop #$%x", i->imm); break;
    case OP_MOVEP_MR: snprintf(buf, n, "movep%s %s,%s", sfx, d, s); break;
    case OP_ASL: case OP_ASR: case OP_LSL: case OP_LSR: case OP_ROXL: case OP_ROXR: case OP_ROL: case OP_ROR:
        if (i->regmem == 2) snprintf(buf, n, "%s.w %s", nm, d);
        else if (i->regmem == 1) snprintf(buf, n, "%s%s %s,%s", nm, sfx, s, d);
        else snprintf(buf, n, "%s%s #%u,%s", nm, sfx, i->imm, d);
        break;
    case OP_ILLEGAL: case OP_LINEA: case OP_LINEF: snprintf(buf, n, "dc.w $%04x ; %s", i->op, nm); break;
    default:
        if (s[0] && d[0]) snprintf(buf, n, "%s%s %s,%s", nm, sfx, s, d);
        else if (s[0] || d[0]) snprintf(buf, n, "%s%s %s", nm, sfx, s[0] ? s : d);
        else snprintf(buf, n, "%s", nm);
    }
}
