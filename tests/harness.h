/* tests/harness.h —— Task 6 cpu 测试公共辅助（R5 裁决：static inline 公共封装，
   替代简报原方案「每个文件逐字复制 one()」，避免三份漂移）。
   依赖 framework.h（CHECK/TEST_DONE）与 include/rvsim.h，先于本头文件包含。
   依赖 mem_add_region 纯追加语义（不合并/不排序，M-q 钉死）：reset_sim 以
   n=0 清空区域表后逐条追加，测试自建数据区（如 test_memops 的 DATA 区）与
   代码区按追加次序并存，无需重叠/排序处理。辅助函数用 static inline 以便
   未被某测试文件引用时（如 test_memops 未用 one()）不产生 unused 告警。 */
#ifndef RVSIM_TEST_HARNESS_H
#define RVSIM_TEST_HARNESS_H

#include "framework.h"
#include "rvsim.h"

static rvsim hs;
static int hs_mem_ready;

/* 复位模拟器：mem 只 init 一次（512MiB calloc 复用，避免逐用例重复分配），
   区域表每轮重建为代码区 [MEM_BASE, MEM_BASE+16)；cpu/fault 清零，pc=MEM_BASE。
   区域重建依赖 mem_add_region 纯追加语义（不合并/不排序）。 */
static inline void reset_sim(void)
{
    if (hs_mem_ready == 0) {
        if (mem_init(&hs.mem) != 0) {
            printf("FAIL harness: mem_init OOM\n");
            rvsim_checks_failed++;
            hs_mem_ready = -1;
            return;
        }
        hs_mem_ready = 1;
    }
    if (hs_mem_ready < 0)
        return;
    rvsim_reset(&hs);
    hs.mem.n = 0;
    mem_add_region(&hs.mem, MEM_BASE, MEM_BASE + 16, 0);
    hs.cpu.pc = MEM_BASE;
}

/* 单指令全路径用例：写指令字 → x10=a（rs1）/x11=b（rs2）→ rv_step 一条。
   断言：步进成功（返回 0）、rd=exp、pc 推进 4、steps 增至 1。 */
static inline void one(uint32_t insn, uint64_t a, uint64_t b, uint64_t exp, int rd)
{
    reset_sim();
    CHECK(mem_write(&hs.mem, MEM_BASE, &insn, 4) == 0);
    hs.cpu.x[10] = a;
    hs.cpu.x[11] = b;
    CHECK(rv_step(&hs) == 0);
    CHECK(hs.cpu.x[rd] == exp);
    CHECK(hs.cpu.pc == MEM_BASE + 4);
    CHECK(hs.cpu.steps == 1);
}

#endif
