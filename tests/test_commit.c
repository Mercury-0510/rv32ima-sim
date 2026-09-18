/* 提交轨迹的字段契约。模拟器写出的 JSONL 是 CLI 统计行与差分工具的接口，
 * 这里逐字段把它钉住：字段名、编码形式、seq 编号、写回抑制、掩码推导。 */
#include "test_util.h"
#include "core/core.h"
#include "core/cpu_state.h"
#include "system/memory.h"
#include <inttypes.h>
#include <string.h>

#define RAM_SIZE 4096u
#define MAX_EVENTS 16

typedef struct CommitEvent
{
    char type[8];
    uint64_t cycle, seq;
    uint32_t pc, insn;
    unsigned wen, rd;
    uint32_t wdata, mem_addr;
    unsigned mem_wmask;
    uint32_t mem_wdata;
    int trap;
    unsigned cause;
    uint32_t tval;
} CommitEvent;

/* 与 utils/trace.c 的 fprintf 一一对应。那边改格式必须同步改这里，反之亦然——
 * 本文件的存在意义就是把这条耦合变成会失败的测试。注意 trap 是裸 JSON 布尔，
 * 没有引号（wen、mem_wmask、cause 同理），只有字符串字段才带引号。 */
static const char *EVENT_SCAN = "{\"type\":\"%7[^\"]\",\"cycle\":%" SCNu64 ",\"seq\":%" SCNu64
                               ",\"pc\":\"%" SCNx32 "\",\"insn\":\"%" SCNx32
                               "\",\"wen\":%u,\"rd\":%u,\"wdata\":\"%" SCNx32
                               "\",\"mem_addr\":\"%" SCNx32 "\",\"mem_wmask\":%u,"
                               "\"mem_wdata\":\"%" SCNx32
                               "\",\"trap\":%7[a-z],\"cause\":%u,\"tval\":\"%" SCNx32 "\"}";

static SimMemory memory;
static INCore core;
static RISCVSIMCPUState cpu;
static char *trace_text;
static size_t trace_size;
static unsigned cases;

/* 把 JSONL 逐行解成结构体，断言因此能写成 events[2].mem_addr 而不是字符串比较。 */
static size_t parse_events(const char *text, CommitEvent *events, size_t capacity)
{
    size_t count = 0;
    const char *line = text;
    while (*line && count < capacity)
    {
        const char *end = strchr(line, '\n');
        if (!end)
            break;
        CommitEvent event = {0};
        char trap_text[8] = {0};
        int matched = sscanf(line, EVENT_SCAN, event.type, &event.cycle, &event.seq, &event.pc,
                             &event.insn, &event.wen, &event.rd, &event.wdata, &event.mem_addr,
                             &event.mem_wmask, &event.mem_wdata, trap_text, &event.cause,
                             &event.tval);
        CHECK(matched == 14);
        event.trap = strcmp(trap_text, "true") == 0;
        events[count++] = event;
        line = end + 1;
    }
    return count;
}

/* 程序按小端写进 RAM 起始处，指令全部经总线取——与 main.c:400-411 的装配一致。
 * 每次调用先释放上一轮的 RAM；memory 是零初始化的静态量，首次 free(NULL) 安全。 */
static size_t run(const uint32_t *words, size_t count, CommitEvent *events)
{
    sim_memory_end(&memory);
    CHECK(sim_memory_init(&memory, BASE, RAM_SIZE) == 0);
    for (size_t i = 0; i < count; ++i)
        for (unsigned b = 0; b < 4; ++b)
            memory.ram[i * 4 + b] = (uint8_t)(words[i] >> (b * 8));

    FILE *stream = open_memstream(&trace_text, &trace_size);
    CHECK(stream != NULL);
    in_core_init(&core, &cpu, &(CoreSetup){.count = count,
                                           .base = BASE,
                                           .entry = BASE,
                                           .bus = sim_memory_bus(&memory),
                                           .commit_trace = stream});
    in_core_run(&core, 500);
    CHECK(fclose(stream) == 0);
    size_t parsed = parse_events(trace_text, events, MAX_EVENTS);
    free(trace_text);
    trace_text = NULL;
    return parsed;
}

/* 字段形式与 seq 编号：seq 从 1 连续递增，pc 随顺序执行递增。 */
static void test_field_contract(void)
{
    const uint32_t program[] = {0x00500093, 0x00700113, 0x002081b3, 0x00100013};
    CommitEvent events[MAX_EVENTS];
    CHECK(run(program, 4, events) == 4);
    for (unsigned i = 0; i < 4; ++i)
    {
        CHECK(strcmp(events[i].type, "commit") == 0 && !events[i].trap);
        CHECK(events[i].seq == i + 1);
        CHECK(events[i].pc == BASE + i * 4);
        CHECK(events[i].insn == program[i]);
        CHECK(events[i].cause == 0 && events[i].tval == 0);
    }
    CHECK(events[0].wen == 1 && events[0].rd == 1 && events[0].wdata == 5);
    CHECK(events[1].wdata == 7);
    CHECK(events[2].wdata == 12);
    /* 写 x0 被抑制：wen、rd、wdata 三者一起归零。 */
    CHECK(events[3].wen == 0 && events[3].rd == 0 && events[3].wdata == 0);
    cases++;
}

/* 存储的子字掩码与截断后的数据。非存储指令一律 mem_wmask=0。 */
static void test_store_mask(void)
{
    const uint32_t program[] = {0x80000237,             /* lui  x4, 0x80000 */
                                imm(0x13, 1, 0, 0, -1), /* addi x1, x0, -1 */
                                store(0, 1, 4, 256),    /* sb   x1, 256(x4) */
                                store(1, 1, 4, 258),    /* sh   x1, 258(x4) */
                                store(2, 1, 4, 260)};   /* sw   x1, 260(x4) */
    CommitEvent events[MAX_EVENTS];
    CHECK(run(program, 5, events) == 5);
    CHECK(events[2].mem_addr == BASE + 256 && events[2].mem_wmask == 1);
    CHECK(events[2].mem_wdata == 0xff);
    CHECK(events[3].mem_addr == BASE + 258 && events[3].mem_wmask == 3);
    CHECK(events[3].mem_wdata == 0xffff);
    CHECK(events[4].mem_addr == BASE + 260 && events[4].mem_wmask == 15);
    CHECK(events[4].mem_wdata == 0xffffffff);
    /* 存储不写整数寄存器。 */
    CHECK(events[2].wen == 0 && events[2].wdata == 0);
    /* 非访存指令的地址字段为零。 */
    CHECK(events[0].mem_addr == 0 && events[0].mem_wmask == 0);
    cases++;
}

/* 非法指令：轨迹以 trap 事件收尾，且更年轻的写回被取消。 */
static void test_trap_event(void)
{
    const uint32_t program[] = {0x00500093, 0xffffffff, 0x00700113};
    CommitEvent events[MAX_EVENTS];
    CHECK(run(program, 3, events) == 2);
    CHECK(strcmp(events[0].type, "commit") == 0 && events[0].wen == 1);
    CHECK(strcmp(events[1].type, "trap") == 0 && events[1].trap);
    CHECK(events[1].cause == 2 && events[1].tval == 0xffffffff);
    CHECK(events[1].seq == 1 && events[1].wen == 0 && events[1].mem_wmask == 0);
    CHECK(cpu.trapped);
    cases++;
}

/* 分支冲刷：被跳过的指令不得进入轨迹，pc 字段直接反映重定向结果。 */
static void test_branch_pc_sequence(void)
{
    const uint32_t program[] = {0x00000463, 0xffffffff, 0x00500093};
    CommitEvent events[MAX_EVENTS];
    CHECK(run(program, 3, events) == 2);
    CHECK(events[0].pc == BASE && events[0].insn == program[0]);
    CHECK(events[1].pc == BASE + 8 && events[1].insn == program[2]);
    cases++;
}

/* 设备访问规则一：故障之前的设备访问照常发生，被冲刷的错误路径不得提交。 */
static void test_device_access_on_correct_path(void)
{
    const uint32_t program[] = {0x10000237,             /* lui  x4, 0x10000 */
                                imm(0x13, 1, 0, 0, 42), /* addi x1, x0, 42 */
                                store(2, 1, 4, 0),      /* sw   x1, 0(x4) */
                                imm(3, 2, 2, 4, 0),     /* lw   x2, 0(x4) */
                                0x00000463,             /* beq  x0, x0, +8 */
                                store(2, 0, 4, 0),      /* sw   x0, 0(x4)，应被冲刷 */
                                0x00000013};            /* nop */
    CommitEvent events[MAX_EVENTS];
    size_t count = run(program, 7, events);
    /* 设备的字读回上一次写入的锁存值。 */
    CHECK(events[3].wdata == 42);
    CHECK(memory.device_reads == 1 && memory.device_writes == 1);
    for (size_t i = 0; i < count; ++i)
        CHECK(events[i].pc != BASE + 20);
    cases++;
}

/* 设备访问规则二：故障之后的年轻设备访问必须被取消，绝不推测性地碰设备。 */
static void test_device_fault_cancels_younger(void)
{
    for (unsigned load = 0; load < 2; ++load)
    {
        const uint32_t program[] = {
            0x10000237,            /* lui x4, 0x10000 */
            imm(3, 1, 2, 0, 0),    /* lw  x1, 0(x0)，地址 0 未映射 */
            load ? imm(3, 2, 2, 4, 0)  /* lw x2, 0(x4) */
                 : store(2, 0, 4, 0)}; /* sw x0, 0(x4) */
        CommitEvent events[MAX_EVENTS];
        size_t count = run(program, 3, events);
        CHECK(count >= 1 && strcmp(events[count - 1].type, "trap") == 0);
        CHECK(memory.device_reads == 0 && memory.device_writes == 0);
        cases++;
    }
}

int main(void)
{
    test_field_contract();
    test_store_mask();
    test_trap_event();
    test_branch_pc_sequence();
    test_device_access_on_correct_path();
    test_device_fault_cancels_younger();
    sim_memory_end(&memory);
    printf("COMMIT: %u directed cases passed\n", cases);
    return 0;
}
