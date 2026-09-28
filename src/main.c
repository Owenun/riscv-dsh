/* src/main.c —— Task 12 分发 A（M6-a）：main 装配 + CLI 完整接线（spec
   §2.1/§3.4/§6.1/§7）。
   装配顺序：cli_parse（未知选项/缺参/坏值 → 200，诊断由 cli 打印；-h →
   usage(stdout) + exit 0）→ mem_init → rvsim_reset（host_fd={0,1,2}）→
   elf_load（-8 → 200，其余细分 → 201）→ heap/stack 区登记 → build_stack
   （sp 写 x2）→ pc=entry →（-s → debugger_run 接管会话：交互至 q/EOF，
   退出码未终止 0/已终止透传；主循环不运行）→ 主循环 → trace 关闭 →
   停机 dump → mem_free。
   主循环（spec §4.3）：steps 上限 = --max-steps（默认 1e8，spec §6.1），
   超限 → 207；每步 rv_step 返回 0（成功退休）→ trace_step（--trace）+
   -v syscall 里程碑日志（ECALL 快照尾部字段，含 brk 变化/mmap/munmap 细节）；
   s.exited → s.exit_code；故障 → §7 诊断升级（台账 M-t）：202 带 addr=0x…、
   203 带 addr=0x… align=N、204 带 word=0x…（字段由 cpu 各故障点填入尾部
   fault_addr/fault_insn/fault_align），-vv 另打全部 GPR 现场（spec §7）。
   -v/-vv 里程碑日志（spec §6.1：始终 stderr，不污染 guest stdout）：段加载
   （每 PT_LOAD lo/hi，kind==0 区域即 elf_load 逐段登记）、退出原因；-vv 另
   打区域映射细节（kind 标注）与栈初始化布局（区域/sp/argc）。guest 终止型
   ECALL（exit/exit_group）也记一行 syscall 日志后随循环终止退出。
   停机 dump（spec §6.1）：--dump-regs（x0..x31+pc）、--dump-mem A:L（hexdump）
   停机时向 stdout 打印；dump 读到未映射区间视为宿主内部错误 → 208（申报）。
   trace 契约：--trace 文件打开失败或写失败 → 208，不吞（spec §7 尾注）；
   FILE 为 "-" 时走 stdout 且不 fclose（trace 层所有权约定）。修复轮 1
   （重要 2）：FILE 为 "-" 时主循环按调试器同款 flush 纪律（每步 rv_step
   前与 trace_step 后各一次 fflush(stdout)）——guest write 直写 fd1 绕过
   stdio 缓冲，不冲缓冲会把 guest 输出嵌进 stdio 缓冲的 trace 块中间
   （golden snap_trace 第 120 行劈开行实证）。
   ELF 镜像区登记：elf_load（M1 契约）已把每个 PT_LOAD 段按实际装载范围
   [va, va+memsz) 逐段登记 kind=0——段间页对齐空隙保持未登记，野访问按 202
   拒绝（spec §3.1）；main 不再重复登记宽区间。heap 区：[brk_base, brk_base)
   初始空登记（kind=1），brk syscall 扩/缩 hi（Task 10 分发 A 契约）。stack
   区：[STACK_TOP-STACK_SIZE, STACK_TOP) 8 MiB 预映射（spec §3.2）。
   容量衔接（修复轮 1/C-1）：本函数在 elf_load 成功后恒追加恰 2 项登记
   （heap+stack），剩余容量由 elf_load 的 nload+2 预留检查统一保证。
   终止型诊断（cpu/syscall 层不依赖 stdio）：统一由 trace.c 的
   rv_diag_stderr 打印（Task 13 分发 B 起 main 与调试器共享）：
   - 205：s.sc_unsupported 时 `rvsim: unsupported syscall N pc=0x…`（§5/§7）；
   - 206：exit_code==206 时 `rvsim: ebreak pc=0x…`——guest 故意 exit(206) 会
     撞号误打此行，属 spec §7 已声明的退出码撞号歧义（cpu 未区分来源，
     M6 可加标志位；现无 guest 使用 206）。 */
#include <stdio.h>
#include <string.h>
#include "rvsim.h"

/* -v：每步 ECALL 的里程碑日志（名/参/返回；brk/mmap/munmap 追加状态变化行）。
   数据源：cpu ECALL 臂填的快照尾部字段（sc_kind 语义同 rv_sc_ret.kind）。
   brk 行（修复轮 1/重要 3）：打 "调用前旧 brk -> 调用后 brk_cur"——旧实现
   打 sc_ret（成功时恰等于新 brk）恒等号无信息；brk_old 由主循环在 rv_step
   前快照 s.brk_cur 传入，失败/查询时 brk_cur 不变 → old==new 仍可读。 */
static void vlog_syscall(const rvsim *s, uint64_t brk_old)
{
    fprintf(stderr, "rvsim: syscall %llu %s(%llu, %llu, %llu, %llu, %llu, %llu)",
            (unsigned long long)s->sc_num, rv_sc_mnem(s->sc_num),
            (unsigned long long)s->sc_args[0], (unsigned long long)s->sc_args[1],
            (unsigned long long)s->sc_args[2], (unsigned long long)s->sc_args[3],
            (unsigned long long)s->sc_args[4], (unsigned long long)s->sc_args[5]);
    if (s->sc_kind == 0)
        fprintf(stderr, " = %lld", (long long)s->sc_ret);
    else if (s->sc_kind == 2)
        fprintf(stderr, " = unsupported");
    fputc('\n', stderr);
    if (s->sc_kind == 0 && s->sc_num == 214)
        fprintf(stderr, "rvsim: brk 0x%016llx -> 0x%016llx\n",
                (unsigned long long)brk_old,             /* 调用前旧 brk（重要 3） */
                (unsigned long long)s->brk_cur);
    if (s->sc_kind == 0 && s->sc_num == 222 && s->sc_ret >= 0)
        fprintf(stderr, "rvsim: mmap len=0x%016llx -> 0x%016llx\n",
                (unsigned long long)s->sc_args[1],
                (unsigned long long)s->sc_ret);
    if (s->sc_kind == 0 && s->sc_num == 215)
        fprintf(stderr, "rvsim: munmap 0x%016llx len=0x%016llx = %lld\n",
                (unsigned long long)s->sc_args[0],
                (unsigned long long)s->sc_args[1],
                (long long)s->sc_ret);
}

int main(int argc, char **argv)
{
    rv_cli opt;
    rvsim s;
    rv_elf e;
    rv_trace tr;
    int rc, prc, code, trace_on = 0;
    uint32_t i;

    prc = cli_parse(argc, argv, &opt);
    if (prc != 0)
        return prc;                             /* 200：诊断已由 cli 打印 */
    if (opt.help) {
        cli_print_usage(stdout);
        return 0;                               /* spec §6.1：-h → exit 0 */
    }

    memset(&s, 0, sizeof s);
    if (mem_init(&s.mem) != 0) {
        fprintf(stderr, "rvsim: internal mem-init OOM\n");
        return RVSIM_EX_HOSTERR;                /* 208 */
    }
    rvsim_reset(&s);                            /* GPR 清零 + host_fd={0,1,2} + 尾部字段 */

    rc = elf_load(&s.mem, opt.elf, &e);
    if (rc != 0) {
        if (rc == -8) {
            /* 文件打不开：spec §7 归用法错误 → 200（细分码 -8，修复轮 1/I-1） */
            fprintf(stderr, "rvsim: usage cannot open ELF %s\n", opt.elf);
            mem_free(&s.mem);
            return RVSIM_EX_USAGE;              /* 200 */
        }
        fprintf(stderr, "rvsim: elf-load %s failed code=%d\n", opt.elf, rc);
        mem_free(&s.mem);
        return RVSIM_EX_ELF;                    /* 201 */
    }

    /* heap 区：初始空 [brk_base, brk_base)，brk syscall 扩 hi（分发 A 契约） */
    s.brk_base = e.brk_base;
    s.brk_cur = e.brk_base;
    mem_add_region(&s.mem, e.brk_base, e.brk_base, 1);

    /* stack 区：8 MiB 预映射（spec §3.2；ELF 区已由 elf_load 逐段登记 kind=0） */
    mem_add_region(&s.mem, STACK_TOP - STACK_SIZE, STACK_TOP, 3);

    if (build_stack(&s, opt.gargc, opt.gargs) != 0) {
        fprintf(stderr, "rvsim: internal build-stack failed\n");
        mem_free(&s.mem);
        return RVSIM_EX_HOSTERR;                /* 208 */
    }
    s.cpu.pc = e.entry;

    /* -v/-vv 装配期里程碑日志（spec §6.1；-vv 附区域映射细节/栈布局） */
    if (opt.verbose >= 1) {
        for (i = 0; i < s.mem.n; i++) {
            const rv_region *r = &s.mem.rgn[i];
            if (opt.verbose >= 2)
                fprintf(stderr, "rvsim: region [0x%016llx, 0x%016llx) kind=%d\n",
                        (unsigned long long)r->lo, (unsigned long long)r->hi,
                        r->kind);
            else if (r->kind == 0)
                fprintf(stderr, "rvsim: load seg lo=0x%016llx hi=0x%016llx\n",
                        (unsigned long long)r->lo, (unsigned long long)r->hi);
        }
        if (opt.verbose >= 2)
            fprintf(stderr,
                    "rvsim: stack [0x%016llx, 0x%016llx) sp=0x%016llx argc=%d\n",
                    (unsigned long long)(STACK_TOP - STACK_SIZE),
                    (unsigned long long)STACK_TOP,
                    (unsigned long long)s.cpu.x[2], opt.gargc);
    }

    /* -s：调试器接管整个会话（Task 13 分发 B，spec §6.2）：装载后停在
       entry 前，s/c/r/m/b/d/q/h 交互至 q/EOF；退出码 = 未终止 0 / 已终止
       透传（guest 退出码/205/206/202..204/207）。-s 下下方主循环不运行，
       --trace/--dump-regs/--dump-mem 不生效（调试器会话即全部输出，申报）。 */
    if (opt.dbg) {
        int dcode = 0;
        debugger_run(&s, opt.max_steps, opt.verbose, &dcode);
        mem_free(&s.mem);
        return dcode;
    }

    /* --trace：打开失败 → 208，不吞（spec §7 尾注） */
    if (opt.trace_path != NULL) {
        if (trace_open(&tr, opt.trace_path) != 0) {
            fprintf(stderr, "rvsim: internal trace open %s failed\n",
                    opt.trace_path);
            mem_free(&s.mem);
            return RVSIM_EX_HOSTERR;            /* 208 */
        }
        trace_on = 1;
    }

    /* 主循环（spec §4.3）：exited（exit/exit_group/EBREAK/205）或故障即停。
       trace_stdout flush 纪律（修复轮 1/重要 2，对齐 debugger do_step）：
       trace 走 stdout 时每步 rv_step 前、trace_step 后各一次 fflush——
       guest write 直写 fd1，stdio 缓冲里的 trace 行必须先落盘，guest 输出
       才不会嵌进缓冲块中间（顺序：… L(n-1)、W(n)、L(n)、W(n+1) …）。 */
    const int trace_stdout =
        (opt.trace_path != NULL && strcmp(opt.trace_path, "-") == 0);
    for (;;) {
        uint64_t brk_before = s.brk_cur;        /* -v brk 行的"调用前旧值" */
        if (s.exited) {
            code = s.exit_code;
            break;
        }
        if (s.cpu.steps >= opt.max_steps) {
            code = RVSIM_EX_STEPLIMIT;          /* 207（诊断行见 rv_diag_stderr） */
            break;
        }
        if (trace_stdout)
            fflush(stdout);                     /* 先冲 stdio，guest write 直写 fd1 不越行 */
        if (rv_step(&s) != 0) {
            switch (s.fault) {                  /* §7 诊断统一在循环后 rv_diag_stderr */
            case RV_FV_UNMAPPED:
                code = RVSIM_EX_UNMAPPED;
                break;
            case RV_FV_MISALIGNED:
                code = RVSIM_EX_MISALIGNED;
                break;
            default:                            /* RV_FV_ILLEGAL，防御越界枚举 */
                code = RVSIM_EX_ILLEGAL;
                break;
            }
            break;
        }
        if (trace_on) {
            trace_step(&tr, &s);                /* 每步成功退休 → 一行 trace */
            if (trace_stdout)
                fflush(stdout);                 /* trace 行立即落 fd1（重要 2） */
        }
        if (opt.verbose >= 1 && s.has_syscall)
            vlog_syscall(&s, brk_before);       /* -v syscall/brk/mmap/munmap 日志 */
    }

    /* 终止型诊断（§7：205/206/202..204/207；guest 正常 exit 不打）；
       main 与调试器共享实现（trace.c，Task 13 分发 B） */
    rv_diag_stderr(&s, code, opt.max_steps, opt.verbose);

    /* -v：退出原因（spec §6.1）+ 退休指令数（cpu.steps 只在指令正常退休时
       递增，见 cpu.c——EBREAK/故障/205 路径不计入，与 trace 行数同口径） */
    if (opt.verbose >= 1)
        fprintf(stderr,
                "rvsim: exit reason=%s code=%d pc=0x%016llx steps=%llu\n",
                rv_exit_reason(&s, code), code, (unsigned long long)s.cpu.pc,
                (unsigned long long)s.cpu.steps);

    /* 停机 dump（spec §6.1：向 stdout；--dump-mem 读未映射 → 208，申报） */
    if (opt.dump_regs)
        rv_dump_regs(stdout, &s.cpu);
    if (opt.dump_mem && rv_dump_mem(stdout, &s.mem, opt.dm_addr, opt.dm_len) != 0) {
        fprintf(stderr, "rvsim: internal dump-mem read failed at 0x%016llx\n",
                (unsigned long long)opt.dm_addr);
        mem_free(&s.mem);
        return RVSIM_EX_HOSTERR;                /* 208 */
    }

    /* --trace 收尾：写/关失败 → 208，不吞（spec §7 尾注） */
    if (trace_on && trace_close(&tr) != 0) {
        fprintf(stderr, "rvsim: internal trace write failed\n");
        mem_free(&s.mem);
        return RVSIM_EX_HOSTERR;                /* 208 */
    }

    mem_free(&s.mem);
    return code;
}
