#ifndef GUEST_H
#define GUEST_H
void gwrite(int fd, const void *buf, unsigned long n);
void gexit(int code) __attribute__((noreturn));
void puts_(const char *s);
void puthex(unsigned long v);
#define CHECK_G(line, cond) do { if (!(cond)) { puts_("FAIL\n"); gexit(line); } } while (0)
#endif
