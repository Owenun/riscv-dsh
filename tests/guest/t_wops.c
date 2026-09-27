/* t_wops: 9 条 W 类指令语义（32 位运算结果符号扩展到 64 位视图，纯 RV64I） */
#include "guest.h"

int main(void)
{
    volatile unsigned int big = 0x7FFFFFFFU;
    volatile unsigned int one = 1U;
    volatile unsigned int half = 0x80000000U;
    volatile unsigned int zero = 0U;

    /* ADDIW：0x7FFFFFFF+1 32 位回绕 → 0x80000000，64 位视图为符号扩展负值 */
    unsigned int s = big + one;
    CHECK_G(__LINE__, s == 0x80000000U);
    CHECK_G(__LINE__, (long long)(int)s == -2147483647LL - 1);
    CHECK_G(__LINE__, (unsigned long long)(long long)(int)s == 0xFFFFFFFF80000000ULL);

    /* ADDIW：负立即数（-200）与寄存器相加 */
    volatile int a = 100;
    CHECK_G(__LINE__, (a + (-200)) == -100);

    /* ADDW：0xFFFFFFFF+1 回绕到 0（64 位视图为 0） */
    volatile unsigned int f = 0xFFFFFFFFU;
    unsigned int w0 = f + one;
    CHECK_G(__LINE__, w0 == 0U);
    CHECK_G(__LINE__, (unsigned long long)(long long)(int)w0 == 0ULL);

    /* ADDW：0x80000000+0 结果符号扩展到 64 位 */
    unsigned int t = half + zero;
    CHECK_G(__LINE__, t == 0x80000000U);
    CHECK_G(__LINE__, (unsigned long long)(long long)(int)t == 0xFFFFFFFF80000000ULL);

    /* SUBW：0-1 借位 → 0xFFFFFFFF，64 位符号扩展视图为 -1 */
    unsigned int d = zero - one;
    CHECK_G(__LINE__, d == 0xFFFFFFFFU);
    CHECK_G(__LINE__, (long long)(int)d == -1LL);
    CHECK_G(__LINE__, (unsigned long long)(long long)(int)d == 0xFFFFFFFFFFFFFFFFULL);

    /* SUBW：0x80000000-1 = 0x7FFFFFFF（正数无符号扩展） */
    CHECK_G(__LINE__, (half - one) == 0x7FFFFFFFU);
    CHECK_G(__LINE__, (long long)(int)(half - one) == 0x7FFFFFFFLL);

    /* SLLIW：1<<31，结果符号扩展（区别于 SLLI 的 64 位移位） */
    volatile unsigned int u1 = 1U;
    unsigned int sl = u1 << 31;
    CHECK_G(__LINE__, sl == 0x80000000U);
    CHECK_G(__LINE__, (long long)(int)sl == -2147483647LL - 1);
    CHECK_G(__LINE__, (unsigned long long)(long long)(int)sl == 0xFFFFFFFF80000000ULL);
    /* SLLW：32 位内连续左移丢弃高位 */
    CHECK_G(__LINE__, (u1 << 31 << 1) == 0U);

    /* SRLIW vs SRAIW 对照：同一位型 0x80000000>>4 */
    unsigned int lr = half >> 4;                 /* SRLIW：逻辑填充 */
    volatile int nmsb = -2147483647 - 1;
    int ar = nmsb >> 4;                          /* SRAIW：算术填充 */
    CHECK_G(__LINE__, lr == 0x08000000U);
    CHECK_G(__LINE__, (long long)(int)lr == 0x08000000LL);
    CHECK_G(__LINE__, ar == (int)0xF8000000U);
    CHECK_G(__LINE__, (long long)ar == -134217728LL);

    /* SRLW：0x80000001>>1 = 0x40000000（无符号填充） */
    volatile unsigned int uv = 0x80000001U;
    CHECK_G(__LINE__, (uv >> 1) == 0x40000000U);

    /* SRAW：负数算术右移，64 位视图符号扩展 */
    volatile int n16 = -16;
    volatile int m1 = -1;
    volatile int n4096 = -4096;
    CHECK_G(__LINE__, (n16 >> 2) == -4);
    CHECK_G(__LINE__, (m1 >> 31) == -1);
    CHECK_G(__LINE__, (n4096 >> 4) == -256);
    CHECK_G(__LINE__, (unsigned long long)(long long)(n4096 >> 4) == 0xFFFFFFFFFFFFFF00ULL);
    CHECK_G(__LINE__, (unsigned long long)(long long)(nmsb >> 3) == 0xFFFFFFFFF0000000ULL);

    puts_("PASS t_wops\n");
    return 0;
}
