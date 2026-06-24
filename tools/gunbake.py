#!/usr/bin/env python3
# Bake the first-person WEAPON sprites (DOOM "ready"/A0 frames) into NeoGeo FIX-layer tiles, so the
# cart can draw the gun-hand on the fix layer at ZERO sprite cost. Shareware doom1.wad has 6 weapons
# (no plasma/BFG weapon sprites). Output: neogeo/gunhand.fix (concatenated 32B fix tiles, row-major per
# weapon) + neogeo/gunhand.h (GUNHAND_BASE/N, per-weapon {base,wt,ht,xoff,yoff} + per-weapon 16-col pals).
# Run:  python3 tools/gunbake.py [doom1.wad]        (writes the bank)
#       python3 tools/gunbake.py doom1.wad --verify  (also dumps /tmp/gunbake_verify.png to eyeball it)
import struct, sys, os
HERE=os.path.dirname(os.path.abspath(__file__)); ROOT=os.path.join(HERE,"..")
NG=os.path.join(ROOT,"neogeo")
WAD=os.environ.get("WAD") or (sys.argv[1] if len(sys.argv)>1 and not sys.argv[1].startswith("--") else os.path.join(ROOT,"doom1.wad"))
VERIFY="--verify" in sys.argv

# weapon ready-frames, in ARMS slot order (1=fist/saw, 2=pistol, ...). plasma/BFG absent in shareware.
WEAPONS=[("FIST","PUNGA0"),("PISTOL","PISGA0"),("SHOTGUN","SHTGA0"),
         ("CHAINGUN","CHGGA0"),("ROCKET","MISGA0"),("CHAINSAW","SAWGA0"),
         ("PLASMA","PLSGA0"),("BFG","BFGGA0")]   # registered/Ultimate doom.wad adds plasma rifle + BFG9000 (absent in shareware)

data=open(WAD,"rb").read()
_,nl,diro=struct.unpack_from("<4sii",data,0)
DIR=[struct.unpack_from("<ii8s",data,diro+i*16) for i in range(nl)]
NAMES=[d[2].rstrip(b"\0").decode("latin1") for d in DIR]
def lump(name):
    i=[j for j,x in enumerate(NAMES) if x==name][0]; off,sz=DIR[i][0],DIR[i][1]; return data[off:off+sz]
pi=[i for i,x in enumerate(NAMES) if x=="PLAYPAL"][0]; PP=data[DIR[pi][0]:DIR[pi][0]+768]
RGB=[(PP[i*3],PP[i*3+1],PP[i*3+2]) for i in range(256)]

def parse_patch(b):
    w,h,lo,to=struct.unpack_from("<hhhh",b,0)
    colofs=struct.unpack_from("<%di"%w,b,8)
    grid=[[-1]*w for _ in range(h)]
    for x in range(w):
        o=colofs[x]
        while True:
            top=b[o]; o+=1
            if top==0xFF: break
            ln=b[o]; o+=2          # length, then 1 unused byte
            for k in range(ln):
                y=top+k
                if 0<=y<h: grid[y][x]=b[o]
                o+=1
            o+=1                   # trailing unused byte
    return w,h,lo,to,grid

def quantize15(grid):             # median-cut to <=15 distinct PLAYPAL indices, in place
    from collections import Counter
    cnt=Counter(p for row in grid for p in row if p>=0)
    if len(cnt)<=15: return
    def axr(box): return [max(p[a] for p in box)-min(p[a] for p in box) for a in range(3)]
    pts=[[RGB[i][0],RGB[i][1],RGB[i][2],cnt[i],i] for i in cnt]; boxes=[pts]
    while len(boxes)<15:
        si,best=-1,-1
        for k,bx in enumerate(boxes):
            if len(bx)<2: continue
            v=max(axr(bx))*sum(p[3] for p in bx)
            if v>best: best,si=v,k
        if si<0: break
        bx=boxes.pop(si); ax=axr(bx).index(max(axr(bx))); bx.sort(key=lambda p:p[ax])
        tot=sum(p[3] for p in bx); acc=0; cut=1
        for k in range(len(bx)):
            acc+=bx[k][3]
            if acc*2>=tot: cut=max(1,min(len(bx)-1,k+1)); break
        boxes.append(bx[:cut]); boxes.append(bx[cut:])
    rep={}
    for bx in boxes:
        tw=sum(p[3] for p in bx) or 1
        cr=sum(p[0]*p[3] for p in bx)/tw; cg=sum(p[1]*p[3] for p in bx)/tw; cb=sum(p[2]*p[3] for p in bx)/tw
        ridx=min(bx,key=lambda p:(p[0]-cr)**2+(p[1]-cg)**2+(p[2]-cb)**2)[4]
        for p in bx: rep[p[4]]=ridx
    for row in grid:
        for x in range(len(row)):
            if row[x]>=0: row[x]=rep[row[x]]

def ngc(c):
    r,g,b=RGB[c]; r5,g5,b5=r>>3,g>>3,b>>3
    return ((r5&1)<<14)|((g5&1)<<13)|((b5&1)<<12)|((r5>>1)<<8)|((g5>>1)<<4)|(b5>>1)

def enc_cell(px):                 # 64 indices (0=transparent) -> 32B NG fix tile (the wad2c swizzle)
    out=bytearray(32); k=0
    for xa,xb in ((4,5),(6,7),(0,1),(2,3)):
        for y in range(8):
            out[k]=(px[8*y+xa]&0xF)|((px[8*y+xb]&0xF)<<4); k+=1
    return bytes(out)

NBOB=2; BOB_STEP=2                                 # vertical bob: 2 baked phases (was 3) so all 8 weapons fit the 4096-tile fix layer; cart clamps 0-1-2-1 -> 0-1-1-1
tiles=[]; meta=[]; pals=[]
for tag,lname in WEAPONS:
    w,h,lo,to,grid=parse_patch(lump(lname))
    quantize15(grid)
    used=sorted(set(p for row in grid for p in row if p>=0))[:15]
    pidx={c:i+1 for i,c in enumerate(used)}       # 1..15 ; 0 = transparent
    wt=(w+7)//8; ht=(h+7)//8 + 1; base=len(tiles)  # +1 row of headroom so the gun can rise within its tile region
    for p in range(NBOB):                          # phase p raises the (bottom-anchored) gun by p*BOB_STEP px
        voff=ht*8 - h - p*BOB_STEP
        for cy in range(ht):
            for cx in range(wt):
                cell=[]
                for y in range(8):
                    for x in range(8):
                        gy=(cy*8+y)-voff; gx=cx*8+x
                        c=grid[gy][gx] if (0<=gy<h and 0<=gx<w) else -1
                        cell.append(0 if c<0 else pidx[c])
                tiles.append(enc_cell(cell))        # phase stride = wt*ht tiles
    meta.append((tag,base,wt,ht,lo,to,w,h))
    p16=[0]+[ngc(c) for c in used]; p16+=[0]*(16-len(p16)); pals.append(p16)
    print("  %-9s %3dx%-3d -> %dx%d tiles x%d phases, %2d colours" % (tag,w,h,wt,ht,NBOB,len(used)))

nhud=os.path.getsize(os.path.join(NG,"hudfix.fix"))//32 if os.path.exists(os.path.join(NG,"hudfix.fix")) else 186
BASE=1280+nhud                                     # fix tile id of the first weapon tile (after font@..1280 + hudfix)
open(os.path.join(NG,"gunhand.fix"),"wb").write(b"".join(tiles))
with open(os.path.join(NG,"gunhand.h"),"w") as f:
    f.write("/* generated by tools/gunbake.py -- %d weapon ready-frames as FIX-layer tiles (zero sprite cost) */\n"%len(meta))
    f.write("#define GUNHAND_N %d\n#define GUNHAND_BASE %d\n#define GUNHAND_NTILES %d\n#define GUNHAND_NBOB %d\n"%(len(meta),BASE,len(tiles),NBOB))
    f.write("/* per weapon: %d vertical bob phases baked back-to-back; phase p tiles at base + p*(wt*ht). ht includes 1 headroom row. */\n"%NBOB)
    f.write("/* per weapon: base tile (rel to GUNHAND_BASE), w/h in 8x8 tiles, sprite x/y offset, px w/h */\n")
    f.write("typedef struct { unsigned short base,wt,ht; short xoff,yoff,pw,ph; } gunhand_t;\n")
    f.write("static const gunhand_t GUNHAND[GUNHAND_N]={\n")
    for tag,base,wt,ht,lo,to,w,h in meta:
        f.write("  {%d,%d,%d,%d,%d,%d,%d}, /* %s */\n"%(base,wt,ht,lo,to,w,h,tag))
    f.write("};\n")
    f.write("static const unsigned short GUNHAND_PAL16[GUNHAND_N][16]={\n")
    for p in pals: f.write("  {%s},\n"%",".join(str(x) for x in p))
    f.write("};\n")
print("gunhand: %d weapons, %d tiles -> gunhand.fix + gunhand.h (BASE=%d, after %d hudfix tiles)"%(len(meta),len(tiles),BASE,nhud))

if VERIFY:
    from PIL import Image
    # decode tiles back to RGB via each weapon's palette and lay the weapons out side by side
    def dec_tile(t):              # 32B -> 64 indices (inverse swizzle)
        px=[0]*64; k=0
        for xa,xb in ((4,5),(6,7),(0,1),(2,3)):
            for y in range(8):
                px[8*y+xa]=t[k]&0xF; px[8*y+xb]=(t[k]>>4)&0xF; k+=1
        return px
    def ng2rgb(v):
        r=(((v>>8)&0xF)<<1)|((v>>14)&1); g=(((v>>4)&0xF)<<1)|((v>>13)&1); b=((v&0xF)<<1)|((v>>12)&1)
        return (r*8,g*8,b*8)
    cells=[]
    for wi,(tag,base,wt,ht,lo,to,w,h) in enumerate(meta):
        img=Image.new("RGB",(wt*8,ht*8),(40,40,60))
        pal=[ (0,0,0) ]+[ng2rgb(pals[wi][i]) for i in range(1,16)]
        for cy in range(ht):
            for cx in range(wt):
                px=dec_tile(tiles[base+cy*wt+cx])
                for y in range(8):
                    for x in range(8):
                        idx=px[8*y+x]
                        if idx: img.putpixel((cx*8+x,cy*8+y),pal[idx])
        cells.append(img)
    H=max(c.height for c in cells); W=sum(c.width+8 for c in cells)
    sheet=Image.new("RGB",(W,H),(20,20,20)); x=0
    for c in cells: sheet.paste(c,(x,H-c.height)); x+=c.width+8
    sheet=sheet.resize((W*2,H*2),Image.NEAREST); sheet.save("/tmp/gunbake_verify.png")
    print("verify -> /tmp/gunbake_verify.png")
