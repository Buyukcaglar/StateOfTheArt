#ifndef SOTA_EXEC_H
#define SOTA_EXEC_H
#include <stdint.h>
#include <stdbool.h>

typedef void (*BlockFn)(void);
typedef struct RecompBlock {
    uint32_t pc;
    uint16_t len;            /* code bytes covered */
    const uint8_t *bytes;    /* original code bytes (identity check) */
    BlockFn fn;
} RecompBlock;

typedef struct {
    uint64_t interp_blocks, interp_insns, recomp_blocks, misses, validations, irqs;
} ExecStats;
extern ExecStats exec_stats;
extern bool exec_use_recomp;

void exec_register_blocks(const RecompBlock *blocks, int count);
void exec_block(void);
void exec_run_frames(int frames);
void exec_coverage_enable(bool on);
int  exec_coverage_write(const char *path);
int  exec_coverage_read(const char *path);
#include <stdio.h>
extern FILE *exec_trace;
extern uint64_t exec_trace_from, exec_trace_to;
#endif
