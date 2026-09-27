/* t_arith: 加法进位回绕 / SUB 借位 / SLT vs SLTU 负数-大数边界（纯 RV64I，禁 M 扩展） */
#include "guest.h"

int main(void)
{
    /* 加法进位回绕：INT64_MAX + 1（无符号运算，避免 -O2 有符号溢出 UB） */
    volatile unsigned long long imax = 0x7FFFFFFFFFFFFFFFULL;
    volatile unsigned long long one = 1ULL;
    unsigned long long carry = imax + one;
    CHECK_G(__LINE__, carry == 0x8000000000000000ULL);
    CHECK_G(__LINE__, (long long)carry == (long long)0x8000000000000000LL);

    /* 全 1 加 1 回绕到 0 */
    volatile unsigned long long all1 = 0xFFFFFFFFFFFFFFFFULL;
    CHECK_G(__LINE__, all1 + one == 0ULL);

    /* 两个大正数相加回绕（carry out 丢弃） */
    CHECK_G(__LINE__, imax + imax == 0xFFFFFFFFFFFFFFFEULL);

    /* SUB 借位：0-1 与 INT64_MIN-1 回绕 */
    volatile unsigned long long zero = 0ULL;
    volatile unsigned long long imin = 0x8000000000000000ULL;
    CHECK_G(__LINE__, zero - one == 0xFFFFFFFFFFFFFFFFULL);
    CHECK_G(__LINE__, (long long)(zero - one) == -1LL);
    CHECK_G(__LINE__, imin - one == 0x7FFFFFFFFFFFFFFFULL);
    CHECK_G(__LINE__, imin - imin == 0ULL);

    /* SLT vs SLTU：负数/大数边界（有符号与无符号比较结论相反） */
    volatile long long n1 = -1LL;
    volatile long long p1 = 1LL;
    CHECK_G(__LINE__, n1 < p1);                                     /* SLT: -1 < 1 */
    CHECK_G(__LINE__, n1 < (long long)zero);                         /* SLT: -1 < 0 */
    CHECK_G(__LINE__, (unsigned long long)n1 > (unsigned long long)p1);  /* SLTU: 0xFF..F > 1 */
    CHECK_G(__LINE__, !((unsigned long long)n1 < (unsigned long long)one));
    CHECK_G(__LINE__, (unsigned long long)zero < (unsigned long long)all1);
    CHECK_G(__LINE__, (long long)imin < (long long)zero);           /* SLT: INT64_MIN < 0 */
    CHECK_G(__LINE__, (unsigned long long)imin > (unsigned long long)p1);
    CHECK_G(__LINE__, (long long)imin < (long long)p1);             /* SLT: INT64_MIN < 1 */
    CHECK_G(__LINE__, (long long)imin < (long long)n1);             /* SLT: INT64_MIN < -1 */

    puts_("PASS t_arith\n");
    return 0;
}
