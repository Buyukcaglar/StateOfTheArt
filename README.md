# State of the Art — static recompilation

Native macOS and iOS builds of *State of the Art* (Spaceballs, 1992, Amiga 500 OCS),
produced by statically recompiling the demo's 68000 code to C and running it on a
cycle-driven A500 chipset runtime. No Kickstart ROM and no emulated CPU are needed at
run time: the boot is high-level, and every block the demo executes is compiled ahead
of time (the interpreter remains only as a safety net for code bytes never seen).

![State of the Art running on the recompiled runtime (frame 2622)](docs/images/screenshot.png)

It runs the **original release** from The Party 1992 (`spb-sota.dms` on scene.org,
converted to ADF), not the later cracked/fixed re-releases. That version decodes data above
$80000 and therefore needs **1 MB of chip RAM** (an A500 with the 8372 "Fat Agnus"); on a
512K-chip machine the loader overwrites the interrupt vectors and crashes. The runtime models
1 MB chip + 512K slow RAM (`CHIP_SIZE` in `runtime/amiga/amiga.h`).

The disk image (`_reference/StateOfTheArt.adf`) and everything derived from it
(`build/coverage.txt`, `build/gen/`) stay local; see `.gitignore`.

## Layout

| Path | Contents |
|---|---|
| `runtime/cpu` | 68000 decoder, shared instruction semantics (`m68k_sem.h`), interpreter, disassembler |
| `runtime/amiga` | A500 runtime: memory map, Agnus/Copper/bitplanes/Denise/sprites (`custom.c`), blitter, Paula audio, CIAs, floppy + MFM, HLE boot |
| `runtime/core/exec.c` | Block executor shared by all modes, recompiled-block registry and code-identity checks, coverage |
| `tools/lift` | Static recompiler: coverage → C (`build/gen/*.c`) |
| `tools/runner` | Headless runner: PNG frame dumps, WAV, frame hashes, coverage, block traces |
| `platform/apple` | Shared host: emulation thread + pacing, AVAudioEngine output, Metal renderer |
| `platform/macos`, `platform/ios` | App shells and asset catalogs (icon = frame 2622) |
| `export/` | 75 s frame dump, icon candidates |

## How it works

* **One semantics, two executors.** The decoder produces an `Insn`; the interpreter executes
  it with the `sem_*` helpers, and the lifter emits C that calls the same helpers with
  constant operands, mirroring the interpreter case by case, including memory access order
  and cycle accounting.
* **Blocks and timing.** Code runs in basic blocks; interrupts are taken only at block
  boundaries in every mode, and custom-chip/CIA accesses first bring the chipset up to
  the exact CPU cycle. Interpreted and recompiled runs therefore see identical I/O timing.
* **Code identity.** A recompiled block runs only while its original bytes are in memory
  (256-byte page write generations, then `memcmp`). A block also ends as soon as any of its
  code pages is written, in both modes, which keeps self-modifying code exact
  (the demo patches immediates inside running blocks).
* **Chipset.** Colour-clock stepped: DMA slots (sprites, bitplanes, copper), Denise pixel
  pipeline with BPLCON1 delay, dual playfield, HAM/EHB, sprite priorities, blitter
  (area, fill, line mode — line mode writes D regardless of USED, as hardware does),
  Paula audio with A500 RC + LED filters, 8520 timers/TOD, and DF0: with real MFM
  streaming and DSKSYNC.

## Verification

* `build/sota_run` (interpreter) vs `build/sota_run_rc` (recompiled): **all 13,000 frames
  (the whole demo, ~4 min 20 s) hash identical**, with 100 % of executed blocks recompiled.
* Section-by-section comparison against WinUAE (cycle-exact 68000, OCS) screenshots taken
  through the ALayer harness, during development on a re-release of the same demo.

## Build

```sh
make                         # host tools: build/sota_run, build/dis
tools/build_apple.sh         # coverage (if missing) -> lift -> StateOfTheArt.xcodeproj
xcodebuild -project StateOfTheArt.xcodeproj -scheme StateOfTheArt-macOS -configuration Release \
           -derivedDataPath build/dd -allowProvisioningUpdates build
xcodebuild -project StateOfTheArt.xcodeproj -scheme StateOfTheArt-iOS -configuration Release \
           -destination 'generic/platform=iOS' -derivedDataPath build/dd -allowProvisioningUpdates build
```

Parity check after touching the runtime or lifter:

```sh
./build/sota_run --frames 13000 --coverage build/coverage.txt --quiet --hashes build/h_i.txt
make gen build/sota_run_rc
./build/sota_run_rc --frames 13000 --quiet --hashes build/h_r.txt
diff <(awk '{print $1,$2}' build/h_i.txt) <(awk '{print $1,$2}' build/h_r.txt)
```

Any change to block boundaries (`m68k_ends_block`, the page rule) requires a fresh coverage run.

## Controls

macOS: ⌘R restart, ⌘P pause, ⇧⌘M mute, ⌃⌘F full screen, ⌘0 2× size; click or Space = mouse button.
iOS/iPadOS: tap toggles aspect fit (letterboxed) and aspect fill (cropped, never stretched), remembered between launches; in fill mode drag pans the cropped picture (tap back to fit recentres); pauses when the app is inactive.

## Known limitations

* CPU timing ignores chip-bus contention (bitplane/blitter DMA stealing CPU cycles), so the
  68000 is slightly faster than real hardware; the demo is frame-synchronised and shows no
  visible difference against WinUAE.
* Not modelled: sprite/playfield collisions (CLXDAT reads as no collision), keyboard,
  serial, disk writes, ECS/AGA features.
* The demo ends on its static end screen; 10 s later it starts again (also on macOS).

## License

The code in this repository is MIT licensed (see `LICENSE`). *State of the Art* itself and
images taken from it belong to Spaceballs and are not covered by that license.
