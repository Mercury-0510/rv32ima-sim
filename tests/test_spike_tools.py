import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('spike_compare', Path(__file__).resolve().parents[1] / 'tools/spike_compare.py')
spike = importlib.util.module_from_spec(spec)
spec.loader.exec_module(spike)


class SpikeTraceTests(unittest.TestCase):
    def test_reg_spacing_and_x0(self):
        self.assertEqual(spike.parse_commit(0x80000000, 0xff300093, ' x1  0xfffffff3')['wdata'], 0xfffffff3)
        self.assertEqual(spike.parse_commit(0x80000000, 0x00100013, '')['wen'], 0)

    def test_store_width_and_load_address(self):
        store = spike.parse_commit(0x80000004, 0x40120223, ' mem 0x80000404 0xf3')
        self.assertEqual((store['mem_wmask'], store['mem_wdata']), (1, 243))
        load = spike.parse_commit(0x80000008, 0x40424383, ' x7  0x000000f3 mem 0x80000404')
        self.assertEqual((load['wdata'], load['mem_addr']), (243, 0x80000404))

    def test_missing_or_unsupported_fields_rejected(self):
        for insn, body in [(0xff300093, ''), (0x40120223, ''),
                           (0x40120223, ' mem 0x80000404 0xfffffff3'),
                           (0xff300093, ' x1 0xfffffff3 c768_mstatus 0x0')]:
            with self.assertRaises(ValueError):
                spike.parse_commit(0x80000000, insn, body)

    def test_comparator_detects_wrong_data_address_and_count(self):
        ref = spike.parse_commit(0x80000000, 0x40424383, ' x7 0x000000f3 mem 0x80000404')
        spike.compare([ref], [dict(ref)])
        for field in ('wdata', 'pc', 'insn', 'mem_addr'):
            wrong = dict(ref)
            wrong[field] ^= 1
            with self.assertRaises(ValueError):
                spike.compare([ref], [wrong])
        with self.assertRaises(ValueError):
            spike.compare([ref], [])
        with self.assertRaises(ValueError):
            spike.compare([], [])


if __name__ == '__main__':
    unittest.main()
