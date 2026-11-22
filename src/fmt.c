/* fmt.c - a tiny sprintf replacement: %s, %u, %c and %% only.
 * cc65's sprintf pulls in the whole printf engine (~2.5 KB); GEOS application space is only ~23 KB. */
#include <stdarg.h>
#include "fmt.h"

void fmt(char *dst, const char *f, ...)
{
    va_list ap;
    const char *s;
    char num[6], *p;
    unsigned u;
    va_start(ap, f);
    while (*f) {
        if (*f != '%') { *dst++ = *f++; continue; }
        ++f;
        switch (*f++) {
        case 's':
            s = va_arg(ap, const char *);
            while (*s) *dst++ = *s++;
            break;
        case 'u':
            u = va_arg(ap, unsigned);
            p = num + sizeof(num) - 1;
            *p = 0;
            do { *--p = '0' + u % 10; u /= 10; } while (u);
            while (*p) *dst++ = *p++;
            break;
        case 'c':
            *dst++ = (char)va_arg(ap, int);
            break;
        case '%':
            *dst++ = '%';
            break;
        case 0:
            --f;
            break;
        }
    }
    *dst = 0;
    va_end(ap);
}

