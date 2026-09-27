#include "framework.h"
#include "rvsim.h"
#include <stdlib.h>

/* 好 ELF 路径（tests/guest-bin/t_hello.elf，由 tests/build_guest.sh 产出）+
   坏 ELF 由 tests/make_bad_elf.sh 生成后以 argv 传入（argv[1]=路径 argv[2]=期望负错误码） */
int main(int argc, char **argv) {
    rv_mem m; rv_elf e;
    CHECK(mem_init(&m) == 0);
    /* 正常 ELF：由交叉工具链产出的 t_hello.elf */
    CHECK(elf_load(&m, "tests/guest-bin/t_hello.elf", &e) == 0);
    CHECK(e.entry != 0);
    CHECK((e.entry & 3) == 0);
    CHECK(e.brk_base > e.entry);
    CHECK((e.brk_base & (PAGE_SIZE - 1)) == 0);
    CHECK(e.bias == 0);                      /* Buildroot 产出为 ET_EXEC */
    /* 装载后 entry 处可取指：前 4 字节非零 */
    uint32_t w = 0;
    CHECK(mem_read(&m, e.entry, &w, 4) == 0 && w != 0);
    /* 坏 ELF：argv[1]=路径 argv[2]=期望负错误码 */
    if (argc == 3) {
        int want = atoi(argv[2]);
        CHECK(elf_load(&m, argv[1], &e) == want);
    }
    mem_free(&m);
    TEST_DONE();
}
