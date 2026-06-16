/* render.c -- BSP renderer. The math path uses `real` (float by default, 16.16
 * under -DUSE_FIXED=1) so we can A/B-verify the fixed-point port against float.
 * Collision/doors/hitscan stay float (they don't affect the rendered frame). */
#include "dng.h"
#include "fixed.h"
#include "trig.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

proj_cfg PCFG = { 160.0f, 8.0f, 1.0f, 0.0f, 16, 1, 0.0f };
int dng_flats = 1;                                     /* host: per-band flats; neo geo: cheap bg */
rstats RSTAT;

void tables_init(void){}                               /* sine table is const now (trig.h) */
float lut_sin(angle_t a){ return SIN256[a]; }
float lut_cos(angle_t a){ return SIN256[(uint8_t)(a+64)]; }     /* cos = sin + 90 deg */
static int isqrt(int v){ if(v<=0)return 0; int x=v,y=(x+1)>>1; while(y<x){ x=y; y=(x+v/x)>>1; } return x; }

#if !USE_FIXED
static float dscale(float d){ RSTAT.divides++; if(PCFG.gamma==1.0f) return PCFG.fov/d; const float p=256.0f; return (PCFG.fov/p)*powf(p/d,PCFG.gamma); }
#endif
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }

/* ---- fixed scratch: verts converted ONCE (world_init), camera+sectors ONCE per
 * frame (render_world). The hot path reads these and never calls rf()/rtof(), so
 * there is no soft-float on the 68000 -- that was the 0.2fps killer. ---- */
#define MAXVERT 700
#define MAXNODE 400
#define MAXSEG  1024
#define MAXLINE 2048
static short LBX0[MAXLINE],LBX1[MAXLINE],LBY0[MAXLINE],LBY1[MAXLINE];  /* per-line int AABB (collision broad-phase) */
static real VXf[MAXVERT], VYf[MAXVERT];          /* vertices, fixed */
static int  NXi[MAXNODE],NYi[MAXNODE],NDXi[MAXNODE],NDYi[MAXNODE];  /* BSP nodes, integer */
static real SEGLEN[MAXSEG];                       /* seg lengths, precomputed (static geometry) */
static real CXf,CYf,COf,SIf;                     /* camera position + cos/sin */
static int  CXi,CYi;                             /* camera position, integer (node tests) */
static real FOVf,NEARZf,CAMZf,HZf,RHALF,BAND4,REPS;  /* projection constants */
static real SCf[256],SFf[256];                   /* sector ceil/floor, fixed */
static long FAR2=0;                               /* draw-distance cull, squared map units (0=off) */
static real LIMF=0;                               /* projection clamp = 180*fov (overflow guard) */
static real CAMD=0,CAMS=0;                        /* camera rotation term (rotation-LUT to_view) */
static int  AIoff=0;                              /* angle-index * ROT_NV, base into ROTD/ROTS */

/* screen projection: coord * fov / depth (fixed build clamps the ratio so the 16.16
 * product can't overflow; gamma curve is float-host only). */
static real screen_off(real coord, real depth){
#if USE_FIXED
    RSTAT.divides++;
    real r=rdiv(coord,depth), lim=ri(180);
    if(r>lim) r=lim; else if(r<-lim) r=-lim;
    return rmul(r, FOVf);
#else
    return coord*dscale(depth);
#endif
}

/* like screen_off but takes a precomputed depth reciprocal -- turns the per-projection
 * divide into a multiply. The seg hot path computes 1/depth twice and reuses it ~10x. */
static real screen_off_f(real coord, real invfov){      /* invfov = (1/depth)*fov, folded -> 1 mul */
#if USE_FIXED
    real r=rmul(coord,invfov);
    if(r>LIMF) r=LIMF; else if(r<-LIMF) r=-LIMF;
    return r;
#else
    return coord*invfov;
#endif
}

static void to_view(real px, real py, real*depth, real*side){      /* math (things, by coord) */
    RSTAT.toviews++;
    real dx=px-CXf, dy=py-CYf;
    *depth=rmul(dx,COf)+rmul(dy,SIf); *side=rmul(dx,SIf)-rmul(dy,COf);
}
static void to_view_v(int v, real*depth, real*side){               /* table (segs, rotation precomputed) */
    RSTAT.toviews++;
    *depth=ri(ROTD[AIoff+v])-CAMD; *side=ri(ROTS[AIoff+v])-CAMS;    /* lookup + subtract, no multiplies */
}
static uint8_t shade(uint8_t base,real depth){
    int d=r2i(depth); if(d<0)d=0;                          /* world-unit distance */
    int f=256-(d*154)/1800; if(f<64)f=64; if(f>256)f=256;  /* 1-(d/1800)*0.6, clamp .25..1 */
    return (uint8_t)clampi((base*f)>>8,0,255);
}

static const level_t*LV; static const camera_t*CAM; static DrawList*DL;
static int wtop[SCREEN_W],wbot[SCREEN_W];
static int flat_line[SCREEN_H];
static int occcols;
static uint8_t dead[256];                        /* killed things, skipped by render */
static int16_t secmid[256];                      /* per-sector fallback wall texture: solid walls the WAD left untextured (never meant to be seen) borrow their sector's common wall texture instead of rendering as placeholder blocks */
static sector_t MSECT[256];                     /* mutable sector heights (doors/lifts) */
static uint8_t dstate[256];                     /* door: 0 closed, 1 opening, 2 wait, 3 closing */
static int dwait[256];
static float dtarget[256], dclosed[256];

static void push(SpriteCmd s){ if(DL->n>=MAX_CMDS||s.w<=0||s.h<=0)return; DL->cmd[DL->n++]=s; if(s.kind==SC_FLAT)return; int y0=clampi(s.sy,0,SCREEN_H),y1=clampi(s.sy+s.h,0,SCREEN_H); for(int y=y0;y<y1;y++)DL->spr_line[y]+=s.hw; DL->spr_total+=s.hw; }

static int g_cursrc=0;   /* set by render_seg: (segidx<<2)|edgeclass+1 -- identity for the codec bake */
static void emit_tex(int x,int w,int ty,int by,int tex,int uL,int uR,int v0,int v1,uint8_t lum,int depth,int dtop,int dbot){
    if(by<=ty||tex<0)return; RSTAT.wall_bands++;
    push((SpriteCmd){x,ty,w,by-ty,0,lum,SC_WALL,(w+NG_SPR_MAXW-1)/NG_SPR_MAXW,tex,uL,uR,v0,v1,depth,(int16_t)dtop,(int16_t)dbot,(int16_t)g_cursrc});
}
static void emit_flat(int x,int w,int ty,int by,int tex,int planez,uint8_t lum,int depth,int isceil){
    if(by<=ty||!dng_flats)return; int fhw=(w+NG_SPR_MAXW-1)/NG_SPR_MAXW; int y0=clampi(ty,0,SCREEN_H),y1=clampi(by,0,SCREEN_H);
    for(int y=y0;y<y1;y++)flat_line[y]+=fhw; RSTAT.flat_hw+=fhw; RSTAT.flat_fills++;
    push((SpriteCmd){x,ty,w,by-ty,0,lum,SC_FLAT,0,tex,0,0,planez,(int16_t)isceil,depth,0,0});   /* v1 = 1 ceiling / 0 floor; depth = band depth so special-floor (pool) flats sort against walls */
}
static void occlude(int x){ if(wtop[x]<wbot[x]){ wtop[x]=wbot[x]=0; occcols++; } }

static real persp_u(real uozL,real uozR,real invL,real invR,real t){ RSTAT.divides++; real inv=invL+rmul(invR-invL,t); return rdiv(uozL+rmul(uozR-uozL,t), inv); }

static void render_seg(int segidx){
    if(occcols>=SCREEN_W)return;
    const seg_t*sg=&LV->segs[segidx]; const linedef_t*ld=&LV->lines[sg->line];
    int fsd=sg->side==0?ld->right:ld->left, bsd=sg->side==0?ld->left:ld->right;
    if(fsd<0)return;
    const sidedef_t*sd=&LV->sides[fsd];
    int fsec=sd->sector, bsec=(bsd<0)?-1:LV->sides[bsd].sector;
    real ax=VXf[sg->v1],ay=VYf[sg->v1],bx=VXf[sg->v2],by=VYf[sg->v2];
    int axi=r2i(ax),ayi=r2i(ay),bxi=r2i(bx),byi=r2i(by),cxi=CXi,cyi=CYi;
    if((bxi-axi)*(cyi-ayi)-(byi-ayi)*(cxi-axi)>=0)return;              /* backface: integer sign */
    if(FAR2 && (long)(axi-cxi)*(axi-cxi)+(long)(ayi-cyi)*(ayi-cyi)>FAR2
            && (long)(bxi-cxi)*(bxi-cxi)+(long)(byi-cyi)*(byi-cyi)>FAR2) return;   /* draw-distance cull */
    real da,sa,db,sb; to_view_v(sg->v1,&da,&sa); to_view_v(sg->v2,&db,&sb);   /* rotation-LUT */
    if(da<NEARZf&&db<NEARZf)return;
    real seglen=SEGLEN[segidx];                          /* precomputed at world_init */
    real uA=ri(sg->offset+sd->xoff), uB=uA+seglen;
    if(da<NEARZf){ real t=rdiv(NEARZf-da,db-da); sa=sa+rmul(sb-sa,t); uA=uA+rmul(uB-uA,t); da=NEARZf; }
    if(db<NEARZf){ real t=rdiv(NEARZf-db,da-db); sb=sb+rmul(sa-sb,t); uB=uB+rmul(uA-uB,t); db=NEARZf; }
    RSTAT.segs++;
    real inv_da=recip(da), inv_db=recip(db);                          /* depth reciprocals via 32-bit divide */
    real ivf_da=rmul(inv_da,FOVf), ivf_db=rmul(inv_db,FOVf);          /* fold fov in -> 1 mul/projection */
    real sxa=ri(HALF_W)+screen_off_f(sa,ivf_da), sxb=ri(HALF_W)+screen_off_f(sb,ivf_db);
    real xl,xr,dL,dR,uLw,uRw,invL,invR,ivfL,ivfR;
    if(sxa<=sxb){ xl=sxa;xr=sxb;dL=da;dR=db;uLw=uA;uRw=uB;invL=inv_da;invR=inv_db;ivfL=ivf_da;ivfR=ivf_db; }
    else        { xl=sxb;xr=sxa;dL=db;dR=da;uLw=uB;uRw=uA;invL=inv_db;invR=inv_da;ivfL=ivf_db;ivfR=ivf_da; }
    if(xr-xl<RHALF)return; real span=xr-xl;
    int xstart=clampi(r2i(xl),0,SCREEN_W),xend=clampi(r2i(xr)+1,0,SCREEN_W);
    { int vis=0; for(int xx=xstart;xx<xend;xx++) if(wtop[xx]<wbot[xx]){vis=1;break;} if(!vis)return; }  /* fully occluded -> skip all the int64 setup + band loop */
    real inv_span=recip(span);
    real uozL=rmul(uLw,invL),uozR=rmul(uRw,invR); RSTAT.divides+=3;    /* inv_da, inv_db, inv_span */
    real fcz=SCf[fsec]-CAMZf, ffz=SFf[fsec]-CAMZf;
    real ftopL=HZf-screen_off_f(fcz,ivfL), ftopR=HZf-screen_off_f(fcz,ivfR);
    real fbotL=HZf-screen_off_f(ffz,ivfL), fbotR=HZf-screen_off_f(ffz,ivfR);
    real btopL=0,btopR=0,bbotL=0,bbotR=0;
    if(bsec>=0){ real bcz=SCf[bsec]-CAMZf,bfz=SFf[bsec]-CAMZf; btopL=HZf-screen_off_f(bcz,ivfL);btopR=HZf-screen_off_f(bcz,ivfR);bbotL=HZf-screen_off_f(bfz,ivfL);bbotR=HZf-screen_off_f(bfz,ivfR); }
    /* (per-band edge slopes are computed in the band loop now; the whole-seg slope is no longer used) */
    /* Band width is NO LONGER throttled by the silhouette slope: the runtime now interpolates
       each 16px chunk's top/height along (dtop,dbot), so a wide band tracks a steep edge exactly.
       Throttling by slope did the opposite of what we want -- it starved the steepest walls of the
       multi-chunk width their slope needs to show. Go full width; only the texture u-warp would
       argue for narrower, and that's deferred. (Bonus: fewer records -> smaller blob.) */
    int bw=PCFG.max_band;
    uint8_t lum=shade(MSECT[fsec].light, rmul(dL+dR,RHALF));
    int ceiltex=MSECT[fsec].ceiltex, floortex=MSECT[fsec].floortex;
    int fcz_i=r2i(SCf[fsec]), ffz_i=r2i(SFf[fsec]);
    for(int x=xstart, xe; x<xend; x=xe){              /* advance by the ACTUAL band end (x=xe), not a fixed bw */
        xe=x+bw; if(xe>xend||xend-xe<16)xe=xend;      /* absorb a sub-chunk (<16px) trailing stub into this band -> no sliver records */
        int xc=clampi((x+xe)/2,0,SCREEN_W-1);
        int winT=wtop[xc],winB=wbot[xc]; if(winT>=winB)continue;
        real tc=rmul(ri(xc)-xl,inv_span); if(tc<0)tc=0; if(tc>ri(1))tc=ri(1);
        int bd=r2i(dL+rmul(dR-dL,tc)); if(bd<1)bd=1;            /* band depth (world units) for Street-View transform */
        int uLb=r2i(persp_u(uozL,uozR,invL,invR,rmul(ri(x)-xl,inv_span)));
        int uRb=r2i(persp_u(uozL,uozR,invL,invR,rmul(ri(xe)-xl,inv_span)));
        int w=xe-x;
        real ftop=ftopL+rmul(ftopR-ftopL,tc), fbot=fbotL+rmul(fbotR-fbotL,tc);
        /* band edge fractions, shared by one- and two-sided: clamp each edge to the occlusion window and
           derive ty+slope from the CLAMPED edges, so adjacent bands meet AND the silhouette respects
           occlusion (no centre-clamp-with-full-slope spikes). Applies to mid, upper AND lower textures. */
        real tcL=rmul(ri(x)-xl,inv_span),tcR=rmul(ri(xe)-xl,inv_span);
        if(tcL<0)tcL=0; if(tcL>ri(1))tcL=ri(1); if(tcR<0)tcR=0; if(tcR>ri(1))tcR=ri(1);
        int xL=clampi(x,0,SCREEN_W-1),xR=clampi(xe-1,0,SCREEN_W-1);   /* sample the occlusion window at BOTH band edges, not just the centre xc -> the clamp follows a SLOPING window instead of stepping flat per band (the wall-top staircase) */
        int wTL=wtop[xL],wBL=wbot[xL],wTR=wtop[xR],wBR=wbot[xR];
        { int dT=wTL-wTR,dB=wBL-wBR; if(dT<0)dT=-dT; if(dB<0)dB=-dB;
          if(dT>=24||dB>=24){ wTL=wTR=winT; wBL=wBR=winB; } }   /* band STRADDLES an occlusion boundary (window steps, doesn't slope): per-edge would draw a fake diagonal instead of a step -> fall back to centre clamp there */
        #define EDGE2(LO,HI,a,b) int LO=clampi(r2i(a##L+rmul(a##R-a##L,tcL)),wTL,wBL),HI=clampi(r2i(a##L+rmul(a##R-a##L,tcR)),wTR,wBR)
        if(bsec<0){
            EDGE2(tLe,tRe,ftop,_); EDGE2(bLe,bRe,fbot,_);
            int ty=(tLe+tRe)>>1,by2=(bLe+bRe)>>1;
            emit_flat(x,w,winT,ty,ceiltex,fcz_i,lum,bd,1); emit_flat(x,w,by2,winB,floortex,ffz_i,lum,bd,0);
            real wH=SCf[fsec]-SFf[fsec], dh=fbot-ftop, tpp=(dh>0)?rdiv(wH,dh):0;
            g_cursrc=(segidx<<2)|1;
            emit_tex(x,w,ty,by2,(sd->midtex>=0?sd->midtex:secmid[fsec]),uLb,uRb,sd->yoff+r2i(rmul(ri(ty)-ftop,tpp)),sd->yoff+r2i(rmul(ri(by2)-ftop,tpp)),lum,bd,tRe-tLe,bRe-bLe);
            for(int xx=x;xx<xe;xx++)occlude(xx);
        } else {
            real btop=btopL+rmul(btopR-btopL,tc), bbot=bbotL+rmul(bbotR-bbotL,tc);
            int fc=clampi(r2i(ftop),winT,winB),ff=clampi(r2i(fbot),winT,winB);
            emit_flat(x,w,winT,fc,ceiltex,fcz_i,lum,bd,1); emit_flat(x,w,ff,winB,floortex,ffz_i,lum,bd,0);
            EDGE2(fcL,fcR,ftop,_); EDGE2(ffL,ffR,fbot,_);   /* front ceiling/floor edges -> the opening's default top/bottom */
            int otL=fcL,otR=fcR,obL=ffL,obR=ffR;            /* opening edges (SLOPED) used to occlude the room behind */
            if(SCf[bsec]<SCf[fsec]){   /* UPPER tex: top=front ceiling, bottom=back ceiling -> both slope -> caps */
                EDGE2(uLe,uRe,btop,_);
                int tyf=(fcL+fcR)>>1,bt=(uLe+uRe)>>1;
                real wH=SCf[fsec]-SCf[bsec], dh=btop-ftop, tpp=(dh>0)?rdiv(wH,dh):0;
                if(sd->uptex>=0){ g_cursrc=(segidx<<2)|2;
                emit_tex(x,w,tyf,bt,sd->uptex,uLb,uRb,sd->yoff+r2i(rmul(ri(tyf)-ftop,tpp)),sd->yoff+r2i(rmul(ri(bt)-ftop,tpp)),lum,bd,fcR-fcL,uRe-uLe); }   /* skip missing (-1) upper -> no gray box */
                otL=uLe;otR=uRe; }
            if(SFf[bsec]>SFf[fsec]){   /* LOWER tex: top=back floor, bottom=front floor -> both slope -> caps (ALL steps drawn; the blue-room steps must render) */
                EDGE2(lLe,lRe,bbot,_);
                int bb=(lLe+lRe)>>1,fff=(ffL+ffR)>>1;
                real wH=SFf[bsec]-SFf[fsec], dh=fbot-bbot, tpp=(dh>0)?rdiv(wH,dh):0;
                if(sd->lowtex>=0){ g_cursrc=(segidx<<2)|3;
                emit_tex(x,w,bb,fff,sd->lowtex,uLb,uRb,sd->yoff+r2i(rmul(ri(bb)-bbot,tpp)),sd->yoff+r2i(rmul(ri(fff)-bbot,tpp)),lum,bd,lRe-lLe,ffR-ffL); }   /* skip missing (-1) lower -> no gray box */
                obL=lLe;obR=lRe; }
            for(int xx=x;xx<xe;xx++){ int t2=wtop[xx],b2=wbot[xx]; if(t2>=b2)continue;
                int otX=otL+(otR-otL)*(xx-x)/w, obX=obL+(obR-obL)*(xx-x)/w;   /* SLOPED opening -> the room behind follows it per-column, no per-band staircase */
                int nT=otX>t2?otX:t2,nB=obX<b2?obX:b2; if(nT>=nB)occlude(xx); else {wtop[xx]=nT;wbot[xx]=nB;} }
        }
        #undef EDGE2
    }
}

static void render_node(int idx){
    if(occcols>=SCREEN_W)return;
    if(idx<0){ const subsector_t*ss=&LV->ssectors[-idx-1]; for(int i=0;i<ss->numsegs;i++)render_seg(ss->firstseg+i); return; }
    const node_t*n=&LV->nodes[idx];
    int dx=CXi-NXi[idx], dy=CYi-NYi[idx]; int side=(NDXi[idx]*dy-NDYi[idx]*dx<0)?0:1;   /* integer BSP side */
    render_node(n->child[side]); render_node(n->child[side^1]);
}

typedef struct { real d, s; const thing_t *th; } thent;
static int cmp_d(const void *a,const void *b){ real x=((const thent*)a)->d,y=((const thent*)b)->d; return x<y?1:(x>y?-1:0); }
static void render_things(void){
    thent ar[256]; int n=0;
    for(int i=0;i<LV->nthings && n<256;i++){ if(dead[i])continue; const thing_t*th=&LV->things[i]; if(th->tex<0)continue;
        real d,s; to_view(rf(th->pos.x),rf(th->pos.y),&d,&s); if(d<NEARZf)continue; ar[n].d=d; ar[n].s=s; ar[n].th=th; n++; }
    qsort(ar,n,sizeof(thent),cmp_d);                          /* far -> near (painter's) */
    real HZ=HZf;
    for(int i=0;i<n;i++){ const thing_t*th=ar[i].th; real d=ar[i].d,s=ar[i].s;
        int tsec=point_sector(LV,(float)th->pos.x,(float)th->pos.y); if(tsec<0||tsec>=256)tsec=CAM->sector;
        real fz=SFf[tsec]-CAMZf;   /* anchor to the THING's OWN sector floor, not the camera's -> props on raised/lower floors stop hovering */
        real by=HZ-screen_off(fz,d), h=screen_off(ri(th->sh),d), w=screen_off(ri(th->sw),d), cx=ri(HALF_W)+screen_off(s,d);
        int xw=r2i(w); int xc=clampi(r2i(cx),0,SCREEN_W-1);
        int xl2=clampi(xc-xw/3,0,SCREEN_W-1), xr2=clampi(xc+xw/3,0,SCREEN_W-1);
        if(wtop[xc]>=wbot[xc] && wtop[xl2]>=wbot[xl2] && wtop[xr2]>=wbot[xr2])continue;   /* occluded only if ALL of centre + both third-points are walled: the old single-centre test FLIPPED between adjacent views when a prop sat half behind a crate edge -> the barrel flicker */
        int sx=r2i(cx-rmul(w,rf(0.5f))), ty=r2i(by-h), iw=r2i(w), ih=r2i(h);
        push((SpriteCmd){sx,ty,iw,ih,0,shade(230,d),SC_THING,(iw+NG_SPR_MAXW-1)/NG_SPR_MAXW,th->tex,0,th->sw,0,th->sh,r2i(d),(int16_t)th->type,0}); }   /* dtop carries the DOOM thing type (unused by draw_thing) so the node bake can filter monsters from the billboard pass */
}

void render_world(const level_t*lv,const camera_t*cam,DrawList*dl){
    memset(dl,0,sizeof(*dl)); memset(&RSTAT,0,sizeof RSTAT); memset(flat_line,0,sizeof flat_line);
    LV=lv;CAM=cam;DL=dl;occcols=0;
    CXf=rf(cam->pos.x); CYf=rf(cam->pos.y); CXi=r2i(CXf); CYi=r2i(CYf);
    int AI=(cam->ang>>1)%ROT_NA; AIoff=AI*ROT_NV; uint8_t sang=(uint8_t)(AI*2);   /* snap to ROT_NA angles */
    COf=rf(SIN256[(uint8_t)(sang+64)]); SIf=rf(SIN256[sang]);
    CAMD=rmul(CXf,COf)+rmul(CYf,SIf); CAMS=rmul(CXf,SIf)-rmul(CYf,COf);            /* camera rotation term */
    FOVf=rf(PCFG.fov); NEARZf=rf(PCFG.nearz); CAMZf=rf(cam->z); HZf=ri(HALF_H)+rf(cam->pitch);
    RHALF=rf(0.5f); BAND4=rf(16.0f); REPS=rf(0.0001f);   /* slope tolerance up: the trapezoid now interpolates the height, so bands can be much wider */
    FAR2=(long)(PCFG.far*PCFG.far); LIMF=rmul(ri(180),FOVf);
    for(int i=0;i<lv->nsectors && i<256;i++){ SCf[i]=rf(MSECT[i].ceil); SFf[i]=rf(MSECT[i].floor); }
    for(int x=0;x<SCREEN_W;x++){ wtop[x]=0; wbot[x]=SCREEN_H; }
    push((SpriteCmd){0,0,SCREEN_W,SCREEN_H,0,40,SC_FLAT,0,-1,0,0,0,0,0,0,0});
    render_node(lv->nnodes?lv->nnodes-1:-1);
    render_things();
    for(int y=0;y<SCREEN_H;y++){ if(dl->spr_line[y]>dl->line_max)dl->line_max=dl->spr_line[y]; int all=dl->spr_line[y]+flat_line[y]; if(all>RSTAT.line_all)RSTAT.line_all=all; }
    RSTAT.wall_hw=dl->spr_total; RSTAT.line_walls=dl->line_max;
}

/* ---- runtime BSP point-location, collision, shooting (float; not in render path) ---- */
int point_sector(const level_t*lv,float x,float y){
    int idx = lv->nnodes ? lv->nnodes-1 : -1;
    int guard=0;                                          /* HARD BOUND: a malformed/out-of-range BSP child must never spin this descent forever -- that's the deterministic stairs freeze. Out-of-range idx exits -> ss<0 -> return 0. */
    while(idx>=0 && idx<lv->nnodes && guard++<2048){ const node_t*n=&lv->nodes[idx]; float c=n->dx*(y-n->y)-n->dy*(x-n->x); idx=n->child[c<0?0:1]; }
    int ss=-idx-1; if(ss<0||ss>=lv->nssectors) return 0;
    const seg_t*sg=&lv->segs[lv->ssectors[ss].firstseg]; const linedef_t*ld=&lv->lines[sg->line];
    int sd=sg->side==0?ld->right:ld->left; return sd<0?0:lv->sides[sd].sector;
}
static int blocked(const level_t*lv,float x,float y,float curfloor){
    const float R=16.0f;
    int xi=(int)x,yi=(int)y;
    for(int i=0;i<lv->nlines;i++){ const linedef_t*ld=&lv->lines[i];
        if(i<MAXLINE && (xi<LBX0[i]||xi>LBX1[i]||yi<LBY0[i]||yi>LBY1[i])) continue;   /* int AABB reject: skip far lines before any float math */
        vec2 a=lv->verts[ld->v1],b=lv->verts[ld->v2];
        float ex=b.x-a.x,ey=b.y-a.y,len2=ex*ex+ey*ey; if(len2<1.0f)continue;
        float t=((x-a.x)*ex+(y-a.y)*ey)/len2; if(t<0)t=0; if(t>1)t=1;
        float ddx=x-(a.x+t*ex),ddy=y-(a.y+t*ey); if(ddx*ddx+ddy*ddy>R*R)continue;
        if(ld->left<0||ld->right<0) return 1;                         /* one-sided = solid */
        const sector_t*fs=&MSECT[lv->sides[ld->right].sector];
        const sector_t*bs=&MSECT[lv->sides[ld->left].sector];
        float hif=fs->floor>bs->floor?fs->floor:bs->floor;
        float loc=fs->ceil<bs->ceil?fs->ceil:bs->ceil;
        if(hif-curfloor>24.0f) return 1;                             /* step too high */
        if(loc-hif<56.0f) return 1;                                  /* no headroom */
        if(ld->flags&1) return 1;                                    /* ML_BLOCKING */
    }
    return 0;
}
void move_player(const level_t*lv,camera_t*cam,float dx,float dy){
    float cf=MSECT[point_sector(lv,cam->pos.x,cam->pos.y)].floor;
    float nx=cam->pos.x+dx, ny=cam->pos.y+dy;
    if(!blocked(lv,nx,ny,cf)){ cam->pos.x=nx; cam->pos.y=ny; }       /* full move */
    else if(!blocked(lv,nx,cam->pos.y,cf)) cam->pos.x=nx;            /* slide X */
    else if(!blocked(lv,cam->pos.x,ny,cf)) cam->pos.y=ny;            /* slide Y */
    cam->sector=point_sector(lv,cam->pos.x,cam->pos.y);
}
static int seg_x(float ax,float ay,float bx,float by,float cx,float cy,float dx,float dy){
    float d1=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax),d2=(bx-ax)*(dy-ay)-(by-ay)*(dx-ax);
    float d3=(dx-cx)*(ay-cy)-(dy-cy)*(ax-cx),d4=(dx-cx)*(by-cy)-(dy-cy)*(bx-cx);
    return ((d1>0)!=(d2>0))&&((d3>0)!=(d4>0));
}
int hitscan(const level_t*lv,const camera_t*cam){
    float co=lut_cos(cam->ang),si=lut_sin(cam->ang); int best=-1; float bd=1e18f;
    for(int i=0;i<lv->nthings;i++){ if(dead[i])continue; const thing_t*th=&lv->things[i]; if(th->tex<0)continue;
        float dx=th->pos.x-cam->pos.x,dy=th->pos.y-cam->pos.y,fwd=dx*co+dy*si; if(fwd<8.0f)continue;
        float side=dx*si-dy*co; if(fabsf(side)>fwd*0.18f)continue;          /* ~10 deg aim cone */
        float dd=dx*dx+dy*dy; if(dd>=bd)continue;
        int hit=0; for(int j=0;j<lv->nlines && !hit;j++){ const linedef_t*ld=&lv->lines[j]; if(ld->left>=0)continue;
            vec2 p=lv->verts[ld->v1],q=lv->verts[ld->v2];
            if(seg_x(cam->pos.x,cam->pos.y,th->pos.x,th->pos.y,p.x,p.y,q.x,q.y)) hit=1; }   /* wall blocks shot */
        if(hit)continue; bd=dd; best=i;
    }
    return best;
}
void thing_kill(int i){ if(i>=0&&i<256) dead[i]=1; }

/* ---- mutable world state: doors (and lifts later) ---- */
void world_init(const level_t *lv){
    for(int i=0;i<lv->nsectors && i<256;i++){ MSECT[i]=lv->sectors[i]; dstate[i]=0; }
    for(int i=0;i<256;i++) secmid[i]=-1;
    for(int i=0;i<lv->nsides;i++){ const sidedef_t*sd=&lv->sides[i];
        if(sd->sector>=0&&sd->sector<256&&secmid[sd->sector]<0&&sd->midtex>=0) secmid[sd->sector]=(int16_t)sd->midtex; }
    for(int i=0;i<lv->nverts && i<MAXVERT;i++){ VXf[i]=rf(lv->verts[i].x); VYf[i]=rf(lv->verts[i].y); }
    for(int i=0;i<lv->nnodes && i<MAXNODE;i++){ NXi[i]=(int)lv->nodes[i].x; NYi[i]=(int)lv->nodes[i].y; NDXi[i]=(int)lv->nodes[i].dx; NDYi[i]=(int)lv->nodes[i].dy; }
    for(int i=0;i<lv->nsegs && i<MAXSEG;i++){ const seg_t*sg=&lv->segs[i];
        int dx=(int)lv->verts[sg->v2].x-(int)lv->verts[sg->v1].x, dy=(int)lv->verts[sg->v2].y-(int)lv->verts[sg->v1].y;
        SEGLEN[i]=ri(isqrt(dx*dx+dy*dy)); }
    for(int i=0;i<lv->nlines && i<MAXLINE;i++){ vec2 a=lv->verts[lv->lines[i].v1],b=lv->verts[lv->lines[i].v2];
        int ax=(int)a.x,bx=(int)b.x,ay=(int)a.y,by=(int)b.y;
        LBX0[i]=(short)((ax<bx?ax:bx)-20); LBX1[i]=(short)((ax>bx?ax:bx)+20);
        LBY0[i]=(short)((ay<by?ay:by)-20); LBY1[i]=(short)((ay>by?ay:by)+20); }   /* collision broad-phase */
}

static int is_door(int sp){
    return sp==1||sp==26||sp==27||sp==28||sp==31||sp==32||sp==33||sp==34||sp==117||sp==118;
}
static float neighbor_min_ceil(const level_t *lv,int sec){
    float m=1e18f; int found=0;
    for(int i=0;i<lv->nlines;i++){ const linedef_t *ld=&lv->lines[i]; if(ld->left<0||ld->right<0) continue;
        int fs=lv->sides[ld->right].sector, bs=lv->sides[ld->left].sector, other=-1;
        if(fs==sec) other=bs; else if(bs==sec) other=fs; else continue;
        if(MSECT[other].ceil<m){ m=MSECT[other].ceil; found=1; } }
    return found ? m-4.0f : MSECT[sec].floor+72.0f;
}
static void door_open(const level_t *lv,int sec){
    if(sec<0||sec>=256||dstate[sec]!=0) return;
    dclosed[sec]=MSECT[sec].ceil; dtarget[sec]=neighbor_min_ceil(lv,sec); dstate[sec]=1;
}
void world_update(const level_t *lv){
    const float SPD=2.0f;
    for(int i=0;i<lv->nsectors && i<256;i++){
        if(dstate[i]==1){ MSECT[i].ceil+=SPD; if(MSECT[i].ceil>=dtarget[i]){ MSECT[i].ceil=dtarget[i]; dstate[i]=2; dwait[i]=140; } }
        else if(dstate[i]==2){ if(--dwait[i]<=0) dstate[i]=3; }
        else if(dstate[i]==3){ MSECT[i].ceil-=SPD; if(MSECT[i].ceil<=dclosed[i]){ MSECT[i].ceil=dclosed[i]; dstate[i]=0; } }
    }
}
int use_door(const level_t *lv,const camera_t *cam){
    float co=lut_cos(cam->ang),si=lut_sin(cam->ang);
    float ex=cam->pos.x+co*64.0f, ey=cam->pos.y+si*64.0f;     /* 64-unit use range */
    int psec=point_sector(lv,cam->pos.x,cam->pos.y), bestsec=-1; float bd=1e18f;
    for(int i=0;i<lv->nlines;i++){ const linedef_t *ld=&lv->lines[i]; if(!is_door(ld->special)) continue;
        vec2 p=lv->verts[ld->v1],q=lv->verts[ld->v2];
        if(!seg_x(cam->pos.x,cam->pos.y,ex,ey,p.x,p.y,q.x,q.y)) continue;
        float mx=(p.x+q.x)*0.5f-cam->pos.x, my=(p.y+q.y)*0.5f-cam->pos.y, d=mx*mx+my*my;
        if(d<bd){ int fs=ld->right>=0?lv->sides[ld->right].sector:-1, bs=ld->left>=0?lv->sides[ld->left].sector:-1;
            int ds=(fs==psec)?bs:fs;                          /* the door is the far sector */
            if(ds>=0){ bd=d; bestsec=ds; } } }
    if(bestsec>=0) door_open(lv,bestsec);
    return bestsec;
}

/* Bake/tour helper: force every door to its open height up-front so the static bake renders
   THROUGH them and the navigable flood can pass. dstate stays 0 -> world_update won't re-close. */
void open_all_doors(const level_t *lv){
    for(int i=0;i<lv->nlines;i++){ const linedef_t *ld=&lv->lines[i]; if(!is_door(ld->special)) continue;
        int fs=ld->right>=0?lv->sides[ld->right].sector:-1, bs=ld->left>=0?lv->sides[ld->left].sector:-1;
        for(int k=0;k<2;k++){ int sec=k?bs:fs; if(sec<0||sec>=256) continue;
            float op=neighbor_min_ceil(lv,sec); if(op>MSECT[sec].ceil) MSECT[sec].ceil=op; } }
}
