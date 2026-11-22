/* fmt.h - tiny sprintf: %s %u %c %% */
#ifndef FMT_H
#define FMT_H
void fmt(char *dst, const char *f, ...);
unsigned char __fastcall__ ncase(const char *a, const char *b, unsigned char n);   /* hw.s: n chars equal
                                                                                     ignoring case (0 = all) */
#define streq_nocase(a, b) ncase((a), (b), 0)                     /* 1 = equal, ignoring case */

#endif
