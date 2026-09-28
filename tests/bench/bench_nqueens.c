/* bench_nqueens.c —— bench 分发 A：N-皇后位运算回溯（N=11）。
 * 解总数为已知值 2680（OEIS A000170），作为已知答案断言。
 * 纯 RV64I：仅移位/位运算/分支，无乘除；递归深度 ≤ N。
 * volatile n 防止编译期常量折叠整个搜索；输出确定性。
 */
#include "guest.h"

/* 校准（分发 B）：1 次 ~0.15s 低于 0.5-3s 计时窗口，8 次 ~1.2s 入窗 */
#define BENCH_ITERS 8

static void putdec(unsigned long v)
{
    char b[20];
    int i = 20;
    do { b[--i] = (char)('0' + (v % 10)); v /= 10; } while (v);
    gwrite(1, b + i, (unsigned long)(20 - i));
}

static unsigned int nsolutions;

/* 经典位掩码回溯：cols=已占列，ld/rd=左/右对角线在当前行的投影 */
static void nq(unsigned int all, unsigned int cols, unsigned int ld, unsigned int rd)
{
    if (cols == all) {
        nsolutions++;
        return;
    }
    unsigned int avail = all & ~(cols | ld | rd);
    while (avail) {
        unsigned int bit = avail & (0u - avail);   /* 最低置位位 */
        avail -= bit;
        nq(all, cols | bit, (ld | bit) << 1, (rd | bit) >> 1);
    }
}

int main(void)
{
    volatile int n = 11;
    unsigned int all = (1u << n) - 1u;

    for (int it = 0; it < BENCH_ITERS; it++) {
        nsolutions = 0;
        nq(all, 0u, 0u, 0u);
    }

    CHECK_G(__LINE__, nsolutions == 2680u);

    puts_("PASS bench_nqueens N=");
    putdec((unsigned long)n);
    puts_(" solutions=");
    putdec((unsigned long)nsolutions);
    puts_("\n");
    return 0;
}
