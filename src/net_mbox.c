/* net_mbox.c - test builds only: the network is two byte rings in the mailbox (mbox.h); tools/vtest.py plays
 * the IRC server on the other side through the VICE binary monitor. */
#include "net.h"
#include "mbox.h"

unsigned char net_probe(void) { return 0; }
unsigned char net_ip(void) { return mbox.nocable; }       /* the test's pulled cable: no address */

unsigned char net_connect(const char *host, unsigned port)
{
    (void)host; (void)port;
    if (mbox.nocable) return 1;             /* the test's pulled cable: no connection */
    mbox.rx_tail = mbox.rx_head;            /* nothing old left to read */
    ++mbox.conn;                            /* Python starts a fresh server session */
    return 0;
}

int net_read(char *buf, unsigned max)
{
    unsigned n = 0;
    if (mbox.drop_req != mbox.drop_seen) {  /* Python dropped the connection */
        mbox.drop_seen = mbox.drop_req;
        return -1;
    }
    while (mbox.rx_tail != mbox.rx_head && n < max) {
        buf[n++] = mbox.rx[mbox.rx_tail];
        ++mbox.rx_tail;
    }
    return (int)n;
}

unsigned char net_write(const char *buf, unsigned len)
{
    unsigned wait;
    unsigned char outer;
    while (len--) {
        for (wait = outer = 0; (unsigned char)(mbox.tx_head + 1) == mbox.tx_tail; ++wait)   /* full: Python drains it */
            if (wait == 60000U) { wait = 0; if (++outer == 30) return 1; }
        mbox.tx[mbox.tx_head] = *buf++;
        ++mbox.tx_head;
    }
    return 0;
}

unsigned char net_time(char *buf)
{
    unsigned char i;
    if (!mbox.time[0]) return 1;
    for (i = 0; i < 19; ++i) buf[i] = mbox.time[i];
    buf[19] = 0;
    return 0;
}

void net_close(void)
{
    ++mbox.closed;
}
