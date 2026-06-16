#!/usr/bin/env python3
"""Game-frame duration distribution from a per-emulator-frame capture.
Counts runs of visually identical frames (geometry region) within the MOVING
window (pre-park). Usage: fpsdur.py LABEL"""
import sys, glob
import numpy as np
from PIL import Image
fs=sorted(glob.glob('/tmp/gpt_*.bmp'))
arrs=[np.asarray(Image.open(f).convert('L'),dtype=np.int16) for f in fs]
lastmove=0; prev=None
for i,a in enumerate(arrs):
    g=a[40:140,8:328]
    if prev is not None and (np.abs(g-prev)>30).sum()>300: lastmove=i
    prev=g
mv=arrs[:max(lastmove,2)]
durs=[]; cur=1
for i in range(1,len(mv)):
    if (np.abs(mv[i][40:230,8:328]-mv[i-1][40:230,8:328])>30).sum()<200: cur+=1
    else: durs.append(cur); cur=1
d=np.array(durs) if durs else np.array([1])
lbl=sys.argv[1] if len(sys.argv)>1 else "RUN"
print(f"{lbl}: moving frames={len(d)} median={int(np.median(d))} p90={int(np.percentile(d,90))} max={d.max()} "
      f"-> worst fps={60.0/d.max():.1f}; park at emu-frame index {lastmove}")
