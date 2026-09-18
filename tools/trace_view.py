#!/usr/bin/env python3
"""生成无需外部依赖的 HTML 五级流水轨迹表。"""
import argparse
import html
import sys
from trace import STAGES, read_trace, output_path
from disasm import disassemble


def render(events):
    cycles = {}
    for e in events:
        cycles.setdefault(e['cycle'], {})[e['stage']] = e
    rows = []
    for cycle, stages in sorted(cycles.items()):
        cells = [f'<th>{cycle}</th>']
        for stage in STAGES:
            e = stages.get(stage)
            if e:
                assembly = html.escape(disassemble(e['insn'], e['pc']))
                cells.append(f'<td class="{stage}"><strong>{assembly}</strong><br>'
                             f'<small>PC: 0x{e["pc"]:08x}<br>机器码: 0x{e["insn"]:08x}</small></td>')
            else:
                cells.append('<td>—</td>')
        rows.append('<tr>' + ''.join(cells) + '</tr>')
    return '''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<title>RV32IMA 流水轨迹</title><style>
body{font:16px system-ui;margin:32px;color:#172337;background:#f5f7fb}
table{border-collapse:collapse;width:100%;font-family:monospace;background:white}
th,td{padding:10px;border:1px solid #d5dce5;text-align:center}thead{position:sticky;top:0;background:#172337;color:white}
small{color:#556}input{padding:8px;width:280px}.IF{background:#e6f3ff}.ID{background:#eaf6e9}.EX{background:#fff2d8}.MEM{background:#f3eaff}.WB{background:#ffe9e5}
</style><h1>RV32IMA 五级流水轨迹</h1>
<p>每格显示汇编指令，下方保留 PC 和机器码。寄存器使用 x0–x31；分支和 JAL 显示目标地址。无法解码的指令显示为 .word。— 表示没有日志事件，可能是气泡或该阶段未执行。
重复 PC 可能是停顿或循环；WB 事件也可能是异常指令，不能直接当作退休数。</p>
<p>显示 ''' + str(len(events)) + ''' 个阶段事件。<input id="filter" placeholder="筛选汇编 / PC / 机器码，例如 amoadd"></p>
<table><thead><tr><th>周期</th>''' + ''.join(f'<th>{html.escape(s)}</th>' for s in STAGES) + '''</tr></thead><tbody>''' + ''.join(rows) + '''</tbody></table>
<script>document.getElementById('filter').addEventListener('input',function(){
const q=this.value.toLowerCase();document.querySelectorAll('tbody tr').forEach(r=>r.hidden=!r.textContent.toLowerCase().includes(q));});</script></html>'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input')
    parser.add_argument('-o', '--output', default='out/trace.html')
    parser.add_argument('--start', type=int, default=1, help='起始周期（含）')
    parser.add_argument('--end', type=int, help='结束周期（含）')
    args = parser.parse_args()
    try:
        if args.start < 1 or (args.end is not None and args.end < args.start):
            raise ValueError('周期范围无效')
        events = [e for e in read_trace(args.input) if e['cycle'] >= args.start and (args.end is None or e['cycle'] <= args.end)]
        if not events:
            raise ValueError('所选范围没有事件')
        output_path(args.output).write_text(render(events), encoding='utf-8')
        print(f'轨迹页面 → {args.output}')
        return 0
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
