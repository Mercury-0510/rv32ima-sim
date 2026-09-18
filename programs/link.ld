OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80000000;
    .text : { *(.text.init) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /* HTIF 通信字。Spike 从符号表里找 tohost/fromhost 来定位它们，目标程序
     * 写 tohost 即请求退出。地址固定，不随程序大小漂移；落在默认 1 MiB RAM 内。 */
    . = 0x80001000;
    .tohost : { *(.tohost) }
}
