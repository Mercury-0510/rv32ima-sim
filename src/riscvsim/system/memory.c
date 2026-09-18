#include "memory.h"
#include <stdlib.h>
#include <string.h>

/* 当前地址空间只有 RAM 和测试设备，区域不重叠。 */
typedef enum MemoryRegion
{
    REGION_NONE,
    REGION_RAM,
    REGION_DEVICE
} MemoryRegion;

static int contains(uint32_t base, size_t size, uint32_t addr, unsigned width)
{
    return addr >= base && (uint64_t)addr - base + width <= size;
}

static MemoryRegion find_region(const SimMemory *m, uint32_t addr, unsigned width)
{
    if (contains(m->base, m->size, addr, width))
        return REGION_RAM;
    if (contains(SIM_TEST_DEVICE, SIM_DEVICE_SIZE, addr, width))
        return REGION_DEVICE;
    return REGION_NONE;
}

int sim_memory_init(SimMemory *m, uint32_t base, size_t size)
{
    memset(m, 0, sizeof(*m));
    if (!size || (size & 4095) || (base & 4095) ||
        size > UINT64_C(0x100000000) - base)
        return -1;
    if (base < (uint64_t)SIM_TEST_DEVICE + SIM_DEVICE_SIZE &&
        (uint64_t)base + size > SIM_TEST_DEVICE)
        return -1;
    m->ram = calloc(1, size);
    if (!m->ram)
        return -1;
    m->base = base;
    m->size = size;
    return 0;
}

void sim_memory_end(SimMemory *m)
{
    free(m->ram);
    memset(m, 0, sizeof(*m));
}

/* 检查地址和访问权限，不读取设备，也不更新访问计数。 */
static int probe(void *opaque, uint32_t addr, unsigned width, int access)
{
    const SimMemory *m = opaque;
    if (!m->ram || access < 0 || access > 2 ||
        (width != 1 && width != 2 && width != 4) || (addr & (width - 1)) ||
        (uint64_t)addr + width > UINT64_C(0x100000000))
        return -1;
    switch (find_region(m, addr, width))
    {
    case REGION_RAM:
        return 0;
    case REGION_DEVICE:
        return width == 4 && access != 2 ? 0 : -1;
    default:
        return -1;
    }
}

static int read_bus(void *opaque, uint32_t addr, unsigned width, uint32_t *value)
{
    SimMemory *m = opaque;
    if (probe(m, addr, width, 0))
        return -1;
    m->reads++;
    if (find_region(m, addr, width) == REGION_DEVICE)
    {
        /* 设备页内所有对齐的字地址访问同一个测试锁存器。 */
        m->device_reads++;
        *value = m->device_value;
        return 0;
    }
    const uint8_t *ptr = m->ram + (addr - m->base);
    *value = 0;
    for (unsigned i = 0; i < width; ++i)
        *value |= (uint32_t)ptr[i] << (i * 8);
    return 0;
}

static int write_bus(void *opaque, uint32_t addr, unsigned width, uint32_t value)
{
    SimMemory *m = opaque;
    if (probe(m, addr, width, 1))
        return -1;
    m->writes++;
    if (find_region(m, addr, width) == REGION_DEVICE)
    {
        m->device_writes++;
        m->device_value = value;
        return 0;
    }
    uint8_t *ptr = m->ram + (addr - m->base);
    for (unsigned i = 0; i < width; ++i)
        ptr[i] = (uint8_t)(value >> (i * 8));
    return 0;
}

SimBus sim_memory_bus(SimMemory *m)
{
    return (SimBus){m, probe, read_bus, write_bus};
}
