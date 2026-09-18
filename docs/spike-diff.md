# Spike 差分验证

`tools/spike_compare.py` 让模拟器与 Spike 执行同一个 RV32IM 测试程序，逐条比较正常提交结果：PC、机器码、寄存器写回、load 地址和内存写入。

## 前置条件

需要已构建的模拟器、可执行的 Spike，以及仓库里提交的 `programs/diff_smoke.elf`。**不需要 RISC-V 交叉工具链**——脚本自己解析 ELF 符号表，不用 `nm`。

Spike 必须带提交日志支持构建：

```bash
git clone https://github.com/riscv-software-src/riscv-isa-sim && cd riscv-isa-sim
./configure --enable-commitlog --prefix="$HOME/.local" CXXFLAGS="-g -O2 -include cstdint"
make -j"$(nproc)" && make install
```

`--enable-commitlog` 是必须的，且该选项不出现在 `--help` 里。漏掉它 Spike 会以 `Commit logging support has not been properly enabled` 报错退出，脚本能识别这条消息并给出上面这个修法。

除 Spike 外不需要任何外部模拟器。仓库根目录的 `marss-riscv/` 只是本地参考检出（已被 `.gitignore` 忽略），构建和测试都不读取它，本项目不依赖 MARSS。

## 运行方法

在项目根目录运行，默认值即可直接工作：

```bash
python3 tools/spike_compare.py
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--sim` | `build/sim` | 被测模拟器 |
| `--spike` | 从 `PATH` 查找 | Spike 路径 |
| `--elf` | `programs/diff_smoke.elf` | 测试镜像 |
| `--out` | `out/spike-compare` | 产物目录 |

## 执行流程

1. 解析镜像符号表，一次取出 `test_done`、`tohost`、`fromhost`。缺任何一个立刻报错并提示 `make -C programs`——缺 `tohost` 会让 Spike 无法握手，等到超时才发现就太晚了。
2. 模拟器装载该 ELF，以符号名 `test_done` 停机，输出提交轨迹。
3. Spike 执行同一个 ELF：

   ```bash
   spike --isa=RV32IM_Zicsr -m0x80000000:0x100000 --log-commits --log=... diff_smoke.elf
   ```

   ISA 串必须带 `Zicsr`，因为 Spike 的复位 ROM 用了 `csrr a0, mhartid`。内存大小与模拟器的默认 1 MiB RAM 一致。**不限制指令数**：程序向 `tohost` 写 1 后 Spike 自行退出，退出码 0 就是成功信号。
4. 校验前 5 条提交是 Spike 的复位 ROM（PC `0x1000`–`0x1010`），然后按 `test_done` 截断参考轨迹。
5. 逐条比较，最后翻转一处写回值再比较一次，确认比较器能报告错误。

## 截断与判定

参考轨迹截断在 `test_done` 那一条，且该 PC 必须**恰好出现一次**：找不到说明程序没跑到，出现多次说明落进了循环，两种情况都直接报错，不会少比几条糊过去。

截断点只来自 Spike 自己的日志，不从被测轨迹推导——否则被测提前停机会被误判为通过。这也意味着本脚本不再固定比较条数：`diff_smoke.elf` 仍是原来那个 33 条退休的冒烟程序的约束，由 [测试](../tests/) 中的 `retired=33` 断言和 CI 保证。

## 输出文件

| 文件 | 内容 |
| --- | --- |
| `dut.jsonl` | 被测模拟器的提交轨迹 |
| `spike-commit.log` | Spike 原始提交日志 |
| `reference-commit.jsonl` | 截断并解析后的参考轨迹 |
| `spike-console.log` | Spike 控制台输出 |
| `run.json` | 运行参数与结果 |

每次运行前会删除上一轮的产物，避免失败时读到陈旧结果。Spike 非零退出时会打印控制台末尾。

## 验证范围

测试覆盖整数运算、字节/半字/字访存、符号扩展、8 条 M 指令、数据相关、循环和跳转。尚未覆盖 CSR、原子指令、异常、中断或特权级切换——提交轨迹里没有 CSR 字段，这是当前格式的覆盖上限。

CTest 中的 `tools_spike` 运行 `tests/test_spike_tools.py`，检查日志解析、ELF 符号表解析和比较工具；它不代替上述真实 Spike 运行。
