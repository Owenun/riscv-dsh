#!/usr/bin/env bash
# build_bench.sh —— bench 分发 A：交叉编译 bench 基准 → tests/bench-bin/*.elf
# 参照 build_guest.sh，差异：
#   - 每个 bench 程序统一追加 tests/guest/softops.c（软算术运行时，解锁乘除；
#     既有 t_*.c 不链接 softops，回归不动）；
#   - 输出到 tests/bench-bin/；不进 make test 回归（独立 make bench，分发 B 建目标）。
# 运行（指令数可达数亿，须显式放宽 --max-steps，默认 1e8 会触发 207）：
#   ./build/rvsim --max-steps 5000000000 tests/bench-bin/bench_<name>.elf
set -euo pipefail
cd "$(dirname "$0")/.."
CROSS_PREFIX=${CROSS_PREFIX:-/home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-}
mkdir -p tests/bench-bin
for b in tests/bench/bench_*.c; do
    name=$(basename "$b" .c)
    "$CROSS_PREFIX"gcc -march=rv64i -mabi=lp64 -nostdlib -static -ffreestanding -fno-stack-protector -O2 -Wall -Wextra \
        -Itests/guest -o "tests/bench-bin/$name.elf" \
        tests/guest/guest_sys.S tests/guest/guest_lib.c tests/guest/softops.c "$b"
    if "$CROSS_PREFIX"objdump -d "tests/bench-bin/$name.elf" |
       grep -qE '\b(mul|mulh|mulhu|mulhsu|div|divu|rem|remu|amo|lr\.w|lr\.d|sc\.w|sc\.d)\b'; then
        echo "$name uses M/A insn"; exit 1
    fi
done
echo "bench build OK"
