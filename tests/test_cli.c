/* 命令行契约：退出码、统计行、以及不该被覆盖的文件。这里必须起子进程——
 * 被测对象就是进程边界本身。sim 路径与示例镜像路径由 CTest 通过 argv 传入。 */
#include "test_util.h"
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ENABLE_TRACE 由 CMake 传入。本文件按约定不包含项目头（它只碰系统接口），
 * 所以把默认值抄一遍，与 utils/trace.h 的回退值保持一致。 */
#ifndef ENABLE_TRACE
#define ENABLE_TRACE 1
#endif

#define ERR_CAP 4096

static char *sim_path;
static char *smoke_path;
static char tmp_dir[64];
static unsigned cases;

/* 以 argv（NULL 结尾）运行 sim，返回退出码，stderr 收进 out。
 * 子进程先于 waitpid 被读空：输出超过管道容量时父子不会互等。
 * out 容量不足会截断，本文件的用例只产生一行统计。 */
static int run_sim(char *const argv[], char *out, size_t capacity)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
    {
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execv(argv[0], argv);
        _exit(127); /* exec 失败：与 sim 会用到的 0/1/2/3 都不冲突 */
    }
    close(fds[1]);

    size_t used = 0;
    while (used + 1 < capacity)
    {
        ssize_t got = read(fds[0], out + used, capacity - 1 - used);
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    out[used] = '\0';
    close(fds[0]);

    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void write_file(const char *path, const void *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, size, file) == size);
    CHECK(fclose(file) == 0);
}

static size_t read_file(const char *path, void *bytes, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    size_t got = fread(bytes, 1, capacity, file);
    CHECK(!ferror(file));
    fclose(file);
    return got;
}

/* 临时目录内的路径。写进调用方的缓冲，避免两个路径共用一块静态内存。 */
static void temp_path(char *out, size_t capacity, const char *name)
{
    snprintf(out, capacity, "%s/%s", tmp_dir, name);
}

static void remove_temp(const char *name)
{
    char path[128];
    temp_path(path, sizeof(path), name);
    remove(path);
}

/* --stop-pc 接受符号名：在符号地址处的指令退休之后停机。 */
static void test_stop_pc_symbol(void)
{
    char by_symbol[ERR_CAP], by_number[ERR_CAP];
    char *symbol[] = {sim_path, smoke_path, "--stop-pc", "_start", NULL};
    char *number[] = {sim_path, smoke_path, "--stop-pc", "0x80000000", NULL};

    CHECK(run_sim(symbol, by_symbol, sizeof(by_symbol)) == 0);
    CHECK(strstr(by_symbol, "retired=1 status=complete") != NULL);
    /* 符号名与它对应的数字地址必须给出逐字相同的统计行。 */
    CHECK(run_sim(number, by_number, sizeof(by_number)) == 0);
    CHECK(strcmp(by_symbol, by_number) == 0);
    cases++;
}

/* 符号名解析不了时退出码 2：ELF 里查无此名，裸镜像则根本没有符号表。 */
static void test_stop_pc_symbol_failures(void)
{
    static const uint8_t RAW_IMAGE[4] = {0x13, 0x00, 0x00, 0x00}; /* addi x0, x0, 0 */
    char out[ERR_CAP], raw[128];

    char *unknown[] = {sim_path, smoke_path, "--stop-pc", "absent", NULL};
    CHECK(run_sim(unknown, out, sizeof(out)) == 2);
    CHECK(strstr(out, "symbol not found") != NULL);

    temp_path(raw, sizeof(raw), "raw.bin");
    write_file(raw, RAW_IMAGE, sizeof(RAW_IMAGE));
    char *bare[] = {sim_path, raw, "--stop-pc", "finish", NULL};
    CHECK(run_sim(bare, out, sizeof(out)) == 2);
    CHECK(strstr(out, "no symbol table") != NULL);
    cases++;
}

/* --max-cycles 用尽：状态是 timeout，退出码 3。示例程序在 halt 处自旋，
 * 不给 --stop-pc 就一定跑不完。 */
static void test_max_cycles_timeout(void)
{
    char out[ERR_CAP];
    char *args[] = {sim_path, smoke_path, "--max-cycles", "3", NULL};
    CHECK(run_sim(args, out, sizeof(out)) == 3);
    CHECK(strstr(out, "status=timeout") != NULL);
    cases++;
}

/* --trace-json 不能指向输入镜像；软链接这类不同写法靠 inode 识别。 */
static void test_trace_alias_rejected(void)
{
    static const uint8_t IMAGE[4] = {0x13, 0x00, 0x10, 0x00}; /* addi x0, x0, 1 */
    char image[128], alias[128], out[ERR_CAP];
    uint8_t before[sizeof(IMAGE)];

    temp_path(image, sizeof(image), "image.bin");
    temp_path(alias, sizeof(alias), "alias.json");
    write_file(image, IMAGE, sizeof(IMAGE));
    CHECK(symlink(image, alias) == 0);

    char *args[] = {sim_path, image, "--trace-json", alias, NULL};
    CHECK(run_sim(args, out, sizeof(out)) == 2);
    CHECK(strstr(out, "must differ") != NULL);
    /* 拒绝发生在打开文件之前，镜像必须原封不动。 */
    CHECK(read_file(image, before, sizeof(before)) == sizeof(before));
    CHECK(memcmp(before, IMAGE, sizeof(IMAGE)) == 0);
    cases++;
}

#if !ENABLE_TRACE
/* 关闭轨迹的构建里，两个开关都必须立刻报错并说明原因，而不是静默产生空轨迹。
 * 顺带确认拒绝发生在 fopen 之前，不会留下一个空文件。 */
static void test_trace_flags_rejected(void)
{
    char out[ERR_CAP], json[128];
    char *stage[] = {sim_path, smoke_path, "--stage-trace", NULL};
    CHECK(run_sim(stage, out, sizeof(out)) == 2);
    CHECK(strstr(out, "--stage-trace") != NULL);
    CHECK(strstr(out, "ENABLE_TRACE=ON") != NULL);

    temp_path(json, sizeof(json), "never.json");
    char *commit[] = {sim_path, smoke_path, "--trace-json", json, NULL};
    CHECK(run_sim(commit, out, sizeof(out)) == 2);
    CHECK(strstr(out, "--trace-json") != NULL);
    CHECK(fopen(json, "rb") == NULL);

    /* 不带轨迹开关的运行不受影响。 */
    char *plain[] = {sim_path, smoke_path, "--stop-pc", "test_done", NULL};
    CHECK(run_sim(plain, out, sizeof(out)) == 0);
    cases++;
}
#endif

/* 仓库自带的 programs/diff_smoke.elf 开箱可跑。程序在 halt 处自旋，因此
 * 「退出 0 且 retired=33」同时说明符号解析成功且停机生效。 */
static void test_prebuilt_image(void)
{
    char out[ERR_CAP];
    char *args[] = {sim_path, smoke_path, "--stop-pc", "test_done", NULL};
    CHECK(run_sim(args, out, sizeof(out)) == 0);
    CHECK(strstr(out, "retired=33 status=complete") != NULL);
    cases++;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3); /* CTest 传入 sim 路径与示例镜像路径 */
    sim_path = argv[1];
    smoke_path = argv[2];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/rv32ima-cli-XXXXXX");
    CHECK(mkdtemp(tmp_dir) != NULL);

    test_stop_pc_symbol();
    test_stop_pc_symbol_failures();
    test_max_cycles_timeout();
    test_trace_alias_rejected();
#if !ENABLE_TRACE
    test_trace_flags_rejected();
#endif
    test_prebuilt_image();

    remove_temp("raw.bin");
    remove_temp("image.bin");
    remove_temp("alias.json");
    remove_temp("never.json");
    CHECK(rmdir(tmp_dir) == 0);
    printf("CLI: %u directed cases passed\n", cases);
    return 0;
}
