#!/usr/bin/env bash
# run_bench.sh —— bench 分发 B：rvsim 性能评估（宿主侧计时 → MIPS / DMIPS）
#
# 方法：
#   - 逐个运行 tests/bench-bin/bench_*.elf（任一缺失时先调 tests/build_bench.sh）；
#   - 命令：./build/rvsim --max-steps 1000000000 -v <bench.elf > /dev/null
#     （默认 --max-steps 1e8 不足以覆盖 dhry/sha256/hsort，会触发退出码 207）；
#   - 计时：宿主 date +%s%N 差值（墙钟，含模拟器启动与 ELF 装载——毫秒级，
#     相对秒级运行可忽略）；
#   - 退休指令数：-v 退出日志行尾 steps=<N>（stderr）。N=cpu.steps，只在指令
#     正常退休时递增，与 --trace 行数同口径；
#   - MIPS = steps / 耗时秒 / 1e6；
#   - DMIPS（仅 dhry）= runs / 1757 / 耗时秒。runs 从程序 stdout 的
#     "PASS bench_dhry nruns=<N>" 行提取（运行期实际值，与源码内
#     BENCH_ITERS 同源）；1757 = Dhrystone 2.1 标准折算常数（VAX 11/780
#     每秒 1757 Dhrystones ≡ 1 DMIPS）。
set -euo pipefail
cd "$(dirname "$0")/.."

BENCHES="dhry nqueens sha256 hsort"
MAXSTEPS=1000000000

[ -x build/rvsim ] || { echo "run_bench: build/rvsim missing, run make first" >&2; exit 1; }
for b in $BENCHES; do
    if [ ! -f "tests/bench-bin/bench_$b.elf" ]; then
        echo "run_bench: bench ELF missing, building..."
        bash tests/build_bench.sh
        break
    fi
done

names=(); insns=(); times=(); mipss=(); dmipss=()
for b in $BENCHES; do
    elf="tests/bench-bin/bench_$b.elf"
    out=/tmp/rvsim_bench_$b.out
    err=/tmp/rvsim_bench_$b.err
    rc=0
    t0=$(date +%s%N)
    ./build/rvsim --max-steps "$MAXSTEPS" -v "$elf" > "$out" 2> "$err" || rc=$?
    t1=$(date +%s%N)
    [ "$rc" -eq 0 ] || { echo "run_bench: $b exit=$rc (stderr in $err)" >&2; exit 1; }
    grep -q "^PASS bench_$b" "$out" || { echo "run_bench: $b no PASS line" >&2; exit 1; }
    steps=$(sed -n 's/.* steps=\([0-9]*\)$/\1/p' "$err" | tail -1)
    [ -n "$steps" ] || { echo "run_bench: $b no steps field in stderr" >&2; exit 1; }
    ns=$((t1 - t0))
    mips=$(awk -v s="$steps" -v ns="$ns" 'BEGIN{printf "%.2f", s/(ns/1e9)/1e6}')
    dmips="-"
    if [ "$b" = dhry ]; then
        runs=$(sed -n 's/^PASS bench_dhry nruns=\([0-9]*\) .*/\1/p' "$out")
        [ -n "$runs" ] || { echo "run_bench: dhry no nruns in stdout" >&2; exit 1; }
        dmips=$(awk -v r="$runs" -v ns="$ns" 'BEGIN{printf "%.2f", r/1757/(ns/1e9)}')
    fi
    names+=("$b")
    insns+=("$steps")
    times+=("$(awk -v ns="$ns" 'BEGIN{printf "%.3f", ns/1e9}')")
    mipss+=("$mips")
    dmipss+=("$dmips")
done

echo "==================== rvsim bench results ===================="
printf "%-8s %14s %9s %10s %10s\n" bench insn time_s MIPS DMIPS
for i in "${!names[@]}"; do
    printf "%-8s %14s %9s %10s %10s\n" \
        "${names[$i]}" "${insns[$i]}" "${times[$i]}" "${mipss[$i]}" "${dmipss[$i]}"
done
