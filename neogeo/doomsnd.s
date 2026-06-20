;; DOOM-NG Z80 sound driver.
;;
;; ADPCM-B (set A, ports 4/5): "At Doom's Gate", looped forever in hardware.
;; ADPCM-A (set B, ports 6/7): one-shot SFX (pistol fire, imp death) fired on a
;; 68k sound command. The 68k writes a command byte -> our NMI stashes it ->
;; the idle loop programs ADPCM-A channel 1 and keys it on. The music keeps
;; looping on its own channel, undisturbed.
;;
;; V-ROM layout (built by the Makefile, cat'd in order):
;;   0x00000  music   (ADPCM-B, 512KB)
;;   0x80000  pistol  (ADPCM-A, 2816B -> start 0x0800 stop 0x080A in 256B units)
;;   0x80B00  scream  (ADPCM-A, 3584B -> start 0x080B stop 0x0818)

        .module doomsnd
        .area   CODE (ABS)

        CMD = 0xf800                 ; Z80 work-RAM byte: pending command from 68k

        .org    0x0000
_reset:
        di
        ld      sp, #0xfffc
        jp      _init

        ;; NMI (0x66): 68k wrote a sound command. Stash it for the idle loop and
        ;; ack so the BIOS handshake completes.
        .org    0x0066
_nmi:
        push    af
        in      a, (0x00)            ; PORT_FROM_68K (command)
        ld      (CMD), a             ; stash
        out     (0x0c), a            ; PORT_TO_68K (ack)
        pop     af
        retn

        .org    0x0100
_init:
        ld      a, #1
        out     (0x08), a            ; PORT_ENABLE_NMI
        xor     a
        ld      (CMD), a             ; NO music on boot -- the 68k sends cmd 3 at level start
_idle:
        ld      a, (CMD)             ; pending command?
        or      a
        jr      z, _idle
        ld      b, a                 ; b = command (1=pistol, 2=scream, 3=start music)
        xor     a
        ld      (CMD), a             ; consume
        ld      a, b
        cp      #0x10                ; cmd >= 0x10 -> play music TRACK (cmd-0x10): per-map soundtracks E1M1..E1M9
        jr      c, _nottrack
        sub     #0x10
        ld      b, a                 ; b = track index 0..8
        call    _music
        jr      _idle
_nottrack:
        cp      #3
        jr      z, _domusic          ; cmd 3 (legacy) -> track 0 (E1M1)
        cp      #4
        jr      nz, _notboom
        ld      b, #3                ; cmd 4 -> SFX index 2 (barrel explosion)
_notboom:
        cp      #5
        jr      nz, _notitem
        ld      b, #4                ; cmd 5 -> SFX index 3 (item pickup chime)
_notitem:
        cp      #6
        jr      nz, _notdoor
        ld      b, #5                ; cmd 6 -> SFX index 4 (door open/close)
_notdoor:
        cp      #7
        jr      nz, _notimpd
        ld      b, #6                ; cmd 7 -> SFX index 5 (imp death, DSBGDTH1)
_notimpd:
        cp      #8
        jr      nz, _nothumd
        ld      b, #7                ; cmd 8 -> SFX index 6 (possessed death, DSPODTH1)
_nothumd:
        dec     b                    ; -> 0-based SFX index
        call    _sfx
        jr      _idle
_domusic:
        ld      b, #0                ; cmd 3 (legacy music start) = track 0 (E1M1)
        call    _music
        jr      _idle

        ;; --- play SFX index B on ADPCM-A channel 1 (set B) ---
_sfx:
        ld      a, b
        add     a, a
        add     a, a                 ; a = index*4 (table stride)
        ld      e, a
        ld      d, #0
        ld      hl, #_sfxtab
        add     hl, de               ; hl -> {startLSB, startMSB, stopLSB, stopMSB}
        ld      c, #0x10             ; A1 start addr LSB
        ld      b, (hl)
        call    _ymb
        inc     hl
        ld      c, #0x18             ; A1 start addr MSB
        ld      b, (hl)
        call    _ymb
        inc     hl
        ld      c, #0x20             ; A1 stop addr LSB
        ld      b, (hl)
        call    _ymb
        inc     hl
        ld      c, #0x28             ; A1 stop addr MSB
        ld      b, (hl)
        call    _ymb
        ld      c, #0x08             ; A1 pan + per-channel volume
        ld      b, #0xdf             ; L+R, max (0x1f)
        call    _ymb
        ld      c, #0x01             ; ADPCM-A master volume
        ld      b, #0x3f             ; max
        call    _ymb
        ld      c, #0x00             ; key-on channel 1 (dump=0, ch1 bit set)
        ld      b, #0x01
        call    _ymb
        ret

        ;; Addresses RECOMPUTED from the actual .adpcma sizes (256B blocks; music=512KB=0x800).
        ;; (was stale: scream/boom/itemup assumed smaller samples -> misaligned playback.)
        ;; pistol 11blk, scream 17, boom 37, itemup 5, door 27 -- inclusive stop = start+blk-1.
        ;; SFX now live AFTER the 10*512KB music (5MB: 9 maps + intro) -> base 0x5000. MSB 0x50.
_sfxtab:
        .db     0x00, 0x50, 0x0a, 0x50   ; 1: pistol  0x5000-0x500A (11 blk)
        .db     0x0b, 0x50, 0x1b, 0x50   ; 2: scream  0x500B-0x501B (17 blk)
        .db     0x1c, 0x50, 0x40, 0x50   ; 3: boom    0x501C-0x5040 (37 blk)
        .db     0x41, 0x50, 0x45, 0x50   ; 4: itemup  0x5041-0x5045 (5 blk)
        .db     0x46, 0x50, 0x60, 0x50   ; 5: door    0x5046-0x5060 (27 blk, DSDOROPN)
        .db     0x61, 0x50, 0x6e, 0x50   ; 6: impdth  0x5061-0x506E (14 blk, DSBGDTH1)
        .db     0x6f, 0x50, 0x88, 0x50   ; 7: posdth  0x506F-0x5088 (26 blk, DSPODTH1)

        ;; --- music: program + start ADPCM-B (set A: 0x10..0x1b), hardware loop ---
_music:                              ; B = track index 0..8 (each track = 512KB = 0x800 in 256B addr units)
        ld      a, b
        add     a, a
        add     a, a
        add     a, a                 ; A = track*8 = ADPCM-B start addr MSB (LSB=0)
        ld      d, a                 ; D = start MSB
        or      #0x07
        ld      e, a                 ; E = stop MSB = track*8 | 7  (track end = start + 0x7FF)
        ld      c, #0x10
        ld      b, #0x01             ; reset ADPCM-B
        call    _yma
        ld      c, #0x11
        ld      b, #0xc0             ; pan L+R
        call    _yma
        ld      c, #0x12
        ld      b, #0x00             ; start LSB
        call    _yma
        ld      c, #0x13
        ld      b, d                 ; start MSB = track*8
        call    _yma
        ld      c, #0x14
        ld      b, #0xff             ; stop LSB
        call    _yma
        ld      c, #0x15
        ld      b, e                 ; stop MSB = track*8 | 7
        call    _yma
        ld      c, #0x19
        ld      b, #0x40             ; delta-N LSB (~18.5kHz)
        call    _yma
        ld      c, #0x1a
        ld      b, #0x55             ; delta-N MSB
        call    _yma
        ld      c, #0x1b
        ld      b, #0xff             ; volume
        call    _yma
        ld      c, #0x10
        ld      b, #0xb0             ; START | MEMDATA | REPEAT
        call    _yma
        ret

        ;; write B into YM2610 register C, address-set A (ports 4/5)
_yma:
        ld      a, c
        out     (0x04), a
        ld      a, #0x18
0$:     dec     a
        jr      nz, 0$
        ld      a, b
        out     (0x05), a
        ld      a, #0x18
1$:     dec     a
        jr      nz, 1$
        ret

        ;; write B into YM2610 register C, address-set B (ports 6/7)
_ymb:
        ld      a, c
        out     (0x06), a
        ld      a, #0x18
2$:     dec     a
        jr      nz, 2$
        ld      a, b
        out     (0x07), a
        ld      a, #0x18
3$:     dec     a
        jr      nz, 3$
        ret
