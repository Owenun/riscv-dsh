/* src/debugger.c —— Task 13 分发 B（M6-b）：交互调试器完整版（spec §6.2）。
   装载后停在 entry 前（main 在 pc=entry 后调用；-s 下 main 主循环不运行，
   本模块接管全部执行）。提示符 rvsim> 逐行读 stdin，命令表（§6.2）：
     s [N]   单步执行 N 条（默认 1），每步打印一行 --trace 格式步进行
             （`T <seq> pc=… insn=… 助记名[ rd=…]`，复用 trace_step；
             seq = 含本步的全局退休步数）；单步不做断点判定（显式推进）。
     c       连续执行至断点/停机；pc 恰在断点上：若非刚命中挂起 → 立即
             命中打印当前指令回提示符；若刚命中挂起（bp_pause）→ 单步跨过
             命中点一次后继续（gdb 语义，保证命中后 c 能继续）。
     r       全部 GPR+pc（rv_dump_regs，同 --dump-regs 格式）。
     m A L   内存 hexdump（rv_dump_mem，同 --dump-mem 格式；未映射报错）。
     b A     设断点（最多 8 个；重复/超限均报错不崩溃，成功打印槽位确认）。
     d A     删除断点（不存在报错）。
     q       立即退出：未终止 → 0；已终止 → 终止码透传（guest 退出码/205/
             206/202..204/207）。
     h       命令帮助。
   终止（guest exit/EBREAK/205/故障/207）在 s/c 中发生：rv_diag_stderr 打
   §7 stderr 诊断 + stdout 打 `rvsim: stopped <原因> code=N pc=…` 一行后回
   提示符；此后 s/c 为空操作，r/m 仍可观测终态。
   stdin EOF → 视为 q。数字参数 strtoull base 0（0x 前缀/十进制皆可），
   拒绝负号与尾随垃圾；未知命令回显 unknown command。guest write 直写宿主
   fd1，故每次 rv_step 前 fflush(stdout) 保证与管道快照的输出时序确定。 */
#include "rvsim.h"
#include <stdlib.h>
#include <string.h>

#define DBG_BP_MAX 8u

typedef struct {
    rv_trace t;             /* s 命令步进行：复用 --trace 行格式（stdout） */
    uint64_t bp[DBG_BP_MAX];
    unsigned nbp;
    uint64_t max_steps;
    int verbose;
    bool stopped;           /* guest 终止后置位：s/c 转空操作 */
    int stop_code;
    bool bp_pause;          /* 刚因断点命中挂起（pc==pause_pc）：c 跨过一次 */
    uint64_t pause_pc;
} dbg;

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* 数字参数：strtoull base 0；拒绝空串/负号/尾随非空白（严格解析，确定性） */
static bool parse_u64(char **pp, uint64_t *out)
{
    const char *p = skip_ws(*pp);
    char *end;
    unsigned long long v;

    if (*p == '\0' || *p == '-' || *p == '+')
        return false;
    v = strtoull(p, &end, 0);
    if (end == p || (*end != '\0' && *end != ' ' && *end != '\t'))
        return false;
    *pp = end;
    *out = (uint64_t)v;
    return true;
}

static bool args_end(const char *p)
{
    return *skip_ws((char *)p) == '\0';
}

static bool bp_hit(const dbg *g, uint64_t pc)
{
    unsigned i;
    for (i = 0; i < g->nbp; i++)
        if (g->bp[i] == pc)
            return true;
    return false;
}

/* 故障码 → §7 退出码映射（与 main 主循环一致） */
static int fault_exit_code(const rvsim *s)
{
    switch (s->fault) {
    case RV_FV_UNMAPPED:   return RVSIM_EX_UNMAPPED;
    case RV_FV_MISALIGNED: return RVSIM_EX_MISALIGNED;
    default:               return RVSIM_EX_ILLEGAL;
    }
}

/* 终止处理：§7 stderr 诊断 + stdout 终止原因行，回提示符（s/c 后续空操作） */
static void dbg_stop(dbg *g, rvsim *s, int code)
{
    rv_diag_stderr(s, code, g->max_steps, g->verbose);
    printf("rvsim: stopped %s code=%d pc=0x%016llx\n",
           rv_exit_reason(s, code), code, (unsigned long long)s->cpu.pc);
    g->stopped = true;
    g->stop_code = code;
}

/* 断点命中行：打印当前（未退休）指令的 pc/指令字/助记名（spec §6.2） */
static void dbg_print_hit(rvsim *s, uint64_t pc)
{
    uint32_t insn = 0;
    rv_dec d;
    const char *mn = "?";

    if (mem_read(&s->mem, pc, &insn, 4) == 0 && rv_decode(insn, &d))
        mn = rv_op_mnem(d.op);
    printf("breakpoint hit: pc=0x%016llx insn=0x%08x %s\n",
           (unsigned long long)pc, (unsigned)insn, mn);
}

/* s [N]：单步 N 条，每步一行 trace 格式；终止（故障/退出/207）即挂起 */
static void do_step(dbg *g, rvsim *s, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n && !g->stopped; i++) {
        if (s->cpu.steps >= g->max_steps) {
            dbg_stop(g, s, RVSIM_EX_STEPLIMIT);     /* max-steps 约束同主循环 */
            return;
        }
        fflush(stdout);                     /* guest write 直写 fd1，先冲本层 */
        if (rv_step(s) != 0) {
            dbg_stop(g, s, fault_exit_code(s));
            return;
        }
        g->t.seq = s->cpu.steps - 1;        /* 行号 = 含本步的全局退休步数（退休后置） */
        trace_step(&g->t, s);               /* 终止型 ECALL/EBREAK 不产行 */
        fflush(stdout);
        if (s->exited) {
            dbg_stop(g, s, s->exit_code);
            return;
        }
    }
}

/* c：连续执行至断点/停机；刚命中挂起时先单步跨过命中点一次 */
static void do_continue(dbg *g, rvsim *s)
{
    if (g->stopped)
        return;
    for (;;) {
        if (s->cpu.steps >= g->max_steps) {
            dbg_stop(g, s, RVSIM_EX_STEPLIMIT);
            return;
        }
        if (g->bp_pause && s->cpu.pc == g->pause_pc)
            g->bp_pause = false;            /* 跨过上次命中点（gdb 语义） */
        else if (bp_hit(g, s->cpu.pc)) {
            g->bp_pause = true;
            g->pause_pc = s->cpu.pc;
            dbg_print_hit(s, s->cpu.pc);
            fflush(stdout);
            return;                         /* 挂起回提示符（spec §6.2） */
        }
        fflush(stdout);
        if (rv_step(s) != 0) {
            dbg_stop(g, s, fault_exit_code(s));
            return;
        }
        if (s->exited) {
            dbg_stop(g, s, s->exit_code);
            return;
        }
    }
}

int debugger_run(rvsim *s, uint64_t max_steps, int verbose, int *out_code)
{
    dbg g;
    char line[256];

    memset(&g, 0, sizeof g);
    trace_attach(&g.t, stdout);             /* s 步进行复用 --trace 格式 */
    g.max_steps = max_steps;
    g.verbose = verbose;

    /* 就绪信息：装载后停在 entry 前（spec §6.1） */
    printf("rvsim debugger: loaded, stopped before entry pc=0x%016llx\n",
           (unsigned long long)s->cpu.pc);
    for (;;) {
        char *p, *tok, *args;

        fputs("rvsim> ", stdout);
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) {  /* stdin EOF → 视为 q */
            *out_code = g.stopped ? g.stop_code : 0;
            return 1;
        }
        if (strchr(line, '\n') == NULL && !feof(stdin)) {
            /* M7 分发 A（评审轻微 3）：超长行（>= sizeof line-1 字符且其后
               尚有输入）确定性处理——整行拒绝并循环消费残留，残留不得被
               fgets 拆成后续命令逐段回显 unknown command（dbg_session7
               回归锁）。恰好填满缓冲且紧跟 EOF 时不误判（feof 已置位，
               按普通命令处理）。 */
            int ch;
            while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
            }
            fputs("error: line too long\n", stdout);
            fflush(stdout);
            continue;
        }
        line[strcspn(line, "\n")] = '\0';
        p = skip_ws(line);
        if (*p == '\0')
            continue;                       /* 空行：重打提示符 */
        tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        args = (*p == '\0') ? p : p + 1;    /* 后续 parse_u64 自会跳过空白 */
        *p = '\0';                          /* 截断命令字 */

        if (strcmp(tok, "q") == 0 || strcmp(tok, "quit") == 0) {
            *out_code = g.stopped ? g.stop_code : 0;    /* 终止码透传（申报） */
            return 1;                       /* 立即退出（spec §6.2） */
        } else if (strcmp(tok, "h") == 0 || strcmp(tok, "help") == 0) {
            fputs("commands: s [N] | c | r | m A L | b A | d A | q | h\n",
                  stdout);
        } else if (strcmp(tok, "s") == 0) {
            uint64_t n = 1;
            bool ok = args_end(args);
            if (!ok)
                ok = parse_u64(&args, &n) && args_end(args);
            if (ok) {
                g.bp_pause = false;         /* 单步推进后命中挂起语义复位 */
                do_step(&g, s, n);
            } else {
                fputs("error: usage s [N]\n", stdout);
            }
        } else if (strcmp(tok, "c") == 0) {
            if (!args_end(args))
                fputs("error: usage c\n", stdout);
            else
                do_continue(&g, s);
        } else if (strcmp(tok, "r") == 0) {
            if (!args_end(args))
                fputs("error: usage r\n", stdout);
            else
                rv_dump_regs(stdout, &s->cpu);
        } else if (strcmp(tok, "m") == 0) {
            uint64_t a, l;
            if (parse_u64(&args, &a) && parse_u64(&args, &l) && args_end(args)) {
                if (rv_dump_mem(stdout, &s->mem, a, l) != 0)
                    printf("error: unreadable memory at 0x%016llx\n",
                           (unsigned long long)a);
            } else {
                fputs("error: usage m A L\n", stdout);
            }
        } else if (strcmp(tok, "b") == 0) {
            uint64_t a;
            if (parse_u64(&args, &a) && args_end(args)) {
                unsigned i;
                bool dup = false;
                for (i = 0; i < g.nbp; i++)
                    if (g.bp[i] == a)
                        dup = true;
                if (dup)
                    printf("error: breakpoint 0x%016llx already set\n",
                           (unsigned long long)a);
                else if (g.nbp >= DBG_BP_MAX)
                    fputs("error: breakpoint table full (max 8)\n", stdout);
                else {
                    g.bp[g.nbp++] = a;
                    printf("breakpoint %u set at 0x%016llx\n",
                           g.nbp, (unsigned long long)a);
                }
            } else {
                fputs("error: usage b A\n", stdout);
            }
        } else if (strcmp(tok, "d") == 0) {
            uint64_t a;
            if (parse_u64(&args, &a) && args_end(args)) {
                unsigned i;
                bool found = false;
                for (i = 0; i < g.nbp; i++)
                    if (g.bp[i] == a) {
                        found = true;
                        break;
                    }
                if (!found)
                    printf("error: no breakpoint at 0x%016llx\n",
                           (unsigned long long)a);
                else {
                    memmove(&g.bp[i], &g.bp[i + 1],
                            (g.nbp - i - 1) * sizeof g.bp[0]);
                    g.nbp--;
                    /* M7 分发 A（评审轻微 2）：删去的若正是刚命中挂起的断点，
                       连带清除 bp_pause——否则挂起态残留，后续 c 会把"单步
                       跨过命中点"用在已不存在的断点上，同址重设后的首次
                       命中被静默跳过（dbg_session6 回归锁）。 */
                    if (g.bp_pause && g.pause_pc == a)
                        g.bp_pause = false;
                    printf("breakpoint at 0x%016llx removed\n",
                           (unsigned long long)a);
                }
            } else {
                fputs("error: usage d A\n", stdout);
            }
        } else {
            printf("unknown command: %s\n", tok);
        }
        fflush(stdout);
    }
}
