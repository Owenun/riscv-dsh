#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CROSS_PREFIX=${CROSS_PREFIX:-/home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-}
mkdir -p tests/guest-bin
for t in tests/guest/t_*.c; do
    name=$(basename "$t" .c)
    "$CROSS_PREFIX"gcc -march=rv64i -mabi=lp64 -nostdlib -static -ffreestanding -fno-stack-protector -O2 -Wall -Wextra \
        -Itests/guest -o "tests/guest-bin/$name.elf" \
        tests/guest/guest_sys.S tests/guest/guest_lib.c "$t"
    if "$CROSS_PREFIX"objdump -d "tests/guest-bin/$name.elf" |
       grep -qE '\b(mul|mulh|mulhu|mulhsu|div|divu|rem|remu|amo|lr\.w|lr\.d|sc\.w|sc\.d)\b'; then
        echo "$name uses M/A insn"; exit 1
    fi
done
echo "guest build OK"
