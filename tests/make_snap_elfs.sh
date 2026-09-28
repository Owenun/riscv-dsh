#!/usr/bin/env bash
# tests/make_snap_elfs.sh — L3 快照探针 ELF 现场生成（产物进 build/，build/ 已
# gitignore，不进版本库）。修复轮 1（重要 5）新增；终审修复波（M7 后）扩展至 5 个。
#
# 全部为手工构造的最小 RV64I ELF（无 libc、无 crt，指令逐条手工编码）：
#
# 1) build/fault_ld.elf —— snap_fault 用：单 PT_LOAD，首指令 `ld x1,0(x0)`
#    访问地址 0x0（MEM_BASE=0x10000 之下，未登记 → 202 fault unmapped，
#    诊断 `pc=0x10000 addr=0x0`，stdout 恒空）。后随 exit 序列不可达
#    （ld 必先故障），仅为"有出口的完整程序"而写。
# 2) build/brk_probe.elf —— snap_brk 用：`brk(brk_base+8)` 后 `exit(0)`。
#    brk_base = page_up(0x10000+28) = 0x11000（page_up 见 src/elf.c），
#    请求 0x11008 合法 → brk_cur 0x11000→0x11008；-v stderr 锁 brk
#    "旧 -> 新" 日志行（修复轮 1 重要 3）。
# 3) build/etdyn_probe.elf —— snap_etdyn 用（终审修复波/重要 1）：真实 ET_DYN
#    （e_type=3）端到端探针。段 vaddr/entry 均为相对域 0x10000（真实 ET_DYN
#    语义：e_entry 不含装载基址），由 elf_load 加 ET_DYN_BIAS=0x100000 →
#    段落 [0x110000, 0x110047)、entry_va=0x110000；经 rvsim main 全路径
#    （bias 后 entry 取指执行、build_stack 栈、write/exit syscall、退出码
#    透传）锁定 M7-A 曾暴露的 "entry 未加 bias" 类缺陷。程序体免重定位：
#    装载地址确定，auipc 直接取运行时 pc（=bias 后绝对地址）定位段内字符串，
#    write "PASS etdyn\n" 后 exit(0)。
# 4) build/illegal.elf —— snap_illegal 用（终审修复波/重要 2）：单词级探针，
#    .word 0xFFFFFFFF（opcode 等全部字段越界）→ 204 + stderr
#    `rvsim: illegal insn pc=0x10000 word=0xffffffff`、stdout 恒空。
# 5) build/ebreak.elf —— snap_ebreak 用（终审修复波/重要 2）：单词级探针，
#    .word 0x00100073（EBREAK 标准编码）→ 206 + stderr
#    `rvsim: ebreak pc=0x10000`（pc 不推进）、stdout 恒空。
#
# 文件布局（五 ELF 同构）：Ehdr 64B + Phdr 56B（@64）+ 补零至 128B，
# 代码 @ 文件偏移 128 = vaddr 0x10000（=MEM_BASE，段落在 512MiB 窗口内，
# entry=0x10000 落在段内 → 通过 src/elf.c 全部校验；ET_DYN 的 0x10000 为
# 相对域，装载时统一加 bias 后同构）。p_align=8：off(128) 与 vaddr(0x10000)
# 均按 8 对齐同余。
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
python3 - <<'PY'
import struct

def make_elf(words, e_type=2, vaddr=0x10000, entry=0x10000, tail=b''):
    """words: 32 位指令字列表；tail: 追加在指令后的原始字节（如字符串常量，
    计入 p_filesz/p_memsz，随段一起装载）。→ 最小 RV64I ELF（单 R+X PT_LOAD）。
    e_type=2 为 ET_EXEC（bias=0）；e_type=3 为 ET_DYN，vaddr/entry 写相对域，
    装载基址 ET_DYN_BIAS=0x100000 由 src/elf.c 统一加（out->entry 亦加 bias）。"""
    code = b''.join(struct.pack('<I', w) for w in words) + tail
    ehdr = bytearray(64)
    ehdr[0:8] = b'\x7fELF\x02\x01\x01\x00'          # ELF64 / 小端 / 版本 1
    struct.pack_into('<HHI', ehdr, 16, e_type, 243, 1)   # e_type e_machine e_version
    struct.pack_into('<QQQ', ehdr, 24, entry, 64, 0)     # e_entry e_phoff=64 e_shoff
    struct.pack_into('<I', ehdr, 48, 0)             # e_flags
    struct.pack_into('<HHHHHH', ehdr, 52, 64, 56, 1, 0, 0, 0)  # ehsize/phentsize/phnum=1/sh*
    ph = bytearray(56)
    # p_type=PT_LOAD p_flags=R+X p_offset=128 p_vaddr=p_paddr=vaddr
    # p_filesz=p_memsz=len(code)（含 tail）p_align=8
    struct.pack_into('<IIQQQQQQ', ph, 0, 1, 5, 128, vaddr, vaddr,
                     len(code), len(code), 8)
    return bytes(ehdr) + bytes(ph) + b'\x00' * (128 - 64 - 56) + code

# ---- fault_ld.elf：ld x1,0(x0) 必故障；exit 序列不可达 ----
fault = make_elf([
    0x00003083,     # ld  x1, 0(x0)   ：访存 0x0 → 未映射，202（pc=0x10000 addr=0x0）
    0x00000513,     # li  a0, 0       ┐
    0x05d00893,     # li  a7, 93      ├ exit(0)，不可达（ld 必先故障）
    0x00000073,     # ecall           ┘
])
open('build/fault_ld.elf', 'wb').write(fault)

# ---- brk_probe.elf：brk(brk_base+8) 后 exit(0) ----
brk = make_elf([
    0x00011537,     # lui  a0, 0x11   ：a0 = 0x11000
    0x00850513,     # addi a0, a0, 8  ：a0 = 0x11008 = brk_base(0x11000)+8
    0x0d600893,     # addi a7, x0, 214：__NR_brk
    0x00000073,     # ecall           ：brk(0x11008) 成功，brk_cur 0x11000→0x11008
    0x00000513,     # li   a0, 0
    0x05d00893,     # addi a7, x0, 93 ：__NR_exit
    0x00000073,     # ecall           ：exit(0)（终止型，pc 停在 0x10024）
])
open('build/brk_probe.elf', 'wb').write(brk)

# ---- etdyn_probe.elf（终审修复波/重要 1）：ET_DYN 真实端到端 ----
# 9 条指令共 36 字节，字符串 "PASS etdyn\n"（11 字节）随后 @ entry_va+36。
# 装载后 entry_va = 0x10000 + ET_DYN_BIAS = 0x110000；auipc a1,0 取运行时
# pc（已是 bias 后绝对地址），addi 36 指向段内字符串 → 免重定位。
etdyn = make_elf([
    0x00000597,     # auipc a1, 0      ：a1 = pc = 0x110000（bias 后绝对地址）
    0x02458593,     # addi a1, a1, 36  ：a1 = 0x110024 = 段内字符串
    0x00b00613,     # li    a2, 11     ：len("PASS etdyn\n") = 11
    0x04000893,     # li    a7, 64     ：__NR_write
    0x00100513,     # li    a0, 1      ：fd = 1
    0x00000073,     # ecall            ：write(1, "PASS etdyn\n", 11)
    0x00000513,     # li    a0, 0
    0x05d00893,     # li    a7, 93     ：__NR_exit
    0x00000073,     # ecall            ：exit(0)
], e_type=3, tail=b'PASS etdyn\n')
open('build/etdyn_probe.elf', 'wb').write(etdyn)

# ---- illegal.elf（终审修复波/重要 2）：单词 .word 0xFFFFFFFF → 204 ----
# 0xFFFFFFFF 各字段全越界（opcode=0x7f 非法）→ decode 拒绝，fault_insn=insn，
# pc 不推进（停在 0x10000）。
illegal = make_elf([0xFFFFFFFF])
open('build/illegal.elf', 'wb').write(illegal)

# ---- ebreak.elf（终审修复波/重要 2）：单词 .word 0x00100073 → 206 ----
# EBREAK 标准编码：exited + exit_code=206，pc 不推进（停在 0x10000）。
ebreak = make_elf([0x00100073])
open('build/ebreak.elf', 'wb').write(ebreak)
PY
