/* tests/test_wops.c —— Task 6：W 类 9 条（32 位运算后符号扩展到 64）。
   重点：负数/回绕结果经 sext32 后为负（如 ADDIW -1 → 0xFFFFFFFFFFFFFFFF）、
   高 32 位忽略、寄存器移位量取 rs2 低 5 位（33≡1）、SRLIW 与 SRAIW 对照。 */
#include "harness.h"
#include "enc.h"

int main(void)
{
    /* ---- op-imm-32（op=0x1B，rs1=10 → a，rd=12） ---- */
    one(ENC_I(1, 10, 0, 12, 0x1B), 5, 0, 6, 12);                                /* ADDIW +1 */
    one(ENC_I(-1, 10, 0, 12, 0x1B), 0, 0, (uint64_t)-1, 12);                    /* ADDIW -1 → 符号扩展全 1 */
    one(ENC_I(1, 10, 0, 12, 0x1B), 0x7FFFFFFFULL, 0, (uint64_t)0xFFFFFFFF80000000ULL, 12); /* ADDIW 32 位回绕+符号扩展 */
    one(ENC_I(-2, 10, 0, 12, 0x1B), (uint64_t)-1, 0, (uint64_t)-3, 12);         /* ADDIW 负立即数 */
    one(ENC_I(31, 10, 1, 12, 0x1B), 1, 0, (uint64_t)0xFFFFFFFF80000000ULL, 12); /* SLLIW 31 →sext(0x80000000) */
    one(ENC_I(31, 10, 1, 12, 0x1B), 0xDEADBEEF00000001ULL, 0, (uint64_t)0xFFFFFFFF80000000ULL, 12); /* SLLIW 高 32 位忽略 */
    one(ENC_I(0, 10, 1, 12, 0x1B), (uint64_t)-1, 0, (uint64_t)-1, 12);          /* SLLIW 0 →sext(0xFFFFFFFF) */
    one(ENC_I(31, 10, 5, 12, 0x1B), 0x0000000080000000ULL, 0, 1, 12);           /* SRLIW 31: 0x80000000>>31=1 */
    one(ENC_I(1, 10, 5, 12, 0x1B), (uint64_t)0xFFFFFFFF80000000ULL, 0, 0x40000000ULL, 12); /* SRLIW 逻辑移位按 32 位 */
    one(ENC_I(31, 10, 5, 12, 0x1B), (uint64_t)0xFFFFFFFF80000000ULL, 0, 1, 12); /* SRLIW 31 →1（与 SRAIW 对照） */
    one(ENC_I(0x41F, 10, 5, 12, 0x1B), 0x0000000080000000ULL, 0, (uint64_t)-1, 12); /* SRAIW 31 符号扩展 */
    one(ENC_I(0x41F, 10, 5, 12, 0x1B), (uint64_t)0xFFFFFFFF80000000ULL, 0, (uint64_t)-1, 12); /* SRAIW 31 */
    one(ENC_I(0x401, 10, 5, 12, 0x1B), (uint64_t)0xFFFFFFFF80000000ULL, 0, (uint64_t)0xFFFFFFFFC0000000ULL, 12); /* SRAIW 1 */

    /* ---- op-32（op=0x3B，rs1=10→a，rs2=11→b，rd=12） ---- */
    one(OP_STD(0x00, 11, 10, 0, 12, 0x3B), 0x7FFFFFFFULL, 1, (uint64_t)0xFFFFFFFF80000000ULL, 12); /* ADDW 回绕+sext */
    one(OP_STD(0x00, 11, 10, 0, 12, 0x3B), (uint64_t)-1, 1, 0, 12);             /* ADDW 32 位和为 0 */
    one(OP_STD(0x20, 11, 10, 0, 12, 0x3B), 0, 1, (uint64_t)-1, 12);             /* SUBW 0-1 →全 1 */
    one(OP_STD(0x20, 11, 10, 0, 12, 0x3B), 0x0000000080000000ULL, 1, 0x7FFFFFFFULL, 12); /* SUBW 高 32 位忽略 */
    one(OP_STD(0x00, 11, 10, 1, 12, 0x3B), 1, 31, (uint64_t)0xFFFFFFFF80000000ULL, 12); /* SLLW 31 */
    one(OP_STD(0x00, 11, 10, 1, 12, 0x3B), 1, 33, 2, 12);                       /* SLLW 移位量取低 5 位（33≡1） */
    one(OP_STD(0x00, 11, 10, 1, 12, 0x3B), (uint64_t)0x8000000000000000ULL, 1, 0, 12); /* SLLW 只用低 32 位 */
    one(OP_STD(0x00, 11, 10, 5, 12, 0x3B), (uint64_t)0xFFFFFFFF80000000ULL, 31, 1, 12); /* SRLW 31 */
    one(OP_STD(0x00, 11, 10, 5, 12, 0x3B), (uint64_t)0xFFFFFFFF80000000ULL, 33, 0x40000000ULL, 12); /* SRLW 33≡1 */
    one(OP_STD(0x20, 11, 10, 5, 12, 0x3B), (uint64_t)0xFFFFFFFF80000000ULL, 31, (uint64_t)-1, 12); /* SRAW 31 符号扩展 */
    one(OP_STD(0x20, 11, 10, 5, 12, 0x3B), 0x0000000080000000ULL, 33, (uint64_t)0xFFFFFFFFC0000000ULL, 12); /* SRAW 33≡1 符号回填 */
    one(OP_STD(0x20, 11, 10, 5, 12, 0x3B), 0x7FFFFFFFULL, 1, 0x3FFFFFFFULL, 12); /* SRAW 正数 */

    TEST_DONE();
}
