/* settings.h - the settings page (settings.c) */
#ifndef SETTINGS_H
#define SETTINGS_H

#define SET_NICK_MAX    15
#define SET_CHAN_MAX    31
#define SET_SERVER_MAX  47
#define SET_PORT_MAX    5
#define NFIELDS         4

/* layout (pixels; the message area is MSG_TOP..STATUS_Y-1) */
#define TITLE_Y     (MSG_TOP + 12)
#define FIELD_Y0    (MSG_TOP + 22)          /* top of the first field box */
#define FIELD_DY    20
#define FIELD_H     14
#define LABEL_X     24
#define BOX_L       120
#define BOX_R       600
#define HINT_Y      (FIELD_Y0 + NFIELDS * FIELD_DY + 8)
#define BTN_T       (HINT_Y + 8)
#define BTN_B       (BTN_T + 15)
#define OK_L        BOX_L
#define OK_R        (BOX_L + 80)
#define CAN_L       (BOX_L + 100)
#define CAN_R       (BOX_L + 180)

extern char set_server[], set_port[], set_nick[], set_chan[];
unsigned parse_port(const char *s);

#define CFG_NAME        "geoChat 128 cfg"
unsigned char settings_load(void);     /* 0 = loaded; 1 no file, 2 read error, 3 not a settings file */
unsigned char settings_save(void);     /* GEOS disk error, 0 = saved */

void settings_open(void);       /* show the page (does nothing if it is already open) */
void settings_key(char c);
void settings_click(void);
void settings_poll(void);       /* from the GEOS process: saves after OK */      /* mouse press while the page is open */      /* keys go here while ui_overlay is set */

#endif
