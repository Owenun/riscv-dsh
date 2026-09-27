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
run_l0 test_mem tests/test_mem.c src/mem.c
run_l0 test_decode tests/test_decode.c src/decode.c
run_l0 test_alu tests/test_alu.c src/mem.c src/decode.c src/cpu.c
run_l0 test_shift tests/test_shift.c src/mem.c src/decode.c src/cpu.c
run_l0 test_wops tests/test_wops.c src/mem.c src/decode.c src/cpu.c

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

# L1 runner：执行 guest ELF + diff golden + 退出码检查（rvsim 可执行 ELF 后启用，M5/Task 11 接入）
run_guest() { # run_guest <name>
    local name=$1
    local out="build/$name.stdout" err="build/$name.stderr"
    build/rvsim "tests/guest-bin/$name.elf" >"$out" 2>"$err"
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "L1 $name: RUN FAIL exit=$rc"; FAIL=1
        cat "$err"   # 回显 rvsim 结构化诊断（stderr 落盘后在此显示）
        return
    fi
    if diff -u "tests/golden/$name.out" "$out" >/dev/null; then
        echo "L1 $name: PASS"
    else
        echo "L1 $name: OUTPUT MISMATCH"; FAIL=1
    fi
}
# rvsim 尚不能执行用户态 ELF（M5/Task 11 接入 syscall/栈初始化后置 1；可用环境变量覆盖）
RVSIM_CAN_RUN=${RVSIM_CAN_RUN:-0}
if [ "$RVSIM_CAN_RUN" = 1 ]; then
    run_guest t_arith; run_guest t_imm_edges; run_guest t_shift; run_guest t_wops
fi

# L2 elf：装载好 ELF（依赖上方 build_guest 产出的 t_hello.elf）
run_l0 test_elf tests/test_elf.c src/mem.c src/elf.c

# L4 畸形 ELF 注入：现场生成（build/ 已 gitignore），逐个断言细分错误码
bash tests/make_bad_elf.sh || { echo "make_bad_elf FAIL"; FAIL=1; }
for c in "build/bad_trunc.elf:-4" "build/bad_class.elf:-2" "build/bad_machine.elf:-3" "build/bad_magic.elf:-1" \
         "build/bad_phnum.elf:-4" "build/bad_entry.elf:-7" "build/bad_noload.elf:-5"; do
    p=${c%%:*}; w=${c##*:}
    if build/test_elf "$p" "$w" >/dev/null 2>&1; then echo "L4 $p: PASS"; else echo "L4 $p: FAIL"; FAIL=1; fi
done

exit $FAIL
