/* store.c - message store.
 *
 * All windows share one ring of records:  prev (4 bytes) | window (1) | length (2) | text.
 * 'prev' is the position of the previous record of the same window, so each window's messages form a linked
 * list that is walked backwards for scrollback, however busy the other windows are. Positions are logical
 * (they only grow); a position is still valid while it is within one ring size of the write position, so
 * records overwritten by newer ones are simply no longer reachable.
 *
 * The ring lives in an REU when GEOS reports one (ramExpSize), in banks above everything GEOS uses: bank 0
 * is the kernal's, and RAM disks / shadowed drives start at ramBase[drive] (Hitchhiker's Guide to GEOS,
 * sysRAMFlg, ramBase, driveType). Transfers use GEOS's StashRAM/FetchRAM. Without an REU (or if a test
 * pattern does not read back) the ring is 32 KB of the C128's backRAM, moved with GEOS 128's MoveBData.
 */
#include <string.h>
#include <geos.h>
#include "store.h"
#include "hw.h"

/* without an REU: the GEOS 128 desk accessory swap area in backRAM (bank 0 $2000-$7FFF, "if your application
 * does not use Desk Accessories this may be used as an application data area") plus $8000-$9FFF ("Unused"):
 * Hitchhiker's Guide to GEOS, appendix E, 128 BackRAM. geoChat 128 has no desk accessory menu. */
#define BACK_BASE   0x2000
#define STORE_RAM   0x8000          /* power of two */
#define MAXBANKS    8               /* REU: 512 KB, thousands of messages */

unsigned char store_banks, store_bank0;
static unsigned long size, wpos;
static spos last[MAXWIN];

struct rec {
    spos prev;
    unsigned char win;
    unsigned len;
};

static void xfer(spos p, char *buf, unsigned n, unsigned char wr)
{
    unsigned long ph;
    unsigned off, chunk;
    while (n) {
        ph = p & (size - 1);
        off = (unsigned)ph;
        if (!store_banks) {
            chunk = STORE_RAM - off;
            if (chunk > n) chunk = n;
            if (wr) { bm_src = (unsigned)buf; bm_dst = BACK_BASE + off; bm_banks[0] = 1; bm_banks[1] = 0; }
            else { bm_src = BACK_BASE + off; bm_dst = (unsigned)buf; bm_banks[0] = 0; bm_banks[1] = 1; }
            bm_len = chunk;
            back_move();
        } else {
            unsigned char bank = store_bank0 + (unsigned char)(ph >> 16);
            chunk = (off && (unsigned)(0 - off) < n) ? (unsigned)(0 - off) : n;   /* stay inside the bank */
            if (wr) StashRAM(bank, chunk, (char *)off, buf);
            else FetchRAM(bank, chunk, (char *)off, buf);
        }
        p += chunk; buf += chunk; n -= chunk;
    }
}

static unsigned char valid(spos p)
{
    return p != SPOS_NONE && wpos - p <= size - sizeof(struct rec) - STORE_TEXT_MAX;
}

/* REU banks free for us: above bank 0 and above every RAM disk / shadowed drive */
static unsigned char free_first(void)
{
    unsigned char i, t, first = 1, end;
    for (i = 0; i < 4; ++i) {
        t = driveType[i];
        if (!(t & 0xC0)) continue;                      /* b7 RAM disk, b6 shadowed */
        t &= 0x0F;                                      /* 1 = 1541, 2 = 1571, 3 = 1581 */
        end = (&ramBase)[i] + (t == 1 ? 3 : t == 2 ? 6 : 13);
        if (end > first) first = end;
    }
    return first;
}

void store_init(void)
{
    static const char pat[4] = { 'I', 'R', 'C', 128 };
    char chk[4];
    unsigned char i, first, avail;
    store_banks = 0;
    if (ramExpSize > 1) {
        first = free_first();
        avail = ramExpSize > first ? ramExpSize - first : 0;
        if (avail > MAXBANKS) avail = MAXBANKS;
        for (store_banks = 1; store_banks * 2 <= avail; store_banks *= 2) ;    /* power of two */
        if (!avail) store_banks = 0;
        if (store_banks) {
            store_bank0 = first;
            StashRAM(first, 4, (char *)0, pat);
            FetchRAM(first, 4, (char *)0, chk);
            if (memcmp(pat, chk, 4)) store_banks = 0;
        }
    }
    size = store_banks ? (unsigned long)store_banks << 16 : STORE_RAM;
    wpos = 0;
    for (i = 0; i < MAXWIN; ++i) last[i] = SPOS_NONE;
}

void store_add(unsigned char win, const char *text)
{
    struct rec r;
    unsigned n = strlen(text);
    if (n > STORE_TEXT_MAX) n = STORE_TEXT_MAX;
    r.prev = valid(last[win]) ? last[win] : SPOS_NONE;
    r.win = win;
    r.len = n;
    xfer(wpos, (char *)&r, sizeof(r), 1);
    xfer(wpos + sizeof(r), (char *)text, n, 1);
    last[win] = wpos;
    wpos += sizeof(r) + n;
}

spos store_last(unsigned char win)
{
    return valid(last[win]) ? last[win] : SPOS_NONE;
}

spos store_prev(spos p)
{
    struct rec r;
    if (!valid(p)) return SPOS_NONE;
    xfer(p, (char *)&r, sizeof(r), 0);
    return valid(r.prev) ? r.prev : SPOS_NONE;
}

unsigned store_get(spos p, char *buf, unsigned max)
{
    struct rec r;
    unsigned n;
    buf[0] = 0;
    if (!valid(p)) return 0;
    xfer(p, (char *)&r, sizeof(r), 0);
    n = r.len < max ? r.len : max - 1;
    xfer(p + sizeof(r), buf, n, 0);
    buf[n] = 0;
    return n;
}

void store_forget(unsigned char win)
{
    last[win] = SPOS_NONE;
}
