/* t_maxsteps —— M7 分发 A（spec §8.4/§6.1/§7）：--max-steps 步限触发 207。
   死循环（gcc -O2 下为单条自跳指令）永不退出，只能由 rvsim --max-steps
   终止：run_guest_err 传 --max-steps 1000，期望退出码 207 + stderr 首行
   `rvsim: step-limit`；golden stdout 为空（0 字节）。
   宿主原生执行不会终止 → NATIVE_SKIP 排除（只走 run_guest_err）。 */
int main(void) {
    for (;;) {
    }
}
