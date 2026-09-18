#ifndef RISCVSIM_TRACE_H
#define RISCVSIM_TRACE_H

#include "../core/cpu_latches.h"

struct INCore;

/* 根据 trace 开关输出阶段状态，不修改核心数据。 */
void trace_stage(const struct INCore *core, const char *name, CPUStage stage);

void trace_commit(const struct INCore *core, int trap);
#endif
