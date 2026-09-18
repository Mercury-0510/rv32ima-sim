#include "core/core.h"
#include "core/cpu_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define BASE UINT32_C(0x80000000)
static INCore core;
static RISCVSIMCPUState cpu;
static uint8_t ram[64];
static unsigned cases;

static uint32_t amo(unsigned f5, unsigned rd, unsigned rs1, unsigned rs2, unsigned order)
{
    return f5 << 27 | order << 25 | rs2 << 20 | rs1 << 15 | 2u << 12 | rd << 7 | 0x2f;
}

static void setup(const uint32_t *program, size_t count)
{
    memset(ram, 0, sizeof(ram));
    CoreSetup core_setup = {.program = program, .count = count, .data = ram,
                            .data_size = sizeof(ram), .base = BASE, .entry = BASE,
                            .config = in_core_default_config()};
    in_core_init(&core, &cpu, &core_setup);
    cpu.regs[1] = 16;
    cpu.regs[2] = 20;
    ram[16] = 10;
    cases++;
}

static void test_swap_timing(void)
{
    for (unsigned order = 0; order < 4; ++order)
    {
        uint32_t p[] = {amo(1, 3, 1, 2, order), 0x00100213, 0x00200293};
        setup(p, 3);
        in_core_run(&core, 3); /* IF, ID, EX */
        CHECK(core.memory.has_data && cpu.clock == 3);
        CPUStage held_ex = core.execute, held_id = core.decode;
        uint32_t held_pc = core.fetch_pc;
        for (unsigned tick = 0; tick < 2; ++tick)
        {
            in_core_run(&core, 1); /* MEM read, MEM modify */
            CHECK(ram[16] == 10 && cpu.regs[3] == 0 && core.stats.retired == 0);
            CHECK(core.execute.latch.pc == held_ex.latch.pc);
            CHECK(core.decode.latch.pc == held_id.latch.pc && core.fetch_pc == held_pc);
            CHECK(!core.commit.has_data && core.memory_stalled);
        }
        in_core_run(&core, 1); /* MEM write */
        CHECK(ram[16] == 20 && cpu.regs[3] == 0 && core.stats.retired == 0);
        in_core_run(&core, 1); /* WB */
        CHECK(cpu.regs[3] == 10 && core.stats.retired == 1);
        in_core_run(&core, 100);
        CHECK(!cpu.trapped && core.stats.retired == 3 && cpu.clock == 9);
        CHECK(core.stats.memory_stalls == 2 && core.stats.stalls == 0);
        CHECK(cpu.regs[4] == 1 && cpu.regs[5] == 2);
        in_core_run(&core, 100);
        CHECK(cpu.clock == 9); /* 排空后不能重放。 */
    }
    uint32_t p = amo(1, 0, 1, 2, 0);
    setup(&p, 1);
    in_core_run(&core, 100);
    CHECK(cpu.regs[0] == 0 && ram[16] == 20 && cpu.clock == 7);
}

static uint32_t word(unsigned address)
{
    return (uint32_t)ram[address] | (uint32_t)ram[address+1] << 8 |
           (uint32_t)ram[address+2] << 16 | (uint32_t)ram[address+3] << 24;
}

static void put_word(unsigned address, uint32_t value)
{
    for (unsigned j = 0; j < 4; ++j)
        ram[address+j] = (uint8_t)(value >> (j*8));
}

static void test_amo_values(void)
{
    static const unsigned operations[] = {0, 1, 4, 8, 12, 16, 20, 24, 28};
    static const struct { uint32_t old, operand, expected[9]; } vectors[] = {
        {10, 3, {13, 3, 9, 11, 2, 3, 10, 3, 10}},
        {0xffffffffu, 1, {0, 1, 0xfffffffeu, 0xffffffffu, 1,
                          0xffffffffu, 1, 1, 0xffffffffu}},
        {0x80000000u, 0x7fffffffu,
            {0xffffffffu, 0x7fffffffu, 0xffffffffu, 0xffffffffu, 0,
             0x80000000u, 0x7fffffffu, 0x7fffffffu, 0x80000000u}},
        {0x7fffffffu, 0x80000000u,
            {0xffffffffu, 0x80000000u, 0xffffffffu, 0xffffffffu, 0,
             0x80000000u, 0x7fffffffu, 0x7fffffffu, 0x80000000u}},
        {0x80000000u, 0x80000000u,
            {0, 0x80000000u, 0, 0x80000000u, 0x80000000u,
             0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}}
    };
    for (unsigned op = 0; op < 9; ++op)
        for (unsigned v = 0; v < sizeof(vectors)/sizeof(vectors[0]); ++v)
            for (unsigned order = 0; order < 4; ++order)
                for (unsigned rd = 0; rd < 4; ++rd) /* x0, rs1/rs2 alias, normal rd */
                {
                    uint32_t p = amo(operations[op], rd, 1, 2, order);
                    setup(&p, 1);
                    put_word(16, vectors[v].old);
                    cpu.regs[2] = vectors[v].operand;
                    in_core_run(&core, 100);
                    CHECK(!cpu.trapped && cpu.clock == 7 && core.stats.retired == 1);
                    CHECK(word(16) == vectors[v].expected[op]);
                    CHECK(cpu.regs[rd] == (rd ? vectors[v].old : 0));
                    CHECK(ram[15] == 0 && ram[20] == 0 && core.stats.memory_stalls == 2);
                }
}

static void test_amo_pipeline(void)
{
    /* 两个源依赖、返回旧值的依赖、后继 load 和 store。 */
    uint32_t p[] = {0x01000093, 0x00300113, amo(0, 3, 1, 2, 3),
                    0x00118213, 0x0000a283, 0x0040a223};
    setup(p, 6);
    in_core_run(&core, 100);
    CHECK(!cpu.trapped && core.stats.retired == 6);
    CHECK(cpu.regs[3] == 10 && cpu.regs[4] == 11 && cpu.regs[5] == 13);
    CHECK(word(16) == 13 && word(20) == 11);
    /* 源操作数等 2 拍、AMO 返回值等 2 拍、末尾 store 等 x4 再等 1 拍。 */
    CHECK(core.stats.stalls == 5 && core.stats.memory_stalls == 2 && cpu.clock == 17);

    /* 较老的 store 在 AMO 读取前完成；紧邻 load 不能越过 AMO。 */
    uint32_t q[] = {0x0020a023, amo(0, 3, 1, 2, 0), 0x0000a203,
                    amo(0, 5, 1, 2, 0), amo(1, 6, 1, 2, 0)};
    setup(q, 5);
    in_core_run(&core, 100);
    CHECK(core.stats.retired == 5 && !cpu.trapped);
    CHECK(cpu.regs[3] == 20 && cpu.regs[4] == 40);
    CHECK(cpu.regs[5] == 40 && cpu.regs[6] == 60 && word(16) == 20);
    CHECK(core.stats.memory_stalls == 6 && core.stats.stalls == 0 && cpu.clock == 15);
}

static uint32_t store(unsigned width_f3, unsigned rs2, unsigned rs1, unsigned offset)
{
    return (offset >> 5) << 25 | rs2 << 20 | rs1 << 15 | width_f3 << 12 |
           (offset & 31) << 7 | 0x23;
}

static void test_lr_sc(void)
{
    for (unsigned order = 0; order < 4; ++order)
    {
        uint32_t p[] = {amo(2, 3, 1, 0, order), amo(3, 4, 1, 2, order)};
        setup(p, 2);
        cpu.regs[4] = 99;
        in_core_run(&core, 3);
        CHECK(!cpu.reservation_valid);
        in_core_run(&core, 1); /* LR MEM */
        CHECK(cpu.reservation_valid && cpu.reservation_addr == 16);
        CHECK(cpu.regs[3] == 0 && word(16) == 10);
        in_core_run(&core, 1); /* LR WB, SC 检查 */
        CHECK(cpu.regs[3] == 10 && !cpu.reservation_valid);
        CHECK(cpu.regs[4] == 99 && word(16) == 10 && core.memory_stalled);
        in_core_run(&core, 1); /* SC 写入 */
        CHECK(word(16) == 20 && cpu.regs[4] == 99);
        in_core_run(&core, 1); /* SC WB */
        CHECK(cpu.regs[4] == 0 && core.stats.retired == 2 && cpu.clock == 7);
        CHECK(core.stats.memory_stalls == 1 && core.stats.stalls == 0);

        uint32_t sc = amo(3, 3, 1, 2, order);
        setup(&sc, 1);
        in_core_run(&core, 100);
        CHECK(cpu.regs[3] == 1 && word(16) == 10 && !cpu.reservation_valid);
        CHECK(cpu.clock == 6 && core.stats.retired == 1 && core.stats.memory_stalls == 1);

        /* LR/ADDI/SC/BNE：真实的数据依赖和结果分支，单 hart 应取得进展。 */
        uint32_t loop[] = {amo(2, 3, 1, 0, order), 0x00118193,
                           amo(3, 4, 1, 3, order), 0xfe021ae3}; /* bne x4,x0,-12 */
        setup(loop, 4);
        in_core_run(&core, 100);
        CHECK(!cpu.trapped && core.stats.retired == 4 && cpu.regs[4] == 0);
        CHECK(word(16) == 11 && core.stats.stalls == 6 && core.stats.memory_stalls == 1);
        CHECK(cpu.clock == 15 && !cpu.reservation_valid);
    }

    /* LR rd=x0 仍建立保留；SC 成功或失败都消费保留。 */
    uint32_t twice[] = {amo(2, 0, 1, 0, 0), amo(3, 0, 1, 2, 0), amo(3, 3, 1, 2, 0)};
    setup(twice, 3);
    in_core_run(&core, 100);
    CHECK(cpu.regs[0] == 0 && cpu.regs[3] == 1 && word(16) == 20);
    CHECK(core.stats.retired == 3 && !cpu.reservation_valid);

    /* 新 LR 替换保留；地址不匹配的 SC 也会使新保留失效。 */
    uint32_t replace[] = {amo(2, 3, 1, 0, 0), amo(2, 4, 6, 0, 0),
                          amo(3, 5, 1, 2, 0), amo(3, 7, 6, 2, 0)};
    setup(replace, 4);
    cpu.regs[6] = 20;
    put_word(20, 30);
    in_core_run(&core, 100);
    CHECK(cpu.regs[3] == 10 && cpu.regs[4] == 30);
    CHECK(cpu.regs[5] == 1 && cpu.regs[7] == 1);
    CHECK(word(16) == 10 && word(20) == 30 && !cpu.reservation_valid);

    for (unsigned rd = 0; rd < 4; ++rd)
    {
        uint32_t lr = amo(2, rd, 1, 0, 0);
        setup(&lr, 1);
        in_core_run(&core, 100);
        CHECK(cpu.regs[rd] == (rd ? 10u : 0u));
        CHECK(cpu.reservation_addr == 16 && cpu.reservation_valid && cpu.clock == 5);
        uint32_t pair[] = {amo(2, 0, 1, 0, 0), amo(3, rd, 1, 2, 0)};
        setup(pair, 2);
        in_core_run(&core, 100);
        CHECK(cpu.regs[rd] == 0 && word(16) == 20 && !cpu.reservation_valid);
    }
}

static void test_reservation_writes(void)
{
    for (unsigned f3 = 0; f3 < 3; ++f3)
        for (unsigned offset = 0; offset < 8; offset += 1u << f3)
        {
            uint32_t p[] = {amo(2, 3, 1, 0, 0), store(f3, 2, 1, offset),
                            amo(3, 4, 1, 2, 0)};
            setup(p, 3);
            in_core_run(&core, 100);
            CHECK(!cpu.trapped && core.stats.retired == 3);
            CHECK(cpu.regs[4] == (offset < 4 ? 1u : 0u));
            CHECK(!cpu.reservation_valid);
        }
    const unsigned ops[] = {0, 1, 4, 8, 12, 16, 20, 24, 28};
    for (unsigned op = 0; op < 9; ++op)
        for (unsigned overlap = 0; overlap < 2; ++overlap)
        {
            uint32_t p[] = {amo(2, 3, 1, 0, 0), amo(ops[op], 0, 6, 2, 0),
                            amo(3, 4, 1, 2, 0)};
            setup(p, 3);
            cpu.regs[6] = overlap ? 16 : 20;
            cpu.regs[2] = 10; /* 值不变的写也必须失效。 */
            in_core_run(&core, 100);
            CHECK(!cpu.trapped && core.stats.retired == 3);
            CHECK(cpu.regs[4] == overlap && !cpu.reservation_valid);
        }
}

static void check_fault(uint32_t instruction, uint32_t address, uint32_t cause, uint32_t value)
{
    uint32_t p[] = {0x00700293, instruction, store(2, 2, 0, 16), 0x00900313};
    setup(p, 4);
    cpu.regs[1] = address;
    cpu.reservation_valid = 1;
    cpu.reservation_addr = 16;
    uint8_t before[sizeof(ram)];
    memcpy(before, ram, sizeof(ram));
    in_core_run(&core, 100);
    CHECK(cpu.trapped && core.halted && cpu.trap_cause == cause);
    CHECK(cpu.trap_pc == BASE + 4 && cpu.trap_value == value);
    CHECK(cpu.regs[5] == 7 && core.stats.retired == 1 && cpu.regs[3] == 0 && cpu.regs[6] == 0);
    CHECK(!memcmp(before, ram, sizeof(ram)) && core.stats.memory_stalls == 0);
    CHECK(cpu.reservation_valid && cpu.reservation_addr == 16);
    uint64_t stopped = cpu.clock;
    in_core_run(&core, 100);
    CHECK(cpu.clock == stopped);
}

static void test_faults(void)
{
    const unsigned ops[] = {0, 1, 2, 3, 4, 8, 12, 16, 20, 24, 28};
    for (unsigned op = 0; op < 11; ++op)
        for (unsigned order = 0; order < 4; ++order)
        {
            uint32_t p = amo(ops[op], 3, 1, ops[op] == 2 ? 0 : 2, order);
            for (unsigned offset = 1; offset < 4; ++offset)
                check_fault(p, 16 + offset, ops[op] == 2 ? 4 : 6, 16 + offset);
            check_fault(p, 64, ops[op] == 2 ? 5 : 7, 64);
            check_fault(p, 0xfffffffcu, ops[op] == 2 ? 5 : 7, 0xfffffffcu);
            for (unsigned f3 = 0; f3 < 8; ++f3)
                if (f3 != 2)
                {
                    uint32_t bad = (p & ~(7u << 12)) | f3 << 12;
                    check_fault(bad, 16, 2, bad);
                }
        }
    for (unsigned f5 = 0; f5 < 32; ++f5)
    {
        int legal = 0;
        for (unsigned i = 0; i < 11; ++i) legal |= f5 == ops[i];
        if (!legal)
        {
            uint32_t bad = amo(f5, 3, 1, 2, 0);
            check_fault(bad, 16, 2, bad);
        }
    }
    for (unsigned rs2 = 1; rs2 < 32; ++rs2)
    {
        uint32_t bad = amo(2, 3, 1, rs2, 0);
        check_fault(bad, 16, 2, bad);
    }
    /* rd=x0 不屏蔽异常；SC 没有保留仍按本模型执行地址检查。 */
    check_fault(amo(1, 0, 1, 2, 0), 1, 6, 1);
    uint32_t sc = amo(3, 3, 1, 2, 0);
    setup(&sc, 1);
    cpu.regs[1] = 64;
    in_core_run(&core, 100);
    CHECK(cpu.trapped && cpu.trap_cause == 7 && core.stats.retired == 0);
    uint32_t lr = amo(2, 3, 1, 0, 0);
    setup(&lr, 1);
    core.data = NULL;
    in_core_run(&core, 100);
    CHECK(cpu.trapped && cpu.trap_cause == 5 && !cpu.reservation_valid);
}

static void test_flush_and_resume(void)
{
    const unsigned ops[] = {1, 2, 3};
    for (unsigned j = 0; j < 3; ++j)
    {
        uint32_t a = amo(ops[j], 3, 1, ops[j] == 2 ? 0 : 2, 0);
        uint32_t p[] = {0x0080006f, a, 0x00700293}; /* jal x0,+8 */
        setup(p, 3);
        cpu.reservation_valid = 1;
        cpu.reservation_addr = 20;
        in_core_run(&core, 100);
        CHECK(!cpu.trapped && core.stats.retired == 2 && core.stats.flushes == 1);
        CHECK(word(16) == 10 && cpu.regs[3] == 0 && cpu.regs[5] == 7);
        CHECK(cpu.reservation_valid && cpu.reservation_addr == 20 && core.stats.memory_stalls == 0);

        uint32_t q[] = {0x00000073, a}; /* 较老 ECALL 阻止原子请求 */
        setup(q, 2);
        in_core_run(&core, 100);
        CHECK(cpu.trapped && core.stats.retired == 0 && core.stats.memory_stalls == 0);
        CHECK(word(16) == 10 && !cpu.reservation_valid);

        uint32_t fault[] = {0x00102403, a}; /* 较老的未对齐 LW 在 MEM 报错 */
        setup(fault, 2);
        in_core_run(&core, 100);
        CHECK(cpu.trapped && cpu.trap_cause == 4 && core.stats.retired == 0);
        CHECK(word(16) == 10 && !cpu.reservation_valid && core.stats.memory_stalls == 0);
    }
    /* AMO 阻塞期间 EX 中的跳转也必须保持，完成后只能重定向一次。 */
    uint32_t p[] = {amo(0, 3, 1, 2, 0), 0x0080006f, amo(1, 4, 1, 2, 0), 0x00700293};
    setup(p, 4);
    in_core_run(&core, 100);
    CHECK(!cpu.trapped && core.stats.retired == 3 && core.stats.flushes == 1);
    CHECK(word(16) == 30 && cpu.regs[3] == 10 && cpu.regs[4] == 0);

    /* AMO 先完成，后继异常才报告，内存不能回滚或重复写入。 */
    uint32_t q[] = {amo(0, 3, 1, 2, 0), 0x00000073, amo(0, 4, 1, 2, 0)};
    setup(q, 3);
    in_core_run(&core, 100);
    CHECK(cpu.trapped && cpu.trap_cause == 11 && cpu.trap_pc == BASE + 4);
    CHECK(core.stats.retired == 1 && word(16) == 30 && cpu.regs[3] == 10 && cpu.regs[4] == 0);

    /* 每个可能的周期预算切点都应与一次运行得到相同的时序/状态。 */
    uint32_t r[] = {amo(2, 3, 1, 0, 0), amo(3, 4, 1, 2, 0),
                    amo(0, 5, 1, 2, 0), 0x0000a303};
    setup(r, 4);
    in_core_run(&core, 100);
    RISCVSIMCPUState expected_cpu = cpu;
    uint64_t cycles = cpu.clock, mem_stalls = core.stats.memory_stalls;
    uint8_t expected_ram[sizeof(ram)];
    memcpy(expected_ram, ram, sizeof(ram));
    for (uint64_t split = 0; split <= cycles; ++split)
    {
        setup(r, 4);
        in_core_run(&core, split);
        in_core_run(&core, 100);
        CHECK(cpu.clock == cycles && core.stats.memory_stalls == mem_stalls && core.stats.retired == 4);
        CHECK(!memcmp(cpu.regs, expected_cpu.regs, sizeof(cpu.regs)));
        CHECK(cpu.reservation_valid == expected_cpu.reservation_valid && !cpu.trapped);
        CHECK(!memcmp(expected_ram, ram, sizeof(ram)));
    }
}

static void test_ram_boundaries(void)
{
    const unsigned ops[] = {1, 2, 3};
    for (unsigned i = 0; i < 3; ++i)
    {
        uint32_t p = amo(ops[i], 3, 1, ops[i] == 2 ? 0 : 2, 0);
        for (unsigned address = 0; address <= 60; address += 60)
        {
            setup(&p, 1);
            cpu.regs[1] = address;
            cpu.reservation_valid = 1;
            cpu.reservation_addr = address;
            put_word(address, 0x89abcdefu);
            in_core_run(&core, 100);
            CHECK(!cpu.trapped && core.stats.retired == 1);
            CHECK(cpu.regs[3] == (ops[i] == 3 ? 0u : 0x89abcdefu));
            CHECK(word(address) == (ops[i] == 2 ? 0x89abcdefu : 20u));
        }
        setup(&p, 1);
        core.data_size = 18; /* 起始地址有效，但剩余字节不足。 */
        in_core_run(&core, 100);
        CHECK(cpu.trapped && cpu.trap_cause == (ops[i] == 2 ? 5u : 7u));
        CHECK(word(16) == 10 && !cpu.reservation_valid && core.stats.retired == 0);
    }
}

int main(void)
{
    test_swap_timing();
    test_amo_values();
    test_amo_pipeline();
    test_lr_sc();
    test_reservation_writes();
    test_faults();
    test_flush_and_resume();
    test_ram_boundaries();
    printf("RV32A: %u directed cases passed\n", cases);
    return 0;
}
