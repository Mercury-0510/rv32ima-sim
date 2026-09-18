# rv32ima-sim

用 C 编写的 RV32IMA 五级流水模拟器，用于学习指令执行、流水线时序和处理器验证。提供命令行运行、流水轨迹可视化和 Spike 差分工具。

## 当前状态

- 单核、单发射、顺序执行，支持数据相关停顿和分支冲刷。
- 实现 RV32I、乘除法、原子指令，以及 CSR 访问和 M/S/U 特权级骨架。
- 支持 ELF 与原始二进制装载、提交与异常轨迹、定向测试和部分 RV32IM 指令的 Spike 差分。

目前用于裸机程序验证，尚不支持操作系统启动、非连续多段装载、MMU 或中断。原子访存仅支持核心数组 RAM，尚未接入外部镜像使用的总线。

## 快速开始

需要 CMake 3.16+、支持 C11 的编译器和 Python 3。Python 工具仅使用标准库。构建和运行都不需要 RISC-V 交叉工具链或 Spike，只有重新生成 `programs/` 下的示例镜像才需要前者。

以下命令在项目根目录执行。

### 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

默认构建带轨迹功能。去掉它以及随之而来的逐拍开销：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TRACE=OFF
cmake --build build -j
```

这两个开关会被该构建拒绝：`--stage-trace`、`--trace-json`。开关的取舍见 [程序装载与提交轨迹](docs/loader-trace.md)。

### 运行与测试

仓库提交了预编译的 `programs/diff_smoke.elf`，无需交叉工具链：

```bash
./build/sim programs/diff_smoke.elf --stop-pc test_done
ctest --test-dir build --output-on-failure
```

正常结束时统计信息包含 `retired=33 status=complete`。`--stop-pc` 接受符号名或数字地址；`test_done` 之后是外部环境的退出序列和等待循环，必须让它退休后停止。`ctest -N` 可列出全部测试，任一失败则整体失败。镜像源码见 [programs/diff_smoke.S](programs/diff_smoke.S)，改动后用 `make -C programs` 重建并一并提交。

### 查看流水轨迹

```bash
mkdir -p out
./build/sim programs/diff_smoke.elf --stop-pc test_done --stage-trace > out/trace.log
python3 tools/trace_view.py out/trace.log -o out/trace.html
```

用浏览器打开 `out/trace.html`，查看指令在各阶段的执行情况。这条路径需要默认构建（`ENABLE_TRACE=ON`）。

镜像可以是 ELF，也可以是裸二进制——裸镜像用 `--base` 指定装载地址。镜像格式、加载地址和停止条件见 [程序装载与提交轨迹](docs/loader-trace.md)。完整参数可通过 `./build/sim --help` 查看。

## 项目结构

```text
src/riscvsim/
    main.c           命令行入口、镜像装载与运行控制
    core/            CPU 状态、五级流水、指令执行与 CSR
    system/          RAM 与 MMIO 地址映射
    utils/           阶段日志与提交轨迹
programs/            汇编测试程序、链接脚本与预编译镜像
tests/              C 测试：指令、流水时序、装载与命令行契约；工具测试为 Python
tools/              轨迹转换、HTML 展示与 Spike 差分
docs/               使用说明、设计与后续计划
CMakeLists.txt       构建与测试入口
```

`core/` 通过 `SimBus` 访问系统内存，不依赖具体后端。核心编为 `rv32_core` 静态库，内存模块编为 `sim_memory`；命令行程序负责连接两者。阅读代码可从 [main.c](src/riscvsim/main.c) 和 [core.c](src/riscvsim/core/core.c) 开始。

`build/` 保存编译产物，`out/` 保存生成的轨迹，均不纳入版本控制。

## 文档

- [程序装载与提交轨迹](docs/loader-trace.md)：外部镜像、总线接口和内存映射。
- [测试](docs/testing.md)：每个测试覆盖什么，以及如何新增。
- [轨迹工具](tools/README.md)：日志转换、比较与可视化。
- [Spike 差分验证](docs/spike-diff.md)：依赖、运行方法和验证范围。
- [乘除单元](docs/multiply-divide.md)、[原子访存](docs/atomic-pipeline.md)：执行时序与设计边界。
- [操作系统支持计划](docs/os-roadmap.md)：后续实现顺序与验收方法。

## 开发

提交前运行上述回归测试。GitHub Actions 在 push 和 pull request 时自动构建并测试。

安装 `clang-format` 后重新运行 CMake 配置，即可使用 `cmake --build build --target fmt` 格式化项目的 C 源码和头文件。格式规则见 `.clang-format`。
