# Spike 差分验证

`tools/spike_compare.py` 让模拟器与 Spike 执行同一 RV32IM 测试程序，逐条比较正常提交结果。比较内容包括 PC、机器码、寄存器写回、load 地址和普通内存写入。

## 运行方法

需要可执行的 Spike、已构建的模拟器，以及提供 `gcc`、`nm` 的 RISC-V 交叉工具链。在项目根目录指定可执行文件路径：

```bash
python3 tools/spike_compare.py --sim ./build/sim --spike /path/to/spike
```

| 参数 | 说明 |
| --- | --- |
| `--sim` | 被测模拟器路径，建议显式指定 |
| `--spike` | Spike 路径；未指定时先从 PATH 查找 |
| `--cross` | 交叉工具链前缀，默认 `riscv64-unknown-elf-` |
| `--out` | 产物目录，默认项目下的 `out/spike-compare` |

脚本保留了项目同级 `cpu/build/` 下的备用工具路径。显式传入 `--sim` 和 `--spike` 可避免依赖该目录布局。

## 执行流程

1. 使用 `programs/link.ld` 链接 `programs/diff_smoke.S`，生成 ELF。
2. 模拟器直接装载该 ELF，用符号名 `test_done` 作为停止 PC，并输出提交轨迹。
3. Spike 执行同一个 ELF，共运行 5 条复位指令和 33 条测试指令。条数固定，不从被测轨迹推导。
4. 校验复位指令的 PC 为 `0x1000` 至 `0x1010`，然后逐条比较 33 个测试提交事件。
5. 翻转一处写回值再比较，确认比较器能报告错误。

成功时输出 `SPIKE DIFFTEST PASS: 33 retirement events matched`。

## 输出文件

| 文件 | 内容 |
| --- | --- |
| `smoke.elf` | 测试程序 |
| `dut.jsonl` | 被测模拟器的提交轨迹 |
| `spike-commit.log` | Spike 原始提交日志 |
| `reference-commit.jsonl` | 解析后的参考轨迹 |
| `spike-console.log` | Spike 控制台输出 |
| `run.json` | 运行参数与结果 |

## 验证范围

测试覆盖整数运算、字节/半字/字访存、符号扩展、8 条 M 指令、数据相关、循环和跳转。尚未覆盖 CSR、原子指令、异常、中断或特权级切换。

统一回归入口 `regression` 包含 `tests/test_spike_tools.py`，检查日志解析和比较工具；它不代替上述真实 Spike 运行。
