#ifndef GUEST_H
#define GUEST_H
void gwrite(int fd, const void *buf, unsigned long n);
void gexit(int code) __attribute__((noreturn));
void puts_(const char *s);
void puthex(unsigned long v);
#define CHECK_G(line, cond) do { if (!(cond)) { puts_("FAIL\n"); gexit(line); } } while (0)
/* bench 分发 A：mini-crt 字符串补充（语义与 C 标准一致；实现见 guest_lib.c） */
char *xstrcpy(char *dst, const char *src);
int xstrcmp(const char *a, const char *b);
unsigned long xstrlen(const char *s);
#endif
