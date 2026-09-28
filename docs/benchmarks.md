# smoke 性能数据备案

> **性能非本项目的验收目标**（规格 §1.2 非目标第 1 条：不做性能优化/JIT/解码缓存）。以下数据仅作备案，**粗测、非验收指标**。

- 测量日期：2026-09-28
- 环境：WSL2（Ubuntu 24.04，内核 6.18.33.2-microsoft-standard-WSL2），AMD Ryzen 7 6800H，宿主 gcc `-std=c99 -O2` 直接解释器（无解码缓存）
- 耗时口径：`date +%s.%N` 差值，多次运行取最好值；被测程序 stdout/stderr 重定向丢弃

## 数据

| # | 项 | 命令 | 指令数 | 耗时（best-of） | 备注 |
|---|----|------|--------|-----------------|------|
| a | t_bubble 全程 | `build/rvsim tests/guest-bin/t_bubble.elf` | 4476 | 0.0025 s（5 次） | 16 元素冒泡排序，指令数取 `--trace` 输出行数 |
| b | t_fib 全程 | `build/rvsim tests/guest-bin/t_fib.elf` | 6888 | 0.0023 s（5 次） | 递归+迭代互验，指令数同上 |
| c | 纯计算吞吐 | `build/rvsim --max-steps 100000000 tests/guest-bin/t_maxsteps.elf` | 100 000 000 | 1.316 s（3 次） | 命中步限，exit 207；**≈76 MIPS（粗测）** |

## 测量说明

1. **指令数口径**：`--trace <file>` 每退休一条指令输出一行 `T n …`，行数即指令数。
2. **c 项偏离计划原文**：计划设想"`--max-steps 100000000` 跑 t_bubble 会 207"，实测 t_bubble 全程仅 4476 条指令，1e8 步限跑不满（正常 exit 0）。故改用死循环 guest `t_maxsteps` 精确执行 1e8 步后命中步限（207），以其耗时/步数推算吞吐级别。
3. a/b 两项单次耗时含 rvsim 进程启动与 ELF 装载，绝对量级太小，换算速率无参考意义；吞吐仅以 c 项 1e8 步纯计算口径为准。
4. trace 写文件的开销与执行同量级但未计入上表耗时列（t_bubble 全程带 `--trace` 到文件约 0.0039 s，无 trace 为 0.0025 s）。
