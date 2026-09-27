#include "guest.h"
/* t_argv —— Task 11/M5 分发 B：argc/argv/envp/auxv 透传验证（spec §3.4）。
   run_all.sh 固定传入 guest-args "one two"（run_guest t_argv one two；
   L1-native/L2 侧由 extra_args 传同一组），故 argc=3：
   argv[0]=ELF/宿主二进制路径（两侧必然不同，故不打印）、argv[1]=one、
   argv[2]=two、argv[3]=NULL；envp 以 NULL 终止；auxv 须含 AT_PAGESZ(6,4096)
   与 AT_RANDOM(25,非0 指针) 且以 AT_NULL(0,0) 终止（glibc 原生进程栈同布局，
   两侧语义一致，可差分）。
   golden（tests/golden/t_argv.out，手算固定）：PASS t_argv n=3 a1=one a2=two
   任何不符 → FAIL 行 + 退出码 1。 */
static unsigned long xstrlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void gput_str(const char *s) { gwrite(1, s, xstrlen(s)); }
static void gput_dec(unsigned long v) {
    char b[20]; int i = 20;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    gwrite(1, b + i, (unsigned long)(20 - i));
}
static void fail(const char *why) {
    gput_str("FAIL t_argv: "); gput_str(why); gput_str("\n");
    gexit(1);
}
int main(int argc, char **argv) {
    unsigned long *aux;
    const unsigned char *rnd = 0;
    int i, seen_pagesz = 0, seen_random = 0;

    if (argc != 3) fail("argc");
    if (argv[3] != 0) fail("argv-null");             /* argv[] 以 NULL 结尾 */
    /* envp：NULL 终止（rvsim 侧为单 NULL 空表；native 侧继承宿主环境变量，
       非 rvsim 特有的单 NULL 检查放 L0 test_stack 手算核对） */
    {
        char **ep = argv + argc + 1;
        for (i = 0; i < 4096 && *ep != 0; i++, ep++) { }
        if (*ep != 0) fail("envp-unterminated");
        aux = (unsigned long *)(ep + 1);
    }

    /* auxv（envp NULL 之后，16 字节/对）：AT_PAGESZ=6 → 4096；
       AT_RANDOM=25 → 非 0 指针；AT_NULL=0 终止（上限 32 对防野走） */
    for (i = 0; i < 32 && aux[0] != 0; i++, aux += 2) {
        if (aux[0] == 6 && aux[1] == 4096) seen_pagesz = 1;
        if (aux[0] == 25 && aux[1] != 0) { seen_random = 1; rnd = (const unsigned char *)aux[1]; }
    }
    if (aux[0] != 0) fail("auxv-unterminated");
    if (!seen_pagesz) fail("auxv-pagesz");
    if (!seen_random) fail("auxv-random");

    gput_str("PASS t_argv n="); gput_dec((unsigned long)argc);
    gput_str(" a1="); gput_str(argv[1]);
    gput_str(" a2="); gput_str(argv[2]); gput_str("\n");
    return 0;
}
