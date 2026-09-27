#include "framework.h"
#include "rvsim.h"
#include <string.h>

int main(void) {
    rv_mem m;
    CHECK(mem_init(&m) == 0);
    /* 未登记区域不可访问 */
    CHECK(mem_check(&m, MEM_BASE, 8) == -1);
    CHECK(mem_write(&m, MEM_BASE, "abc", 3) == -1);
    /* 登记后读写往返 + 边界 */
    mem_add_region(&m, MEM_BASE, MEM_BASE + 16, 0);
    CHECK(mem_write(&m, MEM_BASE, "hello rv64", 10) == 0);
    char buf[16] = {0};
    CHECK(mem_read(&m, MEM_BASE, buf, 10) == 0);
    CHECK(strcmp(buf, "hello rv64") == 0);
    /* [lo,hi) 半开区间：hi 处 1 字节不可访问 */
    CHECK(mem_check(&m, MEM_BASE + 15, 1) == 0);
    CHECK(mem_check(&m, MEM_BASE + 16, 1) == -1);
    /* 区域内部起点越 hi：起点落在区域内但区间终点越出 → 拒绝（Task 3 评审 M-i） */
    CHECK(mem_check(&m, MEM_BASE + 8, 16) == -1);
    /* 跨区域空洞不可访问 */
    mem_add_region(&m, MEM_BASE + 32, MEM_BASE + 64, 0);
    CHECK(mem_check(&m, MEM_BASE + 16, 16) == -1);
    /* 窗口外不可访问 */
    CHECK(mem_check(&m, 0x100, 4) == -1);
    CHECK(mem_check(&m, MEM_BASE + MEM_SIZE - 4, 8) == -1);
    /* len=0 空区间：落区域内（含右端点 hi）可接受——钉死语义防回归 */
    CHECK(mem_check(&m, MEM_BASE + 8, 0) == 0);
    CHECK(mem_check(&m, MEM_BASE + 16, 0) == 0);
    /* 区域贴窗口顶：末字节可读写，越 1 字节不可（Task 3 评审 M-i） */
    mem_add_region(&m, MEM_BASE + MEM_SIZE - 16, MEM_BASE + MEM_SIZE, 0);
    unsigned char b = 0;
    CHECK(mem_check(&m, MEM_BASE + MEM_SIZE - 1, 1) == 0);
    CHECK(mem_write(&m, MEM_BASE + MEM_SIZE - 1, "x", 1) == 0);
    CHECK(mem_read(&m, MEM_BASE + MEM_SIZE - 1, &b, 1) == 0 && b == 'x');
    CHECK(mem_check(&m, MEM_BASE + MEM_SIZE, 1) == -1);
    mem_free(&m);
    TEST_DONE();
}
