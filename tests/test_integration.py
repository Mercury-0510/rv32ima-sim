"""End-to-end image loading, RAM/MMIO, CSR and retirement tests."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SIM = Path(sys.argv[1]).resolve()
sys.argv = sys.argv[:1]
BASE = 0x80000000


def imm(op, rd, f, rs, value):
    return ((value & 4095) << 20) | (rs << 15) | (f << 12) | (rd << 7) | op


def store(f, rs, base, offset):
    v = offset & 4095
    return ((v >> 5) << 25) | (rs << 20) | (base << 15) | (f << 12) | ((v & 31) << 7) | 0x23


MSCRATCH = 0x340
CYCLE = 0xC00
MISA = 0x301
MISA_VALUE = 0x40001100


def csr(f3, rd, addr, rs1):
    """SYSTEM opcode 0x73：f3=1/2/3 寄存器形式，5/6/7 立即数形式。"""
    return (addr << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x73


def addi(rd, rs, value):
    return ((value & 4095) << 20) | (rs << 15) | (rd << 7) | 0x13


class ImageTestCase(unittest.TestCase):
    def run_image(self, words, extra=(), code=0):
        with tempfile.TemporaryDirectory() as d:
            image, trace = Path(d) / "image.bin", Path(d) / "trace.jsonl"
            image.write_bytes(struct.pack("<" + "I" * len(words), *words))
            result = subprocess.run([str(SIM), str(image), "--trace-json", str(trace),
                                     *extra], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, code, result.stderr)
            events = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
            return events, result.stderr


class LoaderTraceTests(ImageTestCase):
    def test_image_required_and_help(self):
        for args in ([], ['--stage-trace'], ['--max-cycles', '10']):
            result = subprocess.run([str(SIM), *args], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('image', result.stderr)
        result = subprocess.run([str(SIM), '--help'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn('sim IMAGE', result.stdout)

    def test_little_endian_arithmetic_and_x0(self):
        e, _ = self.run_image([0x00500093, 0x00700113, 0x002081b3, 0x00100013])
        self.assertEqual([int(x['wdata'], 0) for x in e], [5, 7, 12, 0])
        self.assertEqual(e[-1]['wen'], 0)
        self.assertEqual([x['seq'] for x in e], [1, 2, 3, 4])

    def test_memory_and_subword_mask(self):
        words = [0x80000237, imm(0x13, 1, 0, 0, -1), store(0, 1, 4, 256),
                 imm(3, 2, 0, 4, 256), imm(3, 3, 4, 4, 256),
                 store(1, 1, 4, 258), imm(3, 5, 5, 4, 258)]
        e, _ = self.run_image(words)
        self.assertEqual(int(e[2]['mem_addr'], 0), BASE + 256)
        self.assertEqual(int(e[2]['mem_wdata'], 0), 255)
        self.assertEqual([int(e[i]['wdata'], 0) for i in (3, 4, 6)], [0xffffffff, 255, 65535])

    def test_illegal_is_trap_and_younger_write_cancelled(self):
        e, _ = self.run_image([0x00500093, 0xffffffff, 0x00700113], code=1)
        self.assertEqual([x['type'] for x in e], ['commit', 'trap'])
        self.assertEqual(e[-1]['cause'], 2)
        self.assertEqual(int(e[-1]['tval'], 0), 0xffffffff)
        self.assertEqual(e[-1]['seq'], 1)
        self.assertEqual(e[-1]['wen'], 0)

    def test_unmapped_and_unaligned(self):
        for words, cause in [([imm(3, 1, 2, 0, 0)], 5),
                             ([0x80000237, imm(3, 1, 2, 4, 1)], 4),
                             ([store(2, 0, 0, 0)], 7)]:
            e, _ = self.run_image(words, code=1)
            self.assertEqual(e[-1]['cause'], cause)
            self.assertEqual(e[-1]['mem_wmask'], 0)

    def test_branch_flush_and_stop_pc(self):
        e, _ = self.run_image([0x00000463, 0xffffffff, 0x00500093])
        self.assertEqual([int(x['pc'], 0) for x in e], [BASE, BASE + 8])
        e, _ = self.run_image([0x00500093, 0x00700113], extra=['--stop-pc', hex(BASE)])
        self.assertEqual(len(e), 1)

    def test_timeout(self):
        e, log = self.run_image([0x0000006f], extra=['--max-cycles', '20'], code=3)
        self.assertGreater(len(e), 0)
        self.assertIn('status=timeout', log)

    def test_trace_cannot_overwrite_image_alias(self):
        with tempfile.TemporaryDirectory() as d:
            image, alias = Path(d) / 'image.bin', Path(d) / 'alias'
            image.write_bytes(struct.pack('<I', 0x13))
            alias.symlink_to(image)
            r = subprocess.run([str(SIM), str(image), '--trace-json', str(alias)], capture_output=True)
            self.assertEqual(r.returncode, 2)
            self.assertEqual(image.read_bytes(), struct.pack('<I', 0x13))

    def test_reject_bad_image_and_options(self):
        for data in (b'', b'abc', b'12345'):
            with tempfile.TemporaryDirectory() as d:
                image = Path(d) / 'bad.bin'
                image.write_bytes(data)
                r = subprocess.run([str(SIM), str(image)], capture_output=True)
                self.assertEqual(r.returncode, 2)
        self.run_image([0x13], extra=['--base', '0x80000001'], code=2)
        self.run_image([0x13], extra=['--max-cycles', '-1'], code=2)

    def test_mmio_once_and_wrong_path_cancel(self):
        words = [0x10000237, imm(0x13, 1, 0, 0, 42), store(2, 1, 4, 0),
                 imm(3, 2, 2, 4, 0), 0x00000463, store(2, 0, 4, 0), 0x13]
        e, log = self.run_image(words)
        self.assertEqual(int(e[3]['wdata'], 0), 42)
        self.assertIn('device_reads=1 device_writes=1', log)
        self.assertNotIn(BASE + 20, [int(x['pc'], 0) for x in e])

    def test_fault_cancels_younger_device_read_and_write(self):
        for younger in (imm(3, 2, 2, 4, 0), store(2, 0, 4, 0)):
            e, log = self.run_image([0x10000237, imm(3, 1, 2, 0, 0), younger], code=1)
            self.assertEqual(e[-1]['type'], 'trap')
            self.assertIn('device_reads=0 device_writes=0', log)



class CSRTraceTests(ImageTestCase):
    def test_read_modify_write_sequence(self):
        words = [addi(1, 0, 0x234),
                 csr(1, 0, MSCRATCH, 1),   # csrrw x0, mscratch, x1
                 csr(2, 2, MSCRATCH, 0),   # csrrs x2, mscratch, x0（只读）
                 addi(3, 0, 0x0f0),
                 csr(2, 4, MSCRATCH, 3),   # csrrs x4, mscratch, x3
                 csr(3, 5, MSCRATCH, 3),   # csrrc x5, mscratch, x3
                 csr(5, 6, MSCRATCH, 7),   # csrrwi x6, mscratch, 7
                 csr(6, 7, MSCRATCH, 8),   # csrrsi x7, mscratch, 8
                 csr(7, 8, MSCRATCH, 1),   # csrrci x8, mscratch, 1
                 csr(2, 9, MSCRATCH, 0)]   # csrrs x9, mscratch, x0
        events, _ = self.run_image(words)
        self.assertEqual([e["type"] for e in events], ["commit"] * len(words))
        # 每条 CSR 指令写回的是「动作之前的旧值」，因此序列可证明读改写顺序正确。
        self.assertEqual([int(e["wdata"], 0) for e in events],
                         [564, 0, 564, 240, 564, 756, 516, 7, 15, 14])
        self.assertEqual(events[1]["wen"], 0)  # csrw 不写整数寄存器

    def test_write_readonly_csr_traps(self):
        words = [addi(1, 0, 1), csr(1, 0, CYCLE, 1)]  # csrrw x0, cycle, x1
        events, _ = self.run_image(words, code=1)
        self.assertEqual([e["type"] for e in events], ["commit", "trap"])
        self.assertEqual(events[-1]["cause"], 2)
        self.assertEqual(int(events[-1]["tval"], 0), words[1])
        self.assertEqual(events[-1]["seq"], 1)

    def test_readonly_csr_read_is_allowed(self):
        events, _ = self.run_image([csr(6, 1, CYCLE, 0)])  # csrrsi x1, cycle, 0
        self.assertEqual(events[-1]["type"], "commit")
        self.assertEqual(int(events[-1]["wdata"], 0), events[-1]["cycle"] - 1)

    def test_misa_and_counter(self):
        events, _ = self.run_image([csr(2, 1, MISA, 0)])
        self.assertEqual(int(events[0]["wdata"], 0), MISA_VALUE)
        events, _ = self.run_image([csr(2, 1, CYCLE, 0)])
        self.assertEqual(int(events[0]["wdata"], 0), events[0]["cycle"] - 1)


if __name__ == '__main__':
    unittest.main()
