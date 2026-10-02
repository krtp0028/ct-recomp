# Third-party code and data

ct-recomp is MIT-licensed (`Copyright (c) 2026 Akhil Moola`, see `LICENSE`).
This file tracks third-party sources used by the project and their terms.

This repository is a fork of
[ThisIsAkill/ct-recomp](https://github.com/ThisIsAkill/ct-recomp); the upstream
copyright and MIT license are preserved, and fork changes (Windows and PSP
ports, native-dispatch fixes, PPU work) are contributed under the same terms.

## Currently in use

### Symbol names and entry-state hints

- **dscotton/ct_disassembly** — public domain. Function labels, boundaries,
  and comments are usable directly. Where they conflict with ChronoRET,
  ChronoRET wins.
- **ChronoRET** (this project's own matching disassembly, `../ChronoRET`) —
  source of truth for names and verified entry states.

No files from either source are vendored verbatim; `tools/sync_symbols.py`
reads them to populate `funcs.toml` / `unresolved.toml`.

### Hints only, not copied

- **DocDamage/chronotrigger_disassembly** (MIT) — used only as a hint when
  labeling routines. Anything actually copied from it must be verified
  against our own decoder and keep its MIT notice here.

### Debugging tools, never copied from

- **bsnes**, **Mesen2** (GPL), **snes9x** (non-free) — used only as external
  debuggers/tracers during development. No code from any of them is in this
  repository.

### `third_party/ares/`

- **ares-emulator/ares**, `ares/component/processor/spc700/`, commit
  `4cb8d92b441557cb6bcaf133c4cbc7f6819b1122` (ISC, "ares team, Near et al"; the repository's
  `LICENSE` is copied whole as `third_party/ares/LICENSE`) — the
  cycle-accurate SPC700 CPU core, vendored unmodified. `runtime/spc700_host.cpp`
  compiles it (without its disassembler and serializer) against a small
  `nall` stand-in (`runtime/nall_shim.hpp`, our code) and drives it against
  the zelda3 APU's memory map, DSP and timers. Details in
  `third_party/ares/README.md`.

### `third_party/snes/`

- **snesrev/zelda3**, `snes/` subtree, commit `fbbb3f967a51fafe642e6140d0753979e73b4090`
  (MIT, snesrev + elzo_d) — PPU, DMA/HDMA, APU, SPC700, and DSP emulation.
  Full license text and file list in `third_party/snes/README.md`.
  `runtime/snes_adapter.c` bridges it into `bus.c`; `dma.c`/`dma.h` are
  otherwise unmodified. Local patches to `ppu.c`/`ppu.h` (all marked
  `ct-recomp:` in place):
  - `OBSEL` ($2101): upstream asserted one fixed value and never actually
    decoded it (`objSize`/`objTileAdr1/2` were hardcoded in `ppu_reset`).
    Implemented the real decode.
  - `BGMODE` ($2105) bit 3 (BG3 priority): upstream hardcoded "always on
    for mode 1" instead of reading the bit. Added `Ppu.bg3Priority` and
    read it for real.
  - `BGMODE` ($2105) bits 4-7 (BG character size): upstream never decoded
    them (A Link to the Past uses 8x8 BG tiles only). Added
    `BgLayer.bigTiles`; the per-pixel BG fetch handles 16x16 characters
    per fullsnes.
  - Asserts on `BGMODE`, `M7SEL`, `VMAIN`, `WBGLOG`/`WOBJLOG`, `CGWSEL`,
    `SETINI`, `OAMADDH` restricted every value to what A Link to the Past
    happens to use. Relaxed so Chrono Trigger's actual register writes
    don't abort; several of the underlying features (window AND/XOR/XNOR
    logic, VRAM address remapping, direct color mode, interlace/hi-res/
    overscan) are still genuinely unimplemented, not just untested --
    see issue #9's gap report for the full list.
  - Offset-per-tile (BG modes 2, 4, 6): upstream never implemented it (A
    Link to the Past doesn't use it). Added `ppu_offsetPerTile` and
    `ppu_bg3TilemapWord`, following fullsnes.
  - `CGDATA` ($2122): bit 7 of the high byte is no longer stored (CGRAM is
    15-bit).
  - Brightness: each 5-bit channel is scaled by brightness / 15 before it
    is expanded to 8 bits (Mesen 2); upstream scaled the expanded value.
  - Lines are drawn as they go: `ppu_runLine` sets a line up and
    `ppu_drawTo` draws it up to a pixel, so a register write partway
    through a line (the adapter draws up to the write's dot first) changes
    only the rest of it. Upstream drew each line whole at its start.

  Local patches to `apu.c`/`apu.h`: `apu_tick` (the DSP, timer and cycle
  count half of `apu_cycle`, split out so the SPC700 core in
  `third_party/ares/` can drive it per cycle); `apu_inport_read`, the hook
  the SPC700's $F4-$F7 reads go through; and (marked `ct-recomp:`) the
  timers' stage-1 clock, which falls every 128 cycles (16 for timer 2) from
  reset, first at the end of cycle 128 (bsnes/ares), where upstream ticked
  at cycle 0 and every period from there.

## Auditing

`git ls-files` plus a size scan of this repo are run periodically to confirm
no ROM data or ROM-derived binary assets are tracked. See the setup report
for the most recent audit.
