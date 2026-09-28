#ifndef RVSIM_H
#define RVSIM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

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

#define RVSIM_DEFAULT_MAX_STEPS 100000000ULL   /* --max-steps 默认值（spec §6.1） */

/* ---- mem 模块 ---- */
typedef struct { uint64_t lo, hi; int kind; } rv_region;   /* [lo,hi)，kind: 0=ELF 1=heap 2=mmap 3=stack */
#define RVSIM_RGN_MAX 64u
typedef struct { uint8_t *data; rv_region rgn[RVSIM_RGN_MAX]; uint32_t n; } rv_mem;
/* rgn 容量上限 RVSIM_RGN_MAX=64 的前提：区域按登记次数计项（ELF 各 PT_LOAD 段、
   heap、mmap 每块、stack 各占一项），正常 guest 用量远小于 64。mem_add_region 本身
   不做静默丢弃也无界——调用层必须先校验剩余容量（elf_load 检查 nload+2，其中 2 槽
   为 main 装配恒追加的 heap/stack 预留，修复轮 1/C-1；Task 10 的 mmap 层检查区域数），
   超限在调用层拒绝，绝不可越过 rgn[RVSIM_RGN_MAX] 写入。 */
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
   （含容量超限：PT_LOAD 段数 + 2（为 main 的 heap/stack 登记预留）> RVSIM_RGN_MAX - m->n，
   在 mem_add_region 之前返回） -5 无 PT_LOAD -6 PT_INTERP -7 entry 越界
   -8 文件打不开（修复轮 1/I-1：用法错误族，main 映射退出码 200；其余细分映射 201） */
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
    /* ---- 以下为 Task 10（M5）尾部追加字段（不得插入/重排上方成员） ---- */
    uint64_t brk_base, brk_cur; /* heap 区间 [brk_base, brk_cur)，cap HEAP_CAP=32MiB；
                                   rvsim_reset 置 0，正式赋值在 main 装配
                                   （elf_load 返回 brk 基址后登记 heap 区，分发 B） */
    uint64_t mmap_top;          /* 下一块 mmap 起始（MMAP_BASE 起 bump 向上） */
    int host_fd[3];             /* guest fd 0/1/2 → 宿主 fd，默认 {0,1,2} */
    bool sc_unsupported;        /* ECALL 命中不支持号码（exit_code=205）时置位 */
    int unsupported_syscall_num;/* 上述号码，供 main/trace 打印诊断（cpu 不依赖 stdio） */
    /* ---- 以下为 Task 12 分发 A（M6-a）尾部追加字段（不得插入/重排上方成员）----
       last_dec/has_dec/last_pc：rv_step 解码成功即填（last_pc=本步起始 pc、
       last_dec=本步解码结果）；仅当 rv_step 返回 0（指令正常退休）时 trace 层
       消费——终止型 ECALL/EBREAK 与各故障路径不退休，main 不调 trace_step。
       fault_addr/fault_insn/fault_align：各故障点填充（M6-a 接口裁决字段）；
       取指 pc 未对齐/未映射取指 fault_insn=0（指令字未知）；fault_align 仅
       MISALIGNED 有效：4=取指/跳转目标，2/4/8=访存宽度。
       has_syscall/sc_kind/sc_num/sc_args/sc_ret：ECALL 分发快照（kind 语义同
       rv_sc_ret），-v 里程碑日志（syscall 名/参/返回、brk/mmap/munmap 变化）
       由 main 读取——cpu/syscall 层不依赖 stdio（接口裁决四字段之外，为本
       需求尾部追加，报告已申报）。 */
    rv_dec last_dec; bool has_dec; uint64_t last_pc;
    uint64_t fault_addr; uint32_t fault_insn; uint32_t fault_align;
    bool has_syscall; int sc_kind;
    uint64_t sc_num; uint64_t sc_args[6]; int64_t sc_ret;
} rvsim;
/* 结构体布局说明：本任务按接口契约定稿（mem/cpu/fault/exited/exit_code）；
   后续任务只允许在结构体尾部追加字段，不得插入或重排现有成员。 */
void rvsim_reset(rvsim *s);    /* 清零 cpu/fault/exited/M6-a 尾部字段；mem 由调用方 init */
int  rv_step(rvsim *s);        /* 取指+解码+执行一条；返回 s->fault（0=成功退休）。
                                故障路径无部分副作用：不写回、不推进 pc、不计数。 */

/* 进程栈初始化（Task 11/M5 分发 B，spec §3.4 精确布局）：自 STACK_TOP 向下
   构造 AT_RANDOM(0x42*16)/argv 字符串区/argc/argv+NULL/envp NULL/auxv
   {AT_PAGESZ,AT_RANDOM,AT_NULL}，sp 16 对齐并写入 x2；gargs[0] 为 guest
   argv[0]（ELF 路径本身）。返回 0=ok，-1=mem 写失败（调用方映射退出码 208）。
   须在 rvsim_reset 之后、设置 pc=entry 之前调用（a0/a1/a2 保持 reset 的 0）。 */
int build_stack(rvsim *s, int argc, char **gargs);

/* ---- syscall 模块（Task 10/M5）---- */
/* 语义权威：docs/specs/2026-09-27-rvsim-design.md §5。
   kind: 0=返回值（val 写回 guest a0，负数即 -errno，Linux 失败语义）
         1=guest exit（val = 退出码，cpu 侧置 exited/exit_code，不推进 pc）
         2=不支持号码（cpu 侧置 exited/exit_code=205 并记录号码，诊断由
           main/trace 层打印） */
typedef struct { int kind; int64_t val; } rv_sc_ret;
rv_sc_ret rv_syscall(rvsim *s, uint64_t num, uint64_t a0, uint64_t a1,
                     uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

/* ---- CLI（Task 12 分发 A，spec §6.1）----
   rvsim [OPTIONS] ELF [GUEST_ARGS...]：ELF 路径 = 第一个非选项参数，其后全部
   为 guest-args（即使形如 -v）；-v=1/-vv=2 里程碑日志；--trace FILE（"-"=
   stdout）；--dump-regs；--dump-mem A:L；--max-steps N（默认 1e8，超限 207）；
   -s 调试器；-h 用法说明 → exit 0。未知选项/缺参/坏值 → usage 200（诊断已
   打印到 stderr）。 */
typedef struct {
    bool help;                  /* -h：main 打印 usage(stdout) 后退出 0 */
    int verbose;                /* 0 无；1=-v；2=-vv */
    const char *trace_path;     /* NULL=关闭；"-"=stdout */
    bool dump_regs;
    bool dump_mem; uint64_t dm_addr, dm_len;   /* --dump-mem A:L */
    uint64_t max_steps;         /* 默认 RVSIM_DEFAULT_MAX_STEPS */
    bool dbg;                   /* -s */
    const char *elf;            /* ELF 路径（第一个非选项参数） */
    char **gargs; int gargc;    /* guest-args（gargs[0]=ELF 路径，spec §3.4） */
} rv_cli;
int  cli_parse(int argc, char **argv, rv_cli *o);  /* 0=ok；非 0=退出码 200（已打印诊断） */
void cli_print_usage(FILE *out);

/* ---- 可观测性（Task 12 分发 A，spec §6.1/§7）---- */
typedef struct { FILE *f; uint64_t seq; int err; bool own; } rv_trace;
/* trace 行：`T <seq> pc=0x<16hex> insn=0x<8hex> <助记名>[ rd=xN=0x<16hex>]`，
   seq 自 1 起，每条成功退休的指令一行；助记名 = rv_op 枚举名；rd 字段仅
   写回型指令且 rd!=0 打印（值为退休后 x[rd]，即本步写回值）。 */
void trace_attach(rv_trace *t, FILE *f);        /* 测试/宿主流注入 */
int  trace_open(rv_trace *t, const char *path); /* 0=ok；-1=打开失败（main→208） */
void trace_step(rv_trace *t, const rvsim *s);   /* rv_step 返回 0 后调用一次；
                                终止型 ECALL/EBREAK 步（exited=true）不产行，
                                行数恒等于退休步数（s.cpu.steps） */
int  trace_close(rv_trace *t);                  /* 0=ok；-1=写/关失败（main→208） */
const char *rv_op_mnem(rv_op op);               /* rv_op 枚举名；越界 → "RV_?" */
const char *rv_sc_mnem(uint64_t num);           /* -v 用的 syscall 名（子集） */
/* --dump-regs：x0..x31 + pc 共 33 行，`xN  = 0x<16hex>`（名字列 5 字符宽）。
   --dump-mem：每行 16 字节 = `地址  8字节hex  8字节hex  |ASCII|`（组内 2 位
   十六进制连续、组间/尾前两空格；截断行空位补空格；ASCII 不可打印 → '.'）。
   读失败（未映射）→ 停止并返回 -1（main→208），已写行保留。 */
void rv_dump_regs(FILE *out, const rv_cpu *c);
int  rv_dump_mem(FILE *out, rv_mem *m, uint64_t addr, uint64_t len);

/* 终止原因名与 §7 stderr 诊断（Task 13 分发 B：main -v 行与调试器 stopped
   行共享）：rv_exit_reason ∈ {guest-exit, ebreak, unsupported-syscall,
   fault-unmapped, fault-misaligned, fault-illegal, step-limit, stop}；
   rv_diag_stderr 按状态打印恰一条诊断（205/206/202/203/204/207；guest
   正常 exit 不打；202..204 后 -vv 追加全部 GPR 现场）。 */
const char *rv_exit_reason(const rvsim *s, int code);
void rv_diag_stderr(const rvsim *s, int code, uint64_t max_steps, int verbose);

/* 调试器（-s，spec §6.2，Task 13 分发 B 完整版）：装载后停在 entry 前，
   提示符 rvsim> 逐行读 stdin，命令 s [N]/c/r/m A L/b A/d A/q/h（§6.2 表）。
   断点最多 8 个（重复/超限报错）；断点命中（pc==A）打印当前指令挂起回
   提示符，再次 c 单步跨过命中点继续；guest exit/EBREAK/205/故障/207 在
   s/c 中发生 → §7 诊断（stderr）+ 终止原因行（stdout）后回提示符；
   stdin EOF → 视为 q。调试器接管全部执行（-s 下 main 主循环不运行）。
   返回 1 = 会话结束（q/EOF），*out_code = 退出码（未终止 0；已终止=
   guest 退出码/205/206/202..204/207）。 */
int debugger_run(rvsim *s, uint64_t max_steps, int verbose, int *out_code);
#endif
