/* fastline.h - fast 80-column text lines (fastline.s, fastline.c) */
#ifndef FASTLINE_H
#define FASTLINE_H

#define FL_NCHARS 95                                /* ' ' .. '~' */
#define FL_ROWS   9                                 /* the system font (BSW 9) */

extern unsigned char fl_top;
extern char fl_buf[722];                            /* fl_draw's scratch line: free between draws */                        /* first scanline of the line to draw */
extern unsigned char fl_w[FL_NCHARS];               /* character widths (pixels) */
extern unsigned char fl_g[FL_NCHARS * FL_ROWS];     /* glyph cache: pixels 0-7 of each row */
extern unsigned char fl_xi[FL_NCHARS];              /* wide characters: index into fl_gx, else $FF */
extern unsigned char fl_gx[8 * FL_ROWS];            /* pixels 8-15 */

extern unsigned char fl_ok;                         /* 1 once fl_init has filled the cache */
extern unsigned fl_vaddr;                           /* fl_vread: VDC address */
extern unsigned char fl_vbuf[2];                    /* the 2 bytes read from there */
void fl_vread(void);
unsigned char fl_init(void);                        /* from the active font; 0 = font not usable */
void __fastcall__ fl_draw(const char *s);           /* draw s at x=4, rows fl_top..fl_top+8 (clears them) */

#endif
