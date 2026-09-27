/* src/main.c —— Task 11/M5 分发 B：main 装配（spec §2.1/§3.4/§7）。
   装配顺序：argc 校验 → mem_init → rvsim_reset（host_fd={0,1,2}）→ elf_load →
   heap/stack 区登记 → build_stack（sp 写 x2）→ pc=entry → 主循环 → mem_free。
   ELF 镜像区登记：elf_load（M1 契约，src/elf.c 第二遍）已把每个 PT_LOAD 段按
   实际装载范围 [va, va+memsz) 逐段登记 kind=0——段间页对齐空隙保持未登记，
   野访问按 202 拒绝（spec §3.1「不许野访问」）；故 main 不再重复登记一个跨
   全镜像的宽区间（那会把段间空隙也放进可访问范围，弱化越界检测）。
   heap 区：[brk_base, brk_base) 初始空登记（kind=1），brk syscall 按
   kind==1 且 lo==brk_base 匹配后扩/缩 hi（Task 10 分发 A 契约）；
   mem_check 对 hi==lo 空区域自然拒绝（len>0 落不进），安全。
   stack 区：[STACK_TOP-STACK_SIZE, STACK_TOP) 8 MiB 预映射（spec §3.2），
   STACK_TOP 之上留 1 页 guard（窗口尾页不登记 → 越栈 202）。
   容量衔接（修复轮 1/C-1）：本函数在 elf_load 成功后恒追加恰 2 项登记
   （heap+stack），剩余容量由 elf_load 的 nload+2 预留检查统一保证
   （src/elf.c RVSIM_MAIN_RGN_RESERVE），main 侧不再重复检查。
   CLI：完整解析在 M6（-v/-vv/--trace/--dump/--max-steps/-s）。本任务最小
   处理：第一个非选项参数为 ELF 路径，其余全部作为 guest-args 透传
   （guest argv[0] = ELF 路径本身，spec §3.4）；遇 '-' 开头的参数（M5 无
   任何已实现选项）→ usage 退出码 200。
   主循环（spec §4.3）：steps 上限为内部常量 1e8（--max-steps CLI 在 M6），
   超限 → 207；rv_step 故障 → s.fault 映射 202/203/204 + stderr 诊断（含
   pc，§7；故障地址/指令字字段由 cpu fault 枚举暂未记录，-vv 现场增强在 M6）。
   退出码：s.exited → s.exit_code；故障 → 映射码。
   终止型诊断（cpu/syscall 层不依赖 stdio，由 main 打印）：
   - 205：s.sc_unsupported 时 `rvsim: unsupported syscall N pc=0x…`（§5/§7）；
   - 206：exit_code==206 时 `rvsim: ebreak pc=0x…`——guest 故意 exit(206) 会
     撞号误打此行，属 spec §7 已声明的退出码撞号歧义（cpu 未区分来源，
     M6 可加标志位；M5 无 guest 使用 206）。 */
#include <stdio.h>
#include <string.h>
#include "rvsim.h"

/* M5 内部常量（spec §4.3/§6.1 默认 1e8；--max-steps CLI 在 M6） */
#define RVSIM_DEFAULT_MAX_STEPS 100000000ULL

static int usage_fail(const char *detail)
{
    fprintf(stderr, "rvsim: usage rvsim [opts] ELF [guest-args...] (%s)\n", detail);
    return RVSIM_EX_USAGE;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage_fail("missing ELF");

    /* 最小 CLI：第一个非选项参数为 ELF，其余为 guest-args；选项一律视为未知 */
    int elf_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            fprintf(stderr, "rvsim: usage unknown option %s\n", argv[i]);
            return RVSIM_EX_USAGE;
        }
        elf_idx = i;
        break;
    }
    if (elf_idx == 0)
        return usage_fail("missing ELF");
    char **gargs = &argv[elf_idx];          /* gargs[0] = ELF 路径（guest argv[0]） */
    int gargc = argc - elf_idx;
    const char *elfpath = gargs[0];

    rvsim s;
    memset(&s, 0, sizeof s);
    if (mem_init(&s.mem) != 0) {
        fprintf(stderr, "rvsim: internal mem-init OOM\n");
        return RVSIM_EX_HOSTERR;            /* 208 */
    }
    rvsim_reset(&s);                        /* GPR 清零 + host_fd={0,1,2} */

    rv_elf e;
    int rc = elf_load(&s.mem, elfpath, &e);
    if (rc != 0) {
        if (rc == -8) {
            /* 文件打不开：spec §7 归用法错误 → 200（细分码 -8，修复轮 1/I-1） */
            fprintf(stderr, "rvsim: usage cannot open ELF %s\n", elfpath);
            mem_free(&s.mem);
            return RVSIM_EX_USAGE;          /* 200 */
        }
        fprintf(stderr, "rvsim: elf-load %s failed code=%d\n", elfpath, rc);
        mem_free(&s.mem);
        return RVSIM_EX_ELF;                /* 201 */
    }

    /* heap 区：初始空 [brk_base, brk_base)，brk syscall 扩 hi（分发 A 契约） */
    s.brk_base = e.brk_base;
    s.brk_cur = e.brk_base;
    mem_add_region(&s.mem, e.brk_base, e.brk_base, 1);

    /* stack 区：8 MiB 预映射（spec §3.2；ELF 区已由 elf_load 逐段登记 kind=0） */
    mem_add_region(&s.mem, STACK_TOP - STACK_SIZE, STACK_TOP, 3);

    if (build_stack(&s, gargc, gargs) != 0) {
        fprintf(stderr, "rvsim: internal build-stack failed\n");
        mem_free(&s.mem);
        return RVSIM_EX_HOSTERR;            /* 208 */
    }
    s.cpu.pc = e.entry;

    /* 主循环（spec §4.3）：exited（exit/exit_group/EBREAK/205）或故障即停 */
    int code;
    for (;;) {
        if (s.exited) {
            code = s.exit_code;
            break;
        }
        if (s.cpu.steps >= RVSIM_DEFAULT_MAX_STEPS) {
            fprintf(stderr, "rvsim: step-limit %llu pc=0x%016llx\n",
                    (unsigned long long)RVSIM_DEFAULT_MAX_STEPS,
                    (unsigned long long)s.cpu.pc);
            code = RVSIM_EX_STEPLIMIT;      /* 207 */
            break;
        }
        if (rv_step(&s) != 0) {
            switch (s.fault) {
            case RV_FV_UNMAPPED:
                fprintf(stderr, "rvsim: fault unmapped pc=0x%016llx\n",
                        (unsigned long long)s.cpu.pc);
                code = RVSIM_EX_UNMAPPED;   /* 202 */
                break;
            case RV_FV_MISALIGNED:
                fprintf(stderr, "rvsim: fault misaligned pc=0x%016llx\n",
                        (unsigned long long)s.cpu.pc);
                code = RVSIM_EX_MISALIGNED; /* 203 */
                break;
            default:                        /* RV_FV_ILLEGAL（防御含越界枚举） */
                fprintf(stderr, "rvsim: illegal insn pc=0x%016llx\n",
                        (unsigned long long)s.cpu.pc);
                code = RVSIM_EX_ILLEGAL;    /* 204 */
                break;
            }
            break;
        }
    }

    /* 终止型诊断（见文件头注释） */
    if (s.exited && s.sc_unsupported)
        fprintf(stderr, "rvsim: unsupported syscall %d pc=0x%016llx\n",
                s.unsupported_syscall_num, (unsigned long long)s.cpu.pc);
    else if (s.exited && code == RVSIM_EX_EBREAK)
        fprintf(stderr, "rvsim: ebreak pc=0x%016llx\n", (unsigned long long)s.cpu.pc);

    mem_free(&s.mem);
    return code;
}
