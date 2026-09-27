/* t_string: 手写 strlen/memcpy/memset 循环（无 libc）语义验证。
 * 覆盖口径（objdump 实测：bne=12 bltu=1 jal=25 lui=12，口径见 task-9b 分发 B 报告）：
 * bne（strlen/memcpy/memset 循环回边与长度分支）、jal（noinline 函数调用）。
 * noinline 防止 gcc 对字面量长度/拷贝做编译期常量折叠；字符串指针经 volatile 中转。
 * 纯 RV64I，禁 M/A 扩展；C 层无乘除。
 */
#include "guest.h"

static __attribute__((noinline)) unsigned long g_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static __attribute__((noinline)) void g_memcpy(unsigned char *d, const unsigned char *s, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
}

static __attribute__((noinline)) void g_memset(unsigned char *d, unsigned char c, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++)
        d[i] = c;
}

int main(void)
{
    /* strlen：常量串长度经 volatile 指针读取，运行期才可计算 */
    volatile const char *msg = "rvsim-m4-x";
    volatile unsigned long len_expect = 10;
    unsigned long len = g_strlen((const char *)msg);
    CHECK_G(__LINE__, len == len_expect);
    CHECK_G(__LINE__, g_strlen("") == 0);

    /* memcpy：16 字节逐字节搬运后全量比对 */
    unsigned char src[16], dst[16];
    volatile unsigned long n16 = 16;
    for (unsigned long i = 0; i < 16; i++)
        src[i] = (unsigned char)(i + 1);
    for (unsigned long i = 0; i < 16; i++)
        dst[i] = 0;
    g_memcpy(dst, src, n16);
    for (unsigned long i = 0; i < 16; i++)
        CHECK_G(__LINE__, dst[i] == (unsigned char)(i + 1));

    /* memcpy 零长度：目标保持原值 */
    dst[0] = 0x55;
    g_memcpy(dst, src, 0);
    CHECK_G(__LINE__, dst[0] == 0x55);

    /* memset：12 字节写 0xAB，第 13 字节起不受影响 */
    g_memset(dst, 0xAB, 12);
    for (unsigned long i = 0; i < 12; i++)
        CHECK_G(__LINE__, dst[i] == 0xAB);
    CHECK_G(__LINE__, dst[12] == 13);

    /* memset 零长度：目标保持原值 */
    g_memset(dst + 12, 0xFF, 0);
    CHECK_G(__LINE__, dst[12] == 13);

    puthex(len);
    puts_("\n");
    puts_("PASS t_string\n");
    return 0;
}
