/* src/syscall.c —— Task 10（M5 分发 A）：Linux RV64 ABI 兼容子集（spec §5）。
   rv_syscall：a7 号码 + a0..a5 参数 → rv_sc_ret{kind,val}：
     kind=0 → val 写回 guest a0（负数即 -errno，Linux 失败语义，不设进位位）；
     kind=1 → guest 终止，val = 退出码（exit/exit_group，code&0xff）；
     kind=2 → 不支持号码，cpu 侧置 exited/exit_code=205 并记录号码，诊断输出
              由 main/trace 层负责（cpu/syscall 不依赖 stdio）。
   实现设计申报（详见 sdd/task-10-report.md「分发 A」）：
   1) 指针参数（read/write 的 buf、clock 的 tp、uname 的 buf）未映射/越界 →
      返回 -EFAULT(14)（与 Linux 同型 syscall 行为一致），非 spec §5 尾注的
      202 退出；分发 A 裁决：syscall 层自洽可单测，guest 坏指针不整局终止。
   2) brk 的 heap 登记区定位：rv_syscall 内扫描 rgn[] 找 kind==1 且
      lo==brk_base 的唯一区域，直接更新其 hi（heap 区由 main 装配期登记一次，
      分发 B 接线）。不在 rvsim 存区域索引——munmap 数组压缩会使索引失效。
   3) 匿名 mmap 零页语义：新区域 memset 清零（后备存储为整块缓冲，munmap
      复用后不清零会残留旧数据，违反 Linux 匿名映射语义）。
   4) guest 层 fd 过滤（read 仅 0、write 仅 {1,2}）在 syscall 层完成，不经
      宿主层；宿主 read/write 系统调用本身失败（宿主 fd 无效等）→ -EIO(5)。
   5) read/write 经 mem_check 后直取 guest 内存的宿主视图（mem.data），避免
      按字节搬运与临时大缓冲；mmap prot/off 参数按 spec 忽略（仅匿名私有）。
   6) munmap 回收顶块（hi==mmap_top）时回退 mmap_top，下次 mmap 复用该首地址
      （bump 分配器仅回收尾部；中间空洞不复用，规格未要求）。
   7) 修复轮 1/M-1：read/write 的 count==0 提前返回 0，不做 buf 检查
      （Linux 语义：零长 I/O 不解引用用户指针，坏 buf 亦返回 0）。 */
#define _POSIX_C_SOURCE 200809L
#include "rvsim.h"
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static rv_sc_ret sc_ok(int64_t v)     { rv_sc_ret r = { 0, v }; return r; }
static rv_sc_ret sc_exit(int64_t v)   { rv_sc_ret r = { 1, v }; return r; }
static rv_sc_ret sc_unsupported(void) { rv_sc_ret r = { 2, 0 }; return r; }

/* guest 视图小端序列化（mem 以客户小端视图整块读写，与 cpu.c 取指约定一致） */
static void put_le64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

rv_sc_ret rv_syscall(rvsim *s, uint64_t num, uint64_t a0, uint64_t a1,
                     uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;   /* prot：仅匿名私有，无保护位语义 */
    (void)a5;   /* off：匿名映射无文件偏移 */

    switch (num) {
    case 93:                        /* exit(code)：终止，退出码 code&0xff */
    case 94:                        /* exit_group(code)：单 hart 同 93 */
        return sc_exit((int64_t)(a0 & 0xff));

    case 63: {                      /* read(fd, buf, count)：仅 fd=0 转发宿主 */
        ssize_t r;
        if (a0 != 0)
            return sc_ok(-EBADF);
        if (a2 == 0)
            return sc_ok(0);        /* count=0：不检查 buf（Linux 语义，修复轮 1/M-1） */
        if (mem_check(&s->mem, a1, a2) != 0)
            return sc_ok(-EFAULT);
        r = read(s->host_fd[0], s->mem.data + (a1 - MEM_BASE), a2);
        return sc_ok(r < 0 ? -EIO : (int64_t)r);
    }

    case 64: {                      /* write(fd, buf, count)：仅 fd∈{1,2} */
        ssize_t r;
        if (a0 != 1 && a0 != 2)
            return sc_ok(-EBADF);
        if (a2 == 0)
            return sc_ok(0);        /* count=0：不检查 buf（Linux 语义，修复轮 1/M-1） */
        if (mem_check(&s->mem, a1, a2) != 0)
            return sc_ok(-EFAULT);
        r = write(s->host_fd[a0], s->mem.data + (a1 - MEM_BASE), a2);
        return sc_ok(r < 0 ? -EIO : (int64_t)r);
    }

    case 214: {                     /* brk(addr) */
        uint32_t i;
        if (a0 == 0 || a0 < s->brk_base || a0 > s->brk_base + HEAP_CAP)
            return sc_ok((int64_t)s->brk_cur);      /* 失败不报错：Linux 语义 */
        for (i = 0; i < s->mem.n; i++)              /* heap 区由 main 装配登记一次 */
            if (s->mem.rgn[i].kind == 1 && s->mem.rgn[i].lo == s->brk_base)
                break;
        if (i == s->mem.n)
            return sc_ok((int64_t)s->brk_cur);      /* heap 未登记（装配前）：同失败语义 */
        s->mem.rgn[i].hi = a0;                      /* 直接扩大/回缩登记区 hi */
        s->brk_cur = a0;
        return sc_ok((int64_t)a0);
    }

    case 222: {                     /* mmap：仅 MAP_PRIVATE|MAP_ANONYMOUS，bump 分配 */
        uint64_t len = a1, flags = a3, alen, start;
        if (len == 0)
            return sc_ok(-EINVAL);
        if ((flags & (MAP_PRIVATE | MAP_ANONYMOUS)) != (MAP_PRIVATE | MAP_ANONYMOUS)
            || a4 != (uint64_t)-1)
            return sc_ok(-EINVAL);
        if (len > MMAP_CAP)                          /* 先挡取整溢出，alen 恒 <= MMAP_CAP */
            return sc_ok(-ENOMEM);
        alen = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (s->mmap_top + alen > MMAP_BASE + MMAP_CAP)
            return sc_ok(-ENOMEM);                   /* 超 64MiB 总量 */
        if (s->mem.n >= RVSIM_RGN_MAX)
            return sc_ok(-ENOMEM);                   /* 区域表满（台账 R12），add 前拒绝 */
        start = s->mmap_top;
        mem_add_region(&s->mem, start, start + alen, 2);
        memset(s->mem.data + (start - MEM_BASE), 0, alen);   /* 匿名 mmap 零页语义 */
        s->mmap_top = start + alen;
        return sc_ok((int64_t)start);
    }

    case 215: {                     /* munmap：仅精确匹配一个 mmap 分配区 */
        uint32_t i;
        for (i = 0; i < s->mem.n; i++)
            if (s->mem.rgn[i].kind == 2 && s->mem.rgn[i].lo == a0
                && s->mem.rgn[i].hi - s->mem.rgn[i].lo == a1)
                break;
        if (i == s->mem.n)
            return sc_ok(-EINVAL);
        /* 回收顶块（hi==mmap_top）时回退 bump 指针 → 下次 mmap 复用该首地址；
           中间空洞不回退（bump 分配器只回收尾部，spec §5 未要求空洞复用）。 */
        if (s->mem.rgn[i].hi == s->mmap_top)
            s->mmap_top = s->mem.rgn[i].lo;
        while (i < s->mem.n - 1) {                  /* 数组压缩移除该区域 */
            s->mem.rgn[i] = s->mem.rgn[i + 1];
            i++;
        }
        s->mem.n--;
        return sc_ok(0);
    }

    case 113: {                     /* clock_gettime(id, tp)：宿主时钟 → 16 字节 */
        struct timespec ts;
        uint8_t buf[16];
        if (a0 > 1)
            return sc_ok(-EINVAL);
        clock_gettime(a0 == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
        put_le64(buf, (uint64_t)ts.tv_sec);
        put_le64(buf + 8, (uint64_t)ts.tv_nsec);
        if (mem_write(&s->mem, a1, buf, 16) != 0)
            return sc_ok(-EFAULT);
        return sc_ok(0);
    }

    case 160: {                     /* uname(buf)：390 字节 = 6×65，字段 NUL 截断+余零 */
        static const char *const uts[6] = {
            "Linux", "rvsim", "6.8.0-rvsim", "#1 rvsim", "riscv64", "(none)"
        };
        uint8_t buf[390];
        uint32_t i;
        memset(buf, 0, sizeof buf);
        for (i = 0; i < 6; i++) {
            size_t len = strlen(uts[i]);            /* 各字段 < 65，无截断 */
            memcpy(buf + 65u * i, uts[i], len);
        }
        if (mem_write(&s->mem, a0, buf, sizeof buf) != 0)
            return sc_ok(-EFAULT);
        return sc_ok(0);
    }

    default:                        /* 表中未列号码：kind=2，cpu 侧转 205 退出 */
        return sc_unsupported();
    }
}
