#!/usr/bin/env python3
"""MUS (DMX) -> MIDI type-0. Faithful port of the canonical mus2mid (Chocolate Doom / Vavoom).
DOOM music lumps (D_E1Mn) are MUS; fluidsynth needs SMF. Division 70 + default 120BPM = MUS's 140Hz.
Usage: mus2mid.py in.mus out.mid"""
import sys, struct

# MUS controller index (0-14) -> MIDI controller number. idx 0 = instrument (-> program change, special).
CTRL = [0x00,0x00,0x01,0x07,0x0A,0x0B,0x5B,0x5D,0x40,0x43,0x78,0x7B,0x7E,0x7F,0x79]

def vlq(n):                       # MIDI variable-length quantity
    out=[n & 0x7F]; n>>=7
    while n: out.insert(0,(n & 0x7F)|0x80); n>>=7
    return bytes(out)

def mus2mid(mus):
    if mus[:4]!=b'MUS\x1a': raise ValueError("not a MUS lump")
    score_len, score_start = struct.unpack('<HH', mus[4:8])
    trk=bytearray()
    pos=score_start
    delta=0                       # MUS delay ticks accumulated -> next event's MIDI delta-time
    vel=[127]*16                  # last play-note velocity per MUS channel
    def chan(mc): return 9 if mc==15 else (mc if mc<9 else mc)  # MUS 15 = percussion -> MIDI 9
    def ev(*b): trk.extend(vlq(delta)); trk.extend(bytes(b))
    end=False
    while pos < len(mus) and not end:
        e=mus[pos]; pos+=1
        etype=(e>>4)&7; mc=e&0x0F; last=e&0x80; ch=chan(mc)
        if etype==0:                                  # release note
            note=mus[pos]&0x7F; pos+=1; ev(0x80|ch, note, 64)
        elif etype==1:                                # play note
            b=mus[pos]; pos+=1
            if b&0x80: vel[mc]=mus[pos]&0x7F; pos+=1
            ev(0x90|ch, b&0x7F, vel[mc])
        elif etype==2:                                # pitch bend (0-255, 128=centre) -> 14-bit
            b=mus[pos]; pos+=1; bend=b*64; ev(0xE0|ch, bend&0x7F, (bend>>7)&0x7F)
        elif etype==3:                                # system event (controllers 10-14)
            c=mus[pos]&0x7F; pos+=1; ev(0xB0|ch, CTRL[c], 0)
        elif etype==4:                                # change controller 0-9
            c=mus[pos]&0x7F; pos+=1; v=mus[pos]&0x7F; pos+=1
            if c==0: ev(0xC0|ch, v)                   # instrument -> program change
            else:    ev(0xB0|ch, CTRL[c], v)
        elif etype==6:                                # score end
            end=True
        # types 5/7 carry no data in DOOM MUS; ignore
        delta=0
        if last:                                      # read variable-length delay (140Hz ticks)
            t=0
            while True:
                d=mus[pos]; pos+=1; t=(t<<7)|(d&0x7F)
                if not (d&0x80): break
            delta=t
    trk.extend(vlq(0)); trk.extend(b'\xFF\x2F\x00')   # end of track
    # MIDI type-0 header: division 70 (with default 120BPM = 140Hz, matching MUS)
    hdr=b'MThd'+struct.pack('>IHHH',6,0,1,70)
    trkc=b'MTrk'+struct.pack('>I',len(trk))+bytes(trk)
    return hdr+trkc

if __name__=='__main__':
    data=open(sys.argv[1],'rb').read()
    open(sys.argv[2],'wb').write(mus2mid(data))
    print(f"{sys.argv[1]} -> {sys.argv[2]} ({len(data)}B MUS -> MIDI)")
