/* t_recursion: 递归 fib(15) vs 迭代 fib(15) 互验（fib(15)=610，栈深约 15 层的真实递归）。
 * 覆盖口径（objdump 实测：jal=14 bne=6 bge=1 bltu=1 lui=6，口径见 task-9b 分发 B 报告）：
 * jal（递归自调用 + 调用往返；本程序机器码无 jalr/auipc）。
 * noinline 防止 -O2 把首层递归内联掉。纯 RV64I，禁 M/A 扩展；C 层无乘除（仅加法递推）。
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
    volatile int n15 = 15;
    unsigned long long r = fib_r(n15);
    unsigned long long i = fib_i(n15);

    CHECK_G(__LINE__, r == i);          /* 递归版与迭代版互验 */
    CHECK_G(__LINE__, r == 610ULL);     /* fib(15)=610 手算核对 */
    CHECK_G(__LINE__, fib_i(0) == 0ULL);
    CHECK_G(__LINE__, fib_i(1) == 1ULL);
    CHECK_G(__LINE__, fib_r(2) == 1ULL);

    puthex(r);
    puts_("\n");
    puts_("PASS t_recursion\n");
    return 0;
}
