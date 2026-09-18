#ifndef SIM_MEMORY_H
#define SIM_MEMORY_H
#include "../core/core.h"

/* 项目内实现的物理地址空间：一段 RAM 和一个测试 MMIO 设备页。 */
typedef struct SimMemory {
    uint32_t base;
    size_t size;
    uint8_t *ram;
    uint64_t reads, writes, device_reads, device_writes;
    uint32_t device_value;
} SimMemory;

int sim_memory_init(SimMemory *m, uint32_t base, size_t size);
void sim_memory_end(SimMemory *m);
SimBus sim_memory_bus(SimMemory *m);
#define SIM_TEST_DEVICE UINT32_C(0x10000000)
#define SIM_DEVICE_SIZE 4096u
#endif
