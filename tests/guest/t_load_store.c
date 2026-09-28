/* t_load_store: 访存 11 条（lb/lbu/lh/lhu/lw/lwu/ld/sb/sh/sw/sd）全宽度正例。
   通过 volatile 限定的定宽左值触发对应宽度指令：
   - 带符号 volatile 数组（signed char/short/int/long long）→ 符号扩展加载
     （lb/lh/lw/ld），正负两个方向；
   - 无符号 volatile 数组 → 零扩展加载（lbu/lhu/lwu）与全部存储（sb/sh/sw/sd）；
   - LW/LWU 同位型对照：0x80000000 的两种 64 位视图；
   - 写宽度精确性：store 后邻居单元仍为 0（静态数组零初始化）。
   说明：对齐/未映射故障反例属 rvsim 语义（C 层不产生未对齐访问），由 L0
   test_memops.c 覆盖，不进 guest 程序。
   覆盖口径申报（M3 修复轮，如实申报局限）：优化器会折叠访存——交叉 gcc -O2
   把 lb/lh 优化为 lbu/lhu+移位（符号扩展重构）、lwu 折入 lw（高 32 位无关）。
   本程序 guest ELF 实测（objdump 全量统计）：lbu/lhu/lw/ld/sb/sh/sw/sd 有，
   lb/lh/lwu 各 0 条，即 guest 层实际只执行 8/11 种访存指令。因此 lb/lh/lwu
   的执行臂语义不由本程序钉死，而由 L0 test_memops.c 直接编码逐条钉死
   （符号/零扩展两方向对照）；M5 rvsim 直跑本 ELF 也不构成 lb/lh/lwu 覆盖，
   如需指令级覆盖须后续补不受优化影响的 guest 用例。 */
#include "guest.h"

static volatile signed char        s_b[4];
static volatile unsigned char      u_b[4];
static volatile short              s_h[4];
static volatile unsigned short     u_h[4];
static volatile int                s_w[4];
static volatile unsigned int       u_w[4];
static volatile long long          s_d[2];
static volatile unsigned long long u_d[2];

int main(void)
{
    /* SB + LB/LBU：字节宽度、符号两方向、邻居不动 */
    s_b[0] = (signed char)-128;                /* SB，位型 0x80 */
    s_b[1] = 127;                              /* SB，位型 0x7F */
    u_b[0] = 0x80;                             /* SB，同位型供 LBU 对照 */
    CHECK_G(__LINE__, (long long)s_b[0] == -128LL);            /* LB 符号扩展 */
    CHECK_G(__LINE__, (long long)s_b[1] == 127LL);             /* LB 正方向 */
    CHECK_G(__LINE__, (unsigned long long)u_b[0] == 0x80ULL);  /* LBU 零扩展对照 */
    CHECK_G(__LINE__, s_b[2] == 0 && s_b[3] == 0);             /* SB 邻居不动 */

    /* SH + LH/LHU：半字宽度、符号两方向 */
    s_h[0] = -32768;                           /* SH，位型 0x8000 */
    s_h[1] = 32767;
    u_h[0] = 0x8000;                           /* SH，同位型供 LHU 对照 */
    CHECK_G(__LINE__, (long long)s_h[0] == -32768LL);               /* LH 符号扩展 */
    CHECK_G(__LINE__, (long long)s_h[1] == 32767LL);
    CHECK_G(__LINE__, (unsigned long long)(long long)s_h[0] == 0xFFFFFFFFFFFF8000ULL);
    CHECK_G(__LINE__, (unsigned long long)u_h[0] == 0x8000ULL);     /* LHU 零扩展对照 */
    CHECK_G(__LINE__, s_h[2] == 0 && u_h[3] == 0);                  /* SH 邻居不动 */

    /* SW + LW/LWU：同位型 0x80000000 的两种 64 位视图（spec §8.3 对照项） */
    u_w[0] = 0x80000000U;                      /* SW */
    s_w[0] = -2147483647 - 1;                  /* SW，同位型 0x80000000（C99 范围内常量） */
    s_w[1] = 2147483647;                       /* SW，正方向 */
    CHECK_G(__LINE__, (unsigned long long)u_w[0] == 0x0000000080000000ULL);  /* LWU 零扩展 */
    CHECK_G(__LINE__, (unsigned long long)(long long)s_w[0] == 0xFFFFFFFF80000000ULL); /* LW 符号扩展 */
    CHECK_G(__LINE__, (long long)s_w[1] == 2147483647LL);        /* LW 正方向 */
    CHECK_G(__LINE__, s_w[2] == 0 && u_w[3] == 0);               /* SW 邻居不动 */

    /* SD + LD：全宽往返、负值符号保持 */
    s_d[0] = -123456789012345LL;
    u_d[0] = 0x1122334455667788ULL;
    CHECK_G(__LINE__, s_d[0] == -123456789012345LL);             /* LD 全宽回读 */
    CHECK_G(__LINE__, u_d[0] == 0x1122334455667788ULL);
    CHECK_G(__LINE__, s_d[1] == 0 && u_d[1] == 0);               /* SD 邻居不动 */
    u_d[1] = 0x8000000000000000ULL;
    CHECK_G(__LINE__, u_d[1] == 0x8000000000000000ULL);

    puts_("PASS t_load_store\n");
    return 0;
}
