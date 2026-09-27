#include "framework.h"
#include "rvsim.h"
#include <string.h>

/* test_stack —— Task 11/M5 分发 B：build_stack 栈布局 L0 单测（spec §3.4）。
   期望值全部手算（gargs={"prog.elf","one","two"}，strsz=9+4+4=17）：
     addr_random = STACK_TOP-16        = 0x2000FEF0
     addr_str    = addr_random - 17    = 0x2000FEDF
     blk         = 8+8*4+8+48          = 96
     sp          = (addr_str-96)&~0xF  = 0x2000FE70（16 对齐）
   布局（高→低）：AT_RANDOM(0x42*16) / argv 字符串区(17B) / [padding 0x2000FE7F
   .. 0x2000FE70，无指针指向] / argc / argv[3]+NULL / envp NULL / auxv
   (6,4096)(25,addr_random)(0,0)。argc=1 用例手算：strsz=2、blk=80、
   addr_str=0x2000FEEE、sp=0x2000FE90。 */
static int rd64(rv_mem *m, uint64_t a, uint64_t *out) { return mem_read(m, a, out, 8); }

int main(void) {
    static const uint64_t ADDR_RANDOM = STACK_TOP - 16;
    static const uint64_t ADDR_STR = STACK_TOP - 16 - 17;
    static const uint64_t SP = (STACK_TOP - 16 - 17 - 96) & ~(uint64_t)0xF;

    rvsim s;
    memset(&s, 0, sizeof s);
    CHECK(mem_init(&s.mem) == 0);
    rvsim_reset(&s);
    mem_add_region(&s.mem, STACK_TOP - STACK_SIZE, STACK_TOP, 3);

    char *gargs[3] = { (char *)"prog.elf", (char *)"one", (char *)"two" };
    CHECK(build_stack(&s, 3, gargs) == 0);

    /* sp：16 对齐、落在 stack 区内、写入 x2；argc 值在 sp+0 */
    CHECK(s.cpu.x[2] == SP);
    CHECK(SP % 16 == 0);
    CHECK(SP > STACK_TOP - STACK_SIZE && SP < STACK_TOP);
    uint64_t v = 0;
    CHECK(rd64(&s.mem, SP, &v) == 0 && v == 3);

    /* argv[0..2] 指针 + argv[3]=NULL；字符串内容/位置精确 */
    CHECK(rd64(&s.mem, SP + 8, &v) == 0 && v == ADDR_STR);
    CHECK(rd64(&s.mem, SP + 16, &v) == 0 && v == ADDR_STR + 9);
    CHECK(rd64(&s.mem, SP + 24, &v) == 0 && v == ADDR_STR + 13);
    CHECK(rd64(&s.mem, SP + 32, &v) == 0 && v == 0);
    char buf[16] = {0};
    CHECK(mem_read(&s.mem, ADDR_STR, buf, 9) == 0 && strcmp(buf, "prog.elf") == 0);
    CHECK(mem_read(&s.mem, ADDR_STR + 9, buf, 4) == 0 && strcmp(buf, "one") == 0);
    CHECK(mem_read(&s.mem, ADDR_STR + 13, buf, 4) == 0 && strcmp(buf, "two") == 0);

    /* envp 单 NULL；auxv 顺序钉死：(AT_PAGESZ,4096)(AT_RANDOM,addr)(AT_NULL,0) */
    CHECK(rd64(&s.mem, SP + 40, &v) == 0 && v == 0);
    CHECK(rd64(&s.mem, SP + 48, &v) == 0 && v == 6);
    CHECK(rd64(&s.mem, SP + 56, &v) == 0 && v == 4096);
    CHECK(rd64(&s.mem, SP + 64, &v) == 0 && v == 25);
    CHECK(rd64(&s.mem, SP + 72, &v) == 0 && v == ADDR_RANDOM);
    CHECK(rd64(&s.mem, SP + 80, &v) == 0 && v == 0);
    CHECK(rd64(&s.mem, SP + 88, &v) == 0 && v == 0);

    /* AT_RANDOM 数据：16 字节固定 0x42；字符串区紧贴 AT_RANDOM 数据区下方 */
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        unsigned char c = 0;
        ok = ok && mem_read(&s.mem, ADDR_RANDOM + (uint64_t)i, &c, 1) == 0 && c == 0x42;
    }
    CHECK(ok);
    CHECK(ADDR_STR + 17 == ADDR_RANDOM);

    /* entry 契约：a0/a1/a2（x10/x11/x12）保持 rvsim_reset 的 0 */
    CHECK(s.cpu.x[10] == 0 && s.cpu.x[11] == 0 && s.cpu.x[12] == 0);

    /* argc=1：strsz=2、blk=80、sp=(STACK_TOP-16-2-80)&~0xF=0x2000FE90 */
    rvsim t;
    memset(&t, 0, sizeof t);
    CHECK(mem_init(&t.mem) == 0);
    rvsim_reset(&t);
    mem_add_region(&t.mem, STACK_TOP - STACK_SIZE, STACK_TOP, 3);
    char *gargs1[1] = { (char *)"x" };
    CHECK(build_stack(&t, 1, gargs1) == 0);
    CHECK(t.cpu.x[2] == ((STACK_TOP - 16 - 2 - 80) & ~(uint64_t)0xF));
    CHECK(t.cpu.x[2] % 16 == 0);
    CHECK(rd64(&t.mem, t.cpu.x[2], &v) == 0 && v == 1);
    CHECK(rd64(&t.mem, t.cpu.x[2] + 16, &v) == 0 && v == 0);   /* argv[1]=NULL */
    CHECK(rd64(&t.mem, t.cpu.x[2] + 24, &v) == 0 && v == 0);   /* envp NULL */
    CHECK(rd64(&t.mem, t.cpu.x[2] + 32, &v) == 0 && v == 6);   /* auxv 首对 key（sp+32：argc1+argv2+envp 后） */

    /* 防御：argc<=0 拒绝 */
    CHECK(build_stack(&t, 0, gargs1) == -1);

    mem_free(&s.mem);
    mem_free(&t.mem);
    TEST_DONE();
}
