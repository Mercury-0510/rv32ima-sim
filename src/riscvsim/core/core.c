#include "core_internal.h"
#include "csr.h"
#include "cpu_state.h"
#include "../utils/trace.h"
#include <string.h>

void in_core_init(INCore *core, RISCVSIMCPUState *cpu, const CoreSetup *setup)
{
    memset(core, 0, sizeof(*core));
    memset(cpu, 0, sizeof(*cpu));
    cpu->priv = PRIV_M; /* 复位后运行在 M 模式 */
    core->simcpu = cpu;
    core->program = setup->program;
    core->program_size = setup->count;
    core->program_base = setup->base;
    core->fetch_pc = setup->entry;
    core->data = setup->data;
    core->data_size = setup->data_size;
    core->bus = setup->bus;
    core->commit_trace = setup->commit_trace;
    core->trace = setup->stage_trace;
    core->config = setup->config;
    core->fetch_done = setup->count == 0;
}

int in_core_finished(const INCore *core)
{
    return core->halted ||
           (core->fetch_done &&
            !core->decode.has_data &&
            !core->execute.has_data &&
            !core->memory.has_data &&
            !core->commit.has_data);
}

void in_core_run(INCore *core, uint64_t cycles)
{
    if (in_core_finished(core))
        return;

    for (uint64_t i = 0; i < cycles; ++i)
    {
        int stop = in_core_run_5_stage(core);
        core->simcpu->clock++;
        if (stop)
            break;
    }
}

int in_core_run_5_stage(INCore *core)
{
    core->next_decode = core->next_execute = (CPUStage){0};
    core->next_memory = core->next_commit = (CPUStage){0};
    core->stalled = core->redirected = 0;
    core->memory_stalled = 0;
    core->execute_stalled = 0;
    if (!in_core_commit(core))
    {
        in_core_memory(core);
        if (core->memory_stalled)
        {
            /* MEM 自己保留状态；更年轻的级保持，WB 的旧指令不能重放。 */
            core->next_execute = core->execute;
            core->next_decode = core->decode;
        }
        /* 异常随指令到 WB 报告，较老的指令先完成；丢弃年轻指令。 */
        else if (!core->next_commit.latch.exception)
        {
            in_core_execute(core);
            if (core->execute_stalled)
                core->next_decode = core->decode; /* EX 保持自己，ID/IF 等待。 */
            else if (!core->next_memory.latch.exception && !core->redirected)
            {
                trace_stage(core, "ID", core->decode);
                in_core_decode(core);
                if (!core->next_execute.latch.exception && !core->stalled)
                    in_core_fetch(core);
            }
        }
    }
    /* 时钟上升边沿更新寄存器latch */
    core->decode = core->next_decode;
    core->execute = core->next_execute;
    core->memory = core->next_memory;
    core->commit = core->next_commit;
    return in_core_finished(core);
}
