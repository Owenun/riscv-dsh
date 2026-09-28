#include "guest.h"
/* bench 分发 A：puts_/puthex 改 weak——guest 侧无其他定义，weak 即唯一
 * 实现（行为不变）；native 交叉验证编译时 tests/host/shim.c 的同名强
 * 定义优先，避免双重定义链接错误。 */
__attribute__((weak)) void puts_(const char *s) { unsigned long n = 0; while (s[n]) n++; gwrite(1, s, n); }
__attribute__((weak)) void puthex(unsigned long v) {
    char b[18]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> 60) & 0xF);
        b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v <<= 4;
    }
    gwrite(1, b, 18);
}

/* bench 分发 A：mini-crt 字符串补充（语义与 C 标准一致；guest 端 freestanding
 * 无 libc，供 bench 与后续测试使用；既有 t_*.c 不受影响） */
char *xstrcpy(char *dst, const char *src)
{
    char *p = dst;
    while ((*p++ = *src++) != 0) { }
    return dst;
}

int xstrcmp(const char *a, const char *b)
{
    while (*a != 0 && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

unsigned long xstrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n] != 0) n++;
    return n;
}
