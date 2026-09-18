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
static uint32_t imm(unsigned op, unsigned rd, unsigned f, unsigned rs, int value)
{
    return ((uint32_t)value & 4095) << 20 | rs << 15 | f << 12 | rd << 7 | op;
}
static uint32_t reg(unsigned f, unsigned f7)
{
    return f7 << 25 | 2u << 20 | 1u << 15 | f << 12 | 3u << 7 | 0x33;
}
static uint32_t store(unsigned f, unsigned rs, unsigned base, int offset)
{
    uint32_t v = (uint32_t)offset & 4095;
    return (v >> 5) << 25 | rs << 20 | base << 15 | f << 12 | (v & 31) << 7 | 0x23;
}
static uint32_t branch(unsigned f, int offset)
{
    uint32_t v = (uint32_t)offset & 8191;
    return (v >> 12) << 31 | ((v >> 5) & 63) << 25 | 2u << 20 | 1u << 15 |
           f << 12 | ((v >> 1) & 15) << 8 | ((v >> 11) & 1) << 7 | 0x63;
}
static uint32_t jal(unsigned rd, int offset)
{
    uint32_t v = (uint32_t)offset & 0x1fffff;
    return (v >> 20) << 31 | ((v >> 1) & 1023) << 21 |
           ((v >> 11) & 1) << 20 | ((v >> 12) & 255) << 12 | rd << 7 | 0x6f;
}
static void setup(const uint32_t *p, size_t n)
{
    memset(ram, 0, sizeof(ram));
    CoreSetup core_setup = {.program = p, .count = n, .data = ram,
                            .data_size = sizeof(ram), .base = BASE, .entry = BASE,
                            .config = in_core_default_config()};
    in_core_init(&core, &cpu, &core_setup);
    cases++;
}
static void run(void) { in_core_run(&core, 500); }
static void alu(uint32_t insn, uint32_t a, uint32_t b, uint32_t expected)
{
    setup(&insn, 1);
    cpu.regs[1] = a; cpu.regs[2] = b;
    run();
    unsigned latency = (insn & 0x7f) == 0x33 && (insn >> 25) == 1
                           ? (((insn >> 12) & 7) < 4 ? 3 : 32) : 1;
    CHECK(!cpu.trapped && core.stats.retired == 1 && cpu.clock == latency + 4);
    CHECK(core.stats.execute_stalls == latency - 1);
    CHECK(cpu.regs[3] == expected && cpu.regs[0] == 0);
}
static void test_alu(void)
{
    alu(reg(0,0), 0xffffffff, 1, 0);
    alu(reg(0,32), 0x80000000, 1, 0x7fffffff);
    alu(reg(1,0), 1, 33, 2);
    alu(reg(2,0), 0x80000000, 1, 1);
    alu(reg(3,0), 0x80000000, 1, 0);
    alu(reg(4,0), 0xaa, 0x55, 0xff);
    alu(reg(5,0), 0x80000000, 31, 1);
    alu(reg(5,32), 0x80000000, 31, 0xffffffff);
    alu(reg(5,32), 0x80000000, 32, 0x80000000);
    alu(reg(6,0), 0xa0, 5, 0xa5);
    alu(reg(7,0), 0xaa, 0xf, 0xa);
    alu(imm(0x13,3,0,1,-2048), 2047, 0, 0xffffffff);
    alu(imm(0x13,3,2,1,-1), 0x80000000, 0, 1);
    alu(imm(0x13,3,3,1,-1), 1, 0, 1);
    alu(imm(0x13,3,4,1,-1), 0x55, 0, 0xffffffaa);
    alu(imm(0x13,3,6,1,5), 0xa0, 0, 0xa5);
    alu(imm(0x13,3,7,1,15), 0xaa, 0, 0xa);
    alu(imm(0x13,3,1,1,31), 1, 0, 0x80000000);
    alu(imm(0x13,3,5,1,31), 0x80000000, 0, 1);
    alu(imm(0x13,3,5,1,0x41f), 0x80000000, 0, 0xffffffff);
    alu(0xabcde1b7, 0, 0, 0xabcde000); /* LUI */
    alu(0x00001197, 0, 0, BASE + 4096); /* AUIPC */
}
/* Expected values generated using arbitrary-precision integer arithmetic. */
static void test_m(void)
{
    /* 每行保留一组输入及 8 条 M 指令的期望值，便于逐组核对。 */
    /* clang-format off */
    static const struct { uint32_t a, b, expected[8]; } vectors[] = {
        {0x00000000u, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0x00000001u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0x00000002u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0x00000003u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0x00000007u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0x7fffffffu, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0x80000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0x80000001u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0xfffffffdu, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000000u, 0xffffffffu, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {0x00000001u, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000001u, 0x00000001u}},
        {0x00000001u, 0x00000001u, {0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
        {0x00000001u, 0x00000002u, {0x00000002u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u}},
        {0x00000001u, 0x00000003u, {0x00000003u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u}},
        {0x00000001u, 0x00000007u, {0x00000007u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u}},
        {0x00000001u, 0x7fffffffu, {0x7fffffffu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u}},
        {0x00000001u, 0x80000000u, {0x80000000u, 0xffffffffu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u}},
        {0x00000001u, 0x80000001u, {0x80000001u, 0xffffffffu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u}},
        {0x00000001u, 0xfffffffdu, {0xfffffffdu, 0xffffffffu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u}},
        {0x00000001u, 0xffffffffu, {0xffffffffu, 0xffffffffu, 0x00000000u, 0x00000000u, 0xffffffffu, 0x00000000u, 0x00000000u, 0x00000001u}},
        {0x00000002u, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000002u, 0x00000002u}},
        {0x00000002u, 0x00000001u, {0x00000002u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u, 0x00000000u, 0x00000000u}},
        {0x00000002u, 0x00000002u, {0x00000004u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
        {0x00000002u, 0x00000003u, {0x00000006u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u}},
        {0x00000002u, 0x00000007u, {0x0000000eu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u}},
        {0x00000002u, 0x7fffffffu, {0xfffffffeu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u}},
        {0x00000002u, 0x80000000u, {0x00000000u, 0xffffffffu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u}},
        {0x00000002u, 0x80000001u, {0x00000002u, 0xffffffffu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u}},
        {0x00000002u, 0xfffffffdu, {0xfffffffau, 0xffffffffu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u}},
        {0x00000002u, 0xffffffffu, {0xfffffffeu, 0xffffffffu, 0x00000001u, 0x00000001u, 0xfffffffeu, 0x00000000u, 0x00000000u, 0x00000002u}},
        {0x00000003u, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000003u, 0x00000003u}},
        {0x00000003u, 0x00000001u, {0x00000003u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000003u, 0x00000000u, 0x00000000u}},
        {0x00000003u, 0x00000002u, {0x00000006u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u, 0x00000001u, 0x00000001u}},
        {0x00000003u, 0x00000003u, {0x00000009u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
        {0x00000003u, 0x00000007u, {0x00000015u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000003u}},
        {0x00000003u, 0x7fffffffu, {0x7ffffffdu, 0x00000001u, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000003u}},
        {0x00000003u, 0x80000000u, {0x80000000u, 0xfffffffeu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000003u}},
        {0x00000003u, 0x80000001u, {0x80000003u, 0xfffffffeu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000003u}},
        {0x00000003u, 0xfffffffdu, {0xfffffff7u, 0xffffffffu, 0x00000002u, 0x00000002u, 0xffffffffu, 0x00000000u, 0x00000000u, 0x00000003u}},
        {0x00000003u, 0xffffffffu, {0xfffffffdu, 0xffffffffu, 0x00000002u, 0x00000002u, 0xfffffffdu, 0x00000000u, 0x00000000u, 0x00000003u}},
        {0x00000007u, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000007u, 0x00000007u}},
        {0x00000007u, 0x00000001u, {0x00000007u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000007u, 0x00000007u, 0x00000000u, 0x00000000u}},
        {0x00000007u, 0x00000002u, {0x0000000eu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000003u, 0x00000001u, 0x00000001u}},
        {0x00000007u, 0x00000003u, {0x00000015u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u, 0x00000001u, 0x00000001u}},
        {0x00000007u, 0x00000007u, {0x00000031u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
        {0x00000007u, 0x7fffffffu, {0x7ffffff9u, 0x00000003u, 0x00000003u, 0x00000003u, 0x00000000u, 0x00000000u, 0x00000007u, 0x00000007u}},
        {0x00000007u, 0x80000000u, {0x80000000u, 0xfffffffcu, 0x00000003u, 0x00000003u, 0x00000000u, 0x00000000u, 0x00000007u, 0x00000007u}},
        {0x00000007u, 0x80000001u, {0x80000007u, 0xfffffffcu, 0x00000003u, 0x00000003u, 0x00000000u, 0x00000000u, 0x00000007u, 0x00000007u}},
        {0x00000007u, 0xfffffffdu, {0xffffffebu, 0xffffffffu, 0x00000006u, 0x00000006u, 0xfffffffeu, 0x00000000u, 0x00000001u, 0x00000007u}},
        {0x00000007u, 0xffffffffu, {0xfffffff9u, 0xffffffffu, 0x00000006u, 0x00000006u, 0xfffffff9u, 0x00000000u, 0x00000000u, 0x00000007u}},
        {0x7fffffffu, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x7fffffffu, 0x7fffffffu}},
        {0x7fffffffu, 0x00000001u, {0x7fffffffu, 0x00000000u, 0x00000000u, 0x00000000u, 0x7fffffffu, 0x7fffffffu, 0x00000000u, 0x00000000u}},
        {0x7fffffffu, 0x00000002u, {0xfffffffeu, 0x00000000u, 0x00000000u, 0x00000000u, 0x3fffffffu, 0x3fffffffu, 0x00000001u, 0x00000001u}},
        {0x7fffffffu, 0x00000003u, {0x7ffffffdu, 0x00000001u, 0x00000001u, 0x00000001u, 0x2aaaaaaau, 0x2aaaaaaau, 0x00000001u, 0x00000001u}},
        {0x7fffffffu, 0x00000007u, {0x7ffffff9u, 0x00000003u, 0x00000003u, 0x00000003u, 0x12492492u, 0x12492492u, 0x00000001u, 0x00000001u}},
        {0x7fffffffu, 0x7fffffffu, {0x00000001u, 0x3fffffffu, 0x3fffffffu, 0x3fffffffu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
        {0x7fffffffu, 0x80000000u, {0x80000000u, 0xc0000000u, 0x3fffffffu, 0x3fffffffu, 0x00000000u, 0x00000000u, 0x7fffffffu, 0x7fffffffu}},
        {0x7fffffffu, 0x80000001u, {0xffffffffu, 0xc0000000u, 0x3fffffffu, 0x3fffffffu, 0xffffffffu, 0x00000000u, 0x00000000u, 0x7fffffffu}},
        {0x7fffffffu, 0xfffffffdu, {0x80000003u, 0xfffffffeu, 0x7ffffffdu, 0x7ffffffdu, 0xd5555556u, 0x00000000u, 0x00000001u, 0x7fffffffu}},
        {0x7fffffffu, 0xffffffffu, {0x80000001u, 0xffffffffu, 0x7ffffffeu, 0x7ffffffeu, 0x80000001u, 0x00000000u, 0x00000000u, 0x7fffffffu}},
        {0x80000000u, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x80000000u, 0x80000000u}},
        {0x80000000u, 0x00000001u, {0x80000000u, 0xffffffffu, 0xffffffffu, 0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u, 0x00000000u}},
        {0x80000000u, 0x00000002u, {0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000001u, 0xc0000000u, 0x40000000u, 0x00000000u, 0x00000000u}},
        {0x80000000u, 0x00000003u, {0x80000000u, 0xfffffffeu, 0xfffffffeu, 0x00000001u, 0xd5555556u, 0x2aaaaaaau, 0xfffffffeu, 0x00000002u}},
        {0x80000000u, 0x00000007u, {0x80000000u, 0xfffffffcu, 0xfffffffcu, 0x00000003u, 0xedb6db6eu, 0x12492492u, 0xfffffffeu, 0x00000002u}},
        {0x80000000u, 0x7fffffffu, {0x80000000u, 0xc0000000u, 0xc0000000u, 0x3fffffffu, 0xffffffffu, 0x00000001u, 0xffffffffu, 0x00000001u}},
        {0x80000000u, 0x80000000u, {0x00000000u, 0x40000000u, 0xc0000000u, 0x40000000u, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
        {0x80000000u, 0x80000001u, {0x80000000u, 0x3fffffffu, 0xbfffffffu, 0x40000000u, 0x00000001u, 0x00000000u, 0xffffffffu, 0x80000000u}},
        {0x80000000u, 0xfffffffdu, {0x80000000u, 0x00000001u, 0x80000001u, 0x7ffffffeu, 0x2aaaaaaau, 0x00000000u, 0xfffffffeu, 0x80000000u}},
        {0x80000000u, 0xffffffffu, {0x80000000u, 0x00000000u, 0x80000000u, 0x7fffffffu, 0x80000000u, 0x00000000u, 0x00000000u, 0x80000000u}},
        {0x80000001u, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x80000001u, 0x80000001u}},
        {0x80000001u, 0x00000001u, {0x80000001u, 0xffffffffu, 0xffffffffu, 0x00000000u, 0x80000001u, 0x80000001u, 0x00000000u, 0x00000000u}},
        {0x80000001u, 0x00000002u, {0x00000002u, 0xffffffffu, 0xffffffffu, 0x00000001u, 0xc0000001u, 0x40000000u, 0xffffffffu, 0x00000001u}},
        {0x80000001u, 0x00000003u, {0x80000003u, 0xfffffffeu, 0xfffffffeu, 0x00000001u, 0xd5555556u, 0x2aaaaaabu, 0xffffffffu, 0x00000000u}},
        {0x80000001u, 0x00000007u, {0x80000007u, 0xfffffffcu, 0xfffffffcu, 0x00000003u, 0xedb6db6eu, 0x12492492u, 0xffffffffu, 0x00000003u}},
        {0x80000001u, 0x7fffffffu, {0xffffffffu, 0xc0000000u, 0xc0000000u, 0x3fffffffu, 0xffffffffu, 0x00000001u, 0x00000000u, 0x00000002u}},
        {0x80000001u, 0x80000000u, {0x80000000u, 0x3fffffffu, 0xc0000000u, 0x40000000u, 0x00000000u, 0x00000001u, 0x80000001u, 0x00000001u}},
        {0x80000001u, 0x80000001u, {0x00000001u, 0x3fffffffu, 0xc0000000u, 0x40000001u, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
        {0x80000001u, 0xfffffffdu, {0x7ffffffdu, 0x00000001u, 0x80000002u, 0x7fffffffu, 0x2aaaaaaau, 0x00000000u, 0xffffffffu, 0x80000001u}},
        {0x80000001u, 0xffffffffu, {0x7fffffffu, 0x00000000u, 0x80000001u, 0x80000000u, 0x7fffffffu, 0x00000000u, 0x00000000u, 0x80000001u}},
        {0xfffffffdu, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0xfffffffdu, 0xfffffffdu}},
        {0xfffffffdu, 0x00000001u, {0xfffffffdu, 0xffffffffu, 0xffffffffu, 0x00000000u, 0xfffffffdu, 0xfffffffdu, 0x00000000u, 0x00000000u}},
        {0xfffffffdu, 0x00000002u, {0xfffffffau, 0xffffffffu, 0xffffffffu, 0x00000001u, 0xffffffffu, 0x7ffffffeu, 0xffffffffu, 0x00000001u}},
        {0xfffffffdu, 0x00000003u, {0xfffffff7u, 0xffffffffu, 0xffffffffu, 0x00000002u, 0xffffffffu, 0x55555554u, 0x00000000u, 0x00000001u}},
        {0xfffffffdu, 0x00000007u, {0xffffffebu, 0xffffffffu, 0xffffffffu, 0x00000006u, 0x00000000u, 0x24924924u, 0xfffffffdu, 0x00000001u}},
        {0xfffffffdu, 0x7fffffffu, {0x80000003u, 0xfffffffeu, 0xfffffffeu, 0x7ffffffdu, 0x00000000u, 0x00000001u, 0xfffffffdu, 0x7ffffffeu}},
        {0xfffffffdu, 0x80000000u, {0x80000000u, 0x00000001u, 0xfffffffeu, 0x7ffffffeu, 0x00000000u, 0x00000001u, 0xfffffffdu, 0x7ffffffdu}},
        {0xfffffffdu, 0x80000001u, {0x7ffffffdu, 0x00000001u, 0xfffffffeu, 0x7fffffffu, 0x00000000u, 0x00000001u, 0xfffffffdu, 0x7ffffffcu}},
        {0xfffffffdu, 0xfffffffdu, {0x00000009u, 0x00000000u, 0xfffffffdu, 0xfffffffau, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
        {0xfffffffdu, 0xffffffffu, {0x00000003u, 0x00000000u, 0xfffffffdu, 0xfffffffcu, 0x00000003u, 0x00000000u, 0x00000000u, 0xfffffffdu}},
        {0xffffffffu, 0x00000000u, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}},
        {0xffffffffu, 0x00000001u, {0xffffffffu, 0xffffffffu, 0xffffffffu, 0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000000u, 0x00000000u}},
        {0xffffffffu, 0x00000002u, {0xfffffffeu, 0xffffffffu, 0xffffffffu, 0x00000001u, 0x00000000u, 0x7fffffffu, 0xffffffffu, 0x00000001u}},
        {0xffffffffu, 0x00000003u, {0xfffffffdu, 0xffffffffu, 0xffffffffu, 0x00000002u, 0x00000000u, 0x55555555u, 0xffffffffu, 0x00000000u}},
        {0xffffffffu, 0x00000007u, {0xfffffff9u, 0xffffffffu, 0xffffffffu, 0x00000006u, 0x00000000u, 0x24924924u, 0xffffffffu, 0x00000003u}},
        {0xffffffffu, 0x7fffffffu, {0x80000001u, 0xffffffffu, 0xffffffffu, 0x7ffffffeu, 0x00000000u, 0x00000002u, 0xffffffffu, 0x00000001u}},
        {0xffffffffu, 0x80000000u, {0x80000000u, 0x00000000u, 0xffffffffu, 0x7fffffffu, 0x00000000u, 0x00000001u, 0xffffffffu, 0x7fffffffu}},
        {0xffffffffu, 0x80000001u, {0x7fffffffu, 0x00000000u, 0xffffffffu, 0x80000000u, 0x00000000u, 0x00000001u, 0xffffffffu, 0x7ffffffeu}},
        {0xffffffffu, 0xfffffffdu, {0x00000003u, 0x00000000u, 0xffffffffu, 0xfffffffcu, 0x00000000u, 0x00000001u, 0xffffffffu, 0x00000002u}},
        {0xffffffffu, 0xffffffffu, {0x00000001u, 0x00000000u, 0xffffffffu, 0xfffffffeu, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u}},
    };
    /* clang-format on */
    for (size_t j = 0; j < sizeof(vectors)/sizeof(vectors[0]); ++j)
        for (unsigned f = 0; f < 8; ++f)
            alu(reg(f,1), vectors[j].a, vectors[j].b, vectors[j].expected[f]);

    /* Both input dependencies, result consumption, and rd aliasing rs1. */
    uint32_t p[] = {imm(0x13,1,0,0,21), imm(0x13,2,0,0,4),
                    reg(0,1), imm(0x13,1,0,3,0),
                    (reg(4,1) & ~(31u << 7)) | (1u << 7),
                    store(2,1,0,0)};
    setup(p,6); run();
    CHECK(!cpu.trapped && core.stats.retired == 6);
    CHECK(cpu.regs[3] == 84 && cpu.regs[1] == 21 && ram[0] == 21);
    CHECK(core.stats.stalls == 8 && core.stats.execute_stalls == 33 && cpu.clock == 51);
    for (unsigned f = 0; f < 8; ++f) {
        uint32_t z[] = {reg(f,1) & ~(31u << 7), imm(0x13,3,0,0,9)};
        setup(z,2); cpu.regs[1] = 0x80000000u; cpu.regs[2] = 0; run();
        CHECK(!cpu.trapped && cpu.regs[0] == 0 && cpu.regs[3] == 9);
        CHECK(core.stats.retired == 2 && core.stats.stalls == 0);
    }
}

static void test_memory(void)
{
    const unsigned fs[] = {0,1,2,4,5};
    const uint32_t expected[] = {0xffffff80,0xffff8180,0x83828180,0x80,0x8180};
    for (unsigned j = 0; j < 5; ++j) {
        uint32_t p = imm(3,3,fs[j],1,-4);
        setup(&p,1); cpu.regs[1] = 8;
        ram[4]=0x80; ram[5]=0x81; ram[6]=0x82; ram[7]=0x83;
        run(); CHECK(!cpu.trapped && cpu.regs[3] == expected[j]);
    }
    for (unsigned f = 0; f < 3; ++f) {
        uint32_t p[] = {store(f,2,1,-4), imm(3,3,f == 2 ? 2 : f+4,1,-4)};
        setup(p,2); cpu.regs[1]=8; cpu.regs[2]=0x12345678;
        run(); CHECK(!cpu.trapped);
        uint32_t mask = f == 2 ? 0xffffffff : (1u << ((1u << f)*8))-1;
        CHECK(cpu.regs[3] == (0x12345678 & mask));
        CHECK(ram[3] == 0 && ram[4+(1u<<f)] == 0);
    }
}
static void test_hazards(void)
{
    uint32_t p[] = {imm(0x13,1,0,0,7), imm(0x13,1,0,1,1),
                    store(2,1,0,0), imm(3,2,2,0,0), reg(0,0)};
    setup(p,5); run();
    CHECK(!cpu.trapped && cpu.regs[1]==8 && cpu.regs[2]==8 && cpu.regs[3]==16);
    CHECK(core.stats.stalls == 6 && cpu.clock == 15 && core.stats.retired == 5);
    uint32_t z[] = {imm(0x13,0,0,0,5), imm(0x13,1,0,0,3)};
    setup(z,2); run(); CHECK(cpu.regs[0]==0 && cpu.regs[1]==3 && core.stats.stalls==0);
    uint32_t independent[] = {imm(0x13,1,0,0,1),imm(0x13,2,0,0,2),imm(0x13,3,0,0,3)};
    setup(independent,3); in_core_run(&core,2); CHECK(cpu.clock==2);
    run(); CHECK(cpu.clock==7 && core.stats.retired==3);
    run(); CHECK(cpu.clock==7);
    setup(NULL,0); run(); CHECK(cpu.clock==0);
}
static void test_control(void)
{
    const unsigned f[] = {0,1,4,5,6,7};
    for (unsigned j=0;j<6;++j) for (unsigned taken=0;taken<2;++taken) {
        uint32_t p[] = {branch(f[j],8), imm(0x13,3,0,0,9), imm(0x13,4,0,0,7)};
        setup(p,3);
        if (f[j]==0) { cpu.regs[1]=1; cpu.regs[2]=taken ? 1 : 2; }
        if (f[j]==1) { cpu.regs[1]=1; cpu.regs[2]=taken ? 2 : 1; }
        if (f[j]==4) { cpu.regs[1]=taken ? 0xffffffff : 2; cpu.regs[2]=1; }
        if (f[j]==5) { cpu.regs[1]=taken ? 1 : 0xffffffff; cpu.regs[2]=1; }
        if (f[j]==6) { cpu.regs[1]=taken ? 1 : 0xffffffff; cpu.regs[2]=2; }
        if (f[j]==7) { cpu.regs[1]=taken ? 0xffffffff : 1; cpu.regs[2]=2; }
        run(); CHECK(!cpu.trapped && cpu.regs[4]==7);
        CHECK(cpu.regs[3]==(taken?0u:9u) && core.stats.flushes==taken);
        CHECK(core.stats.retired==(taken?2u:3u));
    }
    uint32_t p[] = {jal(1,8), 0xffffffff, imm(0x13,3,0,1,0)};
    setup(p,3); run(); CHECK(!cpu.trapped && cpu.regs[3]==BASE+4 && core.stats.retired==2);
    uint32_t q[] = {imm(0x67,1,0,1,0),store(2,2,0,0),imm(0x13,3,0,1,0)};
    setup(q,3); cpu.regs[1]=BASE+9; cpu.regs[2]=42; run();
    CHECK(!cpu.trapped && cpu.regs[3]==BASE+4 && ram[0]==0);
    uint32_t loop[] = {imm(0x13,1,0,1,-1),branch(1,-4)};
    setup(loop,2); cpu.regs[1]=3; run();
    CHECK(!cpu.trapped && cpu.regs[1]==0 && core.stats.retired==6 && core.stats.flushes==2);
}
static void fault(uint32_t instruction, uint32_t cause, uint32_t value)
{
    uint32_t p[] = {imm(0x13,5,0,0,7), instruction, store(2,5,0,0),imm(0x13,6,0,0,9)};
    setup(p,4); run();
    CHECK(cpu.trapped && cpu.trap_cause==cause && cpu.trap_pc==BASE+4);
    CHECK(cpu.trap_value==value && core.stats.retired==1 && cpu.regs[5]==7);
    CHECK(ram[0]==0 && cpu.regs[6]==0);
}
static void test_faults(void)
{
    fault(0xffffffff,2,0xffffffff);
    fault(reg(0,2),2,reg(0,2)); /* 未支持的 funct7 */
    fault(imm(0x13,3,1,0,32),2,imm(0x13,3,1,0,32));
    fault(0x0000100f,2,0x0000100f); /* FENCE.I 是独立扩展 */
    fault(0x00000073,11,0);
    fault(0x00100073,3,BASE+4);
    fault(imm(3,0,2,0,1),4,1); /* 即使 rd=x0，load 仍触发异常 */
    fault(imm(3,3,2,0,64),5,64);
    fault(store(1,5,0,1),6,1);
    fault(store(2,5,0,64),7,64);
    fault(jal(3,2),0,BASE+6);
    uint32_t p[]={branch(1,2)}; setup(p,1); run(); CHECK(!cpu.trapped);
    uint32_t q[]={jal(0,12)}; setup(q,1); run();
    CHECK(cpu.trapped && cpu.trap_cause==1 && cpu.trap_pc==BASE+12 && core.stats.retired==1);
    uint32_t fence=0x0ff0000f; setup(&fence,1); run(); CHECK(!cpu.trapped && core.stats.retired==1);
}
static void test_m_timing(void)
{
    const unsigned latencies[] = {1, 2, 5, 32};
    for (unsigned kind = 0; kind < 2; ++kind)
        for (unsigned k = 0; k < 4; ++k)
        {
            unsigned latency = latencies[k];
            uint32_t p[] = {reg(kind ? 4 : 0, 1), imm(0x13,4,0,0,7), imm(0x13,5,0,0,9)};
            setup(p, 3);
            CHECK(!in_core_set_m_latency(&core, 0, 3));
            CHECK(!in_core_set_m_latency(&core, 3, 0));
            CHECK(core.config.mul_cycles == 3 && core.config.div_cycles == 32);
            CHECK(in_core_set_m_latency(&core, latency, latency));
            cpu.regs[1] = 21; cpu.regs[2] = 4;
            in_core_run(&core, 2); /* M 在 EX，后继在 ID。 */
            uint32_t fetch_pc = core.fetch_pc;
            CHECK(!in_core_set_m_latency(&core, 1, 1));
            for (unsigned wait = 0; wait + 1 < latency; ++wait)
            {
                in_core_run(&core, 1);
                CHECK(core.execute_stalled && core.execute.has_data);
                CHECK(core.decode.has_data && core.decode.latch.pc == BASE + 4);
                CHECK(core.fetch_pc == fetch_pc && !core.memory.has_data);
                CHECK(core.execute.latch.ex_cycles_left == latency - wait - 1);
                CHECK(cpu.regs[3] == 0 && core.stats.retired == 0);
            }
            in_core_run(&core, 1); /* EX 结果就绪，但不能提前写 rd。 */
            CHECK(!core.execute_stalled && core.memory.has_data && cpu.regs[3] == 0);
            in_core_run(&core, 1); /* MEM */
            CHECK(cpu.regs[3] == 0 && core.stats.retired == 0);
            in_core_run(&core, 1); /* WB */
            CHECK(cpu.regs[3] == (kind ? 5u : 84u) && core.stats.retired == 1);
            run();
            CHECK(cpu.clock == latency + 6 && core.stats.retired == 3);
            CHECK(core.stats.execute_stalls == latency - 1 && core.stats.stalls == 0);
            CHECK(cpu.regs[4] == 7 && cpu.regs[5] == 9);
        }

    /* MEM 反压优先：较老 AMO 的三拍期间，EX 中的 DIV 尚未启动。
     * 独立 DIV/MUL 也不重叠，单 EX 槽具有结构冒险。 */
    uint32_t mixed[] = {store(2,2,0,0), 0x002022af, reg(4,1), reg(0,1), imm(3,6,2,0,0)};
    setup(mixed, 5);
    CHECK(in_core_set_m_latency(&core, 3, 7));
    cpu.regs[1] = 21; cpu.regs[2] = 4;
    in_core_run(&core, 4);
    CHECK(core.execute.latch.insn == reg(4,1));
    for (unsigned i = 0; i < 2; ++i)
    {
        in_core_run(&core, 1);
        CHECK(core.memory_stalled && !core.execute_stalled && core.stats.execute_stalls == 0);
        CHECK(!core.execute.latch.m_result_ready && core.execute.latch.ex_cycles_left == 0);
        CHECK(core.stats.retired == 1); /* 较老 SW 只退休一次。 */
    }
    run();
    CHECK(cpu.clock == 19 && core.stats.retired == 5 && core.stats.execute_stalls == 8);
    CHECK(core.stats.memory_stalls == 2 && core.stats.stalls == 0);
    CHECK(cpu.regs[3] == 84 && cpu.regs[5] == 4 && cpu.regs[6] == 8 && ram[0] == 8);
    uint64_t total = cpu.clock;
    for (uint64_t split = 0; split <= total; ++split)
    {
        setup(mixed, 5);
        CHECK(in_core_set_m_latency(&core, 3, 7));
        cpu.regs[1] = 21; cpu.regs[2] = 4;
        in_core_run(&core, split);
        run();
        CHECK(cpu.clock == total && core.stats.retired == 5 && core.stats.execute_stalls == 8);
        CHECK(cpu.regs[3] == 84 && cpu.regs[5] == 4 && cpu.regs[6] == 8 && ram[0] == 8);
    }

    /* 更老的 MEM 异常取消尚未启动的 DIV。 */
    uint32_t bad[] = {imm(3,8,2,0,1), reg(4,1)};
    setup(bad, 2); run();
    CHECK(cpu.trapped && cpu.trap_cause == 4 && core.stats.retired == 0);
    CHECK(core.stats.execute_stalls == 0 && !core.execute.has_data && cpu.regs[3] == 0);

    /* 更年轻的 ECALL 只能在 DIV 完成后到达 WB。 */
    uint32_t younger[] = {reg(4,1), 0x00000073, store(2,2,0,0)};
    setup(younger, 3); cpu.regs[1] = 21; cpu.regs[2] = 4;
    run();
    CHECK(cpu.trapped && core.stats.retired == 1 && cpu.regs[3] == 5 && ram[0] == 0);
    CHECK(core.stats.execute_stalls == 31 && cpu.trap_pc == BASE + 4);

    uint32_t wrong_path[] = {jal(0,8), reg(4,1), imm(0x13,4,0,0,9)};
    setup(wrong_path, 3); run();
    CHECK(!cpu.trapped && core.stats.retired == 2 && core.stats.execute_stalls == 0 && cpu.regs[4] == 9);

    /* DIV 之后的跳转在 EX 等待期间不能提前重定向或执行错误路径 store。 */
    uint32_t jump[] = {reg(4,1), jal(0,8), store(2,2,0,0), imm(0x13,4,0,0,9)};
    setup(jump, 4); cpu.regs[1] = 21; cpu.regs[2] = 4;
    in_core_run(&core, 10);
    CHECK(core.stats.flushes == 0 && cpu.regs[4] == 0 && core.stats.retired == 0);
    run();
    CHECK(core.stats.flushes == 1 && core.stats.retired == 3 && cpu.regs[3] == 5 && cpu.regs[4] == 9);
    CHECK(ram[0] == 0 && core.stats.execute_stalls == 31);
}

static void test_run_configuration(void)
{
    uint32_t p[] = {reg(0, 1), store(2, 2, 0, 0), imm(0x13, 4, 0, 0, 9)};
    setup(p, 3);
    CHECK(in_core_set_m_latency(&core, 2, 5));
    core.config.stop_pc = BASE;
    core.config.stop_pc_valid = 1;
    cpu.regs[1] = 21;
    cpu.regs[2] = 4;
    run();
    /* 停止 PC 在 WB 生效：M 指令已退休，年轻 store 不能写 RAM。 */
    CHECK(core.halted && !cpu.trapped && cpu.clock == 6);
    CHECK(core.stats.retired == 1 && core.stats.execute_stalls == 1);
    CHECK(cpu.regs[3] == 84 && cpu.regs[4] == 0 && ram[0] == 0);
    run();
    CHECK(cpu.clock == 6 && core.stats.retired == 1 && ram[0] == 0);

    /* 复用同一核心时重新初始化，恢复默认延迟并清除停止条件和统计。 */
    setup(p, 3);
    cpu.regs[1] = 21;
    cpu.regs[2] = 4;
    run();
    CHECK(!cpu.trapped && cpu.clock == 9);
    CHECK(core.stats.retired == 3 && core.stats.execute_stalls == 2);
    CHECK(cpu.regs[3] == 84 && cpu.regs[4] == 9 && ram[0] == 4);
}

int main(void)
{
    test_alu(); test_m(); test_memory(); test_hazards(); test_control(); test_faults();
    test_m_timing();
    test_run_configuration();
    printf("RV32IM: %u directed cases passed\n", cases);
    return 0;
}
