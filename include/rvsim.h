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

/* ---- mem 模块 ---- */
typedef struct { uint64_t lo, hi; int kind; } rv_region;   /* [lo,hi)，kind: 0=ELF 1=heap 2=mmap 3=stack */
#define RVSIM_RGN_MAX 64u
typedef struct { uint8_t *data; rv_region rgn[RVSIM_RGN_MAX]; uint32_t n; } rv_mem;
/* rgn 容量上限 RVSIM_RGN_MAX=64 的前提：区域按登记次数计项（ELF 各 PT_LOAD 段、
   heap、mmap 每块、stack 各占一项），正常 guest 用量远小于 64。mem_add_region 本身
   不做静默丢弃也无界——调用层必须先校验剩余容量（elf_load 检查 nload，Task 10 的
   mmap 层检查区域数），超限在调用层拒绝，绝不可越过 rgn[RVSIM_RGN_MAX] 写入。 */
int  mem_init(rv_mem *m);                                   /* 0=ok，-1=OOM(将来映射退出码 208) */
void mem_free(rv_mem *m);
void mem_add_region(rv_mem *m, uint64_t lo, uint64_t hi, int kind);
/* len=0 语义（钉死，勿改行为）：len=0 视作空区间，仅要求地址在窗口内且落在某
   区域，等价于要求 lo<=a<=hi——现有实现允许空区间落在区域右端点 hi 上。 */
int  mem_check(rv_mem *m, uint64_t addr, uint64_t len);     /* 0=ok，-1=未映射/出窗 */
int  mem_read (rv_mem *m, uint64_t a, void *dst, uint64_t n);
int  mem_write(rv_mem *m, uint64_t a, const void *src, uint64_t n);

/* ---- elf 模块 ---- */
typedef struct { uint64_t entry, brk_base, bias; } rv_elf;
/* elf_load 返回 0=ok；负数=错误细分：-1 magic -2 class -3 machine -4 截断/装载失败
   （含容量超限：PT_LOAD 段数 > RVSIM_RGN_MAX - m->n，在 mem_add_region 之前返回）
   -5 无 PT_LOAD -6 PT_INTERP -7 entry 越界（全部映射退出码 201） */
int elf_load(rv_mem *m, const char *path, rv_elf *out);
#endif
