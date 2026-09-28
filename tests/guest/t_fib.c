/* t_fib: 递归 fib(12) 与迭代 fib(12) 互验（fib(12)=144）。
 * 覆盖口径（objdump 实测：jal=11 bne=5 bge=1 bltu=1 lui=5，口径见 task-9b 分发 B 报告）：
 * jal（递归自调用与调用往返；本程序机器码无 jalr/auipc）。
 * noinline 防止 -O2 内联首层递归。纯 RV64I，禁 M/A 扩展；C 层无乘除（仅加法递推）。
 */
#include "guest.h"

static __attribute__((noinline)) unsigned long long fib_r(int n)
{
    if (n < 2)
        return (unsigned long long)n;
    return fib_r(n - 1) + fib_r(n - 2);
}

static unsigned long long fib_i(int n)
{
    unsigned long long a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        unsigned long long t = a + b;
        a = b;
        b = t;
    }
    return a;
}

int main(void)
{
    volatile int n12 = 12;
    unsigned long long r = fib_r(n12);
    unsigned long long i = fib_i(n12);

    CHECK_G(__LINE__, r == i);          /* 递归版与迭代版互验 */
    CHECK_G(__LINE__, r == 144ULL);     /* fib(12)=144 手算核对 */
    CHECK_G(__LINE__, fib_i(2) == 1ULL);

    puthex(r);
    puts_("\n");
    puts_("PASS t_fib\n");
    return 0;
}
