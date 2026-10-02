# ct-recomp

[![ci](https://github.com/ThisIsAkill/ct-recomp/actions/workflows/ci.yml/badge.svg)](https://github.com/ThisIsAkill/ct-recomp/actions/workflows/ci.yml)

Static recompiler for Chrono Trigger (SNES, US 1.0) 65816 code to C, plus a C runtime.
The recompiler and runtime are game-agnostic; everything specific to Chrono
Trigger lives in `game/ct/`.

> **Fork note:** this is a fork of
> [ThisIsAkill/ct-recomp](https://github.com/ThisIsAkill/ct-recomp) maintained at
> [krtp0028/ct-recomp](https://github.com/krtp0028/ct-recomp). It adds the
> Windows and PSP ports, native-dispatch fixes and PPU rendering work.
> Upstream code stays MIT (`Copyright (c) 2026 Akhil Moola`); fork changes are
> MIT as well. See `THIRD_PARTY.md` for the full credits.

Planned features after boot (MSU-1, achievements, save states, widescreen and
more) are listed in [ROADMAP.md](ROADMAP.md).

## Progress

<!-- progress:start -->

**69.4% recompiled**: 1414 of 2036 known functions are compiled to C.

- Native at runtime: 97.6% of the instructions in a 1500-frame boot run as compiled C (the rest run in the interpreter).
- Milestones: 3 of 7 closed.

Updated by `tools/progress.py` with every push; details in [game/ct/PROGRESS.md](game/ct/PROGRESS.md).

<!-- progress:end -->

## Layout

- `recomp/` — the translator (Python): decoder with static M/X tracking, C emitter.
- `runtime/` — CPU, bus, cycle model, interpreter, frame scheduler; the vendored
  PPU/DMA/APU core lives in `third_party/snes/`.
- `frontend/` — SDL2 frontend; `tools/ct_boot.c` — headless boot probe.
- `test/` — engine tests; they run on a synthetic ROM generated at build time.
- `game/ct/` — everything specific to Chrono Trigger: `game.toml` (ROM identity),
  `funcs.toml` / `unresolved.toml` (symbols), extern hooks, game tools, the
  ROM-dependent tests, and `PROGRESS.md`.

## Build

Requirements: CMake 3.20+, a C11 compiler, Python 3.11+, SDL2 (optional, for
`ct_sdl`).

Engine only (no ROM needed; what CI runs):

```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build -LE rom
```

With the game. Supply your own headerless US 1.0 ROM; it is checked against
`game/ct/game.toml` at configure time:

```
export CT_ROM=/path/to/chrono_trigger.sfc   # headerless, 4 MB
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

This builds `game/ct` (generated C goes to `build/game/ct/out/`) and two
programs at the top of `build/`. For investigations, a Debug tree builds in
about 30 seconds (runtime at `-Og`, generated code at `-O1`, see `CT_GEN_OPT`):
`cmake -S . -B build-dbg -DCMAKE_BUILD_TYPE=Debug`. The build uses ccache when
it is installed.

```
build/ct_sdl                           # play: window, audio, keyboard/controller
mkdir -p shots && build/ct_boot --frames 1500 --dump shots   # headless: PNGs of what changed
```

`ct_sdl` keys: arrows, Z=B, X=A, A=Y, S=X, Q=L, W=R, Enter=Start, Right Shift=Select;
a game controller works too. Ctrl+Q or closing the window quits.
`ct_sdl --record FILE` saves your pad input as a script; `ct_boot --script FILE
--frames N` replays it exactly (N is on its last line), and `ct_sdl --script
FILE` plays it back in the window. A `reset F` line in a script presses the reset button as
frame F begins. `ct_boot` also takes `--input F1-F2:BUTTONS`
to script the pad, `--wav FILE` for audio, `--hash-log FILE` for per-frame
state hashes (`tools/lockstep.py` compares native and interpreted runs with
them), and `--needed-hw FILE` to report where emulation stops.

Reference comparison: `tools/ref_compare.py --probe build/ct_boot --mesen PATH
--rom $CT_ROM --frames N [--script FILE]` runs the same input in
[Mesen 2](https://github.com/SourMesen/Mesen2) (used as a tool, headless under
`xvfb-run`, with a private settings folder) and reports the first frame where
WRAM or the picture differs, with the differing WRAM ranges.
`tools/trace_diff.py --probe build/ct_boot --mesen PATH --rom $CT_ROM --from F
--to G [--script FILE]` runs both untraced up to frame F and compares every
instruction of frames F-G (address, clock, X, Y), reporting the first
difference; about a minute for a window 12,000 frames in.
`tools/tas_convert.py MOVIE OUT` turns a TASVideos BizHawk `.bk2` or lsnes
`.lsmv` movie (from power-on) into an input script for both, resets included. Replays of a new
game into Leene Square and into the first battle are in `game/ct/test/replay/`.

The ROM is never committed, and CI never sees it.

Symbol names partly derived from dscotton/ct_disassembly (public domain) and ChronoRET.

## License

This code is MIT-licensed (see `LICENSE`). The Chrono Trigger ROM and its
assets are **not included** and remain the property of their copyright
holders — you must supply your own legally obtained ROM via `$CT_ROM`.
Symbol names are credited to dscotton/ct_disassembly (public domain) and
ChronoRET. See `THIRD_PARTY.md` for vendored/adapted third-party code.
