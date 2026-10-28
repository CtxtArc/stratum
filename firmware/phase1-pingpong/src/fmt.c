#include "fmt.h"
#include "board.h"
#include <stdarg.h>
#include <stdbool.h>

static void emit(char c)
{
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
}

static void emit_num(uint32_t v, unsigned base, bool upper, bool neg,
                     int width, bool zero, bool left)
{
    char buf[12];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { buf[n++] = digits[v % base]; v /= base; } while (v);
    int len = n + (neg ? 1 : 0);
    int pad = width > len ? width - len : 0;

    if (!left && !zero) while (pad-- > 0) emit(' ');
    if (neg) emit('-');
    if (!left && zero) while (pad-- > 0) emit('0');
    while (n) emit(buf[--n]);
    if (left) while (pad-- > 0) emit(' ');
}

void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit(*p); continue; }
        p++;
        bool zero = false, left = false;
        int width = 0;
        for (;; p++) {
            if (*p == '0') zero = true;
            else if (*p == '-') left = true;
            else break;
        }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        while (*p == 'l') p++;

        switch (*p) {
        case 'd': case 'i': {
            int32_t v = va_arg(ap, int32_t);
            bool neg = v < 0;
            uint32_t u = neg ? (uint32_t)(-(v + 1)) + 1u : (uint32_t)v;
            emit_num(u, 10, false, neg, width, zero, left);
            break;
        }
        case 'u': emit_num(va_arg(ap, uint32_t), 10, false, false, width, zero, left); break;
        case 'x': emit_num(va_arg(ap, uint32_t), 16, false, false, width, zero, left); break;
        case 'X': emit_num(va_arg(ap, uint32_t), 16, true,  false, width, zero, left); break;
        case 'c': emit((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = 0;
            while (s[len]) len++;
            int pad = width > len ? width - len : 0;
            if (!left) while (pad-- > 0) emit(' ');
            while (*s) emit(*s++);
            if (left) while (pad-- > 0) emit(' ');
            break;
        }
        case '%': emit('%'); break;
        case '\0': p--; break;
        default: emit('%'); emit(*p); break;
        }
    }
    va_end(ap);
}

void out_q4(int32_t q4)
{
    bool neg = q4 < 0;
    uint32_t a = neg ? (uint32_t)(-q4) : (uint32_t)q4;
    if (neg) emit('-');
    out("%u.%02u", a / 4u, (a % 4u) * 25u);
}
