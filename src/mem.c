/* mem 模块：平坦 512MiB 后备存储 + [lo,hi) 区域登记（M1-a） */
#include "rvsim.h"
#include <stdlib.h>
#include <string.h>

int mem_init(rv_mem *m) {
    m->n = 0;                     /* 覆盖成功/OOM 两条路径，此处只写一次 */
    m->data = calloc(1, MEM_SIZE);
    if (!m->data) {
        m->data = NULL;           /* OOM 失败路径：显式置空，防 free 野指针/泄漏 */
        return -1;
    }
    return 0;
}

void mem_free(rv_mem *m) { free(m->data); m->data = 0; m->n = 0; }

void mem_add_region(rv_mem *m, uint64_t lo, uint64_t hi, int kind) {
    m->rgn[m->n++] = (rv_region){ lo, hi, kind };
}

int mem_check(rv_mem *m, uint64_t a, uint64_t len) {
    /* len=0 视作空区间：仅要求地址在窗口内且落在某区域（lo<=a<=hi，
       现有实现允许空区间落在区域右端点 hi 上）——语义已在 rvsim.h 钉死。 */
    if (a < MEM_BASE || len > MEM_SIZE || a - MEM_BASE > MEM_SIZE - len) return -1;
    for (uint32_t i = 0; i < m->n; i++)
        if (a >= m->rgn[i].lo && a + len <= m->rgn[i].hi) return 0;
    return -1;
}

int mem_read(rv_mem *m, uint64_t a, void *dst, uint64_t n) {
    if (mem_check(m, a, n)) return -1;
    memcpy(dst, m->data + (a - MEM_BASE), n); return 0;
}

int mem_write(rv_mem *m, uint64_t a, const void *src, uint64_t n) {
    if (mem_check(m, a, n)) return -1;
    memcpy(m->data + (a - MEM_BASE), src, n); return 0;
}
