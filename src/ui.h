/* ui.h - the 80-column chat screen */
#ifndef UI_H
#define UI_H

#include "store.h"

/* Screen layout (GEOS 128, 640x200, 80-column VDC) */
#define MENU_BOT    14          /* GEOS menu bar: 0..14 */
#define MSG_TOP     16          /* message area */
#define LINE_H      9           /* system font (BSW 9) line height */
#define MSG_LINES   18          /* 16 + 18*9 = 178 */
#define STATUS_Y    180         /* status line 180..189 */
#define INPUT_Y     190         /* input line 190..199 */
#define TEXT_X      4
#define TEXT_W      632         /* usable width in pixels */

#define LINE_MAX    140         /* bytes per display line, incl. style codes */
#define INPUT_MAX   200         /* characters in the input line */
#define KEY_TAB     9           /* the C128's TAB key (not in cc65's GEOS key list) */

struct uiwin {
    char name[32];              /* "IRC" (window 0), "#channel" or a nick */
    unsigned char used, unread, mention;
};
extern struct uiwin ui_win[MAXWIN];
extern unsigned char ui_active;
extern spos ui_view, ui_top;    /* scrollback: newest shown record (SPOS_NONE = live), record at the top */
extern unsigned char ui_more;   /* new messages below while scrolled back */
extern unsigned ui_tab_x[MAXWIN], ui_tab_r[MAXWIN];

#define UI_MENTION  1

void ui_init(void);
void ui_draw_row(unsigned char row, const char *s);    /* one message-area row (0..MSG_LINES-1), replaced whole */
#ifdef SELFTEST
extern unsigned char ui_slow;
unsigned char ui_wrap_rows(const char *t);               /* 1: draw rows with GEOS PutString (the self-test compares) */
#endif
void ui_redraw(void);                       /* full redraw (after menus/dialogs, window switch) */
void ui_add(unsigned char w, const char *text, unsigned char flags);   /* store + show; text may have style codes */
void ui_msg(const char *text);              /* ui_add to the active window */
void ui_status(const char *text);           /* left part of the status line */
void ui_key(char c);                        /* feed one key (keyVector handler, and the self-test) */
void ui_click(void);                        /* mouse press outside menus (otherPressVec) */
const char *ui_input(void);                 /* current input line */

unsigned char ui_win_find(const char *name);    /* window index or 0xFF */
unsigned char ui_win_open(const char *name);    /* find or create; 0 (server window) when all are in use */
void ui_win_close(unsigned char w);
void ui_switch(unsigned char w);
void ui_next(signed char d);                /* next (+1) / previous (-1) window */
void ui_page_up(void);
void ui_page_down(void);
void ui_live(void);                         /* back to the newest messages */

extern void (*ui_on_line)(char *line);      /* called when RETURN is pressed on a non-empty input line */
extern const char *(*ui_complete)(const char *prefix, unsigned char nth);   /* nick completion source */
extern unsigned char ui_redraws;
extern unsigned char ui_vcache;             /* window cache slots in the VDC (3 with 64 KB, else 0) */
extern void (*ui_overlay)(void);            /* draws a page over the message area; messages are stored meanwhile */
void ui_clear(unsigned char top, unsigned char bot, unsigned left, unsigned right);

#endif
