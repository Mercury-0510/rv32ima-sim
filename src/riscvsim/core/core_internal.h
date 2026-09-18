#ifndef RISCVSIM_CORE_INTERNAL_H
#define RISCVSIM_CORE_INTERNAL_H

#include "core.h"
#include "opcodes.h"

/* 流水级只由核心时钟调度；逐级白盒测试需显式包含本内部头文件。 */
int in_core_commit(INCore *core);
void in_core_memory(INCore *core);
void in_core_atomic_memory(INCore *core);
void in_core_execute(INCore *core);
void in_core_decode(INCore *core);
void in_core_fetch(INCore *core);

/* 数据写入使重叠的 LR 保留失效；普通 store 和 AMO 共用。 */
void in_core_invalidate_reservation(INCore *core, uint32_t address, unsigned width);

#endif
