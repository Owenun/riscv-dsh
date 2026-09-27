#!/usr/bin/env bash
# L4 畸形 ELF 现场生成（产物进 build/，build/ 已 gitignore，不进版本库）
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
E=tests/guest-bin/t_hello.elf
head -c 100 "$E" > build/bad_trunc.elf                      # 期望 -4（程序头表被截断）
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
python3 - "$E" build/bad_phnum.elf <<'PY'                   # 70 个 PT_LOAD 段，期望 -4（超出 rgn[64] 剩余容量）
import sys
src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, 'rb').read())
phoff, phnum = 64, 70
d[56:58] = phnum.to_bytes(2, 'little')                      # e_phnum=70
need = phoff + phnum * 56                                   # 程序头表必须完整，否则解析在容量检查前先一步 -4
if len(d) < need: d.extend(b'\x00' * (need - len(d)))
tpl = bytearray(56)                                         # 合法零长 PT_LOAD：vaddr=MEM_BASE、filesz=memsz=0
tpl[0:4]   = (1).to_bytes(4, 'little')                      # p_type=PT_LOAD
tpl[4:8]   = (5).to_bytes(4, 'little')                      # p_flags=R+X
tpl[16:24] = (0x10000).to_bytes(8, 'little')                # p_vaddr=MEM_BASE
for i in range(3, phnum):                                   # 0..2 保留 t_hello 原段（entry 仍落段内）
    d[phoff+i*56 : phoff+(i+1)*56] = tpl
open(dst, 'wb').write(d)
PY
python3 - "$E" build/bad_entry.elf <<'PY'                   # e_entry=0，期望 -7
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); d[24:32] = (0).to_bytes(8, 'little')
open(sys.argv[2], 'wb').write(d)
PY
python3 - "$E" build/bad_noload.elf <<'PY'                  # e_phnum=0 → 无 PT_LOAD，期望 -5
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); d[56:58] = (0).to_bytes(2, 'little')
open(sys.argv[2], 'wb').write(d)
PY
