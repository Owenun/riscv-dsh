# RVSim（RV64I 用户态模拟器）实施计划

> **面向 Agent 执行者：** 必需子技能：使用 superpower-subagent-driven-development（推荐）或 superpower-executing-plans 按任务逐项执行本计划。步骤使用复选框（`- [ ]`）语法进行跟踪。

**目标：** 从零实现 RV64I(+Zifencei) 用户态 ELF64 模拟器 `rvsim`（C99），含 Linux ABI syscall 子集、trace/dump、单步调试器；四层测试全绿。

**架构：** 方案 A 直接解释器：`rv_step()` = 取指→`rv_decode()`→按 `rv_op` 的 switch 执行。九个单一职责模块（mem/elf/decode/cpu/syscall/trace/debugger/cli/main）。测试分五层：L0 宿主单元、L1 自校验 guest 程序、L2 双编译差分、L3 CLI/调试器快照、L4 故障注入。

**技术栈：** C99（`-std=c99 -pedantic`）+ POSIX，仅 libc；GNU make + bash 测试 runner；交叉工具链（仅 guest 测试程序）：`/home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-`，GCC 13.4.0 / binutils 2.43.1。无第三方库、无 qemu。

**规格：** `docs/specs/2026-09-27-rvsim-design.md` —— 语义、syscall 表、CLI、退出码、边界清单以规格为准；本计划与规格冲突时停止执行并向用户确认。

## 全局约束

- 语言 C99；宿主构建 `cc -std=c99 -g -O2 -Wall -Wextra -pedantic`；guest 构建 `-march=rv64i -mabi=lp64 -nostdlib -static`。
- 指令范围 = RV64I 52 条（40 基础 + 12 条 RV64 专属含 W 类）+ FENCE/FENCE.I/ECALL/EBREAK；M/A/C/F/D/Zicsr 编码一律判非法（退出码 204）。
- 内存常量：`MEM_BASE=0x10000`、`MEM_SIZE=0x20000000`(512MiB)、`MMAP_BASE=0x10000000`、mmap 容量 64MiB、heap 容量 32MiB、`STACK_SIZE=0x800000`、`STACK_TOP=0x2000F000`、`ET_DYN bias=0x100000`。
- 对齐规则：LH/LHU/SH 2 对齐，LW/LWU/SW 4 对齐，LD/SD/取指 8/4 对齐，JAL/JALR/分支目标 4 对齐（JALR 先清 bit0）；违反 → 203，未映射 → 202，且故障指令不产生部分副作用。
- 退出码：guest 0–255 透传；200 用法 / 201 ELF / 202 未映射 / 203 未对齐 / 204 非法指令 / 205 不支持 syscall / 206 EBREAK / 207 步数超限 / 208 宿主内部错误；诊断行前缀 `rvsim: `（stderr）。
- syscall 仅 9 项：exit=93、exit_group=94、read=63、write=64、brk=214、mmap=222、munmap=215、clock_gettime=113、uname=160；参数 a0–a5、号码 a7、返回 a0，失败返回 -errno（EBADF=9、ENOMEM=12、EINVAL=22、ENOSYS=38）；未知号码 → 诊断 + 205。
- 所有命令在 WSL Ubuntu-24.04 内、`/home/xzc/projects/riscv-dsh` 下执行；Makefile 用 `CROSS ?=` 配置交叉前缀。
- TDD：先写失败测试并**跑出失败**，再写最小实现；测试预期一经提交不许修改；每个里程碑（M0–M7）恰一个 commit，commit 前 `make && make test` 全绿 + code-review。
- 禁止从 QEMU/mini-qemu/任何模拟器复制实现；规范参考仅限 RISC-V Unprivileged ISA、riscv ELF psABI、Linux `uapi/asm-generic/unistd.h`。

## 文件结构（最终态）

```
Makefile                    all/test/guest/clean 四目标；CROSS 变量
include/rvsim.h             唯一公共头：常量、类型、模块 API 原型
include/modules 内部头       src/mem.h src/elf.h src/decode.h src/cpu.h …（模块私有）
src/main.c                  装配：cli→elf→栈初始化→运行循环→退出码映射
src/cli.c                   参数解析与用法错误
src/mem.c                   平坦内存 + 区域登记 + 读写检查
src/elf.c                   ELF64 解析与 PT_LOAD 装载
src/decode.c                解码纯函数（字段/立即数/识别）
src/cpu.c                   rv_step、各指令执行、栈初始化
src/syscall.c               9 项 syscall 分发
src/trace.c                 -v/-vv 日志、--trace、--dump-regs/--dump-mem
src/debugger.c              -s 交互调试器
tests/framework.h           L0 极简断言框架
tests/enc.h                 指令编码宏（测试专用）
tests/run_all.sh            总 runner（L0→L4，任一红即非零退出）
tests/build_guest.sh        交叉编译 guest 程序 + 无 M/A 助记符守卫
tests/host/shim.c           L2 宿主 twin：gwrite/gexit 的 x86-64 实现
tests/guest/guest.h         mini-crt 头（原型 + 断言/输出辅助）
tests/guest/guest_sys.S     gwrite/gexit 的 ecall 实现 + crt0(_start)
tests/guest/guest_lib.c     puthex/puts（无除法实现）
tests/guest/t_*.c           20 个 L1/L2 测试程序
tests/golden/*.out          L1 金标准与 L3 快照
```

约定：`tests/run/` 为运行期产物（已 gitignore）。`build/` 为构建产物。

---

### 任务 1（M0-a）：构建骨架 + L0 测试框架

**文件：**
- 新建：`Makefile`、`include/rvsim.h`、`src/main.c`、`tests/framework.h`、`tests/test_framework.c`、`tests/run_all.sh`、`.gitignore`（已存在则核对）

**接口：**
- 对外产出：`make`、`make test` 可用；`tests/framework.h` 提供 `CHECK(cond)` / `TEST_DONE()`（后续所有 L0 测试依赖）。

- [ ] **步骤 1：写失败测试** `tests/test_framework.c`

```c
#include "framework.h"
int main(void) {
    CHECK(1 == 1);
    CHECK(0xFFu == 255u);
    TEST_DONE();
}
```

`tests/run_all.sh`：

```sh
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
exit $FAIL
```

- [ ] **步骤 2：确认失败** —— 运行 `make test`，预期：Makefile 不存在/目标缺失而失败（此刻还没有任何实现）。

- [ ] **步骤 3：最小实现** `Makefile`

```make
CC     ?= cc
CFLAGS ?= -std=c99 -g -O2 -Wall -Wextra -pedantic
CROSS  ?= /home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-
BUILD  := build
SRC    := src/main.c src/cli.c src/mem.c src/elf.c src/decode.c src/cpu.c src/syscall.c src/trace.c src/debugger.c

.PHONY: all test guest clean
all: $(BUILD)/rvsim
test: all
	bash tests/run_all.sh
guest:
	bash tests/build_guest.sh
clean:
	rm -rf $(BUILD) tests/guest-bin
$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/rvsim: $(SRC) include/rvsim.h | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -o $@ $(SRC)
```

（`$(SRC)` 中尚不存在的文件本任务先建空占位 `.c`，随任务推进填充——占位 `.c` 只含一行注释，不算实现。）

`include/rvsim.h`：

```c
#ifndef RVSIM_H
#define RVSIM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MEM_BASE    0x0000000000010000ULL
#define MEM_SIZE    0x0000000020000000ULL   /* 512 MiB */
#define MMAP_BASE   0x0000000010000000ULL
#define MMAP_CAP    0x0000000004000000ULL   /* 64 MiB */
#define HEAP_CAP    0x0000000002000000ULL   /* 32 MiB */
#define STACK_SIZE  0x0000000000800000ULL   /* 8 MiB */
#define STACK_TOP   0x000000002000F000ULL
#define PAGE_SIZE   4096ULL
#define ET_DYN_BIAS 0x0000000000100000ULL

#define RVSIM_EX_USAGE     200
#define RVSIM_EX_ELF       201
#define RVSIM_EX_UNMAPPED  202
#define RVSIM_EX_MISALIGNED 203
#define RVSIM_EX_ILLEGAL   204
#define RVSIM_EX_SYSCALL   205
#define RVSIM_EX_EBREAK    206
#define RVSIM_EX_STEPLIMIT 207
#define RVSIM_EX_HOSTERR   208
#endif
```

`src/main.c`（最小可链接版，后续任务替换）：

```c
#include <stdio.h>
int main(void) { fprintf(stderr, "rvsim: usage <elf>\n"); return 200; }
```

其余 `src/*.c` 建为 `/* filled by later task */`。

- [ ] **步骤 4：确认通过** —— `make test`，预期输出 `L0 test_framework: PASS`，退出 0。
- [ ] **步骤 5：不提交**（M0 在任务 2 结束统一提交）。

### 任务 2（M0-b）：mini-crt + guest 构建管线（M0 收尾，含 commit）

**文件：**
- 新建：`tests/guest/guest.h`、`tests/guest/guest_sys.S`、`tests/guest/guest_lib.c`、`tests/host/shim.c`、`tests/build_guest.sh`、`tests/guest/t_hello.c`、`tests/golden/t_hello.out`
- 测试：`tests/run_all.sh`（追加 L1 hello 用例）

**接口：**
- 对外产出：`gwrite(int fd, const void *buf, unsigned long n)`、`gexit(int code)`（rv64 侧=ecall 实现，宿主侧=write/exit 实现）；`guest.h` 内 `puts()`、`puthex(unsigned long)`、`CHECK_G(line, cond)`。

- [ ] **步骤 1：失败测试** —— `tests/run_all.sh` 追加：

```sh
if [ -x tests/guest-bin/t_hello.elf ] && [ -x build/rvsim ]; then :; fi   # M5 前跳过 e2e
# M0 阶段先做构建级验证：
bash tests/build_guest.sh || { echo "guest build FAIL"; FAIL=1; }
if ! file tests/guest-bin/t_hello.elf | grep -q 'ELF 64-bit LSB.*RISC-V'; then
    echo "t_hello.elf not rv64"; FAIL=1
fi
if "$CROSS_PREFIX"objdump -d tests/guest-bin/t_hello.elf 2>/dev/null |
   grep -qE '\b(mul|mulh|div|rem|amo|lr\.|sc\.)'; then
    echo "t_hello uses M/A insn"; FAIL=1
fi
```

（脚本头部取 `CROSS_PREFIX=${CROSS_PREFIX:-/home/xzc/.../riscv64-buildroot-linux-musl-}`；M0 阶段 `build/rvsim` 还不会跑 ELF，L1 e2e 用例在 M5 任务接入，此处只验证"能编译出纯 RV64I 静态 ELF"。）

预期失败：`tests/build_guest.sh` 不存在。

- [ ] **步骤 2：确认失败** —— `make test` → `guest build FAIL`。

- [ ] **步骤 3：实现** —— `tests/guest/guest_sys.S`：

```asm
    .text
    .globl _start
_start:                  # sp → argc; a0/a1/a2 不预置
    ld   a0, 0(sp)       # argc
    addi a1, sp, 8       # argv
    li   a2, 0           # envp = {NULL}
    call main
    li   a7, 93          # exit(ret)
    ecall
    .globl gwrite
gwrite:                  # a0=fd, a1=buf, a2=n
    li   a7, 64
    ecall
    ret
    .globl gexit
gexit:                   # a0=code
    li   a7, 93
    ecall
```

`tests/guest/guest.h`：

```c
#ifndef GUEST_H
#define GUEST_H
void gwrite(int fd, const void *buf, unsigned long n);
void gexit(int code) __attribute__((noreturn));
void puts_(const char *s);
void puthex(unsigned long v);
#define CHECK_G(line, cond) do { if (!(cond)) { puts_("FAIL\n"); gexit(line); } } while (0)
#endif
```

`tests/guest/guest_lib.c`（无除法：移位打十六进制）：

```c
#include "guest.h"
void puts_(const char *s) { unsigned long n = 0; while (s[n]) n++; gwrite(1, s, n); }
void puthex(unsigned long v) {
    char b[18]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> 60) & 0xF);
        b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v <<= 4;
    }
    gwrite(1, b, 18);
}
```

`tests/host/shim.c`（L2 宿主 twin，M4 起使用）：

```c
#include <unistd.h>
#include <stdlib.h>
void gwrite(int fd, const void *buf, unsigned long n) {
    ssize_t r = write(fd, buf, n); (void)r;
}
void gexit(int code) { exit(code); }
void puts_(const char *s) { unsigned long n = 0; while (s[n]) n++; gwrite(1, s, n); }
void puthex(unsigned long v) { /* 与 guest_lib.c 相同实现，逐字复制 */ }
```

`tests/build_guest.sh`：

```sh
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CROSS_PREFIX=${CROSS_PREFIX:-/home/xzc/projects/mini-qemu/work/output/host/bin/riscv64-buildroot-linux-musl-}
mkdir -p tests/guest-bin
for t in tests/guest/t_*.c; do
    name=$(basename "$t" .c)
    "$CROSS_PREFIX"gcc -march=rv64i -mabi=lp64 -nostdlib -static -O2 -Wall -Wextra \
        -Itests/guest -o "tests/guest-bin/$name.elf" \
        tests/guest/crt0_start.S tests/guest/guest_sys.S tests/guest/guest_lib.c "$t"
    if "$CROSS_PREFIX"objdump -d "tests/guest-bin/$name.elf" |
       grep -qE '\b(mul|mulh|mulhu|mulhsu|div|divu|rem|remu|amo|lr\.w|lr\.d|sc\.w|sc\.d)\b'; then
        echo "$name uses M/A insn"; exit 1
    fi
done
echo "guest build OK"
```

（注意：`_start` 放在 `guest_sys.S` 内，无需单独 crt0 文件；上面脚本若列了不存在的 `crt0_start.S` 则删除该项——以 `guest_sys.S` 为准。）

`tests/guest/t_hello.c`：

```c
#include "guest.h"
int main(void) { puts_("PASS t_hello\n"); return 0; }
```

`tests/golden/t_hello.out`：内容一行 `PASS t_hello`。

- [ ] **步骤 4：确认通过** —— `make test` 全绿（L0 + guest 构建验证）。`file tests/guest-bin/t_hello.elf` 显示 `ELF 64-bit LSB executable, UCB RISC-V, soft-float ABI, statically linked`。
- [ ] **步骤 5：提交（M0 里程碑）**：

```bash
git add -A && git commit -m "M0: 构建骨架、L0 测试框架、mini-crt 与 guest 构建管线"
```

提交前：`make && make test` 全绿；code-review 本里程碑 diff。

### 任务 3（M1-a）：mem 模块（TDD）

**文件：**
- 新建：`src/mem.c`、`src/mem.h`、`tests/test_mem.c`
- 修改：`include/rvsim.h`（追加 mem API 原型）、`tests/run_all.sh`（注册 test_mem）

**接口：**
- 对外产出（后续 elf/cpu/syscall 依赖）：

```c
/* include/rvsim.h */
typedef struct { uint64_t lo, hi; int kind; } rv_region;   /* [lo,hi)，kind: 0=ELF 1=heap 2=mmap 3=stack */
typedef struct { uint8_t *data; rv_region rgn[64]; uint32_t n; } rv_mem;
int  mem_init(rv_mem *m);                                   /* 0=ok，-1=OOM(208) */
void mem_free(rv_mem *m);
void mem_add_region(rv_mem *m, uint64_t lo, uint64_t hi, int kind);
int  mem_check(rv_mem *m, uint64_t addr, uint64_t len);     /* 0=ok，-1=未映射/出窗 */
int  mem_read (rv_mem *m, uint64_t a, void *dst, uint64_t n);
int  mem_write(rv_mem *m, uint64_t a, const void *src, uint64_t n);
```

- [ ] **步骤 1：失败测试** `tests/test_mem.c`：

```c
#include "framework.h"
#include "rvsim.h"
int main(void) {
    rv_mem m;
    CHECK(mem_init(&m) == 0);
    /* 未登记区域不可访问 */
    CHECK(mem_check(&m, MEM_BASE, 8) == -1);
    CHECK(mem_write(&m, MEM_BASE, "abc", 3) == -1);
    /* 登记后读写往返 + 边界 */
    mem_add_region(&m, MEM_BASE, MEM_BASE + 16, 0);
    CHECK(mem_write(&m, MEM_BASE, "hello rv64", 10) == 0);
    char buf[16] = {0};
    CHECK(mem_read(&m, MEM_BASE, buf, 10) == 0);
    CHECK(strcmp(buf, "hello rv64") == 0);
    /* [lo,hi) 半开区间：hi 处 1 字节不可访问 */
    CHECK(mem_check(&m, MEM_BASE + 15, 1) == 0);
    CHECK(mem_check(&m, MEM_BASE + 16, 1) == -1);
    /* 跨区域空洞不可访问 */
    mem_add_region(&m, MEM_BASE + 32, MEM_BASE + 64, 0);
    CHECK(mem_check(&m, MEM_BASE + 16, 16) == -1);
    /* 窗口外不可访问 */
    CHECK(mem_check(&m, 0x100, 4) == -1);
    CHECK(mem_check(&m, MEM_BASE + MEM_SIZE - 4, 8) == -1);
    mem_free(&m);
    TEST_DONE();
}
```

`tests/run_all.sh` 追加 `run_l0 test_mem tests/test_mem.c src/mem.c`。

- [ ] **步骤 2：确认失败** —— `make test` → 链接失败 `undefined reference to mem_init`。
- [ ] **步骤 3：最小实现** `src/mem.c`（核心：窗口内 + 覆盖某登记 `[lo,hi)` 才可访问）：

```c
#include "rvsim.h"
#include <stdlib.h>
#include <string.h>
int mem_init(rv_mem *m) {
    m->n = 0;
    m->data = calloc(1, MEM_SIZE);
    return m->data ? 0 : -1;
}
void mem_free(rv_mem *m) { free(m->data); m->data = 0; m->n = 0; }
void mem_add_region(rv_mem *m, uint64_t lo, uint64_t hi, int kind) {
    m->rgn[m->n++] = (rv_region){ lo, hi, kind };
}
int mem_check(rv_mem *m, uint64_t a, uint64_t len) {
    if (a < MEM_BASE || len > MEM_SIZE || a - MEM_BASE > MEM_SIZE - len) return -1;
    for (uint32_t i = 0; i < m->n; i++)
        if (a >= m->rgn[i].lo && a + len <= m->rgn[i].hi) return 0;
    return -1;
}
int mem_read(rv_mem *m, uint64_t a, void *dst, uint64_t n) {
    if (mem_check(m, a, n)) return -1;
    memcpy(dst, m->data + (a - MEM_BASE), n); return 0;
}
int mem_write(rv_mem *m, uint64_t a, const void *src, uint64_t n) {
    if (mem_check(m, a, n)) return -1;
    memcpy(m->data + (a - MEM_BASE), src, n); return 0;
}
```

- [ ] **步骤 4：确认通过** —— `make test` → `L0 test_mem: PASS`。
- [ ] **步骤 5：不提交**（M1 收尾统一提交）。

### 任务 4（M1-b）：elf 模块 + L4 畸形 ELF（M1 收尾，含 commit）

**文件：**
- 新建：`src/elf.c`、`tests/test_elf.c`、`tests/make_bad_elf.sh`
- 修改：`include/rvsim.h`（elf API）、`tests/run_all.sh`（test_elf + L4 用例）

**接口：**

```c
/* include/rvsim.h */
typedef struct { uint64_t entry, brk_base, bias; } rv_elf;
/* 返回 0=ok；负数=错误细分（-1 magic -2 class -3 machine -4 截断
   -5 无 PT_LOAD -6 PT_INTERP -7 entry 越界），全部映射退出码 201 */
int elf_load(rv_mem *m, const char *path, rv_elf *out);
```

语义：ET_EXEC bias=0；ET_DYN bias=ET_DYN_BIAS；每段按 `p_offset/p_vaddr+bias/p_filesz` 复制，`p_memsz>p_filesz` 部分清零（calloc 已保证）；`brk_base = max(p_vaddr+bias+p_memsz)` 向上页对齐。

- [ ] **步骤 1：失败测试** —— `tests/test_elf.c`（好 ELF 路径检查；坏 ELF 由 shell 生成后以 argv 传入）：

```c
#include "framework.h"
#include "rvsim.h"
int main(int argc, char **argv) {
    rv_mem m; rv_elf e;
    CHECK(mem_init(&m) == 0);
    /* 正常 ELF：由交叉工具链产出的 t_hello.elf */
    CHECK(elf_load(&m, "tests/guest-bin/t_hello.elf", &e) == 0);
    CHECK(e.entry != 0);
    CHECK((e.entry & 3) == 0);
    CHECK(e.brk_base > e.entry);
    CHECK((e.brk_base & (PAGE_SIZE - 1)) == 0);
    CHECK(e.bias == 0);                      /* Buildroot 产出为 ET_EXEC */
    /* 装载后 entry 处可取指：前 4 字节非零 */
    uint32_t w = 0;
    CHECK(mem_read(&m, e.entry, &w, 4) == 0 && w != 0);
    /* 坏 ELF：argv[1]=路径 argv[2]=期望负错误码 */
    if (argc == 3) {
        int want = atoi(argv[2]);
        CHECK(elf_load(&m, argv[1], &e) == want);
    }
    mem_free(&m);
    TEST_DONE();
}
```

`tests/make_bad_elf.sh`：

```sh
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
E=tests/guest-bin/t_hello.elf
head -c 100 "$E" > build/bad_trunc.elf                      # 期望 -4
printf '\x7fELF\x01' > build/bad_class.elf; cat "$E" >> build/bad_class.elf  # 期望 -2（class=1）
python3 - "$E" build/bad_machine.elf <<'PY'                 # e_machine→0x3E(x86)，期望 -3
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); d[18:20] = (0x3E).to_bytes(2, 'little')
open(sys.argv[2], 'wb').write(d)
PY
python3 - "$E" build/bad_magic.elf <<'PY'                   # 魔数破坏，期望 -1
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); d[0] = 0x7A
open(sys.argv[2], 'wb').write(d)
PY
```

`tests/run_all.sh` 追加：

```sh
bash tests/build_guest.sh   # test_elf 依赖 t_hello.elf
run_l0 test_elf tests/test_elf.c src/mem.c src/elf.c
bash tests/make_bad_elf.sh
for c in "build/bad_trunc.elf:-4" "build/bad_class.elf:-2" "build/bad_machine.elf:-3" "build/bad_magic.elf:-1"; do
    p=${c%%:*}; w=${c##*:}
    if build/test_elf "$p" "$w" >/dev/null 2>&1; then echo "L4 $p: PASS"; else echo "L4 $p: FAIL"; FAIL=1; fi
done
```

- [ ] **步骤 2：确认失败** —— `make test` → `undefined reference to elf_load`。
- [ ] **步骤 3：最小实现** `src/elf.c`（要点：手写解析 Elf64_Ehdr/Phdr，逐字段小端读取；`e_phoff/e_phnum` 遍历；仅接受 `EM_RISCV=243`、`ELFCLASS64`、`ET_EXEC/ET_DYN`；拒绝 `PT_INTERP`；至少一个 `PT_LOAD`；entry 在窗口内且落某 PT_LOAD 区间）——完整实现约 90 行，按上述语义逐条编码。
- [ ] **步骤 4：确认通过** —— `make test` → `L0 test_elf: PASS` 且 4 个 L4 用例 PASS。
- [ ] **步骤 5：提交（M1）**：`git add -A && git commit -m "M1: mem/elf 模块 + L4 畸形 ELF 注入测试"`

### 任务 5（M2-a）：decode 模块（TDD）

**文件：**
- 新建：`src/decode.c`、`tests/enc.h`、`tests/test_decode.c`
- 修改：`include/rvsim.h`（rv_op/rv_dec/rv_decode）

**接口：**

```c
typedef enum { RV_LUI, RV_AUIPC, RV_JAL, RV_JALR, RV_BEQ, RV_BNE, RV_BLT, RV_BGE,
  RV_BLTU, RV_BGEU, RV_LB, RV_LH, RV_LW, RV_LD, RV_LBU, RV_LHU, RV_LWU,
  RV_SB, RV_SH, RV_SW, RV_SD, RV_ADDI, RV_SLTI, RV_SLTIU, RV_XORI, RV_ORI,
  RV_ANDI, RV_SLLI, RV_SRLI, RV_SRAI, RV_ADD, RV_SUB, RV_SLL, RV_SLT, RV_SLTU,
  RV_XOR, RV_SRL, RV_SRA, RV_OR, RV_AND, RV_ADDIW, RV_SLLIW, RV_SRLIW, RV_SRAIW,
  RV_ADDW, RV_SUBW, RV_SLLW, RV_SRLW, RV_SRAW, RV_FENCE, RV_FENCEI,
  RV_ECALL, RV_EBREAK, RV_OP_COUNT } rv_op;
typedef struct { rv_op op; uint8_t rd, rs1, rs2; uint32_t raw; int64_t imm; } rv_dec;
bool rv_decode(uint32_t insn, rv_dec *d);   /* false = 非法/保留编码 */
```

imm 语义：I/S 符号扩展；B/J 符号扩展且 LSB=0（非法则 false）；U 为 `insn & 0xFFF00000` 符号扩展到 64；移位类 imm = imm[5:0]（正数），`imm[11:6] != 0` → false。

- [ ] **步骤 1：失败测试** —— `tests/enc.h`：

```c
#define OP_STD(f7,rs2,rs1,f3,rd,op) (((f7)<<25)|((rs2)<<20)|((rs1)<<15)|((f3)<<12)|((rd)<<7)|(op))
#define ENC_I(imm,rs1,f3,rd,op)  (((imm)&0xFFF)<<20 | (rs1)<<15 | (f3)<<12 | (rd)<<7 | (op))
#define ENC_S(imm,rs2,rs1,f3,op) (((imm)&0xFE0)<<20 | (rs2)<<20 | ((imm)&0x1F)<<7 | (rs1)<<15 | (f3)<<12 | (op))
#define ENC_B(imm,rs2,rs1,f3,op) ((((imm)&0x1000)<<19) | (((imm)&0x7E0)<<20) | (rs2)<<20 | (rs1)<<15 | (f3)<<12 | (((imm)&0x1E)<<7) | (((imm)&0x800)<<4) | (op))
#define ENC_U(imm,rd,op)         (((imm)&0xFFFFF)<<12 | (rd)<<7 | (op))
#define ENC_J(imm,rd,op)         ((((imm)&0x100000)<<11) | (((imm)&0xFFE)<<20) | (((imm)&0x800)<<9) | (((imm)&0x7FE)<<20) | (rd)<<7 | (op))
#define OPC(op) (op)
```

`tests/test_decode.c`（每种格式极值 + 保留编码黑名单）：

```c
#include "framework.h"
#include "enc.h"
int main(void) {
    rv_dec d;
    CHECK(rv_decode(ENC_I(-1, 2, 0, 1, 0x13), &d) && d.op == RV_ADDI && d.imm == -1 && d.rd == 1);
    CHECK(rv_decode(ENC_I(0x7FF, 2, 0, 1, 0x13), &d) && d.imm == 0x7FF);
    CHECK(rv_decode(ENC_U(0xFFFFF, 5, 0x37), &d) && d.op == RV_LUI && d.imm == (int64_t)(int32_t)0xFFFFF000u);
    CHECK(rv_decode(ENC_J(0x100000, 1, 0x6F), &d) && d.imm == -1048576);   /* J 型负极值 */
    CHECK(rv_decode(ENC_B(-4096, 2, 1, 0, 0x63), &d) && d.imm == -4096);   /* B 型负极值 */
    /* 移位：shamt 高位非零 → 非法 */
    CHECK(!rv_decode(ENC_I(0xFC0, 1, 1, 1, 0x13), &d));      /* SLLI imm[11:6]!=0 */
    /* 保留编码：S 段 funct7=0000001(M)、AMO opcode 0x2F、C 扩展 16 位字 */
    CHECK(!rv_decode(OP_STD(0x01, 3, 2, 0, 1, 0x33), &d));   /* MUL */
    CHECK(!rv_decode(0x0000002Fu | 2u<<27, &d));             /* AMOADD.W */
    CHECK(!rv_decode(0x00000013u + 2, &d));                  /* 0x15 非法低 2 位 */
    TEST_DONE();
}
```

- [ ] **步骤 2：确认失败** —— `undefined reference to rv_decode`。
- [ ] **步骤 3：最小实现** `src/decode.c`：先提取字段，再按 `insn & 0x7F` 分派 opcode，每个 opcode 校验 funct3/funct7 后填 `rv_op` 与 imm。52 条全部列入（无占位）。
- [ ] **步骤 4：确认通过** —— `make test` → `L0 test_decode: PASS`。
- [ ] **步骤 5：不提交。**

### 任务 6（M2-b）：rv_step 核心 + 整数/移位指令（TDD）

**文件：**
- 新建：`src/cpu.c`、`tests/test_alu.c`、`tests/test_shift.c`、`tests/test_wops.c`
- 修改：`include/rvsim.h`（rvsim/rv_step）、`tests/run_all.sh`

**接口：**

```c
typedef struct { uint64_t x[32], pc; uint64_t steps; } rv_cpu;
typedef struct {
    rv_mem mem; rv_cpu cpu;
    int fault;                 /* 0=无 RV_FV_UNMAPPED=1 RV_FV_MISALIGNED=2 RV_FV_ILLEGAL=3 */
    bool exited; int exit_code;
} rvsim;
void rvsim_reset(rvsim *s);
int  rv_step(rvsim *s);        /* 执行一条；返回 s->fault；guest exit 置 s->exited */
```

- [ ] **步骤 1：失败测试** `tests/test_alu.c`（全路径：编码→取指→解码→执行）：

```c
#include "framework.h"
#include "enc.h"
static rvsim s;
static void one(uint32_t insn, uint64_t a, uint64_t b, uint64_t exp, int rd) {
    rvsim_reset(&s);
    s.cpu.pc = MEM_BASE; mem_add_region(&s.mem, MEM_BASE, MEM_BASE + 16, 0);
    mem_write(&s.mem, MEM_BASE, &insn, 4);
    s.cpu.x[10] = a; s.cpu.x[11] = b;
    CHECK(rv_step(&s) == 0);
    CHECK(s.cpu.x[rd] == exp);
    CHECK(s.cpu.pc == MEM_BASE + 4);
}
int main(void) {
    one(ENC_I(1, 10, 0, 12, 0x13), 5, 0, 6, 12);                    /* ADDI */
    one(ENC_I(-1, 10, 0, 12, 0x13), 5, 0, 4, 12);                   /* ADDI -1 */
    one(OP_STD(0x00, 11, 10, 0, 12, 0x33), 0x7FFFFFFFFFFFFFFFULL, 1, (uint64_t)0x8000000000000000ULL, 12); /* ADD 回绕 */
    one(OP_STD(0x20, 11, 10, 0, 12, 0x33), 0, 1, (uint64_t)-1, 12); /* SUB */
    one(ENC_I(5, 10, 2, 12, 0x13), (uint64_t)-2, 0, 0, 12);         /* SLTI: -2<5 →1? 用 -2: (int64)-2 < 5 → 1 */
    one(ENC_I(5, 10, 3, 12, 0x13), (uint64_t)-2, 0, 1, 12);         /* SLTIU: 0xFF..FE > 5 → 0 */
    one(OP_STD(0x00, 11, 10, 2, 12, 0x33), 1, 2, 0, 12);            /* SLT 1<2 */
    one(OP_STD(0x00, 11, 10, 3, 12, 0x33), (uint64_t)-1, 1, 1, 12); /* SLTU 无符号大 */
    one(OP_STD(0x00, 10, 10, 0, 0, 0x33), 0x1234, 0, 0, 0);         /* x0 写入丢弃 */
    TEST_DONE();
}
```

`tests/test_shift.c`：SLL/SRL/SRA 用 `0x8000000000000000` 边界（SRA 后符号保持、SRL 后为 0）、移位量取低 6 位（rs=65 等价 1）；`tests/test_wops.c`：9 条 W 指令逐条验证 32→64 符号扩展（如 `ADDIW -1 → 0xFFFFFFFFFFFFFFFF`、`SRLIW 1<<31 → 1`、`SRAIW` 对比）。写法同 `test_alu.c` 的 `one()` 辅助（每个文件自带该辅助函数，逐字复制，不跨文件引用）。

- [ ] **步骤 2：确认失败** —— `undefined reference to rv_step`。
- [ ] **步骤 3：最小实现** `src/cpu.c`：`rv_step` = 对齐检查取指（4 对齐 + mem_read 4 字节）→ `rv_decode`（false → fault=ILLEGAL）→ switch 执行（本任务实现 ALU/移位/W 分支臂，其余臂暂 `fault=ILLEGAL`——这是真实语义：未实现的组此刻本就应判非法）→ 写回（rd!=0）→ `pc += 4`（控制流组后续任务接入）。
- [ ] **步骤 4：确认通过** —— `make test` 全绿。
- [ ] **步骤 5：不提交。**

### 任务 7（M2-c）：M2 收尾 guest 程序 + commit

- [ ] **步骤 1：失败测试** —— 新建 `tests/guest/t_arith.c`、`t_imm_edges.c`、`t_shift.c`、`t_wops.c`。示例 `t_arith.c`（其余同构，各自覆盖规格 §8.3 对应边界，全部用 `CHECK_G(__LINE__, …)`）：

```c
#include "guest.h"
int main(void) {
    long long a = (long long)0x7FFFFFFFFFFFFFFFLL;
    CHECK_G(__LINE__, a + 1 == (long long)0x8000000000000000LL);
    unsigned long long u = 0xFFFFFFFFFFFFFFFFULL;
    CHECK_G(__LINE__, u + 1 == 0);
    CHECK_G(__LINE__, (long long)-1 < 0 && (unsigned long long)-1 > 0ULL); /* SLT/SLTU 语义 */
    puts_("PASS t_arith\n");
    return 0;
}
```

`tests/run_all.sh` 追加 L1 runner（此刻 rvsim 还跑不了 ELF，先接入 M5）——**本任务只把程序加入 build_guest.sh 循环与 golden 文件**（`tests/golden/t_arith.out` = `PASS t_arith`），e2e 校验函数写成但以 `rvsim_can_run=0` 跳过。

- [ ] **步骤 2：确认失败→通过** —— guest 编译失败会红；补实现后 `make test` 绿。注意 guest 侧编译器若因 `a+1` 有符号溢出告警，改用无符号运算+显式比较实现（不改预期值）。
- [ ] **步骤 3：提交（M2）**：`git commit -m "M2: decode + 整数/移位/W 类指令 + L0/L1 测试"`

### 任务 8（M3）：访存 11 条 + 对齐/未映射故障（M3 收尾，含 commit）

**文件：**
- 修改：`src/cpu.c`（访存臂 + 故障检查）、`tests/test_memops.c`、新增 `tests/guest/t_load_store.c`、`t_x0.c` 与 golden
- [ ] **步骤 1：失败测试** `tests/test_memops.c`（`one()` 辅助同前，另加内存准备）：

```c
/* LD: ENC_I(0,10,3,12,0x03) rs1=10 基址；先在 MEM_BASE+8 放 0x88..88 */
one_load(RV_LD, MEM_BASE + 8, (uint64_t)0x8888888888888888ULL, 8);
/* LW 符号扩展：放 0x80000000 → 0xFFFFFFFF80000000 */
one_load(RV_LW, MEM_BASE + 8, 0xFFFFFFFF80000000ULL, 4);
/* LWU 零扩展 → 0x0000000080000000 */
/* LH/LHU/LB/LBU 同理；SB/SH/SW/SD 写后读回 */
/* 未对齐：LW @+2 → rv_step 返回 RV_FV_MISALIGNED，且目标寄存器不变 */
/* 未映射：访问未登记地址 → RV_FV_UNMAPPED */
```

guest 侧 `t_load_store.c` 覆盖全部 11 条正反例（用 `CHECK_G`），`t_x0.c` 验证 rd=x0 写丢弃与 store 到 x0 基址。
- [ ] **步骤 2：确认失败** → 步骤 3：实现访存臂（先对齐检查 → mem_read/write → 扩展/截断写回；store 的 imm 用 S 型）；步骤 4：`make test` 全绿。
- [ ] **步骤 5：提交（M3）**：`git commit -m "M3: load/store 11 条 + 对齐与未映射故障"`

### 任务 9（M4）：控制流 10 条（M4 收尾，含 commit）

**文件：**
- 修改：`src/cpu.c`、`tests/test_flow.c`、新增 `tests/guest/t_branch.c`、`t_call.c`、`t_recursion.c`、`t_bubble.c`、`t_string.c`、`t_fib.c` 与 golden
- [ ] **步骤 1：失败测试** `tests/test_flow.c`：JAL/ JALR（bit0 清除）/6 条分支（相等/不等/有符号/无符号边界）/LUI/AUIPC 逐条验证 pc 写入；`ENC_J/ENC_B` 负偏移回跳；JALR 目标未对齐 → `RV_FV_MISALIGNED`。
- [ ] **步骤 2：确认失败** → 步骤 3：实现控制流臂（AUIPC = pc + imm；分支比较用 `(int64_t)`/`(uint64_t)` 双路径）。
- [ ] **步骤 4：确认通过** + guest 程序接入 build_guest.sh 与 golden（`t_bubble.c` 同时用于 L2：`tests/run_all.sh` 加宿主 twin 构建比对 `cc -O2 -std=c99 -Itests/guest -o build/host_t_bubble tests/guest/t_bubble.c tests/host/shim.c`，两侧 stdout 与退出码必须一致）。
- [ ] **步骤 5：提交（M4）**：`git commit -m "M4: 控制流 10 条 + L2 双编译差分(t_bubble)"`

### 任务 10（M5-a）：fence/ecall/ebreak + syscall 模块（TDD）

**文件：**
- 新建：`src/syscall.c`、`tests/test_syscall.c`
- 修改：`include/rvsim.h`（syscall API、`rvsim` 增 `uint64_t brk_base, brk_cur, mmap_top; int host_fd[3];`）、`src/cpu.c`（ECALL/EBREAK/FENCE 臂）

**接口：**

```c
typedef struct { int kind;  /* 0=返回值 1=guest exit 2=不支持 */ int64_t val; } rv_sc_ret;
rv_sc_ret rv_syscall(rvsim *s, uint64_t num, uint64_t a0, uint64_t a1,
                     uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
```

- [ ] **步骤 1：失败测试** `tests/test_syscall.c`（fd 重定向：`s.host_fd[1] = 管道写端`；指针参数通过 mem_write 预置）：

```c
/* write: mem_write(s.host buf "hi", len2); rv_syscall(&s,64,1,buf,2) → val==2, 管道读端收到 "hi" */
/* write fd=5 → val==-9(EBADF) */
/* brk: 初始 brk_cur==brk_base; 请求 brk_base+4096 → 返回新值且 heap 区登记可写 */
/*      请求 brk_base+HEAP_CAP+1 → 返回旧值(不变) */
/* mmap: flags=0x22,fd=-1,len=8192 → 返回 MMAP_BASE, 第二次返回 MMAP_BASE+8192 */
/* mmap len=0 → -22(EINVAL); flags=0x03 → -22; 总量超 MMAP_CAP → -12(ENOMEM) */
/* munmap: 精确匹配回收 → 再次 mmap 复用首地址; 非精确 → -22 */
/* uname: 预置 390 字节缓冲区 → sysname=="Linux" machine=="riscv64" */
/* clock_gettime: id=0/1 → tv_sec>0 且返回 0; id=99 → -22 */
/* 未知号码 999 → kind==2 */
```

- [ ] **步骤 2：确认失败** → **步骤 3：实现** `src/syscall.c`（逐项按规格 §5；read/write 仅转发 `s->host_fd[]`；brk/mmap 操作 mem 区域登记；EBREAK 由 cpu.c 置 `exited, exit_code=RVSIM_EX_EBREAK`；FENCE/FENCE.I 为空操作退休）→ **步骤 4**：`make test` 全绿。
- [ ] **步骤 5：不提交。**

### 任务 11（M5-b）：main 装配 + 栈初始化 + 首个端到端（M5 收尾，含 commit）

**文件：**
- 修改：`src/main.c`、`src/cpu.c`（build_stack）、`tests/run_all.sh`（L1 e2e 接入）
- [ ] **步骤 1：失败测试** —— run_all.sh 去掉 `rvsim_can_run=0` 跳过逻辑，接入：

```sh
run_guest() { # run_guest <name> [args...]
    local name=$1; shift
    ./build/rvsim "tests/guest-bin/$name.elf" "$@" > tests/run/$name.out 2> tests/run/$name.err
    local ec=$?
    if [ "$ec" = 0 ] && diff -q tests/golden/$name.out tests/run/$name.out >/dev/null; then
        echo "L1 $name: PASS"
    else
        echo "L1 $name: FAIL ec=$ec"; cat tests/run/$name.err; FAIL=1
    fi
}
run_guest t_hello
run_guest t_arith
```

预期失败：rvsim 尚不能执行 ELF（main 还是 usage stub）。
- [ ] **步骤 2：实现** `src/main.c`：argc 校验 → `mem_init` → `elf_load` → `build_stack`（按规格 §3.4 精确布局：argc/argv/NULL/envp NULL/auxv{AT_PAGESZ,AT_RANDOM,AT_NULL}/字符串区，sp 16 对齐）→ 主循环 `while(!s.exited) rv_step(&s)` + steps 上限 → 退出码映射（fault→202/203/204，未知 syscall→205，正常→exit_code）。`tests/guest/t_argv.c` 验证 argc/argv 透传（golden 固定）。
- [ ] **步骤 3：确认通过** —— `make test`：`L1 t_hello/t_arith/t_argv: PASS`（首个端到端闭环）。
- [ ] **步骤 4：提交（M5）**：`git commit -m "M5: syscall 子集 + 栈初始化 + 首个端到端 t_hello"`

### 任务 12（M6-a）：CLI 可观测性（-v/-vv/--trace/--dump-regs/--dump-mem/--max-steps）

**文件：**
- 新建：`src/trace.c`、`src/cli.c`（从 stub 扩展）、`tests/test_trace.c`、golden 快照 `tests/golden/snap_*.out`
- [ ] **步骤 1：失败测试**：L0 层 `test_trace.c` 直测 trace/dump 输出到 `fmemopen`/临时文件并与 golden 字符串全等；L3 层 run_all.sh 增加快照用例：

```sh
snap() { # snap <golden> <cmd...>
    "$@" > tests/run/snap.out 2> tests/run/snap.err; local ec=$?
    diff -q tests/golden/$1.out tests/run/snap.out >/dev/null && [ "$ec" = "$1_ec" ] ...
}
snap snap_trace  ./build/rvsim --trace - tests/guest-bin/t_arith.elf
snap snap_regs   ./build/rvsim --dump-regs tests/guest-bin/t_hello.elf
snap snap_mem    ./build/rvsim --dump-mem 0x100000:32 tests/guest-bin/t_hello.elf
snap snap_limit  ./build/rvsim --max-steps 10 tests/guest-bin/t_fib.elf   # 退出 207
```

- [ ] **步骤 2：确认失败** → **步骤 3：实现**（trace 行格式 `T <seq> pc=0x<16hex> insn=0x<8hex> <助记名>[ rd=xN=0x<16hex>]`；-v/-vv 走 stderr；dump 格式按规格 §6）→ **步骤 4**：全绿。
- [ ] **步骤 5：不提交。**

### 任务 13（M6-b）：交互调试器（M6 收尾，含 commit）

**文件：**
- 新建：`src/debugger.c`、golden `tests/golden/dbg_*.out`
- [ ] **步骤 1：失败测试**（L3 会话快照，stdin 用管道喂入）：

```sh
printf 's 4\nr\nb 0x%s\nq\n' "$ENTRY" | ./build/rvsim -s tests/guest-bin/t_hello.elf > tests/run/dbg1.out 2>&1
diff tests/golden/dbg_session1.out tests/run/dbg1.out
printf 'c\nq\n' | ./build/rvsim -s tests/guest-bin/t_arith.elf ...   # 断点命中会话
printf 'm 0x100000 16\nc\n' | ./build/rvsim -s ...                   # 内存查看会话
```

- [ ] **步骤 2：确认失败** → **步骤 3：实现**（命令循环按规格 §6.2：s/c/r/m/b/d/q/h；断点 8 个上限；stdin EOF=q）→ **步骤 4**：全绿。
- [ ] **步骤 5：提交（M6）**：`git commit -m "M6: CLI 可观测性 + 交互调试器 + L3 快照测试"`

### 任务 14（M7-a）：测试矩阵收口（剩余 guest 程序 + 全边界补齐）

- [ ] **步骤 1：失败测试** —— 补齐规格 §8.4 全部 20 个程序中尚缺者：`t_mmap.c`、`t_brk.c`、`t_uname.c`、`t_clock.c`、`t_badcall.c`（错误路径断言 + golden 含 stderr 诊断行，`run_guest_err` 函数比对退出码 205 与 `rvsim: unsupported syscall` 前缀）、`t_maxsteps.c`（207）、`t_misaligned.c`（203）。每程序先加 runner（红）再写程序（绿）。
- [ ] **步骤 2：边界复核** —— 对照规格 §8.3 清单逐项勾验，缺测补测（每个补测同样先红后绿）。
- [ ] **步骤 3：确认通过** —— `make && make guest && make test` 全绿；`bash tests/run_all.sh | tee /tmp/regression.txt` 存档。

### 任务 15（M7-b）：验收收尾（M7 收尾，最终 commit）

- [ ] **步骤 1：README**（构建/运行/调试示例 + 测试说明）。
- [ ] **步骤 2：code-review** —— 加载 superpower-requesting-code-review，对全量 diff 评审；发现问题按 TDD 修复（先补失败测试）。
- [ ] **步骤 3：verification-before-completion** —— 加载 superpower-verification-before-completion，运行并记录：`make && make guest && make test`、`git log --oneline`（应含 8 个里程碑 commit）、 smoke 性能数据（`--max-steps` 下 t_bubble 耗时记录，无指标仅存档）。
- [ ] **步骤 4：提交（M7）**：`git commit -m "M7: 测试矩阵收口 + README + 验收"`

## 计划自检记录（执行前已完成）

1. **规格覆盖度**：§2 模块→任务 1/3/4/5/6/10/11/12/13；§3 内存与栈→任务 3/11；§4 指令 52 条→任务 5/6/7/8/9/10（ALU 19+移位 3→6，W 9→6，访存 11→8，控制流 10→9，系统 4→10）；§5 syscall→任务 10/11；§6 CLI→任务 12/13；§7 退出码→任务 11（映射）+12（207）+10（205/206）；§8 测试→任务 1–14 各层 + 14（边界收口）；§9 里程碑→任务 2/4/7/8/9/11/13/15 各一个 commit。无遗漏项。
2. **占位符扫描**：无 TBD/TODO；任务 4 步骤 3 与任务 10 步骤 3 为"按已给语义逐条编码"式条目，其语义在规格 §4/§5 中为完整冻结条目，且对应接口签名已在任务内给出——属数据引自规格，非占位。
3. **类型一致性**：`rv_op` 枚举（任务 5）与 `rv_step`（任务 6）、`rv_sc_ret/rv_syscall`（任务 10）与 `src/cpu.c` ECALL 臂（任务 10/11）、`guest.h` 的 `gwrite/gexit/puts_/CHECK_G`（任务 2）与全部 t_*.c 一致；`host_fd[3]` 在任务 10 引入且 syscall 与测试两侧同名。
