"""Python 工具 CLI 回归：格式往返、差异、坏输入、HTML。"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from disasm import disassemble


class ToolsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.log = self.directory / 'trace.log'
        self.log.write_text('cycle=1 IF pc=0x80000000 insn=0x00000013\n'
                            'cycle=5 WB pc=0x80000000 insn=0x00000013\n'
                            'clock = 5, retired = 1\n')

    def cli(self, tool, *args, code=0):
        result = subprocess.run([sys.executable, str(ROOT/'tools'/tool), *map(str,args)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result

    def test_round_trip(self):
        csv = self.directory/'trace.csv'
        jsonl = self.directory/'trace.jsonl'
        self.cli('trace.py', self.log, '-o', csv)
        self.cli('trace.py', csv, '-o', jsonl)
        self.cli('trace_diff.py', '--actual', jsonl, '--expected', self.log)
        self.assertEqual(len(jsonl.read_text().splitlines()), 2)

    def test_difference_and_wb_timing(self):
        other = self.directory/'other.log'
        other.write_text(self.log.read_text().replace('cycle=5','cycle=6'))
        result = self.cli('trace_diff.py','--actual',other,'--expected',self.log,code=1)
        self.assertIn('第 2 个事件',result.stdout)
        self.cli('trace_diff.py','--actual',other,'--expected',self.log,'--mode','wb')
        other.write_text(other.read_text().replace('00000013','00100093'))
        self.cli('trace_diff.py','--actual',other,'--expected',self.log,'--mode','wb',code=1)

    def test_truncated(self):
        other = self.directory/'short.log'
        other.write_text(self.log.read_text().splitlines()[0]+'\n')
        self.cli('trace_diff.py','--actual',other,'--expected',self.log,code=1)
        self.cli('trace_diff.py','--actual',other,'--expected',self.log,'--mode','wb',code=2)

    def test_invalid(self):
        for text in ('', 'garbage\n', 'cycle=0 IF pc=0x0 insn=0x13\n',
                     'cycle=1 IF pc=0x0 insn=0x13\n'*2):
            self.log.write_text(text)
            self.cli('trace.py',self.log,'-o',self.directory/'out.csv',code=2)
        invalid=self.directory/'bad.jsonl'
        invalid.write_text(json.dumps(dict(cycle=True,stage='IF',pc=0,insn=19)))
        self.cli('trace.py',invalid,'-o',self.directory/'out.csv',code=2)

    def test_html_range(self):
        output=self.directory/'nested'/'view.html'
        self.cli('trace_view.py',self.log,'-o',output,'--start',5,'--end',5)
        text=output.read_text()
        self.assertIn('<th>5</th>',text)
        self.assertNotIn('<th>1</th>',text)
        self.assertIn('0x80000000',text)
        self.assertIn('addi x0, x0, 0', text)
        self.cli('trace_view.py',self.log,'--start',7,code=2)

    def test_disassembly(self):
        vectors = {
            0x01000093: 'addi x1, x0, 16',
            0xfff10093: 'addi x1, x2, -1',
            0xabcde1b7: 'lui x3, 0xabcde',
            0x00001197: 'auipc x3, 0x1',
            0xffc12083: 'lw x1, -4(x2)',
            0xfe112e23: 'sw x1, -4(x2)',
            0x0080006f: 'jal x0, 0x80000008',
            0xffdff06f: 'jal x0, 0x7ffffffc',
            0xfe021ae3: 'bne x4, x0, 0x7ffffff4',
            0xffc100e7: 'jalr x1, -4(x2)',
            0x41f15093: 'srai x1, x2, 31',
            0x0620a1af: 'amoadd.w.aqrl x3, x2, (x1)',
            0x1400a22f: 'lr.w.aq x4, (x1)',
            0x1a40a2af: 'sc.w.rl x5, x4, (x1)',
            0x0ff0000f: 'fence iorw, iorw',
            0x0310000f: 'fence rw, w',
            0x8330000f: 'fence.tso',
            0x0100000f: 'pause',
            0x73: 'ecall', 0x100073: 'ebreak',
            0xffffffff: '.word 0xffffffff',
            0x1010a22f: '.word 0x1010a22f',  # LR rs2 非零
            0x02011093: '.word 0x02011093',  # 非法 SLLI
            0x0020b1af: '.word 0x0020b1af',  # RV64 AMO.D
        }
        for word, expected in vectors.items():
            with self.subTest(word=hex(word)):
                self.assertEqual(disassemble(word, 0x80000000), expected)
        self.assertEqual(disassemble(0x0080006f, 0xfffffffc), 'jal x0, 0x00000004')
        for f3, name in enumerate(('mul', 'mulh', 'mulhsu', 'mulhu', 'div', 'divu', 'rem', 'remu')):
            self.assertEqual(disassemble(0x022081b3 | f3 << 12), f'{name} x3, x1, x2')
        ops = {0:'amoadd', 1:'amoswap', 3:'sc', 4:'amoxor', 8:'amoor',
               12:'amoand', 16:'amomin', 20:'amomax', 24:'amominu', 28:'amomaxu'}
        for f5, name in ops.items():
            for order, suffix in enumerate(('', '.rl', '.aq', '.aqrl')):
                self.assertEqual(disassemble(f5 << 27 | order << 25 | 0x0020a1af),
                                 f'{name}.w{suffix} x3, x2, (x1)')

    def test_html_atomic(self):
        self.log.write_text('cycle=1 IF pc=0x80000000 insn=0x0620a1af\n'
                            'cycle=2 ID pc=0x80000000 insn=0x0620a1af\n')
        output = self.directory / 'atomic.html'
        self.cli('trace_view.py', self.log, '-o', output)
        text = output.read_text()
        self.assertIn('<strong>amoadd.w.aqrl x3, x2, (x1)</strong>', text)
        self.assertIn('机器码: 0x0620a1af', text)
        self.assertIn('筛选汇编', text)


if __name__ == '__main__':
    unittest.main()
