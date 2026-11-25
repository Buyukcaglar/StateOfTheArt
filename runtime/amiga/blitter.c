/* OCS blitter. The operation is carried out at once when BLTSIZE is written;
 * BBUSY stays set (and the BLIT interrupt is deferred) for the time the real
 * DMA would take, so code that waits on the blitter keeps its timing. */
#include "amiga.h"
#include "mem_gen.h"
#include <stdlib.h>

static inline uint16_t minterm(uint8_t lf, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t d = 0;
    if (lf & 0x01) d |= ~a & ~b & ~c;
    if (lf & 0x02) d |= ~a & ~b &  c;
    if (lf & 0x04) d |= ~a &  b & ~c;
    if (lf & 0x08) d |= ~a &  b &  c;
    if (lf & 0x10) d |=  a & ~b & ~c;
    if (lf & 0x20) d |=  a & ~b &  c;
    if (lf & 0x40) d |=  a &  b & ~c;
    if (lf & 0x80) d |=  a &  b &  c;
    return d;
}

static void blit_area(int w, int h)
{
    uint16_t con0 = cs.bltcon0, con1 = cs.bltcon1;
    int ash = con0 >> 12, bsh = con1 >> 12;
    bool usea = con0 & 0x800, useb = con0 & 0x400, usec = con0 & 0x200, used = con0 & 0x100;
    uint8_t lf = con0 & 0xff;
    bool desc = con1 & 2;
    bool ife = con1 & 8, efe = con1 & 16, fill = ife || efe;
    int step = desc ? -2 : 2;
    uint32_t apt = cs.bltpt[0], bpt = cs.bltpt[1], cpt = cs.bltpt[2], dpt = cs.bltpt[3];
    uint16_t aold = 0, bold = 0;
    uint16_t adat = cs.bltdat[0], bdat = cs.bltdat[1], cdat = cs.bltdat[2];
    bool zero = true;

    for (int y = 0; y < h; y++) {
        int fc = (con1 >> 2) & 1;
        for (int x = 0; x < w; x++) {
            if (usea) { adat = chip_rw(apt); apt += step; }
            if (useb) { bdat = chip_rw(bpt); bpt += step; }
            if (usec) { cdat = chip_rw(cpt); cpt += step; }
            uint16_t am = adat;
            if (x == 0) am &= cs.bltafwm;
            if (x == w - 1) am &= cs.bltalwm;
            uint16_t a, b;
            if (!desc) {
                a = (uint16_t)((((uint32_t)aold << 16) | am) >> ash);
                b = (uint16_t)((((uint32_t)bold << 16) | bdat) >> bsh);
            } else {
                a = (uint16_t)((((uint32_t)am << 16) | aold) >> (16 - ash));
                b = (uint16_t)((((uint32_t)bdat << 16) | bold) >> (16 - bsh));
            }
            aold = am; bold = bdat;
            uint16_t d = minterm(lf, a, b, cdat);
            if (fill) {
                uint16_t out = 0;
                for (int i = 0; i < 16; i++) {
                    int bit = (d >> i) & 1;
                    if (efe) { fc ^= bit; out |= (uint16_t)(fc << i); }
                    else { out |= (uint16_t)((fc | bit) << i); fc ^= bit; }
                }
                d = out;
            }
            if (d) zero = false;
            if (used) { chip_ww(dpt, d); dpt += step; }
        }
        if (usea) apt += desc ? -cs.bltmod[0] : cs.bltmod[0];
        if (useb) bpt += desc ? -cs.bltmod[1] : cs.bltmod[1];
        if (usec) cpt += desc ? -cs.bltmod[2] : cs.bltmod[2];
        if (used) dpt += desc ? -cs.bltmod[3] : cs.bltmod[3];
    }
    cs.bltpt[0] = apt & 0x7fffe; cs.bltpt[1] = bpt & 0x7fffe;
    cs.bltpt[2] = cpt & 0x7fffe; cs.bltpt[3] = dpt & 0x7fffe;
    cs.bltdat[0] = adat; cs.bltdat[1] = bdat; cs.bltdat[2] = cdat;
    cs.blit_zero = zero;
}

static void blit_line(int len)
{
    uint16_t con0 = cs.bltcon0, con1 = cs.bltcon1;
    int ash = con0 >> 12;
    int bsh = con1 >> 12;
    uint8_t lf = con0 & 0xff;
    bool sud = con1 & 0x10, sul = con1 & 0x08, aul = con1 & 0x04, sing = con1 & 0x02;
    bool sign = con1 & 0x40;
    uint32_t cpt = cs.bltpt[2], dpt = cs.bltpt[3];
    int32_t err = (int16_t)(cs.bltpt[0] & 0xffff);
    uint16_t cdat = cs.bltdat[2];
    bool zero = true;
    bool onedot = false;

    for (int i = 0; i < len; i++) {
        uint16_t a = (uint16_t)((cs.bltdat[0] & cs.bltafwm) >> ash);
        uint16_t texbit = (cs.bltdat[1] >> ((15 - bsh) & 15)) & 1;
        uint16_t b = texbit ? 0xffff : 0;
        if (con0 & 0x200) cdat = chip_rw(cpt);
        if (sing && onedot) a = 0;
        onedot = true;
        uint16_t d = minterm(lf, a, b, cdat);
        if (d) zero = false;
        chip_ww(dpt, d);          /* line mode always writes D, whatever USED says */
        bsh = (bsh + 1) & 15;

        /* error term */
        if (sign) err += (int16_t)cs.bltmod[1]; else err += (int16_t)cs.bltmod[0];
        bool step_minor = !sign;
        #define INCX() do { if (++ash == 16) { ash = 0; cpt += 2; } } while (0)
        #define DECX() do { if (ash-- == 0) { ash = 15; cpt -= 2; } } while (0)
        #define INCY() do { cpt += (int16_t)cs.bltmod[2]; onedot = false; } while (0)
        #define DECY() do { cpt -= (int16_t)cs.bltmod[2]; onedot = false; } while (0)
        if (step_minor) {
            if (sud) { if (sul) DECY(); else INCY(); }
            else     { if (sul) DECX(); else INCX(); }
        }
        if (sud) { if (aul) DECX(); else INCX(); }
        else     { if (aul) DECY(); else INCY(); }
        sign = (int16_t)err < 0;
        dpt = cpt;
    }
    cs.bltpt[0] = (cs.bltpt[0] & 0x70000) | ((uint32_t)err & 0xffff);
    cs.bltpt[2] = cpt & 0x7fffe;
    cs.bltpt[3] = dpt & 0x7fffe;
    cs.bltcon0 = (uint16_t)((cs.bltcon0 & 0x0fff) | (ash << 12));
    cs.bltcon1 = (uint16_t)((cs.bltcon1 & 0x0fbf) | (bsh << 12) | (sign ? 0x40 : 0));
    cs.bltdat[2] = cdat;
    cs.blit_zero = zero;
}

void blitter_size_write(uint16_t v)
{
    int h = v >> 6; if (!h) h = 1024;
    int w = v & 63; if (!w) w = 64;
    uint64_t cck;
    static int logged;
    if (getenv("SOTA_BLITLOG") && logged < 40) {
        logged++;
        amiga_log("[blit] f%llu con0=%04x con1=%04x size=%dx%d A=%05x B=%05x C=%05x D=%05x mod=%d,%d,%d,%d dat=%04x,%04x,%04x fwm=%04x lwm=%04x\n",
            (unsigned long long)cs.frame, cs.bltcon0, cs.bltcon1, w, h, cs.bltpt[0], cs.bltpt[1], cs.bltpt[2], cs.bltpt[3],
            cs.bltmod[0], cs.bltmod[1], cs.bltmod[2], cs.bltmod[3], cs.bltdat[0], cs.bltdat[1], cs.bltdat[2], cs.bltafwm, cs.bltalwm);
    }
    if (cs.bltcon1 & 1) {
        blit_line(h);
        cck = (uint64_t)h * 4 + 2;
    } else {
        blit_area(w, h);
        int ch = ((cs.bltcon0 >> 11) & 1) + ((cs.bltcon0 >> 10) & 1) + ((cs.bltcon0 >> 9) & 1) + ((cs.bltcon0 >> 8) & 1);
        int per = ch < 2 ? 2 : ch;
        if ((cs.bltcon1 & 0x18) && per < 3) per = 3;
        cck = (uint64_t)w * h * per + 2;
    }
    cs.blit_busy_until = cs.cck + cck;
    cs.blit_pending_irq = true;
}
