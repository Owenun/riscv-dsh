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
# L0 可观测性（Task 12 分发 A）：trace 行/dump 格式/fault 现场尾部字段直测
run_l0 test_trace tests/test_trace.c src/mem.c src/decode.c src/cpu.c src/syscall.c src/trace.c

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
# 错误路径 guest（M7 分发 A，spec §7/§8.1）：t_badcall/t_maxsteps/t_misaligned
# —— 程序自身到不了 PASS 输出：golden stdout 为空（空 golden 文件），断言三方
# = 退出码 + stderr 首行前缀（grep -F 固定串）+ stdout 与 golden 逐字一致。
# rvsim-opts 透传在 ELF 之前（CLI 约定：选项只出现在 ELF 之前）。
run_guest_err() { # run_guest_err <name> <expect_exit> <stderr_first_line_prefix> [rvsim-opts...]
    local name=$1 want=$2 prefix=$3; shift 3
    local out="build/$name.stdout" err="build/$name.stderr"
    build/rvsim "$@" "tests/guest-bin/$name.elf" >"$out" 2>"$err"
    local rc=$? ok=1
    [ "$rc" -eq "$want" ] || ok=0
    head -n 1 "$err" | grep -qF -- "$prefix" || ok=0
    diff -u "tests/golden/$name.out" "$out" >/dev/null || ok=0
    if [ "$ok" = 1 ]; then
        echo "L1err $name: PASS"
    else
        echo "L1err $name: FAIL rc=$rc want=$want"; FAIL=1
        head -3 "$err"
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
    # M7 分发 A（spec §8.4 程序清单收口）：brk/mmap/uname/clock 正常路径。
    # 这 4 个程序 mini-crt 新原语（gbrk/gmmap/gmunmap/guname/gclock）在 shim.c
    # 无宿主对应实现 → 只走 rvsim 执行路径（见下方 NATIVE_SKIP）。
    run_guest t_brk; run_guest t_mmap; run_guest t_uname; run_guest t_clock
    # 错误路径三程序（t_badcall→205 / t_maxsteps→207 / t_misaligned→203），
    # golden stdout 为空；断言细节见 run_guest_err 头注释。
    run_guest_err t_badcall     205 "rvsim: unsupported syscall 9999"
    run_guest_err t_maxsteps    207 "rvsim: step-limit" --max-steps 1000
    run_guest_err t_misaligned  203 "rvsim: fault misaligned"
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
# 修复轮 1（重要 4）：CLI 负数拒绝——选项值前导 '-' → usage 200（与调试器
# parse_u64 口径一致）。旧代码 strtoull 吞负号：--max-steps -1 曾被接受为
# ULLONG_MAX（rc=0 跑通全程）、--dump-mem -1:32 曾被接受后在 dump 期炸 208。
smoke_rc usage-neg-max-steps-200 200 build/rvsim --max-steps -1 tests/guest-bin/t_bubble.elf
smoke_rc usage-neg-dumpmem-200 200 build/rvsim --dump-mem -1:32 tests/guest-bin/t_bubble.elf

# L1-native：guest 程序宿主双编译差分（R21 裁决；独立于 RVSIM_CAN_RUN 的固定段）。
# 同一份 C 语义宿主原生编译执行：stdout 须等于 golden、退出码 0。
# shim.c 复刻 write/exit；覆盖 tests/guest/ 全部 14 个程序（M5 分发 B 新增 t_argv）。
extra_args() { # extra_args <name>：与 run_guest 一致的固定 guest-args（未定义则空）
    if [ "$1" = t_argv ]; then echo "one two"; fi
}
# NATIVE_SKIP（M7 分发 A）：这些 guest 程序不进 L1-native/L2 双编译差分。
# 原因：tests/host/shim.c 只复刻 write/exit —— brk(214)/mmap(222)/munmap(215)/
# uname(160)/clock_gettime(113) 在宿主侧无对应实现（程序引用 gbrk/gmmap/
# gmunmap/guname/gclock 原语会链接失败）；t_badcall（gbadsys 依赖 rvsim 的
# 未知号码 205 拒绝路径）、t_maxsteps（死循环依赖 rvsim --max-steps→207）、
# t_misaligned（依赖 rvsim 未对齐访存故障→203）在宿主原生执行语义不同构，
# 差分无意义。这些程序只走 run_guest / run_guest_err（rvsim 执行路径）。
NATIVE_SKIP="t_brk t_mmap t_uname t_clock t_badcall t_maxsteps t_misaligned"
native_skip() { case " $NATIVE_SKIP " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
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
    native_skip "$n" && continue
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
        native_skip "$n" && continue
        # shellcheck disable=SC2046
        run_l2 "$n" $(extra_args "$n")
    done
fi

# L3 快照探针 ELF（修复轮 1/重要 5）：fault_ld.elf（202 故障路径）与
# brk_probe.elf（-v brk 日志）现场生成（build/ 已 gitignore，不进版本库），
# 构造与逐条指令编码注释见 tests/make_snap_elfs.sh。
bash tests/make_snap_elfs.sh || { echo "make_snap_elfs FAIL"; FAIL=1; }

# L3 CLI 快照（Task 12 分发 A，spec §6.1）：退出码断言 + golden 快照 diff。
# 修复轮 1（重要 5）：golden/<name>.out 存在才 diff stdout、<name>.err 存在
# 才 diff stderr（err golden 缺省不锁，向后兼容既有 4 个快照）——此前
# stderr 完全不锁，故障/步限诊断行无回归保护。
# golden 逐字手核记录见 sdd/task-12-report.md 与 sdd/task-13-report.md
# 「修复轮 1」：snap_trace 修复轮 1（重要 2）重构——golden 原锁的是 stdio
# 交错损坏态（第 120 行劈开行），现按正确交错语义手核（行数=143 退休步
# +1 guest 输出行、seq 1..143 连续、write 输出先于本步 trace 行）；snap_regs
# 逐寄存器按反汇编人工推演；snap_mem 首行字节对 objdump 机器码；
# snap_limit stderr 行 pc=第 11 条指令 pc（第 10 条 SD 退休后，对 trace 文件）；
# snap_fault/snap_brk 为手工编码探针 ELF（tests/make_snap_elfs.sh）锁定。
snap() { # snap <golden> <want_rc> <cmd...>
    local g=$1 want=$2; shift 2
    "$@" > build/snap.out 2> build/snap.err
    local rc=$? ok=1
    [ "$rc" -eq "$want" ] || ok=0
    if [ -f "tests/golden/$g.out" ] && ! diff -u "tests/golden/$g.out" build/snap.out >/dev/null; then
        ok=0
    fi
    if [ -f "tests/golden/$g.err" ] && ! diff -u "tests/golden/$g.err" build/snap.err >/dev/null; then
        ok=0
    fi
    if [ "$ok" = 1 ]; then
        echo "L3snap $g: PASS"
    else
        echo "L3snap $g: FAIL rc=$rc want=$want"; FAIL=1
        head -5 build/snap.err
    fi
}
snap snap_trace 0   build/rvsim --trace - tests/guest-bin/t_arith.elf
snap snap_regs  0   build/rvsim --dump-regs tests/guest-bin/t_hello.elf
# t_hello 为 EXEC 链接（VMA 0x10000，见 readelf；0x100000 未映射 → 208），
# 故取 .text 前 32 字节：--dump-mem 0x10000:32（任务文本的 0x100000 地址偏离申报）
snap snap_mem   0   build/rvsim --dump-mem 0x10000:32 tests/guest-bin/t_hello.elf
snap snap_limit 207 build/rvsim --max-steps 10 tests/guest-bin/t_bubble.elf
# 修复轮 1（重要 5）新增：故障路径三方锁定（stdout 空 + stderr 诊断行 + rc 202）
snap snap_fault 202 build/rvsim build/fault_ld.elf
# 修复轮 1（重要 3）新增：-v brk 日志锁（brk_probe 探针 ELF：brk(base+8) 后
# exit(0)；锁 "旧 brk -> 新 brk" 语义，旧实现恒打 "新 -> 新"）
snap snap_brk   0   build/rvsim -v build/brk_probe.elf
# 终审修复波（重要 1/2）新增 3 锁（探针 ELF 均由 tests/make_snap_elfs.sh 现场
# 生成，构造注释在该脚本内）：
#   snap_etdyn —— ET_DYN 真实端到端（重要 1）：etdyn_probe.elf e_type=3、段
#       vaddr/entry 相对域 0x10000，经 main 全路径（bias 后 entry=0x110000
#       执行、build_stack 栈、write/exit syscall）打印 "PASS etdyn" 且 exit 0；
#       stdout/stderr/rc 三方锁（M7-A "entry 未加 bias" 类缺陷的 main 级防线）。
#   snap_illegal —— 单词级 .word 0xFFFFFFFF → 204 + stderr `illegal insn …
#       word=0xffffffff`、stdout 空（重要 2，三方锁）。
#   snap_ebreak —— 单词级 .word 0x00100073 → 206 + stderr `rvsim: ebreak
#       pc=0x10000`、stdout 空（重要 2，三方锁）。
snap snap_etdyn   0   build/rvsim build/etdyn_probe.elf
snap snap_illegal 204 build/rvsim build/illegal.elf
snap snap_ebreak  206 build/rvsim build/ebreak.elf

# L3 调试器会话快照（Task 13 分发 B，spec §6.2）：stdin 管道喂命令脚本，
# stdout 快照 diff（修复轮 1 起 stderr 同受锁：<golden>.err 存在才 diff，
# 会话 stderr 本应为空，暂不设 err golden）+ 退出码断言。会话钉死：
#   session1 步进 4 + regs；session2 断点命中（entry=0x1010c）；session3
#   内存查看 + c 跑完（guest-exit 透传 0）；session4 空 stdin（EOF→q）；
#   session5 未知命令 + 重复断点报错 + 8 上限报错（互异地址至第 9 个互异触发）。
# golden 逐字手核记录见 sdd/task-13-report.md（entry/前 4 指令按 objdump、
# 初始 x2 按 build_stack 布局、ELF 头 32 字节对 snap_mem、终止 pc=0x10128 对 snap_regs）。
dbg() { # dbg <golden> <want_rc> <printf-input> <elf>
    local g=$1 want=$2 input=$3 elf=$4
    printf "$input" | build/rvsim -s "tests/guest-bin/$elf" > build/dbg.out 2> build/dbg.err
    local rc=$? ok=1
    [ "$rc" -eq "$want" ] || ok=0
    if [ -f "tests/golden/$g.out" ] && ! diff -u "tests/golden/$g.out" build/dbg.out >/dev/null; then
        ok=0
    fi
    if [ -f "tests/golden/$g.err" ] && ! diff -u "tests/golden/$g.err" build/dbg.err >/dev/null; then
        ok=0
    fi
    if [ "$ok" = 1 ]; then
        echo "L3dbg $g: PASS"
    else
        echo "L3dbg $g: FAIL rc=$rc want=$want"; FAIL=1
        head -5 build/dbg.err
    fi
}
dbg dbg_session1 0 's 4\nr\nq\n'            t_hello.elf
dbg dbg_session2 0 'b 0x000000000001010c\nc\nq\n' t_hello.elf
dbg dbg_session3 0 'm 0x10000 32\nc\n'      t_hello.elf
dbg dbg_session4 0 ''                       t_hello.elf
dbg dbg_session5 0 'x\nb 0x10000\nb 0x10000\nb 0x10004\nb 0x10008\nb 0x1000c\nb 0x10010\nb 0x10014\nb 0x10018\nb 0x1001c\nb 0x10020\nq\n' t_hello.elf
# M7 分发 A（评审轻微 2 回归锁）session6：断点删除须连带清除 bp_pause ——
# 命中 → d 删除 → 同址重设 → c 必须再次命中（旧实现 bp_pause 残留导致
# 第二次命中被单步跨过而丢失）。
dbg dbg_session6 0 'b 0x000000000001010c\nc\nd 0x000000000001010c\nb 0x000000000001010c\nc\nq\n' t_hello.elf
# M7 分发 A（评审轻微 3 回归锁）session7：超长行（>255 字符）确定性处理 ——
# 整行拒绝（error: line too long）并消费残留；残留不得被拆成后续命令逐段
# 回显 unknown command（旧 fgets 残留行为）。
long_line=$(printf 'a%.0s' $(seq 1 300))
dbg dbg_session7 0 "${long_line}\nq\n" t_hello.elf

exit $FAIL
