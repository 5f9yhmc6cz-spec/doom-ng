#!/usr/bin/env python3
# Extract ALL of Episode 1 (E1M1..E1M9) for the vertical-slice live renderer, BSP form -> neogeo/vs_e1.h.
#   Per map: SEGS (endpoints + front/back sector floor+ceil + upper/lower/mid texture ids + two-sided/sky
#            flags), SSECTORS (count, first seg), NODES (partition + child bboxes; root = last node), spawn.
# Geometry for every map is emitted as A1 POINTER TABLES: per-map arrays VE<name>_<m> + a pointer table
# VE<name>_MAP[VE_NMAP], plus per-map scalar arrays VE_*_MAP[VE_NMAP]. The renderer repoints file-scope
# ve_* pointers per map (vs_set_map) -> the hot path keeps a single indirection. Texture ids match the cart's
# ALL-E1 textile order (tools/vs_flats canonical union), shared with wad2c.py, so every map indexes the
# SAME C-ROM tiles. (The dead per-seg VESAL salience array is dropped -- it was never read at runtime.)
import struct, os, sys
WAD=os.environ.get("WAD","doom1.wad"); EPISODE=1
data=open(WAD,"rb").read()
_sig,nl,dofs=struct.unpack_from("<4sii",data,0)
D=[(struct.unpack_from("<ii8s",data,dofs+i*16)) for i in range(nl)]
D=[(nm.split(b'\0')[0].decode('latin1'),fp,sz) for (fp,sz,nm) in D]
# ALL-E1 ordering (shared with wad2c via tools/vs_flats): every map indexes the SAME C-ROM tiles
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vs_flats import e1_assets, flat_slot, _episodes
WALLS_E1,_FLATS,_NM=e1_assets(WAD)
texid={n:i for i,n in enumerate(WALLS_E1)}
def tid(n): return texid.get(n.upper(),-1) if (n and n!="-") else -1

def extract_map(MAP):
    mi=[k for k,d in enumerate(D) if d[0]==MAP][0]
    def lump(off): return D[mi+off][1],D[mi+off][2]
    TH=lump(1); LD=lump(2); SD=lump(3); VX=lump(4); SG=lump(5); SS=lump(6); ND=lump(7); SE=lump(8)
    verts=[struct.unpack_from("<hh",data,VX[0]+i*4) for i in range(VX[1]//4)]
    sectors=[]; sectag=[]
    for i in range(SE[1]//26):
        fl,ce=struct.unpack_from("<hh",data,SE[0]+i*26)
        fp=data[SE[0]+i*26+4 :SE[0]+i*26+12].split(b'\0')[0].decode('latin1')   # floorpic
        cp=data[SE[0]+i*26+12:SE[0]+i*26+20].split(b'\0')[0].decode('latin1')   # ceilpic
        sectors.append((fl,ce,fp,cp)); sectag.append(struct.unpack_from("<h",data,SE[0]+i*26+24)[0])   # sector TAG -> lift target lookup
    sides=[]
    for i in range(SD[1]//30):
        xo=struct.unpack_from("<h",data,SD[0]+i*30)[0]   # sidedef X texture offset -> wall-U texture alignment
        yo=struct.unpack_from("<h",data,SD[0]+i*30+2)[0] # sidedef Y texture offset (rowoffset) -> vertical peg (V companion 2b)
        up=data[SD[0]+i*30+4:SD[0]+i*30+12].split(b'\0')[0].decode('latin1')
        lo=data[SD[0]+i*30+12:SD[0]+i*30+20].split(b'\0')[0].decode('latin1')
        mid=data[SD[0]+i*30+20:SD[0]+i*30+28].split(b'\0')[0].decode('latin1')
        sec=struct.unpack_from("<h",data,SD[0]+i*30+28)[0]
        sides.append((up,lo,mid,sec,xo,yo))
    linedefs=[struct.unpack_from("<HHHHHHH",data,LD[0]+i*14) for i in range(LD[1]//14)]
    segs=[]; segtag=[]
    for i in range(SG[1]//12):
        v1,v2,ang,ld,side,off=struct.unpack_from("<HHhHHh",data,SG[0]+i*12)
        x0,y0=verts[v1]; x1,y1=verts[v2]
        L=linedefs[ld]; rs,ls=L[5],L[6]
        fsd = rs if side==0 else ls
        bsd = ls if side==0 else rs
        fside=sides[fsd]; fsec=sectors[fside[3]]
        ff,fc,ffp,fcp=fsec
        two = (bsd!=0xFFFF)
        if two:
            bsec=sectors[sides[bsd][3]]; bf,bc,bfp,bcp=bsec
            ut=tid(fside[0]); lt=tid(fside[1]); mt=-1
            sky=1 if fcp=="F_SKY1" else 0   # FRONT ceiling only: a window's room-side ceiling is NOT sky
            sky |= (2 if bcp=="F_SKY1" else 0)   # bit2=front sky (-> vs_sky band), bit3=back sky (-> sky through opening)
        else:
            bf=bc=0; ut=lt=-1; mt=tid(fside[2])
            if mt<0: mt=tid(fside[0])
            sky=1 if fcp=="F_SKY1" else 0
        ffl = flat_slot(ffp,WAD) & 0xFF                               # front FLOOR flat slot (shared order)
        cfl = 0xFF if fcp=="F_SKY1" else (flat_slot(fcp,WAD) & 0xFF)  # 0xFF = sky -> renderer leaves backdrop
        bfl = (flat_slot(bfp,WAD) & 0xFF) if two else 0xFF                              # BACK floor flat (the zone seen THROUGH a two-sided opening); 0xFF=none
        bcl = (flat_slot(bcp,WAD) & 0xFF) if (two and bcp!="F_SKY1") else 0xFF          # BACK ceil  flat (0xFF = sky/none -> no back ceiling drawn)
        door = 1 if (L[3] in DOORSP) else 0                                             # DOOR linedef special -> openable (raise the back sector's ceiling on use)
        exit_sw = 1 if (L[3] in EXITSP) else 0                                          # EXIT linedef special -> USE advances to the next map
        lift = 1 if (L[3] in LIFTSP) else 0                                             # LIFT/raise-floor special -> USE toggles the back sector's FLOOR up/down
        walk = 1 if (L[3] in WALKDOOR) else 0                                           # WALK-trigger door -> crossing the line opens the TAG-resolved door sector
        swd  = 1 if (L[3] in SWDOOR) else 0                                             # SWITCH-trigger door -> USE the switch opens the TAG-resolved door sector
        fsec = fside[3]; bsec = (sides[bsd][3] if two else 0xFFFF)                      # FRONT + BACK sector ids -> the runtime per-sector ceiling override (g_secdc)
        u0 = (fside[4] + off) % 256                                                     # WALL-U at vertex v1: sidedef xoffset + seg offset, mod 256 (a multiple of every texture width -> tcol%wt stays exact)
        ulen = int(round(((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5))                     # seg world length = U span in texture pixels (DOOM maps 1 unit = 1 px horizontally)
        yoff = fside[5]                                                                 # FRONT sidedef rowoffset (px, signed) -> vertical texture peg (one rowoffset feeds all 3 textures)
        peg  = (1 if (L[2]&0x8) else 0)|(2 if (L[2]&0x10) else 0)                       # linedef peg flags: DONTPEGTOP=0x8->bit0, DONTPEGBOTTOM=0x10->bit1 (V companion 2b)
        segs.append((x0,y0,x1,y1,ff,fc,bf,bc,mt,ut,lt,(1 if two else 0)|(sky<<1)|(door<<3)|(lift<<4)|(exit_sw<<5)|(walk<<6)|(swd<<7),ffl,cfl,bfl,bcl,fsec,bsec,0xFFFF,0,u0,ulen,yoff,peg))   # ...,use_sec,use_lo (resolved below), wall-U base, U span, Y peg offset, peg flags
        segtag.append(L[4])                                                            # the linedef TAG -> the lift/door-trigger's target sector
    # LIFT resolution: a lift linedef's TAG points to a remote sector (DOOM "lower lift"). Resolve per-seg
    # the TARGET sector + its drop = lowest-adjacent-floor - own-floor (<=0). Runtime lowers then raises it.
    tagsec={}
    for i,t in enumerate(sectag):
        if t: tagsec.setdefault(t,[]).append(i)
    lowfloor=[sectors[i][0] for i in range(len(sectors))]                               # lowest adjacent floor per sector (LIFT drop)
    INF=1<<30; nbceil=[INF]*len(sectors)                                                # lowest NEIGHBOUR ceiling per sector (DOOR opens to nbceil-4)
    for s in segs:
        if s[17]==0xFFFF: continue                                                     # one-sided -> no neighbour
        a,b=s[16],s[17]
        if a<len(sectors) and b<len(sectors):
            lowfloor[a]=min(lowfloor[a],sectors[b][0]); lowfloor[b]=min(lowfloor[b],sectors[a][0])
            nbceil[a]=min(nbceil[a],sectors[b][1]);     nbceil[b]=min(nbceil[b],sectors[a][1])
    for i,s in enumerate(segs):                                                        # resolve per-seg TAG -> target sector + delta (LIFT drop or DOOR raise)
        t=segtag[i]
        if not (t and t in tagsec): continue
        T=tagsec[t][0]
        if (s[11]&16):                                                                 # LIFT -> drop to lowest adjacent floor (<=0)
            segs[i]=s[:18]+(T, lowfloor[T]-sectors[T][0])+s[20:]   # keep U0/ULEN (indices 20,21)
        elif (s[11]&(64|128)):                                                         # WALK/SWITCH door trigger -> raise tagged ceiling to (lowest-neighbour - 4)
            oc=(nbceil[T]-4) if nbceil[T]<INF else (sectors[T][1]+72); rz=oc-sectors[T][1]
            if rz<8: rz=72
            segs[i]=s[:18]+(T, rz)+s[20:]                                              # use_sec=tagged door sector, use_lo=ceiling raise (>0); keep U0/ULEN
    ssecs=[struct.unpack_from("<HH",data,SS[0]+i*4) for i in range(SS[1]//4)]   # (count, first)
    nodes=[]; rbb=[]; lbb=[]
    for i in range(ND[1]//28):
        x,y,dx,dy=struct.unpack_from("<hhhh",data,ND[0]+i*28)
        rt,rbo,rl,rr=struct.unpack_from("<hhhh",data,ND[0]+i*28+8)    # right bbox: top,bottom,left,right
        ltp,lbo,ll,lr=struct.unpack_from("<hhhh",data,ND[0]+i*28+16)  # left  bbox
        rc,lc=struct.unpack_from("<HH",data,ND[0]+i*28+24)            # children (bit15 => subsector)
        nodes.append((x,y,dx,dy,rc,lc))
        rbb.append((rl,rbo,rr,rt)); lbb.append((ll,lbo,lr,ltp))       # store as (xmin,ymin,xmax,ymax)
    root=len(nodes)-1
    sx=sy=sa=0
    for t in range(TH[1]//10):
        tx,ty,a,typ,_=struct.unpack_from("<hhhhh",data,TH[0]+t*10)
        if typ==1: sx,sy,sa=tx,ty,a; break
    # floor-Z at (x,y): descend the BSP nodes to the containing subsector, take its first seg's FRONT floor
    # (mirrors the cart's vs_floor_at: cross>0 -> right child; child bit15 => subsector).
    def floor_z(tx,ty):
        n=root
        while not (n & 0x8000):
            nx,ny,ndx,ndy,rc,lc=nodes[n]
            cross=(tx-nx)*ndy-(ty-ny)*ndx
            n = rc if cross>0 else lc
        ss=n & 0x7fff
        return segs[ssecs[ss][1]][4] if ssecs[ss][0]>0 else 0   # front floor height of the subsector's first seg
    # THINGS for the live engine. THING_CLASS = single source of truth (also emits CLS_* + SPRCLASS order).
    # S2 keeps only BARRELS (2035). Filter: single-player (NOT flag 0x10) present on UV (flag 0x04).
    things=[]
    for t in range(TH[1]//10):
        tx,ty,ta,typ,fl=struct.unpack_from("<hhhhh",data,TH[0]+t*10)
        cls=THING_CLASS.get(typ)
        if cls is None: continue
        if (fl & 0x10) or not (fl & 0x04): continue        # drop multiplayer-only; keep UV/single-player
        things.append((tx,ty,(ta*256//360)&0xff,floor_z(tx,ty),cls))
    return dict(segs=segs,ssecs=ssecs,nodes=nodes,rbb=rbb,lbb=lbb,root=root,sx=sx,sy=sy,sa=sa,things=things,nsec=len(sectors))

# THING type -> class enum (single source of truth, emitted as CLS_* into vs_e1.h). S2 = barrels only;
# monsters/items get added in later milestones (M3/M5/L1) -- the cart's SPRCLASS[] table indexes this.
THING_CLASS={2035:0, 3001:1, 3004:2, 9:3,                 # barrel, imp, former-human(zombie), shotgun-guy(sergeant)
             2018:4, 2019:5, 2015:6,                       # ARMOUR: green(ARM1), blue(ARM2), helmet bonus(BON2)
             2007:7, 2048:8, 2008:9, 2049:10,              # AMMO: clip, bullet box, shells, shell box
             2010:11, 2046:12}                             # AMMO: rocket, rocket box
                                                            # classes 4..12 = "visible billboard" items (drawn, not collectible) -- see SPRCLASS in main.c.
                                                            # (CELL 2047 / CELP 17 omitted: no plasma/BFG ammo sprites in shareware doom1.wad)
DOORSP={1,26,27,28,31,32,33,34,117,118}      # DOOR linedef specials (manual/keyed/fast); used to flag openable segs
EXITSP={11,51,52,124}                         # EXIT linedef specials (S1/secret switch exit, W1/secret walk exit) -> USE advances to the next map
LIFTSP={10,21,62,88,120,121,122,123,18,22,69,95,119,128}   # LIFT + raise-floor specials -> USE toggles the back sector FLOOR up/down
WALKDOOR={2,4,86,90,105,106,108,109}                       # WALK-trigger OPEN-door specials (W1/WR) -> crossing the line raises the TAG-resolved door sector (open-stay)
SWDOOR={29,61,63,103,111,112,114,115}                      # SWITCH-trigger OPEN-door specials (S1/SR) -> USE the switch raises the TAG-resolved door sector (open-stay)
CLASS_NAMES=["CLS_BAR","CLS_IMP","CLS_POSS","CLS_SPOS",
             "CLS_ARM1","CLS_ARM2","CLS_BON2",
             "CLS_CLIP","CLS_AMMO","CLS_SHEL","CLS_SBOX",
             "CLS_ROCK","CLS_BROK"]

MAPS=[f"E{ep}M{m}" for ep in _episodes(None) for m in range(1,10) if any(d[0]==f"E{ep}M{m}" for d in D)]   # DOOMNG_EPISODES env: "1"=E1, "all"=E1-E4 (36 maps)
md=[extract_map(M) for M in MAPS]
NMAP=len(MAPS)
for i,M in enumerate(MAPS):
    print("// %s: %d segs, %d ssectors, %d nodes (root %d), start (%d,%d,%d)"
          %(M,len(md[i]['segs']),len(md[i]['ssecs']),len(md[i]['nodes']),md[i]['root'],md[i]['sx'],md[i]['sy'],md[i]['sa']))

def J(vals): return ",".join(str(v) for v in vals) if vals else "0"

# --- BANKED geometry: pack every map into a flat blob (neogeo/vs_geo.bin) placed in .text2/ROM2.
#     C-array pointer tables can't bank (linker VMAs past 0x2FFFFF aren't runtime-addressable), so the
#     geometry is a blob accessed by offset. vs_set_map(m): P_ROM_SWITCH_BANK(VE_BANK[m]) then walk the
#     ve_* pointers from (0x200000 + VE_OFF[m]). Layout per map (MUST match vs_set_map in main.c): every
#     16-bit array first (big-endian = m68k pre-swab order), then every 8-bit array -> shorts stay
#     word-aligned. Maps are bank-PACKED so none straddles a 1MB (0x100000) boundary; each base is even.
BANK=0x100000
def _pack(m):
    S=m['segs']; SS=m['ssecs']; ND=m['nodes']; TH=m['things']; b=bytearray()
    w=lambda v: b.extend(struct.pack(">H", v & 0xFFFF))                 # 16-bit big-endian
    for ci in (0,1,2,3,4,5,6,7,8,9,10,16,17,18,19,21,22):              # X0..LT,FSEC,BSEC,USESEC,USELO,ULEN,YOFF (xNSEG)
        for s in S: w(s[ci])
    for ci in (0,1):                                                    # SSC,SSF (xNSS)
        for s in SS: w(s[ci])
    for ci in (0,1,2,3,4,5):                                            # NX,NY,NDX,NDY,NR,NL (xNNODE)
        for n in ND: w(n[ci])
    for key in ('rbb','lbb'):                                           # NRB,NLB (x4*NNODE)
        for box in m[key]:
            for v in box: w(v)
    for ci in (0,1,3):                                                  # THX,THY,THZ (xNTH)
        for t in TH: w(t[ci])
    for ci in (11,12,13,14,15,20,23):                                  # FLAG,FFL,CFL,BFL,BCL,U0,PEG (xNSEG)
        for s in S: b.append(s[ci] & 0xFF)
    for ci in (2,4):                                                    # THA,THC (xNTH)
        for t in TH: b.append(t[ci] & 0xFF)
    if len(b)&1: b.append(0)                                            # even-pad: keep the next map's shorts aligned
    return bytes(b)

blob=bytearray(); VE_BANK=[]; VE_OFF=[]; _bk=0; _off=0
for m in md:
    mb=_pack(m)
    assert len(mb)<=BANK, "a single map's geometry exceeds one 1MB bank"
    if _off+len(mb)>BANK: blob.extend(b'\xff'*(BANK-_off)); _bk+=1; _off=0   # don't straddle a bank boundary
    VE_BANK.append(_bk); VE_OFF.append(_off); blob.extend(mb); _off+=len(mb)
NBANK=_bk+1
blob.extend(b'\xff'*(NBANK*BANK-len(blob)))                                   # pad to whole 1MB banks
open("neogeo/vs_geo.bin","wb").write(blob)

# self-check: reconstruct from the blob and assert == the in-memory extraction, for EVERY map
# (catches endianness / array-order / bank-offset bugs before the blob ever reaches the cart).
def _rd16(o): return struct.unpack(">H", blob[o:o+2])[0]
for mi,m in enumerate(md):
    base=VE_BANK[mi]*BANK+VE_OFF[mi]; ns=len(m['segs']); nss=len(m['ssecs']); nn=len(m['nodes']); nth=len(m['things'])
    for i in range(ns):                                                   # X0 = first 16-bit array (tests endianness + base)
        assert _rd16(base+2*i)==(m['segs'][i][0]&0xFFFF), "X0 mismatch map %d seg %d"%(mi,i)
    nshort=17*ns + 2*nss + 14*nn + 3*nth                                  # all 16-bit elements before the byte section
    bb=base+2*nshort
    for i in range(ns):                                                   # FLAG = first byte array (tests the section split)
        assert blob[bb+i]==(m['segs'][i][11]&0xFF), "FLAG mismatch map %d seg %d"%(mi,i)
    if nth:                                                               # THC = last byte array (tests the full block length)
        thc=bb+7*ns+nth
        for i in range(nth): assert blob[thc+i]==(m['things'][i][4]&0xFF), "THC mismatch map %d thing %d"%(mi,i)
print("self-check OK: blob round-trips for all %d maps"%NMAP)

with open("neogeo/vs_e1.h","w") as f:
    f.write("/* generated by tools/vs_extract.py -- BANKED geometry (neogeo/vs_geo.bin -> .text2/ROM2).\n")
    f.write("   vs_set_map(m): P_ROM_SWITCH_BANK(VE_BANK[m]); walk ve_* from (0x200000 + VE_OFF[m]).\n")
    f.write("   Blob order per map: 16-bit [X0 Y0 X1 Y1 FF FC BF BC MT UT LT FSEC BSEC USESEC USELO ULEN YOFF]xNSEG,\n")
    f.write("   [SSC SSF]xNSS, [NX NY NDX NDY NR NL]xNNODE, [NRB NLB]x4NNODE, [THX THY THZ]xNTH;\n")
    f.write("   then 8-bit [FLAG FFL CFL BFL BCL U0 PEG]xNSEG, [THA THC]xNTH. */\n")
    f.write("#define VE_NMAP %d\n"%NMAP)
    f.write("#define VE_NBANK %d\n"%NBANK)
    f.write("#define VE_MAXSEG %d\n"%max(len(m['segs']) for m in md))
    f.write("#define VE_MAXSS %d\n"%max(len(m['ssecs']) for m in md))
    f.write("#define VE_MAXNODE %d\n"%max(len(m['nodes']) for m in md))
    f.write("#define VE_MAXSEC %d\n"%max(m['nsec'] for m in md))
    f.write("#define VE_MAXTH %d\n"%max(1,max(len(m['things']) for m in md)))
    f.write("static const short VE_NSEG_MAP[VE_NMAP]={%s};\n"%J([len(m['segs']) for m in md]))
    f.write("static const short VE_NSEC_MAP[VE_NMAP]={%s};\n"%J([m['nsec'] for m in md]))
    f.write("static const short VE_NSS_MAP[VE_NMAP]={%s};\n"%J([len(m['ssecs']) for m in md]))
    f.write("static const short VE_NNODE_MAP[VE_NMAP]={%s};\n"%J([len(m['nodes']) for m in md]))
    f.write("static const short VE_NTH_MAP[VE_NMAP]={%s};\n"%J([len(m['things']) for m in md]))
    f.write("static const short VE_ROOT_MAP[VE_NMAP]={%s};\n"%J([m['root'] for m in md]))
    f.write("static const short VE_SX_MAP[VE_NMAP]={%s};\n"%J([m['sx'] for m in md]))
    f.write("static const short VE_SY_MAP[VE_NMAP]={%s};\n"%J([m['sy'] for m in md]))
    f.write("static const short VE_SA_MAP[VE_NMAP]={%s};\n"%J([m['sa'] for m in md]))
    f.write("static const unsigned char VE_BANK[VE_NMAP]={%s};\n"%J(VE_BANK))
    f.write("static const unsigned long VE_OFF[VE_NMAP]={%s};\n"%J(VE_OFF))
    for i,nm in enumerate(CLASS_NAMES): f.write("#define %s %d\n"%(nm,i))
print("wrote neogeo/vs_e1.h + vs_geo.bin: %d maps, %d bank(s), blob %.2f MB"%(NMAP,NBANK,len(blob)/1048576))
print("  -> set PROM2SIZE=%d (0x%X) in neogeo/rom.mk"%(NBANK*BANK,NBANK*BANK))
print("  things/map: "+", ".join("%s=%d"%(M,len(md[i]['things'])) for i,M in enumerate(MAPS)))
