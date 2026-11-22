/* mbox.h - the test mailbox shared with tools/vtest.py (which reads/writes it through the VICE binary monitor).
 *
 * The emulator is stopped while Python touches it. Every index has exactly one writer and is a single byte,
 * so a stop in the middle of an update can never corrupt anything:
 *   req (Python) / ack (C128): agent commands;   rx_head (Python) / rx_tail (C128): server -> client bytes;
 *   tx_head (C128) / tx_tail (Python): client -> server bytes;   drop_req (Python) / drop_seen (C128);
 *   conn, closed (C128): counters of net_connect / net_close calls.
 * Python finds 'mbox' through the linker map (build/geochat128-test.map). Keep this layout in sync with tools/vtest.py.
 */
#ifndef MBOX_H
#define MBOX_H

struct mbox {
    char magic[4];                  /* "MBX1" once the agent runs */
    unsigned char req, ack;         /* Python increments req after filling op/args; the agent sets ack = req */
    unsigned char op, a1;
    unsigned x, y;                  /* arguments */
    unsigned result;
    unsigned char conn, closed, drop_req, drop_seen;
    unsigned char rx_head, rx_tail, tx_head, tx_tail;
    char time[20];                  /* "yyyy/mm/dd hh:mm:ss" from Python (net_time) */
    char text[256];                 /* text argument / text result */
    char rx[256];                   /* server -> client ring */
    char tx[256];                   /* client -> server ring */
    unsigned char nocable;          /* Python: 1 = the network cable is "pulled" (net_connect fails) */
};
extern struct mbox mbox;

enum {                              /* agent operations */
    OP_KEYS = 1,                    /* type text (bytes go through keyData + keyVector) */
    OP_CLICK,                       /* press at (x, y); released a few ticks later */
    OP_REDRAW,                      /* ui_redraw() */
    OP_LAST,                        /* text = newest stored message of window a1 */
    OP_FLOOD,                       /* add x messages "flood <n>" to window a1 */
    OP_WALK,                        /* result = consecutive "flood" records walking back from the newest;
                                       text[0] = 1 if the walk ended cleanly (no older record or a non-flood one) */
    OP_INPUT,                       /* text = the input line */
    OP_FINDCFG,                     /* result = FindFile(settings file) */
    OP_SETLOAD,                     /* result = settings_load() */
    OP_EXIT,                        /* test result a1 to $D7FF (VICE exits) */
    OP_SWITCH,                      /* ui_switch(a1) */
    OP_DRAWROW,                     /* draw text on message row a1, with PutString if x = 1 (fast line if 0) */
    OP_TIME                         /* result = time of 10 x part a1 (1 store_get, 2 store_prev, 3 wrap, 4 draw) */
};

#endif
