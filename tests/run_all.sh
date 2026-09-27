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
run_l0 test_alu tests/test_alu.c src/mem.c src/decode.c src/cpu.c src/syscall.c
run_l0 test_shift tests/test_shift.c src/mem.c src/decode.c src/cpu.c src/syscall.c
run_l0 test_wops tests/test_wops.c src/mem.c src/decode.c src/cpu.c src/syscall.c
run_l0 test_memops tests/test_memops.c src/mem.c src/decode.c src/cpu.c src/syscall.c
run_l0 test_flow tests/test_flow.c src/mem.c src/decode.c src/cpu.c src/syscall.c
run_l0 test_syscall tests/test_syscall.c src/mem.c src/decode.c src/cpu.c src/syscall.c
# build_stack 栈布局 L0（Task 11/M5 分发 B）：期望值全部手算（见用例头注释）
run_l0 test_stack tests/test_stack.c src/mem.c src/decode.c src/cpu.c src/syscall.c

# L1 mini-crt：构建级验证（编译出纯 RV64I 静态 ELF）
CROSS_PREFIX=${CROSS_PREFIX:-/home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-}
bash tests/build_guest.sh || { echo "guest build FAIL"; FAIL=1; }
if ! file tests/guest-bin/t_hello.elf | grep -q 'ELF 64-bit LSB.*RISC-V'; then
    echo "t_hello.elf not rv64"; FAIL=1
fi
if "$CROSS_PREFIX"objdump -d tests/guest-bin/t_hello.elf 2>/dev/null |
   grep -qE '\b(mul|mulh|div|rem|amo|lr\.|sc\.)'; then
    echo "t_hello uses M/A insn"; FAIL=1
fi

# L1 runner：执行 guest ELF + diff golden + 退出码检查（Task 11/M5 分发 B 激活：
# 首个端到端——rvsim 真正装载并执行 ELF；可环境变量覆盖关闭）
run_guest() { # run_guest <name> [guest-args...]
    local name=$1; shift
    local out="build/$name.stdout" err="build/$name.stderr"
    build/rvsim "tests/guest-bin/$name.elf" "$@" >"$out" 2>"$err"
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
RVSIM_CAN_RUN=${RVSIM_CAN_RUN:-1}
if [ "$RVSIM_CAN_RUN" = 1 ]; then
    run_guest t_hello
    run_guest t_arith; run_guest t_imm_edges; run_guest t_shift; run_guest t_wops
    run_guest t_load_store; run_guest t_x0
    run_guest t_branch; run_guest t_call; run_guest t_recursion; run_guest t_bubble
    run_guest t_string; run_guest t_fib
    run_guest t_argv one two    # argc/argv/envp/auxv 透传（spec §3.4，golden 手算）
fi

# L2 elf：装载好 ELF（依赖上方 build_guest 产出的 t_hello.elf）
run_l0 test_elf tests/test_elf.c src/mem.c src/elf.c

# L4 畸形 ELF 注入：现场生成（build/ 已 gitignore），逐个断言细分错误码
# bad_rgn64 为修复轮 1（C-1）新增：恰好 64 个 PT_LOAD 的合法头 ELF，
# 期望 elf_load -4（容量检查须为 main 的 heap/stack 登记预留 2 槽）
bash tests/make_bad_elf.sh || { echo "make_bad_elf FAIL"; FAIL=1; }
for c in "build/bad_trunc.elf:-4" "build/bad_class.elf:-2" "build/bad_machine.elf:-3" "build/bad_magic.elf:-1" \
         "build/bad_phnum.elf:-4" "build/bad_entry.elf:-7" "build/bad_noload.elf:-5" "build/bad_rgn64.elf:-4"; do
    p=${c%%:*}; w=${c##*:}
    if build/test_elf "$p" "$w" >/dev/null 2>&1; then echo "L4 $p: PASS"; else echo "L4 $p: FAIL"; FAIL=1; fi
done

# L4 main 级退出码冒烟（修复轮 1：I-1/C-1 端到端口径）：
#   文件打不开 → 200（spec §7 用法错误"文件打不开"，细分码 -8）；
#   坏 ELF（魔数）→ 201；恰好 64 段 PT_LOAD（恰满容量）→ 201（旧代码在
#   main 侧 heap/stack 登记时越过 rgn[63]，ASan 实证越界写）。
smoke_rc() { # smoke_rc <desc> <want_rc> <cmd...>
    local desc=$1 want=$2; shift 2
    "$@" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" -eq "$want" ]; then
        echo "L4smoke $desc: PASS"
    else
        echo "L4smoke $desc: FAIL exit=$rc want=$want"; FAIL=1
    fi
}
smoke_rc usage-file-open-200 200 build/rvsim /tmp/rvsim-no-such-file.$$
smoke_rc bad-elf-201 201 build/rvsim build/bad_magic.elf
smoke_rc rgn64-cap-201 201 build/rvsim build/bad_rgn64.elf

# L1-native：guest 程序宿主双编译差分（R21 裁决；独立于 RVSIM_CAN_RUN 的固定段）。
# 同一份 C 语义宿主原生编译执行：stdout 须等于 golden、退出码 0。
# shim.c 复刻 write/exit；覆盖 tests/guest/ 全部 14 个程序（M5 分发 B 新增 t_argv）。
extra_args() { # extra_args <name>：与 run_guest 一致的固定 guest-args（未定义则空）
    if [ "$1" = t_argv ]; then echo "one two"; fi
}
run_native() { # run_native <name> [args...]
    local name=$1; shift
    local out="build/native_$name.stdout"
    if ! cc -std=c99 -O2 -Itests/guest -o "build/native_$name" "tests/guest/$name.c" tests/host/shim.c; then
        echo "L1-native $name: BUILD FAIL"; FAIL=1; return
    fi
    "build/native_$name" "$@" >"$out"
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "L1-native $name: RUN FAIL exit=$rc"; FAIL=1; return
    fi
    if diff -u "tests/golden/$name.out" "$out" >/dev/null; then
        echo "L1-native $name: PASS"
    else
        echo "L1-native $name: OUTPUT MISMATCH"; FAIL=1
    fi
}
for t in tests/guest/t_*.c; do
    n=$(basename "$t" .c)
    # shellcheck disable=SC2046  # extra_args 有意按空白展开为多个参数
    run_native "$n" $(extra_args "$n")
done

# L2 差分（规格 §8.1 L2：rvsim 执行 vs 宿主 native 双编译差分）。
# M5 分发 B 激活：rvsim 已能执行用户态 ELF（syscall + 栈初始化接入）。
# 比对口径：对每个 guest 程序，[rvsim 执行 tests/guest-bin/<name>.elf 的
# stdout + 退出码] 与 [宿主 native 产物 build/native_<name>（上方 L1-native 段
# 构建，含同一组 extra_args）的 stdout + 退出码] 两侧必须一致。
# t_bubble 为 L2 差分主角（纯计算）；t_argv 验证栈初始化语义两侧一致
# （native 进程栈同为 [argc][argv+NULL][envp NULL][auxv] 布局）。
run_l2() { # run_l2 <name> [args...]
    local name=$1; shift
    local sim_out="build/l2_$name.simout" sim_err="build/l2_$name.simerr"
    local nat_out="build/l2_$name.natout"
    build/rvsim "tests/guest-bin/$name.elf" "$@" >"$sim_out" 2>"$sim_err"
    local rc_sim=$?
    "build/native_$name" "$@" >"$nat_out"
    local rc_nat=$?
    local ok=1
    [ "$rc_sim" -eq "$rc_nat" ] || ok=0
    diff -u "$nat_out" "$sim_out" >/dev/null 2>&1 || ok=0
    if [ "$ok" = 1 ]; then
        echo "L2 $name: PASS"
    else
        echo "L2 $name: MISMATCH sim_rc=$rc_sim native_rc=$rc_nat"; FAIL=1
    fi
}
RVSIM_L2=${RVSIM_L2:-1}
if [ "$RVSIM_L2" = 1 ]; then
    for t in tests/guest/t_*.c; do
        n=$(basename "$t" .c)
        # shellcheck disable=SC2046
        run_l2 "$n" $(extra_args "$n")
    done
fi

exit $FAIL
