# 程序装载与提交轨迹

本文说明外部二进制装载、提交轨迹和物理内存映射。核心构建与模块划分见 [README](../README.md)。

## 构建与运行

内存由项目内的 `system/memory.c` 实现，无需外部源码。按 [README](../README.md) 在 `build/` 构建模拟器，然后运行仓库自带的示例镜像并保存提交轨迹：

```bash
./build/sim programs/diff_smoke.elf --stop-pc test_done \
  --trace-json out/commit.jsonl --max-cycles 100000
```

`IMAGE` 是必填的位置参数：可以是 ELF32 小端可执行文件，也可以是小端、定长 32 位指令的原始二进制。两者装载规则不同。

ELF 的地址全部取自文件。所有 `PT_LOAD` 段按 `p_vaddr` 写入 RAM，`p_memsz > p_filesz` 的部分清零；入口取 `e_entry`，取指窗口只覆盖 `PF_X` 段。可执行段之间不允许有空洞——非连续的多段装载尚未支持，遇到会报错而非静默截断。RAM 默认覆盖全部段，显式给出 `--ram-size` 时按给定值，装不下就报错。对 ELF 指定 `--base` 会被拒绝，因为文件已自带地址。

裸镜像则整块装载到 `--base`（默认 `0x80000000`），入口即 `--base`，取指窗口就是镜像本身；镜像必须非空、长度为 4 的倍数且能放入 RAM。

两种模式下 `--base` 与 `--ram-size` 都必须按 4096 字节对齐，RAM 最大为 256 MiB，地址范围不能超过 32 位，且 RAM 不得与设备页 `0x10000000` 重叠。

`--stop-pc` 接受数字地址，也接受 ELF 符号名（在 `.symtab` 中查找）；裸镜像没有符号表，只能给数字。停止 PC 对应的指令成功退休后才停止。顺序取指到取指窗口末尾时，模拟器排空流水线后停止；混有数据的镜像应通过 `--stop-pc` 指定最后一条要执行的指令。当前不支持压缩指令或在取指窗口之外执行。`--max-cycles` 限制周期预算，`--stage-trace` 额外输出阶段日志（需要开启轨迹的构建，见下节）。退出码为：0 正常完成，1 架构异常，2 配置或文件错误，3 超时。

## 编译期开关

阶段日志和提交轨迹由 CMake 选项 `ENABLE_TRACE` 统一控制，默认开启：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TRACE=OFF
cmake --build build -j
```

关闭时 `utils/trace.c` 不参与编译，`utils/trace.h` 把两个 trace 函数定义成空宏，于是各阶段的调用点连同 `CPUStage` 的按值传参一起消失——这正是关闭它的收益，否则每拍仍要付 5 次函数调用和 5 次 76 字节的结构体拷贝。代价是关闭构建下 `--stage-trace` 与 `--trace-json` 都以退出码 2 拒绝并说明原因，而不是静默产生空的轨迹文件。

不经过 CMake 直接编译（编辑器索引、手工 gcc）时按默认开启处理，与构建系统一致。

关闭后 `tools/trace.py`、`trace_view.py`、`trace_diff.py` 与 `spike_compare.py` 都不可用，因为它们的输入来自被关掉的功能。`tests/test_commit.c` 在关闭构建下自报跳过。

## 提交轨迹

`--trace-json` 每行输出一个 JSON 对象。成功写回后记录 `commit`；异常记录 `trap`，不增加退休数，也不包含有效的寄存器或内存写入。

| 字段                    | 含义                                               |
| ----------------------- | -------------------------------------------------- |
| `type`                  | `commit` 或 `trap`                                 |
| `cycle`                 | 从 1 开始的模拟周期                                |
| `seq`                   | 截至该事件的退休数，异常不递增                     |
| `pc`, `insn`            | 指令地址和机器码                                   |
| `wen`, `rd`, `wdata`    | 整数寄存器写回，x0 的 `wen` 为 0                   |
| `mem_addr`              | 普通 load/store 的有效字节地址                     |
| `mem_wmask`             | 相对 `mem_addr` 的字节掩码，SB/SH/SW 分别为 1/3/15 |
| `mem_wdata`             | 普通 store 的写入数据，窄写的高位清零              |
| `trap`, `cause`, `tval` | 异常标记、原因和附加值                             |

轨迹用于比较架构结果，不要求参考模型与本模拟器周期一致。当前内存写字段描述普通 store，尚未覆盖原子写入。

## 内存接口

`core/core.h` 定义 `SimBus`，提供地址检查 `probe`、读取 `read` 和写入 `write`。`probe` 不得产生设备副作用。

外部镜像通过总线取指。普通 load/store 在 MEM 检查地址，在 WB 执行总线读写，防止年轻指令在较老指令发生异常前访问设备。该接口采用同步模型，异步访存需要另行定义请求和响应协议。

内存模块自行检查区域归属、访问宽度、对齐和边界，再执行小端 RAM 读写或 MMIO 操作。MARSS 仅作设计参考，构建不编译或链接上游代码，也不需要指定源码路径。

测试设备位于 `0x10000000`，占用 4096 字节，区域内所有对齐的字地址访问同一个仅支持 32 位读写的锁存器。RAM 不得与该区域重叠。`device_reads` 和 `device_writes` 记录访问次数，用于检查错误路径和异常后的设备操作是否被取消。设备区域不能用于取指。

原子指令目前直接访问核心的数组 RAM，尚未接入 `SimBus`，因此外部镜像模式不支持原子访存。具体规则见 [原子访存](atomic-pipeline.md)。

## 验证

`tests/test_memory.c` 直接检查总线权限、地址边界和无副作用探测。`tests/test_commit.c` 在进程内捕获提交轨迹，逐字段检查 JSONL 的序列化契约，以及设备访问在流水线上的表现。`tests/test_loader.c` 直接调用装载器接口，检查畸形 ELF 的拒绝、段落位与 `.bss` 清零、符号解析。`tests/test_cli.c` 起子进程检查命令行契约：`--stop-pc` 的符号解析、超时退出码与轨迹别名保护。

完整回归通过 [README](../README.md) 中的 `ctest` 运行。真实 Spike 对照需另行运行，依赖和方法见 [Spike 差分验证](spike-diff.md)。
