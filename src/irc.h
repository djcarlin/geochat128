/* irc.h - IRC protocol: connection, parsing, routing to windows, user commands */
#ifndef IRC_H
#define IRC_H

void irc_start(const char *server, unsigned port, const char *nick, const char *channel);
void irc_init(void);
void irc_clock(void);               /* set GEOS's clock from the network side (Ultimate RTC), if it can */
void irc_poll(void);                /* call often (GEOS process): reads the network, handles lines, reconnects */
void irc_input(char *line);         /* a line typed by the user (in the active window) */
void irc_quit(const char *reason);  /* user-initiated: no automatic reconnect */
const char *irc_complete(const char *prefix, unsigned char nth);   /* nth recently seen nick with that prefix */

extern unsigned irc_lines_in;       /* lines received from the server (for tests/status) */
extern unsigned char irc_state;     /* 0 offline, 1 registering, 2 registered */
extern unsigned irc_retry;          /* polls until the next reconnect attempt (0 = none) */

#endif
