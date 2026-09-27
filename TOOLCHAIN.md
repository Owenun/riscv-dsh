# TOOLCHAIN.md — 交叉工具链

## 位置与版本

| 项 | 值 |
|----|-----|
| 工具链根 | `/home/xzc/projects/mini-qemu/work/output/host/bin`（Buildroot 2025.02.18 产出，位于 mini-qemu 构建树内，勿删改该树） |
| 前缀 | `riscv64-buildroot-linux-musl-` |
| gcc | 13.4.0（`riscv64-buildroot-linux-musl-gcc --version`） |
| binutils | GNU ld / as 2.43.1 |
| C 库 | musl（本项目测试程序用 `-nostdlib`，不链接 musl） |
| 目标 | `riscv64-buildroot-linux-musl`，soft-float ABI（lp64） |
| PATH 中 | 无（2026-09-27 验证），必须用绝对路径或本仓库 Makefile 的 `CROSS` 变量 |

## Makefile 配置约定

```make
CROSS ?= /home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-
GUEST_CC = $(CROSS)gcc
GUEST_CFLAGS = -march=rv64i -mabi=lp64 -nostdlib -static -O2 -Wall -Wextra
```

工具链迁移（如 mini-qemu 构建树被清理）时只需改 `CROSS` 一处。

## 验证命令（编译并确认一个 rv64 测试程序）

```bash
H=/home/xzc/projects/mini-qemu/work/output/host/bin
$H/riscv64-buildroot-linux-musl-gcc --version | head -1
# → riscv64-buildroot-linux-musl-gcc.br_real (Buildroot 2025.02.18) 13.4.0

# 最小 rv64i 裸机程序：write(1,msg,14) + exit(0)，两次 ecall
cat > /tmp/start.S <<'EOF'
.global _start
.text
_start:
    li a0, 1
    la a1, msg
    li a2, 14
    li a7, 64          # write
    ecall
    li a0, 0
    li a7, 93          # exit
    ecall
.data
msg: .ascii "hello rv64i!\n"
EOF
$H/riscv64-buildroot-linux-musl-gcc -march=rv64i -mabi=lp64 -nostdlib -static \
    -o /tmp/t-rv64i /tmp/start.S && echo BUILD_OK
file /tmp/t-rv64i
# → ELF 64-bit LSB executable, UCB RISC-V, soft-float ABI, statically linked
```

2026-09-27 实测：`BUILD_OK`，产物为静态 RV64I ELF；`objdump -d` 中含 2 条 ecall。
该程序是 M5 里程碑 `t_hello` 的雏形——模拟器完成后用它闭环验证 write/exit syscall。

## 与模拟器的关系
- 模拟器本体（rvsim）：宿主 gcc 编译，不用此工具链。
- 测试程序（tests/guest/）：用此工具链 `-march=rv64i -mabi=lp64 -nostdlib -static` 编译，链接仓库自带的 mini-crt。
- 不使用 qemu（见 docs/specs/2026-09-27-rvsim-design.md 决策 D6）。
