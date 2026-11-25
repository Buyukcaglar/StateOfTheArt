# Host build of tools (the apps use CMake/Xcode).
CC ?= clang
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
RT = runtime/cpu/m68k_decode.c runtime/cpu/m68k_interp.c runtime/cpu/m68k_dasm.c \
     runtime/amiga/memory.c runtime/amiga/custom.c runtime/amiga/blitter.c runtime/amiga/cia.c \
     runtime/amiga/floppy.c runtime/amiga/audio.c runtime/amiga/machine.c runtime/core/exec.c
GEN = $(wildcard build/gen/*.c)

all: build/sota_run build/dis

build/sota_run: $(RT) tools/runner/sota_run.c tools/runner/png.h $(wildcard runtime/*/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(RT) tools/runner/sota_run.c runtime/core/recomp_none.c -lz

build/dis: tools/dis/dis.c runtime/cpu/m68k_decode.c runtime/cpu/m68k_dasm.c
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: all

build/lift: tools/lift/lift.c runtime/cpu/m68k_decode.c runtime/cpu/m68k_dasm.c runtime/cpu/m68k.h
	$(CC) $(CFLAGS) -o $@ tools/lift/lift.c runtime/cpu/m68k_decode.c runtime/cpu/m68k_dasm.c

# coverage -> generated C (local only: derived from the demo's code)
gen: build/lift
	@rm -rf build/gen && mkdir -p build/gen
	build/lift build/coverage.txt build/gen

RT_NOSTUB = $(RT)
build/sota_run_rc: $(RT) tools/runner/sota_run.c build/gen/registry.c
	$(CC) $(CFLAGS) -Wno-unused-variable -Wno-unused-but-set-variable -o $@ $(RT) tools/runner/sota_run.c build/gen/*.c -lz
