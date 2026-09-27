    .text
    .globl _start
_start:                  # sp → argc; a0/a1/a2 不预置
    .option push
    .option norelax      # gp 初始化（Task 11 分发 B 首次端到端修复）：gcc 对
    la   gp, __global_pointer$   # .sbss/.bss 小全局生成 gp 相对寻址（-msmall-data-limit
    .option pop          # 默认 8），gp 不设则访存落低地址 → 202；norelax 防 la 自身被松弛
    ld   a0, 0(sp)       # argc
    addi a1, sp, 8       # argv
    li   a2, 0           # envp = {NULL}
    call main
    li   a7, 93          # exit(ret)
    ecall
    .globl gwrite
gwrite:                  # a0=fd, a1=buf, a2=n
    li   a7, 64
    ecall
    ret
    .globl gexit
gexit:                   # a0=code
    li   a7, 93
    ecall
