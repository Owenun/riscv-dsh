/* tests/test_syscall.c —— Task 10（M5 分发 A）：syscall 模块 9 项 + cpu 系统臂
   （FENCE/FENCE.I/ECALL/EBREAK）。
   语义权威：docs/specs/2026-09-27-rvsim-design.md §5（syscall 表）+ §4.2（系统组）。
   覆盖：
   - write：fd=1/2 经 host_fd 管道重定向成功、部分写（非阻塞管道）、EBADF、
     buf 未映射 → -EFAULT（设计申报：指针参数坏指针返回 -14，非 202，见报告）；
     count=0 不检查 buf → 0（Linux 语义，修复轮 1/M-1）；
   - read：fd=0 经 host_fd 转发（含 EOF=0）、fd≠0 → -9、count=0 不检查 buf → 0
     （修复轮 1/M-1）；
   - exit/exit_group：code&0xff、kind=1；
   - brk：正常扩展（heap 登记 hi 更新且区间可写）、addr=0/越界(<base、>base+CAP)
      返回当前 brk、端点 base+HEAP_CAP 合法、回缩；
   - mmap：bump 连续分配页对齐、len=0 → -22、非法 flags/fd → -22、总量超
      MMAP_CAP → -12、区域表满（n=64，台账 R12）→ -12；
   - munmap：精确匹配回收（再 mmap 复用首地址、复用区零页）、非精确 → -22；
   - clock_gettime：id=0/1 → 0 且 tv_sec>0、tv_nsec<1e9（小端序列化）、
      id=99 → -22、tp 未映射 → -14；
   - uname：390 字节 6×65 布局，sysname/nodename/machine 断言 + NUL/余零 + 越界哨兵；
   - 未知号码 999 → kind=2；
   - cpu 臂：FENCE/FENCE.I 无副作用退休；EBREAK → exited/206 且 pc/steps 不动；
      ECALL exit(42) → exited/42；ECALL 未知 → exited/205 + sc_unsupported 记录号码；
      ECALL write 全路径（寄存器 → a0 写回）。
   测试自建 heap 登记（kind=1）与 brk_base 赋值，作为 main 装配（分发 B）的替身。 */
#include "harness.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define DBASE 0x0000000000020000ULL   /* 数据区（guest 缓冲） */
#define DSZ   0x0000000000040000ULL   /* 256 KiB，够部分写用例 */
#define BRK0  0x0000000000100000ULL   /* 测试用 heap 基址 */

static void sc_reset(void)
{
    reset_sim();
    mem_add_region(&hs.mem, DBASE, DBASE + DSZ, 0);  /* 数据区 */
    hs.brk_base = BRK0;
    hs.brk_cur = BRK0;
    mem_add_region(&hs.mem, BRK0, BRK0, 1);          /* heap 区：分发 B main 装配的替身 */
}

static rv_sc_ret sc(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4)
{
    return rv_syscall(&hs, num, a0, a1, a2, a3, a4, 0);
}

/* 从 guest 内存读 n 字节到宿主 buf（断言成功） */
static void gread(uint64_t addr, void *dst, uint64_t n)
{
    CHECK(mem_read(&hs.mem, addr, dst, n) == 0);
}

/* guest 小端视图读 u64（与 syscall.c 的 put_le64 序列化约定对应） */
static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

/* 往 guest 内存写 n 字节（断言成功） */
static void gwrite(uint64_t addr, const void *src, uint64_t n)
{
    CHECK(mem_write(&hs.mem, addr, src, n) == 0);
}

int main(void)
{
    /* ---- rvsim_reset：新字段初始化（host_fd 默认 {0,1,2}、mmap_top=MMAP_BASE、
        brk 置 0 待 main 装配） ---- */
    reset_sim();
    CHECK(hs.host_fd[0] == 0 && hs.host_fd[1] == 1 && hs.host_fd[2] == 2);
    CHECK(hs.mmap_top == MMAP_BASE);
    CHECK(hs.brk_base == 0 && hs.brk_cur == 0);
    CHECK(hs.sc_unsupported == false);

    /* ---- write：fd=1 经管道重定向，成功返回 2 且对端收到 ---- */
    {
        int p[2];
        uint8_t buf[8] = { 0 };
        sc_reset();
        CHECK(pipe(p) == 0);
        hs.host_fd[1] = p[1];
        gwrite(DBASE, "hi", 2);
        rv_sc_ret r = sc(64, 1, DBASE, 2, 0, 0);
        CHECK(r.kind == 0 && r.val == 2);
        CHECK(read(p[0], buf, 8) == 2 && memcmp(buf, "hi", 2) == 0);
        close(p[0]);
        close(p[1]);
    }

    /* ---- write：fd=2 同样经 host_fd[2]（重定向到管道） ---- */
    {
        int p[2];
        uint8_t buf[8] = { 0 };
        sc_reset();
        CHECK(pipe(p) == 0);
        hs.host_fd[2] = p[1];
        gwrite(DBASE, "err", 3);
        rv_sc_ret r = sc(64, 2, DBASE, 3, 0, 0);
        CHECK(r.kind == 0 && r.val == 3);
        CHECK(read(p[0], buf, 8) == 3 && memcmp(buf, "err", 3) == 0);
        close(p[0]);
        close(p[1]);
    }

    /* ---- write：部分写（非阻塞管道、请求超管道容量）→ 返回 0<n<count ---- */
    {
        int p[2];
        sc_reset();
        CHECK(pipe(p) == 0);
        CHECK(fcntl(p[1], F_SETFL, O_NONBLOCK) == 0);
        hs.host_fd[1] = p[1];
        rv_sc_ret r = sc(64, 1, DBASE, 200000, 0, 0);
        CHECK(r.kind == 0 && r.val > 0 && r.val < 200000);
        close(p[0]);
        close(p[1]);
    }

    /* ---- write：EBADF（fd=5、fd=0 不属于 {1,2}） ---- */
    sc_reset();
    CHECK(sc(64, 5, DBASE, 2, 0, 0).val == -9);
    CHECK(sc(64, 0, DBASE, 2, 0, 0).val == -9);

    /* ---- write：buf 未映射 → -EFAULT(-14)（设计申报） ---- */
    sc_reset();
    CHECK(sc(64, 1, DBASE + DSZ, 2, 0, 0).val == -14);

    /* ---- 修复轮 1（M-1）：count=0 不检查 buf（Linux 语义）→ 返回 0 ----
       地址必须取真正未映射处（0x80000 位于数据区上沿 0x60000 与 heap 0x100000
       之间的空隙；DBASE+DSZ 恰为数据区右端点 hi，len=0 语义下属合法，测不出红） */
    sc_reset();
    CHECK(sc(64, 1, 0x80000ULL, 0, 0, 0).val == 0);     /* write：未映射 buf + count=0 → 0 */
    CHECK(sc(63, 0, 0x80000ULL, 0, 0, 0).val == 0);     /* read：未映射 buf + count=0 → 0 */

    /* ---- read：fd=0 经管道转发；EOF → 0；fd≠0 → -9 ---- */
    {
        int p[2];
        uint8_t buf[8] = { 0 };
        sc_reset();
        CHECK(pipe(p) == 0);
        hs.host_fd[0] = p[0];
        CHECK(write(p[1], "abcd", 4) == 4);
        rv_sc_ret r = sc(63, 0, DBASE, 4, 0, 0);
        CHECK(r.kind == 0 && r.val == 4);
        gread(DBASE, buf, 4);
        CHECK(memcmp(buf, "abcd", 4) == 0);
        close(p[1]);                        /* 关写端 → 下一次 read 得 EOF */
        r = sc(63, 0, DBASE, 4, 0, 0);
        CHECK(r.kind == 0 && r.val == 0);
        CHECK(sc(63, 3, DBASE, 4, 0, 0).val == -9);
        close(p[0]);
    }

    /* ---- exit/exit_group：kind=1、code&0xff ---- */
    sc_reset();
    {
        rv_sc_ret r = sc(93, 42, 0, 0, 0, 0);
        CHECK(r.kind == 1 && r.val == 42);
        r = sc(94, 300, 0, 0, 0, 0);        /* 300&0xff=44 */
        CHECK(r.kind == 1 && r.val == 44);
        r = sc(93, 0, 0, 0, 0, 0);
        CHECK(r.kind == 1 && r.val == 0);
    }

    /* ---- brk：初始 cur==base；正常扩展登记区且新区间可写；端点 CAP 合法 ---- */
    {
        uint8_t buf[4] = { 0 };
        sc_reset();
        CHECK(hs.brk_cur == BRK0);
        rv_sc_ret r = sc(214, BRK0 + 4096, 0, 0, 0, 0);
        CHECK(r.kind == 0 && r.val == BRK0 + 4096);
        CHECK(hs.brk_cur == BRK0 + 4096);
        CHECK(mem_check(&hs.mem, BRK0, 4096) == 0);      /* heap 区登记扩大 */
        gwrite(BRK0, "heap", 4);
        gread(BRK0, buf, 4);
        CHECK(memcmp(buf, "heap", 4) == 0);
        /* 恰到 base+HEAP_CAP（含端点，spec §5 [brk_base, brk_base+32MiB]） */
        r = sc(214, BRK0 + HEAP_CAP, 0, 0, 0, 0);
        CHECK(r.kind == 0 && r.val == BRK0 + HEAP_CAP);
        /* 回缩合法 */
        r = sc(214, BRK0 + 2048, 0, 0, 0, 0);
        CHECK(r.kind == 0 && r.val == BRK0 + 2048);
        CHECK(mem_check(&hs.mem, BRK0, 2048) == 0);
        CHECK(mem_check(&hs.mem, BRK0 + 2048, 1) != 0);  /* 回缩后右端开放 */
    }

    /* ---- brk：addr=0 / <base / >base+CAP → 返回当前 brk 且状态不变 ---- */
    {
        sc_reset();
        CHECK(sc(214, BRK0 + 4096, 0, 0, 0, 0).val == BRK0 + 4096);
        CHECK(sc(214, 0, 0, 0, 0, 0).val == BRK0 + 4096);            /* addr=0 → 查询 */
        CHECK(sc(214, BRK0 - 1, 0, 0, 0, 0).val == BRK0 + 4096);     /* 低于 base */
        CHECK(sc(214, BRK0 + HEAP_CAP + 1, 0, 0, 0, 0).val == BRK0 + 4096);
        CHECK(hs.brk_cur == BRK0 + 4096);                            /* 全部未生效 */
    }

    /* ---- mmap：bump 连续两次，页对齐，非页对齐 len 向上取整 ---- */
    {
        sc_reset();
        rv_sc_ret r1 = sc(222, 0, 8192, 3, 0x22, (uint64_t)-1);
        CHECK(r1.kind == 0 && r1.val == MMAP_BASE);
        CHECK(mem_check(&hs.mem, MMAP_BASE, 8192) == 0);
        rv_sc_ret r2 = sc(222, 0, 8192, 3, 0x22, (uint64_t)-1);
        CHECK(r2.kind == 0 && r2.val == MMAP_BASE + 8192);
        rv_sc_ret r3 = sc(222, 0, 100, 3, 0x22, (uint64_t)-1);       /* 取整到 4096 */
        CHECK(r3.kind == 0 && r3.val == MMAP_BASE + 16384);
        CHECK(mem_check(&hs.mem, MMAP_BASE + 16384, 4096) == 0);
    }

    /* ---- mmap：len=0 / flags 缺位 / fd≠-1 → -22 ---- */
    sc_reset();
    CHECK(sc(222, 0, 0, 3, 0x22, (uint64_t)-1).val == -22);          /* len=0 */
    CHECK(sc(222, 0, 4096, 3, 0x03, (uint64_t)-1).val == -22);       /* 缺 ANONYMOUS */
    CHECK(sc(222, 0, 4096, 3, 0x20, (uint64_t)-1).val == -22);       /* 缺 PRIVATE */
    CHECK(sc(222, 0, 4096, 3, 0x22, 0).val == -22);                  /* fd≠-1 */

    /* ---- mmap：总量超 MMAP_CAP → -12（len 直接超限 + 累计超限） ---- */
    {
        sc_reset();
        CHECK(sc(222, 0, MMAP_CAP + 1, 3, 0x22, (uint64_t)-1).val == -12);
        CHECK(sc(222, 0, MMAP_CAP - 4096, 3, 0x22, (uint64_t)-1).val == MMAP_BASE);
        CHECK(sc(222, 0, 8192, 3, 0x22, (uint64_t)-1).val == -12);   /* 再放不下 */
        CHECK(hs.mmap_top == MMAP_BASE + MMAP_CAP - 4096);           /* 失败不前进 */
    }

    /* ---- mmap：区域表满（n>=RVSIM_RGN_MAX=64，台账 R12）→ -12 ---- */
    {
        sc_reset();
        while (hs.mem.n < RVSIM_RGN_MAX)   /* 填满区域表（数据/heap 各占 1 + 追加） */
            mem_add_region(&hs.mem, 0x40000ULL + 0x1000ULL * hs.mem.n,
                           0x41000ULL + 0x1000ULL * hs.mem.n, 2);
        CHECK(hs.mem.n == RVSIM_RGN_MAX);
        CHECK(sc(222, 0, 4096, 3, 0x22, (uint64_t)-1).val == -12);
    }

    /* ---- munmap：精确回收 → 再 mmap 复用首地址且零页；非精确 → -22 ---- */
    {
        uint8_t pat[8] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
        uint8_t buf[8];
        sc_reset();
        CHECK(sc(222, 0, 8192, 3, 0x22, (uint64_t)-1).val == MMAP_BASE);
        gwrite(MMAP_BASE, pat, 8);                                   /* 弄脏旧区 */
        CHECK(sc(215, MMAP_BASE, 4096, 0, 0, 0).val == -22);         /* len 非精确 */
        CHECK(sc(215, MMAP_BASE + 4096, 4096, 0, 0, 0).val == -22);  /* addr 非精确 */
        CHECK(sc(215, DBASE, 4096, 0, 0, 0).val == -22);             /* 非 mmap 区 */
        rv_sc_ret rm = sc(215, MMAP_BASE, 8192, 0, 0, 0);            /* 精确回收 */
        CHECK(rm.kind == 0 && rm.val == 0);
        CHECK(mem_check(&hs.mem, MMAP_BASE, 8) != 0);                /* 回收为未映射 */
        CHECK(sc(222, 0, 8192, 3, 0x22, (uint64_t)-1).val == MMAP_BASE); /* 复用首地址 */
        gread(MMAP_BASE, buf, 8);
        CHECK(buf[0] == 0 && buf[7] == 0);                           /* 复用区零页 */
    }

    /* ---- uname：390 字节 6×65，字段内容 + NUL + 余零 + 越界哨兵 ---- */
    {
        uint8_t buf[391];
        uint8_t sent = 0x5A;
        sc_reset();
        memset(buf, 0xEE, sizeof buf);
        gwrite(DBASE + 0x1000, buf, 391);
        gwrite(DBASE + 0x1000 + 390, &sent, 1);                      /* 越界哨兵 */
        rv_sc_ret r = sc(160, DBASE + 0x1000, 0, 0, 0, 0);
        CHECK(r.kind == 0 && r.val == 0);
        gread(DBASE + 0x1000, buf, 391);
        CHECK(strncmp((char *)buf, "Linux", 6) == 0);                /* sysname + NUL */
        CHECK(strncmp((char *)buf + 65, "rvsim", 6) == 0);           /* nodename */
        CHECK(strncmp((char *)buf + 130, "6.8.0-rvsim", 11) == 0);   /* release */
        CHECK(strncmp((char *)buf + 195, "#1 rvsim", 9) == 0);       /* version */
        CHECK(strncmp((char *)buf + 260, "riscv64", 8) == 0);        /* machine */
        CHECK(strncmp((char *)buf + 325, "(none)", 7) == 0);         /* domainname */
        CHECK(buf[10] == 0 && buf[100] == 0 && buf[380] == 0);       /* 余零填充 */
        CHECK(buf[390] == 0x5A);                                     /* 哨兵未被踩 */
    }

    /* ---- clock_gettime：id=0/1 → 0 且 tv_sec>0、0<=tv_nsec<1e9；id=99 → -22 ---- */
    {
        uint8_t buf[16];
        uint64_t sec, nsec;
        sc_reset();
        rv_sc_ret r = sc(113, 0, DBASE + 0x200, 0, 0, 0);            /* CLOCK_REALTIME */
        CHECK(r.kind == 0 && r.val == 0);
        gread(DBASE + 0x200, buf, 16);
        sec = get_le64(buf);
        nsec = get_le64(buf + 8);
        CHECK(sec > 0);                                              /* 2026 年附近，远大于 0 */
        CHECK(nsec < 1000000000ULL);
        r = sc(113, 1, DBASE + 0x200, 0, 0, 0);                      /* CLOCK_MONOTONIC */
        CHECK(r.kind == 0 && r.val == 0);
        gread(DBASE + 0x200, buf, 16);
        CHECK(get_le64(buf) > 0);                                    /* 开机至今秒数 > 0 */
        CHECK(get_le64(buf + 8) < 1000000000ULL);
        CHECK(sc(113, 99, DBASE + 0x200, 0, 0, 0).val == -22);       /* 非法 id */
        CHECK(sc(113, 0, DBASE + DSZ, 0, 0, 0).val == -14);          /* tp 未映射 */
    }

    /* ---- 未知号码 999 → kind=2 ---- */
    sc_reset();
    {
        rv_sc_ret r = sc(999, 0, 0, 0, 0, 0);
        CHECK(r.kind == 2);
    }

    /* ---- cpu 臂：FENCE / FENCE.I 无副作用退休 ---- */
    {
        uint32_t fence = 0x0FF0000Fu, fencei = 0x0000100Fu;
        sc_reset();
        gwrite(MEM_BASE, &fence, 4);
        CHECK(rv_step(&hs) == 0);
        CHECK(hs.cpu.pc == MEM_BASE + 4 && hs.cpu.steps == 1);
        CHECK(!hs.exited);
        gwrite(MEM_BASE + 4, &fencei, 4);
        hs.cpu.pc = MEM_BASE + 4;
        CHECK(rv_step(&hs) == 0);
        CHECK(hs.cpu.pc == MEM_BASE + 8 && hs.cpu.steps == 2);
        CHECK(!hs.exited);
    }

    /* ---- cpu 臂：EBREAK → exited + 206，不推进 pc/steps ---- */
    {
        uint32_t ebreak = 0x00100073u;
        sc_reset();
        gwrite(MEM_BASE, &ebreak, 4);
        CHECK(rv_step(&hs) == 0);                    /* 非故障：正常受控停机 */
        CHECK(hs.exited && hs.exit_code == RVSIM_EX_EBREAK);
        CHECK(hs.exit_code == 206);
        CHECK(hs.cpu.pc == MEM_BASE && hs.cpu.steps == 0);
    }

    /* ---- cpu 臂：ECALL exit(42) → exited + 42，不推进 pc/steps ---- */
    {
        uint32_t ecall = 0x00000073u;
        sc_reset();
        gwrite(MEM_BASE, &ecall, 4);
        hs.cpu.x[17] = 93;
        hs.cpu.x[10] = 42;
        CHECK(rv_step(&hs) == 0);
        CHECK(hs.exited && hs.exit_code == 42);
        CHECK(hs.cpu.pc == MEM_BASE && hs.cpu.steps == 0);
    }

    /* ---- cpu 臂：ECALL 未知号码 → exited + 205 + 记录号码（诊断由 main/trace） ---- */
    {
        uint32_t ecall = 0x00000073u;
        sc_reset();
        gwrite(MEM_BASE, &ecall, 4);
        hs.cpu.x[17] = 999;
        CHECK(rv_step(&hs) == 0);
        CHECK(hs.exited && hs.exit_code == RVSIM_EX_SYSCALL);
        CHECK(hs.exit_code == 205);
        CHECK(hs.sc_unsupported && hs.unsupported_syscall_num == 999);
        CHECK(hs.cpu.pc == MEM_BASE);
    }

    /* ---- cpu 臂：ECALL write 全路径（x17/x10..x12 → a0 写回 + pc 推进） ---- */
    {
        int p[2];
        uint32_t ecall = 0x00000073u;
        uint8_t buf[8] = { 0 };
        sc_reset();
        CHECK(pipe(p) == 0);
        hs.host_fd[1] = p[1];
        gwrite(DBASE, "ok", 2);
        gwrite(MEM_BASE, &ecall, 4);
        hs.cpu.x[17] = 64;                           /* write */
        hs.cpu.x[10] = 1;                            /* fd */
        hs.cpu.x[11] = DBASE;                        /* buf */
        hs.cpu.x[12] = 2;                            /* count */
        CHECK(rv_step(&hs) == 0);
        CHECK(!hs.exited);
        CHECK(hs.cpu.x[10] == 2);                    /* 返回值写 a0 */
        CHECK(hs.cpu.pc == MEM_BASE + 4 && hs.cpu.steps == 1);
        CHECK(read(p[0], buf, 8) == 2 && memcmp(buf, "ok", 2) == 0);
        close(p[0]);
        close(p[1]);
    }

    TEST_DONE();
}
