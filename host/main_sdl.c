/* main_sdl.c -- HOST backend: textured wall rendering + free-look camera, view
 * modes (1 first / 2 second / 3 third / 4 top-down), and a stubbed weapon HUD.
 * Combat (z/x) is a stub until the gameplay layer exists. */
#include "dng.h"
#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* HERO-PATH TARGETS (E1M1 world coords): the demo dolly runs ARMOUR -> spawn -> EXIT.
   Retargeted from "leftmost/farthest cell" to the real green-armour pickup and the exit switch
   (the nearest reachable cell to each), so the route ends AT the exit, not in the room above it. */
#define ARMOUR_X (-224.0f)
#define ARMOUR_Y (-3232.0f)
#define EXIT_X    (2912.0f)
#define EXIT_Y   (-4768.0f)

static SDL_Color PAL[256];
typedef struct { int w, h; const uint8_t *pix; } Tex;
static Tex TEX[1024];
static int NTEXh = 0;
static uint8_t *CROM = NULL;

static const char *WEAPONS[] = { "Fist","Pistol","Shotgun","Chaingun","Rocket Launcher","Plasma Rifle","BFG9000" };

static int load_crom(const char *path){
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    CROM=malloc(sz); if(!CROM||fread(CROM,1,sz,f)!=(size_t)sz){ fclose(f); return 0; } fclose(f);
    for(int i=0;i<256;i++){ PAL[i].r=CROM[i*3]; PAL[i].g=CROM[i*3+1]; PAL[i].b=CROM[i*3+2]; PAL[i].a=255; }
    int p=768, ntex; memcpy(&ntex,CROM+p,4); p+=4;
    uint8_t *blob=CROM+p+ntex*8;
    for(int i=0;i<ntex && i<1024;i++){ short w,h; int ofs; memcpy(&w,CROM+p,2); memcpy(&h,CROM+p+2,2); memcpy(&ofs,CROM+p+4,4); p+=8; TEX[i].w=w; TEX[i].h=h; TEX[i].pix=blob+ofs; }
    NTEXh=ntex; return 1;
}

static int imod(int a,int n){ if(n<=0) return 0; int m=a%n; return m<0?m+n:m; }
static void putpx(SDL_Surface *fb,int x,int y,Uint32 c){ if(x<0||y<0||x>=SCREEN_W||y>=SCREEN_H) return; ((Uint32*)fb->pixels)[y*(fb->pitch/4)+x]=c; }
static void line(SDL_Surface *fb,int x0,int y0,int x1,int y1,Uint32 c){
    int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
    for(;;){ putpx(fb,x0,y0,c); if(x0==x1&&y0==y1) break; int e2=2*err; if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} }
}

static void draw_wall(SDL_Surface *fb,const SpriteCmd *c){
    if(c->tex<0||c->tex>=NTEXh||TEX[c->tex].w<=0){ SDL_Rect r={c->sx,c->sy,c->w,c->h}; SDL_FillRect(fb,&r,SDL_MapRGB(fb->format,140,40,140)); return; }
    Tex t=TEX[c->tex]; Uint32 *px=(Uint32*)fb->pixels; int pitch=fb->pitch/4; float l=c->light/255.0f;
    for(int cx=0;cx<c->w;cx++){
        int X=c->sx+cx; if(X<0||X>=SCREEN_W) continue;
        float fu=(c->w>1)?(float)cx/(c->w-1):0.0f; int U=imod((int)(c->uL+fu*(c->uR-c->uL)),t.w);
        const uint8_t *col=t.pix+U;
        for(int cy=0;cy<c->h;cy++){
            int Y=c->sy+cy; if(Y<0||Y>=SCREEN_H) continue;
            float fv=(c->h>1)?(float)cy/(c->h-1):0.0f; int V=imod((int)(c->v0+fv*(c->v1-c->v0)),t.h);
            SDL_Color k=PAL[col[V*t.w]];
            px[Y*pitch+X]=SDL_MapRGB(fb->format,(Uint8)(k.r*l),(Uint8)(k.g*l),(Uint8)(k.b*l));
        }
    }
}
/* floor/ceiling span: per-pixel floor-cast against the plane's world height.
 * (On real hardware this becomes coarse depth-bands; here we render the ideal.) */
/* F_SKY1 ceiling: a vertical-strip background scrolled by view angle (no perspective,
   no distance fade) -- the one piece that's *more* sprite-hardware-friendly than floors */
static void draw_sky(SDL_Surface *fb,const SpriteCmd *c,const camera_t *cam){
    if(SKY_TEX<0||SKY_TEX>=NTEXh){ SDL_Rect r={c->sx,c->sy,c->w,c->h};
        SDL_FillRect(fb,&r,SDL_MapRGB(fb->format,52,52,96)); return; }
    Tex t=TEX[SKY_TEX]; Uint32 *px=(Uint32*)fb->pixels; int pitch=fb->pitch/4;
    int scroll=(int)cam->ang/4;                          /* very gentle = far, near-static backdrop */
    for(int X=c->sx;X<c->sx+c->w;X++){ if(X<0||X>=SCREEN_W) continue;
        int U=imod(X*t.w/SCREEN_W - scroll, t.w);         /* MINUS scroll -> sky pans WITH the world (matches emit_nodeview) */
        for(int Y=c->sy;Y<c->sy+c->h;Y++){ if(Y<0||Y>=SCREEN_H) continue;
            SDL_Color k=PAL[t.pix[imod(Y,t.h)*t.w+U]];
            px[Y*pitch+X]=SDL_MapRGB(fb->format,k.r,k.g,k.b); } }
}
static void draw_flat(SDL_Surface *fb,const SpriteCmd *c,const camera_t *cam){
    if(c->tex==-2){ draw_sky(fb,c,cam); return; }            /* F_SKY1 ceiling */
    if(c->tex<0||c->tex>=NTEXh||TEX[c->tex].w<=0){   /* missing-texture sector: neutral floor-gray, NOT near-black -- the cart paints these via the uniform floor LUT, so they aren't real artefacts and shouldn't read as black squares in the tuner */
        SDL_Rect r={c->sx,c->sy,c->w,c->h}; SDL_FillRect(fb,&r,SDL_MapRGB(fb->format,86,86,90)); return;
    }
    Tex t=TEX[c->tex]; Uint32 *px=(Uint32*)fb->pixels; int pitch=fb->pitch/4;
    float horizon=HALF_H+cam->pitch, co=lut_cos(cam->ang), si=lut_sin(cam->ang), relz=cam->z-(float)c->v0;
    for(int Y=c->sy;Y<c->sy+c->h;Y++){
        if(Y<0||Y>=SCREEN_H) continue;
        float dy=Y-horizon; if(dy>-0.5f&&dy<0.5f) continue;
        float D=relz*PCFG.fov/dy; if(D<0) D=-D;
        float fwx=cam->pos.x+D*co, fwy=cam->pos.y+D*si, rt=D/PCFG.fov;
        float sh=1.0f-(D/1800.0f)*0.6f; if(sh<0.25f) sh=0.25f; if(sh>1.0f) sh=1.0f;
        for(int X=c->sx;X<c->sx+c->w;X++){
            if(X<0||X>=SCREEN_W) continue;
            float off=(X-HALF_W)*rt;
            int U=imod((int)floorf(fwx+off*si),t.w), V=imod((int)floorf(fwy-off*co),t.h);
            SDL_Color k=PAL[t.pix[V*t.w+U]];
            px[Y*pitch+X]=SDL_MapRGB(fb->format,(Uint8)(k.r*sh),(Uint8)(k.g*sh),(Uint8)(k.b*sh));
        }
    }
}
/* billboard sprite with transparency (palette index 255 = transparent) */
static void draw_thing(SDL_Surface *fb,const SpriteCmd *c){
    if(c->tex<0||c->tex>=NTEXh||TEX[c->tex].w<=0) return;
    Tex t=TEX[c->tex]; Uint32 *px=(Uint32*)fb->pixels; int pitch=fb->pitch/4; float l=c->light/255.0f;
    for(int cx=0;cx<c->w;cx++){ int X=c->sx+cx; if(X<0||X>=SCREEN_W) continue;
        float fu=(c->w>1)?(float)cx/(c->w-1):0.0f; int U=imod((int)(c->uL+fu*(c->uR-c->uL)),t.w);
        for(int cy=0;cy<c->h;cy++){ int Y=c->sy+cy; if(Y<0||Y>=SCREEN_H) continue;
            float fv=(c->h>1)?(float)cy/(c->h-1):0.0f; int V=imod((int)(c->v0+fv*(c->v1-c->v0)),t.h);
            uint8_t idx=t.pix[V*t.w+U]; if(idx==255) continue;
            SDL_Color k=PAL[idx]; px[Y*pitch+X]=SDL_MapRGB(fb->format,(Uint8)(k.r*l),(Uint8)(k.g*l),(Uint8)(k.b*l)); } }
}
static void draw(SDL_Surface *fb,const DrawList *dl,const camera_t *cam){
    for(int i=0;i<dl->n;i++){ const SpriteCmd *c=&dl->cmd[i];
        if(c->kind==SC_WALL) draw_wall(fb,c);
        else if(c->kind==SC_FLAT) draw_flat(fb,c,cam);
        else draw_thing(fb,c);
    }
}

/* placeholder doomguy billboard, for 2nd/3rd-person views */
/* Faithful host preview of the Neo Geo NODE-RENDER: rasterise the SAME packed
   records the ROM emits, mirroring emit_nodeview exactly -- per 16px screen chunk
   draw one texture tile-column (colbase+k)%wt, vertically scaled to band height.
   Uses host TEX[] pixels (true colours, not the 15-colour C-ROM quant) so it shows
   the geometry/layout the hardware produces, permission-free, cross-platform. */
static int isqrtI(int v){ if(v<=0)return 0; int x=v,y=(x+1)>>1; while(y<x){x=y;y=(x+v/x)>>1;} return x; }   /* match the cart's perspective flat split */
static void draw_nodeview(SDL_Surface *fb,const unsigned char *data,long off,int ang8){
    Uint32 *px=(Uint32*)fb->pixels; int pitch=fb->pitch/4;
    for(int y=0;y<SCREEN_H;y++){                          /* gray ceiling / floor backdrop */
        Uint32 c=(y<112)?SDL_MapRGB(fb->format,204,204,204):SDL_MapRGB(fb->format,68,68,68);
        for(int x=0;x<SCREEN_W;x++) px[y*pitch+x]=c;
    }
    /* (the old whole-column mask-sky draw is gone -- the tt=0x7F records paint the sky now; the 3 mask
       bytes are repurposed as the cart's "sky-from-top" LUT-ceiling-skip flag, irrelevant to this preview
       which doesn't draw the LUT.) */
    int cnt=data[off]|(data[off+1]<<8); off+=2; off+=3;   /* skip the 3-byte sky-from-top mask */
    for(int i=0;i<cnt;i++){
        int tt=data[off++]; int sx=data[off]|(data[off+1]<<8); off+=2;
        int sy=data[off++], w=data[off++], h=data[off++], colb=data[off++], dep=data[off++]; (void)dep;
        int du=(signed char)data[off++];
        int dtop=(signed char)data[off++], dbot=(signed char)data[off++];   /* 11-byte record: trapezoid top/bottom slope across the band (was missing -> 2-byte/record misalignment) */
        int wall=tt&0x80, tex=tt&0x7f;
        if(tt==0x7E){    /* THING billboard: scaled transparent textile blit (cart-faithful preview of the 0x7E record) */
            int ttex=colb; if(ttex<NTEXh&&TEX[ttex].w>0){ Tex tt2=TEX[ttex]; int tsx=(short)sx;
                float l=(dep&3)==0?1.0f:((dep&3)==1?0.875f:0.75f);   /* fog level -> brightness (mirror the palette ramp) */
                for(int cx=0;cx<w;cx++){ int X=tsx+cx; if(X<0||X>=SCREEN_W)continue;
                    int U=(w>1)?(cx*tt2.w/w):0; if(U>=tt2.w)U=tt2.w-1;
                    for(int cy=0;cy<h;cy++){ int Y=sy+cy; if(Y<0||Y>=SCREEN_H)continue;
                        int V=(h>1)?(cy*tt2.h/h):0; if(V>=tt2.h)V=tt2.h-1;
                        uint8_t idx=tt2.pix[V*tt2.w+U]; if(idx==255)continue;   /* transparent */
                        SDL_Color kc=PAL[idx];
                        px[Y*pitch+X]=SDL_MapRGB(fb->format,(Uint8)(kc.r*l),(Uint8)(kc.g*l),(Uint8)(kc.b*l)); } } }
            continue;
        }
        if(tt==0x7f){    /* SKY-THROUGH-OPENING: mirror emit_nodeview's sky-draw (tiled SKY1, angle-scrolled, screen-anchored) */
            if(SKY_TEX>=0&&SKY_TEX<NTEXh&&TEX[SKY_TEX].w>0){ Tex st=TEX[SKY_TEX]; int swt=st.w/16, sth=st.h/16;
                if(swt<1)swt=1; if(sth<1)sth=1;
                int sxL=sx&~15, syT=sy&~15;   /* SNAP to the 16px screen grid (see emit_nodeview): adjacent records share cells -> seamless */
                int schunks=((sx+w+15)>>4)-(sxL>>4); if(schunks<1)schunks=1;
                int subpx=(ang8&3)<<2; if(subpx) schunks++;   /* #15 sub-tile sky scroll (mirror the cart) */
                int sK=((sy+h+15)>>4)-(syT>>4); if(sK<1)sK=1; if(sK>32)sK=32;
                int srow0=(syT>>4);
                for(int c=0;c<schunks;c++){ int ssx=sxL+c*16-subpx;
                    int stc=(((ssx>>4)-(ang8>>2))%swt+swt)%swt;   /* ABS screen column - angle -> sky pans WITH the world */
                    for(int r=0;r<sK;r++){ int sr=((srow0+r)%sth+sth)%sth;   /* ABS screen row (wraps) */
                        for(int yy=0;yy<16;yy++){ int Y=syT+r*16+yy; if(Y<0||Y>=SCREEN_H)continue;
                            for(int xx=0;xx<16;xx++){ int X=ssx+xx; if(X<0||X>=SCREEN_W)continue;
                                int U=stc*16+xx; if(U>=st.w)U=st.w-1;
                                int V=sr*16+yy; if(V>=st.h)V=st.h-1;
                                SDL_Color kc=PAL[st.pix[V*st.w+U]];
                                px[Y*pitch+X]=SDL_MapRGB(fb->format,kc.r,kc.g,kc.b); } } } } }
            continue;
        }
        if(tex>=NTEXh||TEX[tex].w<=0){
            if(wall){ SDL_Rect r={sx,sy,w,h}; SDL_FillRect(fb,&r,SDL_MapRGB(fb->format,40,40,40)); }   /* untextured wall = dark fill (cart uses the dark-gray palette now) */
            continue;
        }
        Tex t=TEX[tex]; int wt=t.w/16; if(wt<1)wt=1; int th=t.h/16; if(th<1)th=1;
        if(wall){
            int chunks=(w+15)/16;
            for(int k=0;k<chunks;k++){
                /* CART-FAITHFUL: replicate emit_nodeview's integer-tile stack + caps + vshrink + index-0 alpha,
                   so the preview shows EXACTLY what the Neo Geo sprite hardware draws (incl. the square artefact). */
                int sx2=sx+k*16, cw=w-k*16; if(cw>16)cw=16; if(cw<1)cw=1;
                int col=(colb+((k*du)>>4))%wt; if(col<0)col+=wt; int u0=col*16;
                int xc=k*16+8; if(xc>w)xc=w;
                int chsy=sy+dtop*(2*xc-w)/(2*w); if(chsy<0)chsy=0; if(chsy>255)chsy=255;
                int chh=h+(dbot-dtop)*(2*xc-w)/(2*w); if(chh<1)chh=1;
                int dtc=(w>0)?dtop*16/w:0; if(dtc<-32)dtc=-32; if(dtc>32)dtc=32; { int a=dtc<0?-dtc:dtc; if(a>16){a=((a+2)>>2)<<2; dtc=dtc<0?-a:a;} }
                int dbc=(w>0)?dbot*16/w:0; if(dbc<-32)dbc=-32; if(dbc>32)dbc=32; { int a=dbc<0?-dbc:dbc; if(a>16){a=((a+2)>>2)<<2; dbc=dbc<0?-a:a;} }
                int adt=dtc<0?-dtc:dtc, adb=dbc<0?-dbc:dbc;
                int ntt=(adt+15)/16, ntb=(adb+15)/16; if(ntt<1)ntt=1; if(ntb<1)ntb=1;
                int rtop_ok=(adt>2), rbot_ok=(adb>2);          /* cap baked only for |drop|>2 (wad2c manifest) */
                int ytop=chsy-(rtop_ok?adt/2:0); if(ytop<0)ytop=0;
                int ybot=chsy+chh+(rbot_ok?adb/2:0);
                int cot=(ybot-ytop+15)/16; if(cot<1)cot=1; if(cot>32)cot=32;
                int vsh=(ybot-ytop)*255/(cot*16); if(vsh<1)vsh=1; if(vsh>255)vsh=255;
                int capbot=rbot_ok?(ntb<cot?ntb:cot):0;        /* bottom-priority (matches the cart fix) */
                int captop=rtop_ok?ntt:0; if(captop>cot-capbot)captop=cot-capbot; if(captop<0)captop=0;
                int Hp=ybot-ytop; if(Hp<1)Hp=1;
                for(int cx=0;cx<cw;cx++){ int X=sx2+cx; if(X<0||X>=SCREEN_W) continue;
                    int xi=(cw>1)?cx*16/cw:0; if(xi>15)xi=15;
                    int U=u0+(cw>1?cx*16/cw:0); if(U>=t.w)U=t.w-1;
                    for(int oy=0;oy<Hp;oy++){ int Y=ytop+oy; if(Y<0||Y>=SCREEN_H) continue;
                        int sr=oy*cot*16/Hp; if(sr>=cot*16)sr=cot*16-1; int r=sr/16, ty=sr%16; int V;
                        if(captop && r<captop){                                  /* TOP cap tile k=r */
                            int el=((dtc>=0)?(adt*xi)/15:(adt*(15-xi))/15)-r*16; if(ty<el)continue;   /* index-0 transparent above the edge */
                            V=(r%th)*16+ty;
                        } else if(capbot && r>=cot-capbot){                      /* BOTTOM cap tile kk=cot-1-r */
                            int kk=cot-1-r; int elb=(kk+1)*16-((dbc>=0)?(adb*(15-xi))/15:(adb*xi)/15); if(ty>=elb)continue;  /* transparent below the edge */
                            V=((th-1-kk)%th)*16+ty;
                        } else { int srow=(cot>1)?(r*th/cot):0; if(srow>=th)srow=th-1; V=srow*16+ty; }   /* body */
                        if(V>=t.h)V=t.h-1;
                        SDL_Color kk=PAL[t.pix[V*t.w+U]];
                        px[Y*pitch+X]=SDL_MapRGB(fb->format,kk.r,kk.g,kk.b); } }
            }
        } else if(colb&0x80){                             /* flat: solid fallback */
            SDL_Color kk=PAL[t.pix[(t.h/2)*t.w + t.w/2]];
            SDL_Rect r={sx,sy,w,h}; SDL_FillRect(fb,&r,SDL_MapRGB(fb->format,kk.r,kk.g,kk.b));
        } else {                                          /* flat: textured rect, PER-BAND depth rows + PERSPECTIVE 2-zone foreshortening (mirror the cart) */
            int rows=t.h/16; if(rows<1)rows=1; int cols=t.w/16; if(cols<1)cols=1;
            int fV[8]; fV[0]=dep&0xf; fV[1]=(dep>>4)&0xf; fV[2]=du&0xf; fV[3]=(du>>4)&0xf; fV[4]=dtop&0xf; fV[5]=(dtop>>4)&0xf; fV[6]=dbot&0xf; fV[7]=(dbot>>4)&0xf;
            int isceil=(colb>>6)&1;
            int nz=(h>=128)?4:((h>=64)?3:((h>=16)?2:1)); int zh_[4]; zh_[0]=h; zh_[1]=0; zh_[2]=0; zh_[3]=0;
            if(nz>1){ int H2=112,df,dn; if(!isceil){ df=sy-H2; dn=(sy+h)-H2; } else { df=H2-(sy+h); dn=H2-sy; }
                if(df<1)df=1; if(dn<df+nz)dn=df+nz;
                int b[5]; b[0]=df; b[nz]=dn;
                { int dm=isqrtI(df*dn);
                  if(nz==2){ b[1]=dm; }
                  else if(nz==3){ b[1]=isqrtI(df*dm); b[2]=isqrtI(dm*dn); }
                  else { b[2]=dm; b[1]=isqrtI(df*dm); b[3]=isqrtI(dm*dn); } }
                for(int z2=1;z2<nz;z2++) if(b[z2]<=b[z2-1]) b[z2]=b[z2-1]+1;
                for(int z2=nz-1;z2>=1;z2--) if(b[z2]>=b[z2+1]) b[z2]=b[z2+1]-1;
                { int sgm[4]; int ok=1; for(int z2=0;z2<nz;z2++){ sgm[z2]=b[z2+1]-b[z2]; if(sgm[z2]<1)ok=0; }
                  if(ok){
                    for(int z2=0;z2<nz;z2++) zh_[z2] = isceil ? sgm[nz-1-z2] : sgm[z2];
                    for(int z2=0;z2<nz;z2++) if(zh_[z2]<8){ int mx=0; for(int q=1;q<nz;q++) if(zh_[q]>zh_[mx])mx=q; zh_[mx]-=8-zh_[z2]; zh_[z2]=8; }
                    { int tot=0; for(int z2=0;z2<nz;z2++)tot+=zh_[z2]; zh_[nz-1]+=h-tot; if(zh_[nz-1]<1){ zh_[nz-2]+=zh_[nz-1]-1; zh_[nz-1]=1; } }
                  } else { nz=1; zh_[0]=h; } } }
            int zcot_[4], growtot=0; for(int z2=0;z2<nz;z2++){ int zh=zh_[z2]; if(zh<1)zh=1; int zc=(zh+15)/16; if(zc<1)zc=1; if(zc>8)zc=8; zcot_[z2]=zc; growtot+=zc; }
            for(int xx=0;xx<w;xx++){ int X=sx+xx; if(X<0||X>=SCREEN_W) continue;
                int col=(xx>>4)%cols; int U=col*16+(xx&15); if(U>=t.w)U=t.w-1;
                for(int cy=0;cy<h;cy++){ int Y=sy+cy; if(Y<0||Y>=SCREEN_H) continue;
                    int z=0, zy0=0, gbase=0; while(z<nz-1 && cy>=zy0+zh_[z]){ zy0+=zh_[z]; gbase+=zcot_[z]; z++; }
                    int zh=zh_[z]; if(zh<1)zh=1; int zcot=zcot_[z];
                    int zcy=cy-zy0, srcy=zcy*zcot*16/zh; int r=srcy>>4, ty=srcy&15; if(r>=zcot)r=zcot-1;
                    int vi=(nz==1)?r:(((gbase+r)*8)/growtot); if(vi>7)vi=7;
                    int sr=fV[vi]; if(sr>=rows)sr=rows-1;
                    int V=sr*16+ty; if(V>=t.h)V=t.h-1;
                    SDL_Color kk=PAL[t.pix[V*t.w+U]];
                    px[Y*pitch+X]=SDL_MapRGB(fb->format,kk.r,kk.g,kk.b); } }
        }
    }
}
/* ===================== LIVE NODE-RENDER TUNER =====================
   Renders the dl in the CART's style (per-chunk trapezoid + diagonal-alpha caps
   + depth fog + draw-distance cull) live for any free-look camera, with every
   knob we've tuned exposed as an on-screen slider. Lets us explore draw distance,
   fog, caps, etc. on one frame without rebuilding the ROM. */
typedef struct { const char*name; float val,mn,mx,step; } Tunable;
static Tunable TUNE[]={
    {"drawdist",4000,128,4000,128},  /* 0: max wall world-depth (>=max = no cull). Default 4000 = full map (matches the bake's PCFG.far) */
    {"fogMidAt", 224,  0,2400,16},  /* 1: world-depth where MID fog starts */
    {"fogFarAt", 480,  0,2400,16},  /* 2: world-depth where FAR fog starts (>=here = darkest) */
    {"fogMidB", 0.85f, 0,   1,0.05f},/* 3: brightness in the MID band (1 = no dim) */
    {"fogFarB",   1,   0,   1,0.05f},/* 4: brightness in the FAR band (1 = no dim) */
    {"murk",    0.12f, 0,   1,0.02f},/* 5: backdrop brightness (where nothing is drawn) */
    {"cullMinW",  1,   0,  40,1},   /* 6: drop walls narrower than this (px) */
    {"caps",      1,   0,   1,1},   /* 7: diagonal-alpha smooth caps on/off */
    {"capDrop",  32,   0,  48,4},   /* 8: max cap drop (RDMAX, px) */
    {"capBottom", 1,   0,   1,1},   /* 9: 0=top-priority caps, 1=bottom-priority */
    {"capExt",  0.50f, 0,   1,0.05f},/*10: cap downward/upward extension (0=none .. 0.5=cart) */
    {"snapOn",    0,   0,   1,1},   /*11: simulate the on-rails discretisation (angle + position snap) */
    {"snapAng",  45,   6, 128,1},   /*12: baked angle count -- cart NODE_NA=45 (smaller = coarser rotation) */
    {"snapStep", 24,   4,  96,2},   /*13: node spacing in world units -- cart NODE_S=24 (bigger = coarser steps) */
    {"fov",     160,  80, 240,8},   /*14: focal length px; horizontal FOV deg = 2*atan(160/fov). 160 = 90 deg. LIVE PREVIEW ONLY -- the bake is fixed at 160; change PCFG.fov to re-bake at another FOV. */
};
enum { NTUNE=(int)(sizeof(TUNE)/sizeof(TUNE[0])) };
enum { T_DRAWDIST,T_FOGMIDAT,T_FOGFARAT,T_FOGMIDB,T_FOGFARB,T_MURK,T_CULLMINW,T_CAPS,T_CAPDROP,T_CAPBOT,T_CAPEXT,T_SNAPON,T_SNAPANG,T_SNAPSTEP,T_FOV };
static int tune_sel=0;
static int g_spr=0,g_line=0; static float g_fps=0; static long g_rom_prom=0,g_rom_nodes=0,g_rom_crom=0;   /* stats panel */
static long filesize(const char*p){ FILE*f=fopen(p,"rb"); if(!f)return 0; fseek(f,0,SEEK_END); long s=ftell(f); fclose(f); return s; }
#define TV(i) (TUNE[i].val)

static void draw_dl_nodestyle(SDL_Surface *fb,const DrawList *dl,const camera_t *cam){
    Uint32 *px=(Uint32*)fb->pixels; int pitch=fb->pitch/4;
    int mk=(int)(TV(T_MURK)*70);                                       /* murk backdrop (shows where flats/walls don't cover) */
    for(int y=0;y<SCREEN_H;y++){ int b=(y<112)?mk+36:mk; if(b>255)b=255; Uint32 c=SDL_MapRGB(fb->format,b,b,b+2>255?255:b+2);
        for(int x=0;x<SCREEN_W;x++) px[y*pitch+x]=c; }
    for(int i=0;i<dl->n;i++){ const SpriteCmd *c=&dl->cmd[i]; if(c->kind==SC_FLAT) draw_flat(fb,c,cam); }   /* real floor + ceiling (LUT-quality stand-in) */
    int RDM=(int)TV(T_CAPDROP), CAPS=(int)TV(T_CAPS), CPRI=(int)TV(T_CAPBOT), CMW=(int)TV(T_CULLMINW);
    for(int i=0;i<dl->n;i++){ const SpriteCmd *c=&dl->cmd[i];
        if(c->kind!=SC_WALL||c->w<=0||c->h<=0) continue;
        int dep=c->depth; if(dep>(int)TV(T_DRAWDIST)) continue; if(c->w<CMW) continue;
        int tex=c->tex; if(tex<0||tex>=NTEXh||TEX[tex].w<=0) continue;
        Tex t=TEX[tex]; int th=t.h/16; if(th<1)th=1;
        int sx=c->sx, sy=c->sy, w=c->w, h=c->h, dtop=c->dtop, dbot=c->dbot;
        float fbr=1.0f; if(dep>(int)TV(T_FOGFARAT)) fbr=TV(T_FOGFARB); else if(dep>(int)TV(T_FOGMIDAT)) fbr=TV(T_FOGMIDB);
        int chunks=(w+15)/16;
        for(int k=0;k<chunks;k++){
            int sx2=sx+k*16, cw=w-k*16; if(cw>16)cw=16; if(cw<1)cw=1;
            int xc=k*16+8; if(xc>w)xc=w;
            int chsy=sy+dtop*(2*xc-w)/(2*w); if(chsy<0)chsy=0; if(chsy>255)chsy=255;
            int chh=h+(dbot-dtop)*(2*xc-w)/(2*w); if(chh<1)chh=1;
            int dtc=(w>0)?dtop*16/w:0; if(dtc<-RDM)dtc=-RDM; if(dtc>RDM)dtc=RDM;
            int dbc=(w>0)?dbot*16/w:0; if(dbc<-RDM)dbc=-RDM; if(dbc>RDM)dbc=RDM;
            int adt=dtc<0?-dtc:dtc, adb=dbc<0?-dbc:dbc;
            int ntt=(adt+15)/16, ntb=(adb+15)/16; if(ntt<1)ntt=1; if(ntb<1)ntb=1;
            int rtop_ok=CAPS&&(adt>2), rbot_ok=CAPS&&(adb>2);
            int ytop=chsy-(rtop_ok?(int)(adt*TV(T_CAPEXT)):0); if(ytop<0)ytop=0;
            int ybot=chsy+chh+(rbot_ok?(int)(adb*TV(T_CAPEXT)):0);
            int cot=(ybot-ytop+15)/16; if(cot<1)cot=1; if(cot>32)cot=32;
            int captop,capbot;
            if(CPRI){ capbot=rbot_ok?(ntb<cot?ntb:cot):0; captop=rtop_ok?ntt:0; if(captop>cot-capbot)captop=cot-capbot; }
            else    { captop=rtop_ok?(ntt<cot?ntt:cot):0; capbot=rbot_ok?ntb:0; if(capbot>cot-captop)capbot=cot-captop; }
            if(captop<0)captop=0; if(capbot<0)capbot=0;
            int Hp=ybot-ytop; if(Hp<1)Hp=1;
            for(int cx=0;cx<cw;cx++){ int X=sx2+cx; if(X<0||X>=SCREEN_W) continue;
                int xi=(cw>1)?cx*16/cw:0; if(xi>15)xi=15;
                int U=c->uL+(X-sx)*(c->uR-c->uL)/(w>0?w:1); U=((U%t.w)+t.w)%t.w;
                for(int oy=0;oy<Hp;oy++){ int Y=ytop+oy; if(Y<0||Y>=SCREEN_H) continue;
                    int sr=oy*cot*16/Hp; if(sr>=cot*16)sr=cot*16-1; int r=sr/16, ty=sr%16; int V;
                    if(captop&&r<captop){ int el=((dtc>=0)?(adt*xi)/15:(adt*(15-xi))/15)-r*16; if(ty<el)continue; V=(r%th)*16+ty; }
                    else if(capbot&&r>=cot-capbot){ int kk=cot-1-r; int elb=(kk+1)*16-((dbc>=0)?(adb*(15-xi))/15:(adb*xi)/15); if(ty>=elb)continue; V=((th-1-kk)%th)*16+ty; }
                    else { int srow=(cot>1)?(r*th/cot):0; if(srow>=th)srow=th-1; V=srow*16+ty; }
                    if(V>=t.h)V=t.h-1;
                    SDL_Color kk=PAL[t.pix[V*t.w+U]];
                    px[Y*pitch+X]=SDL_MapRGB(fb->format,(Uint8)(kk.r*fbr),(Uint8)(kk.g*fbr),(Uint8)(kk.b*fbr)); } }
        }
    }
}
/* tiny 5x7 column-major font (bit0=top), uppercase + digits + a few symbols */
static const unsigned char FONT5[96][5]={
 [' '-32]={0,0,0,0,0},['-'-32]={8,8,8,8,8},['.'-32]={0,0x60,0x60,0,0},[':'-32]={0,0x36,0x36,0,0},['='-32]={0x14,0x14,0x14,0x14,0x14},['/'-32]={0x20,0x10,8,4,2},
 ['0'-32]={0x3E,0x51,0x49,0x45,0x3E},['1'-32]={0,0x42,0x7F,0x40,0},['2'-32]={0x42,0x61,0x51,0x49,0x46},['3'-32]={0x21,0x41,0x45,0x4B,0x31},['4'-32]={0x18,0x14,0x12,0x7F,0x10},
 ['5'-32]={0x27,0x45,0x45,0x45,0x39},['6'-32]={0x3C,0x4A,0x49,0x49,0x30},['7'-32]={1,0x71,9,5,3},['8'-32]={0x36,0x49,0x49,0x49,0x36},['9'-32]={6,0x49,0x49,0x29,0x1E},
 ['A'-32]={0x7E,0x11,0x11,0x11,0x7E},['B'-32]={0x7F,0x49,0x49,0x49,0x36},['C'-32]={0x3E,0x41,0x41,0x41,0x22},['D'-32]={0x7F,0x41,0x41,0x22,0x1C},['E'-32]={0x7F,0x49,0x49,0x49,0x41},
 ['F'-32]={0x7F,9,9,9,1},['G'-32]={0x3E,0x41,0x49,0x49,0x7A},['H'-32]={0x7F,8,8,8,0x7F},['I'-32]={0,0x41,0x7F,0x41,0},['J'-32]={0x20,0x40,0x41,0x3F,1},
 ['K'-32]={0x7F,8,0x14,0x22,0x41},['L'-32]={0x7F,0x40,0x40,0x40,0x40},['M'-32]={0x7F,2,0x0C,2,0x7F},['N'-32]={0x7F,4,8,0x10,0x7F},['O'-32]={0x3E,0x41,0x41,0x41,0x3E},
 ['P'-32]={0x7F,9,9,9,6},['Q'-32]={0x3E,0x41,0x51,0x21,0x5E},['R'-32]={0x7F,9,0x19,0x29,0x46},['S'-32]={0x46,0x49,0x49,0x49,0x31},['T'-32]={1,1,0x7F,1,1},
 ['U'-32]={0x3F,0x40,0x40,0x40,0x3F},['V'-32]={0x1F,0x20,0x40,0x20,0x1F},['W'-32]={0x7F,0x20,0x18,0x20,0x7F},['X'-32]={0x63,0x14,8,0x14,0x63},['Y'-32]={7,8,0x70,8,7},['Z'-32]={0x61,0x51,0x49,0x45,0x43},
};
static void draw_text(SDL_Surface*fb,int x,int y,const char*s,Uint32 col){
    Uint32*px=(Uint32*)fb->pixels; int pitch=fb->pitch/4;
    for(;*s;s++){ int c=*s; if(c>='a'&&c<='z')c-=32; if(c<32||c>=128)c=' ';
        const unsigned char*g=FONT5[c-32];
        for(int cx=0;cx<5;cx++)for(int ry=0;ry<7;ry++) if(g[cx]&(1<<ry)){ int X=x+cx,Y=y+ry; if(X>=0&&X<SCREEN_W&&Y>=0&&Y<SCREEN_H) px[Y*pitch+X]=col; }
        x+=6; }
}
static void draw_tuner_hud(SDL_Surface *fb){
    Uint32 *px=(Uint32*)fb->pixels; int pitch=fb->pitch/4; int bw=64;
    Uint32 white=SDL_MapRGB(fb->format,235,235,235), yellow=SDL_MapRGB(fb->format,255,235,0);
    for(int i=0;i<NTUNE;i++){ int y0=3+i*9; int sel=(i==tune_sel);
        float f=(TUNE[i].val-TUNE[i].mn)/(TUNE[i].mx-TUNE[i].mn); if(f<0)f=0; if(f>1)f=1;
        int fillw=(int)(f*(bw-2));
        Uint32 bord=sel?yellow:SDL_MapRGB(fb->format,80,80,80);
        Uint32 fil =sel?SDL_MapRGB(fb->format,255,200,0):SDL_MapRGB(fb->format,70,150,70);
        Uint32 bg  =SDL_MapRGB(fb->format,18,18,18);
        for(int x=0;x<bw;x++) for(int yy=0;yy<7;yy++){ int X=3+x,Y=y0+yy; if(X>=SCREEN_W||Y>=SCREEN_H)continue;
            px[Y*pitch+X]=(yy==0||yy==6||x==0||x==bw-1)?bord:((x-1<fillw)?fil:bg); }
        char buf[48]; snprintf(buf,sizeof buf,"%s %g",TUNE[i].name,TUNE[i].val);
        draw_text(fb,3+bw+4,y0,buf,sel?yellow:white);
    }
    draw_text(fb,3,3+NTUNE*9+2,"UP/DN SELECT  -/= ADJUST",SDL_MapRGB(fb->format,160,160,160));
    int yb=3+NTUNE*9+12; char s2[64]; Uint32 cyan=SDL_MapRGB(fb->format,120,220,220);
    snprintf(s2,sizeof s2,"SPR %d/381  LINE %d/96",g_spr,g_line); draw_text(fb,3,yb,s2,cyan);
    snprintf(s2,sizeof s2,"FPS %d",(int)(g_fps+0.5f)); draw_text(fb,3,yb+9,s2,cyan);
    snprintf(s2,sizeof s2,"PROM %ldK  NODES %ldK",g_rom_prom/1024,g_rom_nodes/1024); draw_text(fb,3,yb+18,s2,cyan);
    snprintf(s2,sizeof s2,"CROM %ldK (c1+c2)",g_rom_crom/1024); draw_text(fb,3,yb+27,s2,cyan);
    snprintf(s2,sizeof s2,"FOV %d deg",(int)(2.0f*atan2f(160.0f,PCFG.fov)*57.2958f+0.5f)); draw_text(fb,3,yb+36,s2,cyan);
}
static void tuner_print(void){ printf("[tuner] >%s< = %.2f   (TAB=next  -/=:adjust)\n",TUNE[tune_sel].name,TUNE[tune_sel].val); }

static void draw_avatar(SDL_Surface *fb,const camera_t *rc,vec2 ppos,float pz){
    float dx=ppos.x-rc->pos.x, dy=ppos.y-rc->pos.y, co=lut_cos(rc->ang), si=lut_sin(rc->ang);
    float depth=dx*co+dy*si; if(depth<8) return;
    float side=dx*si-dy*co, sc=PCFG.fov/depth, cx=HALF_W+side*sc, HZ=HALF_H+rc->pitch;
    float feet=HZ-((pz-41)-rc->z)*sc, head=HZ-((pz+15)-rc->z)*sc, w=28*sc;
    SDL_Rect r={(int)(cx-w/2),(int)head,(int)w,(int)(feet-head)};
    SDL_FillRect(fb,&r,SDL_MapRGB(fb->format,80,200,90));
}

static void render_topdown(SDL_Surface *fb,const level_t *lv,const camera_t *cam){
    SDL_FillRect(fb,NULL,SDL_MapRGB(fb->format,12,12,16));
    float s=0.07f, co=lut_cos(cam->ang), si=lut_sin(cam->ang);
    Uint32 solid=SDL_MapRGB(fb->format,210,210,210), portal=SDL_MapRGB(fb->format,90,90,120);
    for(int i=0;i<lv->nlines;i++){ const linedef_t *ld=&lv->lines[i];
        vec2 a=lv->verts[ld->v1], b=lv->verts[ld->v2];
        float ax=a.x-cam->pos.x, ay=a.y-cam->pos.y, bx=b.x-cam->pos.x, by=b.y-cam->pos.y;
        int ax2=HALF_W+(int)((ax*si-ay*co)*s), ay2=HALF_H-(int)((ax*co+ay*si)*s);
        int bx2=HALF_W+(int)((bx*si-by*co)*s), by2=HALF_H-(int)((bx*co+by*si)*s);
        line(fb,ax2,ay2,bx2,by2,(ld->left>=0)?portal:solid);
    }
    Uint32 p=SDL_MapRGB(fb->format,240,60,60);                 /* player arrow, pointing up */
    line(fb,HALF_W,HALF_H+4,HALF_W,HALF_H-8,p);
    line(fb,HALF_W,HALF_H-8,HALF_W-4,HALF_H-3,p);
    line(fb,HALF_W,HALF_H-8,HALF_W+4,HALF_H-3,p);
}

static void render_view(SDL_Surface *fb,const level_t *lv,const camera_t *pcam,int mode,DrawList *dl){
    if(mode==4){ render_topdown(fb,lv,pcam); return; }
    camera_t rc=*pcam; float co=lut_cos(pcam->ang), si=lut_sin(pcam->ang);
    if(mode==2){ rc.pos.x=pcam->pos.x+co*96; rc.pos.y=pcam->pos.y+si*96; rc.ang=(angle_t)(pcam->ang+128); }   /* facing player */
    else if(mode==3){ rc.pos.x=pcam->pos.x-co*112; rc.pos.y=pcam->pos.y-si*112; rc.z=pcam->z+40; }            /* behind player */
    render_world(lv,&rc,dl); SDL_FillRect(fb,NULL,0); draw(fb,dl,&rc);
    if(mode==2||mode==3) draw_avatar(fb,&rc,pcam->pos,pcam->z);
}

static void report(const DrawList *dl){ int over=(dl->spr_total>NG_MAX_SPR_FRAME)||(dl->line_max>NG_MAX_SPR_LINE);
    int th=0; for(int i=0;i<dl->n;i++) if(dl->cmd[i].kind==SC_THING) th++;
    printf("cmds=%d hw=%d/%d worst=%d/%d things=%d -> %s | segs=%d toviews=%d DIVIDES=%d wallbands=%d flatfills=%d\n",dl->n,dl->spr_total,NG_MAX_SPR_FRAME,dl->line_max,NG_MAX_SPR_LINE,th,over?"OVER":"fits",RSTAT.segs,RSTAT.toviews,RSTAT.divides,RSTAT.wall_bands,RSTAT.flat_fills); }

/* ---- top-down map-dump helpers (debug viz only) ---- */
static void mput(SDL_Surface*s,int x,int y,Uint32 c){ if(x<0||y<0||x>=s->w||y>=s->h)return; ((Uint32*)s->pixels)[y*(s->pitch/4)+x]=c; }
static void mbox(SDL_Surface*s,int cx,int cy,int hw,Uint32 c){ for(int y=-hw;y<=hw;y++)for(int x=-hw;x<=hw;x++)mput(s,cx+x,cy+y,c); }
static void mdisc(SDL_Surface*s,int cx,int cy,int r,Uint32 c){ for(int y=-r;y<=r;y++)for(int x=-r;x<=r;x++)if(x*x+y*y<=r*r)mput(s,cx+x,cy+y,c); }
static void mline(SDL_Surface*s,int x0,int y0,int x1,int y1,Uint32 c){ int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy;
    for(;;){ mput(s,x0,y0,c); if(x0==x1&&y0==y1)break; int e2=2*e; if(e2>=dy){e+=dy;x0+=sx;} if(e2<=dx){e+=dx;y0+=sy;} } }

/* Is the straight cell-line a..b fully navigable AND >= marg cells clear of walls? (string-pull visibility) */
/* shortest path between two cells over the reach grid (4-neighbour BFS). Used to chain the
   rail through forced WAYPOINTS (the zigzag-bridge centreline): the spawn-rooted move_player
   tree can only express spawn->X routes, and the simplifier would otherwise straighten the
   bridge's iconic S into a diagonal. out[] gets cells from a to b inclusive; returns count. */
static int grid_route(int a,int b,int NX,int NC,const unsigned char*reach,int*out,int outcap){
    static int *gpar=NULL,*gq=NULL; if(!gpar){ gpar=(int*)malloc((long)NC*4); gq=(int*)malloc((long)NC*4); }
    for(int i=0;i<NC;i++) gpar[i]=-2;
    int qh=0,qt=0; gpar[a]=-1; gq[qt++]=a;
    while(qh<qt && gpar[b]==-2){ int c=gq[qh++],ix=c%NX;
        int nb[4]={c+1,c-1,c+NX,c-NX};
        for(int d=0;d<4;d++){ int n=nb[d]; if(n<0||n>=NC)continue;
            if(d==0&&ix==NX-1)continue; if(d==1&&ix==0)continue;
            if(!reach[n]||gpar[n]!=-2)continue; gpar[n]=c; gq[qt++]=n; } }
    if(gpar[b]==-2) return 0;
    int n=0; for(int c=b;c>=0&&n<outcap;c=gpar[c]) out[n++]=c;
    for(int i=0;i<n/2;i++){ int t=out[i]; out[i]=out[n-1-i]; out[n-1-i]=t; }   /* a -> b order */
    return n;
}
static int line_clear(int a,int b,int NX,const unsigned char*reach,const int*clr,int marg){
    int ax=a%NX,ay=a/NX,bx=b%NX,by=b/NX;
    int dx=abs(bx-ax),sx=ax<bx?1:-1,dy=-abs(by-ay),sy=ay<by?1:-1,e=dx+dy;
    for(;;){ int c=ay*NX+ax; if(!reach[c]||clr[c]<marg) return 0;
        if(ax==bx&&ay==by) return 1; int e2=2*e; if(e2>=dy){e+=dy;ax+=sx;} if(e2<=dx){e+=dx;ay+=sy;} }
}
/* STRAIGHTEN the hero path: string-pull it into taut line segments (keeping a clearance margin so it
   stays off the walls), then resample at S-unit spacing. Removes the medial-axis "fluting" -> the
   node budget reaches FAR further along the level. Returns the new node count (<= cap). */

static int pathang_quick(float*PX,float*PY,int N,int p){
    int b2=(p+1<N)?p+1:p, a0=(p+1<N)?p:(p>0?p-1:0);
    float tx=PX[b2]-PX[a0], ty=PY[b2]-PY[a0]; int best=0; float bd=-1e30f;
    for(int q=0;q<24;q++){ angle_t aa=(angle_t)(q*256/24); float d=tx*lut_cos(aa)+ty*lut_sin(aa); if(d>bd){bd=d;best=q;} }
    return best*256/24;
}
static int straighten_path(int R,int S,const unsigned char*reach,float*PX,float*PY,int N,int cap){
    int minx=(int)MAP_START.pos.x-R, miny=(int)MAP_START.pos.y-R, NX=2*R/S+1, NY=2*R/S+1; long NC=(long)NX*NY;
    int *clr=(int*)malloc(NC*sizeof(int)), *cq=(int*)malloc(NC*sizeof(int)); long ch=0,ct=0;
    int dx4[4]={1,-1,0,0},dy4[4]={0,0,1,-1};
    for(long c=0;c<NC;c++){ if(!reach[c]){clr[c]=0;cq[ct++]=(int)c;} else clr[c]=-1; }   /* clearance field (cells from nearest wall) */
    while(ch<ct){ int c=cq[ch++],ix=c%NX,iy=c/NX;
        for(int d=0;d<4;d++){ int nix=ix+dx4[d],niy=iy+dy4[d]; if(nix<0||niy<0||nix>=NX||niy>=NY)continue;
            long nc=(long)niy*NX+nix; if(clr[nc]!=-1)continue; clr[nc]=clr[c]+1; cq[ct++]=(int)nc; } }
    int *cell=(int*)malloc((long)(N>0?N:1)*sizeof(int));
    for(int i=0;i<N;i++){ int ix=(int)((PX[i]-minx)/S), iy=(int)((PY[i]-miny)/S);
        if(ix<0)ix=0; if(iy<0)iy=0; if(ix>=NX)ix=NX-1; if(iy>=NY)iy=NY-1; cell[i]=iy*NX+ix; }
    int MARG=4; static int keep[4096]; int kn=0; if(N>0) keep[kn++]=cell[0];   /* string-pull: taut waypoints, >=4 cells (~96u) off walls (the author: "desperate to move it off the wall") */
    for(int i=0;i<N-1 && kn<4096;){ int best=i+1;
        for(int j=i+2;j<N;j++){ if(line_clear(cell[i],cell[j],NX,reach,clr,MARG)) best=j; else break; }
        keep[kn++]=cell[best]; i=best; }
    int M=0; float need=(float)S*1.25f;                                  /* resample the taut polyline at S-unit spacing */
    if(kn>0){ float px=minx+(keep[0]%NX)*S+S*0.5f, py=miny+(keep[0]/NX)*S+S*0.5f; PX[M]=px; PY[M]=py; M++;
        for(int w=1;w<kn && M<cap;w++){ float wx=minx+(keep[w]%NX)*S+S*0.5f, wy=miny+(keep[w]/NX)*S+S*0.5f;
            float dx=wx-px,dy=wy-py,seg=sqrtf(dx*dx+dy*dy); if(seg<0.001f){px=wx;py=wy;continue;}
            float ux=dx/seg,uy=dy/seg,pos=0;
            while(pos+need<=seg && M<cap){ pos+=need; PX[M]=px+ux*pos; PY[M]=py+uy*pos; M++; need=(float)S*1.25f; }
            need-=(seg-pos); px=wx; py=wy; } }
    /* LATERAL CENTRING + MIN-CLEARANCE: in a tight corridor (both walls <300u) ride the midpoint;
       otherwise (one wall close, one open -- e.g. the spawn area) just push off the near wall to a
       minimum clearance so the dolly never sandpapers a wall. Covers the ENDPOINTS too (i=0,M-1) so
       the Laplacian can't drag the start back onto the spawn wall. */
    const int MINCLR=128;   /* open areas: ride at least 128u off the near wall (was 88 -- still read as wall-hugging) */
    static float shv[4096], pxv[4096], pyv[4096];
    for(int i=0;i<M&&i<4096;i++){ shv[i]=0; pxv[i]=0; pyv[i]=1; }
    for(int i=0;i<M;i++){
        float tx,ty;
        if(i==0){tx=PX[1]-PX[0];ty=PY[1]-PY[0];} else if(i==M-1){tx=PX[i]-PX[i-1];ty=PY[i]-PY[i-1];} else {tx=PX[i+1]-PX[i-1];ty=PY[i+1]-PY[i-1];}
        float tl=sqrtf(tx*tx+ty*ty); if(tl<0.001f)continue;
        float pxn=-ty/tl, pyn=tx/tl, dR=0, dL=0;
        for(int d=4; d<=360; d+=4){ int ix=(int)((PX[i]+pxn*d-minx)/S),iy=(int)((PY[i]+pyn*d-miny)/S); if(ix<0||iy<0||ix>=NX||iy>=NY||!reach[iy*NX+ix])break; dR=d; }
        for(int d=4; d<=360; d+=4){ int ix=(int)((PX[i]-pxn*d-minx)/S),iy=(int)((PY[i]-pyn*d-miny)/S); if(ix<0||iy<0||ix>=NX||iy>=NY||!reach[iy*NX+ix])break; dL=d; }
        float sh=0;
        if(dR>0&&dL>0&&dR<480&&dL<480)      sh=(dR-dL)*0.5f;          /* corridor/room: ride the midline (threshold 480 centres medium rooms too, not just tight corridors) */
        else if(dR>0&&dR<MINCLR)            sh=-(MINCLR-dR);          /* too close to +perp wall -> push away */
        else if(dL>0&&dL<MINCLR)            sh=+(MINCLR-dL);
        shv[i]=sh; pxv[i]=pxn; pyv[i]=pyn;               /* collect; apply AFTER smoothing the shift field */
    }
    /* SMOOTH THE CENTRING SHIFTS before applying: the wall-distance probe samples on a 4u grid, so
       raw per-node shifts carry +-2u noise -> visible MICROSTRAFES while walking. Three 3-tap passes
       over the shift field kill the noise but keep the genuine corridor-centring trend. */
    for(int pass=0;pass<3;pass++) for(int i=1;i<M-1;i++) shv[i]=0.5f*shv[i]+0.25f*(shv[i-1]+shv[i+1]);
    for(int i=0;i<M;i++){ if(shv[i]==0)continue;
        float nx=PX[i]+pxv[i]*shv[i], ny=PY[i]+pyv[i]*shv[i];
        int ix=(int)((nx-minx)/S),iy=(int)((ny-miny)/S);
        if(ix>=0&&iy>=0&&ix<NX&&iy<NY&&reach[iy*NX+ix]){ PX[i]=nx; PY[i]=ny; }   /* only if still navigable */
    }
    for(int pass=0;pass<0;pass++) for(int i=1;i<M-1;i++){   /* HARD JUMP TURNS (the author, after playing the squircle): NO corner rounding. A curve drifts the heading across 15-degree bin boundaries -- every node on it is a full 15-degree view cut (zero signature hits, full rebuilds, "each shot nothing like the previous"). Straight runs hold ONE bin (signature heaven, the glass-smooth tunnel); a sharp corner is ONE decisive cut, like a camera change. The discrete-view engine is piecewise-linear by nature -- the path must be too. */
        float nx=0.5f*PX[i]+0.25f*(PX[i-1]+PX[i+1]), ny=0.5f*PY[i]+0.25f*(PY[i-1]+PY[i+1]);
        int ix=(int)((nx-minx)/S),iy=(int)((ny-miny)/S);
        if(ix>=0&&iy>=0&&ix<NX&&iy<NY&&reach[iy*NX+ix]){ PX[i]=nx; PY[i]=ny; } }
    free(clr); free(cq); free(cell); return M;
}

int main(int argc,char**argv){
    const char *dump=NULL; int modes=0,spin=0,hasat=0,bench=0,bakeS=0,bakecR=0,bakecS=0,bakefloor=0,bakeceil=0,mapdump=0; float atx=0,aty=0; int ata=0;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--dump")&&i+1<argc) dump=argv[++i];
        else if(!strcmp(argv[i],"--modes")) modes=1; else if(!strcmp(argv[i],"--spin")) spin=1;
        else if(!strcmp(argv[i],"--bench")&&i+1<argc) bench=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--far")&&i+1<argc) PCFG.far=atof(argv[++i]);
        else if(!strcmp(argv[i],"--maxband")&&i+1<argc) PCFG.max_band=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--fov")&&i+1<argc) PCFG.fov=atof(argv[++i]);
        else if(!strcmp(argv[i],"--bake")&&i+1<argc) bakeS=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--bakec")&&i+1<argc) bakecR=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--bakeS")&&i+1<argc) bakecS=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--bakefloor")) bakefloor=1;
        else if(!strcmp(argv[i],"--bakeceil")) bakeceil=1;
        else if(!strcmp(argv[i],"--mapdump")) mapdump=1;
        else if(!strcmp(argv[i],"--at")&&i+3<argc){ atx=atof(argv[i+1]); aty=atof(argv[i+2]); ata=atoi(argv[i+3]); i+=3; hasat=1; } }
    if(dump) setenv("SDL_VIDEODRIVER","dummy",1);
    if(SDL_Init(SDL_INIT_VIDEO)!=0){ fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1; }
    tables_init();
    if(!load_crom("e1m1.crom")) fprintf(stderr,"warn: e1m1.crom not loaded; walls untextured\n");
    const level_t *lv=map_load(); camera_t cam=MAP_START;
    world_init(lv); open_all_doors(lv);   /* tour: bake & top-down map with all doors held open */
    if(hasat){ cam.pos.x=atx; cam.pos.y=aty; cam.ang=(angle_t)(ata*256/45); cam.sector=point_sector(lv,atx,aty); if(cam.sector>=0)cam.z=lv->sectors[cam.sector].floor+41.0f; }   /* --at X Y Aindex: aim a headless dump at any view (NODE_NA=45; sets eye height from the sector floor) */
    SDL_Surface *fb=SDL_CreateRGBSurfaceWithFormat(0,SCREEN_W,SCREEN_H,32,SDL_PIXELFORMAT_ARGB8888);
    DrawList dl;

    if(bench){                                  /* profile render_world in isolation */
        Uint64 c0=SDL_GetPerformanceCounter();
        for(int i=0;i<bench;i++) render_world(lv,&cam,&dl);
        Uint64 c1=SDL_GetPerformanceCounter();
        double ms=(double)(c1-c0)*1000.0/(double)SDL_GetPerformanceFrequency();
        printf("bench: %d frames %.1fms total %.5f ms/frame | ",bench,ms,ms/bench); report(&dl);
        SDL_Quit(); return 0;
    }

    if(mapdump){                                /* top-down debug map: geometry + navigable area + hero path */
        int BIG=1<<28, minx=BIG,miny=BIG,maxx=-BIG,maxy=-BIG;
        for(int i=0;i<lv->nverts;i++){ int vx=(int)lv->verts[i].x,vy=(int)lv->verts[i].y;
            if(vx<minx)minx=vx; if(vx>maxx)maxx=vx; if(vy<miny)miny=vy; if(vy>maxy)maxy=vy; }
        int MARG=28, spanx=maxx-minx, spany=maxy-miny, span=spanx>spany?spanx:spany; if(span<1)span=1;
        float msc=980.0f/span; int W=(int)(spanx*msc)+2*MARG, H=(int)(spany*msc)+2*MARG;
        SDL_Surface*m=SDL_CreateRGBSurfaceWithFormat(0,W,H,32,SDL_PIXELFORMAT_ARGB8888);
        SDL_FillRect(m,NULL,0xFF0C0C12);
        #define WX2I(wx) (MARG+(int)(((float)(wx)-minx)*msc))
        #define WY2I(wy) (MARG+(int)(((float)maxy-(wy))*msc))   /* flip Y so north is up */
        /* flood-fill the navigable area from spawn (identical logic to the bake) */
        int S=bakecS>0?bakecS:24, R=bakecR>0?bakecR:1300;
        int fminx=(int)MAP_START.pos.x-R, fminy=(int)MAP_START.pos.y-R, NX=2*R/S+1, NY=2*R/S+1, NC=NX*NY;
        unsigned char*reach=(unsigned char*)calloc(NC,1); int*qq=(int*)malloc((long)NC*4),qh=0,qt=0; int*par=(int*)malloc((long)NC*4);
        int spc=(R/S)*NX+(R/S); reach[spc]=1; par[spc]=-1; qq[qt++]=spc;
        int dx4[4]={1,-1,0,0},dy4[4]={0,0,1,-1};
        while(qh<qt){ int c=qq[qh++],ix=c%NX,iy=c/NX; float cx=fminx+ix*S+S*0.5f,cy=fminy+iy*S+S*0.5f;
            for(int d=0;d<4;d++){ int nix=ix+dx4[d],niy=iy+dy4[d]; if(nix<0||niy<0||nix>=NX||niy>=NY)continue;
                int nc=niy*NX+nix; if(reach[nc])continue; float ncx=fminx+nix*S+S*0.5f,ncy=fminy+niy*S+S*0.5f;
                camera_t t; t.pos.x=cx;t.pos.y=cy;t.ang=0;t.pitch=0;t.sector=point_sector(lv,cx,cy);t.z=0;
                move_player(lv,&t,ncx-cx,ncy-cy); float ex=t.pos.x-ncx,ey=t.pos.y-ncy; if(ex<0)ex=-ex;if(ey<0)ey=-ey;
                if(ex<S*0.2f&&ey<S*0.2f){reach[nc]=1;par[nc]=c;qq[qt++]=nc;} } }
        /* navigable cells: filled dark-green squares */
        int hw=(int)(S*msc*0.5f); if(hw<1)hw=1;
        for(int c=0;c<NC;c++) if(reach[c]){ int ix=c%NX,iy=c/NX; float cx=fminx+ix*S+S*0.5f,cy=fminy+iy*S+S*0.5f; mbox(m,WX2I(cx),WY2I(cy),hw,0xFF184A18); }
        /* linedefs: white=solid wall, orange=floor-height step (ledge/pit edge), dim=flat portal */
        for(int i=0;i<lv->nlines;i++){ linedef_t L=lv->lines[i]; vec2 a=lv->verts[L.v1], b=lv->verts[L.v2];
            int rs=L.right>=0?lv->sides[L.right].sector:-1, ls=L.left>=0?lv->sides[L.left].sector:-1; Uint32 col;
            if(rs<0||ls<0) col=0xFFE6E6E6;
            else { float df=lv->sectors[rs].floor-lv->sectors[ls].floor; if(df<0)df=-df; col=(df>1.0f)?0xFFFF8C28:0xFF50506E; }
            mline(m,WX2I(a.x),WY2I(a.y),WX2I(b.x),WY2I(b.y),col); }
        /* hero path = armour-room(left) -> spawn -> far(exit): a left-to-right traverse through spawn.
           CENTRE it (identical to the bake), then draw. */
        int chain[4096], cn=0;
        int leftc=-1,farc=-1; { float bl=1e18f,bf=1e18f;   /* path ends = nearest reachable cell to ARMOUR / EXIT (not leftmost/farthest) */
          for(int c=0;c<NC;c++) if(reach[c]){ int ix=c%NX,iy=c/NX; float cx=fminx+ix*S+S*0.5f,cy=fminy+iy*S+S*0.5f;
            float dl=(cx-ARMOUR_X)*(cx-ARMOUR_X)+(cy-ARMOUR_Y)*(cy-ARMOUR_Y); if(dl<bl){bl=dl;leftc=c;}
            float df=(cx-EXIT_X)*(cx-EXIT_X)+(cy-EXIT_Y)*(cy-EXIT_Y); if(df<bf){bf=df;farc=c;} } }
        static float PX[2048],PY[2048]; int PATH_N=0;
        cn=0; for(int c=leftc;c>=0&&cn<4096;c=par[c]) chain[cn++]=c;                                                 /* armour(left) -> spawn, forward */
        for(int k=0;k<cn&&PATH_N<2048;k++){ int c=chain[k],ix=c%NX,iy=c/NX; PX[PATH_N]=fminx+ix*S+S*0.5f; PY[PATH_N]=fminy+iy*S+S*0.5f; PATH_N++; }
        cn=0; for(int c=farc;c>=0&&cn<4096;c=par[c]) chain[cn++]=c;                                                  /* spawn -> far(exit), skip the dup spawn */
        for(int k=cn-2;k>=0&&PATH_N<2048;k--){ int c=chain[k],ix=c%NX,iy=c/NX; PX[PATH_N]=fminx+ix*S+S*0.5f; PY[PATH_N]=fminy+iy*S+S*0.5f; PATH_N++; }
        PATH_N=straighten_path(R,S,reach,PX,PY,PATH_N,400);
        int ppx=-1,ppy=-1;
        for(int i=0;i<PATH_N;i++){ int X=WX2I(PX[i]),Y=WY2I(PY[i]); if(ppx>=0){ mline(m,ppx,ppy,X,Y,0xFFFF3030); mline(m,ppx+1,ppy,X+1,Y,0xFFFF3030); } mdisc(m,X,Y,2,0xFFFFE000); ppx=X;ppy=Y; }
        /* spawn (cyan) + facing arrow (green); far end of the path (magenta) */
        int SX=WX2I(MAP_START.pos.x), SY=WY2I(MAP_START.pos.y);
        mdisc(m,SX,SY,5,0xFF00E0FF);
        int AX=SX+(int)(46*lut_cos(MAP_START.ang)), AY=SY-(int)(46*lut_sin(MAP_START.ang)); mline(m,SX,SY,AX,AY,0xFF20FF20); mdisc(m,AX,AY,2,0xFF20FF20);
        if(PATH_N>0) mdisc(m,WX2I(PX[PATH_N-1]),WY2I(PY[PATH_N-1]),5,0xFFFF00FF);   /* magenta: centred path end */
        SDL_SaveBMP(m,"/tmp/mapdump.bmp");
        int rc=0; for(int c=0;c<NC;c++) rc+=reach[c];
        printf("MAPDUMP: world x[%d..%d] y[%d..%d] -> %dx%d px; navigable %d cells; hero path %d nodes (armour->spawn->far) -> /tmp/mapdump.bmp\n",minx,maxx,miny,maxy,W,H,rc,PATH_N);
        #undef WX2I
        #undef WY2I
        SDL_Quit(); return 0;
    }

    if(bakeS){                                  /* node-render feasibility: flood walkable grid, measure */
        PCFG.far=1000.0f; PCFG.fov=160.0f; dng_flats=1;   /* match the v20 runtime renderer */
        int S=bakeS, NA=12, BIG=1<<28, minx=BIG,miny=BIG,maxx=-BIG,maxy=-BIG;
        for(int i=0;i<lv->nverts;i++){ int vx=(int)lv->verts[i].x,vy=(int)lv->verts[i].y;
            if(vx<minx)minx=vx; if(vx>maxx)maxx=vx; if(vy<miny)miny=vy; if(vy>maxy)maxy=vy; }
        int nx=(maxx-minx)/S+2, ny=(maxy-miny)/S+2, NC=nx*ny;
        unsigned char *reach=(unsigned char*)calloc(NC,1); int *q=(int*)malloc((long)NC*sizeof(int)),qh=0,qt=0;
        int six=((int)MAP_START.pos.x-minx)/S, siy=((int)MAP_START.pos.y-miny)/S;
        reach[siy*nx+six]=1; q[qt++]=siy*nx+six;
        int dx4[4]={1,-1,0,0},dy4[4]={0,0,1,-1};
        while(qh<qt){ int c=q[qh++],ix=c%nx,iy=c/nx; float cx=minx+ix*S+S*0.5f,cy=miny+iy*S+S*0.5f;
            for(int d=0;d<4;d++){ int nix=ix+dx4[d],niy=iy+dy4[d];
                if(nix<0||niy<0||nix>=nx||niy>=ny)continue; int nc=niy*nx+nix; if(reach[nc])continue;
                float ncx=minx+nix*S+S*0.5f,ncy=miny+niy*S+S*0.5f;
                camera_t t; t.pos.x=cx;t.pos.y=cy;t.ang=0;t.pitch=0;t.sector=point_sector(lv,cx,cy);t.z=0;
                move_player(lv,&t,ncx-cx,ncy-cy);
                float ex=t.pos.x-ncx,ey=t.pos.y-ncy; if(ex<0)ex=-ex; if(ey<0)ey=-ey;
                if(ex<S*0.2f&&ey<S*0.2f){ reach[nc]=1; q[qt++]=nc; } } }   /* tight: full moves only */
        int nodes=0; for(int i=0;i<NC;i++) if(reach[i]) nodes++;
        long ICOUNT=(long)nx*ny*NA; int *idx=(int*)malloc(ICOUNT*sizeof(int));
        for(long i=0;i<ICOUNT;i++) idx[i]=-1;
        unsigned char *data=(unsigned char*)malloc(16<<20); long dp=0,tw=0; int ne=0;
        for(int i=0;i<NC;i++){ if(!reach[i])continue; int ix=i%nx,iy=i/nx;
            float cx=minx+ix*S+S*0.5f,cy=miny+iy*S+S*0.5f; int sec=point_sector(lv,cx,cy);
            for(int a=0;a<NA;a++){ camera_t t; t.pos.x=cx;t.pos.y=cy;t.ang=(angle_t)(a*256/NA);
                t.pitch=0;t.sector=sec;t.z=lv->sectors[sec].floor+41.0f; render_world(lv,&t,&dl);
                long cpos=dp; dp+=2; int cnt=0;                      /* reserve u16 count */
                for(int j=0;j<dl.n && cnt<400;j++){ const SpriteCmd*c=&dl.cmd[j];
                    int wall=c->kind==SC_WALL, flat=(c->kind==SC_FLAT && c->w<200);
                    if((!wall&&!flat)||c->w<=0||c->h<=0) continue;
                    int sx=c->sx<0?0:(c->sx>511?511:c->sx), sy=c->sy<0?0:(c->sy>255?255:c->sy);
                    int w=c->w>255?255:c->w, h=c->h>255?255:c->h, tex=(c->tex<0||c->tex>=128)?0:c->tex;
                    data[dp++]=(unsigned char)((wall?0x80:0)|(tex&0x7f));
                    data[dp++]=(unsigned char)(sx&0xff); data[dp++]=(unsigned char)(sx>>8);
                    data[dp++]=(unsigned char)sy; data[dp++]=(unsigned char)w; data[dp++]=(unsigned char)h;
                    data[dp++]=(unsigned char)(wall?((c->uL>>4)&0xff):0); cnt++; }
                if(cnt){ data[cpos]=cnt&0xff; data[cpos+1]=(cnt>>8)&0xff; idx[(long)(iy*nx+ix)*NA+a]=(int)cpos; ne++; tw+=cnt; }
                else dp=cpos; } }
        FILE*f=fopen("neogeo/nodes.bin","wb"); int hdr[6]={minx,miny,S,nx,ny,NA};
        fwrite(hdr,4,6,f); fwrite(idx,sizeof(int),ICOUNT,f); fwrite(data,1,dp,f); fclose(f);
        long total=24+ICOUNT*4+dp;
        printf("BAKE S=%du NA=%d: %d nodes, %d non-empty views, ~%ld sprites/view\n",S,NA,nodes,ne,ne?tw/ne:0);
        printf("  nodes.bin = %.2f MB (data %.2f + index %.2f) -> fits 8MB HW P-ROM: %s\n",
               total/1e6, dp/1e6, (double)ICOUNT*4/1e6, total<=8L*1024*1024?"YES":"NO");
        free(idx); free(data);
        SDL_Quit(); return 0;
    }
    if(getenv("DBGPROBE")){ float px2,py2; if(sscanf(getenv("DBGPROBE"),"%f,%f",&px2,&py2)==2){
        int sc2=point_sector(lv,px2,py2);
        if(sc2>=0) fprintf(stderr,"PROBE (%.0f,%.0f): sector %d floor=%.0f ceil=%.0f light=%d floortex=%d ceiltex=%d\n",
            px2,py2,sc2,lv->sectors[sc2].floor,lv->sectors[sc2].ceil,lv->sectors[sc2].light,lv->sectors[sc2].floortex,lv->sectors[sc2].ceiltex);
        else fprintf(stderr,"PROBE (%.0f,%.0f): no sector\n",px2,py2); } }
    if(bakecR){                                 /* PoC: bake a spawn-cluster sub-grid as a C header */
        PCFG.far=getenv("NODE_FAR")?atof(getenv("NODE_FAR")):4000.0f; PCFG.fov=160.0f; dng_flats=1; PCFG.max_band=32;   /* draw distance for the baked views (NODE_FAR overrides). 4000 = full E1M1 extent (saturates: 4000==5000); measured cost is only 72 cmds/view, 66/96 sprites/scanline. Was 1000 -- the cart saw a fraction of the tuner. WIDE trapezoid segments: runtime interpolates the slope. */
        int S=bakecS>0?bakecS:24, NA=24;   /* EMPIRICAL DENSITY TRADE: 24 angles (15deg bins) buys ~2x node density at the same 8MB -- forward cadence halves, which is where the demo lives. LUTs rebaked at 24 to stay bin-coherent. */              /* ON-RAILS: 45 angles (8deg/turn; was 30) -- the perf rework + C-ROM reclaim paid for the finer angular snap (smoother turning, less view-pop). Matches the floor/ceiling LUTs 1:1 (ai->fai). P2 ~7MB = the 7-bank ceiling. */
        int R=bakecR>0?bakecR:1300;
        int minx=(int)MAP_START.pos.x-R, miny=(int)MAP_START.pos.y-R, NX=2*R/S+1, NY=2*R/S+1, NC=NX*NY;
        /* BFS-flood the navigable space from spawn, recording each cell's parent so we can back-trace a route. */
        unsigned char *reach=(unsigned char*)calloc(NC,1); int *qq=(int*)malloc((long)NC*4),qh=0,qt=0; int *par=(int*)malloc((long)NC*4);
        int sc=(R/S)*NX+(R/S); reach[sc]=1; par[sc]=-1; qq[qt++]=sc;
        int dx4[4]={1,-1,0,0},dy4[4]={0,0,1,-1};
        while(qh<qt){ int c=qq[qh++],ix=c%NX,iy=c/NX; float cx=minx+ix*S+S*0.5f,cy=miny+iy*S+S*0.5f;
            for(int d=0;d<4;d++){ int nix=ix+dx4[d],niy=iy+dy4[d]; if(nix<0||niy<0||nix>=NX||niy>=NY)continue;
                int nc=niy*NX+nix; if(reach[nc])continue; float ncx=minx+nix*S+S*0.5f,ncy=miny+niy*S+S*0.5f;
                camera_t t; t.pos.x=cx;t.pos.y=cy;t.ang=0;t.pitch=0;t.sector=point_sector(lv,cx,cy);t.z=0;
                move_player(lv,&t,ncx-cx,ncy-cy); float ex=t.pos.x-ncx,ey=t.pos.y-ncy; if(ex<0)ex=-ex;if(ey<0)ey=-ey;
                if(ex<S*0.2f&&ey<S*0.2f){
                    int nsec2=point_sector(lv,ncx,ncy);   /* the dolly never walks DAMAGE FLOORS: DOOM steps down into nukage happily, so the
                                                             zigzag-room pit read as navigable and the smoother legally bisected the bridge.
                                                             With nukage (floortex 48 = NUKAGE3) out of reach, the clearance field treats the
                                                             pit as wall and the centring shift-field hugs the BRIDGE CENTRE through the zigs. */
                    if(nsec2>=0 && lv->sectors[nsec2].floortex==48) continue;
                    reach[nc]=1;par[nc]=c;qq[qt++]=nc; } } }
        /* HERO PATH = armour -> spawn -> exit: back-trace the nearest-reachable cell to the green
           armour AND to the exit switch, joined at spawn (a left-to-right traverse through the start). */
        static float PX[2048],PY[2048]; int PATH_N=0;
        int leftc=-1,farc=-1; { float bl=1e18f,bf=1e18f;
          for(int c=0;c<NC;c++) if(reach[c]){ int ix=c%NX,iy=c/NX; float cx=minx+ix*S+S*0.5f,cy=miny+iy*S+S*0.5f;
            float dl=(cx-ARMOUR_X)*(cx-ARMOUR_X)+(cy-ARMOUR_Y)*(cy-ARMOUR_Y); if(dl<bl){bl=dl;leftc=c;}
            float df=(cx-EXIT_X)*(cx-EXIT_X)+(cy-EXIT_Y)*(cy-EXIT_Y); if(df<bf){bf=df;farc=c;} }
          printf("PATH targets: armour end (%d,%d) %.0fu from pickup; exit end (%d,%d) %.0fu from switch\n",
                 minx+(leftc%NX)*S+S/2, miny+(leftc/NX)*S+S/2, sqrtf(bl),
                 minx+(farc%NX)*S+S/2, miny+(farc/NX)*S+S/2, sqrtf(bf)); }
        /* (armour skipped: it's a 120u platform the flood-fill can't reach; the ARMOUR target just
            pins the left end at the stairs base, which is the desired left terminus) */
        { int chain[4096],cn=0;
          for(int c=leftc;c>=0&&cn<4096;c=par[c]) chain[cn++]=c;                                                    /* armour(left) -> spawn, forward */
          for(int k=0;k<cn&&PATH_N<2048;k++){ int c=chain[k],ix=c%NX,iy=c/NX; PX[PATH_N]=minx+ix*S+S*0.5f; PY[PATH_N]=miny+iy*S+S*0.5f; PATH_N++; }
          /* ZIGZAG-BRIDGE WAYPOINTS: the author wants the dolly to RIDE the iconic S of the nukage bridge,
             not bisect it. Derive the bridge centreline from data: per-row centroid of walkable
             cells inside the bridge bbox (the pits are nukage = !reach, so centroids trace the
             walkway), sampled every ~6 rows. The rail is then CHAINED spawn -> centreline -> exit
             with grid BFS, and the simplifier cannot re-straighten it (a chord across a zig spans
             !reach pit cells and fails line_clear). */
          int spawnc; { int six=(int)((MAP_START.pos.x-minx)/S), siy=(int)((MAP_START.pos.y-miny)/S); spawnc=siy*NX+six;
              if(spawnc<0||spawnc>=NC||!reach[spawnc]){ float bd2=1e18f; for(int c=0;c<NC;c++) if(reach[c]){ int ix=c%NX,iy=c/NX; float cx=minx+ix*S+S*0.5f,cy=miny+iy*S+S*0.5f,dx2=cx-MAP_START.pos.x,dy2=cy-MAP_START.pos.y,d2=dx2*dx2+dy2*dy2; if(d2<bd2){bd2=d2;spawnc=c;} } } }
          int wps[16]; int nwp=0;
          { int last_iy=-999; float prevx=2905.0f;            /* the bridge entry sits under the north corridor */
            for(int iy=NY-1;iy>=0&&nwp<14;iy--){ float cy=miny+iy*S+S*0.5f; if(cy>-3060.0f||cy<-3460.0f)continue;
                if(last_iy-iy<6 && last_iy!=-999)continue;     /* sample every ~6 rows (~60u) */
                /* a horizontal row cuts SEVERAL legs of the W: a plain centroid lands in the pit
                   between them. FOLLOW THE SNAKE instead: among this row's walkable runs, take the
                   one nearest the previous waypoint and use its centre. */
                int rs2=-1; float bestc=0; float bestd=1e18f; int inrun=0,run0=0;
                for(int ix=0;ix<=NX;ix++){
                    float cx=minx+ix*S+S*0.5f;
                    int ok=(ix<NX) && cx>=2720.0f && cx<=3060.0f && reach[iy*NX+ix];
                    if(ok && !inrun){ inrun=1; run0=ix; }
                    else if(!ok && inrun){ inrun=0;
                        float c2=minx+((run0+ix-1)/2.0f)*S+S*0.5f; float d2=c2-prevx; if(d2<0)d2=-d2;
                        if(d2<bestd){ bestd=d2; bestc=c2; rs2=run0; } } }
                if(rs2<0)continue;
                int wix=(int)((bestc-minx)/S); wps[nwp++]=iy*NX+wix; prevx=bestc; last_iy=iy;
                fprintf(stderr,"ZIGZAG row y=%.0f -> wp x=%.0f\n",cy,bestc); }
            fprintf(stderr,"ZIGZAG: %d centreline waypoints\n",nwp); }
          { static int seg[4096]; int prev=spawnc;
            for(int w=0;w<=nwp;w++){ int tgt=(w<nwp)?wps[w]:farc;
                int sn=grid_route(prev,tgt,NX,NC,reach,seg,4096);
                for(int k=(PATH_N>0)?1:0;k<sn&&PATH_N<2048;k++){ int c=seg[k],ix=c%NX,iy=c/NX; PX[PATH_N]=minx+ix*S+S*0.5f; PY[PATH_N]=miny+iy*S+S*0.5f; PATH_N++; }
                prev=tgt; } } }
        PATH_N=straighten_path(R,S,reach,PX,PY,PATH_N,1300);   /* ride corridor centres. CAP MUST EXCEED the full armour->spawn->exit resample count or the path SILENTLY TRUNCATES BEFORE THE EXIT (at S=10 the full path is ~528 nodes; the old cap of 512 cut the last ~470u -- the exit elevator was off the rail). */
        int START_NODE=0;   /* #12: set below to the biggest-sky view (the courtyard-window opening) -- the player spawn is the enclosed first room, NOT a courtyard overlook. W/S walk the rail from there. */
        { FILE *pf=fopen("path_nodes.txt","w");               /* ground-truth ref list: per-node x, y, forward-tangent deg (ZDoom: 0=east, CCW) for refcap.py */
          if(pf){ for(int i=0;i<PATH_N;i++){ float dx,dy;
              if(i+1<PATH_N){dx=PX[i+1]-PX[i];dy=PY[i+1]-PY[i];} else {dx=PX[i]-PX[i-1];dy=PY[i]-PY[i-1];}
              float ad=atan2f(dy,dx)*57.2957795f; if(ad<0)ad+=360.0f;
              fprintf(pf,"%d %d %d %d\n",i,(int)PX[i],(int)PY[i],(int)(ad+0.5f)); }
            fclose(pf); printf("wrote path_nodes.txt (%d nodes: i x y fwd-deg)\n",PATH_N); } }
        long ICOUNT=(long)PATH_N*NA; int *idx=(int*)malloc(ICOUNT*4); for(long i=0;i<ICOUNT;i++)idx[i]=-1;
        unsigned char *data=(unsigned char*)malloc(64<<20); long dp=0;  /* generous host buffer; cart capped at 7 P2 banks */
        long slp_tot=0, slp_nz=0, slp_clamp=0, slp_clamp2=0; int slp_max=0;  /* DEBUG: trapezoid slope + du-cap distribution */
        enum { RDMAX=32, RDN=2*RDMAX+1 };   /* TUNED ramp drop cap +/-32 -> <=2-tile stacks (no tall jaggy 3-4-tile caps) */
        static int rampneed[128][RDN][2]; for(int a=0;a<128;a++)for(int b=0;b<RDN;b++){rampneed[a][b][0]=0;rampneed[a][b][1]=0;}  /* distinct (tex, per-chunk drop+RDMAX, edge) ramp combos; drop in [-32,32] -> up to 2-tile stacks */
        /* PERF PROFILE: per-view command count + worst per-scanline sprite load. The cart parses `cmds`
           records/frame (68k CPU cost) and the Neo Geo drops sprites + stalls past ~96/scanline. */
        int prof_maxcnt=0,prof_mc_p=0,prof_mc_a=0, prof_maxwl=0,prof_mw_p=0,prof_mw_a=0, prof_caphits=0,prof_busyn=0;
        long sky_recs=0; int sky_views=0, sky_maxh=0;   /* DEBUG: sky-record emission stats */
        long sky_bestarea=0; int sky_bp=0, sky_ba=0;    /* view with the most sky pixels (for A/B) */
        /* ACCURATE SPRITE ACCOUNTING (the cart's real `spr`): LUT(~40) + every record's ceil(w/16) strips. */
        long spr_sum=0; int spr_max=0,spr_mp=0,spr_ma=0, scanmax=0,scan_mp=0,scan_ma=0, scan_over96=0,spr_over160=0; long scan_sum=0; int sprhist[9]={0,0,0,0,0,0,0,0,0}; int hotn=0;
        for(int p=0;p<PATH_N;p++){ float cx=PX[p],cy=PY[p]; int sec=point_sector(lv,cx,cy);
            for(int a=0;a<NA;a++){ camera_t t; t.pos.x=cx;t.pos.y=cy;t.ang=(angle_t)(a*256/NA);t.pitch=0;t.sector=sec;t.z=lv->sectors[sec].floor+41.0f;
                render_world(lv,&t,&dl);
                if(getenv("CODECSTATS")){
                    static FILE*cf=NULL; if(!cf)cf=fopen("/tmp/codecstats.txt","w");
                    int fwd=(a==((pathang_quick(PX,PY,PATH_N,p)*24+128)>>8)%24);
                    if(fwd || (p%50)==0){
                        for(int ci=0;ci<dl.n;ci++){ const SpriteCmd*c=&dl.cmd[ci];
                            if(c->kind!=SC_WALL||c->srcid==0) continue;
                            fprintf(cf,"%d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                                p,a,c->srcid,c->sx,c->sy,c->w,c->h,c->uL,c->uR,c->v0,c->v1,c->depth,c->dtop,c->dbot); } }
                }
                /* MERGE BSP-fragmented floor/ceiling flats (#13a): the renderer chops each plane into many
                   per-seg-band rects, so a wide floor became dozens of strips incl. isolated single-column
                   slivers. Combine horizontally-adjacent rects of the SAME texture + height + screen-extent
                   into one wide rect (the per-band recede rows are recomputed for the merged rect, so shape
                   is preserved). 3 passes catches chains. Far fewer strips; no lone slivers. */
                for(int pass=0;pass<3;pass++) for(int i=0;i<dl.n;i++){ SpriteCmd*A=&dl.cmd[i];
                    if(A->kind!=SC_FLAT||A->tex<0||A->w<=0)continue;
                    for(int j=0;j<dl.n;j++){ if(j==i)continue; SpriteCmd*B=&dl.cmd[j];
                        if(B->kind!=SC_FLAT||B->w<=0||B->tex!=A->tex||B->v1!=A->v1)continue;
                        int dvz=B->v0-A->v0; if(dvz<0)dvz=-dvz; if(dvz>8)continue;            /* same world height */
                        int dsy=B->sy-A->sy; if(dsy<0)dsy=-dsy; int dby=(B->sy+B->h)-(A->sy+A->h); if(dby<0)dby=-dby;
                        if(dsy>6||dby>6)continue;                                              /* same screen extent (small slope) */
                        int aend=A->sx+A->w, bend=B->sx+B->w;
                        if(B->sx<=aend+4 && bend>aend && B->sx>=A->sx){                        /* B abuts/overlaps A on the right -> grow A */
                            A->w=(int16_t)(bend-A->sx);
                            if(B->sy<A->sy)A->sy=B->sy;
                            { int nb=B->sy+B->h; if(nb>A->sy+A->h)A->h=(int16_t)(nb-A->sy); }
                            B->w=0; } } }
                /* LIGHT-FIXTURE merge (the red-dot ceiling boxes): DOOM fakes the angular overhead fixture
                   with STEPPED sectors, and the BSP further slices each step into small strips -- a dozen
                   tiny tex-52 ceiling fragments at staggered sy/h/v0 that the general pass above (same
                   screen extent, same height) can never recombine. Drawn separately they're each too short
                   to foreshorten (h<48) and their dot rows land scattered = the "2D red dots". For
                   FULL-BRIGHT ceilings of the same texture, BOX-UNION any touching fragments (allowing the
                   step height difference); the emit then recomputes the 8 perspective V-bands + the 2-zone
                   foreshortening over the WHOLE box, so the dots form converging rows on one dark slab --
                   matching the real-DOOM frame (refcap). Capped so a runaway union can't exceed the record's
                   byte fields. */
                for(int pass=0;pass<3 && !getenv("NOUNION");pass++) for(int i=0;i<dl.n;i++){ SpriteCmd*A=&dl.cmd[i];
                    if(A->kind!=SC_FLAT||A->tex<0||A->w<=0||A->v1==0)continue;
                    for(int j=0;j<dl.n;j++){ if(j==i)continue; SpriteCmd*B=&dl.cmd[j];
                        if(B->kind!=SC_FLAT||B->w<=0||B->tex!=A->tex||B->v1==0)continue;
                        if(A->light<=200||B->light<=200)continue;   /* BRIGHT light-fixtures only: unioning ordinary ceilings made giant featureless slabs (worse than the steps) */
                        int dvz=B->v0-A->v0; if(dvz<0)dvz=-dvz; if(dvz>32)continue;
                        if(B->sx>A->sx+A->w+6 || B->sx+B->w<A->sx-6) continue;                 /* must touch in x (6px gap tolerance) */
                        if(B->sy>A->sy+A->h+6 || B->sy+B->h<A->sy-6) continue;                 /* and in y */
                        int nsx=A->sx<B->sx?A->sx:B->sx, nex=(A->sx+A->w)>(B->sx+B->w)?(A->sx+A->w):(B->sx+B->w);
                        int nsy=A->sy<B->sy?A->sy:B->sy, ney=(A->sy+A->h)>(B->sy+B->h)?(A->sy+A->h):(B->sy+B->h);
                        if(nex-nsx>250||ney-nsy>200)continue;                                  /* record w/h are bytes; don't union past them */
                        A->sx=(int16_t)nsx; A->w=(int16_t)(nex-nsx); A->sy=(int16_t)nsy; A->h=(int16_t)(ney-nsy);
                        B->w=0; } }
                /* bank-align: a draw-list must never straddle a 1MB P2-bank seam (the runtime maps
                   one 1MB bank into the 0x200000 window at a time). Pad to the next bank if needed. */
                if(((unsigned long)dp & 0xFFFFF) + 4096UL > 0x100000UL){ long pad=0x100000-(dp&0xFFFFF); while(pad-->0) data[dp++]=0; }
                long cpos=dp; dp+=2; int cnt=0;
                /* 3-byte SKY-COLUMN MASK (after cnt, before records): bit c set => screen column c (16px)
                   shows an F_SKY1 ceiling. The host already emits sky as SC_FLAT with tex==-2; collapse those
                   spans to a 20-col mask so the cart paints the SKY1 column there instead of the gray ceiling. */
                long mpos=dp; dp+=3;
                /* SKY-FROM-TOP mask (repurposes the old whole-column sky mask): a column is flagged only if
                   a sky flat covers it from the top (sy<=8) down past most of the ceiling band (sy+h>=88).
                   The cart skips the LUT ceiling on those columns -- the sky records paint them, so the LUT
                   ceiling there was ~20 wasted sprites/exterior. Mixed columns (ceiling above a low sky band)
                   stay unflagged so their LUT ceiling still draws. */
                /* BRIGHT-FIXTURE CEILING predicate, shared by the skytop skip / keep filter / priority
                   pass so they can never disagree. Two hard lessons baked in:
                   - LAMP FLATS ONLY (TLITE6_x, tex 50-53): ANY bright ceiling used to qualify, so a lit
                     grey ceiling near the exit emitted a banded grey slab that punched a hole in the
                     red-lamp band of the END-OF-RAIL parked view (truncated lamps mid-pixel on the tile
                     grid). Grey lit ceilings are what the LUT already approximates -- only the lamps
                     are worth a record.
                   - h>=6: a fixture record collapsing to a 2px strip draws a crushed 28x1 "light" with
                     a hard seam (node ~492). A sliver lamp is worse than no lamp. */
                #define BRIGHT_CEIL_REC(c) (0)   /* lamp RECORDS stay retired. Forensic note: the red dots the author liked were NOT these -- his 16:10 screenshot predated any restoration and showed LUT C, whose "white" spot slots are naturally red-orange (the TLITE6_5 lamp pixels). The per-pixel LUT dots are the keeper; the banded record version was what read as glitchy. */
                { unsigned skytop=0; int colminsy[20]; for(int cc=0;cc<20;cc++) colminsy[cc]=999;   /* per-column TOPMOST sky y (union over fragmented sky flats) */
                  for(int j=0;j<dl.n;j++){ const SpriteCmd*c=&dl.cmd[j];
                    int issky=(c->kind==SC_FLAT && c->tex==-2 && c->w>0);
                    int isbrightceil=(c->kind==SC_FLAT && c->tex>=0 && BRIGHT_CEIL_REC(c) && c->w>0 && c->depth<1400);  /* full-bright light-fixture ceiling (red lights), kept by the priority pass -> its flat paints the column, so skip the grey LUT here too (embeds the domes + saves the LUT sprites) */
                    if(issky || isbrightceil){ int c0=c->sx>>4, c1=(c->sx+c->w-1)>>4; if(c0<0)c0=0; if(c1>19)c1=19;
                      for(int cc=c0;cc<=c1;cc++) if(c->sy<colminsy[cc]) colminsy[cc]=c->sy; } }
                  for(int cc=0;cc<20;cc++) if(colminsy[cc]<=8) skytop|=(1u<<cc);   /* sky OR a bright ceiling reaches the top -> skip the (wrong+wasted) grey LUT ceiling; the sky/red-flat records paint it instead */
                  data[mpos]=skytop&0xff; data[mpos+1]=(skytop>>8)&0xff; data[mpos+2]=(skytop>>16)&0xff; }
                /* SKY-THROUGH-OPENING records (tt=0x7F marker, tex 0x7F is free since NTEXTILE=76). Emitted
                   FIRST so they sit behind the walls; the cart paints the SKY1 panorama (scrolled by view
                   angle, 1:1 vertical) at each opening's precise (sx,sy,w,h) -- the proper vertical extent
                   the old whole-column mask lacked. */
                { int vsky=0; long varea=0;
                for(int j=0;j<dl.n && cnt<400;j++){ const SpriteCmd*c=&dl.cmd[j];
                    if(c->kind!=SC_FLAT || c->tex!=-2 || c->w<=0 || c->h<=0) continue;
                    int psx=c->sx<0?0:(c->sx>511?511:c->sx), pw=c->w>255?255:c->w;
                    int psy=c->sy<0?0:(c->sy>255?255:c->sy), ph=c->h>255?255:c->h;
                    data[dp++]=0x7F; data[dp++]=(unsigned char)(psx&0xff); data[dp++]=(unsigned char)((psx>>8)&0xff);
                    data[dp++]=(unsigned char)psy; data[dp++]=(unsigned char)pw; data[dp++]=(unsigned char)ph;
                    data[dp++]=0; data[dp++]=0; data[dp++]=0; data[dp++]=0; data[dp++]=0; data[dp++]=0; cnt++;   /* 12 bytes (v2): tt=0x7F + sx,sy,w,h + 5 pad + srcid(0) */
                    sky_recs++; vsky++; varea+=(long)pw*ph; if(ph>sky_maxh)sky_maxh=ph; }
                if(vsky)sky_views++; if(varea>sky_bestarea){sky_bestarea=varea;sky_bp=p;sky_ba=a;} }
                static int prof_scan[256]; for(int y=0;y<256;y++)prof_scan[y]=0;   /* per-view: sprites covering each scanline */
                /* depth-sort wall cmds FAR->NEAR so the near wall gets the higher sprite index (drawn on top):
                   fixes "rear pops over near" where a band's centre-column occlusion test leaks at its edges. */
                static int ord[1024]; int on=0;
                /* MULTI-HEIGHT, PER-COLUMN UNCAP: ALL walls + ALL non-dominant FLOOR/CEILING height-spans.
                   Each flat rect is now ONE record drawn by the cart as ceil(w/16) VERTICAL sprite strips
                   (a strip = a vertical tile stack), not K_depthbands x ceil(w/16) HORIZONTAL chunks -- ~K x
                   fewer sprites, so the old FLAT_SPR_BUDGET cap (which dropped pits + all ceilings, causing
                   the "pop") is gone. The cam's own floor/ceiling stays the cheap perspective LUT; only the
                   deviations (pits, raised steps, other-height ceilings seen through openings) are records. */
                /* WALLS nearest-first, capped by STRIP count (ADAPTIVE far-wall cap). Most views are well
                   under the budget = ALL walls kept (full long draw distance). Only the ~10 densest views
                   (node 131 etc. = 120 wall strips, crawled) get trimmed, and the FARTHEST wall-chunks drop
                   first (deep background; near walls always kept). Tunable. NOT the blanket dep-ramp cull. */
                int WALL_STRIP_BUDGET = getenv("MAXQ")?9999:88;   /* MAXQ=1: REFERENCE BAKE -- every compromise off (the author: "maximum possible quality build, then we'll gracefully degrade it the right way") */
                #define WALL_STRIP_BUDGET_DOC 88    /* commit-fits-vblank budget: WALL+FLAT+THING <= 144 keeps the far rewrite ~1 vblank, so dense-room commits stop costing 7 refreshes (the 8fps holes). the author: clean > dense, twice. */   /* raised from 80: the view-cache + de-divisioned emit + imp-scan fix transformed the 68k budget; the old cap was cutting the upper-wall bands in dense rooms (the "missing coverage" glitches, e.g. node 28). Scanline peak verified < 96 after the raise. */
                { static int wford[1024]; int wn=0;
                  for(int j=0;j<dl.n&&wn<1024;j++){ const SpriteCmd*c=&dl.cmd[j]; if(c->kind==SC_WALL&&c->w>0&&c->h>0) wford[wn++]=j; }
                  for(int a2=1;a2<wn;a2++){ int v=wford[a2],dv=dl.cmd[v].depth,b2=a2-1; while(b2>=0&&dl.cmd[wford[b2]].depth>dv){wford[b2+1]=wford[b2];b2--;} wford[b2+1]=v; }  /* NEAREST first */
                  int wb=0; for(int i=0;i<wn&&on<1024;i++){ const SpriteCmd*c=&dl.cmd[wford[i]]; int st=(c->w+15)/16; if(st<1)st=1;
                      if(wb+st>WALL_STRIP_BUDGET)break; wb+=st; ord[on++]=wford[i]; } }
                /* flats nearest-first, capped by STRIP count. The vertical-strip rewrite made flats ~Kx
                   cheaper so the cap is GENEROUS (vs the old 24): only the extreme multi-height views (armour
                   area hit 136 strips -> 235 spr) get trimmed, and the farthest deviations drop first (least
                   visible, fall back to the LUT). Normal views stay under the cap = no pop. */
                int FLAT_STRIP_BUDGET = getenv("MAXQ")?9999:64;
                #define FLAT_STRIP_BUDGET_DOC 64    /* see WALL budget note (72+56+16=144) */
                { static int ford[1024]; int fn=0;
                  for(int j=0;j<dl.n&&fn<1024;j++){ const SpriteCmd*c=&dl.cmd[j];
                      if(c->kind!=SC_FLAT||c->w<=0||c->h<=0||c->tex<0||c->tex>=NTEXh||TEX[c->tex].w<=0)continue;
                      float rz=t.z-(float)c->v0, camceil=lv->sectors[sec].ceil, dceil=(float)c->v0-camceil; if(dceil<0)dceil=-dceil;
                      int keep = c->v1==0 ? (c->depth<1400 && ((rz>4.0f&&rz<33.0f)||rz>49.0f) && c->sy>=80)
                                          : (c->depth<1400 && BRIGHT_CEIL_REC(c));   /* FLOORS: at/below the horizon only (the old above-horizon "tree trunk" cull). CEILINGS: BRIGHT FIXTURES ONLY (the red lights) -- dark other-height ceiling bands always read as misaligned "crowbarred glitch textures"; the smooth ceiling LUT covering them is approximately correct, which beats accurately wrong. Also kills the ceiling-fragment pop = a big slice of the walk flicker. */
                      if(keep) ford[fn++]=j; }
                  for(int a2=1;a2<fn;a2++){ int v=ford[a2],dv=dl.cmd[v].depth,b2=a2-1; while(b2>=0&&dl.cmd[ford[b2]].depth>dv){ford[b2+1]=ford[b2];b2--;} ford[b2+1]=v; }  /* NEAREST first */
                  int fb=0;
                  /* PRIORITY: full-bright light-fixture ceilings (the red ceiling lights) kept FIRST so the
                     STRIP budget never cuts them. They sit at the screen TOP (far depth), so nearest-first
                     sorts them LAST and drops them in dense rooms -> the grey LUT shows through = the
                     "floating domes". Kept here, the cart (with the skytop-skip below) paints the real dark
                     band + domes instead of grey. */
                  for(int i=0;i<fn&&on<1024;i++){ const SpriteCmd*c=&dl.cmd[ford[i]];
                      if(!BRIGHT_CEIL_REC(c))continue; int st=(c->w+15)/16; if(st<1)st=1;
                      if(fb+st>FLAT_STRIP_BUDGET)break; fb+=st; ord[on++]=ford[i]; ford[i]=-1; }
                  for(int i=0;i<fn&&on<1024;i++){ if(ford[i]<0)continue; const SpriteCmd*c=&dl.cmd[ford[i]]; int st=(c->w+15)/16; if(st<1)st=1;
                      if(fb+st>FLAT_STRIP_BUDGET)break; fb+=st; ord[on++]=ford[i]; } }
                /* THINGS: decorations / lights / pickups as billboards (the red torches, techlamps, columns,
                   candles, barrels, armour...). render_things already projected + occlusion-culled them against
                   the wall depth buffer, so the screen rect is exact. MONSTERS are skipped -- they'd need
                   8-rotation frames and the imp is its own runtime billboard. Cap to the nearest THING_BUDGET
                   so a lamp-filled room can't blow the sprite ceiling. They join ord[] -> the unified depth
                   sort below gives correct thing<->wall occlusion (a nearer wall draws OVER a farther prop). */
                #define THING_BUDGET 18   /* cart cap (72+56+16=144); was 8: in cluttered views (the barrel room = 6 barrels + bonus clusters) the nearest-8 set CHANGED between adjacent views -> things flickered in/out while walking. 20 covers every E1M1 view; ~2-3 strips per thing, still inside the record band. */
                { static int tord[256]; int tn=0;
                  for(int j=0;j<dl.n&&tn<256&&!getenv("NOTHINGS");j++){ const SpriteCmd*c=&dl.cmd[j];
                      if(c->kind!=SC_THING||c->w<=0||c->h<=0||c->tex<0||c->tex>=NTEXh||TEX[c->tex].w<=0)continue;
                      if(c->sx+c->w<=0||c->sx>=SCREEN_W||c->sy+c->h<=0||c->sy>=SCREEN_H)continue;  /* entirely off-screen: don't waste a record (render_things' centre test clamps x, so off-screen props survive it) */
                      int ty=c->dtop;   /* DOOM thing type stashed in dtop by render_things */
                      if(ty==3004||ty==9||ty==65||ty==3001||ty==3002||ty==3005||ty==3006||ty==3003)continue;  /* monsters: skip */
                      if(ty==2035)continue;  /* barrels: RUNTIME actors now (shootable/explodable) -- baking them would leave unkillable ghosts */
                      tord[tn++]=j; }
                  for(int a2=1;a2<tn;a2++){ int v=tord[a2],dv=dl.cmd[v].depth,b2=a2-1; while(b2>=0&&dl.cmd[tord[b2]].depth>dv){tord[b2+1]=tord[b2];b2--;} tord[b2+1]=v; }  /* NEAREST first */
                  for(int i=0;i<tn&&i<THING_BUDGET&&on<1024;i++) ord[on++]=tord[i]; }
                for(int a2=1;a2<on;a2++){ int v=ord[a2],dv=dl.cmd[v].depth,b2=a2-1; while(b2>=0&&dl.cmd[ord[b2]].depth<dv){ord[b2+1]=ord[b2];b2--;} ord[b2+1]=v; }
                for(int oi=0;oi<on&&cnt<400;oi++){ int j=ord[oi]; const SpriteCmd*c=&dl.cmd[j];
                    int wall=c->kind==SC_WALL;
                    if(c->w<=0||c->h<=0)continue;         /* walls + pit-floor flats; dominant floor/ceiling = LUT */
                    if(c->kind==SC_THING){               /* billboard prop/light: byte0=0x7E, byte6=sprite tex, byte7=fog level. sx SIGNED. */
                        int tsx=c->sx; if(tsx<-256)tsx=-256; if(tsx>511)tsx=511;
                        int tsy=c->sy<0?0:(c->sy>255?255:c->sy), th2=c->h>255?255:c->h, tw=c->w>255?255:c->w;
                        int ttex=(c->tex<0||c->tex>=NTEXh)?0:c->tex;
                        int tpl=(c->depth<=224)?0:((c->depth<=540)?1:2);              /* same fog ramp as the walls (FOGD0/FOGD1 in world units) */
                        data[dp++]=0x7E; data[dp++]=(unsigned char)(tsx&0xff); data[dp++]=(unsigned char)((tsx>>8)&0xff);
                        data[dp++]=(unsigned char)tsy; data[dp++]=(unsigned char)tw; data[dp++]=(unsigned char)th2;
                        data[dp++]=(unsigned char)(ttex&0x7f); data[dp++]=(unsigned char)(tpl&3);
                        data[dp++]=0; data[dp++]=0; data[dp++]=0; data[dp++]=0; cnt++;   /* pad to the 12-byte record (srcid 0) */
                        { int chk=(TEX[ttex].w+15)/16; if(chk<1)chk=1; for(int y=tsy;y<tsy+th2&&y<256;y++)prof_scan[y]+=chk; }
                        continue;
                    }
                    int tex=(c->tex<0||c->tex>=128)?-1:c->tex;
                    int psx=c->sx<0?0:(c->sx>511?511:c->sx),pw=c->w>255?255:c->w;
                    if(wall){                                    /* wall: byte0=0x80|tex, byte6=texel column */
                        int psy=c->sy<0?0:(c->sy>255?255:c->sy),ph=c->h>255?255:c->h,pt=tex<0?0:tex;
                        int pd=c->depth>>2; if(pd<0)pd=0; if(pd>255)pd=255;     /* depth/4 -> 1 byte (Street-View transform) */
                        if(!getenv("MAXQ")){ int f=256-(c->depth*154)/1800; if(f<64)f=64; if(f>256)f=256;   /* invert shade(): recover the sector's raw DOOM light from the record's lum (MAXQ: no fog/light manipulation at all) */
                          int sl2=(int)c->light*256/f;
                          if(sl2>=192){ if(pd>56&&pd<=135) pd=56; }       /* bright sector (192..255): hold full colour (L0) to the murk line. Thresholds mirror FOGD0/FOGD1 in neogeo/main.c -- keep in sync. */
                          else if(sl2<=144){ if(pd>32&&pd<=56) pd=57; } } /* dim sector (128/144): L1 floor beyond arm's reach -- DOOM's per-sector mood with NO record-format change (a wall record's dep byte has exactly one live reader: the fog-level pick) */
                        data[dp++]=(unsigned char)(0x80|(pt&0x7f)); data[dp++]=(unsigned char)(psx&0xff); data[dp++]=(unsigned char)(psx>>8);
                        data[dp++]=(unsigned char)psy; data[dp++]=(unsigned char)pw; data[dp++]=(unsigned char)ph;
                        int du=(c->w>0)?((c->uR-c->uL)*16/c->w):16; if(du<-16||du>16)slp_clamp2++;
                        { int dcap=getenv("MAXQ")?127:32; if(du<-dcap)du=-dcap; if(du>dcap)du=dcap; }   /* MAXQ: uncap oblique texture compression (the 32 cap leaves 71%% of strips stretched) */  /* texels/chunk: cap raised 16->32 so oblique side-walls compress instead of stretching (cost: some aliasing past 2x) */
                        int twp=(tex>=0&&tex<NTEXh&&TEX[tex].w>0)?(((TEX[tex].w+15)/16)*16):16;  /* padded tex width (texels) */
                        data[dp++]=(unsigned char)((((c->uL%twp)+twp)%twp)&0xff);  /* colb in TEXELS now (was tiles) -> runtime picks sub-tile phase */
                        data[dp++]=(unsigned char)pd; data[dp++]=(unsigned char)(du&0xff);
                        int pdt=c->dtop,pdb=c->dbot; if(pdt<-127)pdt=-127; if(pdt>127)pdt=127; if(pdb<-127)pdb=-127; if(pdb>127)pdb=127;
                        data[dp++]=(unsigned char)(pdt&0xff); data[dp++]=(unsigned char)(pdb&0xff);
                        data[dp++]=(unsigned char)((c->srcid%251)+1); cnt++;   /* byte 11 (v2): srcid HASH 1..255 -- the tween matcher's identity key (collisions gated by param sanity at match time) */
                        { int chk=(pw+15)/16; for(int y=psy;y<psy+ph&&y<256;y++)prof_scan[y]+=chk; }       /* this wall adds ceil(w/16) sprites to rows [sy,sy+h) */
                        slp_tot++; { int am=pdt<0?-pdt:pdt, bm=pdb<0?-pdb:pdb; if(am>2||bm>2)slp_nz++; if(am>slp_max)slp_max=am; if(bm>slp_max)slp_max=bm; }
                        if(tex>=0&&tex<128&&c->w>0){ int dtq=c->dtop*16/c->w, dbq=c->dbot*16/c->w;   /* per-chunk drop (px across one 16px chunk) */
                            if(dtq<-RDMAX||dtq>RDMAX||dbq<-RDMAX||dbq>RDMAX)slp_clamp++;              /* residual: steeper than a 2-tile stack (near edge-on, thin) -> clamped, small caps */
                            if(dtq<-RDMAX)dtq=-RDMAX; if(dtq>RDMAX)dtq=RDMAX; if(dbq<-RDMAX)dbq=-RDMAX; if(dbq>RDMAX)dbq=RDMAX;
                            { int a=dtq<0?-dtq:dtq; if(a>16){a=((a+2)>>2)<<2; dtq=dtq<0?-a:a;} }       /* steep (>16) -> quantize to step 4: smoother than step-8, still bounds the tile budget */
                            { int a=dbq<0?-dbq:dbq; if(a>16){a=((a+2)>>2)<<2; dbq=dbq<0?-a:a;} }
                            rampneed[tex][dtq+RDMAX][0]=1; rampneed[tex][dbq+RDMAX][1]=1; }
                    } else if(tex<0||tex>=NTEXh||TEX[tex].w<=0){  /* untextured flat: solid (byte6=0x80) */
                        int psy=c->sy<0?0:(c->sy>255?255:c->sy),ph=c->h>255?255:c->h;
                        data[dp++]=(unsigned char)((tex<0?0:tex)&0x7f); data[dp++]=(unsigned char)(psx&0xff); data[dp++]=(unsigned char)(psx>>8);
                        data[dp++]=(unsigned char)psy; data[dp++]=(unsigned char)pw; data[dp++]=(unsigned char)ph; data[dp++]=0x80; cnt++;
                    } else {                                     /* textured FLOOR/CEILING rect -> ONE 11-byte record carrying the PER-BAND depth rows
                                                                    (8 V-nibbles in bytes 7-10) so the cart's vertical strip RECEDES (each 16px tile = its own
                                                                    depth slice) instead of tiling one row flat. byte6 = flags (bit7=0 so it's not mistaken for
                                                                    the 0x80 untextured marker): isceil<<6 | fog-level. */
                        float si=lut_sin(t.ang), relz=t.z-(float)c->v0, horizon=112.0f; int isceil=(c->v1!=0);
                        int Hpx=TEX[tex].h; if(Hpx<16)Hpx=16; int rows=Hpx/16; if(rows<1)rows=1;
                        int K=(c->h+15)/16; if(K<1)K=1; if(K>8)K=8;
                        int vp[8]; float middep=0;
                        for(int b=0;b<8;b++){ int bb=b<K?b:K-1;                       /* per band: depth-correct texture row (recede); pad >K with the far band */
                            float my=c->sy+bb*16+8, dy=my-horizon;
                            if(isceil){ if(dy>-0.5f)dy=-0.5f; } else { if(dy<0.5f)dy=0.5f; }
                            float D=relz*PCFG.fov/dy; if(D<0)D=-D;
                            float fwy=t.pos.y+D*si;
                            int Vrow=((((int)fwy)%Hpx)+Hpx)%Hpx/16; if(Vrow>=rows)Vrow=rows-1; if(Vrow<0)Vrow=0;
                            vp[b]=Vrow&0xf; if(bb==K/2) middep=D; }
                        int plvl=(middep<=224.0f)?0:((middep<=480.0f)?1:2);
                        if(c->light<150) plvl=2; else if(c->light<195 && plvl<1) plvl=1;   /* SHADE BY SECTOR LIGHT too: a raised floor/ceiling in a DIM room was drawn full-bright (plvl from depth only) -> it popped as a flat 2D panel vs the dark room. Darken it to match. */
                        int psy=c->sy<0?0:(c->sy>255?255:c->sy),psh=c->h>255?255:c->h;
                        data[dp++]=(unsigned char)(tex&0x7f); data[dp++]=(unsigned char)(psx&0xff); data[dp++]=(unsigned char)(psx>>8);
                        data[dp++]=(unsigned char)psy; data[dp++]=(unsigned char)pw; data[dp++]=(unsigned char)psh;
                        data[dp++]=(unsigned char)(((isceil?1:0)<<6)|(plvl&3));        /* byte6 flags (bit7 clear) */
                        data[dp++]=(unsigned char)(vp[0]|(vp[1]<<4)); data[dp++]=(unsigned char)(vp[2]|(vp[3]<<4));
                        data[dp++]=(unsigned char)(vp[4]|(vp[5]<<4)); data[dp++]=(unsigned char)(vp[6]|(vp[7]<<4)); data[dp++]=0; cnt++;   /* bytes 7-10: V0..V7; byte 11 srcid(0) */
                        { int chk=(pw+15)/16; for(int y=psy;y<psy+psh&&y<256;y++)prof_scan[y]+=chk; }   /* count flat strips in the per-scanline profile too */
                    } }
                data[cpos]=cnt&0xff; data[cpos+1]=(cnt>>8)&0xff;       /* always index the view (floor+ceiling draw even with 0 walls) */
                if((cpos>>20)!=((dp-1)>>20)) fprintf(stderr,"BANK STRADDLE: view n%d a%d cpos=%lx..%lx (%ld bytes > 4096 headroom!)\n",p,a,cpos,dp,dp-cpos);
                /* ACCURATE sprite re-scan (the cart's real `spr`): LUT(40) + every record's ceil(w/16) strips,
                   INCLUDING the sky strips the old wall-only profile missed (why exteriors read 140-160). */
                { int skt=0; { unsigned m=data[cpos+2]|(data[cpos+3]<<8)|((unsigned)data[cpos+4]<<16); while(m){skt+=m&1;m>>=1;} }  /* skytop cols -> LUT ceiling skipped there */
                  static int sc[256]; for(int y=0;y<256;y++) sc[y]=20;   /* one LUT layer/scanline (floor below; ceiling above, minus skytop) */
                  long rr=cpos+5; int vspr=40-skt, wstr=0,fstr=0,sstr=0;  /* LUT = 20 floor + (20-skytop) ceiling */
                  for(int i=0;i<cnt;i++){ int tt=data[rr], rsy=data[rr+3], rw=data[rr+4], rh=data[rr+5];
                      int strips=(rw+15)/16; if(strips<1)strips=1; int rlen=12, hh=rh;
                      if(!(tt&0x80) && tt!=0x7F && data[rr+6]==0x80) rlen=7;        /* untextured flat = 7 bytes */
                      if(tt&0x80){ int dep=data[rr+7]; if(rw < 1 + (dep>>8)){ rr+=rlen; continue; } }  /* mirror the cart wall cull (dep>>8 == OFF -> keep long draw distance) */
                      int pf=strips;   /* per-FRAME strips: textured flats >=48px draw 2 perspective zones (per-scanline stays 1 -- zones don't overlap in y) */
                      if(!(tt&0x80)&&tt!=0x7F&&tt!=0x7E&&data[rr+6]<0x80&&rh>=16) pf=strips*((rh>=128)?4:((rh>=64)?3:2));
                      if(tt==0x7F){ int sK=(rh+15)/16; if(sK<1)sK=1; hh=sK*16; sstr+=strips; }    /* sky snapped to 16px grid */
                      else if(tt&0x80) wstr+=strips; else fstr+=pf;
                      vspr+=pf;
                      for(int y=rsy;y<rsy+hh&&y<256;y++) if(y>=0) sc[y]+=strips;
                      rr+=rlen; }
                  int swl=0; for(int y=0;y<224;y++) if(sc[y]>swl)swl=sc[y];
                  spr_sum+=vspr; scan_sum+=swl;
                  if(vspr>spr_max){spr_max=vspr;spr_mp=p;spr_ma=a;}
                  if(swl>scanmax){scanmax=swl;scan_mp=p;scan_ma=a;}
                  if(swl>96)scan_over96++; if(vspr>160)spr_over160++;
                  { int b=vspr/40; if(b>8)b=8; if(b<0)b=0; sprhist[b]++; }
                  if((vspr>=200||swl>=110)&&hotn<60){ hotn++; fprintf(stderr,"HOT node=%-3d a=%-2d  spr=%-3d (LUT=%d wall=%d flat=%d sky=%d)  scan=%-3d  cmds=%-3d\n",p,a,vspr,40-skt,wstr,fstr,sstr,swl,cnt); } }
                { int wl=0; for(int y=0;y<224;y++) if(prof_scan[y]>wl)wl=prof_scan[y];   /* worst per-scanline wall-sprite load this view */
                  if(cnt>prof_maxcnt){prof_maxcnt=cnt;prof_mc_p=p;prof_mc_a=a;}
                  if(wl>prof_maxwl){prof_maxwl=wl;prof_mw_p=p;prof_mw_a=a;}
                  if(cnt>=400)prof_caphits++;
                  if((wl>=72||cnt>=380)&&prof_busyn<48){ prof_busyn++; printf("BUSY node=%-3d a=%-2d cmds=%-3d wall-spr/worstline=%d (+~20-40 LUT)\n",p,a,cnt,wl); } }
                idx[(long)p*NA+a]=(int)cpos; } }
        printf("BAKEC PROFILE: max %d cmds/view @node%d a%d | max wall-sprites/scanline %d @node%d a%d (~%d/96 with LUT) | views hitting 400-cmd cap: %d\n",
               prof_maxcnt,prof_mc_p,prof_mc_a, prof_maxwl,prof_mw_p,prof_mw_a, prof_maxwl+30, prof_caphits);
        { long NV=(long)PATH_N*NA;
          printf("BAKEC SPRITES (LUT+walls+flats+sky): avg %.0f/frame, max %d @node%d a%d | per-scanline avg %.0f, max %d @node%d a%d (HW caps: 381/frame, 96/line)\n",
                 (double)spr_sum/NV, spr_max,spr_mp,spr_ma, (double)scan_sum/NV, scanmax,scan_mp,scan_ma);
          printf("BAKEC SPRITE DIST: views >96/scanline=%d (%.1f%%)  >160/frame=%d (%.1f%%) | frame-sprite histogram [0-40..>320 by 40]: %d %d %d %d %d %d %d %d %d\n",
                 scan_over96,100.0*scan_over96/NV, spr_over160,100.0*spr_over160/NV,
                 sprhist[0],sprhist[1],sprhist[2],sprhist[3],sprhist[4],sprhist[5],sprhist[6],sprhist[7],sprhist[8]); }
        long NBANK=(dp+0xFFFFFL)/0x100000L; if(NBANK<1)NBANK=1;
        START_NODE=0; { float bd=1e18f; for(int q=0;q<PATH_N;q++){ float dx=PX[q]-MAP_START.pos.x,dy=PY[q]-MAP_START.pos.y,d=dx*dx+dy*dy; if(d<bd){bd=d;START_NODE=q;} } }  /* start at the DEFAULT level spawn (nearest path node to MAP_START) */
        (void)sky_bp;
        /* nodes_data.h: shared decls. NODEIDX (small) -> ROM1 (always addressable). NODES (the
           bank-aligned blob) -> __bank/.text2 -> banked P2, mapped 1MB at a time @0x200000. */
        FILE*f=fopen("neogeo/nodes_data.h","w");
        fprintf(f,"/* PoC node-render data (spawn cluster, generated) -- decls only */\n");
        fprintf(f,"#define NODE_NA %d\n#define PATH_N %d\n#define START_NODE %d\n",NA,PATH_N,START_NODE);   /* on-rails: hero-path nodes x NA angles; START_NODE = spawn (dolly's initial pt) */
        fprintf(f,"#define NODEIDX_COUNT %ld\n#define NODE_NBANK %ld\n",ICOUNT,NBANK);
        fprintf(f,"extern const int NODEIDX[NODEIDX_COUNT];\n");          /* byte offset of each (node*NA+angle) draw-list */
        fprintf(f,"extern const short PATHX[PATH_N], PATHY[PATH_N];\n");  /* world XY of each path node (imp projection / floor phase) */
        fprintf(f,"extern const unsigned char PATHANG[PATH_N];\n");
        fprintf(f,"extern const unsigned char PATHCEIL[PATH_N];\n");   /* 1 = this node's room uses the DARK ceiling LUT (B) */        /* per-node forward tangent (256-angle), PRECOMPUTED here: the cart's old boot-time argmax over NODE_NA soft-float dots took ~10s on the 68000 */
        fprintf(f,"#define NODES ((const volatile unsigned char*)0x200000)\n");  /* bank window: P_ROM_SWITCH_BANK(off>>20), then NODES[off&0xFFFFF] */
        fclose(f);
        FILE*ci=fopen("neogeo/nodes_idx.c","w");                          /* -> ROM1 (always readable, never banked out) */
        fprintf(ci,"#include \"nodes_data.h\"\n");
        fprintf(ci,"const int NODEIDX[NODEIDX_COUNT]={"); for(long i=0;i<ICOUNT;i++)fprintf(ci,"%d,",idx[i]); fprintf(ci,"};\n");
        fprintf(ci,"const short PATHX[PATH_N]={"); for(int p=0;p<PATH_N;p++)fprintf(ci,"%d,",(int)PX[p]); fprintf(ci,"};\n");
        fprintf(ci,"const short PATHY[PATH_N]={"); for(int p=0;p<PATH_N;p++)fprintf(ci,"%d,",(int)PY[p]); fprintf(ci,"};\n");
        fprintf(ci,"const unsigned char PATHANG[PATH_N]={");                /* forward tangents, STICKY-BINNED: raw per-node tangents cross 15-degree bin
                                                                               boundaries on every gentle wiggle, and each crossing swaps the whole view +
                                                                               rotates the floor LUT (the "floor flowing the wrong way" lurch). Hysteresis:
                                                                               hold the current bin until the tangent strays >3/4 of a bin from its centre,
                                                                               so the heading only changes at real corners. Also: fewer view swaps. */
        { /* WIDE-WINDOW TANGENTS + MIN-RUN BINS. The old 1-node tangents carried the centring
             micro-shifts as +-5-10 degrees of noise, so any segment near a 15-degree boundary
             flapped bins EVERY NODE despite the hysteresis: the bake measured 438 bin changes
             over 1146 nodes, median run length ONE. Every flap is a full view rebuild with zero
             signature hits -- the author's "each shot nothing like the previous" (the one 105-node run
             was his glass-smooth tunnel). Tangents over +-4 nodes (~200u) average the noise out;
             min-run enforcement then guarantees a bin change can only survive at a real corner. */
          static unsigned char binv[2048];
          for(int p=0;p<PATH_N;p++){
              int a0=p-4; if(a0<0)a0=0; int b2=p+4; if(b2>PATH_N-1)b2=PATH_N-1; if(b2<=a0)b2=(a0+1<PATH_N)?a0+1:a0;
              float tx=PX[b2]-PX[a0], ty=PY[b2]-PY[a0]; int best=0; float bd=-1e30f;
              for(int q=0;q<NA;q++){ angle_t aa=(angle_t)(q*256/NA); float d=tx*lut_cos(aa)+ty*lut_sin(aa); if(d>bd){bd=d;best=q;} }
              binv[p]=(unsigned char)best; }
          { const int MINRUN=5; int changed=1;
            while(changed){ changed=0; int i=0;
                while(i<PATH_N){ int j=i; while(j<PATH_N && binv[j]==binv[i]) j++;
                    int len=j-i;
                    if(len<MINRUN && len<PATH_N){
                        int prevlen=0,nextlen=0;
                        if(i>0){ int k=i-1; unsigned char pb=binv[i-1]; while(k>=0&&binv[k]==pb)k--; prevlen=(i-1)-k; }
                        if(j<PATH_N){ int k=j; unsigned char nb2=binv[j]; while(k<PATH_N&&binv[k]==nb2)k++; nextlen=k-j; }
                        unsigned char tgt=(i==0)?((j<PATH_N)?binv[j]:binv[i]):((j>=PATH_N)?binv[i-1]:((prevlen>=nextlen)?binv[i-1]:binv[j]));
                        if(tgt!=binv[i]){ for(int k=i;k<j;k++) binv[k]=tgt; changed=1; }
                    }
                    i=j; } } }
          for(int p=0;p<PATH_N;p++) fprintf(ci,"%d,",(binv[p]*256/NA)&0xff); }
        fprintf(ci,"};\n");
        { int ffA2=lv->sectors[point_sector(lv,MAP_START.pos.x,MAP_START.pos.y)].ceiltex;   /* LUT A = the spawn room's ceiling */
          /* LUT B = the most common OTHER ceiltex ALONG THE RAIL -- a sector census picks rooms the
             player never enters (it chose CEIL5_1, which the path crosses ZERO times -> PATHCEIL was
             all-0 and dark rooms kept the grey ceiling). Census what the dolly actually rides over. */
          int ph[1026]; memset(ph,0,sizeof ph);
          for(int p=0;p<PATH_N;p++){ int sc=point_sector(lv,PX[p],PY[p]); int ct=(sc>=0)?lv->sectors[sc].ceiltex:-1;
              ph[(ct>=0&&ct<1024)?ct:1025]++; }
          int ffB2=ffA2; { int best=0; for(int q=0;q<1024&&q<NTEXh;q++) if(q!=ffA2&&TEX[q].w>0&&ph[q]>best){best=ph[q];ffB2=q;} }
          fprintf(stderr,"PATHCEIL census (path nodes per ceiltex):");
          for(int q=0;q<1026;q++) if(ph[q]) fprintf(stderr," tex%d:%d",q,ph[q]);
          fprintf(stderr,"  [LUT B=tex %d]\n",ffB2);
          { FILE*cb=fopen("/tmp/ceilB.txt","w"); if(cb){ fprintf(cb,"%d\n",ffB2); fclose(cb); } }   /* sidecar: bakeceil bakes LUT B from the SAME pick */
          fprintf(ci,"const unsigned char PATHCEIL[PATH_N]={");   /* 0=A grey 1=B dark 2=C-white 3=C-red 4=C-amber (TLITE rooms ride the tintable spotted LUT) */
          for(int p=0;p<PATH_N;p++){ int sc=point_sector(lv,PX[p],PY[p]); int ct=(sc>=0)?lv->sectors[sc].ceiltex:-1;
              int v=0; if(ct==ffB2)v=1; else if(ct>=50&&ct<=53)v=2;   /* every TLITE room rides LUT C with its NATURAL spot colours (red-orange by source art) */
              fprintf(ci,"%d,",v); }
          fprintf(ci,"};\n"); }
        fclose(ci);
        /* (nodes.bin write retired: the dead 8MB P2 it fed is gone. The node-view pass still runs because the
           ramp manifest below is gathered as its side-effect -- only the unused banked blob output is dropped.) */
        printf("BAKEC: %d path nodes x %d angles = %ld views computed (ramp manifest only; NODES blob no longer written)\n",
               PATH_N,NA,ICOUNT);
        printf("BAKEC slope: %ld/%ld wall records have |slope|>2 (%.0f%%), max |slope|=%d px\n",
               slp_nz,slp_tot,slp_tot?100.0*slp_nz/slp_tot:0.0,slp_max);
        printf("BAKEC SKY: %ld sky records across %d/%ld views, max sky h=%dpx; biggest sky view node=%d a=%d (%ld px)\n",
               sky_recs,sky_views,ICOUNT,sky_maxh,sky_bp,sky_ba,sky_bestarea);
        printf("BAKEC ramp-clamp: %ld/%ld records (%.0f%%) have a per-chunk drop >32px (residual, near edge-on -> small clamped caps)\n",
               slp_clamp,slp_tot,slp_tot?100.0*slp_clamp/slp_tot:0.0);
        printf("BAKEC du-cap: %ld/%ld records (%.0f%%) are oblique enough that the texture WANTED >1 tile/strip (these were stretching at cap 16)\n",
               slp_clamp2,slp_tot,slp_tot?100.0*slp_clamp2/slp_tot:0.0);
        { int combos=0,texused=0; long rtiles=0;                       /* MANIFEST: the exact (tex, drop, edge) ramp combos the runtime will request */
          FILE *rm=fopen("ramps_used.txt","w"); if(rm) fprintf(rm,"# tex drop edge   (drop=px across one 16px chunk, edge 0=top 1=bottom)\n");
          for(int t=0;t<128;t++){ int any=0;
            for(int d=0;d<RDN;d++)for(int e=0;e<2;e++) if(rampneed[t][d][e] && (d<RDMAX-2||d>RDMAX+2)){   /* skip |drop|<=2px: near-flat, ramp==flat tile */
                int drop=d-RDMAX, nt=(abs(drop)+15)/16; if(nt<1)nt=1;                             /* multi-tile stack height (<=2 with the +/-32 cap) */
                combos++; any=1; rtiles += (long)(((t<NTEXh&&TEX[t].w>0)?(TEX[t].w+15)/16:1))*8*nt;  /* wt*NSHIFT*nt tiles per combo */
                if(rm) fprintf(rm,"%d %d %d\n", t, drop, e); }
            if(any)texused++; }
          if(rm) fclose(rm);
          printf("RAMP manifest: %d combos across %d textures -> ~%ld ramp tiles (%.0fKB c1+c2) written to ramps_used.txt\n",
                 combos,texused,rtiles,rtiles*128/1024.0); }
        if(getenv("DBGAUDIT")){   /* AUTOMATED END-TO-END SCAN: every baked view's records, flag glitch signatures, report worst nodes */
          long tot_uni=0,tot_abf=0,tot_lcl=0; struct{int node,cnt;}worst[24]; for(int i=0;i<24;i++){worst[i].node=-1;worst[i].cnt=0;}
          for(int n=0;n<PATH_N;n++){ int nc=0;
            for(int a=0;a<NA;a++){ long oo=idx[(long)n*NA+a]; if(oo<0)continue; long rr=oo; int rcc=data[rr]|(data[rr+1]<<8); rr+=2; rr+=3;
              for(int i=0;i<rcc;i++){ int tt=data[rr],rlen=12; (void)tt;
                if(rlen==11&&!(tt&0x80)&&tt!=0x7F&&tt!=0x7E){ int rsy=data[rr+3],rh=data[rr+5],isceil=(data[rr+6]>>6)&1;
                  int V0=data[rr+7]&0xf,uni=1; for(int b=0;b<4;b++){ if((data[rr+7+b]&0xf)!=V0||((data[rr+7+b]>>4)&0xf)!=V0)uni=0; }
                  if(uni&&rh>=48){tot_uni++;nc++;} if(!isceil&&rsy<80){tot_abf++;nc++;} if(isceil&&rsy+rh>152){tot_lcl++;nc++;} }
                rr+=rlen; } }
            if(nc>0){ int mi=0; for(int i=1;i<24;i++) if(worst[i].cnt<worst[mi].cnt)mi=i; if(nc>worst[mi].cnt){worst[mi].node=n;worst[mi].cnt=nc;} } }
          for(int i=0;i<24;i++)for(int j=i+1;j<24;j++) if(worst[j].cnt>worst[i].cnt){int a=worst[i].node,b=worst[i].cnt;worst[i].node=worst[j].node;worst[i].cnt=worst[j].cnt;worst[j].node=a;worst[j].cnt=b;}
          printf("DBGAUDIT (%d nodes x %d ang): uniform-2D-flats(h>=48)=%ld  above-horizon-floors=%ld  below-horizon-ceils=%ld\n",PATH_N,NA,tot_uni,tot_abf,tot_lcl);
          printf("DBGAUDIT worst nodes:"); for(int i=0;i<14;i++) if(worst[i].node>=0) printf(" %d(%d)",worst[i].node,worst[i].cnt); printf("\n");
          SDL_Quit(); return 0; }
        if(getenv("DBGPLAY")){   /* AUDIT: render the cart-faithful FORWARD view at every DBGSTEP-th path node -> /tmp/play_NNNN.bmp (headless full playthrough) */
          int step=getenv("DBGSTEP")?atoi(getenv("DBGSTEP")):4; if(step<1)step=1; int dumped=0;
          for(int n=0;n<PATH_N;n+=step){
            float dx,dy; if(n+1<PATH_N){dx=PX[n+1]-PX[n];dy=PY[n+1]-PY[n];} else if(n>0){dx=PX[n]-PX[n-1];dy=PY[n]-PY[n-1];} else {dx=1;dy=0;}
            float ad=atan2f(dy,dx)*57.2957795f; if(ad<0)ad+=360.0f;
            int A=((int)(ad/(360.0f/NA)+0.5f))%NA; if(A<0)A+=NA;
            long o=idx[(long)n*NA+A]; if(o<0)continue;
            draw_nodeview(fb,data,o,(int)(A*256/NA));
            char fn[64]; snprintf(fn,sizeof fn,"/tmp/play_%04d.bmp",n); SDL_SaveBMP(fb,fn); dumped++; }
          printf("DBGPLAY: dumped %d node views (step %d) -> /tmp/play_*.bmp\n",dumped,step);
          SDL_Quit(); return 0; }
        { int NN=getenv("DBGNODE")?atoi(getenv("DBGNODE")):58, A=getenv("DBGANG")?atoi(getenv("DBGANG")):12;
          if(getenv("DBGFINDX")){ float fx=atof(getenv("DBGFINDX")),fy=atof(getenv("DBGFINDY")); float bd=1e18f; for(int q=0;q<PATH_N;q++){float dx=PX[q]-fx,dy=PY[q]-fy,d=dx*dx+dy*dy; if(d<bd){bd=d;NN=q;}} printf("NEAREST path node to (%.0f,%.0f) = %d at (%.0f,%.0f)\n",fx,fy,NN,PX[NN],PY[NN]); }
          if(NN<0||NN>=PATH_N)NN=58; if(A<0||A>=NA)A=12;
          int NS=getenv("DBGSWEEP")?atoi(getenv("DBGSWEEP")):1; if(NS<1)NS=1; if(NS>NA)NS=NA;   /* render NS consecutive angles -> /tmp/cmp_node_a{A}.bmp for rotation A/B */
          for(int k=0;k<NS;k++){ int AA=(A+k)%NA; camera_t cam2; cam2.pos.x=PX[NN]; cam2.pos.y=PY[NN]; cam2.ang=(angle_t)(AA*256/NA); cam2.pitch=0;
            cam2.sector=point_sector(lv,PX[NN],PY[NN]); cam2.z=lv->sectors[cam2.sector].floor+41.0f;
            render_world(lv,&cam2,&dl);
            { Uint32*px=(Uint32*)fb->pixels; int pitch=fb->pitch/4;
              for(int y=0;y<SCREEN_H;y++){ Uint32 c=(y<112)?SDL_MapRGB(fb->format,204,204,204):SDL_MapRGB(fb->format,68,68,68); for(int x=0;x<SCREEN_W;x++)px[y*pitch+x]=c; } }
            long o=idx[(long)NN*NA+AA]; if(o>=0){ draw_nodeview(fb,data,o,(int)cam2.ang); char fn[64]; if(NS>1)snprintf(fn,sizeof fn,"/tmp/cmp_node_a%d.bmp",AA); else snprintf(fn,sizeof fn,"/tmp/cmp_node.bmp"); SDL_SaveBMP(fb,fn); }
            if(getenv("DBGREC") && o>=0){   /* tree-trunk hunt: baked WALL records vs the (correct) dl, tall ones */
              long rr=o; int rc=data[rr]|(data[rr+1]<<8); rr+=2; rr+=3; int nt=0;
              for(int i=0;i<rc;i++){ int tt=data[rr],rlen=12; (void)tt;
                int rsx=(short)(data[rr+1]|(data[rr+2]<<8)),rsy=data[rr+3],rw=data[rr+4],rh=data[rr+5],rdt=(signed char)data[rr+9],rdb=(signed char)data[rr+10];
                if((tt&0x80)&&rh>=120){ if(nt++<14) fprintf(stderr,"REC WALL tex=%-2d sx=%-3d sy=%-3d w=%-3d h=%-3d dtop=%-4d dbot=%-4d\n",tt&0x7f,rsx,rsy,rw,rh,rdt,rdb); }
                rr+=rlen; }
              fprintf(stderr,"-- (%d tall baked walls h>=120) vs dl tall walls: --\n",nt); int dt=0;
              for(int j=0;j<dl.n;j++){ const SpriteCmd*c=&dl.cmd[j]; if(c->kind==SC_WALL&&c->h>=120&&dt++<14) fprintf(stderr,"  dl WALL tex=%-2d sx=%-3d sy=%-3d w=%-3d h=%-3d dtop=%-4d dbot=%-4d dep=%d\n",c->tex,c->sx,c->sy,c->w,c->h,c->dtop,c->dbot,c->depth); }
              fprintf(stderr,"-- baked FLAT records (textured, w>16) + their 8 recede V-nibbles (uniform = flat/2D): --\n");
              rr=o; rc=data[rr]|(data[rr+1]<<8); rr+=2; rr+=3; int nf=0;
              for(int i=0;i<rc;i++){ int tt=data[rr],rlen=12; (void)tt;
                if(!(tt&0x80)&&tt!=0x7F&&tt!=0x7E&&rlen==11){ int ftex=tt&0x7f,rsx=(short)(data[rr+1]|(data[rr+2]<<8)),rsy=data[rr+3],rw=data[rr+4],rh=data[rr+5];
                  int V[8]={data[rr+7]&0xf,(data[rr+7]>>4)&0xf,data[rr+8]&0xf,(data[rr+8]>>4)&0xf,data[rr+9]&0xf,(data[rr+9]>>4)&0xf,data[rr+10]&0xf,(data[rr+10]>>4)&0xf};
                  int isceil=(data[rr+6]>>6)&1, uniform=1; for(int v=1;v<8;v++) if(V[v]!=V[0])uniform=0;
                  if(rw>16&&nf++<16) fprintf(stderr,"  FLAT tex=%-2d %s sx=%-3d sy=%-3d w=%-3d h=%-3d V=[%d %d %d %d %d %d %d %d]%s\n",ftex,isceil?"CEIL":"flr ",rsx,rsy,rw,rh,V[0],V[1],V[2],V[3],V[4],V[5],V[6],V[7],uniform?"  <-- UNIFORM(2D)":""); }
                rr+=rlen; } }
            SDL_FillRect(fb,NULL,0); draw(fb,&dl,&cam2); { char fn[64]; if(NS>1)snprintf(fn,sizeof fn,"/tmp/cmp_ideal_a%d.bmp",AA); else snprintf(fn,sizeof fn,"/tmp/cmp_ideal.bmp"); SDL_SaveBMP(fb,fn); } }
          printf("comparison (node %d a%d+%d): dl.n=%d -> /tmp/cmp_*.bmp\n",NN,A,NS,dl.n); }
        SDL_Quit(); return 0;
    }
    if(bakefloor){            /* periodic floor LUT: floor-cast the spawn floor flat at NA angles -> /tmp/floorlut.raw */
        int sec=point_sector(lv,MAP_START.pos.x,MAP_START.pos.y);
        int ff=getenv("FLAT")?atoi(getenv("FLAT")):lv->sectors[sec].floortex;   /* FLAT env -> bake ANY flat texid (per-texture LUT); unset = spawn floor (legacy) */
        if(ff<0||ff>=NTEXh||TEX[ff].w<=0){ fprintf(stderr,"bakefloor: no floor tex (sec %d ff %d)\n",sec,ff); SDL_Quit(); return 1; }
        Tex t=TEX[ff]; int fw=t.w, fh=t.h;
        long hist[256]={0}; for(int i=0;i<fw*fh;i++) hist[t.pix[i]]++;
        int pidx[15], np=0;
        for(int pass=0;pass<15;pass++){ long best=0; int bi=-1;
            for(int c=0;c<256;c++){ if(hist[c]>best){ int used=0; for(int q=0;q<np;q++) if(pidx[q]==c)used=1; if(!used){best=hist[c];bi=c;} } }
            if(bi<0)break; pidx[np++]=bi; }
        int remap[256]; for(int c=0;c<256;c++){ int bj=0; long bd=1L<<60;
            for(int q=0;q<np;q++){ int dr=PAL[c].r-PAL[pidx[q]].r,dg=PAL[c].g-PAL[pidx[q]].g,db=PAL[c].b-PAL[pidx[q]].b;
                long d=(long)dr*dr+dg*dg+db*db; if(d<bd){bd=d;bj=q;} } remap[c]=bj+1; }
        int NAL=getenv("FLNAL")?atoi(getenv("FLNAL")):24, NA=getenv("FLNA")?atoi(getenv("FLNA")):13, NPH=getenv("FLNPHASE")?atoi(getenv("FLNPHASE")):16, H=112, W=320;   /* MIRRORED ANGLES (default 13/24): cart h-flips 13..23. VS variant: FLNA=21 FLNAL=128 -> 21 sets over 60deg (hex symmetry, cart folds heading%21, no mirror). */ unsigned char *buf=(unsigned char*)malloc((long)W*H*NA*NPH);
        /* FLVS (VS synthetic floor only): COLMAP-FLOOR depth-fade bake -- 15-level mean-hued luminance
           ramp x perspective depth (far -> dark), the same trick as the ceiling (CLVS). NOT for the
           per-flat (FLAT=N) gen0 bakes, which keep real texture colours. FLFADE/FLMINB env-tunable. */
        int flvs=getenv("FLVS")?1:0;
        float fhr=1.f,fhg=1.f,fhb=1.f, flfade=0.18f, flminb=0.10f, flvis=80.f;   /* flvis = the VISIBLE floor's screen-row extent (dy 0..~80; the near floor below that is under the gun/HUD) */
        (void)flfade;
        if(flvs){ long sr=0,sg=0,sb=0; long npx=(long)fw*fh; for(long i=0;i<npx;i++){ SDL_Color c=PAL[t.pix[i]]; sr+=c.r;sg+=c.g;sb+=c.b; }
            float mr=(float)sr/npx,mg=(float)sg/npx,mb=(float)sb/npx, ml=0.299f*mr+0.587f*mg+0.114f*mb; if(ml<1.f)ml=1.f; fhr=mr/ml;fhg=mg/ml;fhb=mb/ml;
            if(getenv("FLMINB"))flminb=(float)atof(getenv("FLMINB")); if(getenv("FLVIS"))flvis=(float)atof(getenv("FLVIS")); }
        for(int A=0;A<NA;A++){ angle_t ang=(angle_t)(A*256/NAL); float co=lut_cos(ang), si=lut_sin(ang);
            for(int p=0;p<NPH;p++){               /* phase = 64u/NPH forward steps over the floor period -> flow */
                float px0=MAP_START.pos.x+p*(64.0f/NPH)*co, py0=MAP_START.pos.y+p*(64.0f/NPH)*si, relz=41.0f;
                for(int yy=0;yy<H;yy++){ float dy=(float)yy; if(dy<0.5f)dy=0.5f; float D=relz*PCFG.fov/dy, fwx=px0+D*co, fwy=py0+D*si, rt=D/PCFG.fov;
                    float b=1.f; if(flvs){ b=flminb+(1.f-flminb)*(dy>=flvis?1.f:dy/flvis); }   /* SCREEN-LINEAR fade: near (large dy, FRONT) = 1.0 lighter, far (dy->0, BACK) = flminb darker -> an EVEN front-light/back-dark gradient across the visible band (the author 2026-06-23: the floor's near is under the gun/HUD, so a perspective fade read flat) */
                    for(int x=0;x<W;x++){ float off=(x-160)*rt; int U=imod((int)floorf(fwx+off*si),fw), V=imod((int)floorf(fwy-off*co),fh);
                        if(flvs){ SDL_Color c=PAL[t.pix[V*fw+U]]; float lum=0.299f*c.r+0.587f*c.g+0.114f*c.b; int Lv=(int)(lum*b*15.f/256.f); if(Lv<0)Lv=0; if(Lv>14)Lv=14; buf[(((long)(A*NPH+p)*H)+yy)*W+x]=(unsigned char)(Lv+1); }
                        else buf[(((long)(A*NPH+p)*H)+yy)*W+x]=(unsigned char)remap[t.pix[V*fw+U]]; } } } }
        const char*flout=getenv("FLOUT")?getenv("FLOUT"):"/tmp/floorlut.raw";
        const char*flpal=getenv("FLPAL")?getenv("FLPAL"):"/tmp/floorlut.pal";
        FILE*f=fopen(flout,"wb"); int ww=W,hh=H*NA*NPH,na=NA,nph=NPH;
        fwrite(&ww,4,1,f); fwrite(&hh,4,1,f); fwrite(&na,4,1,f); fwrite(&nph,4,1,f); fwrite(buf,1,(long)W*H*NA*NPH,f); fclose(f);
        f=fopen(flpal,"w");
        if(flvs){ for(int q=0;q<15;q++){ float Lb=(q+1)*255.f/15.f; int rr=(int)(fhr*Lb),gg=(int)(fhg*Lb),bb=(int)(fhb*Lb); if(rr>255)rr=255;if(gg>255)gg=255;if(bb>255)bb=255; fprintf(f,"%d %d %d\n",rr,gg,bb); } }
        else { for(int q=0;q<np;q++) fprintf(f,"%d %d %d\n",PAL[pidx[q]].r,PAL[pidx[q]].g,PAL[pidx[q]].b); }
        fclose(f);
        printf("BAKEFLOOR: flat=%d (%dx%d), %d colours, %d angles x %d phases, sheet %dx%d -> /tmp/floorlut.raw\n",ff,fw,fh,np,NA,NPH,W,H*NA*NPH);
        SDL_Quit(); return 0;
    }
    if(bakeceil){            /* TWO periodic ceiling LUTs (4 phases each = the old 8-phase footprint):
                                A = the spawn ceiling (grey tech, most sectors), B = the dark-room ceiling
                                (2nd most common ceiltex). The cart picks per node -> rooms get a ceiling
                                that READS right instead of one texture everywhere. */
        int sec=point_sector(lv,MAP_START.pos.x,MAP_START.pos.y), ffA=lv->sectors[sec].ceiltex;
        if(ffA<0||ffA>=NTEXh||TEX[ffA].w<=0){ fprintf(stderr,"bakeceil: no usable tex\n"); SDL_Quit(); return 1; }
        int cnt2[1024]; for(int i2=0;i2<1024;i2++)cnt2[i2]=0;
        for(int i2=0;i2<lv->nsectors;i2++){ int ct=lv->sectors[i2].ceiltex; if(ct>=0&&ct<1024&&ct!=ffA)cnt2[ct]++; }
        int ffB=ffA; { int best=0; for(int i2=0;i2<1024;i2++) if(i2<NTEXh&&TEX[i2].w>0&&cnt2[i2]>best){best=cnt2[i2];ffB=i2;} }
        { FILE*cb=fopen("/tmp/ceilB.txt","r"); if(cb){ int v=-1; if(fscanf(cb,"%d",&v)==1&&v>=0&&v<NTEXh&&TEX[v].w>0)ffB=v; fclose(cb);
              fprintf(stderr,"bakeceil: LUT B overridden by path census sidecar -> tex %d\n",ffB); } }   /* written by --bakec: the rail's true second ceiling */
        fprintf(stderr,"bakeceil: LUT A=tex %d (spawn), LUT B=tex %d (dark rooms)\n",ffA,ffB);
        /* LUT C = the SPOTTED LAMP ceiling (TLITE6_5-class), tintable at runtime: its 4 brightest
           palette entries are forced into reserved slots 12..15, so the cart recolors just those
           entries per room (white/red/amber) from ONE set of tiles. Baked at 2 phases (32u steps)
           -- lamp grids tile coarsely, the halved scroll keeps it inside the spare C-ROM. */
        int ffC=-1; { int bestc=0; for(int q=50;q<=53&&q<NTEXh;q++){ if(TEX[q].w>0){ int cnt3=0; for(int i2=0;i2<lv->nsectors;i2++) if(lv->sectors[i2].ceiltex==q)cnt3++; if(cnt3>bestc){bestc=cnt3;ffC=q;} } } if(ffC<0)ffC=ffA; }
        fprintf(stderr,"bakeceil: LUT C=tex %d (spotted lamp panel, tintable)\n",ffC);
        int NAL=getenv("CLNAL")?atoi(getenv("CLNAL")):24, NA=getenv("CLNA")?atoi(getenv("CLNA")):13, NPH=4, H=112, W=320; unsigned char *buf=(unsigned char*)malloc((long)W*H*NA*NPH);   /* default 13/24 mirrored (cart h-flips); VS variant: CLVS=1 CLNA=32 CLNAL=128 -> grey LUT A only, 32 sets over 90deg (cart folds heading%32) */
        int clvs=getenv("CLVS")?1:0;
        int ffs[3]; ffs[0]=ffA; ffs[1]=ffB; ffs[2]=ffC;
        for(int L=0;L<(clvs?1:3);L++){
        int NPHL=(L==2)?2:NPH;
        int ff=ffs[L];
        Tex t=TEX[ff]; int fw=t.w, fh=t.h;
        long hist[256]={0}; for(int i=0;i<fw*fh;i++) hist[t.pix[i]]++;
        int pidx[15], np=0;
        for(int pass=0;pass<15;pass++){ long best=0; int bi=-1;
            for(int c=0;c<256;c++){ if(hist[c]>best){ int used=0; for(int q=0;q<np;q++) if(pidx[q]==c)used=1; if(!used){best=hist[c];bi=c;} } }
            if(bi<0)break; pidx[np++]=bi; }
        if(L==2){ /* spots into reserved indices: luminance-sort, brightest LAST (slots 12..15) */
            for(int a2=1;a2<np;a2++){ int v=pidx[a2]; long lv2=(long)PAL[v].r+PAL[v].g+PAL[v].b; int b2=a2-1;
                while(b2>=0){ int u=pidx[b2]; if((long)PAL[u].r+PAL[u].g+PAL[u].b>lv2){ pidx[b2+1]=pidx[b2]; b2--; } else break; }
                pidx[b2+1]=v; } }
        int remap[256]; for(int c=0;c<256;c++){ int bj=0; long bd=1L<<60;
            for(int q=0;q<np;q++){ int dr=PAL[c].r-PAL[pidx[q]].r,dg=PAL[c].g-PAL[pidx[q]].g,db=PAL[c].b-PAL[pidx[q]].b;
                long d=(long)dr*dr+dg*dg+db*db; if(d<bd){bd=d;bj=q;} } remap[c]=bj+1; }
        /* INC2 (clvs only): COLMAP-CEILING depth-fade bake -- a 15-level mean-hued LUMINANCE ramp,
           darkened by perspective depth (far -> dark) -> a smooth 15-step gradient baked into the tiles
           (vs the 4 runtime palette banks). clfade = darkening per unit relative-depth; clminb = darkest.
           Env-tunable (CLFADE/CLMINB) for the ride. g_ceildark is dead in the live engine so the ramp
           palette can own bank 12 outright. */
        float chr=1.f,chg=1.f,chb=1.f, clfade=0.18f, clminb=0.38f, clpitch=1.f;   /* clpitch>1 = tighter texture pitch (denser pattern) without changing the ceiling height/relz. Ship 1.66 (the author 2026-06-24). */
        if(clvs){ long sr=0,sg=0,sb=0; long npx=(long)fw*fh; for(long i=0;i<npx;i++){ SDL_Color c=PAL[t.pix[i]]; sr+=c.r;sg+=c.g;sb+=c.b; }
            float mr=(float)sr/npx,mg=(float)sg/npx,mb=(float)sb/npx, ml=0.299f*mr+0.587f*mg+0.114f*mb; if(ml<1.f)ml=1.f; chr=mr/ml;chg=mg/ml;chb=mb/ml;
            if(getenv("CLFADE"))clfade=(float)atof(getenv("CLFADE")); if(getenv("CLMINB"))clminb=(float)atof(getenv("CLMINB")); if(getenv("CLPITCH"))clpitch=(float)atof(getenv("CLPITCH")); }
        for(int A=0;A<NA;A++){ angle_t ang=(angle_t)(A*256/NAL); float co=lut_cos(ang), si=lut_sin(ang);
            for(int p=0;p<NPHL;p++){
                float px0=MAP_START.pos.x+p*(L==2?32:16)*co, py0=MAP_START.pos.y+p*(L==2?32:16)*si, relz=(getenv("CLRELZ")?(float)atof(getenv("CLRELZ")):41.0f);   /* ceiling height above eye; CLRELZ env tunes it. A/B: 4 x 16u; C: 2 x 32u */
                for(int yy=0;yy<H;yy++){ float dy=(float)yy; if(dy<0.5f)dy=0.5f; float D=relz*PCFG.fov/dy, fwx=px0+D*co, fwy=py0+D*si, rt=D/PCFG.fov;
                    float b=1.f; if(clvs){ b=1.f-(112.f/dy-1.f)*clfade; if(b<clminb)b=clminb; if(b>1.f)b=1.f; }   /* 112/dy = perspective depth ratio (1=near .. large=horizon) -> b fades far rows dark */
                    for(int x=0;x<W;x++){ float off=(x-160)*rt; int U=imod((int)floorf((fwx+off*si)*clpitch),fw), V=imod((int)floorf((fwy-off*co)*clpitch),fh);   /* CLPITCH scales the texture sampling -> denser pattern (tighter pitch), same height */
                        if(clvs){ SDL_Color c=PAL[t.pix[V*fw+U]]; float lum=0.299f*c.r+0.587f*c.g+0.114f*c.b; int Lv=(int)(lum*b*15.f/256.f); if(Lv<0)Lv=0; if(Lv>14)Lv=14; buf[(((long)(A*NPHL+p)*H)+yy)*W+x]=(unsigned char)(Lv+1); }   /* texel luminance (panel pattern) x depth fade -> ramp level 1..15 */
                        else buf[(((long)(A*NPHL+p)*H)+yy)*W+x]=(unsigned char)remap[t.pix[V*fw+U]]; } } } }
        { const char*rn=clvs?"/tmp/vsceil.raw":((L==0)?"/tmp/ceillut.raw":((L==1)?"/tmp/ceillut2.raw":"/tmp/ceillut3.raw"));
          const char*pn=clvs?"/tmp/vsceil.pal":((L==0)?"/tmp/ceillut.pal":((L==1)?"/tmp/ceillut2.pal":"/tmp/ceillut3.pal"));
          FILE*f=fopen(rn,"wb"); int ww=W,hh=H*NA*NPHL,na=NA,nph=NPHL;
          fwrite(&ww,4,1,f); fwrite(&hh,4,1,f); fwrite(&na,4,1,f); fwrite(&nph,4,1,f); fwrite(buf,1,(long)W*H*NA*NPHL,f); fclose(f);
          f=fopen(pn,"w");
          if(clvs){ for(int q=0;q<15;q++){ float Lb=(q+1)*255.f/15.f; int rr=(int)(chr*Lb),gg=(int)(chg*Lb),bb=(int)(chb*Lb); if(rr>255)rr=255;if(gg>255)gg=255;if(bb>255)bb=255; fprintf(f,"%d %d %d\n",rr,gg,bb); } }   /* 15-level mean-hued ramp: idx i -> luminance i*255/15 */
          else { for(int q=0;q<np;q++) fprintf(f,"%d %d %d\n",PAL[pidx[q]].r,PAL[pidx[q]].g,PAL[pidx[q]].b); }
          fclose(f); }
        }
        printf("BAKECEIL: 3 LUTs (A+B at 4 phases, spotted C at 2) -> /tmp/ceillut{,2,3}.raw\n");
        SDL_Quit(); return 0;
    }
    if(dump){
        if(modes){ for(int m=1;m<=4;m++){ render_view(fb,lv,&cam,m,&dl); char fn[64]; snprintf(fn,sizeof fn,"frame%d.bmp",m-1);
            SDL_SaveBMP(fb,fn); printf("mode %d: ",m); if(m<4) report(&dl); else printf("top-down\n"); } }
        else if(spin){ for(int k=0;k<4;k++){ cam.ang=(angle_t)(MAP_START.ang+k*64); render_view(fb,lv,&cam,1,&dl);
            char fn[64]; snprintf(fn,sizeof fn,"frame%d.bmp",k); SDL_SaveBMP(fb,fn); printf("view %d: ",k); report(&dl); } }
        else if(getenv("TUNE")){ g_rom_prom=filesize("neogeo/build/rom/202-p1.p1"); g_rom_nodes=filesize("neogeo/build/rom/202-p2.p2"); g_rom_crom=filesize("neogeo/build/rom/202-c1.c1")+filesize("neogeo/build/rom/202-c2.c2"); render_world(lv,&cam,&dl); g_spr=dl.spr_total; g_line=dl.line_max; draw_dl_nodestyle(fb,&dl,&cam); draw_tuner_hud(fb); SDL_SaveBMP(fb,dump); printf("tuner-dump -> %s\n",dump); }
        else { render_view(fb,lv,&cam,1,&dl); if(SDL_SaveBMP(fb,dump)!=0) fprintf(stderr,"SaveBMP: %s\n",SDL_GetError()); report(&dl);
               printf("sector=%d hitscan=%d use_door=%d\n",point_sector(lv,cam.pos.x,cam.pos.y),
                      hitscan(lv,&cam), use_door(lv,&cam)); }
        SDL_FreeSurface(fb); SDL_Quit(); return 0;
    }

    const int S=3; SDL_Window *win=SDL_CreateWindow("doomng",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,SCREEN_W*S,SCREEN_H*S,0);
    SDL_Surface *ws=SDL_GetWindowSurface(win);
    int run=1, mode=getenv("TUNE")?5:1, weap=1, flymode=0; Uint32 last=0;   /* TUNE=1 -> start in the node-render tuner */
    if(mode==5){ printf("== NODE-RENDER TUNER ==  TAB=select param, -/= adjust; 1-4=BSP views; WASD/JL/IK to move/look\n"); tuner_print(); }
    g_rom_prom=filesize("neogeo/build/rom/202-p1.p1"); g_rom_nodes=filesize("neogeo/build/rom/202-p2.p2"); g_rom_crom=filesize("neogeo/build/rom/202-c1.c1")+filesize("neogeo/build/rom/202-c2.c2");
    float base=MAP_START.z, bob=0, vz=0;                       /* base height + jump bob */
    while(run){
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_QUIT) run=0;
            else if(e.type==SDL_KEYDOWN) switch(e.key.keysym.sym){
                case SDLK_ESCAPE: run=0; break;
                case SDLK_1: mode=1; break; case SDLK_2: mode=2; break;
                case SDLK_3: mode=3; break; case SDLK_4: mode=4; break;
                case SDLK_5: mode=5; printf("== NODE-RENDER TUNER ==  TAB=select param, -/= adjust\n"); tuner_print(); break;
                case SDLK_TAB: case SDLK_DOWN: if(mode==5){ tune_sel=(tune_sel+1)%NTUNE; tuner_print(); } break;
                case SDLK_UP: if(mode==5){ tune_sel=(tune_sel+NTUNE-1)%NTUNE; tuner_print(); } break;
                case SDLK_SPACE: if(bob<=0){ vz=7.0f; } break;
                case SDLK_f: flymode=!flymode; printf("%s mode\n",flymode?"FLY":"WALK"); break;
                case SDLK_x: weap=(weap+1)%7; printf("weapon: %s\n",WEAPONS[weap]); break;
                case SDLK_z: { int hit=hitscan(lv,&cam); printf("** FIRE %s **",WEAPONS[weap]);
                    if(hit>=0){ thing_kill(hit); printf(" -- target down!"); }
                    if(weap==6) printf(" (BFG ftw)"); printf("\n"); } break;
                case SDLK_u: { int s=use_door(lv,&cam);
                    if(s>=0) printf("** door opening (sector %d) **\n",s); else printf("** nothing to use **\n"); } break;
                default: break;
            }
        }
        const Uint8 *ks=SDL_GetKeyboardState(NULL);
        float fx=lut_cos(cam.ang), fy=lut_sin(cam.ang), spd=8.0f, mvx=0, mvy=0;
        if(ks[SDL_SCANCODE_W]){ mvx+=fx*spd; mvy+=fy*spd; }
        if(ks[SDL_SCANCODE_S]){ mvx-=fx*spd; mvy-=fy*spd; }
        if(ks[SDL_SCANCODE_A]){ mvx-=fy*spd; mvy+=fx*spd; }
        if(ks[SDL_SCANCODE_D]){ mvx+=fy*spd; mvy-=fx*spd; }
        if(flymode){ cam.pos.x+=mvx; cam.pos.y+=mvy; if(ks[SDL_SCANCODE_Q]) base+=6; if(ks[SDL_SCANCODE_E]) base-=6; }
        else move_player(lv,&cam,mvx,mvy);                    /* collision + step physics */
        if(ks[SDL_SCANCODE_J]||ks[SDL_SCANCODE_LEFT])  cam.ang+=3;
        if(ks[SDL_SCANCODE_L]||ks[SDL_SCANCODE_RIGHT]) cam.ang-=3;
        if(ks[SDL_SCANCODE_I]) cam.pitch+=4; if(ks[SDL_SCANCODE_K]) cam.pitch-=4;
        if(cam.pitch>90) cam.pitch=90; if(cam.pitch<-90) cam.pitch=-90;
        if(ks[SDL_SCANCODE_LEFTBRACKET])  PCFG.fov-=3;  if(ks[SDL_SCANCODE_RIGHTBRACKET]) PCFG.fov+=3;
        if(PCFG.fov<60) PCFG.fov=60; if(PCFG.fov>400) PCFG.fov=400;
        if(ks[SDL_SCANCODE_SEMICOLON])  PCFG.fisheye-=0.03f; if(ks[SDL_SCANCODE_APOSTROPHE]) PCFG.fisheye+=0.03f;
        if(PCFG.fisheye<-1) PCFG.fisheye=-1; if(PCFG.fisheye>1) PCFG.fisheye=1;
        if(ks[SDL_SCANCODE_COMMA])  PCFG.gamma-=0.03f; if(ks[SDL_SCANCODE_PERIOD]) PCFG.gamma+=0.03f;
        if(PCFG.gamma<0.3f) PCFG.gamma=0.3f; if(PCFG.gamma>2.0f) PCFG.gamma=2.0f;
        if(mode==5){ static int tcd=0; if(tcd>0)tcd--;                       /* hold -/= to adjust the selected slider (throttled) */
            int dn=ks[SDL_SCANCODE_MINUS]||ks[SDL_SCANCODE_KP_MINUS], up=ks[SDL_SCANCODE_EQUALS]||ks[SDL_SCANCODE_KP_PLUS];
            if((dn||up)&&tcd==0){ Tunable*p=&TUNE[tune_sel]; p->val+=up?p->step:-p->step;
                if(p->val<p->mn)p->val=p->mn; if(p->val>p->mx)p->val=p->mx; tcd=4; tuner_print(); } }

        bob+=vz; vz-=0.5f; if(bob<0){ bob=0; vz=0; }           /* jump arc */
        cam.z = flymode ? base+bob : lv->sectors[cam.sector].floor+41.0f+bob;   /* walk = floor-follow */

        world_update(lv);                                   /* animate doors */
        if(mode==5){ camera_t scam=cam;
            if((int)TV(T_SNAPON)){ int NA=(int)TV(T_SNAPANG); if(NA<1)NA=1;   /* feel the on-rails pop: snap angle to N + position to the node grid */
                int ai=((int)cam.ang*NA+128)>>8; ai=((ai%NA)+NA)%NA; scam.ang=(angle_t)(ai*256/NA);
                float st=TV(T_SNAPSTEP); if(st<1)st=1;
                scam.pos.x=(float)((int)(cam.pos.x>=0?cam.pos.x/st+0.5f:cam.pos.x/st-0.5f))*st;
                scam.pos.y=(float)((int)(cam.pos.y>=0?cam.pos.y/st+0.5f:cam.pos.y/st-0.5f))*st;
                int ss=point_sector(lv,scam.pos.x,scam.pos.y); if(ss>=0){ scam.sector=ss; scam.z=lv->sectors[ss].floor+41.0f; } }
            PCFG.fov=TV(T_FOV);                              /* live FOV preview (bake stays 160=90deg) */
            render_world(lv,&scam,&dl); g_spr=dl.spr_total; g_line=dl.line_max; draw_dl_nodestyle(fb,&dl,&scam); draw_tuner_hud(fb); }   /* live cart-style node-render + slider HUD */
        else render_view(fb,lv,&cam,mode,&dl);
        if(mode!=4){ Uint32 cc=SDL_MapRGB(fb->format,255,255,255);   /* crosshair */
            for(int o=2;o<=4;o++){ putpx(fb,HALF_W+o,HALF_H,cc); putpx(fb,HALF_W-o,HALF_H,cc); putpx(fb,HALF_W,HALF_H+o,cc); putpx(fb,HALF_W,HALF_H-o,cc); } }
        SDL_BlitScaled(fb,NULL,ws,NULL); SDL_UpdateWindowSurface(win);
        { static Uint32 lf=0; Uint32 nf=SDL_GetTicks(); if(lf&&nf>lf) g_fps=g_fps*0.85f+(1000.0f/(nf-lf))*0.15f; lf=nf; }   /* smoothed fps */
        Uint32 now=SDL_GetTicks(); if(now-last>500){ last=now;
            printf("mode=%d pos(%.0f,%.0f) ang=%u z=%.0f pitch=%.0f fov=%.0f fish=%.2f gam=%.2f | ",
                mode,cam.pos.x,cam.pos.y,cam.ang,cam.z,cam.pitch,PCFG.fov,PCFG.fisheye,PCFG.gamma);
            if(mode==4) printf("top-down\n"); else report(&dl); }
        SDL_Delay(16);
    }
    SDL_DestroyWindow(win); SDL_FreeSurface(fb); SDL_Quit(); return 0;
}
