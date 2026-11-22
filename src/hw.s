; hw.s - C128 hardware helpers for geochat128 (ca65, cc65 calling conventions)
;
; vdc_scroll: scroll a band of the GEOS 128 80-column screen up with the VDC's block-copy engine.
;   GEOS 128 80-column bitmap: scanline y starts at VDC address y*80 (GetScanLine/GSC80 in the GEOS 2.0
;   source: r5 = y*64 + y*16). Copy mode = VDC register 24 bit 7; source = R32/R33; destination (update
;   address) = R18/R19; R30 = byte count, starts the copy/fill; R31 = data byte for fill.
;   Direct screen writes must call TempHideMouse first (Geos128.txt, "Writing directly to the screen").
;   vdc_and(): AND of vdc_n bytes from VDC address vdc_addr, read through R18/R19 + R31 (each R31 read
;   advances the update address; Bauer VIC/VDC docs). $FF = all pixels set. Used by the self-test.
; test_result: write a byte to $D7FF (VICE debug cartridge: exits with that code; 0 = PASS).
;
; I/O is made visible with the C128 MMU: config ($FF00) = CIOIN ($7E), the same thing GEOS 128's own
; START_IO_128 macro does (geos-src_inc.asm.txt), then the old value is put back.

        .export _vdc_scroll, _test_result, _vdc_and, _vdc_addr, _vdc_n, _tod_set, _tod_h, _tod_m, _tod_s
        .export _vdc_xor, _vdc_add, _back_move, _bm_src, _bm_dst, _bm_len, _bm_banks
        .export _scr_top, _scr_bot, _scr_amt
        .export _ncase, _beep
        .import popax
        .importzp ptr1, ptr2, tmp3, tmp4
        .export _vc_detect, _vc_restore, _vc_copy, _vc_src, _vc_dst, _vc_len

VDC_ADDR        = $D600
VDC_DATA        = $D601
CONFIG          = $FF00
CIOIN           = $7E
TempHideMouse   = $C2D7

        .bss
_scr_top:   .res 1          ; first scanline of the band
_scr_bot:   .res 1          ; last scanline of the band
_scr_amt:   .res 1          ; scanlines to scroll by (= text line height)
m80:        .res 2          ; result of mul80
t16:        .res 2
count:      .res 2          ; bytes left for blockop
_bm_src:    .res 2          ; back_move: source address
_bm_dst:    .res 2          ; destination address
_bm_len:    .res 2          ; bytes
_bm_banks:  .res 2          ; source bank, destination bank: 0 = backRAM (bank 0), 1 = frontRAM (bank 1)
_vdc_addr:  .res 2
_vdc_n:     .res 1
_tod_h:     .res 1          ; tod_set: hour 0-23
_tod_m:     .res 1          ; minute
_tod_s:     .res 1          ; second
_vdc_xor:   .res 1
_vdc_add:   .res 1
acc:        .res 1
_vc_src:    .res 2          ; vc_copy: VDC source address
_vc_dst:    .res 2          ; destination
_vc_len:    .res 2          ; bytes
r28:        .res 1          ; vc_detect: R28 as GEOS left it
vcset:      .res 1          ; 1 = vc_detect switched the VDC to 64 KB mode

        .code

; ---- write A to VDC register X / read VDC register X into A -----------------------------------------
vdcw:   stx VDC_ADDR
@w:     bit VDC_ADDR        ; bit 7 = ready
        bpl @w
        sta VDC_DATA
        rts
vdcr:   stx VDC_ADDR
@w:     bit VDC_ADDR
        bpl @w
        lda VDC_DATA
        rts

; ---- m80 = A * 80 -------------------------------------------------------------------------------------
mul80:  sta m80
        lda #0
        sta m80+1
        asl m80             ; *2
        rol m80+1
        asl m80             ; *4
        rol m80+1
        asl m80             ; *8
        rol m80+1
        asl m80             ; *16
        rol m80+1
        lda m80
        sta t16
        lda m80+1
        sta t16+1
        asl m80             ; *32
        rol m80+1
        asl m80             ; *64
        rol m80+1
        clc
        lda m80
        adc t16
        sta m80
        lda m80+1
        adc t16+1
        sta m80+1
        rts

; ---- run the current copy/fill for 'count' bytes, at most 255 per R30 write ---------------------------
blockop:
@loop:  lda count+1
        bne @big
        lda count
        beq @done
        ldx #30
        jsr vdcw
        lda #0
        sta count
        rts
@big:   lda #255
        ldx #30
        jsr vdcw
        sec
        lda count
        sbc #255
        sta count
        lda count+1
        sbc #0
        sta count+1
        jmp @loop
@done:  rts

; ---- void vdc_scroll(void): scroll scr_top..scr_bot up by scr_amt lines, clear the freed lines ---------
_vdc_scroll:
        jsr TempHideMouse
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG

        lda _scr_top            ; destination = top*80
        jsr mul80
        lda m80+1
        ldx #18
        jsr vdcw
        lda m80
        ldx #19
        jsr vdcw

        ldx #24                 ; copy mode on
        jsr vdcr
        ora #$80
        jsr vdcw

        lda _scr_top            ; source = (top+amt)*80
        clc
        adc _scr_amt
        jsr mul80
        lda m80+1
        ldx #32
        jsr vdcw
        lda m80
        ldx #33
        jsr vdcw

        lda _scr_bot            ; bytes = (bot - top + 1 - amt) * 80
        sec
        sbc _scr_top
        sec
        sbc _scr_amt
        clc
        adc #1
        jsr mul80
        lda m80
        sta count
        lda m80+1
        sta count+1
        jsr blockop

        ldx #24                 ; fill mode (copy bit off)
        jsr vdcr
        and #$7F
        jsr vdcw
        ; the update address now points just past the copied block = first freed line
        lda #0                  ; first fill byte (writing R31 stores it and advances)
        ldx #31
        jsr vdcw
        lda _scr_amt            ; then amt*80 - 1 more bytes of the same value
        jsr mul80
        sec
        lda m80
        sbc #1
        sta count
        lda m80+1
        sbc #0
        sta count+1
        jsr blockop

        pla
        sta CONFIG
        plp
        rts

; ---- void beep(void): a short "ding" on SID voice 1 (someone mentioned you) ------------------------------
; C128 Programmer's Reference Guide, "SID" (register map, and the steps: attack/decay $D405, sustain/release
; $D406, waveform $D404, volume $D418, frequency $D400/$D401 = F_out / 0.06097: $4000 = ~1 kHz). Attack 0,
; decay 9 (750 ms), sustain 0: the note dies away by itself, so the gate can stay on; switching it off and on
; again restarts it for the next ding.
SID             = $D400
_beep:  php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG
        lda #0
        sta SID+4               ; gate off
        sta SID+0               ; frequency $4000
        sta SID+6               ; sustain 0, release 0
        lda #$40
        sta SID+1
        lda #$09
        sta SID+5               ; attack 0, decay 9
        lda #$0F
        sta SID+24              ; volume 15
        lda #$11
        sta SID+4               ; triangle, gate on
        pla
        sta CONFIG
        plp
        rts

; ---- unsigned char __fastcall__ ncase(const char *a, const char *b, unsigned char n) -------------------------
; 1 if a and b match for n characters ignoring ASCII case, 0 if not; n = 0: whole strings (they end together).
; A shorter string never matches a longer prefix (its NUL differs from the other's character).
_ncase:
        sta tmp3                ; n (0 = 256: whole strings, at most 255 characters)
        jsr popax
        sta ptr2
        stx ptr2+1              ; b
        jsr popax
        sta ptr1
        stx ptr1+1              ; a
        ldy #0
@l:     lda (ptr1),y
        jsr lower
        sta tmp4
        lda (ptr2),y
        jsr lower
        cmp tmp4
        bne @no
        tax                     ; both ended together
        beq @yes
        iny
        dec tmp3
        bne @l
@yes:   lda #1
        ldx #0
        rts
@no:    lda #0
        tax
        rts
lower:  cmp #'A'
        bcc @x
        cmp #'Z'+1
        bcs @x
        ora #$20
@x:     rts

; ---- unsigned char vc_detect(void): 1 if the VDC has 64 KB of RAM (then left in 64 KB mode), else 0 ------
; R28 bit 4 selects the RAM type: 0 = 4416 (16 KB), 1 = 4164 (64 KB) (C128 Programmer's Reference Guide,
; "R28(4) 8563 RAM TYPE"). With bit 4 set, a marker is written to one byte of every 256-byte page (value =
; page number) and read back: if the chips ignore any of the upper address lines (16 KB), two pages share a
; byte and a marker comes back wrong. The markers overwrite 256 screen bytes: call before drawing the screen.
; Refused (0) if attributes are on (R25 bit 6): they would live in VDC RAM too.
_vc_detect:
        jsr TempHideMouse
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG
        ldy #0                  ; Y = result so far
        ldx #25
        jsr vdcr
        and #$40
        bne @out
        ldx #28
        jsr vdcr
        sta r28
        ora #$10
        jsr vdcw
@w:     jsr page                ; markers: page Y gets the value Y
        tya
        ldx #31
        jsr vdcw
        iny
        bne @w
@r:     jsr page
        ldx #31
        jsr vdcr
        sta acc
        cpy acc
        bne @bad
        iny
        bne @r
        ldy #1                  ; every page kept its own byte: 64 KB
        sty vcset
        bne @out
@bad:   lda r28                 ; 16 KB: put GEOS's mode back
        ldx #28
        jsr vdcw
        ldy #0
@out:   pla
        sta CONFIG
        plp
        tya
        ldx #0
        rts
page:   tya                     ; VDC address = Y * 256 + $7F
        ldx #18
        jsr vdcw
        lda #$7F
        inx
        jmp vdcw

; ---- void vc_restore(void): put R28 back as GEOS had it (before quitting to the deskTop) ------------------
_vc_restore:
        lda vcset
        beq @x
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG
        lda r28
        ldx #28
        jsr vdcw
        pla
        sta CONFIG
        plp
@x:     rts

; ---- void vc_copy(void): copy vc_len bytes of VDC RAM from vc_src to vc_dst with the VDC's block copy -------
; (R24 bit 7 = 1, source R32/R33, destination R18/R19, R30 = count: C128 Programmer's Reference Guide, "Block
; Copy"). The mouse pointer is taken off the screen first so it is never copied.
_vc_copy:
        jsr TempHideMouse
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG
        ldx #24                 ; copy mode
        jsr vdcr
        ora #$80
        jsr vdcw
        lda _vc_dst+1
        ldx #18
        jsr vdcw
        lda _vc_dst
        inx
        jsr vdcw
        lda _vc_src+1
        ldx #32
        jsr vdcw
        lda _vc_src
        inx
        jsr vdcw
        lda _vc_len
        sta count
        lda _vc_len+1
        sta count+1
        jsr blockop
        ldx #24                 ; back to fill mode
        jsr vdcr
        and #$7F
        jsr vdcw
        pla
        sta CONFIG
        plp
        rts

; ---- unsigned char vdc_and(void) ---------------------------------------------------------------------
_vdc_and:
        jsr TempHideMouse       ; the 80-column pointer is drawn into the bitmap
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG
        lda _vdc_addr+1
        ldx #18
        jsr vdcw
        lda _vdc_addr
        ldx #19
        jsr vdcw
        lda #$FF
        sta acc
        lda #0
        sta _vdc_xor
        sta _vdc_add
        ldy _vdc_n
@l:     ldx #31
        jsr vdcr
        tax
        eor _vdc_xor            ; fingerprint: XOR and sum of the bytes
        sta _vdc_xor
        txa
        clc
        adc _vdc_add
        sta _vdc_add
        txa
        and acc
        sta acc
        dey
        bne @l
        pla
        sta CONFIG
        plp
        lda acc
        ldx #0
        rts

; ---- void back_move(void): GEOS 128 MoveBData ($C2E3): r0 source, r1 dest, r2 count, r3L source bank,
; r3H dest bank, 0 = backRAM, 1 = frontRAM (Hitchhiker's Guide, MoveBData; the kernal's _DoBOp turns r3L into
; the MMU bank bit with ror/ror/ror/and #$C0, so 1 = bank 1 = the application's front RAM).
MoveBData       = $C2E3
r0              = $02
_back_move:
        ldx #7
@c:     lda _bm_src,x
        sta r0,x
        dex
        bpl @c
        jmp MoveBData

; ---- void tod_set(void): set CIA 1's time-of-day clock, which GEOS's DoUpdateTime copies into hour/minutes/
; seconds every main loop (geos-src_kernal_time.asm). Registers $DC08 tenths, $DC09 sec, $DC0A min, $DC0B hours
; (BCD, 1-12, bit 7 = PM). Writing hours stops the clock, writing tenths starts it again, and CRB ($DC0F)
; bit 7 must be 0 or the writes set the alarm (CIA6526-datasheet, Time of Day Clock).
CIA1            = $DC00
tobcd:  ldx #0                  ; A (0-99) -> BCD
@t:     cmp #10
        bcc @d
        sbc #10
        inx
        bne @t
@d:     sta t16
        txa
        asl
        asl
        asl
        asl
        ora t16
        rts

_tod_set:
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG
        lda CIA1+15             ; CRB bit 7 = 0: TOD writes set the time, not the alarm
        and #$7F
        sta CIA1+15
        lda _tod_h              ; 0-23 -> 12-hour + PM bit
        ldy #0
        cmp #12
        bcc @am
        ldy #$80
        sbc #12
@am:    cmp #0
        bne @nz
        lda #12
@nz:    jsr tobcd
        sty t16+1
        ora t16+1
        sta CIA1+11             ; hours (stops the clock)
        lda _tod_m
        jsr tobcd
        sta CIA1+10
        lda _tod_s
        jsr tobcd
        sta CIA1+9
        lda #0
        sta CIA1+8              ; tenths (starts the clock)
        pla
        sta CONFIG
        plp
        rts

; ---- void __fastcall__ test_result(unsigned char code) -------------------------------------------------
_test_result:
        tax
        php
        sei
        lda CONFIG
        pha
        lda #CIOIN
        sta CONFIG
        stx $D7FF
        pla
        sta CONFIG
        plp
        rts
