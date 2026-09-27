/* src/cpu.c —— RV64I 用户态模拟器：单步执行核心（Task 6/Task 8/Task 9）。
   rv_step 流程：pc 4 对齐检查 → mem_read 4 字节取指 → rv_decode →
   switch 执行 → 写回（rd!=0）→ pc+=4 → steps++（跳转类臂自行更新 pc
   并提前返回，见下）。
   提交模型（spec §4.1）：一条指令要么完整生效，要么完全不生效并报故障——
   故障路径在任何写回/pc 推进/计数之前返回，无部分副作用。
   现实现 49 条执行臂：立即数算术 9、寄存器算术 10、W 类 9（Task 6）、
   访存 11（Task 8/M3）、控制流 10（Task 9/M4 分发 A）；系统组按任务
   边界暂判非法指令（fault=RV_FV_ILLEGAL），由后续任务接入真实语义。
   移位量语义（spec §4.2 以 RISC-V 非特权级规范为准）：立即数移位 shamt 由
   decode 校验（SLLI/SRLI/SRAI 为 6 位且合法，W 类 5 位）；寄存器移位
   SLL/SRL/SRA 取 rs2 低 6 位、W 类取 rs2 低 5 位（设计文档 §4.2 写作 rs1
   系笔误，与规范及 gcc 生成代码不符，见 sdd/task-6-report.md）。
   访存语义（Task 8）：有效地址 ea=rs1+imm 按 2^64 回绕；先对齐检查
   （LH/LHU/SH 2、LW/LWU/SW 4、LD/SD 8，LB/LBU/SB 无要求）再
   mem_read/mem_write（未映射→UNMAPPED）；加载按宽度截断后符号/零扩展写回；
   store 只写低 w 字节且无 rd 写回。mem 以客户小端视图整块读写（与取指
   memcpy 的既有约定一致）。
   控制流语义（Task 9/M4 分发 A，spec §4.2/§3.3）：LUI 的 U 型 imm 已由
   decode 符号扩展 32→64；AUIPC = pc + imm（按 2^64 回绕）。JAL/JALR/
   分支目标须 4 对齐（JALR 先清 bit0 再判），不对齐 → RV_FV_MISALIGNED
   且零副作用（rd 不写、pc 不变、steps 不增）——对齐检查先于任何写回。
   JALR 目标 t=(rs1+imm)&~1 用旧 rs1 计算（rd==rs1 时先求 t 后写回）；
   分支仅在 taken 时检查目标对齐，未 taken 顺序推进 pc+4。跳转类臂
   （JAL/JALR/分支）的写回与 pc 更新由臂内自行完成并提前返回；LUI/AUIPC
   语义上属控制流组但按普通路径退休（res 写回 + pc+=4，行为等价）。 */

#include "rvsim.h"

/* 有符号 64 位比较（portable）：不依赖「无符号→有符号超界转换」的实现定义
   行为（与 decode.c 的规避原则一致）。符号位不同时高位者较小，相同时无符号
   比较即给出有符号序。 */
static int slt64(uint64_t a, uint64_t b)
{
    return ((a ^ b) >> 63) ? (int)(a >> 63) : (a < b);
}

/* 32 位符号扩展（portable：不依赖实现定义行为） */
static uint64_t sext32(uint32_t v)
{
    return (v & 0x80000000u) ? (uint64_t)((int64_t)v - 0x100000000LL) : (uint64_t)v;
}

/* 64 位算术右移（portable）：C99 不保证负数右移为算术移位（与 decode.c 的
   实现定义规避原则一致），改为无符号逻辑移位 + 符号位回填。sh 须 < 64。 */
static uint64_t sra64(uint64_t u, unsigned sh)
{
    if (sh == 0)
        return u;
    if (u >> 63)
        return (u >> sh) | (~(uint64_t)0 << (64 - sh));
    return u >> sh;
}

/* SRAIW/SRAW：先符号扩展到 64 再算术右移。结果自动保持 64 位符号扩展形式
   （对符号扩展值的算术右移不会破坏 63..32 位为 bit31 复制的不变量）。 */
static uint64_t sra_w(uint64_t a, unsigned sh)
{
    return sra64(sext32((uint32_t)a), sh);
}

void rvsim_reset(rvsim *s)
{
    for (int i = 0; i < 32; i++)
        s->cpu.x[i] = 0;
    s->cpu.pc = 0;
    s->cpu.steps = 0;
    s->fault = RV_FV_NONE;
    s->exited = false;
    s->exit_code = 0;
}

int rv_step(rvsim *s)
{
    s->fault = RV_FV_NONE;

    /* 1. 取指 pc 对齐（RV64I 无 C 扩展，指令恒 4 字节 4 对齐） */
    if (s->cpu.pc & 3u) {
        s->fault = RV_FV_MISALIGNED;
        return s->fault;
    }

    /* 2. 取指（先检查后使用，失败不产生副作用） */
    uint32_t insn;
    if (mem_read(&s->mem, s->cpu.pc, &insn, 4) != 0) {
        s->fault = RV_FV_UNMAPPED;
        return s->fault;
    }

    /* 3. 解码（false = 保留/非法编码） */
    rv_dec d;
    if (!rv_decode(insn, &d)) {
        s->fault = RV_FV_ILLEGAL;
        return s->fault;
    }

    /* 4. 执行：49 条臂（Task 6 算术/移位 + Task 8 访存 + Task 9 控制流）；
       default 覆盖未实现组（系统）。
       ADD/SUB 及移位等按 2^64 回绕，由无符号算术自然实现；
       SLT/SLTU 结果恒 0/1；移位量越界风险由 decode（立即数）或掩码（寄存器）排除。 */
    uint64_t a = s->cpu.x[d.rs1];
    uint64_t b = s->cpu.x[d.rs2];
    uint64_t res = 0;
    bool write_rd = true;       /* store 无 rd 写回（S 型 rd 位段是 imm[4:0] 残留） */

    switch (d.op) {
    /* ---- 立即数算术（imm 已按 I 型符号扩展） ---- */
    case RV_ADDI:  res = a + (uint64_t)d.imm; break;
    case RV_SLTI:  res = (uint64_t)slt64(a, (uint64_t)d.imm); break;
    case RV_SLTIU: res = (uint64_t)(a < (uint64_t)d.imm); break; /* 立即数符号扩展后按 u64 比 */
    case RV_XORI:  res = a ^ (uint64_t)d.imm; break;
    case RV_ORI:   res = a | (uint64_t)d.imm; break;
    case RV_ANDI:  res = a & (uint64_t)d.imm; break;
    case RV_SLLI:  res = a << (uint64_t)d.imm; break;            /* decode 保证 imm∈[0,63] */
    case RV_SRLI:  res = a >> (uint64_t)d.imm; break;            /* 无符号：逻辑右移 */
    case RV_SRAI:  res = sra64(a, (unsigned)d.imm); break;
    /* ---- 寄存器算术（移位量取 rs2 低 6 位） ---- */
    case RV_ADD:   res = a + b; break;
    case RV_SUB:   res = a - b; break;
    case RV_SLL:   res = a << (b & 63u); break;
    case RV_SLT:   res = (uint64_t)slt64(a, b); break;
    case RV_SLTU:  res = (uint64_t)(a < b); break;
    case RV_XOR:   res = a ^ b; break;
    case RV_SRL:   res = a >> (b & 63u); break;
    case RV_SRA:   res = sra64(a, (unsigned)(b & 63u)); break;
    case RV_OR:    res = a | b; break;
    case RV_AND:   res = a & b; break;
    /* ---- W 类：32 位运算后符号扩展到 64 ---- */
    case RV_ADDIW: res = sext32((uint32_t)(a + (uint64_t)d.imm)); break;
    case RV_SLLIW: res = sext32((uint32_t)(a << (uint64_t)d.imm)); break; /* decode 保证 imm∈[0,31] */
    case RV_SRLIW: res = sext32((uint32_t)((uint32_t)a >> (uint64_t)d.imm)); break;
    case RV_SRAIW: res = sra_w(a, (unsigned)d.imm); break;
    case RV_ADDW:  res = sext32((uint32_t)(a + b)); break;
    case RV_SUBW:  res = sext32((uint32_t)(a - b)); break;
    case RV_SLLW:  res = sext32((uint32_t)(a << (b & 31u))); break;
    case RV_SRLW:  res = sext32((uint32_t)((uint32_t)a >> (b & 31u))); break;
    case RV_SRAW:  res = sra_w(a, (unsigned)(b & 31u)); break;
    /* ---- 访存（Task 8/M3）：ea = rs1+imm 按 2^64 回绕（无符号加法自然实现）。
       顺序：对齐检查（不对齐→MISALIGNED）→ mem_read/mem_write（未映射→
       UNMAPPED）→ 截断/扩展写回。故障路径在任何访存生效/写回/pc/steps 之前
       返回，无部分副作用（spec §4.1 提交模型；加载本身即写回前最后一步）。
       加载截断：mem_read 只填低 w 字节（其余保持 0），按操作符号/零扩展；
       store：只写低 w 字节（小端最低字节在低地址）。 */
    case RV_LB:
    case RV_LBU:
    case RV_LH:
    case RV_LHU:
    case RV_LW:
    case RV_LWU:
    case RV_LD: {
        unsigned w = (d.op == RV_LD) ? 8u
                   : (d.op == RV_LW || d.op == RV_LWU) ? 4u
                   : (d.op == RV_LH || d.op == RV_LHU) ? 2u : 1u;
        uint64_t ea = a + (uint64_t)d.imm;
        if (ea & (w - 1u)) {
            s->fault = RV_FV_MISALIGNED;
            return s->fault;
        }
        uint64_t raw = 0;
        if (mem_read(&s->mem, ea, &raw, w) != 0) {
            s->fault = RV_FV_UNMAPPED;
            return s->fault;
        }
        switch (d.op) {
        case RV_LB:  res = (raw & 0x80u) ? raw | ~(uint64_t)0xFFu : raw; break;
        case RV_LBU: res = raw; break;
        case RV_LH:  res = (raw & 0x8000u) ? raw | ~(uint64_t)0xFFFFu : raw; break;
        case RV_LHU: res = raw; break;
        case RV_LW:  res = sext32((uint32_t)raw); break;
        case RV_LWU: res = raw; break;
        default:     res = raw; break;   /* RV_LD：全宽直取 */
        }
        break;
    }
    case RV_SB:
    case RV_SH:
    case RV_SW:
    case RV_SD: {
        unsigned w = (d.op == RV_SD) ? 8u
                   : (d.op == RV_SW) ? 4u
                   : (d.op == RV_SH) ? 2u : 1u;
        uint64_t ea = a + (uint64_t)d.imm;
        if (ea & (w - 1u)) {
            s->fault = RV_FV_MISALIGNED;
            return s->fault;
        }
        uint64_t val = b;
        if (mem_write(&s->mem, ea, &val, w) != 0) {
            s->fault = RV_FV_UNMAPPED;
            return s->fault;
        }
        write_rd = false;
        break;
    }
    /* ---- 控制流（Task 9/M4 分发 A）----
       LUI/AUIPC 按普通路径退休（res 写回 + pc+=4）。JAL/JALR/分支的写回
       与 pc 更新由臂内自行完成并提前返回；目标对齐检查先于任何写回/pc
       更新（故障零副作用契约，spec §4.1/§3.3）。JALR 目标先清 bit0 再判
       4 对齐，且用旧 rs1 计算（rd==rs1 时先求 t 后写回 pc+4）；分支仅在
       taken 时目标才需对齐（未 taken 顺序推进 pc+4，无目标概念）。 */
    case RV_LUI:   res = (uint64_t)d.imm; break;               /* U 型 imm 已符号扩展 32→64 */
    case RV_AUIPC: res = s->cpu.pc + (uint64_t)d.imm; break;   /* pc 相对，按 2^64 回绕 */
    case RV_JAL: {
        uint64_t target = s->cpu.pc + (uint64_t)d.imm;
        if (target & 3u) {
            s->fault = RV_FV_MISALIGNED;                       /* 先检查：rd 不写、pc 不变 */
            return s->fault;
        }
        if (d.rd != 0)
            s->cpu.x[d.rd] = s->cpu.pc + 4;
        s->cpu.pc = target;
        s->cpu.steps++;
        return s->fault;
    }
    case RV_JALR: {
        uint64_t target = (a + (uint64_t)d.imm) & ~(uint64_t)1; /* 清 bit0；a 为旧 rs1 */
        if (target & 3u) {
            s->fault = RV_FV_MISALIGNED;
            return s->fault;
        }
        if (d.rd != 0)
            s->cpu.x[d.rd] = s->cpu.pc + 4;
        s->cpu.pc = target;
        s->cpu.steps++;
        return s->fault;
    }
    case RV_BEQ:
    case RV_BNE:
    case RV_BLT:
    case RV_BGE:
    case RV_BLTU:
    case RV_BGEU: {
        bool taken;
        if (d.op == RV_BEQ)
            taken = (a == b);
        else if (d.op == RV_BNE)
            taken = (a != b);
        else if (d.op == RV_BLT)
            taken = slt64(a, b) != 0;                          /* 有符号 */
        else if (d.op == RV_BGE)
            taken = slt64(a, b) == 0;
        else if (d.op == RV_BLTU)
            taken = (a < b);                                   /* 无符号 */
        else
            taken = !(a < b);                                  /* RV_BGEU */
        if (taken) {
            uint64_t target = s->cpu.pc + (uint64_t)d.imm;
            if (target & 3u) {
                s->fault = RV_FV_MISALIGNED;                   /* 分支本无写回，pc 亦未动 */
                return s->fault;
            }
            s->cpu.pc = target;
        } else {
            s->cpu.pc += 4;
        }
        s->cpu.steps++;
        return s->fault;
    }
    /* ---- 未实现组（系统：FENCE/FENCE.I/ECALL/EBREAK，后续任务接入）：
       此刻按非法指令处理，无副作用 ---- */
    default:
        s->fault = RV_FV_ILLEGAL;
        return s->fault;
    }

    /* 5. 提交：写回（x0 恒 0，写入丢弃）→ pc 推进 → 计数。仅非跳转臂走到
       此处（ALU/访存 + 控制流组的 LUI/AUIPC）：按 pc+4 退休；JAL/JALR/
       分支已在臂内更新 pc 并提前返回。 */
    if (write_rd && d.rd != 0)
        s->cpu.x[d.rd] = res;
    s->cpu.pc += 4;
    s->cpu.steps++;
    return s->fault;
}
