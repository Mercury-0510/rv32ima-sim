# Python 轨迹工具

工具只依赖 Python 3 标准库。先按 [README](../README.md) 构建模拟器，再在项目根目录运行（示例镜像随仓库提交，无需自行编译）：

```bash
cmake --build build -j
mkdir -p out
./build/sim programs/diff_smoke.elf --stop-pc test_done --stage-trace > out/trace.log
python3 tools/trace.py out/trace.log -o out/trace.csv
python3 tools/trace.py out/trace.log -o out/trace.jsonl
python3 tools/trace_view.py out/trace.log -o out/trace.html
```

用浏览器打开 `out/trace.html`，或使用 VS Code 的 HTML 预览。页面按周期、阶段排列，优先显示 RV32IMA 汇编，下方保留 PC 和机器码，支持按汇编、PC 或机器码筛选。
寄存器使用 `x0`～`x31`，立即数按指令格式显示；分支和 JAL 显示目标地址，A 指令保留 `.aq`/`.rl` 后缀。未知或未支持的编码显示 `.word 0x...`，不会猜测成其他指令。使用标准指令名，例如 NOP 编码显示为 `addi x0, x0, 0`。
反汇编仅用于展示，不改变原始日志、CSV/JSONL 格式或轨迹比较行为；更新工具后需重新生成已有 HTML。
大日志可以使用 `--start 100 --end 200` 限制展示周期。页面只显示有事件的周期，空单元格表示无日志事件，不能单凭它区分气泡、冲刷与暂停。

## 比较轨迹

先保存已知正确的参考日志，再运行修改后的模拟器：

```bash
cp out/trace.log out/reference.log
./build/sim programs/diff_smoke.elf --stop-pc test_done --stage-trace > out/actual.log
python3 tools/trace_diff.py --actual out/actual.log --expected out/reference.log
python3 tools/trace_diff.py --actual out/actual.log --expected out/reference.log --mode wb
```

默认 `cycle` 模式比较周期、阶段、PC 和机器码；同一周期内部的日志打印顺序不影响结果。
`wb` 模式只比较 WB 的 PC/机器码序列，忽略周期差异。循环中的重复 PC 保留，不做去重。
首个差异会输出两侧事件及前后上下文（`--context 2`）。退出码：0 一致，1 不一致，2 输入或使用错误。
相同轨迹只能证明所记录的字段一致，不能证明指令语义正确。

**`trace_diff.py` 不是架构状态差分。** 阶段日志没有寄存器写回值、内存写入值、异常结果等提交字段，WB 事件甚至可能是故障指令。真正的架构状态差分见下方 `spike_compare.py`。
因此不能把WB数量当作退休数，也不能据此计算可靠CPI。

## 提交结果差分（Spike）

`sim IMAGE --trace-json FILE` 输出独立的提交/异常JSONL，包含寄存器与内存写回。
它与上面的阶段日志是两种格式，不能传给 `trace.py`、`trace_view.py` 或 `trace_diff.py`。
使用 Spike 作为参考模型做架构状态差分：

```bash
python3 tools/spike_compare.py --sim ./build/sim --spike /path/to/spike
```

脚本使用同一份 `programs/diff_smoke.S`：模拟器与 Spike 运行编译出的同一个 ELF，逐条比较
33 条正常退休事件的 PC、指令、寄存器写回、load 地址与内存写，并在末尾做负向检查。
详见 [Spike 差分说明](../docs/spike-diff.md)。

## 数据格式

输入支持原始 `.log` 文本、`.csv`、`.jsonl`（后两者按扩展名识别）。
固定字段为 `cycle,stage,pc,insn`。周期从 1 开始，阶段为 IF/ID/EX/MEM/WB。
CSV 导出 PC/机器码为十六进制字符串，JSONL 导出为整数，两者均可再次作为工具输入。
不识别的日志行、重复的周期/阶段事件、无事件输入均报错；原始日志末尾的 `clock = ..., retired = ...` 汇总行允许存在。
输出目录自动创建，同名输出文件会覆盖。

工具测试包含在统一的 `regression` 入口中，运行命令见 [README](../README.md)。
