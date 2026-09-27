/* t_imm_edges: I/S/B/U/J 六种立即数格式极值下 gcc -O2 实际生成的指令行为（纯 RV64I） */
#include "guest.h"

static volatile long long s_arr[8];
static volatile long long s_chk[8];
static volatile long long s_acc;

/* noinline：主体足够长，每个提前返回分支都跳向函数末尾 → 生成大位移 B 型指令 */
__attribute__((noinline)) static int far_checks(void)
{
    if (s_chk[0] != 11) return 1;
    if (s_chk[1] != 22) return 2;
    if (s_chk[2] != 33) return 3;
    if (s_chk[3] != 44) return 4;
    if (s_chk[4] != -55) return 5;
    if (s_chk[5] != 66) return 6;
    if (s_chk[6] != 77) return 7;
    if (s_chk[7] != -88) return 8;
    s_acc = s_acc + 1;
    return 0;
}

int main(void)
{
    /* I 型：大正立即数 li（lui+addi 组合生成） */
    volatile long long big = 0x7FFFFFFFFFFFFFFFLL;
    CHECK_G(__LINE__, big == 0x7FFFFFFFFFFFFFFFLL);

    /* I 型：12 位符号立即数极值 -2048 恰好单条 addi；-2049 需 lui+addi 拆分 */
    volatile long long m2048 = -2048LL;
    volatile long long m2049 = -2049LL;
    CHECK_G(__LINE__, m2048 == -2048LL);
    CHECK_G(__LINE__, m2049 == -2049LL);

    /* I 型：全 1 符号扩展立即数参与运算 */
    volatile long long vn1 = -1LL;
    CHECK_G(__LINE__, vn1 + (-1LL) == -2LL);

    /* S 型：负偏移数组寻址（sd 使用负 offset） */
    s_arr[0] = 1;
    s_arr[1] = 2;
    s_arr[2] = 3;
    s_arr[3] = 4;
    volatile long long *p = &s_arr[4];
    p[-3] = 42;
    CHECK_G(__LINE__, s_arr[1] == 42);
    CHECK_G(__LINE__, s_arr[0] == 1 && s_arr[2] == 3 && s_arr[3] == 4);
    p[-1] = -5;
    CHECK_G(__LINE__, s_arr[3] == -5);

    /* U 型：LUI 构造高位常数（bit31=1 需零扩展修正） */
    volatile unsigned long long lui_v = 0xDEADB000ULL;
    CHECK_G(__LINE__, lui_v == 0xDEADB000ULL);

    /* U 型：0x80000000 正值（LUI 符号扩展后需 64 位修正） */
    volatile unsigned long long half = 0x80000000ULL;
    CHECK_G(__LINE__, half == 0x80000000ULL);

    /* U+I 型：-4097 超出 12 位立即数范围，lui+addi 两段拼出 */
    volatile long long neg4k = -4097LL;
    CHECK_G(__LINE__, neg4k == -4097LL);

    /* B 型：远距离前向条件分支（far_checks 内 8 个大位移分支） */
    s_chk[0] = 11;
    s_chk[1] = 22;
    s_chk[2] = 33;
    s_chk[3] = 44;
    s_chk[4] = -55;
    s_chk[5] = 66;
    s_chk[6] = 77;
    s_chk[7] = -88;
    CHECK_G(__LINE__, far_checks() == 0);           /* J 型：直接调用 jal */
    CHECK_G(__LINE__, s_acc == 1);

    /* JALR：函数指针间接调用 */
    int (*fp)(void) = far_checks;
    CHECK_G(__LINE__, fp() == 0);
    CHECK_G(__LINE__, s_acc == 2);

    /* B 型：后向循环分支（负位移） */
    volatile int i;
    int sum = 0;
    for (i = 300; i > 0; i = i - 1)
        sum = sum + 1;
    CHECK_G(__LINE__, sum == 300);

    puts_("PASS t_imm_edges\n");
    return 0;
}
