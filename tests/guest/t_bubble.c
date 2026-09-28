/* t_bubble: O(n^2) 冒泡排序固定数组（n=16，volatile 输入防折叠）。
 * 输出：排序后 16 个元素逐个 puthex + 校验和 puthex，末尾 PASS 行。
 * 本程序是 L2 双编译差分主角：rvsim 执行与宿主 native 执行的 stdout+exit 必须一致
 * （run_all.sh 的 run_l2 段，M5 激活）。
 * 覆盖口径（objdump 实测：bltu=2 bgeu=1 bne=9 jal=15 lui=8，口径见 task-9b 分发 B 报告）：
 * bltu/bgeu（无符号数组比较）、bne（双层循环回边）；本程序机器码无 blt/jalr/auipc。
 * 纯 RV64I，禁 M/A 扩展；C 层无乘除（校验和用加法累加）。
 */
#include "guest.h"

static void bubble(volatile unsigned long long *v, int n)
{
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (v[j] > v[j + 1]) {
                unsigned long long t = v[j];
                v[j] = v[j + 1];
                v[j + 1] = t;
            }
        }
    }
}

int main(void)
{
    volatile unsigned long long arr[16] = {
        0x0FULL, 0x03ULL, 0x2AULL, 0x11ULL, 0x07ULL, 0x1CULL, 0x05ULL, 0x30ULL,
        0x0AULL, 0x25ULL, 0x01ULL, 0x18ULL, 0x0DULL, 0x22ULL, 0x09ULL, 0x2FULL
    };

    bubble(arr, 16);

    /* 升序断言 + 首尾元素断言（手算：排序后最小 0x01、最大 0x30） */
    for (int i = 0; i < 15; i++)
        CHECK_G(__LINE__, arr[i] <= arr[i + 1]);
    CHECK_G(__LINE__, arr[0] == 0x01ULL);
    CHECK_G(__LINE__, arr[15] == 0x30ULL);

    /* 校验和（加法累加）：手算 16 元素之和 = 340 = 0x154 */
    unsigned long long sum = 0;
    for (int i = 0; i < 16; i++)
        sum += arr[i];
    CHECK_G(__LINE__, sum == 0x154ULL);

    /* 排序结果逐个输出（golden 由此固定） */
    for (int i = 0; i < 16; i++) {
        puthex(arr[i]);
        puts_("\n");
    }
    puthex(sum);
    puts_("\n");
    puts_("PASS t_bubble\n");
    return 0;
}
