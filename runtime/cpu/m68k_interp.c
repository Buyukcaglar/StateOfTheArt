/* 68000 interpreter over decoded instructions. Used for code the static
 * recompiler has not seen (or that has been modified at runtime), and as the
 * reference when generating coverage. */
#include "m68k_sem.h"
#include <string.h>

M68K cpu;

uint16_t m68k_get_sr(void)
{
    return (uint16_t)(cpu.t << 15 | cpu.s << 13 | (cpu.ipl_mask & 7) << 8 | get_ccr());
}

void m68k_set_sr(uint16_t sr)
{
    uint8_t ns = (sr >> 13) & 1;
    if (ns != cpu.s) {
        if (ns) { cpu.usp = cpu.a[7]; cpu.a[7] = cpu.ssp; }
        else    { cpu.ssp = cpu.a[7]; cpu.a[7] = cpu.usp; }
        cpu.s = ns;
    }
    cpu.t = (sr >> 15) & 1;
    cpu.ipl_mask = (sr >> 8) & 7;
    set_ccr(sr & 0x1f);
}

void m68k_reset(uint32_t ssp, uint32_t pc)
{
    memset(&cpu, 0, sizeof cpu);
    cpu.s = 1; cpu.ipl_mask = 7;
    cpu.a[7] = cpu.ssp = ssp;
    cpu.pc = pc;
}

/* Push a group-1/2 frame and vector. cpu.pc must hold the PC to stack. */
void m68k_exception(int vector)
{
    uint16_t sr = m68k_get_sr();
    m68k_set_sr((sr | 0x2000) & 0x7fff);   /* supervisor, trace off */
    push32(cpu.pc);
    push16(sr);
    cpu.pc = mem_rl((uint32_t)vector * 4);
    cpu.cycles += 34;
}

bool m68k_check_irq(void)
{
    int lvl = cpu.pending_ipl;
    if (lvl && (lvl > cpu.ipl_mask || lvl == 7)) {
        cpu.stopped = 0;
        m68k_exception(24 + lvl);
        cpu.ipl_mask = (uint8_t)lvl;
        cpu.cycles += 10;
        return true;
    }
    return false;
}

/* ---- generic EA access ---- */
static uint32_t ea_addr(const EA *e, int sz)
{
    switch (e->mode) {
    case EA_AIND: return cpu.a[e->reg];
    case EA_POSTINC: { uint32_t a = cpu.a[e->reg]; cpu.a[e->reg] += step_of(e->reg, sz); return a; }
    case EA_PREDEC: cpu.a[e->reg] -= step_of(e->reg, sz); return cpu.a[e->reg];
    case EA_ADISP: return cpu.a[e->reg] + (uint32_t)e->disp;
    case EA_AIDX: return cpu.a[e->reg] + (uint32_t)e->disp + xval(e->xreg, e->xlong);
    case EA_ABSW: case EA_ABSL: case EA_PCDISP: return e->val;
    case EA_PCIDX: return e->val + xval(e->xreg, e->xlong);
    default: return 0;
    }
}

static uint32_t ea_read(const EA *e, int sz)
{
    switch (e->mode) {
    case EA_DREG: return cpu.d[e->reg] & sz_mask(sz);
    case EA_AREG: return cpu.a[e->reg] & sz_mask(sz);
    case EA_IMM: return e->val & sz_mask(sz);
    default: return sem_read(ea_addr(e, sz), sz);
    }
}

/* write to a destination whose address has NOT been computed yet */
static void ea_write(const EA *e, uint32_t v, int sz)
{
    switch (e->mode) {
    case EA_DREG: set_dreg(e->reg, v, sz); break;
    case EA_AREG: cpu.a[e->reg] = v; break;
    default: sem_write(ea_addr(e, sz), v, sz); break;
    }
}

/* read-modify-write helper state */
typedef struct { const EA *e; uint32_t addr; int sz; } Rmw;
static uint32_t rmw_read(Rmw *r)
{
    if (r->e->mode == EA_DREG) return cpu.d[r->e->reg] & sz_mask(r->sz);
    if (r->e->mode == EA_AREG) return cpu.a[r->e->reg];
    r->addr = ea_addr(r->e, r->sz);
    return sem_read(r->addr, r->sz);
}
static void rmw_write(Rmw *r, uint32_t v)
{
    if (r->e->mode == EA_DREG) set_dreg(r->e->reg, v, r->sz);
    else if (r->e->mode == EA_AREG) cpu.a[r->e->reg] = v;
    else sem_write(r->addr, v, r->sz);
}

static void privilege_violation(uint32_t insn_pc)
{
    cpu.pc = insn_pc;
    m68k_exception(8);
}

/* Executes a decoded instruction. cpu.pc already points past it. */
void m68k_execute_insn(const Insn *i)
{
    int sz = i->size;
    cpu.cycles += i->cycles;
    switch (i->kind) {
    case OP_ILLEGAL: cpu.pc = i->pc; m68k_exception(4); break;
    case OP_LINEA: cpu.pc = i->pc; m68k_exception(10); break;
    case OP_LINEF: cpu.pc = i->pc; m68k_exception(11); break;

    case OP_ORI: case OP_ANDI: case OP_EORI: case OP_OR: case OP_AND: case OP_EOR: {
        uint32_t s = ea_read(&i->src, sz);
        Rmw r = { &i->dst, 0, sz };
        uint32_t d = rmw_read(&r), v;
        if (i->kind == OP_ORI || i->kind == OP_OR) v = d | s;
        else if (i->kind == OP_ANDI || i->kind == OP_AND) v = d & s;
        else v = d ^ s;
        flags_logic(v, sz);
        rmw_write(&r, v);
        break;
    }
    case OP_ADDI: case OP_ADD: case OP_ADDQ: case OP_SUBI: case OP_SUB: case OP_SUBQ: {
        uint32_t s = ea_read(&i->src, sz);
        bool add = i->kind == OP_ADDI || i->kind == OP_ADD || i->kind == OP_ADDQ;
        if (i->dst.mode == EA_AREG) { /* ADDQ/SUBQ to An: full 32 bits, no flags */
            cpu.a[i->dst.reg] += add ? s : (uint32_t)-s;
            break;
        }
        Rmw r = { &i->dst, 0, sz };
        uint32_t d = rmw_read(&r);
        rmw_write(&r, add ? sem_add(s, d, sz) : sem_sub(s, d, sz));
        break;
    }
    case OP_CMPI: case OP_CMP: {
        uint32_t s = ea_read(&i->src, sz);
        sem_cmp(s, ea_read(&i->dst, sz), sz);
        break;
    }
    case OP_CMPA: {
        uint32_t s = ea_read(&i->src, sz);
        if (sz == 2) s = (uint32_t)(int32_t)(int16_t)s;
        sem_cmp(s, cpu.a[i->dst.reg], 4);
        break;
    }
    case OP_CMPM: {
        uint32_t s = ea_read(&i->src, sz);
        uint32_t d = ea_read(&i->dst, sz);
        sem_cmp(s, d, sz);
        break;
    }
    case OP_ADDA: case OP_SUBA: {
        uint32_t s = ea_read(&i->src, sz);
        if (sz == 2) s = (uint32_t)(int32_t)(int16_t)s;
        cpu.a[i->dst.reg] += i->kind == OP_ADDA ? s : (uint32_t)-s;
        break;
    }
    case OP_ADDX: case OP_SUBX: {
        uint32_t s = ea_read(&i->src, sz);
        Rmw r = { &i->dst, 0, sz };
        uint32_t d = rmw_read(&r);
        rmw_write(&r, i->kind == OP_ADDX ? sem_addx(s, d, sz) : sem_subx(s, d, sz));
        break;
    }
    case OP_ABCD: case OP_SBCD: {
        uint32_t s = ea_read(&i->src, 1);
        Rmw r = { &i->dst, 0, 1 };
        uint32_t d = rmw_read(&r);
        rmw_write(&r, i->kind == OP_ABCD ? sem_abcd(s, d) : sem_sbcd(s, d));
        break;
    }
    case OP_NBCD: {
        Rmw r = { &i->dst, 0, 1 };
        uint32_t d = rmw_read(&r);
        rmw_write(&r, sem_sbcd(d, 0));
        break;
    }
    case OP_ORI_CCR: set_ccr(get_ccr() | i->imm); break;
    case OP_ANDI_CCR: set_ccr(get_ccr() & i->imm); break;
    case OP_EORI_CCR: set_ccr(get_ccr() ^ i->imm); break;
    case OP_ORI_SR: case OP_ANDI_SR: case OP_EORI_SR: {
        if (!cpu.s) { privilege_violation(i->pc); break; }
        uint16_t sr = m68k_get_sr();
        sr = i->kind == OP_ORI_SR ? sr | i->imm : i->kind == OP_ANDI_SR ? sr & i->imm : sr ^ i->imm;
        m68k_set_sr(sr);
        break;
    }
    case OP_BTST: case OP_BCHG: case OP_BCLR: case OP_BSET: {
        uint32_t bit = i->src.mode == EA_DREG ? cpu.d[i->src.reg] : i->src.val;
        if (i->kind == OP_BTST) { sem_bitop(OP_BTST, ea_read(&i->dst, sz), bit, sz); break; }
        Rmw r = { &i->dst, 0, sz };
        uint32_t d = rmw_read(&r);
        rmw_write(&r, sem_bitop(i->kind, d, bit, sz));
        break;
    }
    case OP_MOVEP_MR: { /* (d16,Ay) -> Dx */
        uint32_t a = cpu.a[i->dst.reg] + (uint32_t)i->dst.disp;
        int r = i->src.reg;
        if (sz == 4) cpu.d[r] = mem_rb(a) << 24 | mem_rb(a + 2) << 16 | mem_rb(a + 4) << 8 | mem_rb(a + 6);
        else set_dreg(r, mem_rb(a) << 8 | mem_rb(a + 2), 2);
        break;
    }
    case OP_MOVEP_RM: {
        uint32_t a = cpu.a[i->dst.reg] + (uint32_t)i->dst.disp, v = cpu.d[i->src.reg];
        if (sz == 4) { mem_wb(a, v >> 24); mem_wb(a + 2, v >> 16); mem_wb(a + 4, v >> 8); mem_wb(a + 6, v); }
        else { mem_wb(a, v >> 8); mem_wb(a + 2, v); }
        break;
    }
    case OP_MOVE: {
        uint32_t v = ea_read(&i->src, sz);
        flags_logic(v, sz);
        ea_write(&i->dst, v, sz);
        break;
    }
    case OP_MOVEA: {
        uint32_t v = ea_read(&i->src, sz);
        cpu.a[i->dst.reg] = sz == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
        break;
    }
    case OP_MOVEQ: cpu.d[i->dst.reg] = i->imm; flags_logic(i->imm, 4); break;
    case OP_NEGX: case OP_NEG: case OP_NOT: {
        Rmw r = { &i->dst, 0, sz };
        uint32_t d = rmw_read(&r), v;
        if (i->kind == OP_NOT) { v = ~d & sz_mask(sz); flags_logic(v, sz); }
        else v = i->kind == OP_NEG ? sem_neg(d, sz) : sem_negx(d, sz);
        rmw_write(&r, v);
        break;
    }
    case OP_CLR: {
        /* the 68000 performs a read before the write */
        Rmw r = { &i->dst, 0, sz };
        if (i->dst.mode != EA_DREG) { r.addr = ea_addr(&i->dst, sz); sem_write(r.addr, 0, sz); }
        else set_dreg(i->dst.reg, 0, sz);
        cpu.n = 0; cpu.z = 1; cpu.v = 0; cpu.c = 0;
        break;
    }
    case OP_TST: flags_logic(ea_read(&i->dst, sz), sz); break;
    case OP_TAS: {
        Rmw r = { &i->dst, 0, 1 };
        uint32_t d = rmw_read(&r);
        flags_logic(d, 1);
        rmw_write(&r, d | 0x80);
        break;
    }
    case OP_MOVE_FROM_SR: ea_write(&i->dst, m68k_get_sr(), 2); break;
    case OP_MOVE_TO_CCR: set_ccr(ea_read(&i->src, 2)); break;
    case OP_MOVE_TO_SR:
        if (!cpu.s) { privilege_violation(i->pc); break; }
        m68k_set_sr((uint16_t)ea_read(&i->src, 2));
        break;
    case OP_EXT: {
        int r = i->dst.reg;
        if (sz == 2) { set_dreg(r, (uint32_t)(int32_t)(int8_t)cpu.d[r], 2); flags_logic(cpu.d[r], 2); }
        else { cpu.d[r] = (uint32_t)(int32_t)(int16_t)cpu.d[r]; flags_logic(cpu.d[r], 4); }
        break;
    }
    case OP_SWAP: { int r = i->dst.reg; cpu.d[r] = cpu.d[r] << 16 | cpu.d[r] >> 16; flags_logic(cpu.d[r], 4); break; }
    case OP_PEA: { uint32_t a = ea_addr(&i->src, 4); push32(a); break; }
    case OP_LEA: cpu.a[i->dst.reg] = ea_addr(&i->src, 4); break;
    case OP_MOVEM_RM: {
        if (i->dst.mode == EA_PREDEC) {
            cpu.a[i->dst.reg] = sem_movem_rm(i->imm, cpu.a[i->dst.reg], sz, true);
        } else sem_movem_rm(i->imm, ea_addr(&i->dst, sz), sz, false);
        break;
    }
    case OP_MOVEM_MR: {
        if (i->src.mode == EA_POSTINC) {
            uint32_t a = sem_movem_mr(i->imm, cpu.a[i->src.reg], sz);
            cpu.a[i->src.reg] = a;
        } else sem_movem_mr(i->imm, ea_addr(&i->src, sz), sz);
        cpu.cycles += 4; /* extra prefetch read */
        break;
    }
    case OP_TRAP: m68k_exception((int)i->imm); break;
    case OP_TRAPV: if (cpu.v) m68k_exception(7); break;
    case OP_CHK: {
        int32_t v = (int16_t)cpu.d[i->dst.reg];
        int32_t bound = (int16_t)ea_read(&i->src, 2);
        if (v < 0) { cpu.n = 1; m68k_exception(6); }
        else if (v > bound) { cpu.n = 0; m68k_exception(6); }
        break;
    }
    case OP_LINK: {
        int r = i->dst.reg;
        push32(cpu.a[r]);
        cpu.a[r] = cpu.a[7];
        cpu.a[7] += i->imm;
        break;
    }
    case OP_UNLK: { int r = i->dst.reg; cpu.a[7] = cpu.a[r]; cpu.a[r] = pop32(); break; }
    case OP_MOVE_USP_TO:
        if (!cpu.s) { privilege_violation(i->pc); break; }
        cpu.usp = cpu.a[i->src.reg]; break;
    case OP_MOVE_USP_FROM:
        if (!cpu.s) { privilege_violation(i->pc); break; }
        cpu.a[i->dst.reg] = cpu.usp; break;
    case OP_RESET:
        if (!cpu.s) { privilege_violation(i->pc); break; }
        m68k_on_reset_instruction(); break;
    case OP_NOP: break;
    case OP_STOP:
        if (!cpu.s) { privilege_violation(i->pc); break; }
        m68k_set_sr((uint16_t)i->imm);
        cpu.stopped = 1;
        break;
    case OP_RTE: {
        if (!cpu.s) { privilege_violation(i->pc); break; }
        uint16_t sr = (uint16_t)pop16();
        cpu.pc = pop32();
        m68k_set_sr(sr);
        break;
    }
    case OP_RTS: cpu.pc = pop32(); break;
    case OP_RTR: { set_ccr(pop16()); cpu.pc = pop32(); break; }
    case OP_JSR: { uint32_t t = ea_addr(&i->src, 4); push32(cpu.pc); cpu.pc = t; break; }
    case OP_JMP: cpu.pc = ea_addr(&i->src, 4); break;
    case OP_SCC: {
        bool t = test_cc(i->cond);
        if (t && i->dst.mode == EA_DREG) cpu.cycles += 2;
        ea_write(&i->dst, t ? 0xff : 0, 1);
        break;
    }
    case OP_DBCC: {
        if (test_cc(i->cond)) { cpu.cycles += 2; break; }
        int r = i->dst.reg;
        uint16_t w = (uint16_t)(cpu.d[r] - 1);
        set_dreg(r, w, 2);
        if (w != 0xffff) cpu.pc = i->target;
        else cpu.cycles += 4;
        break;
    }
    case OP_BRA: cpu.pc = i->target; break;
    case OP_BSR: push32(cpu.pc); cpu.pc = i->target; break;
    case OP_BCC:
        if (test_cc(i->cond)) cpu.pc = i->target;
        else cpu.cycles += i->size == 1 ? (uint64_t)-2 : 2;
        break;
    case OP_DIVU: case OP_DIVS: {
        uint32_t s = ea_read(&i->src, 2);
        bool ok = i->kind == OP_DIVU ? sem_divu(s, i->dst.reg) : sem_divs(s, i->dst.reg);
        if (!ok) m68k_exception(5);
        break;
    }
    case OP_MULU: { uint32_t s = ea_read(&i->src, 2); cpu.d[i->dst.reg] = sem_mulu(s, cpu.d[i->dst.reg]); break; }
    case OP_MULS: { uint32_t s = ea_read(&i->src, 2); cpu.d[i->dst.reg] = sem_muls(s, cpu.d[i->dst.reg]); break; }
    case OP_EXG: {
        uint32_t *x = i->src.mode == EA_DREG ? &cpu.d[i->src.reg] : &cpu.a[i->src.reg];
        uint32_t *y = i->dst.mode == EA_DREG ? &cpu.d[i->dst.reg] : &cpu.a[i->dst.reg];
        uint32_t t = *x; *x = *y; *y = t;
        break;
    }
    case OP_ASL: case OP_ASR: case OP_LSL: case OP_LSR:
    case OP_ROXL: case OP_ROXR: case OP_ROL: case OP_ROR: {
        if (i->regmem == 2) {
            uint32_t a = ea_addr(&i->dst, 2);
            mem_ww(a, sem_shift(i->kind, mem_rw(a), 1, 2));
            cpu.cycles -= 2; /* memory form is a flat 8+ea */
        } else {
            uint32_t cnt = i->regmem ? cpu.d[i->src.reg] & 63 : i->imm;
            if (!i->regmem) cpu.cycles -= 2 * cnt; /* already counted statically */
            int r = i->dst.reg;
            set_dreg(r, sem_shift(i->kind, cpu.d[r], cnt, sz), sz);
        }
        break;
    }
    default:
        cpu.pc = i->pc; m68k_exception(4); break;
    }
}

int m68k_step(void)
{
    uint64_t before = cpu.cycles;
    if (m68k_check_irq()) return (int)(cpu.cycles - before);
    if (cpu.stopped) { cpu.cycles += 4; return 4; }
    Insn i;
    m68k_decode(cpu.pc, &i);
    cpu.pc += i.len;
    m68k_execute_insn(&i);
    return (int)(cpu.cycles - before);
}
