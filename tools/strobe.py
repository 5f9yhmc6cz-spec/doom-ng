#!/usr/bin/env python3
"""A-B-A strobe metric over a /tmp/gpt_*.bmp capture run.
A pixel 'strobes' when it changes frame i->i+1 and changes BACK by i+2:
the signature of flicker (vs legitimate motion). Usage: strobe.py LABEL"""
import sys, glob
import numpy as np
from PIL import Image
fs=sorted(glob.glob('/tmp/gpt_*.bmp'))
ims=[np.asarray(Image.open(f).convert('L'),dtype=np.int16)[40:230,8:328] for f in fs]
sc=[]
for i in range(len(ims)-2):
    d1=np.abs(ims[i+1]-ims[i]); d2=np.abs(ims[i+2]-ims[i])
    sc.append(int(((d1>40)&(d2<20)).sum()))
lbl=sys.argv[1] if len(sys.argv)>1 else "RUN"
print(f"{lbl}: frames={len(sc)} strobe_sum={sum(sc)} mean={sum(sc)//max(1,len(sc))} max={max(sc) if sc else 0} "
      f"ev>500={sum(1 for v in sc if v>500)} ev>2000={sum(1 for v in sc if v>2000)}")
