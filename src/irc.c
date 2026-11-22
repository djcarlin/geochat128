/* irc.c - IRC protocol: connection, line assembly, parsing, routing to windows, user commands
 * (RFC 1459/2812 subset).
 *
 * Windows (ui.c): 0 = server window, then one per channel and per private chat. Channel messages go to the
 * channel's window, private messages open a window named after the sender, notices go to the active window.
 * QUIT and NICK go to every window where that nick has been seen: each window keeps a 256-bit Bloom filter
 * of nicks (from NAMES, JOIN and messages) - 32 bytes instead of a member list.
 *
 * Text from the network is cleaned before display: mIRC bold/reverse become GEOS style codes, colour codes
 * and other control bytes are dropped, and each UTF-8 sequence becomes a single '?'.
 *
 * If the connection drops without the user asking (no /quit, no disconnect), we reconnect after 10, 30, 60 s
 * and rejoin every channel window.
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <geos.h>          /* after the C headers: GEOS defines a macro named 'string' */
#include "irc.h"
#include "net.h"
#include "fmt.h"
#include "ui.h"
#include "hw.h"
#include "version.h"

#ifndef RECONNECT_TICKS
#define RECONNECT_TICKS 300                 /* irc_poll runs every 2 jiffies: ~10 s */
#endif
/* A pulled network cable reports nothing: no data and no error. After ~3 minutes of silence send our own PING
 * (servers ping every few minutes, so silence that long is already suspicious); after ~90 s more of silence the
 * connection counts as lost. (Seen on the real C128: 6 minutes unplugged went unnoticed.) */
#ifndef KEEPALIVE_TICKS
#define KEEPALIVE_TICKS (RECONNECT_TICKS * 18)
#endif

unsigned irc_lines_in;
unsigned char irc_state;                    /* 0 offline, 1 registering, 2 registered */
unsigned irc_retry;                         /* polls until the next reconnect attempt (0 = none planned) */

static char server[64];
static unsigned port;
static char nick[17];
static char want_chan[33];                  /* channel to join after registering (settings) */
static unsigned char retries;
static unsigned quiet;                      /* irc_poll ticks without any data from the server */
static void retry_later(void);

static unsigned irc_rxlen;                  /* bytes in rx */
#pragma bss-name (push, "HIBSS")     /* $A040-$BF7F, not cleared at start-up (src/geochat128.cfg) */
static char tx[300];
static char rx[530];
static char out[420];                       /* message being built for display */
static char clean[400];

static char *arg[12];                       /* parameters of the line being handled; the trailing one last */
static unsigned char argc_;

static unsigned char bloom[MAXWIN][32];
#define POOLSZ 768
static char pool[POOLSZ];                   /* nicks for completion, packed "a\0b\0...\0\0": people who talk or join at
                                               the front, names from NAMES lists at the back (about 75 nicks) */
#pragma bss-name (pop)

/* ---- helpers ------------------------------------------------------------------------------------------ */

static void send_line(const char *s)
{
    unsigned n = strlen(s);
    if (n > sizeof(tx) - 3) n = sizeof(tx) - 3;
    memmove(tx, s, n);
    tx[n] = '\r'; tx[n + 1] = '\n';
    net_write(tx, n + 2);
}

static void update_status(void)
{
    char s[48];
    if (irc_state)
        fmt(s, "%s @ %s", nick, server);
    else if (irc_retry)
        fmt(s, "reconnecting...");
    else
        strcpy(s, "not connected");
    ui_status(s);
}

static void sys_to(unsigned char w, const char *msg)
{
    strcpy(out, "*** ");
    strncat(out, msg, sizeof(out) - 6);
    ui_add(w, out, 0);
}

static void sys(const char *msg)
{
    sys_to(ui_active, msg);
}

static unsigned char is_chan(const char *s)
{
    return *s == '#' || *s == '&' || *s == '+' || *s == '!';
}

/* network text -> displayable text with GEOS style codes */
static void sanitize(const char *src, char *dst, unsigned max)
{
    unsigned char c, bold = 0, rev = 0;
    unsigned n = 0;
    while ((c = *src++) != 0 && n < max - 3) {
        if (c >= 32 && c < 127) { dst[n++] = c; continue; }
        if (c == 2) { bold = !bold; dst[n++] = bold ? BOLDON : PLAINTEXT; if (!bold && rev) dst[n++] = REV_ON; continue; }
        if (c == 0x16) { rev = !rev; dst[n++] = rev ? REV_ON : REV_OFF; continue; }
        if (c == 0x0f) { bold = rev = 0; dst[n++] = PLAINTEXT; continue; }
        if (c == 3) {                           /* mIRC colour: ^C[fg[,bg]] */
            if (isdigit(*src)) { ++src; if (isdigit(*src)) ++src; }
            if (*src == ',' && isdigit(src[1])) { src += 2; if (isdigit(*src)) ++src; }
            continue;
        }
        if (c >= 0xc0) {                        /* UTF-8 lead byte: skip continuation bytes */
            while ((*src & 0xc0) == 0x80) ++src;
            dst[n++] = '?';
            continue;
        }
        if (c >= 0x80) { dst[n++] = '?'; continue; }
        /* other control characters: dropped */
    }
    if (bold || rev) dst[n++] = PLAINTEXT;
    dst[n] = 0;
}

/* nicks: lower-case hash into each window's Bloom filter, and the recent-nick pool */
static unsigned char nhash(const char *s, unsigned char h)
{
    char c;
    while ((c = *s++) != 0) {
        if (c >= 'A' && c <= 'Z') c += 32;
        h = (unsigned char)((h << 3) | (h >> 5)) ^ c;
        h += 0x3D;
    }
    return h;
}

static void bloom_add(unsigned char w, const char *n)
{
    unsigned char a = nhash(n, 0x5A), b = nhash(n, 0xC3);
    bloom[w][a >> 3] |= 1 << (a & 7);
    bloom[w][b >> 3] |= 1 << (b & 7);
}

static unsigned char bloom_has(unsigned char w, const char *n)
{
    unsigned char a = nhash(n, 0x5A), b = nhash(n, 0xC3);
    return (bloom[w][a >> 3] & (1 << (a & 7))) && (bloom[w][b >> 3] & (1 << (b & 7)));
}

static unsigned pool_len;                   /* bytes used, not counting the final empty string */

static char *nxt(const char *p)             /* the next nick in the pool */
{
    return (char *)p + strlen(p) + 1;
}

static char *pool_find(const char *n)
{
    char *p;
    for (p = pool; *p; p = nxt(p))
        if (streq_nocase(p, n)) return p;
    return 0;
}

static void pool_del(char *e)
{
    unsigned l = strlen(e) + 1;
    memmove(e, e + l, pool_len - (unsigned)(e - pool) - l + 1);
    pool_len -= l;
}

/* front: someone talked, joined or changed nick (completion offers them first); otherwise a NAMES entry, which
 * goes at the back and only if there is room, so a big channel's list never pushes out the people talking */
static void pool_add(const char *n, unsigned char front)
{
    unsigned l = strlen(n) + 1;
    char *p;
    if (l < 2 || l > 32) return;
    if ((p = pool_find(n)) != 0) {
        if (!front) return;
        pool_del(p);
    }
    if (!front) {
        if (pool_len + l >= POOLSZ) return;
        strcpy(pool + pool_len, n);
        pool_len += l;
        pool[pool_len] = 0;
        return;
    }
    while (pool_len + l >= POOLSZ) {        /* drop the last entries */
        for (p = pool; nxt(p) < pool + pool_len; p = nxt(p)) ;
        pool_len = (unsigned)(p - pool);
        *p = 0;
    }
    memmove(pool + l, pool, pool_len + 1);
    strcpy(pool, n);
    pool_len += l;
}

static void seen(unsigned char w, const char *n)
{
    bloom_add(w, n);
    pool_add(n, 1);
}

const char *irc_complete(const char *prefix, unsigned char nth)
{
    unsigned char k = strlen(prefix);
    const char *p;
    for (p = pool; *p; p = nxt(p)) {
        if (streq_nocase(p, nick)) continue;
        if ((!k || ncase(p, prefix, k)) && nth-- == 0) return p;
    }
    return 0;
}

/* does the message mention our nick (as a word)? */
static unsigned char mentions(const char *t)
{
    unsigned char n = strlen(nick);
    for (; *t; ++t)
        if (ncase(t, nick, n) && !isalnum(t[n])) return 1;
    return 0;
}

static void ctcp_reply(const char *to, const char *what)
{
    fmt(tx, "NOTICE %s :\001%s\001", to, what);
    send_line(tx);
}

/* ---- incoming lines -------------------------------------------------------------------------------- */

static void show_privmsg(const char *from, const char *target, const char *text, unsigned char notice)
{
    unsigned n;
    const char *t = text;
    unsigned char action = 0, w, flags = 0;
    char *e;

    if (t[0] == 1) {                                             /* CTCP */
        strncpy(clean, t + 1, 120); clean[120] = 0;
        e = strchr(clean, 1); if (e) *e = 0;
        if (!notice && strncmp(clean, "ACTION ", 7) == 0) { action = 1; t += 8; }
        else {
            if (notice) {                                        /* a reply to one of ours */
                fmt(out, "-%s- CTCP reply: %s", from, clean);
                sanitize(out, clean, sizeof(clean));
                ui_add(ui_active, clean, 0);
            } else if (strcmp(clean, "VERSION") == 0) {
                ctcp_reply(from, "VERSION " APP_NAME " " GEOCHAT_VERSION " (GEOS 128)");
            } else if (strncmp(clean, "PING", 4) == 0) {
                ctcp_reply(from, clean);                         /* echo the argument back */
            } else if (strcmp(clean, "TIME") == 0) {
                char tm[24];
                fmt(tm, "TIME %u:%c%u", (unsigned)system_date.s_hour,
                    '0' + system_date.s_minutes / 10, (unsigned)(system_date.s_minutes % 10));
                ctcp_reply(from, tm);
            } else if (strcmp(clean, "CLIENTINFO") == 0) {
                ctcp_reply(from, "CLIENTINFO ACTION CLIENTINFO PING TIME VERSION");
            }
            return;
        }
    }
    sanitize(t, clean, sizeof(clean));
    n = strlen(clean);
    if (action && n && clean[n - 1] == 1) clean[n - 1] = 0;

    if (is_chan(target)) {
        w = ui_win_open(target);
        seen(w, from);
        if (!notice && mentions(clean)) flags = UI_MENTION;
    } else if (!notice && streq_nocase(target, nick)) {        /* private message: its own window */
        w = ui_win_open(from);
        seen(w, from);
        flags = UI_MENTION;
    } else
        w = irc_state == 2 ? ui_active : 0;                      /* notices: while connecting (the server's
                                                                    "*** Looking up your hostname"...) to the
                                                                    server window, later (NickServ) where you are */

    if (notice)
        fmt(out, "-%s- %s", from, clean);
    else if (action)
        fmt(out, "* %s %s", from, clean);
    else if (flags && is_chan(target))
        fmt(out, "%c<%s>%c %s", REV_ON, from, REV_OFF, clean); /* mentions us: nick in reverse */
    else
        fmt(out, "%c<%s>%c %s", BOLDON, from, PLAINTEXT, clean);
    ui_add(w, out, flags);
}

/* to every window where 'who' has been seen (and their private-chat window) */
static void to_seen(const char *who, const char *msg)
{
    unsigned char i, any = 0;
    for (i = 1; i < MAXWIN; ++i)
        if (ui_win[i].used && (bloom_has(i, who) || streq_nocase(ui_win[i].name, who))) {
            ui_add(i, msg, 0); any = 1;
        }
    if (!any) ui_add(0, msg, 0);
}

static void join_all(void)                  /* after registering: the open channel windows + the settings channel */
{
    unsigned char i;
    for (i = 1; i < MAXWIN; ++i)
        if (ui_win[i].used && is_chan(ui_win[i].name)) { fmt(tx, "JOIN %s", ui_win[i].name); send_line(tx); }
    if (want_chan[0] && ui_win_find(want_chan) == 0xFF) { fmt(tx, "JOIN %s", want_chan); send_line(tx); }
}

static void numeric(unsigned num, const char *from)
{
    const char *a1 = argc_ > 1 ? arg[1] : "", *last = arg[argc_ - 1];
    unsigned char w;
    switch (num) {
    case 1:                                                     /* RPL_WELCOME: registered */
        irc_state = 2; retries = 0;
        strncpy(nick, arg[0], sizeof(nick) - 1);
        update_status();
        join_all();
        break;
    case 433:                                                   /* nickname in use */
        if (irc_state == 1) {
            unsigned char n = strlen(nick);
            fmt(clean, "%s is taken, trying ", nick);
            if (n < 15) { nick[n] = '_'; nick[n + 1] = 0; }
            fmt(tx, "NICK %s", nick); send_line(tx);
            strcat(clean, nick);
            sys_to(0, clean);
            return;
        }
        break;
    case 332:                                                   /* topic: <me> <#chan> :topic */
        sanitize(last, clean, sizeof(clean));
        fmt(out, "*** Topic: %s", clean);
        ui_add(ui_win_open(a1), out, 0);
        return;
    case 353: {                                                 /* names: <me> = <#chan> :a @b +c */
        char *p, *q;
        w = ui_win_open(argc_ > 2 ? (const char *)arg[2] : a1);
        sanitize(last, clean, sizeof(clean));
        fmt(out, "*** Users: %s", clean);
        ui_add(w, out, 0);
        for (p = (char *)last; *p; p = q) {
            while (*p == ' ') ++p;
            q = strchr(p, ' ');
            if (q) *q++ = 0; else q = p + strlen(p);
            while (*p == '@' || *p == '+' || *p == '%' || *p == '~' || *p == '&') ++p;
            if (*p) { bloom_add(w, p); pool_add(p, 0); }
        }
        return;
    }
    case 333: case 366: case 329: case 318: case 315:           /* topic info, end of lists: quiet */
        return;
    case 311:                                                   /* whois: <me> nick user host * :real */
        fmt(out, "*** %s is %s@%s (%s)", a1, argc_ > 2 ? arg[2] : "", argc_ > 3 ? arg[3] : "", last);
        sanitize(out, clean, sizeof(clean)); ui_add(ui_active, clean, 0);
        return;
    case 312: case 319: case 330: case 317: case 301: case 671: /* whois details, away */
        sanitize(last, clean, sizeof(clean));
        if (num == 317) fmt(out, "*** %s has been idle %s seconds", a1, argc_ > 2 ? arg[2] : "?");
        else if (num == 301) fmt(out, "*** %s is away: %s", a1, clean);
        else fmt(out, "*** %s %s %s", a1, num == 312 ? arg[2] : "", clean);
        w = ui_win_find(a1);
        ui_add(num == 301 && w != 0xFF ? w : ui_active, out, 0);
        return;
    }
    /* everything else: errors (4xx/5xx) to the active window, the rest to the server window */
    out[0] = 0;
    for (w = 1; w < argc_; ++w) {
        if (w > 1) strcat(out, " ");
        strncat(out, arg[w], 300);
    }
    sanitize(out, clean, sizeof(clean));
    if (clean[0]) sys_to(num >= 400 && num < 600 ? ui_active : 0, clean);
    (void)from;
}

static void handle(char *l)
{
    char *prefix = 0, *cmd, *from = "";
    char *sp;
    unsigned char w;

    ++irc_lines_in;
    if (*l == ':') {
        prefix = l + 1;
        l = strchr(l, ' ');
        if (!l) return;
        *l++ = 0;
        from = prefix;
        sp = strchr(prefix, '!');
        if (sp) *sp = 0;
        for (sp = prefix; *prefix; ++prefix)        /* no control codes or 8-bit bytes in a nick: GEOS's  */
            if (*prefix >= 32 && *prefix < 127)     /* PutString would take them as commands */
                *sp++ = *prefix;
        *sp = 0;
    }
    while (*l == ' ') ++l;
    cmd = l;
    argc_ = 0;
    l = strchr(l, ' ');
    if (l) *l++ = 0;
    while (l && *l && argc_ < sizeof(arg) / sizeof(arg[0]) - 1) {      /* split the parameters */
        while (*l == ' ') ++l;
        if (*l == ':') { arg[argc_++] = l + 1; break; }
        arg[argc_++] = l;
        l = strchr(l, ' ');
        if (l) *l++ = 0;
    }
    arg[argc_] = "";
    if (!argc_) arg[argc_++] = "";

    if (strcmp(cmd, "PING") == 0) {
        fmt(tx, "PONG :%s", arg[argc_ - 1]);
        send_line(tx);
        return;
    }
    if (strcmp(cmd, "PRIVMSG") == 0) { show_privmsg(from, arg[0], arg[1], 0); return; }
    if (strcmp(cmd, "NOTICE") == 0)  { show_privmsg(prefix ? from : "server", arg[0], arg[1], 1); return; }
    if (strcmp(cmd, "JOIN") == 0) {
        w = ui_win_open(arg[0]);
        if (streq_nocase(from, nick)) {
            memset(bloom[w], 0, sizeof(bloom[w]));
            fmt(out, "--> You joined %s", arg[0]);
            ui_add(w, out, 0);
            ui_switch(w);
        } else {
            seen(w, from);
            fmt(out, "--> %s has joined %s", from, arg[0]);
            ui_add(w, out, 0);
        }
        return;
    }
    if (strcmp(cmd, "PART") == 0) {
        sanitize(arg[1], clean, 200);
        fmt(out, "<-- %s has left %s%s%s%s", from, arg[0], clean[0] ? " (" : "", clean, clean[0] ? ")" : "");
        w = ui_win_find(arg[0]);
        if (streq_nocase(from, nick)) {
            ui_add(0, out, 0);
            if (w != 0xFF) ui_win_close(w);
        } else
            ui_add(w == 0xFF ? 0 : w, out, 0);
        return;
    }
    if (strcmp(cmd, "KICK") == 0) {                            /* KICK #chan victim :reason */
        sanitize(arg[2], clean, 200);
        fmt(out, "<-- %s was kicked from %s by %s (%s)", arg[1], arg[0], from, clean);
        w = ui_win_find(arg[0]);
        ui_add(w == 0xFF ? 0 : w, out, streq_nocase(arg[1], nick) ? UI_MENTION : 0);
        return;
    }
    if (strcmp(cmd, "QUIT") == 0) {
        sanitize(arg[0], clean, 200);
        fmt(out, "<-- %s has quit (%s)", from, clean);
        to_seen(from, out);
        return;
    }
    if (strcmp(cmd, "NICK") == 0) {
        fmt(out, "--- %s is now known as %s", from, arg[0]);
        if (streq_nocase(from, nick)) {
            strncpy(nick, arg[0], sizeof(nick) - 1);
            update_status();
            ui_add(ui_active, out, 0);
            return;
        }
        to_seen(from, out);
        for (w = 1; w < MAXWIN; ++w) {
            if (!ui_win[w].used) continue;
            if (streq_nocase(ui_win[w].name, from)) strncpy(ui_win[w].name, arg[0], sizeof(ui_win[w].name) - 1);
            if (bloom_has(w, from)) bloom_add(w, arg[0]);
        }
        { char *e = pool_find(from); if (e) pool_del(e); }
        pool_add(arg[0], 1);
        return;
    }
    if (strcmp(cmd, "TOPIC") == 0) {
        sanitize(arg[1], clean, 300);
        fmt(out, "*** %s changed the topic to: %s", from, clean);
        w = ui_win_find(arg[0]);
        ui_add(w == 0xFF ? 0 : w, out, 0);
        return;
    }
    if (strcmp(cmd, "MODE") == 0) {
        fmt(out, "*** %s sets mode %s %s %s", from, arg[1], argc_ > 2 ? arg[2] : "", argc_ > 3 ? arg[3] : "");
        sanitize(out, clean, 300);
        w = ui_win_find(arg[0]);
        ui_add(w == 0xFF ? 0 : w, clean, 0);
        return;
    }
    if (strcmp(cmd, "INVITE") == 0) {
        fmt(out, "*** %s invites you to %s (/join %s)", from, arg[1], arg[1]);
        sanitize(out, clean, 300);
        ui_add(ui_active, clean, UI_MENTION);
        return;
    }
    if (strcmp(cmd, "ERROR") == 0) {
        sanitize(arg[0], clean, 300);
        sys_to(0, clean);
        return;
    }
    if (isdigit(cmd[0])) { numeric(atoi(cmd), from); return; }
    /* anything else: show it in the server window */
    sanitize(arg[argc_ - 1], clean, 300);
    fmt(out, "*** %s %s", cmd, clean);
    ui_add(0, out, 0);
}

/* ---- connecting ---------------------------------------------------------------------------------------- */

/* set GEOS's clock from the network side (the Ultimate's real-time clock): date in system_date (2-digit year,
 * Hitchhiker's Guide: year/month/day at $8516), time in CIA 1's TOD clock (tod_set) */
static unsigned char num2(const char *p) { return (p[0] - '0') * 10 + (p[1] - '0'); }

void irc_clock(void)
{
    char t[32];
    if (net_time(t) || t[4] != '/' || t[13] != ':') return;
    system_date.s_year = num2(t + 2);
    system_date.s_month = num2(t + 5);
    system_date.s_day = num2(t + 8);
    tod_h = system_date.s_hour = num2(t + 11);
    tod_m = system_date.s_minutes = num2(t + 14);
    tod_s = num2(t + 17);
    tod_set();
}

static void connect_now(void)
{
    irc_clock();
    irc_retry = 0;
    irc_rxlen = 0;
    quiet = 0;
    if (net_probe()) {
        sys_to(0, "No Ultimate network: enable its Command Interface.");
        update_status();
        return;
    }
    fmt(out, "*** Connecting to %s port %u...", server, port);
    ui_add(0, out, 0);
    if (net_connect(server, port)) {
        sys_to(0, net_ip() ? "No network address (cable? Wi-Fi?)." : "Connection failed.");
        if (retries) retry_later();         /* reconnecting by itself: keep trying (cable still out...) */
        update_status();
        return;
    }
    irc_state = 1;
    update_status();
    fmt(tx, "NICK %s", nick);
    send_line(tx);
    fmt(tx, "USER %s 0 * :%s", nick, app_name);
    send_line(tx);
}

void irc_init(void)                         /* HIBSS is not cleared at start-up */
{
    memset(bloom, 0, sizeof(bloom));
    pool[0] = 0;
    pool_len = 0;
}

void irc_start(const char *srv, unsigned prt, const char *nk, const char *channel)
{
    irc_quit("Reconnecting");               /* closes the old socket if we were connected */
    strncpy(server, srv, sizeof(server) - 1);
    port = prt;
    strncpy(nick, nk, sizeof(nick) - 1);
    strncpy(want_chan, channel, sizeof(want_chan) - 1);
    retries = 0;
    connect_now();
}

static void retry_later(void)               /* reconnect after 10, 30, then every 60 s */
{
    static const unsigned char delay[3] = { 1, 3, 6 };
    irc_retry = RECONNECT_TICKS * delay[retries < 2 ? retries : 2];
    if (retries < 255) ++retries;
}

static void lost(void)                      /* the connection dropped without us asking */
{
    irc_state = 0;
    net_close();
    retry_later();
    sys_to(0, "Connection lost. Reconnecting...");
    update_status();
}

void irc_poll(void)
{
    int n;
    char *nl;
    unsigned used;
    if (irc_state == 0) {
        if (irc_retry && --irc_retry == 0) connect_now();
        return;
    }
    n = net_read(rx + irc_rxlen, sizeof(rx) - 1 - irc_rxlen);
    if (n < 0) { lost(); return; }
    if (n) quiet = 0;
    else if (++quiet == KEEPALIVE_TICKS) send_line("PING :geochat128");
    else if (quiet == KEEPALIVE_TICKS + KEEPALIVE_TICKS / 2) { lost(); return; }
    irc_rxlen += n;
    rx[irc_rxlen] = 0;
    for (;;) {
        nl = strchr(rx, '\n');
        if (!nl) {
            if (irc_rxlen >= sizeof(rx) - 1) { handle(rx); irc_rxlen = 0; rx[0] = 0; }   /* overlong line */
            break;
        }
        *nl = 0;
        if (nl > rx && nl[-1] == '\r') nl[-1] = 0;
        used = nl - rx + 1;
        if (rx[0]) handle(rx);
        if (irc_state == 0) return;         /* ERROR / lost while handling */
        memmove(rx, rx + used, irc_rxlen - used + 1);
        irc_rxlen -= used;
    }
}

/* ---- commands ------------------------------------------------------------------------------------------ */

static const char help[] =
    "Commands: /join #chan /part /close /query nick /msg nick text /me action /notice nick text /nick name "
    "/topic /names /whois nick /away /win n /clear /raw line /quit. "
    "Keys: F1/F3 page up/down, ESC newest, F5/F7 windows, cursor up/down history, left/right edit, TAB nicks.";

void irc_input(char *line)
{
    char *a, *text;
    const char *target = ui_win[ui_active].name;
    unsigned char w;
    if (line[0] != '/') {
        if (irc_state != 2) { sys("Not connected: use connect in the menu."); return; }
        if (ui_active == 0) { sys("This is the server window: /join #channel or /query nick"); return; }
        fmt(tx, "PRIVMSG %s :%s", target, line);
        send_line(tx);
        fmt(out, "%c<%s>%c %s", BOLDON, nick, PLAINTEXT, line);
        ui_add(ui_active, out, 0);
        return;
    }
    a = strchr(line, ' ');
    if (a) { *a++ = 0; while (*a == ' ') ++a; } else a = "";
    text = strchr(a, ' ');
    if (text) { *text++ = 0; while (*text == ' ') ++text; } else text = "";
    /* now: line = command, a = first word, text = the rest (a is still NUL-terminated at the first word) */

    if (streq_nocase(line, "/help")) { sys(help); return; }
    if (streq_nocase(line, "/win")) { ui_switch(atoi(a) - 1); return; }
    if (streq_nocase(line, "/clear")) { store_forget(ui_active); ui_redraw(); return; }
    if (streq_nocase(line, "/query")) {
        if (!*a) { sys("Usage: /query nick [text]"); return; }
        ui_switch(w = ui_win_open(a));
        if (*text && irc_state == 2) {
            fmt(tx, "PRIVMSG %s :%s", a, text); send_line(tx);
            fmt(out, "%c<%s>%c %s", BOLDON, nick, PLAINTEXT, text); ui_add(w, out, 0);
        }
        return;
    }
    if (streq_nocase(line, "/close")) {
        if (ui_active == 0) return;
        if (is_chan(target) && irc_state == 2) { fmt(tx, "PART %s", target); send_line(tx); }
        else ui_win_close(ui_active);
        return;
    }
    if (streq_nocase(line, "/quit")) {
        if (*text) a[strlen(a)] = ' ';      /* rejoin "a text" */
        irc_quit(*a ? (const char *)a : app_name);
        return;
    }
    if (irc_state == 0) { sys("Not connected: use connect in the menu."); return; }

    if (streq_nocase(line, "/join")) {
        if (!*a) { sys("Usage: /join #channel"); return; }
        fmt(tx, "JOIN %s%s%s", a, *text ? " " : "", text); send_line(tx);
    } else if (streq_nocase(line, "/part")) {
        if (is_chan(a)) { target = a; a = text; }
        else if (*text) a[strlen(a)] = ' ';
        if (!is_chan(target)) { sys("Not a channel."); return; }
        fmt(tx, "PART %s :%s", target, *a ? (const char *)a : app_name); send_line(tx);
    } else if (streq_nocase(line, "/nick")) {
        fmt(tx, "NICK %s", a); send_line(tx);
        if (irc_state != 2) strncpy(nick, a, sizeof(nick) - 1);
    } else if (streq_nocase(line, "/me")) {
        if (ui_active == 0) { sys("Not in a channel."); return; }
        if (*text) a[strlen(a)] = ' ';
        fmt(tx, "PRIVMSG %s :\001ACTION %s\001", target, a); send_line(tx);
        fmt(out, "* %s %s", nick, a); ui_add(ui_active, out, 0);
    } else if (streq_nocase(line, "/msg") || streq_nocase(line, "/notice")) {
        unsigned char notice = line[1] == 'n' || line[1] == 'N';
        if (!*a || !*text) { sys(notice ? "Usage: /notice nick text" : "Usage: /msg nick text"); return; }
        fmt(tx, "%s %s :%s", notice ? "NOTICE" : "PRIVMSG", a, text); send_line(tx);
        w = ui_win_find(a);
        if (w != 0xFF && !notice) fmt(out, "%c<%s>%c %s", BOLDON, nick, PLAINTEXT, text);
        else { fmt(out, "%c-> %s%s%c %s", REV_ON, notice ? "-" : "*", a, REV_OFF, text); w = ui_active; }
        ui_add(w, out, 0);
    } else if (streq_nocase(line, "/topic")) {
        if (!is_chan(target)) { sys("Not a channel."); return; }
        if (*a) { if (*text) a[strlen(a)] = ' '; fmt(tx, "TOPIC %s :%s", target, a); }
        else fmt(tx, "TOPIC %s", target);
        send_line(tx);
    } else if (streq_nocase(line, "/names")) {
        fmt(tx, "NAMES %s", *a ? (const char *)a : target); send_line(tx);
    } else if (streq_nocase(line, "/whois")) {
        if (!*a) { sys("Usage: /whois nick"); return; }
        fmt(tx, "WHOIS %s", a); send_line(tx);
    } else if (streq_nocase(line, "/away")) {
        if (*text) a[strlen(a)] = ' ';
        if (*a) fmt(tx, "AWAY :%s", a); else strcpy(tx, "AWAY");
        send_line(tx);
    } else if (streq_nocase(line, "/raw") || streq_nocase(line, "/quote")) {
        if (*text) a[strlen(a)] = ' ';
        send_line(a);
    } else {
        sys("Unknown command. Try /help");
    }
}

void irc_quit(const char *reason)
{
    irc_retry = 0;                          /* the user asked: no reconnecting */
    if (irc_state) {
        fmt(tx, "QUIT :%s", reason);
        send_line(tx);
        net_close();
    }
    irc_state = 0;
    update_status();
}
