#include "guest.h"
#include "guest_syscalls.h"
/* t_uname —— M7 分发 A（spec §8.4/§5，syscall 160）：uname 缓冲区 390 字节。
   断言：返回 0；sysname=="Linux"、machine=="riscv64"、release 非空；
   字段 65 字节 NUL 填充（sysname 尾 buf[64]=='\0'）。
   只进 run_guest（NATIVE_SKIP：shim.c 无 uname 实现）。 */
static void fail(const char *why) {
    puts_("FAIL t_uname: "); puts_(why); gexit(1);
}
static int eq_field(const char *buf, const char *want) {
    int i;
    for (i = 0; want[i] != '\0'; i++)
        if (buf[i] != want[i]) return 0;
    return buf[i] == '\0';                   /* 字段内 NUL 终止 */
}
int main(void) {
    char buf[390];
    int i;
    for (i = 0; i < 390; i++)                /* 非零预填充：验证整块写入 */
        buf[i] = (char)(unsigned char)i;
    if (guname(buf) != 0) fail("ret");
    if (!eq_field(buf, "Linux")) fail("sysname");
    if (!eq_field(buf + 260, "riscv64")) fail("machine");
    if (buf[130] == '\0') fail("release-empty");   /* release 非空 */
    if (buf[64] != '\0') fail("pad");        /* sysname 字段 65 字节 NUL 填充 */
    puts_("PASS t_uname\n");
    return 0;
}
