/* tests/test_trace.c —— Task 12 分发 A（M6-a）：可观测性 L0 直测。
   覆盖（期望值全部手算，golden 字符串内置）：
   1. trace 行格式（spec §6.1）：`T <seq> pc=0x<16hex> insn=0x<8hex> <助记名>[ rd=xN=0x<16hex>]`
      —— 5 条指令（LUI/ADDI/ADDI/ADD/SW）逐字符比对；SW（store）不打印 rd；
      JAL 打印 rd=pc+4；助记名 = rv_op 枚举名（rv_op_mnem）。
      注意 fmemopen 须用 "w+"（写后要读回比对，"w" 只写流 fread 失败）。
   2. 故障现场尾部字段：UNMAPPED/MISALIGNED/ILLEGAL 各点 fault_addr/fault_insn/
      fault_align 填充（pc 取指对齐故障 fault_insn=0、fault_align=4；访存对齐
      fault_align=访存宽度）；has_dec 语义（rv_step 进入清 false，解码成功置 true）。
   3. ECALL 快照尾部字段（has_syscall/sc_kind/sc_num/sc_args/sc_ret）。
   4. --dump-regs 格式：`xN  = 0x<16hex>` 33 行（x0..x31 + pc）全字面 golden。
   5. --dump-mem 格式：地址行 + 两个 8 字节分组 + |ASCII|（16 字节整行 + 5 字节
      截断行，不可打印字节 → '.'）。
   6. trace 宿主 IO 契约：打开失败返回 -1（main 映射 208，不吞）。
   输出经 fmemopen 捕获后与 golden 全等比对。 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "enc.h"

/* 把 FILE*（fmemopen "w+"）内容与 golden 全等比对 */
static int buf_is(FILE *f, const char *golden)
{
    char big[8192];
    long n;
    fflush(f);
    n = ftell(f);
    if (n < 0 || (size_t)n >= sizeof big)
        return 0;
    rewind(f);
    if (fread(big, 1, (size_t)n, f) != (size_t)n)
        return 0;
    big[n] = '\0';
    return strcmp(big, golden) == 0;
}

/* 5 条指令：LUI x2,0x10; ADDI x2,x2,0x200; ADDI x1,x0,5; ADD x3,x1,x2;
   SW x1,0(x2)（写 data 区 [0x10200,0x10210)——store 目标必须已映射） */
static const uint32_t prog5[5] = {
    ENC_U(0x10, 2, 0x37),               /* 0x00010137 LUI  x2,0x10   */
    ENC_I(0x200, 2, 0, 2, 0x13),        /* 0x20010113 ADDI x2,x2,0x200 */
    ENC_I(5, 0, 0, 1, 0x13),            /* 0x00500093 ADDI x1,x0,5   */
    OP_STD(0x00, 2, 1, 0, 3, 0x33),     /* 0x002081b3 ADD  x3,x1,x2  */
    ENC_S(0, 1, 2, 2, 0x23),            /* 0x00112023 SW   x1,0(x2) */
};

static void test_trace_lines(void)
{
    static const char *const golden =
        "T 1 pc=0x0000000000010000 insn=0x00010137 RV_LUI rd=x2=0x0000000000010000\n"
        "T 2 pc=0x0000000000010004 insn=0x20010113 RV_ADDI rd=x2=0x0000000000010200\n"
        "T 3 pc=0x0000000000010008 insn=0x00500093 RV_ADDI rd=x1=0x0000000000000005\n"
        "T 4 pc=0x000000000001000c insn=0x002081b3 RV_ADD rd=x3=0x0000000000010205\n"
        "T 5 pc=0x0000000000010010 insn=0x00112023 RV_SW\n";
    char buf[2048];
    FILE *f = fmemopen(buf, sizeof buf, "w+");
    rv_trace tr;
    int i;
    uint8_t b = 0;
    CHECK(f != NULL);
    reset_sim();
    mem_add_region(&hs.mem, MEM_BASE + 16, MEM_BASE + 32, 0);  /* 第 5 条指令槽 */
    mem_add_region(&hs.mem, MEM_BASE + 0x200, MEM_BASE + 0x210, 0); /* SW data 区 */
    for (i = 0; i < 5; i++)
        CHECK(mem_write(&hs.mem, MEM_BASE + 4u * (unsigned)i, &prog5[i], 4) == 0);
    trace_attach(&tr, f);
    for (i = 0; i < 5; i++) {
        CHECK(rv_step(&hs) == 0);
        trace_step(&tr, &hs);
    }
    CHECK(trace_close(&tr) == 0);
    CHECK(buf_is(f, golden));
    /* SW 副作用：mem[0x10200] = x1 = 5（小端单字节） */
    CHECK(mem_read(&hs.mem, MEM_BASE + 0x200, &b, 1) == 0 && b == 5);
    fclose(f);
}

static void test_trace_jal_rd(void)
{
    static const char *const golden =
        "T 1 pc=0x0000000000010000 insn=0x008000ef RV_JAL rd=x1=0x0000000000010004\n";
    char buf[512];
    FILE *f = fmemopen(buf, sizeof buf, "w+");
    rv_trace tr;
    uint32_t insn = ENC_J(8, 1, 0x6f);      /* JAL x1,+8 = 0x008000ef */
    CHECK(f != NULL);
    reset_sim();
    CHECK(mem_write(&hs.mem, MEM_BASE, &insn, 4) == 0);
    trace_attach(&tr, f);
    CHECK(rv_step(&hs) == 0);
    trace_step(&tr, &hs);
    CHECK(trace_close(&tr) == 0);
    CHECK(buf_is(f, golden));
    CHECK(hs.cpu.pc == MEM_BASE + 8);
    fclose(f);
}

static void test_op_mnem(void)
{
    CHECK(strcmp(rv_op_mnem(RV_LUI), "RV_LUI") == 0);
    CHECK(strcmp(rv_op_mnem(RV_ADDI), "RV_ADDI") == 0);
    CHECK(strcmp(rv_op_mnem(RV_SD), "RV_SD") == 0);
    CHECK(strcmp(rv_op_mnem(RV_ECALL), "RV_ECALL") == 0);
    CHECK(strcmp(rv_op_mnem(RV_EBREAK), "RV_EBREAK") == 0);
    CHECK(strcmp(rv_op_mnem((rv_op)999), "RV_?") == 0);   /* 越界枚举防御 */
}

/* 故障现场尾部字段（取指 pc 未对齐：insn 未知 → fault_insn=0，align=4） */
static void test_fault_pc_misaligned(void)
{
    reset_sim();
    hs.cpu.pc = MEM_BASE + 2;
    CHECK(rv_step(&hs) == RV_FV_MISALIGNED);
    CHECK(hs.has_dec == false);
    CHECK(hs.fault_addr == MEM_BASE + 2);
    CHECK(hs.fault_insn == 0);
    CHECK(hs.fault_align == 4);
}

/* 未映射取指：addr=pc，insn 未知 → 0 */
static void test_fault_fetch_unmapped(void)
{
    reset_sim();
    hs.cpu.pc = 0x0000000000900000ULL;
    CHECK(rv_step(&hs) == RV_FV_UNMAPPED);
    CHECK(hs.fault_addr == 0x0000000000900000ULL);
    CHECK(hs.fault_insn == 0);
}

/* 非法指令：fault_insn=指令字本身，fault_addr=pc */
static void test_fault_illegal(void)
{
    uint32_t insn = 0xFFFFFFFFu;
    reset_sim();
    CHECK(mem_write(&hs.mem, MEM_BASE, &insn, 4) == 0);
    CHECK(rv_step(&hs) == RV_FV_ILLEGAL);
    CHECK(hs.fault_addr == MEM_BASE);
    CHECK(hs.fault_insn == 0xFFFFFFFFu);
}

/* 访存故障：ea 未映射 → fault_addr=ea、fault_insn=指令字；对齐故障 fault_align=宽度 */
static void test_fault_load(void)
{
    uint32_t ld = ENC_I(0, 10, 3, 1, 0x03);     /* LD x1,0(x10) = 0x00053083 */
    reset_sim();
    CHECK(mem_write(&hs.mem, MEM_BASE, &ld, 4) == 0);
    hs.cpu.x[10] = 0x0000000000300000ULL;       /* 窗口内未登记地址 → UNMAPPED */
    CHECK(rv_step(&hs) == RV_FV_UNMAPPED);
    CHECK(hs.fault_addr == 0x0000000000300000ULL);
    CHECK(hs.fault_insn == 0x00053083u);

    reset_sim();
    CHECK(mem_write(&hs.mem, MEM_BASE, &ld, 4) == 0);
    hs.cpu.x[10] = MEM_BASE + 2;                /* 代码区内但 8 对齐失败（先查对齐） */
    CHECK(rv_step(&hs) == RV_FV_MISALIGNED);
    CHECK(hs.fault_addr == MEM_BASE + 2);
    CHECK(hs.fault_align == 8);
    CHECK(hs.fault_insn == 0x00053083u);
}

/* has_dec 语义：解码成功即置 true（last_pc=起始 pc）；进入 rv_step 先清 false */
static void test_has_dec(void)
{
    uint32_t insn = ENC_I(5, 0, 0, 1, 0x13);
    reset_sim();
    CHECK(hs.has_dec == false);
    CHECK(mem_write(&hs.mem, MEM_BASE, &insn, 4) == 0);
    CHECK(rv_step(&hs) == 0);
    CHECK(hs.has_dec == true);
    CHECK(hs.last_dec.op == RV_ADDI);
    CHECK(hs.last_dec.rd == 1);
    CHECK(hs.last_dec.imm == 5);
    CHECK(hs.last_pc == MEM_BASE);
}

/* ECALL 快照尾部字段（uname：kind=0，返回 0） */
static void test_syscall_capture(void)
{
    uint32_t ecall = 0x00000073u;
    reset_sim();
    mem_add_region(&hs.mem, MEM_BASE + 16, MEM_BASE + 16 + 512, 0);
    CHECK(mem_write(&hs.mem, MEM_BASE, &ecall, 4) == 0);
    hs.cpu.x[17] = 160;                         /* uname */
    hs.cpu.x[10] = MEM_BASE + 16;               /* buf */
    CHECK(rv_step(&hs) == 0);
    CHECK(hs.has_syscall == true);
    CHECK(hs.sc_kind == 0);
    CHECK(hs.sc_num == 160);
    CHECK(hs.sc_args[0] == MEM_BASE + 16);
    CHECK(hs.sc_ret == 0);
    /* 终止型：exit(42) → kind=1，val=42（不退休；trace_step 不产行） */
    {
        char tbuf[256];
        FILE *tf = fmemopen(tbuf, sizeof tbuf, "w+");
        rv_trace tr;
        CHECK(tf != NULL);
        memset(tbuf, 0, sizeof tbuf);
        reset_sim();
        CHECK(mem_write(&hs.mem, MEM_BASE, &ecall, 4) == 0);
        hs.cpu.x[17] = 93;
        hs.cpu.x[10] = 42;
        trace_attach(&tr, tf);
        CHECK(rv_step(&hs) == 0);               /* 无故障返回，但未退休 */
        trace_step(&tr, &hs);
        CHECK(trace_close(&tr) == 0);
        CHECK(hs.exited == true && hs.exit_code == 42);
        CHECK(hs.has_syscall == true);
        CHECK(hs.sc_kind == 1);
        CHECK(hs.sc_ret == 42);
        fflush(tf);
        CHECK(ftell(tf) == 0);                  /* 终止型无 trace 行 */
        fclose(tf);
    }
}

static void test_dump_regs(void)
{
    static const char *const golden =
        "x0  = 0x0000000000000000\n"
        "x1  = 0x0000000000000001\n"
        "x2  = 0xdeadbeefcafebabe\n"
        "x3  = 0x0000000000000000\n"
        "x4  = 0x0000000000000000\n"
        "x5  = 0x0000000000000000\n"
        "x6  = 0x0000000000000000\n"
        "x7  = 0x0000000000000000\n"
        "x8  = 0x0000000000000000\n"
        "x9  = 0x0000000000000000\n"
        "x10 = 0x0000000000000000\n"
        "x11 = 0x0000000000000000\n"
        "x12 = 0x0000000000000000\n"
        "x13 = 0x0000000000000000\n"
        "x14 = 0x0000000000000000\n"
        "x15 = 0x0000000000000000\n"
        "x16 = 0x0000000000000000\n"
        "x17 = 0x0000000000000000\n"
        "x18 = 0x0000000000000000\n"
        "x19 = 0x0000000000000000\n"
        "x20 = 0x0000000000000000\n"
        "x21 = 0x0000000000000000\n"
        "x22 = 0x0000000000000000\n"
        "x23 = 0x0000000000000000\n"
        "x24 = 0x0000000000000000\n"
        "x25 = 0x0000000000000000\n"
        "x26 = 0x0000000000000000\n"
        "x27 = 0x0000000000000000\n"
        "x28 = 0x0000000000000000\n"
        "x29 = 0x0000000000000000\n"
        "x30 = 0x0000000000000000\n"
        "x31 = 0x0000000000001234\n"
        "pc  = 0x0000000000010000\n";
    char buf[4096];
    FILE *f = fmemopen(buf, sizeof buf, "w+");
    CHECK(f != NULL);
    reset_sim();
    hs.cpu.x[1] = 1;
    hs.cpu.x[2] = 0xDEADBEEFCAFEBABEULL;
    hs.cpu.x[31] = 0x1234;
    rv_dump_regs(f, &hs.cpu);
    CHECK(buf_is(f, golden));
    fclose(f);
}

/* 16 字节：两个 8 字节分组 + ASCII；不可打印（<0x20 / >=0x7f）→ '.' */
static void test_dump_mem_full(void)
{
    static const char *const golden =
        "0000000000010000  4865007f0120feff  4142007f00010203  |He... ..AB......|\n";
    static const uint8_t data[16] = {
        0x48, 0x65, 0x00, 0x7f, 0x01, 0x20, 0xfe, 0xff,
        0x41, 0x42, 0x00, 0x7f, 0x00, 0x01, 0x02, 0x03
    };
    char buf[512];
    FILE *f = fmemopen(buf, sizeof buf, "w+");
    CHECK(f != NULL);
    reset_sim();
    CHECK(mem_write(&hs.mem, MEM_BASE, data, 16) == 0);
    CHECK(rv_dump_mem(f, &hs.mem, MEM_BASE, 16) == 0);
    CHECK(buf_is(f, golden));
    fclose(f);
}

/* 5 字节截断行：hex 组内空格补位、第二组全空格、ASCII 只含实际字节 */
static void test_dump_mem_partial(void)
{
    static const char *const golden =
        "0000000000010008  4142007f00                          |AB...|\n";
    static const uint8_t data[16] = {
        0x48, 0x65, 0x00, 0x7f, 0x01, 0x20, 0xfe, 0xff,
        0x41, 0x42, 0x00, 0x7f, 0x00, 0x01, 0x02, 0x03
    };
    char buf[512];
    FILE *f = fmemopen(buf, sizeof buf, "w+");
    CHECK(f != NULL);
    reset_sim();
    CHECK(mem_write(&hs.mem, MEM_BASE, data, 16) == 0);
    CHECK(rv_dump_mem(f, &hs.mem, MEM_BASE + 8, 5) == 0);
    CHECK(buf_is(f, golden));
    fclose(f);
}

/* 未映射 dump → -1（main 映射 208，不吞） */
static void test_dump_mem_unmapped(void)
{
    reset_sim();
    CHECK(rv_dump_mem(stdout, &hs.mem, 0x0000000000F00000ULL, 16) == -1);
}

/* trace 打开失败 → -1（不吞） */
static void test_trace_open_fail(void)
{
    rv_trace tr;
    CHECK(trace_open(&tr, "/nonexistent-rvsim-dir-zz/trace.out") == -1);
}

int main(void)
{
    test_op_mnem();
    test_trace_lines();
    test_trace_jal_rd();
    test_has_dec();
    test_fault_pc_misaligned();
    test_fault_fetch_unmapped();
    test_fault_illegal();
    test_fault_load();
    test_syscall_capture();
    test_dump_regs();
    test_dump_mem_full();
    test_dump_mem_partial();
    test_dump_mem_unmapped();
    test_trace_open_fail();
    TEST_DONE();
}
