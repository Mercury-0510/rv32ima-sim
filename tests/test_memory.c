#include "test_util.h"
#include "system/memory.h"

int main(void)
{
    SimMemory memory;
    CHECK(sim_memory_init(&memory, 0, 0) != 0);
    CHECK(sim_memory_init(&memory, 1, 4096) != 0);
    CHECK(sim_memory_init(&memory, 0, 4095) != 0);
    CHECK(sim_memory_init(&memory, 0xfffff000u, 8192) != 0);
    CHECK(sim_memory_init(&memory, SIM_TEST_DEVICE, 4096) != 0);
    CHECK(sim_memory_init(&memory, SIM_TEST_DEVICE - 4096, 8192) != 0);
    /* 相邻区域可以存在，只有重叠才被拒绝。 */
    CHECK(sim_memory_init(&memory, SIM_TEST_DEVICE - 4096, 4096) == 0);
    sim_memory_end(&memory);
    CHECK(sim_memory_init(&memory, SIM_TEST_DEVICE + SIM_DEVICE_SIZE, 4096) == 0);
    sim_memory_end(&memory);

    /* RAM 可延伸至 32 位地址空间的最后一个字节。 */
    CHECK(sim_memory_init(&memory, 0xfffff000u, 4096) == 0);
    SimBus bus = sim_memory_bus(&memory);
    uint32_t value = 0;
    CHECK(bus.read(bus.opaque, 0xfffffffcu, 4, &value) == 0 && value == 0);
    CHECK(bus.write(bus.opaque, 0xfffffffcu, 4, 0x87654321u) == 0);
    CHECK(bus.read(bus.opaque, 0xffffffffu, 1, &value) == 0 && value == 0x87);
    CHECK(bus.read(bus.opaque, 0xfffffffeu, 2, &value) == 0 && value == 0x8765);
    CHECK(bus.probe(bus.opaque, 0xfffffffcu, 4, 2) == 0);
    CHECK(bus.probe(bus.opaque, 0xffffffffu, 4, 0) != 0);
    CHECK(bus.probe(bus.opaque, 0xffffeffcu, 4, 0) != 0);
    CHECK(bus.probe(bus.opaque, 0, 4, 0) != 0);

    uint64_t reads = memory.reads, writes = memory.writes;
    CHECK(bus.probe(bus.opaque, SIM_TEST_DEVICE, 4, 0) == 0);
    CHECK(bus.probe(bus.opaque, SIM_TEST_DEVICE, 4, 1) == 0);
    CHECK(bus.probe(bus.opaque, SIM_TEST_DEVICE, 4, 2) != 0);
    CHECK(bus.read(bus.opaque, SIM_TEST_DEVICE, 1, &value) != 0);
    CHECK(bus.write(bus.opaque, SIM_TEST_DEVICE, 2, 1) != 0);
    CHECK(bus.write(bus.opaque, SIM_TEST_DEVICE + 1, 4, 1) != 0);
    CHECK(bus.probe(bus.opaque, SIM_TEST_DEVICE, 0, 0) != 0);
    CHECK(bus.probe(bus.opaque, SIM_TEST_DEVICE, 8, 0) != 0);
    CHECK(bus.probe(bus.opaque, SIM_TEST_DEVICE, 4, 3) != 0);
    CHECK(bus.probe(bus.opaque, SIM_TEST_DEVICE + SIM_DEVICE_SIZE, 4, 0) != 0);
    CHECK(memory.reads == reads && memory.writes == writes);
    CHECK(memory.device_reads == 0 && memory.device_writes == 0);

    CHECK(bus.write(bus.opaque, SIM_TEST_DEVICE, 4, 42) == 0);
    CHECK(bus.read(bus.opaque, SIM_TEST_DEVICE + SIM_DEVICE_SIZE - 4, 4, &value) == 0);
    CHECK(value == 42 && memory.device_reads == 1 && memory.device_writes == 1);
    CHECK(memory.reads == reads + 1 && memory.writes == writes + 1);
    sim_memory_end(&memory);
    CHECK(bus.probe(bus.opaque, SIM_TEST_DEVICE, 4, 0) != 0);
    sim_memory_end(&memory);
    puts("RAM/MMIO: address boundaries, permissions and side effects passed");
    return 0;
}
