# psx-rt — Realtime Ray Tracing on PlayStation 1 hardware constraints

This project demonstrates true Whitted-style ray tracing (primary rays +
reflective bounce + shadow rays) computed entirely in software on the PS1's
R3000A CPU at 33.8688 MHz — no FPU, all fixed-point Q16.16 math — plus a
PlayStation emulator written completely from scratch to run it.

## Layout

- `demo/`        — the ray tracing demo (PSn00bSDK homebrew, builds `psxrt.exe`)
- `game/`        — **REFLECTOR**, a complete ray-traced 3D dodge game (`game.exe`)
- `emulator/`    — "oxpsx", a from-scratch PS1 emulator (C + SDL2)
- `cputest/`     — CPU instruction test suite (validates the emulator)
- `toolchain/`   — prebuilt mipsel-none-elf GCC 12.3 + PSn00bSDK 0.24

## The game (game/main.c)

**REFLECTOR** — you are a mirror sphere on an infinite checkered plane.
Obstacle spheres rush toward you; steer with LEFT/RIGHT, START to begin or
restart after crashing. Score and best score are tracked.

Every pixel is fully ray traced each frame: primary rays against all
spheres + the plane, shadow rays toward the sun (obstacles cast shadows on
the ground and on each other), a reflective bounce on the player's mirror
sphere (you can see the obstacles and sky in it), and distance fog. The
title, HUD and game-over text are drawn into the framebuffer by a custom
5x7 font, then the whole frame is DMA'd to VRAM.

Runs at roughly 3-5 fps on real hardware timing — honest software ray
tracing on a 33.8 MHz CPU without a floating point unit.

## The demo (demo/main.c)

True ray tracing under real hardware constraints:

- **No FPU**: all math is 32-bit fixed point (Q16.16). Multiplies use the
  R3000A `mult` instruction via inline asm; divides use the hardware `div`
  where needed; sqrt is an integer bit-by-bit algorithm.
- **Per-pixel rays**: primary ray per pixel, sphere + infinite plane
  intersections, one reflective bounce on the mirror sphere, shadow rays
  toward the light, distance fog, checkered plane.
- **Output**: finished frame is DMA'd straight into VRAM (GP0 A0 transfer),
  double-buffered, 128x96 RT image centered in a 320x240 15-bit screen.
- Performance on real hardware timing: ~400 cycles/pixel → roughly 8-10 fps
  at 128x96 — genuine realtime software ray tracing on 1994 hardware.

## The emulator (emulator/src)

Written from scratch, no existing emulator code:

- `cpu.c`     — full R3000A interpreter (all loads/stores incl. LWL/LWR/SWL/SWR,
                mult/div, branch delay slots, COP0 exceptions/RFE)
- `hle.c`     — minimal bootstrap kernel (dispatch tables 0xA0/0xB0/0xC0,
                EnterCriticalSection syscalls, HookEntryInt/ReturnFromException
                context switching used by PSn00bSDK's interrupt dispatcher)
- `gpu.c`     — GPU: VRAM, GP0/GP1 state machines, VRAM fill + CPU/DMA transfers
- `dma.c`     — all 7 DMA channels (burst/block/linked-list/OTC) + IRQ controller
- `timers.c`  — 3 root counters with hblank sync
- `cdrom.c`   — minimal CDROM controller (BIOS probe responses)
- `main.c`    — SDL2 host, PS-EXE loader, frame dumps

## Build & run

```bash
# demo
cd demo
export PSN00BSDK_LIBS=../toolchain/psn00bsdksdk/lib/libpsn00b
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PSN00BSDK_LIBS/cmake/sdk.cmake \
  -DPSN00BSDK_TC=../toolchain/gcc-mipsel-none-elf \
  -DCMAKE_BUILD_TYPE=Release
ninja -C build   # produces build/psxrt.exe

# emulator
cd ../emulator && make

# run the game (windowed; arrows = steer, Enter = start)
./oxpsx --exe ../game/build/game.exe

# run (windowed)
./oxpsx --exe ../demo/build/psxrt.exe

# headless with frame dumps (PPM)
./oxpsx --exe ../demo/build/psxrt.exe --headless --frames 400 \
    --dump /tmp/frame
```

The demo also runs in DuckStation (System → Load EXE) and on real PS1
hardware via the standard PS-EXE loading methods.
