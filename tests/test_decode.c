/* tests/test_decode.c —— Task 5：rv_decode 数据驱动测试（TDD 先行）。
   期望值全部手算（推导见 sdd/task-5-report.md 覆盖矩阵），禁止用实现生成。
   覆盖：全部 rv_op 逐条可解码、各格式（I/S/B/U/J/R）极值与符号扩展、
   R 型/W 型 funct7×funct3 全矩阵、保留编码黑名单（M 扩展、AMO 0x2F、
   非法低 2 位、移位 imm[11:6] 非零、CSR/mret/wfi、fence.tso、压缩编码等）。
   修复轮 1（C-2）：W 类移位 shamt>=32（bit25=1）为保留编码，一律 false。 */
#include <stdio.h>
#include <stdbool.h>
#include "rvsim.h"
#include "framework.h"
#include "enc.h"

static int g_cases;   /* 用例计数（自检用） */

/* 单用例执行：ok=1 时校验 op/rd/rs1/rs2/imm（-1 = 该字段不检查），
   ok=0 时仅要求 rv_decode 返回 false。失败时打印期望 vs 实际。 */
static bool check_case(const char *name, uint32_t insn, int ok, int op,
                       int rd, int rs1, int rs2, int has_imm, int64_t imm)
{
    rv_dec d;
    bool got = rv_decode(insn, &d);
    int pass;

    g_cases++;
    if (ok) {
        pass = got
            && (op  < 0 || (int)d.op  == op)
            && (rd  < 0 || (int)d.rd  == rd)
            && (rs1 < 0 || (int)d.rs1 == rs1)
            && (rs2 < 0 || (int)d.rs2 == rs2)
            && (!has_imm || d.imm == imm);
    } else {
        pass = !got;
    }
    if (!pass) {
        printf("      want %s: insn=0x%08X ok=%d op=%d rd=%d rs1=%d rs2=%d imm=%lld"
               " | got ok=%d op=%d rd=%u rs1=%u rs2=%u imm=%lld\n",
               name, insn, ok, op, rd, rs1, rs2, (long long)imm,
               got, (int)d.op, d.rd, d.rs1, d.rs2, (long long)d.imm);
    }
    return pass;
}

int main(void)
{
    rv_dec d;
    char nm[64];
    int f7i, f3;

    /* ---- A 组：R 型 0x33 全 funct7(0x00/0x20)×funct3(0..7) 矩阵 ----
       f7=0x00: ADD SLL SLT SLTU XOR SRL OR AND；f7=0x20: 仅 SUB(f3=0)/SRA(f3=5)，
       其余 6 组合保留。同时校验 rd/rs1/rs2 字段提取。 */
    {
        static const int rmap[2][8] = {
            { RV_ADD, RV_SLL, RV_SLT, RV_SLTU, RV_XOR, RV_SRL, RV_OR, RV_AND },
            { RV_SUB, -1, -1, -1, -1, RV_SRA, -1, -1 }
        };
        for (f7i = 0; f7i < 2; f7i++) {
            for (f3 = 0; f3 < 8; f3++) {
                uint32_t f7 = (f7i == 0) ? 0x00u : 0x20u;
                int want = rmap[f7i][f3];
                snprintf(nm, sizeof nm, "R33 f7=%02X f3=%d", (unsigned)f7, f3);
                if (want < 0)
                    CHECK(check_case(nm, OP_STD(f7, 3, 2, f3, 1, 0x33), 0, 0, -1, -1, -1, 0, 0));
                else
                    CHECK(check_case(nm, OP_STD(f7, 3, 2, f3, 1, 0x33), 1, want, 1, 2, 3, 0, 0));
            }
        }
    }

    /* ---- B 组：W 寄存器型 0x3B 全 funct7(0x00/0x20)×funct3(0..7) 矩阵 ----
       f7=0x00: ADDW/SLLW/SRLW；f7=0x20: SUBW/SRAW；其余保留（含 f7=0x01 之外的
       一切 funct7，MULW 由 D 组黑名单单独覆盖）。 */
    {
        static const int wmap[2][8] = {
            { RV_ADDW, RV_SLLW, -1, -1, -1, RV_SRLW, -1, -1 },
            { RV_SUBW, -1, -1, -1, -1, RV_SRAW, -1, -1 }
        };
        for (f7i = 0; f7i < 2; f7i++) {
            for (f3 = 0; f3 < 8; f3++) {
                uint32_t f7 = (f7i == 0) ? 0x00u : 0x20u;
                int want = wmap[f7i][f3];
                snprintf(nm, sizeof nm, "R3B f7=%02X f3=%d", (unsigned)f7, f3);
                if (want < 0)
                    CHECK(check_case(nm, OP_STD(f7, 3, 2, f3, 1, 0x3B), 0, 0, -1, -1, -1, 0, 0));
                else
                    CHECK(check_case(nm, OP_STD(f7, 3, 2, f3, 1, 0x3B), 1, want, 1, 2, 3, 0, 0));
            }
        }
    }

    /* ---- C 组：I 型算术（0x13）极值/符号扩展 + 移位 shamt 边界 ---- */
    CHECK(check_case("addi imm=-1",        ENC_I(-1,    2, 0, 1, 0x13), 1, RV_ADDI,  1, 2, -1, 1, -1));
    CHECK(check_case("addi imm=+2047max",  ENC_I(0x7FF, 2, 0, 1, 0x13), 1, RV_ADDI,  1, 2, -1, 1, 2047));
    CHECK(check_case("addi imm=-2048min",  ENC_I(-2048, 2, 0, 1, 0x13), 1, RV_ADDI,  1, 2, -1, 1, -2048));
    CHECK(check_case("slti imm=2047",      ENC_I(0x7FF, 2, 2, 1, 0x13), 1, RV_SLTI,  1, 2, -1, 1, 2047));
    CHECK(check_case("sltiu imm=-1",       ENC_I(-1,    2, 3, 1, 0x13), 1, RV_SLTIU, 1, 2, -1, 1, -1));
    CHECK(check_case("xori imm=0x123",     ENC_I(0x123, 2, 4, 1, 0x13), 1, RV_XORI,  1, 2, -1, 1, 0x123));
    CHECK(check_case("ori imm=-16",        ENC_I(-16,   2, 6, 1, 0x13), 1, RV_ORI,   1, 2, -1, 1, -16));
    CHECK(check_case("andi imm=0x7F",      ENC_I(0x7F,  2, 7, 1, 0x13), 1, RV_ANDI,  1, 2, -1, 1, 0x7F));
    CHECK(check_case("slli shamt=31",      ENC_I(31,    2, 1, 1, 0x13), 1, RV_SLLI,  1, 2, -1, 1, 31));
    CHECK(check_case("slli shamt=63max",   ENC_I(63,    2, 1, 1, 0x13), 1, RV_SLLI,  1, 2, -1, 1, 63));
    CHECK(check_case("srli shamt=1",       ENC_I(1,     2, 5, 1, 0x13), 1, RV_SRLI,  1, 2, -1, 1, 1));
    CHECK(check_case("srai shamt=5",       ENC_I(0x405, 2, 5, 1, 0x13), 1, RV_SRAI,  1, 2, -1, 1, 5));
    CHECK(check_case("srai shamt=63",      ENC_I(0x43F, 2, 5, 1, 0x13), 1, RV_SRAI,  1, 2, -1, 1, 63));
    /* 移位类保留编码：imm[11:6] != 0（SLLI/SRLI 需 000000，SRAI 需 010000）。
       非 W 类为 6 位 shamt（RV64I），shamt=63 合法——与 H 组 W 类对照，防误伤。 */
    CHECK(check_case("slli hi=0x3F bad",   ENC_I(0xFC0, 1, 1, 1, 0x13), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("slli hi=0x01 bad",   ENC_I(0x40,  1, 1, 1, 0x13), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("srli hi=0x1F bad",   ENC_I(0x7C0, 1, 5, 1, 0x13), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("srai hi=0x11 bad",   ENC_I(0x445, 1, 5, 1, 0x13), 0, 0, -1, -1, -1, 0, 0));

    /* ---- D 组：load（0x03）全 funct3 + 极值 ---- */
    CHECK(check_case("lb imm=-1",      ENC_I(-1,    2, 0, 1, 0x03), 1, RV_LB,  1, 2, -1, 1, -1));
    CHECK(check_case("lh imm=+2047",   ENC_I(0x7FF, 2, 1, 1, 0x03), 1, RV_LH,  1, 2, -1, 1, 2047));
    CHECK(check_case("lw imm=-2048",   ENC_I(-2048, 2, 2, 1, 0x03), 1, RV_LW,  1, 2, -1, 1, -2048));
    CHECK(check_case("ld imm=8",       ENC_I(8,     2, 3, 1, 0x03), 1, RV_LD,  1, 2, -1, 1, 8));
    CHECK(check_case("lbu imm=0xFFF",  ENC_I(0xFFF, 2, 4, 1, 0x03), 1, RV_LBU, 1, 2, -1, 1, -1));
    CHECK(check_case("lhu imm=0x800",  ENC_I(0x800, 2, 5, 1, 0x03), 1, RV_LHU, 1, 2, -1, 1, -2048));
    CHECK(check_case("lwu imm=4",      ENC_I(4,     2, 6, 1, 0x03), 1, RV_LWU, 1, 2, -1, 1, 4));
    CHECK(check_case("load f3=7 bad",  ENC_I(0,     2, 7, 1, 0x03), 0, 0, -1, -1, -1, 0, 0));

    /* ---- E 组：store（0x23）全 funct3 + 极值 ---- */
    CHECK(check_case("sb imm=-1",      ENC_S(-1,    3, 2, 0, 0x23), 1, RV_SB, -1, 2, 3, 1, -1));
    CHECK(check_case("sh imm=+2047",   ENC_S(0x7FF, 3, 2, 1, 0x23), 1, RV_SH, -1, 2, 3, 1, 2047));
    CHECK(check_case("sw imm=-2048",   ENC_S(-2048, 3, 2, 2, 0x23), 1, RV_SW, -1, 2, 3, 1, -2048));
    CHECK(check_case("sd imm=31",      ENC_S(31,    3, 2, 3, 0x23), 1, RV_SD, -1, 2, 3, 1, 31));
    CHECK(check_case("store f3=4 bad", ENC_S(1,     3, 2, 4, 0x23), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("store f3=7 bad", ENC_S(1,     3, 2, 7, 0x23), 0, 0, -1, -1, -1, 0, 0));

    /* ---- F 组：branch（0x63）全 funct3 + B 型极值（-4096..+4094，恒偶） ---- */
    CHECK(check_case("beq imm=+8",     ENC_B(8,    3, 2, 0, 0x63), 1, RV_BEQ,  -1, 2, 3, 1, 8));
    CHECK(check_case("bne imm=-2",     ENC_B(-2,   3, 2, 1, 0x63), 1, RV_BNE,  -1, 2, 3, 1, -2));
    CHECK(check_case("blt imm=+4094",  ENC_B(4094, 3, 2, 4, 0x63), 1, RV_BLT,  -1, 2, 3, 1, 4094));
    CHECK(check_case("bge imm=-4096",  ENC_B(-4096,3, 2, 5, 0x63), 1, RV_BGE,  -1, 2, 3, 1, -4096));
    CHECK(check_case("bltu imm=0",     ENC_B(0,    3, 2, 6, 0x63), 1, RV_BLTU, -1, 2, 3, 1, 0));
    CHECK(check_case("bgeu imm=+2",    ENC_B(2,    3, 2, 7, 0x63), 1, RV_BGEU, -1, 2, 3, 1, 2));
    CHECK(check_case("branch f3=2 bad", ENC_B(4,   3, 2, 2, 0x63), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("branch f3=3 bad", ENC_B(4,   3, 2, 3, 0x63), 0, 0, -1, -1, -1, 0, 0));

    /* ---- G 组：U/J 型极值与符号扩展 ---- */
    CHECK(check_case("lui 0xFFFFF",      ENC_U(0xFFFFF, 5, 0x37), 1, RV_LUI,   5, -1, -1, 1, -4096));
    CHECK(check_case("lui 0",            ENC_U(0,       5, 0x37), 1, RV_LUI,   5, -1, -1, 1, 0));
    CHECK(check_case("lui 0x80000 int32min", ENC_U(0x80000, 1, 0x37), 1, RV_LUI, 1, -1, -1, 1, -2147483648LL));
    CHECK(check_case("auipc 0x12345",    ENC_U(0x12345, 5, 0x17), 1, RV_AUIPC, 5, -1, -1, 1, 0x12345000LL));
    CHECK(check_case("auipc 0xFFFED neg",ENC_U(0xFFFED, 5, 0x17), 1, RV_AUIPC, 5, -1, -1, 1, -77824LL));
    CHECK(check_case("jal -1048576min",  ENC_J(0x100000, 1, 0x6F), 1, RV_JAL, 1, -1, -1, 1, -1048576LL));
    CHECK(check_case("jal +1048574max",  ENC_J(0xFFFFE,  1, 0x6F), 1, RV_JAL, 1, -1, -1, 1, 1048574LL));
    CHECK(check_case("jal +4",           ENC_J(4,        1, 0x6F), 1, RV_JAL, 1, -1, -1, 1, 4));
    CHECK(check_case("jalr imm=-4",      ENC_I(-4, 2, 0, 1, 0x67), 1, RV_JALR, 1, 2, -1, 1, -4));
    CHECK(check_case("jalr f3=1 bad",    ENC_I(0,  2, 1, 1, 0x67), 0, 0, -1, -1, -1, 0, 0));

    /* ---- H 组：W 型立即数（0x1B）；ADDIW 是普通 I 型（shamt 规则不适用） ---- */
    CHECK(check_case("addiw imm=-1",       ENC_I(-1,    2, 0, 1, 0x1B), 1, RV_ADDIW, 1, 2, -1, 1, -1));
    CHECK(check_case("addiw 0xFC0 legal",  ENC_I(0xFC0, 2, 0, 1, 0x1B), 1, RV_ADDIW, 1, 2, -1, 1, -64));
    CHECK(check_case("slliw shamt=31",     ENC_I(31,    2, 1, 1, 0x1B), 1, RV_SLLIW, 1, 2, -1, 1, 31));
    CHECK(check_case("srliw shamt=3",      ENC_I(3,     2, 5, 1, 0x1B), 1, RV_SRLIW, 1, 2, -1, 1, 3));
    CHECK(check_case("sraiw shamt=4",      ENC_I(0x404, 2, 5, 1, 0x1B), 1, RV_SRAIW, 1, 2, -1, 1, 4));
    CHECK(check_case("sraiw shamt=31max",  ENC_I(0x41F, 2, 5, 1, 0x1B), 1, RV_SRAIW, 1, 2, -1, 1, 31));
    /* W 类移位保留编码：imm[11:5] 须为 0000000（SLLIW/SRLIW）/0100000（SRAIW），
       shamt 仅 5 位，bit25（imm[5]）=1 即 shamt>=32 一律保留（修复轮 1：C-2）。
       注意非 W 类 SRAI 的 6 位 shamt（如 shamt=63）仍合法，不得误伤（C 组对照）。 */
    CHECK(check_case("sraiw shamt=63 bad", ENC_I(0x43F, 2, 5, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("slliw shamt=32 bad", ENC_I(32,    2, 1, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("slliw shamt=63 bad", ENC_I(63,    2, 1, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("srliw shamt=32 bad", ENC_I(32,    2, 5, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("srliw shamt=63 bad", ENC_I(63,    2, 5, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("sraiw shamt=32 bad", ENC_I(0x420, 2, 5, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("slliw hi!=0 bad",    ENC_I(0xFC0, 2, 1, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("sraiw hi=0x11 bad",  ENC_I(0x445, 2, 5, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("op-imm-32 f3=2 bad", ENC_I(1,     2, 2, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("op-imm-32 f3=7 bad", ENC_I(1,     2, 7, 1, 0x1B), 0, 0, -1, -1, -1, 0, 0));

    /* ---- I 组：FENCE/FENCE.I/ECALL/EBREAK + 非法系统编码 ----
       FENCE=0x0FF0000F（fm=0, rd=rs1=0, pred/succ=1111）；FENCE.I=0x0000100F；
       ECALL=0x00000073（imm=0）；EBREAK=0x00100073（imm=1）。
       fence.i 的 imm 保留"应为零"（Zifencei），imm!=0 一律 false（修复轮 1）。 */
    CHECK(check_case("fence std",        0x0FF0000Fu, 1, RV_FENCE,  -1, -1, -1, 0, 0));
    CHECK(check_case("fence pred=succ=0",0x0000000Fu, 1, RV_FENCE,  -1, -1, -1, 0, 0));
    CHECK(check_case("fence.i",          0x0000100Fu, 1, RV_FENCEI, -1, -1, -1, 0, 0));
    CHECK(check_case("fence.i rd!=0 bad",0x000010AFu, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("fence.i imm!=0 bad",0x0010100Fu, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("fence f3=2 bad",   0x0000200Fu, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("fence.tso fm!=0 bad", 0x8330000Fu, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("ecall",            0x00000073u, 1, RV_ECALL,  -1, -1, -1, 0, 0));
    CHECK(check_case("ebreak",           0x00100073u, 1, RV_EBREAK, -1, -1, -1, 0, 0));
    CHECK(check_case("system imm=2 bad", 0x00200073u, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("mret bad",         0x30200073u, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("wfi bad",          0x10500073u, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("csr f3!=0 bad",    0x00002573u, 0, 0, -1, -1, -1, 0, 0));

    /* ---- J 组：保留编码黑名单 ---- */
    CHECK(check_case("MUL M-f7 bad",       OP_STD(0x01, 3, 2, 0, 1, 0x33), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("DIVU M-f7 f3=5 bad", OP_STD(0x01, 3, 2, 5, 1, 0x33), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("MULW bad",           OP_STD(0x01, 3, 2, 0, 1, 0x3B), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("R33 f7=0x7F bad",    OP_STD(0x7F, 3, 2, 0, 1, 0x33), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("AMOADD.W bad",       0x0000002Fu | (2u << 27), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("LR.W bad",           0x1000202Fu, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("0x15 low2=01 bad",   0x00000013u + 2u, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("0x01 low2=01 bad",   0x00000001u, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("0x02 low2=10 bad",   0x00000002u, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("0x00000000 bad",     0x00000000u, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("all-ones 0xFFFFFFFF bad", 0xFFFFFFFFu, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("custom-0 0x0B bad",  0x0000000Bu, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("0x6B bad",           0x0000006Bu, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("0x77 bad",           0x00000077u, 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("load-fp 0x07 bad",   ENC_I(0, 2, 2, 1, 0x07), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("store-fp 0x27 bad",  ENC_S(0, 3, 2, 2, 0x27), 0, 0, -1, -1, -1, 0, 0));
    CHECK(check_case("fmadd 0x43 bad",     0x00000043u, 0, 0, -1, -1, -1, 0, 0));

    printf("decode cases: %d\n", g_cases);
    (void)d;
    TEST_DONE();
}
