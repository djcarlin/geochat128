/* hw.h - C128 hardware helpers (hw.s) */
#ifndef HW_H
#define HW_H

extern unsigned char scr_top, scr_bot, scr_amt;
void vdc_scroll(void);                              /* scroll scr_top..scr_bot up by scr_amt, clear bottom */
extern unsigned vdc_addr;                           /* VDC address (scanline y starts at y*80) */
extern unsigned char vdc_n;                         /* bytes, 1..255 */
extern unsigned char vdc_xor, vdc_add;              /* fingerprint of the bytes read by vdc_and */
unsigned char vdc_and(void);                        /* AND of those bytes: $FF = all pixels set */
extern unsigned bm_src, bm_dst, bm_len;             /* back_move parameters */
extern unsigned char bm_banks[2];                   /* source, destination: 0 = backRAM, 1 = frontRAM */
void back_move(void);                               /* GEOS 128 MoveBData */
extern unsigned char tod_h, tod_m, tod_s;           /* tod_set: 24-hour time */
void tod_set(void);                                 /* set CIA 1's TOD clock (GEOS reads its time from it) */
unsigned char vc_detect(void);                      /* 1 = 64 KB VDC (left in 64 KB mode); trashes the screen */
void beep(void);                                    /* SID "ding" (a mention) */
void vc_restore(void);                              /* R28 back as GEOS had it (quitting) */
extern unsigned vc_src, vc_dst, vc_len;
void vc_copy(void);                                 /* VDC block copy vc_src -> vc_dst, vc_len bytes */
void __fastcall__ test_result(unsigned char code);  /* VICE debug cartridge: exit code, 0 = PASS */

#endif
