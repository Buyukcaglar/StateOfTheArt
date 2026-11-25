/* DF0: double-density drive. Tracks are MFM-encoded on demand from the ADF in
 * the standard AmigaDOS sector format, then streamed bit by bit (2 us cells,
 * 300 rpm: 100000 bits per 709379 cck) through DSKSYNC matching and disk DMA. */
#include "amiga.h"
#include "mem_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADF_SIZE    901120
#define TRACK_WORDS 6250
#define TRACK_BITS  (TRACK_WORDS * 16)
#define REV_CCK     709379

static uint8_t adf[ADF_SIZE];
static bool disk_in;
static uint16_t mfm[TRACK_WORDS];
static int mfm_track = -1;

static struct {
    int cyl, side;
    bool motor, selected;
    uint32_t bitpos;       /* rotation position, in bits */
    uint32_t acc;          /* fractional bit accumulator */
    uint16_t shreg;        /* incoming bit shift register */
    int bitcount;
    bool dma_on, dma_write, synced;
    int words_left;
    uint16_t last_word; bool byte_ready; bool word_equal;
    uint64_t motor_on_cck;
} fd;

static void mfm_encode(void);

bool amiga_load_adf_memory(const uint8_t *data, size_t len)
{
    if (len != ADF_SIZE) return false;
    memcpy(adf, data, ADF_SIZE);
    disk_in = true;
    mfm_track = -1;
    return true;
}

bool amiga_load_adf(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *buf = malloc(ADF_SIZE + 1);
    size_t n = fread(buf, 1, ADF_SIZE + 1, f);
    fclose(f);
    bool ok = amiga_load_adf_memory(buf, n);
    free(buf);
    return ok;
}

const uint8_t *amiga_adf_data(void) { return disk_in ? adf : 0; }

void floppy_reset(void)
{
    memset(&fd, 0, sizeof fd);
    fd.cyl = 0;
}

/* ---- MFM ---- */
static int put_pos;
static uint16_t prev_bit;
static void put_raw(uint16_t w) { mfm[put_pos++ % TRACK_WORDS] = w; prev_bit = w & 1; }

/* data word with only the odd (0x5555) bits meaningful: add clock bits */
static void put_data(uint16_t d)
{
    d &= 0x5555;
    uint16_t out = d;
    for (int b = 15; b >= 1; b -= 2) {
        int prev = b == 15 ? prev_bit : (d >> (b + 1)) & 1;
        int next = (d >> (b - 1)) & 1;
        if (!prev && !next) out |= (uint16_t)(1u << b);
    }
    put_raw(out);
}
static void put_long_data(uint32_t l) { put_data((uint16_t)(l >> 16)); put_data((uint16_t)l); }
static void put_oddeven(uint32_t l) { put_long_data(l >> 1); put_long_data(l); }

static void mfm_encode(void)
{
    int track = fd.cyl * 2 + fd.side;
    mfm_track = track;
    put_pos = 0; prev_bit = 0;
    const uint8_t *t = adf + (size_t)track * 11 * 512;
    for (int s = 0; s < 11; s++) {
        put_data(0); put_data(0);
        put_raw(0x4489); put_raw(0x4489);
        uint32_t info = 0xff000000u | (uint32_t)track << 16 | (uint32_t)s << 8 | (uint32_t)(11 - s);
        uint32_t hsum = ((info >> 1) ^ info) & 0x55555555u;
        put_oddeven(info);
        for (int k = 0; k < 4; k++) put_long_data(0);   /* label, odd */
        for (int k = 0; k < 4; k++) put_long_data(0);   /* label, even */
        const uint8_t *p = t + s * 512;
        uint32_t dsum = 0;
        for (int k = 0; k < 128; k++) {
            uint32_t l = (uint32_t)p[k * 4] << 24 | p[k * 4 + 1] << 16 | p[k * 4 + 2] << 8 | p[k * 4 + 3];
            dsum ^= (l >> 1) ^ l;
        }
        dsum &= 0x55555555u;
        put_oddeven(hsum);
        put_oddeven(dsum);
        for (int k = 0; k < 128; k++) {
            uint32_t l = (uint32_t)p[k * 4] << 24 | p[k * 4 + 1] << 16 | p[k * 4 + 2] << 8 | p[k * 4 + 3];
            put_long_data(l >> 1);
        }
        for (int k = 0; k < 128; k++) {
            uint32_t l = (uint32_t)p[k * 4] << 24 | p[k * 4 + 1] << 16 | p[k * 4 + 2] << 8 | p[k * 4 + 3];
            put_long_data(l);
        }
    }
    while (put_pos < TRACK_WORDS) put_data(0);
}

/* ---- drive control (CIA-B PRB) ---- */
void floppy_ciab_prb_write(uint8_t v, uint8_t old)
{
    bool sel = !(v & 0x08);
    bool was_sel = !(old & 0x08);
    if (sel && !was_sel) {
        bool m = !(v & 0x80);
        if (m && !fd.motor) fd.motor_on_cck = cs.cck;
        if (m != fd.motor) amiga_log("[disk] motor %s\n", m ? "on" : "off");
        fd.motor = m;
    }
    fd.selected = sel;
    if (sel) {
        fd.side = (v & 0x04) ? 0 : 1;
        if ((old & 0x01) && !(v & 0x01)) {         /* step pulse */
            if (v & 0x02) { if (fd.cyl > 0) fd.cyl--; }
            else if (fd.cyl < 79) fd.cyl++;
        }
    }
}

uint8_t floppy_status_bits(void)
{
    uint8_t v = 0x3c;
    if (!fd.selected) return v;
    if (fd.motor) v &= ~0x20;                     /* /RDY */
    if (fd.cyl == 0) v &= ~0x10;                  /* /TK0 */
    v &= ~0x08;                                   /* /WPRO: protected */
    if (!disk_in) v &= ~0x04;                     /* /CHNG */
    return v;
}

void floppy_dsklen_write(uint16_t v)
{
    uint16_t prev = cs.dsklen;
    cs.dsklen = v;
    if (!(v & 0x8000)) { fd.dma_on = false; return; }
    if (prev & 0x8000) {
        fd.dma_on = true;
        fd.dma_write = v & 0x4000;
        fd.words_left = v & 0x3fff;
        fd.synced = !(cs.adkcon & 0x0400);
        fd.bitcount = 0;
        amiga_log("[disk] DMA start cyl %d side %d len %d -> $%05x sync=%d f%llu\n", fd.cyl, fd.side, fd.words_left, cs.dskpt, !fd.synced, (unsigned long long)cs.frame);
        if (fd.dma_write) amiga_log("[disk] write DMA ignored\n");
    }
}

uint16_t floppy_dskbytr(void)
{
    uint16_t v = (uint16_t)((fd.byte_ready ? 0x8000 : 0) | (fd.dma_on ? 0x4000 : 0) | (fd.dma_write ? 0x2000 : 0) |
                            (fd.word_equal ? 0x1000 : 0) | (fd.last_word & 0xff));
    fd.byte_ready = false;
    return v;
}

static void dma_word(uint16_t w)
{
    if (!fd.dma_on || fd.dma_write || !fd.synced) return;
    if ((cs.dmacon & 0x0210) != 0x0210) return;
    chip_ww(cs.dskpt, w);
    cs.dskpt = (cs.dskpt + 2) & CHIP_DMA_MASK;
    if (--fd.words_left <= 0) {
        fd.dma_on = false;
        amiga_log("[disk] DMA done f%llu\n", (unsigned long long)cs.frame);
        cs.intreq |= 0x0002;
        irq_update();
    }
}

void floppy_tick(void)
{
    if (!disk_in || !fd.motor) return;
    fd.acc += 100000;
    if (fd.acc < REV_CCK) return;
    fd.acc -= REV_CCK;
    /* one bit cell passes the head */
    int track = fd.cyl * 2 + fd.side;
    if (track != mfm_track) mfm_encode();
    uint32_t pos = fd.bitpos;
    int bit = (mfm[pos >> 4] >> (15 - (pos & 15))) & 1;
    if (++fd.bitpos >= TRACK_BITS) { fd.bitpos = 0; cia_flag(1); }
    fd.shreg = (uint16_t)(fd.shreg << 1 | bit);
    fd.word_equal = false;
    if (++fd.bitcount == 8 || fd.bitcount == 16) { fd.last_word = fd.shreg; fd.byte_ready = true; }
    if (fd.shreg == cs.dsksync) {
        fd.word_equal = true;
        cs.intreq |= 0x1000; irq_update();
        if (cs.adkcon & 0x0400) {
            /* word sync: realign; the first match only starts DMA, later ones are stored */
            fd.bitcount = 0;
            if (fd.dma_on && !fd.synced) { fd.synced = true; return; }
            dma_word(fd.shreg);
            return;
        }
    }
    if (fd.bitcount >= 16) {
        fd.bitcount = 0;
        dma_word(fd.shreg);
    }
}
