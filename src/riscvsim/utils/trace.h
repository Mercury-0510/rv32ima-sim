#ifndef RISCVSIM_TRACE_H
#define RISCVSIM_TRACE_H

#include "../core/cpu_latches.h"

struct INCore;

/* 轨迹功能的总开关，由 CMake 的 -DENABLE_TRACE=0/1 提供，默认开。不经 CMake
   直接编译（编辑器索引、手工 gcc）时按默认值处理，与构建系统保持一致。 */
#ifndef ENABLE_TRACE
#define ENABLE_TRACE 1
#endif

#if ENABLE_TRACE
/* 根据 trace 开关输出阶段状态，不修改核心数据。 */
void trace_stage(const struct INCore *core, const char *name, CPUStage stage);

void trace_commit(const struct INCore *core, int trap);
#else
/* 关闭时调用点原样保留，整句消失。用宏而非空函数：CPUStage 有 76 字节，按值
 * 传参的拷贝在 -O0 下连 static inline 空函数都省不掉，只有宏能让实参求值一起
 * 消失。代价是实参不再求值，所以调用点只能传无副作用的表达式。 */
#define trace_stage(core, name, stage) ((void)0)
#define trace_commit(core, trap) ((void)0)
#endif
#endif
