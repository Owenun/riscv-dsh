/* t_branch: 6 种分支 beq/bne/blt/bge/bltu/bgeu 的 taken/not-taken 双路径产生可观察的不同输出。
 * 覆盖口径（objdump 实测：beq=6 bne=3 blt=4 bge=6 bltu=5 bgeu=4 jal=42 lui=8 口径见
 * task-9b 分发 B 报告）：6 种分支助记符均在本程序机器码中出现（13 个 if/else 分支点）
 * + jal（puts_/gexit 外部调用）。
 * volatile 输入防止 -O2 把分支折叠成常量；分支体内 puts_ 外部调用强制保留真实分支。
 * 纯 RV64I，禁 M/A 扩展；C 层无乘除。
 */
#include "guest.h"

int main(void)
{
    volatile long long a = 3, b = 5;
    volatile long long c = 7, d = 7;
    volatile long long n1 = -1LL;
    volatile unsigned long long u1 = 1ULL;
    volatile unsigned long long umax = 0xFFFFFFFFFFFFFFFFULL;

    /* beq：相等 taken(T) / 不等 not-taken(N) */
    if (c == d) puts_("T"); else puts_("N");
    if (a == b) puts_("T"); else puts_("N");
    /* bne：不等 taken / 相等 not-taken */
    if (a != b) puts_("T"); else puts_("N");
    if (c != d) puts_("T"); else puts_("N");
    /* blt：有符号小于（含负数边界 -1 < 1） */
    if (a < b) puts_("T"); else puts_("N");
    if (b < a) puts_("T"); else puts_("N");
    if (n1 < (long long)u1) puts_("T"); else puts_("N");
    /* bge：有符号大于等于（相等值分支） */
    if (c >= d) puts_("T"); else puts_("N");
    if (a >= b) puts_("T"); else puts_("N");
    /* bltu：无符号比较（0xFF..F 无符号视角最大，有符号视角为 -1，两条路径结论相反） */
    if (u1 < umax) puts_("T"); else puts_("N");
    if (umax < u1) puts_("T"); else puts_("N");
    /* bgeu：无符号大于等于（相等值分支） */
    if (umax >= u1) puts_("T"); else puts_("N");
    if (u1 >= umax) puts_("T"); else puts_("N");

    /* 语义断言：比较结果与分支走向一致 */
    CHECK_G(__LINE__, (c == d) && !(a == b));
    CHECK_G(__LINE__, (a != b) && !(c != d));
    CHECK_G(__LINE__, (a < b) && !(b < a) && (n1 < (long long)u1));
    CHECK_G(__LINE__, (c >= d) && !(a >= b));
    CHECK_G(__LINE__, (u1 < umax) && !(umax < u1));
    CHECK_G(__LINE__, (umax >= u1) && !(u1 >= umax));

    puts_("\n");
    puts_("PASS t_branch\n");
    return 0;
}
