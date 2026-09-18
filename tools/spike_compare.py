#!/usr/bin/env python3
"""RV32IM smoke-test differential verification against Spike, not MARSS.

Spike executes the same ELF's text as the DUT raw image. The pinned Spike
reset ROM retires five instructions; this is checked rather than silently
discarding arbitrary low-address events. The 33 test commits are all compared.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CPU = ROOT.parent / 'cpu'
HEADER = re.compile(r'^core\s+(\d+):\s+([0-3])\s+(0x[0-9a-fA-F]+)\s+\((0x[0-9a-fA-F]+)\)(.*)$')
REG = re.compile(r'\s*x\s*(\d+)\s+(0x[0-9a-fA-F]+)')
MEM = re.compile(r'\s*mem\s+(0x[0-9a-fA-F]+)(?:\s+(0x[0-9a-fA-F]+))?')
FIELDS = ('pc', 'insn', 'wen', 'rd', 'wdata', 'mem_addr', 'mem_wmask', 'mem_wdata', 'trap', 'cause')
RESET_PCS = [0x1000, 0x1004, 0x1008, 0x100c, 0x1010]
EXPECTED_COUNT = 33


def parse_commit(pc, insn, body):
    """Strictly parse RV32IM integer commit records; no guessed store values."""
    if not 0 <= insn <= 0xffffffff or insn & 3 != 3:
        raise ValueError('expected a 32-bit instruction')
    event = dict(type='commit', pc=pc, insn=insn, wen=0, rd=0, wdata=0,
                 mem_addr=0, mem_wmask=0, mem_wdata=0, trap=False, cause=0)
    reg_seen = mem_seen = False
    body = body.strip()
    while body:
        match = REG.match(body)
        if match:
            if reg_seen:
                raise ValueError('multiple register writes are unsupported')
            reg_seen = True
            rd, value = int(match[1]), int(match[2], 16)
            if not 0 <= rd < 32 or value > 0xffffffff:
                raise ValueError('invalid RV32 register write')
            if rd:
                event.update(wen=1, rd=rd, wdata=value)
        else:
            match = MEM.match(body)
            if not match or mem_seen:
                raise ValueError(f'unsupported Spike commit fields: {body!r}')
            mem_seen = True
            address = int(match[1], 16)
            if address > 0xffffffff:
                raise ValueError('invalid RV32 address')
            event['mem_addr'] = address
            if match[2] is not None:
                f3 = (insn >> 12) & 7
                if insn & 0x7f != 0x23 or f3 > 2:
                    raise ValueError('memory write is not SB/SH/SW')
                width = 1 << f3
                value = int(match[2], 16)
                if value >= 1 << (width * 8):
                    raise ValueError('store value exceeds instruction width')
                event.update(mem_wmask=(1 << width) - 1, mem_wdata=value)
            elif insn & 0x7f != 3:
                raise ValueError('memory read is not a load')
        body = body[match.end():].strip()
    opcode = insn & 0x7f
    if opcode == 0x23 and not event['mem_wmask']:
        raise ValueError('store is missing its address/data')
    if opcode == 3 and not mem_seen:
        raise ValueError('load is missing its address')
    writes = opcode in (0x37, 0x17, 0x6f, 0x67, 3, 0x13, 0x33)
    rd = (insn >> 7) & 31
    if event['wen'] != int(writes and rd != 0) or (event['wen'] and event['rd'] != rd):
        raise ValueError('missing/unexpected destination register')
    return event


def read_spike(path, stop):
    raw = []
    for line_no, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        m = HEADER.fullmatch(line.strip())
        if not m:
            raise ValueError(f'{path}:{line_no}: unrecognized commit log line: {line}')
        if int(m[1]) != 0 or int(m[2]) != 3:
            raise ValueError('expected hart 0 in M mode')
        raw.append((int(m[3], 16), int(m[4], 16), m[5]))
    if [row[0] for row in raw[:5]] != RESET_PCS:
        raise ValueError('Spike reset ROM changed; expected exactly five known boot PCs')
    events = [parse_commit(*row) for row in raw[5:]]
    if len(events) != EXPECTED_COUNT or events[-1]['pc'] != stop:
        raise ValueError('reference is truncated, has extra events, or missed test_done')
    return events


def canonical(e):
    if e.get('type') != 'commit' or e.get('trap', False):
        raise ValueError('this RV32IM smoke test requires normal retirement, not traps')
    result = {}
    for key in FIELDS:
        if key not in e:
            raise ValueError(f'missing required field: {key}')
        value = e[key]
        result[key] = int(value, 0) if isinstance(value, str) else int(value)
    if not result['wen']:
        result['rd'] = result['wdata'] = 0
    if not result['mem_wmask']:
        result['mem_wdata'] = 0
    # Unlike the old comparator, retain the effective load address too.
    if result['insn'] & 0x7f not in (3, 0x23):
        result['mem_addr'] = 0
    return result


def compare(reference, dut):
    if not reference or len(reference) != len(dut):
        raise ValueError(f'event_count: reference={len(reference)}, DUT={len(dut)}')
    for index, (ref, actual) in enumerate(zip(reference, dut), 1):
        a, b = canonical(ref), canonical(actual)
        differences = {k: {'reference': a[k], 'dut': b[k]} for k in FIELDS if a[k] != b[k]}
        if differences:
            raise ValueError(f'commit #{index}, PC={a["pc"]:#010x}: {differences}')


def run(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, timeout=60, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sim', type=Path, default=CPU / 'build/rv32ima-increment/sim')
    parser.add_argument('--spike', type=Path, default=Path(shutil.which('spike') or CPU / 'build/spike/spike'))
    parser.add_argument('--out', type=Path, default=ROOT / 'out/spike-compare')
    parser.add_argument('--cross', default='riscv64-unknown-elf-')
    args = parser.parse_args()
    sim, spike, out = args.sim.resolve(), args.spike.resolve(), args.out.resolve()
    for executable in (sim, spike):
        if not executable.is_file():
            parser.error(f'executable missing: {executable}; see docs/spike-diff.md')
    out.mkdir(parents=True, exist_ok=True)
    # 模拟器直接装载 ELF 并按符号名停机，无需再转出裸二进制。
    elf = out / 'smoke.elf'
    run([args.cross + 'gcc', '-march=rv32im', '-mabi=ilp32', '-nostdlib', '-nostartfiles',
         '-Wl,--no-relax', '-T', ROOT / 'programs/link.ld', ROOT / 'programs/diff_smoke.S', '-o', elf])
    # nm 仍用于校验 Spike 侧轨迹确实停在 test_done。
    symbols = run([args.cross + 'nm', elf], capture_output=True, text=True).stdout
    stop = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1] == 'test_done')
    dut_file = out / 'dut.jsonl'
    run([sim, elf, '--trace-json', dut_file, '--stop-pc', 'test_done'])
    dut = [json.loads(line) for line in dut_file.read_text().splitlines()]

    env = os.environ.copy()
    deps = CPU / 'build/spike-deps/usr'
    if (deps / 'bin/dtc').is_file():
        env['PATH'] = str(deps / 'bin') + os.pathsep + env.get('PATH', '')
        env['LD_LIBRARY_PATH'] = str(deps / 'lib/x86_64-linux-gnu') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    raw = out / 'spike-commit.log'
    # Static test length, independent of DUT's trace count. Stop before the
    # MARSS-specific HTIF shutdown sequence. No proxy kernel is involved.
    argv = [spike, '--isa=RV32IM_Zicsr', '-m0x80000000:0x100000',
            '--log-commits', f'--log={raw}', f'--instructions={5 + EXPECTED_COUNT}', elf]
    with (out / 'spike-console.log').open('w') as console:
        run(argv, env=env, stdin=subprocess.DEVNULL, stdout=console, stderr=subprocess.STDOUT)
    reference = read_spike(raw, stop)
    (out / 'reference-commit.jsonl').write_text(''.join(json.dumps(e) + '\n' for e in reference))
    compare(reference, dut)
    # Negative check exercises the same comparator used for the real traces.
    wrong = [dict(e) for e in dut]
    wrong[1]['wdata'] = hex(int(wrong[1]['wdata'], 0) ^ 1)
    try:
        compare(reference, wrong)
    except ValueError:
        pass
    else:
        raise ValueError('negative test did not detect wrong writeback')
    (out / 'run.json').write_text(json.dumps(dict(reference='Spike', command=[str(x) for x in argv],
        sim=str(sim), commits=len(dut), result='PASS'), indent=2) + '\n')
    print(f'SPIKE DIFFTEST PASS: {len(dut)} retirement events matched')
    print('Compared PC, instruction, register writeback, load address, memory writes; negative check PASS')
    print(f'Artifacts: {out}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError, StopIteration) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        sys.exit(1)
