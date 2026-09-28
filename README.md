# RVSim — RV64I 用户态指令集模拟器

RVSim 是一个用 C99 实现的 RV64I（+Zifencei）用户态指令集模拟器：直接解释执行静态链接的 Linux ABI RV64 用户态 ELF64 程序（ET_EXEC 与 ET_DYN），完整透传 stdin/stdout/stderr 与退出码，以可观测性与精确故障诊断为首要设计目标。指令语义、内存布局、syscall 约定、退出码表与测试方案见 [docs/specs/2026-09-27-rvsim-design.md](docs/specs/2026-09-27-rvsim-design.md)。

## 特性

- **指令集**：RV64I base + Zifencei 共 53 条；M/A/C/F/D、Zicsr 一律按非法指令处理（退出码 204）
- **syscall**：Linux RV64 ABI 兼容子集 9 项——exit(93)、exit_group(94)、read(63)、write(64)、brk(214)、mmap(222)、munmap(215)、clock_gettime(113)、uname(160)；不支持号码 → 205
- **ELF64 装载**：ET_EXEC 与 ET_DYN（含 entry bias）；存在 PT_INTERP（动态链接）→ 拒绝，退出码 201
- **可观测性**：`-v/-vv` 分级日志、`--trace` 逐指令 trace、`--dump-regs`、`--dump-mem A:L`、`--max-steps N`（默认 1e8，命中 → 207）、`-s` 交互调试器
- **精确故障诊断**：非法指令/未映射/未对齐/不支持 syscall/步限等均输出 PC + 指令字/地址的结构化诊断行，绝不静默（完整退出码表见规格 §7）

## 环境要求

| 组件 | 要求 | 项目验证环境（实测量） |
|------|------|------------------------|
| 宿主 OS | Linux x86-64（guest 内存以 memcpy 直映宿主，要求小端；WSL2 亦可） | Ubuntu 24.04.4 LTS（WSL2），x86_64 |
| 宿主编译器 | 支持 C99 的 gcc（代码按 `-std=c99 -Wall -Wextra -pedantic` 0 警告标准开发） | gcc 13.3.0 |
| 构建工具 | GNU Make、bash（测试脚本为 bash） | GNU Make 4.3 / bash 5.2.21 |
| RISC-V 交叉工具链 | riscv64 gcc + binutils，需支持 `-march=rv64i -mabi=lp64`；**仅 `make test` / `make guest` / `make bench` 需要** | Buildroot 2025.02.18 产出：gcc 13.4.0 / binutils 2.43.1（`riscv64-buildroot-linux-musl-` 前缀） |

要点：

- **只想编译模拟器本体**：`make` 即可，不依赖任何交叉工具链。
- **跑测试或基准必须先备好交叉工具链**：guest 测试程序不在仓库中（`*.elf` 被 ignore），由 `tests/build_guest.sh` / `tests/build_bench.sh` 现场从源码编译。Makefile 的 `CROSS` 默认指向开发者的私有 Buildroot 树，迁移时在命令行覆盖即可，例如：

  ```bash
  make test CROSS=/path/to/your/riscv64-unknown-linux-musl-
  ```

  工具链是否可用，用 [TOOLCHAIN.md](TOOLCHAIN.md) 的最小 rv64i 程序验证命令确认（能编出静态 RV64I ELF 即可）。
- **获取与项目同源的工具链**（Buildroot 2025.02.18，官方流程）：

  ```bash
  wget https://buildroot.org/downloads/buildroot-2025.02.18.tar.gz
  tar xf buildroot-2025.02.18.tar.gz && cd buildroot-2025.02.18
  make qemu_riscv64_virt_defconfig
  make menuconfig    # Toolchain 菜单：C library 选 musl、GCC compiler version 选 13.x
  make toolchain     # 仅构建工具链，产物在 output/host/bin/
  ```

  然后以 `CROSS=$PWD/output/host/bin/riscv64-buildroot-linux-musl-` 运行上述 make 目标（具体菜单项以该版本 Buildroot 文档为准）。其他发行版打包的 riscv64 工具链（如 `gcc-riscv64-linux-gnu`）理论上同样只需改 `CROSS`，但本仓库未逐一验证，请以 TOOLCHAIN.md 的验证程序通过为准。

## 构建与测试

```bash
make && make test
```

`make test` 全绿是每个里程碑 commit 的前置条件。当前矩阵：**1503 行 PASS**（90 个分层用例组 + 1413 条框架级断言），无 FAIL，退出码 0。若只编译而不跑测试：`make`；只构建 guest ELF：`make guest`（产出 `tests/guest-bin/*.elf`，`make test` 也会自动构建）；构建并运行 4 个基准的性能评估：`make bench`（不并入 `make test`，方法与数据见 [docs/benchmarks.md](docs/benchmarks.md)）。

## 运行示例

以下均为真实输出摘录（WSL Ubuntu 24.04，先 `make guest` 构建 guest ELF）。

直接执行：

```
$ build/rvsim tests/guest-bin/t_hello.elf
PASS t_hello
```

`-v` 分级日志（stderr；段加载、syscall、退出原因）：

```
$ build/rvsim -v tests/guest-bin/t_hello.elf
rvsim: load seg lo=0x0000000000010000 hi=0x000000000001030c
PASS t_hello
rvsim: syscall 64 write(1, 66136, 13, 0, 0, 0) = 13
rvsim: syscall 93 exit(0, 66136, 13, 0, 0, 0)
rvsim: exit reason=guest-exit code=0 pc=0x0000000000010128 steps=77
```

`--trace -` 逐指令 trace（每退休一条指令一行；`FILE` 也可写普通文件，摘录）：

```
$ build/rvsim --trace - tests/guest-bin/t_hello.elf
T 1 pc=0x000000000001010c insn=0x00001197 RV_AUIPC rd=x3=0x000000000001110c
T 2 pc=0x0000000000010110 insn=0x6f418193 RV_ADDI rd=x3=0x0000000000011800
T 3 pc=0x0000000000010114 insn=0x00013503 RV_LD rd=x10=0x0000000000000001
T 4 pc=0x0000000000010118 insn=0x00810593 RV_ADDI rd=x11=0x000000002000ef88
T 5 pc=0x000000000001011c insn=0x00000613 RV_ADDI rd=x12=0x0000000000000000
...
```

`--dump-regs` / `--dump-mem A:L`（停止后现场，摘录）：

```
$ build/rvsim --dump-regs tests/guest-bin/t_hello.elf
PASS t_hello
x0  = 0x0000000000000000
x1  = 0x0000000000010124
x2  = 0x000000002000ef80
...
pc  = 0x0000000000010128

$ build/rvsim --dump-mem 0x10000:32 tests/guest-bin/t_hello.elf
PASS t_hello
0000000000010000  7f454c4602010100  0000000000000000  |.ELF............|
0000000000010010  0200f30001000000  0c01010000000000  |................|
```

`--max-steps` 步限（命中 → 退出码 207）：

```
$ build/rvsim --max-steps 10 tests/guest-bin/t_bubble.elf
rvsim: step-limit 10 pc=0x00000000000100f8
$ echo $?
207
```

`-s` 交互调试器（`rvsim>` 提示符后为键入命令，管道喂入时命令本身不回显）：

```
$ build/rvsim -s tests/guest-bin/t_hello.elf
rvsim debugger: loaded, stopped before entry pc=0x000000000001010c
rvsim> s 4            ← 单步 4 条
T 1 pc=0x000000000001010c insn=0x00001197 RV_AUIPC rd=x3=0x000000000001110c
T 2 pc=0x0000000000010110 insn=0x6f418193 RV_ADDI rd=x3=0x0000000000011800
T 3 pc=0x0000000000010114 insn=0x00013503 RV_LD rd=x10=0x0000000000000001
T 4 pc=0x0000000000010118 insn=0x00810593 RV_ADDI rd=x11=0x000000002000ef88
rvsim> r              ← 寄存器现场
x0  = 0x0000000000000000
x2  = 0x000000002000ef80
...
rvsim> m 0x10000 16   ← 内存查看
0000000000010000  7f454c4602010100  0000000000000000  |.ELF............|
rvsim> c              ← 跑到结束
PASS t_hello
rvsim: stopped guest-exit code=0 pc=0x0000000000010128
rvsim> q
```

调试器其余命令（`b` 断点/`d` 删断点等）与逐字快照见 `tests/run_all.sh` 的 L3dbg 段。

## 测试分层

`make test` 的分层用例组（各层一句话验证什么）：

| 层 | 用例组数 | 验证什么 |
|----|---------|----------|
| L0 | 12 | 宿主单元测试：decode 穷举、mem 读写/越界、ALU/移位/W/访存/控制流各指令组、syscall 表、栈布局（期望值手算）、trace/dump 格式直测 |
| L1 | 18 | guest 自校验程序：交叉编译纯 RV64I 静态 ELF，rvsim 真实装载执行，stdout 对金标准 diff + 退出码检查 |
| L1err | 3 | 错误路径 guest：不支持 syscall → 205、`--max-steps` 步限 → 207、未对齐访存 → 203（退出码 + stderr 诊断前缀 + 空 stdout 三方断言） |
| L1-native | 14 | 同一 guest 源码宿主双编译（配 mini-shim 复刻 write/exit），独立验证 C 语义与金标准一致 |
| L2 | 14 | 双编译差分：rvsim 执行与宿主 native 执行的 stdout + 退出码两侧必须一致（同一 C 语义在两种 ISA 上等价） |
| L3snap | 9 | CLI 可观测性 golden 快照：trace 行格式、寄存器/内存 dump、步限、故障现场、`-v` brk 日志、ET_DYN 端到端与 204/206 单词级探针逐字锁定（含退出码 + stderr） |
| L3dbg | 7 | 调试器会话快照：stdin 管道喂命令脚本，stdout 逐字锁定（步进/断点命中/断点删除/超长行拒绝） |
| L4 | 8 | 畸形 ELF 注入：截断/错 class/错 machine/错 magic/超 phnum/坏 entry/PT_NLOAD/恰满 64 段，逐个断言细分错误码 |
| L4smoke | 5 | main 级退出码冒烟：文件打不开 → 200、坏 ELF → 201、CLI 负数选项值 → 200 等 |

分层定义与测试方案（oracle 选择、golden 生成口径）见规格 §8。

## 文档索引

- [docs/specs/2026-09-27-rvsim-design.md](docs/specs/2026-09-27-rvsim-design.md) — 设计规格 v1.0（指令语义 §4、syscall §5、CLI §6、退出码 §7、测试 §8）
- [TOOLCHAIN.md](TOOLCHAIN.md) — 交叉工具链路径、版本与验证命令
- [docs/benchmarks.md](docs/benchmarks.md) — 基准套件（Dhrystone/N-Queens/SHA-256/Heapsort）与性能评估，附 smoke 数据备案（非验收指标）
- 实施计划（任务 1–15、里程碑 M0–M7）：`.superpowers/sdd/2026-09-27-rvsim-implementation/`

## 限制（非目标，摘自规格 §1.2）

- 不做性能优化/JIT/解码缓存（正确性、可测性、可读性优先；吞吐粗测见 [docs/benchmarks.md](docs/benchmarks.md)）
- 单 hart 顺序一致：无多核、多线程、信号、异步事件
- 无动态链接：PT_INTERP → 201
- 无 M/A/C/F/D、Zicsr、用户态 CSR 指令 → 非法指令 204
- 平坦内存模型：无虚拟内存/页表/权限位
- 对齐访问不仿真：未对齐访存/跳转按故障处理（203），不模拟 Linux 内核的对齐修补
- 客户内存视图假设宿主小端（memcpy 直映 guest 内存），当前仅支持 x86-64 宿主
