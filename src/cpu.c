/* src/cpu.c —— RV64I 用户态模拟器：单步执行核心（Task 6/Task 8/Task 9/Task 10
   + Task 11 分发 B build_stack + Task 12 分发 A 可观测性字段填充）。
   rv_step 流程：pc 4 对齐检查 → mem_read 4 字节取指 → rv_decode →
   switch 执行 → 写回（rd!=0）→ pc+=4 → steps++（跳转类臂自行更新 pc
   并提前返回，见下）。
   提交模型（spec §4.1）：一条指令要么完整生效，要么完全不生效并报故障——
   故障路径在任何写回/pc 推进/计数之前返回，无部分副作用。
   现实现 53 条执行臂全覆盖：立即数算术 9、寄存器算术 10、W 类 9（Task 6）、
   访存 11（Task 8/M3）、控制流 10（Task 9/M4 分发 A）、系统 4（Task 10/M5）。
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
   语义上属控制流组但按普通路径退休（res 写回 + pc+=4，行为等价）。
   系统组语义（Task 10/M5 分发 A，spec §4.2）：FENCE/FENCE.I 单 hart 顺序
   一致 → 无副作用退休（普通提交路径 pc+=4、steps++；FENCE.I 的 imm 保留
   字段校验在 decode）。ECALL 分发 rv_syscall（a7=x17 号码、a0..a5=x10..x15）：
   kind=0 → val 写 a0 并正常退休；kind=1 → guest 终止（置 exited/exit_code，
   不推进 pc/steps——指令未正常退休）；kind=2 → 不支持号码：置
   exited/exit_code=205 并记录号码（sc_unsupported/unsupported_syscall_num），
   诊断输出由 main/trace 层完成，cpu 不依赖 stdio。EBREAK = 停机诊断
   （exited + exit_code=206，不推进 pc/steps）。
   可观测性填充（Task 12 分发 A，M6-a 接口裁决 + 尾部追加申报）：
   - 每步进入先清 has_dec/has_syscall；解码成功即填 last_dec/last_pc（本步
     起始 pc），供 main 每步调 trace_step 生成 `T <seq> pc=… insn=… 名` 行
     （终止型 ECALL/EBREAK 与故障路径不退休，main 门控不产 trace 行）；
   - 各故障点填 fault_addr/fault_insn/fault_align：pc 取指对齐（addr=pc、
     insn 未知=0、align=4）、未映射取指（addr=pc、insn=0）、非法指令
     （addr=pc、insn=指令字）、访存对齐（addr=ea、align=2/4/8）、访存未映射
     （addr=ea）、跳转目标未对齐（addr=target、align=4）——main 据此升级
     §7 诊断（202 addr=、203 addr= align=N、204 word=0x…，台账 M-t）；
   - ECALL 分发前快照号码/参数，分发后记 kind/返回值（has_syscall），供
     main -v 里程碑日志（syscall 名/参/返回、brk/mmap/munmap 变化）。
   build_stack（Task 11/M5 分发 B，spec §3.4）：自 STACK_TOP 向下构造进程
   初始栈（布局与顺序见函数头注释），sp 16 对齐写 x2，a0/a1/a2 保持 0。 */

#include "rvsim.h"
#include <string.h>

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

/* guest 视图小端序列化（与 cpu 取指/syscall 层的 mem 小端整块读写约定一致） */
static void put_le64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* 写一个 u64 到客户内存（小端）；失败返回 -1 供 build_stack 上抛 */
static int wr64(rv_mem *m, uint64_t a, uint64_t v)
{
    uint8_t b[8];
    put_le64(b, v);
    return mem_write(m, a, b, 8);
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
    /* Task 10（M5）追加字段。brk_base/brk_cur 置 0 占位：正式值在 main 装配
       （elf_load 返回 brk 基址后设置并登记 heap 区，分发 B 接线）。 */
    s->brk_base = 0;
    s->brk_cur = 0;
    s->mmap_top = MMAP_BASE;
    s->host_fd[0] = 0;
    s->host_fd[1] = 1;
    s->host_fd[2] = 2;
    s->sc_unsupported = false;
    s->unsupported_syscall_num = 0;
    /* Task 12 分发 A（M6-a）尾部追加字段 */
    s->last_dec.op = RV_OP_COUNT;   /* 解码残留位语义：无效标记 */
    s->last_dec.rd = 0;
    s->last_dec.rs1 = 0;
    s->last_dec.rs2 = 0;
    s->last_dec.raw = 0;
    s->last_dec.imm = 0;
    s->has_dec = false;
    s->last_pc = 0;
    s->fault_addr = 0;
    s->fault_insn = 0;
    s->fault_align = 0;
    s->has_syscall = false;
    s->sc_kind = 0;
    s->sc_num = 0;
    s->sc_args[0] = 0;
    s->sc_args[1] = 0;
    s->sc_args[2] = 0;
    s->sc_args[3] = 0;
    s->sc_args[4] = 0;
    s->sc_args[5] = 0;
    s->sc_ret = 0;
}

/* ---- 进程栈初始化（Task 11/M5 分发 B，spec §3.4 精确布局）----
   自 STACK_TOP 向下排版，顺序钉死（高处→低处）：
     AT_RANDOM 16 字节（固定 0x42*16，保证确定性）
     argv 字符串区（argc 个字符串紧凑连接，各 NUL 结尾；gargs[0]=ELF 路径）
     [16 对齐 padding：addr_str-blk .. sp 之间，guest 无指针指向此处]
   sp 指向指针块低界：
     sp+0   argc (u64)
     sp+8   argv[0..argc-1] 指针 + NULL
     ...    envp：单个 NULL（不传环境变量）
     ...    auxv 3 对（顺序与 spec §3.4 文本一致、钉死）：
              (AT_PAGESZ=6, 4096), (AT_RANDOM=25, 数据区地址), (AT_NULL=0, 0)
   sp 16 对齐：STACK_TOP 页对齐 ⇒ 16 对齐 ⇒ AT_RANDOM/字符串区基址链路确定，
   指针块尺寸为 8 的倍数，sp=(addr_str-blk)&~0xF 向下取整补 padding。
   entry 契约（spec §3.4）：sp(x2)=sp；a0/a1/a2 保持 rvsim_reset 的 0
   （mini-crt 自行从栈取参）；pc=entry 由 main 设置。 */
int build_stack(rvsim *s, int argc, char **gargs)
{
    if (argc <= 0 || !gargs)
        return -1;

    /* 1. 尺寸合计（先算总尺寸再一次排版，不逐段探边） */
    size_t strsz = 0;
    for (int i = 0; i < argc; i++)
        strsz += strlen(gargs[i]) + 1;
    const size_t random_sz = 16;
    const size_t blk = 8                          /* argc */
                     + 8u * (size_t)(argc + 1)    /* argv[] + NULL */
                     + 8                          /* envp 单 NULL */
                     + 2u * 8u * 3u;              /* auxv 3 对（16B/对） */

    /* 2. 排版（地址全部落在已登记 stack 区内：blk+strsz+16 << STACK_SIZE） */
    uint64_t addr_random = STACK_TOP - random_sz;
    uint64_t addr_str = addr_random - strsz;      /* 字符串区低界 */
    uint64_t sp = (addr_str - blk) & ~(uint64_t)0xF;

    /* 3. AT_RANDOM 数据：16 字节固定 0x42（spec §3.4 确定性要求） */
    uint8_t rnd[16];
    memset(rnd, 0x42, sizeof rnd);
    if (mem_write(&s->mem, addr_random, rnd, random_sz) != 0)
        return -1;

    /* 4. argv 字符串区：紧凑连接写入 */
    uint64_t p = addr_str;
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(gargs[i]) + 1;
        if (mem_write(&s->mem, p, gargs[i], len) != 0)
            return -1;
        p += len;
    }

    /* 5. 指针块：argc → argv[]+NULL → envp NULL → auxv（任一写失败即上抛） */
    uint64_t q = sp;
    if (wr64(&s->mem, q, (uint64_t)argc) != 0)    /* sp+0：argc */
        return -1;
    q += 8;
    uint64_t off = 0;
    for (int i = 0; i < argc; i++) {              /* argv[i] 指向字符串区 */
        if (wr64(&s->mem, q, addr_str + off) != 0)
            return -1;
        q += 8;
        off += strlen(gargs[i]) + 1;
    }
    if (wr64(&s->mem, q, 0) != 0)                 /* argv[argc] = NULL */
        return -1;
    q += 8;
    if (wr64(&s->mem, q, 0) != 0)                 /* envp：单 NULL */
        return -1;
    q += 8;
    /* auxv 顺序钉死（AT_PAGESZ → AT_RANDOM → AT_NULL），val 为构造区内
       真实客户地址：AT_PAGESZ=页大小 4096；AT_RANDOM 指向 0x42 数据区。 */
    if (wr64(&s->mem, q, 6) != 0 || wr64(&s->mem, q + 8, PAGE_SIZE) != 0)
        return -1;
    q += 16;
    if (wr64(&s->mem, q, 25) != 0 || wr64(&s->mem, q + 8, addr_random) != 0)
        return -1;
    q += 16;
    if (wr64(&s->mem, q, 0) != 0 || wr64(&s->mem, q + 8, 0) != 0)
        return -1;

    /* 6. entry 契约：sp 写 x2（a0/a1/a2 已由 rvsim_reset 清 0） */
    s->cpu.x[2] = sp;
    return 0;
}

int rv_step(rvsim *s)
{
    s->fault = RV_FV_NONE;
    s->has_dec = false;                 /* M6-a：本步解码/系统调用快照先失效 */
    s->has_syscall = false;

    /* 1. 取指 pc 对齐（RV64I 无 C 扩展，指令恒 4 字节 4 对齐）。
       故障现场：addr=pc、指令字未知（fault_insn=0）、align=4。 */
    if (s->cpu.pc & 3u) {
        s->fault_addr = s->cpu.pc;
        s->fault_insn = 0;
        s->fault_align = 4;
        s->fault = RV_FV_MISALIGNED;
        return s->fault;
    }

    /* 2. 取指（先检查后使用，失败不产生副作用）。故障现场：addr=pc。 */
    uint32_t insn;
    if (mem_read(&s->mem, s->cpu.pc, &insn, 4) != 0) {
        s->fault_addr = s->cpu.pc;
        s->fault_insn = 0;
        s->fault = RV_FV_UNMAPPED;
        return s->fault;
    }

    /* 3. 解码（false = 保留/非法编码）。故障现场：addr=pc、insn=指令字。 */
    rv_dec d;
    if (!rv_decode(insn, &d)) {
        s->fault_addr = s->cpu.pc;
        s->fault_insn = insn;
        s->fault = RV_FV_ILLEGAL;
        return s->fault;
    }
    /* M6-a：解码成功即记录本步现场（供 main 每步调 trace_step 产 trace 行；
       只有 rv_step 返回 0——指令正常退休——时 trace 层才消费）。 */
    s->last_dec = d;
    s->last_pc = s->cpu.pc;
    s->has_dec = true;

    /* 4. 执行：53 条臂全覆盖（Task 6 算术/移位 + Task 8 访存 + Task 9 控制流
       + Task 10 系统）。
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
       store：只写低 w 字节（小端最低字节在低地址）。
       故障现场（M6-a）：addr=ea、insn=指令字、对齐故障 align=访存宽度。 */
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
            s->fault_addr = ea;
            s->fault_insn = insn;
            s->fault_align = w;
            s->fault = RV_FV_MISALIGNED;
            return s->fault;
        }
        uint64_t raw = 0;
        if (mem_read(&s->mem, ea, &raw, w) != 0) {
            s->fault_addr = ea;
            s->fault_insn = insn;
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
            s->fault_addr = ea;
            s->fault_insn = insn;
            s->fault_align = w;
            s->fault = RV_FV_MISALIGNED;
            return s->fault;
        }
        uint64_t val = b;
        if (mem_write(&s->mem, ea, &val, w) != 0) {
            s->fault_addr = ea;
            s->fault_insn = insn;
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
       taken 时目标才需对齐（未 taken 顺序推进 pc+4，无目标概念）。
       目标未对齐故障现场（M6-a）：addr=target、align=4、insn=指令字。 */
    case RV_LUI:   res = (uint64_t)d.imm; break;               /* U 型 imm 已符号扩展 32→64 */
    case RV_AUIPC: res = s->cpu.pc + (uint64_t)d.imm; break;   /* pc 相对，按 2^64 回绕 */
    case RV_JAL: {
        uint64_t target = s->cpu.pc + (uint64_t)d.imm;
        if (target & 3u) {
            s->fault_addr = target;                            /* 先检查：rd 不写、pc 不变 */
            s->fault_insn = insn;
            s->fault_align = 4;
            s->fault = RV_FV_MISALIGNED;
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
            s->fault_addr = target;
            s->fault_insn = insn;
            s->fault_align = 4;
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
                s->fault_addr = target;                        /* 分支本无写回，pc 亦未动 */
                s->fault_insn = insn;
                s->fault_align = 4;
                s->fault = RV_FV_MISALIGNED;
                return s->fault;
            }
            s->cpu.pc = target;
        } else {
            s->cpu.pc += 4;
        }
        s->cpu.steps++;
        return s->fault;
    }
    /* ---- 系统（Task 10/M5 分发 A，spec §4.2）----
       FENCE/FENCE.I：单 hart 顺序一致，无副作用 → 落到底部普通提交路径
       （pc+=4、steps++；FENCE.I 的 imm 保留字段由 decode 判非法）。
       ECALL：分发 rv_syscall（a7=x17，a0..a5=x10..x15）。分发前快照号码/参数、
       分发后记 kind/返回值（has_syscall，M6-a：-v 里程碑日志用）。kind=0 →
       val 直接写 a0=x10（ECALL 编码 rd 恒 0，decode 强制，通用写回路径不适用）
       后经底部普通退休（pc+=4、steps++）；kind=1/2 → guest 终止（exited +
       退出码；kind=2 恒 205 并记录号码供 main/trace 诊断），提前返回不推进
       pc/steps（指令未退休，main 门控不产 trace 行）。
       EBREAK：停机诊断 → exited + 206，提前返回不推进（同上不退休）。 */
    case RV_FENCE:
    case RV_FENCEI:
        break;
    case RV_ECALL: {
        rv_sc_ret r;
        s->sc_num = s->cpu.x[17];
        s->sc_args[0] = s->cpu.x[10];
        s->sc_args[1] = s->cpu.x[11];
        s->sc_args[2] = s->cpu.x[12];
        s->sc_args[3] = s->cpu.x[13];
        s->sc_args[4] = s->cpu.x[14];
        s->sc_args[5] = s->cpu.x[15];
        r = rv_syscall(s, s->cpu.x[17], s->cpu.x[10], s->cpu.x[11],
                       s->cpu.x[12], s->cpu.x[13], s->cpu.x[14],
                       s->cpu.x[15]);
        s->has_syscall = true;
        s->sc_kind = r.kind;
        s->sc_ret = r.val;
        if (r.kind == 1) {
            s->exited = true;
            s->exit_code = (int)r.val;
            return s->fault;                   /* guest 终止：不推进 pc/steps */
        }
        if (r.kind == 2) {
            s->exited = true;
            s->exit_code = RVSIM_EX_SYSCALL;
            s->sc_unsupported = true;
            s->unsupported_syscall_num = (int)s->cpu.x[17];
            return s->fault;                   /* 不支持：同上，诊断由 main/trace 打印 */
        }
        /* kind=0：返回值写 a0（x10）。ECALL 编码 rd 恒 0（decode 强制），
           不能借通用提交路径写回，须显式写 a0；pc/steps 仍走底部普通退休。 */
        s->cpu.x[10] = (uint64_t)r.val;
        break;
    }
    case RV_EBREAK:
        s->exited = true;
        s->exit_code = RVSIM_EX_EBREAK;
        return s->fault;                       /* 不推进 pc、不计数 */
    default:                                   /* RV_OP_COUNT 及越界枚举：不可达防御 */
        s->fault_addr = s->cpu.pc;
        s->fault_insn = insn;
        s->fault = RV_FV_ILLEGAL;
        return s->fault;
    }

    /* 5. 提交：写回（x0 恒 0，写入丢弃）→ pc 推进 → 计数。仅非跳转臂走到
       此处（ALU/访存 + 控制流组的 LUI/AUIPC + 系统组的 FENCE/FENCE.I 与
       返回值型 ECALL）：按 pc+4 退休；JAL/JALR/分支已在臂内更新 pc 并提前
       返回；终止型 ECALL/EBREAK 已置 exited 并提前返回。 */
    if (write_rd && d.rd != 0)
        s->cpu.x[d.rd] = res;
    s->cpu.pc += 4;
    s->cpu.steps++;
    return s->fault;
}
