/* src/cli.c —— Task 12 分发 A（M6-a）：完整命令行解析（spec §6.1）。
   rvsim [OPTIONS] ELF [GUEST_ARGS...]
     -v/-vv、--trace FILE（"-"=stdout）、--dump-regs、--dump-mem A:L、
     --max-steps N（默认 1e8）、-s、-h。
   解析规则（钉死）：
   - 选项只出现在 ELF 之前；ELF 路径 = 第一个非选项参数，其后全部为
     guest-args 透传（即使形如 -v，guest argv 语义见 spec §3.4）；
   - -h：置 help，main 打印 usage(stdout) 后退出 0（无需 ELF）；
   - 未知选项/选项缺值/坏值（--dump-mem 无冒号或非法数字、--max-steps
     非法数字）→ stderr `rvsim: usage …` + 返回 200；
   - 数字解析：strtoull，全串消费；前导负号/正号拒绝（修复轮 1/重要 4：
     strtoull 会吞掉 '-' 并按回绕产出 ULLONG_MAX——"--max-steps -1" 曾被
     接受跑通全程；先跳过前导空白再看首字符，与调试器 parse_u64 口径
     一致）；--dump-mem 基 0（0x 前缀/十进制均可，spec 示例 `0x100000:32`）；
     --max-steps 基 10；ERANGE → 200。
   -v 与 -vv 同时出现取更细（-vv 语义包含 -v）。 */
#include "rvsim.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void cli_print_usage(FILE *out)
{
    fprintf(out,
        "usage: rvsim [OPTIONS] ELF [GUEST_ARGS...]\n"
        "  -v              milestone log (segments/brk/mmap/syscalls/exit) to stderr\n"
        "  -vv             finer: region map, stack layout, full GPR dump on fault\n"
        "  --trace FILE    per-instruction trace ('-' = stdout)\n"
        "  --dump-regs     print x0..x31 and pc on stop\n"
        "  --dump-mem A:L  hexdump [A, A+L) on stop\n"
        "  --max-steps N   instruction limit (default 100000000, exit 207 on hit)\n"
        "  -s              interactive debugger (minimal in M6-A)\n"
        "  -h              this help\n");
}

static int usage_fail(const char *detail)
{
    fprintf(stderr, "rvsim: usage %s\n", detail);
    return RVSIM_EX_USAGE;
}

/* strtoull 吞负号防御（修复轮 1/重要 4）：跳过前导空白后首字符不得为
   '-'/'+'（与调试器 parse_u64 口径一致），否则 strtoull 按回绕接受负数。 */
static const char *skip_sign_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')             /* strtoull 自会跳前导空白，先看穿它 */
        s++;
    return s;
}

/* 全串消费的无符号解析；base 0 允许 0x 前缀。失败（空串/负号/残留字符/
   溢出）返回 -1。 */
static int parse_u64(const char *s, int base, uint64_t *out)
{
    char *end;
    uint64_t v;
    if (s == NULL)
        return -1;
    s = skip_sign_ws(s);
    if (*s == '\0' || *s == '-' || *s == '+')
        return -1;
    errno = 0;
    v = strtoull(s, &end, base);
    if (errno == ERANGE || end == s || *end != '\0')
        return -1;
    *out = v;
    return 0;
}

/* --dump-mem A:L（':' 分隔；A 允许 0x 前缀，L 同；A/L 均拒前导负号/正号） */
static int parse_dump_mem(const char *spec, uint64_t *addr, uint64_t *len)
{
    char *end;
    const char *p;
    uint64_t a, l;
    if (spec == NULL)
        return -1;
    spec = skip_sign_ws(spec);
    if (*spec == '\0' || *spec == '-' || *spec == '+')
        return -1;
    errno = 0;
    a = strtoull(spec, &end, 0);
    if (errno == ERANGE || end == spec || *end != ':')
        return -1;
    p = end + 1;
    p = skip_sign_ws(p);
    if (*p == '\0' || *p == '-' || *p == '+')
        return -1;
    errno = 0;
    l = strtoull(p, &end, 0);
    if (errno == ERANGE || end == p || *end != '\0')
        return -1;
    *addr = a;
    *len = l;
    return 0;
}

int cli_parse(int argc, char **argv, rv_cli *o)
{
    int i;
    memset(o, 0, sizeof *o);
    o->max_steps = RVSIM_DEFAULT_MAX_STEPS;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-')
            break;                              /* ELF 及其后全部为 guest-args */
        if (strcmp(a, "-v") == 0) {
            if (o->verbose < 1)
                o->verbose = 1;
        } else if (strcmp(a, "-vv") == 0) {
            o->verbose = 2;
        } else if (strcmp(a, "--trace") == 0) {
            if (++i >= argc)
                return usage_fail("--trace missing FILE");
            o->trace_path = argv[i];
        } else if (strcmp(a, "--dump-regs") == 0) {
            o->dump_regs = true;
        } else if (strcmp(a, "--dump-mem") == 0) {
            if (++i >= argc)
                return usage_fail("--dump-mem missing A:L");
            if (parse_dump_mem(argv[i], &o->dm_addr, &o->dm_len) != 0)
                return usage_fail("--dump-mem bad A:L spec");
            o->dump_mem = true;
        } else if (strcmp(a, "--max-steps") == 0) {
            if (++i >= argc)
                return usage_fail("--max-steps missing N");
            if (parse_u64(argv[i], 10, &o->max_steps) != 0)
                return usage_fail("--max-steps bad number");
        } else if (strcmp(a, "-s") == 0) {
            o->dbg = true;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            o->help = true;
            return 0;
        } else {
            fprintf(stderr, "rvsim: usage unknown option %s\n", a);
            return RVSIM_EX_USAGE;
        }
    }

    if (i >= argc)
        return usage_fail("missing ELF");
    o->elf = argv[i];
    o->gargs = &argv[i];                        /* gargs[0] = ELF 路径（guest argv[0]） */
    o->gargc = argc - i;
    return 0;
}
