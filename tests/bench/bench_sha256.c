/* bench_sha256.c —— bench 分发 A：自实现 SHA-256（纯移位/异或/加法/常量表）。
 * 消息 = LCG 确定性序列 1 MiB（x = x*1664525 + 1013904223 (mod 2^32)，
 * 种子 0x12345678，每字节取 x>>24）。
 * 已知答案（宿主 python hashlib 独立计算）：
 *   sha256 = b9a56eb486a3f0de877bfc5063efc387683db561edd38f0273fcc3078153980a
 * 完整实现含填充与长度字段（可与标准实现逐字节对照）；纯 RV64I，
 * 无乘除（u32 乘常量由 gcc 展开为移位加）。BENCH_ITERS = 整条消息完整
 * 摘要的重复次数（输出取终次摘要；每次重置 IV，确定性）。
 */
#include "guest.h"

#define BENCH_ITERS 1
#define MSG_SIZE (1u << 20)   /* 1048576 = 2^20，64 的倍数 */

#define SHA_EXPECTED "b9a56eb486a3f0de877bfc5063efc387683db561edd38f0273fcc3078153980a"

static unsigned char msg[MSG_SIZE];
static unsigned char digest[32];

static void fill_msg(void)
{
    volatile unsigned int seed = 0x12345678u;
    unsigned int x = seed;
    for (unsigned int i = 0; i < MSG_SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        msg[i] = (unsigned char)(x >> 24);
    }
}

static const unsigned int K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static unsigned int H[8];

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(const unsigned char *p)
{
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[4 * i] << 24) |
               ((unsigned int)p[4 * i + 1] << 16) |
               ((unsigned int)p[4 * i + 2] << 8) |
               (unsigned int)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    unsigned int a = H[0], b = H[1], c = H[2], d = H[3];
    unsigned int e = H[4], f = H[5], g = H[6], h = H[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = h + S1 + ch + K[i] + w[i];
        unsigned int S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    H[0] += a; H[1] += b; H[2] += c; H[3] += d;
    H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

static void sha256(const unsigned char *m, unsigned long len, unsigned char *out)
{
    static const unsigned int iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    for (int i = 0; i < 8; i++)
        H[i] = iv[i];

    for (unsigned long i = 0; i + 64 <= len; i += 64)
        sha256_block(m + i);

    /* 填充：0x80 + k 个 0 + 64 位大端比特长度；k = (55 - rem) mod 64 */
    unsigned char tail[128];
    unsigned long rem = 0;
    {
        unsigned long i = 0;
        while (i + 64 <= len) i += 64;
        rem = len - i;
        for (unsigned long j = 0; j < rem; j++)
            tail[j] = m[i + j];
    }
    tail[rem] = 0x80;
    unsigned long pad = (rem <= 55) ? (55 - rem) : (119 - rem);
    for (unsigned long j = 1; j <= pad; j++)
        tail[rem + j] = 0;
    unsigned long bits = len << 3;
    for (int i = 0; i < 8; i++)
        tail[rem + pad + 1 + i] = (unsigned char)(bits >> (56 - 8 * i));
    unsigned long tail_len = rem + pad + 9;
    for (unsigned long i = 0; i < tail_len; i += 64)
        sha256_block(tail + i);

    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (unsigned char)(H[i] >> 24);
        out[4 * i + 1] = (unsigned char)(H[i] >> 16);
        out[4 * i + 2] = (unsigned char)(H[i] >> 8);
        out[4 * i + 3] = (unsigned char)(H[i]);
    }
}

static void to_hex(const unsigned char *d, char *out /* 65 字节 */)
{
    for (int i = 0; i < 32; i++) {
        unsigned int hi = d[i] >> 4, lo = d[i] & 0xF;
        out[2 * i]     = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        out[2 * i + 1] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
    }
    out[64] = 0;
}

int main(void)
{
    char hexs[65];

    fill_msg();
    for (int it = 0; it < BENCH_ITERS; it++)
        sha256(msg, MSG_SIZE, digest);

    to_hex(digest, hexs);
    CHECK_G(__LINE__, xstrcmp(hexs, SHA_EXPECTED) == 0);

    puts_("PASS bench_sha256 len=0x100000 sha256=");
    puts_(hexs);
    puts_("\n");
    return 0;
}
