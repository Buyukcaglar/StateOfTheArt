/* Two 8520 CIAs. CIA-A (odd bytes, $BFE001) -> INT2/PORTS, CIA-B (even bytes,
 * $BFD000) -> INT6/EXTER. Timers count E-clock ticks (cck/5). */
#include "amiga.h"
#include "../cpu/m68k.h"
#include <string.h>

typedef struct {
    uint8_t pra, prb, ddra, ddrb;
    uint16_t ta, tb, ta_latch, tb_latch;
    uint8_t cra, crb;
    uint8_t icr, imask;
    uint32_t tod, tod_alarm, tod_latch;
    bool tod_latched, tod_halt;
    uint8_t sdr;
} Cia;

static Cia cia[2];
static bool mouse_left, mouse_right;

void amiga_set_mouse_button(bool left, bool right) { mouse_left = left; mouse_right = right; }

static void cia_irq_check(int w)
{
    if (cia[w].icr & cia[w].imask & 0x1f) {
        cia[w].icr |= 0x80;
        cs.intreq |= w ? 0x2000 : 0x0008;
        irq_update();
    }
}

static void cia_set_icr(int w, uint8_t bits)
{
    cia[w].icr |= bits;
    cia_irq_check(w);
}

void cia_reset(void)
{
    memset(cia, 0, sizeof cia);
    for (int w = 0; w < 2; w++) {
        cia[w].ta = cia[w].tb = cia[w].ta_latch = cia[w].tb_latch = 0xffff;
    }
    cia[0].pra = 0x03; cia[0].ddra = 0x03;    /* OVL/LED outputs */
    cia[1].prb = 0xff; cia[1].ddrb = 0xff;    /* drive control outputs, all inactive */
    cia[1].pra = 0xff; cia[1].ddra = 0xc0;
}

uint8_t ciaa_pra_inputs(void)
{
    uint8_t v = 0xff;
    v &= floppy_status_bits() | 0xc3;
    if (mouse_left) v &= ~0x40;
    return v;
}

static void timer_b_count(int w);

void cia_tick_eclock(int n)
{
    while (n-- > 0) {
        for (int w = 0; w < 2; w++) {
            Cia *c = &cia[w];
            if ((c->cra & 0x21) == 0x01) {
                if (c->ta == 0) {
                    c->ta = c->ta_latch;
                    if (c->cra & 0x08) c->cra &= ~1;
                    cia_set_icr(w, 0x01);
                    if ((c->crb & 0x61) == 0x41) timer_b_count(w);
                } else c->ta--;
            }
            if ((c->crb & 0x61) == 0x01) timer_b_count(w);
        }
    }
}

static void timer_b_count(int w)
{
    Cia *c = &cia[w];
    if (c->tb == 0) {
        c->tb = c->tb_latch;
        if (c->crb & 0x08) c->crb &= ~1;
        cia_set_icr(w, 0x02);
    } else c->tb--;
}

void cia_tod_tick(int w)
{
    Cia *c = &cia[w];
    if (c->tod_halt) return;
    c->tod = (c->tod + 1) & 0xffffff;
    if (c->tod == c->tod_alarm) cia_set_icr(w, 0x04);
}

void cia_flag(int w) { cia_set_icr(w, 0x10); }

/* lets INTREQ re-latch while a CIA still holds its interrupt line */
void cia_reassert(void)
{
    for (int w = 0; w < 2; w++)
        if (cia[w].icr & 0x80) cs.intreq |= w ? 0x2000 : 0x0008;
}

uint8_t cia_read(int w, int reg)
{
    Cia *c = &cia[w];
    switch (reg) {
    case 0x0:
        if (w == 0) return (uint8_t)((c->pra & c->ddra) | (ciaa_pra_inputs() & ~c->ddra));
        return (uint8_t)((c->pra & c->ddra) | (0xff & ~c->ddra));
    case 0x1: return (uint8_t)((c->prb & c->ddrb) | (0xff & ~c->ddrb));
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return c->ta & 0xff;
    case 0x5: return c->ta >> 8;
    case 0x6: return c->tb & 0xff;
    case 0x7: return c->tb >> 8;
    case 0x8: {
        uint32_t t = c->tod_latched ? c->tod_latch : c->tod;
        c->tod_latched = false;
        return t & 0xff;
    }
    case 0x9: return ((c->tod_latched ? c->tod_latch : c->tod) >> 8) & 0xff;
    case 0xa:
        c->tod_latch = c->tod; c->tod_latched = true;
        return (c->tod_latch >> 16) & 0xff;
    case 0xc: return c->sdr;
    case 0xd: {
        uint8_t v = c->icr;
        c->icr = 0;
        return v;
    }
    case 0xe: return c->cra & ~0x10;
    case 0xf: return c->crb & ~0x10;
    default: return 0xff;
    }
}

void cia_write(int w, int reg, uint8_t v)
{
    Cia *c = &cia[w];
    switch (reg) {
    case 0x0: c->pra = v; break;
    case 0x1: {
        uint8_t old = (uint8_t)((c->prb & c->ddrb) | ~c->ddrb);
        c->prb = v;
        if (w == 1) floppy_ciab_prb_write((uint8_t)((c->prb & c->ddrb) | ~c->ddrb), old);
        break;
    }
    case 0x2: c->ddra = v; break;
    case 0x3: {
        uint8_t old = (uint8_t)((c->prb & c->ddrb) | ~c->ddrb);
        c->ddrb = v;
        if (w == 1) floppy_ciab_prb_write((uint8_t)((c->prb & c->ddrb) | ~c->ddrb), old);
        break;
    }
    case 0x4: c->ta_latch = (c->ta_latch & 0xff00) | v; break;
    case 0x5:
        c->ta_latch = (uint16_t)((c->ta_latch & 0x00ff) | v << 8);
        if (!(c->cra & 1)) c->ta = c->ta_latch;
        if (c->cra & 0x08) { c->ta = c->ta_latch; c->cra |= 1; }
        break;
    case 0x6: c->tb_latch = (c->tb_latch & 0xff00) | v; break;
    case 0x7:
        c->tb_latch = (uint16_t)((c->tb_latch & 0x00ff) | v << 8);
        if (!(c->crb & 1)) c->tb = c->tb_latch;
        if (c->crb & 0x08) { c->tb = c->tb_latch; c->crb |= 1; }
        break;
    case 0x8:
        if (c->crb & 0x80) c->tod_alarm = (c->tod_alarm & 0xffff00) | v;
        else { c->tod = (c->tod & 0xffff00) | v; c->tod_halt = false; }
        break;
    case 0x9:
        if (c->crb & 0x80) c->tod_alarm = (c->tod_alarm & 0xff00ff) | (uint32_t)v << 8;
        else c->tod = (c->tod & 0xff00ff) | (uint32_t)v << 8;
        break;
    case 0xa:
        if (c->crb & 0x80) c->tod_alarm = (c->tod_alarm & 0x00ffff) | (uint32_t)v << 16;
        else { c->tod = (c->tod & 0x00ffff) | (uint32_t)v << 16; c->tod_halt = true; }
        break;
    case 0xc: c->sdr = v; break;
    case 0xd:
        if (v & 0x80) c->imask |= v & 0x1f; else c->imask &= ~(v & 0x1f);
        cia_irq_check(w);
        break;
    case 0xe:
        if (v & 0x10) c->ta = c->ta_latch;
        c->cra = v & ~0x10;
        break;
    case 0xf:
        if (v & 0x10) c->tb = c->tb_latch;
        c->crb = v & ~0x10;
        break;
    }
}

bool cia_led_on(void) { return !(((cia[0].pra & cia[0].ddra) | ~cia[0].ddra) & 0x02); }
