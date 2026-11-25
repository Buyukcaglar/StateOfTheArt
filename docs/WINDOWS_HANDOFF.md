# Windows port — handoff

Everything below `runtime/` and `tools/` is portable C11 and is the same code the macOS/iOS
apps use. Only a Windows host shell is missing (the Apple one is `platform/apple` +
`platform/macos`, ~400 lines in total).

## Recreate the generated code on Windows

`build/coverage.txt` and `build/gen/` are derived from the disk and are not committed.
Regenerate them (or copy them over from the Mac):

1. Build `tools/runner/sota_run.c` + all of `runtime/` + `runtime/core/recomp_none.c`
   (needs zlib for the PNG writer, or stub `png_write` out).
2. `sota_run --frames 13000 --coverage build/coverage.txt --quiet`
3. Build `tools/lift/lift.c` + `runtime/cpu/m68k_decode.c` + `runtime/cpu/m68k_dasm.c`,
   then `lift build/coverage.txt build/gen`.
4. App = `runtime/{cpu,amiga}/*.c` + `runtime/core/exec.c` + `build/gen/*.c` (not `*.inc`,
   they are `#include`d) + your host. Do **not** link `recomp_none.c` into the app.

## Host contract (mirror `platform/apple/SotaEngine.m`)

```c
amiga_init();
amiga_load_adf_memory(bytes, 901120);
audio_set_rate(48000);
amiga_set_frame_callback(on_frame);   // called on the emulation thread at each frame end
recomp_register_all();                // from build/gen/registry.c
exec_use_recomp = true;
amiga_boot_hle();
loop: exec_run_frames(1); pace to 49.92 Hz (3546895 / (227*313))
```

* Frame: `framebuffer[FB_H][FB_W]`, 0xAARRGGBB (= BGRA bytes). Visible crop used on Apple:
  x 194..897, y 28..311 (704x284), shown with lines doubled (aspect 704:568).
* Audio: `audio_pull(float *interleaved_stereo, frames)` is lock-free SPSC; call it from the
  audio thread. Nudge pacing with `audio_available()` (Apple keeps ~70 ms buffered).
* Input: `amiga_set_mouse_button(left, right)`.
* All runtime state is global: one instance per process; stop the thread before `amiga_init()`.

## MSVC notes

* Needs C11 (`/std:c11`) and `<stdatomic.h>` (VS 2022 17.5+: `/experimental:c11atomics`)
  for `runtime/amiga/audio.c`; or swap the two atomics for `InterlockedExchange`.
* No GCC builtins/attributes remain in `runtime/`.
* `build/gen` is ~90 MB of C in 26 files; compile with `/O2` and expect a few minutes.

## Verify

Frame-hash parity (`README.md`, "Verification") must hold on Windows as well: the runtime is
deterministic, so the interpreter and recompiled hash files should match the Mac's too.
