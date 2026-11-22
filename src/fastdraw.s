; fastdraw.s - draw one 9-pixel-high text line of the GEOS 128 80-column screen, fast.
; (Not named fastline.s: cl65 compiles fastline.c through a temporary fastline.s and would overwrite it.)
;
; GEOS's PutString draws an 80-column character by reading and rewriting each of its pixel rows through the
; VDC's address/data registers, one character at a time: a full line of ~100 characters costs about a quarter
; of a second. fl_draw instead builds the whole line (80 bytes x 9 rows) in RAM from a cache of the system
; font's glyphs and sends it to the VDC in one stream: one address, then 720 data writes (the VDC's update
; address advances by itself after each R31 access: Bauer VIC/VDC docs; scanline y is at VDC address y*80:
; GetScanLine/GSC80 in the GEOS 2.0 source). Writing the whole line also clears it.
;
; The output is meant to be pixel-identical to PutString with the system font (BSW 9), including bold
; (GEOS ORs each glyph row with itself shifted right one pixel and makes the character one pixel wider:
; FntShJump, geos-src_kernal_fonts.asm.txt) and reverse (the character's box is inverted: Font_2/r10L, same
; file). The self-test compares both pixel for pixel (tools/vtest.py, "fast line drawing").
;
; The glyph cache is filled by fl_init (fastline.c) from the font GEOS has active.
; Direct screen writes must call TempHideMouse first (Geos128.txt, "Writing directly to the screen").
; I/O is made visible with MMU config CIOIN ($7E), as GEOS 128's START_IO_128 does (geos-src_inc.asm.txt).

        .export _fl_draw, _fl_buf, _fl_top, _fl_w, _fl_g, _fl_xi, _fl_gx, _fl_vread, _fl_vaddr, _fl_vbuf
        .importzp ptr1, ptr2, ptr3, tmp1, tmp2, tmp3, tmp4

VDC_ADDR        = $D600
VDC_DATA        = $D601
CONFIG          = $FF00
CIOIN           = $7E
TempHideMouse   = $C2D7

NCHARS          = 95            ; ' ' .. '~'
ROWS            = 9
LINE_BYTES      = 80
TEXT_RIGHT      = 640           ; nothing is drawn past the screen's right edge

BOLDON          = 24            ; GEOS style codes (cc65 geos/ggraph.h)
PLAINTEXT       = 27
REV_ON          = 18
REV_OFF         = 19

        .bss
_fl_vaddr:  .res 2              ; fl_vread: VDC address
_fl_vbuf:   .res 2              ; the 2 bytes read
_fl_top:    .res 1              ; first scanline of the line (the character cell's top row)
_fl_w:      .res NCHARS         ; width of each character in pixels
_fl_xi:     .res NCHARS         ; for characters wider than 8 pixels: index*9 into fl_gx, else $FF
_fl_gx:     .res 8*ROWS         ; pixels 8-15 of those rows
xpos:       .res 2              ; current pixel column
cw:         .res 1              ; width of the current character, including the bold column
bold:       .res 1
rev:        .res 1
shift:      .res 1              ; x & 7
xi:         .res 1              ; fl_xi of the current character
mhi:        .res 1              ; reverse mask: cw bits set from the left
mlo:        .res 1
lcnt:       .res 1              ; 8 - shift (shifting left is shorter for shifts 5-7)
row:        .res 1
gi:         .res 1              ; row offset into fl_gx

        .segment "HIBSS"        ; upper RAM (src/geochat128.cfg)
_fl_g:      .res NCHARS*ROWS    ; pixels 0-7 of each glyph row, left-justified (char*9 + row)
_fl_buf:
buf:        .res ROWS*LINE_BYTES+2  ; the line being built (+2: the last character's spill bytes are 0)

        .code

; ---- void fl_vread(void): fl_vbuf = 2 bytes of VDC memory at fl_vaddr (each R31 read advances the address)
_fl_vread:
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG
        ldx #18
        lda _fl_vaddr+1
        jsr vw
        inx
        lda _fl_vaddr
        jsr vw
        ldx #31
        stx VDC_ADDR
@r1:    bit VDC_ADDR
        bpl @r1
        lda VDC_DATA
        sta _fl_vbuf
@r2:    bit VDC_ADDR
        bpl @r2
        lda VDC_DATA
        sta _fl_vbuf+1
        pla
        sta CONFIG
        plp
        rts
vw:     stx VDC_ADDR
@w:     bit VDC_ADDR
        bpl @w
        sta VDC_DATA
        rts

; ---- void __fastcall__ fl_draw(const char *s): draw s at TEXT_X=4 on the line starting at scanline fl_top
_fl_draw:
        sta ptr1                ; ptr1 = the string
        stx ptr1+1

        lda #<buf               ; clear the line
        sta ptr2
        lda #>buf
        sta ptr2+1
        lda #0
        ldx #>(ROWS*LINE_BYTES+2)
        ldy #0
@clr:   sta (ptr2),y
        iny
        bne @clr
        inc ptr2+1
        dex
        bne @clr
        ldy #<(ROWS*LINE_BYTES+2)
@clr2:  dey
        sta (ptr2),y
        bne @clr2

        lda #4                  ; TEXT_X (ui.h)
        sta xpos
        lda #0
        sta xpos+1
        sta bold
        sta rev

next:   ldy #0
        lda (ptr1),y
        bne @have
        jmp send
@have:  inc ptr1
        bne @n1
        inc ptr1+1
@n1:    cmp #BOLDON
        bne @s1
        lda #1
        sta bold
        bne next
@s1:    cmp #PLAINTEXT
        bne @s2
        lda #0
        sta bold
        sta rev
        beq next
@s2:    cmp #REV_ON
        bne @s3
        lda #1
        sta rev
        bne next
@s3:    cmp #REV_OFF
        bne @s4
        lda #0
        sta rev
        beq next
@s4:    cmp #32
        bcc next                ; other control codes: nothing (ui.c drops them before this anyway)
        cmp #127
        bcc @ok
        lda #'?'
@ok:    sec
        sbc #32
        tax                     ; X = character index 0..94

        lda _fl_w,x             ; cw = width + bold
        clc
        adc bold
        sta cw
        clc                     ; stop if it would cross the right edge
        adc xpos
        tay
        lda xpos+1
        adc #0
        cmp #>TEXT_RIGHT
        bcc @fits
        bne @full
        cpy #<(TEXT_RIGHT+1)
        bcc @fits
@full:  jmp send
@fits:
        lda _fl_xi,x
        sta xi

        stx tmp1                ; ptr3 = fl_g + index*9
        lda #0
        sta tmp2
        txa
        asl
        rol tmp2
        asl
        rol tmp2
        asl
        rol tmp2                ; index*8
        clc
        adc tmp1
        bcc @p1
        inc tmp2
@p1:    clc
        adc #<_fl_g
        sta ptr3
        lda tmp2
        adc #>_fl_g
        sta ptr3+1

        lda rev                 ; reverse mask: cw bits from the left
        beq @m0
        lda #0
        sta mhi
        sta mlo
        ldx cw
        beq @m0
@m:     sec
        ror mhi
        ror mlo
        dex
        bne @m
@m0:
        lda xpos                ; ptr2 = buf + x/8, shift = x & 7
        and #7
        sta shift
        lda xpos+1
        sta tmp1
        lda xpos
        lsr tmp1
        ror
        lsr tmp1
        ror
        lsr tmp1
        ror
        clc
        adc #<buf
        sta ptr2
        lda #>buf
        adc #0
        sta ptr2+1

        lda #8
        sec
        sbc shift
        sta lcnt
        lda #0
        sta row
        lda xi
        sta gi
        lda bold                ; the common case: not bold, not reverse, at most 8 pixels wide
        ora rev
        bne rowlp
        lda xi
        cmp #$FF
        bne rowlp
        jmp simple

rowlp:  ldy row
        lda (ptr3),y            ; pixels 0-7
        sta tmp1
        lda #0
        ldx xi
        cpx #$FF
        beq @nw
        ldx gi
        lda _fl_gx,x            ; pixels 8-15
        inc gi
@nw:    sta tmp2
        lda #0
        sta tmp3

        lda bold                ; bold: OR with itself shifted right one pixel
        beq @nb
        lda tmp1
        lsr
        sta tmp4
        lda tmp2
        ror
        ora tmp2
        sta tmp2
        lda tmp4
        ora tmp1
        sta tmp1
@nb:
        lda rev                 ; reverse: invert the character box
        beq @nr
        lda tmp1
        eor mhi
        sta tmp1
        lda tmp2
        eor mlo
        sta tmp2
@nr:
        ldx shift               ; move to the pixel column
        beq @ns
@sh:    lsr tmp1
        ror tmp2
        ror tmp3
        dex
        bne @sh
@ns:
        ldy #0                  ; OR into the line
        lda (ptr2),y
        ora tmp1
        sta (ptr2),y
        iny
        lda (ptr2),y
        ora tmp2
        sta (ptr2),y
        iny
        lda (ptr2),y
        ora tmp3
        sta (ptr2),y

        clc                     ; next row: +80 in the buffer (the glyph row is Y = row)
        lda ptr2
        adc #LINE_BYTES
        sta ptr2
        bcc @r1
        inc ptr2+1
@r1:    inc row
        lda row
        cmp #ROWS
        beq @done
        jmp rowlp
@done:

        clc                     ; x += cw
        lda xpos
        adc cw
        sta xpos
        bcc @x1
        inc xpos+1
@x1:    jmp next

; ---- the common case, row by row: empty glyph rows cost almost nothing; the glyph byte is moved to its pixel
; column by shifting right (shift 1-4) or left (as a 16-bit value from the low byte: 8-shift = 1-3 steps)
simple: ldy row
        lda (ptr3),y
        beq @adv
        ldx shift
        cpx #5
        bcs @left
        sta tmp1                ; hi = glyph, lo = 0, shift right
        lda #0
        cpx #0
        beq @or
@rl:    lsr tmp1
        ror
        dex
        bne @rl
        beq @or
@left:  ldx #0                  ; hi = 0, lo = glyph, shift left 8-shift
        stx tmp1
        ldx lcnt
@ll:    asl
        rol tmp1
        dex
        bne @ll
@or:    sta tmp2
        ldy #0
        lda (ptr2),y
        ora tmp1
        sta (ptr2),y
        iny
        lda (ptr2),y
        ora tmp2
        sta (ptr2),y
@adv:   clc
        lda ptr2
        adc #LINE_BYTES
        sta ptr2
        bcc @a1
        inc ptr2+1
@a1:    inc row
        lda row
        cmp #ROWS
        bne simple
        clc                     ; x += cw
        lda xpos
        adc cw
        sta xpos
        bcc @x1
        inc xpos+1
@x1:    jmp next

;----- send the line to the VDC at fl_top*80 --------------------------------------------------------------
; The VDC takes a CPU byte only every ~70 cycles at 2 MHz (measured in VICE: an empty line sent byte by byte
; took 28 ms). So: first clear all 720 bytes with the VDC's block fill (R24 bit 7 = 0: fill; one byte written
; through R31, then R30 = count repeats it: Bauer VIC/VDC docs, Mapping the C128 "VDC registers"), then send
; each of the 9 scanlines only as far as the text reaches (xpos).
send:   jsr TempHideMouse
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG

        lda _fl_top             ; tmp2:tmp1 = top*80 = top*64 + top*16
        sta tmp1
        lda #0
        sta tmp2
        asl tmp1
        rol tmp2
        asl tmp1
        rol tmp2
        asl tmp1
        rol tmp2
        asl tmp1
        rol tmp2                ; *16
        lda tmp1
        sta tmp3
        lda tmp2
        sta tmp4
        asl tmp1
        rol tmp2
        asl tmp1
        rol tmp2                ; *64
        clc
        lda tmp1
        adc tmp3
        sta tmp1
        lda tmp2
        adc tmp4
        sta tmp2

        ldx #24                 ; block mode: fill, not copy
        jsr vr
        and #$7F
        jsr vw
        jsr vaddr
        lda #0
        ldx #31                 ; the first byte...
        jsr vw
        ldx #30                 ; ...and 719 more: 255 + 255 + 209
        lda #255
        jsr vw
        jsr vw
        lda #209
        jsr vw

        lda xpos+1              ; bytes per scanline = (xpos + 7) / 8, at most 80
        sta tmp4
        lda xpos
        clc
        adc #7
        bcc @e1
        inc tmp4
@e1:    lsr tmp4
        ror
        lsr tmp4
        ror
        lsr tmp4
        ror
        cmp #LINE_BYTES+1
        bcc @e2
        lda #LINE_BYTES
@e2:    sta tmp3
        cmp #1                  ; xpos is at least 4 (TEXT_X): an empty string has 1 byte, still all 0
        beq @done

        lda #<buf
        sta ptr2
        lda #>buf
        sta ptr2+1
        lda #ROWS
        sta row
@line:  jsr vaddr
        ldx #31
        stx VDC_ADDR
        ldy #0
@b:     lda (ptr2),y
@w:     bit VDC_ADDR
        bpl @w
        sta VDC_DATA
        iny
        cpy tmp3
        bne @b
        clc                     ; next scanline: buffer +80, VDC address +80
        lda ptr2
        adc #LINE_BYTES
        sta ptr2
        bcc @n1
        inc ptr2+1
@n1:    clc
        lda tmp1
        adc #LINE_BYTES
        sta tmp1
        bcc @n2
        inc tmp2
@n2:    dec row
        bne @line

@done:  pla
        sta CONFIG
        plp
        rts

vaddr:  ldx #18                 ; VDC update address = tmp2:tmp1
        lda tmp2
        jsr vw
        inx
        lda tmp1
        jmp vw
vr:     stx VDC_ADDR            ; A = VDC register X
@r:     bit VDC_ADDR
        bpl @r
        lda VDC_DATA
        rts
