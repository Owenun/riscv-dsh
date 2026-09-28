#include "guest.h"
#include "guest_syscalls.h"
/* t_mmap —— M7 分发 A（spec §8.4/§5，syscall 222/215）：匿名 mmap/munmap 往返。
   语义锚点（L0 test_syscall 钉死的 bump/尾块回退，guest 层复核）：
   1) 连续 mmap 自 MMAP_BASE bump 页对齐分配（第二块首地址 = 第一块首地址 +
      取整后长度）；
   2) 匿名零页：新区域读为 0、写入可回读；
   3) munmap 仅精确匹配（addr 与 len 均须相等）→ 部分长度 -EINVAL；
   4) 顶块回收后 mmap_top 回退 → 再 mmap 复用首地址；
   5) len=0 / 缺 MAP_ANONYMOUS / fd != -1 → -EINVAL；
   6) M-4 备注（本轮 spec 增注）：flags 允许携带其他位（按位与判定）、
      addr 提示忽略。
   只进 run_guest（NATIVE_SKIP：shim.c 无 mmap/munmap 实现）。 */
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define E_INVAL       (-22)
static void fail(const char *why) {
    puts_("FAIL t_mmap: "); puts_(why); gexit(1);
}
int main(void) {
    long p1 = gmmap(0, 8192, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p1 < 0) fail("mmap1");
    if ((p1 & 4095) != 0) fail("align1");                /* 页对齐返回 */
    if (*(volatile char *)p1 != 0) fail("zero1");        /* 匿名零页 */
    *(volatile char *)p1 = 0x5a;
    if (*(volatile char *)p1 != 0x5a) fail("rw1");
    long p2 = gmmap(0, 4096, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p2 != p1 + 8192) fail("bump");                   /* bump 顺序分配 */
    if (gmunmap(p1, 4096) != E_INVAL) fail("munmap-partial");  /* len 不精确 */
    if (gmunmap(p2, 4096) != 0) fail("munmap2");         /* 顶块回收 */
    if (gmunmap(p1, 8192) != 0) fail("munmap1");
    /* M-4：addr 提示忽略 + flags 多余位允许 */
    long p3 = gmmap(0x12340000, 4096, 3,
                    (MAP_PRIVATE | MAP_ANONYMOUS) | 0x100, -1, 0);
    if (p3 != p1) fail("reuse");                         /* 尾块回退复用首地址 */
    if (gmmap(0, 0, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != E_INVAL) fail("len0");
    if (gmmap(0, 4096, 3, MAP_PRIVATE, -1, 0) != E_INVAL) fail("flags");   /* 缺 ANON */
    if (gmmap(0, 4096, 3, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0) != E_INVAL) fail("fd");
    puts_("PASS t_mmap\n");
    return 0;
}
