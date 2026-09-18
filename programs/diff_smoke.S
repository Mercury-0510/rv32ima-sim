/* RV32IM only。每条源寄存器都显式初始化，避免复位后的未知值混进差分比较。 */
.section .text.init
.globl _start, test_done
_start:
    lui x4, 0x80000
    addi x1, x0, -13
    addi x2, x0, 5
    add x3, x1, x2
    sw x3, 0x400(x4)
    lw x5, 0x400(x4)
    bne x3, x5, fail
    sb x1, 0x404(x4)
    lb x6, 0x404(x4)
    lbu x7, 0x404(x4)
    sh x1, 0x406(x4)
    lh x8, 0x406(x4)
    lhu x9, 0x406(x4)
    mul x10, x1, x2
    mulh x11, x1, x2
    mulhsu x12, x1, x2
    mulhu x13, x1, x2
    div x14, x1, x2
    divu x15, x1, x2
    rem x16, x1, x2
    remu x17, x1, x2
    addi x18, x0, 3
loop:
    addi x18, x18, -1
    bne x18, x0, loop
    jal x19, target
    sw x0, 0x400(x4) /* wrong path */
fail:
    addi x31, x0, -1
    jal x0, fail
target:
    auipc x20, 0
    addi x20, x20, 12
    jalr x21, x20, 0
test_done:
    addi x31, x0, 1
    /* 用 HTIF 通知 Spike 退出（exitcode 0）：向 tohost 写 64 位值 1。
     * 模拟器不实现 HTIF，它靠 --stop-pc test_done 在上一行停机，所以这几条
     * 不会在被测侧执行——这段代码唯一的验证来源是 Spike 的进程退出码。 */
    lui  x7, %hi(tohost)
    addi x7, x7, %lo(tohost)
    addi x8, x0, 1
    sw   x8, 0(x7)
    sw   x0, 4(x7)
halt:
    jal x0, halt

/* HTIF 通信字。link.ld 把它固定在 0x80001000，与 .text 分开，因此不会
 * 撑大取指窗口，也不影响 test_done 之前任何一条指令的退休计数。 */
.section .tohost,"aw",@progbits
.align 3
.globl tohost
tohost:
    .dword 0
.globl fromhost
fromhost:
    .dword 0
