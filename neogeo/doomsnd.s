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
        SFXVOL = 0xf801              ; Z80 work-RAM byte: ADPCM-A pan+volume (reg 0x08+N) for the next SFX -- set per-SFX (positional) or 0xDF (full)
        SFXCH = 0xf802               ; Z80 work-RAM byte: round-robin ADPCM-A channel (0..5) -- POLYPHONY: each SFX takes the next of the 6 PCM channels so they overlap instead of cutting each other off

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
        ld      a, #0xdf
        ld      (SFXVOL), a          ; default SFX volume = full, both speakers
        xor     a
        ld      (SFXCH), a           ; round-robin SFX channel starts at 0
_idle:
        ld      a, (CMD)             ; pending command?
        or      a
        jr      z, _idle
        ld      b, a                 ; b = command (1=pistol, 2=scream, 3=start music)
        xor     a
        ld      (CMD), a             ; consume
        ld      a, b
        cp      #0x80                ; cmd >= 0x80 -> POSITIONAL SFX: 0x80|(idx<<4)|(vol<<2)|pan (distance volume + L/R pan)
        jp      nc, _possfx
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
        cp      #9
        jr      nz, _notdcls
        ld      b, #8                ; cmd 9  -> SFX index 7 (door CLOSE, DSDORCLS)
_notdcls:
        cp      #10
        jr      nz, _notpsta
        ld      b, #9                ; cmd 10 -> SFX index 8 (lift START, DSPSTART)
_notpsta:
        cp      #11
        jr      nz, _notpsto
        ld      b, #10               ; cmd 11 -> SFX index 9 (lift STOP, DSPSTOP)
_notpsto:
        ld      a, #0xdf
        ld      (SFXVOL), a          ; plain SFX (pistol/door/pickup/etc.) -> full volume, both speakers
        dec     b                    ; -> 0-based SFX index
        call    _sfx
        jr      _idle
_domusic:
        ld      b, #0                ; cmd 3 (legacy music start) = track 0 (E1M1)
        call    _music
        jr      _idle
        ;; --- POSITIONAL SFX: a = 0x80|(idx<<4)|(vol<<2)|pan -> set ADPCM-A ch1 pan+vol then play idx ---
_possfx:
        ld      c, a                 ; c = positional byte
        and     #0x0c                ; vol field (<<2): 0,4,8,12
        add     a, a                 ; <<1 -> 0,8,16,24
        add     a, #7                ; +7 -> 7,15,23,31 (ADPCM-A 5-bit level)
        ld      d, a                 ; d = volume
        ld      a, c
        and     #0x03                ; pan: bit1=L, bit0=R
        rrca
        rrca                         ; -> bits 7-6 (YM2610 reg 0x08 L/R output enable)
        or      d                    ; reg-0x08 value = pan | volume
        ld      (SFXVOL), a
        ld      a, c
        rrca
        rrca
        rrca
        rrca
        and     #0x07                ; idx = (byte>>4)&7
        ld      b, a
        call    _sfx
        jp      _idle

        ;; --- play SFX index B on a ROUND-ROBIN ADPCM-A channel 0..5 (set B) -> SFX overlap (polyphony) ---
_sfx:
        ld      a, b
        add     a, a
        add     a, a                 ; a = index*4 (table stride)
        ld      e, a
        ld      d, #0
        ld      hl, #_sfxtab
        add     hl, de               ; hl -> {startLSB, startMSB, stopLSB, stopMSB}
        ;; pick + advance the round-robin channel N (0..5); keep N in E (survives _ymb)
        ld      a, (SFXCH)
        ld      e, a                 ; E = N -> register offset for this channel
        inc     a
        cp      #6
        jr      c, 4$
        xor     a                    ; wrap 6 -> 0
4$:     ld      (SFXCH), a           ; the next SFX takes the next channel
        ld      a, #0x10
        add     a, e
        ld      c, a                 ; AN start addr LSB (0x10+N)
        ld      b, (hl)
        call    _ymb
        inc     hl
        ld      a, #0x18
        add     a, e
        ld      c, a                 ; AN start addr MSB (0x18+N)
        ld      b, (hl)
        call    _ymb
        inc     hl
        ld      a, #0x20
        add     a, e
        ld      c, a                 ; AN stop addr LSB (0x20+N)
        ld      b, (hl)
        call    _ymb
        inc     hl
        ld      a, #0x28
        add     a, e
        ld      c, a                 ; AN stop addr MSB (0x28+N)
        ld      b, (hl)
        call    _ymb
        ld      a, #0x08
        add     a, e
        ld      c, a                 ; AN pan + per-channel volume (0x08+N)
        ld      a, (SFXVOL)          ; per-SFX pan+volume (positional) or 0xDF (full); set by the dispatch
        ld      b, a
        call    _ymb
        ld      c, #0x01             ; ADPCM-A master volume (global)
        ld      b, #0x3f             ; max
        call    _ymb
        ;; key-on channel N: reg 0x00, value = (1<<N) (dump bit7=0)
        ld      a, #1
        ld      b, e
        inc     b                    ; loop shifts left exactly N times
5$:     dec     b
        jr      z, 6$
        add     a, a                 ; a <<= 1
        jr      5$
6$:     ld      b, a                 ; b = 1<<N = the key-on bit for channel N
        ld      c, #0x00
        call    _ymb
        ret

        ;; Addresses RECOMPUTED from the actual .adpcma sizes (256B blocks; music=512KB=0x800).
        ;; (was stale: scream/boom/itemup assumed smaller samples -> misaligned playback.)
        ;; pistol 11blk, scream 17, boom 37, itemup 5, door 27 -- inclusive stop = start+blk-1.
        ;; SFX now live AFTER the 28*512KB music (14MB: E1 9+intro, E2 9, E3 9) -> base 0xE000. MSB 0xE0.
        ;; (E4 maps reuse E1-E3 tracks at runtime; see main.c MUSIC[]. Re-run gen_sfxtab.py NTRACKS=N if the track count changes.)
_sfxtab:
        ;; GENERATED by tools/gen_sfxtab.py from the resampled (18518Hz) .adpcma sizes -- re-run it if SFX change.
        .db     0x00, 0xe0, 0x12, 0xe0   ; 1: pistol  0xE000-0xE012 (19 blk, DSPISTOL)
        .db     0x13, 0xe0, 0x2f, 0xe0   ; 2: scream  0xE013-0xE02F (29 blk, DSPOPAIN)
        .db     0x30, 0xe0, 0x6c, 0xe0   ; 3: boom    0xE030-0xE06C (61 blk, DSBAREXP)
        .db     0x6d, 0xe0, 0x74, 0xe0   ; 4: itemup  0xE06D-0xE074 (8 blk, DSITEMUP)
        .db     0x75, 0xe0, 0xa2, 0xe0   ; 5: door    0xE075-0xE0A2 (46 blk, DSDOROPN)
        .db     0xa3, 0xe0, 0xba, 0xe0   ; 6: impdth  0xE0A3-0xE0BA (24 blk, DSBGDTH1)
        .db     0xbb, 0xe0, 0xe5, 0xe0   ; 7: posdth  0xE0BB-0xE0E5 (43 blk, DSPODTH1)
        .db     0xe6, 0xe0, 0x13, 0xe1   ; 8: dorcls  0xE0E6-0xE113 (46 blk, DSDORCLS) -- door CLOSE
        .db     0x14, 0xe1, 0x2e, 0xe1   ; 9: pstart  0xE114-0xE12E (27 blk, DSPSTART) -- lift START
        .db     0x2f, 0xe1, 0x44, 0xe1   ; 10: pstop  0xE12F-0xE144 (22 blk, DSPSTOP) -- lift STOP

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
