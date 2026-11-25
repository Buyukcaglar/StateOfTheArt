/* Shared 68000 instruction semantics. Included by the interpreter and by every
 * generated translation unit; all helpers are static inline so constant
 * operands from the lifter fold away. */
#ifndef SOTA_M68K_SEM_H
#define SOTA_M68K_SEM_H

#include "m68k.h"

#define C (&cpu)

static inline uint32_t sz_mask(int sz) { return sz == 1 ? 0xffu : sz == 2 ? 0xffffu : 0xffffffffu; }
static inline uint32_t sz_msb(int sz)  { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }
static inline int32_t  sext(uint32_t v, int sz)
{
    return sz == 1 ? (int32_t)(int8_t)v : sz == 2 ? (int32_t)(int16_t)v : (int32_t)v;
}

static inline uint32_t sem_read(uint32_t a, int sz)
{
    return sz == 1 ? mem_rb(a) : sz == 2 ? mem_rw(a) : mem_rl(a);
}
static inline void sem_write(uint32_t a, uint32_t v, int sz)
{
    if (sz == 1) mem_wb(a, v); else if (sz == 2) mem_ww(a, v); else mem_wl(a, v);
}

/* data register partial write */
static inline void set_dreg(int r, uint32_t v, int sz)
{
    if (sz == 1) cpu.d[r] = (cpu.d[r] & 0xffffff00u) | (v & 0xff);
    else if (sz == 2) cpu.d[r] = (cpu.d[r] & 0xffff0000u) | (v & 0xffff);
    else cpu.d[r] = v;
}

/* index register value for (d8,An,Xn) */
static inline uint32_t xval(int xreg, int xlong)
{
    uint32_t v = xreg < 8 ? cpu.d[xreg] : cpu.a[xreg - 8];
    return xlong ? v : (uint32_t)(int32_t)(int16_t)v;
}

/* postincrement/predecrement step: byte on A7 keeps the stack even */
static inline uint32_t step_of(int reg, int sz) { return (sz == 1 && reg == 7) ? 2 : (uint32_t)sz; }

/* ---- condition codes ---- */
static inline void flags_logic(uint32_t r, int sz)
{
    r &= sz_mask(sz);
    cpu.n = (r & sz_msb(sz)) != 0; cpu.z = r == 0; cpu.v = 0; cpu.c = 0;
}

static inline bool test_cc(int cc)
{
    switch (cc) {
    case 0: return true;
    case 1: return false;
    case 2: return !cpu.c && !cpu.z;          /* HI */
    case 3: return cpu.c || cpu.z;            /* LS */
    case 4: return !cpu.c;                    /* CC */
    case 5: return cpu.c;                     /* CS */
    case 6: return !cpu.z;                    /* NE */
    case 7: return cpu.z;                     /* EQ */
    case 8: return !cpu.v;                    /* VC */
    case 9: return cpu.v;                     /* VS */
    case 10: return !cpu.n;                   /* PL */
    case 11: return cpu.n;                    /* MI */
    case 12: return cpu.n == cpu.v;           /* GE */
    case 13: return cpu.n != cpu.v;           /* LT */
    case 14: return !cpu.z && cpu.n == cpu.v; /* GT */
    default: return cpu.z || cpu.n != cpu.v;  /* LE */
    }
}

static inline uint32_t sem_add(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz), msb = sz_msb(sz);
    s &= m; d &= m;
    uint64_t full = (uint64_t)s + d;
    uint32_t r = (uint32_t)full & m;
    cpu.c = cpu.x = sz == 4 ? (full >> 32) & 1 : (full >> (sz * 8)) & 1;
    cpu.v = ((~(s ^ d) & (r ^ d)) & msb) != 0;
    cpu.n = (r & msb) != 0; cpu.z = r == 0;
    return r;
}
static inline uint32_t sem_sub(uint32_t s, uint32_t d, int sz)   /* d - s */
{
    uint32_t m = sz_mask(sz), msb = sz_msb(sz);
    s &= m; d &= m;
    uint32_t r = (d - s) & m;
    cpu.c = cpu.x = s > d;
    cpu.v = (((s ^ d) & (r ^ d)) & msb) != 0;
    cpu.n = (r & msb) != 0; cpu.z = r == 0;
    return r;
}
static inline void sem_cmp(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz), msb = sz_msb(sz);
    s &= m; d &= m;
    uint32_t r = (d - s) & m;
    cpu.c = s > d;
    cpu.v = (((s ^ d) & (r ^ d)) & msb) != 0;
    cpu.n = (r & msb) != 0; cpu.z = r == 0;
}
static inline uint32_t sem_addx(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz), msb = sz_msb(sz);
    s &= m; d &= m;
    uint64_t full = (uint64_t)s + d + cpu.x;
    uint32_t r = (uint32_t)full & m;
    cpu.c = cpu.x = sz == 4 ? (full >> 32) & 1 : (full >> (sz * 8)) & 1;
    cpu.v = ((~(s ^ d) & (r ^ d)) & msb) != 0;
    cpu.n = (r & msb) != 0; if (r) cpu.z = 0;
    return r;
}
static inline uint32_t sem_subx(uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz), msb = sz_msb(sz);
    s &= m; d &= m;
    uint64_t sub = (uint64_t)s + cpu.x;
    uint32_t r = (uint32_t)(d - sub) & m;
    cpu.c = cpu.x = sub > d;
    cpu.v = (((s ^ d) & (r ^ d)) & msb) != 0;
    cpu.n = (r & msb) != 0; if (r) cpu.z = 0;
    return r;
}
static inline uint32_t sem_neg(uint32_t d, int sz) { return sem_sub(d, 0, sz); }
static inline uint32_t sem_negx(uint32_t d, int sz) { return sem_subx(d, 0, sz); }

static inline uint32_t sem_abcd(uint32_t s, uint32_t d)
{
    uint32_t r = (s & 15) + (d & 15) + cpu.x;
    uint32_t v = ~r;
    if (r > 9) r += 6;
    r += (s & 0xf0) + (d & 0xf0);
    cpu.c = cpu.x = r > 0x99;
    if (cpu.c) r -= 0xa0;
    cpu.v = (v & r & 0x80) != 0;
    r &= 0xff;
    cpu.n = (r & 0x80) != 0;
    if (r) cpu.z = 0;
    return r;
}
static inline uint32_t sem_sbcd(uint32_t s, uint32_t d)   /* d - s - x */
{
    uint32_t r = (d & 15) - (s & 15) - cpu.x;
    uint32_t v = ~r;
    if (r > 9) r -= 6;
    r += (d & 0xf0) - (s & 0xf0);
    cpu.c = cpu.x = r > 0x99;
    if (cpu.c) r += 0xa0;
    r &= 0xff;
    cpu.v = (v & r & 0x80) != 0;
    cpu.n = (r & 0x80) != 0;
    if (r) cpu.z = 0;
    return r;
}

/* ---- shifts: return result, set flags, add 2 cycles per bit ---- */
static inline uint32_t sem_shift(int kind, uint32_t v, uint32_t cnt, int sz)
{
    uint32_t m = sz_mask(sz), msb = sz_msb(sz);
    int bits = sz * 8;
    v &= m;
    cnt &= 63;
    cpu.cycles += 2 * cnt;
    cpu.v = 0;
    if (cnt == 0) {
        cpu.c = (kind == OP_ROXL || kind == OP_ROXR) ? cpu.x : 0;
        cpu.n = (v & msb) != 0; cpu.z = v == 0;
        return v;
    }
    uint32_t r = v;
    switch (kind) {
    case OP_ASL: {
        /* V set if the msb changes at any time during the shift */
        uint32_t c = 0; bool ov = false;
        for (uint32_t k = 0; k < cnt; k++) {
            c = (r & msb) != 0;
            r = (r << 1) & m;
            if (((r & msb) != 0) != (bool)c) ov = true;
        }
        cpu.c = cpu.x = (uint8_t)c; cpu.v = ov;
        break;
    }
    case OP_LSL:
        if (cnt > (uint32_t)bits) { r = 0; cpu.c = cpu.x = 0; }
        else { cpu.c = cpu.x = (uint8_t)(((uint64_t)v << (cnt - 1)) >> (bits - 1) & 1); r = (uint32_t)(((uint64_t)v << cnt) & m); }
        break;
    case OP_ASR: {
        int64_t sv = sext(v, sz);
        if (cnt >= (uint32_t)bits) { r = sv < 0 ? m : 0; cpu.c = cpu.x = sv < 0; }
        else { cpu.c = cpu.x = (sv >> (cnt - 1)) & 1; r = (uint32_t)(sv >> cnt) & m; }
        break;
    }
    case OP_LSR:
        if (cnt > (uint32_t)bits) { r = 0; cpu.c = cpu.x = 0; }
        else { cpu.c = cpu.x = (v >> (cnt - 1)) & 1; r = cnt == 32 ? 0 : (v >> cnt) & m; if (cnt == (uint32_t)bits) r = 0; }
        break;
    case OP_ROL: {
        uint32_t k = cnt % bits;
        r = k ? ((v << k) | (v >> (bits - k))) & m : v;
        cpu.c = r & 1;
        break;
    }
    case OP_ROR: {
        uint32_t k = cnt % bits;
        r = k ? ((v >> k) | (v << (bits - k))) & m : v;
        cpu.c = (r & msb) != 0;
        break;
    }
    case OP_ROXL: {
        uint32_t x = cpu.x;
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t out = (r & msb) != 0;
            r = ((r << 1) | x) & m;
            x = out;
        }
        cpu.c = cpu.x = (uint8_t)x;
        break;
    }
    case OP_ROXR: {
        uint32_t x = cpu.x;
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t out = r & 1;
            r = (r >> 1) | (x ? msb : 0);
            x = out;
        }
        cpu.c = cpu.x = (uint8_t)x;
        break;
    }
    }
    cpu.n = (r & msb) != 0; cpu.z = r == 0;
    return r;
}

/* ---- multiply / divide ---- */
static inline int popcount16(uint32_t v)   /* portable (MSVC has no __builtin_popcount) */
{
    v &= 0xffff;
    v = v - ((v >> 1) & 0x5555);
    v = (v & 0x3333) + ((v >> 2) & 0x3333);
    v = (v + (v >> 4)) & 0x0f0f;
    return (int)((v + (v >> 8)) & 0x1f);
}
static inline uint32_t sem_mulu(uint32_t s, uint32_t d)
{
    s &= 0xffff;
    uint32_t r = s * (d & 0xffff);
    int ones = popcount16(s);
    cpu.cycles += 2 * ones;
    cpu.n = r >> 31; cpu.z = r == 0; cpu.v = 0; cpu.c = 0;
    return r;
}
static inline uint32_t sem_muls(uint32_t s, uint32_t d)
{
    int32_t r = (int32_t)(int16_t)s * (int32_t)(int16_t)d;
    uint32_t pat = ((s & 0xffff) << 1) ^ (s & 0xffff);
    cpu.cycles += 2 * popcount16(pat);
    cpu.n = (uint32_t)r >> 31; cpu.z = r == 0; cpu.v = 0; cpu.c = 0;
    return (uint32_t)r;
}
/* return false on divide by zero (caller raises vector 5) */
static inline bool sem_divu(uint32_t s, int dreg)
{
    s &= 0xffff;
    if (!s) return false;
    uint32_t d = cpu.d[dreg];
    uint32_t q = d / s, r = d % s;
    cpu.c = 0;
    if (q > 0xffff) { cpu.v = 1; cpu.n = 1; cpu.z = 0; return true; }
    cpu.d[dreg] = (r << 16) | q;
    cpu.n = (q & 0x8000) != 0; cpu.z = q == 0; cpu.v = 0;
    return true;
}
static inline bool sem_divs(uint32_t s, int dreg)
{
    int32_t sv = (int16_t)s;
    if (!sv) return false;
    int32_t d = (int32_t)cpu.d[dreg];
    cpu.c = 0;
    if (d == INT32_MIN && sv == -1) { cpu.v = 1; cpu.n = 1; cpu.z = 0; return true; }
    int32_t q = d / sv, r = d % sv;
    if (q < -32768 || q > 32767) { cpu.v = 1; cpu.n = 1; cpu.z = 0; return true; }
    cpu.d[dreg] = ((uint32_t)(r & 0xffff) << 16) | (q & 0xffff);
    cpu.n = (q & 0x8000) != 0; cpu.z = (q & 0xffff) == 0; cpu.v = 0;
    return true;
}

/* ---- bit ops: returns the new value; sets Z from the tested bit ---- */
static inline uint32_t sem_bitop(int kind, uint32_t v, uint32_t bit, int sz)
{
    bit &= (sz == 4 ? 31 : 7);
    uint32_t m = 1u << bit;
    cpu.z = (v & m) == 0;
    if (sz == 4 && kind != OP_BTST && bit >= 16) cpu.cycles += 2;
    switch (kind) {
    case OP_BCHG: return v ^ m;
    case OP_BCLR: return v & ~m;
    case OP_BSET: return v | m;
    default: return v;
    }
}

/* ---- stack / supervisor ---- */
static inline void push16(uint32_t v) { cpu.a[7] -= 2; mem_ww(cpu.a[7], v); }
static inline void push32(uint32_t v) { cpu.a[7] -= 4; mem_wl(cpu.a[7], v); }
static inline uint32_t pop16(void) { uint32_t v = mem_rw(cpu.a[7]); cpu.a[7] += 2; return v; }
static inline uint32_t pop32(void) { uint32_t v = mem_rl(cpu.a[7]); cpu.a[7] += 4; return v; }

static inline uint8_t get_ccr(void)
{
    return (uint8_t)(cpu.x << 4 | cpu.n << 3 | cpu.z << 2 | cpu.v << 1 | cpu.c);
}
static inline void set_ccr(uint32_t v)
{
    cpu.x = (v >> 4) & 1; cpu.n = (v >> 3) & 1; cpu.z = (v >> 2) & 1; cpu.v = (v >> 1) & 1; cpu.c = v & 1;
}

/* MOVEM helpers */
static inline uint32_t sem_movem_rm(uint32_t mask, uint32_t addr, int sz, bool predec)
{
    if (predec) {
        /* mask bit 0 = A7 ... bit 15 = D0 */
        for (int k = 0; k < 16; k++) if (mask & (1u << k)) {
            int r = 15 - k;
            uint32_t v = r < 8 ? cpu.d[r] : cpu.a[r - 8];
            addr -= sz; sem_write(addr, v, sz);
            cpu.cycles += sz == 4 ? 8 : 4;
        }
        return addr;
    }
    for (int r = 0; r < 16; r++) if (mask & (1u << r)) {
        uint32_t v = r < 8 ? cpu.d[r] : cpu.a[r - 8];
        sem_write(addr, v, sz); addr += sz;
        cpu.cycles += sz == 4 ? 8 : 4;
    }
    return addr;
}
static inline uint32_t sem_movem_mr(uint32_t mask, uint32_t addr, int sz)
{
    for (int r = 0; r < 16; r++) if (mask & (1u << r)) {
        uint32_t v = sz == 2 ? (uint32_t)(int32_t)(int16_t)mem_rw(addr) : mem_rl(addr);
        if (r < 8) cpu.d[r] = v; else cpu.a[r - 8] = v;
        addr += sz;
        cpu.cycles += sz == 4 ? 8 : 4;
    }
    return addr;
}

/* Hooks the machine provides for privileged/system events */
void m68k_on_reset_instruction(void);

#endif
