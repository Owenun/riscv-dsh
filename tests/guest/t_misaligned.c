#include "guest.h"
/* t_misaligned —— M7 分发 A（spec §8.4/§7）：未对齐访存 → 203。
   C 层无法可靠让编译器生成未对齐 load（-O2 会合成字节访问/改写寻址），
   沿 gbadsys 同款手法用内联 asm 固定发出一条 LD：地址 = .sbss 变量地址
   +1 —— 变量本身 8 对齐且在已映射 ELF 段内，偏移 +1 → 8 字节访问未对齐
   （对齐检查先于映射检查的次序由 L0 test_memops 钉死）。rvsim 打印
   `rvsim: fault misaligned pc=… addr=… align=8`（stderr）并以 203 终止 ——
   golden stdout 为空（0 字节），走 run_guest_err。
   NATIVE_SKIP 排除：宿主 x86-64 支持非对齐 load，语义不同构。 */
static volatile long sink;
int main(void) {
    unsigned long a = (unsigned long)&sink + 1;
    long v;
    __asm__ volatile ("ld %0, 0(%1)" : "=r"(v) : "r"(a));
    return (int)v;               /* 不可达（故障终止）；使用 v 防 dead-code 消除 */
}
