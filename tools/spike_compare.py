#!/usr/bin/env python3
"""RV32IM 冒烟测试的 Spike 差分验证。

被测模拟器与 Spike 执行同一个 ELF，逐条比较提交结果：PC、机器码、寄存器
写回、load 地址和内存写入。参考轨迹固定截断在 test_done 处，且截断点只由
Spike 自己的日志决定——不从被测轨迹推导，否则被测提前停机会被误判为通过。
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = re.compile(r'^core\s+(\d+):\s+([0-3])\s+(0x[0-9a-fA-F]+)\s+\((0x[0-9a-fA-F]+)\)(.*)$')
REG = re.compile(r'\s*x\s*(\d+)\s+(0x[0-9a-fA-F]+)')
MEM = re.compile(r'\s*mem\s+(0x[0-9a-fA-F]+)(?:\s+(0x[0-9a-fA-F]+))?')
FIELDS = ('pc', 'insn', 'wen', 'rd', 'wdata', 'mem_addr', 'mem_wmask', 'mem_wdata', 'trap', 'cause')
# Spike 自带的 5 条复位 ROM（riscv/sim.cc），PC 固定：csrr / lui / addi / csrw / jalr。
# 它们不是测试程序的一部分，必须原样出现而不是被当成"任意几条低地址事件"丢掉。
RESET_PCS = [0x1000, 0x1004, 0x1008, 0x100c, 0x1010]

# ELF32 布局（小端）。只解析静态符号表，够用且不必为了读几个地址去依赖 binutils。
ELF_MAGIC = b'\x7fELF'
SHT_SYMTAB = 2
STT_SECTION = 3
SHN_UNDEF = 0
EHDR_SHOFF = 32
SHDR_SIZE = 40
SYM_SIZE = 16

# 缺 test_done 说明镜像没重建过；缺 tohost/fromhost 则 Spike 的 HTIF 无法握手。
REQUIRED_SYMBOLS = ('test_done', 'tohost', 'fromhost')


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


def read_symbols(path):
    """解析 ELF32 静态符号表，返回 {符号名: 地址}。

    只认 32 位小端 ELF 的 .symtab。未定义符号（st_shndx == SHN_UNDEF）没有
    地址可言，空名条目也不是符号，两者都跳过。
    """
    data = Path(path).read_bytes()
    if len(data) < 52 or data[:4] != ELF_MAGIC:
        raise ValueError(f'{path}: not an ELF file')
    if data[4] != 1 or data[5] != 1:
        raise ValueError(f'{path}: expected a 32-bit little-endian ELF')
    shoff, = struct.unpack_from('<I', data, EHDR_SHOFF)
    shentsize, shnum = struct.unpack_from('<HH', data, 46)
    if not shoff or not shnum or shentsize < SHDR_SIZE:
        raise ValueError(f'{path}: no section header table')
    if shoff + shnum * shentsize > len(data):
        raise ValueError(f'{path}: truncated section header table')
    symbols = {}
    for index in range(shnum):
        header = struct.unpack_from('<10I', data, shoff + index * shentsize)
        if header[1] != SHT_SYMTAB:
            continue
        offset, size, link, entsize = header[4], header[5], header[6], header[9]
        if entsize != SYM_SIZE:
            raise ValueError(f'{path}: symbol entry size {entsize}, expected {SYM_SIZE}')
        if link >= shnum:
            raise ValueError(f'{path}: symbol table has no string table')
        if offset + size > len(data):
            raise ValueError(f'{path}: truncated symbol table')
        strtab, strsize = struct.unpack_from('<II', data, shoff + link * shentsize + 16)
        if strtab + strsize > len(data):
            raise ValueError(f'{path}: truncated string table')
        for entry in range(offset, offset + size, SYM_SIZE):
            name, value, _, info, _, shndx = struct.unpack_from('<IIIBBH', data, entry)
            # STT_SECTION 的 st_name 按约定索引 .shstrtab 而不是本表链接的字符串
            # 表，拿这个表去解会得到别的符号的名字，直接跳过。
            if not name or shndx == SHN_UNDEF or info & 0xf == STT_SECTION:
                continue
            if name >= strsize:
                raise ValueError(f'{path}: symbol name offset {name} outside string table')
            end = data.find(b'\0', strtab + name, strtab + strsize)
            if end < 0:
                raise ValueError(f'{path}: unterminated symbol name')
            symbols[data[strtab + name:end].decode()] = value
    if not symbols:
        raise ValueError(f'{path}: no .symtab symbols (stripped image?)')
    return symbols


def read_spike(path, stop):
    """解析 Spike 提交日志，并截取到 stop PC 的那一条为止。

    stop 必须恰好出现一次：找不到说明程序没跑到，出现多次说明是循环里的重复
    PC，两种情况都不能"少比几条"糊过去。
    """
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
    if [row[0] for row in raw[:len(RESET_PCS)]] != RESET_PCS:
        raise ValueError('Spike reset ROM changed; expected exactly five known boot PCs')
    events = [parse_commit(*row) for row in raw[len(RESET_PCS):]]
    hits = [i for i, e in enumerate(events) if e['pc'] == stop]
    if len(hits) != 1:
        raise ValueError(f'expected exactly one commit at stop PC {stop:#010x}, found {len(hits)}')
    return events[:hits[0] + 1]


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
    """运行外部命令；失败时连 stderr 一起抛出，否则调用方只看到退出码。"""
    command = [str(x) for x in argv]
    proc = subprocess.run(command, timeout=60, **kwargs)
    if proc.returncode != 0:
        detail = proc.stderr.decode(errors='replace').strip() if proc.stderr else ''
        raise ValueError(f'{" ".join(command)} exited {proc.returncode}'
                         + (f'\n{detail}' if detail else ''))
    return proc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sim', type=Path, default=ROOT / 'build/sim',
                        help='被测模拟器，默认 build/sim')
    parser.add_argument('--spike', default=shutil.which('spike'),
                        help='Spike 路径，默认从 PATH 查找')
    parser.add_argument('--elf', type=Path, default=ROOT / 'programs/diff_smoke.elf',
                        help='测试镜像，默认仓库里提交的那份')
    parser.add_argument('--out', type=Path, default=ROOT / 'out/spike-compare',
                        help='产物目录')
    args = parser.parse_args()
    if not args.spike:
        parser.error('spike not found in PATH; see docs/spike-diff.md')
    sim, spike, elf, out = (args.sim.resolve(), Path(args.spike).resolve(),
                            args.elf.resolve(), args.out.resolve())
    for executable in (sim, spike):
        if not executable.is_file():
            parser.error(f'executable missing: {executable}; see docs/spike-diff.md')
    if not elf.is_file():
        raise ValueError(f'image missing: {elf}; run `make -C programs`')
    # 符号一次性取全：Spike 靠 tohost 握手退出，靠 test_done 定位截断点。
    symbols = read_symbols(elf)
    missing = [name for name in REQUIRED_SYMBOLS if name not in symbols]
    if missing:
        raise ValueError(f'{elf} lacks {", ".join(missing)}; run `make -C programs`')
    stop = symbols['test_done']
    out.mkdir(parents=True, exist_ok=True)
    dut_file, raw = out / 'dut.jsonl', out / 'spike-commit.log'
    console = out / 'spike-console.log'
    for stale in (dut_file, raw, console):  # 残留产物会让失败的下一次运行被误读
        stale.unlink(missing_ok=True)

    # 模拟器直接装载 ELF，按符号名停机，并输出架构状态的提交轨迹。
    # stderr 收下来是为了失败时能报出原因；模拟器的 clock/retired 汇总行也在
    # stderr 上，所以成功时同样转发出去。
    proc = run([sim, elf, '--trace-json', dut_file, '--stop-pc', 'test_done'],
               stderr=subprocess.PIPE)
    sys.stderr.write(proc.stderr.decode(errors='replace'))
    dut = [json.loads(line) for line in dut_file.read_text().splitlines()]

    # 不限制指令数：程序写完 tohost 后 Spike 自己退出，退出码 0 即成功信号。
    # 复位 ROM 用了 csrr a0, mhartid，所以 ISA 串必须带 Zicsr。
    argv = [spike, '--isa=RV32IM_Zicsr', '-m0x80000000:0x100000',
            '--log-commits', f'--log={raw}', elf]
    with console.open('w') as log:
        proc = subprocess.run([str(x) for x in argv], stdin=subprocess.DEVNULL,
                              stdout=log, stderr=subprocess.STDOUT, timeout=60)
    output = console.read_text()
    # 这一条几乎总是意味着装了个没开 commitlog 的 Spike，提示直接给出修法。
    if 'enable-commitlog' in output:
        raise ValueError('Spike was built without commit logging; '
                         'rebuild with ./configure --enable-commitlog')
    if proc.returncode != 0:
        raise ValueError(f'Spike exited {proc.returncode}, expected 0 (the tohost '
                         f'handshake); console tail:\n{output[-600:]}')

    reference = read_spike(raw, stop)
    (out / 'reference-commit.jsonl').write_text(''.join(json.dumps(e) + '\n' for e in reference))
    compare(reference, dut)
    # 负向检查用同一个比较器：翻掉第一条真正写寄存器的提交，必须被检出。
    wrong = [dict(e) for e in dut]
    index = next(i for i, e in enumerate(wrong) if canonical(e)['wen'])
    wrong[index]['wdata'] = hex(int(wrong[index]['wdata'], 0) ^ 1)
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
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        sys.exit(1)
