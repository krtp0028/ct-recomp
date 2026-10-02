#!/usr/bin/env python3
"""tools/lockstep.py against a fake probe: identical runs pass; a run whose
interpreted side changes WRAM $0123 from frame 5 on is reported at frame 5
with the component (wram) and the differing range."""
import os
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "lockstep.py")

FAKE = r'''#!/usr/bin/env python3
import sys
a = sys.argv[1:]
frames = int(a[a.index("--frames") + 1])
interp = "--interp-only" in a
diverge = "--diverge" in a
bad = lambda f: diverge and interp and f >= 5
if "--hash-log" in a:
    with open(a[a.index("--hash-log") + 1], "w") as out:
        for f in range(1, frames + 1):
            w = "2" if bad(f) else "1"
            out.write(f"{f} cpu=0 wram={w} sram=0 vram=0 cgram=0 oam=0 frame=0 apu=0\n")
wram = bytearray(0x20000)
if bad(frames):
    wram[0x123] = 0x42
if "--wram" in a:
    open(a[a.index("--wram") + 1], "wb").write(wram)
if "--vram" in a:
    open(a[a.index("--vram") + 1], "wb").write(bytes(0x10000 + 0x200 + 0x220))
'''

fails = 0
with tempfile.TemporaryDirectory() as tmp:
    probe = os.path.join(tmp, "probe")
    if os.name == "nt":
        # Windows cannot execute a shebang script: wrap it in a .cmd.
        py = probe + ".py"
        open(py, "w").write(FAKE)
        probe += ".cmd"
        open(probe, "w").write(f'@echo off\r\n"{sys.executable}" "{py}" %*\r\n')
    else:
        open(probe, "w").write(FAKE)
        os.chmod(probe, os.stat(probe).st_mode | stat.S_IEXEC)
    r = subprocess.run([sys.executable, TOOL, probe, "--frames", "10"], capture_output=True,
                       text=True)
    if r.returncode != 0 or "10 frames identical" not in r.stdout:
        fails += 1
        print("FAIL identical runs:", r.stdout)
    r = subprocess.run([sys.executable, TOOL, probe, "--frames", "10", "--diverge"],
                       capture_output=True, text=True)
    if (r.returncode != 1 or "first divergent frame 5: wram differ" not in r.stdout
            or "$00123-$00123 native 00 interp 42" not in r.stdout):
        fails += 1
        print("FAIL divergence report:", r.stdout)
print(f"lockstep: {'ok' if not fails else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)
