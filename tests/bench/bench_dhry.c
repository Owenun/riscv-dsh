/* bench_dhry.c —— bench 分发 A：Dhrystone 2.1 改造版（单文件自包含）。
 *
 * 算法与语句分布忠实于 Weicker 的公有领域 C 版本 2.1（dhry.h/dhry_1.c/
 * dhry_2.c，SIGPLAN Notices 23,8 1988；main 主循环逐语句对应），改造点：
 *   - 单文件：原 dhry_1.c/dhry_2.c 合并，全局变量改 static；
 *   - libc 替换：malloc → 静态 Rec_Pool[2]（指针终态因此可断言）；
 *     strcpy/strcmp → mini-crt xstrcpy/xstrcmp；printf/scanner/timing 全删；
 *   - Number_Of_Runs 取 volatile 常量 BENCH_ITERS（防主循环被编译期折叠，
 *     亦为分发 B 校准留口）。
 *
 * 结果校验：原版输出块 "Final values ... should be" 的终态全部改为断言：
 * Int_Glob==5、Bool_Glob==1、Ch_1/Ch_2=='A'/'B'、Arr_1_Glob[8]==7、
 * Arr_2_Glob[8][7]==BENCH_ITERS+10、Ptr_Glob/Next_Ptr_Glob 终态
 * （Discr/Enum_Comp/Int_Comp/Str_Comp/Ptr_Comp 指向关系）、main 局部
 * Int_1/2/3、Enum_Loc、Str_1/Str_2。任一不符 → FAIL + gexit(行号)。
 *
 * 纯 RV64I：int/int 除法与 int 乘法由 gcc 落到 __divsi3/__mulsi3（softops
 * 软实现）；输出确定性（相同输入 → 逐字节相同 stdout），供宿主 native
 * shim 交叉验证。
 */
#include "guest.h"

#define BENCH_ITERS 100000

typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;
typedef int  One_Thirty;
typedef int  One_Fifty;
typedef char Capital_Letter;
typedef int  Boolean;
typedef char Str_30[31];
typedef int  Arr_1_Dim[50];
typedef int  Arr_2_Dim[50][50];

typedef struct record {
    struct record *Ptr_Comp;
    Enumeration    Discr;
    union {
        struct {
            Enumeration Enum_Comp;
            int         Int_Comp;
            char        Str_Comp[31];
        } var_1;
        struct {
            Enumeration E_Comp_2;
            char        Str_2_Comp[31];
        } var_2;
        struct {
            char Ch_1_Comp;
            char Ch_2_Comp;
        } var_3;
    } variant;
} Rec_Type, *Rec_Pointer;

#define structassign(d, s) (d) = (s)
#define Null 0
#define true  1
#define false 0

/* Global Variables（原 dhry_1.c，改 static） */
static Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
static int      Int_Glob;
static Boolean  Bool_Glob;
static char     Ch_1_Glob, Ch_2_Glob;
static int      Arr_1_Glob[50];
static int      Arr_2_Glob[50][50];

/* malloc 替代：两个静态记录块（native/rvsim 两侧布局同为静态地址） */
static Rec_Type Rec_Pool[2];

/* 十进制打印（guest 端经 softops __udivdi3/__umoddi3；native 端硬件除） */
static void putdec(unsigned long v)
{
    char b[20];
    int i = 20;
    do { b[--i] = (char)('0' + (v % 10)); v /= 10; } while (v);
    gwrite(1, b + i, (unsigned long)(20 - i));
}

static Enumeration Func_3(Enumeration Enum_Par_Val)
{
    Enumeration Enum_Loc;
    Enum_Loc = Enum_Par_Val;
    if (Enum_Loc == Ident_3)
        return (true);
    else
        return (false);
}

static void Proc_7(One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val,
                   One_Fifty *Int_Par_Ref)
{
    One_Fifty Int_Loc;
    Int_Loc = Int_1_Par_Val + 2;
    *Int_Par_Ref = Int_2_Par_Val + Int_Loc;
}

static void Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par)
{
    *Enum_Ref_Par = Enum_Val_Par;
    if (!Func_3(Enum_Val_Par))
        *Enum_Ref_Par = Ident_4;
    switch (Enum_Val_Par) {
    case Ident_1:
        *Enum_Ref_Par = Ident_1;
        break;
    case Ident_2:
        if (Int_Glob > 100)
            *Enum_Ref_Par = Ident_1;
        else
            *Enum_Ref_Par = Ident_4;
        break;
    case Ident_3:
        *Enum_Ref_Par = Ident_2;
        break;
    case Ident_4:
        break;
    case Ident_5:
        *Enum_Ref_Par = Ident_3;
        break;
    }
}

static Enumeration Func_1(Capital_Letter Ch_1_Par_Val, Capital_Letter Ch_2_Par_Val)
{
    Capital_Letter Ch_1_Loc, Ch_2_Loc;
    Ch_1_Loc = Ch_1_Par_Val;
    Ch_2_Loc = Ch_1_Loc;
    if (Ch_2_Loc != Ch_2_Par_Val)
        return (Ident_1);
    else {
        Ch_1_Glob = Ch_1_Loc;
        return (Ident_2);
    }
}

static Boolean Func_2(Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref)
{
    One_Thirty     Int_Loc;
    Capital_Letter Ch_Loc;
    Int_Loc = 2;
    while (Int_Loc <= 2)
        if (Func_1(Str_1_Par_Ref[Int_Loc], Str_2_Par_Ref[Int_Loc + 1]) == Ident_1) {
            Ch_Loc = 'A';
            Int_Loc += 1;
        }
    if (Ch_Loc >= 'W' && Ch_Loc < 'Z')
        Int_Loc = 7;
    if (Ch_Loc == 'R')
        return (true);
    else {
        if (xstrcmp(Str_1_Par_Ref, Str_2_Par_Ref) > 0) {
            Int_Loc += 7;
            Int_Glob = Int_Loc;
            return (true);
        } else
            return (false);
    }
}

static void Proc_8(Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref,
                   int Int_1_Par_Val, int Int_2_Par_Val)
{
    One_Fifty Int_Index;
    One_Fifty Int_Loc;
    Int_Loc = Int_1_Par_Val + 5;
    Arr_1_Par_Ref[Int_Loc] = Int_2_Par_Val;
    Arr_1_Par_Ref[Int_Loc + 1] = Arr_1_Par_Ref[Int_Loc];
    Arr_1_Par_Ref[Int_Loc + 30] = Int_Loc;
    for (Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; ++Int_Index)
        Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
    Arr_2_Par_Ref[Int_Loc][Int_Loc - 1] += 1;
    Arr_2_Par_Ref[Int_Loc + 20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
    Int_Glob = 5;
}

static void Proc_3(Rec_Pointer *Ptr_Ref_Par)
{
    if (Ptr_Glob != Null)
        *Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
    Proc_7(10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
}

static void Proc_2(One_Fifty *Int_Par_Ref)
{
    One_Fifty  Int_Loc;
    Enumeration Enum_Loc;
    Int_Loc = *Int_Par_Ref + 10;
    do {
        if (Ch_1_Glob == 'A') {
            Int_Loc -= 1;
            *Int_Par_Ref = Int_Loc - Int_Glob;
            Enum_Loc = Ident_1;
        }
    } while (Enum_Loc != Ident_1);
}

static void Proc_4(void)
{
    Boolean Bool_Loc;
    Bool_Loc = Ch_1_Glob == 'A';
    Bool_Glob = Bool_Loc | Bool_Glob;
    Ch_2_Glob = 'B';
}

static void Proc_5(void)
{
    Ch_1_Glob = 'A';
    Bool_Glob = false;
}

static void Proc_1(Rec_Pointer Ptr_Val_Par)
{
    Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;
    structassign(*Ptr_Val_Par->Ptr_Comp, *Ptr_Glob);
    Ptr_Val_Par->variant.var_1.Int_Comp = 5;
    Next_Record->variant.var_1.Int_Comp = Ptr_Val_Par->variant.var_1.Int_Comp;
    Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
    Proc_3(&Next_Record->Ptr_Comp);
    if (Next_Record->Discr == Ident_1) {
        Next_Record->variant.var_1.Int_Comp = 6;
        Proc_6(Ptr_Val_Par->variant.var_1.Enum_Comp,
               &Next_Record->variant.var_1.Enum_Comp);
        Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
        Proc_7(Next_Record->variant.var_1.Int_Comp, 10,
               &Next_Record->variant.var_1.Int_Comp);
    } else
        structassign(*Ptr_Val_Par, *Ptr_Val_Par->Ptr_Comp);
}

int main(void)
{
    One_Fifty   Int_1_Loc = 0;
    One_Fifty   Int_2_Loc = 0;
    One_Fifty   Int_3_Loc = 0;
    char        Ch_Index;
    Enumeration Enum_Loc = Ident_2;
    Str_30      Str_1_Loc;
    Str_30      Str_2_Loc;
    int         Run_Index;
    volatile int Number_Of_Runs = BENCH_ITERS;

    /* 初始化（原 main 的 Proc_0 部分；malloc → 静态池） */
    Next_Ptr_Glob = &Rec_Pool[0];
    Ptr_Glob = &Rec_Pool[1];
    Ptr_Glob->Ptr_Comp = Next_Ptr_Glob;
    Ptr_Glob->Discr = Ident_1;
    Ptr_Glob->variant.var_1.Enum_Comp = Ident_3;
    Ptr_Glob->variant.var_1.Int_Comp = 40;
    xstrcpy(Ptr_Glob->variant.var_1.Str_Comp, "DHRYSTONE PROGRAM, SOME STRING");
    xstrcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");
    Arr_2_Glob[8][7] = 10;

    for (Run_Index = 1; Run_Index <= Number_Of_Runs; ++Run_Index) {
        Proc_5();
        Proc_4();
        /* Ch_1_Glob == 'A', Ch_2_Glob == 'B', Bool_Glob == true */
        Int_1_Loc = 2;
        Int_2_Loc = 3;
        xstrcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
        Enum_Loc = Ident_2;
        Bool_Glob = ! Func_2(Str_1_Loc, Str_2_Loc);
        /* Bool_Glob == 1 */
        while (Int_1_Loc < Int_2_Loc) {         /* loop body executed once */
            Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
            Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
            Int_1_Loc += 1;
        }
        /* Int_1_Loc == 3, Int_2_Loc == 3, Int_3_Loc == 7 */
        Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
        /* Int_Glob == 5 */
        Proc_1(Ptr_Glob);
        for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index) {
            if (Enum_Loc == Func_1(Ch_Index, 'C')) {   /* then, not executed */
                Proc_6(Ident_1, &Enum_Loc);
                xstrcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
                Int_2_Loc = Run_Index;
                Int_Glob = Run_Index;
            }
        }
        Int_2_Loc = Int_2_Loc * Int_1_Loc;
        Int_1_Loc = Int_2_Loc / Int_3_Loc;
        Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
        /* Int_1_Loc == 1, Int_2_Loc == 13, Int_3_Loc == 7 */
        Proc_2(&Int_1_Loc);
        /* Int_1_Loc == 5 */
    }

    /* ---- 终态校验（原版 "should be" 输出块） ---- */
    CHECK_G(__LINE__, Int_Glob == 5);
    CHECK_G(__LINE__, Bool_Glob == 1);
    CHECK_G(__LINE__, Ch_1_Glob == 'A');
    CHECK_G(__LINE__, Ch_2_Glob == 'B');
    CHECK_G(__LINE__, Arr_1_Glob[8] == 7);
    CHECK_G(__LINE__, Arr_2_Glob[8][7] == BENCH_ITERS + 10);
    CHECK_G(__LINE__, Ptr_Glob->Discr == Ident_1);
    CHECK_G(__LINE__, Ptr_Glob->variant.var_1.Enum_Comp == Ident_3);  /* should be 2 */
    CHECK_G(__LINE__, Ptr_Glob->variant.var_1.Int_Comp == 17);
    CHECK_G(__LINE__, xstrcmp(Ptr_Glob->variant.var_1.Str_Comp,
                              "DHRYSTONE PROGRAM, SOME STRING") == 0);
    CHECK_G(__LINE__, Next_Ptr_Glob->Discr == Ident_1);
    CHECK_G(__LINE__, Next_Ptr_Glob->variant.var_1.Enum_Comp == Ident_2);  /* should be 1 */
    CHECK_G(__LINE__, Next_Ptr_Glob->variant.var_1.Int_Comp == 18);
    CHECK_G(__LINE__, xstrcmp(Next_Ptr_Glob->variant.var_1.Str_Comp,
                              "DHRYSTONE PROGRAM, SOME STRING") == 0);
    CHECK_G(__LINE__, Ptr_Glob->Ptr_Comp == Next_Ptr_Glob);
    CHECK_G(__LINE__, Next_Ptr_Glob->Ptr_Comp == Next_Ptr_Glob);
    CHECK_G(__LINE__, Int_1_Loc == 5);
    CHECK_G(__LINE__, Int_2_Loc == 13);
    CHECK_G(__LINE__, Int_3_Loc == 7);
    CHECK_G(__LINE__, Enum_Loc == Ident_2);
    CHECK_G(__LINE__, xstrcmp(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING") == 0);
    CHECK_G(__LINE__, xstrcmp(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING") == 0);

    puts_("PASS bench_dhry nruns=");
    putdec((unsigned long)BENCH_ITERS);
    puts_(" arr2=");
    putdec((unsigned long)Arr_2_Glob[8][7]);
    puts_(" int=5 bool=1\n");
    return 0;
}
