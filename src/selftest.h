/* selftest.h - scripted self-test (SELFTEST builds, with net_fake.c) */
#ifndef SELFTEST_H
#define SELFTEST_H

void selftest_init(void);       /* clear the mailbox (before connecting) */
void selftest_tick(void);       /* called from the GEOS process (every 2 jiffies) on the main screen */
void selftest_dlg_hook(void);   /* DB_USR_ROUT of every dialog: drives the dialog from inside */

#endif
