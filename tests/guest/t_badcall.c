#include "guest.h"
#include "guest_syscalls.h"
/* t_badcall —— M7 分发 A（spec §8.4/§5/§7）：未知 syscall 号 → 205。
   mini-crt 新原语 gbadsys（guest_syscalls.h，内联 asm：a0=号码 → a7、a1..a5 清零、ecall）。
   号码 9999 不在 spec §5 表中：rvsim 打印
   `rvsim: unsupported syscall 9999 pc=0x…`（stderr）并以 205 终止 ——
   本程序自身到不了 PASS 输出：golden stdout 为空（0 字节），由
   run_guest_err 断言退出码 205 + stderr 首行前缀。
   只进 run_guest_err（NATIVE_SKIP：宿主侧无"未知号码拒绝"语义）。 */
int main(void) {
    gbadsys(9999);
    return 0;                                /* 不可达：rvsim 已 205 终止 */
}
