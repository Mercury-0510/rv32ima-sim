#ifndef RISCVSIM_CORE_H
#define RISCVSIM_CORE_H

#include "cpu_latches.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

struct RISCVSIMCPUState;

/* Physical bus: probe MUST NOT cause device side effects. Return 0 on success.
 * access: 0=read, 1=write, 2=instruction fetch. No virtual translation yet. */
typedef struct SimBus {
    void *opaque;
    int (*probe)(void *, uint32_t, unsigned, int);
    int (*read)(void *, uint32_t, unsigned, uint32_t *);
    int (*write)(void *, uint32_t, unsigned, uint32_t);
} SimBus;

/* 初始化后、运行前设置；乘除延迟通过 in_core_set_m_latency 校验。 */
typedef struct CoreConfig
{
    unsigned mul_cycles, div_cycles; /* EX 总延迟，至少 1 拍。 */
    uint32_t stop_pc;
    int stop_pc_valid; /* 指定 PC 的指令退休后停止。 */
} CoreConfig;

/* in_core_init 的全部输入。核心不保留本结构，字段随用随取。 */
typedef struct CoreSetup
{
    /* 取指与数据来源：数组模式给内嵌数组，总线模式两项都留空。 */
    const uint32_t *program;
    size_t count; /* program 的指令条数 */
    uint8_t *data;
    size_t data_size;
    /* 取指窗口下界与复位入口。裸镜像下两者相同，ELF 下各取其地址。 */
    uint32_t base, entry;
    /* 可选接线：总线后端与轨迹输出，不用时留空。 */
    SimBus bus;
    FILE *commit_trace;
    int stage_trace;
    /* 运行参数，用 in_core_default_config 起手再覆盖需要的字段。 */
    CoreConfig config;
} CoreSetup;

/* 默认运行参数。调用方在此基础上改写 stop_pc 等字段，避免依赖"零值即默认"。 */
CoreConfig in_core_default_config(void);

/* 由核心维护，调用方只读取；各类停顿分别计数。 */
typedef struct CoreStats
{
    uint64_t stalls;         /* 寄存器数据冒险等待拍数。 */
    uint64_t flushes;        /* 分支、跳转重定向次数。 */
    uint64_t memory_stalls;  /* MEM 阻塞拍数。 */
    uint64_t execute_stalls; /* 乘除等待拍数，不含 MEM 反压。 */
    uint64_t retired;        /* 退休事件数；CSR 计数由 CPU 架构状态维护。 */
} CoreStats;

typedef struct INCore
{
    CoreConfig config;
    CoreStats stats;

    /* 流水线级 */
    CPUStage fetch;
    CPUStage decode;
    CPUStage execute;
    CPUStage memory;
    CPUStage commit;

    /* 本拍计算，下个时钟边沿生效。 */
    CPUStage next_decode, next_execute, next_memory, next_commit;

    const uint32_t *program;
    size_t program_size; /* 指令条数 */
    size_t fetch_index;
    uint32_t program_base; /* 取指窗口下界 */
    uint32_t fetch_pc;
    int fetch_done, stalled, redirected, halted, trace;
    int memory_stalled; /* 本拍 MEM 尚未完成，反压 EX/ID/IF。 */
    int execute_stalled;
    /* 从地址 0 开始的小端数据 RAM；访存地址直接作为数组下标。 */
    uint8_t *data;
    size_t data_size;

    SimBus bus; /* Optional mapped memory; legacy array mode remains supported. */
    FILE *commit_trace;
    struct RISCVSIMCPUState *simcpu; /* 架构状态，由调用方持有。 */
} INCore;

/* setup 的 base 是取指窗口下界（裸镜像下同时是装载地址），entry 是复位后的
 * PC；只有带入口地址的镜像（如 ELF）才会让两者不同。返回后 core 的每个字段
 * 都已就位，调用方不必再补赋值。 */
void in_core_init(INCore *core, struct RISCVSIMCPUState *cpu,
                  const CoreSetup *setup);

/* 运行结束：显式停机（陷阱或 stop-pc 命中），或流水线已彻底排空。调用方据此
 * 判断是否正常结束，无需知道有几级流水、各级叫什么。 */
int in_core_finished(const INCore *core);

/* Run at most cycles ticks; a stop request still counts the current tick. */
void in_core_run(INCore *core, uint64_t cycles);
/* 只允许在初始化后、运行前配置非零 EX 延迟；成功返回 1。 */
int in_core_set_m_latency(INCore *core, unsigned mul_cycles, unsigned div_cycles);
int in_core_run_5_stage(INCore *core);

#endif
