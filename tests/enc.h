/* tests/enc.h —— 指令编码辅助宏（仅测试用）。
   取自 task-5-brief 原文，做三处必要修正（详见 sdd/task-5-report.md）：
   1. 所有掩码加 u 后缀/无符号转换：负立即数按补码取位段，且避免
      gcc -Wshift-overflow 对「正数移入符号位」的警告；
   2. ENC_B 的 imm[11] 项：简报写作 (imm&0x800)<<4（落到 rs1 字段，破坏编码），
      按 B 型编码 insn[7]=imm[11] 应为 (imm&0x800)>>4；
   3. ENC_J 的 imm[19:12] 项：简报写作 (imm&0xFFE)<<20（与 imm[10:1] 项
      (imm&0x7FE)<<20 完全重叠），按 J 型编码应为恒等映射 (imm&0xFF000)。 */
#ifndef RVSIM_TEST_ENC_H
#define RVSIM_TEST_ENC_H

#define OP_STD(f7,rs2,rs1,f3,rd,op) ((((uint32_t)(f7))<<25)|(((uint32_t)(rs2))<<20)|(((uint32_t)(rs1))<<15)|(((uint32_t)(f3))<<12)|(((uint32_t)(rd))<<7)|((uint32_t)(op)))
#define ENC_I(imm,rs1,f3,rd,op)  ((((uint32_t)(imm))&0xFFFu)<<20 | ((uint32_t)(rs1))<<15 | ((uint32_t)(f3))<<12 | ((uint32_t)(rd))<<7 | (uint32_t)(op))
#define ENC_S(imm,rs2,rs1,f3,op) ((((uint32_t)(imm))&0xFE0u)<<20 | ((uint32_t)(rs2))<<20 | (((uint32_t)(imm))&0x1Fu)<<7 | ((uint32_t)(rs1))<<15 | ((uint32_t)(f3))<<12 | (uint32_t)(op))
#define ENC_B(imm,rs2,rs1,f3,op) (((((uint32_t)(imm))&0x1000u)<<19) | ((((uint32_t)(imm))&0x7E0u)<<20) | ((uint32_t)(rs2))<<20 | ((uint32_t)(rs1))<<15 | ((uint32_t)(f3))<<12 | ((((uint32_t)(imm))&0x1Eu)<<7) | ((((uint32_t)(imm))&0x800u)>>4) | (uint32_t)(op))
#define ENC_U(imm,rd,op)         ((((uint32_t)(imm))&0xFFFFFu)<<12 | ((uint32_t)(rd))<<7 | (uint32_t)(op))
#define ENC_J(imm,rd,op)         (((((uint32_t)(imm))&0x100000u)<<11) | (((uint32_t)(imm))&0xFF000u) | ((((uint32_t)(imm))&0x800u)<<9) | ((((uint32_t)(imm))&0x7FEu)<<20) | ((uint32_t)(rd))<<7 | (uint32_t)(op))
#define OPC(op) ((uint32_t)(op))

#endif
