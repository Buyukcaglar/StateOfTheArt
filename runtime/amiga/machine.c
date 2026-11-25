/* Machine reset and high-level boot. No Kickstart is used: the bootblock is
 * placed in chip RAM and entered the way Kickstart's strap would, with the
 * hardware in the state trackdisk.device leaves it in (DSKEN on, motor off,
 * head on cylinder 0, interrupts off). */
#include "amiga.h"
#include "mem_gen.h"
#include "../cpu/m68k.h"
#include <string.h>

void cia_reassert(void);
const uint8_t *amiga_adf_data(void);

#define BOOT_BUFFER 0x5000u

void m68k_on_reset_instruction(void)
{
    amiga_log("[cpu] RESET instruction at $%06x\n", cpu.pc);
}

void amiga_init(void)
{
    memset(chip_ram, 0, sizeof chip_ram);
    memset(slow_ram, 0, sizeof slow_ram);
    /* advance (never rewind) page generations: recompiled blocks validated before a
     * reset must not look valid against the new memory contents */
    for (int k = 0; k < PAGE_GEN_COUNT; k++) page_gen[k] += 0x10000;
    custom_reset();
    cia_reset();
    floppy_reset();
    audio_reset();
}

void amiga_boot_hle(void)
{
    const uint8_t *adf = amiga_adf_data();
    if (adf) memcpy(chip_ram + BOOT_BUFFER, adf, 1024);
    cs.dmacon = 0x0210;          /* DMAEN | DSKEN left on by trackdisk */
    cs.intena = 0;
    cs.diwstrt = 0x2c81; cs.diwstop = 0xf4c1;
    cs.ddfstrt = 0x38; cs.ddfstop = 0xd0;
    cs.color[0] = 0x000;
    m68k_reset(0x7ff00, BOOT_BUFFER + 12);
    m68k_set_sr(0x2000);         /* supervisor, IPL 0 */
    cpu.a[1] = 0; cpu.a[6] = 0;
}
