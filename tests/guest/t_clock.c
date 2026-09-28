#include "guest.h"
#include "guest_syscalls.h"
/* t_clock —— M7 分发 A（spec §8.4/§5，syscall 113）：clock_gettime 双时钟。
   断言：id=0(CLOCK_REALTIME)/1(CLOCK_MONOTONIC) 均返回 0 且 tv_sec>0、
   0<=tv_nsec<1e9；REALTIME 两次调用 (tv_sec,tv_nsec) 二元组单调不减
   （REALTIME 可能同秒，故断言二元组不减而非严格递增）；id=2 → -EINVAL。
   只进 run_guest（NATIVE_SKIP：shim.c 无 clock_gettime 实现）。 */
#define E_INVAL (-22)
struct ts { long sec; long nsec; };          /* spec §5：{i64 tv_sec; i64 tv_nsec} */
static void fail(const char *why) {
    puts_("FAIL t_clock: "); puts_(why); gexit(1);
}
static int ts_geq(struct ts a, struct ts b) {            /* 二元组 a >= b */
    if (a.sec != b.sec) return a.sec > b.sec;
    return a.nsec >= b.nsec;
}
int main(void) {
    struct ts a, b, c;
    if (gclock(0, &a) != 0) fail("realtime");
    if (gclock(1, &b) != 0) fail("monotonic");
    if (a.sec <= 0 || b.sec <= 0) fail("sec");
    if (a.nsec < 0 || a.nsec >= 1000000000L) fail("nsec-a");
    if (b.nsec < 0 || b.nsec >= 1000000000L) fail("nsec-b");
    if (gclock(0, &c) != 0) fail("realtime2");
    if (!ts_geq(c, a)) fail("non-decreasing");           /* 二元组单调不减 */
    if (gclock(2, &c) != E_INVAL) fail("bad-id");
    puts_("PASS t_clock\n");
    return 0;
}
