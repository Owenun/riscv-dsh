# SDD ledger — plan: docs/superpowers/plans/2026-09-27-rvsim-implementation.md

## 环境约定（所有子代理分发必带）
- 仓库：WSL 内 `/home/xzc/projects/riscv-dsh`（Windows 侧经 UNC `\\wsl.localhost\Ubuntu-24.04\home\xzc\projects\riscv-dsh` 可读；**写文件的方法：write 工具写到 Windows 工作区暂存路径，再 `wsl cp` 进仓库；编辑现有文件优先用 edit 工具（若 UNC edit 可用，见 R7）**）。
- 命令执行：pwsh 中 `wsl.exe -d Ubuntu-24.04 bash --noprofile --norc -c "cd /home/xzc/projects/riscv-dsh && <cmd>"`；复杂脚本先写入仓库内 .sh 再执行；不要用 sandbox_permissions。
- 交叉前缀：`/home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-`。

## 飞行前裁决（Rulings）
- R1: 不建 git worktree，直接在专用新仓库 main 分支实现 — 用户明确规定项目根与里程碑提交，仓库为独立新库无共享风险 — 若错：可事后 `git branch` 重整，成本低。
- R2: 技能的模型分级不可用（本 harness 的 subagent 工具无 model 参数），全部子代理用会话默认模型 — 以简报自足性 + 测试门禁 + 评审循环补偿 — 若错：多轮修复时以更详细上下文补偿。
- R3: 停点折中：里程碑内任务连续执行不向用户确认；每个里程碑 commit 后停下汇报等待确认 — 用户硬要求阶段汇报优先于技能"持续到底" — 若错：用户多等几段，无结构风险。
- R4: build_guest.sh 简报文本中 `crt0_start.S` 为笔误，`_start` 内嵌于 guest_sys.S，编译列表只含 guest_sys.S + guest_lib.c + t_*.c — 简报自身已附注 — 若错：构建失败即被发现。
- R5: 测试辅助函数（one() 等）不逐字复制，放入共享头 `tests/harness.h`（static inline），测试语义不变 — 避免评审按"逻辑块逐字重复"判 Important — 若错：无，纯结构改进。
- R6: 计划文本 test_elf.c 依赖 <stdlib.h>/<string.h> 等头文件未显式列出，实现者按编译错误补齐 — 简报代码为骨架 — 若错：编译期即暴露。
- R7: （已实测）edit 工具 read 后可编辑 UNC 文件；write 工具对 UNC 不可用（ENOTSUP）→ 新文件一律"Windows 暂存 + wsl cp"。
- R8: Task 1 实现者完成实现与全部验证后、写报告前长时间无响应（9 轮），控制器中断接管：其停止前自述证实负例路径已自验（FAIL 机制+退出 1），控制器验证 `make test` 全绿（exit 0）；以 task-1-controller-note.md 替代报告进入评审 — 若错：RED 阶段记录缺失，由评审者 diff 视角与可复现命令补偿。
- R10: Task 1 评审者正式裁决：通过（无严重/重要）。轻微 5 条顺延：M-a framework.h static 计数器多 TU 风险（run_l0 现状每二进制单 TU，行为正确）；M-b PASS 走 stdout 无总计数；M-c 新文件 755 位（应 644）；M-d main.c 字面量 200（Task 11 替换时改 RVSIM_EX_USAGE）；M-e cc 硬编码（简报强制）。与 R9 控制器评审结论一致，R9 撤销、以本裁决为准。
（Task 1 complete 行记录于 M1 停点段落，见 R15 之后。）
- R11: Task 2 评审通过（无严重/重要）。guest CFLAGS 偏差 `-ffreestanding -fno-stack-protector` 经评审者独立复现：简报原样 flags 链接失败（__stack_chk_fail + strlen），二 flag 均不可省、语义不变 → 采纳并回写计划（commit b5cf94b）。Task 1 评审者两条 ⚠️ 消解：Makefile 内容经 M0 diff 与 task-1-diff 比对一致；Task 1 迟到报告补上 RED 证据。轻微 3 条顺延：M-f shim.c 缺 CI 语法冒烟（M4 前加）；M-g run_all.sh M/A 短模式弱于 build_guest.sh 全模式（有兜底）；M-h 守卫 2>/dev/null（已被 pipefail 兜住）。
- Task 2: complete (commit 3a9eed0 M0, 评审通过, 3 minor 顺延)
- 里程碑 M0: 完成（b5cf94b 文档回写；用户确认 2026-09-27 继续后续里程碑）
- R13: （用户指示后固化）WSL 命令引号问题规范：根因=PS 双引号展开 $/三层转义/位置绑定。规则：① 含 $、引号、循环、管道的命令一律写 .sh 脚本执行；② 统一执行器 `powershell -NoProfile -ExecutionPolicy Bypass -File <ws>\stage\wslrun.ps1 <script.sh> [args...]`（自测通过：参数绑定正确、exit 42 透传）；③ 退出码由脚本内 exit N 决定；④ PS 单引号 one-liner 仅限无 $ 的只读短命令。所有后续子代理分发必带此约定。
- R14: Task 4 评审裁决"需修复"：1 严重 C-1（elf_load 缺 rgn[64] 容量调用层检查，≥65 PT_LOAD 恶意 ELF 越界写，评审者 ASan 实证 stack-buffer-overflow）；契约空白裁定：e_type 非法→-3 接受、fopen 失败→-4 接受、-5/-6/-7 无注入文件接受但建议补。修复轮 1/5 启动：容量检查归 -4 族+rvsim.h 注释钉死、补 L4 注入（65 段/-7 entry=0/-5 无 PT_LOAD）、轻微项（m->n=0 去重、elf.c:102 注释）；修复后 git reset --soft b5cf94b 重建单个 M1 commit。
- R12: Task 3 评审通过（1 重要 + 5 轻微）。偏差裁决：不建 src/mem.h（契约统一 rvsim.h，简报文件清单与契约自相矛盾，以契约为准）；mem_add_region rgn[64] 越界写属计划强制缺陷 → **Task 10 硬性验收项：mmap 层强制区域数上限（超限 -ENOMEM）**，并随 Task 4 补容量注释 + mem_init OOM 置 data=NULL + len=0 语义注释。轻微顺延：M-i 测试覆盖广度（区域内部起点/窗口顶读写）；M-j 报告 CHECK 计数表述混淆（证据真实）。
- Task 3: complete (未提交, 归 M1 commit; 评审通过, 遗留 2 项随 Task 4, 1 项归 Task 10)
- R15: Task 4 修复轮 1 复审：C-1 ADDRESSED（容量检查先于登记、ASan 实证消除、用例有牙——删检查行即复现溢出）、轻微 3 项 ADDRESSED、无新破坏 → 任务关闭。复审者勘误：好 ELF 预登记区域数为 1 而非报告所称 3（不影响代码）。Task 4 修复轮 1/5 收敛。
- R16: Task 5 评审裁决"需修复"：1 严重 C-2（W 类移位 SLLIW/SRLIW/SRAIW 缺 bit25=imm[5]=0 校验，96 种保留编码放行；test_decode.c:157 把 sraiw shamt=63 固化为期望合法须翻转）。实现者申报 5 项偏差经评审全部成立（ENC_B<<4→>>4、ENC_J 补恒等项、U 掩码 0xFFFFF000、SRAI 按 f6=0x10 区分、指令数实为 53——spec 标题"52 条"待 M2 收尾修正，连同 §4.2 SRAI 规则措辞）。轻微：M-k FENCE.I imm（本轮已补）；M-l 0xFFFFFFFF（本轮已补）；M-m U/J 残留位（本轮已注明）；M-n 覆盖广度。
- R17: Task 5 修复轮 1 复审：C-2 + 轻微 A/B/C 全部 ADDRESSED（8 个独立探针验证：slliw shamt=32 false、sraiw shamt=63 false、sraiw shamt=31 合法、srai shamt=63 非 W 未误伤；fence.i 合法/非法区分正确），无新破坏 → 任务关闭。遗留装饰性项（rvsim.h 注释重复）已由 Task 6 实现者完成删除。decode 127 用例。
- R18: Task 6 评审通过（40 项独立探针全过，无严重/重要）。偏差裁定：①"移位量取 rs2 低 6 位（W 类低 5 位）"正确，spec §4.2"取 rs1"系笔误 → Task 7 修文档；②简报骨架 SLTI/SLTIU/SLT/SLTU 四处期望值与 RISC-V 语义相反，实现者手算修正全部正确（非"改测试凑通过"）。轻微顺延：M-o 报告计数小误差（450 vs 453）；M-p 执行级 imm 极值/SLT 零值覆盖广度；M-q harness.h 依赖 mem_add_region 纯追加语义需注释；M-r cpu.c:19 排版空格。Task 7 收尾清单：spec §4.2 修正（rs2 移位量、W 类 imm[11:5] 表述、53 条计数、FENCE.I imm==0、SRAI 规则）+ M2 commit。
- Task 6: complete (未提交, 归 M2 commit; 评审通过)
- R19: Task 7 修复轮 1 复审：C-3 ADDRESSED（三行断言独立手算复核 + 四程序宿主 shim 原生执行全 PASS）、轻微 3 项 ADDRESSED、无新破坏 → 任务关闭，M2 达成（commit 4cfce7d）。
- Task 7: complete (M2 commit 4cfce7d; 评审经 1 轮修复)
- 里程碑 M2: 完成 → 停点，等待用户确认后进入 M3
- R22（用户指示）: 新流程颗粒度修正——每个里程碑至少拆成 **2 个实现分发**，不得单分发做完整个里程碑（避免评审包过大、实现上下文过载）。拆分示例：M4=A 控制流 10 条+L0 / B guest 程序+L2 差分+commit；M5=A syscall+fence/ecall/ebreak / B main 装配+栈初始化+端到端；M6=A CLI/trace/dump / B 调试器；M7=A 矩阵收口 / B README+终审收尾。里程碑评审仍为一次整体闭环。M3 已按单分发执行（既成事实），评审按全量 diff 进行。
- R20（流程变更，用户指示）: 自 M3 起，评审从"每 task 一次"改为"每里程碑一次整体闭环"：①实现——里程碑内 task 合并 1-2 次实现分发，task 内 TDD 不变；②里程碑评审 1 次（全量 diff：规格符合度+质量+TDD 证据）；③修复轮上限 2 轮（每轮=修复分发+范围化复审），第 2 轮后未关闭项由控制器逐条裁决；④收尾——控制器独立回归+里程碑单 commit。回退条件：严重项密度过高则回退 per-task 评审。
- R21（用户指示）: 取消里程碑停点，M3-M7 连续推进不停（特殊情况例外：不可逆操作/规格冲突/修复不收敛）；用户门禁仅在最终验收时进行。附带改进：guest 程序一律加"宿主 shim 原生执行"验证（run_all.sh 新增 run_native 段），M-f（shim.c 缺 CI 冒烟）就此解决。
- R23: M3 修复轮 1 复审：重要 1/2（RED 复现留档独立重跑 252/100 精确一致；t_load_store 8/11 覆盖口径申报）+ 轻微 a/b/c 全部 ADDRESSED，跨界用例有牙（先落低 4 字节再故障会被当场抓住），无新破坏 → M3 关闭。备注非阻塞：修复者报告 objdump sw×5 应为 sw×3。
- Task 8（=M3 单分发）: complete (M3 commit aa7ee42; 评审通过经 1 轮修复; L0×8 含 test_memops 352 断言 + L4×7 + L1-native×7 全绿)
- Task 6: complete (评审通过; 归 M2 commit 4cfce7d)
- 里程碑 M3: 完成 → 无停点，直接进入 M4（R21）
- R24: M5-A 偏差裁决：①syscall 内指针参数指向未映射 → 返回 -EFAULT(-14)（替代规格 §5 尾注的"202"）——与 D2 Linux ABI 兼容一致，202 保留给 CPU 访存级故障；随 M5-B 回写 spec §5。②宿主 read/write 失败兜底 -EIO(5)——接受。③munmap 尾块回退 mmap_top、中间空洞不复用（bump 语义）——接受，注释申报。④ECALL 返回值直写 x[10]（通用写回路径 rd=0 不适用）——实现注意事项。
- R25: M5 修复轮 1 复审：C-1 ADDRESSED（elf_load 预留 2 槽单一检查点复算完备：调用方穷举 elf 批量/main 恒 2/mmap 自守卫；ASan 跑 bad_rgn64 零报告，62 段恰满成功/63 段拒绝两侧边界验证）+ I-1（-8→200）/I-2（spec §7 措辞）/M-1（count=0→0）全 ADDRESSED，无新破坏 → M5 关闭（commit 11cfe98，1381 PASS）。I-3（5 个 guest 程序）按计划 Task 14 归 M7。轻微顺延：M-s make_bad_elf.sh/run_all.sh 执行位丢失（M6 恢复 755）；M-t 故障诊断 addr/word 字段（M6 -vv 同补）。
- 里程碑 M5: 完成 → 无停点直接 M6（R21）
- M6 拆分（R22）: 分发 A=CLI 完整化+trace/dump+故障现场增强；分发 B=交互调试器+L3 快照+M6 提交
- Task 5: complete (未提交, 归 M2 commit; 评审经 1 轮修复)
- Task 6: complete (评审通过; 归 M2 commit 4cfce7d)（历史行，正式记录见上方 R18/Task 5-6 块）
- Task 4: complete (M1 commit 962321c 重建为单提交; 评审通过经 1 轮修复)
- 里程碑 M1: 完成 → 停点，等待用户确认后进入 M2
- Task 1: complete (评审通过, 5 minor 顺延; M0 commit 由 Task 2 完成)

## 飞行前冲突扫描表
| 任务对 | 产出→消费 | 结论 |
|---|---|---|
| T1→T2 | run_all.sh 结构(run_l0/FAIL)/Makefile CROSS 变量 | 一致，T2 仅追加 |
| T1→T3.. | Makefile $(SRC) 全列表 + 空占位 .c | 一致（T1 建桩文件） |
| T2→T4 | t_hello.elf 由 build_guest.sh 产出，test_elf 依赖 | 一致（run_all.sh 先 build_guest） |
| T3→T4/T10 | mem_add_region/rgn[64]/mem_check 单区域内覆盖语义；heap 扩展=改 hi；mmap=追加/回收区域 | 一致 |
| T5→T6/T8/T9 | rv_op 枚举/rv_dec 字段/enc.h 宏（含 ENC_J、ENC_B 负极值） | 一致 |
| T6→T10 | rvsim 结构体（T10 追加 brk_*/mmap_top/host_fd[3]） | 一致（均为修改 include/rvsim.h） |
| T7→T11 | L1 runner 写好但以跳过开关挂起，M5 接管 | 一致 |
| T10→T11 | rv_syscall 返回契约（kind=1 exit）被主循环消费 | 一致 |
| T12/T13 | snap()/dbg 会话 golden 文件由 runner 与测试两侧共用 | 一致 |
| 各任务自洽 | 测试代码 vs 实现语义 vs 文件清单 | 逐个核对通过（R4/R6 例外已裁决） |

## 任务台账
- BASE（计划执行起点）: 8c8ef29
- Task 1: 实现完成（未提交，工作区变更）→ 评审中（reviewer 3b424298, diff=task-1-diff.txt 229 行）
