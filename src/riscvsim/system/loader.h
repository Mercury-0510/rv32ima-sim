#ifndef SIM_LOADER_H
#define SIM_LOADER_H
#include "memory.h"
#include <stddef.h>
#include <stdint.h>

/* ELF32 小端可执行镜像的解析结果。地址均为客户机物理地址。
 * 取指窗口不覆盖全部段：只有 PF_X 段才可能被取指。 */
typedef struct ElfImage
{
    uint32_t entry;      /* e_entry：复位后的 PC。 */
    uint32_t ram_base;   /* 所有 PT_LOAD 的最低 vaddr，向下按页对齐。 */
    uint32_t ram_limit;  /* 所有 PT_LOAD 的最高 vaddr + memsz。 */
    uint32_t exec_base;  /* PF_X 段的最低 vaddr，取指窗口下界。 */
    uint32_t exec_limit; /* PF_X 段的最高 vaddr + memsz，取指窗口上界。 */
    uint32_t phoff;      /* 段头表偏移，供 elf_load_segments 复用。 */
    uint16_t phnum;
} ElfImage;

/* 三个函数失败时返回非 0，并把静态字符串写入 *error（可传 NULL）。 */

/* 只校验并解析，不接触内存。可执行段必须连续，否则拒绝。 */
int elf_parse(const uint8_t *buf, size_t size, ElfImage *image, const char **error);

/* 按 p_vaddr 写入 RAM，memsz > filesz 的部分清零（.bss）。 */
int elf_load_segments(const ElfImage *image, const uint8_t *buf, size_t size,
                      SimMemory *memory, const char **error);

/* 在 .symtab 中查符号地址；裸机静态链接产物没有 .dynsym。 */
int elf_find_symbol(const uint8_t *buf, size_t size, const char *name,
                    uint32_t *addr, const char **error);
#endif
