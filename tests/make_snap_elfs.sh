#!/usr/bin/env bash
# tests/make_snap_elfs.sh — L3 快照探针 ELF 现场生成（产物进 build/，build/ 已
# gitignore，不进版本库）。修复轮 1（重要 5）新增。
#
# 生成两个手工构造的最小 RV64I ELF（无 libc、无 crt，指令逐条手工编码）：
#
# 1) build/fault_ld.elf —— snap_fault 用：单 PT_LOAD，首指令 `ld x1,0(x0)`
#    访问地址 0x0（MEM_BASE=0x10000 之下，未登记 → 202 fault unmapped，
#    诊断 `pc=0x10000 addr=0x0`，stdout 恒空）。后随 exit 序列不可达
#    （ld 必先故障），仅为"有出口的完整程序"而写。
# 2) build/brk_probe.elf —— snap_brk 用：`brk(brk_base+8)` 后 `exit(0)`。
#    brk_base = page_up(0x10000+28) = 0x11000（page_up 见 src/elf.c），
#    请求 0x11008 合法 → brk_cur 0x11000→0x11008；-v stderr 锁 brk
#    "旧 -> 新" 日志行（修复轮 1 重要 3）。
#
# 文件布局（两 ELF 同构）：Ehdr 64B + Phdr 56B（@64）+ 补零至 128B，
# 代码 @ 文件偏移 128 = vaddr 0x10000（=MEM_BASE，段落在 512MiB 窗口内，
# entry=0x10000 落在段内 → 通过 src/elf.c 全部校验）。p_align=8：
# off(128) 与 vaddr(0x10000) 均按 8 对齐同余。
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
python3 - <<'PY'
import struct

def make_elf(words):
    """words: 32 位指令字列表 → 最小 ET_EXEC RV64I ELF（单 R+X PT_LOAD）。"""
    code = b''.join(struct.pack('<I', w) for w in words)
    ehdr = bytearray(64)
    ehdr[0:8] = b'\x7fELF\x02\x01\x01\x00'          # ELF64 / 小端 / 版本 1
    struct.pack_into('<HHI', ehdr, 16, 2, 243, 1)   # e_type=ET_EXEC e_machine=EM_RISCV e_version
    struct.pack_into('<QQQ', ehdr, 24, 0x10000, 64, 0)   # e_entry=0x10000 e_phoff=64 e_shoff=0
    struct.pack_into('<I', ehdr, 48, 0)             # e_flags
    struct.pack_into('<HHHHHH', ehdr, 52, 64, 56, 1, 0, 0, 0)  # ehsize/phentsize/phnum=1/sh*
    ph = bytearray(56)
    # p_type=PT_LOAD p_flags=R+X p_offset=128 p_vaddr=p_paddr=0x10000
    # p_filesz=p_memsz=len(code) p_align=8
    struct.pack_into('<IIQQQQQQ', ph, 0, 1, 5, 128, 0x10000, 0x10000,
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
PY
