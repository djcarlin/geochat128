/* ui.c - the 80-column chat screen: windows (one per channel / private chat, plus the server window),
 * message area drawn from the message store with hardware scrolling and scrollback, a status line with the
 * window tabs, and the input line with history and nick completion.
 *
 * We draw only to the foreground screen (dispBufferOn = ST_WR_FORE). The visible text is always re-rendered
 * from the store (store.c), so after a menu or dialog GEOS calls RecoverVector and we just redraw.
 */
#include <string.h>
#include <geos.h>          /* after the C headers: GEOS defines a macro named 'string' */
#include "ui.h"
#include "hw.h"
#include "store.h"
#include "fmt.h"
#include "version.h"
#include "fastline.h"

void (*ui_on_line)(char *line);
const char *(*ui_complete)(const char *prefix, unsigned char nth);
unsigned char ui_redraws;                   /* full redraws done (for the self-test) */
void (*ui_overlay)(void);                   /* set while a page (settings) covers the message area */

#pragma bss-name (push, "HIBSS")     /* $A040-$BF7F, not cleared at start-up (src/geochat128.cfg) */
struct uiwin ui_win[MAXWIN];
#pragma bss-name (pop)
unsigned char ui_active;

/* scrollback of the active window */
spos ui_view = SPOS_NONE;                   /* newest record shown; SPOS_NONE = live (following new messages) */
spos ui_top;                                /* record drawn at the top after the last render */
static unsigned char top_partial;
#pragma bss-name (push, "HIBSS")
static spos pstack[24];                     /* views before each page up, for page down (psp counts them) */
#pragma bss-name (pop)
static unsigned char psp;
unsigned char ui_more;                      /* new messages arrived below while scrolled back */

#pragma bss-name (push, "HIBSS")     /* $A040-$BF7F, not cleared at start-up (src/geochat128.cfg) */
static char status[48];
static char input[INPUT_MAX + 1];
#pragma bss-name (pop)
static unsigned char inlen, incur;          /* length of the input line, cursor position in it */
#pragma bss-name (push, "HIBSS")     /* $A040-$BF7F, not cleared at start-up (src/geochat128.cfg) */
static char line[STORE_TEXT_MAX + 1];       /* a message being added (ui_add), or a record being drawn
                                               (render): one buffer, since neither calls the other */
#define sbuf line
static char hist[256];                      /* input history: NUL-separated lines, oldest first */
#pragma bss-name (pop)
unsigned ui_tab_x[MAXWIN], ui_tab_r[MAXWIN];   /* tab positions on the status line, for clicks */

static unsigned hlen;                       /* bytes used in hist */
static unsigned char hpos;                  /* 0 = not browsing; n = n-th newest */

/* nick completion state */
static unsigned char cnth, cstart;
static char cprefix[17];

#define BASELINE 6                          /* BSW 9: baseline 6 pixels below the top of a 9-row cell (rows base-6..base+2); PutString clears the whole cell, so keep cells off the divider lines */

void ui_clear(unsigned char top, unsigned char bot, unsigned left, unsigned right)
{
    struct window w;
    w.top = top; w.bot = bot; w.left = left; w.right = right;
    SetPattern(0);
    InitDrawWindow(&w);
    Rectangle();
}

#ifdef SELFTEST
unsigned char ui_beeps;                     /* the self-test counts the dings */
unsigned ui_status_w;                       /* width of the status line's text in pixels */
unsigned char ui_slow;                      /* the self-test draws with PutString to compare */
#else
#define ui_slow 0
#endif

/* width of a printable character (32..126) */
static unsigned char chw(char c)
{
    if (c < 32 || c > 126) c = '?';         /* as fastdraw.s draws them */
    return fl_ok ? fl_w[c - 32] : GetCharWidth(c);
}

void ui_draw_row(unsigned char row, const char *s)
{
    unsigned char top = MSG_TOP + row * LINE_H;
    if (fl_ok && !ui_slow) {
        fl_top = top;
        fl_draw(s);
        return;
    }
    ui_clear(top, top + LINE_H - 1, 0, 639);
    if (*s)
        PutString((char *)s, top + BASELINE, TEXT_X);
}

/* the status and input lines: a divider at y, the text in the 9 rows below it */
static void text_line(unsigned char y, const char *s)
{
    HorizontalLine(0xff, y, 0, 639);
    if (fl_ok && !ui_slow) {
        fl_top = y + 1;
        fl_draw(s);
        return;
    }
    ui_clear(y + 1, y + 9, 0, 639);
    PutString((char *)s, y + BASELINE + 1, TEXT_X);
}

/* ---- status line: "nick @ server  1:IRC 2:#chan* 3:bob!" ---------------------------------------------- */
static unsigned char safe(char c)
{
    return c >= 32 && c < 127;
}

static char *sp;                            /* the status / input line being built, and its width */
static unsigned sx;

static void add(char c)
{
    *sp++ = c;
    sx += chw(c);
}

static void adds(const char *p, unsigned char max)
{
    while (*p && max--) add(*p++);
}

static void draw_status(void)
{
    char s[160];
    unsigned char i, used = 0, maxlen;
    const char *p = status;
    /* all 8 tabs must fit in 640 pixels: names get shorter as windows open (12, 8, then 5 characters);
     * while scrolled back the note takes the place of "nick @ server", so it is never pushed off the screen */
    for (i = 0; i < MAXWIN; ++i) used += ui_win[i].used;
    maxlen = used > 6 ? 5 : used > 4 ? 8 : 12;
    if (ui_view != SPOS_NONE) p = ui_more ? "[new below: ESC]" : "[scrolled back: ESC]";
    sp = s; sx = TEXT_X;
    adds(p, 255); add(' '); add(' ');
    for (i = 0; i < MAXWIN; ++i) {
        ui_tab_x[i] = ui_tab_r[i] = 0;
        if (!ui_win[i].used) continue;
        if (sp - s > 140) break;
        ui_tab_x[i] = sx;
        if (i == ui_active) *sp++ = REV_ON;
        add('1' + i); add(':');
        adds(ui_win[i].name, maxlen);
        if (ui_win[i].mention) add('!');
        else if (ui_win[i].unread) add('*');
        if (i == ui_active) *sp++ = REV_OFF;
        ui_tab_r[i] = sx;
        add(' ');
    }
    *sp = 0;
#ifdef SELFTEST
    ui_status_w = sx;                       /* the self-test checks it all fits on the screen */
#endif
    text_line(STATUS_Y, s);
}

void ui_status(const char *text)
{
    unsigned char n = 0;
    char c;
    while ((c = *text++) != 0 && n < sizeof(status) - 1)
        if (safe(c)) status[n++] = c;       /* no GEOS control codes in the status line */
    status[n] = 0;
    draw_status();
}

/* input line: "> " + as much as fits up to the cursor, the character under the cursor in reverse ("_" at the
 * end of the line), then what follows (cut at the screen edge) */
static void draw_input(void)
{
    unsigned w = 0;
    unsigned char start = incur, i;
    char vis[INPUT_MAX + 6];
    while (start > 0) {
        w += chw(input[start - 1]);
        if (w > TEXT_W - 30) break;
        --start;
    }
    sp = vis; add('>'); add(' ');
    for (i = start; i < inlen; ++i) {
        if (i == incur) *sp++ = REV_ON;
        *sp++ = input[i];
        if (i == incur) *sp++ = REV_OFF;
    }
    if (incur == inlen) *sp++ = '_';
    *sp = 0;
    text_line(INPUT_Y, vis);
}

/* ---- word wrap ---------------------------------------------------------------------------------------- */
static unsigned char wmode;                 /* 0 count lines, 1 draw at wrow, 2 scroll in at the bottom */
static signed char wrow;
static unsigned char wcount;

static void emit(const char *s)
{
    if (wmode == 0) { ++wcount; return; }
    if (wmode == 1) {
        if (wrow >= 0 && wrow < MSG_LINES) ui_draw_row(wrow, s);
        ++wrow;
        return;
    }
    scr_top = MSG_TOP;
    scr_bot = MSG_TOP + MSG_LINES * LINE_H - 1;
    scr_amt = LINE_H;
    vdc_scroll();
    ui_draw_row(MSG_LINES - 1, s);
}

static unsigned char is_style(char c)
{
    return c == BOLDON || c == PLAINTEXT || c == REV_ON || c == REV_OFF;
}

/* Wrap 'text' into display lines no wider than TEXT_W, breaking at the last space where possible.
 * Continuation lines are indented and re-open the style that was active at the break. */
static void wrap(const char *text)
{
    const char *p = text;
    char out[LINE_MAX];
    unsigned char o, bold, rev, first = 1;
    unsigned char sbold = 0, srev = 0;          /* style carried into the next line */
    unsigned w, cw;
    const char *brk_p;
    unsigned char brk_o, brk_bold, brk_rev;
    char c;

    while (*p || first) {
        o = 0; w = 0;
        bold = sbold; rev = srev;
        if (!first) { memcpy(out, "    ", 4); o = 4; w = 4 * chw(' '); }
        if (bold) out[o++] = BOLDON;
        if (rev) out[o++] = REV_ON;
        brk_p = 0; brk_o = 0; brk_bold = bold; brk_rev = rev;
        while ((c = *p) != 0) {
            if (c < 32 && is_style(c)) {
                out[o++] = c;
                if (c == BOLDON) bold = 1;
                else if (c == PLAINTEXT) bold = rev = 0;
                else if (c == REV_ON) rev = 1;
                else rev = 0;
                ++p;
                continue;
            }
            if (c < 32) { ++p; continue; }      /* other control codes are GEOS commands (PutString): drop */
            if (c >= 127) c = '?';               /* outside the system font */
            cw = (fl_ok ? fl_w[c - 32] : GetCharWidth(c)) + bold;    /* inline: this loop runs per character */
            if (w + cw > TEXT_W || o >= LINE_MAX - 4) break;
            if (c == ' ') { brk_p = p; brk_o = o; brk_bold = bold; brk_rev = rev; }
            out[o++] = c;
            w += cw;
            ++p;
        }
        if (*p && brk_p) {                      /* break at the last space */
            o = brk_o; p = brk_p + 1; bold = brk_bold; rev = brk_rev;
        }
        out[o] = 0;
        emit(out);
        sbold = bold; srev = rev;
        first = 0;
    }
}

/* ---- window cache in a 64 KB VDC's spare RAM -------------------------------------------------------------
 * GEOS's screen uses VDC $0000-$3E7F; with 64 KB, $4000/$8000/$C000 each hold one window's message area
 * (MSG_LINES*LINE_H*80 = 12,960 bytes). Leaving a window that shows its newest messages copies its message
 * area there (VDC block copy); coming back copies it in instead of drawing it. A slot is dropped when it is used,
 * when a message arrives in its window, and when the window closes, so it never shows anything stale. */
#define VC_SLOTS 3
#define MSG_ADDR (MSG_TOP * 80)
#define MSG_BYTES (MSG_LINES * LINE_H * 80)
unsigned char ui_vcache;                    /* slots: 3 with a 64 KB VDC, else 0 */
static unsigned char vc_win[VC_SLOTS], vc_age[VC_SLOTS], vc_part[VC_SLOTS], vc_clock;
static spos vc_top[VC_SLOTS];

static void vc_forget(unsigned char w)
{
    unsigned char i;
    for (i = 0; i < VC_SLOTS; ++i)
        if (vc_win[i] == w) vc_win[i] = 0xFF;
}

static void vc_save(void)
{
    unsigned char i, s = 0;
    if (!ui_vcache || ui_view != SPOS_NONE || ui_overlay) return;
    for (i = 0; i < VC_SLOTS; ++i) {        /* a free slot, else the least recently saved one */
        if (vc_win[i] == 0xFF) { s = i; break; }
        if ((unsigned char)(vc_age[i] - vc_age[s]) & 0x80) s = i;
    }
    vc_src = MSG_ADDR; vc_dst = (s + 1) << 14; vc_len = MSG_BYTES;
    vc_copy();
    vc_win[s] = ui_active; vc_age[s] = ++vc_clock;
    vc_top[s] = ui_top; vc_part[s] = top_partial;
}

#ifdef SELFTEST
unsigned char ui_wrap_rows(const char *t)   /* the self-test times word wrap */
{
    wmode = 0; wcount = 0; wrap(t);
    return wcount;
}
#endif

/* ---- drawing the message area from the store ---------------------------------------------------------- */
static unsigned char measuring;             /* render only works out ui_top (nothing is drawn) */

static void render(void)
{
    spos p = ui_view == SPOS_NONE ? store_last(ui_active) : ui_view;
    signed char bottom = MSG_LINES;
    ui_top = SPOS_NONE; top_partial = 0;
    while (p != SPOS_NONE && bottom > 0) {
        store_get(p, sbuf, sizeof(sbuf));
        wmode = 0; wcount = 0; wrap(sbuf);
        if (!measuring) { wmode = 1; wrow = bottom - wcount; wrap(sbuf); }
        ui_top = p;
        top_partial = bottom < wcount;
        bottom -= wcount;
        p = store_prev(p);
    }
    if (fl_ok && !ui_slow && !measuring)    /* ui_redraw did not clear the message area: blank the rest */
        while (bottom > 0) ui_draw_row(--bottom, "");
}

void ui_redraw(void)
{
    ++ui_redraws;
    UseSystemFont();
    if (fl_ok && !ui_slow && !ui_overlay) {     /* every message row is rewritten whole: clear only the rest */
        ui_clear(MSG_TOP - 1, MSG_TOP - 1, 0, 639);
        ui_clear(MSG_TOP + MSG_LINES * LINE_H, STATUS_Y - 1, 0, 639);
    } else
        ui_clear(MSG_TOP - 1, STATUS_Y - 1, 0, 639);
    if (ui_overlay) ui_overlay();
    else render();
    draw_status();
    draw_input();
}

void ui_init(void)
{
    dispBufferOn = ST_WR_FORE;
    UseSystemFont();
    ui_vcache = vc_detect() ? VC_SLOTS : 0;     /* before anything is drawn: it writes markers */
    memset(vc_win, 0, sizeof(vc_win));      /* slot = window 0... */
    vc_forget(0);                           /* ...then all free (never a non-zero memset: settings.c, settings_save) */
    fl_ok = fl_init();
    incur = inlen = 0; input[0] = 0;
    memset(ui_win, 0, sizeof(ui_win));
    ui_win[0].used = 1;
    strcpy(ui_win[0].name, "IRC");
    ui_active = 0;
    strcpy(status, app_name);
    ui_clear(0, 199, 0, 639);
    ui_redraw();
}

/* ---- scrollback --------------------------------------------------------------------------------------- */
void ui_page_up(void)
{
    spos v;
    if (ui_view == SPOS_NONE) {             /* live: lines scrolled in since the last full redraw moved the */
        measuring = 1;                      /* top of the screen, so work it out again */
        render();
        measuring = 0;
    }
    if (ui_top == SPOS_NONE) return;
    v = top_partial ? ui_top : store_prev(ui_top);
    if (v == ui_view) v = store_prev(v);    /* a single message taller than the screen */
    if (v == SPOS_NONE) return;             /* already at the oldest message */
    if (psp == sizeof(pstack) / sizeof(pstack[0])) {    /* history full: forget the oldest step */
        memmove(pstack, pstack + 1, sizeof(pstack) - sizeof(pstack[0]));
        --psp;
    }
    pstack[psp++] = ui_view;
    ui_view = v;
    ui_redraw();
}

void ui_page_down(void)
{
    if (ui_view == SPOS_NONE) return;
    ui_view = psp ? pstack[--psp] : SPOS_NONE;
    if (ui_view == SPOS_NONE) { psp = 0; ui_more = 0; }
    ui_redraw();
}

void ui_live(void)
{
    if (ui_view == SPOS_NONE) return;
    ui_view = SPOS_NONE; psp = 0; ui_more = 0;
    ui_redraw();
}

/* ---- windows ------------------------------------------------------------------------------------------ */
unsigned char ui_win_find(const char *name)
{
    unsigned char i;
    for (i = 0; i < MAXWIN; ++i)
        if (ui_win[i].used && streq_nocase(ui_win[i].name, name)) return i;
    return 0xFF;
}

unsigned char ui_win_open(const char *name)
{
    unsigned char i = ui_win_find(name);
    if (i != 0xFF) return i;
    for (i = 1; i < MAXWIN; ++i)
        if (!ui_win[i].used) {
            memset(&ui_win[i], 0, sizeof(ui_win[i]));
            ui_win[i].used = 1;
            strncpy(ui_win[i].name, name, sizeof(ui_win[i].name) - 1);
            store_forget(i);
            draw_status();
            return i;
        }
    return 0;                               /* all in use: the server window */
}

void ui_switch(unsigned char w)
{
    unsigned char i;
    if (w >= MAXWIN || !ui_win[w].used) return;
    vc_save();
    ui_active = w;
    ui_win[w].unread = ui_win[w].mention = 0;
    ui_view = SPOS_NONE; psp = 0; ui_more = 0;
    for (i = 0; i < VC_SLOTS; ++i)
        if (ui_vcache && vc_win[i] == w) {
            vc_src = (i + 1) << 14; vc_dst = MSG_ADDR; vc_len = MSG_BYTES;
            vc_copy();
            vc_win[i] = 0xFF;
            ui_top = vc_top[i]; top_partial = vc_part[i];
            ++ui_redraws;
            draw_status();
            draw_input();
            return;
        }
    ui_redraw();
}

void ui_win_close(unsigned char w)
{
    if (w == 0 || w >= MAXWIN) return;
    ui_win[w].used = 0;
    vc_forget(w);
    store_forget(w);
    if (ui_active == w) ui_switch(0);
    else draw_status();
}

void ui_next(signed char d)
{
    unsigned char i, w = ui_active;
    for (i = 0; i < MAXWIN; ++i) {
        w = (w + MAXWIN + d) % MAXWIN;
        if (ui_win[w].used) { ui_switch(w); return; }
    }
}

/* ---- adding messages ---------------------------------------------------------------------------------- */
void ui_add(unsigned char w, const char *text, unsigned char flags)
{
    unsigned char h = system_date.s_hour, m = system_date.s_minutes;
    if (flags & UI_MENTION) {               /* someone mentioned you, or a private message: ding */
        beep();
#ifdef SELFTEST
        ++ui_beeps;
#endif
    }
    if (w >= MAXWIN || !ui_win[w].used) w = 0;
    line[0] = '0' + h / 10; line[1] = '0' + h % 10; line[2] = ':';
    line[3] = '0' + m / 10; line[4] = '0' + m % 10; line[5] = ' ';
    strncpy(line + 6, text, sizeof(line) - 7);
    line[sizeof(line) - 1] = 0;
    store_add(w, line);
    if (w == ui_active) {
        if (ui_overlay) return;             /* shown when the page closes */
        if (ui_view == SPOS_NONE) { wmode = 2; wrap(line); }
        else if (!ui_more) { ui_more = 1; draw_status(); }
    } else {
        unsigned char was;
        vc_forget(w);                       /* its cached screen is out of date */
        was = ui_win[w].unread | (ui_win[w].mention << 1);
        ui_win[w].unread = 1;
        if (flags & UI_MENTION) ui_win[w].mention = 1;
        if (was != (ui_win[w].unread | (ui_win[w].mention << 1))) draw_status();
    }
}

void ui_msg(const char *text)               /* to the active window */
{
    ui_add(ui_active, text, 0);
}

/* ---- mouse: clicks on the window tabs (called through otherPressVec) ----------------------------------- */
void ui_click(void)
{
    unsigned x = mousePos.x;
    unsigned char y = mousePos.y, i;
    if (mouseData & 0x80) return;           /* button released */
    if (y < STATUS_Y || y > STATUS_Y + 9) return;
    for (i = 0; i < MAXWIN; ++i)
        if (ui_tab_r[i] && x >= ui_tab_x[i] && x < ui_tab_r[i]) { ui_switch(i); return; }
}

/* ---- keyboard ----------------------------------------------------------------------------------------- */
static void hist_add(const char *s)
{
    unsigned n = strlen(s) + 1;
    if (n > sizeof(hist)) return;
    while (hlen + n > sizeof(hist)) {       /* drop the oldest lines */
        unsigned k = strlen(hist) + 1;
        memmove(hist, hist + k, hlen - k);
        hlen -= k;
    }
    memcpy(hist + hlen, s, n);
    hlen += n;
}

static const char *hist_get(unsigned char nth)   /* 1 = newest */
{
    unsigned i = hlen;
    unsigned char k = 0;
    while (i) {
        unsigned j = i - 1;                 /* hist[j] is the NUL of a line */
        while (j && hist[j - 1]) --j;
        if (++k == nth) return hist + j;
        i = j;
    }
    return 0;
}

static void set_input(const char *s)
{
    strncpy(input, s, INPUT_MAX);
    input[INPUT_MAX] = 0;
    incur = inlen = strlen(input);
    draw_input();
}

const char *ui_input(void) { return input; }

static void complete(void)
{
    const char *m;
    unsigned char n;
    if (!ui_complete || incur != inlen) return;    /* completes the word at the end of the line */
    if (!cnth) {                            /* first TAB: remember the word being completed */
        cstart = inlen;
        while (cstart && input[cstart - 1] != ' ') --cstart;
        n = inlen - cstart;
        if (!n || n > 16) return;
        memcpy(cprefix, input + cstart, n); cprefix[n] = 0;
    }
    m = ui_complete(cprefix, cnth);
    if (!m) { cnth = 0; m = ui_complete(cprefix, 0); if (!m) return; }   /* wrap around */
    ++cnth;
    input[cstart] = 0;
    strncat(input, m, INPUT_MAX - cstart - 2);
    strcat(input, cstart ? " " : ": ");
    incur = inlen = strlen(input);
    draw_input();
}

void ui_key(char c)
{
    char l[INPUT_MAX + 1];
    if (c != KEY_TAB) cnth = 0;
    switch (c) {
    case KEY_ENTER:
        if (inlen == 0) return;
        strcpy(l, input);
        hist_add(l);
        hpos = 0;
        incur = inlen = 0; input[0] = 0;
        draw_input();
        if (ui_on_line) ui_on_line(l);
        return;
    case KEY_DELETE:                        /* DEL: the character before the cursor (GEOS KbdDecodeTab1: DEL = 29, */
        if (incur) {                        /* the cursor keys: left = 8 (BACKSPACE), right = 30) */
            memmove(input + incur - 1, input + incur, inlen - incur + 1);
            --inlen; --incur; draw_input();
        }
        return;
    case 8:                                 /* cursor left */
        if (incur) { --incur; draw_input(); }
        return;
    case KEY_RIGHT:
        if (incur < inlen) { ++incur; draw_input(); }
        return;
    case KEY_CLEAR:
        incur = inlen = 0; input[0] = 0; hpos = 0; draw_input();
        return;
    case KEY_UP:                            /* history: older */
        if (hist_get(hpos + 1)) set_input(hist_get(++hpos));
        return;
    case KEY_DOWN:                          /* history: newer, then empty */
        if (hpos > 1) set_input(hist_get(--hpos));
        else if (hpos == 1) { hpos = 0; set_input(""); }
        return;
    case KEY_TAB: complete(); return;
    case KEY_F1: ui_page_up(); return;
    case KEY_F3: ui_page_down(); return;
    case KEY_ESC: ui_live(); return;
    case KEY_F5: ui_next(-1); return;
    case KEY_F7: ui_next(1); return;
    }
    if (c >= 32 && c < 127 && inlen < INPUT_MAX) {     /* typed at the cursor */
        memmove(input + incur + 1, input + incur, inlen - incur + 1);
        input[incur++] = c; ++inlen;
        draw_input();
    }
}
