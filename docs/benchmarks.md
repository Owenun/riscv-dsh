# 基准套件与性能评估（bench）

> **性能非本项目的验收目标**（规格 §1.2 非目标第 1 条：不做性能优化/JIT/解码缓存）。以下数据仅作备案与相对参考，**粗测、非验收指标**。

## 基准组成

4 个纯 RV64I 基准程序（`tests/bench/bench_*.c`，由 `tests/build_bench.sh` 交叉编译 → `tests/bench-bin/*.elf`）。乘除运算经 `tests/guest/softops.c` 软算术运行时（`__muldi3`/`__divdi3` 等 weak 符号）展开为 RV64I 基础指令流，不依赖 M 扩展；每个基准终态自带已知答案断言（错误答案直接非零退出，不会产出假数据）：

| 基准 | 内容 | 规模（BENCH_ITERS） | 正确性校验 |
|------|------|---------------------|-----------|
| bench_dhry | Dhrystone 2.1（Weicker 公有领域 C 2.1 逐语句改造：malloc→静态池、strcpy/strcmp→自实现、计时/printf 全删） | 100000 runs | 原版 "should be" 终态 22 项断言 |
| bench_nqueens | N-皇后位掩码回溯，N=11 | 8 | 解数 == 2680（OEIS A000170） |
| bench_sha256 | 自实现 SHA-256（含填充+长度字段），LCG 生成 1 MiB 消息 | 1 | 摘要 == python hashlib 对同一序列的独立计算值 |
| bench_hsort | 64K u32 堆排序，LCG 输入 | 3 | 有序性 + sum/xor == python 独立计算值 |

BENCH_ITERS 按"单次运行耗时落在 0.5–3 s 窗口"校准（nqueens 单次约 0.15 s，故重复 8 次）。

## 测量方法

`bash tests/run_bench.sh`（Makefile 目标 `make bench`，不并入 `make test` 回归）：

- 命令：`./build/rvsim --max-steps 1000000000 -v <bench.elf > /dev/null`——默认步限 1e8 会被 dhry/sha256/hsort 的指令数超出（触发退出码 207），故显式放宽；
- 计时：宿主 `date +%s%N` 差值（墙钟，含模拟器进程启动与 ELF 装载；毫秒级，相对秒级运行可忽略）；
- 退休指令数：`-v` 退出日志行尾 `steps=<N>` 字段（stderr）。N = `cpu.steps`，只在指令正常退休时递增，与 `--trace` 行数同口径；
- **MIPS = steps / 耗时秒 / 1e6**；
- **DMIPS（仅 dhry）= runs / 1757 / 耗时秒**。runs 取程序 stdout 的 `PASS bench_dhry nruns=<N>` 行（运行期实际值，与源码内 BENCH_ITERS 同源）；1757 为 Dhrystone 2.1 标准折算常数（VAX 11/780 每秒 1757 Dhrystones ≡ 1 DMIPS）。

## 结果

测量日期：2026-09-28。环境：WSL2（Ubuntu 24.04），AMD Ryzen 7 6800H，rvsim 为宿主 gcc `-std=c99 -O2` 直接解释器（无解码缓存）；单次运行，墙钟方差约 ±10%。

| 基准 | 退休指令数 | 耗时 (s) | MIPS | DMIPS |
|------|-----------|----------|------|-------|
| dhry | 106,717,587 | 1.902 | 56.12 | 29.93 |
| nqueens | 49,010,424 | 0.969 | 50.59 | — |
| sha256 | 109,616,211 | 1.934 | 56.68 | — |
| hsort | 87,670,339 | 1.547 | 56.68 | — |

`make bench` 原始输出（上表数据来源）：

```
==================== rvsim bench results ====================
bench              insn    time_s       MIPS      DMIPS
dhry          106717587     1.902      56.12      29.93
nqueens        49010424     0.969      50.59          -
sha256        109616211     1.934      56.68          -
hsort          87670339     1.547      56.68          -
```

## 解读

- 三个计算/访存混合基准（dhry、sha256、hsort）吞吐一致落在 ~56–57 MIPS，与文末 smoke 的 1e8 步纯计算口径（≈76 MIPS，best-of）同量级；基准负载含访存与密集分支，低于纯循环口径属预期。
- dhry：DMIPS 29.93 对应 Dhrystone 原始速率 52576 Dhrystones/s（100000 runs / 1.902 s）。DMIPS/MIPS 比 ≈ 0.53——Dhrystone 以字符串与结构体拷贝为主，平均每条指令的"有效工作"低，比值偏小是常态，跨模拟器比较时应同时看两个数。
- nqueens（50.59 MIPS）略低：递归深、分支密集，对宿主侧分支预测与取指局部性最不友好。

## 与 native 对比说明

每个基准都做了宿主 x86-64 双编译交叉验证（`cc -std=c99 -O2` + `tests/host/shim.c` mini-shim；softops 的 weak 符号让位宿主 libgcc）：**rvsim 与 native 的 stdout 逐字节一致（cmp 全等）且退出码全 0**，证明基准程序在两种 ISA 上语义等价，数据不依赖模拟器行为。

native 侧墙钟仅作量级参考：dhry ≈ 0.006 s、nqueens ≈ 0.012 s、sha256 ≈ 0.011 s、hsort ≈ 0.025 s（同机单次）。**跨 ISA 的吞吐数字不可直接比较**：RV64I 软算术把每条乘/除展开成数十条基础指令，调用约定与寄存器分配不同，宿主编译器对两种目标的优化机会也不同。本项目的 native 复跑只用于验证语义一致性，不构成性能对标。

## 附：smoke 性能数据备案（历史，保留）

> 以下为 M7 收尾时的粗测备案，口径与上文的 bench 套件不同（best-of 计时、--trace 行数计指令），仅留档。

- 测量日期：2026-09-28
- 环境：WSL2（Ubuntu 24.04，内核 6.18.33.2-microsoft-standard-WSL2），AMD Ryzen 7 6800H，宿主 gcc `-std=c99 -O2` 直接解释器（无解码缓存）
- 耗时口径：`date +%s.%N` 差值，多次运行取最好值；被测程序 stdout/stderr 重定向丢弃

| # | 项 | 命令 | 指令数 | 耗时（best-of） | 备注 |
|---|----|------|--------|-----------------|------|
| a | t_bubble 全程 | `build/rvsim tests/guest-bin/t_bubble.elf` | 4476 | 0.0025 s（5 次） | 16 元素冒泡排序，指令数取 `--trace` 输出行数 |
| b | t_fib 全程 | `build/rvsim tests/guest-bin/t_fib.elf` | 6888 | 0.0023 s（5 次） | 递归+迭代互验，指令数同上 |
| c | 纯计算吞吐 | `build/rvsim --max-steps 100000000 tests/guest-bin/t_maxsteps.elf` | 100 000 000 | 1.316 s（3 次） | 命中步限，exit 207；**≈76 MIPS（粗测）** |

测量说明：

1. **指令数口径**：`--trace <file>` 每退休一条指令输出一行 `T n …`，行数即指令数。
2. **c 项偏离计划原文**：计划设想"`--max-steps 100000000` 跑 t_bubble 会 207"，实测 t_bubble 全程仅 4476 条指令，1e8 步限跑不满（正常 exit 0）。故改用死循环 guest `t_maxsteps` 精确执行 1e8 步后命中步限（207），以其耗时/步数推算吞吐级别。
3. a/b 两项单次耗时含 rvsim 进程启动与 ELF 装载，绝对量级太小，换算速率无参考意义；吞吐仅以 c 项 1e8 步纯计算口径为准。
4. trace 写文件的开销与执行同量级但未计入上表耗时列（t_bubble 全程带 `--trace` 到文件约 0.0039 s，无 trace 为 0.0025 s）。
