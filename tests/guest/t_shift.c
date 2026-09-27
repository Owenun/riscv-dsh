/* t_shift: SLL/SRL/SRA 符号保持与移位量 0/63 边界 + W 类移位 31 边界（纯 RV64I） */
#include "guest.h"

int main(void)
{
    volatile unsigned long long one = 1ULL;
    volatile unsigned long long n8 = 0xFFFFFFFFFFFFFFF8ULL; /* -8 的位型 */
    volatile unsigned long long imin = 0x8000000000000000ULL;
    volatile long long sn8 = -8LL;
    volatile long long sn1 = -1LL;

    /* SLL：1<<63 回绕，高位丢弃 */
    CHECK_G(__LINE__, (one << 63) == 0x8000000000000000ULL);
    CHECK_G(__LINE__, (one << 63 << 1) == 0ULL);

    /* SRL：逻辑右移补 0 */
    CHECK_G(__LINE__, (n8 >> 60) == 0xFULL);
    CHECK_G(__LINE__, (imin >> 63) == 1ULL);

    /* SRA：算术右移保持符号 */
    CHECK_G(__LINE__, (sn8 >> 60) == -1LL);
    CHECK_G(__LINE__, (sn1 >> 63) == -1LL);
    CHECK_G(__LINE__, ((long long)imin >> 63) == -1LL);

    /* SRL vs SRA 对照：同一负位型两种填充 */
    CHECK_G(__LINE__, (n8 >> 60) != (unsigned long long)(sn8 >> 60));

    /* 移位量 0 与 63（volatile 移位量防止常量折叠，生成真实 sll/srl/sra） */
    volatile unsigned long long v = 0x0123456789ABCDEFULL;
    volatile unsigned long long s0 = 0ULL;
    volatile unsigned long long s63 = 63ULL;
    CHECK_G(__LINE__, (v >> s0) == v);
    CHECK_G(__LINE__, (v << s0) == v);
    CHECK_G(__LINE__, (v >> s63) == 0ULL);
    CHECK_G(__LINE__, (v << s63) == 0x8000000000000000ULL);
    CHECK_G(__LINE__, (sn8 >> s0) == sn8);
    CHECK_G(__LINE__, ((long long)v >> s63) == 0LL);

    /* W 类移位 31 边界：SLLIW 结果符号扩展后为负 64 位视图 */
    volatile unsigned int u1 = 1U;
    unsigned int w = u1 << 31;
    CHECK_G(__LINE__, w == 0x80000000U);
    CHECK_G(__LINE__, (long long)(int)w == -2147483647LL - 1);
    CHECK_G(__LINE__, (unsigned long long)(long long)(int)w == 0xFFFFFFFF80000000ULL);

    /* SLLIW：1<<31 左移一位（32 位内回绕）→ 0 */
    CHECK_G(__LINE__, (u1 << 31 << 1) == 0U);

    /* SRAIW：负数 >> 31 保持符号；>> 2 保持符号 */
    volatile int m1 = -1;
    volatile int n16 = -16;
    CHECK_G(__LINE__, (m1 >> 31) == -1);
    CHECK_G(__LINE__, (n16 >> 2) == -4);

    /* SRLIW：0x80000000 逻辑右移 31 → 1（无符号视图） */
    volatile unsigned int uh = 0x80000000U;
    CHECK_G(__LINE__, (uh >> 31) == 1U);

    puts_("PASS t_shift\n");
    return 0;
}
