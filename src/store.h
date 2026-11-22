/* store.h - message store: every window's messages, in one ring in C128 RAM or in the REU */
#ifndef STORE_H
#define STORE_H

#define MAXWIN          8           /* window 0 = server/status, 1..7 = channels and private chats */
#define STORE_TEXT_MAX  430         /* longest stored message */

typedef unsigned long spos;         /* logical position of a record in the ring (never wraps) */
#define SPOS_NONE       0xFFFFFFFFUL

extern unsigned char store_banks;   /* REU banks in use (0 = C128 RAM ring) */
extern unsigned char store_bank0;   /* first REU bank used */

void store_init(void);
void store_add(unsigned char win, const char *text);
spos store_last(unsigned char win);         /* newest record of a window, or SPOS_NONE */
spos store_prev(spos p);                    /* previous record of the same window, or SPOS_NONE */
unsigned store_get(spos p, char *buf, unsigned max);   /* text of a record (NUL-terminated) */
void store_forget(unsigned char win);       /* drop a window's messages (they become unreachable) */

#endif
