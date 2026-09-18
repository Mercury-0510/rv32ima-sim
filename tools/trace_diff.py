#!/usr/bin/env python3
"""比较两份阶段轨迹，报告首个差异；不等同于寄存器/内存语义差分。"""
import argparse
import sys
from trace import read_trace, describe


def compare(actual, expected, mode):
    if mode == 'wb':
        actual = [e for e in actual if e['stage'] == 'WB']
        expected = [e for e in expected if e['stage'] == 'WB']
        if not actual or not expected:
            raise ValueError('WB 模式要求两份轨迹均包含 WB 事件')
    fields = ('cycle', 'stage', 'pc', 'insn') if mode == 'cycle' else ('pc', 'insn')
    for index in range(max(len(actual), len(expected))):
        a = actual[index] if index < len(actual) else None
        e = expected[index] if index < len(expected) else None
        if a is None or e is None or any(a[f] != e[f] for f in fields):
            return index, actual, expected
    return None, actual, expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--actual', required=True, help='待检查轨迹')
    parser.add_argument('--expected', required=True, help='参考轨迹')
    parser.add_argument('--mode', choices=('cycle', 'wb'), default='cycle',
                        help='cycle 比较周期和阶段；wb 只比较 WB 的 PC/机器码顺序')
    parser.add_argument('--context', type=int, default=2, help='首个差异前后的事件数')
    args = parser.parse_args()
    try:
        if args.context < 0:
            raise ValueError('context 不能为负数')
        index, actual, expected = compare(read_trace(args.actual), read_trace(args.expected), args.mode)
        if index is None:
            print(f'一致：{len(actual)} 个事件（{args.mode} 模式）')
            return 0
        print(f'首个差异：第 {index + 1} 个事件（{args.mode} 模式）')
        for j in range(max(0, index-args.context), min(max(len(actual), len(expected)), index+args.context+1)):
            print(f"{'>>' if j == index else '  '} #{j+1}")
            print('  expected:', describe(expected[j] if j < len(expected) else None))
            print('  actual:  ', describe(actual[j] if j < len(actual) else None))
        return 1
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
