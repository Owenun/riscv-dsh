/* tests/test_memops.c —— Task 8（M3）：访存 11 条执行臂 + 对齐/未映射故障。
   全路径：enc.h 编码 → 取指 → decode → 执行臂 → mem_read/write → 写回。
   期望值全部手算（逐条注释）；数据区为显式小端字节序列（RISC-V 加载/存储
   小端，与 mem 平坦字节语义一致）。
   覆盖（spec §8.3 访存边界清单）：
   - 11 条正例：LB/LBU/LH/LHU/LW/LWU/LD 每宽度符号+零扩展方向对照；
     SB/SH/SW/SD 写低 w 字节且邻居为 0（store 后 mem_read 回读核对）；
   - LW/LWU 同位型对照：0x80000000 → 0xFFFFFFFF80000000 vs 0x0000000080000000；
   - 对齐故障 2/4/8 三档反例：LH/LHU@+1、LW/LWU@+2、LD@+4、SH@+1、SW@+2、
     SD@+4 → MISALIGNED 且 rd/pc/steps 零副作用（store 另断言内存不变）；
     LB@+1、SB@+1 正例证明 1 字节访问无对齐要求；
   - 未映射：窗口内未登记地址的加载/存储 → UNMAPPED；区域右端点越界；
     SD 跨右端点（12 字节辅助区域上 ea 恰 8 对齐但 [ea,ea+8) 跨出区域末
     → UNMAPPED 且区域内前 4 字节无部分写）；
   - 地址回绕（spec §4.2）：rs1=0xFF..FF+imm=1 → ea=0（MEM_BASE 之下的未映射
     区，ea=0 恰好 8 对齐故先过对齐检查）→ UNMAPPED；rs1=0、imm=-1（LB）→
     ea=0xFF..FF（窗口之上）→ UNMAPPED——即按回绕后的有效地址访存；
   - x0：store 到 x0 基址（ea=x0+0=0 未映射 → UNMAPPED，且 x0 恒 0）；
     rd=x0 的加载写入丢弃（步进成功、x0 仍 0）；
   - S 型 rd 位段是 imm[4:0] 残留：store 不得按其写回寄存器。 */
#include "harness.h"
#include "enc.h"

#define DATA     (MEM_BASE + 0x100u)  /* 数据区基址（代码区 [MEM_BASE,+16) 之外） */
#define DATA_LEN 32u

/* 数据区前 12 字节预置内容（手写字节序列，小端）：
   [0..7]  = 80 7F 82 83 84 85 86 87
   [8..11] = 00 00 00 80  → word 0x80000000（LW/LWU 同位型对照输入） */
static const uint8_t PAT[12] = { 0x80, 0x7F, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
                                 0x00, 0x00, 0x00, 0x80 };

/* 每用例独立现场：reset_sim 重建代码区后纯追加数据区
   （依赖 mem_add_region 追加语义，见 harness.h 注释）。
   mem 后备存储只 calloc 一次跨用例复用，区域重建不清字节——显式清零数据区，
   保证「store 后邻居为 0」等断言与用例顺序无关。 */
static void setup(void)
{
    uint8_t z[DATA_LEN] = { 0 };
    reset_sim();
    mem_add_region(&hs.mem, DATA, DATA + DATA_LEN, 0);
    CHECK(mem_write(&hs.mem, DATA, z, DATA_LEN) == 0);
}

static void write_insn(uint32_t insn)
{
    CHECK(mem_write(&hs.mem, MEM_BASE, &insn, 4) == 0);
}

/* 加载正例：rs1 编码 x10=base，rd 编码 x12；单步后核对扩展结果与退休。 */
static void one_load(uint32_t insn, uint64_t base, uint64_t exp)
{
    setup();
    CHECK(mem_write(&hs.mem, DATA, PAT, sizeof PAT) == 0);
    write_insn(insn);
    hs.cpu.x[10] = base;
    CHECK(rv_step(&hs) == 0);
    CHECK(hs.cpu.x[12] == exp);
    CHECK(hs.cpu.pc == MEM_BASE + 4);
    CHECK(hs.cpu.steps == 1);
}

/* store 正例：rs1 编码 x10=DATA，rs2 编码 x11=val，imm=off；
   回读 DATA+off 起 w 字节逐一比对期望序列，并断言其后 8 字节仍为 0
   （宽度精确、无越界写）。要求 off+w+8 <= DATA_LEN。 */
static void one_store(uint32_t insn, uint64_t val, uint32_t off, uint32_t w,
                      const uint8_t *exp)
{
    uint8_t got[8] = { 0 }, tail[8];
    uint32_t i;
    setup();
    write_insn(insn);
    hs.cpu.x[10] = DATA;
    hs.cpu.x[11] = val;
    CHECK(rv_step(&hs) == 0);
    CHECK(hs.cpu.pc == MEM_BASE + 4);
    CHECK(hs.cpu.steps == 1);
    CHECK(mem_read(&hs.mem, DATA + off, got, w) == 0);
    CHECK(mem_read(&hs.mem, DATA + off + w, tail, 8) == 0);
    for (i = 0; i < w; i++)
        CHECK(got[i] == exp[i]);
    for (i = 0; i < 8; i++)
        CHECK(tail[i] == 0);
}

/* 加载故障用例：fault 正确、pc/steps 不动、rd=x12 保持哨兵（无写回副作用）。 */
static void fault_load(uint32_t insn, uint64_t rs1val, int fv)
{
    int r;
    setup();
    CHECK(mem_write(&hs.mem, DATA, PAT, sizeof PAT) == 0);
    write_insn(insn);
    hs.cpu.x[10] = rs1val;
    hs.cpu.x[12] = 0xDEADBEEFCAFEBABEULL;
    r = rv_step(&hs);
    CHECK(r == fv);
    CHECK(hs.cpu.x[12] == 0xDEADBEEFCAFEBABEULL);
    CHECK(hs.cpu.pc == MEM_BASE);
    CHECK(hs.cpu.steps == 0);
}

/* store 故障用例：fault 正确、pc/steps 不动、数据区 12 字节保持预置内容
   （对齐检查先于 mem_write，失败无部分写）。 */
static void fault_store(uint32_t insn, uint64_t rs1val, uint64_t rs2val, int fv)
{
    int r;
    uint8_t got[12];
    uint32_t i;
    setup();
    CHECK(mem_write(&hs.mem, DATA, PAT, sizeof PAT) == 0);
    write_insn(insn);
    hs.cpu.x[10] = rs1val;
    hs.cpu.x[11] = rs2val;
    r = rv_step(&hs);
    CHECK(r == fv);
    CHECK(hs.cpu.pc == MEM_BASE);
    CHECK(hs.cpu.steps == 0);
    CHECK(mem_read(&hs.mem, DATA, got, sizeof got) == 0);
    for (i = 0; i < sizeof PAT; i++)
        CHECK(got[i] == PAT[i]);
}

int main(void)
{
    /* ---- 加载 7 条：扩展方向对照（期望值手算，输入见 PAT） ---- */
    /* LB：PAT[0]=0x80（负）/ PAT[1]=0x7F（正）两方向；地址 DATA+1 顺带证明
       1 字节访问无对齐要求 */
    one_load(ENC_I(0, 10, 0, 12, 0x03), DATA, 0xFFFFFFFFFFFFFF80ULL);
    one_load(ENC_I(1, 10, 0, 12, 0x03), DATA, 0x000000000000007FULL);
    /* LBU：同字节零扩展（与 LB 首例对照） */
    one_load(ENC_I(0, 10, 4, 12, 0x03), DATA, 0x0000000000000080ULL);
    /* LH/LHU：PAT[0..1]=80 7F → 半字 0x7F80（正，两指令同值） */
    one_load(ENC_I(0, 10, 1, 12, 0x03), DATA, 0x0000000000007F80ULL);
    one_load(ENC_I(0, 10, 5, 12, 0x03), DATA, 0x0000000000007F80ULL);
    /* LH/LHU：PAT[2..3]=82 83 → 半字 0x8382（负值符号扩展 vs 零扩展对照） */
    one_load(ENC_I(2, 10, 1, 12, 0x03), DATA, 0xFFFFFFFFFFFF8382ULL);
    one_load(ENC_I(2, 10, 5, 12, 0x03), DATA, 0x0000000000008382ULL);
    /* LW/LWU：PAT[0..3] → word 0x83827F80（负值符号扩展 vs 零扩展对照） */
    one_load(ENC_I(0, 10, 2, 12, 0x03), DATA, 0xFFFFFFFF83827F80ULL);
    one_load(ENC_I(0, 10, 6, 12, 0x03), DATA, 0x0000000083827F80ULL);
    /* LW/LWU 同位型对照（spec §8.3）：word 0x80000000 的两种 64 位视图 */
    one_load(ENC_I(8, 10, 2, 12, 0x03), DATA, 0xFFFFFFFF80000000ULL);
    one_load(ENC_I(8, 10, 6, 12, 0x03), DATA, 0x0000000080000000ULL);
    /* LD：PAT[0..7] 全 8 字节 */
    one_load(ENC_I(0, 10, 3, 12, 0x03), DATA, 0x8786858483827F80ULL);

    /* ---- 存储 4 条：低 w 字节写入、截断正确、邻居为 0（期望序列手写） ---- */
    {
        uint8_t e8[8] = { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
        one_store(ENC_S(0, 11, 10, 3, 0x23), 0x1122334455667788ULL, 0, 8, e8); /* SD */
    }
    {
        uint8_t e4[4] = { 0x88, 0x77, 0x66, 0x55 };
        one_store(ENC_S(0, 11, 10, 2, 0x23), 0x1122334455667788ULL, 0, 4, e4); /* SW */
    }
    {
        uint8_t e2[2] = { 0x88, 0x77 };
        one_store(ENC_S(0, 11, 10, 1, 0x23), 0x1122334455667788ULL, 0, 2, e2); /* SH */
    }
    {
        uint8_t e1[1] = { 0x88 };
        one_store(ENC_S(0, 11, 10, 0, 0x23), 0x1122334455667788ULL, 0, 1, e1); /* SB */
    }
    {
        uint8_t e1[1] = { 0x5A };
        /* SB @DATA+1：1 字节无对齐要求（对齐正例反例矩阵的 +1 档） */
        one_store(ENC_S(1, 11, 10, 0, 0x23), 0x5AULL, 1, 1, e1);
    }
    /* S 型 rd 位段=imm[4:0] 残留：SB x11,8(x10) 的 rd 位段=8，x8 不得被写 */
    {
        uint8_t g;
        setup();
        write_insn(ENC_S(8, 11, 10, 0, 0x23));
        hs.cpu.x[10] = DATA;
        hs.cpu.x[11] = 0x5A;
        hs.cpu.x[8] = 0xA5A5A5A5A5A5A5A5ULL;
        CHECK(rv_step(&hs) == 0);
        CHECK(hs.cpu.x[8] == 0xA5A5A5A5A5A5A5A5ULL);
        CHECK(mem_read(&hs.mem, DATA + 8, &g, 1) == 0);
        CHECK(g == 0x5A);
    }

    /* ---- 对齐故障：2/4/8 三档反例（对齐检查先于访存，零副作用） ---- */
    fault_load(ENC_I(1, 10, 1, 12, 0x03), DATA, RV_FV_MISALIGNED);  /* LH  @DATA+1 */
    fault_load(ENC_I(1, 10, 5, 12, 0x03), DATA, RV_FV_MISALIGNED);  /* LHU @DATA+1 */
    fault_load(ENC_I(2, 10, 2, 12, 0x03), DATA, RV_FV_MISALIGNED);  /* LW  @DATA+2 */
    fault_load(ENC_I(2, 10, 6, 12, 0x03), DATA, RV_FV_MISALIGNED);  /* LWU @DATA+2 */
    fault_load(ENC_I(4, 10, 3, 12, 0x03), DATA, RV_FV_MISALIGNED);  /* LD  @DATA+4 */
    fault_store(ENC_S(1, 11, 10, 1, 0x23), DATA, 0x1122334455667788ULL,
                RV_FV_MISALIGNED);                                  /* SH @DATA+1 */
    fault_store(ENC_S(2, 11, 10, 2, 0x23), DATA, 0x1122334455667788ULL,
                RV_FV_MISALIGNED);                                  /* SW @DATA+2 */
    fault_store(ENC_S(4, 11, 10, 3, 0x23), DATA, 0x1122334455667788ULL,
                RV_FV_MISALIGNED);                                  /* SD @DATA+4 */

    /* ---- 未映射：窗口内未登记地址（仅代码区+数据区已登记） ---- */
    fault_load(ENC_I(0x400, 10, 3, 12, 0x03), DATA, RV_FV_UNMAPPED); /* LD  @DATA+0x400 */
    fault_load(ENC_I(0x400, 10, 0, 12, 0x03), DATA, RV_FV_UNMAPPED); /* LB  @DATA+0x400 */
    fault_store(ENC_S(0x400, 11, 10, 3, 0x23), DATA, 0x99ULL,
                RV_FV_UNMAPPED);                                     /* SD  @DATA+0x400 */
    /* 区域右端点：LD @DATA+DATA_LEN 恰好整段出界 */
    fault_load(ENC_I(DATA_LEN, 10, 3, 12, 0x03), DATA, RV_FV_UNMAPPED);
    /* 未映射跨界用例（M3 修复轮补充，评审「有牙」项）：SD 目标
       [区域末-4, 区域末+4) 且 ea 8 对齐。32 字节数据区的 DATA+28 ≡ 4
       (mod 8) 不满足 8 对齐（会先报 MISALIGNED，探不到 mem 区间检查），
       故本用例现场追加辅助区域 R=[DATA+0x100, +12)（12 ≡ 4 mod 8，使
       R+8 恰 8 对齐）：SD 目标 [R+8, R+16) 跨出区域末 R+12 → 对齐通过、
       mem_write 整体拒绝 → UNMAPPED；另断言区域内前 4 字节（R+8..11，
       已显式清零）保持 0——访存整体拒绝、无部分写（若先落低 4 字节再
       越界即在此被抓）。R 与数据区不相交，随下一用例 setup() 重建区域表
       而消失，不影响其他用例。 */
    {
        uint8_t z[12] = { 0 };
        uint8_t got[4];
        uint32_t i;
        uint64_t R = DATA + 0x100;
        setup();
        mem_add_region(&hs.mem, R, R + 12, 0);
        CHECK(mem_write(&hs.mem, R, z, sizeof z) == 0);
        write_insn(ENC_S(8, 11, 10, 3, 0x23));   /* SD x11, 8(x10) */
        hs.cpu.x[10] = R;
        hs.cpu.x[11] = 0x1122334455667788ULL;
        CHECK(rv_step(&hs) == RV_FV_UNMAPPED);
        CHECK(hs.cpu.pc == MEM_BASE);
        CHECK(hs.cpu.steps == 0);
        CHECK(mem_read(&hs.mem, R + 8, got, 4) == 0);
        for (i = 0; i < 4; i++)
            CHECK(got[i] == 0);          /* 区域内前 4 字节无部分写 */
    }

    /* ---- 地址回绕（spec §4.2：ea=rs1+imm 按 2^64 回绕后访存） ----
       rs1=0xFFFFFFFFFFFFFFFF、imm=1 → ea=0：MEM_BASE 之下的未映射区；
       ea=0 恰好 8 对齐，故先过对齐检查再报 UNMAPPED。 */
    fault_load(ENC_I(1, 10, 3, 12, 0x03), 0xFFFFFFFFFFFFFFFFULL, RV_FV_UNMAPPED);
    /* rs1=0、imm=-1（LB 无对齐要求）→ ea=0xFFFFFFFFFFFFFFFF：窗口之上 → UNMAPPED */
    fault_load(ENC_I(-1, 10, 0, 12, 0x03), 0, RV_FV_UNMAPPED);

    /* ---- x0：store 到 x0 基址（ea=x0+0=0 → UNMAPPED；x0 恒 0） ---- */
    {
        uint32_t insn = ENC_S(0, 11, 0, 0, 0x23); /* SB x11, 0(x0)：rs1 编码 x0 */
        setup();
        CHECK(mem_write(&hs.mem, DATA, PAT, sizeof PAT) == 0);
        write_insn(insn);
        hs.cpu.x[11] = 0xAA;
        CHECK(rv_step(&hs) == RV_FV_UNMAPPED);
        CHECK(hs.cpu.x[0] == 0);          /* x0 恒 0 */
        CHECK(hs.cpu.pc == MEM_BASE);
        CHECK(hs.cpu.steps == 0);
    }
    /* rd=x0 的加载：写入丢弃（步进成功退休，x0 仍 0） */
    {
        setup();
        CHECK(mem_write(&hs.mem, DATA, PAT, sizeof PAT) == 0);
        write_insn(ENC_I(0, 10, 6, 0, 0x03)); /* LWU x0, 0(x10) */
        hs.cpu.x[10] = DATA;
        CHECK(rv_step(&hs) == 0);
        CHECK(hs.cpu.x[0] == 0);
        CHECK(hs.cpu.pc == MEM_BASE + 4);
        CHECK(hs.cpu.steps == 1);
    }

    TEST_DONE();
}
