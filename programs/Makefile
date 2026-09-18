# 重新生成示例镜像。只有改动 diff_smoke.S 或 link.ld 时才需要，且需要
# RISC-V 交叉工具链；构建模拟器和运行测试都不依赖它——仓库里已提交编译好的
# diff_smoke.elf，`make` 重建后请一并提交，否则 CI 会因退休数不符而失败。
CROSS ?= riscv64-unknown-elf-
FLAGS := -march=rv32im -mabi=ilp32 -nostdlib -nostartfiles -Wl,--no-relax

all: diff_smoke.elf

# 分两步编译：一次性调用时 gcc 的临时目标文件名随机（ccXXXXXX.o），
# 而该名字会记进符号表，导致每次重建都有几字节无意义的差异。
diff_smoke.elf: diff_smoke.S link.ld
	$(CROSS)gcc $(FLAGS) -c diff_smoke.S -o diff_smoke.o
	$(CROSS)gcc $(FLAGS) -T link.ld diff_smoke.o -o $@
	rm -f diff_smoke.o

clean:
	rm -f diff_smoke.elf diff_smoke.o

.PHONY: all clean
