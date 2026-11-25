/* Agnus/Denise/Paula register file, beam, DMA slots, Copper and display.
 *
 * Time base is the colour clock (cck, 3.546895 MHz). Each cck is stepped in
 * order: DMA slot use (bitplanes, sprites, copper), then Denise output of
 * four hires pixels. A lores bitplane fetch unit that starts at cck X is
 * loaded into the shifters at lores pixel 2X+17 (the "$38 -> $81" relation of
 * a standard display); sprites and the display window use the same lores
 * pixel coordinates. */
#include "amiga.h"
#include "mem_gen.h"
#include "../cpu/m68k.h"
#include <string.h>

void cia_reassert(void);

Custom cs;
uint32_t framebuffer[FB_H][FB_W];
static uint32_t backbuffer[FB_H][FB_W];
static FrameCallback frame_cb;
void amiga_set_frame_callback(FrameCallback cb) { frame_cb = cb; }

static uint32_t rgb_lut[4096];
static uint32_t rgb_half_lut[4096];

/* ---- interrupts ---- */
void irq_update(void)
{
    int lvl = 0;
    uint16_t act = cs.intena & cs.intreq & 0x3fff;
    if ((cs.intena & 0x4000) && act) {
        if (act & 0x2000) lvl = 6;
        else if (act & 0x1800) lvl = 5;
        else if (act & 0x0780) lvl = 4;
        else if (act & 0x0070) lvl = 3;
        else if (act & 0x0008) lvl = 2;
        else if (act & 0x0007) lvl = 1;
    }
    cpu.pending_ipl = (uint8_t)lvl;
}
static inline void intreq_set(uint16_t bits) { cs.intreq |= bits; irq_update(); }

/* ---- copper ---- */
enum { COP_STOP, COP_READ1, COP_READ2, COP_WAIT, COP_WAKE };

static inline bool cop_dma(void) { return (cs.dmacon & 0x0280) == 0x0280; }
static inline bool blt_dma(void) { return (cs.dmacon & 0x0240) == 0x0240; }
static inline bool blitter_busy(void) { return cs.cck < cs.blit_busy_until; }

static bool cop_compare(void)
{
    uint16_t ve = ((cs.cop_ir2 >> 8) & 0x7f) | 0x80, he = cs.cop_ir2 & 0xfe;
    uint16_t vp = (cs.cop_ir1 >> 8) & ve, hp = cs.cop_ir1 & 0xfe & he;
    uint16_t cv = (uint16_t)(cs.vpos & 0xff) & ve, ch = (uint16_t)(cs.hpos & 0xfe) & he;
    bool beam = cv > vp || (cv == vp && ch >= hp);
    if (!beam) return false;
    if (!(cs.cop_ir2 & 0x8000) && blitter_busy()) return false;
    return true;
}

static void cop_jump(uint32_t lc)
{
    cs.coppc = lc;
    cs.cop_state = COP_READ1;
}

/* executes one copper slot (an even cck not taken by bitplane DMA) */
static void copper_slot(void)
{
    switch (cs.cop_state) {
    case COP_READ1:
        cs.cop_ir1 = chip_rw(cs.coppc); cs.coppc += 2;
        cs.cop_state = COP_READ2;
        break;
    case COP_READ2:
        cs.cop_ir2 = chip_rw(cs.coppc); cs.coppc += 2;
        if (!(cs.cop_ir1 & 1)) {
            uint16_t reg = cs.cop_ir1 & 0x1fe;
            if (reg < 0x40 || (reg < 0x80 && !(cs.copcon & 2))) { cs.cop_state = COP_STOP; break; }
            cs.cop_state = COP_READ1;
            custom_write(reg, cs.cop_ir2);
        } else if (!(cs.cop_ir2 & 1)) {
            cs.cop_state = cop_compare() ? COP_READ1 : COP_WAIT;
        } else {
            if (cop_compare()) cs.coppc += 4;
            cs.cop_state = COP_READ1;
        }
        break;
    default: break;
    }
}

/* ---- display state ---- */
/* Denise runs DENISE_DS colour clocks behind Agnus: its line covers lores
 * pixels 2*DS .. 2*DS+454, so overscan data fetched at DDFSTOP $D8 (shown up
 * to lores $1D0) stays on the line it belongs to. Framebuffer x (hires) =
 * 2 * (lores - 2*DS). */
#define DENISE_DS 8
static inline int denise_pos(void) { return ((cs.hpos - DENISE_DS + LINE_CCK) % LINE_CCK) * 4; }
static int  line_bpl_on;          /* bitplane DMA allowed this line */
static int  fetch_active, fetch_unit_pos, fetch_last_unit, fetch_started;
static uint16_t shifter[6];
static uint16_t pend[6];
static int  pend_pos = -1;        /* hires pixel position of pending parallel load */
static uint8_t raw_odd[FB_W + 64], raw_even[FB_W + 64];   /* pre-delay plane bits */
static int  ham_prev;
/* sprites */
typedef struct {
    int state;     /* 0 idle, 1 waiting for vstart, 2 active */
    int vstart, vstop;
    bool armed;
    uint16_t sa, sb; int left;   /* shift registers and remaining lores pixels */
    int hpos_px;
} Sprite;
static Sprite spr[8];

static inline int bpu(void)
{
    int n = (cs.bplcon0 >> 12) & 7;
    return n > 6 ? 4 : n;
}
static inline bool hires(void) { return cs.bplcon0 & 0x8000; }

static void sprite_compute(int n)
{
    uint16_t pos = cs.sprpos[n], ctl = cs.sprctl[n];
    spr[n].vstart = (pos >> 8) | ((ctl & 4) << 6);
    spr[n].vstop  = (ctl >> 8) | ((ctl & 2) << 7);
    spr[n].hpos_px = (((pos & 0xff) << 1) | (ctl & 1)) + 1;
}

/* sprite DMA: two slots per sprite at cck $15+4n and $17+4n */
static void sprite_dma(int n, int second)
{
    if ((cs.dmacon & 0x0220) != 0x0220) return;
    int line = cs.vpos;
    if (line < 0x19 || line >= PAL_LINES - 1) return;
    Sprite *s = &spr[n];
    uint32_t *pt = &cs.sprpt[n];
    bool ctl = line == 0x19;
    if (!ctl) {
        if (s->state == 1 && line == s->vstart) s->state = 2;
        if (s->state != 2) return;
        ctl = line == s->vstop;
    }
    uint16_t w = chip_rw(*pt); *pt += 2;
    if (ctl) {
        if (!second) cs.sprpos[n] = w;
        else { cs.sprctl[n] = w; s->armed = false; sprite_compute(n); s->state = 1; }
    } else {
        if (!second) { cs.sprdata[n] = w; s->armed = true; }
        else cs.sprdatb[n] = w;
    }
}

/* bitplane fetch slot tables: plane index (0-based) per cck offset in an 8-cck unit, -1 none */
static const int8_t lores_slot[8] = { -1, 3, 5, 1, -1, 2, 4, 0 };
static const int8_t hires_slot[8] = { 3, 1, 2, 0, 3, 1, 2, 0 };

static void bpl_fetch(int plane)
{
    cs.bpldat[plane] = chip_rw(cs.bplpt[plane]);
    cs.bplpt[plane] += 2;
}

static void denise_parallel_load(void)
{
    /* plane 1 data written: shifters load 3 lores pixels later (hires units: +6) */
    memcpy(pend, cs.bpldat, sizeof pend);
    pend_pos = denise_pos() + 6;
}

/* returns true if this cck's slot is used by bitplane DMA */
static bool bitplane_dma_cck(void)
{
    int h = cs.hpos;
    uint16_t ddfstrt = cs.ddfstrt & 0xfc, ddfstop = cs.ddfstop & 0xfc;
    if (ddfstrt < 0x18) ddfstrt = 0x18;
    if (ddfstop > 0xd8) ddfstop = 0xd8;
    if (!fetch_active) {
        if (line_bpl_on && h == ddfstrt && !fetch_started && (cs.dmacon & 0x0300) == 0x0300 && bpu() > 0) {
            fetch_active = 1; fetch_unit_pos = 0; fetch_last_unit = 0; fetch_started = 1;
        } else return false;
    }
    int off = fetch_unit_pos & 7;
    if (off == 0 && h > ddfstop) fetch_last_unit = 1;     /* unit starts past DDFSTOP: none */
    if (off == 0 && fetch_last_unit) { fetch_active = 0; return false; }
    if (off == 0 && h == ddfstop) fetch_last_unit = 2;    /* this is the last unit */
    bool used = false;
    int plane = hires() ? hires_slot[off] : lores_slot[off];
    if (plane >= 0 && plane < bpu() && (cs.dmacon & 0x0300) == 0x0300) {
        bpl_fetch(plane);
        used = true;
        if (plane == 0) denise_parallel_load();
    }
    fetch_unit_pos++;
    if ((fetch_unit_pos & 7) == 0) {
        if (fetch_last_unit == 2 || h + 1 > ddfstop) {
            /* end of the line's fetch: apply modulos */
            int n = bpu();
            for (int p = 0; p < n; p++) cs.bplpt[p] += (uint32_t)(int32_t)((p & 1) ? cs.bpl2mod : cs.bpl1mod);
            fetch_active = 0;
            fetch_last_unit = 1;
        }
    }
    return used;
}

/* ---- Denise pixel output ---- */
static int diw_vstart(void) { return cs.diwstrt >> 8; }
static int diw_vstop(void)  { int v = cs.diwstop >> 8; return v | ((~v & 0x80) << 1); }
static int diw_hstart(void) { return cs.diwstrt & 0xff; }
static int diw_hstop(void)  { return (cs.diwstop & 0xff) | 0x100; }

static uint32_t *outline;

static inline uint8_t shifter_bits(void)
{
    return (uint8_t)(((shifter[0] >> 15) & 1) | (((shifter[1] >> 15) & 1) << 1) | (((shifter[2] >> 15) & 1) << 2) |
                     (((shifter[3] >> 15) & 1) << 3) | (((shifter[4] >> 15) & 1) << 4) | (((shifter[5] >> 15) & 1) << 5));
}

static void denise_cck(void)
{
    int base = denise_pos();
    bool hr = hires();
    int n = bpu();
    uint8_t planemask = (uint8_t)((1u << n) - 1);
    int d1 = cs.bplcon1 & 15, d2 = (cs.bplcon1 >> 4) & 15;
    int vline = cs.vpos;
    bool vwin = vline >= diw_vstart() && vline < diw_vstop();
    int hs = diw_hstart(), he = diw_hstop();
    bool dual = cs.bplcon0 & 0x0400, ham = (cs.bplcon0 & 0x0800) && n == 6 && !hr;
    bool ehb = !dual && !ham && n == 6;
    int pf1p = cs.bplcon2 & 7, pf2p = (cs.bplcon2 >> 3) & 7;
    bool pf2pri = cs.bplcon2 & 0x40;

    for (int k = 0; k < 4; k++) {
        int p = base + k;
        if (p == pend_pos) { memcpy(shifter, pend, sizeof shifter); pend_pos = -1; }
        uint8_t b = shifter_bits();
        raw_odd[p] = b & 0x15; raw_even[p] = b & 0x2a;
        if (hr || (k & 1)) for (int q = 0; q < 6; q++) shifter[q] <<= 1;

        /* delayed plane value: delay counts lores pixels (2 hires) */
        int po = p - 2 * d1, pe = p - 2 * d2;
        uint8_t v = (uint8_t)((po >= 0 ? raw_odd[po] : 0) | (pe >= 0 ? raw_even[pe] : 0)) & planemask;

        int lx = (p >> 1) + 2 * DENISE_DS;  /* lores pixel coordinate */
        bool inwin = vwin && lx >= hs && lx < he;
        uint32_t px;
        if (!inwin) { px = rgb_lut[cs.color[0] & 0xfff]; outline[p] = px; continue; }

        /* sprites (lores resolution, evaluated once per lores pixel) */
        int sprcol = 0, sprnum = -1;
        for (int s = 0; s < 8; s++) {
            Sprite *sp = &spr[s];
            if (!(k & 1)) {
                if (sp->armed && lx == sp->hpos_px && sp->left == 0) {
                    sp->sa = cs.sprdata[s]; sp->sb = cs.sprdatb[s]; sp->left = 16;
                }
            }
        }
        for (int s = 0; s < 8; s += 2) {
            Sprite *e0 = &spr[s], *e1 = &spr[s + 1];
            int c0 = e0->left ? ((e0->sa >> 15) & 1) | (((e0->sb >> 15) & 1) << 1) : 0;
            int c1 = e1->left ? ((e1->sa >> 15) & 1) | (((e1->sb >> 15) & 1) << 1) : 0;
            if (cs.sprctl[s + 1] & 0x80) { /* attached */
                int c = c0 | (c1 << 2);
                if (c) { sprcol = 16 + c; sprnum = s; break; }
            } else {
                if (c0) { sprcol = 16 + s * 2 + c0; sprnum = s; break; }
                if (c1) { sprcol = 16 + s * 2 + c1; sprnum = s + 1; break; }
            }
        }
        if (k & 1) {
            for (int s = 0; s < 8; s++) if (spr[s].left) { spr[s].sa <<= 1; spr[s].sb <<= 1; spr[s].left--; }
        }

        int col; bool pf_front = false;
        uint32_t rgb = 0; bool rgb_set = false;
        if (dual) {
            int c1v = ((v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4));
            int c2v = (((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4));
            int pf1 = c1v ? c1v : 0, pf2 = c2v ? 8 + c2v : 0;
            int pair = sprnum >= 0 ? sprnum >> 1 : 99;
            /* playfield order */
            int first = pf2pri ? pf2 : pf1, second = pf2pri ? pf1 : pf2;
            int firstp = pf2pri ? pf2p : pf1p, secondp = pf2pri ? pf1p : pf2p;
            if (sprnum >= 0 && pair < firstp) col = sprcol;
            else if (first) col = first;
            else if (sprnum >= 0 && pair < secondp) col = sprcol;
            else if (second) col = second;
            else col = sprnum >= 0 ? sprcol : 0;
            (void)pf_front;
        } else {
            int pair = sprnum >= 0 ? sprnum >> 1 : 99;
            if (sprnum >= 0 && (pair < pf2p || v == 0)) col = sprcol;
            else if (ham) {
                int ctl = v >> 4, d = v & 15;
                if (ctl == 0) ham_prev = cs.color[d] & 0xfff;
                else if (ctl == 1) ham_prev = (ham_prev & 0xff0) | d;
                else if (ctl == 2) ham_prev = (ham_prev & 0x0ff) | d << 8;
                else ham_prev = (ham_prev & 0xf0f) | d << 4;
                rgb = rgb_lut[ham_prev]; rgb_set = true; col = 0;
            } else if (ehb && v >= 32) {
                rgb = rgb_half_lut[cs.color[v - 32] & 0xfff]; rgb_set = true; col = 0;
            } else col = v & 31;
        }
        if (ham && sprnum >= 0 && col >= 16) rgb_set = false;
        px = rgb_set ? rgb : rgb_lut[cs.color[col] & 0xfff];
        outline[p] = px;
    }
}

/* ---- line / frame ---- */
static int denise_vpos;
static void end_frame(void);
static void denise_start_line(void)
{
    if (cs.vpos == 0) end_frame();
    denise_vpos = cs.vpos;
    outline = backbuffer[cs.vpos];
    pend_pos = -1;
    memset(shifter, 0, sizeof shifter);
    memset(raw_odd, 0, sizeof raw_odd); memset(raw_even, 0, sizeof raw_even);
    for (int s = 0; s < 8; s++) spr[s].left = 0;
    ham_prev = cs.color[0] & 0xfff;
}

static void start_line(void)
{
    fetch_active = 0; fetch_started = 0;
    int v = cs.vpos;
    line_bpl_on = v >= 0x1a && v >= diw_vstart() && v < diw_vstop();
    cia_tod_tick(1);
}

static void end_frame(void)
{
    memcpy(framebuffer, backbuffer, sizeof framebuffer);
    cs.frame++;
    if (frame_cb) frame_cb(cs.frame);
}

static void start_frame(void)
{
    /* vertical blank: copper restart, VERTB, CIA-A TOD */
    cop_jump(cs.cop1lc);
    intreq_set(0x0020);
    cia_tod_tick(0);
    for (int s = 0; s < 8; s++) { spr[s].state = 0; spr[s].armed = false; }
}

static void step_cck(void)
{
    int h = cs.hpos;
    if (h == 0) start_line();
    if (h == DENISE_DS) denise_start_line();

    /* fixed slots: disk 7,9,11  audio 13..19  sprites 21..51 (odd) */
    bool slot_used = false;
    if (h >= 0x15 && h < 0x35 && (h & 1)) {
        int idx = (h - 0x15) >> 1;
        sprite_dma(idx >> 1, idx & 1);
    }
    slot_used = bitplane_dma_cck();

    /* copper: even cycles not taken by bitplanes */
    if (!(h & 1) && !slot_used && h < 0xe2) {
        if (cs.cop_state == COP_WAIT) {
            if (cop_dma() && cop_compare()) cs.cop_state = COP_WAKE;
        } else if (cs.cop_state == COP_WAKE) {
            cs.cop_state = COP_READ1;
        } else if (cop_dma() && cs.cop_state != COP_STOP) {
            copper_slot();
        }
    }

    denise_cck();

    if (cs.blit_pending_irq && cs.cck + 1 >= cs.blit_busy_until) {
        cs.blit_pending_irq = false;
        intreq_set(0x0040);
    }

    audio_tick();
    floppy_tick();
    if ((cs.cck % 5) == 4) cia_tick_eclock(1);

    cs.cck++;
    if (++cs.hpos >= LINE_CCK) {
        cs.hpos = 0;
        if (++cs.vpos >= PAL_LINES) {
            cs.vpos = 0;
            start_frame();
        }
    }
}

void chip_sync(uint64_t cpu_cycle)
{
    while (cs.cck * 2 < cpu_cycle) step_cck();
}

uint64_t chip_next_event_cpu(void) { return (cs.cck + 1) * 2; }

/* ---- register file ---- */
static uint16_t vposr(void)
{
    return (uint16_t)(0x8000 | ((cs.vpos >> 8) & 1));   /* LOF set, OCS PAL Agnus id 0 */
}

uint16_t custom_read(uint32_t reg)
{
    switch (reg) {
    case 0x002: return (uint16_t)((cs.dmacon & 0x07ff) | (blitter_busy() ? 0x4000 : 0) | (cs.blit_zero ? 0x2000 : 0));
    case 0x004: return vposr();
    case 0x006: return (uint16_t)((cs.vpos & 0xff) << 8 | (cs.hpos & 0xff));
    case 0x00a: return cs.joy0dat;
    case 0x00c: return cs.joy1dat;
    case 0x00e: { uint16_t v = cs.clxdat | 0x8000; cs.clxdat = 0; return v; }
    case 0x010: return cs.adkcon;
    case 0x012: case 0x014: return 0;
    case 0x016: return 0xff00 & (cs.potgo | 0x5500);  /* right buttons released */
    case 0x018: return 0x3000;
    case 0x01a: return floppy_dskbytr();
    case 0x01c: return cs.intena;
    case 0x01e: return cs.intreq;
    case 0x000: return 0;
    default: return 0xffff;
    }
}

static inline uint32_t hi(uint32_t old, uint16_t v) { return (old & 0x0000ffff) | ((uint32_t)(v & ((CHIP_SIZE - 1) >> 16)) << 16); }
static inline uint32_t lo(uint32_t old, uint16_t v) { return (old & 0xffff0000) | (v & 0xfffe); }

static void setclr(uint16_t *r, uint16_t v, uint16_t mask)
{
    if (v & 0x8000) *r |= v & mask; else *r &= ~(v & mask);
}

void custom_write(uint32_t reg, uint16_t v)
{
    if (reg >= 0x180 && reg < 0x1c0) { cs.color[(reg - 0x180) >> 1] = v & 0xfff; return; }
    if (reg >= 0x0e0 && reg < 0x0f8) {
        int p = (reg - 0x0e0) >> 2;
        cs.bplpt[p] = (reg & 2) ? lo(cs.bplpt[p], v) : hi(cs.bplpt[p], v);
        return;
    }
    if (reg >= 0x120 && reg < 0x140) {
        int s = (reg - 0x120) >> 2;
        cs.sprpt[s] = (reg & 2) ? lo(cs.sprpt[s], v) : hi(cs.sprpt[s], v);
        return;
    }
    if (reg >= 0x140 && reg < 0x180) {
        int s = (reg - 0x140) >> 3;
        switch (reg & 6) {
        case 0: cs.sprpos[s] = v; sprite_compute(s); break;
        case 2: cs.sprctl[s] = v; sprite_compute(s); spr[s].armed = false; break;
        case 4: cs.sprdata[s] = v; spr[s].armed = true; break;
        case 6: cs.sprdatb[s] = v; break;
        }
        return;
    }
    if (reg >= 0x0a0 && reg < 0x0e0) {
        int ch = (reg - 0x0a0) >> 4;
        switch (reg & 0xe) {
        case 0x0: cs.audlc[ch] = hi(cs.audlc[ch], v); break;
        case 0x2: cs.audlc[ch] = lo(cs.audlc[ch], v); break;
        case 0x4: audio_set_len(ch, v); break;
        case 0x6: audio_set_period(ch, v); break;
        case 0x8: cs.audvol[ch] = v & 0x7f; break;
        case 0xa: audio_write_dat(ch, v); break;
        }
        return;
    }
    if (reg >= 0x110 && reg < 0x11c) {
        int p = (reg - 0x110) >> 1;
        cs.bpldat[p] = v;
        if (p == 0) denise_parallel_load();
        return;
    }
    switch (reg) {
    case 0x020: cs.dskpt = hi(cs.dskpt, v); break;
    case 0x022: cs.dskpt = lo(cs.dskpt, v); break;
    case 0x024: floppy_dsklen_write(v); break;
    case 0x02a: break; /* VPOSW */
    case 0x02e: cs.copcon = v; break;
    case 0x030: break;
    case 0x032: cs.serper = v; break;
    case 0x034: cs.potgo = v; break;
    case 0x036: break;
    case 0x040: cs.bltcon0 = v; break;
    case 0x042: cs.bltcon1 = v; break;
    case 0x044: cs.bltafwm = v; break;
    case 0x046: cs.bltalwm = v; break;
    case 0x048: cs.bltpt[2] = hi(cs.bltpt[2], v); break;
    case 0x04a: cs.bltpt[2] = lo(cs.bltpt[2], v); break;
    case 0x04c: cs.bltpt[1] = hi(cs.bltpt[1], v); break;
    case 0x04e: cs.bltpt[1] = lo(cs.bltpt[1], v); break;
    case 0x050: cs.bltpt[0] = hi(cs.bltpt[0], v); break;
    case 0x052: cs.bltpt[0] = lo(cs.bltpt[0], v); break;
    case 0x054: cs.bltpt[3] = hi(cs.bltpt[3], v); break;
    case 0x056: cs.bltpt[3] = lo(cs.bltpt[3], v); break;
    case 0x058: blitter_size_write(v); break;
    case 0x060: cs.bltmod[2] = (int16_t)(v & 0xfffe); break;
    case 0x062: cs.bltmod[1] = (int16_t)(v & 0xfffe); break;
    case 0x064: cs.bltmod[0] = (int16_t)(v & 0xfffe); break;
    case 0x066: cs.bltmod[3] = (int16_t)(v & 0xfffe); break;
    case 0x070: cs.bltdat[2] = v; break;
    case 0x072: cs.bltdat[1] = v; break;
    case 0x074: cs.bltdat[0] = v; break;
    case 0x07e: cs.dsksync = v; break;
    case 0x080: cs.cop1lc = hi(cs.cop1lc, v); break;
    case 0x082: cs.cop1lc = lo(cs.cop1lc, v); break;
    case 0x084: cs.cop2lc = hi(cs.cop2lc, v); break;
    case 0x086: cs.cop2lc = lo(cs.cop2lc, v); break;
    case 0x088: cop_jump(cs.cop1lc); break;
    case 0x08a: cop_jump(cs.cop2lc); break;
    case 0x08e: cs.diwstrt = v; break;
    case 0x090: cs.diwstop = v; break;
    case 0x092: cs.ddfstrt = v & 0xfc; break;
    case 0x094: cs.ddfstop = v & 0xfc; break;
    case 0x096: {
        uint16_t old = cs.dmacon;
        setclr(&cs.dmacon, v, 0x07ff);
        if ((old ^ cs.dmacon) & 0x020f) audio_dma_changed(old);
        break;
    }
    case 0x098: cs.clxcon = v; break;
    case 0x09a: setclr(&cs.intena, v, 0x7fff); irq_update(); break;
    case 0x09c: setclr(&cs.intreq, v, 0x7fff); cia_reassert(); irq_update(); break;
    case 0x09e: setclr(&cs.adkcon, v, 0x7fff); break;
    case 0x100: cs.bplcon0 = v; break;
    case 0x102: cs.bplcon1 = v; break;
    case 0x104: cs.bplcon2 = v; break;
    case 0x108: cs.bpl1mod = (int16_t)(v & 0xfffe); break;
    case 0x10a: cs.bpl2mod = (int16_t)(v & 0xfffe); break;
    default: break;
    }
}

void custom_reset(void)
{
    memset(&cs, 0, sizeof cs);
    memset(spr, 0, sizeof spr);
    for (int i = 0; i < 4096; i++) {
        uint32_t r = (i >> 8) & 15, g = (i >> 4) & 15, b = i & 15;
        rgb_lut[i] = 0xff000000u | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
        rgb_half_lut[i] = 0xff000000u | ((r >> 1) * 17) << 16 | ((g >> 1) * 17) << 8 | ((b >> 1) * 17);
    }
    cs.cop_state = COP_STOP;
    cs.vpos = 0; cs.hpos = 0;
    memset(backbuffer, 0, sizeof backbuffer);   /* no picture from a previous run */
    memset(framebuffer, 0, sizeof framebuffer);
    outline = backbuffer[0];
}
