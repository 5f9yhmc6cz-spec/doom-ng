#!/usr/bin/env python3
"""BSP TRAVERSAL TRACER -- understand (and tune) the live VSLICE BSP walk.

Replicates neogeo/main.c vs_render's front-to-back BSP traversal + cull ladder for a
camera pose + the dials you set on the HUD, then dumps SPREADSHEETS:

  bsp_tree.csv   the tree itself (every node: split, child bboxes, children, depth)
  bsp_trace.csv  the traversal STEP BY STEP (each bbox test: which cull STAGE hit it,
                 pass/cull + reason, the depth/screen span, and the COST in MULs/cycles)
  bsp_trace.svg  a map view: walls, the camera + frustum, VISITED vs CULLED regions
  (stdout)       a per-stage cost ROLLUP -- where the walk spends its budget

FIDELITY (verified against main.c):
  * node/bbox/child parse mirrors tools/vs_extract.py (bbox stored xmin,ymin,xmax,ymax;
    child bit15 => subsector; root = last node).
  * the side-pick  cross=(px-nx)*ndy-(py-ny)*ndx ; cross>0 => RIGHT child is NEAR  (main.c ~1287)
  * push FAR then NEAR  => near pops first => front-to-back                          (main.c ~1291)
  * vs_bbox_vis cull ladder reproduced exactly: inside-box, 2-MUL far-cull, ~4-MUL
    frustum side-reject (Phase 1), then the ~24-MUL 4-corner project (allR/allL/dmin). (main.c ~1048)
  * Phase 2 walk budget: the loop halts when bx (node-box tests) hits --bxc.          (main.c ~1283)
  * sine table + g_rf reciprocal + FOCAL/HALF copied verbatim from main.c.

LIMITATION (v1): the per-column OCCLUSION prune (the vs_ct/vs_cb "covered column still
open?" tail of vs_bbox_vis, and the radial column-close) is NOT modeled -- that needs the
full per-seg emit. So a node the geometry-culls survive is reported VISIBLE even if a nearer
wall would have occluded it. => node/visible counts are UPPER BOUNDS; the geometric culls
(far / frustum-side / off-screen) are EXACT. v2 = simulate the column buffer for true occlusion.

Run:  python3 tools/bsp_trace.py E1M1 --wad doom1.wad --pos -179 -3215 26 --dd 750 --near 16 --bxc 2048 --ncull 1
      python3 tools/bsp_trace.py E1M1                      # spawn pose, default dials
"""
import struct, sys, os, math, csv

# ---- constants copied verbatim from neogeo/main.c ----
VS_SIN=[0,6,13,19,25,31,38,44,50,56,62,68,74,80,86,92,98,104,109,115,121,126,132,137,142,147,152,157,162,167,172,177,
181,185,190,194,198,202,206,209,213,216,220,223,226,229,231,234,237,239,241,243,245,247,248,250,251,252,253,254,255,255,256,256,
256,256,256,255,255,254,253,252,251,250,248,247,245,243,241,239,237,234,231,229,226,223,220,216,213,209,206,202,198,194,190,185,
181,177,172,167,162,157,152,147,142,137,132,126,121,115,109,104,98,92,86,80,74,68,62,56,50,44,38,31,25,19,13,6,
0,-6,-13,-19,-25,-31,-38,-44,-50,-56,-62,-68,-74,-80,-86,-92,-98,-104,-109,-115,-121,-126,-132,-137,-142,-147,-152,-157,-162,-167,-172,-177,
-181,-185,-190,-194,-198,-202,-206,-209,-213,-216,-220,-223,-226,-229,-231,-234,-237,-239,-241,-243,-245,-247,-248,-250,-251,-252,-253,-254,-255,-255,-256,-256,
-256,-256,-256,-255,-255,-254,-253,-252,-251,-250,-248,-247,-245,-243,-241,-239,-237,-234,-231,-229,-226,-223,-220,-216,-213,-209,-206,-202,-198,-194,-190,-185,
-181,-177,-172,-167,-162,-157,-152,-147,-142,-137,-132,-126,-121,-115,-109,-104,-98,-92,-86,-80,-74,-68,-62,-56,-50,-44,-38,-31,-25,-19,-13,-6]
VS_FOCAL=160; VS_HALF=160; VS_RFMAX=1536
VS_HOR=112; VS_LBT=0; VS_LBB=192; VS_EYE=41   # play-band horizon/top/bottom + eye height above floor (main.c)
def VS_SN(a): return VS_SIN[a & 255]
def VS_CS(a): return VS_SIN[(a+64) & 255]
def rf(d):  return (VS_FOCAL<<8)//d if (1<=d<VS_RFMAX) else ((VS_FOCAL<<8)//d if d>0 else 0)   # g_rf LUT / divide fallback

# ---- 68000 cost model (estimates; relative cost is the insight) ----
MUL_CYC=70      # MULS.W 16x16
DIVW_CYC=140    # DIVU.W 32/16 (the g_rf far fallback)
FRAME_CYC=200000  # ~ one 60fps frame budget

# ===================== WAD / BSP load (mirrors tools/vs_extract.py) =====================
def load(wad, mapn):
    data=open(wad,"rb").read()
    _sig,nl,dofs=struct.unpack_from("<4sii",data,0)
    D=[struct.unpack_from("<ii8s",data,dofs+i*16) for i in range(nl)]
    names=[d[2].rstrip(b"\0").decode("latin1") for d in D]
    if mapn not in names: sys.exit("map %s not in %s (have: %s)"%(mapn,wad,", ".join(n for n in names if n.startswith("E"))[:120]))
    mi=names.index(mapn)
    def lump(want):
        for j in range(mi+1,mi+12):
            if j<len(names) and names[j]==want: return D[j][0],D[j][1]
        return None
    VX=lump("VERTEXES"); SG=lump("SEGS"); SS=lump("SSECTORS"); ND=lump("NODES"); TH=lump("THINGS")
    if not (VX and SG and SS and ND): sys.exit("map %s missing BSP lumps (GL-only / UDMF?)"%mapn)
    verts=[struct.unpack_from("<hh",data,VX[0]+i*4) for i in range(VX[1]//4)]
    LD=lump("LINEDEFS"); SD=lump("SIDEDEFS"); SC=lump("SECTORS")
    linedefs=[struct.unpack_from("<HHHHHHH",data,LD[0]+i*14) for i in range(LD[1]//14)] if LD else []
    sidesec=[struct.unpack_from("<h",data,SD[0]+i*30+28)[0] for i in range(SD[1]//30)] if SD else []   # sidedef -> sector id
    sectors=[struct.unpack_from("<hh",data,SC[0]+i*26) for i in range(SC[1]//26)] if SC else []         # sector -> (floorh, ceilh)
    def sh(sd):   # sidedef -> (floor,ceil) heights
        if sd==0xFFFF or sd>=len(sidesec): return (0,0)
        sec=sidesec[sd]; return sectors[sec] if 0<=sec<len(sectors) else (0,0)
    segends=[]; segtwo=[]; seghts=[]   # segtwo=1 if TWO-SIDED (opening); seghts=(ff,fc,bf,bc) front/back floor+ceil
    for i in range(SG[1]//12):
        v1,v2,ang,ld,side,off=struct.unpack_from("<HHhHHh",data,SG[0]+i*12)
        if v1<len(verts) and v2<len(verts): segends.append((verts[v1],verts[v2]))
        else: segends.append(((0,0),(0,0)))
        two=0; ff=fc=bf=bc=0
        if ld<len(linedefs):
            L=linedefs[ld]; rs,ls=L[5],L[6]
            fsd = rs if side==0 else ls; bsd = ls if side==0 else rs   # front / back sidedef (mirrors vs_extract)
            (ff,fc)=sh(fsd); two = 1 if bsd!=0xFFFF else 0
            if two: (bf,bc)=sh(bsd)
        segtwo.append(two); seghts.append((ff,fc,bf,bc))
    ssecs=[struct.unpack_from("<HH",data,SS[0]+i*4) for i in range(SS[1]//4)]   # (count, first)
    nodes=[]; rbb=[]; lbb=[]
    for i in range(ND[1]//28):
        x,y,dx,dy=struct.unpack_from("<hhhh",data,ND[0]+i*28)
        rt,rbo,rl,rr=struct.unpack_from("<hhhh",data,ND[0]+i*28+8)     # right bbox top,bottom,left,right
        ltp,lbo,ll,lr=struct.unpack_from("<hhhh",data,ND[0]+i*28+16)   # left  bbox
        rc,lc=struct.unpack_from("<HH",data,ND[0]+i*28+24)             # children (bit15 => subsector)
        nodes.append((x,y,dx,dy,rc,lc))
        rbb.append((rl,rbo,rr,rt)); lbb.append((ll,lbo,lr,ltp))        # store xmin,ymin,xmax,ymax (engine order)
    root=len(nodes)-1
    sx=sy=sa=0
    for t in range(TH[1]//10):
        tx,ty,a,typ,_=struct.unpack_from("<hhhhh",data,TH[0]+t*10)
        if typ==1: sx,sy,sa=tx,ty,(a*256//360)&255; break    # P1 start (DOOM deg -> byte angle, like vs_set_map)
    # tree depth per node (root=0), via one descent of the structure
    depth={}
    def setd(n,d):
        if n & 0x8000: depth[n]=d; return
        depth[n]=d; setd(nodes[n][4],d+1); setd(nodes[n][5],d+1)
    setd(root,0)
    return dict(verts=verts,segends=segends,segtwo=segtwo,seghts=seghts,ssecs=ssecs,nodes=nodes,rbb=rbb,lbb=lbb,root=root,sx=sx,sy=sy,sa=sa,depth=depth)

def floor_at(M, x, y):   # BSP-descend to the containing subsector -> its first seg's front floor (mirrors vs_floor_at)
    n=M['root']
    while not (n & 0x8000):
        nx,ny,ndx,ndy,rc,lc=M['nodes'][n]; n = rc if (x-nx)*ndy-(y-ny)*ndx>0 else lc
    ss=n&0x7fff; cnt,first=(M['ssecs'][ss] if ss<len(M['ssecs']) else (0,0))
    return M['seghts'][first][0] if cnt>0 else 0

# ===================== seg -> screen columns (mirrors vs_render_seg ~905-923) =====================
def seg_cols(a, b, S):
    """Project a seg's endpoints to a screen-column range [ca,cb], or None if culled/back-facing.
       Used by the occlusion sim: a ONE-SIDED (solid) seg closes these columns front-to-back."""
    px,py=S['px'],S['py']; fcs,fsn=S['fcs'],S['fsn']; near=S['near']; ncol=S['ncol']
    ax,ay=a[0]-px,a[1]-py; bx,by=b[0]-px,b[1]-py
    ad=(ax*fcs+ay*fsn)>>8; bd=(bx*fcs+by*fsn)>>8
    if ad<near and bd<near: return None                       # both behind near plane
    aS=(ax*fsn-ay*fcs)>>8; bS=(bx*fsn-by*fcs)>>8
    if aS*VS_FOCAL> 160*ad and bS*VS_FOCAL> 160*bd: return None   # wholly off right
    if aS*VS_FOCAL<-160*ad and bS*VS_FOCAL<-160*bd: return None   # wholly off left
    if ad<near:                                               # near-clip vertex A
        den=bd-ad
        if den<=0: return None
        t=((near-ad)<<8)//den; aS+=((bS-aS)*t)>>8; ad=near
    elif bd<near:
        den=ad-bd
        if den<=0: return None
        t=((near-bd)<<8)//den; bS+=((aS-bS)*t)>>8; bd=near
    sxa=VS_HALF+((aS*rf(ad))>>8); sxb=VS_HALF+((bS*rf(bd))>>8)
    if sxa>=sxb: return None                                  # back-facing (BSP segs are one-directional)
    if sxb<0 or sxa>319: return None
    sa=max(0,sxa); sb=min(320,sxb)
    return (min(ncol-1, sa*ncol//320), min(ncol-1, sb*ncol//320))

def occlude_seg(a, b, hts, two, S):
    """Replicate vs_render_seg's per-column occlusion (main.c 1024-1052): close columns behind this seg.
       one-sided -> close; two-sided -> pinch the open [ct,cb] to the opening, count strips, and CLOSE when
       the strip count hits the dc depth-cap (the dominant pruner) OR the opening collapses (opaque)."""
    px,py=S['px'],S['py']; fcs,fsn=S['fcs'],S['fsn']; near=S['near']; ncol=S['ncol']; eye=S['eye']; dcap=S['dcap']
    vct=S['vct']; vcb=S['vcb']; vns=S['vns']
    ax,ay=a[0]-px,a[1]-py; bx,by=b[0]-px,b[1]-py
    ad=(ax*fcs+ay*fsn)>>8; bd=(bx*fcs+by*fsn)>>8
    if ad<near and bd<near: return
    aS=(ax*fsn-ay*fcs)>>8; bS=(bx*fsn-by*fcs)>>8
    if aS*VS_FOCAL> 160*ad and bS*VS_FOCAL> 160*bd: return
    if aS*VS_FOCAL<-160*ad and bS*VS_FOCAL<-160*bd: return
    ff,fc,bf,bc=hts
    if ad<near:
        den=bd-ad
        if den<=0: return
        t=((near-ad)<<8)//den; aS+=((bS-aS)*t)>>8; ad=near
    elif bd<near:
        den=ad-bd
        if den<=0: return
        t=((near-bd)<<8)//den; bS+=((aS-bS)*t)>>8; bd=near
    scA=rf(ad); scB=rf(bd)
    sxa=VS_HALF+((aS*scA)>>8); sxb=VS_HALF+((bS*scB)>>8)
    if sxa>=sxb: return                      # back-facing
    if sxb<0 or sxa>319: return
    ytFa=VS_HOR-(((fc-eye)*scA)>>8); ybFa=VS_HOR-(((ff-eye)*scA)>>8)   # front ceiling/floor screen rows at A
    ytFb=VS_HOR-(((fc-eye)*scB)>>8); ybFb=VS_HOR-(((ff-eye)*scB)>>8)
    if two:
        ytOa=VS_HOR-(((bc-eye)*scA)>>8); ybOa=VS_HOR-(((bf-eye)*scA)>>8)   # back ceiling/floor (the opening) at A
        ytOb=VS_HOR-(((bc-eye)*scB)>>8); ybOb=VS_HOR-(((bf-eye)*scB)>>8)
    sa=max(0,sxa); sb=min(320,sxb)
    ca=min(ncol-1, sa*ncol//320); cb=min(ncol-1, sb*ncol//320)
    den2=(cb-ca) or 1
    for c in range(ca, cb+1):
        if vct[c] > vcb[c]: continue          # already closed
        fr=(c-ca)/den2                        # affine fraction across the span (occlusion approx of ff2)
        ytF=int(ytFa+(ytFb-ytFa)*fr); ybF=int(ybFa+(ybFb-ybFa)*fr)
        if not two:
            vct[c]=vcb[c]+1                    # one-sided solid -> close
        else:
            ytO=int(ytOa+(ytOb-ytOa)*fr); ybO=int(ybOa+(ybOb-ybOa)*fr)
            if ytO>=ybO:
                vct[c]=vcb[c]+1               # opening collapsed (closed door) -> opaque close
            else:
                if ytO>ytF:                    # upper strip (ceiling down to opening)
                    if min(ytO,vcb[c])-max(ytF,vct[c])>=4: vns[c]+=1
                if ybF>ybO:                    # lower strip (opening floor down to floor)
                    if min(ybF,vcb[c])-max(ybO,vct[c])>=4: vns[c]+=1
                if ytO>vct[c]: vct[c]=ytO      # pinch the open span to the opening
                if ybO<vcb[c]: vcb[c]=ybO
                if vns[c]>=dcap and vct[c]<=vcb[c]: vct[c]=vcb[c]+1   # DEPTH CAP -> close

# ===================== faithful vs_bbox_vis (main.c ~1048) =====================
def bbox_vis(bb, S):
    """Returns (visible, stage, reason, dmin, c0, c1, muls, divs). S = the frame/dial state dict.
       Mirrors vs_bbox_vis EXCEPT the final per-column occlusion check (v1 assumes visible)."""
    px,py=S['px'],S['py']; fcs,fsn=S['fcs'],S['fsn']; murk=S['murk']; near=S['near']; ncull=S['ncull']
    frP,frQ,frR=S['frP'],S['frQ'],S['frR']; ncol=S['ncol']
    muls=0; divs=0
    # camera inside the box -> always visible (no MULs)
    if bb[0]<=px<=bb[2] and bb[1]<=py<=bb[3]:
        return (True,"inside-box","cam inside bbox",0,0,ncol-1,0,0)
    if ncull:
        nx = bb[0] if fcs>=0 else bb[2]; ny = bb[1] if fsn>=0 else bb[3]
        dmin=((nx-px)*fcs + (ny-py)*fsn)>>8; muls+=2          # 2-MUL far-cull
        if dmin>murk:
            return (False,"far-cull","dmin>murk (wholly beyond horizon)",dmin,None,None,muls,divs)
        if dmin>near:                                          # wholly in front -> side test exact
            rx = bb[0] if frP>=0 else bb[2]; ry = bb[1] if frQ>=0 else bb[3]
            muls+=2
            if (rx-px)*frP + (ry-py)*frQ > 255:
                return (False,"frustum-R","off the RIGHT frustum edge",dmin,None,None,muls,divs)
            lx = bb[2] if frR>=0 else bb[0]; ly = bb[3] if frP>=0 else bb[1]
            muls+=2
            if (lx-px)*frR + (ly-py)*frP < 0:
                return (False,"frustum-L","off the LEFT frustum edge",dmin,None,None,muls,divs)
    # ---- full 4-corner project ----
    cx=[bb[0],bb[2],bb[2],bb[0]]; cy=[bb[1],bb[1],bb[3],bb[3]]
    anyfront=anyback=0; allR=allL=1; sxmin=320; sxmax=-1; dmn=0x7fffffff
    for i in range(4):
        dx=cx[i]-px; dy=cy[i]-py
        d=(dx*fcs+dy*fsn)>>8; muls+=2
        if d<near: anyback=1; continue
        sd=(dx*fsn-dy*fcs)>>8; muls+=2; anyfront=1
        if d<dmn: dmn=d
        if not (sd*VS_FOCAL >  160*d): allR=0
        muls+=1                                                # sd*FOCAL (160*d is a shift on HW; count the sd*FOCAL MUL)
        if not (sd*VS_FOCAL < -160*d): allL=0
        r = rf(d);
        if d>=VS_RFMAX: divs+=1
        sx=VS_HALF+((sd*r)>>8); muls+=1
        if sx<sxmin: sxmin=sx
        if sx>sxmax: sxmax=sx
    if not anyfront: return (False,"full:behind","all corners behind near plane",dmn,None,None,muls,divs)
    if anyback:      return (True,"full:straddle","straddles near plane -> kept (safe)",dmn,0,ncol-1,muls,divs)
    if ncull and dmn>murk: return (False,"full:far-cull","subtree wholly beyond horizon",dmn,None,None,muls,divs)
    if allR or allL: return (False,"full:off-side","wholly off %s frustum"%("RIGHT" if allR else "LEFT"),dmn,None,None,muls,divs)
    smin=max(0,min(320,sxmin)); smax=max(0,min(320,sxmax))
    c0=min(ncol-1, smin*ncol//320); c1=min(ncol-1, smax*ncol//320)
    vct=S.get('vct'); vcb=S.get('vcb')   # v2 occlusion: prune if EVERY covered column is already CLOSED (ct>cb) by nearer walls / the dc-cap
    if vct is not None and not any(vct[c]<=vcb[c] for c in range(c0,c1+1)):
        return (False,"full:occluded","every covered column closed (nearer walls / dc-cap)",dmn,None,None,muls,divs)
    return (True,"full:visible","on-screen, has an open column",dmn,c0,c1,muls,divs)

# ===================== the walk (main.c ~1282) =====================
def walk(M, S):
    nodes=M['nodes']; rbb=M['rbb']; lbb=M['lbb']; ssecs=M['ssecs']; depth=M['depth']
    segends=M['segends']; segtwo=M['segtwo']; seghts=M['seghts']
    px,py=S['px'],S['py']; bxc=S['bxc']; ncol=S['ncol']
    S['vct']=[VS_LBT]*ncol; S['vcb']=[VS_LBB]*ncol; S['vns']=[0]*ncol   # v2 occlusion: per-column open span [ct,cb] (closed when ct>cb) + strip count for the dc-cap, filled front-to-back
    trace=[]; bx=0; step=0; bx_cyc=0; vopen=ncol; openhalt=False   # vopen = open columns; the engine HALTS the walk when ALL close (the corridor mechanism)
    stage_calls={}; stage_muls={}; stage_divs={}    # rollup accumulators
    ss_visited=0; ss_segs=0; halted=False
    defer=S.get('defer',False)
    stack=[(M['root'], depth.get(M['root'],0), None)]   # entries: (node, depth, deferred_bbox|None)
    def acc(stage,muls,divs):
        stage_calls[stage]=stage_calls.get(stage,0)+1
        stage_muls[stage]=stage_muls.get(stage,0)+muls
        stage_divs[stage]=stage_divs.get(stage,0)+divs
    def do_test(child,bb,which,dd):                      # test a child bbox, record the step, return visible
        nonlocal bx, step, bx_cyc
        bx+=1
        vis,stage,reason,dmin,c0,c1,muls,divs = bbox_vis(bb,S)
        bx_cyc+=muls*MUL_CYC+divs*DIVW_CYC; acc(stage,muls,divs)
        ckind="ss%d"%(child&0x7fff) if (child&0x8000) else "node%d"%child
        step+=1
        trace.append(dict(step=step,kind="TEST",depth=dd,parent="",child=ckind,side=which,
                          stage=stage,result=("PUSH" if vis else "CULL"),reason=reason,
                          dmin=dmin,span=("%d-%d"%(c0,c1) if (vis and c0 is not None) else ""),
                          muls=muls,cyc=muls*MUL_CYC+divs*DIVW_CYC))
        return vis
    while stack and vopen>0:
        if bx>=bxc:
            halted=True
            trace.append(dict(step=step,kind="BXC-HALT",detail="bx=%d hit --bxc=%d; %d nodes still on stack DROPPED (far subtrees)"%(bx,bxc,len(stack))))
            break
        n,d,dbb=stack.pop()
        if dbb is not None:                              # DEFERRED far child -> test NOW vs the CURRENT cliplist (DOOM R_CheckBBox order)
            if not do_test(n,dbb,"far*",d): continue
        if n & 0x8000:                                   # subsector leaf
            ss=n & 0x7fff; cnt,first = (ssecs[ss] if ss<len(ssecs) else (0,0))
            ss_visited+=1; ss_segs+=cnt; step+=1
            for k in range(first, first+cnt):            # close/pinch columns behind this subsector's walls (front-to-back occlusion)
                if k<len(segends): occlude_seg(segends[k][0], segends[k][1], seghts[k], segtwo[k], S)
            vopen=sum(1 for c in range(ncol) if S['vct'][c]<=S['vcb'][c])
            trace.append(dict(step=step,kind="VISIT-SS",depth=d,id=ss,nsegs=cnt,detail="%d seg(s); %d/%d cols still open"%(cnt,vopen,ncol)))
            continue
        nx,ny,ndx,ndy,rc,lc=nodes[n]
        cross=(px-nx)*ndy - (py-ny)*ndx                  # side-pick: 2 MULs/node
        bx_cyc+=2*MUL_CYC; acc("node:side-pick",2,0)
        if cross>0: nearc,farc,nearbb,farbb = rc,lc,rbb[n],lbb[n]
        else:       nearc,farc,nearbb,farbb = lc,rc,lbb[n],rbb[n]
        if defer:                                        # DOOM ORDER: push FAR deferred, process NEAR fully first -> FAR is tested AFTER near's walls close columns -> occlusion prunes whole far subtrees
            if len(stack)<126: stack.append((farc,d+1,farbb))
            if len(stack)<126 and do_test(nearc,nearbb,"near",d+1): stack.append((nearc,d+1,None))
        else:                                            # EAGER order (current engine): far child tested NOW, before near closes columns -> far subtree rarely occlusion-pruned
            if len(stack)<126 and do_test(farc,farbb,"far",d+1): stack.append((farc,d+1,None))
            if len(stack)<126 and do_test(nearc,nearbb,"near",d+1): stack.append((nearc,d+1,None))
    if vopen<=0 and stack:   # all columns closed -> the engine's vs_open check halts the walk (corridor: cheap)
        openhalt=True
        trace.append(dict(step=step,kind="VOPEN-HALT",detail="all %d columns closed -> walk HALTS with %d nodes still on stack (the corridor mechanism)"%(ncol,len(stack))))
    return dict(trace=trace,bx=bx,bx_cyc=bx_cyc,stage_calls=stage_calls,stage_muls=stage_muls,
                stage_divs=stage_divs,ss_visited=ss_visited,ss_segs=ss_segs,halted=halted,openhalt=openhalt,vopen=vopen)

# ===================== outputs =====================
def write_tree_csv(M, path):
    with open(path,"w",newline="") as f:
        w=csv.writer(f); w.writerow(["node_id","depth","split_x","split_y","split_dx","split_dy",
            "R_child","R_kind","R_bbox(xmin,ymin,xmax,ymax)","L_child","L_kind","L_bbox"])
        for i,(x,y,dx,dy,rc,lc) in enumerate(M['nodes']):
            rk="ss" if rc&0x8000 else "node"; lk="ss" if lc&0x8000 else "node"
            w.writerow([i,M['depth'].get(i,""),x,y,dx,dy, rc&0x7fff,rk,M['rbb'][i], lc&0x7fff,lk,M['lbb'][i]])

def write_trace_csv(R, path):
    with open(path,"w",newline="") as f:
        w=csv.writer(f); w.writerow(["step","kind","depth","parent","child","side","stage","result","reason","dmin","screen_span(cols)","muls","cyc"])
        for r in R['trace']:
            if r['kind']=="TEST":
                w.writerow([r['step'],r['kind'],r['depth'],r['parent'],r['child'],r['side'],r['stage'],r['result'],r['reason'],r['dmin'],r['span'],r['muls'],r['cyc']])
            elif r['kind']=="VISIT-SS":
                w.writerow([r['step'],r['kind'],r['depth'],"","ss%d"%r['id'],"","subsector","VISIT",r.get('detail',"%d segs"%r['nsegs']),"","","",""])
            else:
                w.writerow([r.get('step',''),r['kind'],"","","","","","",r.get('detail',''),"","","",""])

def write_svg(M, R, S, path):
    xs=[v[0] for v in M['verts']]; ys=[v[1] for v in M['verts']]
    if not xs: return
    minx,maxx,miny,maxy=min(xs),max(xs),min(ys),max(ys)
    W=1000.0; pad=20; sc=(W-2*pad)/max(1,(maxx-minx)); H=(maxy-miny)*sc+2*pad
    def X(x): return pad+(x-minx)*sc
    def Y(y): return H-pad-(y-miny)*sc          # flip: DOOM +y is up
    # which subsectors were visited
    vis_ss=set(r['id'] for r in R['trace'] if r['kind']=="VISIT-SS")
    # build ssector -> seg-range for highlighting visited walls
    el=['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" font-family="monospace" font-size="11">'%(int(W),int(H))]
    el.append('<rect width="100%%" height="100%%" fill="#111"/>')
    # all walls faint
    for (a,b) in M['segends']:
        el.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#444" stroke-width="1"/>'%(X(a[0]),Y(a[1]),X(b[0]),Y(b[1])))
    # visited subsectors' segs in green
    for ss in vis_ss:
        if ss>=len(M['ssecs']): continue
        cnt,first=M['ssecs'][ss]
        for k in range(first,first+cnt):
            if k<len(M['segends']):
                a,b=M['segends'][k]
                el.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#3f6" stroke-width="2"/>'%(X(a[0]),Y(a[1]),X(b[0]),Y(b[1])))
    # camera + 90deg frustum
    px,py,a=S['px'],S['py'],S['ang']; cx,cy=X(px),Y(py)
    fcs,fsn=S['fcs'],S['fsn']; L=300
    # view dir + the two 45deg frustum edges (screen y flipped)
    for da,col in ((0,'#ff0'),):
        dirx=fcs/256.0; diry=fsn/256.0
        el.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="1.5"/>'%(cx,cy,X(px+dirx*L),Y(py+diry*L),col))
    for sgn in (+1,-1):                                  # +-45deg edges: rotate dir by +-45
        ca=math.cos(math.radians(45)*sgn); sa=math.sin(math.radians(45)*sgn)
        ex=(fcs*ca-fsn*sa)/256.0; ey=(fcs*sa+fsn*ca)/256.0
        el.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#fa0" stroke-width="1" stroke-dasharray="4 3"/>'%(cx,cy,X(px+ex*L),Y(py+ey*L)))
    el.append('<circle cx="%.1f" cy="%.1f" r="4" fill="#f33"/>'%(cx,cy))
    el.append('<text x="8" y="16" fill="#ccc">%s  pos(%d,%d) a%d  dd=%d near=%d bxc=%d ncull=%d | visited %d/%d ss, bx=%d</text>'
              %(S['map'],px,py,a,S['murk'],S['near'],S['bxc'],S['ncull'],len(vis_ss),len(M['ssecs']),R['bx']))
    el.append('</svg>')
    open(path,"w").write("\n".join(el))

def main():
    import argparse
    ap=argparse.ArgumentParser()
    ap.add_argument("map"); ap.add_argument("--wad",default="doom1.wad")
    ap.add_argument("--pos",nargs=3,type=int,metavar=("X","Y","A"),help="camera x y angle(0-255); default = map spawn")
    ap.add_argument("--dd",type=int,default=750,help="draw distance / far-horizon (HUD dd / ef)")
    ap.add_argument("--near",type=int,default=16,help="near-clip (HUD nclp)")
    ap.add_argument("--bxc",type=int,default=2048,help="Phase 2 walk budget (HUD bxc; 2048=off)")
    ap.add_argument("--ncull",type=int,default=1,help="cheap cull ladder on/off (HUD ncul)")
    ap.add_argument("--dc",type=int,default=5,help="per-column DEPTH CAP (HUD dc): close a column after this many strips -- the dominant walk-pruner")
    ap.add_argument("--ncol",type=int,default=20,help="column count (cart runs ~20; occlusion is coarser/aggressive at low counts -> match your col dial)")
    ap.add_argument("--defer",action="store_true",help="PROTOTYPE the DOOM walk order: defer the far-child bbox test until the near subtree closes columns (DOOM R_CheckBBox) -> far subtrees behind solids get occlusion-pruned")
    ap.add_argument("--out",default="dist/bsp")
    A=ap.parse_args()
    M=load(A.wad, A.map)
    if A.pos: px,py,ang=A.pos
    else:     px,py,ang=M['sx'],M['sy'],M['sa']
    fcs,fsn=VS_CS(ang),VS_SN(ang)
    eye=floor_at(M,px,py)+VS_EYE
    S=dict(map=A.map,px=px,py=py,ang=ang,fcs=fcs,fsn=fsn,murk=A.dd,near=A.near,bxc=A.bxc,ncull=A.ncull,ncol=A.ncol,
           dcap=A.dc,eye=eye,defer=A.defer, frP=fsn-fcs, frQ=-(fcs+fsn), frR=fsn+fcs)
    R=walk(M,S)
    os.makedirs(A.out,exist_ok=True)
    tag="%s_%d_%d_a%d"%(A.map,px,py,ang)
    tp=os.path.join(A.out,"tree_%s.csv"%A.map); rp=os.path.join(A.out,"trace_%s.csv"%tag); sp=os.path.join(A.out,"trace_%s.svg"%tag)
    write_tree_csv(M,tp); write_trace_csv(R,rp); write_svg(M,R,S,sp)
    # ---- rollup ----
    print("="*78)
    print("BSP TRACE  %s  pos(%d,%d) a%d eye=%d | dials: dd=%d dc=%d near=%d bxc=%d ncull=%d ncol=%d"%(A.map,px,py,ang,eye,A.dd,A.dc,A.near,A.bxc,A.ncull,A.ncol))
    print("tree: %d nodes, %d subsectors, root=%d, max depth=%d"%(len(M['nodes']),len(M['ssecs']),M['root'],max(M['depth'].values())))
    print("="*78)
    order=["inside-box","far-cull","frustum-R","frustum-L","full:behind","full:straddle","full:far-cull","full:off-side","full:occluded","full:visible","node:side-pick"]
    seen=[s for s in order if s in R['stage_calls']]+[s for s in R['stage_calls'] if s not in order]
    print("%-16s %7s %9s %9s   %s"%("STAGE","calls","MULs","~cyc","note"))
    print("-"*78)
    notes={"inside-box":"cam inside bbox (free)","far-cull":"2-MUL nearest-corner depth > horizon",
      "frustum-R":"off right edge (Phase 1)","frustum-L":"off left edge (Phase 1)",
      "full:behind":"all corners behind near","full:straddle":"near-plane straddle -> kept",
      "full:far-cull":"4-corner far prune","full:off-side":"4-corner off-screen prune",
      "full:occluded":"behind nearer solid walls (occlusion)","full:visible":"PASSED to push (has an open column)","node:side-pick":"per internal node (cross)"}
    tot_mul=tot_cyc=0
    for s in seen:
        mul=R['stage_muls'].get(s,0); cyc=mul*MUL_CYC+R['stage_divs'].get(s,0)*DIVW_CYC
        tot_mul+=mul; tot_cyc+=cyc
        print("%-16s %7d %9d %9d   %s"%(s,R['stage_calls'][s],mul,cyc,notes.get(s,"")))
    print("-"*78)
    # Phase-1 cheap-ladder payoff: nodes the 2-MUL far-cull / ~4-MUL frustum diverted from the ~30-MUL full project
    FULL_MUL=30
    farc=R['stage_calls'].get('far-cull',0); fr=R['stage_calls'].get('frustum-R',0)+R['stage_calls'].get('frustum-L',0)
    saved=farc*(FULL_MUL-2)+fr*(FULL_MUL-4)
    print("WALK COST (geometric, this pose): bx=%d node-box tests, %d MULs ~%d cyc @%dcyc/MUL"%(R['bx'],tot_mul,tot_cyc,MUL_CYC))
    print("Phase-1 cheap-ladder payoff: %d nodes diverted from the ~%d-MUL full project (%d far-cull@2 + %d frustum@4) -> ~%d MULs saved"%(farc+fr,FULL_MUL,farc,fr,saved))
    occ=R['stage_calls'].get('full:occluded',0)
    print("subsectors VISITED: %d / %d   segs to project/emit: %d   columns still open at end: %d/%d"%(R['ss_visited'],len(M['ssecs']),R['ss_segs'],R['vopen'],A.ncol))
    print("OCCLUSION: %d nodes pruned (all covered cols closed by nearer walls + the dc=%d cap)."%(occ,A.dc))
    if R['halted']:   print("** --bxc HALTED the walk (far subtrees dropped) **")
    if R['openhalt']: print("** vs_open HALT: every column closed -> walk STOPPED early (this is why CORRIDORS are cheap) **")
    print("-"*78)
    print("HOW TO READ THIS (the decomposition that lets you tune):")
    print("  * bx (the WALK) is mostly GEOMETRIC -- the tree shape + far-cull + frustum. Occlusion and the dc-cap")
    print("    mainly cut SEGS (sg) and EMIT (ns), NOT node-tests, because a far node is tested BEFORE the near walls")
    print("    that hide it have closed their columns (true on the device too). So:")
    print("      - to cut bx (walk-bound: STAIRS, node-dense): cheap-ladder (ncul/Phase1), bxc/Phase2, dd, coarser tree.")
    print("      - to cut sg/emit (emit-bound: WINDOWS, open vistas): dc, occlusion, bud, psd.")
    print("  * CORRIDOR = one-sided walls close all columns -> vs_open HALT -> tiny bx (cheap).")
    print("    WINDOW/STEPS = two-sided openings keep columns open -> walk runs far -> high bx AND high sg (expensive).")
    print("  * NOT a whole-frame budget (EMIT + FLATS aren't modeled). Absolute bx is approximate (the engine's")
    print("    vertical band-reject + per-seg feedback aren't fully modeled); trust the SHAPE + the lever decomposition.")
    print("  * Match --ncol/--dc/--dd to your dials; the tree, order, and geometric culls are exact.")
    print("="*78)
    print("wrote:\n  %s\n  %s\n  %s"%(tp,rp,sp))

if __name__=="__main__": main()
