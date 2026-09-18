"""ELF 装载：格式校验、符号解析，以及预编译示例镜像的端到端结果。"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SIM = Path(sys.argv[1]).resolve()
sys.argv = sys.argv[:1]
ROOT = Path(__file__).resolve().parents[1]
SMOKE = ROOT / "programs" / "diff_smoke.elf"

EHDR_FMT = "<16sHHIIIIIHHHHHH"
PHDR_FMT = "<IIIIIIII"
SHDR_FMT = "<IIIIIIIIII"
SYM_FMT = "<IIIBBH"

ET_EXEC, ET_DYN = 2, 3
EM_RISCV, EM_X86_64 = 243, 62
PT_LOAD = 1
PF_X, PF_R = 1, 4
SHT_SYMTAB, SHT_STRTAB = 2, 3

EHDR_SIZE, PHDR_SIZE, SHDR_SIZE = 52, 32, 40


def ehdr(phnum, entry, phoff=EHDR_SIZE, shoff=0, shnum=0, shentsize=SHDR_SIZE,
         etype=ET_EXEC, machine=EM_RISCV, cls=1, data=1, phentsize=PHDR_SIZE):
    ident = b"\x7fELF" + bytes([cls, data, 1, 0]) + bytes(8)
    return struct.pack(EHDR_FMT, ident, etype, machine, 1, entry, phoff, shoff,
                       0, EHDR_SIZE, phentsize, phnum, shentsize, shnum, 0)


def phdr(offset, vaddr, filesz, memsz=None, flags=PF_R | PF_X, ptype=PT_LOAD):
    """默认把段放在文件里紧跟段头表之后，因此 offset 由调用方按布局给出。"""
    return struct.pack(PHDR_FMT, ptype, offset, vaddr, vaddr, filesz,
                       filesz if memsz is None else memsz, flags, 0x1000)


class ElfCase(unittest.TestCase):
    def run_sim(self, blob, extra=(), code=2):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "image.elf"
            path.write_bytes(blob)
            result = subprocess.run([str(SIM), str(path), *extra],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, code, result.stderr)
            return result

    def build(self, segments, entry, **header):
        """segments: (vaddr, data, memsz, flags)。数据依次排在段头表之后。"""
        blob = bytearray(ehdr(len(segments), entry, **header))
        data_at = EHDR_SIZE + len(segments) * PHDR_SIZE
        for vaddr, data, memsz, flags in segments:
            blob += phdr(data_at, vaddr, len(data), memsz, flags)
            data_at += len(data)
        for _, data, _, _ in segments:
            blob += data
        return bytes(blob)


class RejectionTests(ElfCase):
    """畸形镜像一律拒绝，退出码 2，而不是当成裸镜像跑起来。"""

    def assert_rejected(self, blob, needle):
        self.assertIn(needle, self.run_sim(blob).stderr)

    def test_truncated_header(self):
        self.assert_rejected(b"\x7fELF", "smaller than an ELF header")

    def test_wrong_class(self):
        blob = self.build([(0x80000000, b"\x13\0\0\0", 4, PF_R | PF_X)],
                          0x80000000, cls=2)
        self.assert_rejected(blob, "only ELF32")

    def test_wrong_endianness(self):
        blob = self.build([(0x80000000, b"\x13\0\0\0", 4, PF_R | PF_X)],
                          0x80000000, data=2)
        self.assert_rejected(blob, "little-endian")

    def test_foreign_machine(self):
        blob = self.build([(0x80000000, b"\x13\0\0\0", 4, PF_R | PF_X)],
                          0x80000000, machine=EM_X86_64)
        self.assert_rejected(blob, "not built for RISC-V")

    def test_not_executable_type(self):
        blob = self.build([(0x80000000, b"\x13\0\0\0", 4, PF_R | PF_X)],
                          0x80000000, etype=ET_DYN)
        self.assert_rejected(blob, "only executable")

    def test_program_header_table_outside_file(self):
        blob = ehdr(1, 0x80000000, phoff=0x1000)  # 段头表指向文件之外
        self.assert_rejected(blob, "truncated")

    def test_no_loadable_segment(self):
        blob = ehdr(1, 0x80000000) + phdr(0, 0, 0, 0, ptype=0)
        self.assert_rejected(blob, "no loadable segment")

    def test_no_executable_segment(self):
        blob = self.build([(0x80000000, b"\x13\0\0\0", 4, PF_R)], 0x80000000)
        self.assert_rejected(blob, "no executable segment")

    def test_disjoint_executable_segments(self):
        """可执行段之间有空洞时无法用单一取指窗口描述，必须拒绝。"""
        blob = self.build([(0x80000000, b"\x13\0\0\0", 4, PF_R | PF_X),
                           (0x80002000, b"\x13\0\0\0", 4, PF_R | PF_X)],
                          0x80000000)
        self.assert_rejected(blob, "not contiguous")

    def test_entry_outside_executable_segment(self):
        blob = self.build([(0x80000000, b"\x13\0\0\0", 4, PF_R | PF_X)],
                          0x80004000)
        self.assert_rejected(blob, "outside the executable")

    def test_memory_size_smaller_than_file_size(self):
        blob = self.build([(0x80000000, b"\x13\0\0\0", 2, PF_R | PF_X)],
                          0x80000000)
        self.assert_rejected(blob, "smaller in memory")

    def test_segment_beyond_end_of_file(self):
        blob = ehdr(1, 0x80000000) + phdr(0x1000, 0x80000000, 4)
        self.assert_rejected(blob, "past the end of the file")


class SymbolTests(ElfCase):
    """--stop-pc 的符号名走 .symtab，找不到要报错而不是静默接受。"""

    def with_symbol(self, name, value):
        """构造只含一条可执行段和一个符号的最小 ELF。"""
        code = struct.pack("<II", 0x00500093, 0x00100013)
        sym_at = EHDR_SIZE + PHDR_SIZE + len(code)
        strtab = b"\0" + name.encode() + b"\0"
        symtab = struct.pack(SYM_FMT, 1, value, 0, 0x12, 0, 1)
        str_at = sym_at + len(symtab)
        blob = bytearray(ehdr(1, 0x80000000, shoff=str_at + len(strtab), shnum=3))
        blob += phdr(EHDR_SIZE + PHDR_SIZE, 0x80000000, len(code), len(code),
                     PF_R | PF_X)
        blob += code
        blob += symtab
        blob += strtab
        # 节头表：[0] NULL、[1] .symtab（link=2 指向 .strtab）、[2] .strtab
        blob += struct.pack(SHDR_FMT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        blob += struct.pack(SHDR_FMT, 0, SHT_SYMTAB, 0, 0, sym_at, len(symtab),
                            2, 0, 4, 16)
        blob += struct.pack(SHDR_FMT, 0, SHT_STRTAB, 0, 0, str_at, len(strtab),
                            0, 0, 1, 0)
        return bytes(blob)

    def test_symbol_resolves_and_runs(self):
        result = self.run_sim(self.with_symbol("finish", 0x80000004),
                              extra=["--stop-pc", "finish"], code=0)
        # 命中的那条指令退休后才停，因此两条都计入退休数。
        self.assertIn("retired=2", result.stderr)
        self.assertIn("status=complete", result.stderr)

    def test_unknown_symbol_is_rejected(self):
        result = self.run_sim(self.with_symbol("finish", 0x80000004),
                              extra=["--stop-pc", "absent"])
        self.assertIn("symbol not found", result.stderr)

    def test_raw_image_rejects_symbol_name(self):
        with tempfile.TemporaryDirectory() as d:
            image = Path(d) / "raw.bin"
            image.write_bytes(struct.pack("<I", 0x00100013))
            result = subprocess.run([str(SIM), str(image), "--stop-pc", "finish"],
                                    capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("no symbol table", result.stderr)


class PrebuiltImageTests(unittest.TestCase):
    """仓库自带的 programs/diff_smoke.elf 必须开箱可跑，结果与裸镜像一致。"""

    def test_repository_image_runs_to_completion(self):
        self.assertTrue(SMOKE.is_file(), f"missing prebuilt image: {SMOKE}")
        result = subprocess.run([str(SIM), str(SMOKE), "--stop-pc", "test_done"],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("retired=33", result.stderr)
        self.assertIn("status=complete", result.stderr)

    def test_elf_and_raw_paths_agree(self):
        """同一程序走 ELF 与裸镜像两条装载路径，周期和退休数必须相同。"""
        with tempfile.TemporaryDirectory() as d:
            raw = Path(d) / "smoke.bin"
            result = subprocess.run(
                ["riscv64-unknown-elf-objcopy", "-O", "binary", str(SMOKE), str(raw)],
                capture_output=True)
            if result.returncode:
                self.skipTest("cross toolchain unavailable for raw comparison")
            elf_run = subprocess.run([str(SIM), str(SMOKE), "--stop-pc", "test_done"],
                                     capture_output=True, text=True, timeout=5)
            raw_run = subprocess.run([str(SIM), str(raw), "--stop-pc", "0x8000007c"],
                                     capture_output=True, text=True, timeout=5)
        self.assertEqual(elf_run.stderr, raw_run.stderr)


if __name__ == "__main__":
    unittest.main()
