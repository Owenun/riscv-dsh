/* softops.c —— bench 分发 A：RV64I 软算术运行时（解锁乘除）。
 *
 * gcc 在 -march=rv64i -mabi=lp64 -nostdlib 下遇到乘/除/取模会引用 libgcc
 * 符号（__muldi3/__divdi3/__umoddi3 等）；-nostdlib 不链接 libgcc，这里以
 * 纯移位-加法 / 移位-减法自备实现（正确性优先，不调用任何库函数/内建）。
 *
 * 全部公开符号标 __attribute__((weak))：宿主 native 交叉验证编译时，宿主
 * 自身的内建/强定义（若有引用）优先，互不冲突；guest 侧无其他定义，weak
 * 即唯一实现，链接后行为与强定义一致。
 *
 * 除上述 10 个软算术符号外，额外提供 __udivmodsi4/__udivmoddi4
 * （gcc 会把同操作数的除法与取模合并为对它的单次调用）。除零按确定性返回
 * 商 0 余 0（bench 程序不触发除零；不复制 libgcc 的 SIGFPE 语义）。
 *
 * 另附 memcpy/memset：-ffreestanding 下 gcc 允许为结构体赋值/聚合初始化
 * 生成对二者的 libcall（C 标准 freestanding 环境要求自备）；函数体以
 * -fno-tree-loop-distribute-patterns 编译，防止本体字节循环被再识别回
 * memcpy/memset 调用导致无穷递归。
 */
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                s32;
typedef long long          s64;

#define SO_WEAK __attribute__((weak))
#define SO_NO   __attribute__((optimize("-fno-tree-loop-distribute-patterns")))

/* ---------- 内部实现：无符号除法（65 位余数技巧，逐位恢复） ---------- */

/* 32 位：不变量 r < d（每轮 v = 2r+bit < 2d，故减后仍 < d）；
 * v 的第 32 位进位由 c 单独承载，纯 u32 算术即可精确完成。 */
static SO_NO u32 udivmodsi4(u32 n, u32 d, u32 *rp)
{
    u32 q = 0, r = 0;
    if (d == 0) { if (rp) *rp = 0; return 0; }
    for (int i = 31; i >= 0; i--) {
        u32 c = r >> 31;
        r = (r << 1) | ((n >> i) & 1u);
        q <<= 1;
        if (c || r >= d) { r -= d; q |= 1u; }
    }
    if (rp) *rp = r;
    return q;
}

static SO_NO u64 udivmoddi4(u64 n, u64 d, u64 *rp)
{
    u64 q = 0, r = 0;
    if (d == 0) { if (rp) *rp = 0; return 0; }
    for (int i = 63; i >= 0; i--) {
        u64 c = r >> 63;
        r = (r << 1) | ((n >> i) & 1u);
        q <<= 1;
        if (c || r >= d) { r -= d; q |= 1u; }
    }
    if (rp) *rp = r;
    return q;
}

/* ---------- 内部实现：移位-加法乘法（模 2^n 回绕即精确） ---------- */

static SO_NO u32 umulsi3(u32 a, u32 b)
{
    u32 r = 0;
    while (b) {
        if (b & 1u) r += a;
        a <<= 1;
        b >>= 1;
    }
    return r;
}

static SO_NO u64 umuldi3(u64 a, u64 b)
{
    u64 r = 0;
    while (b) {
        if (b & 1u) r += a;
        a <<= 1;
        b >>= 1;
    }
    return r;
}

/* ---------- libgcc 兼容符号（weak） ---------- */

SO_WEAK u32 __mulsi3(u32 a, u32 b) { return umulsi3(a, b); }
SO_WEAK u64 __muldi3(u64 a, u64 b) { return umuldi3(a, b); }

SO_WEAK u32 __udivsi3(u32 n, u32 d) { return udivmodsi4(n, d, 0); }
SO_WEAK u32 __umodsi3(u32 n, u32 d) { u32 r; (void)udivmodsi4(n, d, &r); return r; }
SO_WEAK u64 __udivdi3(u64 n, u64 d) { return udivmoddi4(n, d, 0); }
SO_WEAK u64 __umoddi3(u64 n, u64 d) { u64 r; (void)udivmoddi4(n, d, &r); return r; }
SO_WEAK u32 __udivmodsi4(u32 n, u32 d, u32 *rp) { return udivmodsi4(n, d, rp); }
SO_WEAK u64 __udivmoddi4(u64 n, u64 d, u64 *rp) { return udivmoddi4(n, d, rp); }

/* 有符号版本：截断除法（商向零取整，余数符号随被除数），与 libgcc 一致。 */
SO_WEAK s32 __divsi3(s32 n, s32 d)
{
    u32 un = (u32)(n < 0 ? 0u - (u32)n : (u32)n);
    u32 ud = (u32)(d < 0 ? 0u - (u32)d : (u32)d);
    u32 q = udivmodsi4(un, ud, 0);
    return (s32)(((n < 0) != (d < 0)) ? 0u - q : q);
}

SO_WEAK s32 __modsi3(s32 n, s32 d)
{
    u32 un = (u32)(n < 0 ? 0u - (u32)n : (u32)n);
    u32 ud = (u32)(d < 0 ? 0u - (u32)d : (u32)d);
    u32 r;
    (void)udivmodsi4(un, ud, &r);
    return (s32)((n < 0) ? 0u - r : r);
}

SO_WEAK s64 __divdi3(s64 n, s64 d)
{
    u64 un = (u64)(n < 0 ? 0ull - (u64)n : (u64)n);
    u64 ud = (u64)(d < 0 ? 0ull - (u64)d : (u64)d);
    u64 q = udivmoddi4(un, ud, 0);
    return (s64)(((n < 0) != (d < 0)) ? 0ull - q : q);
}

SO_WEAK s64 __moddi3(s64 n, s64 d)
{
    u64 un = (u64)(n < 0 ? 0ull - (u64)n : (u64)n);
    u64 ud = (u64)(d < 0 ? 0ull - (u64)d : (u64)d);
    u64 r;
    (void)udivmoddi4(un, ud, &r);
    return (s64)((n < 0) ? 0ull - r : r);
}

/* ---------- freestanding 必备的块操作（weak，供 gcc libcall 落点） ---------- */

SO_WEAK SO_NO void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) { *d++ = *s++; }
    return dst;
}

SO_WEAK SO_NO void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) { *d++ = (unsigned char)c; }
    return dst;
}
