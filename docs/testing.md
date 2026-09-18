# 测试

测试全部注册到 CTest。C 测试各自是一个独立可执行文件，直接链接被测模块，断言失败即退出，CTest 只看退出码。

## 运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`ctest -N` 列出全部测试，`ctest -R <名字>` 只跑其中一项。

CI 用 `ENABLE_TRACE` 的 ON/OFF 矩阵各构建一次，见 [.github/workflows/test.yml](../.github/workflows/test.yml)。这个开关的含义见 [程序装载与提交轨迹](loader-trace.md)。

## 测试清单

| 文件 | 被测对象 | 覆盖内容 |
| --- | --- | --- |
| `tests/test_rv32i.c` | `core/` | RV32IM 指令的运算结果；乘除单元的 EX 时序、数据相关、冲刷与预算切分 |
| `tests/test_rv32a.c` | `core/` | AMO 与 LR/SC 的读改写时序、保留集失效规则、各类访存故障 |
| `tests/test_csr.c` | `core/csr.c` | CSR 读改写返回旧值、立即数形式、只读 CSR 的读写权限、`misa` 取值 |
| `tests/test_memory.c` | `system/memory.c` | 地址空间边界与对齐、RAM/MMIO 权限、子字访问、设备副作用与无副作用探测 |
| `tests/test_commit.c` | `utils/trace.c`、流水线 | 提交轨迹 JSONL 的字段契约与 `seq` 编号、子字存储掩码、异常事件、设备访问只在正确路径上发生；`ENABLE_TRACE=OFF` 时自报跳过 |
| `tests/test_loader.c` | `system/loader.c` | 12 类畸形 ELF 的拒绝、段落位与 `.bss` 清零、符号命中与未命中 |
| `tests/test_cli.c` | `main.c` | 进程契约：`--stop-pc` 的符号解析、超时退出码、`--trace-json` 别名保护、自带镜像端到端；`ENABLE_TRACE=OFF` 时改用例检查两个轨迹开关被拒绝 |
| `tests/test_trace_tools.py` | `tools/` | 轨迹转换、比较与 HTML 输出 |
| `tests/test_spike_tools.py` | `tools/` | Spike 日志解析与差分比较 |

`tests/test_util.h` 提供 `CHECK` 断言宏、`BASE` 装载地址和指令编码器，所有 C 测试共用。

## 有意的耦合

`tests/test_commit.c` 的 `sscanf` 格式串镜像 `utils/trace.c` 的输出，`tests/test_loader.c` 自行拼装 ELF 字节。这类耦合是有意的：格式或布局一改，测试就失败。改 `trace.c` 的 `fprintf` 时必须同步改 `EVENT_SCAN`。

`tests/test_cli.c` 起子进程运行 sim，路径由 CMake 通过 `$<TARGET_FILE:sim>` 传入，因此对 `sim` 有构建依赖。它是唯一跨进程边界的测试——其余测试都直接链接被测模块，断言读的是 `core.stats`、`memory.device_reads` 这类结构体字段，而不是打印出来的文本。

## 新增测试

在 `CMakeLists.txt` 的 `if(BUILD_TESTING)` 块里加一个 `test_<名字>` 目标，链接被测模块，调用 `configure_sim_target()` 后 `add_test()` 注册。测试二进制自己统计用例数并打印一行 `名字: N directed cases passed`，不需要外部驱动器。

`sim_memory` 没有导出 include 路径，链接它的测试需要自己写 `target_include_directories(... PRIVATE src/riscvsim)`；链接 `rv32_core` 则不必，它已 `PUBLIC` 导出。

依赖编译期开关的测试**不要**用 CMake 条件注册——那样它只会从 `ctest -N` 里无声消失，看不出是被跳过还是被删了。让测试始终编译、始终注册，关闭时打印一行跳过说明并以约定的退出码退出，再用 `set_tests_properties(... SKIP_RETURN_CODE 77)` 如实报成「跳过」而非「通过」。`tests/test_commit.c` 是现成例子，它的守卫包住整个文件：只包 `main` 会留下没人调用的静态函数，被 `-Werror=unused-function` 拒绝。
