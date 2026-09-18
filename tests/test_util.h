#ifndef TEST_UTIL_H
#define TEST_UTIL_H

/* 必须最先包含。_POSIX_C_SOURCE 要抢在 glibc 的 features.h 之前生效，否则
 * -std=c11 下不会声明 open_memstream、fork、symlink、mkdtemp 这些接口。 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* 断言失败立即中止整个测试二进制，打印「文件:行号: 表达式」。 */
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* 测试统一使用的装载地址。 */
#define BASE UINT32_C(0x80000000)

/* 指令编码器。用 static inline 而非 static：头文件里的 static 函数在未使用它
 * 的编译单元里会触发 -Wunused-function，而构建开着 -Werror。 */

/* 立即数类（I 型）：rd = rs op imm。 */
static inline uint32_t imm(unsigned op, unsigned rd, unsigned f, unsigned rs, int value)
{
    return ((uint32_t)value & 4095) << 20 | rs << 15 | f << 12 | rd << 7 | op;
}

/* 寄存器类（R 型）：x3 = x1 op x2，供 ALU 用例复用。 */
static inline uint32_t reg(unsigned f, unsigned f7)
{
    return f7 << 25 | 2u << 20 | 1u << 15 | f << 12 | 3u << 7 | 0x33;
}

/* 存储类（S 型）：*(base + offset) = rs。 */
static inline uint32_t store(unsigned f, unsigned rs, unsigned base, int offset)
{
    uint32_t v = (uint32_t)offset & 4095;
    return (v >> 5) << 25 | rs << 20 | base << 15 | f << 12 | (v & 31) << 7 | 0x23;
}

/* 分支类（B 型），目标为相对当前 PC 的偏移。 */
static inline uint32_t branch(unsigned f, int offset)
{
    uint32_t v = (uint32_t)offset & 8191;
    return (v >> 12) << 31 | ((v >> 5) & 63) << 25 | 2u << 20 | 1u << 15 | f << 12 |
           ((v >> 1) & 15) << 8 | ((v >> 11) & 1) << 7 | 0x63;
}

/* 跳转类（J 型）。 */
static inline uint32_t jal(unsigned rd, int offset)
{
    uint32_t v = (uint32_t)offset & 0x1fffff;
    return (v >> 20) << 31 | ((v >> 1) & 1023) << 21 | ((v >> 11) & 1) << 20 |
           ((v >> 12) & 255) << 12 | rd << 7 | 0x6f;
}

/* 原子类（AMO）：f5 选操作，order 的 bit0 是 .aq、bit1 是 .rl。 */
static inline uint32_t amo(unsigned f5, unsigned rd, unsigned rs1, unsigned rs2, unsigned order)
{
    return f5 << 27 | order << 25 | rs2 << 20 | rs1 << 15 | 2u << 12 | rd << 7 | 0x2f;
}

#endif
