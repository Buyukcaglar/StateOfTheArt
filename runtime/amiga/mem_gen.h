/* Page write generations: chip RAM pages first, then slow RAM pages. */
#ifndef SOTA_MEM_GEN_H
#define SOTA_MEM_GEN_H
#include <stdint.h>
#include "amiga.h"

#define PAGE_SHIFT 8
#define CHIP_PAGES (CHIP_SIZE >> PAGE_SHIFT)
#define PAGE_GEN_COUNT ((int)((CHIP_SIZE + SLOW_SIZE) >> PAGE_SHIFT))
extern uint32_t page_gen[PAGE_GEN_COUNT];

/* page index for a CPU address (chip mirrors fold), or -1 if not RAM */
static inline int page_index(uint32_t a)
{
    a &= 0xffffff;
    if (a < 0x200000) return (int)((a & (CHIP_SIZE - 1)) >> PAGE_SHIFT);
    if (a - SLOW_BASE < SLOW_SIZE) return (int)(CHIP_PAGES + ((a - SLOW_BASE) >> PAGE_SHIFT));
    return -1;
}
static inline void page_touch(uint32_t a)
{
    int p = page_index(a);
    if (p >= 0) page_gen[p]++;
}

/* Sum of the write generations of the three pages a block starting at `pc` can
 * span (blocks are < 512 bytes). Any RAM write there changes it. Both the
 * interpreter and recompiled code end a block as soon as it changes, which
 * makes self-modifying code behave identically in both modes. */
static inline uint32_t code_gen_sum(uint32_t pc)
{
    int p = page_index(pc);
    if (p < 0) return 0;
    uint32_t s = page_gen[p];
    if (p + 1 < PAGE_GEN_COUNT) s += page_gen[p + 1];
    if (p + 2 < PAGE_GEN_COUNT) s += page_gen[p + 2];
    return s;
}

uint16_t chip_rw(uint32_t a);
void chip_ww(uint32_t a, uint16_t v);
#endif
