/* src/decode.c —— RV64I 用户态模拟器：32 位指令字 → 结构化解码（纯函数，不执行）。
   覆盖 spec §4.2 指令表全部条目（rv_op 逐条，无占位）；opcode/funct3/funct6/funct7
   不匹配的组合一律返回 false（非保留=false，上层报退出码 204）。
   imm 语义（钉死）：I/S 符号扩展 12 位；B/J 符号扩展且最低位 0（编码不含
   imm[0]，恒偶）；U = insn&0xFFFFF000 符号扩展到 64；移位类 imm=imm[5:0]
   正数，非 W 类（SLLI/SRLI/SRAI）为 6 位 shamt：SLLI/SRLI 要求
   imm[11:6]=000000，SRAI 要求 imm[11:6]=010000；W 类（SLLIW/SRLIW/SRAIW）
   shamt 仅 5 位，imm[11:5]（=funct7 位段，含 bit25）须为 0000000/0100000
   （修复轮 1：C-2，shamt>=32 为保留编码）；W 类整数指令（ADDIW）imm 为
   普通 I 型符号扩展。 */

#include "rvsim.h"

/* 符号扩展：v < 2^bits，返回 64 位补码值。
   用减法而非算术右移，避免依赖实现定义行为（C99 不保证负数右移为算术移位）。 */
static int64_t sext(uint32_t v, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);

    return (v & sign) ? (int64_t)v - 2 * (int64_t)sign : (int64_t)v;
}

/* I 型：imm[11:0] = insn[31:20]，符号扩展 */
static int64_t imm_i(uint32_t insn)
{
    return sext(insn >> 20, 12);
}

/* S 型：imm[11:5]|imm[4:0] = insn[31:25]|insn[11:7]，符号扩展 */
static int64_t imm_s(uint32_t insn)
{
    return sext(((insn >> 20) & 0xFE0u) | ((insn >> 7) & 0x1Fu), 12);
}

/* B 型：imm[12|10:5|4:1|11] = insn[31|30:25|11:8|7]，符号扩展 13 位；
   编码不含 imm[0]（恒 0），故结果恒为偶数，无需额外判 LSB。 */
static int64_t imm_b(uint32_t insn)
{
    uint32_t v = ((insn >> 31) & 1u) << 12
               | ((insn >> 7) & 1u) << 11
               | ((insn >> 25) & 0x3Fu) << 5
               | ((insn >> 8) & 0xFu) << 1;

    return sext(v, 13);
}

/* J 型：imm[20|10:1|11|19:12] = insn[31|30:21|20|19:12]，符号扩展 21 位；
   同 B 型，结果恒偶。 */
static int64_t imm_j(uint32_t insn)
{
    uint32_t v = ((insn >> 31) & 1u) << 20
               | ((insn >> 12) & 0xFFu) << 12
               | ((insn >> 20) & 1u) << 11
               | ((insn >> 21) & 0x3FFu) << 1;

    return sext(v, 21);
}

/* 移位量：imm[5:0] = insn[25:20]，恒为非负数（0..63） */
static int64_t imm_sh(uint32_t insn)
{
    return (int64_t)((insn >> 20) & 0x3Fu);
}

bool rv_decode(uint32_t insn, rv_dec *d)
{
    uint32_t opc, f3, f6, f7;

    if (d == NULL)
        return false;
    d->raw = insn;
    if ((insn & 3u) != 3u)          /* 低 2 位非 11：压缩/非法编码（含 0x0000） */
        goto illegal;

    d->rd  = (uint8_t)((insn >> 7) & 0x1Fu);
    d->rs1 = (uint8_t)((insn >> 15) & 0x1Fu);
    d->rs2 = (uint8_t)((insn >> 20) & 0x1Fu);
    d->imm = 0;

    opc = insn & 0x7Fu;
    f3  = (insn >> 12) & 7u;
    f6  = (insn >> 26) & 0x3Fu;     /* = imm[11:6]，移位类校验用 */
    f7  = (insn >> 25) & 0x7Fu;     /* R 型 funct7；I 型移位 = imm[11:5] */

    switch (opc) {
    case 0x37u:                     /* LUI */
        d->op = RV_LUI;
        d->imm = sext(insn >> 12, 20) << 12;    /* = (int32)(insn & 0xFFFFF000) */
        return true;
    case 0x17u:                     /* AUIPC */
        d->op = RV_AUIPC;
        d->imm = sext(insn >> 12, 20) << 12;
        return true;
    case 0x6Fu:                     /* JAL */
        d->op = RV_JAL;
        d->imm = imm_j(insn);
        return true;
    case 0x67u:                     /* JALR：仅 funct3=000 */
        if (f3 != 0u)
            goto illegal;
        d->op = RV_JALR;
        d->imm = imm_i(insn);
        return true;
    case 0x63u:                     /* branch：f3=2/3 保留 */
        switch (f3) {
        case 0u: d->op = RV_BEQ;  break;
        case 1u: d->op = RV_BNE;  break;
        case 4u: d->op = RV_BLT;  break;
        case 5u: d->op = RV_BGE;  break;
        case 6u: d->op = RV_BLTU; break;
        case 7u: d->op = RV_BGEU; break;
        default: goto illegal;
        }
        d->imm = imm_b(insn);
        return true;
    case 0x03u:                     /* load：f3=7 保留 */
        switch (f3) {
        case 0u: d->op = RV_LB;  break;
        case 1u: d->op = RV_LH;  break;
        case 2u: d->op = RV_LW;  break;
        case 3u: d->op = RV_LD;  break;
        case 4u: d->op = RV_LBU; break;
        case 5u: d->op = RV_LHU; break;
        case 6u: d->op = RV_LWU; break;
        default: goto illegal;
        }
        d->imm = imm_i(insn);
        return true;
    case 0x23u:                     /* store：f3=4..7 保留 */
        switch (f3) {
        case 0u: d->op = RV_SB; break;
        case 1u: d->op = RV_SH; break;
        case 2u: d->op = RV_SW; break;
        case 3u: d->op = RV_SD; break;
        default: goto illegal;
        }
        d->imm = imm_s(insn);
        return true;
    case 0x13u:                     /* op-imm：非 W 类移位为 6 位 shamt（RV64I） */
        switch (f3) {
        case 0u: d->op = RV_ADDI;  d->imm = imm_i(insn); return true;
        case 2u: d->op = RV_SLTI;  d->imm = imm_i(insn); return true;
        case 3u: d->op = RV_SLTIU; d->imm = imm_i(insn); return true;
        case 4u: d->op = RV_XORI;  d->imm = imm_i(insn); return true;
        case 6u: d->op = RV_ORI;   d->imm = imm_i(insn); return true;
        case 7u: d->op = RV_ANDI;  d->imm = imm_i(insn); return true;
        case 1u:                    /* SLLI：imm[11:6] 须为 000000 */
            if (f6 != 0u)
                goto illegal;
            d->op = RV_SLLI;
            break;
        case 5u:                    /* SRLI:000000 / SRAI:010000（6 位 shamt） */
            if (f6 == 0u)
                d->op = RV_SRLI;
            else if (f6 == 0x10u)
                d->op = RV_SRAI;
            else
                goto illegal;
            break;
        default:
            goto illegal;
        }
        d->imm = imm_sh(insn);
        return true;
    case 0x33u:                     /* op：f7=0000000 全部 8 条；f7=0100000 仅 SUB/SRA；
                                       其余 funct7（含 M 扩展 0000001）非法 */
        if (f7 == 0x00u) {
            switch (f3) {
            case 0u: d->op = RV_ADD;  break;
            case 1u: d->op = RV_SLL;  break;
            case 2u: d->op = RV_SLT;  break;
            case 3u: d->op = RV_SLTU; break;
            case 4u: d->op = RV_XOR;  break;
            case 5u: d->op = RV_SRL;  break;
            case 6u: d->op = RV_OR;   break;
            default: d->op = RV_AND;  break;    /* f3=7 */
            }
        } else if (f7 == 0x20u) {
            if (f3 == 0u)
                d->op = RV_SUB;
            else if (f3 == 5u)
                d->op = RV_SRA;
            else
                goto illegal;
        } else {
            goto illegal;
        }
        return true;
    case 0x1Bu:                     /* op-imm-32：W 类移位 shamt 仅 5 位，
                                       imm[11:5]（含 bit25）精确校验（C-2） */
        switch (f3) {
        case 0u:                    /* ADDIW：普通 I 型符号扩展，无 shamt 校验 */
            d->op = RV_ADDIW;
            d->imm = imm_i(insn);
            return true;
        case 1u:                    /* SLLIW：imm[11:5] 须为 0000000 */
            if (f7 != 0u)
                goto illegal;
            d->op = RV_SLLIW;
            break;
        case 5u:                    /* SRLIW:0000000 / SRAIW:0100000（bit25=0） */
            if (f7 == 0x00u)
                d->op = RV_SRLIW;
            else if (f7 == 0x20u)
                d->op = RV_SRAIW;
            else
                goto illegal;
            break;
        default:                    /* f3=2/3/4/6/7 保留 */
            goto illegal;
        }
        d->imm = imm_sh(insn);
        return true;
    case 0x3Bu:                     /* op-32：f7=0000000 ADDW/SLLW/SRLW；
                                       f7=0100000 SUBW/SRAW；其余（含 MULW）非法 */
        if (f7 == 0x00u) {
            switch (f3) {
            case 0u: d->op = RV_ADDW; break;
            case 1u: d->op = RV_SLLW; break;
            case 5u: d->op = RV_SRLW; break;
            default: goto illegal;
            }
        } else if (f7 == 0x20u) {
            if (f3 == 0u)
                d->op = RV_SUBW;
            else if (f3 == 5u)
                d->op = RV_SRAW;
            else
                goto illegal;
        } else {
            goto illegal;
        }
        return true;
    case 0x0Fu:                     /* FENCE / FENCE.I：rd=rs1=0，fm=0
                                       （fence.tso 等 fm!=0 不在指令表内） */
        if ((insn & 0xF0000000u) != 0u)
            goto illegal;
        if (d->rd != 0u || d->rs1 != 0u)
            goto illegal;
        if (f3 == 0u)
            d->op = RV_FENCE;       /* pred/succ（imm 低 8 位）单 hart 无语义 */
        else if (f3 == 1u) {
            if ((insn & 0xFFF00000u) != 0u)  /* fence.i imm[11:0] 保留"应为零" */
                goto illegal;
            d->op = RV_FENCEI;
        } else
            goto illegal;
        return true;
    case 0x73u:                     /* SYSTEM：仅 ECALL/EBREAK；f3!=0 为 CSR
                                       （Zicsr 不在范围），imm!=0/1 为 mret/wfi 等 */
        if (f3 != 0u || d->rd != 0u || d->rs1 != 0u)
            goto illegal;
        if ((insn >> 20) == 0u)
            d->op = RV_ECALL;
        else if ((insn >> 20) == 1u)
            d->op = RV_EBREAK;
        else
            goto illegal;
        return true;
    default:                        /* AMO 0x2F、LOAD/STORE-FP 0x07/0x27、
                                       FMADD 0x43、custom 0x0B/0x5B/0x6B/0x7B 等 */
        goto illegal;
    }

illegal:
    d->op = RV_OP_COUNT;
    return false;
}
