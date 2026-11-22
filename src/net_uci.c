/* net_uci.c - network driver for the Ultimate Command Interface (Ultimate 1541 II+, Ultimate 64, C64 Ultimate).
 *
 * Written from Gideon's documentation (1541u-documentation: uci/core_uci_architecture.rst and
 * uci/network_target.rst). The Command Interface must be enabled in the Ultimate's menu.
 *
 * Registers ($DF1C-$DF1F, I/O2): control (write) / status (read), command data (write) / ID (read, $C9),
 * response data, status data. A command is written byte by byte, pushed with PUSH_CMD, then the reply's data
 * and status queues are read until the state machine leaves "data more", accepting each block with DATA_ACC.
 *
 * C128 notes: I/O is made visible through the MMU ($FF00 = $7E, as GEOS 128's START_IO_128 does), and the
 * CPU is put to 1 MHz ($D030 bit 0) while talking to the cartridge, then restored.
 */
#include <string.h>
#include "net.h"

#define MMU_CFG   (*(volatile unsigned char *)0xFF00)
#define CIOIN     0x7E
#define CLKREG    (*(volatile unsigned char *)0xD030)

#define UCI_CTRL  (*(volatile unsigned char *)0xDF1C)   /* write: control  / read: status */
#define UCI_CMD   (*(volatile unsigned char *)0xDF1D)   /* write: command  / read: identification ($C9) */
#define UCI_RESP  (*(volatile unsigned char *)0xDF1E)   /* response data */
#define UCI_STAT  (*(volatile unsigned char *)0xDF1F)   /* status data ("00,OK") */

/* control bits */
#define C_PUSH    0x01
#define C_ACC     0x02
#define C_ABORT   0x04
#define C_CLRERR  0x08
/* status bits */
#define S_DATA_AV 0x80
#define S_STAT_AV 0x40
#define S_STATE   0x30
#define ST_IDLE   0x00
#define ST_BUSY   0x10
#define ST_MORE   0x30
#define S_ERROR   0x08

#define TGT_NET   0x03
#define NET_OPEN_TCP  0x07
#define NET_CLOSE     0x09
#define NET_READ      0x10
#define NET_WRITE     0x11

#define MAX_IO    512           /* per read/write command; queues are 896 bytes (issue #807: >892 fails) */

static unsigned char sock = 0xff;
static unsigned char saved_cfg, saved_clk;
#pragma bss-name (push, "HIBSS")     /* $A040-$BF7F, not cleared at start-up (src/geochat128.cfg) */
static unsigned char cmd[4 + 64];
static char status[32];
static unsigned char rbuf[MAX_IO + 2];
#pragma bss-name (pop)

static void io_on(void)
{
    saved_cfg = MMU_CFG;
    MMU_CFG = CIOIN;
    saved_clk = CLKREG;
    CLKREG = saved_clk & 0xFE;          /* 1 MHz while on the cartridge bus */
}

static void io_off(void)
{
    CLKREG = saved_clk;
    MMU_CFG = saved_cfg;
}

/* wait until the state machine is idle; abort a stuck command. 0 = ok */
static unsigned char wait_idle(void)
{
    unsigned t;
    if (UCI_CTRL & S_ERROR) UCI_CTRL = C_CLRERR;
    for (t = 0; t < 20000; ++t)
        if ((UCI_CTRL & S_STATE) == ST_IDLE) return 0;
    UCI_CTRL = C_ABORT;
    for (t = 0; t < 20000; ++t)
        if ((UCI_CTRL & S_STATE) == ST_IDLE) return 0;
    return 1;
}

/* Send cmd[0..clen) followed by payload, collect up to rmax response bytes into resp.
 * Returns the status code (first two status digits, e.g. 0 for "00,OK"), or 255 on a protocol error.
 * *rlen = number of response bytes stored. Called with io_on(). */
static unsigned char uci(unsigned clen, const char *payload, unsigned plen,
                         unsigned char *resp, unsigned rmax, unsigned *rlen)
{
    unsigned i, n = 0, t;
    unsigned char st, s = 0, more;

    if (wait_idle()) return 255;
    for (i = 0; i < clen; ++i) UCI_CMD = cmd[i];
    for (i = 0; i < plen; ++i) UCI_CMD = payload[i];
    UCI_CTRL = C_PUSH;

    do {
        for (t = 0; ; ++t) {                        /* wait for the reply (DNS/connect can take seconds) */
            st = UCI_CTRL;
            if ((st & S_STATE) != ST_BUSY && (st & S_STATE) != ST_IDLE) break;
            if (t == 60000U) { UCI_CTRL = C_ABORT; return 255; }
        }
        while (UCI_CTRL & S_DATA_AV) {
            st = UCI_RESP;
            if (n < rmax) resp[n++] = st;
        }
        while (UCI_CTRL & S_STAT_AV) {
            st = UCI_STAT;
            if (s < sizeof(status) - 1) status[s++] = st;
        }
        more = (UCI_CTRL & S_STATE) == ST_MORE;
        UCI_CTRL = C_ACC;
    } while (more);
    status[s] = 0;
    if (rlen) *rlen = n;
    if (s >= 2 && status[0] >= '0' && status[0] <= '9')
        return (status[0] - '0') * 10 + (status[1] - '0');
    return 0;
}

/* the Ultimate's real-time clock: DOS target ($01), DOS_CMD_GET_TIME ($26), format 0 = "yyyy/mm/dd hh:mm:ss"
 * (ultimate_uci_ultimate_dos_target, firmware DOS 1.2+) */
unsigned char net_time(char *buf)
{
    unsigned char r;
    unsigned got;
    if (net_probe()) return 1;
    cmd[0] = 0x01; cmd[1] = 0x26; cmd[2] = 0;
    io_on();
    r = uci(3, 0, 0, (unsigned char *)buf, 31, &got);
    io_off();
    buf[got] = 0;
    return r || got < 19;
}

/* does any network interface have an address? NET_CMD_GET_IPADDR ($03 $05 n) for n = 0, 1, ... until one reports
 * an address or the index is out of range: 12 bytes, the first 4 the address (ultimate_uci_network_target).
 * Ethernet and Wi-Fi are separate interfaces, and interface 0 reports 0.0.0.0 on a Wi-Fi-only machine
 * (ultimateii-dos-lib PR #63). */
static unsigned char ipb[12];
static unsigned ipgot;
unsigned char net_ip(void)
{
    if (net_probe()) return 1;
    cmd[0] = TGT_NET; cmd[1] = 0x05; cmd[2] = 0;
    io_on();
    while (!uci(3, 0, 0, ipb, 12, &ipgot) && ipgot >= 4) {
        if (ipb[0] | ipb[1] | ipb[2] | ipb[3]) { io_off(); return 0; }
        if (++cmd[2] == 4) break;
    }
    io_off();
    return 1;
}

unsigned char net_probe(void)
{
    unsigned char id;
    io_on();
    id = UCI_CMD;                                   /* identification register: $C9 ($49 with IRQ pending) */
    io_off();
    return (id & 0x7F) == 0x49 ? 0 : 1;
}

unsigned char net_connect(const char *host, unsigned port)
{
    unsigned char r, h;
    unsigned n = strlen(host);
    if (n > 63) return 1;
    cmd[0] = TGT_NET; cmd[1] = NET_OPEN_TCP;
    cmd[2] = port & 0xff; cmd[3] = port >> 8;
    memcpy(cmd + 4, host, n);
    cmd[4 + n] = 0;
    io_on();
    r = uci(5 + n, 0, 0, &h, 1, 0);
    io_off();
    if (r) return 1;
    sock = h;
    return 0;
}

int net_read(char *buf, unsigned max)
{
    unsigned char r;
    unsigned got, count;
    if (sock == 0xff) return -1;
    if (max > MAX_IO) max = MAX_IO;
    cmd[0] = TGT_NET; cmd[1] = NET_READ; cmd[2] = sock;
    cmd[3] = max & 0xff; cmd[4] = max >> 8;
    io_on();
    r = uci(5, 0, 0, rbuf, max + 2, &got);
    io_off();
    if (r == 1) { sock = 0xff; return -1; }        /* 01,CONNECTION CLOSED BY HOST */
    if (r || got < 2) return 0;                     /* e.g. 02,NO DATA */
    count = rbuf[0] | (rbuf[1] << 8);
    if (count == 0xFFFF || count > got - 2) count = got - 2;   /* issue #847: 65535 on an idle read */
    memcpy(buf, rbuf + 2, count);
    return (int)count;
}

unsigned char net_write(const char *buf, unsigned len)
{
    unsigned char r = 0;
    unsigned chunk;
    if (sock == 0xff) return 1;
    io_on();
    while (len && !r) {
        chunk = len > MAX_IO ? MAX_IO : len;
        cmd[0] = TGT_NET; cmd[1] = NET_WRITE; cmd[2] = sock;
        r = uci(3, buf, chunk, 0, 0, 0);
        buf += chunk; len -= chunk;
    }
    io_off();
    return r;
}

void net_close(void)
{
    if (sock == 0xff) return;
    cmd[0] = TGT_NET; cmd[1] = NET_CLOSE; cmd[2] = sock;
    io_on();
    uci(3, 0, 0, 0, 0, 0);
    io_off();
    sock = 0xff;
}
