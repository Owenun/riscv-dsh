/* t_call: 直接调用链（jal）+ volatile 函数指针间接调用（强制 jalr）+ 深层调用链回传值。
 * 覆盖口径（objdump 实测：jal=25 jalr=3 bne=5 bltu=1 lui=9，口径见 task-9b 分发 B 报告）：
 * jal（add10..add1 直接调用链，noinline 防内联折叠）+ jalr×3（volatile 函数指针间接调用）。
 * 纯 RV64I，禁 M/A 扩展；C 层无乘除。
 */
#include "guest.h"

#define NOINLINE __attribute__((noinline))

static NOINLINE int add1(int x) { return x + 1; }
static NOINLINE int add2(int x) { return add1(x) + 1; }
static NOINLINE int add3(int x) { return add2(x) + 1; }
static NOINLINE int add4(int x) { return add3(x) + 1; }
static NOINLINE int add5(int x) { return add4(x) + 1; }
static NOINLINE int add6(int x) { return add5(x) + 1; }
static NOINLINE int add7(int x) { return add6(x) + 1; }
static NOINLINE int add8(int x) { return add7(x) + 1; }
static NOINLINE int add9(int x) { return add8(x) + 1; }
static NOINLINE int add10(int x) { return add9(x) + 1; }
static NOINLINE int add_xy(int x, int y) { return x + y; }

int main(void)
{
    /* 深层直接调用链：add10 → add9 → ... → add1，逐层 +1 回传 */
    volatile int base = 6;
    int r = add10(base);
    CHECK_G(__LINE__, r == 16);

    /* volatile 函数指针：指针本身 volatile，强制运行期间接跳转（jalr） */
    int (*volatile fp2)(int, int) = add_xy;
    int s = fp2(20, 22);
    CHECK_G(__LINE__, s == 42);

    int (*volatile fp1)(int) = add1;
    CHECK_G(__LINE__, fp1(41) == 42);
    CHECK_G(__LINE__, fp1(s) == 43);

    puthex((unsigned long)r);
    puts_("\n");
    puthex((unsigned long)s);
    puts_("\n");
    puts_("PASS t_call\n");
    return 0;
}
