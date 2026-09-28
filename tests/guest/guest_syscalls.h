#ifndef GUEST_SYSCALLS_H
#define GUEST_SYSCALLS_H
/* guest_syscalls.h —— M7 分发 A：扩展 syscall 原语（static inline + 内联 asm，
   spec §5 子集，供 §8.4 程序清单 t_brk/t_mmap/t_uname/t_clock/t_badcall 使用）。
   设计说明：曾评估在 guest_sys.S 追加汇编原语，但 mini-crt 位于链接序最前，
   追加代码会使 guest_lib/t_*.c 全部代码地址整体后移，破坏 snap_trace/
   snap_regs/dbg_session* 逐字手核的 pc 快照 golden；改为头内 static inline
   （ecall 直发），只随使用它的程序编译 —— 既有 14 个 ELF 位级不变，golden
   全部稳定。使用这些原语的程序引用 shim.c 没有的符号路径 → 只进 rvsim
   执行路径（run_all.sh NATIVE_SKIP）。 */

__attribute__((unused)) static long gbrk(long addr)                      /* 214：新 brk / 当前 brk */
{
    register long a0 asm("a0") = addr;
    register long a7 asm("a7") = 214;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

__attribute__((unused)) static long gmmap(long addr, unsigned long len, long prot, long flags,
                  long fd, long off)             /* 222：匿名私有 mmap */
{
    register long a0 asm("a0") = addr;
    register long a1 asm("a1") = (long)len;
    register long a2 asm("a2") = prot;
    register long a3 asm("a3") = flags;
    register long a4 asm("a4") = fd;
    register long a5 asm("a5") = off;
    register long a7 asm("a7") = 222;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                  : "memory");
    return a0;
}

__attribute__((unused)) static long gmunmap(long addr, unsigned long len)            /* 215 */
{
    register long a0 asm("a0") = addr;
    register long a1 asm("a1") = (long)len;
    register long a7 asm("a7") = 215;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

__attribute__((unused)) static long gclock(long id, void *tp)                        /* 113 */
{
    register long a0 asm("a0") = id;
    register long a1 asm("a1") = (long)tp;
    register long a7 asm("a7") = 113;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

__attribute__((unused)) static long guname(void *buf)                                /* 160 */
{
    register long a0 asm("a0") = (long)buf;
    register long a7 asm("a7") = 160;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

/* 未知号码：rvsim 打 `rvsim: unsupported syscall N pc=…` 并以 205 终止，
   本函数实际不可返回（返回路径仅为汇编完整性）。 */
__attribute__((unused)) static long gbadsys(long num)
{
    register long a0 asm("a0") = 0;
    register long a1 asm("a1") = 0;
    register long a2 asm("a2") = 0;
    register long a3 asm("a3") = 0;
    register long a4 asm("a4") = 0;
    register long a5 asm("a5") = 0;
    register long a7 asm("a7") = num;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                  : "memory");
    return a0;
}
#endif
