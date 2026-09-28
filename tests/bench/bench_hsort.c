/* bench_hsort.c —— bench 分发 A：64K 元素堆排序（确定性 LCG 输入）。
 * 元素 = LCG：x = x*1103515245 + 12345 (mod 2^32)，种子 0xCAFEBABE。
 * 已知答案（宿主 python 独立计算，与排序无关——多重集校验和不变）：
 *   sum(模 2^64) = 0x7ffa42788000, xor = 0x41510000。
 * 校验：有序性 + 两个校验和；纯 RV64I（无乘除；2*i+1 用移位书写）。
 */
#include "guest.h"

#define BENCH_ITERS 3
#define HN (64u * 1024u)

#define SUM_EXPECTED 0x7ffa42788000ULL
#define XOR_EXPECTED 0x41510000u

static unsigned int arr[HN];

static void gen(void)
{
    volatile unsigned int seed = 0xCAFEBABEu;
    unsigned int x = seed;
    for (unsigned int i = 0; i < HN; i++) {
        x = x * 1103515245u + 12345u;
        arr[i] = x;
    }
}

static void sift(unsigned int root, unsigned int end)
{
    for (;;) {
        unsigned int child = (root << 1) + 1;
        if (child > end)
            return;
        if (child < end && arr[child + 1] > arr[child])
            child++;
        if (arr[root] >= arr[child])
            return;
        unsigned int t = arr[root];
        arr[root] = arr[child];
        arr[child] = t;
        root = child;
    }
}

static void hsort(void)
{
    for (unsigned int i = HN / 2; i-- > 0; )
        sift(i, HN - 1);
    for (unsigned int end = HN - 1; end > 0; end--) {
        unsigned int t = arr[0];
        arr[0] = arr[end];
        arr[end] = t;
        sift(0, end - 1);
    }
}

int main(void)
{
    unsigned long long sum = 0;
    unsigned int xr = 0;

    for (int it = 0; it < BENCH_ITERS; it++) {
        gen();
        hsort();
    }

    for (unsigned int i = 0; i + 1 < HN; i++)
        CHECK_G(__LINE__, arr[i] <= arr[i + 1]);
    for (unsigned int i = 0; i < HN; i++) {
        sum += arr[i];
        xr ^= arr[i];
    }
    CHECK_G(__LINE__, sum == SUM_EXPECTED);
    CHECK_G(__LINE__, xr == XOR_EXPECTED);

    puts_("PASS bench_hsort n=0x10000 sum=");
    puthex(sum);
    puts_(" xor=");
    puthex((unsigned long)xr);
    puts_("\n");
    return 0;
}
