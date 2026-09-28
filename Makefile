CC     ?= cc
CFLAGS ?= -std=c99 -g -O2 -Wall -Wextra -pedantic
CROSS  ?= /home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-
BUILD  := build
SRC    := src/main.c src/cli.c src/mem.c src/elf.c src/decode.c src/cpu.c src/syscall.c src/trace.c src/debugger.c

.PHONY: all test guest bench clean
all: $(BUILD)/rvsim
test: all
	bash tests/run_all.sh
guest:
	bash tests/build_guest.sh
# bench：交叉编译 4 个基准 + 宿主侧计时性能评估（不并入 test 回归）
bench: all
	bash tests/build_bench.sh
	bash tests/run_bench.sh
clean:
	rm -rf $(BUILD) tests/guest-bin
$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/rvsim: $(SRC) include/rvsim.h | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -o $@ $(SRC)
