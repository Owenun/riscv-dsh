#include "guest.h"
#include "guest_syscalls.h"
/* t_brk —— M7 分发 A（spec §8.4/§5，syscall 214）：brk 生命周期可观察验证。
   mini-crt 新原语 gbrk（guest_syscalls.h，内联 asm ecall）：brk(addr) 直传。断言（spec §5 语义）：
   1) brk(0) 返回当前 brk（查询语义）；
   2) brk(cur+8) 返回新 brk（返回值递增），且新扩展区域可写；
   3) 越界请求（addr 远超 brk_base+32MiB 上限 / addr < brk_base）返回值
      不变（失败不报错、返回当前 brk 的 Linux 语义），brk(0) 复核状态未变。
   本程序引用 gbrk —— tests/host/shim.c 无宿主对应实现 → 只进 run_guest
   （run_all.sh NATIVE_SKIP）。 */
static void fail(const char *why) {
    puts_("FAIL t_brk: "); puts_(why); gexit(1);
}
int main(void) {
    long cur = gbrk(0);                          /* addr=0：查询当前 brk */
    if (cur == 0) fail("query");
    long nxt = gbrk(cur + 8);
    if (nxt != cur + 8) fail("grow");            /* 返回新 brk（递增） */
    *(volatile char *)cur = 0x42;                /* 新扩展区域可写 */
    if (*(volatile char *)cur != 0x42) fail("rw");
    *(volatile char *)(cur + 7) = 0x77;
    if (*(volatile char *)(cur + 7) != 0x77) fail("rw2");
    if (gbrk(cur + (64L << 20)) != cur + 8) fail("oob");   /* 远超 32MiB 上限 */
    if (gbrk(1) != cur + 8) fail("below-base");  /* addr < brk_base → 拒绝 */
    if (gbrk(0) != cur + 8) fail("cur");         /* 状态复核：brk 未变 */
    puts_("PASS t_brk\n");
    return 0;
}
