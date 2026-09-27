/* tests/test_alu.c —— Task 6：整数立即数算术(9) + 寄存器算术(10) 执行臂，
   以及 rv_step 故障契约（无部分副作用）。
   全路径：enc.h 编码 → mem 取指 → rv_decode → 执行 → 写回。
   期望值全部手算；寄存器移位（SLL/SRL/SRA）移位量取 rs2 低 6 位（RISC-V
   非特权级规范；设计文档 §4.2 写作 rs1 系笔误，见 task-6-report）。
   简报骨架中 SLTI/SLTIU/SLT/SLTU 四处期望值与手算相反，已修正留档。
   Task 8（M3）适配：访存组执行臂接入后，故障契约探针由 LD 换为仍属
   未实现组的 JAL（断言意图不变：可解码但无执行臂 → ILLEGAL 且零副作用）。 */
#include "harness.h"
#include "enc.h"

int main(void)
{
    /* ---- 立即数算术 9 条（op=0x13，rs1=10 → a，rd=12） ---- */
    one(ENC_I(1, 10, 0, 12, 0x13), 5, 0, 6, 12);                        /* ADDI +1 */
    one(ENC_I(-1, 10, 0, 12, 0x13), 5, 0, 4, 12);                       /* ADDI -1 */
    one(ENC_I(3, 10, 0, 12, 0x13), (uint64_t)-5, 0, (uint64_t)-2, 12);  /* ADDI 负数边界 */
    one(ENC_I(5, 10, 2, 12, 0x13), (uint64_t)-2, 0, 1, 12);             /* SLTI: -2<5 →1 */
    one(ENC_I(5, 10, 2, 12, 0x13), 5, 0, 0, 12);                        /* SLTI 相等 →0 */
    one(ENC_I(-1, 10, 2, 12, 0x13), 1, 0, 0, 12);                       /* SLTI: 1<-1 →0（有符号） */
    one(ENC_I(5, 10, 3, 12, 0x13), (uint64_t)-2, 0, 0, 12);             /* SLTIU: 0xFF..FE<5 →0（无符号大数） */
    one(ENC_I(-1, 10, 3, 12, 0x13), 1, 0, 1, 12);                       /* SLTIU: 1<0xFF..FF →1（无符号语义判别） */
    one(ENC_I(5, 10, 3, 12, 0x13), 5, 0, 0, 12);                        /* SLTIU 相等 →0 */
    one(ENC_I(0x0FF, 10, 4, 12, 0x13), 0xFF00FF00FF00FF00ULL, 0, 0xFF00FF00FF00FFFFULL, 12); /* XORI */
    one(ENC_I(-1, 10, 4, 12, 0x13), (uint64_t)-1, 0, 0, 12);            /* XORI -1 自身清零 */
    one(ENC_I(0x034, 10, 6, 12, 0x13), 0x1200, 0, 0x1234, 12);          /* ORI */
    one(ENC_I(-1, 10, 6, 12, 0x13), 0, 0, (uint64_t)-1, 12);            /* ORI -1 全 1 */
    one(ENC_I(0x00F, 10, 7, 12, 0x13), (uint64_t)-1, 0, 0xF, 12);       /* ANDI */
    one(ENC_I(-16, 10, 7, 12, 0x13), 0x1234, 0, 0x1230, 12);            /* ANDI 负立即数掩码 */
    one(ENC_I(63, 10, 1, 12, 0x13), 1, 0, (uint64_t)0x8000000000000000ULL, 12); /* SLLI 63 */
    one(ENC_I(4, 10, 1, 12, 0x13), 0x0123456789ABCDEFULL, 0, 0x123456789ABCDEF0ULL, 12); /* SLLI 4 */
    one(ENC_I(63, 10, 5, 12, 0x13), (uint64_t)0x8000000000000000ULL, 0, 1, 12); /* SRLI 63 →1（仅剩 bit0） */
    one(ENC_I(4, 10, 5, 12, 0x13), (uint64_t)-1, 0, 0x0FFFFFFFFFFFFFFFULL, 12); /* SRLI 4 */
    one(ENC_I(0x43F, 10, 5, 12, 0x13), (uint64_t)0x8000000000000000ULL, 0, (uint64_t)-1, 12); /* SRAI 63: -2^63>>63=-1 符号回填满 */
    one(ENC_I(0x402, 10, 5, 12, 0x13), (uint64_t)-16, 0, (uint64_t)-4, 12); /* SRAI 2 负数 */

    /* ---- 寄存器算术 10 条（op=0x33，rs1=10→a，rs2=11→b，rd=12） ---- */
    one(OP_STD(0x00, 11, 10, 0, 12, 0x33), 0x7FFFFFFFFFFFFFFFULL, 1, (uint64_t)0x8000000000000000ULL, 12); /* ADD 回绕 */
    one(OP_STD(0x00, 11, 10, 0, 12, 0x33), (uint64_t)-2, 1, (uint64_t)-1, 12);  /* ADD 负数 */
    one(OP_STD(0x20, 11, 10, 0, 12, 0x33), 0, 1, (uint64_t)-1, 12);             /* SUB */
    one(OP_STD(0x20, 11, 10, 0, 12, 0x33), 0, (uint64_t)0x8000000000000000ULL, (uint64_t)0x8000000000000000ULL, 12); /* SUB 回绕 */
    one(OP_STD(0x00, 11, 10, 1, 12, 0x33), 1, 65, 2, 12);                       /* SLL 移位量取 b 低 6 位（65≡1） */
    one(OP_STD(0x00, 11, 10, 1, 12, 0x33), (uint64_t)0x8000000000000000ULL, 1, 0, 12); /* SLL 移出为 0 */
    one(OP_STD(0x00, 11, 10, 2, 12, 0x33), 1, 2, 1, 12);                        /* SLT 1<2 →1 */
    one(OP_STD(0x00, 11, 10, 2, 12, 0x33), (uint64_t)-1, 1, 1, 12);             /* SLT 有符号 -1<1 →1 */
    one(OP_STD(0x00, 11, 10, 2, 12, 0x33), 5, 5, 0, 12);                        /* SLT 相等 →0 */
    one(OP_STD(0x00, 11, 10, 2, 12, 0x33), 1, (uint64_t)-1, 0, 12);             /* SLT 1<-1 →0（符号判别） */
    one(OP_STD(0x00, 11, 10, 3, 12, 0x33), 1, 2, 1, 12);                        /* SLTU 1<2 →1 */
    one(OP_STD(0x00, 11, 10, 3, 12, 0x33), (uint64_t)-1, 1, 0, 12);             /* SLTU 无符号大数 →0 */
    one(OP_STD(0x00, 11, 10, 3, 12, 0x33), 1, (uint64_t)-1, 1, 12);             /* SLTU 1<0xFF..FF →1（无符号判别） */
    one(OP_STD(0x00, 11, 10, 4, 12, 0x33), 0xF0F0F0F0F0F0F0F0ULL, 0x0F0F0F0F0F0F0F0FULL, (uint64_t)-1, 12); /* XOR */
    one(OP_STD(0x00, 11, 10, 5, 12, 0x33), (uint64_t)0x8000000000000000ULL, 63, 1, 12); /* SRL 63 →1 */
    one(OP_STD(0x00, 11, 10, 5, 12, 0x33), (uint64_t)0x8000000000000000ULL, 65, (uint64_t)0x4000000000000000ULL, 12); /* SRL 65≡1 低 6 位 */
    one(OP_STD(0x00, 11, 10, 5, 12, 0x33), (uint64_t)-1, 4, 0x0FFFFFFFFFFFFFFFULL, 12); /* SRL 4 */
    one(OP_STD(0x20, 11, 10, 5, 12, 0x33), (uint64_t)0x8000000000000000ULL, 63, (uint64_t)-1, 12); /* SRA 63: -2^63>>63=-1 */
    one(OP_STD(0x20, 11, 10, 5, 12, 0x33), (uint64_t)-16, 2, (uint64_t)-4, 12); /* SRA 负数 */
    one(OP_STD(0x20, 11, 10, 5, 12, 0x33), (uint64_t)0x8000000000000000ULL, 66, (uint64_t)0xE000000000000000ULL, 12); /* SRA 66≡2 符号回填 */
    one(OP_STD(0x00, 11, 10, 6, 12, 0x33), 0x1200, 0x34, 0x1234, 12);           /* OR */
    one(OP_STD(0x00, 11, 10, 7, 12, 0x33), (uint64_t)-1, 0xFF00, 0xFF00, 12);   /* AND */

    /* ---- x0 写入丢弃 ---- */
    one(OP_STD(0x00, 11, 10, 0, 0, 0x33), 0x1234, 1, 0, 0);                     /* ADD x0 →x0 恒 0 */

    /* ---- rv_step 故障契约：返回值=fault，无任何部分副作用 ---- */
    {
        int r;
        reset_sim();
        hs.cpu.pc = MEM_BASE + 2;                    /* pc 4 对齐检查 */
        r = rv_step(&hs);
        CHECK(r == RV_FV_MISALIGNED);
        CHECK(hs.cpu.pc == MEM_BASE + 2);
        CHECK(hs.cpu.steps == 0);

        reset_sim();
        hs.cpu.pc = MEM_BASE + 16;                   /* 代码区 [MEM_BASE, MEM_BASE+16) 之外 */
        r = rv_step(&hs);
        CHECK(r == RV_FV_UNMAPPED);
        CHECK(hs.cpu.pc == MEM_BASE + 16);
        CHECK(hs.cpu.steps == 0);

        reset_sim();
        {
            uint32_t bad = 0xFFFFFFFFu;              /* 保留编码 → decode false */
            CHECK(mem_write(&hs.mem, MEM_BASE, &bad, 4) == 0);
        }
        r = rv_step(&hs);
        CHECK(r == RV_FV_ILLEGAL);
        CHECK(hs.cpu.pc == MEM_BASE);
        CHECK(hs.cpu.steps == 0);

        reset_sim();
        {
            uint32_t jal = 0x0000006Fu;              /* JAL x0,0：可解码但控制流组无执行臂
                                                        （访存组已由 Task 8 接入，探针换 JAL）。
                                                        预告（M3 修复轮）：M4 控制流组接入后
                                                        JAL 将有执行臂，本探针随之失效；届时
                                                        改用 FENCE/ECALL 组编码作「可解码但
                                                        无执行臂 → ILLEGAL」探针。 */
            CHECK(mem_write(&hs.mem, MEM_BASE, &jal, 4) == 0);
        }
        hs.cpu.x[12] = 0xDEADBEEFCAFEBABEULL;        /* 确认无写回副作用 */
        r = rv_step(&hs);
        CHECK(r == RV_FV_ILLEGAL);
        CHECK(hs.cpu.x[12] == 0xDEADBEEFCAFEBABEULL);
        CHECK(hs.cpu.pc == MEM_BASE);
        CHECK(hs.cpu.steps == 0);
    }

    TEST_DONE();
}
