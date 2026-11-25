/* Paula audio: 4 DMA channels, period counters in cck, AUDx interrupts on
 * (re)load, box-filtered down to the host rate, then the A500's fixed RC
 * low-pass (~4.9 kHz) and the switchable "LED" filter (~3.3 kHz, 2-pole). */
#include "amiga.h"
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>
#include <stdatomic.h>

bool cia_led_on(void);

typedef struct {
    bool dma;              /* DMA running on this channel */
    uint32_t ptr;
    uint32_t len;          /* words left */
    uint16_t dat;          /* current word being played */
    int byte;              /* 0 = high byte next, 1 = low byte next, 2 = need word */
    int8_t out;            /* current sample */
    uint32_t per_left;
    bool cpu_pending;      /* non-DMA mode: word written by CPU waiting */
    bool cpu_playing;
} Chan;

static Chan ch[4];
static double out_rate = 48000.0, phase;
static double acc_l, acc_r; static int acc_n;
#define RING 32768
static float ring[RING * 2];
/* single producer (emulation thread) / single consumer (audio thread) */
static _Atomic size_t ring_w, ring_r;
static double rc_l, rc_r, led1_l, led1_r, led2_l, led2_r;
static double a_rc, a_led;

static void coefs(void)
{
    a_rc = 1.0 - exp(-2.0 * M_PI * 4900.0 / out_rate);
    a_led = 1.0 - exp(-2.0 * M_PI * 3300.0 / out_rate);
}

void audio_set_rate(double hz) { out_rate = hz; coefs(); }

void audio_reset(void)
{
    memset(ch, 0, sizeof ch);
    phase = 0; acc_l = acc_r = 0; acc_n = 0;
    atomic_store(&ring_w, 0); atomic_store(&ring_r, 0);
    rc_l = rc_r = led1_l = led1_r = led2_l = led2_r = 0;
    coefs();
}

static inline uint32_t period(int c) { uint32_t p = cs.audper[c]; return p ? p : 65536; }
static inline uint32_t length(int c) { uint32_t l = cs.audlen[c]; return l ? l : 65536; }
static inline void aud_irq(int c) { cs.intreq |= (uint16_t)(0x0080 << c); irq_update(); }

static void fetch_word(int c)
{
    Chan *a = &ch[c];
    a->dat = (uint16_t)(chip_ram[a->ptr & CHIP_DMA_MASK] << 8 | chip_ram[(a->ptr & CHIP_DMA_MASK) + 1]);
    a->ptr += 2;
    if (--a->len == 0) {
        a->ptr = cs.audlc[c];
        a->len = length(c);
        aud_irq(c);
    }
    a->byte = 0;
}

void audio_dma_changed(uint16_t old)
{
    bool master = cs.dmacon & 0x0200, was = old & 0x0200;
    for (int c = 0; c < 4; c++) {
        bool on = master && (cs.dmacon & (1 << c));
        bool was_on = was && (old & (1 << c));
        if (on && !was_on) {
            Chan *a = &ch[c];
            a->dma = true;
            a->ptr = cs.audlc[c];
            a->len = length(c);
            fetch_word(c);
            aud_irq(c);           /* location registers may now be reloaded */
            a->per_left = period(c);
            a->out = (int8_t)(a->dat >> 8);
            a->byte = 1;
        } else if (!on && was_on) {
            ch[c].dma = false;
            ch[c].out = 0;
        }
    }
}

void audio_set_len(int c, uint16_t v) { cs.audlen[c] = v; }
void audio_set_period(int c, uint16_t v) { cs.audper[c] = v; }

void audio_write_dat(int c, uint16_t v)
{
    cs.auddat[c] = v;
    if (!ch[c].dma) {
        if (!ch[c].cpu_playing) {
            ch[c].cpu_playing = true;
            ch[c].dat = v; ch[c].byte = 1; ch[c].out = (int8_t)(v >> 8);
            ch[c].per_left = period(c);
            aud_irq(c);
        } else ch[c].cpu_pending = true;
    }
}

static void emit(double l, double r)
{
    /* fixed RC filter, then optional LED filter */
    rc_l += a_rc * (l - rc_l); rc_r += a_rc * (r - rc_r);
    l = rc_l; r = rc_r;
    if (cia_led_on()) {
        led1_l += a_led * (l - led1_l); led2_l += a_led * (led1_l - led2_l);
        led1_r += a_led * (r - led1_r); led2_r += a_led * (led1_r - led2_r);
        l = led2_l; r = led2_r;
    } else { led1_l = led2_l = l; led1_r = led2_r = r; }
    size_t w = atomic_load_explicit(&ring_w, memory_order_relaxed);
    size_t next = (w + 1) % RING;
    if (next == atomic_load_explicit(&ring_r, memory_order_acquire)) return;   /* overrun: drop newest */
    ring[w * 2] = (float)l; ring[w * 2 + 1] = (float)r;
    atomic_store_explicit(&ring_w, next, memory_order_release);
}

void audio_tick(void)
{
    for (int c = 0; c < 4; c++) {
        Chan *a = &ch[c];
        if (!a->dma && !a->cpu_playing) continue;
        if (--a->per_left == 0) {
            a->per_left = period(c);
            if (a->byte == 1) { a->out = (int8_t)(a->dat & 0xff); a->byte = 2; }
            else {
                if (a->dma) fetch_word(c);
                else if (a->cpu_pending) {
                    a->cpu_pending = false; a->dat = cs.auddat[c]; a->byte = 0; aud_irq(c);
                } else { a->cpu_playing = false; a->out = 0; continue; }
                a->out = (int8_t)(a->dat >> 8);
                a->byte = 1;
            }
        }
    }
    int v0 = cs.audvol[0] > 64 ? 64 : cs.audvol[0], v1 = cs.audvol[1] > 64 ? 64 : cs.audvol[1];
    int v2 = cs.audvol[2] > 64 ? 64 : cs.audvol[2], v3 = cs.audvol[3] > 64 ? 64 : cs.audvol[3];
    acc_l += ch[0].out * v0 + ch[3].out * v3;
    acc_r += ch[1].out * v1 + ch[2].out * v2;
    acc_n++;
    phase += out_rate;
    if (phase >= CCK_HZ) {
        phase -= CCK_HZ;
        double s = 1.0 / (acc_n * 16384.0);
        emit(acc_l * s, acc_r * s);
        acc_l = acc_r = 0; acc_n = 0;
    }
}

size_t audio_pull(float *out, size_t max_frames)
{
    size_t n = 0;
    size_t r = atomic_load_explicit(&ring_r, memory_order_relaxed);
    size_t w = atomic_load_explicit(&ring_w, memory_order_acquire);
    while (n < max_frames && r != w) {
        out[n * 2] = ring[r * 2];
        out[n * 2 + 1] = ring[r * 2 + 1];
        r = (r + 1) % RING;
        n++;
    }
    atomic_store_explicit(&ring_r, r, memory_order_release);
    return n;
}

size_t audio_available(void) { return (atomic_load(&ring_w) + RING - atomic_load(&ring_r)) % RING; }
