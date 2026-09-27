/* tests/test_flow.c —— Task 9（M4）分发 A：控制流 10 条执行臂
   （LUI/AUIPC/JAL/JALR/BEQ/BNE/BLT/BGE/BLTU/BGEU）+ 对齐故障契约。
   全路径：enc.h 编码 → mem 取指 → rv_decode → 执行 → 写回/pc 更新。
   期望值全部手算。覆盖：10 条各 ≥1 正例；JAL/JALR 写回 pc+4；JALR 清 bit0
   （奇数目标落到偶数地址执行）；JAL 负偏移回跳 + BNE 负偏移回跳循环；
   BLT/BLTU 负数对照、BGE/BGEU 相等值分支；JAL/JALR/分支目标未对齐 →
   RV_FV_MISALIGNED 且零副作用（rd 哨兵/pc/steps 均不动）；跳转落点顺序
   衔接（落点 ADDI 两步执行）；JALR rd==rs1 用旧 rs1 求目标。
   harness 的 reset_sim 代码区仅 [MEM_BASE, MEM_BASE+16)，reset_flow 按纯
   追加语义补一段相邻区域以容纳指令序列/循环。 */
#include "harness.h"
#include "enc.h"

#define NOP      0x00000013u              /* ADDI x0,x0,0 */
#define SENTINEL 0xDEADBEEFCAFEBABEULL

static void reset_flow(void)
{
    reset_sim();
    mem_add_region(&hs.mem, MEM_BASE + 16, MEM_BASE + 512, 0); /* 与代码区相邻衔接 */
}

static void load(const uint32_t *insns, int n)
{
    int i;
    for (i = 0; i < n; i++)
        CHECK(mem_write(&hs.mem, MEM_BASE + 4u * (unsigned)i, &insns[i], 4) == 0);
}

/* 连续退休 n 步（任一步故障即在该步 FAIL）。 */
static void run(int n)
{
    int i;
    for (i = 0; i < n; i++)
        CHECK(rv_step(&hs) == 0);
}

/* 单分支用例：BASE 处一条分支，a/b 装入 x10(rs1)/x11(rs2)，断言退休后
   pc=exp_pc（taken 传 MEM_BASE+imm，未 taken 传 MEM_BASE+4）且无 rd 副作用。 */
static void br(uint32_t insn, uint64_t a, uint64_t b, uint64_t exp_pc)
{
    reset_sim();
    CHECK(mem_write(&hs.mem, MEM_BASE, &insn, 4) == 0);
    hs.cpu.x[10] = a;
    hs.cpu.x[11] = b;
    hs.cpu.x[12] = SENTINEL;                 /* 分支 rd 字段是 imm 残留，不得写回 */
    CHECK(rv_step(&hs) == 0);
    CHECK(hs.cpu.pc == exp_pc);
    CHECK(hs.cpu.steps == 1);
    CHECK(hs.cpu.x[12] == SENTINEL);
}

int main(void)
{
    /* ---- LUI：U 型 imm 符号扩展 32→64（rd=12） ---- */
    one(ENC_U(0x80000, 12, 0x37), 0, 0, 0xFFFFFFFF80000000ULL, 12); /* 最高位 1 → 符号扩展 */
    one(ENC_U(0xFFFFF, 12, 0x37), 0, 0, 0xFFFFFFFFFFFFF000ULL, 12); /* 全 1 → -4096 */
    one(ENC_U(0x12345, 12, 0x37), 0, 0, 0x12345000ULL, 12);         /* 正常值 */
    one(ENC_U(0x00000, 12, 0x37), 0, 0, 0, 12);                     /* 全 0 */
    one(ENC_U(0x80000, 0, 0x37), 0, 0, 0, 0);                       /* x0 写入丢弃 */

    /* ---- AUIPC：rd = pc + imm（按 2^64 回绕）；ENC_U 参数是 20 位字段，
       生效立即数 = 字段<<12（如字段 1 → +0x1000，字段 0x80000 → -0x80000000） ---- */
    one(ENC_U(0x00001, 12, 0x17), 0, 0, MEM_BASE + 0x1000ULL, 12);  /* 正偏移 +4096 */
    one(ENC_U(0xFFFFC, 12, 0x17), 0, 0, MEM_BASE - 0x4000ULL, 12);  /* 字段 -4 → -0x4000 */
    one(ENC_U(0x80000, 12, 0x17), 0, 0, 0xFFFFFFFF80010000ULL, 12); /* 负大数 + pc */
    one(ENC_U(0x00000, 12, 0x17), 0, 0, MEM_BASE, 12);              /* imm=0 → pc 本身 */

    /* AUIPC pc 相对性：同 imm=0、相邻两条，结果随 pc 递增而不同 */
    reset_flow();
    {
        uint32_t code[2];
        code[0] = ENC_U(0, 12, 0x17);
        code[1] = ENC_U(0, 13, 0x17);
        load(code, 2);
        run(2);
        CHECK(hs.cpu.x[12] == MEM_BASE);
        CHECK(hs.cpu.x[13] == MEM_BASE + 4);
    }

    /* ---- JAL：rd=pc+4；pc+=imm；落点指令顺序衔接（两步执行） ---- */
    reset_flow();
    {
        uint32_t code[4];
        code[0] = ENC_J(8, 12, 0x6F);        /* JAL x12,+8 → 落 BASE+8 */
        code[1] = NOP;                       /* BASE+4：被跳过 */
        code[2] = ENC_I(7, 0, 0, 15, 0x13);  /* BASE+8：落点 ADDI x15,x0,7 */
        code[3] = NOP;
        load(code, 4);
        run(1);
        CHECK(hs.cpu.x[12] == MEM_BASE + 4); /* 写回 pc+4 */
        CHECK(hs.cpu.pc == MEM_BASE + 8);    /* pc = pc + imm */
        run(1);
        CHECK(hs.cpu.x[15] == 7);            /* 落点 ADDI 已执行（顺序衔接） */
        CHECK(hs.cpu.pc == MEM_BASE + 12);   /* 落点退休后顺序推进 */
        CHECK(hs.cpu.steps == 2);
    }

    /* JAL 负偏移回跳：BASE+8 → BASE+0 */
    reset_flow();
    {
        uint32_t code[3];
        code[0] = NOP;
        code[1] = NOP;
        code[2] = ENC_J(-8, 0, 0x6F);
        load(code, 3);
        hs.cpu.pc = MEM_BASE + 8;
        run(1);
        CHECK(hs.cpu.pc == MEM_BASE);
        CHECK(hs.cpu.steps == 1);
    }

    /* ---- JALR：t=(rs1+imm)&~1；rd=pc+4；pc=t（t 用旧 rs1） ---- */
    reset_flow();
    {
        uint32_t code[5];
        code[0] = ENC_I(0, 10, 0, 12, 0x67); /* JALR x12,0(x10) */
        code[1] = NOP;
        code[2] = NOP;
        code[3] = NOP;
        code[4] = ENC_I(7, 0, 0, 15, 0x13);  /* 落点 BASE+16 */
        load(code, 5);
        hs.cpu.x[10] = MEM_BASE + 16;
        run(1);
        CHECK(hs.cpu.x[12] == MEM_BASE + 4); /* 写回 pc+4 */
        CHECK(hs.cpu.pc == MEM_BASE + 16);   /* pc = (rs1+imm)&~1 */
        run(1);
        CHECK(hs.cpu.x[15] == 7);            /* 落点指令衔接 */
        CHECK(hs.cpu.pc == MEM_BASE + 20);
        CHECK(hs.cpu.steps == 2);
    }

    /* JALR 负 imm：x10=BASE+20，imm=-8 → t=BASE+12 */
    reset_flow();
    {
        uint32_t code[3];
        code[0] = NOP;
        code[1] = NOP;
        code[2] = ENC_I(-8, 10, 0, 0, 0x67);
        load(code, 3);
        hs.cpu.x[10] = MEM_BASE + 20;
        hs.cpu.pc = MEM_BASE + 8;
        run(1);
        CHECK(hs.cpu.pc == MEM_BASE + 12);
        CHECK(hs.cpu.steps == 1);
    }

    /* JALR 清 bit0：目标奇数 BASE+17 → 落到偶数 BASE+16 执行 */
    reset_flow();
    {
        uint32_t code[6];
        code[0] = ENC_I(0, 10, 0, 12, 0x67);
        code[1] = NOP;
        code[2] = NOP;
        code[3] = NOP;
        code[4] = ENC_I(77, 0, 0, 13, 0x13); /* BASE+16：ADDI x13,x0,77 */
        code[5] = NOP;
        load(code, 6);
        hs.cpu.x[10] = MEM_BASE + 17;        /* bit0=1，须被清除 */
        run(2);
        CHECK(hs.cpu.x[13] == 77);           /* 已落到偶数地址 BASE+16 执行 */
        CHECK(hs.cpu.pc == MEM_BASE + 20);
        CHECK(hs.cpu.x[12] == MEM_BASE + 4);
        CHECK(hs.cpu.steps == 2);
    }

    /* JALR rd==rs1：目标用旧 rs1 计算，写回 pc+4 在后 */
    reset_flow();
    {
        uint32_t code[3];
        code[0] = NOP;
        code[1] = NOP;
        code[2] = ENC_I(0, 10, 0, 10, 0x67); /* JALR x10,0(x10) */
        load(code, 3);
        hs.cpu.x[10] = MEM_BASE + 8;
        hs.cpu.pc = MEM_BASE + 8;
        run(1);
        CHECK(hs.cpu.pc == MEM_BASE + 8);    /* 原地跳（旧 x10） */
        CHECK(hs.cpu.x[10] == MEM_BASE + 12);/* 写回旧 pc+4 */
    }

    /* ---- 分支 6 条：正例（taken → BASE+8）/ 反例（not taken → BASE+4） ----
       BLT vs BLTU 负数对照（同一对操作数结论相反）；BGE/BGEU 相等值 taken；
       BGEU 无符号大数 taken（同值下 BGE not taken）。 */
    br(ENC_B(8, 11, 10, 0, 0x63), 5, 5, MEM_BASE + 8);                     /* BEQ taken */
    br(ENC_B(8, 11, 10, 0, 0x63), 5, 6, MEM_BASE + 4);                     /* BEQ not */
    br(ENC_B(8, 11, 10, 1, 0x63), 5, 6, MEM_BASE + 8);                     /* BNE taken */
    br(ENC_B(8, 11, 10, 1, 0x63), 5, 5, MEM_BASE + 4);                     /* BNE not */
    br(ENC_B(8, 11, 10, 4, 0x63), (uint64_t)-1, 1, MEM_BASE + 8);          /* BLT taken（-1<1 有符号） */
    br(ENC_B(8, 11, 10, 4, 0x63), 1, (uint64_t)-1, MEM_BASE + 4);          /* BLT not（1<-1 否） */
    br(ENC_B(8, 11, 10, 5, 0x63), 5, 5, MEM_BASE + 8);                     /* BGE taken（相等） */
    br(ENC_B(8, 11, 10, 5, 0x63), (uint64_t)-2, 1, MEM_BASE + 4);          /* BGE not（-2>=1 否） */
    br(ENC_B(8, 11, 10, 6, 0x63), (uint64_t)-1, 1, MEM_BASE + 4);          /* BLTU not（同 BLT taken 值） */
    br(ENC_B(8, 11, 10, 6, 0x63), 1, (uint64_t)-1, MEM_BASE + 8);          /* BLTU taken（无符号大） */
    br(ENC_B(8, 11, 10, 7, 0x63), 5, 5, MEM_BASE + 8);                     /* BGEU taken（相等） */
    br(ENC_B(8, 11, 10, 7, 0x63), (uint64_t)-2, 1, MEM_BASE + 8);          /* BGEU taken（同 BGE not 值） */
    br(ENC_B(8, 11, 10, 7, 0x63), 1, (uint64_t)-2, MEM_BASE + 4);          /* BGEU not */
    br(ENC_B(8, 0, 0, 0, 0x63), 0, 0, MEM_BASE + 8);                       /* BEQ x0,x0 taken */

    /* B 负偏移回跳：计数循环两次后退出
       BASE+0  ADDI x14,x0,2
       BASE+4  ADDI x14,x14,-1
       BASE+8  BNE x14,x0,-4   → 回 BASE+4
       BASE+12 ADDI x15,x0,99  （出口标记） */
    reset_flow();
    {
        uint32_t code[4];
        code[0] = ENC_I(2, 0, 0, 14, 0x13);
        code[1] = ENC_I(-1, 14, 0, 14, 0x13);
        code[2] = ENC_B(-4, 0, 14, 1, 0x63);
        code[3] = ENC_I(99, 0, 0, 15, 0x13);
        load(code, 4);
        run(6);
        CHECK(hs.cpu.x[14] == 0);            /* 恰好递减两次 */
        CHECK(hs.cpu.x[15] == 99);           /* 已到出口 */
        CHECK(hs.cpu.pc == MEM_BASE + 16);
        CHECK(hs.cpu.steps == 6);            /* 1 + (1+1)*2 + 1+1 */
    }

    /* ---- 故障契约：目标未对齐 → RV_FV_MISALIGNED，零副作用 ---- */
    /* JAL 目标 2 对齐：rd 哨兵不写、pc 不动、steps 不增 */
    reset_flow();
    {
        uint32_t jal2 = ENC_J(2, 12, 0x6F);  /* 目标 BASE+2：2%4≠0 */
        hs.cpu.x[12] = SENTINEL;
        CHECK(mem_write(&hs.mem, MEM_BASE, &jal2, 4) == 0);
        CHECK(rv_step(&hs) == RV_FV_MISALIGNED);
        CHECK(hs.cpu.x[12] == SENTINEL);
        CHECK(hs.cpu.pc == MEM_BASE);
        CHECK(hs.cpu.steps == 0);
    }

    /* JALR 清 bit0 后仍 2 模 4：x10=BASE+15 → t=BASE+14 */
    reset_flow();
    {
        uint32_t jr = ENC_I(0, 10, 0, 12, 0x67);
        hs.cpu.x[12] = SENTINEL;
        CHECK(mem_write(&hs.mem, MEM_BASE, &jr, 4) == 0);
        hs.cpu.x[10] = MEM_BASE + 15;
        CHECK(rv_step(&hs) == RV_FV_MISALIGNED);
        CHECK(hs.cpu.x[12] == SENTINEL);
        CHECK(hs.cpu.pc == MEM_BASE);
        CHECK(hs.cpu.steps == 0);
    }

    /* 分支目标未对齐：BEQ x0,x0,+2 taken → 目标 BASE+2 */
    reset_flow();
    {
        uint32_t b2 = ENC_B(2, 0, 0, 0, 0x63);
        CHECK(mem_write(&hs.mem, MEM_BASE, &b2, 4) == 0);
        CHECK(rv_step(&hs) == RV_FV_MISALIGNED);
        CHECK(hs.cpu.pc == MEM_BASE);
        CHECK(hs.cpu.steps == 0);
    }

    TEST_DONE();
}
