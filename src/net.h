/* net.h - network driver interface. Two implementations:
 *   net_uci.c     the Ultimate Command Interface (real hardware)
 *   net_mbox.c    a mailbox served by tools/vtest.py (VICE tests)
 */
#ifndef NET_H
#define NET_H

unsigned char net_probe(void);                                   /* 0 = network interface present */
unsigned char net_connect(const char *host, unsigned port);      /* 0 = connected */
int net_read(char *buf, unsigned max);                           /* bytes read, 0 = none yet, -1 = closed */
unsigned char net_write(const char *buf, unsigned len);          /* 0 = ok */
void net_close(void);
unsigned char net_time(char *buf);                               /* 0 = "yyyy/mm/dd hh:mm:ss" in buf (32 bytes) */
unsigned char net_ip(void);                                      /* 0 = an interface (Ethernet or Wi-Fi) has an
                                                                    address, 1 = none */

#endif
