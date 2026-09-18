/* ELF 装载器的直接测试：手工拼出镜像字节，调用 loader.h 的三个接口，检查结果。
 * 全程在进程内完成，既不起子进程也不写临时文件。 */
#include "test_util.h"
#include "system/loader.h"
#include "system/memory.h"
#include <string.h>

/* 结构尺寸、e_ident 取值与常量，含义同 loader.c。 */
enum
{
    EHDR_SIZE = 52, /* Elf32_Ehdr */
    PHDR_SIZE = 32, /* Elf32_Phdr */
    SHDR_SIZE = 40, /* Elf32_Shdr */
    SYM_SIZE = 16,  /* Elf32_Sym */
};

enum
{
    ET_EXEC = 2,
    ET_DYN = 3,
    EM_RISCV = 243,
    EM_X86_64 = 62,
    PT_LOAD = 1,
    PF_X = 1,
    PF_W = 2,
    PF_R = 4,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
};

#define BLOB_CAP 512

static unsigned cases;

/* 固定容量镜像缓冲区：所有字段都按小端逐字节写出，不依赖主机字节序。 */
typedef struct Blob
{
    uint8_t bytes[BLOB_CAP];
    size_t size;
} Blob;

static void put_bytes(Blob *b, const void *data, size_t count)
{
    CHECK(b->size + count <= sizeof(b->bytes));
    memcpy(b->bytes + b->size, data, count);
    b->size += count;
}

static void put8(Blob *b, uint8_t value)
{
    put_bytes(b, &value, 1);
}

static void put16(Blob *b, uint16_t value)
{
    uint8_t raw[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    put_bytes(b, raw, sizeof(raw));
}

static void put32(Blob *b, uint32_t value)
{
    uint8_t raw[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16),
                      (uint8_t)(value >> 24)};
    put_bytes(b, raw, sizeof(raw));
}

static void put_zeros(Blob *b, size_t count)
{
    CHECK(b->size + count <= sizeof(b->bytes));
    memset(b->bytes + b->size, 0, count);
    b->size += count;
}

/* 文件头里会被畸形用例覆盖的字段；其余取 default_ehdr 的默认值。 */
typedef struct Ehdr
{
    uint8_t cls, data;
    uint8_t shnum;
    uint16_t etype, machine, phentsize, shentsize;
    uint32_t phoff, shoff;
} Ehdr;

/* 一份合法的 RV32 可执行文件头。 */
static Ehdr default_ehdr(void)
{
    return (Ehdr){.cls = 1,
                  .data = 1,
                  .etype = ET_EXEC,
                  .machine = EM_RISCV,
                  .phoff = EHDR_SIZE,
                  .phentsize = PHDR_SIZE,
                  .shentsize = SHDR_SIZE};
}

static void write_ehdr(Blob *b, const Ehdr *h, uint8_t phnum, uint32_t entry)
{
    uint8_t ident[16] = {0x7f, 'E', 'L', 'F'};
    ident[4] = h->cls;
    ident[5] = h->data;
    ident[6] = 1; /* EI_VERSION */
    put_bytes(b, ident, sizeof(ident));
    put16(b, h->etype);
    put16(b, h->machine);
    put32(b, 1); /* e_version */
    put32(b, entry);
    put32(b, h->phoff);
    put32(b, h->shoff);
    put32(b, 0); /* e_flags */
    put16(b, EHDR_SIZE);
    put16(b, h->phentsize);
    put16(b, phnum);
    put16(b, h->shentsize);
    put16(b, h->shnum);
    put16(b, 0); /* e_shstrndx */
}

/* p_paddr 与 p_vaddr 相同，p_align 取 4096。 */
static void write_phdr(Blob *b, uint32_t type, uint32_t offset, uint32_t vaddr, uint32_t filesz,
                       uint32_t memsz, uint32_t flags)
{
    put32(b, type);
    put32(b, offset);
    put32(b, vaddr);
    put32(b, vaddr);
    put32(b, filesz);
    put32(b, memsz);
    put32(b, flags);
    put32(b, 0x1000);
}

/* loader 只读 sh_type、sh_offset、sh_size、sh_link，其余字段填固定值即可。 */
static void write_shdr(Blob *b, uint32_t type, uint32_t offset, uint32_t size, uint32_t link)
{
    put32(b, 0);      /* sh_name */
    put32(b, type);   /* sh_type */
    put32(b, 0);      /* sh_flags */
    put32(b, 0);      /* sh_addr */
    put32(b, offset); /* sh_offset */
    put32(b, size);   /* sh_size */
    put32(b, link);   /* sh_link */
    put32(b, 0);      /* sh_info */
    put32(b, 4);      /* sh_addralign */
    put32(b, 16);     /* sh_entsize */
}

/* 一个 PT_LOAD 段。data 指向的内容会在 build 里跟在段头表之后写出。 */
typedef struct Segment
{
    uint32_t vaddr;
    const void *data;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
} Segment;

/* 文件头、段头表、各段数据依次排列；段的 p_offset 在写段头时按布局回填。 */
static void build(Blob *b, const Ehdr *h, uint32_t entry, const Segment *segs, size_t count)
{
    b->size = 0;
    write_ehdr(b, h, (uint8_t)count, entry);
    uint32_t data_at = EHDR_SIZE + (uint32_t)count * PHDR_SIZE;
    for (size_t i = 0; i < count; ++i)
    {
        write_phdr(b, PT_LOAD, data_at, segs[i].vaddr, segs[i].filesz, segs[i].memsz,
                   segs[i].flags);
        data_at += segs[i].filesz;
    }
    for (size_t i = 0; i < count; ++i)
        put_bytes(b, segs[i].data, segs[i].filesz);
}

/* 只含一条可执行段的最小镜像；畸形用例多在它上面只改一个文件头字段。 */
static void build_code(Blob *b, const Ehdr *h, uint32_t vaddr, uint32_t entry)
{
    static const uint8_t CODE[4] = {0x13, 0x00, 0x00, 0x00}; /* addi x0, x0, 0 */
    const Segment seg = {
        .vaddr = vaddr, .data = CODE, .filesz = 4, .memsz = 4, .flags = PF_R | PF_X};
    build(b, h, entry, &seg, 1);
}

/* 最小可解析 ELF：一条可执行段加一张只含单个符号的 .symtab，节头表接在段数据之后。 */
static void with_symbol(Blob *b, const char *name, uint32_t value)
{
    static const uint8_t INFO[2] = {0x12, 0x00}; /* st_info 全局函数，st_other */
    size_t name_len = strlen(name);
    uint32_t sym_at = EHDR_SIZE + PHDR_SIZE + 8;      /* 段头表之后是 8 字节代码 */
    uint32_t str_at = sym_at + SYM_SIZE;
    uint32_t str_len = (uint32_t)name_len + 2;        /* 前后各一个 NUL */
    Ehdr h = default_ehdr();
    h.shoff = str_at + str_len;
    h.shnum = 3;

    b->size = 0;
    write_ehdr(b, &h, 1, BASE);
    write_phdr(b, PT_LOAD, EHDR_SIZE + PHDR_SIZE, BASE, 8, 8, PF_R | PF_X);
    put32(b, 0x00500093); /* addi x1, x0, 5 */
    put32(b, 0x00100013); /* addi x0, x0, 1 */

    /* st_name 指向字符串表里跳过首字节 NUL 之后的名字。 */
    put32(b, 1);     /* st_name */
    put32(b, value); /* st_value */
    put32(b, 0);     /* st_size */
    put_bytes(b, INFO, sizeof(INFO));
    put16(b, 1); /* st_shndx */
    put8(b, 0);  /* 字符串表首字节 NUL */
    put_bytes(b, name, name_len);
    put8(b, 0);

    /* 节头表：[0] NULL、[1] .symtab（sh_link=2 指向 .strtab）、[2] .strtab。 */
    put_zeros(b, SHDR_SIZE);
    write_shdr(b, SHT_SYMTAB, sym_at, SYM_SIZE, 2);
    write_shdr(b, SHT_STRTAB, str_at, str_len, 0);
}

/* 畸形镜像一律拒绝：elf_parse 返回非 0 并给出可读原因。写成宏是为了让失败报出
 * 用例自己的行号，而不是这段辅助代码的行号。 */
#define CHECK_REJECTED(blob, needle)                                                               \
    do                                                                                             \
    {                                                                                              \
        ElfImage image_;                                                                           \
        const char *error_ = NULL;                                                                 \
        CHECK(elf_parse((blob)->bytes, (blob)->size, &image_, &error_) != 0);                      \
        CHECK(error_ != NULL && strstr(error_, (needle)) != NULL);                                 \
        cases++;                                                                                   \
    } while (0)

/* 畸形镜像只能被拒绝，绝不能当成裸镜像跑起来。 */
static void test_rejections(void)
{
    static const uint8_t CODE[4] = {0x13, 0x00, 0x00, 0x00};
    const Segment read_only = {
        .vaddr = BASE, .data = CODE, .filesz = 4, .memsz = 4, .flags = PF_R};
    const Segment disjoint[2] = {
        {.vaddr = BASE, .data = CODE, .filesz = 4, .memsz = 4, .flags = PF_R | PF_X},
        {.vaddr = BASE + 0x2000, .data = CODE, .filesz = 4, .memsz = 4, .flags = PF_R | PF_X}};
    const Segment shrunk = {
        .vaddr = BASE, .data = CODE, .filesz = 4, .memsz = 2, .flags = PF_R | PF_X};
    Blob b;
    Ehdr h;

    /* 文件头本身就不完整。 */
    b.size = 0;
    put_bytes(&b, "\x7f" "ELF", 4);
    CHECK_REJECTED(&b, "smaller than an ELF header");

    /* e_ident 与 e_machine、e_type 的取值校验。 */
    h = default_ehdr();
    h.cls = 2;
    build_code(&b, &h, BASE, BASE);
    CHECK_REJECTED(&b, "only ELF32");

    h = default_ehdr();
    h.data = 2;
    build_code(&b, &h, BASE, BASE);
    CHECK_REJECTED(&b, "little-endian");

    h = default_ehdr();
    h.machine = EM_X86_64;
    build_code(&b, &h, BASE, BASE);
    CHECK_REJECTED(&b, "not built for RISC-V");

    h = default_ehdr();
    h.etype = ET_DYN;
    build_code(&b, &h, BASE, BASE);
    CHECK_REJECTED(&b, "only executable");

    /* 段头表整个落在文件之外：文件里只有 52 字节的文件头。 */
    h = default_ehdr();
    h.phoff = 0x1000;
    b.size = 0;
    write_ehdr(&b, &h, 1, BASE);
    CHECK_REJECTED(&b, "truncated");

    /* 段头表齐全，但段数据越过文件末尾。 */
    h = default_ehdr();
    b.size = 0;
    write_ehdr(&b, &h, 1, BASE);
    write_phdr(&b, PT_LOAD, 0x1000, BASE, 4, 4, PF_R | PF_X);
    CHECK_REJECTED(&b, "past the end of the file");

    /* 没有 PT_LOAD 段。 */
    h = default_ehdr();
    b.size = 0;
    write_ehdr(&b, &h, 1, BASE);
    write_phdr(&b, 0, 0, 0, 0, 0, 0);
    CHECK_REJECTED(&b, "no loadable segment");

    /* 有可装载段但没有可执行段，取指窗口无从确定。 */
    h = default_ehdr();
    build(&b, &h, BASE, &read_only, 1);
    CHECK_REJECTED(&b, "no executable segment");

    /* 可执行段之间有空洞，单一取指窗口无法描述。 */
    h = default_ehdr();
    build(&b, &h, BASE, disjoint, 2);
    CHECK_REJECTED(&b, "not contiguous");

    /* 入口点落在可执行段之外。 */
    h = default_ehdr();
    build_code(&b, &h, BASE, BASE + 0x4000);
    CHECK_REJECTED(&b, "outside the executable");

    /* memsz 小于 filesz：段的字节数自相矛盾。 */
    h = default_ehdr();
    build(&b, &h, BASE, &shrunk, 1);
    CHECK_REJECTED(&b, "smaller in memory");
}

/* 段按 p_vaddr 落位，memsz 超出 filesz 的尾部（.bss）必须清零。RAM 先填 0xff，
 * 因此漏清零会立刻暴露，而不是碰巧读到 malloc 留下的零页。 */
static void test_load_segments(void)
{
    static const uint8_t CODE[8] = {0x93, 0x00, 0x50, 0x00, 0x13, 0x00, 0x10, 0x00};
    static const uint8_t DATA[4] = {0xef, 0xbe, 0xad, 0xde};
    const Segment segs[2] = {
        {.vaddr = BASE, .data = CODE, .filesz = 8, .memsz = 8, .flags = PF_R | PF_X},
        {.vaddr = BASE + 0x1000, .data = DATA, .filesz = 4, .memsz = 16, .flags = PF_R | PF_W}};
    Blob b;
    Ehdr h = default_ehdr();
    ElfImage image;
    SimMemory memory;
    const char *error = NULL;

    build(&b, &h, BASE, segs, 2);
    CHECK(elf_parse(b.bytes, b.size, &image, &error) == 0);
    CHECK(image.entry == BASE && image.phoff == EHDR_SIZE && image.phnum == 2);
    /* ram_base 向下取整到页；exec 窗口只覆盖 PF_X 段。 */
    CHECK(image.ram_base == BASE && image.ram_limit == BASE + 0x1010);
    CHECK(image.exec_base == BASE && image.exec_limit == BASE + 8);

    CHECK(sim_memory_init(&memory, image.ram_base, 8192) == 0);
    memset(memory.ram, 0xff, memory.size);
    CHECK(elf_load_segments(&image, b.bytes, b.size, &memory, &error) == 0);
    CHECK(memcmp(memory.ram, CODE, sizeof(CODE)) == 0);
    CHECK(memcmp(memory.ram + 0x1000, DATA, sizeof(DATA)) == 0);
    for (unsigned i = sizeof(DATA); i < 16; ++i)
        CHECK(memory.ram[0x1000 + i] == 0);
    sim_memory_end(&memory);
    cases++;
}

/* --stop-pc 的符号名走 .symtab：命中、未命中，以及根本没有节头表三种结果。 */
static void test_symbols(void)
{
    Blob b;
    Ehdr h = default_ehdr();
    uint32_t addr = 0;
    const char *error = NULL;

    with_symbol(&b, "finish", BASE + 4);
    CHECK(elf_find_symbol(b.bytes, b.size, "finish", &addr, &error) == 0);
    CHECK(addr == BASE + 4);

    error = NULL;
    CHECK(elf_find_symbol(b.bytes, b.size, "absent", &addr, &error) != 0);
    CHECK(error != NULL && strstr(error, "symbol not found") != NULL);
    cases++;

    /* 没有节头表的镜像：符号名无从查起。 */
    build_code(&b, &h, BASE, BASE);
    error = NULL;
    CHECK(elf_find_symbol(b.bytes, b.size, "finish", &addr, &error) != 0);
    CHECK(error != NULL && strstr(error, "no section header table") != NULL);
    cases++;
}

int main(void)
{
    test_rejections();
    test_load_segments();
    test_symbols();
    printf("LOADER: %u directed cases passed\n", cases);
    return 0;
}
