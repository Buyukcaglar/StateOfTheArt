/* Headless runner: boots the ADF, runs N frames, optionally dumps PNG frames,
 * records coverage, and prints per-frame hashes for mode comparisons.
 *
 *   sota_run --adf D.adf --frames N [--dump DIR] [--every K] [--from F]
 *            [--crop x,y,w,h | --full] [--scale S] [--coverage FILE]
 *            [--interp] [--hashes FILE] [--wav FILE] [--quiet]           */
#include "../../runtime/amiga/amiga.h"
#include "../../runtime/core/exec.h"
#include "../../runtime/cpu/m68k.h"
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

size_t audio_available(void);
void recomp_register_all(void);

static const char *dump_dir;
static int every = 1, from_frame = 0, cx = 0, cy = 0, cw = FB_W, ch_ = FB_H, scale = 1;
static FILE *hashes, *wav;
static uint32_t wav_frames;

static uint64_t fnv(const void *p, size_t n)
{
    const uint8_t *b = p; uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

static void on_frame(uint64_t frame)
{
    if (hashes) fprintf(hashes, "%llu %016llx %06x diw=%04x,%04x ddf=%02x,%02x con0=%04x\n", (unsigned long long)frame,
                        (unsigned long long)fnv(framebuffer, sizeof framebuffer), cpu.pc,
                        cs.diwstrt, cs.diwstop, cs.ddfstrt, cs.ddfstop, cs.bplcon0);
    if (wav) {
        float buf[4096];
        size_t n;
        while ((n = audio_pull(buf, 2048)) > 0) {
            for (size_t k = 0; k < n * 2; k++) {
                float v = buf[k] * 1.6f; if (v > 1) v = 1; if (v < -1) v = -1;
                int16_t s = (int16_t)(v * 32767);
                fwrite(&s, 2, 1, wav);
            }
            wav_frames += (uint32_t)n;
        }
    }
    if (!dump_dir || (int64_t)frame < from_frame || ((frame - from_frame) % every)) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%05llu.png", dump_dir, (unsigned long long)frame);
    if (scale == 1) png_write(path, &framebuffer[cy][cx], cw, ch_, FB_W);
    else {
        /* the framebuffer is hires-wide; `scale` duplicates lines for square-ish pixels */
        static uint32_t tmp[FB_H * 2][FB_W];
        for (int y = 0; y < ch_; y++) for (int s = 0; s < scale; s++)
            memcpy(tmp[y * scale + s], &framebuffer[cy + y][cx], cw * 4);
        png_write(path, &tmp[0][0], cw, ch_ * scale, FB_W);
    }
}

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, riff = 36 + data, rate = 48000, br = rate * 4, fmt = 16;
    uint16_t pcm = 1, chn = 2, ba = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&chn, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

int main(int argc, char **argv)
{
    const char *adf = "_reference/StateOfTheArt.adf", *covfile = 0, *memdump = 0;
    int frames = 50, reboot_at = 0;
    bool interp = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--adf")) adf = v, i++;
        else if (!strcmp(a, "--frames")) frames = atoi(v), i++;
        else if (!strcmp(a, "--dump")) dump_dir = v, i++;
        else if (!strcmp(a, "--every")) every = atoi(v), i++;
        else if (!strcmp(a, "--from")) from_frame = atoi(v), i++;
        else if (!strcmp(a, "--crop")) { sscanf(v, "%d,%d,%d,%d", &cx, &cy, &cw, &ch_); i++; }
        else if (!strcmp(a, "--scale")) scale = atoi(v), i++;
        else if (!strcmp(a, "--full")) { cx = cy = 0; cw = FB_W; ch_ = FB_H; }
        else if (!strcmp(a, "--coverage")) covfile = v, i++;
        else if (!strcmp(a, "--interp")) interp = true;
        else if (!strcmp(a, "--hashes")) { hashes = fopen(v, "w"); i++; }
        else if (!strcmp(a, "--wav")) { wav = fopen(v, "wb"); wav_header(wav, 0); i++; }
        else if (!strcmp(a, "--memdump")) memdump = v, i++;
        else if (!strcmp(a, "--trace")) { exec_trace = fopen(v, "w"); i++; }
        else if (!strcmp(a, "--trace-frames")) { unsigned long long x, y; sscanf(v, "%llu,%llu", &x, &y); exec_trace_from = x; exec_trace_to = y; i++; }
        else if (!strcmp(a, "--reboot-at")) reboot_at = atoi(v), i++;
        else if (!strcmp(a, "--quiet")) amiga_log_level = 0;
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (dump_dir) mkdir(dump_dir, 0755);
    amiga_init();
    if (!amiga_load_adf(adf)) { fprintf(stderr, "cannot load %s\n", adf); return 1; }
    audio_set_rate(48000);
    amiga_set_frame_callback(on_frame);
    if (!interp) recomp_register_all();
    exec_use_recomp = !interp;
    if (covfile) { exec_coverage_read(covfile); exec_coverage_enable(true); }
    amiga_boot_hle();
    clock_t t0 = clock();
    if (reboot_at) {
        /* boot twice in one process (as the apps do when looping); frame numbers restart */
        exec_run_frames(reboot_at);
        amiga_init();
        amiga_load_adf(adf);
        audio_set_rate(48000);
        amiga_set_frame_callback(on_frame);
        amiga_boot_hle();
        if (hashes) fprintf(hashes, "# reboot\n");
    }
    exec_run_frames(frames);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fprintf(stderr, "ran %d frames in %.2fs (%.1fx realtime) pc=$%06x | interp blocks %llu insns %llu, recomp blocks %llu, misses %llu, irqs %llu\n",
            frames, secs, frames / 50.0 / secs, cpu.pc,
            (unsigned long long)exec_stats.interp_blocks, (unsigned long long)exec_stats.interp_insns,
            (unsigned long long)exec_stats.recomp_blocks, (unsigned long long)exec_stats.misses,
            (unsigned long long)exec_stats.irqs);
    if (covfile) fprintf(stderr, "coverage: %d block variants -> %s\n", exec_coverage_write(covfile), covfile);
    if (memdump) { FILE *m = fopen(memdump, "wb"); fwrite(chip_ram, 1, CHIP_SIZE, m); fwrite(slow_ram, 1, SLOW_SIZE, m); fclose(m); }
    fprintf(stderr, "regs: d=%08x %08x %08x %08x %08x %08x %08x %08x\n      a=%08x %08x %08x %08x %08x %08x %08x %08x sr=%04x\n", cpu.d[0],cpu.d[1],cpu.d[2],cpu.d[3],cpu.d[4],cpu.d[5],cpu.d[6],cpu.d[7],cpu.a[0],cpu.a[1],cpu.a[2],cpu.a[3],cpu.a[4],cpu.a[5],cpu.a[6],cpu.a[7],m68k_get_sr());
    fprintf(stderr, "custom: dmacon=%04x intena=%04x intreq=%04x cop1lc=%06x bplcon0=%04x frame=%llu\n", cs.dmacon, cs.intena, cs.intreq, cs.cop1lc, cs.bplcon0, (unsigned long long)cs.frame);
    if (hashes) fclose(hashes);
    if (wav) { wav_header(wav, wav_frames); fclose(wav); }
    return 0;
}
