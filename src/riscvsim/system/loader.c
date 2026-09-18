#include "loader.h"
#include <string.h>

/* 项目自带一份最小 ELF32 定义，不依赖平台头文件，字段按小端从字节流读取。 */

/* 结构尺寸。 */
enum
{
    EHDR_SIZE = 52, /* Elf32_Ehdr */
    PHDR_SIZE = 32, /* Elf32_Phdr */
    SHDR_SIZE = 40, /* Elf32_Shdr */
    SYM_SIZE = 16,  /* Elf32_Sym */
};

/* e_ident 索引与取值。 */
enum
{
    EI_CLASS = 4,
    EI_DATA = 5,
    ELFCLASS32 = 1,
    ELFDATA2LSB = 1,
};

/* Elf32_Ehdr 字段偏移。 */
enum
{
    EHDR_TYPE = 16,
    EHDR_MACHINE = 18,
    EHDR_ENTRY = 24,
    EHDR_PHOFF = 28,
    EHDR_SHOFF = 32,
    EHDR_PHENTSIZE = 42,
    EHDR_PHNUM = 44,
    EHDR_SHENTSIZE = 46,
    EHDR_SHNUM = 48,
};

/* Elf32_Phdr 字段偏移。 */
enum
{
    PHDR_TYPE = 0,
    PHDR_OFFSET = 4,
    PHDR_VADDR = 8,
    PHDR_FILESZ = 16,
    PHDR_MEMSZ = 20,
    PHDR_FLAGS = 24,
};

/* Elf32_Shdr 字段偏移；SHDR_LEN 为 sh_size 字段本身的位置。 */
enum
{
    SHDR_TYPE = 4,
    SHDR_OFFSET = 16,
    SHDR_LEN = 20,
    SHDR_LINK = 24,
};

/* Elf32_Sym 字段偏移。 */
enum
{
    SYM_NAME = 0,
    SYM_VALUE = 4,
};

/* 常量取值。 */
enum
{
    ET_EXEC = 2,
    EM_RISCV = 243,
    PT_LOAD = 1,
    PF_X = 1,
    SHT_SYMTAB = 2,
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static int fail(const char **error, const char *message)
{
    if (error)
        *error = message;
    return -1;
}

/* 段/节头表整体落在文件内，且单条记录长度符合预期。 */
static int table_fits(uint32_t offset, uint32_t count, uint32_t entry_size,
                      size_t size)
{
    return offset <= size && (uint64_t)offset + (uint64_t)count * entry_size <= size;
}

/* ELF 文件头校验，返回段头表位置与数量。 */
static int read_ehdr(const uint8_t *buf, size_t size, uint32_t *phoff,
                     uint16_t *phnum, const char **error)
{
    if (size < EHDR_SIZE)
        return fail(error, "file is smaller than an ELF header");
    if (memcmp(buf, "\x7f" "ELF", 4))
        return fail(error, "not an ELF file");
    if (buf[EI_CLASS] != ELFCLASS32)
        return fail(error, "only ELF32 images are supported");
    if (buf[EI_DATA] != ELFDATA2LSB)
        return fail(error, "only little-endian images are supported");
    if (rd16(buf + EHDR_TYPE) != ET_EXEC)
        return fail(error, "only executable ELF images are supported");
    if (rd16(buf + EHDR_MACHINE) != EM_RISCV)
        return fail(error, "ELF image is not built for RISC-V");
    if (rd16(buf + EHDR_PHENTSIZE) != PHDR_SIZE)
        return fail(error, "unexpected program header entry size");

    *phoff = rd32(buf + EHDR_PHOFF);
    *phnum = rd16(buf + EHDR_PHNUM);
    if (!*phnum)
        return fail(error, "ELF image has no program headers");
    if (!table_fits(*phoff, *phnum, PHDR_SIZE, size))
        return fail(error, "program header table is truncated");
    return 0;
}

int elf_parse(const uint8_t *buf, size_t size, ElfImage *image, const char **error)
{
    uint32_t phoff;
    uint16_t phnum;
    if (read_ehdr(buf, size, &phoff, &phnum, error))
        return -1;
    uint32_t entry = rd32(buf + EHDR_ENTRY); /* 头部已确认在文件内 */

    /* 一次遍历同时统计取指窗口（PF_X 段）和 RAM 覆盖范围（全部段）。 */
    int loaded = 0, executable = 0;
    uint32_t ram_lo = 0, ram_hi = 0, exec_lo = 0, exec_hi = 0;
    for (uint16_t i = 0; i < phnum; i++)
    {
        const uint8_t *ph = buf + phoff + (size_t)i * PHDR_SIZE;
        if (rd32(ph + PHDR_TYPE) != PT_LOAD)
            continue;
        uint32_t offset = rd32(ph + PHDR_OFFSET);
        uint32_t vaddr = rd32(ph + PHDR_VADDR);
        uint32_t filesz = rd32(ph + PHDR_FILESZ);
        uint32_t memsz = rd32(ph + PHDR_MEMSZ);
        if (memsz < filesz)
            return fail(error, "segment is smaller in memory than in the file");
        if ((uint64_t)offset + filesz > size)
            return fail(error, "segment extends past the end of the file");
        /* 上界取 >= 2^32，保证 vaddr + memsz 在 32 位内不溢出。 */
        if ((uint64_t)vaddr + memsz > UINT32_C(0xffffffff))
            return fail(error, "segment extends past the 32-bit address space");

        if (!loaded || vaddr < ram_lo)
            ram_lo = vaddr;
        if (!loaded || vaddr + memsz > ram_hi)
            ram_hi = vaddr + memsz;
        loaded = 1;

        if (!(rd32(ph + PHDR_FLAGS) & PF_X))
            continue;
        if (!executable)
        {
            exec_lo = vaddr;
            exec_hi = vaddr + memsz;
            executable = 1;
        }
        else
        {
            /* 取指窗口是单一区间，可执行段之间不允许有空洞。 */
            if (vaddr > exec_hi || vaddr + memsz < exec_lo)
                return fail(error, "executable segments are not contiguous");
            if (vaddr < exec_lo)
                exec_lo = vaddr;
            if (vaddr + memsz > exec_hi)
                exec_hi = vaddr + memsz;
        }
    }
    if (!loaded)
        return fail(error, "ELF image has no loadable segment");
    if (!executable)
        return fail(error, "ELF image has no executable segment");
    if (entry < exec_lo || entry >= exec_hi)
        return fail(error, "entry point lies outside the executable segments");

    image->entry = entry;
    image->ram_base = ram_lo & ~UINT32_C(0xfff); /* sim_memory_init 要求页对齐 */
    image->ram_limit = ram_hi;
    image->exec_base = exec_lo;
    image->exec_limit = exec_hi;
    image->phoff = phoff;
    image->phnum = phnum;
    return 0;
}

int elf_load_segments(const ElfImage *image, const uint8_t *buf, size_t size,
                      SimMemory *memory, const char **error)
{
    for (uint16_t i = 0; i < image->phnum; i++)
    {
        const uint8_t *ph = buf + image->phoff + (size_t)i * PHDR_SIZE;
        if (rd32(ph + PHDR_TYPE) != PT_LOAD)
            continue;
        uint32_t offset = rd32(ph + PHDR_OFFSET);
        uint32_t vaddr = rd32(ph + PHDR_VADDR);
        uint32_t filesz = rd32(ph + PHDR_FILESZ);
        uint32_t memsz = rd32(ph + PHDR_MEMSZ);
        /* elf_parse 已校验段在文件内，这里只需确认它落在 RAM 窗口内。 */
        if ((uint64_t)offset + filesz > size)
            return fail(error, "segment extends past the end of the file");
        if (vaddr < memory->base ||
            (uint64_t)vaddr + memsz > (uint64_t)memory->base + memory->size)
            return fail(error, "segment does not fit in RAM; raise --ram-size");

        uint8_t *dest = memory->ram + (vaddr - memory->base);
        memcpy(dest, buf + offset, filesz);
        if (memsz > filesz)
            memset(dest + filesz, 0, memsz - filesz);
    }
    return 0;
}

int elf_find_symbol(const uint8_t *buf, size_t size, const char *name,
                    uint32_t *addr, const char **error)
{
    if (size < EHDR_SIZE)
        return fail(error, "file is smaller than an ELF header");
    uint32_t shoff = rd32(buf + EHDR_SHOFF);
    uint16_t shnum = rd16(buf + EHDR_SHNUM);
    if (rd16(buf + EHDR_SHENTSIZE) != SHDR_SIZE || !shnum)
        return fail(error, "ELF image has no section header table");
    if (!table_fits(shoff, shnum, SHDR_SIZE, size))
        return fail(error, "section header table is truncated");

    for (uint16_t i = 0; i < shnum; i++)
    {
        const uint8_t *sh = buf + shoff + (size_t)i * SHDR_SIZE;
        if (rd32(sh + SHDR_TYPE) != SHT_SYMTAB)
            continue;
        uint32_t str_index = rd32(sh + SHDR_LINK);
        if (str_index >= shnum)
            return fail(error, "symbol table links to a missing string table");
        const uint8_t *str_sh = buf + shoff + (size_t)str_index * SHDR_SIZE;

        uint32_t sym_off = rd32(sh + SHDR_OFFSET), sym_len = rd32(sh + SHDR_LEN);
        uint32_t str_off = rd32(str_sh + SHDR_OFFSET), str_len = rd32(str_sh + SHDR_LEN);
        if ((uint64_t)sym_off + sym_len > size ||
            (uint64_t)str_off + str_len > size)
            return fail(error, "symbol or string table is truncated");

        for (uint32_t at = 0; at + SYM_SIZE <= sym_len; at += SYM_SIZE)
        {
            const uint8_t *sym = buf + sym_off + at;
            uint32_t name_at = rd32(sym + SYM_NAME);
            if (name_at >= str_len)
                continue;
            const char *text = (const char *)buf + str_off + name_at;
            /* 名称必须以 NUL 结尾，否则整张表不可信。 */
            if (!memchr(text, 0, str_len - name_at))
                continue;
            if (!strcmp(text, name))
            {
                *addr = rd32(sym + SYM_VALUE);
                return 0;
            }
        }
        return fail(error, "symbol not found in ELF image");
    }
    return fail(error, "ELF image has no symbol table");
}
