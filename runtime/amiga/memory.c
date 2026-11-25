/* A500 memory map: chip RAM (mirrored to $1FFFFF), CIAs, slow RAM at $C00000,
 * custom chips at $DFF000. Every write to RAM bumps a 256-byte page generation
 * so recompiled blocks can cheaply tell whether their code bytes changed. */
#include "amiga.h"
#include "mem_gen.h"
#include "../cpu/m68k.h"
#include <stdio.h>
#include <stdarg.h>

uint8_t chip_ram[CHIP_SIZE];
uint8_t slow_ram[SLOW_SIZE];
uint32_t page_gen[PAGE_GEN_COUNT];
int amiga_log_level = 1;

void amiga_log(const char *fmt, ...)
{
    if (!amiga_log_level) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static unsigned unmapped_reports;
static void unmapped(const char *what, uint32_t a)
{
    if (unmapped_reports < 32) {
        unmapped_reports++;
        amiga_log("[mem] %s unmapped $%06x pc=$%06x\n", what, a, cpu.pc);
    }
}

/* IO access: bring the chipset up to the current CPU time first */
static inline void io_sync(void) { chip_sync(cpu.cycles); }

static uint32_t io_read16(uint32_t a)
{
    if ((a & 0xfff000) == 0xdff000 || (a >= 0xc80000 && a < 0xe00000 && (a & 0x1ff000) == 0x1ff000)) {
        io_sync();
        return custom_read(a & 0x1fe);
    }
    if ((a & 0xff0000) == 0xbf0000) {
        io_sync();
        /* word read: high byte from CIA-B (even), low byte from CIA-A (odd) */
        int reg = (a >> 8) & 15;
        uint8_t hi = (a & 0x2000) ? 0xff : cia_read(1, reg);
        uint8_t lo = (a & 0x1000) ? 0xff : cia_read(0, reg);
        cpu.cycles += 6;  /* average E-clock synchronisation */
        return (uint32_t)hi << 8 | lo;
    }
    if (a >= 0xf80000) { unmapped("rom read", a); return 0; }
    unmapped("read", a);
    return 0;
}

static void io_write16(uint32_t a, uint32_t v, bool byte)
{
    if ((a & 0xfff000) == 0xdff000) {
        io_sync();
        if (byte) v = (v & 0xff) | (v & 0xff) << 8;
        custom_write(a & 0x1fe, (uint16_t)v);
        return;
    }
    if ((a & 0xff0000) == 0xbf0000) {
        io_sync();
        int reg = (a >> 8) & 15;
        cpu.cycles += 6;
        if (byte) {
            if (a & 1) { if (!(a & 0x1000)) cia_write(0, reg, (uint8_t)v); }
            else       { if (!(a & 0x2000)) cia_write(1, reg, (uint8_t)v); }
        } else {
            if (!(a & 0x2000)) cia_write(1, reg, (uint8_t)(v >> 8));
            if (!(a & 0x1000)) cia_write(0, reg, (uint8_t)v);
        }
        return;
    }
    unmapped("write", a);
}

static inline uint8_t *ram_ptr(uint32_t a)
{
    a &= 0xffffff;
    if (a < 0x200000) return chip_ram + (a & (CHIP_SIZE - 1));
    if (a - SLOW_BASE < SLOW_SIZE) return slow_ram + (a - SLOW_BASE);
    return 0;
}

uint32_t mem_fetch16(uint32_t a)
{
    uint8_t *p = ram_ptr(a);
    return p ? (uint32_t)(p[0] << 8 | p[1]) : 0;
}

uint32_t mem_rb(uint32_t a)
{
    uint8_t *p = ram_ptr(a);
    if (p) return *p;
    a &= 0xffffff;
    if ((a & 0xff0000) == 0xbf0000) {
        io_sync();
        cpu.cycles += 6;
        int reg = (a >> 8) & 15;
        if (a & 1) return (a & 0x1000) ? 0xff : cia_read(0, reg);
        return (a & 0x2000) ? 0xff : cia_read(1, reg);
    }
    uint32_t w = io_read16(a & ~1u);
    return (a & 1) ? (w & 0xff) : (w >> 8);
}

uint32_t mem_rw(uint32_t a)
{
    uint8_t *p = ram_ptr(a);
    if (p) return (uint32_t)(p[0] << 8 | p[1]);
    return io_read16(a & 0xfffffe);
}

uint32_t mem_rl(uint32_t a)
{
    uint8_t *p = ram_ptr(a);
    if (p && ((a & 0xffffff) & 0xffff) <= 0xfffc)
        return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    return mem_rw(a) << 16 | mem_rw(a + 2);
}

void mem_wb(uint32_t a, uint32_t v)
{
    uint8_t *p = ram_ptr(a);
    if (p) { *p = (uint8_t)v; page_touch(a); return; }
    io_write16(a & 0xffffff, v & 0xff, true);
}

void mem_ww(uint32_t a, uint32_t v)
{
    uint8_t *p = ram_ptr(a);
    if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; page_touch(a); return; }
    io_write16(a & 0xfffffe, v & 0xffff, false);
}

void mem_wl(uint32_t a, uint32_t v)
{
    uint8_t *p = ram_ptr(a);
    if (p && ((a & 0xffffff) & 0xffff) <= 0xfffc) {
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
        page_touch(a); page_touch(a + 3);
        return;
    }
    mem_ww(a, v >> 16);
    mem_ww(a + 2, v & 0xffff);
}

/* chip-bus DMA helpers (blitter, disk, copper, bitplanes) */
uint16_t chip_rw(uint32_t a)
{
    a &= (CHIP_SIZE - 1) & ~1u;
    return (uint16_t)(chip_ram[a] << 8 | chip_ram[a + 1]);
}
void chip_ww(uint32_t a, uint16_t v)
{
    a &= (CHIP_SIZE - 1) & ~1u;
    chip_ram[a] = (uint8_t)(v >> 8); chip_ram[a + 1] = (uint8_t)v;
    page_gen[a >> PAGE_SHIFT]++;
}
