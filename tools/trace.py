#!/usr/bin/env python3
"""解析模拟器阶段日志；仅使用 Python 标准库。"""
import argparse
import csv
import json
import re
import sys
from pathlib import Path

STAGES = ('IF', 'ID', 'EX', 'MEM', 'WB')
PATTERN = re.compile(r'cycle=(\d+)\s+(IF|ID|EX|MEM|WB)\s+pc=0x([0-9a-fA-F]+)\s+insn=0x([0-9a-fA-F]+)')
SUMMARY = re.compile(r'clock = \d+, retired = \d+')
FIELDS = ('cycle', 'stage', 'pc', 'insn')


def read_trace(path):
    """返回按 cycle、stage 排序的事件；拒绝坏行和重复的阶段事件。"""
    path = Path(path)
    rows = []
    with path.open(encoding='utf-8') as stream:
        if path.suffix.lower() == '.csv':
            reader = csv.DictReader(stream)
            if reader.fieldnames != list(FIELDS):
                raise ValueError(f'{path}: CSV 列必须为 {FIELDS}')
            rows = list(reader)
        else:
            for number, line in enumerate(stream, 1):
                line = line.strip()
                if not line:
                    continue
                if path.suffix.lower() == '.jsonl':
                    try:
                        rows.append(json.loads(line))
                    except json.JSONDecodeError as error:
                        raise ValueError(f'{path}:{number}: JSON 错误: {error}') from error
                elif SUMMARY.fullmatch(line):
                    continue
                else:
                    match = PATTERN.fullmatch(line)
                    if not match:
                        raise ValueError(f'{path}:{number}: 无法识别的轨迹行: {line}')
                    cycle, stage, pc, insn = match.groups()
                    rows.append(dict(cycle=int(cycle), stage=stage,
                                     pc=int(pc, 16), insn=int(insn, 16)))
    events, seen = [], set()
    for number, row in enumerate(rows, 1):
        try:
            if not isinstance(row, dict) or set(row) != set(FIELDS):
                raise ValueError('字段不匹配')
            event = {'stage': row['stage']}
            for field in ('cycle', 'pc', 'insn'):
                value = row[field]
                if isinstance(value, str):
                    value = int(value, 16 if value.lower().startswith('0x') else 10)
                if type(value) is not int:
                    raise ValueError(f'{field} 必须是整数')
                event[field] = value
            if event['stage'] not in STAGES or event['cycle'] < 1:
                raise ValueError('阶段或周期无效')
            if not all(0 <= event[f] <= 0xffffffff for f in ('pc', 'insn')):
                raise ValueError('PC 或机器码超出 32 位')
            key = event['cycle'], event['stage']
            if key in seen:
                raise ValueError('同一拍同一阶段出现重复事件')
            seen.add(key)
            events.append(event)
        except (ValueError, TypeError, KeyError) as error:
            raise ValueError(f'{path}:事件 {number}: {error}') from error
    if not events:
        raise ValueError(f'{path}: 没有阶段事件，请用 --stage-trace 运行模拟器'
                         f'（构建需 ENABLE_TRACE=ON）')
    return sorted(events, key=lambda e: (e['cycle'], STAGES.index(e['stage'])))


def describe(event):
    if event is None:
        return '<无事件>'
    return (f"cycle={event['cycle']} {event['stage']} "
            f"pc=0x{event['pc']:08x} insn=0x{event['insn']:08x}")


def output_path(value):
    path = Path(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', help='原始日志、.csv 或 .jsonl')
    parser.add_argument('-o', '--output', required=True, help='输出 .csv 或 .jsonl')
    args = parser.parse_args()
    try:
        events = read_trace(args.input)
        suffix = Path(args.output).suffix.lower()
        if suffix not in ('.csv', '.jsonl'):
            raise ValueError('输出扩展名必须是 .csv 或 .jsonl')
        with output_path(args.output).open('w', encoding='utf-8', newline='') as stream:
            if suffix == '.csv':
                writer = csv.DictWriter(stream, fieldnames=FIELDS)
                writer.writeheader()
                for event in events:
                    writer.writerow(dict(event, pc=f"0x{event['pc']:08x}", insn=f"0x{event['insn']:08x}"))
            else:
                for event in events:
                    stream.write(json.dumps(event) + '\n')
        print(f'导出 {len(events)} 个阶段事件 → {args.output}')
        return 0
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
