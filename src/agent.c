/* agent.c - test builds only: carries out commands from tools/vtest.py (posted in the mailbox, mbox.h).
 *
 * Input goes through GEOS itself:
 *  - keys: keyData + the current keyVector, exactly what MainLoop does with a key from the queue
 *    (_DoCheckButtons / KbdScanHelp3, geos-src_kernal_keyboard.asm), so dialogs and pages see them too;
 *  - clicks: pointer position (mousePos $3A/$3C), button down (mouseData $8505 bit 7 = 0) and the
 *    "button changed" flag (pressFlag MOUSE_BIT), so GEOS's own menu, icon and otherPressVec code handles them.
 * GEOS pauses processes while a dialog is open, so each dialog carries a DB_USR_ROUT (selftest_dlg_hook) that
 * installs a driver in appMain, which the dialog's MainLoop calls: it clicks OK.
 * Everything else - the IRC server, the script and the checks - is Python (tools/vtest.py).
 */
#include <string.h>
#include <geos.h>
#include "ui.h"
#include "hw.h"
#include "fmt.h"
#include "store.h"
#include "settings.h"
#include "selftest.h"
#include "mbox.h"
#include "fastline.h"

/* OK icon of a default-position dialog (measured from the 80-column screenshot) */
#define OK_X        192
#define OK_Y        112

#pragma bss-name (push, "HIBSS")
struct mbox mbox;
#pragma bss-name (pop)
static unsigned char released = 1, rticks;

static void key(char c)
{
    keyData = c;
    keyVector();
}

static void click(unsigned x, unsigned char y)
{
    mousePos.x = x; mousePos.y = y;
    mouseData = 0;                          /* button down */
    pressFlag |= 1 << MOUSE_BIT;
    released = 0; rticks = 0;
}

static void release(void)
{
    mouseData = 0x80;
    pressFlag |= 1 << MOUSE_BIT;
    released = 1;
}

/* ---- inside dialogs: click OK ------------------------------------------------------------------------- */
static unsigned dcnt;
unsigned char agent_dlgs;                   /* dialogs opened (read by vtest.py) */

static void dlg_pump(void)                  /* appMain while a dialog is open */
{
    ++dcnt;
    if (dcnt == 0x200) click(OK_X, OK_Y);
    if (dcnt == 0x400) release();
}

void selftest_dlg_hook(void)
{
    ++agent_dlgs;
    dcnt = 0;
    appMain = dlg_pump;
}

/* ---- timing: CIA 2's timers cascaded into one 32-bit down-counter of the 1 MHz clock (timer B counts timer A
 * underflows: CRB bits 6-5 = %10; bit 4 loads the latch, bit 0 starts: CIA6526-software-model.txt,
 * CIA6526-datasheet.txt). Their interrupts stay masked (ICR), so no NMI. Result in units of 256 cycles. */
#define CIA2 ((volatile unsigned char *)0xDD00)
static void t_start(void)
{
    CIA2[0x0E] = 0; CIA2[0x0F] = 0;
    CIA2[4] = CIA2[5] = CIA2[6] = CIA2[7] = 0xFF;
    CIA2[0x0F] = 0x51;
    CIA2[0x0E] = 0x11;
}
static unsigned t_stop(void)
{
    unsigned long t;
    CIA2[0x0E] = 0;
    t = ~(((unsigned long)(CIA2[7] << 8 | CIA2[6]) << 16) | (unsigned)(CIA2[5] << 8 | CIA2[4]));
    CIA2[0x0F] = 0;
    return (unsigned)(t >> 8);
}

/* ---- commands ----------------------------------------------------------------------------------------- */
static void run(void)
{
    const char *t;
    unsigned n, k;
    spos p;
    char m[16];
    mbox.result = 0;
    switch (mbox.op) {
    case OP_KEYS:
        for (t = mbox.text; *t; ++t) key(*t);
        break;
    case OP_CLICK:
        click(mbox.x, (unsigned char)mbox.y);
        break;
    case OP_REDRAW:                         /* result = time taken */
        t_start();
        ui_redraw();
        mbox.result = t_stop();
        break;
#ifdef PROFILE                              /* only in profiling builds: the test build is nearly full */
    case OP_TIME:                           /* result = time of 10 x part a1, on the active window's newest */
        p = store_last(ui_active);
        store_get(p, mbox.text, sizeof(mbox.text));
        t_start();
        for (k = 0; k < 10; ++k)
            switch (mbox.a1) {
            case 1: store_get(p, mbox.rx, sizeof(mbox.rx)); break;
            case 2: store_prev(p); break;
            case 3: ui_wrap_rows(mbox.text); break;
            case 4: fl_top = 16 + 3 * 9; fl_draw(mbox.text); break;
            }
        mbox.result = t_stop();
        break;
#endif
    case OP_DRAWROW:
        ui_slow = (unsigned char)mbox.x;
        currentMode = 0;                            /* PutString leaves GEOS's style (bold...) switched on */
        ui_draw_row(mbox.a1, mbox.text);
        ui_slow = 0;
        break;
    case OP_SWITCH:                         /* result = time taken */
        t_start();
        ui_switch(mbox.a1);
        mbox.result = t_stop();
        break;
    case OP_LAST:
        store_get(store_last(mbox.a1), mbox.text, sizeof(mbox.text));
        break;
    case OP_FLOOD:
        for (n = 0; n < mbox.x; ++n) { fmt(m, "flood %u", n); ui_add(mbox.a1, m, 0); }
        break;
    case OP_WALK:
        k = 0; n = mbox.x;
        for (p = store_last(mbox.a1); p != SPOS_NONE; p = store_prev(p)) {
            store_get(p, mbox.text, sizeof(mbox.text));
            fmt(m, "flood %u", n - 1);
            if (strcmp(mbox.text + 6, m)) break;
            ++k;
            if (!--n) { p = store_prev(p); break; }
        }
        mbox.result = k;
        n = 1;
        if (p != SPOS_NONE) {               /* stopped before the oldest record: it must be a real message */
            store_get(p, mbox.text, sizeof(mbox.text));
            n = mbox.text[2] == ':' && mbox.text[5] == ' ';
        }
        mbox.text[0] = (char)n;
        break;
    case OP_INPUT:
        strcpy(mbox.text, ui_input());
        break;
    case OP_FINDCFG:
        mbox.result = FindFile(CFG_NAME);
        break;
    case OP_SETLOAD:
        mbox.result = settings_load();
        break;
    case OP_EXIT:
        test_result(mbox.a1);
        break;
    }
}

void selftest_init(void)                    /* HIBSS is not cleared at start-up */
{
    memset(&mbox, 0, sizeof(mbox));
    memcpy(mbox.magic, "MBX1", 4);
}

void selftest_tick(void)                    /* called from the GEOS process, every 2 jiffies */
{
    if (!released && ++rticks > 8) release();
    if (mbox.req != mbox.ack) {
        run();
        mbox.ack = mbox.req;
    }
}
