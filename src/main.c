/* main.c - geoChat 128: an IRC client for GEOS 128 in 80-column mode.
 *
 * Build with -DSELFTEST to run a scripted session against net_fake.c and report PASS/FAIL through the VICE
 * debug cartridge ($D7FF), so the whole client can be tested headless in x128.
 */
#include <string.h>
#include <geos.h>          /* after the C headers: GEOS defines a macro named 'string' */
#include "ui.h"
#include "version.h"
#include "fastline.h"
#include "irc.h"
#include "hw.h"
#include "fmt.h"
#include "settings.h"
#include "store.h"
#ifdef SELFTEST
#include "selftest.h"
#endif

#ifndef DEF_SERVER
#define DEF_SERVER  "irc.libera.chat"
#endif
#ifndef DEF_PORT
#define DEF_PORT    6667
#endif
#ifndef DEF_NICK
#define DEF_NICK    ""                  /* none: each user sets theirs in settings */
#endif
#ifndef DEF_CHAN
#define DEF_CHAN    "#c64friends"
#endif

static void poll_proc(void);
static struct process ptab[2];

/* ---- dialogs ------------------------------------------------------------------------------------------
 * Built here (not cc65's DlgBoxOk/DlgBoxGetString) so the self-test can add a DB_USR_ROUT entry: GEOS pauses
 * processes during a dialog (DlgBoxPrep saves TimersTab and calls InitGEOEnv; geos-src_kernal_dlgbox.asm),
 * so the test can only drive a dialog from inside it. Command format: gdlgbox.h / GEOS Programmer's Ref. */
static char db[24], *dp;

static void db_begin(void)
{
    dp = db;
    *dp++ = DEF_DB_POS | 1;
}

static void db_text(char y, const char *t)
{
    *dp++ = DBTXTSTR; *dp++ = TXT_LN_X; *dp++ = y;
    *(const char **)dp = t; dp += 2;
}

static char db_run(void)
{
#ifdef SELFTEST
    *dp++ = DB_USR_ROUT;
    *(void (**)(void))dp = selftest_dlg_hook; dp += 2;
#endif
    *dp = DB_END;
    return DoDlgBox(db);
}

static void dlg_ok(const char *l1, const char *l2)
{
    db_begin();
    db_text(TXT_LN_2_Y, l1);
    db_text(TXT_LN_3_Y, l2);
    *dp++ = OK; *dp++ = DBI_X_0; *dp++ = DBI_Y_2;
    db_run();
}

/* ---- menus ------------------------------------------------------------------------------------------ */

static void m_about(void)
{
    ReDoMenu();                         /* top-level menu: un-highlight + mouse back on (GotoFirstMenu only closes submenus) */
    dlg_ok(CBOLDON APP_NAME " " GEOCHAT_VERSION CPLAINTEXT " - IRC for GEOS 128, 80 columns",
           "Type /help for commands and keys.");
    ui_redraw();                        /* GEOS restores the dialog's area from the background screen; we draw to the foreground */
}

static void m_settings(void)
{
    ReDoMenu();
    settings_open();
}

const char app_name[] = APP_NAME;
static const char need_nick[] = "*** Set your nick in settings, then connect.";

static void m_connect(void)
{
    ReDoMenu();
    if (!set_nick[0]) { ui_msg(need_nick); return; }  /* no default nick: "NICK" alone would be refused */
    irc_start(set_server, parse_port(set_port[0] ? set_port : "6667"), set_nick, set_chan);
}

static void m_disconnect(void)
{
    ReDoMenu();
    irc_quit(app_name);
}

static void m_quit(void)
{
    ReDoMenu();
    irc_quit(app_name);
    vc_restore();                           /* GEOS and the deskTop expect the VDC mode they set */
    EnterDeskTop();
}

static struct menu main_menu = {
    { 0, MENU_BOT, 0, 311 },
    5 | HORIZONTAL,
    {
        { "about", MENU_ACTION, m_about },
        { "settings", MENU_ACTION, m_settings },
        { "connect", MENU_ACTION, m_connect },
        { "disconnect", MENU_ACTION, m_disconnect },
        { "quit", MENU_ACTION, m_quit },
    }
};

/* ---- GEOS callbacks ------------------------------------------------------------------------------------ */

static void click(void)                     /* otherPressVec: mouse presses outside menus */
{
    if (ui_overlay) settings_click();
    else ui_click();
}

static void key_handler(void)
{
    if (ui_overlay) settings_key(keyData);
    else ui_key(keyData);
}

static void recover(void)
{
    ui_redraw();
}

static void poll_proc(void)
{
    settings_poll();
    irc_poll();
#ifdef SELFTEST
    selftest_tick();
#endif
}

#ifdef LOAD_INPUT
/* The boot-disk build replaces the deskTop, and it is the deskTop that loads the input driver the user picked:
 * "GEOS will always boot up with the joystick as the input device" (geostechref.txt, Input Drivers).
 * So load the first 128 input driver on the disk ourselves. GEOS 128 input drivers live at MOUSEBASE
 * $FD00-$FE7F; the InitMouse vector is at MOUSEJMP $FE80 (Geos128.txt). The file is read into a buffer first
 * and copied with interrupts off, so the IRQ never calls a half-loaded driver. */
static void load_input_driver(void)
{
    static char names[17];
    char *buf = fl_buf;                     /* fl_draw's scratch line (722 bytes) is free here */
    unsigned load, len;
    memset(names, 0, sizeof(names));
    FindFTypes(names, INPUT_128, 1, NULL);
    if (!names[0] || FindFile(names) || GetFHdrInfo(&dirEntryBuf)) return;
    load = fileHeader.load_address;
    len = fileHeader.end_address - load;
    if (load < 0xFD00 || load + len > 0xFE80 || len > 0x180) return;
    if (ReadFile(&dirEntryBuf.n_block, buf, 0x180)) return;
    __asm__("sei");
    memcpy((char *)load, buf, len);
    ((void (*)(void))0xFE80)();         /* InitMouse */
    __asm__("cli");
    ui_msg(names);
}
#endif

void main(void)
{
    store_init();
    irc_init();
    ui_init();
#ifdef SELFTEST
    selftest_init();
#endif
    irc_clock();                            /* GEOS's clock from the Ultimate's real-time clock */
#ifdef LOAD_INPUT
    load_input_driver();                    /* after the clock, so its message gets the right time */
#endif
    if (settings_load() == 0) ui_msg("*** Settings loaded.");
    ui_on_line = irc_input;
    ui_complete = irc_complete;
    otherPressVec = click;
    if (store_banks) {
        char m[60];
        fmt(m, "*** Scrollback: %u KB in the REU.", (unsigned)store_banks * 64);
        ui_msg(m);
    }
    if (ui_vcache) ui_msg("*** 64 KB VDC: fast window switching.");
    keyVector = key_handler;
    RecoverVector = recover;
    DoMenu(&main_menu);

    ptab[0].pointer = (unsigned)poll_proc;
    ptab[0].jiffies = 2;
    ptab[1].pointer = 0;
    ptab[1].jiffies = 0;
    InitProcesses(1, ptab);
    RestartProcess(0);

    ui_msg(CBOLDON APP_NAME " " GEOCHAT_VERSION CPLAINTEXT " - type /help for commands and keys.");
#if defined(SELFTEST)
    irc_start(DEF_SERVER, 6667, DEF_NICK, DEF_CHAN);   /* (the test network ignores the name) */
#elif defined(AUTOCONNECT)
    irc_start(DEF_SERVER, DEF_PORT, DEF_NICK, DEF_CHAN);
#else
    if (!set_nick[0]) ui_msg(need_nick);    /* only when there is no nick yet (not loaded, no built-in one) */
#endif
    MainLoop();
}
