/* elf 模块：解析 ELF64 → 校验 → 把 PT_LOAD 段复制进 mem（M1-b）
   手工按小端逐字段解析，不依赖 <elf.h> 的结构体布局/宿主字节序假设。 */
#include "rvsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 小端读取（逐字节，C99 可移植） ---- */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24 |
           (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 | (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

/* Elf64_Phdr 手写布局（字节偏移:宽度）：p_type@0:4 p_flags@4:4 p_offset@8:8
   p_vaddr@16:8 p_paddr@24:8 p_filesz@32:8 p_memsz@40:8 p_align@48:8 */
#define ELF_PHDR_SIZE 56u
#define PT_LOAD   1u
#define PT_INTERP 3u
#define EM_RISCV  243u
#define ET_EXEC   2u
#define ET_DYN    3u
#define ELFCLASS64 2u

static uint64_t page_up(uint64_t x) { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }

int elf_load(rv_mem *m, const char *path, rv_elf *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -4;                       /* 文件不可读：归入截断/装载失败族 */
    long szl = -1;
    if (fseek(f, 0, SEEK_END) == 0) { szl = ftell(f); fseek(f, 0, SEEK_SET); }
    if (szl < 0) { fclose(f); return -4; }
    size_t sz = (size_t)szl;
    uint8_t *buf = malloc(sz ? sz : 1);
    if (!buf || fread(buf, 1, sz, f) != sz) { free(buf); fclose(f); return -4; }
    fclose(f);

    /* ---- 头部校验（按错误码序：-1 magic → -2 class → -3 machine → -4 截断） ---- */
    static const uint8_t MAGIC[4] = { 0x7F, 'E', 'L', 'F' };
    if (sz < 4 || memcmp(buf, MAGIC, 4) != 0) { free(buf); return -1; }
    if (sz < 5 || buf[4] != ELFCLASS64) { free(buf); return -2; }          /* EI_CLASS */
    if (sz < 6 || buf[5] != 1) { free(buf); return -2; }                   /* EI_DATA：仅小端 */
    if (sz < 20 || rd16(buf + 18) != EM_RISCV) { free(buf); return -3; }   /* e_machine */
    uint16_t e_type = rd16(buf + 16);
    /* 契约未给 e_type 独立细分码，归入 -3 头部字段不支持族（见任务报告） */
    if (e_type != ET_EXEC && e_type != ET_DYN) { free(buf); return -3; }
    if (sz < 64) { free(buf); return -4; }                                 /* Ehdr 不完整 */
    uint64_t phoff = rd64(buf + 32);
    uint16_t phentsize = rd16(buf + 54), phnum = rd16(buf + 56);
    if (phentsize != ELF_PHDR_SIZE || phoff > sz || phnum > (sz - phoff) / ELF_PHDR_SIZE) {
        free(buf); return -4;                                              /* 程序头表截断/布局不符 */
    }

    /* ---- 第一遍：全量校验（不动 mem，任何失败保证零副作用） ---- */
    uint64_t bias = (e_type == ET_DYN) ? ET_DYN_BIAS : 0;
    const uint8_t *base = buf + phoff;
    uint32_t nload = 0;
    uint64_t brk_max = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = base + (uint64_t)i * ELF_PHDR_SIZE;
        if (rd32(p) == PT_INTERP) { free(buf); return -6; }
        if (rd32(p) != PT_LOAD) continue;
        uint64_t off = rd64(p + 8), vaddr = rd64(p + 16);
        uint64_t filesz = rd64(p + 32), memsz = rd64(p + 40);
        if (filesz > memsz || off > sz || filesz > sz - off) { free(buf); return -4; }
        if (vaddr > UINT64_MAX - bias) { free(buf); return -4; }           /* va 计算溢出 */
        uint64_t va = vaddr + bias;
        if (va < MEM_BASE || memsz > MEM_SIZE || va - MEM_BASE > MEM_SIZE - memsz) {
            free(buf); return -4;                                          /* 段不落在 512MiB 窗口 */
        }
        nload++;
        if (va + memsz > brk_max) brk_max = va + memsz;
    }
    if (nload == 0) { free(buf); return -5; }

    /* 容量检查（仍在校验阶段，零副作用）：mem_add_region 无界，越 rgn[RVSIM_RGN_MAX]
       即越界写；且 mem 里可能已有调用方登记的区域，必须按剩余容量判断。
       超限按装载失败族返回 -4（契约见 rvsim.h），绝不进入第二遍登记。 */
    if (m->n > RVSIM_RGN_MAX || nload > RVSIM_RGN_MAX - m->n) { free(buf); return -4; }

    /* entry：4 对齐、在窗口内、且落在某 PT_LOAD 装载范围（含 bss）内 */
    uint64_t entry = rd64(buf + 24);
    int hit = 0;
    if ((entry & 3) == 0 && entry >= MEM_BASE && entry - MEM_BASE < MEM_SIZE) {
        for (uint16_t i = 0; i < phnum && !hit; i++) {
            const uint8_t *p = base + (uint64_t)i * ELF_PHDR_SIZE;
            if (rd32(p) != PT_LOAD) continue;
            uint64_t va = rd64(p + 16) + bias;
            if (entry >= va && entry < va + rd64(p + 40)) hit = 1;
        }
    }
    if (!hit) { free(buf); return -7; }

    /* ---- 第二遍：登记区域 + 复制。memsz>filesz 的 bss 零页由 mem_init 的 calloc
        天然保证，这里不做整块清零，以免破坏已写入数据。到达此处的 nload 已通过
        上方剩余容量检查，n 不会越过 rgn[RVSIM_RGN_MAX]。 ---- */
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = base + (uint64_t)i * ELF_PHDR_SIZE;
        if (rd32(p) != PT_LOAD) continue;
        uint64_t off = rd64(p + 8), va = rd64(p + 16) + bias;
        uint64_t filesz = rd64(p + 32), memsz = rd64(p + 40);
        mem_add_region(m, va, va + memsz, 0);
        /* 防御分支（实际不可达）：region 刚以 [va, va+memsz) 登记且 filesz<=memsz，
           必然覆盖写范围，mem_write 不可能失败；保留以防上方不变式日后被改动。 */
        if (filesz > 0 && mem_write(m, va, buf + off, filesz) != 0) { free(buf); return -4; }
    }
    out->entry = entry;
    out->brk_base = page_up(brk_max);
    out->bias = bias;
    free(buf);
    return 0;
}
