# RVSim — RV64I 用户态指令集模拟器 · 设计规格 v1.0

- 日期：2026-09-27
- 状态：待用户复核
- 仓库：`/home/xzc/projects/riscv-dsh`（WSL Ubuntu 24.04）
- 语言：C99（宿主侧构建用系统 gcc/clang 均可，仅依赖 libc + POSIX）

## 0. 决策记录（已与用户确认）

| # | 决策点 | 结论 |
|---|--------|------|
| D1 | 实现语言 | C99 |
| D2 | syscall 约定 | Linux RV64 ABI 兼容子集（编号与语义对齐 Linux，见 §5） |
| D3 | 测试 oracle | 自校验程序 + 金标准输出 + 宿主双编译差分（见 §8），不引入 qemu |
| D4 | 指令范围 | 严格 RV64I + Zifencei（52 条），无 M/A/C/F/D/Zicsr |
| D5 | 核心架构 | 方案 A：直接解释器（取指→switch 解码→执行），无解码缓存 |
| D6 | 差分参考 qemu | 不使用。理由：mini-qemu 为 M-mode 裸机模拟器无法运行用户态 Linux 程序；qemu-user 需 sudo 安装且引入外部依赖；D3 的双编译差分已提供独立 oracle |
| D7 | 工具链 | 复用 `/home/xzc/projects/mini-qemu/work/output/host/bin` 的 Buildroot musl 交叉工具链（仅编译测试程序，见 TOOLCHAIN.md） |

## 1. 目标与非目标

### 1.1 目标
1. 正确执行静态链接的 RV64I(+Zifencei) Linux ABI 用户态 ELF64 程序（ET_EXEC 与 ET_DYN），完整透传 stdin/stdout/stderr 与退出码。
2. 可观测性优先：分级日志、逐指令 trace、寄存器/内存 dump、交互式单步调试器。
3. 精确错误报告：所有故障（非法指令、未映射/未对齐访问、不支持 syscall）给出 PC、指令字、地址的结构化诊断，绝不静默。
4. 测试驱动：§8 的测试方案在写任何实现代码前经用户确认；每个里程碑 commit 前全量回归全绿。

### 1.2 非目标（明确不做）
- 性能优化/JIT/解码缓存（正确性、可测性、可读性优先）。
- 多核、多线程、信号、异步事件（单 hart、顺序一致）。
- 动态链接：存在 `PT_INTERP` 的 ELF 拒绝加载（退出码 201）。
- M/A/C/F/D、Zicsr、用户态 CSR 指令：一律按非法指令处理（退出码 204）。
- 虚拟内存/页表/权限位：平坦内存模型（§3），无 MMU 语义。
- 对齐访问仿真：不对齐访存/跳转按故障处理，不模拟 Linux 内核的对齐修补。

## 2. 总体架构

### 2.1 数据流

```
rvsim [opts] ELF [guest-args...]
  │
  ├─ cli.c        解析参数、设置日志级别
  ├─ elf.c        解析 ELF64 → 校验 → 把 PT_LOAD 段复制进 mem → 返回 entry/brk 基址
  ├─ mem.c        平坦客户内存 + 区域登记(ELF/heap/mmap/stack) + 读写 API + 越界检测
  ├─ cpu.c        主循环：fetch → decode → execute → retire；故障即停
  ├─ decode.c     纯函数：字段提取、6 种立即数符号扩展、指令识别（可宿主单测穷举）
  ├─ syscall.c    Linux ABI 子集表（§5），对接宿主 read/write/clock
  ├─ trace.c      -v/-vv 日志、--trace 逐指令记录、--dump-regs/--dump-mem
  ├─ debugger.c   -s 交互调试器（stdin 命令循环）
  └─ main.c       装配以上模块；栈初始化(argc/argv/envp/auxv)
```

### 2.2 模块边界（各自可独立理解与测试）

| 模块 | 职责 | 不负责 |
|------|------|--------|
| decode | 32 位指令字 → (op, rd, rs1, rs2, funct3, imm) 的纯函数转换 | 访存、改状态 |
| mem | 地址→字节的读写与区域登记，对齐检查辅助 | 指令语义 |
| elf | 文件→内存映像 | 执行 |
| cpu | 指令级状态机 | 文件、宿主 IO |
| syscall | a7 号码→宿主行为 | 解码 |
| trace/debugger | 展示与交互 | 修改语义 |

跨模块数据结构只有一个：`struct rvsim { mem; cpu; config; }`（`include/rvsim.h`），各模块头文件只暴露函数原型与常量，内部结构体不外泄。

## 3. 内存模型与进程初始化

### 3.1 地址空间（平坦窗口）
- `MEM_BASE = 0x0000000000010000`，`MEM_SIZE = 512 MiB`（固定，不做配置项），有效区间 `[MEM_BASE, MEM_BASE+MEM_SIZE)`。
- 该区间外任何访问（含取指）→ 未映射故障（退出码 202）。
- 区间内部按四个登记区管理：**ELF 段区、brk 堆区、mmap 区、栈区**；登记区之外但窗口之内同样判 202（不许野访问）。

### 3.2 各区域布局
| 区域 | 位置 | 上限 |
|------|------|------|
| ELF PT_LOAD | 按 `p_vaddr`（ET_EXEC）或固定基址 `0x00100000` 加偏移（ET_DYN，bias=0x00100000） | — |
| heap（brk） | `brk_base` = 最高 PT_LOAD 段末尾向上页对齐 | 增长不超过 32 MiB |
| mmap | `MMAP_BASE = 0x10000000` 起向上 bump 分配 | 总量 64 MiB |
| stack | 预映射固定 8 MiB，栈顶 `STACK_TOP = MEM_BASE+MEM_SIZE - 4096`（顶端留 1 页 guard） | 固定 8 MiB，越界 202 |

数值布局（512MiB 窗口，四区互不重叠，写入规格即为验收依据）：

```
窗口      [0x00010000, 0x20010000)
ELF/heap  0x00100000 起（ET_DYN 基址），heap 上限 0x02110000
mmap      [0x10000000, 0x14000000)
stack     [0x1F80F000, 0x2000F000)，STACK_TOP = 0x2000F000
```

### 3.3 对齐规则（RV64I 无 C 扩展）
- 取指：PC 必须 4 对齐且已映射，否则 203/202。
- LD/SD：8 对齐；LW/LWU/SW：4 对齐；LH/LHU/SH：2 对齐；LB/LBU/SB：无要求。违反 → 203。
- JAL/JALR/分支目标：4 对齐（JALR 先清 bit0 再判）→ 否则 203。
- 语义参考：这是本模拟器的显式设计选择（Linux 内核实际会修补对齐访存，我们不模拟），§8 有对应故障测试。

### 3.4 进程栈初始化（entry 时寄存器与栈布局，SysV riscv64 Linux 约定）
- entry 时：`sp` 指向 argc；`a0=a1=a2=0`（不预置参数寄存器，mini-crt 自行从栈取）；其余 GPR=0；`pc=ELF entry`。
- 栈内容（从 STACK_TOP 向下构造，最终 sp 16 字节对齐）：
  ```
  sp+0        argc (u64)
  sp+8        argv[0] 指针, ..., argv[argc-1], NULL
  ...         envp: 仅一个 NULL（不传环境变量）
  ...         auxv: (AT_PAGESZ=6, 4096), (AT_RANDOM=25, ptr), (AT_NULL=0, 0)
  高处        argv 字符串与 AT_RANDOM 指向的 16 字节固定数据(0x42*16，保证确定性)
  ```
- argv[0] = guest ELF 路径字符串本身，其后为 guest-args。

## 4. CPU 与指令语义

### 4.1 状态
- `x0..x31`（u64，x0 恒 0，写入丢弃）、`pc`。无 CSR（Zicsr 不在范围）。
- 指令提交模型：一条指令要么完整生效（写回/访存/pc 更新），要么完全不生效并报故障。访存先检查后写入，失败不产生部分写。

### 4.2 指令集（52 条）
语义以 RISC-V 非特权级规范（Unprivileged ISA, ratified 20211203）为准，此处只记录实现要点与易错语义：

| 组 | 指令 | 实现要点 |
|----|------|----------|
| 立即数算术(9) | ADDI SLTI SLTIU XORI ORI ANDI SLLI SRLI SRAI | I 型立即数符号扩展；SLLI/SRLI/SRAI 的 shamt=imm[5:0]，**imm[11:6]≠0 → 非法指令**；SLTIU 按无符号比较（立即数仍符号扩展后按 u64 比） |
| 寄存器算术(10) | ADD SUB SLL SLT SLTU XOR SRL SRA OR AND | SRA/SRL 的移位量取 rs1 低 6 位；SLT/SLTU 结果 0/1 |
| W 类(9) | ADDIW SLLIW SRLIW SRAIW ADDW SUBW SLLW SRLW SRAW | 32 位运算后**符号扩展到 64**；SLLIW/SRLIW/SRAIW 的 imm[11:6]≠0 → 非法；ADDIW 的立即数是符号扩展 I 型 |
| 访存(11) | LB LH LW LD LBU LHU LWU SB SH SW SD | 符号/零扩展规则按规范；有效地址 = rs1+imm 按 2^64 回绕 |
| 控制流(10) | LUI AUIPC JAL JALR BEQ BNE BLT BGE BLTU BGEU | LUI 符号扩展 32→64；JALR 清目标 bit0；BLT/BGE 有符号、BLTU/BGEU 无符号；B 型偏移 13 位符号扩展 |
| 系统(4) | FENCE FENCE.I ECALL EBREAK | 单 hart 顺序一致 → FENCE/FENCE.I 无副作用退休；ECALL 进 syscall 分发；EBREAK = 停机诊断（退出码 206） |

保留编码（opcode/funct3/funct7 不匹配上表）→ 非法指令，退出码 204。

### 4.3 主循环
```
while (steps < max_steps) {
  if (debugger 有断点或单步挂起) → 交调试器
  insn = mem.fetch32(pc)          // 202/203 检查
  (op, fields) = decode(insn)     // 204 检查
  execute(op, fields)             // 202/203/205 可能触发
  retire: pc 已由执行更新; trace 记录; steps++
}
→ 207 step 超限
```
`--max-steps N`，默认 `100_000_000`。

## 5. syscall 约定表（Linux RV64 ABI 兼容子集）

调用约定：`a7`=syscall 号，参数 `a0..a5`，返回值写 `a0`；失败返回 **-errno**（Linux 语义，不设进位位）。列出的 errno 编号取自 Linux：`EBADF=9, ENOMEM=12, EINVAL=22, ENOSYS=38`。

| # | 名称 | 原型（客户视角） | 语义 | 错误 |
|---|------|------------------|------|------|
| 93 | exit | `exit(int code)` | 终止，`code & 0xff` 为模拟器退出码 | — |
| 94 | exit_group | `exit_group(int code)` | 同 93（单 hart） | — |
| 63 | read | `read(int fd, void *buf, size_t n)` | 仅 `fd=0`：转发宿主 stdin，返回读到的字节数 | fd≠0 → -EBADF |
| 64 | write | `write(int fd, const void *buf, size_t n)` | 仅 `fd∈{1,2}`：转发宿主 stdout/stderr，返回写入数 | 其余 → -EBADF |
| 214 | brk | `brk(void *addr)` | addr=0 或超出 `[brk_base, brk_base+32MiB]` → 返回当前 brk（失败不报错，Linux 语义）；否则扩展登记区并返回新 brk | — |
| 222 | mmap | `mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)` | 仅匿名私有：`flags` 必含 `MAP_PRIVATE(0x02)|MAP_ANONYMOUS(0x20)` 且 `fd=-1`；addr 提示忽略；从 MMAP_BASE bump 分配页对齐区域，返回地址 | len=0 → -EINVAL；非法 flags/fd → -EINVAL；超 64MiB → -ENOMEM |
| 215 | munmap | `munmap(void *addr, size_t len)` | 仅允许完整匹配一个 mmap 分配区（addr 与 len 精确相等），成功后该区间回收为未映射 | 其余 → -EINVAL |
| 113 | clock_gettime | `clock_gettime(clockid_t id, struct timespec *tp)` | `id=0(CLOCK_REALTIME)/1(CLOCK_MONOTONIC)` 取宿主时钟写 `{i64 tv_sec; i64 tv_nsec}` | 其余 → -EINVAL |
| 160 | uname | `uname(struct utsname *buf)` | 写 390 字节（6×65）：`Linux / rvsim / 6.8.0-rvsim / #1 rvsim / riscv64 / (none)` | — |

- 表中未列的任何号码：写 stderr 诊断 `rvsim: unsupported syscall N pc=0x…`，终止，退出码 205。
- buf/指针参数指向未映射地址 → 202（与访存一致）。
- mini-crt（§8）只依赖 93/94/64/63/214/222/160 中的子集，逐条会有测试。

## 6. CLI、可观测性与调试器

### 6.1 命令行
```
rvsim [OPTIONS] ELF [GUEST_ARGS...]
  -v              里程碑日志: 段加载、brk/mmap 变更、每次 syscall(名/参/返回)、退出原因 → stderr
  -vv             更细: 区域映射细节、栈初始化布局、故障现场全部 GPR
  --trace FILE    逐指令 trace 到 FILE（FILE 为 - 时走 stdout），格式:
                  `T <seq> pc=0x<16hex> insn=0x<8hex> <助记名>[ rd=xN=0x<16hex>]`
  --dump-regs     停机时向 stdout 打印 x0..x31 与 pc（`xN  = 0x<16hex>`，固定 5 列对齐）
  --dump-mem A:L  停机时打印 [A, A+L) 十六进制 dump（标准 hexdump 布局，8 字节行内分组）
  --max-steps N   指令数上限（默认 1e8），超限退出码 207
  -s              交互调试器（命令见 6.2），装载后停在 entry 前并打印提示
  -h              用法说明
```
- `-v` 的里程碑日志始终走 stderr，guest 的 stdout 不被污染。

### 6.2 调试器（`-s`，提示符 `rvsim> `，stdin 逐行）
| 命令 | 行为 |
|------|------|
| `s [N]` | 单步执行 N 条（默认 1），每步打印 `pc insn 助记名` |
| `c` | 连续执行至断点/停机 |
| `r` | 打印全部 GPR + pc（同 --dump-regs 格式） |
| `m A L` | 打印 [A,A+L) 内存（同 --dump-mem 格式） |
| `b A` | 设断点（最多 8 个，pc==A 时挂起） |
| `d A` | 删除断点 |
| `q` | 立即退出（退出码 0） |
| `h` | 命令帮助 |
- 断点命中打印当前指令并回到提示符。stdin EOF → 视为 `q`。
- 调试器为纯观测+流程控制，不提供写寄存器/内存（YAGNI；需要时后续加）。

## 7. 错误处理与退出码

| 退出码 | 含义 | stderr 诊断格式（固定前缀 `rvsim: `） |
|--------|------|----------------------------------------|
| 0..255 | guest `exit(code)` 透传（guest 若故意 exit(200+) 会与下表撞号，属已知歧义，靠 stderr 诊断行区分） | — |
| 200 | 用法错误（参数不合法、文件打不开） | `rvsim: usage ...` |
| 201 | ELF 加载失败（魔数/类别/机器/截断/无 PT_LOAD/PT_INTERP/entry 越界） | `rvsim: elf-load ...` |
| 202 | 未映射访问（含取指、syscall 指针参数） | `rvsim: fault unmapped pc=0x… addr=0x…` |
| 203 | 未对齐访存/取指/跳转目标 | `rvsim: fault misaligned pc=0x… addr=0x… align=N` |
| 204 | 非法/保留指令 | `rvsim: illegal insn pc=0x… word=0x…` |
| 205 | 不支持的 syscall | `rvsim: unsupported syscall N pc=0x…` |
| 206 | EBREAK 停机 | `rvsim: ebreak pc=0x…` |
| 207 | 超过 --max-steps | `rvsim: step-limit N pc=0x…` |
| 208 | 宿主内部错误（OOM、io 失败等） | `rvsim: internal ...` |

- 故障发生时若开了 `-vv`，额外打印全部 GPR 现场。
- 所有宿主资源错误（文件读写失败、OOM）不得被吞掉或误报为 guest 故障。

## 8. 测试方案（oracle / 自动化 / 边界）★硬门：本节经用户确认后才写实现代码

### 8.1 oracle 层次（独立性递进）
- **L0 宿主单元测试**：decode.c 纯函数、mem.c、elf.c 用宿主 gcc 原生编译测试（自研极简 assert 头，无第三方框架）。期望值全部手算，覆盖每个立即数格式的符号扩展边界与全部保留编码的非法判定。
- **L1 rv64 自校验程序**：每个测试是一个 C 程序（`-march=rv64i -mabi=lp64 -nostdlib -static`，链接自研 mini-crt：crt0.S + syscall 内联 asm 包装 + 极简 `puts/puthex/strlen`）。程序运行时用常量重算期望值，全部一致 → 打印 `PASS <name>` 且 exit(0)；任一不符 → 打印 `FAIL <name> line=N` 且 exit(N)。**金标准 stdout 存于 `tests/golden/`，期望内容由 C 语义推导，与模拟器实现无关。**
- **L2 双编译差分**：纯计算类测试源码（不含 syscall 除 write/exit）额外用宿主 gcc 原生编译（宿主端配一个等价 mini-shim 复刻 write/exit），同一输入下比对 stdout 与退出码。宿主原生结果即独立 oracle——同一份 C 语义在两种 ISA 上必须一致。
- **L3 CLI 端到端**：shell 驱动。固定参数组合调用 rvsim，比对 stdout/stderr/退出码三方快照；调试器用管道喂命令脚本（`printf 's 4\nr\nq\n' | rvsim -s prog`），比对输出。
- **L4 故障注入**：构造畸形 ELF（坏魔数、错 machine、错 class、截断、无 PT_LOAD、带 PT_INTERP、entry 越界）与坏用法参数，断言退出码 200/201 与诊断前缀。

### 8.2 自动化
- `make`         → 宿主侧构建 rvsim + L0 测试
- `make test`    → 依次跑 L0→L4 全量回归（bash 脚本 `tests/run_all.sh` + golden 文件 diff，不用第三方测试框架）
- `make guest`   → 交叉编译全部 rv64 测试程序（CROSS 前缀变量，见 TOOLCHAIN.md）
- **回归门**：每个里程碑 commit 前 `make && make test` 必须全绿；红着不许提交（TDD 节奏：先写失败测试 → 最小实现 → 全绿 → review → commit）。

### 8.3 边界用例清单（首版 ≥35 项，分布在各里程碑）
- 算术：加法进位回绕、SUB 借位、`0x7FFFFFFFFFFFFFFF±1`、`0x8000000000000000` 边界、SLT vs SLTU 负数/大数边界。
- 立即数：I/S/B/U/J 六种格式符号扩展的极端值（全 0、全 1、最高位为 1）、B/J 偏移 LSB=0 约束、U 型 LUI 符号扩展。
- 移位：shamt=0/1/63、SRA 负数符号保持、SRLI/SRAI 高位差异、W 类移位仅低 5 位有效。
- W 类：32 位结果符号扩展正确性（如 ADDIW 产生负值→LD 侧 64 位视图）、SRLIW/SRAIW 对比。
- 访存：全部 11 条的加载符号/零扩展、x0 作 rd 的写入丢弃、store 到 x0 基址、2/4/8 对齐各正反例、地址回绕（rs1+imm 跨 2^64）。
- 控制流：JALR bit0 清除、JAL/JALR/分支目标未对齐故障、BGE/BLTU 相等值分支、后向跳转、无限循环 + --max-steps 触发 207。
- syscall：write 成功/部分写/EBADF、read 成功/EOF(0)/EBADF、brk 正常/越界返回旧值/mmap-munmap 往返/mmap 非法 flags/未知号码 205、clock_gettime 双时钟、uname 缓冲区 390 字节。
- 栈初始化：argc/argv/NULL/envp NULL/auxv 精确布局、sp 16 对齐、argv 透传 guest-args。
- ELF：ET_EXEC/ET_DYN 双模式、多 PT_LOAD、bss( filesz<memsz ) 清零、§8.1 L4 全部畸形输入。
- 可观测性：--trace 内容与条数快照、--dump-regs/--dump-mem 格式快照、-v 日志快照、调试器脚本会话快照。

### 8.4 测试程序清单（首版）
`t_arith`、`t_imm_edges`、`t_shift`、`t_wops`、`t_load_store`、`t_branch`、`t_call`（函数调用+jal/jalr 往返）、`t_recursion`（栈压力）、`t_bubble`（O(n²) 排序纯计算，供 L2 差分）、`t_string`（strlen/memcpy 手写循环）、`t_hello`（write 输出）、`t_brk`、`t_mmap`、`t_uname`、`t_clock`、`t_badcall`、`t_maxsteps`、`t_misaligned`、`t_x0`、`t_fib`（递归+迭代互验）。

## 9. 里程碑与流程（每里程碑一个 commit，commit 前全绿 + code-review）

| 里程碑 | 内容 | 主要验证 |
|--------|------|----------|
| M0 | 仓库骨架、Makefile（宿主+CROSS 双构建）、TOOLCHAIN.md、mini-crt、L0 框架 | 工具链验证命令、L0 空跑 |
| M1 | elf.c + mem.c（加载/登记/读写 API） | L0 单测 + L4 故障注入 + 加载 -v 日志快照 |
| M2 | decode.c + cpu.c + 算术/移位/比较/W 类 | L0 解码穷举 + t_arith/t_shift/t_wops/t_imm_edges |
| M3 | 访存 11 条 + 对齐/越界故障 | t_load_store/t_x0 + 对齐故障用例 |
| M4 | 控制流 10 条 | t_branch/t_call/t_recursion/t_bubble(L2) |
| M5 | fence/ecall/ebreak + §5 syscall 表 + 栈初始化 | t_hello/t_brk/t_mmap/t_uname/t_clock/t_badcall |
| M6 | CLI 完整化（-v/-vv/--trace/dump/--max-steps）+ 交互调试器 | L3 全部快照测试 |
| M7 | 验收：全量回归、性能不设标但记录 smoke 数据、code-review、收尾 | 全绿矩阵 + review 通过 |

## 10. 合规与参考
- 全部实现代码从零编写；只参考**公开规范**获取语义：RISC-V Unprivileged ISA（指令语义）、riscv ELF psABI + System V ABI（加载与栈布局）、Linux 内核 `uapi/asm-generic/unistd.h`（syscall 编号）。不从 QEMU、mini-qemu 或任何模拟器源码复制/改写实现。
- mini-qemu 的复用仅限：交叉工具链二进制（D7）。
- 测试失败只修实现，不改测试预期，不硬编码通过。

## 11. 开放问题
无 TBD。所有上文未显式定义的行为（如 ET_DYN bias、mmap bump 方向、退出码撞号歧义）均已在本文件内固定取值。
