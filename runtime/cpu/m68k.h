/* 68000 core shared by the interpreter and statically recompiled code.
 *
 * One decoder (m68k_decode.c) produces an Insn; the interpreter executes it
 * with the sem_* helpers in m68k_sem.h, and the lifter emits C that calls the
 * very same helpers with constant operands. Semantics live in one place. */
#ifndef SOTA_M68K_H
#define SOTA_M68K_H

#include <stdint.h>
#include <stdbool.h>

typedef struct M68K {
    uint32_t d[8];
    uint32_t a[8];          /* a[7] is the active stack pointer */
    uint32_t pc;
    uint32_t usp, ssp;      /* the inactive one is kept here */
    uint8_t  s, t, ipl_mask;/* supervisor, trace, interrupt mask */
    uint8_t  x, n, z, v, c; /* condition codes, 0/1 */
    uint8_t  stopped;
    uint8_t  pending_ipl;   /* level presented by Paula, 0..7 */
    uint64_t cycles;        /* CPU clocks consumed since reset */
} M68K;

extern M68K cpu;

/* Memory interface implemented by the machine. All big-endian. */
uint32_t mem_rb(uint32_t a);
uint32_t mem_rw(uint32_t a);
uint32_t mem_rl(uint32_t a);
void     mem_wb(uint32_t a, uint32_t v);
void     mem_ww(uint32_t a, uint32_t v);
void     mem_wl(uint32_t a, uint32_t v);
/* Instruction-stream fetch (no side effects, used by the decoder). */
uint32_t mem_fetch16(uint32_t a);

/* ---- decoded instruction ---- */
enum {
    EA_NONE, EA_DREG, EA_AREG, EA_AIND, EA_POSTINC, EA_PREDEC, EA_ADISP,
    EA_AIDX, EA_ABSW, EA_ABSL, EA_PCDISP, EA_PCIDX, EA_IMM,
    EA_SR, EA_CCR, EA_USP
};

typedef struct EA {
    uint8_t  mode, reg;
    uint8_t  xreg;     /* index register 0..15 (8+ = An) */
    uint8_t  xlong;    /* index size .l */
    int32_t  disp;     /* displacement / short abs */
    uint32_t val;      /* absolute address, immediate, or PC-relative target base */
} EA;

enum {
    OP_ILLEGAL, OP_LINEA, OP_LINEF,
    OP_ORI, OP_ANDI, OP_SUBI, OP_ADDI, OP_EORI, OP_CMPI,
    OP_ORI_CCR, OP_ORI_SR, OP_ANDI_CCR, OP_ANDI_SR, OP_EORI_CCR, OP_EORI_SR,
    OP_BTST, OP_BCHG, OP_BCLR, OP_BSET, OP_MOVEP_RM, OP_MOVEP_MR,
    OP_MOVE, OP_MOVEA,
    OP_NEGX, OP_CLR, OP_NEG, OP_NOT, OP_TST, OP_TAS, OP_NBCD,
    OP_MOVE_FROM_SR, OP_MOVE_TO_CCR, OP_MOVE_TO_SR,
    OP_EXT, OP_SWAP, OP_PEA, OP_LEA, OP_MOVEM_RM, OP_MOVEM_MR,
    OP_TRAP, OP_LINK, OP_UNLK, OP_MOVE_USP_TO, OP_MOVE_USP_FROM,
    OP_RESET, OP_NOP, OP_STOP, OP_RTE, OP_RTS, OP_TRAPV, OP_RTR,
    OP_JSR, OP_JMP, OP_CHK,
    OP_ADDQ, OP_SUBQ, OP_SCC, OP_DBCC,
    OP_BRA, OP_BSR, OP_BCC,
    OP_MOVEQ,
    OP_OR, OP_DIVU, OP_DIVS, OP_SBCD,
    OP_SUB, OP_SUBA, OP_SUBX,
    OP_CMP, OP_CMPA, OP_CMPM, OP_EOR,
    OP_AND, OP_MULU, OP_MULS, OP_ABCD, OP_EXG,
    OP_ADD, OP_ADDA, OP_ADDX,
    OP_ASL, OP_ASR, OP_LSL, OP_LSR, OP_ROXL, OP_ROXR, OP_ROL, OP_ROR,
    OP_COUNT
};

/* Insn.flow */
enum { FLOW_NONE = 0, FLOW_BRANCH, FLOW_COND, FLOW_CALL, FLOW_RET,
       FLOW_JUMP_IND, FLOW_CALL_IND, FLOW_TRAP, FLOW_STOP };

typedef struct Insn {
    uint32_t pc;
    uint16_t op;          /* opcode word */
    uint8_t  len;         /* bytes */
    uint8_t  kind;        /* OP_* */
    uint8_t  size;        /* 1, 2, 4 (0 = n/a) */
    uint8_t  cond;        /* Bcc/DBcc/Scc condition */
    uint8_t  flow;        /* FLOW_* */
    uint8_t  regmem;      /* shift: 0 = count in imm, 1 = count in Dn(imm), 2 = memory form */
    EA       src, dst;
    uint32_t imm;         /* immediate / quick value / movem mask / trap vec */
    uint32_t target;      /* static branch target */
    uint16_t cycles;      /* static cycle cost (taken-branch cost for branches) */
    uint8_t  ext[10];     /* raw words beyond the first (for code-identity checks) */
} Insn;

int  m68k_decode(uint32_t pc, Insn *out);       /* returns length in bytes */
bool m68k_ends_block(const Insn *i);
#define BLOCK_MAX_INSNS 48
void m68k_disasm(const Insn *i, char *buf, int n);
const char *m68k_opname(int kind);

/* interpreter */
void     m68k_reset(uint32_t ssp, uint32_t pc);
void     m68k_execute_insn(const Insn *i);
int      m68k_step(void);                          /* executes one instruction (or an exception); returns cycles */
void     m68k_exception(int vector);               /* group 1/2 exception: push frame, vector */
bool     m68k_check_irq(void);                     /* takes a pending interrupt if allowed */
uint16_t m68k_get_sr(void);
void     m68k_set_sr(uint16_t sr);

#endif
