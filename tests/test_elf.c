#include "framework.h"
#include "rvsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* M7 分发 A：craft ET_DYN ELF 用的小端序列化助手（布局口径同 make_bad_elf.sh） */
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { int i; for (i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t *p, uint64_t v) { int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* 好 ELF 路径（tests/guest-bin/t_hello.elf，由 tests/build_guest.sh 产出）+
   坏 ELF 由 tests/make_bad_elf.sh 生成后以 argv 传入（argv[1]=路径 argv[2]=期望负错误码） */
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
    /* M7 分发 A（spec §8.3 ELF 项补勾验）：ET_DYN 双模式 + 多 PT_LOAD 成功
       装载 + bss(filesz<memsz) 清零——三者此前只被"失败路径"（bad_phnum/
       bad_rgn64 超容量拒绝）或单段 ET_EXEC 覆盖。现场 craft 最小 ET_DYN ELF：
       2 个 PT_LOAD（RX 代码段 4 字节含合法 insn + RW 数据段 filesz=8/
       memsz=16 → 尾部 8 字节 bss），写 build/et_dyn_tmp.elf（build/ 已
       gitignore）后装载断言。 */
    {
        uint8_t buf[0xc0];
        FILE *f;
        unsigned k, cnt0;
        uint64_t v = 0;
        memset(buf, 0, sizeof buf);
        memcpy(buf, "\x7f" "ELF", 4);
        buf[4] = 2; buf[5] = 1; buf[6] = 1;             /* class64 / LSB / EV_CURRENT */
        wr16(buf + 16, 3);                              /* e_type = ET_DYN */
        wr16(buf + 18, 243);                            /* e_machine = EM_RISCV */
        wr32(buf + 20, 1);                              /* e_version */
        wr64(buf + 24, 0x20000);                        /* e_entry（代码段内，4 对齐） */
        wr64(buf + 32, 64);                             /* e_phoff */
        wr16(buf + 52, 64);                             /* e_ehsize */
        wr16(buf + 54, 56);                             /* e_phentsize */
        wr16(buf + 56, 2);                              /* e_phnum = 2 */
        /* phdr[0]: PT_LOAD R+X，offset=0x80 vaddr=0x2000 filesz=memsz=4 */
        wr32(buf + 64 + 0, 1); wr32(buf + 64 + 4, 5);
        wr64(buf + 64 + 8, 0xb0); wr64(buf + 64 + 16, 0x20000);
        wr64(buf + 64 + 24, 0x20000); wr64(buf + 64 + 32, 4); wr64(buf + 64 + 40, 4);
        wr64(buf + 64 + 48, 0x1000);
        /* phdr[1]: PT_LOAD R+W，offset=0x90 vaddr=0x3000 filesz=8 memsz=16 */
        wr32(buf + 120 + 0, 1); wr32(buf + 120 + 4, 6);
        wr64(buf + 120 + 8, 0xb8); wr64(buf + 120 + 16, 0x30000);
        wr64(buf + 120 + 24, 0x30000); wr64(buf + 120 + 32, 8); wr64(buf + 120 + 40, 16);
        wr64(buf + 120 + 48, 0x1000);
        wr32(buf + 0xb0, 0x00000013u);                  /* nop：entry 处合法指令 */
        for (k = 0; k < 8; k++) buf[0xb8 + k] = 0xAA;   /* 数据段 filesz 内容 */
        f = fopen("build/et_dyn_tmp.elf", "wb");
        CHECK(f != NULL);
        CHECK(fwrite(buf, 1, sizeof buf, f) == sizeof buf);
        CHECK(fclose(f) == 0);
        mem_free(&m);
        CHECK(mem_init(&m) == 0);                       /* 干净 mem 上装载 */
        CHECK(elf_load(&m, "build/et_dyn_tmp.elf", &e) == 0);
        CHECK(e.bias == ET_DYN_BIAS);                   /* ET_DYN → bias 生效 */
        CHECK(e.entry == 0x20000 + ET_DYN_BIAS);        /* entry 加 bias */
        cnt0 = 0;
        for (k = 0; k < m.n; k++)
            if (m.rgn[k].kind == 0) cnt0++;
        CHECK(cnt0 == 2);                               /* 多 PT_LOAD：两段均登记 */
        CHECK(mem_read(&m, 0x30000 + ET_DYN_BIAS, &v, 8) == 0);
        CHECK(v == 0xAAAAAAAAAAAAAAAAULL);              /* filesz 内容按 offset 就位 */
        CHECK(mem_read(&m, 0x30008 + ET_DYN_BIAS, &v, 8) == 0);
        CHECK(v == 0);                                  /* bss(filesz<memsz) 清零 */
        CHECK(e.brk_base == 0x31000 + ET_DYN_BIAS);     /* brk_base=页对齐段尾（含 bias） */
    }
    /* 坏 ELF：argv[1]=路径 argv[2]=期望负错误码。
       修复轮 1（C-1）：坏 ELF 一律在干净 mem（n=0）上装载——沿用前置好 ELF
       装载后的 m（n>0）会掩盖"恰满容量"边界：64 段 PT_LOAD 必须在 n=0 时
       也判 -4（需为 main 的 heap/stack 预留 2 槽）。 */
    if (argc == 3) {
        int want = atoi(argv[2]);
        mem_free(&m);
        CHECK(mem_init(&m) == 0);
        CHECK(elf_load(&m, argv[1], &e) == want);
    }
    /* 修复轮 1（I-1）：文件打不开 → 细分码 -8（用法错误族，main 映射 200）。
       置于坏 ELF 块之后：避免本检查先行失败遮蔽上方逐项 L4 断言的独立红。 */
    CHECK(elf_load(&m, "tests/guest-bin/no-such-file.elf", &e) == -8);
    mem_free(&m);
    TEST_DONE();
}
