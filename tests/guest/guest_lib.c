#include "guest.h"
void puts_(const char *s) { unsigned long n = 0; while (s[n]) n++; gwrite(1, s, n); }
void puthex(unsigned long v) {
    char b[18]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> 60) & 0xF);
        b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v <<= 4;
    }
    gwrite(1, b, 18);
}
