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

/* ---- decode 模块（Task 5）---- */
typedef enum { RV_LUI, RV_AUIPC, RV_JAL, RV_JALR, RV_BEQ, RV_BNE, RV_BLT, RV_BGE,
  RV_BLTU, RV_BGEU, RV_LB, RV_LH, RV_LW, RV_LD, RV_LBU, RV_LHU, RV_LWU,
  RV_SB, RV_SH, RV_SW, RV_SD, RV_ADDI, RV_SLTI, RV_SLTIU, RV_XORI, RV_ORI,
  RV_ANDI, RV_SLLI, RV_SRLI, RV_SRAI, RV_ADD, RV_SUB, RV_SLL, RV_SLT, RV_SLTU,
  RV_XOR, RV_SRL, RV_SRA, RV_OR, RV_AND, RV_ADDIW, RV_SLLIW, RV_SRLIW, RV_SRAIW,
  RV_ADDW, RV_SUBW, RV_SLLW, RV_SRLW, RV_SRAW, RV_FENCE, RV_FENCEI,
  RV_ECALL, RV_EBREAK, RV_OP_COUNT } rv_op;
/* rv_dec 非相关字段（如 U/J 型的 rs1/rs2、R 型的 imm）为编码残留位：
   rv_decode 仅保证与该指令格式相关字段有效，其余字段值未定义，
   消费方（cpu 执行层）不得依赖。 */
typedef struct { rv_op op; uint8_t rd, rs1, rs2; uint32_t raw; int64_t imm; } rv_dec;
bool rv_decode(uint32_t insn, rv_dec *d);   /* false = 非法/保留编码 */

/* ---- cpu 执行模块（Task 6）---- */
typedef struct { uint64_t x[32], pc; uint64_t steps; } rv_cpu;
/* 取指/执行故障码：rv_step 失败时写入 s->fault 并作为返回值；0=本步成功退休。 */
enum { RV_FV_NONE = 0, RV_FV_UNMAPPED = 1, RV_FV_MISALIGNED = 2, RV_FV_ILLEGAL = 3 };
typedef struct {
    rv_mem mem; rv_cpu cpu;
    int fault;                  /* RV_FV_*；rv_step 返回前设置 */
    bool exited; int exit_code; /* guest 正常退出/EBREAK 等（Task 10 接入，本任务不触发） */
} rvsim;
/* 结构体布局说明：本任务按接口契约定稿（mem/cpu/fault/exited/exit_code）；
   后续任务（Task 10 等）只允许在结构体尾部追加字段，不得插入或重排现有成员。 */
void rvsim_reset(rvsim *s);    /* 清零 cpu/fault/exited；mem 由调用方 init */
int  rv_step(rvsim *s);        /* 取指+解码+执行一条；返回 s->fault（0=成功退休）。
                                  故障路径无部分副作用：不写回、不推进 pc、不计数。 */
#endif
