/* settings.c - the settings page: nickname, channel, server and port on one screen, with OK and Cancel.
 *
 * Drawn over the message area by the app itself (not a GEOS dialog: a dialog holds only one text field, and
 * GEOS pauses processes while one is open, so IRC traffic would stall). Incoming messages are stored while
 * the page is open and shown when it closes (ui_overlay in ui.c).
 *
 * OK also saves the settings to the file CFG_NAME on the current disk; settings_load reads it at start-up.
 *
 * Keys: type to edit the focused field, DEL deletes, RETURN or cursor-down = next field (RETURN on the last
 * field = OK), cursor-up = previous field. Mouse: click a field to edit it, click OK or Cancel.
 * Mouse clicks outside menus and icons come through otherPressVec (main.c passes them here); bit 7 of
 * mouseData is 1 when the button is up, so releases are ignored.
 */
#include <string.h>
#include <geos.h>
#include "ui.h"
#include "fmt.h"
#include "version.h"
#include "settings.h"
#include "hw.h"

#ifndef DEF_SERVER
#define DEF_SERVER  "irc.libera.chat"
#endif
#ifndef DEF_PORT
#define DEF_PORT    6667
#endif
#ifndef DEF_NICK
#define DEF_NICK    ""                  /* none: each user sets theirs */
#endif
#ifndef DEF_CHAN
#define DEF_CHAN    "#c64friends"
#endif

char set_server[SET_SERVER_MAX + 1] = DEF_SERVER;
char set_port[SET_PORT_MAX + 1];
char set_nick[SET_NICK_MAX + 1] = DEF_NICK;
char set_chan[SET_CHAN_MAX + 1] = DEF_CHAN;

#pragma bss-name (push, "HIBSS")     /* $A040-$BF7F, not cleared at start-up (src/geochat128.cfg) */
static char e_nick[SET_NICK_MAX + 1], e_chan[SET_CHAN_MAX + 1];
static char e_server[SET_SERVER_MAX + 1], e_port[SET_PORT_MAX + 1];
#pragma bss-name (pop)

static const char * const labels[NFIELDS] = { "Nickname", "Channel", "Server", "Port" };
static char * const bufs[NFIELDS] = { e_nick, e_chan, e_server, e_port };
static const unsigned char maxlen[NFIELDS] = { SET_NICK_MAX, SET_CHAN_MAX, SET_SERVER_MAX, SET_PORT_MAX };
static unsigned char focus;

unsigned parse_port(const char *s)
{
    unsigned p = 0;
    while (*s >= '0' && *s <= '9') p = p * 10 + (*s++ - '0');
    return p ? p : 6667;
}

static void frame(unsigned char t, unsigned char b, unsigned l, unsigned r)
{
    struct window w;
    w.top = t; w.bot = b; w.left = l; w.right = r;
    InitDrawWindow(&w);
    FrameRectangle(0xff);
}

static void draw_field(unsigned char i)
{
    unsigned char t = FIELD_Y0 + i * FIELD_DY;
    char vis[SET_SERVER_MAX + 3];
    ui_clear(t, t + FIELD_H - 1, LABEL_X, BOX_R);
    PutString((char *)labels[i], t + 10, LABEL_X);
    frame(t, t + FIELD_H - 1, BOX_L, BOX_R);
    if (i == focus) frame(t + 1, t + FIELD_H - 2, BOX_L + 1, BOX_R - 1);   /* focused: double frame */
    strcpy(vis, bufs[i]);
    if (i == focus) strcat(vis, "_");
    PutString(vis, t + 10, BOX_L + 6);
}

static void button(unsigned l, unsigned r, const char *text, unsigned tx)
{
    frame(BTN_T, BTN_B, l, r);
    frame(BTN_T + 1, BTN_B - 1, l + 1, r - 1);
    PutString((char *)text, BTN_T + 11, tx);
}

static void draw_page(void)                 /* ui_overlay: called by ui_redraw */
{
    unsigned char i;
    PutString(CBOLDON "Settings" CPLAINTEXT, TITLE_Y, LABEL_X);
    for (i = 0; i < NFIELDS; ++i) draw_field(i);
    PutString("Click a field or press RETURN. OK saves to this disk.", HINT_Y, LABEL_X);
    button(OK_L, OK_R, CBOLDON "OK" CPLAINTEXT, OK_L + 32);
    button(CAN_L, CAN_R, "Cancel", CAN_L + 22);
}

static void set_focus(unsigned char f)
{
    unsigned char old = focus;
    focus = f;
    draw_field(old);
    draw_field(f);
}

static void close_page(void)
{
    ui_overlay = 0;
    ui_redraw();
}

/* ---- settings file ------------------------------------------------------------------------------------
 * A GEOS sequential DATA file "geoChat 128 cfg" on the disk geoChat 128 was started from, holding struct cfg.
 * Written with SaveFile: the header's first word points to the file name, start/end address give the memory
 * to save, and SaveFile fills in the rest (Hitchhiker's Guide to GEOS, SaveFile). Read back with FindFile +
 * ReadFile from the first data block (dirEntryBuf). */
struct cfg {
    char magic[4];                          /* "I12" + format version */
    char nick[SET_NICK_MAX + 1];
    char chan[SET_CHAN_MAX + 1];
    char server[SET_SERVER_MAX + 1];
    char port[SET_PORT_MAX + 1];
};
#pragma bss-name (push, "HIBSS")     /* $A040-$BF7F, not cleared at start-up (src/geochat128.cfg) */
static struct cfg cfg;
static char cfg_rd[sizeof(struct cfg) + 2];
static const char cfg_magic[4] = { 'I', '1', '2', 1 };
static const char cfg_name[] = CFG_NAME;
static struct fileheader hdr;
#pragma bss-name (pop)

static void copy_in(char *dst, const char *src, unsigned char max)
{
    strncpy(dst, src, max);
    dst[max] = 0;
}

unsigned char settings_load(void)
{
    struct cfg *c = (struct cfg *)cfg_rd;
    if (FindFile(cfg_name)) return 1;
    memset(cfg_rd, 0, sizeof(cfg_rd));
    if (ReadFile(&dirEntryBuf.n_block, cfg_rd, sizeof(cfg_rd))) return 2;
    if (memcmp(c->magic, cfg_magic, 4)) return 3;
    copy_in(set_nick, c->nick, SET_NICK_MAX);
    copy_in(set_chan, c->chan, SET_CHAN_MAX);
    copy_in(set_server, c->server, SET_SERVER_MAX);
    copy_in(set_port, c->port, SET_PORT_MAX);
    return 0;
}

unsigned char settings_save(void)
{
    unsigned char e;
    memcpy(cfg.magic, cfg_magic, 4);
    strcpy(cfg.nick, set_nick);
    strcpy(cfg.chan, set_chan);
    strcpy(cfg.server, set_server);
    strcpy(cfg.port, set_port);
    DeleteFile(cfg_name);                   /* replace the old one (FILE_NOT_FOUND is fine) */
    memset(&hdr, 0, sizeof(hdr));
    *(const char **)&hdr.n_block = cfg_name;            /* SaveFile: word 0 = pointer to the file name */
    hdr.icon_desc[0] = 3; hdr.icon_desc[1] = 21; hdr.icon_desc[2] = 0x80 | 63;   /* 24x21 icon, 63 bytes */
    /* a plain box. Not memset(..., 0xFF, ...): cc65 2.19's GEOS library makes memset an alias of FillRam, which
     * takes 'what' as a char while memset passes an int - every non-zero memset fills a wrong address and leaves
     * a byte on the C stack (zero is safe: the compiler turns memset(p, 0, n) into _bzero). */
    for (e = 0; e < 3; ++e) hdr.icon_pic[e] = hdr.icon_pic[60 + e] = 0xFF;
    for (e = 3; e < 60; e += 3) { hdr.icon_pic[e] = 0x80; hdr.icon_pic[e + 2] = 0x01; }
    hdr.dostype = 0x80 | USR;               /* closed USR file, like every GEOS file */
    hdr.type = DATA;
    hdr.structure = SEQUENTIAL;
    hdr.load_address = (unsigned)&cfg;
    hdr.end_address = (unsigned)&cfg + sizeof(cfg);
    strcpy(hdr.class_name, "geoChat cfg V1.0");   /* 12-character class + version */
    strcpy(hdr.author, app_name);
    return SaveFile(0, &hdr);
}

static unsigned char save_pending;

static void ok(void)
{
    strcpy(set_nick, e_nick);
    strcpy(set_chan, e_chan);
    strcpy(set_server, e_server);
    strcpy(set_port, e_port[0] ? e_port : "6667");
    close_page();
    save_pending = 1;                       /* written from the GEOS process, not from inside the mouse handler */
}

/* Called from the GEOS process: the disk work happens outside GEOS's mouse handling, once the button is up. */
void settings_poll(void)
{
    unsigned char e;
    char msg[60];
    if (!save_pending) return;
    if (!(mouseData & 0x80)) return;        /* wait until the mouse button is up */
    save_pending = 0;
    e = settings_save();
    if (e) { fmt(msg, "*** Settings changed, but not saved: disk error %u.", (unsigned)e); ui_msg(msg); }
    else ui_msg("*** Settings saved. Use connect to apply them.");
}

void settings_click(void)                   /* mouse press while the page is open (main.c dispatches) */
{
    unsigned x = mousePos.x;
    unsigned char y = mousePos.y, i, t;
    if (mouseData & 0x80) return;           /* button released */
    if (y >= BTN_T && y <= BTN_B) {
        if (x >= OK_L && x <= OK_R) { ok(); return; }
        if (x >= CAN_L && x <= CAN_R) { close_page(); return; }
    }
    for (i = 0; i < NFIELDS; ++i) {
        t = FIELD_Y0 + i * FIELD_DY;
        if (y >= t && y < t + FIELD_H && x >= LABEL_X && x <= BOX_R) { set_focus(i); return; }
    }
}

void settings_open(void)
{
    if (ui_overlay) return;
    if (!set_port[0]) fmt(set_port, "%u", (unsigned)DEF_PORT);
    strcpy(e_nick, set_nick);
    strcpy(e_chan, set_chan);
    strcpy(e_server, set_server);
    strcpy(e_port, set_port);
    focus = 0;
    ui_overlay = draw_page;
    ui_redraw();
}

void settings_key(char c)
{
    char *b = bufs[focus];
    unsigned char n = strlen(b);
    if (c == KEY_ENTER) {
        if (focus == NFIELDS - 1) ok();
        else set_focus(focus + 1);
    } else if (c == KEY_DOWN) {
        set_focus(focus == NFIELDS - 1 ? 0 : focus + 1);
    } else if (c == KEY_UP) {
        set_focus(focus ? focus - 1 : NFIELDS - 1);
    } else if (c == KEY_DELETE || c == 8) {
        if (n) { b[n - 1] = 0; draw_field(focus); }
    } else if (c == KEY_CLEAR) {
        b[0] = 0; draw_field(focus);
    } else if (c > 32 && c < 127 && n < maxlen[focus]) {   /* no spaces: none of these fields has one */
        if (focus == 3 && (c < '0' || c > '9')) return;
        b[n] = c; b[n + 1] = 0;
        draw_field(focus);
    }
}
