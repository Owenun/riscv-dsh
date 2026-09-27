/* t_x0: x0（zero 寄存器）语义的 C 可观察面 —— 常量 0 传播等价性。
   局限说明（task-8-report 详述）：C 语言层无法可移植地表达「写 x0」（无
   内联 asm 手段；L1-native 宿主双编译也不允许目标特定 asm），因此硬件级
   「rd=x0 写入丢弃」「store 到 x0 基址」由 L0 test_memops.c 覆盖（LWU
   rd=x0 写丢弃、SB 0(x0) → ea=0 未映射且 x0 恒 0），test_alu.c 覆盖
   ADD x0 写丢弃。本程序做编译器层面的性质测试：任何「结构上恒为 0」的
   表达式（常量 0 参与的加/减/与/异或/移位、自反减）在任何合法实现下
   必须等于 0，且 volatile 参与运算不得破坏该等价性 —— 即 C 视角下
   「写入 x0 的值不存在、读到的零恒为 0」。仅用 RV64I 基础算术（无乘除，
   M 扩展不在范围）。 */
#include "guest.h"

int main(void)
{
    volatile long x = 12345;
    volatile long y = -6789;
    volatile unsigned long uz = 0;
    long z = 0;                                /* 常量 0（x0 的 C 影子） */

    CHECK_G(__LINE__, z == 0);
    CHECK_G(__LINE__, z + x == x);             /* 0+x ≡ x */
    CHECK_G(__LINE__, x + z == x);
    CHECK_G(__LINE__, x - z == x);
    CHECK_G(__LINE__, z - z == 0);
    CHECK_G(__LINE__, x - x == 0);             /* 自反减恒 0 */
    CHECK_G(__LINE__, (x ^ x) == 0);
    CHECK_G(__LINE__, (x & 0) == 0);
    CHECK_G(__LINE__, (x | 0) == x);
    CHECK_G(__LINE__, (z << 10) == 0);
    CHECK_G(__LINE__, (z >> 3) == 0);
    CHECK_G(__LINE__, ((long)uz >> 63) == 0);  /* 无符号零的最高位为 0 */
    CHECK_G(__LINE__, (x | y) != 0);           /* 非零输入不被零化 */
    CHECK_G(__LINE__, (z > 0) == 0 && (z < 0) == 0 && (z >= 0) == 1);
    CHECK_G(__LINE__, !(x == x) == 0);
    CHECK_G(__LINE__, (0 - y) == -y);          /* 0-y ≡ -y */

    puts_("PASS t_x0\n");
    return 0;
}
