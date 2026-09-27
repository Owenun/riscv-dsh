#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
FAIL=0
run_l0() { # run_l0 <name> <src...>
    local name=$1; shift
    cc -std=c99 -g -Wall -Wextra -pedantic -Iinclude -o "build/$name" "$@" || { echo "L0 $name: BUILD FAIL"; FAIL=1; return; }
    if "build/$name"; then echo "L0 $name: PASS"; else echo "L0 $name: FAIL"; FAIL=1; fi
}
mkdir -p build
run_l0 test_framework tests/test_framework.c

# L1 mini-crt：构建级验证（编译出纯 RV64I 静态 ELF；e2e 执行由 M5/Task 11 接入）
CROSS_PREFIX=${CROSS_PREFIX:-/home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-}
bash tests/build_guest.sh || { echo "guest build FAIL"; FAIL=1; }
if ! file tests/guest-bin/t_hello.elf | grep -q 'ELF 64-bit LSB.*RISC-V'; then
    echo "t_hello.elf not rv64"; FAIL=1
fi
if "$CROSS_PREFIX"objdump -d tests/guest-bin/t_hello.elf 2>/dev/null |
   grep -qE '\b(mul|mulh|div|rem|amo|lr\.|sc\.)'; then
    echo "t_hello uses M/A insn"; FAIL=1
fi

exit $FAIL
