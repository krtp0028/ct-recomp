#!/usr/bin/env python3
"""recomp/emit.py batch_runs: which straight-line register-only sequences may
be grouped into one ct_insn_run, and which must stay per-instruction (targeted
intermediate labels, bus/control-flow opcodes, bank or M/X changes, bodies
with their own control flow). Grouping only affects how the scheduler is
called; the correctness of the batched accounting itself is covered by
diff_all and ct_boot."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))
import emit  # noqa: E402

fails = 0
checks = 0


def check(ok, what):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print('FAIL', what)


def insn(addr, op, body='{ nop(cpu); }'):
    return [f'L_{addr:06X}: /* op */',
            f'    ct_insn(cpu, 0x{addr:06X}, 0x{op:02X}, 0x{op:02X});',
            f'    {body}']


def grouped(lines):
    return sum(1 for ln in lines if ln.strip().startswith('ct_insn_run('))


# Two adjacent register-only instructions with unreferenced labels group.
lines = ['void f(CPU *cpu)', '{'] + insn(0x010000, 0xEA) + insn(0x010001, 0xEA) + ['}']
out = emit.batch_runs(lines)
check(grouped(out) == 1, 'adjacent register-only ops group')
check(sum(1 for ln in out if 'ct_insn(' in ln) == 0, 'member ct_insn calls are replaced')

# A jump into the middle of the run must block grouping (the batch call
# would be skipped, losing that member's charge).
lines = (['void f(CPU *cpu)', '{', '    if (cpu->n) { goto L_010001; }']
         + insn(0x010000, 0xEA) + insn(0x010001, 0xEA) + ['}'])
check(grouped(emit.batch_runs(lines)) == 0, 'targeted intermediate label blocks grouping')

# A bus-addressing opcode is not register-only.
lines = ['void f(CPU *cpu)', '{'] + insn(0x010000, 0xEA) + insn(0x010001, 0x0D) + ['}']
check(grouped(emit.batch_runs(lines)) == 0, 'bus opcode blocks grouping')

# One fetch speed per run: different 64K banks never group.
lines = ['void f(CPU *cpu)', '{'] + insn(0x010000, 0xEA) + insn(0x020000, 0xEA) + ['}']
check(grouped(emit.batch_runs(lines)) == 0, 'different banks block grouping')

# A member body with its own control flow is not plain register work.
lines = (['void f(CPU *cpu)', '{']
         + insn(0x010000, 0xEA, body='{ goto L_010001; }') + insn(0x010001, 0xEA) + ['}'])
check(grouped(emit.batch_runs(lines)) == 0, 'body with control flow blocks grouping')

# A single register-only instruction is left exactly as it was.
lines = ['void f(CPU *cpu)', '{'] + insn(0x010000, 0xEA) + ['}']
check(grouped(emit.batch_runs(lines)) == 0, 'single instruction is not batched')

print('batch_runs: %d checks, %d failed' % (checks, fails))
sys.exit(1 if fails else 0)
