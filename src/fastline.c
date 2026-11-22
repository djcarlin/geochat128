/* fastline.c - the glyph cache for fastdraw.s, taken from what GEOS itself draws.
 *
 * Reading the font tables directly does not work: curIndexTable/cardDataPntr ($2A/$2C) point into GEOS 128's
 * kernal area, which the application's memory configuration does not show (the self-test read garbage there).
 * So each character is drawn once with PutChar at start-up (call after UseSystemFont, before the screen is
 * drawn) and its pixels are read back from VDC memory: the cache is then exactly what PutString would draw.
 * Widths come from GetCharWidth.
 */
#include <geos.h>
#include "fastline.h"

#define SCRATCH_Y   16                  /* a scanline of the message area: cleared again before use */
#define BASE        6                   /* BSW 9: baseline 6 rows below the cell top (ui.c BASELINE) */
#define X           8                   /* draw at pixel 8 = VDC byte 1 of the scanline */

unsigned char fl_ok;

unsigned char fl_init(void)
{
    unsigned char c, r, w, nx = 0;

    fl_ok = 0;
    if (*(unsigned char *)0x29 != FL_ROWS) return 0;    /* curHeight (geos-src_inc.asm.txt; cc65's struct
                                                           fontdesc has setWidth as a byte, so not .height) */
    currentMode = 0;
    for (c = 0; c < FL_NCHARS; ++c) {
        w = GetCharWidth(c + 32);
        if (w > 16 || (w > 8 && nx == 8)) return 0;
        fl_w[c] = w;
        fl_xi[c] = w > 8 ? nx * FL_ROWS : 0xFF;
        fl_top = SCRATCH_Y;
        fl_draw("");                    /* clear the scratch line (fastdraw.s works before the cache is filled) */
        PutChar(c + 32, SCRATCH_Y + BASE, X);
        for (r = 0; r < FL_ROWS; ++r) {
            fl_vaddr = (SCRATCH_Y + r) * 80 + X / 8;
            fl_vread();
            fl_g[c * FL_ROWS + r] = fl_vbuf[0];
            if (w > 8) fl_gx[nx * FL_ROWS + r] = fl_vbuf[1];
        }
        if (w > 8) ++nx;
    }
    fl_top = SCRATCH_Y;
    fl_draw("");
    return fl_ok = 1;
}
