/* src/trace.c —— Task 12 分发 A（M6-a）：可观测性（spec §6.1/§7）。
   1. 逐指令 trace：行格式钉死
        `T <seq> pc=0x<16hex> insn=0x<8hex> <助记名>[ rd=xN=0x<16hex>]`
      seq 自 1 起、每条成功退休的指令递增；助记名 = rv_op 枚举名
      （rv_op_mnem 静态表，顺序与 rvsim.h 的 rv_op 枚举一字不差，编译期
      断言防漂移）；rd 字段仅写回型指令且 rd!=0 打印（store/分支/FENCE/
      FENCE.I/ECALL/EBREAK 无写回；值为退休后的 x[rd]，即本步写回值——
      JAL/JALR 为 pc+4，加载为装载值）。数据源：rv_step 填充的 rvsim 尾部
      字段 last_dec/last_pc（M6-a 接口裁决），trace 层不自算。
      写失败（磁盘满/IO 错）置 err，trace_close 上抛 → main 映射 208，不吞。
   2. --dump-regs：x0..x31 + pc 共 33 行，`xN  = 0x<16hex>`（名字列 5 字符：
      "x0  = "… "x31 = "… "pc  = "），小端十六进制小写。
   3. --dump-mem：每行 16 字节 = `地址两空格 分组1(8B 连续 hex) 两空格
      分组2 两空格 |ASCII|`；截断行 hex 空位以空格补齐、ASCII 只含实际
      字节；不可打印字节（<0x20 或 >=0x7f）→ '.'；读失败（未映射）→
      立即返回 -1（main → 208），已写行保留。
   4. rv_sc_mnem：-v 里程碑日志的 syscall 名（spec §5 子集），未知 → "?"。
   trace 流所有者：trace_open（路径文件）拥有并 fclose；trace_attach
   （测试 fmemopen/"-" 的 stdout）不拥有，只 fflush 校验错误。 */
#include "rvsim.h"
#include <string.h>

/* 助记名表：顺序与 include/rvsim.h 的 rv_op 枚举严格一致（编译期断言） */
static const char *const op_names[RV_OP_COUNT] = {
    "RV_LUI", "RV_AUIPC", "RV_JAL", "RV_JALR", "RV_BEQ", "RV_BNE",
    "RV_BLT", "RV_BGE", "RV_BLTU", "RV_BGEU",
    "RV_LB", "RV_LH", "RV_LW", "RV_LD", "RV_LBU", "RV_LHU", "RV_LWU",
    "RV_SB", "RV_SH", "RV_SW", "RV_SD",
    "RV_ADDI", "RV_SLTI", "RV_SLTIU", "RV_XORI", "RV_ORI", "RV_ANDI",
    "RV_SLLI", "RV_SRLI", "RV_SRAI",
    "RV_ADD", "RV_SUB", "RV_SLL", "RV_SLT", "RV_SLTU", "RV_XOR",
    "RV_SRL", "RV_SRA", "RV_OR", "RV_AND",
    "RV_ADDIW", "RV_SLLIW", "RV_SRLIW", "RV_SRAIW",
    "RV_ADDW", "RV_SUBW", "RV_SLLW", "RV_SRLW", "RV_SRAW",
    "RV_FENCE", "RV_FENCEI", "RV_ECALL", "RV_EBREAK",
};
typedef char op_names_count_check[
    (sizeof op_names / sizeof op_names[0]) == (size_t)RV_OP_COUNT ? 1 : -1];

const char *rv_op_mnem(rv_op op)
{
    return ((unsigned)op < (unsigned)RV_OP_COUNT) ? op_names[op] : "RV_?";
}

const char *rv_sc_mnem(uint64_t num)
{
    switch (num) {
    case 93:  return "exit";
    case 94:  return "exit_group";
    case 63:  return "read";
    case 64:  return "write";
    case 113: return "clock_gettime";
    case 160: return "uname";
    case 214: return "brk";
    case 215: return "munmap";
    case 222: return "mmap";
    default:  return "?";
    }
}

/* 写回型指令集合（rd 字段非编码残留）：store/分支/FENCE/FENCE.I/ECALL/
   EBREAK 之外全部写回（JAL/JALR 写 pc+4、加载写装载值、ALU 写结果）。 */
static bool op_writes_rd(rv_op op)
{
    switch (op) {
    case RV_SB: case RV_SH: case RV_SW: case RV_SD:
    case RV_BEQ: case RV_BNE: case RV_BLT: case RV_BGE:
    case RV_BLTU: case RV_BGEU:
    case RV_FENCE: case RV_FENCEI: case RV_ECALL: case RV_EBREAK:
        return false;
    default:
        return true;
    }
}

void trace_attach(rv_trace *t, FILE *f)
{
    t->f = f;
    t->seq = 0;
    t->err = 0;
    t->own = false;
}

int trace_open(rv_trace *t, const char *path)
{
    if (strcmp(path, "-") == 0) {
        trace_attach(t, stdout);
        return 0;
    }
    t->f = fopen(path, "w");
    if (t->f == NULL)
        return -1;
    t->seq = 0;
    t->err = 0;
    t->own = true;
    return 0;
}

void trace_step(rv_trace *t, const rvsim *s)
{
    const rv_dec *d;

    /* 终止型 ECALL/EBREAK 步：rv_step 返回 0（无故障）但指令未退休
       （pc/steps 未推进，spec §4.1），不产 trace 行——行数恒等于退休步数 */
    if (t->f == NULL || t->err != 0 || !s->has_dec || s->exited)
        return;
    d = &s->last_dec;
    t->seq++;
    if (fprintf(t->f, "T %llu pc=0x%016llx insn=0x%08x %s",
                (unsigned long long)t->seq,
                (unsigned long long)s->last_pc,
                (unsigned)d->raw, rv_op_mnem(d->op)) < 0) {
        t->err = 1;
        return;
    }
    if (op_writes_rd(d->op) && d->rd != 0
        && fprintf(t->f, " rd=x%u=0x%016llx", (unsigned)d->rd,
                   (unsigned long long)s->cpu.x[d->rd]) < 0) {
        t->err = 1;
        return;
    }
    if (fputc('\n', t->f) == EOF || ferror(t->f))
        t->err = 1;
}

int trace_close(rv_trace *t)
{
    FILE *f = t->f;
    int bad = t->err;

    if (f == NULL)
        return bad ? -1 : 0;
    t->f = NULL;
    if (t->own) {
        if (fclose(f) != 0)
            bad = 1;
    } else {
        if (fflush(f) != 0 || ferror(f))
            bad = 1;
    }
    return bad ? -1 : 0;
}

void rv_dump_regs(FILE *out, const rv_cpu *c)
{
    int i;
    for (i = 0; i < 32; i++)
        fprintf(out, "x%-2d = 0x%016llx\n", i, (unsigned long long)c->x[i]);
    fprintf(out, "pc  = 0x%016llx\n", (unsigned long long)c->pc);
}

int rv_dump_mem(FILE *out, rv_mem *m, uint64_t addr, uint64_t len)
{
    uint64_t off;
    for (off = 0; off < len; off += 16) {
        uint8_t buf[16];
        uint64_t chunk = (len - off >= 16) ? 16 : len - off;
        unsigned j;
        memset(buf, 0, sizeof buf);
        if (mem_read(m, addr + off, buf, chunk) != 0)
            return -1;                          /* 未映射：main → 208 */
        fprintf(out, "%016llx  ", (unsigned long long)(addr + off));
        for (j = 0; j < 16; j++) {
            if (j == 8)
                fputs("  ", out);               /* 组间两空格 */
            if (j < chunk)
                fprintf(out, "%02x", buf[j]);
            else
                fputs("  ", out);               /* 截断行 hex 空位补空格 */
        }
        fputs("  |", out);
        for (j = 0; j < chunk; j++) {
            uint8_t b = buf[j];
            fputc((b >= 0x20 && b < 0x7f) ? b : '.', out);
        }
        fputs("|\n", out);
    }
    return ferror(out) ? -1 : 0;
}

/* ---- 终止原因与 §7 诊断（Task 13 分发 B：main 与调试器共享）----
   rv_exit_reason：reason 字符串钉死（main -v 退出原因行与调试器 stopped
   行共用，报告快照备案）。
   rv_diag_stderr：按 s 状态打印恰一条 §7 stderr 诊断——exited 优先
   （205/206），其次故障（202 addr=/203 addr= align=N/204 word=0x…，-vv
   追加全部 GPR 现场），否则 code==207 打 step-limit 行；guest 正常 exit
   不打。main 主循环与 debugger 的 s/c 终止路径统一走本函数。 */
const char *rv_exit_reason(const rvsim *s, int code)
{
    if (s->exited) {
        if (s->sc_unsupported)
            return "unsupported-syscall";
        if (code == RVSIM_EX_EBREAK)
            return "ebreak";
        return "guest-exit";
    }
    switch (code) {
    case RVSIM_EX_UNMAPPED:   return "fault-unmapped";
    case RVSIM_EX_MISALIGNED: return "fault-misaligned";
    case RVSIM_EX_ILLEGAL:    return "fault-illegal";
    case RVSIM_EX_STEPLIMIT:  return "step-limit";
    default:                  return "stop";
    }
}

void rv_diag_stderr(const rvsim *s, int code, uint64_t max_steps, int verbose)
{
    bool fault = false;
    if (s->exited) {
        if (s->sc_unsupported)
            fprintf(stderr, "rvsim: unsupported syscall %d pc=0x%016llx\n",
                    s->unsupported_syscall_num, (unsigned long long)s->cpu.pc);
        else if (code == RVSIM_EX_EBREAK)
            fprintf(stderr, "rvsim: ebreak pc=0x%016llx\n",
                    (unsigned long long)s->cpu.pc);
        return;
    }
    switch (s->fault) {
    case RV_FV_UNMAPPED:                    /* 202 带 addr=0x…（台账 M-t） */
        fprintf(stderr, "rvsim: fault unmapped pc=0x%016llx addr=0x%016llx\n",
                (unsigned long long)s->cpu.pc,
                (unsigned long long)s->fault_addr);
        fault = true;
        break;
    case RV_FV_MISALIGNED:                  /* 203 带 addr=0x… align=N */
        fprintf(stderr, "rvsim: fault misaligned pc=0x%016llx addr=0x%016llx align=%u\n",
                (unsigned long long)s->cpu.pc,
                (unsigned long long)s->fault_addr,
                (unsigned)s->fault_align);
        fault = true;
        break;
    case RV_FV_ILLEGAL:                     /* 204 带 word=0x… */
        fprintf(stderr, "rvsim: illegal insn pc=0x%016llx word=0x%08x\n",
                (unsigned long long)s->cpu.pc, (unsigned)s->fault_insn);
        fault = true;
        break;
    default:
        if (code == RVSIM_EX_STEPLIMIT)     /* 207（fault==NONE 时才打） */
            fprintf(stderr, "rvsim: step-limit %llu pc=0x%016llx\n",
                    (unsigned long long)max_steps,
                    (unsigned long long)s->cpu.pc);
        break;
    }
    if (fault && verbose >= 2) {            /* spec §7：-vv 故障现场全部 GPR */
        fprintf(stderr, "rvsim: fault GPR dump:\n");
        rv_dump_regs(stderr, &s->cpu);
    }
}
