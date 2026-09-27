    .text
    .globl _start
_start:                  # sp → argc; a0/a1/a2 不预置
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
