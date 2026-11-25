/* Amiga 500 (OCS, PAL, 512K chip + 512K slow) chipset runtime. */
#ifndef SOTA_AMIGA_H
#define SOTA_AMIGA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CHIP_SIZE   0x80000u
#define SLOW_BASE   0xC00000u
#define SLOW_SIZE   0x80000u

#define PAL_LINES   313
#define LINE_CCK    227
/* framebuffer: 4 hires pixels per colour clock, one row per line */
#define FB_W        (LINE_CCK * 4)
#define FB_H        PAL_LINES

#define CCK_HZ      3546895.0
#define CPU_HZ      (CCK_HZ * 2)

extern uint8_t chip_ram[CHIP_SIZE];
extern uint8_t slow_ram[SLOW_SIZE];

/* custom chip state needed across files */
typedef struct Custom {
    /* beam */
    int vpos, hpos;
    uint64_t cck;              /* colour clocks since reset */
    uint64_t frame;            /* completed frames */
    /* DMA / interrupts */
    uint16_t dmacon, intena, intreq, adkcon;
    /* copper */
    uint32_t cop1lc, cop2lc, coppc;
    uint16_t copcon;
    int cop_state;             /* see custom.c */
    uint16_t cop_ir1, cop_ir2;
    int cop_wake_delay;
    /* display */
    uint16_t bplcon0, bplcon1, bplcon2;
    uint16_t diwstrt, diwstop, ddfstrt, ddfstop;
    int16_t  bpl1mod, bpl2mod;
    uint32_t bplpt[6];
    uint16_t bpldat[6];
    uint16_t color[32];
    uint32_t sprpt[8];
    uint16_t sprpos[8], sprctl[8], sprdata[8], sprdatb[8];
    uint16_t clxcon;
    uint16_t clxdat;
    /* blitter */
    uint16_t bltcon0, bltcon1, bltafwm, bltalwm;
    uint32_t bltpt[4];         /* A B C D */
    int16_t  bltmod[4];
    uint16_t bltdat[3];        /* A B C holding */
    uint64_t blit_busy_until;  /* cck */
    bool     blit_zero;
    bool     blit_pending_irq;
    /* disk */
    uint32_t dskpt;
    uint16_t dsklen, dsklen_prev, dsksync;
    /* audio */
    uint32_t audlc[4];
    uint16_t audlen[4], audper[4], audvol[4], auddat[4];
    /* misc */
    uint16_t potgo, joy0dat, joy1dat;
    uint16_t serper;
} Custom;
extern Custom cs;

/* lifecycle */
void amiga_init(void);
bool amiga_load_adf(const char *path);
bool amiga_load_adf_memory(const uint8_t *data, size_t len);
void amiga_boot_hle(void);              /* emulate Kickstart reading and entering the bootblock */

/* timing */
void chip_sync(uint64_t cpu_cycle);     /* advance chipset to the given CPU time */
uint64_t chip_next_event_cpu(void);     /* conservative: CPU time of the next interrupt-relevant event */
void irq_update(void);

/* custom registers / CIAs */
uint16_t custom_read(uint32_t reg);
void     custom_write(uint32_t reg, uint16_t v);
uint8_t  cia_read(int which, int reg);
void     cia_write(int which, int reg, uint8_t v);
void     cia_tick_eclock(int n);        /* n E-clock ticks */
void     cia_tod_tick(int which);
void     cia_flag(int which);
void     cia_reset(void);
uint8_t  ciaa_pra_inputs(void);

/* blitter */
void blitter_size_write(uint16_t v);
void custom_reset(void);

/* floppy */
void floppy_reset(void);
void floppy_ciab_prb_write(uint8_t v, uint8_t old);
uint8_t floppy_status_bits(void);       /* CIA-A PRA bits 2..5 (active low) */
void floppy_tick(void);                 /* once per cck */
void floppy_dsklen_write(uint16_t v);
uint16_t floppy_dskbytr(void);

/* audio */
void audio_reset(void);
void audio_dma_changed(uint16_t old_dmacon);
void audio_tick(void);                  /* once per cck */
void audio_write_dat(int ch, uint16_t v);
void audio_set_period(int ch, uint16_t v);
void audio_set_len(int ch, uint16_t v);
/* host side: pull interleaved stereo float samples generated so far */
size_t audio_pull(float *out, size_t max_frames);
void audio_set_rate(double hz);

/* display output */
extern uint32_t framebuffer[FB_H][FB_W];   /* 0xAARRGGBB of the last complete frame */
typedef void (*FrameCallback)(uint64_t frame);
void amiga_set_frame_callback(FrameCallback cb);

/* host input */
void amiga_set_mouse_button(bool left, bool right);

/* diagnostics */
extern int amiga_log_level;
void amiga_log(const char *fmt, ...);

#endif
