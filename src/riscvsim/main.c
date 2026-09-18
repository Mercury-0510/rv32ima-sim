#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "core/cpu_state.h"
#include "core/core.h"
#include "system/loader.h"
#include "system/memory.h"

/* 进程退出码，与 --help 列出的一致。 */
enum
{
    STATUS_DONE = 0,   /* 正常结束 */
    STATUS_TRAP = 1,   /* 指令陷阱 */
    STATUS_CONFIG = 2, /* 配置或 I/O 错误 */
    STATUS_TIMEOUT = 3 /* 超出 --max-cycles */
};

enum
{
    DEFAULT_RAM_SIZE = 1 * 1024 * 1024,
    MAX_RAM_SIZE = 256 * 1024 * 1024,
    PAGE_SIZE = 4096
};

static const char USAGE[] =
    "sim IMAGE [--trace-json FILE] [--base ADDRESS] [--ram-size BYTES]\n"
    "    [--max-cycles N] [--stop-pc ADDRESS|SYMBOL] [--stage-trace]\n"
    "IMAGE is an ELF32 executable or a raw little-endian RV32IM image;\n"
    "raw images load at --base, ELF images use the addresses they carry.\n"
    "SYMBOL is looked up in the ELF symbol table.\n"
    "Image end drains the pipeline; stop-pc stops AFTER retirement.\n"
    "Exit: 0 completed, 1 trap, 2 configuration/I/O error, 3 timeout.\n";

/* 解析非负整数，接受十进制与 0x/0 前缀；空串、负号和尾随字符都算失败。 */
static int number(const char *s, uint64_t *out)
{
    char *end;
    if (!*s || *s == '-')
        return -1;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 0);
    if (errno || *end)
        return -1;
    *out = n;
    return 0;
}

/* 命令行解析结果。除 stop 外都是最终值；stop 保留原文，因为符号名要等镜像
   装载后才能查符号表。ram_size 为 0 表示按镜像自动确定大小。 */
typedef struct Config
{
    const char *image; /* 位置参数：镜像路径。 */
    const char *trace; /* --trace-json；NULL 表示不输出提交轨迹。 */
    const char *stop;  /* --stop-pc 原文；NULL 表示不停机。 */
    uint32_t load_base;
    size_t ram_size;
    uint64_t cycles;
    int load_base_given; /* ELF 自带地址，此时再给 --base 应报错而非静默忽略。 */
    int stage_trace;
} Config;

/* 装载结果。取指窗口与入口分开保存：ELF 的入口不一定等于最低可执行地址。 */
typedef struct Image
{
    uint8_t *bytes; /* 文件内容；解析符号表时还要用，保留到运行结束。 */
    size_t size;
    int is_elf;
    uint32_t ram_base;
    uint32_t entry;     /* 复位后的 PC。 */
    uint32_t exec_base; /* 取指窗口下界。 */
    size_t count;       /* 取指窗口覆盖的指令条数。 */
} Image;

typedef enum ParseResult
{
    PARSE_OK,
    PARSE_HELP,
    PARSE_ERROR
} ParseResult;

/* 选项表。has_value 是解析循环判断"是否消费下一个 argv"的唯一依据，带值选项
   和不带值的开关因此走同一条路径，不必为某个选项写特例。 */
typedef struct OptionSpec
{
    const char *name;
    int has_value;
    int id;
} OptionSpec;

enum
{
    OPT_HELP,
    OPT_TRACE_JSON,
    OPT_BASE,
    OPT_RAM_SIZE,
    OPT_MAX_CYCLES,
    OPT_STOP_PC,
    OPT_STAGE_TRACE
};

static const OptionSpec OPTION_SPECS[] = {
    {"--help", 0, OPT_HELP},
    {"--stage-trace", 0, OPT_STAGE_TRACE},
    {"--trace-json", 1, OPT_TRACE_JSON},
    {"--base", 1, OPT_BASE},
    {"--ram-size", 1, OPT_RAM_SIZE},
    {"--max-cycles", 1, OPT_MAX_CYCLES},
    {"--stop-pc", 1, OPT_STOP_PC},
};

static const OptionSpec *find_option(const char *name)
{
    size_t count = sizeof(OPTION_SPECS) / sizeof(OPTION_SPECS[0]);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(OPTION_SPECS[i].name, name))
            return &OPTION_SPECS[i];
    return NULL;
}

static ParseResult parse_options(int argc, char **argv, Config *cfg)
{
    *cfg = (Config){.load_base = UINT32_C(0x80000000), .cycles = 100000};

    for (int i = 1; i < argc; i++)
    {
        const char *arg = argv[i];
        const OptionSpec *spec = arg[0] == '-' ? find_option(arg) : NULL;
        if (arg[0] == '-' && !spec)
        {
            fprintf(stderr, "unknown option: %s\n", arg);
            return PARSE_ERROR;
        }
        /* value 非空当且仅当该选项带值，下面用到它的分支因此不必再判断。 */
        const char *value = NULL;
        if (spec && spec->has_value)
        {
            if (++i == argc)
            {
                fprintf(stderr, "missing value for %s\n", arg);
                return PARSE_ERROR;
            }
            value = argv[i];
        }

        uint64_t parsed;
        switch (spec ? spec->id : -1)
        {
        case OPT_HELP:
            fputs(USAGE, stdout);
            return PARSE_HELP;
        case OPT_STAGE_TRACE:
            cfg->stage_trace = 1;
            break;
        case OPT_TRACE_JSON:
            cfg->trace = value;
            break;
        case OPT_STOP_PC:
            cfg->stop = value;
            break;
        case OPT_BASE:
            if (number(value, &parsed) || parsed > UINT32_MAX)
            {
                fprintf(stderr, "invalid --base: %s\n", value);
                return PARSE_ERROR;
            }
            cfg->load_base = (uint32_t)parsed;
            cfg->load_base_given = 1;
            break;
        case OPT_RAM_SIZE:
            /* 0 保留给"自动确定"，因此不接受显式的 0。 */
            if (number(value, &parsed) || !parsed || parsed > MAX_RAM_SIZE)
            {
                fprintf(stderr, "invalid --ram-size: %s\n", value);
                return PARSE_ERROR;
            }
            cfg->ram_size = (size_t)parsed;
            break;
        case OPT_MAX_CYCLES:
            if (number(value, &parsed) || !parsed)
            {
                fprintf(stderr, "invalid --max-cycles: %s\n", value);
                return PARSE_ERROR;
            }
            cfg->cycles = parsed;
            break;
        default:
            if (cfg->image)
            {
                fprintf(stderr, "unexpected extra argument: %s\n", arg);
                return PARSE_ERROR;
            }
            cfg->image = arg;
            break;
        }
    }

    if (!cfg->image)
    {
        fputs("missing image path; use --help\n", stderr);
        return PARSE_ERROR;
    }

    /* 拒绝把 trace 写到输入镜像上。除路径字符串外还比较 inode，
       这样 "a.bin" 与 "./a.bin" 这类不同写法也能识别为同一个文件。 */
    struct stat input_stat, output_stat;
    if (cfg->trace && (!strcmp(cfg->image, cfg->trace) ||
        (!stat(cfg->image, &input_stat) && !stat(cfg->trace, &output_stat) &&
         input_stat.st_dev == output_stat.st_dev && input_stat.st_ino == output_stat.st_ino)))
    {
        fputs("trace output must differ from input image\n", stderr);
        return PARSE_ERROR;
    }

    return PARSE_OK;
}

/* 读入整个镜像。失败时释放本函数取得的资源，成功时内容由 image->bytes 持有。 */
static int load_image(const char *path, Image *image)
{
    FILE *input = fopen(path, "rb");
    if (!input)
    {
        perror(path);
        return STATUS_CONFIG;
    }
    if (fseek(input, 0, SEEK_END))
    {
        fclose(input);
        return STATUS_CONFIG;
    }
    long length = ftell(input);
    if (length <= 0)
    {
        fputs("image must be nonempty\n", stderr);
        fclose(input);
        return STATUS_CONFIG;
    }
    rewind(input);

    uint8_t *bytes = malloc((size_t)length);
    if (!bytes)
    {
        fputs("out of memory\n", stderr);
        fclose(input);
        return STATUS_CONFIG;
    }
    if (fread(bytes, 1, (size_t)length, input) != (size_t)length)
    {
        fputs("cannot read image\n", stderr);
        free(bytes);
        fclose(input);
        return STATUS_CONFIG;
    }
    fclose(input);

    image->bytes = bytes;
    image->size = (size_t)length;
    /* 按文件头区分裸镜像与 ELF；ELF 自带地址，装载方式完全不同。 */
    image->is_elf = length >= 4 && !memcmp(bytes, "\x7f" "ELF", 4);
    return STATUS_DONE;
}

/* 裸镜像：整块放在 --base 处，入口就是 --base，取指窗口即镜像本身。 */
static int load_raw(const Config *cfg, Image *image, SimMemory *memory)
{
    if (image->size % 4)
    {
        fputs("raw image length must be a multiple of 4 bytes\n", stderr);
        return STATUS_CONFIG;
    }
    uint64_t size = cfg->ram_size ? cfg->ram_size : DEFAULT_RAM_SIZE;
    if (image->size > size)
    {
        fputs("raw image does not fit RAM; raise --ram-size\n", stderr);
        return STATUS_CONFIG;
    }
    if (sim_memory_init(memory, cfg->load_base, (size_t)size))
    {
        fputs("cannot initialize memory (check alignment, address range and device overlap)\n",
              stderr);
        return STATUS_CONFIG;
    }
    memcpy(memory->ram, image->bytes, image->size);
    image->ram_base = cfg->load_base;
    image->exec_base = cfg->load_base;
    image->entry = cfg->load_base;
    image->count = image->size / 4;
    return STATUS_DONE;
}

/* 未显式给出 --ram-size 时按段范围自动放大；显式给出时严格遵守，
   装不下就报错，避免用户以为限额生效而实际被悄悄放宽。 */
static uint64_t elf_ram_size(const Config *cfg, const ElfImage *elf)
{
    if (cfg->ram_size)
        return cfg->ram_size;
    uint64_t needed = (uint64_t)elf->ram_limit - elf->ram_base;
    uint64_t size = (needed + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return size > DEFAULT_RAM_SIZE ? size : DEFAULT_RAM_SIZE;
}

/* ELF：地址全部取自文件，取指窗口只覆盖可执行段。 */
static int load_elf(const Config *cfg, Image *image, SimMemory *memory)
{
    if (cfg->load_base_given)
    {
        fputs("--base does not apply to an ELF image, which carries its own addresses\n",
              stderr);
        return STATUS_CONFIG;
    }
    const char *why = NULL;
    ElfImage elf;
    if (elf_parse(image->bytes, image->size, &elf, &why))
    {
        fprintf(stderr, "%s\n", why);
        return STATUS_CONFIG;
    }
    uint64_t size = elf_ram_size(cfg, &elf);
    if (size > MAX_RAM_SIZE)
    {
        fputs("ELF segments need more RAM than the 256 MiB limit\n", stderr);
        return STATUS_CONFIG;
    }
    if (sim_memory_init(memory, elf.ram_base, (size_t)size))
    {
        fputs("cannot initialize memory (check alignment, address range and device overlap)\n",
              stderr);
        return STATUS_CONFIG;
    }
    if (elf_load_segments(&elf, image->bytes, image->size, memory, &why))
    {
        fprintf(stderr, "%s\n", why);
        sim_memory_end(memory);
        return STATUS_CONFIG;
    }
    image->ram_base = elf.ram_base;
    image->exec_base = elf.exec_base;
    image->entry = elf.entry;
    image->count = (elf.exec_limit - elf.exec_base) / 4;
    return STATUS_DONE;
}

/* 解析 --stop-pc：先试数字，失败则按符号名查 ELF 符号表。stop_pc 与
   stop_pc_valid 在这里一并写入，不再由调用方分头拼接。 */
static int resolve_stop(const Config *cfg, const Image *image, CoreConfig *config)
{
    config->stop_pc = 0;
    config->stop_pc_valid = 0;
    if (!cfg->stop)
        return STATUS_DONE;

    uint64_t value;
    if (!number(cfg->stop, &value))
    {
        if (value > UINT32_MAX)
        {
            fprintf(stderr, "stop-pc out of range: %s\n", cfg->stop);
            return STATUS_CONFIG;
        }
        config->stop_pc = (uint32_t)value;
    }
    else if (!image->is_elf)
    {
        fputs("raw image has no symbol table; --stop-pc takes a number\n", stderr);
        return STATUS_CONFIG;
    }
    else
    {
        const char *why = NULL;
        if (elf_find_symbol(image->bytes, image->size, cfg->stop, &config->stop_pc, &why))
        {
            fprintf(stderr, "%s: %s\n", cfg->stop, why);
            return STATUS_CONFIG;
        }
    }
    if (config->stop_pc & 3)
    {
        fprintf(stderr, "stop-pc must be 4-byte aligned: %s\n", cfg->stop);
        return STATUS_CONFIG;
    }
    config->stop_pc_valid = 1;
    return STATUS_DONE;
}

/* 内存由调用方管理；本函数负责运行和关闭轨迹文件。 */
static int run_program(const Config *cfg, const Image *image, CoreConfig config,
                       SimMemory *memory)
{
    FILE *out = cfg->trace ? fopen(cfg->trace, "w") : NULL;
    if (cfg->trace && !out)
    {
        perror(cfg->trace);
        return STATUS_CONFIG;
    }

    /* 镜像已在 RAM 中，故 program/data 留空，指令全部经总线取。 */
    RISCVSIMCPUState cpu;
    INCore core;
    in_core_init(&core, &cpu, &(CoreSetup){
        .count = image->count,
        .base = image->exec_base,
        .entry = image->entry,
        .bus = sim_memory_bus(memory),
        .commit_trace = out,
        .stage_trace = cfg->stage_trace,
        .config = config,
    });

    in_core_run(&core, cfg->cycles);

    /* 正常结束由核心判定：CLI 不需要知道流水线有几级、各级叫什么。 */
    int status = cpu.trapped ? STATUS_TRAP
                             : in_core_finished(&core) ? STATUS_DONE : STATUS_TIMEOUT;

    /* 统计信息写 stderr，与程序自身的 stdout 输出分开。 */
    fprintf(stderr, "clock=%" PRIu64 " retired=%" PRIu64
        " status=%s reads=%" PRIu64 " writes=%" PRIu64
        " device_reads=%" PRIu64 " device_writes=%" PRIu64 "\n",
        cpu.clock, core.stats.retired,
        cpu.trapped ? "trap" : status == STATUS_DONE ? "complete" : "timeout",
        memory->reads, memory->writes, memory->device_reads, memory->device_writes);

    /* 关闭 trace 并检查写入错误（例如磁盘写满），出错则按 I/O 错误返回。 */
    if (out)
    {
        int failed = ferror(out);
        if (fclose(out)) failed = 1;
        if (failed) status = STATUS_CONFIG;
    }

    return status;
}

int main(int argc, char **argv)
{
    Config cfg;
    ParseResult result = parse_options(argc, argv, &cfg);
    if (result != PARSE_OK)
        return result == PARSE_HELP ? STATUS_DONE : STATUS_CONFIG;

    Image image = {0};
    SimMemory memory = {0};
    CoreConfig config = {0}; /* 零值即不停机；resolve_stop 只在给出 --stop-pc 时改写。 */
    int status = load_image(cfg.image, &image);
    if (!status)
        status = image.is_elf ? load_elf(&cfg, &image, &memory)
                              : load_raw(&cfg, &image, &memory);
    if (!status)
        status = resolve_stop(&cfg, &image, &config);
    if (!status)
        status = run_program(&cfg, &image, config, &memory);

    sim_memory_end(&memory); /* 未初始化时 ram 为空指针，free 无害 */
    free(image.bytes);
    return status;
}
