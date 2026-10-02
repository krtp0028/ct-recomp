#!/usr/bin/env python3
"""Build step for overlays (#92): decompress each blob listed in
game/ct/overlays.toml from the ROM into OUT_DIR/<name>.bin, check it against
the recorded FNV-1a 64 hash (a mismatch means the decompressor port or the
list is wrong: fail the build), and write OUT_DIR/manifest.toml, the
game-agnostic list recomp/emit.py --overlays reads:

  [[overlay]]  name, dst (24-bit WRAM address), image (path), and
  [[overlay.entry]]  addr, states

Everything written is derived from the ROM: it stays in the build tree.

usage: overlay_images.py OVERLAYS_TOML OUT_DIR
"""
from __future__ import annotations

import os
import sys
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..',
                                'recomp'))
import ct_decompress  # noqa: E402
import decode  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    with open(argv[0], 'rb') as f:
        listing = tomllib.load(f)
    out = argv[1]
    os.makedirs(out, exist_ok=True)
    rom = decode.load_rom()
    manifest = ['# Written by game/ct/tools/overlay_images.py (derived from the ROM).', '']
    for ov in listing.get('overlay', []):
        data = ct_decompress.decompress(rom, ov['src'])
        got = f'{ct_decompress.fnv64(data):016x}'
        if len(data) != ov['size'] or got != ov['fnv64']:
            print(f'overlay_images: {ov["name"]}: {len(data):#x} bytes, fnv64 {got}; '
                  f'overlays.toml says {ov["size"]:#x}, {ov["fnv64"]}', file=sys.stderr)
            return 1
        path = os.path.join(out, f'{ov["name"]}.bin')
        old = open(path, 'rb').read() if os.path.exists(path) else None
        if old != data:
            with open(path, 'wb') as f:
                f.write(data)
        # TOML has no raw backslashes in basic strings; forward slashes work
        # for the open() in recomp/emit.py too.
        manifest += ['[[overlay]]', f'name = "{ov["name"]}"', f'dst = 0x{ov["dst"]:06X}',
                     f'image = "{path.replace(chr(92), "/")}"']
        for e in ov.get('entry', []):
            states = ', '.join(f'"{s}"' for s in e['states'])
            manifest += ['[[overlay.entry]]', f'addr = 0x{e["addr"]:06X}', f'states = [{states}]']
        manifest.append('')
    text = '\n'.join(manifest)
    path = os.path.join(out, 'manifest.toml')
    if not os.path.exists(path) or open(path).read() != text:
        with open(path, 'w') as f:
            f.write(text)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except ct_decompress.DecompressError as ex:
        print(f'overlay_images: {ex}', file=sys.stderr)
        sys.exit(1)
