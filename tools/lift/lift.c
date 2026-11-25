/* Static recompiler: coverage (block entry PC + original code bytes) -> C.
 *
 *   lift <coverage.txt> <outdir> [blocks_per_file]
 *
 * Every block variant becomes a C function. Instructions are decoded with the
 * runtime's own decoder and emitted as specialised code over the shared
 * sem_* helpers, mirroring m68k_execute_insn() case by case, including the
 * order of memory accesses and the cycle accounting, so a recompiled block
 * behaves exactly like the interpreter on the same bytes. Rare, privileged or
 * exception-raising instructions call the interpreter's executor with a
 * constant Insn. */
#include "../../runtime/cpu/m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- decoder memory: just the block's bytes ---- */
static uint32_t blk_pc, blk_len;
static uint8_t blk_bytes[8192];
uint32_t mem_fetch16(uint32_t a)
{
    if (a >= blk_pc && a + 1 < blk_pc + blk_len) { uint32_t o = a - blk_pc; return (uint32_t)(blk_bytes[o] << 8 | blk_bytes[o + 1]); }
    return 0;
}
uint32_t mem_rb(uint32_t a) { (void)a; return 0; }
uint32_t mem_rw(uint32_t a) { (void)a; return 0; }
uint32_t mem_rl(uint32_t a) { (void)a; return 0; }
void mem_wb(uint32_t a, uint32_t v) { (void)a; (void)v; }
void mem_ww(uint32_t a, uint32_t v) { (void)a; (void)v; }
void mem_wl(uint32_t a, uint32_t v) { (void)a; (void)v; }
M68K cpu;

/* ---- output ---- */
static FILE *out;
static int tmpn;
static void E(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("    ", out);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    va_end(ap);
}
static unsigned long stat_generic, stat_special;

static const char *RD(int sz) { return sz == 1 ? "mem_rb" : sz == 2 ? "mem_rw" : "mem_rl"; }
static const char *WR(int sz) { return sz == 1 ? "mem_wb" : sz == 2 ? "mem_ww" : "mem_wl"; }
static uint32_t MASK(int sz) { return sz == 1 ? 0xff : sz == 2 ? 0xffff : 0xffffffffu; }

static bool is_mem(const EA *e)
{
    return e->mode != EA_DREG && e->mode != EA_AREG && e->mode != EA_IMM && e->mode != EA_NONE;
}

/* emits the address computation (with side effects) and returns a variable name */
static void ea_addr(const EA *e, int sz, char *name)
{
    sprintf(name, "ea%d", tmpn++);
    int step = (sz == 1 && e->reg == 7) ? 2 : sz;
    char x[64] = "";
    if (e->mode == EA_AIDX || e->mode == EA_PCIDX) sprintf(x, "xval(%d, %d)", e->xreg, e->xlong);
    switch (e->mode) {
    case EA_AIND: E("uint32_t %s = cpu.a[%d];", name, e->reg); break;
    case EA_POSTINC: E("uint32_t %s = cpu.a[%d]; cpu.a[%d] += %d;", name, e->reg, e->reg, step); break;
    case EA_PREDEC: E("cpu.a[%d] -= %d; uint32_t %s = cpu.a[%d];", e->reg, step, name, e->reg); break;
    case EA_ADISP: E("uint32_t %s = cpu.a[%d] + (uint32_t)%d;", name, e->reg, e->disp); break;
    case EA_AIDX: E("uint32_t %s = cpu.a[%d] + (uint32_t)%d + %s;", name, e->reg, e->disp, x); break;
    case EA_ABSW: case EA_ABSL: case EA_PCDISP: E("uint32_t %s = 0x%xu;", name, e->val); break;
    case EA_PCIDX: E("uint32_t %s = 0x%xu + %s;", name, e->val, x); break;
    default: E("uint32_t %s = 0; /* bad ea */", name); break;
    }
}

/* emits a read of an operand into a new variable; returns its name */
static void ea_read(const EA *e, int sz, char *name)
{
    char a[16];
    switch (e->mode) {
    case EA_DREG: sprintf(name, "v%d", tmpn++); E("uint32_t %s = cpu.d[%d] & 0x%xu;", name, e->reg, MASK(sz)); return;
    case EA_AREG: sprintf(name, "v%d", tmpn++); E("uint32_t %s = cpu.a[%d] & 0x%xu;", name, e->reg, MASK(sz)); return;
    case EA_IMM: sprintf(name, "0x%xu", e->val & MASK(sz)); return;
    default:
        ea_addr(e, sz, a);
        sprintf(name, "v%d", tmpn++);
        E("uint32_t %s = %s(%s);", name, RD(sz), a);
        return;
    }
}

/* write to a destination not yet addressed */
static void ea_write(const EA *e, int sz, const char *val)
{
    char a[16];
    switch (e->mode) {
    case EA_DREG: E("set_dreg(%d, %s, %d);", e->reg, val, sz); return;
    case EA_AREG: E("cpu.a[%d] = %s;", e->reg, val); return;
    default: ea_addr(e, sz, a); E("%s(%s, %s);", WR(sz), a, val); return;
    }
}

/* read-modify-write: emits the read; rmw_write emits the matching write */
typedef struct { const EA *e; int sz; char addr[16]; } Rmw;
static void rmw_read(Rmw *r, char *name)
{
    sprintf(name, "v%d", tmpn++);
    if (r->e->mode == EA_DREG) { E("uint32_t %s = cpu.d[%d] & 0x%xu;", name, r->e->reg, MASK(r->sz)); return; }
    if (r->e->mode == EA_AREG) { E("uint32_t %s = cpu.a[%d];", name, r->e->reg); return; }
    ea_addr(r->e, r->sz, r->addr);
    E("uint32_t %s = %s(%s);", name, RD(r->sz), r->addr);
}
static void rmw_write(Rmw *r, const char *val)
{
    if (r->e->mode == EA_DREG) E("set_dreg(%d, %s, %d);", r->e->reg, val, r->sz);
    else if (r->e->mode == EA_AREG) E("cpu.a[%d] = %s;", r->e->reg, val);
    else E("%s(%s, %s);", WR(r->sz), r->addr, val);
}

/* generic fallback: interpreter executor on a constant Insn */
static int insn_consts;
static void emit_generic(const Insn *i, uint32_t next, FILE *consts)
{
    stat_generic++;
    int id = insn_consts++;
    const EA *s = &i->src, *d = &i->dst;
    fprintf(consts, "static const Insn I%d = { 0x%xu, 0x%x, %u, %u, %u, %u, %u, %u, "
            "{ %u, %u, %u, %u, %d, 0x%xu }, { %u, %u, %u, %u, %d, 0x%xu }, 0x%xu, 0x%xu, 0, {0} };\n",
            id, i->pc, i->op, i->len, i->kind, i->size, i->cond, i->flow, i->regmem,
            s->mode, s->reg, s->xreg, s->xlong, s->disp, s->val,
            d->mode, d->reg, d->xreg, d->xlong, d->disp, d->val, i->imm, i->target);
    E("cpu.pc = 0x%xu; cpu.cycles += %u; m68k_execute_insn(&I%d);", next, i->cycles, id);
}

static const char *ARITH(int k)
{
    switch (k) {
    case OP_ORI: case OP_OR: return "|";
    case OP_ANDI: case OP_AND: return "&";
    default: return "^";
    }
}

/* returns 1 if the instruction ended the block (emitted a return / set pc) */
static int emit_insn(const Insn *i, FILE *consts, bool last)
{
    int sz = i->size;
    uint32_t next = i->pc + i->len;
    char s[16], d[16], t[64];
    char dis[96]; m68k_disasm(i, dis, sizeof dis);
    fprintf(out, "    /* %06x  %s */\n", i->pc, dis);
    fprintf(out, "    {\n");
    stat_special++;
    #define CYC() E("cpu.cycles += %u;", i->cycles)
    switch (i->kind) {
    case OP_ORI: case OP_ANDI: case OP_EORI: case OP_OR: case OP_AND: case OP_EOR: {
        CYC();
        ea_read(&i->src, sz, s);
        Rmw r = { &i->dst, sz, "" }; rmw_read(&r, d);
        sprintf(t, "r%d", tmpn++);
        E("uint32_t %s = %s %s %s;", t, d, ARITH(i->kind), s);
        E("flags_logic(%s, %d);", t, sz);
        rmw_write(&r, t);
        break;
    }
    case OP_ADDI: case OP_ADD: case OP_ADDQ: case OP_SUBI: case OP_SUB: case OP_SUBQ: {
        CYC();
        bool add = i->kind == OP_ADDI || i->kind == OP_ADD || i->kind == OP_ADDQ;
        ea_read(&i->src, sz, s);
        if (i->dst.mode == EA_AREG) { E("cpu.a[%d] %s= %s;", i->dst.reg, add ? "+" : "-", s); break; }
        Rmw r = { &i->dst, sz, "" }; rmw_read(&r, d);
        sprintf(t, "%s(%s, %s, %d)", add ? "sem_add" : "sem_sub", s, d, sz);
        rmw_write(&r, t);
        break;
    }
    case OP_CMPI: case OP_CMP:
        CYC(); ea_read(&i->src, sz, s); ea_read(&i->dst, sz, d);
        E("sem_cmp(%s, %s, %d);", s, d, sz);
        break;
    case OP_CMPA:
        CYC(); ea_read(&i->src, sz, s);
        if (sz == 2) E("sem_cmp((uint32_t)(int32_t)(int16_t)%s, cpu.a[%d], 4);", s, i->dst.reg);
        else E("sem_cmp(%s, cpu.a[%d], 4);", s, i->dst.reg);
        break;
    case OP_CMPM:
        CYC(); ea_read(&i->src, sz, s); ea_read(&i->dst, sz, d);
        E("sem_cmp(%s, %s, %d);", s, d, sz);
        break;
    case OP_ADDA: case OP_SUBA:
        CYC(); ea_read(&i->src, sz, s);
        if (sz == 2) E("cpu.a[%d] %s= (uint32_t)(int32_t)(int16_t)%s;", i->dst.reg, i->kind == OP_ADDA ? "+" : "-", s);
        else E("cpu.a[%d] %s= %s;", i->dst.reg, i->kind == OP_ADDA ? "+" : "-", s);
        break;
    case OP_ADDX: case OP_SUBX: {
        CYC(); ea_read(&i->src, sz, s);
        Rmw r = { &i->dst, sz, "" }; rmw_read(&r, d);
        sprintf(t, "%s(%s, %s, %d)", i->kind == OP_ADDX ? "sem_addx" : "sem_subx", s, d, sz);
        rmw_write(&r, t);
        break;
    }
    case OP_ABCD: case OP_SBCD: {
        CYC(); ea_read(&i->src, 1, s);
        Rmw r = { &i->dst, 1, "" }; rmw_read(&r, d);
        sprintf(t, "%s(%s, %s)", i->kind == OP_ABCD ? "sem_abcd" : "sem_sbcd", s, d);
        rmw_write(&r, t);
        break;
    }
    case OP_ORI_CCR: CYC(); E("set_ccr(get_ccr() | 0x%xu);", i->imm); break;
    case OP_ANDI_CCR: CYC(); E("set_ccr(get_ccr() & 0x%xu);", i->imm); break;
    case OP_EORI_CCR: CYC(); E("set_ccr(get_ccr() ^ 0x%xu);", i->imm); break;
    case OP_BTST: case OP_BCHG: case OP_BCLR: case OP_BSET: {
        CYC();
        char bit[32];
        if (i->src.mode == EA_DREG) sprintf(bit, "cpu.d[%d]", i->src.reg); else sprintf(bit, "0x%xu", i->src.val);
        if (i->kind == OP_BTST) { ea_read(&i->dst, sz, d); E("sem_bitop(OP_BTST, %s, %s, %d);", d, bit, sz); break; }
        Rmw r = { &i->dst, sz, "" }; rmw_read(&r, d);
        sprintf(t, "sem_bitop(%d, %s, %s, %d)", i->kind, d, bit, sz);
        rmw_write(&r, t);
        break;
    }
    case OP_MOVE:
        CYC(); ea_read(&i->src, sz, s);
        E("flags_logic(%s, %d);", s, sz);
        ea_write(&i->dst, sz, s);
        break;
    case OP_MOVEA:
        CYC(); ea_read(&i->src, sz, s);
        if (sz == 2) E("cpu.a[%d] = (uint32_t)(int32_t)(int16_t)%s;", i->dst.reg, s);
        else E("cpu.a[%d] = %s;", i->dst.reg, s);
        break;
    case OP_MOVEQ:
        CYC(); E("cpu.d[%d] = 0x%xu; flags_logic(0x%xu, 4);", i->dst.reg, i->imm, i->imm);
        break;
    case OP_NEGX: case OP_NEG: case OP_NOT: {
        CYC();
        Rmw r = { &i->dst, sz, "" }; rmw_read(&r, d);
        if (i->kind == OP_NOT) {
            sprintf(t, "r%d", tmpn++);
            E("uint32_t %s = ~%s & 0x%xu; flags_logic(%s, %d);", t, d, MASK(sz), t, sz);
        } else sprintf(t, "%s(%s, %d)", i->kind == OP_NEG ? "sem_neg" : "sem_negx", d, sz);
        rmw_write(&r, t);
        break;
    }
    case OP_CLR:
        CYC();
        if (i->dst.mode != EA_DREG) { char a[16]; ea_addr(&i->dst, sz, a); E("%s(%s, 0);", WR(sz), a); }
        else E("set_dreg(%d, 0, %d);", i->dst.reg, sz);
        E("cpu.n = 0; cpu.z = 1; cpu.v = 0; cpu.c = 0;");
        break;
    case OP_TST: CYC(); ea_read(&i->dst, sz, d); E("flags_logic(%s, %d);", d, sz); break;
    case OP_EXT:
        CYC();
        if (sz == 2) E("set_dreg(%d, (uint32_t)(int32_t)(int8_t)cpu.d[%d], 2); flags_logic(cpu.d[%d], 2);", i->dst.reg, i->dst.reg, i->dst.reg);
        else E("cpu.d[%d] = (uint32_t)(int32_t)(int16_t)cpu.d[%d]; flags_logic(cpu.d[%d], 4);", i->dst.reg, i->dst.reg, i->dst.reg);
        break;
    case OP_SWAP:
        CYC(); E("cpu.d[%d] = cpu.d[%d] << 16 | cpu.d[%d] >> 16; flags_logic(cpu.d[%d], 4);", i->dst.reg, i->dst.reg, i->dst.reg, i->dst.reg);
        break;
    case OP_PEA: { CYC(); char a[16]; ea_addr(&i->src, 4, a); E("push32(%s);", a); break; }
    case OP_LEA: { CYC(); char a[16]; ea_addr(&i->src, 4, a); E("cpu.a[%d] = %s;", i->dst.reg, a); break; }
    case OP_MOVEM_RM:
        CYC();
        if (i->dst.mode == EA_PREDEC) E("cpu.a[%d] = sem_movem_rm(0x%xu, cpu.a[%d], %d, true);", i->dst.reg, i->imm, i->dst.reg, sz);
        else { char a[16]; ea_addr(&i->dst, sz, a); E("sem_movem_rm(0x%xu, %s, %d, false);", i->imm, a, sz); }
        break;
    case OP_MOVEM_MR:
        CYC();
        if (i->src.mode == EA_POSTINC) E("{ uint32_t a_ = sem_movem_mr(0x%xu, cpu.a[%d], %d); cpu.a[%d] = a_; }", i->imm, i->src.reg, sz, i->src.reg);
        else { char a[16]; ea_addr(&i->src, sz, a); E("sem_movem_mr(0x%xu, %s, %d);", i->imm, a, sz); }
        E("cpu.cycles += 4;");
        break;
    case OP_LINK:
        CYC(); E("push32(cpu.a[%d]); cpu.a[%d] = cpu.a[7]; cpu.a[7] += 0x%xu;", i->dst.reg, i->dst.reg, i->imm);
        break;
    case OP_UNLK:
        CYC(); E("cpu.a[7] = cpu.a[%d]; cpu.a[%d] = pop32();", i->dst.reg, i->dst.reg);
        break;
    case OP_NOP: CYC(); break;
    case OP_SCC:
        CYC();
        if (i->dst.mode == EA_DREG) E("{ bool t_ = test_cc(%d); if (t_) cpu.cycles += 2; set_dreg(%d, t_ ? 0xff : 0, 1); }", i->cond, i->dst.reg);
        else { E("bool t%d = test_cc(%d);", tmpn, i->cond); sprintf(t, "(t%d ? 0xffu : 0u)", tmpn); tmpn++; ea_write(&i->dst, 1, t); }
        break;
    case OP_MULU: case OP_MULS:
        CYC(); ea_read(&i->src, 2, s);
        E("cpu.d[%d] = %s(%s, cpu.d[%d]);", i->dst.reg, i->kind == OP_MULU ? "sem_mulu" : "sem_muls", s, i->dst.reg);
        break;
    case OP_DIVU: case OP_DIVS:
        CYC(); ea_read(&i->src, 2, s);
        E("if (!%s(%s, %d)) { cpu.pc = 0x%xu; m68k_exception(5); return; }", i->kind == OP_DIVU ? "sem_divu" : "sem_divs", s, i->dst.reg, next);
        break;
    case OP_EXG: {
        CYC();
        const char *x = i->src.mode == EA_DREG ? "d" : "a", *y = i->dst.mode == EA_DREG ? "d" : "a";
        E("{ uint32_t t_ = cpu.%s[%d]; cpu.%s[%d] = cpu.%s[%d]; cpu.%s[%d] = t_; }", x, i->src.reg, x, i->src.reg, y, i->dst.reg, y, i->dst.reg);
        break;
    }
    case OP_ASL: case OP_ASR: case OP_LSL: case OP_LSR:
    case OP_ROXL: case OP_ROXR: case OP_ROL: case OP_ROR:
        CYC();
        if (i->regmem == 2) {
            char a[16]; ea_addr(&i->dst, 2, a);
            E("mem_ww(%s, sem_shift(%d, mem_rw(%s), 1, 2)); cpu.cycles -= 2;", a, i->kind, a);
        } else if (i->regmem == 1) {
            E("set_dreg(%d, sem_shift(%d, cpu.d[%d], cpu.d[%d] & 63, %d), %d);", i->dst.reg, i->kind, i->dst.reg, i->src.reg, sz, sz);
        } else {
            E("cpu.cycles -= %u; set_dreg(%d, sem_shift(%d, cpu.d[%d], %u, %d), %d);", 2 * i->imm, i->dst.reg, i->kind, i->dst.reg, i->imm, sz, sz);
        }
        break;

    /* ---- control flow ---- */
    case OP_BRA: CYC(); E("cpu.pc = 0x%xu; return;", i->target); fprintf(out, "    }\n"); return 1;
    case OP_BSR: CYC(); E("push32(0x%xu); cpu.pc = 0x%xu; return;", next, i->target); fprintf(out, "    }\n"); return 1;
    case OP_BCC:
        CYC();
        E("if (test_cc(%d)) cpu.pc = 0x%xu; else { cpu.cycles += %s; cpu.pc = 0x%xu; }", i->cond, i->target,
          i->size == 1 ? "(uint64_t)-2" : "2", next);
        E("return;");
        fprintf(out, "    }\n"); return 1;
    case OP_DBCC:
        CYC();
        E("if (test_cc(%d)) { cpu.cycles += 2; cpu.pc = 0x%xu; return; }", i->cond, next);
        E("{ uint16_t w_ = (uint16_t)(cpu.d[%d] - 1); set_dreg(%d, w_, 2);", i->dst.reg, i->dst.reg);
        E("  if (w_ != 0xffff) cpu.pc = 0x%xu; else { cpu.cycles += 4; cpu.pc = 0x%xu; } }", i->target, next);
        E("return;");
        fprintf(out, "    }\n"); return 1;
    case OP_JMP: { CYC(); char a[16]; ea_addr(&i->src, 4, a); E("cpu.pc = %s; return;", a); fprintf(out, "    }\n"); return 1; }
    case OP_JSR: { CYC(); char a[16]; ea_addr(&i->src, 4, a); E("push32(0x%xu); cpu.pc = %s; return;", next, a); fprintf(out, "    }\n"); return 1; }
    case OP_RTS: CYC(); E("cpu.pc = pop32(); return;"); fprintf(out, "    }\n"); return 1;

    default:
        stat_special--;
        emit_generic(i, next, consts);
        if (m68k_ends_block(i)) { E("return;"); fprintf(out, "    }\n"); return 1; }
        break;
    }
    fprintf(out, "    }\n");
    if (last) E("cpu.pc = 0x%xu;", next);
    return 0;
}

typedef struct { uint32_t pc; uint32_t len; uint8_t *bytes; } Cov;

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: lift coverage.txt outdir [blocks_per_file]\n"); return 2; }
    int per_file = argc > 3 ? atoi(argv[3]) : 1500;
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    Cov *cov = 0; int n = 0, cap = 0;
    static char line[20000];
    while (fgets(line, sizeof line, f)) {
        unsigned pc; char *sp = strchr(line, ' ');
        if (!sp || sscanf(line, "%x", &pc) != 1) continue;
        char *hex = sp + 1; size_t L = strcspn(hex, "\r\n") / 2;
        if (n == cap) { cap = cap ? cap * 2 : 1024; cov = realloc(cov, cap * sizeof *cov); }
        cov[n].pc = pc; cov[n].len = (uint32_t)L; cov[n].bytes = malloc(L);
        for (size_t k = 0; k < L; k++) { unsigned b; sscanf(hex + 2 * k, "%2x", &b); cov[n].bytes[k] = (uint8_t)b; }
        n++;
    }
    fclose(f);
    /* deterministic order */
    for (int a = 1; a < n; a++) {        /* insertion sort by pc, then bytes */
        Cov c = cov[a]; int b = a - 1;
        while (b >= 0 && (cov[b].pc > c.pc || (cov[b].pc == c.pc && (cov[b].len > c.len ||
               (cov[b].len == c.len && memcmp(cov[b].bytes, c.bytes, c.len) > 0))))) { cov[b + 1] = cov[b]; b--; }
        cov[b + 1] = c;
    }
    int files = (n + per_file - 1) / per_file;
    char path[1024];
    for (int fi = 0; fi < files; fi++) {
        snprintf(path, sizeof path, "%s/blocks_%03d.c", argv[2], fi);
        out = fopen(path, "w");
        snprintf(path, sizeof path, "%s/blocks_%03d.inc", argv[2], fi);
        FILE *consts = fopen(path, "w");
        fprintf(out, "/* Generated by tools/lift from the demo's own code. Do not edit. */\n");
        fprintf(out, "#include \"../../runtime/cpu/m68k_sem.h\"\n#include \"../../runtime/core/exec.h\"\n#include \"../../runtime/amiga/mem_gen.h\"\n");
        fprintf(out, "#include \"blocks_%03d.inc\"\n\n", fi);
        int lo = fi * per_file, hi = lo + per_file < n ? lo + per_file : n;
        for (int k = lo; k < hi; k++) {
            blk_pc = cov[k].pc; blk_len = cov[k].len; memcpy(blk_bytes, cov[k].bytes, blk_len);
            fprintf(out, "static const uint8_t c%d[%u] = {", k, blk_len);
            for (uint32_t b = 0; b < blk_len; b++) fprintf(out, "%s0x%02x", b ? "," : "", blk_bytes[b]);
            fprintf(out, "};\n");
            fprintf(out, "static void b%d(void) /* $%06x */\n{\n", k, blk_pc);
            fprintf(out, "    const uint32_t g0_ = code_gen_sum(0x%xu);\n", blk_pc);
            tmpn = 0;
            uint32_t pc = blk_pc;
            int ended = 0;
            while (pc < blk_pc + blk_len) {
                Insn i; m68k_decode(pc, &i);
                bool last = pc + i.len >= blk_pc + blk_len;
                ended = emit_insn(&i, consts, last);
                pc += i.len;
                if (ended) break;
                if (!last) fprintf(out, "    if (code_gen_sum(0x%xu) != g0_) { cpu.pc = 0x%xu; return; }\n", blk_pc, pc);
            }
            fprintf(out, "}\n\n");
        }
        fprintf(out, "const RecompBlock recomp_blocks_%03d[] = {\n", fi);
        for (int k = lo; k < hi; k++) fprintf(out, "    { 0x%xu, %u, c%d, b%d },\n", cov[k].pc, cov[k].len, k, k);
        fprintf(out, "};\nconst int recomp_blocks_%03d_count = %d;\n", fi, hi - lo);
        fclose(out); fclose(consts);
    }
    snprintf(path, sizeof path, "%s/registry.c", argv[2]);
    out = fopen(path, "w");
    fprintf(out, "/* Generated by tools/lift. */\n#include \"../../runtime/core/exec.h\"\n");
    for (int fi = 0; fi < files; fi++) fprintf(out, "extern const RecompBlock recomp_blocks_%03d[]; extern const int recomp_blocks_%03d_count;\n", fi, fi);
    fprintf(out, "int recomp_block_total = %d;\nvoid recomp_register_all(void)\n{\n", n);
    for (int fi = 0; fi < files; fi++) fprintf(out, "    exec_register_blocks(recomp_blocks_%03d, recomp_blocks_%03d_count);\n", fi, fi);
    fprintf(out, "}\n");
    fclose(out);
    fprintf(stderr, "lifted %d block variants into %d files: %lu specialised insns, %lu generic\n", n, files, stat_special, stat_generic);
    return 0;
}
