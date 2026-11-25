/* Block executor shared by every mode.
 *
 * Code runs in basic blocks (m68k_ends_block). Interrupts are taken only at
 * block boundaries, in the interpreter as in recompiled code, so both modes
 * see I/O and interrupts at identical CPU times and produce identical frames.
 * A recompiled block is used only while its original code bytes are still in
 * memory (checked by page generation, then memcmp). */
#include "exec.h"
#include "../amiga/amiga.h"
#include "../amiga/mem_gen.h"
#include "../cpu/m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ExecStats exec_stats;
bool exec_use_recomp = true;

/* ---- recompiled block registry ---- */
typedef struct Variant {
    const RecompBlock *b;
    uint32_t gen_first, gen_last;   /* page generations when last validated */
    int page_first, page_last;
    bool valid_cached;
    struct Variant *next;
} Variant;

#define HASH_BITS 16
static Variant *table[1 << HASH_BITS];
static inline unsigned hpc(uint32_t pc) { return (pc * 2654435761u) >> (32 - HASH_BITS); }

void exec_register_blocks(const RecompBlock *blocks, int count)
{
    for (int k = 0; k < count; k++) {
        Variant *v = calloc(1, sizeof *v);
        v->b = &blocks[k];
        v->page_first = page_index(blocks[k].pc);
        v->page_last = page_index(blocks[k].pc + blocks[k].len - 1);
        unsigned h = hpc(blocks[k].pc);
        v->next = table[h];
        table[h] = v;
    }
}

static bool code_matches(const RecompBlock *b)
{
    for (uint32_t k = 0; k < b->len; k += 2) {
        if (mem_fetch16(b->pc + k) != (uint32_t)(b->bytes[k] << 8 | b->bytes[k + 1])) return false;
    }
    return true;
}

static Variant *lookup(uint32_t pc)
{
    for (Variant *v = table[hpc(pc)]; v; v = v->next) {
        if (v->b->pc != pc || v->page_first < 0 || v->page_last < 0) continue;
        if (v->valid_cached && page_gen[v->page_first] == v->gen_first && page_gen[v->page_last] == v->gen_last)
            return v;
        v->valid_cached = false;
        exec_stats.validations++;
        if (code_matches(v->b)) {
            v->valid_cached = true;
            v->gen_first = page_gen[v->page_first];
            v->gen_last = page_gen[v->page_last];
            return v;
        }
    }
    return 0;
}

/* ---- coverage ---- */
typedef struct CovEntry { uint32_t pc; uint16_t len; uint8_t *bytes; struct CovEntry *next; } CovEntry;
static CovEntry *cov[1 << HASH_BITS];
static bool cov_on;
static unsigned cov_count;

void exec_coverage_enable(bool on) { cov_on = on; }

static void cov_record(uint32_t pc, const uint8_t *bytes, uint16_t len)
{
    unsigned h = hpc(pc);
    for (CovEntry *e = cov[h]; e; e = e->next)
        if (e->pc == pc && e->len == len && !memcmp(e->bytes, bytes, len)) return;
    CovEntry *e = malloc(sizeof *e);
    e->pc = pc; e->len = len;
    e->bytes = malloc(len); memcpy(e->bytes, bytes, len);
    e->next = cov[h]; cov[h] = e;
    cov_count++;
}

int exec_coverage_write(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (unsigned h = 0; h < (1u << HASH_BITS); h++)
        for (CovEntry *e = cov[h]; e; e = e->next) {
            fprintf(f, "%06x ", e->pc);
            for (int k = 0; k < e->len; k++) fprintf(f, "%02x", e->bytes[k]);
            fputc('\n', f);
        }
    fclose(f);
    return (int)cov_count;
}

/* load existing coverage so repeated runs accumulate */
int exec_coverage_read(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[8192]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned pc; char hex[8000];
        if (sscanf(line, "%x %7999s", &pc, hex) != 2) continue;
        size_t L = strlen(hex) / 2;
        uint8_t buf[4000];
        if (L > sizeof buf) continue;
        for (size_t k = 0; k < L; k++) { unsigned b; sscanf(hex + 2 * k, "%2x", &b); buf[k] = (uint8_t)b; }
        cov_record(pc, buf, (uint16_t)L);
        n++;
    }
    fclose(f);
    return n;
}

/* ---- execution ---- */
static void interpret_block(void)
{
    uint32_t pc0 = cpu.pc;
    uint8_t bytes[BLOCK_MAX_INSNS * 10];
    uint16_t len = 0;
    const uint32_t gen0 = code_gen_sum(pc0);
    for (int n = 0; n < BLOCK_MAX_INSNS; n++) {
        Insn i;
        m68k_decode(cpu.pc, &i);
        if (cov_on) for (int k = 0; k < i.len; k += 2) {
            uint32_t w = mem_fetch16(i.pc + k);
            bytes[len++] = (uint8_t)(w >> 8); bytes[len++] = (uint8_t)w;
        }
        uint32_t next = cpu.pc + i.len;
        cpu.pc = next;
        m68k_execute_insn(&i);
        exec_stats.interp_insns++;
        if (m68k_ends_block(&i) || cpu.pc != next) break;
        if (code_gen_sum(pc0) != gen0) break;   /* code may have been modified: re-dispatch */
    }
    if (cov_on) cov_record(pc0, bytes, len);
    exec_stats.interp_blocks++;
}

FILE *exec_trace;
uint64_t exec_trace_from, exec_trace_to;
static void trace_state(void)
{
    if (!exec_trace || cs.frame < exec_trace_from || cs.frame > exec_trace_to) return;
    fprintf(exec_trace, "%06x c=%llu sr=%04x", cpu.pc, (unsigned long long)cpu.cycles, m68k_get_sr());
    for (int r = 0; r < 8; r++) fprintf(exec_trace, " %08x", cpu.d[r]);
    for (int r = 0; r < 8; r++) fprintf(exec_trace, " %08x", cpu.a[r]);
    fputc('\n', exec_trace);
}

void exec_block(void)
{
    trace_state();
    if (m68k_check_irq()) { exec_stats.irqs++; chip_sync(cpu.cycles); return; }
    if (cpu.stopped) {
        /* idle until an interrupt can be taken */
        int guard = 0;
        do { cpu.cycles += 4; chip_sync(cpu.cycles); }
        while (!(cpu.pending_ipl > cpu.ipl_mask || cpu.pending_ipl == 7) && ++guard < 100000);
        return;
    }
    if (exec_use_recomp) {
        Variant *v = lookup(cpu.pc);
        if (v) {
            v->b->fn();
            exec_stats.recomp_blocks++;
            chip_sync(cpu.cycles);
            return;
        }
        exec_stats.misses++;
    }
    interpret_block();
    chip_sync(cpu.cycles);
}

/* run until the chipset completes `frames` more frames */
void exec_run_frames(int frames)
{
    uint64_t target = cs.frame + (uint64_t)frames;
    while (cs.frame < target) exec_block();
}
