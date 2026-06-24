/* DOOM-NG -- Neo Geo backend, NODE-RENDER runtime (the 60fps path).
 *
 * The BSP renderer is too heavy for the FPU-less 68000 to run per-frame. So the
 * level is PRE-RENDERED off-line: flood-fill the walkable floor into a 120-unit
 * grid, render each (cell x 12 angles) viewpoint on the host, and store the
 * emit-ready sprite records in P-ROM (neogeo/nodes_data.h).
 *
 * At runtime there is NO 3D math: snap the camera to the nearest grid node + angle,
 * look up that view's records, and blit them as hardware-scaled sprites. That's
 * all a normal Neo Geo game does -- push pre-made sprites -- so it runs at 60fps.
 *
 * This is the spawn-cluster proof-of-concept: a 7x7 sub-grid (R=360u, 39 nodes,
 * 402 views, ~443KB) that fits the 1MB ROM1 with no bank-switching. */
#include <ngdevkit/neogeo.h>
#include <ngdevkit/ng-fix.h>
#include <ngdevkit/ng-video.h>
#include <ngdevkit/bank-switch.h>
#include <stdio.h>
#include "dng.h"
#include "textiles.h"
const int SKY_TEX = SKY_TEX_VAL;   /* live cart's sky texture id (from textiles.h); satisfies dng.h's extern now the on-rails map.c is delinked */
#include "floorlut.h"
#include "ceillut.h"
#include "sprites.h"
#include "ramps.h"
#include "hudfix.h"          /* RAMP_TILE0 + RAMP_OFF[tex][drop][edge]: texture-baked smooth-wall edge tiles */
#include "gunhand.h"         /* GUNHAND[] + GUNHAND_PAL16[]: the 6 first-person weapons as fix-layer tiles (gunbake.py) */
#include "fbsolid.h"         /* FB SPIKE: FB_SOLID_TILE_BASE + FB_PAL[]: 15 solid fix tiles + the 128-colour global palette */
#include "fogcm.h"           /* COLORMAP floor/ceiling fade bands (FOG_FLOOR/FOG_CEIL): DOOM light-diminish curve vs the linear scale */
#include "titlepic.h"       /* TITLE_TILE0 + TITLE_PAL16[][] + TITLE_MAP[][]: DOOM title screen tilemap */
#include "interpic.h"       /* INTER_TILE0 + INTER_PAL16[][] + INTER_MAP[][]: DOOM WIMAP0 intermission tilemap */
#include "menu.h"           /* MENU_*: DOOM new-game flow graphics (episode/skill names + skull cursor) */

/* sprites sit in C-ROM right after the floor LUT */
#define SPR_TILE0 (FLOORLUT_TILE0 + FLOORLUT_NA*FLOORLUT_NPHASE*FLOORLUT_ROWS*FLOORLUT_COLS)

#define PLACEHOLDER_TILE 256        /* BIOS owns 0..255; placeholder at 256 */
#define BLANK_TILE 64               /* an all-index-0 (fully transparent) tile in the reserved 0..255 C-ROM region; used to pad SCB1 slots the vshrink-expansion over-reads */
#define TEXBASE 16                  /* per-texture palettes start here (0-15: system/sky) */
#define FIRST_TEX_TILE 257          /* C-ROM: base 0-255, placeholder 256, real textures 257+ */
#define SHOW_DEBUG 1                 /* 1 = show the top-left frame/pos/node readout (for perf glitches); 0 = clean */
#define DNG_VERSION "DOOM-NG v66"   /* bump on each notable build; shown in the debug HUD + boot banner */
#define TITLE_FLOW  1
#define AUTOPILOT   0   /* dev: 1 = skip title implied off; hold W automatically (headless full-playthrough verification + profiling). SHIP = 0. */
#define MENU_CAP    0   /* DBG headless capture: 0=normal, 1=skip title -> stop on EPISODE, 2=skip title+episode -> stop on SKILL */
#define OPENING_SKY 0   /* opening-sky overlaid the interior ceiling -> OFF (gray ceiling). 1 re-enables (needs vertical-extent fix). */                /* 1 = TITLE -> LOADING -> game (ship flow); 0 = boot straight into the game (debug) */
#define CULL_MINW  2                 /* skip 1px wall slivers (the audit's floating bright hairlines in dark recesses). 2px+ strips all draw; depth does NOT scale the cull (long draw distance preserved). Revert to 1 to keep hairlines. */

extern u8 bios_p1current;           /* P1 controller state, updated by the BIOS */

static int g_nodebank=-1;
static int g_view_sky=0;
static int g_vbob=0;   /* HEAD BOB: vertical view offset folded into every sprite Y at emit time (quantized 2px so the view-cache still skips when standing) */
static angle_t g_binang=0;   /* angle of the view ON SCREEN (its 8-degree bin centre). Billboards and the sky
                                rotate with THIS, not the continuous cam.ang: smooth sprites against snapped
                                walls appear to swing independently -- the whole world snaps as ONE. */
static int g_ceildark=0;   /* this node's room uses ceiling LUT B (dark) -- from the baked PATHCEIL table */   /* HEAD BOB: vertical view offset folded into every sprite Y at emit time (quantized 2px so the view-cache still skips when standing) */    /* last emitted view contains sky records -> the view-cache must also key on the sky-scroll angle bin */   /* currently-mapped P2 bank for NODES. Cached: re-issuing P_ROM_SWITCH_BANK every frame makes gngeo re-map/flush -> the BK1 slideshow. emit_nodeview is the only switcher. */

static int g_cmfog=1;   /* (48) cmfg: floor/ceiling fade bank COLOURS via the DOOM COLORMAP curve+hue (1) vs the linear RGB scale (0). Default ON. The colmap-ceiling FADE itself is toggled by cmap(14); cmfg only chooses the curve of the fade banks. */
static int g_cmceil=1;  /* (14, was 'sky') COLMAP CEILING brightness: ON (default) = DIMMED ramp (CEIL_DIM/16, blends into murk); OFF = full bright. Both keep the BAKED depth-fade ramp + texture (vs_ceil_pal scales bank 12). */
static void vs_fog_bands(void){   /* ONLY the floor/ceiling distance-fade band slots (9/10/11/14/15). Re-callable standalone (the cmfg toggle) so it touches NOTHING else (the gun/HUD/LUT palettes are untouched). Reads slot 13 = the floor base. */
    if(g_cmfog){   /* COLORMAP curve+hue (baked, one diminish step at the band's darkness) */
        for(int i=1;i<16;i++){ MMAP_PALBANK1[11*16+i]=FOG_FLOOR[0][i]; MMAP_PALBANK1[10*16+i]=FOG_FLOOR[1][i]; MMAP_PALBANK1[14*16+i]=FOG_FLOOR[2][i];
            MMAP_PALBANK1[9*16+i]=FOG_CEIL[0][i]; MMAP_PALBANK1[15*16+i]=FOG_CEIL[1][i]; }
    } else
    for(int i=1;i<16;i++){ unsigned short c=MMAP_PALBANK1[13*16+i]; int r=(c>>8)&0xF, g=(c>>4)&0xF, b=c&0xF;
        MMAP_PALBANK1[11*16+i]=(unsigned short)((((r*15)>>4)<<8)|(((g*15)>>4)<<4)|((b*15)>>4));    /* floor band 1: 0.94x */
        MMAP_PALBANK1[10*16+i]=(unsigned short)((((r*13)>>4)<<8)|(((g*13)>>4)<<4)|((b*13)>>4));    /* floor band 2: 0.81x */
        MMAP_PALBANK1[14*16+i]=(unsigned short)((((r*11)>>4)<<8)|(((g*11)>>4)<<4)|((b*11)>>4));    /* band 3 (DEEP, shared): 0.69x */
        unsigned short cc=CEILLUT_PAL16[i]; int cr=(cc>>8)&0xF, cg=(cc>>4)&0xF, cb=cc&0xF;
        MMAP_PALBANK1[9*16+i]=(unsigned short)((((cr*15)>>4)<<8)|(((cg*15)>>4)<<4)|((cb*15)>>4));  /* ceiling band 1: 0.94x */
        MMAP_PALBANK1[15*16+i]=(unsigned short)((((cr*13)>>4)<<8)|(((cg*13)>>4)<<4)|((cb*13)>>4)); }   /* ceiling band 2: 0.81x */
    MMAP_PALBANK1[11*16]=0x8000; MMAP_PALBANK1[10*16]=0x8000; MMAP_PALBANK1[9*16]=0x8000;
}
static void init_palettes(void){
    MMAP_PALBANK1[1*16]=0x8000; for(int i=1;i<16;i++) MMAP_PALBANK1[1*16+i]=0x7FFF;   /* fix palette 1 = WHITE (idx0 transparent) -> the debug HUD readout */
    for(int i=0;i<16;i++){ MMAP_PALBANK1[2*16+i]=HUDFIX_PAL2[i]; MMAP_PALBANK1[3*16+i]=HUDFIX_PAL3[i];
        MMAP_PALBANK1[4*16+i]=HUDFIX_PAL4[i]; MMAP_PALBANK1[5*16+i]=HUDFIX_PAL5[i]; MMAP_PALBANK1[6*16+i]=HUDFIX_PAL6[i]; }   /* fix palettes 2-5: the status bar (HUD lives on the FIX LAYER now -- ~30 sprites freed for world records) */
    MMAP_PALBANK1[255*16+15]=0x0111;   /* hardware backdrop (0x401FFE) -> near-black murk: far walls + horizon gaps recede into it */
    MMAP_PALBANK1[15*16]=0x8000; MMAP_PALBANK1[14*16]=0x8000;     /* slots 14/15 REPURPOSED (were unused Doom gray fallbacks): the 3rd (deep) murk band -- 14=shared deepest, 15=ceiling mid. Colours filled in the murk loop below. */
    MMAP_PALBANK1[13*16]=FLOORLUT_PAL16[0];                        /* floor LUT palette, brightened toward the OG mid-grey */
    for(int i=1;i<16;i++){ unsigned short c=FLOORLUT_PAL16[i];
        int r=(c>>8)&0xF, g=(c>>4)&0xF, b=c&0xF;
        r=(r*11)>>4; g=(g*11)>>4; b=(b*11)>>4;                     /* ~0.69x: uniform DARKER floor (the chosen shade), one palette = no banding */
        if(r>15)r=15; if(g>15)g=15; if(b>15)b=15;
        MMAP_PALBANK1[13*16+i]=(unsigned short)((r<<8)|(g<<4)|b); }
    MMAP_PALBANK1[12*16]=0x8000;                                    /* ceiling LUT A: the grey-tech ceiltex palette */
    for(int i=1;i<16;i++) MMAP_PALBANK1[12*16+i]=CEILLUT_PAL16[i];
    MMAP_PALBANK1[7*16]=0x8000;                                     /* ceiling LUT B: the dark-room ceiltex palette */
    for(int i=1;i<16;i++) MMAP_PALBANK1[7*16+i]=CEILLUT2_PAL16[i];
    /* ceiling LUT C (spotted lamp panel): slots 8/9/10 = white/red/amber. Base colours 1..11 are
       shared; only the reserved SPOT entries 12..15 are recoloured -- one tile set, three room
       moods (TLITE6_5 white, TLITE6_4 red, TLITE6_6 amber). Slots 8-10 were the retired fog ramps. */
    for(int sl=0;sl<3;sl++){
        MMAP_PALBANK1[(8+sl)*16]=0x8000;
        for(int i=1;i<16;i++){
            unsigned short c=CEILLUT3_PAL16[i];
            if(i>=12){ int r=(c>>8)&0xF, g=(c>>4)&0xF, b=c&0xF, lum=r; if(g>lum)lum=g; if(b>lum)lum=b;
                if(sl==1){ r=lum; g=lum>>2; b=lum>>2; }                  /* red spots */
                else if(sl==2){ r=lum; g=(lum*3)>>2; b=lum>>3; }         /* amber spots */
                c=(unsigned short)((r<<8)|(g<<4)|b); }
            MMAP_PALBANK1[(8+sl)*16+i]=c; } }
    /* DISTANT MURK (subtle): the farthest two floor rows + the grey ceiling's horizon row ease
       toward darkness. Gentle multipliers of the BASE shades -- a fade, not the old harsh fog
       stripes. Floor: 13 near / 11 (0.90x) / 10 (0.78x) far. Grey ceiling: 12 near / 9 (0.80x). */
    vs_fog_bands();   /* floor/ceiling distance-fade bands (colormap or linear, per g_cmfog) -> slots 9/10/11/14/15 */
    /* texture palettes (3 distance-shade fog bands) moved to vs_upload_tex_pals(): with the all-E1
       C-ROM (NTEXTILE=173) a global-tile-id upload overflows the 256 HW palette slots, so they are
       now uploaded PER MAP into COMPACTED slots at VS init / map toggle. init_palettes keeps only the
       system/LUT/sprite slots (which the title + menu flow also need). */
    /* demo sprites: pistol(244) imp(245) stbar(246) flash(247) face(248) hudnum(249). Index 0 = transparent. */
    for(int i=0;i<16;i++){
        MMAP_PALBANK1[SPR_PISTOL_PAL*16+i]=SPR_PISTOL_PAL16[i];
        MMAP_PALBANK1[SPR_IMP_PAL*16+i]   =SPR_IMP_PAL16[i];
        MMAP_PALBANK1[SPR_STBAR_PAL*16+i] =SPR_STBAR_PAL16[i];
        MMAP_PALBANK1[SPR_HEDGER_PAL*16+i]=SPR_HEDGER_PAL16[i];   /* full-width HUD right-edge sprite (slot 254) */
        MMAP_PALBANK1[SPR_FLASH_PAL*16+i] =SPR_FLASH_PAL16[i];
        MMAP_PALBANK1[SPR_FACE_PAL*16+i]  =SPR_FACE_PAL16[i];
        MMAP_PALBANK1[SPR_HUDNUM_PAL*16+i]=SPR_HUDNUM_PAL16[i];
        MMAP_PALBANK1[SPR_POSS_PAL*16+i]  =SPR_POSS_PAL16[i];    /* the grunts/barrels/explosion palettes were NEVER uploaded -> they rendered BLACK */
        MMAP_PALBANK1[SPR_SPOS_PAL*16+i]  =SPR_SPOS_PAL16[i];
        MMAP_PALBANK1[SPR_BAR_PAL*16+i]   =SPR_BAR_PAL16[i];
        MMAP_PALBANK1[SPR_BEXPC_PAL*16+i] =SPR_BEXPC_PAL16[i];
    }
}

static int clampI(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static int isqrtI(int v){ if(v<=0)return 0; int x=v,y=(x+1)>>1; while(y<x){x=y;y=(x+v/x)>>1;} return x; }   /* integer sqrt (Newton) for the perspective flat split */

/* Custom vblank handler (overrides ngdevkit's rom_handler_VBlank_default). The default jsr's the
   BIOS SYSTEM_IO every interrupt; nullbios's credit/forced-start logic in there re-enters the
   cart's start vector seconds into gameplay (the deterministic "crash at spawn" reboot loop) and
   its FIX writes can race the VRAM sequences. None of its services are needed after boot: ack the
   IRQ, kick the watchdog, run ngdevkit's callback (the ng_wait_vblank counter) -- and never call
   the BIOS again. */
static volatile unsigned short g_vbl=0;
extern void rom_callback_VBlank(void);
/* VBLANK-DRIVEN FLOOR ("interstitial frames"): the floor phase ticks inside the vblank IRQ,
   decoupled from the main loop -- in heavy rooms the geometry updates at 20-30Hz but the floor
   keeps flowing every refresh the main loop isn't mid-VRAM-write. Design intent: a smoother
   baseline with some jumping geometry is preferred over jumping everything. g_vrambusy brackets every
   main-loop VRAM writer (the VRAM address register is write-only -- an IRQ write mid-sequence
   would corrupt the cursor), so the IRQ only touches VRAM in provably idle windows. */
static volatile int g_vrambusy=1;       /* 1 while the main loop owns a VRAM sequence */
__attribute__((interrupt_handler)) void rom_handler_VBlank(void){
    *REG_IRQACK = 4;                                   /* acknowledge the vblank interrupt */
    *((volatile u8*)0x300001) = 1;                     /* kick the hardware watchdog */
    g_vbl++;                                           /* free-running vblank tick: measures how many refreshes an emit spanned */
    rom_callback_VBlank();                             /* ngdevkit's per-frame counter (ng_wait_vblank) */
}

/* Floor/ceiling kept UNIFORM (no per-row distance fog) -- the fog lives on the WALLS (far-shade
   palettes) + the near-black backdrop, rather than fogging the planes underfoot. */
static const unsigned char FLOORPAL[7]={10,11,13,13,13,13,13};   /* DISTANT MURK, subtle: the two farthest floor rows ease into darkness (10=0.78x, 11=0.90x of the floor shade) -- a gradient, not stripes; rows 2+ stay uniform */
/* DISTANCE FOG -- keyed on DEPTH, not on-screen height. (Height-keyed left tall far walls bright,
   so there was no consistent gradient.) dep = depth/4 (the wall record byte). <=FOGD0 clean (L0);
   FOGD0..FOGD1 = L1 (~0.5); beyond FOGD1 = L2 (~0.25, fading into the 0x0111 murk). So EVERYTHING
   at a given distance darkens the same, tall or short -> a real depth gradient. */
#define FOGD0 56    /* dep -> world depth ~224: murk introduced NEARER (nearer field kept as subtle as possible) -- the L1 step is gentle (0.81) so the onset reads as atmosphere, not a dim wall */
#define FOGD1 135   /* ~world depth 540: middle ground -- the 480/0.375 combo crushed mid-distance walls to black blobs (visually degraded) */
static const unsigned char CEILPAL5[5]={12,7,8,8,8};   /* per-room ceiling palette: A grey, B dark, C spotted (red/amber rooms ride the white spots -- their slots went to murk; the red ROOM character comes from the dome records) */
#define CEILPAL_NOW (CEILPAL5[g_ceildark])
#define CEILROWPAL(r,re) (((r)==(re)-1) ? 9 : CEILPAL_NOW)   /* ceiling murk matches the floor: every room type eases its horizon row into the fade (dark rooms previously never eased). ONE row -- two pulled the murk line visibly too close. */
#ifndef VSLICE
#define VSLICE 0   /* 1 = VERTICAL SLICE: live raycaster -> wall sprite-strips, free movement in one test room. Measures the Super-Scaler-DOOM referencing/composition perf (the only open question). Build via `gmake run-vs`. */
#endif
#ifndef VS_PLANES
#define VS_PLANES 1   /* slice: also fill floor/ceiling wedges (cost probe). 0 = walls only (the A/B). */
#endif
/* (was: #if VSLICE -> TITLE_FLOW 0, which skipped the title/menu and booted straight into the live
   game. RESTORED -- the title -> episode -> skill flow already leads into the VSLICE game below, and
   VS_DIAG capture still bypasses it via its own #ifdef guards.) */
static void park_all_sprites(void){
    *REG_VRAMMOD=0x200;
    for(int s=0;s<381;s++){ *REG_VRAMADDR=ADDR_SCB2+s; *REG_VRAMRW=0x0001; *REG_VRAMRW=0; *REG_VRAMRW=0; }
}

#define SND(n) (*((volatile unsigned char*)0x320000)=(unsigned char)(n))   /* sound-code latch: 1=pistol/menu, 2=scream(legacy), 3=music, 4=boom, 5=itemup, 6=door OPEN, 7=imp death, 8=possessed death, 9=door CLOSE, 10=lift START, 11=lift STOP, 0x10+map=track */
#if !MENU_HAVE
static const char *const EPISODES[]={ "EPISODE 1", "EPISODE 2 (LOCKED)", "EPISODE 3 (LOCKED)" };   /* text-mode fallback only (real menu = the WAD's own graphics when menu.h is present); kept id-free */
static const char *const SKILLS[]  ={ "SKILL 1", "SKILL 2", "SKILL 3", "SKILL 4", "SKILL 5" };

/* DOOM-style vertical menu: > cursor, pistol blip on move/select. Returns the chosen index. Rows
   0..firstlock-1 are selectable; rows below show (e.g. LOCKED episodes) but START ignores them.
   Text fallback -- only built when the WAD menu graphics (menu.h) are absent. */
static int menu_select(const char *title, const char *const *opts, int n, int firstlock, int defsel){
    park_all_sprites(); ng_cls();
    ng_center_text(4,0,title);
    ng_center_text(24,0,"UP/DOWN  CHOOSE      START  OK");
    ng_center_text(26,0,DNG_VERSION);
    int sel=defsel; u8 pj=0xff, pst=0xff;
    for(;;){ ng_wait_vblank();
        u8 jp=(u8)~(*REG_P1CNT), st=(u8)~(*REG_STATUS_B);
        if((jp&CNT_UP)  &&!(pj&CNT_UP)   && sel>0)   { sel--; SND(1); }
        if((jp&CNT_DOWN)&&!(pj&CNT_DOWN) && sel<n-1) { sel++; SND(1); }
        int go=((st&CNT_START1)&&!(pst&CNT_START1)) || ((jp&CNT_A)&&!(pj&CNT_A));
        pj=jp; pst=st;
        for(int i=0;i<n;i++){ ng_text(5,8+i*2,0,(i==sel)?">":" "); ng_text(7,8+i*2,0,opts[i]); }
        if(go && sel<firstlock){ SND(1); return sel; }
    }
}
#endif

#if MENU_HAVE
/* ===== Authentic DOOM new-game flow: episode select then skill select, using the real WAD menu
   graphics (M_EPISOD, M_EPIx, M_NEWG, M_SKILL, skill names) + animated skull cursor (M_SKULL1/2),
   drawn over black. DOOM's 320x200 menu coords, shifted +12y to centre in the 224 viewport. ===== */
#define OYM 12
/* draw one menu lump's tilemap at screen (X,Y) as 16px column-sprites using palette slot `slot` */
static int draw_lump(int s,int lump,int X,int Y,int slot){
    int C=MENU_COLS[lump], R=MENU_ROWS[lump]; unsigned int base=MENU_TILE0+MENU_OFF[lump];
    int cyf=(496-Y)&0x1FF;
    for(int tx=0;tx<C && s<380;tx++){
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+s*64;
        for(int ty=0;ty<R;ty++){ unsigned int T=base+ty*C+tx;
            *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((slot<<8)|(((T>>16)&0xF)<<4)); }
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+s;
        *REG_VRAMRW=(u16)((15<<8)|255);                  /* full 16px col, no shrink */
        *REG_VRAMRW=(u16)((cyf<<7)|R);
        *REG_VRAMRW=(u16)(((X+tx*16)&0x1FF)<<7);
        s++;
    }
    return s;
}
static void load_lump_pal(int lump,int slot){ for(int i=0;i<16;i++) MMAP_PALBANK1[slot*16+i]=MENU_PAL16[lump][i]; }
/* a DOOM menu page: h1 = lower header (y38), h2 = upper header (y14, -1=none), `items` selectable
   rows (y63+16i). Only rows < lock proceed (locked ones blip). Returns the chosen index. */
static int doom_menu(int h1,int h1x,int h2,int h2x,const int*items,int n,int lock,int defsel){
    if(h1>=0) load_lump_pal(h1,8);
    if(h2>=0) load_lump_pal(h2,9);
    for(int i=0;i<n;i++) load_lump_pal(items[i],10+i);
    load_lump_pal(ML_SKULL1,10+n); load_lump_pal(ML_SKULL2,11+n);
    park_all_sprites(); ng_cls();
    int sel=defsel; u8 pj=0xff,pst=0xff; u16 t=0;
    for(;;){
        ng_wait_vblank();
        u8 jp=(u8)~(*REG_P1CNT), st=(u8)~(*REG_STATUS_B);
        if((jp&CNT_UP)  &&!(pj&CNT_UP))   { sel=(sel>0)?sel-1:n-1; SND(1); }
        if((jp&CNT_DOWN)&&!(pj&CNT_DOWN)) { sel=(sel<n-1)?sel+1:0; SND(1); }
        int go=((st&CNT_START1)&&!(pst&CNT_START1)) || ((jp&CNT_A)&&!(pj&CNT_A));
        pj=jp; pst=st;
        int s=1;
        if(h2>=0) s=draw_lump(s,h2,h2x,14+OYM,9);
        if(h1>=0) s=draw_lump(s,h1,h1x,38+OYM,8);
        for(int i=0;i<n;i++) s=draw_lump(s,items[i],48,63+i*16+OYM,10+i);
        int blink=(t>>3)&1;                               /* skull blinks every 8 frames, like DOOM */
        s=draw_lump(s, blink?ML_SKULL2:ML_SKULL1, 16, 58+sel*16+OYM, blink?(11+n):(10+n));
        t++;
        if(go){ if(sel<lock){ SND(1); return sel; } else SND(1); }   /* locked episode -> blip, stay */
    }
}
#endif

#if VSLICE
/* ===== VERTICAL SLICE: live raycaster -> wall sprite-strips (Super-Scaler DOOM probe) =====
   Per column: cast a ray into a hardcoded test room, find the nearest wall segment, project
   to a screen height, emit ONE sprite-strip referencing the strip engine's texture tiles
   (vshrink does the depth scaling -- the whole point). The 68000 does only visibility; the
   sprite hardware draws. The measurement is how many of these passes fit in a second (MAXFPS). */
static const short VS_SIN[256]={
  0,6,13,19,25,31,38,44,50,56,62,68,74,80,86,92,
  98,104,109,115,121,126,132,137,142,147,152,157,162,167,172,177,
  181,185,190,194,198,202,206,209,213,216,220,223,226,229,231,234,
  237,239,241,243,245,247,248,250,251,252,253,254,255,255,256,256,
  256,256,256,255,255,254,253,252,251,250,248,247,245,243,241,239,
  237,234,231,229,226,223,220,216,213,209,206,202,198,194,190,185,
  181,177,172,167,162,157,152,147,142,137,132,126,121,115,109,104,
  98,92,86,80,74,68,62,56,50,44,38,31,25,19,13,6,
  0,-6,-13,-19,-25,-31,-38,-44,-50,-56,-62,-68,-74,-80,-86,-92,
  -98,-104,-109,-115,-121,-126,-132,-137,-142,-147,-152,-157,-162,-167,-172,-177,
  -181,-185,-190,-194,-198,-202,-206,-209,-213,-216,-220,-223,-226,-229,-231,-234,
  -237,-239,-241,-243,-245,-247,-248,-250,-251,-252,-253,-254,-255,-255,-256,-256,
  -256,-256,-256,-255,-255,-254,-253,-252,-251,-250,-248,-247,-245,-243,-241,-239,
  -237,-234,-231,-229,-226,-223,-220,-216,-213,-209,-206,-202,-198,-194,-190,-185,
  -181,-177,-172,-167,-162,-157,-152,-147,-142,-137,-132,-126,-121,-115,-109,-104,
  -98,-92,-86,-80,-74,-68,-62,-56,-50,-44,-38,-31,-25,-19,-13,-6,
};
#define VS_SN(a) VS_SIN[(a)&255]
#define VS_CS(a) VS_SIN[((a)+64)&255]
#include "vs_e1.h"               /* ALL of Episode 1: per-map BSP geometry as A1 pointer tables (VE*_MAP[VE_NMAP]) */
/* MAP TOGGLE: file-scope geometry pointers, reseated per map by vs_set_map(). The hot path (vs_render_seg /
   vs_floor_at / vs_move_ok) reads through these -> one indirection, same cost as the old fixed arrays; the
   per-map array-of-arrays index is paid ONCE per toggle, never per seg. */
static const short *ve_x0,*ve_y0,*ve_x1,*ve_y1,*ve_ff,*ve_fc,*ve_bf,*ve_bc,*ve_mt,*ve_ut,*ve_lt,*ve_nx,*ve_ny,*ve_ndx,*ve_ndy,*ve_nrb,*ve_nlb;
static const unsigned char  *ve_flag,*ve_ffl,*ve_cfl,*ve_bfl,*ve_bcl;   /* per-seg flag + FRONT + BACK floor/ceil flat slot (real flats; BACK = the zone seen through a two-sided opening, 0xFF=none) */
static const unsigned short *ve_ssc,*ve_ssf,*ve_nr,*ve_nl,*ve_fsec,*ve_bsec;   /* per-seg FRONT/BACK sector ids -> the door ceiling override */
static const unsigned short *ve_usesec;   /* per-seg LIFT target sector (tag-resolved; 0xFFFF=none) */
static const short *ve_uselo;             /* per-seg LIFT drop = low-high (<=0); the lift lowers to here, then raises back */
static const unsigned char  *ve_u0;       /* per-seg WALL-U at vertex v1 (texture px, mod 256) -- perspective texture mapping */
static const unsigned short *ve_ulen;     /* per-seg U span = seg world length (texture px) */
static const short          *ve_yoff;     /* per-seg FRONT sidedef rowoffset (px, signed) -- vertical texture peg (V companion 2b) */
static const unsigned char  *ve_peg;      /* per-seg peg flags: bit0=DONTPEGTOP, bit1=DONTPEGBOTTOM (V companion 2b) */
static int ve_nseg, ve_root, ve_sx, ve_sy, ve_sa, g_map=0, g_nsec=0;
static const unsigned char MUSIC[36]={   /* per-map ADPCM-B track slot (SND cmd 0x10+slot). E1M1-9 -> 0-8, intro -> 9, E2M1-9 -> 10-18, E3M1-9 -> 19-27. E4 has no original music -> REUSES E1-E3 per Ultimate Doom's spmus[] (G_DoLoadLevel). */
    0,1,2,3,4,5,6,7,8,            /* E1M1-9 */
    10,11,12,13,14,15,16,17,18,   /* E2M1-9 */
    19,20,21,22,23,24,25,26,27,   /* E3M1-9 */
    22,20,21,4,16,13,15,14,8 };   /* E4M1-9 -> E3M4,E3M2,E3M3,E1M5,E2M7,E2M4,E2M6,E2M5,E1M9 */
/* DOORS: per-sector ceiling RAISE (world units) added to ve_fc/ve_bc in vs_render_seg. 0 = closed/baked.
   A use-raycast (CNT_D) picks a door seg, opens its BACK sector's delta toward the room ceiling, holds, auto-closes. */
static short g_secdc[VE_MAXSEC];
static int g_doorsec=-1, g_doorprog=0, g_doortgt=0, g_doorstate=0, g_doorhold=0, g_doorstay=0;   /* active door: sector, progress, target raise, state(0 idle/1 opening/2 hold/3 closing), hold timer; g_doorstay=1 -> tagged TRIGGER door: opens then frees the machine + stays open (so the level progresses) */
/* LIFTS: per-sector FLOOR delta (mirrors g_secdc for ceilings). USE (CNT_D) on a lift seg (fl&16) LOWERS its
   TAG-resolved target sector to the lowest adjacent floor (DOOM lower-lift), then a second USE raises it back.
   g_secdf goes NEGATIVE while lowered. EXIT segs (fl&32) advance the map. */
static short g_secdf[VE_MAXSEC];
static int g_liftsec=-1, g_liftprog=0, g_lifttgt=0, g_liftstate=0;   /* active lift: sector, progress(<=0), drop target, state(0 rest/up,1 lowering,2 down,3 raising) */
/* WALK-TRIGGER doors: segs flagged ve_flag&64 open their TAG-resolved sector when the player's move crosses them.
   Switch-trigger doors (ve_flag&128) open on USE. Both reuse ve_usesec/ve_uselo (the lift tag system) + the door
   state machine in open-stay mode. g_walktrig caches the walk-trigger seg indices for a cheap per-frame test. */
static unsigned short g_walktrig[64]; static int g_nwalktrig=0;
static const short *ve_thx,*ve_thy,*ve_thz;   /* per-map THING tables: world x/y, floor z */
static const unsigned char *ve_tha,*ve_thc;   /* thing angle (0..255), class (CLS_*) */
static int ve_nth;                              /* thing count for the current map */
static int vs_camx, vs_camy, vs_camang;   /* current map's spawn pose (re-seeded into the render loop on toggle) */
#include "vsfloor.h"             /* live-engine HEX floor LUT (21 sets / 60deg fold, chain-end tiles) */
#include "vsceil.h"              /* live-engine grey ceiling LUT (32 sets / 90deg fold) */
#define CEIL_DIM 8   /* (cmap ON) ceiling ramp brightness, /16. Lower = darker, blends harder into the murk (2026-06-23: previously too light end to end). 16 = full bright (= cmap OFF). */
static unsigned short ng_dim(unsigned short c,int num){   /* scale an NG16 colour's brightness by num/16 (decode 5-bit r/g/b: hi-nibble<<1 | LSB-bit 12..14, scale, re-pack) */
    int r=(((c>>8)&0xF)<<1)|((c>>14)&1), g=(((c>>4)&0xF)<<1)|((c>>13)&1), b=((c&0xF)<<1)|((c>>12)&1);
    r=r*num/16; g=g*num/16; b=b*num/16;
    return (unsigned short)(((r&1)<<14)|((g&1)<<13)|((b&1)<<12)|((r>>1)<<8)|((g>>1)<<4)|(b>>1));
}
static void vs_ceil_pal(void){   /* INC2: bank 12 = the baked vsceil depth-fade RAMP, DIMMED for the murk blend. cmap ON (default) = CEIL_DIM/16 brightness (darker, blends into murk); cmap OFF = full bright -> a useful A/B (both keep the texture + depth fade, unlike the old solid-grey OFF). */
    MMAP_PALBANK1[12*16]=0x8000;
    int num=g_cmceil?CEIL_DIM:16;
    for(int i=1;i<16;i++) MMAP_PALBANK1[12*16+i]=ng_dim(VSCEIL_PAL16[i],num);
}
static void vs_floor_pal(void){  /* INC2 FLOOR: bank 13 = the baked vsfloor ramp at FULL brightness (NO runtime dim). The floor's depth gradient is BAKED screen-linear (FRONT lighter -> BACK darker, 2026-06-23); a uniform runtime dim flattened it (dimmed the bright near). So the front reads as cmap-OFF bright, the baked far as cmap-ON dim. cmap dims only the ceiling now. */
    MMAP_PALBANK1[13*16]=0x8000;
    for(int i=1;i<16;i++) MMAP_PALBANK1[13*16+i]=VSFLOOR_PAL16[i];
}
#include "vsflat.h"              /* per-flat perspective LUT bank: real DOOM floor/ceiling per sector */
#define VS_FLATS 1               /* 1 = real per-flat floor/ceiling; 0 = synthetic hex/grey LUT (instant revert) */
#define VS_SKYWIN 0              /* 1 = WINDOW-SKY (front OR back F_SKY1) -- TESTED Jun14: produced garbage/bleed, the other fixes did NOT brute-force it (needs the parked 2-span). 0 = front-sky only (a42eec2 anti-bleed). */
#define VS_EYE  41
#define VS_NCOL 20
#define VS_NCOL_MAX 160  /* widest column-res mode. Per-column arrays sized to this; live count = g_ncol. Raised 80->160 for the OVER-SPEC col160 mode (2px columns -> >96 sprites/scanline = the HW per-line ceiling; the over-spec showcase). RAM is roomy post-excision. */
#define VS_HOR  112      /* match the baked FLOORLUT/CEILLUT horizon (y112) so walls meet the LUT floor/ceiling */
#define VS_LBT  0        /* FULL-RES band 0..192 (192px); walls + LUT floor/ceiling clip to it. Horizon stays y112 (LUT-baked): ceiling 7 rows y0..112, floor 5 rows y112..192; y192..224 = HUD. (was letterbox 16..176) */
#define VS_LBB  192
#define VS_HALF 160
#define VS_FOCAL 160   /* 90deg HFOV (2*atan(160/160)); MATCHES the floor/ceiling LUT bake (host PCFG.fov=160) -- walls were drifted to 178 (~84deg), narrower than DOOM AND out of sync with the floors */
#define VS_NEAR 24
#define VS_MAXVIS 120
#ifndef VS_BENCH
#define VS_BENCH 0       /* perf breakdown: one-shot proj32/lut32/strip readout on the HUD (frame 180). One-time stall; dev-only. Teleport the spawn to the view under test. */
#endif
#ifndef VS_CAPS
#define VS_CAPS 1        /* ramp-cap triangles ON: diagonal-alpha edge tiles bevel the 16px wall-top staircase into a smooth silhouette. Picket-proof by construction (top-only, NO raise -> can't spike above the flat top; slope gated to +-32). Affordable now that occlusion-cull cut the seg count (4 slope-divides/seg). */
#endif
#define VS_RAD 24        /* player collision radius: the move-test is extended by this in the travel direction so the camera halts VS_RAD short of a wall (no nose-clipping into walls = no fps=5 close-ups / wall-clip artifacts). DOOM's radius is 16; a touch more for camera comfort. */
#define VS_BUDGET 24     /* DEPTH-CULL framerate guarantee: at most this many wall strips per frame. BSP is front-to-back, so the budget is spent on NEAR geometry first; once hit, remaining (far) walls are dropped to fog AND the traversal halts (vs_open=0) -> bounds BOTH emit and projection => bounded frame time. Tune for the target cadence. */
static volatile long g_vs_sink;
static short vs_ct[VS_NCOL_MAX], vs_cb[VS_NCOL_MAX];     /* per-column open clip range (DOOM portal clip) */
static int vs_prevspr=41;       /* walls live at sprite 41+; LUT floor/ceiling owns 1..40 */
static int vs_px, vs_py, vs_spr, vs_open, vs_emit;   /* BSP-traversal shared state */
static int g_vs_tiles, g_vs_dmin, g_vs_dmax;          /* per-frame debug: wall tiles emitted + depth range */
static int g_bbox_n, g_seg_n, g_seg_clipped;         /* per-frame perf counters: vs_bbox_vis calls, segs projected, segs killed by the vertical band-reject */
#ifdef VS_DIAG
static int g_cull_near,g_cull_frus,g_cull_back,g_cull_off,g_cull_bud,g_cull_occ;   /* per-frame seg early-return tallies (sign-inversion diag); occ = occlusion pre-reject (#49) */
#define VSCULL(x) (g_cull_##x++)
#else
#define VSCULL(x) ((void)0)
#endif
static short vs_fcs, vs_fsn;
static int g_frP, g_frQ, g_frR;   /* per-frame frustum side-reject coeffs (Phase 1 cheap node cull): P=fsn-fcs, Q=-(fcs+fsn), R=fsn+fcs (S=P). A bbox is WHOLLY off-right iff min over it of (dx*P+dy*Q)>0, off-left iff max of (dx*R+dy*P)<0 -- exactly the 4-corner allR/allL, evaluated at 1 extreme corner (~2 MULs each). */
static unsigned short vs_stk[128];                   /* BSP node/subsector traversal stack (was 80; deeper so a big map never silently prunes a far subtree at the push guard) */
static int vs_eye=41;                                /* eased eye z (floor+41) -> walking up stairs raises the view */
static int g_flooreye=41;                            /* INSTANT player-floor eye ref (floor+41), NOT eased. Actor/FX feet anchor to THIS so they stay glued to the static LUT floor grid; using the eased vs_eye made far enemies float in front of the floor while climbing stairs. */
static unsigned char vs_nstr[VS_NCOL_MAX];               /* strips drawn per column (depth-complexity cap) */
static int g_dphist[64];                                 /* DEPTH-PRIORITY budget: per-frame depth histogram (32-unit buckets) -> the depth cutoff that keeps the NEAREST 'bud' strips */
static int g_dcap=16;                                /* per-column see-through layer cap -- RUNTIME dial (button B). Was a hidden #define (3); now crankable. The global sprite budget is the real bound. */
static short vs_clY[VS_NCOL_MAX], vs_flY[VS_NCOL_MAX];       /* per-column highest wall-top / lowest wall-bottom -> ceiling/floor fill extents */
static short vs_ctop[VS_NCOL_MAX], vs_cbot[VS_NCOL_MAX];     /* per-column top/bottom of the SOLID (or opaque-door) wall that CLOSED the column -> the ONLY correct LUT full-occlusion signal. vs_clY is the front-ceiling edge for 2-sided segs (over-parks above crates); vs_ct is vs_cb+1 once closed (over-parks above solid walls). Set ONLY where a column truly closes. */
static unsigned char vs_sky[VS_NCOL_MAX];                /* column sees a sky sector -> leave ceiling as backdrop sky */
static unsigned char vs_skd[VS_NCOL_MAX];                /* sky DECIDED: the nearest seg owns each column ceiling so recursed far sky cannot bleed the room ceiling above a window */
static unsigned char vs_ffl[VS_NCOL_MAX];                /* nearest-seg FRONT FLOOR flat slot per column (0xFF unset -> synthetic) */
static unsigned char vs_cfl[VS_NCOL_MAX];                /* nearest-seg FRONT CEIL flat slot per column (0xFF sky/unset) */
static unsigned char vs_ffr[FLOORLUT_COLS][5];           /* ZONAL: per-(BLOCK,row) FLOOR flat. Block-indexed (20-wide) like vs_cfr -- the LUT only reads the 20 block-centre columns. */
static unsigned char vs_cfr[FLOORLUT_COLS][7];           /* ZONAL: per-(BLOCK,row) CEIL flat. Block-indexed (20-wide), not g_ncol-wide: the LUT only reads the 20 block-centre columns, so stamping all 80 was 75% never-read waste. */
static short vs_wdep[VS_NCOL_MAX];                       /* per-column nearest SOLID-wall depth (for actor occlusion); 0x7FFF = no solid wall (open/capped) */
static short vs_stepd[VS_NCOL_MAX];                      /* per-column nearest STEP/lower-wall (riser) depth; 0x7FFF = none. With vs_stept -> occludes a far actor whose feet sit BELOW a nearer step crest (floor geometry never used to occlude actors). */
static short vs_stept[VS_NCOL_MAX];                      /* that nearest step's CREST screen-row (ybO). Actor hidden in this column when (vs_stepd nearer) AND (vs_stept <= feet) -> feet are behind/below the step. */
static unsigned char vs_skyblk[FLOORLUT_COLS];           /* SKY-IN-OPENING dedup: 1 if this 16px block already got an opening-sky strip this frame (so one even strip per block, not per wall column) */
static short vs_fdep[FLOORLUT_COLS][5];                  /* ZONAL: per-(BLOCK,row) FLOOR owner = winning seg's floor-edge row fr (LINE-priority). Block-indexed (20-wide). -1 = unstamped -> synthetic. */
static short vs_fbndy[FLOORLUT_COLS][5];                 /* zon>=6: EXACT floor-edge screen-y of whoever claims (block,row) as its TOP -> sub-tile boundary overlay = the smooth diagonal (vs the 16px row snap). -1 = none. */
static short vs_cdep[FLOORLUT_COLS][7];                  /* ZONAL: per-(BLOCK,row) CEIL depth (nearest wins). Block-indexed (20-wide) -- see vs_cfr. */
static signed short  g_flatpal[VSFLAT_NFLAT];        /* per-map: flat slot -> compacted HW palette slot (-1 = not uploaded) */
/* emit ONE 16px wall/cap column strip [y0..y1] of texture tex (depth d -> fog shade) */
#define VS_MURK 800           /* draw distance: walls beyond are simply not drawn (floor/ceiling fill + backdrop carry it) */
/* DEBUG DIALS (rising-edge): A=draw distance, B=tile/strip budget, C=cap mode; D reserved (map toggle).
   EMIT-only -- they never override the BSP. */
#define VS_NDD 33
#define VS_NDC 11
static int g_dd=14, g_dci=4;   /* (default DD[14]=750 / DC[4]=5) draw distance (DD[] idx) + per-column DEPTH CAP (DC[] idx -> g_dcap). Default DD[11]=600 (playable) / DC[5]=6. (Was DD[max]=32000 "draw everything"; moved to a playable default now that perf is mature + the node far-cull makes dd actually prune the walk.) */
static const short DD[VS_NDD]={0,100,150,200,250,300,350,400,450,500,550,600,650,700,750,800,850,900,950,1000,1500,2000,2500,3000,3500,4000,4500,5000,6000,8000,12000,16000,32000};   /* draw distance, world units. FILE SCOPE: both the wall dd dial AND vs_render's actor-dd cap read it. DD[0]=0 (fog-only) .. DD[11]=600 default .. DD[32]=32000 whole-map */
static int g_actddi=3;         /* (41) ACTOR draw distance = DD[] idx; default DD[3]=200 (default pick) -- monsters/props cull at 200u, independently of the wall dd. Dial UP (toward 32000=off) for more visible actors, DOWN for denser-map perf. */
/* DEBUG SHUTTLE: SPACE (CNT_A) = show/hide HUD; P (CNT_B) = cycle WHICH param; N (CNT_C) = value DOWN, B (CNT_D) = value UP; LEVEL is param 15 in the cycle (no longer a dedicated button). */
#define NSEL 49
static int g_sel=13;           /* selected debug param: 0=dd 1=dc 2=col 3=cap 4=zon 5=gen 6=ease 7=wpn 8=mmin 9=mbg 10=fog 11=flod 12=occl 13=bud 14=sky 15=seam 16=clod 17=prop 18=fmrk(floor murk) 19=cmrk(ceil murk) 20=hgun 21=hhud 22=dwlk(door walk-through) 23=LEVEL. '>' caret. Default 13. */
static int g_dbg=0;            /* debug HUD on/off (P toggles). OFF = clean view + skips the per-frame snprintf/ng_text overhead (the measurement perturbs what it measures). */
/* DISPLAY ORDER: HUD grid cell (row*4+col, row-major, 4 cols) -> param index. Identity 0..23; the tail is
   reordered and 'rset'(27) is DROPPED from the grid (no longer cursor-reachable; case 27 code stays, can be
   re-added). hclp(28) sits immediately before the perf preset pf(26) on the last row. WASD nav + the caret
   both run through this, so the param 'case' numbers + tk[] never move (no renumber). NDISP = grid count. */
#define NDISP 36
static const unsigned char DSEL[NDISP]={0,1,2,4, 5,6,24,8, 9,10,11,12, 13,14,15,16, 17,20,39,22, 23,3,25,40, 47,38,32,34, 35,36,41,42, 29,44,45,46};   /* 36 cells -> 9 rows (screen 3..11). 2026-06-23: DROPPED fmrk(18)/cmrk(19)/cmfg(48) -- redundant now the floor/ceiling depth fade is BAKED (INC2); and bnch(33) -- its cost is now on the bsp HUD line (rc=). Those 4 cases stay off-grid/P-cycle. wpn->gameplay-P, hmap(38)+vmap(24) separate, cap(3) on grid, fdbg->wbob(32). */   /* hgun(20)+hhud(21) merged into one CLEAN-VIEW toggle at slot 20 -> freed slot now holds bclp(39, vertical band-reject A/B). bspv(37)/pf(26)/hhud(21) off-grid (cases exist, not cursor-reachable). add(41)+mxa(42) actor-dd + actor-cap on a partial 10th row. */
static int g_zonal=0;          /* ZONAL flats: per-row visplane (correct floor/ceil flat per depth band). DEFAULT 0 (off/blanket, 2026-06-23): the real-flat zonal visplane is parked behind the toggle. 1=zonal; 2/3=bias up/down; 4=round (no bias); 5=round+grey-wins; 6=sub-tile. Shuttle param 4. Needs gen=0. */
static int g_generic=1;        /* GENERIC mode: synthetic floor/ceiling LUT + sky (the blanket look) instead of real flats; shuttle param 5. DEFAULT 1 (2026-06-23) = the synthetic blanket is the ship default; set gen=0 to enable real flats + the zonal visplane (param 4). */
#define NFOGBIAS 10
static const unsigned char FOGBIAS[NFOGBIAS]={15,20,25,30,35,40,48,56,66,78};   /* (6) WALL-FOG CURVE: g_fog0 (the near band edge) as a % of g_fog1 (the far edge, set by fogext). Low=heavy FRONT bias (mid-fog onsets close); idx5=40=NEUTRAL (the old 2:5 = 300/750); high=REAR bias (bright most of the way, dark only at the far edge). 10 steps (2026-06-23: finer range). */
static int g_fogcrv=5;          /* (23, was 'rad') WALL-FOG CURVE bias index 0..NFOGBIAS-1, front..rear. 5=neutral (FOGBIAS[5]=40 = the old 300/750). Sets g_fog0=g_fog1*FOGBIAS[]/100; fogext(10) sets g_fog1 (overall extent). */
static int g_murkease=4;        /* (6) 'ease' = far-horizon TRIM (2026-06-23): ef (g_murk_eff) = dd - ease*16, always on, 0..16 step 4 -> trims 0..256 units off the draw distance. The little far-draw opt (smaller ef = less node-walk + emit). Visible in the HUD's ef. Default 4. The fog curve moved to fcrv(23). */
static int g_weapon=1, g_pweap=-1; /* current weapon (0=fist..7=bfg; default pistol=1) + last-drawn (for change-only redraw). GAMEPLAY P cycles it now (2026-06-23, moved out of the menu); debug case 7 kept off-grid. */
static int g_wbob=1;               /* (32, was fdbg) WEAPON BOB on/off. 1=bob (the ~4px walk sway); 0=static gun -> no per-phase-change gun-band redraw while walking (the FastDoom dirty-HUD lesson [[doomng-fastdoom-bsp]]). */
/* MURK EXTENT controls, shuttle params 8/9/10: */
#define NMURKMIN 30
static int g_murkmin=5;    /* (8, default MURKMIN[5]=250) far-cull FLOOR: min g_murk_eff under load. Default idx10=500 -> with the node far-cull this prunes the WALK to ~500u under budget pressure (perf). Last idx (-1) = OFF (draw everything). */
static const short MURKMIN[NMURKMIN]={0,50,100,150,200,250,300,350,400,450,500,550,600,650,700,750,800,850,900,950,1000,1500,2000,2500,3000,3500,4000,4500,5000,-1};   /* 0 + 50-steps from 50 to 1000, then 500-steps to 5000 (matches DD); -1 (last) = OFF: far-cull disabled (draw to the strip budget) */
#define NMURKBG 8
static int g_murkbg=1;    /* (9) far-field BACKDROP colour (undrawn regions) = the FURTHEST murk layer. Default idx1=0x0111 near-black (deepest murk; floor/ceiling cull to it via the gradient layers). */
static const unsigned short MURKBG[NMURKBG]={0x0000,0x0111,0x0222,0x0333,0x0444,0x0555,0x0210,0x0421};
#define NFOGEXT 16
static int g_fogext=14;   /* (10) wall fog-band EXTENT %: scales the depth thresholds. Default idx14=75%. LAST = 0 = OFF (no wall fog). LOW % = fog onset near the camera (heavy fog). */
static const unsigned char FOGEXT[NFOGEXT]={5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,0};   /* 5..75% in steps of 5 (low = heavy/near fog) + OFF; % of the 400/1000 base depth thresholds */
static int g_fog0=300, g_fog1=750;   /* live wall fog-band thresholds (init = 75% for default g_fogext=1; recomputed from g_fogext on change) */
/* PERF presets RETIRED (Phase 3a, 2026-06-22): the OLD 7-field PERF[]/g_perf/pf was a strict subset of the
   23-field CFGP[] (see cfg_apply, ~line 1587). Param 26 (label "cf") now cycles CFGP, sharing g_cfgsel with the
   gameplay-P key -> one table, one selector, two entry points. */
#define NFCLOD 10
static int g_floorlod=0, g_ceillod=0;   /* (11) FLOOR crop + (16) CEILING crop, INDEPENDENT now: each drops FAR (horizon) rows one at a time (FCLOD_F/FCLOD_C, 0=full .. 9=off). Floor keeps NEAR=bottom, ceiling keeps top -> both recede from the horizon (centre) OUTWARD. Default 3 = floor 4 / ceil 5 (old combined look). */
static int g_props=1;     /* (17) PROPS visible: 1=draw actors (enemies/barrels), 0=hide them (debug A/B for geometry-only views). */
static int g_hidegun=0, g_hidehud=0;     /* (20) hide weapon / (21) hide player status bar -- clean-view debug toggles */
static int g_phidegun=-1, g_phidehud=-1; /* last-drawn states: the fix layer persists, so toggling must clear/redraw once */
static int g_doorwalk=1;                 /* (22) doors walk-through without opening collision (default ON -- frictionless flow while the trigger system settles) */
static int g_vpw=0, g_vph=5;   /* (18) FLOOR (fmrk) + (19) CEILING (cmrk) murk: # of FAR rows (toward the horizon) faded to the murk shades. SYNTHETIC planes (gen=1). g_vph=5 default -> the COLMAP CEILING depth fade shows (kills the perspective banding, 2026-06-23); cmrk(19) tunes it. fmrk(18) floor still 0=off by default. */
#define NVP 10                 /* murk levels 0..9 (2026-06-23, was 0..5); fmrk/cmrk clamp to the band's row count (floor 5 / ceiling 7) so the high end darkens the whole band */
static int g_vpl=0, g_vpr=9999, g_vpt=VS_LBT, g_vpb=VS_LBB;   /* computed each frame: H column range [g_vpl,g_vpr] + V band [g_vpt,g_vpb] */
static int g_occl=1;      /* actor occlusion vs walls (M2): barrels/enemies hidden behind nearer solid walls. HARDWIRED ON 2026-06-23 (enemies always need occluding) -> param 12 freed for the perfP toggle. */
#define MAXACT 32         /* per-view cap: draw at most the NEAREST 32 actors (bounds the sort + protects the sprite/scanline budget); the rest fog out */
static int g_maxact=8;        /* (42) runtime nearest-N actor cap (1..MAXACT array size); default 8 (default pick). Dial UP for more simultaneous actors, DOWN for monster-heavy-view perf */
static unsigned char g_thalive[VE_MAXTH];   /* COMBAT: runtime per-thing ALIVE flag (the const ve_th* tables can't be mutated); reset each map load */
static unsigned char ve_mvoff[VE_MAXSEG], ve_uvoff[VE_MAXSEG], ve_lvoff[VE_MAXSEG];   /* PERF: per-seg texture pegging (mid/upper/lower voff) BAKED at map load for sectors AT REST. vs_render_seg uses these unless a door/lift is mid-cycle on the seg's sector (pdyn) -> removes 3 %th DIVs/seg every frame. Camera-independent, so it never needed to be in the hot path. */
#define NFX 6             /* barrel-explosion FX pool */
static short g_fxx[NFX], g_fxy[NFX], g_fxz[NFX]; static unsigned char g_fxt[NFX];   /* world pos + frames-left (0=free) */
static const unsigned char FCLOD_F[NFCLOD]={5,4,3,2,1,0,0,0,0,0}, FCLOD_C[NFCLOD]={7,6,5,4,3,2,1,0,0,0};   /* 1 increment = 1 row: flod 0..5 = floor 5..0 rows, clod 0..7 = ceiling 7..0 rows; steps past 0 stay parked */
static int g_capmode=0;        /* wall cap MODE: index into CAPMODE[]. Default OFF (idx 0): the cap quantization made step-riser diagonals snap frame-to-frame. idx 1 TB32f = flat picket-proof bevel if re-enabled. */
/* COLUMN-RESOLUTION toggle (button C): trade wall-strip width for serration. ncol*colw=320 always.
   20x16 (baseline ~40 spr/scanline), 32x10 (~52, the sweet-spot pick), 40x8 (~60, serration halved),
   64x5 (~84 wall+lut, ~at the limit), 80x4 (~100 -> HW sprite dropout: the "watch it break" rung).
   colw = strip px width (HW h-shrink 0..15 = colw-1). screen-x -> column index = (sx*colrcp)>>16
   (colrcp=ceil(2^16/colw)) -- a MULS.W reciprocal instead of a shift, so colw need NOT be a power of 2
   (that's how 32x10 / 64x5 are possible). EXACT for sx in [0,320] (brute-verified); for pow2 widths it
   equals the old sx>>log2(colw) bit-for-bit. */
static int g_ncoli=0;   /* default col20 (default pick) */
static const struct { short ncol,colw; unsigned short colrcp; const char *name; } NCOLW[]={{20,16,4096,"col20"},{32,10,6554,"col32"},{40,8,8192,"col40"},{64,5,13108,"col64"},{80,4,16384,"col80"},{160,2,32768,"col160"}};   /* col160 = OVER-SPEC: 2px columns, 160 strips -> exceeds the ~96/scanline HW limit in dense views (per-line dropout = the ceiling, on purpose). NOTE: tk display does name+3 so the prefix MUST be 3 chars ("col") + digits. */
#define NNCOL ((int)(sizeof(NCOLW)/sizeof(NCOLW[0])))
#define g_ncol   (NCOLW[g_ncoli].ncol)
#define g_colw   (NCOLW[g_ncoli].colw)
#define g_colrcp (NCOLW[g_ncoli].colrcp)
/* EXHAUSTIVE cap permutation table (button C cycles). Per mode, INDEPENDENT for each edge:
     tg/bg = slope GATE for the top/bottom cap = max |dy| per 16px column that still earns a bevel
             (0 = no cap on that edge; steeper than the gate = plain staircase). Tile range is +-32.
     tr/br = RAISE for that edge: 1 = lift the strip to the high/low corner so the diagonal CHAINS the
             neighbour (smooth, but convex corners bump = pickets); 0 = FLAT, the bevel cuts INTO the
             wall only -> never protrudes (picket-proof) at the cost of a slight per-column sawtooth.
   The off-screen clip-gate (no cap where the edge runs off the play band) is ALWAYS on (see topclip/
   botclip) -- that is the "nocapswhenboundoffscreen" baseline, independent of the mode. */
static const struct { unsigned char tg, bg, tr, br; const char *name; } CAPMODE[] = {
    { 0, 0,0,0, "off"   },
    {32,32,0,0, "TB32f" },   /* both edges, widest gate, FLAT -- the leading candidate */
    {32,32,2,2, "TB32g" },   /* GRID-LOCK (tr/br==2): snap the capped strip height to a 16px boundary so the strip's single hardware vsh==255 over the cap tiles -> the baked diagonal renders FULL-height and CHAINS across columns, killing the per-column vsh sawtooth that picket-fences full-height segs. Needs BOTH edges capped (full-height) to make (y1-y0) a whole 16px multiple. Not a raise. A/B vs off/TB32f. */
    {32,32,3,3, "TB32o" },   /* OPAQUE-CORNER overlay (tr/br==3): the wall strip stays PLAIN (no in-strip caps -> rtop/rbot forced -1), and a SEPARATE overlay sprite is laid over each capped corner. The overlay is SOLID (idx1) above the diagonal + TRANSPARENT below, drawn with a cap palette (idx1 = mean ceiling colour for the top / floor colour for the bottom), so the bevel MERGES with the ceiling/floor instead of revealing the gridded floor/ceiling LUT (the "picket fence" the transparent-corner caps suffer). Costs 1 extra HW sprite per capped corner (budget-backstopped). */
    {32,32,1,1, "TB32r" },
    {16,16,0,0, "TB16f" },
    {16,16,1,1, "TB16r" },
    { 8, 8,0,0, "TB8f"  },
    { 8, 8,1,1, "TB8r"  },
    {32, 0,0,0, "T32f"  },
    {32, 0,1,0, "T32r"  },
    {16, 0,0,0, "T16f"  },
    {16, 0,1,0, "T16r"  },   /* the original top cap */
    { 8, 0,0,0, "T8f"   },
    { 8, 0,1,0, "T8r"   },
    { 0,32,0,0, "B32f"  },
    { 0,32,0,1, "B32r"  },
    { 0,16,0,0, "B16f"  },
    { 0,16,0,1, "B16r"  },
    { 0, 8,0,0, "B8f"   },
    { 0, 8,0,1, "B8r"   },
    {16,32,1,1, "16t32r"},   /* classic asymmetric: tight-raised top, wide-raised bottom (the prior default) */
    {16,32,0,0, "16t32f"},
    { 8,32,0,0, "8t32f" },   /* tight top, wide bottom, both flat */
};
#define NCAPMODE ((int)(sizeof(CAPMODE)/sizeof(CAPMODE[0])))
/* OPAQUE-CORNER cap (TB32o) palette slots. The HW palette RAM is fully booked in VS gameplay
   (0..15 system/LUT/murk, 16..243 walls+flats, 244..254 sprites, 255 backdrop), so there is no
   PERMANENTLY-free slot. The TOP TWO wall/flat slots (242,243) are reserved: the all-E1 wall+flat
   allocation never realises them on a ride (worst measured E1M3 = 193 wall-band slots; flats pack
   after but stay well under 242 for the seg-referenced subset), and when this cap mode is ACTIVE
   the wall/flat allocator's ceiling drops by 2 (gated on CAP_OPAQUE, below) so it CANNOT hand 242/243
   to a texture, and the cap palettes are re-asserted after every per-map upload. Caps OFF -> the ceiling
   is unchanged (243) and these slots are never written -> byte-identical to before. */
#define CAP_CEIL_PALSLOT  242
#define CAP_FLOOR_PALSLOT 243
#define CAP_OPAQUE (CAPMODE[g_capmode].tr==3)   /* the opaque-corner overlay cap mode is active */
static int g_vs_murk=VS_MURK, g_vs_budget=VS_BUDGET;  /* wall draw-distance (A dial) + strip budget (now driven by the BUDGET dial below, param 13) */
static const short BUDGET[]={24,36,48,60,72,84,96,108,120,132,144,156,168,180,192,204,216,228,240,252,264,276,288,300,312,324,336};   /* DRAW-COUNT CAP steps (12 apart). Up to 288 = HW-safe (41+288+32 actors <380). 300-336 = OVER-SPEC: walls crowd the 380-slot ceiling, actors get squeezed, far walls drop (the SCB-safety cap makes it graceful). */
#define NBUDGET ((int)(sizeof(BUDGET)/sizeof(BUDGET[0])))
static int g_budgeti=26;   /* draw-count cap dial (param 13): nearest-N wall strips. Default = BUDGET[26]=336 (over-spec; perf is mature enough to default here). 288 (idx22) was the old HW-safe default; far excess drops gracefully via the SCB-safety cap. Dial DOWN to shrink the flicker window + speed dense rooms. */
static const short BXCAP[]={8,12,16,20,24,28,32,40,48,64,68,72,76,80,84,88,92,96,100,104,108,112,116,120,124,128,160,192,256,384,2048};   /* (44) PHASE 2 WALK BUDGET: max BSP node-box tests (bx=g_bbox_n) per frame. FINE low end (8..48) + FINE 64..128 in steps of 4 (2026-06-23) to find where the walk breaks + eases into the murk. The walk halts when bx hits this; front-to-back order means the unvisited stack is the FAR subtrees -> they drop first (depth-ordered, ~ a node-count dd). Bounds the WORST-CASE walk on node-dense/stair maps. Last=2048=OFF. */
#define NBXCAP ((int)(sizeof(BXCAP)/sizeof(BXCAP[0])))
static int g_bxcapi=NBXCAP-1;   /* default OFF (2048). Dial DOWN to cap the walk; caveat: the cut is walk-order (mostly far, occasional near-sibling pop) -- v2 = depth-priority if the pop bites. */
static const unsigned char PSTEPV[]={1,2,4,8,16,32};   /* (45) PHASE 5 PERSPECTIVE SUBDIVIDE step: the hmap3/vmap3 1/z fraction is computed EXACT (one 32-bit divide) only every PSTEP cols, then affine-interpolated between. 1=exact-per-col (the OLD per-column divide tax) .. 8=default (~4x fewer divides) .. 32=coarsest (~16x fewer, faint texture swim mid-span). The divide was un-throttled by dd/dc/bud/bxc/col -> this is the broad perf lever. */
static const unsigned char PSTEPSHV[]={0,1,2,3,4,5};   /* log2(PSTEPV[]) -> the slope shift (PSTEP is a power of 2) */
#define NPSTEP ((int)(sizeof(PSTEPV)/sizeof(PSTEPV[0])))
static int g_pstepi=3;                 /* default PSTEP=8 */
static int g_pstepv=8, g_pstepsh=3;    /* resolved per-frame from g_pstepi (file-scope so vs_render_seg can read them) */
static int g_opensky=1;   /* SKY-IN-OPENING: draw sky through windows/openings whose BACK ceiling is sky (#42). HARDWIRED ON 2026-06-23 -> param 14 freed for the cmap (colmap-ceiling) toggle. */
static int g_seamover=0;  /* SEAM OVERDRAW (param 15): widen each wall strip by N px to the right so neighbours OVERLAP -> a column mid-rebuild is masked by its neighbour's overdraw instead of a blank gap (a temporal-parallax flicker mask). 0=off; only bites at col>=32 (col20 strips are already 16px=max width). */
static int g_murk_eff=VS_MURK;   /* FLICKER FIX: effective far-horizon, eased per frame by budget pressure -> the far-cull drops by DEPTH (stable) not strip-emit-order (which reshuffled frame-to-frame = the flicker) */
static const unsigned char GOVFPS[]={0,1,2,3,4,5,6,10,12,15,30,60};   /* (46) gov dial = TARGET FPS (0=off). All divide 60 evenly -> the governor's vblank target = 60/fps (1fps->60 vbl .. 60fps->1 vbl). Sweep it to read the honest fps<->view-distance curve; the low end (1-5fps) shows how much geometry survives at a lower speed target. */
#define NGOVFPS ((int)(sizeof(GOVFPS)/sizeof(GOVFPS[0])))
static int g_govtgt=0;           /* (46) GRACEFUL FAR-DROP governor: INDEX into GOVFPS[] (0=OFF). vblank target = 60/GOVFPS[idx]; when vs_render's OWN cost (g_rcost) exceeds it, pull the far-horizon (g_govmurk) IN proportional to the overshoot, always extend gently (AIMD recovery). Holds a steady framerate by trading view distance under load. Drives off g_rcost (NOT whole-frame cad) so the debug HUD doesn't poison it. */
static int g_govmurk=VS_MURK;    /* the governed far-horizon, eased by measured frame time in the main loop, clamped to [floor(MURKMIN), DD[dd]] */
static int g_lastrendered=0;     /* did the PREVIOUS frame actually render (vs static-skip)? gates the governor so idle frames don't push the horizon back out */
static int g_rcost=1;            /* vs_render's OWN vblank cost (measured around the call) -> the governor's metric: EXCLUDES the debug-HUD/gun/audio so it's HUD-independent + sub-frame (0=fit under a vblank) */
static int g_perfP=0;            /* perfP (46, 2026-06-23): the UNIFIED smoothing governor's PRESSURE 0..PERFP_MAX. Rises when the gov far-horizon is pinned at its floor and STILL over the fps target; falls on headroom. Drives a LOD LADDER below horizon (lever 0): ceiling rows shed first, then floor. INERT when gov(46) is off. */
static int g_perf_clod=0, g_perf_flod=0;   /* perfP-derived EXTRA ceiling/floor LOD steps, added to g_ceillod/g_floorlod (clamped). */
#define PERFP_MAX 24
static int g_perfen=0;           /* (12, was 'occl') perfP ENABLE: standalone auto-LOD governor toggle. Default OFF (opt-in). When ON, sheds ceiling-then-floor LOD under sustained load to hold ~1 vblank -- independent of gov(46). */
/* RADIAL draw-distance: the cull is on PERPENDICULAR depth (a flat plane), so the screen edges reach ~1/cos
   farther in euclidean distance -> eccentric far-geometry over-included (the wide-view cost). g_murkcol[c] =
   g_murk_eff * cos(view-angle of column c) carves the slab into a uniform-radius cone-sector (edges cull sooner). */
static short g_cosrad[VS_NCOL_MAX]; static int g_cosrad_n=-1;   /* per-column cos(angle)*256; recomputed only on column-res change */
static int g_murkcol[VS_NCOL_MAX];                              /* per-column radial far-cull threshold (= g_murk_eff*cos), refreshed each frame */
#define NRAD 9
static int g_radial=0;   /* DEPRECATED 2026-06-23 (param 23 reused for 'fcrv' fog curve). Stays 0 -> the radial far-cull paths below (1035/1118/1119/1131/1443) are inert. (Was: RADIAL far-cull strength curve, 1..8 = per-column euclidean pull-in at eccentric angles.) */
static int g_vmap=3;     /* (24, default 3) VERTICAL MAP: 0 = ORIGINAL stretch (whole texture to wall height, no tiling/peg); 1 = V companion everywhere (1:1 world-scale + vertical tiling + DOOM peg, AFFINE depth); 2 = SELECTIVE (companion only for single-copy posters/signs, stretch for tiling walls); 3 = companion everywhere + PERSPECTIVE-CORRECT depth (the wf 1/z-linear depth -> the 1:1 vertical scale tiles at the true rate, so brick/panel courses stay level on angled walls; twin of hmap=3). Default 2. */
static int g_hclip=0;    /* (28) HEIGHT-CLIP flats: clip each floor/ceiling LUT band to the NEAREST wall's top/bottom (vs_clY/vs_flY) so the flat can't overspill past a wall at col<16. TRADE-OFF: clips to the near wall, so see-through floor/ceiling THROUGH openings is reduced (single band sprite can't both clip the near wall AND show the far flat). Default OFF; A/B it. */
static int g_lutcull=0;  /* (29) LUT-CULL (perf): PARK a floor/ceiling LUT block when the nearest wall fully covers its band (vs_clY at/above the ceiling-band top, or vs_flY at/below the floor-band bottom). Pixel-identical (walls 41+ overdraw the LUT 1..40). Default OFF: measured no speedup on the ride (the fully-covered case is too rare to pay for the per-block span check) -- kept as a toggle. */
static int g_hwtail=1;   /* (30) SCB TAIL-CLEAR mode: 1 = high-water (blank only the stale delta past 'cot'); 0 = the old full-pad-to-16 on every shrunk strip. Toggle to A/B that the high-water bookkeeping is not a net loss vs the simple pad. Default ON. */
static int g_untex=0;    /* (31) DEBUG sprite-budget view: draw every wall strip UNTEXTURED (the PLACEHOLDER tile, uniform) so the sprite GRID is visible -- how many strips across (vertical seams), each strip's tile-height down, and the hardware V-shrink (the tile squashes vertically as a strip is scaled down). Pair with the HUD ns=/t= count. Default OFF. */
static int g_flatdbg=0;  /* (32) DEBUG flat-source view: paint each floor/ceiling patch a SOLID colour keyed by its flat ID (TEXBASE+(fid&7)) -> SEE which sector's flat draws where (the pit-blue-on-main-floor overspill). Default OFF. */
static int g_bspviz=0;   /* (37) BSP TRAVERSAL VIZ: walls drawn as solid DEPTH-coloured strips (8 bands near->far), REVEALED progressively in front-to-back walk order (g_bspstep strips this frame) so the live BSP sweep near->far is visible. Forces a redraw each frame (overrides the static-skip). */
static int g_bspstep=0;  /* BSP viz: how many front-to-back wall strips to reveal this frame; ramps up then loops */
static int g_hmap=3;     /* (38, default 3) HMAP horizontal wall-U: 0 = screen-mapped (tc=c) everywhere; 1 = AFFINE wall-U everywhere (glued, but affine-swims on tiling walls); 2 = SELECTIVE (affine only for single-copy posters/signs, screen-mapped for tiling walls); 3 = PERSPECTIVE-CORRECT wall-U everywhere (glued AND swim-free via a 1/z-linear world-fraction interp; costs ~1 divide/walked-column -- A/B the fps). Default 2 (a notable improvement); 3 is the proper fix if the perf is acceptable. */
static int g_bench=0;    /* (33) bnch: ON-DEMAND per-stage timing. Toggle ON -> ONE 16x burst at the current view (a ~1-2s hitch), freezes the breakdown on the HUD; toggle OFF to clear. The ONLY timer is g_vbl (vblank-granular), so a single frame's stage delta is unmeasurable -> each config runs 16x and reads accumulated vblanks (a periodic auto-burst was the old "big stall", so this is request-only). */
static int g_bn_fa=0,g_bn_fp=0,g_bn_fb=0,g_bn_fe=0;   /* captured 16x-vblank totals: FULL / walls-no-flats / baseline / proj-only. Derive: emit=fa-fe, flat=fa-fp, wall=fp-fb, proj=fe-fb. */
static int g_skip=1, g_redraw=1, g_drewactors=0;   /* (34) skip: STATIC-FRAME SKIP -- reuse last frame's SCB when nothing visible changed (camera/anim/doors/lifts/fire/fx all static), so idle frames cost ~0. g_redraw forces a render (dial/map/kill); g_drewactors gates the anim-phase wake so empty rooms skip 100%. */
static int g_bandclip=1;   /* (39) bclp: VERTICAL BAND-REJECT -- skip a seg whose projected height misses every open column's surviving sliver (the near-window stutter fix). Default ON; A/B toggle. */
static int g_dpri=1;       /* (40) dpri: DEPTH-PRIORITISED BUDGET -- record every strip within the horizon, then emit the NEAREST 'bud' by DEPTH (a depth histogram) instead of halting the walk at the budget. Fixes near walls dropping when the BSP visits them after the budget fills (the dense-courtyard near-wall flicker). Default ON; A/B toggle (off = old walk-order halt). */
static int s_px=0x7fffffff, s_py=0, s_ang=0, s_aph=0;   /* static-skip signature: last-RENDERED camera + (g_anim>>3) actor phase */
static int g_skipcnt=0;   /* running count of SKIPPED frames -> climbs ~60/sec while idle (independent proof the skip fires); frozen while moving */
#define FCN 32
static unsigned char g_fcost[FCN]; static int g_fci=0;   /* frame-cost SCOPE ring: per-frame vblank cost (cad), logged EVERY frame incl. while moving. The live fps= readout freezes to 60 on stopping (the HUD locks movement, so it cannot be read while moving); this ring keeps the MOVING history -> read it after a move to see the heavy frames, then the tail settles to 1 as the static-skip kicks in. */
static int g_act_sel=0, g_act_emit=0;   /* ACTOR cull instrument: act=emit/sel on the HUD. sel = nearest-N actors selected (after the frustum cull); emit = how many actually drew >=1 on-screen, non-occluded column. emit<<sel => slots wasted on occluded/edge actors (a reason to add an occlusion pre-cull). */
static int g_ncull=1;   /* (35) NODE FAR-CULL: prune a BSP subtree wholly beyond the far-horizon (g_murk_eff) in vs_bbox_vis -> dd/murk limits the WALK (pj), not just the strip emit. Default ON; A/B the far-horizon look (floor/ceil extent past the fog). Most effective at playable dd / dense views where the murk pulls in; a no-op at dd=32000 (nothing is beyond the map edge). */
#define NNCLIP 18
static int g_nclip=1;   /* (36) NEAR-CLIP dial: idx into NCLIP[]; default idx1=8 (2026-06-23, was idx2=16). Push the near plane OUT to clip the huge near WALLS (perf probe: do the widest near strips / screen-eccentric edges dominate? does drawing slightly deeper save time?). 0 = degenerate near vtx project to centre + back-face cull (safe). */
static const short NCLIP[NNCLIP]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,176,192,200};
static int g_vs_near=VS_NEAR;   /* live near-clip = NCLIP[g_nclip]; replaces VS_NEAR for the WALLS + the BSP-walk bbox. Actors/shoot keep the fixed VS_NEAR so aim/gameplay is stable. */
static int g_dbg_ffl=-1, g_dbg_bfl=-1;   /* FLAT-DBG readout (fdbg on): centre column's nearest-seg FRONT + BACK floor flat -> see if a main-floor cell wrongly latches the pit's flat from the seg's front side */
static int g_cam_ffl=0xFF;   /* camera's current-sector FLOOR flat, recomputed once/frame -> the DEFAULT near-floor flat (fixes the nearest-seg-is-a-far-wall mis-latch; keyed on a frame constant so the majority of columns stay byte-identical) */
static int g_cam_cfl=0xFF;   /* camera's current-sector CEIL flat, recomputed once/frame in the SAME BSP descent -> the DEFAULT top-band ceil flat (symmetric twin of g_cam_ffl; fixes the near far-wall ceiling mis-latch the floor already got, no extra descent) */
static int g_floor_rows=5, g_ceil_rows=7;             /* LUT rows drawn (full-res band: floor 5, ceil 7), from g_lod_floor/ceil (0 => parked) */
static int g_noemit=0;                                /* forensic: project+clip (early-out intact) but skip vs_strip writes -> isolate wall projection from emit */
#define VS_RFMAX 1536                                 /* reciprocal LUT range: covers all drawn depths (<= murk 1500); far falls back to a divide */
static short g_rf[VS_RFMAX];                          /* g_rf[d] = (FOCAL<<8)/d -> kills the per-seg depth DIVS.W (scA/scB exact, sxa/sxb 1 MULS) */
#define VS_SPANMAX 1024                               /* 1/span reciprocal LUT range: covers segs up to 1023px wide; extreme near walls fall back to a divide */
static unsigned short g_rspan[VS_SPANMAX];           /* g_rspan[span] = 65536/span -> kills the per-seg /span DIVS.W (x-step + cap slopes; MULS instead) */
/* PER-MAP PALETTE COMPACTION: the all-E1 C-ROM holds NTEXTILE(=173) textiles, but the HW has only
   256 palette slots; 16 + 3*173 fog bands would overflow (the boot upload's "Invalid write" flood).
   Tile-ids stay GLOBAL (C-ROM addressing via TEXTBASE); the PALETTE slot becomes a dense PER-MAP
   index. vs_upload_tex_pals scans this map's wall texids (VEMT/VEUT/VELT) + the sky, assigns slots
   0..vs_nlpal-1, and uploads 3 distance-shade bands each (L0 full / L1 *13/16 / L2 *7/16 -- the same
   fog init_palettes used). E1M1 = 29 walls + sky -> 16+3*30 = 106 slots. Re-run on the map toggle. */
static signed short vs_lpal[NTEXTILE];   /* global textile id -> compacted local palette slot, -1 unused */
static int vs_nlpal=1;                    /* distinct textures uploaded for the current map (band stride) */
static int g_nfogband=3;                  /* per-map distance-fog band count (1-3); ADAPTIVE: reduced on dense maps so ALL wall palettes fit (no clamp-to-243 blank/solid walls). */
static const unsigned char FOGSCALE[3][3]={{16,16,16},{16,7,7},{16,13,7}};   /* [nb-1][band] darkness/16: nb1=full only / nb2=full+0.44 / nb3=full+0.81+0.44 */
static int g_itembase=16;                 /* per-map base palette slot for ITEM billboards (armour/ammo); set after the flats in vs_upload_tex_pals. draw adds the item's SPR_x_PAL index. */
/* OPAQUE-CORNER cap palettes -> the 2 reserved slots. idx1 = mean ceiling colour (top overlays) /
   mean floor colour (bottom overlays); the wedge sprite is idx0/idx1 only. Called only when the
   opaque cap mode is active (after each per-map wall/flat upload, which would otherwise clobber the
   top wall/flat slots). Caps OFF -> never called -> these slots are untouched. */
static void vs_upload_cap_pals(void){
    for(int i=0;i<16;i++){ MMAP_PALBANK1[CAP_CEIL_PALSLOT*16+i]=CAP_CEIL_PAL[i];
                           MMAP_PALBANK1[CAP_FLOOR_PALSLOT*16+i]=CAP_FLOOR_PAL[i]; }
}
static void vs_upload_tex_pals(void){
    for(int t=0;t<NTEXTILE;t++) vs_lpal[t]=-1;
    int n=0;
    for(int s=0;s<ve_nseg;s++){
        int e[3]; e[0]=ve_mt[s]; e[1]=ve_ut[s]; e[2]=ve_lt[s];
        for(int k=0;k<3;k++){ int t=e[k]; if(t>=0 && t<NTEXTILE && vs_lpal[t]<0) vs_lpal[t]=n++; }
    }
    if(SKY_TEX>=0 && SKY_TEX<NTEXTILE && vs_lpal[SKY_TEX]<0) vs_lpal[SKY_TEX]=n++;
    if(n<1) n=1;
    int palcap = CAP_OPAQUE ? (CAP_CEIL_PALSLOT-1) : 243;   /* OPAQUE cap ON: reserve 242/243 for the cap palettes -> flats clamp at 241. OFF: unchanged (243). */
    /* count distinct flats FIRST (needed for the adaptive-band budget below) */
    for(int i=0;i<VSFLAT_NFLAT;i++) g_flatpal[i]=-1;
    int nflat=0;
    for(int s=0;s<ve_nseg;s++){ int fe[2]; fe[0]=ve_ffl[s]; fe[1]=ve_cfl[s];
        for(int k=0;k<2;k++){ int fsl=fe[k];
            if(fsl<VSFLAT_NFLAT && VSFLAT_BASE[fsl]>=0 && g_flatpal[fsl]==-1){ g_flatpal[fsl]=-2; nflat++; } } }
    for(int i=0;i<VSFLAT_NFLAT;i++) if(g_flatpal[i]==-2) g_flatpal[i]=-1;   /* clear the seen-markers; real slots assigned below */
    /* ADAPTIVE FOG: use the MOST distance-shade bands (<=3) whose wall pals + flats + items still fit the
       slot budget. Dense maps (E3M3: 83 walls -> 3*83 overflowed; the old "n=76" clamp left L>=76 textures
       pointing at UNWRITTEN slots = the blank/solid walls) now drop to 2 or 1 bands so EVERY texture gets a
       real palette. Trade: flatter distance fog on those maps. */
    int nb=(palcap-TEXBASE-nflat-NITEMPAL)/n; if(nb>3)nb=3; if(nb<1)nb=1;
    vs_nlpal=n; g_nfogband=nb;
    for(int t=0;t<NTEXTILE;t++){ int L=vs_lpal[t]; if(L<0||L>=vs_nlpal) continue;
        for(int b=0;b<nb;b++){ int sb=(TEXBASE+b*n+L)*16, sc=FOGSCALE[nb-1][b]; MMAP_PALBANK1[sb]=0x8000;
            for(int i=1;i<16;i++){ unsigned short c=TEXPAL16[t][i]; int r=(c>>8)&0xF, g=(c>>4)&0xF, bl=c&0xF;
                MMAP_PALBANK1[sb+i]=(unsigned short)(((r*sc/16)<<8)|((g*sc/16)<<4)|(bl*sc/16)); } }   /* band b darkness = FOGSCALE[nb-1][b]/16 */
    }
    /* per-map FLAT palettes: ONE band each (flats need no distance fog), packed AFTER the nb wall bands
       at FLATBASE (reusing the slots freed when nb<3); first-seen distinct flat slots only. */
    { int fn=0, FLATBASE=TEXBASE+nb*n;
      for(int s=0;s<ve_nseg;s++){
        int fe[2]; fe[0]=ve_ffl[s]; fe[1]=ve_cfl[s];
        for(int k=0;k<2;k++){ int fsl=fe[k];
            if(fsl<VSFLAT_NFLAT && VSFLAT_BASE[fsl]>=0 && g_flatpal[fsl]<0){
                int slot=FLATBASE+fn++; if(slot>palcap){ g_flatpal[fsl]=palcap; continue; }   /* clamp: degrade colour, never overflow palette RAM (or stomp the reserved cap slots) */
                g_flatpal[fsl]=slot; MMAP_PALBANK1[slot*16]=0x8000;
                for(int i=1;i<16;i++) MMAP_PALBANK1[slot*16+i]=VSFLAT_PAL16[fsl][i]; } }
      }
      /* ITEM billboard palettes (armour/ammo): one band each, packed right after the flats. Items appear
         on every map; g_itembase is the runtime slot the draw path adds each item's SPR_x_PAL index to.
         Worst E1 map: 215 flats + 9 items = 224 <= palcap (241/243) -> never clamps. */
      g_itembase = FLATBASE + fn;
      for(int ii=0; ii<NITEMPAL; ii++){ int slot=g_itembase+ii; if(slot>palcap) break;
          MMAP_PALBANK1[slot*16]=0x8000;
          for(int i=1;i<16;i++) MMAP_PALBANK1[slot*16+i]=ITEMPAL16[ii][i]; } }
    if(CAP_OPAQUE) vs_upload_cap_pals();   /* OPAQUE cap ON: (re)assert the 2 cap palettes after the per-map upload so the wall/flat pass can't clobber them */
}
/* dyt/dyb = this strip's top/bottom edge slope in screen-px of drop per g_colw-px column (L->R), so the
   cap diagonal (a 16px tile h-shrunk to the column width) matches the local wall slant -- the per-column
   staircase becomes a smooth edge. (caller scales by g_colw so this holds at any column resolution.) */
/* TWO-PASS EMIT (dense-room flicker fix): during the BSP walk, vs_strip RECORDS each visible wall strip
   here instead of writing VRAM inline; vs_render BURSTS them all (vs_strip_emit) AFTER the walk. Inline
   emit interleaved the SCB writes across the whole ~5-vblank projection walk, so the wall SCB set was in
   flux ~2/3 of every frame -> the raster sampled a half-built frame = the near-geometry flicker. Bursting
   confines the flux to the ~0.4-vblank emit tail. PIXEL-IDENTICAL: same strips, same slots (41+i in walk
   order), same emit body. Buffer index = spr-41; vs_spr still increments in the walk (budget cull intact).
   tcol fits short exactly -- DOOM texture widths are powers of 2, so 65536-wrap preserves tcol%wt. */
#define VS_SBUFN 512   /* RECORD buffer. With DEPTH-PRIORITY (dpri) the walk records ALL strips within the horizon (not just the budget), then the burst keeps the nearest 'bud' by depth -> needs headroom above the budget (col40 dense courtyard records well past 336). SCB EMIT still hard-capped at slot 380. RAM roomy post-excision. */
typedef struct { short y0,y1,tex,tcol,d,dyt,dyb,yt0,voff; unsigned char c,sky,poster; } vstrip_t;   /* yt0 = UNCLIPPED wall top (clip-aware vertical peg); voff = texture-TILE offset at yt0 (DOOM pegging: 0 = peg top, >0 = peg bottom for uppers); sky=1 -> screen-anchored window-opening sky strip; poster=1 -> single-copy seg (VMAP mode 2 companion-maps it) */
static vstrip_t g_sbuf[VS_SBUFN];
/* PERF (crib from Doom8088 FLAT-pad discipline): per-SCB-slot real-tile HIGH-WATER. The shrunk-strip
   vshrink over-read (gngeo) only reads stale REAL tiles past 'cot', so the invariant is kept "tiles
   cot..15 are BLANK after every emit" by blanking ONLY the delta from this slot's previous real extent
   down to 'cot' -- instead of always padding to 16 (which undid the PERF#1 SCB-halving). EVERY dynamic
   SCB1 writer (walls/sky/actors/muzzle/hud-edges) calls vs_scb_clear_tail so the invariant survives the
   slot<->geometry reuse that shifts frame-to-frame; g_scb_hw is reset to 16 on map load to force a full
   first clear over boot/old-map-undefined SCB1. Strictly <= the old per-strip blank count (usually 0-2). */
static unsigned char g_scb_hw[381];
static inline void vs_scb_clear_tail(int slot,int cot,int pal){
    int hw=g_hwtail?g_scb_hw[slot]:16;   /* hwt=0 -> old full-pad-to-16 (A/B that high-water isn't slower) */
    for(int r=cot;r<hw;r++){ *REG_VRAMRW=(u16)(BLANK_TILE&0xFFFF); *REG_VRAMRW=(u16)(pal<<8); }
    g_scb_hw[slot]=(unsigned char)(cot<16?cot:16);
}
static void vs_strip(int spr,int c,int y0,int y1,int tex,int tcol,int d,int dyt,int dyb,int yt0,int voff,int poster){
    int i=spr-41; if(i<0||i>=VS_SBUFN) return;   /* record into the burst buffer (replayed post-walk) */
    vstrip_t *e=&g_sbuf[i]; e->sky=0; e->c=(unsigned char)c; e->y0=(short)y0; e->y1=(short)y1; e->tex=(short)tex; e->tcol=(short)tcol; e->d=(short)d; e->dyt=(short)dyt; e->dyb=(short)dyb; e->yt0=(short)yt0; e->voff=(short)voff; e->poster=(unsigned char)poster;
}
static void vs_strip_sky(int spr,int c,int y0,int y1){   /* SKY-IN-OPENING: record a screen-anchored sky strip (two-pass; bursts via vs_sky_strip_emit post-walk) */
    int i=spr-41; if(i<0||i>=VS_SBUFN) return;
    vstrip_t *e=&g_sbuf[i]; e->sky=1; e->c=(unsigned char)c; e->y0=(short)y0; e->y1=(short)y1; e->voff=0; e->poster=0;
}
/* OPAQUE-CORNER cap overlay sprite: one SCB column-group laid over a wall strip's top/bottom corner.
   The wedge tiles are SOLID (idx1) above the diagonal + TRANSPARENT (idx0) below, drawn with the cap
   palette (idx1 = mean ceiling/floor colour) so the bevel merges with the ceiling/floor; the
   transparent part lets the (already-drawn) plain wall body show through. ytop/vsh/hsh/screen-x are
   matched to the wall strip so the wedge registers exactly over the corner. The wedge is baked as the
   negative-slope TOP stack (k=0 nearest the corner); 'flip' carries the in-strip cap's flip semantics
   (rtfl for top, rbfl for bottom = vh-flip). 'bottom' reverses the stack order so k=0 sits at the
   corner (screen bottom). Budget-backstopped at 379 (< 380 HW). Uses a FRESH slot (vs_spr++). */
static void vs_cap_overlay(int ytop,int nt,int ovloff,int flip,int palslot,int vsh,int hsh,int sx,int bottom){
    if(nt<1 || ovloff<0) return;
    if(ytop<VS_LBT){ int over=VS_LBT-ytop; int pt=(vsh*16)/255; if(pt<1)pt=1; int drop=over/pt; if(drop>=nt)return; ytop+=drop*pt; nt-=drop; }   /* TOP-CLIP: drop off-band tiles + lower the anchor so the 9-bit y can't wrap (mirrors vs_billboard) */
    if(nt<1) return;
    if(vs_spr>=379) return;                                  /* sprite-record backstop (< 380 HW) */
    int spr=vs_spr++;
    *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
    for(int r=0;r<nt;r++){ int k=bottom?(nt-1-r):r;          /* bottom: k=0 (corner) at the SCREEN BOTTOM (last row) */
        int T=RAMP_OVL_TILE0+ovloff+k;
        *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((palslot<<8)|(((T>>16)&0xF)<<4)|flip); }
    vs_scb_clear_tail(spr,nt,palslot);   /* keep the blank-tail invariant on this shared slot */
    int cyf=(496-ytop)&0x1FF;
    *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
    *REG_VRAMRW=(u16)((hsh<<8)|vsh); *REG_VRAMRW=(u16)((cyf<<7)|nt); *REG_VRAMRW=(u16)(sx&0x1FF)<<7;
}
static void vs_strip_emit(int spr,int c,int y0,int y1,int tex,int tcol,int d,int dyt,int dyb,int yt0,int voff,int poster){
    int topclip=(y0<VS_LBT), botclip=(y1>VS_LBB);   /* edge runs OFF the play band -> no real visible edge there -> no cap (clean diagonal where the edge shows, no picket where it's off-screen) */
    if(y0<VS_LBT)y0=VS_LBT; if(y1>VS_LBB)y1=VS_LBB; if(y1<=y0||tex<0) return;
    int wt=TEXWT[tex], th=TEXHT[tex]; tcol=((tcol%wt)+wt)%wt;
    int plvl=(d<g_fog0)?0:((d<g_fog1)?1:2); if(plvl>=g_nfogband)plvl=g_nfogband-1;   /* wall fog band, CLAMPED to the map's adaptive band count (dense maps run 1-2 bands); thresholds scaled by g_fogext */
    int lp=vs_lpal[tex]; if(lp<0)lp=0; int pal=TEXBASE+plvl*vs_nlpal+lp; if(pal>243)pal=243;   /* per-map compacted slot (not global tex) */
    /* RAMP CAPS: reuse the strip engine's baked diagonal-alpha edge tiles. Quantize the local
       slope exactly as the bake did (step 1 to +-16, step 4 beyond), look up RAMP_OFF[tex][slope][edge].
       Seat the strip's top at the high corner (raise by adt/2) so the diagonal crosses the original
       edge at column-center -> no net wall raise, just a smoothed silhouette. NO extra sprites. */
    int textured=(tex<NTEXTILE && wt>0 && th>0);
    int dtc=dyt; if(dtc<RAMP_DMIN)dtc=RAMP_DMIN; if(dtc>-RAMP_DMIN)dtc=-RAMP_DMIN;
    { int a=dtc<0?-dtc:dtc; if(a>16){a=((a+2)>>2)<<2; dtc=dtc<0?-a:a;} }
    int dbc=dyb; if(dbc<RAMP_DMIN)dbc=RAMP_DMIN; if(dbc>-RAMP_DMIN)dbc=-RAMP_DMIN;
    { int a=dbc<0?-dbc:dbc; if(a>16){a=((a+2)>>2)<<2; dbc=dbc<0?-a:a;} }
    /* GATE: the 16px cap tile faithfully bevels an edge only up to a +-16px/col slope (the step-1
       baked band, all proven A/B-clean). Beyond that the runtime slope saturates the +-32 clamp into a
       degenerate cap whose adt/2 raise (up to 16px) protrudes above the true silhouette -> picket fence
       on grazing hall walls. So when the TRUE (unclamped) per-column slope exceeds the tile's range,
       drop the cap (no lookup, no raise) and fall back to the plain 16px staircase. */
    /* GATE widened to the full +-32 the cap tiles represent: now that there is NO raise, caps are
       picket-proof at ANY slope (they only cut INTO the wall, never protrude), so the bevel can cover the
       grazing walls that used to fall back to the 16px staircase (the residual zigzag/picket). Only
       truly edge-on walls (|slope|>32, unrepresentable) stay blunt. */
    int tg=CAPMODE[g_capmode].tg, bg=CAPMODE[g_capmode].bg;   /* per-edge slope gates (0 = edge off) */
    int rtop=(tg&&textured&&!topclip&&(dyt<=tg&&dyt>=-tg))?RAMP_OFF[tex][dtc-RAMP_DMIN][0]:-1;   /* TOP cap: edge on + IN-band + within its gate */
    int rbot=(bg&&textured&&!botclip&&(dyb<=bg&&dyb>=-bg))?RAMP_OFF[tex][dbc-RAMP_DMIN][0]:-1;   /* BOTTOM cap: edge on + IN-band + within its gate */
    int rtfl=(dtc>0)?0x01:0x00, rbfl=(dbc>0)?0x02:0x03;   /* only TOP stacks baked: bottom(d)==vh-flip(top(d)) */
    int adt=dtc<0?-dtc:dtc, adb=dbc<0?-dbc:dbc;
    int ntt=(adt+15)>>4, ntb=(adb+15)>>4; if(ntt<1)ntt=1; if(ntb<1)ntb=1;
    /* OPAQUE-CORNER cap (TB32o): the wall strip stays PLAIN -- force the in-strip cap tiles OFF so y0/y1/
       cot/vsh/raise are all the unmodified wall values (the strip body is byte-identical to "no cap").
       Latch which edges WOULD have been capped (same gate) + their slope/flip so the overlay can be laid
       sprites after the SCB2 write. The tr==1/2 raise/grid-lock blocks below are gated on tr==1/2, so
       tr==3 leaves the strip untouched. */
    int ovl_top=(CAP_OPAQUE && rtop>=0), ovl_bot=(CAP_OPAQUE && rbot>=0);
    int ovl_toff=ovl_top?RAMP_OVL_OFF[dtc-RAMP_DMIN]:-1, ovl_boff=ovl_bot?RAMP_OVL_OFF[dbc-RAMP_DMIN]:-1;
    if(CAP_OPAQUE){ rtop=-1; rbot=-1; }   /* plain wall body; overlay drawn separately below */
    /* RAISE the capped strip top to the column's HIGH corner (y0 = wall-top at column CENTRE; the high
       corner sits adt/2 above it). Without this, no-raise left each column's LEFT HALF flat at the centre
       height -> a flat-then-diagonal SAWTOOTH per column (= the close-up pickets). Raising makes the
       diagonal span the FULL column (high corner -> low corner) so it CHAINS with the neighbour = smooth
       silhouette. Residual: convex corners bump up <= adt/2 (<=8px) -- localized, far milder than the
       sawtooth. (gate is +-16, so adt<=16, bump<=8.) */
    if(rtop>=0 && CAPMODE[g_capmode].tr==1){ y0-=adt/2; if(y0<VS_LBT)y0=VS_LBT; }   /* TOP raise (tr==1); FLAT edges cut into the wall (picket-proof) */
    if(rbot>=0 && CAPMODE[g_capmode].br==1){ y1+=adb/2; if(y1>VS_LBB)y1=VS_LBB; }   /* BOTTOM raise/extend (br==1) */
    if(y1<=y0) return;
    if(CAPMODE[g_capmode].tr==2 && rtop>=0){ y0&=~15; if(y0<VS_LBT)y0=VS_LBT; }   /* GRID-LOCK (tr==2): snap the capped top DOWN to a 16px boundary so vsh==255 over the cap tiles -> un-squashed, chaining diagonal (kills the per-column vsh sawtooth). */
    if(CAPMODE[g_capmode].br==2 && rbot>=0){ y1=(y1+15)&~15; if(y1>VS_LBB)y1=VS_LBB; }   /* GRID-LOCK (br==2): snap the capped bottom UP. Both edges snapped (full-height seg) -> (y1-y0) is a whole 16px multiple -> vsh==255. Only widens the gap, so y1>y0 still holds. */
    int cot=(y1-y0+15)>>4; if(cot<1)cot=1; if(cot>32)cot=32;
    g_vs_tiles+=cot; if(d<g_vs_dmin)g_vs_dmin=d; if(d>g_vs_dmax)g_vs_dmax=d;   /* debug tally */
    int cyf=(496-y0)&0x1FF;
    int vsh=((y1-y0)*255)/(cot*16); if(vsh<1)vsh=1; if(vsh>255)vsh=255;
    int capbot=(rbot>=0)?(ntb<cot?ntb:cot):0;            /* bottom cap (against the floor) has tile priority */
    int captop=(rtop>=0)?ntt:0; if(captop>cot-capbot)captop=cot-capbot; if(captop<0)captop=0;
    *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
    int st8, racc;
    if(g_vmap==1 || g_vmap==3 || (g_vmap==2 && poster)){           /* V companion: 1:1 world-scale vertical map, tiles when taller, DOOM-pegged. mode 1 = all walls (affine d); mode 2 = SELECTIVE posters; mode 3 = all walls fed PERSPECTIVE-CORRECT d (see the wf depth in vs_render_seg) -> level tiling on angled walls */
        int Hvis=(int)(((long)(y1-y0)*d*410)>>16);                 /* visible WORLD height in texture px (1:1; d/FOCAL per screen-px, FOCAL=160 ~= *410>>16) */
        int Vtop=(int)(((long)(y0-yt0)*d*410)>>16); if(Vtop<0)Vtop=0;   /* texture px at the visible top -> clip-aware peg from the UNCLIPPED wall top yt0 */
        st8=(cot>1)?(int)(((long)(Hvis>>4)*g_rspan[cot])>>8):0; racc=(Vtop<<4)+(voff<<8);   /* anchored at (Vtop+voff tiles); voff = DOOM peg offset */
    } else {                                                       /* ORIGINAL (param 24 OFF): stretch the FULL texture to the wall height -- no 1:1, no tiling, no peg */
        st8=(cot>1)?(int)(((long)th*g_rspan[cot])>>8):0; racc=0;
    }
    int bspb=0; if(g_bspviz){ bspb=d>>6; if(bspb>7)bspb=7; }   /* BSP VIZ (37): depth band 0(near)..7(far) -> a solid colour per band; the whole strip is one band (d is per-strip) */
    for(int r=0;r<cot;r++){ int T, rfl=0;
        if(g_untex||g_bspviz){           T=PLACEHOLDER_TILE; }                                /* DEBUG (31 untex / 37 bspviz): uniform placeholder tile -> solid colour from the palette below */
        else if(captop && r<captop){          T=RAMP_TILE0+rtop+r*wt+tcol; rfl=rtfl; }            /* top cap stack (high corner down) */
        else if(capbot && r>=cot-capbot){ T=RAMP_TILE0+rbot+(cot-1-r)*wt+tcol; rfl=rbfl; }   /* bottom cap = top stack vh-flipped */
        else { int srow=(th>0)?(racc>>8)%th:0; T=FIRST_TEX_TILE+TEXTBASE[tex]+srow*wt+tcol; }   /* V companion: tile vertically (mod th) instead of clamping -> tall walls repeat, not stretch */
        racc+=st8;
        int wp=g_bspviz ? (TEXBASE+bspb) : (g_untex ? (TEXBASE+((c&1)|((r&1)<<1))) : pal);   /* bspviz: solid DEPTH-band colour; untx: 2x2 (col,row) palette checker; else the wall palette */
        *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((wp<<8)|(((T>>16)&0xF)<<4)|rfl); }
    vs_scb_clear_tail(spr,cot,pal);   /* PERF: blank only the genuinely-stale tail past 'cot' (was: always pad to 16, which undid PERF#1). Same garbage-trail safety via the high-water invariant. */
    *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
    int hsh=g_colw-1+g_seamover; if(hsh>15)hsh=15;   /* SEAM OVERDRAW (g_seamover px): widen right so neighbours overlap -> a column mid-rebuild is masked by the overlap, not a blank seam */
    *REG_VRAMRW=(u16)((hsh<<8)|vsh); *REG_VRAMRW=(u16)((cyf<<7)|cot); *REG_VRAMRW=(u16)((c*g_colw)&0x1FF)<<7;
    /* OPAQUE-CORNER cap (TB32o): lay the overlay wedge(s) over the PLAIN strip's top/bottom corner. The
       strip body above (top) / below (bottom) the diagonal is painted the ceiling/floor colour so the
       bevel merges with the floor/ceiling LUT instead of revealing it (the "picket fence"). One extra HW
       sprite per capped corner (budget-backstopped inside vs_cap_overlay). */
    if(ovl_top || ovl_bot){
        int sx=(c*g_colw)&0x1FF, pt=(vsh*16)/255; if(pt<1)pt=1;   /* on-screen px per source-16 tile (matches the strip's vsh) */
        if(ovl_top){ int cb=ntt; if(cb>cot)cb=cot;
            vs_cap_overlay(y0,cb,ovl_toff,rtfl,CAP_CEIL_PALSLOT,vsh,hsh,sx,0); }     /* top: anchored at the wall top (tile 0), k=0 (corner) at top */
        if(ovl_bot){ int cb=ntb; if(cb>cot)cb=cot;
            vs_cap_overlay(y1-cb*pt,cb,ovl_boff,rbfl,CAP_FLOOR_PALSLOT,vsh,hsh,sx,1); }   /* bottom: anchored up from the wall bottom (y1), k=0 (corner) at the screen bottom */
    }
}
/* SKY-IN-OPENING strip: SCREEN-ANCHORED sky (texture row = screen tile-row, hscrolled by view angle, NO depth
   vshrink) drawn in a window opening, continuous with the ceiling-band sky in vs_lut. Avoids the 3 prior
   failures: not the ceiling band (no vs_sky[c] flood = no bleed); screen-anchored (the "vertical-extent" fix);
   emitted at the window seg's slot so the back sector's far walls (higher slot) overdraw it = correct layering. */
static void vs_sky_strip_emit(int spr,int k,int y0,int y1,int ang){   /* k = the 16px sky-BLOCK (0..FLOORLUT_COLS-1) -> drawn on the SAME 16px grid as the ceiling-band sky, so it stays even/aligned at ANY wall col-res (deduped per block by vs_skyblk) */
    if(y0<VS_LBT)y0=VS_LBT; if(y1>VS_LBB)y1=VS_LBB; if(y1<=y0||SKY_TEX<0) return;
    int swt=TEXWT[SKY_TEX], sth=TEXHT[SKY_TEX], spal=TEXBASE+(vs_lpal[SKY_TEX]<0?0:vs_lpal[SKY_TEX]);
    int stc=(((k-(ang>>2))%swt)+swt)%swt;                 /* hscroll by view angle, exactly like the ceiling-band sky */
    int cyf=(496-y0)&0x1FF; int cot=(y1-y0+15)>>4; if(cot<1)cot=1; if(cot>32)cot=32;
    *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
    int sr=(y0>>4)%sth;                                   /* texture row = screen tile-row -> continuous with the ceiling band */
    for(int r=0;r<cot;r++){ int T=FIRST_TEX_TILE+TEXTBASE[SKY_TEX]+sr*swt+stc;
        *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((spal<<8)|(((T>>16)&0xF)<<4)); sr++; if(sr>=sth)sr=0; }
    vs_scb_clear_tail(spr,cot,spal);   /* PERF: keep the blank-tail invariant on this shared (wall<->sky) slot */
    *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
    *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((cyf<<7)|cot); *REG_VRAMRW=(u16)((k*16)&0x1FF)<<7;   /* full 16px tile (hsh=15), vsh=255 (no depth scale), at the block's grid x=k*16 */
}
static void vs_park(int spr){ *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr; *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=0; *REG_VRAMRW=0; }
/* Static DOOM status bar -> fix layer rows 24..27 (the HUD letterbox y192..223, never touched by world
   sprites). HUDFIX_MAP is row-major [row*40+col]; the fix map is column-major (addr=col*32+row, MOD=32
   steps DOWN a column). Each word is already (pal<<12)|tile -> written verbatim. Neutral face is baked
   into HUDFIX_MAP (cols 18..21). Drawn ONCE; persists across frames + map toggles. */
#define HUD_ROW 26   /* fix-layer HUD top row. gngeo's visible_area.y=16 (=2 fix-rows) puts the active band's bottom 4 rows at 26..29, not 24..27. The SCB/3D layer already uses the y16-aware base (496); the fix layer (bar/face/gun) must match it -> bar flush at the bottom. */
static void draw_status_bar(void){
    for(int col=0;col<40;col++){
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+col*32+HUD_ROW;   /* MOD=1 = step DOWN the column (was 32 = next column -> only the top row drew = the real "missing rows" bug). rows 23-26 (clears overscan). */
        for(int row=0;row<4;row++) *REG_VRAMRW=HUDFIX_MAP[row*40+col];
    }
}
/* FIRST-PERSON WEAPON on the FIX layer (zero sprite cost). Drawn ONCE per weapon change (the fix layer
   persists like the status bar). Bottom-centred, just above the status bar (rows ..22). Palette SLOT 7
   (free: hudfix uses 2..6). Tiles from gunbake.py: GUNHAND_BASE + GUNHAND[w].base + r*wt + c. */
#define GUN_PAL 7
static int g_bobc=0, g_pphase=-1, g_fire=0, g_pfire=-1;   /* walk-bob accumulator + last-drawn phase; g_fire = firing-recoil countdown (frames), g_pfire = last-drawn fire state */
static int g_fwdph=0;   /* FLOW-PHASE accumulator: running signed FORWARD distance (world units). Drives the floor/ceiling flat scroll INSTEAD of absolute (px+py), so the flow tracks forward MOTION not world heading -> no reversal in the -x-y (120-240deg) quadrant. Pure rotation adds 0 -> still no turn-pop. (#40) */
static int g_sndq=0, g_sndqt=0;   /* delayed SFX queue: a HIT plays the gun pop NOW, then this (scream/boom) a couple frames later (one SFX channel -> sequence them) */
/* BOB: gunbake bakes GUNHAND_NBOB vertical phases per weapon (stride = wt*ht tiles). Cart picks the phase
   from g_bobc via a 0-1-2-1 triangle -> a gentle ~4px up/down while walking; holds steady when still. */
static void draw_gun(void){
    static const unsigned char BOBSEQ[4]={0,1,2,1};
    int phase=0; if(g_wbob){ phase=(g_bobc>>1)&3; phase=BOBSEQ[phase]; if(phase>=GUNHAND_NBOB)phase=GUNHAND_NBOB-1; }   /* clamp to baked phase count: NBOB<3 to fit 8 guns in the 4096-tile fix layer. wbob OFF -> phase fixed 0 -> the change-only guard below short-circuits every walking frame (no gun-band redraw cost) */
    int fire=(g_fire>0);
    if(g_weapon==g_pweap && phase==g_pphase && fire==g_pfire && g_hidegun==g_phidegun) return;
    g_pweap=g_weapon; g_pphase=phase; g_pfire=fire; g_phidegun=g_hidegun;
    for(int col=8;col<32;col++){ *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+col*32+15;   /* clear the weapon band (cols 8..31, rows 15..HUD_ROW-1). Tall guns (CHAINGUN/ROCKET/BFG) are top-clipped by 2 so even firing they top at row 15; all other guns top lower. Rows 11-14 are NEVER weapon pixels -> free for the debug HUD (grid 3..12, x@13, stk@14). */
        for(int r=15;r<HUD_ROW;r++) *REG_VRAMRW=(u16)SROM_EMPTY_TILE; }
    if(g_hidegun) return;                         /* HIDE WEAPON (param 20): band cleared above, draw nothing */
    if(g_weapon<0||g_weapon>=GUNHAND_N) return;
    const gunhand_t *gw=&GUNHAND[g_weapon];
    int wt=gw->wt, ht=gw->ht, leftcol=(40-wt)/2;
    int clip=(g_weapon==3||g_weapon==4||g_weapon==7)?1:0;   /* TALL guns CHAINGUN(3)/ROCKET(4)/BFG(7): the baker (gunbake.py: ht=ceil(h/8)+1) adds ONE empty headroom tile-row at the top for the bob to rise into. At rest it's blank -> skip it (clip=1) so the gun's (ht-1) real content rows fit the 11-row band (15..25) with the FULL top shown. The old clip=2 ate the headroom AND the gun's real top row = the observed top-crop; botcrop ate the bottom. clip=1 restores the top + keeps the bottom (2026-06-23). */
    int toprow=HUD_ROW-ht-(fire?1:0);              /* bottom anchored at row HUD_ROW-1, just above the bar. FIRE: kick up 1 tile */
    if(toprow+clip<15) toprow=15-clip;             /* keep the top within the cleared band (rows 15..25) so a firing kick leaves no residue above */
    int pbase=gw->base + phase*(wt*ht);            /* this phase's tile block */
    for(int c=0;c<wt;c++){ int scol=leftcol+c; if(scol<0||scol>=40) continue;
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+scol*32+toprow+clip;
        for(int r=clip;r<ht;r++){ int t=GUNHAND_BASE+pbase+r*wt+c; *REG_VRAMRW=(u16)((GUN_PAL<<12)|(t&0xFFF)); } }
    for(int i=0;i<16;i++) MMAP_PALBANK1[GUN_PAL*16+i]=GUNHAND_PAL16[g_weapon][i];   /* this weapon's palette -> slot 7 */
}
/* HUD FACE ANIMATION: cycle the 4 baked expressions (wad2c.py composites neutral/look-A/look-B/evil) for an
   idle look-around. Drawn into the face slot (fix cols 18..21, bar rows 23..26) only when the frame changes. */
static const unsigned short *const FACES[4]={HUDFIX_FACE0,HUDFIX_FACE1,HUDFIX_FACE2,HUDFIX_FACE3};
static int g_faceN=-1;
static void draw_face(int t){
    static const unsigned char FSEQ[8]={0,0,1,1,0,0,2,2};   /* slow idle: forward, look-A, forward, look-B */
    int n=FSEQ[(t>>4)&7]; if(n==g_faceN) return; g_faceN=n;
    const unsigned short *F=FACES[n]; g_vrambusy=1;
    for(int i=0;i<4;i++){ *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+(18+i)*32+HUD_ROW;
        for(int cy=0;cy<4;cy++) *REG_VRAMRW=F[cy*4+i]; }
    g_vrambusy=0;
}
/* ===== FB SPIKE (Doom64KB-style fix-layer framebuffer; toggle param 47 'fb') ===================
   A chunky byte/pixel buffer in WORK RAM is blitted to the fix layer, each pixel = one 8x8 solid
   cell. The 128-colour global palette (FB_PAL, packed 15/group since fix index 0 is HW-transparent)
   sits in fix slots 8..15; a global-index -> cell-word LUT (g_fbcw) avoids a per-pixel divide in
   the blit hot loop. Cell word = (pal<<12)|tile (verified main.c fix writes). Fix map is COLUMN-
   major: addr = col*32+row, MOD=1 steps DOWN a column. Gated: VSLICE untouched, default OFF;
   init_palettes() restores slots 8..15 on toggle-off. v1 = REAL SCENE: reuses the VSLICE BSP walk
   (vs_render emit=0 -> walk only, populates the per-column arrays) then flat-shades ceiling/wall/floor
   per column from vs_clY/vs_flY/vs_sky/vs_wdep. v1 limits (next increments): nearest-wall only (no
   see-through openings), placeholder flat colours (no per-flat bake), crude 2-band wall depth. */
static void vs_render(int px,int py,int ang,int emit);   /* fwd: fb_render runs the walk with emit=0 */
static int  vs_floor_at(int px,int py);                  /* fwd: set vs_eye (emit=0 skips the eye update) */
#define FBW 40
#define FBH 28
#define FB_ROW0 2                  /* top visible fix row (gngeo y16 offset = 2 fix rows) */
#define FB_FIXPAL 8                /* base fix palette slot; FB uses 8..15 (VSLICE's murk/LUT slots) */
#define FBC_CEIL  2                /* placeholder flat-shade colours (FB_PAL indices) until the per-flat */
#define FBC_WALLN 3                /* colour bake (v2): grey ceiling, light-grey near wall, */
#define FBC_WALLF 0                /* darker far wall, */
#define FBC_FLOOR 12               /* brown floor, */
#define FBC_SKY   1                /* blue sky */
#define FB_FOG_SHIFT 5             /* depth fog: light level = dC>>5 (0..31), ramps over the ~800-unit draw distance */
static int g_fb_mode=0;            /* (47) fb: 0=off / 1=FB (Doom64KB chunky framebuffer) / 2=unshaded (untextured-strip debug, the old untx) -- cycled by case 47 */
static const char *FBVNM[3]={"off","d64","utx"};   /* fb-toggle mode names for the HUD */
static int g_fb_savecol=0;         /* saved g_ncoli to restore on FB toggle-off (FB forces col40) */
static int g_fbcost=1;             /* fb_render's own vblank cost (coarse; N-burst for real numbers) */
static unsigned char g_fbbuf[FBW*FBH];   /* column-major chunky buffer: [col*FBH+row] = global colour index */
static unsigned short g_fbcw[FB_PAL_N];  /* global index -> fix cell word */
static unsigned char g_fb_texcol[NTEXTILE], g_fb_flatcol[VSFLAT_NFLAT];   /* per-surface representative -> nearest FB_PAL index */
static unsigned char g_fb_rowlvl[FBH];   /* per-FB-row fog level for floor/ceiling (perspective distance by row -> visible depth gradient) */
static int g_fb_built=0;
#define FB_R5(w) (((((w)>>8)&0xF)<<1)|(((w)>>14)&1))   /* NeoGeo RGB444 word -> 5-bit channel (shared dark LSB), matches ngc()/pack444 */
#define FB_G5(w) (((((w)>>4)&0xF)<<1)|(((w)>>13)&1))
#define FB_B5(w) ((((w)&0xF)<<1)|(((w)>>12)&1))
static int fb_nearest(int r,int g,int b){
    int bi=0,bd=1<<30;
    for(int i=0;i<FB_PAL_N;i++){ int w=FB_PAL[i],dr=r-FB_R5(w),dg=g-FB_G5(w),db=b-FB_B5(w),d=dr*dr+dg*dg+db*db; if(d<bd){bd=d;bi=i;} }
    return bi;
}
static void fb_build_cols(void){   /* avg each texture/flat palette (idx 1..15) -> nearest global colour. Built ONCE (map-independent: TEXPAL16/VSFLAT_PAL16 are global). ~0.1s one-time hitch. */
    if(g_fb_built) return; g_fb_built=1;
    for(int t=0;t<NTEXTILE;t++){ int r=0,g=0,b=0; for(int i=1;i<16;i++){ int w=TEXPAL16[t][i]; r+=FB_R5(w);g+=FB_G5(w);b+=FB_B5(w);} g_fb_texcol[t]=(unsigned char)fb_nearest(r/15,g/15,b/15); }
    for(int f=0;f<VSFLAT_NFLAT;f++){ int r=0,g=0,b=0; for(int i=1;i<16;i++){ int w=VSFLAT_PAL16[f][i]; r+=FB_R5(w);g+=FB_G5(w);b+=FB_B5(w);} g_fb_flatcol[f]=(unsigned char)fb_nearest(r/15,g/15,b/15); }
    for(int r=0;r<FBH;r++){ int dy=r*8+4-VS_HOR; if(dy<0)dy=-dy; int dist=(dy<2)?9999:(VS_EYE*VS_FOCAL)/dy; int lv=dist>>FB_FOG_SHIFT; if(lv>31)lv=31; g_fb_rowlvl[r]=(unsigned char)lv; }   /* floor/ceiling fog by ROW distance (bright at the top/bottom edges -> dark at the horizon) */
}
static void fb_vspan(unsigned char *fbc,int y0,int y1,int ct,int cb,int col){   /* fill column rows [y0,y1) clamped to the open span [ct,cb] + the 3D area, quantised to 8px FB rows */
    if(y0<ct)y0=ct; if(y1>cb+1)y1=cb+1; if(y0<0)y0=0; if(y1>192)y1=192;
    if(y1<=y0)return;
    int r0=y0>>3,r1=y1>>3; if(r1>FBH)r1=FBH;
    for(int r=r0;r<r1;r++) fbc[r]=(unsigned char)col;
}
static void fb_vspan_rowfog(unsigned char *fbc,int y0,int y1,int ct,int cb,int base){   /* floor/ceiling: fog each row by its perspective DISTANCE (g_fb_rowlvl) -> the surface darkens into the distance */
    if(y0<ct)y0=ct; if(y1>cb+1)y1=cb+1; if(y0<0)y0=0; if(y1>192)y1=192;
    if(y1<=y0)return;
    int r0=y0>>3,r1=y1>>3; if(r1>FBH)r1=FBH;
    for(int r=r0;r<r1;r++) fbc[r]=PAL_COLORMAP[g_fb_rowlvl[r]][base];
}
static void fb_upload_pals(void){
    for(int g=0;g<8;g++){
        MMAP_PALBANK1[(FB_FIXPAL+g)*16]=0x8000;                           /* index 0 = transparent (unused) */
        for(int w=0;w<15;w++){ int gi=g*15+w; if(gi<FB_PAL_N) MMAP_PALBANK1[(FB_FIXPAL+g)*16+1+w]=FB_PAL[gi]; } }
    for(int G=0;G<FB_PAL_N;G++)
        g_fbcw[G]=(unsigned short)(((FB_FIXPAL+G/15)<<12)|((FB_SOLID_TILE_BASE+G%15)&0xFFF));
}
static void fb_blit(void){
    g_vrambusy=1;
    for(int c=0;c<FBW;c++){
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+c*32+FB_ROW0;           /* MOD=1 = step DOWN the column */
        const unsigned char *s=&g_fbbuf[c*FBH];
        for(int r=0;r<FBH;r++) *REG_VRAMRW=g_fbcw[s[r]]; }
    g_vrambusy=0;
}
static void fb_render(int px,int py,int ang){
    vs_eye=vs_floor_at(px,py)+VS_EYE;          /* emit=0 skips the eye-height update at vs_render -> set it here */
    for(int c=0;c<FBW;c++){ unsigned char *cc=&g_fbbuf[c*FBH]; int r=0; for(;r<24;r++)cc[r]=FBC_SKY; for(;r<FBH;r++)cc[r]=FBC_FLOOR; }   /* init: sky/backdrop on top (openings to void/sky), floor in the rows below the 3D area; the walk overwrites covered cells */
    vs_render(px,py,ang,0);                      /* WALK: vs_render_seg fills g_fbbuf per-seg front-to-back with the LIVE clip (see-through openings), NO SCB/LUT emit (early-out at the !emit return) */
    fb_blit();
}
/* ===== end FB SPIKE ===== */
/* SEAMLESS FLOOR + CEILING: the strip engine's baked perspective hex LUT (FLOORLUT/CEILLUT),
   drawn at sprites 1..40 (BEHIND the walls at 41+). Ceiling 1..20, floor 21..40; mirror-folded
   13->24 headings; depth-shaded via FLOORPAL/CEILROWPAL; phase scrolls the periodic floor with
   movement. Sky columns skip the ceiling (backdrop sky shows). The wheel, reused. */
/* resolve a flat SLOT -> absolute base tile for the current view (angle a64, scroll phase sph);
   *pal gets the compacted palette slot. scroll=1 floor (flows with translation), 0 ceiling (static).
   returns the synthetic-LUT base (synth) when the slot is unset/sky/unbaked. */
static inline int vs_flatres(int fs,int a64,int sph,int scroll,int synth,int synthpal,int *pal){
    if(fs<VSFLAT_NFLAT && VSFLAT_BASE[fs]>=0){ int na=VSFLAT_NA[fs], np=VSFLAT_NPH[fs];
        *pal=(g_flatpal[fs]>=0)?g_flatpal[fs]:synthpal;
        return VSFLAT_BASE[fs]+(((a64*na)>>6)*np+(scroll?(sph&(np-1)):0))*VSFLAT_ROWS*VSFLAT_COLS; }
    *pal=synthpal; return synth;
}
static void vs_lut(int ang){
    int phase=(((g_fwdph)>>3)&(VSFLOOR_NPHASE-1));     /* periodic floor/ceiling flow driven by FORWARD distance (g_fwdph) not absolute px+py -> scrolls the same way for every heading + stays put on pure turns (no pop). >>3 keeps the old per-unit cadence. (#40) */
    int fset=(ang>>1)%VSFLOOR_NA;                          /* fold: 128 half-units / VSFLOOR_NA sets. NA=64 -> 2-fold (180deg period); NA=32 was 4-fold (90deg). No mirror -- 180deg ROTATIONAL period (so an h/v-asymmetric base flat repeats every half-turn, not every quarter). */
    int cset=ang%VSCEIL_NA;                                /* FINER 180deg fold: NA=128 over 180deg, indexed by the FULL heading (1.4deg/step = half the turn-jitter); was (ang>>1)%64 = 2.8deg/step */
    int fbase0=VSFLOOR_TILE0 + (fset*VSFLOOR_NPHASE+phase)*VSFLOOR_ROWS*VSFLOOR_COLS;    /* synthetic floor fallback */
    int cbase0=VSCEIL_TILE0 + (cset*VSCEIL_NPHASE + phase)*VSCEIL_ROWS*VSCEIL_COLS;  /* synthetic ceiling fallback -- phase (was phase>>2, which collapsed to 0 with NPHASE=4 = no scroll); now scrolls like the floor (gen=1) */
    int a64=ang&63, sph=(g_fwdph>>3);   /* per-column flat 90deg fold + scroll-phase source: forward-distance accumulator (g_fwdph) not absolute px+py -> consistent flow direction, turn-invariant (#40) */
    int cskip=g_vpt>>4;                                   /* LETTERBOX: skip ceiling tiles above the band top (g_vpt = full 0, or the V-viewport top) */
    int fkeep=g_floor_rows;                                /* floor rows drawn (LOD NEAR/MID/FAR; 0 => parked) */
    int crow=g_ceil_rows; if(crow>VSCEIL_ROWS-cskip)crow=VSCEIL_ROWS-cskip;   /* ceiling rows drawn (LOD) */
    int ccyf=(496-cskip*16)&0x1FF;
    for(int k=0;k<FLOORLUT_COLS;k++){
        int sx=k*16; int wc=((unsigned short)(sx+8)*(unsigned short)g_colrcp)>>16; if(wc>g_ncol-1)wc=g_ncol-1;   /* map each 16px LUT block to the wall sub-column at its CENTRE (sx+8), not the left edge -> halves the flat-overspill at sector boundaries when colw<16 (col32/64/80). No-op at col20 (16px blocks map 1:1). */
        if(wc<g_vpl||wc>g_vpr){ vs_park(1+k); vs_park(21+k); continue; }   /* H VIEWPORT: this block falls outside the column window -> backdrop (letterbox left/right) */
        int fbase=fbase0, fpal=13, cbase=cbase0, cpal=12;     /* SINGLE-flat (non-zonal) bases = nearest seg's front flat, blanketed; default synthetic floor(13)/ceiling(12) */
#if VS_FLATS
        if(!g_generic){   /* GENERIC mode: skip real flats -> fbase/cbase stay synthetic (fbase0/cbase0) */
        fbase=vs_flatres(vs_ffl[wc],a64,sph,1,fbase0,13,&fpal);   /* nearest-seg FRONT floor flat; un-latched (0xFF) -> synthetic (vs_flatres). TAKE NO ACTION on unresolved cells (2026-06-23: the RHS-wrong was incomplete-BSP geometry, not a fallback choice -- camera flat mis-painted it, murk darkened half the screen; original synthetic is the honest baseline). */
        cbase=vs_flatres(vs_cfl[wc],a64,sph,1,cbase0,12,&cpal);   /* nearest-seg FRONT ceil flat (scroll=1); un-latched -> synthetic */
        }
#endif
        int ceilcov=0; if(g_lutcull){ int cbt=cskip*16, cbb=(cskip+crow)*16, cla=((unsigned short)sx*(unsigned short)g_colrcp)>>16, clb=((unsigned short)(sx+15)*(unsigned short)g_colrcp)>>16; if(cla>g_ncol-1)cla=g_ncol-1; if(clb>g_ncol-1)clb=g_ncol-1; ceilcov=1; for(int cc=cla;cc<=clb;cc++) if(vs_ctop[cc]>cbt || vs_cbot[cc]<cbb){ceilcov=0;break;} }   /* LUT-CULL (conservative, full-occlusion ONLY): park a ceiling block ONLY if EVERY column's CLOSING solid strip [vs_ctop,vs_cbot] (the CLIPPED y0/y1 of the wall that closed the column) SPANS the whole band: top at/above cbt AND bottom at/below cbb. Both ends needed -- top alone over-parks a wall clipped short by a nearer opening BELOW it (the open ceiling between y1 and cbb shows through). 2-sided/open columns keep init -> never park. */
        if(g_ceil_rows>0 && !ceilcov){
#ifdef VS_NOSKY
        if(1){                                             /* DIAG: force-draw ceiling, ignore vs_sky (isolate the black-ceiling cause) */
#else
        if(vs_sky[wc]==2){ vs_park(1+k); }                  /* FAR-CULLED sky (dd): backdrop, not the always-on panorama -> sky obeys draw-distance like walls */
        else if(!vs_sky[wc]){                               /* ceiling 1+k: rows cskip.., top-anchored at the band top */
#endif
            int cend=cskip+crow; if(g_hclip){ int cl=vs_clY[wc]>>4; if(cl<cend)cend=cl; if(cend<=cskip)cend=cskip+1; }   /* HEIGHT-CLIP (param 28): clip ceiling to nearest wall top. DEAD-END for overspill (R2): guillotines far ceiling legitimately seen OVER near walls. Real fix = per-region flat (zonal). Kept as the old band-aid toggle. */
            *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+(1+k)*64;
            for(int r=cskip;r<cend;r++){ int cb_=cbase, cp=cpal;   /* INC2: the BAKED depth-fade vsceil tiles + bank 12 ramp (vs_ceil_pal) carry the smooth 15-level gradient -> no runtime band murk. cmap(14) A/Bs ramp-vs-flat via the bank palette; gen0 keeps the real flat palette (cpal from vs_flatres). */
                if(g_zonal && !g_generic) cb_=vs_flatres((vs_cdep[k][r]<0x7FFF)?vs_cfr[k][r]:0xFF,a64,sph,1,cbase0,12,&cp);   /* ZONAL ceil: stamped -> real flat; unstamped -> synthetic (no action -- the RHS-wrong is incomplete-BSP geometry, not the fallback). */
                int T=cb_+(VSCEIL_ROWS-1-r)*VSCEIL_COLS+k;   /* square fold: no mirror */
                if(g_flatdbg){ int cid=(g_zonal&&!g_generic)?((vs_cdep[k][r]<0x7FFF)?vs_cfr[k][r]:0xFF):vs_cfl[wc]; T=PLACEHOLDER_TILE; cp=TEXBASE+(cid&7); }   /* FLAT-SOURCE DEBUG (param 32): solid colour keyed by the ceiling flat ID */
                *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((cp<<8)|(((T>>16)&0xF)<<4)|0x02); }   /* per-column ceil pal + VFLIP (ceiling = floor-cast flipped) */
            *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+(1+k);
            *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((ccyf<<7)|(cend-cskip)); *REG_VRAMRW=(u16)(sx<<7);
        } else if(SKY_TEX>=0){                            /* SKY column: real SKY1 panorama, hscrolled by view angle, upright at infinity */
            int swt=TEXWT[SKY_TEX], sth=TEXHT[SKY_TEX], spal=TEXBASE+(vs_lpal[SKY_TEX]<0?0:vs_lpal[SKY_TEX]);   /* per-map compacted sky slot */
            int stc=(((k-(ang>>2))%swt)+swt)%swt;
            *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+(1+k)*64;
            int sr=cskip%sth;
            for(int r=cskip;r<cskip+crow;r++){ int T=FIRST_TEX_TILE+TEXTBASE[SKY_TEX]+sr*swt+stc;
                *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((spal<<8)|(((T>>16)&0xF)<<4)); sr++; if(sr>=sth)sr=0; }
            *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+(1+k);
            *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((ccyf<<7)|crow); *REG_VRAMRW=(u16)(sx<<7);
        } else { *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+(1+k); *REG_VRAMRW=0x0001; *REG_VRAMRW=0; *REG_VRAMRW=0; }
        } else vs_park(1+k);                               /* ceiling OFF -> backdrop above walls */
        /* floor 21+k: rows [fskip,fhi), anchored at the horizon (y112 -> NG y 384). fskip=flod (drops FAR/horizon rows, keeps NEAR); fhi=V-VIEWPORT clip (drops NEAR rows below the band bottom g_vpb) */
        int fskip=5-fkeep; if(fskip<0)fskip=0;   /* full-res band = 5 floor rows (y112..192); keep NEAR rows, cull FAR (horizon) rows for LOD */
        int fhi=(g_vpb-112)>>4; if(fhi>5)fhi=5;  /* V VIEWPORT: clip the floor's bottom to the band (snaps to 16px rows) */
        int floorcov=0; if(g_lutcull){ int fbt=112+fskip*16, fbb=112+fhi*16, fla=((unsigned short)sx*(unsigned short)g_colrcp)>>16, flb=((unsigned short)(sx+15)*(unsigned short)g_colrcp)>>16; if(fla>g_ncol-1)fla=g_ncol-1; if(flb>g_ncol-1)flb=g_ncol-1; floorcov=1; for(int cc=fla;cc<=flb;cc++) if(vs_ctop[cc]>fbt || vs_cbot[cc]<fbb){floorcov=0;break;} }   /* LUT-CULL (conservative): symmetric -- park a floor block ONLY if EVERY column's closing solid strip SPANS the floor band: top at/above fbt AND bottom at/below fbb. Both ends needed (a wall clipped short by a nearer opening leaves open floor). 2-sided/open columns keep init -> never park. */
        if((g_zonal==2||g_zonal==3||g_zonal==5) && !g_generic && g_cam_ffl!=0xFF){   /* BOUNDARY BIAS (zon=2/3, + zon=5 atop the deterministic round): shift the contested main/pit boundary by ONE 16px row. zon=4 = round only, NO bias. */
            int top=5; for(int r=4;r>=0;r--){ int fid=(vs_fdep[k][r]>=0)?vs_ffr[k][r]:0xFF; if(fid==(unsigned char)g_cam_ffl)top=r; else break; }   /* top = highest row of the contiguous bottom camera-floor band */
            if(top>=1 && top<=4){ int up=top-1; int fu=(vs_fdep[k][up]>=0)?vs_ffr[k][up]:0xFF;
                if(fu!=0xFF && fu!=(unsigned char)g_cam_ffl){   /* a pit cell sits directly above the camera band -> the boundary is here */
                    if(g_zonal!=3){ vs_ffr[k][up]=(unsigned char)g_cam_ffl; vs_fdep[k][up]=vs_fdep[k][top]; }       /* zon=2 & zon=5: pit row -> grey (boundary leans UP, grey-into-pit, blue never out) */
                    else          { vs_ffr[k][top]=(unsigned char)fu;        vs_fdep[k][top]=vs_fdep[k][up];  }      /* zon=3: top grey row -> pit (boundary leans DOWN, pit-into-grey, the opposite lean) */
                } }
        }
        if(g_floor_rows>0 && fhi>fskip && !floorcov){
        int fstart=fskip; if(g_hclip){ int fl=(vs_flY[wc]-112)>>4; if(fl>fstart)fstart=fl; if(fstart>=fhi)fstart=fhi-1; }   /* HEIGHT-CLIP (param 28): clip floor to nearest wall bottom. Same DEAD-END as the ceiling (R2). Real fix = per-region flat (zonal). Kept as the old band-aid toggle. */
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+(21+k)*64;
        for(int r=fstart;r<fhi;r++){ int fb=fbase, vv=fskip+g_vpw-r, fp=(g_generic && r<fskip+g_vpw)?((vv*3>2*g_vpw)?14:(vv*3>g_vpw?10:11)):fpal;   /* FLOOR MURK (param 18): far g_vpw rows = 3-band gradient -> 11 (0.90x) -> 10 (0.78x) -> 14 (0.45x deep). gen0: keep the REAL floor flat's own palette (don't stomp with synthetic murk slots). */
            if(g_zonal && !g_generic) fb=vs_flatres((vs_fdep[k][r]>=0)?vs_ffr[k][r]:0xFF,a64,sph,1,fbase0,13,&fp);   /* ZONAL floor: stamped -> real flat; unstamped -> synthetic (no action -- the RHS-wrong is incomplete-BSP geometry, not the fallback). */
            int T=fb+r*VSFLOOR_COLS+k;     /* square fold: no mirror */
            if(g_flatdbg){ int fid=(g_zonal&&!g_generic)?((vs_fdep[k][r]>=0)?vs_ffr[k][r]:0xFF):vs_ffl[wc]; T=PLACEHOLDER_TILE; fp=TEXBASE+(fid&7); }   /* FLAT-SOURCE DEBUG (param 32): solid colour keyed by the floor flat ID -> see pit-blue bleeding onto the main floor */
            *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((fp<<8)|(((T>>16)&0xF)<<4)); }   /* per-column floor pal (real flat) */
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+(21+k);
        *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((((384-fstart*16)&0x1FF)<<7)|(fhi-fstart)); *REG_VRAMRW=(u16)(sx<<7);
        } else vs_park(21+k);                              /* floor toggle OFF / clipped away -> backdrop below walls */
        if(g_zonal>=6 && g_floor_rows>0 && !g_generic){    /* zon>=6 SUB-TILE BOUNDARY: at a floor flat transition, overlay the ABOVE flat down to the EXACT edge (vs_fbndy) -> the 16px row snap becomes the true sub-row = smooth diagonal across columns. ~1 fresh slot per boundary block. */
            for(int r=fskip+1;r<fhi;r++){
                if(vs_fdep[k][r]<0||vs_fdep[k][r-1]<0) continue;            /* both rows real-stamped */
                if(vs_ffr[k][r]==vs_ffr[k][r-1]) continue;                 /* same flat -> no boundary */
                int by=vs_fbndy[k][r]; if(by<0) continue;
                int oh=by-(112+r*16); if(oh<1||oh>15) continue;            /* sub-tile slice within row r only */
                if(vs_spr>=379) break;                                     /* SCB backstop */
                int ap; int ab=vs_flatres(vs_ffr[k][r-1],a64,sph,1,fbase0,13,&ap);   /* the ABOVE flat (its row-r perspective tile) */
                int spr=vs_spr++; int T=ab+r*VSFLOOR_COLS+k; int vsh=(oh*255)>>4; if(vsh<1)vsh=1; if(vsh>255)vsh=255;
                *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
                *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((ap<<8)|(((T>>16)&0xF)<<4));
                vs_scb_clear_tail(spr,1,ap);
                *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
                *REG_VRAMRW=(u16)((15<<8)|vsh); *REG_VRAMRW=(u16)((((384-r*16)&0x1FF)<<7)|1); *REG_VRAMRW=(u16)(sx<<7);
            }
        }
    }
}
static int g_vproj=1;    /* (28, repurposed from the dead hclp) PER-VERTEX PROJECTION CACHE: reuse a seg's v1 projection when v1==the previous seg's v2 (the subsector-loop common case) -> skips the 4 v1 rotation MULs. A/B toggle. */
static int g_vcx=0x40000000, g_vcy=0, g_vcd=0, g_vcs=0;   /* previous seg's v2 raw coord + its raw projection (depth, lateral); reset each frame (camera-dependent) */
/* render ONE BSP seg into the per-column clip (called front-to-back from the traversal) */
static void vs_render_seg(int s){
    if(vs_emit && vs_spr-41>=(g_dpri?VS_SBUFN:g_vs_budget)){ VSCULL(bud); vs_open=0; return; }   /* RECORD cap: dpri records up to the buffer (then the burst keeps the nearest bud by depth); !dpri halts at the user budget (old walk-order behaviour). vs_open=0 stops the walk. */
    short ax=(short)(ve_x0[s]-vs_px), ay=(short)(ve_y0[s]-vs_py), bx=(short)(ve_x1[s]-vs_px), by=(short)(ve_y1[s]-vs_py);
    int vchit=g_vproj && ve_x0[s]==g_vcx && ve_y0[s]==g_vcy;   /* VPROJ: v1 == previous seg's v2 -> reuse its raw projection (skip the 4 v1 MULs) */
    int ad=vchit?g_vcd:(int)(((long)ax*vs_fcs+(long)ay*vs_fsn)>>8), bd=(int)(((long)bx*vs_fcs+(long)by*vs_fsn)>>8);
    if(ad<g_vs_near && bd<g_vs_near){ VSCULL(near); return; }   /* near-clip dial (param 36) */
    int aS=vchit?g_vcs:(int)(((long)ax*vs_fsn-(long)ay*vs_fcs)>>8), bS=(int)(((long)bx*vs_fsn-(long)by*vs_fcs)>>8);
    if(g_vproj){ g_vcx=ve_x1[s]; g_vcy=ve_y1[s]; g_vcd=bd; g_vcs=bS; }   /* cache this seg's v2 raw projection for the next seg's v1 */
    if((long)(short)aS*VS_FOCAL >  160L*ad && (long)(short)bS*VS_FOCAL >  160L*bd){ VSCULL(frus); return; }
    if((long)(short)aS*VS_FOCAL < -160L*ad && (long)(short)bS*VS_FOCAL < -160L*bd){ VSCULL(frus); return; }
    if(g_radial){ long R2=(long)g_murk_eff*g_murk_eff; if((long)ad*ad+(long)aS*aS>R2 && (long)bd*bd+(long)bS*bS>R2){ VSCULL(frus); return; } }   /* RADIAL (param 23): skip segs WHOLLY beyond the EUCLIDEAN far-cull BEFORE projection. NOTE: costs 4 MULS on EVERY seg, only culls the wholly-far minority -> net-slower in practice (see g_radial). */
    int Ua=ve_u0[s], Ub=ve_u0[s]+ve_ulen[s];   /* WALL-U (tex px) at vertex A (v1) and B (v2); near-clip slides them with aS/bS */
    if(ad<g_vs_near){ int den=bd-ad; if(den<=0)return; int t=((g_vs_near-ad)<<8)/den; aS+=(int)(((long)(short)(bS-aS)*(short)t)>>8); Ua+=(int)(((long)(Ub-Ua)*t)>>8); ad=g_vs_near; }
    else if(bd<g_vs_near){ int den=ad-bd; if(den<=0)return; int t=((g_vs_near-bd)<<8)/den; bS+=(int)(((long)(short)(aS-bS)*(short)t)>>8); Ub+=(int)(((long)(Ua-Ub)*t)>>8); bd=g_vs_near; }
    int rfa=(ad<VS_RFMAX)?g_rf[ad]:(int)(((long)VS_FOCAL<<8)/ad);   /* 1/depth: LUT (near) or divide (far, rare/undrawn) */
    int rfb=(bd<VS_RFMAX)?g_rf[bd]:(int)(((long)VS_FOCAL<<8)/bd);
    int sxa=VS_HALF+(int)(((long)(short)aS*(short)rfa)>>8), sxb=VS_HALF+(int)(((long)(short)bS*(short)rfb)>>8);
    if(sxa>=sxb){ VSCULL(back); return; }         /* back-facing seg (BSP segs are one-directional) */
    if(sxb<0 || sxa>319){ VSCULL(off); return; }
    int sa=sxa<0?0:sxa, sb=sxb>320?320:sxb;        /* clamp to [0,320] so the (unsigned short) reciprocal-MUL can't overflow (sxa/sxb are off-screen-unbounded ints) */
    int ca=((unsigned short)sa*(unsigned short)g_colrcp)>>16; if(ca>g_ncol-1)ca=g_ncol-1; int cb=((unsigned short)sb*(unsigned short)g_colrcp)>>16; if(cb>g_ncol-1)cb=g_ncol-1;
    { int op=0; for(int c=ca;c<=cb;c++) if(vs_ct[c]<=vs_cb[c]){op=1;break;} if(!op){ VSCULL(occ); return; } }   /* #49 OCCLUSION PRE-REJECT (HORIZONTAL): every column this seg covers is already fully CLOSED by a nearer wall -> it can draw NOTHING. Bail BEFORE the ~30 height-MULs + the 3 pegging DIVs (910-912). Front-to-back guarantees nothing nearer follows; a fully-closed column can't host flats/vs_wdep either, so nothing is lost. Visible/near segs hit an open column at c=ca and exit the scan O(1); only fully-occluded far segs walk the whole span -- and they skip the entire projection. THE win at high dd (whole-map views where most far segs sit behind near walls). */
    int fsec=ve_fsec[s], bsec=ve_bsec[s], bv=(bsec<g_nsec);
    int dcf=g_secdc[fsec], dff=g_secdf[fsec], dcb=bv?g_secdc[bsec]:0, dfb=bv?g_secdf[bsec]:0;   /* per-sector ceiling(door)/floor(lift) deltas; 0 = at rest */
    int fc=ve_fc[s]+dcf, ff=ve_ff[s]+dff, bc=ve_bc[s]+dcb, bff=ve_bf[s]+dfb, fl=ve_flag[s];   /* DOORS: +ceiling raise; LIFTS: +floor raise */
    int pdyn=dcf|dff|dcb|dfb;   /* any delta nonzero -> this seg's pegging must recompute live; else use the map-load bake */
    int two=fl&1, mt=ve_mt[s], ut=ve_ut[s], lt=ve_lt[s];
    int isposter = (mt>=0 && ve_ulen[s]<=TEXWT[mt]);   /* SINGLE-COPY seg (poster/sign): the wall's U-span is <= one texture width, so it isn't tiled -> safe to perspective/companion-map. Tiling walls (U-span > 1 width) must stay screen-mapped/stretched (the affine swim shows on repeats). Drives both HMAP mode 2 (horizontal) and VMAP mode 2 (vertical). */
    int persp = (g_hmap==1) || (g_hmap==2 && isposter);   /* HMAP: 1 = perspective wall-U everywhere, 2 = SELECTIVE (posters only), 0 = screen-mapped everywhere. */
    short scA=(short)rfa, scB=(short)rfb;          /* = (FOCAL<<8)/depth, exact via the reciprocal LUT (was 2 DIVS.W) */
    short fcE=(short)(fc-vs_eye), ffE=(short)(ff-vs_eye), bcE=(short)(bc-vs_eye), bfE=(short)(bff-vs_eye);
    int ytFa=VS_HOR-(int)(((long)fcE*scA)>>8), ybFa=VS_HOR-(int)(((long)ffE*scA)>>8);
    int ytFb=VS_HOR-(int)(((long)fcE*scB)>>8), ybFb=VS_HOR-(int)(((long)ffE*scB)>>8);
    /* VERTICAL BAND-REJECT (#49 in the OTHER axis). #49 above proved SOME column is still open; this proves
       the seg's PROJECTED HEIGHT misses every open column's surviving [vs_ct..vs_cb] sliver. A near window
       pinches each covered column down to a thin band but leaves it OPEN, so a far seg behind the window
       survives the horizontal #49, then projects a near-full-height bar that lands entirely above/below the
       sliver -> it would pay the 3 pegging DIVs + the reciprocal-span + the whole per-column interp loop to
       emit 0px. segt/segb = the seg's conservative vertical extent (the front face bounds upper+lower+sky for
       two-sided). SAFE: a column only has a PINCHED band because a NEARER seg pinched it, and that nearer seg
       already latched the column's flat (vs_skd) + occluder (vs_wdep) -- so a band-rejected far seg provably
       contributes nothing. Breaks O(1) on the common visible seg (first open column overlaps at c=ca). */
    if(g_bandclip){ int segt=ytFa<ytFb?ytFa:ytFb, segb=ybFa>ybFb?ybFa:ybFb, hit=0;
      for(int c=ca;c<=cb;c++) if(vs_ct[c]<=vs_cb[c] && vs_cb[c]>=segt && vs_ct[c]<=segb){ hit=1; break; }
      if(!hit){ g_seg_clipped++; return; } }
    g_seg_n++;                                     /* perf: a seg that survives BOTH pre-rejects -> full per-column projection */
    int ytOa=0,ybOa=0,ytOb=0,ybOb=0;
    if(two){ ytOa=VS_HOR-(int)(((long)bcE*scA)>>8); ybOa=VS_HOR-(int)(((long)bfE*scA)>>8);
             ytOb=VS_HOR-(int)(((long)bcE*scB)>>8); ybOb=VS_HOR-(int)(((long)bfE*scB)>>8); }
    int span=sxb-sxa; if(span<1)span=1;   /* sa/sb/ca/cb now computed UP-FRONT (before projection) for the #49 occlusion pre-reject */
    long rs=(span<VS_SPANMAX)?(long)g_rspan[span]:(65536L/span);   /* 1/span reciprocal (LUT; divide only for span>=1024, extreme near walls) */
    int df=(int)(((long)(g_colw<<8)*rs)>>16), f=(int)(((long)((ca*g_colw+(g_colw>>1))-sxa)*rs)>>8);   /* x-step + start fraction, via the reciprocal (was 2 DIVS) */
    /* per-column edge slopes (screen-px of edge-drop per g_colw-px COLUMN, L->R) -- only needed by the caps;
       4 divides/seg. Runtime-gated on g_capmode: OFF -> slopes stay 0 (skips the divides = lean) and
       vs_strip's rtop/rbot also gate on g_capmode, so no cap tile is drawn either way. SCALE BY g_colw,
       NOT 16: the cap ramp tile (baked to drop N px across its 16px width) is h-shrunk to the column width,
       so its on-screen drop must equal the wall edge's drop ACROSS ONE g_colw-px column = slope*g_colw.
       (At col20 g_colw==16 so this is the old behaviour bit-for-bit.) */
    int dyt=0, dyb=0, dytO=0, dybO=0;
    if(g_capmode){
        dyt=(int)(((long)((ytFb-ytFa)*g_colw)*rs)>>16); dyb=(int)(((long)((ybFb-ybFa)*g_colw)*rs)>>16);
        dytO=two?(int)(((long)((ytOb-ytOa)*g_colw)*rs)>>16):0; dybO=two?(int)(((long)((ybOb-ybOa)*g_colw)*rs)>>16):0;
    }
    /* V companion 2b -- DOOM vertical pegging per section (TILE-quantized; voff = texture tile-row sitting at the section TOP).
       peg bit0=DONTPEGTOP, bit1=DONTPEGBOTTOM; yo_t = sidedef rowoffset in tiles (16px-quantized, so sub-16px offsets vanish). */
    int mvoff, uvoff, lvoff;
    /* V companion 2b -- DOOM vertical pegging per section. CAMERA-INDEPENDENT: depends only on peg flags, rowoffset,
       texture height + the section heights (fc-ff etc.). Those move ONLY when a door/lift is mid-cycle on the seg's
       sector -> baked at map load (vs_bake_pegging) and used wholesale unless pdyn. Removes 3 %th DIVs/seg/frame.
       bottom-peg b = texture tile-row at the section TOP = floor((th*16 - sectionHeight)/16) mod th; top-peg b = 0. */
    if(!pdyn){ mvoff=ve_mvoff[s]; uvoff=ve_uvoff[s]; lvoff=ve_lvoff[s]; }   /* FAST PATH: sectors at rest -> use the bake (bit-identical to the live formula with deltas=0) */
    else { int peg=ve_peg[s], yo_t=ve_yoff[s]>>4; mvoff=uvoff=lvoff=0;       /* door/lift moving this sector -> recompute live */
      if(mt>=0){ int th=TEXHT[mt]; if(th>0){ int b=(peg&2)?((((th<<4)-(fc-ff))>>4)%th):0; mvoff=(((b+yo_t)%th)+th)%th; } }            /* one-sided middle: peg TOP by default; DONTPEGBOTTOM -> texture bottom at the floor */
      if(two && ut>=0){ int th=TEXHT[ut]; if(th>0){ int b=(peg&1)?0:((((th<<4)-(fc-bc))>>4)%th); uvoff=(((b+yo_t)%th)+th)%th; } }    /* upper: peg BOTTOM by default; DONTPEGTOP -> peg top */
      if(two && lt>=0){ int th=TEXHT[lt]; if(th>0){ int b=(peg&2)?(((fc-bff)>>4)%th):0; lvoff=(((b+yo_t)%th)+th)%th; } } }          /* lower: peg TOP at the opening floor by default; DONTPEGBOTTOM -> hang from the front ceiling */
    int pwf=0, pwfs=0, psub=0;   /* PHASE 5 subdivide-affine perspective state (resets per seg): pwf=current 1/z fraction, pwfs=per-col slope, psub=cols until the next EXACT resample */
    for(int c=ca;c<=cb;c++, f+=df){
        short ff2=(short)f; if(ff2<0)ff2=0; if(ff2>256)ff2=256;
        int wf=0;
        if(g_hmap==3 || g_vmap==3){   /* PHASE 5: advance the subdivide-affine 1/z interpolant EVERY column -> it stays locked to SCREEN-X (not the visible-column count), so a wall peeking past a near occluder doesn't swim (the desync the verify caught). */
            if(psub<=0){   /* resample the EXACT 1/z fraction (one 32-bit divide) only every g_pstepv cols -- was a divide EVERY col = the hidden per-column tax, un-throttled by any dial */
                long den=(long)(256-ff2)*rfa+(long)ff2*rfb; int w0=den?(int)((((long)ff2*rfb)<<8)/den):ff2;   /* exact 1/z fraction HERE */
                if(g_pstepv>1 && cb-c>=1){ long fl2=(long)f+(long)g_pstepv*df; if(fl2<0)fl2=0; if(fl2>256)fl2=256; int ffl=(int)fl2;   /* ff2 g_pstepv cols ahead, clamped in LONG space (no short-cast wrap on tiny spans) */
                    long den2=(long)(256-ffl)*rfa+(long)ffl*rfb; int w1=den2?(int)((((long)ffl*rfb)<<8)/den2):ffl;
                    pwfs=(w1-w0)>>g_pstepsh; }                              /* affine slope to the next sample (df>0 here -> pwfs>=0) */
                else pwfs=0;                                               /* PSTEP=1, OR no next col in this seg (1-col / last-col): SKIP the lookahead divide -> a sliver / narrow-col-mode seg pays 1 divide not 2 (to preserve the benefit at ~20 cols) */
                pwf=w0; psub=g_pstepv;
            }
            wf=pwf; pwf+=pwfs; psub--;                                     /* hold this col's 1/z fraction; advance the interpolant for the next col */
        }
        if(vs_ct[c]>vs_cb[c]) continue;
        int ytF=ytFa+(((long)(short)(ytFb-ytFa)*ff2)>>8), ybF=ybFa+(((long)(short)(ybFb-ybFa)*ff2)>>8);
        int dC=ad+(((long)(short)(bd-ad)*ff2)>>8);   /* AFFINE depth: drives vis/far-cull, the zonal stamp, vs_wdep occlusion -- all want the cheap affine. wf (perspective) is computed above, only when hmap3/vmap3. */
        if(g_radial && dC>g_murkcol[c]){ vs_ct[c]=vs_cb[c]+1; vs_open--; continue; }   /* RADIAL v2: nearest wall in this column is beyond the radial reach -> CLOSE the column (end its walk) instead of just hiding the strip. Front-to-back guarantees nothing nearer follows; vs_lut (floor/ceil) is independent of vs_ct/vs_cb so the near floor/ceil still draw + fade via the murk gradient. THIS is what makes radial save the walk. */
        int vis = vs_emit && !g_noemit && dC<=(g_radial?g_murkcol[c]:g_murk_eff) && vs_spr<41+(g_dpri?VS_SBUFN:g_vs_budget);        /* far-cull + RECORD cap (dpri records to the buffer; the burst then keeps the nearest bud by depth) */
        int tc=c, dpix=dC;   /* tc=texture column (screen-mapped default), dpix=strip depth */
        if(vis){
            if(g_hmap==3) tc=(Ua+(int)(((long)(Ub-Ua)*wf)>>8))>>4;          /* hmap3: perspective-correct wall-U (subdivided) */
            else if(persp) tc=(Ua+(int)(((long)(Ub-Ua)*ff2)>>8))>>4;        /* hmap1/2 affine wall-U */
            if(g_vmap==3) dpix=ad+(int)(((long)(short)(bd-ad)*wf)>>8);      /* vmap3: perspective depth -> level vertical tiling (subdivided) */
        }
        int just=!vs_skd[c];                          /* this seg is the NEAREST to latch column c (owns its FRONT flat + the BACK zone if two-sided) */
        if(just){ vs_clY[c]=(short)ytF; vs_flY[c]=(short)ybF; }   /* nearest wall TOP/BOTTOM screen rows -> flat height-occlusion extents (consumed by vs_lut when g_hclip) */
#if VS_SKYWIN
        if(just){ int sk=(fl&6)?1:0; vs_sky[c]=sk; vs_skd[c]=1; vs_ffl[c]=ve_ffl[s]; vs_cfl[c]=sk?0xFF:ve_cfl[s]; }   /* blanket FRONT floor (blanket-back flip reverted: it broke the MAJORITY front-visible columns; correct fix is per-column/per-row sector selection, not a blanket flip) */
#else
        if(just){ int fsky=(fl&2)?1:0; vs_sky[c]= fsky?((dC<=(g_radial?g_murkcol[c]:g_murk_eff))?1:2):0; vs_skd[c]=1; vs_ffl[c]=ve_ffl[s]; vs_cfl[c]=fsky?0xFF:ve_cfl[s]; }   /* FRONT-ceiling sky: panorama (1) if within the far horizon, else PARK-to-backdrop (2) so dd-culled sky doesn't bleed past the fog like walls do. blanket FRONT floor. */
#endif
        if(g_flatdbg && just && c==(g_ncol>>1)){ g_dbg_ffl=ve_ffl[s]; g_dbg_bfl=two?ve_bfl[s]:-1; }   /* FLAT-DBG: centre column's nearest seg -> front (picked) + back floor flat. If a main-floor cell reads ffl=pit-flat / bfl=main-flat, the seg's FRONT side is the pit (wrong sector chosen). */
        if(g_zonal && !g_generic){   /* VISPLANE STAMP -- only when the LUT actually READS it (gen=1 uses the synthetic blanket, so the stamp was pure wasted memory-bound work = the perf win). */
            short dep=(short)dC;
            int bk=(c*g_colw+(g_colw>>1))>>4; if(bk>FLOORLUT_COLS-1)bk=FLOORLUT_COLS-1;   /* col -> its 16px LUT block (the 20 cols the LUT reads). At col20 (g_colw=16) bk==c. Shared by floor + ceiling stamp. */
            int fr=(g_zonal>=4?(ybF-VS_HOR+8):(ybF-VS_HOR))>>4; if(fr<0)fr=0; if(fr>4)fr=4;   /* zon>=4: ROUND the floor edge to the NEAREST 16px row (deterministic visplane boundary) vs floor() in zon1-3 */
            int ffl_s=(fr>=4)?g_cam_ffl:ve_ffl[s];   /* CAMERA-SECTOR DEFAULT: near-band floor edge (fr==4 = the camera's level floor) takes the camera's own sector flat; a higher edge (fr<4 = a distinct stepped/lower floor) keeps the seg's own flat. Fixes the nearest-seg-is-a-far-wall mis-latch without the blanket-flip regression. */
            unsigned char *ffr_c=vs_ffr[bk]; short *fdep_c=vs_fdep[bk];   /* floor: LINE-priority (largest fr=nearest owns the rows below), BLOCK-indexed. */
            if(g_zonal>=6 && fr<5 && fdep_c[fr]<0) vs_fbndy[bk][fr]=(short)ybF;   /* zon>=6: record the EXACT floor edge of the seg about to claim row fr as its top -> sub-tile boundary at the LUT */
            for(int r=fr; r<5 && fdep_c[r]<0; r++){ ffr_c[r]=(unsigned char)ffl_s; fdep_c[r]=(short)fr; }   /* OCCUPANCY-FIRST: front-to-back, the NEAREST seg to claim a row wins. zon=2 INWARD BIAS is applied later at the LUT read (vs_lut), not here. */
            int cr=(g_zonal>=4?(ytF+8):ytF)>>4; if(cr<0)cr=0; if(cr>VSCEIL_ROWS-1)cr=VSCEIL_ROWS-1;   /* zon>=4: ROUND the ceil edge to the nearest 16px row (deterministic visplane boundary) */
            int cfl_s=(cr<=0)?g_cam_cfl:ve_cfl[s];   /* CEILING CAMERA-SECTOR DEFAULT (twin of the floor's fr>=4 at line 916): a seg whose ceiling-top reaches the very top band (cr==0 = the camera's own high ceiling) takes the camera-sector ceil flat; a stepped/lowered ceiling (cr>0, top edge lower on screen) keeps the seg's own front flat. Fixes the near far-wall ceiling mis-latch without a blanket flip. */
            unsigned char *cfr_c=vs_cfr[bk]; short *cdep_c=vs_cdep[bk];   /* ceil: DEPTH-priority, nearest wins per row (bk computed above, shared with floor) */
            for(int r=0;r<=cr;r++) if(dep<cdep_c[r]){ cfr_c[r]=(unsigned char)cfl_s; cdep_c[r]=dep; }
        }
        if(g_fb_mode==1){   /* FB SPIKE during-walk fill: paint ceiling/wall/floor into the chunky buffer using the LIVE clip [vs_ct,vs_cb] (front-to-back). Two-sided openings are left UNFILLED -> farther segs (clipped to the opening) paint the room behind = see-through. */
            int fct=vs_ct[c], fcb=vs_cb[c];
            if(fct<=fcb){
                unsigned char *fbc=&g_fbbuf[c*FBH];
                int lvl=dC>>FB_FOG_SHIFT; if(lvl>31)lvl=31;                     /* WALL fog: per-column wall depth -> light level */
                const unsigned char *fog=PAL_COLORMAP[lvl];
                int cbase=(ve_cfl[s]!=0xFF)?g_fb_flatcol[ve_cfl[s]]:FBC_SKY;    /* UNfogged ceiling/floor base -> floor/ceiling get PER-ROW depth fog (the visible gradient) */
                int fbase=(ve_ffl[s]!=0xFF)?g_fb_flatcol[ve_ffl[s]]:FBC_FLOOR;
                if(!two){
                    int wcol=fog[(mt>=0)?g_fb_texcol[mt]:FBC_WALLF];
                    fb_vspan_rowfog(fbc,fct,ytF,fct,fcb,cbase);  /* ceiling: per-row depth fog */
                    fb_vspan(fbc,ytF,ybF,fct,fcb,wcol);          /* solid wall: per-column fog */
                    fb_vspan_rowfog(fbc,ybF,fcb+1,fct,fcb,fbase);/* floor: per-row depth fog */
                } else {
                    int fytO=ytOa+(((long)(short)(ytOb-ytOa)*ff2)>>8), fybO=ybOa+(((long)(short)(ybOb-ybOa)*ff2)>>8);
                    if(fytO>=fybO){                              /* opaque (closed door) -> solid */
                        int dtex=(ut>=0)?ut:((lt>=0)?lt:mt), wcol=fog[(dtex>=0)?g_fb_texcol[dtex]:FBC_WALLF];
                        fb_vspan_rowfog(fbc,fct,ytF,fct,fcb,cbase);
                        fb_vspan(fbc,ytF,ybF,fct,fcb,wcol);
                        fb_vspan_rowfog(fbc,ybF,fcb+1,fct,fcb,fbase);
                    } else {                                     /* open: ceiling / upper wall / [opening skipped] / lower wall / floor */
                        int ucol=fog[(ut>=0)?g_fb_texcol[ut]:cbase], lcol=fog[(lt>=0)?g_fb_texcol[lt]:fbase];
                        fb_vspan_rowfog(fbc,fct,ytF,fct,fcb,cbase);   /* front ceiling: per-row */
                        fb_vspan(fbc,ytF,fytO,fct,fcb,ucol);          /* upper wall: per-column */
                        fb_vspan(fbc,fybO,ybF,fct,fcb,lcol);          /* lower wall: per-column */
                        fb_vspan_rowfog(fbc,ybF,fcb+1,fct,fcb,fbase); /* front floor: per-row */
                    }                                            /* opening [fytO,fybO) untouched -> the room behind shows through */
                }
            }
        }
        if(!two){
            int y0=ytF<vs_ct[c]?vs_ct[c]:ytF, y1=ybF>vs_cb[c]?vs_cb[c]:ybF;
            if(vis && y1-y0>=4) vs_strip(vs_spr++,c,y0,y1,mt,tc,dpix,dyt,dyb,ytF,mvoff,isposter);   /* one-sided: top=ceiling slope, bottom=floor slope; voff=mvoff (peg top default, peg bottom if DONTPEGBOTTOM) */
            if(vs_ct[c]<=vs_cb[c]) vs_open--;
            if(dC<vs_wdep[c])vs_wdep[c]=(short)dC;   /* nearest solid wall in this column -> actor occlusion threshold */
            vs_ct[c]=vs_cb[c]+1; vs_ctop[c]=(short)y0; vs_cbot[c]=(short)y1;   /* solid wall closes the column + records the CLIPPED solid-coverage extent (y0/y1, NOT ytF/ybF) for the LUT cull: y0=max(ytF,vs_ct) so a nearer see-through opening above leaves y0>0 -> ceiling not parked (the far ceiling shows through it) */
        } else {
            int ytO=ytOa+(((long)(short)(ytOb-ytOa)*ff2)>>8), ybO=ybOa+(((long)(short)(ybOb-ybOa)*ff2)>>8);
            if(ytO>=ybO){                                  /* OPAQUE: opening collapsed (closed door, back ceil<=floor) -> draw solid + CLOSE the column (no see-through, no traversal beyond) */
                int dtex=(ut>=0)?ut:((lt>=0)?lt:mt);
                int y0=ytF<vs_ct[c]?vs_ct[c]:ytF, y1=ybF>vs_cb[c]?vs_cb[c]:ybF;
                if(vis && y1-y0>=4 && dtex>=0) vs_strip(vs_spr++,c,y0,y1,dtex,tc,dpix,dyt,dyb,ytF,0,isposter);
                if(vs_ct[c]<=vs_cb[c]) vs_open--;
                if(dC<vs_wdep[c])vs_wdep[c]=(short)dC;   /* opaque (closed door) wall -> actor occlusion threshold */
                vs_ct[c]=vs_cb[c]+1; vs_ctop[c]=(short)y0; vs_cbot[c]=(short)y1;   /* opaque door closes the column -> record the CLIPPED extent (y0/y1) for the LUT cull */
            } else {
            if(ytO>ytF && ut>=0){ int y0=ytF<vs_ct[c]?vs_ct[c]:ytF, y1=ytO>vs_cb[c]?vs_cb[c]:ytO; if(vis&&y1-y0>=4){ vs_strip(vs_spr++,c,y0,y1,ut,tc,dpix,dyt,dytO,ytF,uvoff,isposter); vs_nstr[c]++; } }   /* upper: top=front ceiling, bottom=opening ceiling; voff=uvoff -> peg the texture's BOTTOM to the opening (DOOM default) */
            if(ybF>ybO && lt>=0){ int y0=ybO<vs_ct[c]?vs_ct[c]:ybO, y1=ybF>vs_cb[c]?vs_cb[c]:ybF;
                if(dC<vs_stepd[c]){ vs_stepd[c]=(short)dC; vs_stept[c]=(short)(ybO<VS_LBT?VS_LBT:ybO); }   /* STEP OCCLUSION: nearest riser crest (back/upper floor edge) -> hides far actors standing below it */
                if(vis&&y1-y0>=4){ vs_strip(vs_spr++,c,y0,y1,lt,tc,dpix,dybO,dyb,ybO,lvoff,isposter); vs_nstr[c]++; } }   /* lower: top=opening floor, bottom=front floor; voff=lvoff (peg top default, hang-from-ceiling if DONTPEGBOTTOM) */
            if((fl&4) && !(fl&2) && g_opensky && SKY_TEX>=0){   /* SKY-IN-OPENING: back ceiling is sky AND front is NOT (i.e. the view is INDOORS looking out -> draw sky; suppresses the from-OUTSIDE-into-an-indoor-window case where front=sky) */
                int sk=(c*g_colw+(g_colw>>1))>>4; if(sk<0)sk=0; if(sk>FLOORLUT_COLS-1)sk=FLOORLUT_COLS-1;   /* the 16px sky BLOCK at this column's centre */
                if(!vs_skyblk[sk]){ int sy0=ytO<vs_ct[c]?vs_ct[c]:ytO, sy1=ybO>VS_HOR?VS_HOR:ybO; if(sy1>vs_cb[c])sy1=vs_cb[c]; if(sy0<g_vpt)sy0=g_vpt;   /* ABOVE the horizon only; one even strip per 16px block. sy0 clamped UP to the band top (g_vpt) so the screen-anchored opening-sky fully covers the opening -> no ceiling LUT revealed above the strip when sky is shown. */
                    if(vis&&sy1-sy0>=4){ vs_strip_sky(vs_spr++,sk,sy0,sy1); vs_skyblk[sk]=1; } } }
            if(ytO>vs_ct[c]) vs_ct[c]=(short)ytO;
            if(ybO<vs_cb[c]) vs_cb[c]=(short)ybO;
            if(vs_nstr[c]>=g_dcap && vs_ct[c]<=vs_cb[c]){ if(dC<vs_wdep[c])vs_wdep[c]=(short)dC; vs_open--; vs_ct[c]=vs_cb[c]+1; }   /* DEPTH CAP: fog beyond g_dcap layers; the fog-out depth now also OCCLUDES far actors behind it (was: only solid walls set vs_wdep -> far enemies leaked through fogged columns) */
            }
        }
    }
}
/* child bbox potentially visible? behind-camera + frustum reject + OCCLUSION: cull a subtree
   whose whole screen-column span is already solid (closed by nearer walls). This is the BSP's
   point -- visit only what's still on screen. Conservative (camera-inside / near-straddle => keep);
   never culls a column that's still open, so no visible geometry is dropped. */
static int vs_bbox_vis(const short*bb){
    g_bbox_n++;
    if(vs_px>=bb[0] && vs_px<=bb[2] && vs_py>=bb[1] && vs_py<=bb[3]) return 1;   /* camera inside the box -> always visible */
    if(g_ncull){   /* CHEAP CULL LADDER (Phase 1): far-cull (nearest corner, 2 MULs) then frustum side-reject (~4 MULs) -- BOTH before the ~28-MUL 4-corner loop. The walk-bound (bx>>sg) win; A/B via ncul. */
        short nx=(vs_fcs>=0)?bb[0]:bb[2], ny=(vs_fsn>=0)?bb[1]:bb[3];
        int dmin=(int)(((long)((short)(nx-vs_px)*vs_fcs)+(long)((short)(ny-vs_py)*vs_fsn))>>8);   /* MIN view-depth = the nearest corner (toward -view) */
        if(dmin>g_murk_eff) return 0;   /* WHOLLY beyond the horizon */
        if(dmin>g_vs_near){             /* wholly IN FRONT -> the off-side test is exact (no near-plane straddle to confuse sd/d) */
            short rx=(g_frP>=0)?bb[0]:bb[2], ry=(g_frQ>=0)?bb[1]:bb[3];
            if((long)((short)(rx-vs_px)*(short)g_frP) + (long)((short)(ry-vs_py)*(short)g_frQ) > 255) return 0;   /* min(S-D)>255 (one >>8 ULP) -> WHOLLY off the RIGHT frustum edge. The >255 (not >0) margin makes the un-floored cheap test STRICTLY conservative vs the full loop's floored strict allR (sd>d after >>8) -> cheap subset of full, never a false reject; near-edge boxes just fall to the full loop. */
            short lx=(g_frR>=0)?bb[2]:bb[0], ly=(g_frP>=0)?bb[3]:bb[1];
            if((long)((short)(lx-vs_px)*(short)g_frR) + (long)((short)(ly-vs_py)*(short)g_frP) < 0) return 0;   /* max(sd*FOCAL+160d)<0 -> WHOLLY off the LEFT (= allL); S=P=fsn-fcs */
        }
    }
    int cx[4]={bb[0],bb[2],bb[2],bb[0]}, cy[4]={bb[1],bb[1],bb[3],bb[3]};
    int anyfront=0, anyback=0, allR=1, allL=1, sxmin=320, sxmax=-1, dmin=0x7fffffff;
    for(int i=0;i<4;i++){
        short dx=(short)(cx[i]-vs_px), dy=(short)(cy[i]-vs_py);
        int d=(int)(((long)(dx*vs_fcs)+(long)(dy*vs_fsn))>>8);   /* 16x16 muls.w (was (long)dx*vs_fcs = __mulsi3) */
        if(d<g_vs_near){ anyback=1; continue; }   /* near-clip dial: bbox corner behind the (movable) near plane */
        int sd=(int)(((long)(dx*vs_fsn)-(long)(dy*vs_fcs))>>8); anyfront=1; if(d<dmin)dmin=d;
        if(!((long)(short)sd*VS_FOCAL >  160L*d)) allR=0;
        if(!((long)(short)sd*VS_FOCAL < -160L*d)) allL=0;
        int rf=(d<VS_RFMAX)?g_rf[d]:(int)(((long)VS_FOCAL<<8)/d);
        int sx=VS_HALF+(int)(((long)(short)sd*(short)rf)>>8);
        if(sx<sxmin)sxmin=sx; if(sx>sxmax)sxmax=sx;
    }
    if(!anyfront) return 0;                 /* wholly behind the near plane */
    if(anyback)   return 1;                 /* straddles the near plane -> span unreliable, keep (safe) */
    if(g_ncull && dmin>g_murk_eff) return 0;   /* (35) NODE FAR-CULL: subtree wholly beyond the far-horizon -> prune the WALK. Depth is linear over an AABB so its min is at a corner; all corners front here (anyback==0). No emitted strip is beyond g_murk_eff (917), so nothing visible is lost; the open columns show the floor/ceil LUT + murk backdrop = the beyond-fog look. Makes dd/murk limit pj (the walk), not just the emit. */
    if(allR||allL) return 0;                /* wholly off one side of the frustum */
    int smin=sxmin<0?0:(sxmin>320?320:sxmin), smax=sxmax<0?0:(sxmax>320?320:sxmax);   /* clamp to [0,320] for the (short) reciprocal-MUL (corner sx are off-screen-unbounded ints) */
    int c0=((unsigned short)smin*(unsigned short)g_colrcp)>>16; if(c0>g_ncol-1)c0=g_ncol-1; int c1=((unsigned short)smax*(unsigned short)g_colrcp)>>16; if(c1>g_ncol-1)c1=g_ncol-1;
    for(int c=c0;c<=c1;c++) if(vs_ct[c]<=vs_cb[c]) return 1;   /* a covered column is still open -> visible */
    return 0;                               /* every covered column already solid -> OCCLUDED, prune */
}
/* locate the subsector containing (px,py) by descending the BSP -> its sector floor */
static int vs_floor_at(int px,int py){
    unsigned short n=ve_root;
    while(!(n&0x8000)){
        long cross=(long)((short)(px-ve_nx[n])*(short)ve_ndy[n])-(long)((short)(py-ve_ny[n])*(short)ve_ndx[n]);   /* 16x16 muls.w (not __mulsi3) */
        n=(cross>0)?ve_nr[n]:ve_nl[n];
    }
    { int sg=ve_ssf[n&0x7FFF], sc=ve_fsec[sg]; return ve_ff[sg]+(sc<g_nsec?g_secdf[sc]:0); }   /* first seg's front floor + LIFT raise -> the eye rides a raising platform */
}
/* MAP TOGGLE: reseat all geometry pointers to map m, snap the camera/eye to its spawn, drop the old map's
   sprites, and upload its compacted palettes. Per-frame cost unchanged; the array-of-arrays index is paid
   once here, never per seg. */
/* PERF: bake the camera-independent texture pegging (mid/upper/lower voff) for every seg with its sectors AT REST.
   vs_render_seg uses these wholesale unless a door/lift is mid-cycle on the seg's sector (pdyn). Bit-identical to the
   live formula with the door/lift deltas = 0, so this is pure work-relocation: 3 %th DIVs/seg move from per-frame to once-per-map. */
static void vs_bake_pegging(void){
    for(int s=0;s<ve_nseg;s++){
        int peg=ve_peg[s], yo_t=ve_yoff[s]>>4, two=ve_flag[s]&1;
        int mt=ve_mt[s], ut=ve_ut[s], lt=ve_lt[s];
        int fc=ve_fc[s], ff=ve_ff[s], bc=ve_bc[s], bff=ve_bf[s];   /* sectors at rest: door/lift deltas = 0 */
        int mv=0,uv=0,lv=0;
        if(mt>=0){ int th=TEXHT[mt]; if(th>0){ int b=(peg&2)?((((th<<4)-(fc-ff))>>4)%th):0; mv=(((b+yo_t)%th)+th)%th; } }
        if(two && ut>=0){ int th=TEXHT[ut]; if(th>0){ int b=(peg&1)?0:((((th<<4)-(fc-bc))>>4)%th); uv=(((b+yo_t)%th)+th)%th; } }
        if(two && lt>=0){ int th=TEXHT[lt]; if(th>0){ int b=(peg&2)?(((fc-bff)>>4)%th):0; lv=(((b+yo_t)%th)+th)%th; } }
        ve_mvoff[s]=(unsigned char)mv; ve_uvoff[s]=(unsigned char)uv; ve_lvoff[s]=(unsigned char)lv;
    }
}
static void vs_set_map(int m){
    g_redraw=1;                                     /* static-skip: a new map invalidates everything -> force a render */
    if(m<0||m>=VE_NMAP) m=0;
    g_map=m;
    { static int g_curbank=-1; if(VE_BANK[m]!=g_curbank){ P_ROM_SWITCH_BANK(VE_BANK[m]); g_curbank=VE_BANK[m]; } }   /* BANKED GEOMETRY: map this map's bank into the 0x200000 window, but ONLY re-issue when the bank actually changes -- re-issuing the same bank makes gngeo re-map/flush (the BK1 dwell stall, commit 50fcd00). vs_set_map is the only switcher -> the bank stays selected for every frame of the map. */
    { int ns=VE_NSEG_MAP[m], nss=VE_NSS_MAP[m], nn=VE_NNODE_MAP[m], nth=VE_NTH_MAP[m];
      const short *q=(const short*)(0x200000UL + VE_OFF[m]);   /* blob base for map m: 16-bit section then 8-bit section (layout = tools/vs_extract.py) */
      ve_x0=q;q+=ns; ve_y0=q;q+=ns; ve_x1=q;q+=ns; ve_y1=q;q+=ns;
      ve_ff=q;q+=ns; ve_fc=q;q+=ns; ve_bf=q;q+=ns; ve_bc=q;q+=ns;
      ve_mt=q;q+=ns; ve_ut=q;q+=ns; ve_lt=q;q+=ns;
      ve_fsec=(const unsigned short*)q;q+=ns; ve_bsec=(const unsigned short*)q;q+=ns; ve_usesec=(const unsigned short*)q;q+=ns;
      ve_uselo=q;q+=ns; ve_ulen=(const unsigned short*)q;q+=ns; ve_yoff=q;q+=ns;
      ve_ssc=(const unsigned short*)q;q+=nss; ve_ssf=(const unsigned short*)q;q+=nss;
      ve_nx=q;q+=nn; ve_ny=q;q+=nn; ve_ndx=q;q+=nn; ve_ndy=q;q+=nn;
      ve_nr=(const unsigned short*)q;q+=nn; ve_nl=(const unsigned short*)q;q+=nn;
      ve_nrb=q;q+=4*nn; ve_nlb=q;q+=4*nn;
      ve_thx=q;q+=nth; ve_thy=q;q+=nth; ve_thz=q;q+=nth;
      const unsigned char *bp=(const unsigned char*)q;
      ve_flag=bp;bp+=ns; ve_ffl=bp;bp+=ns; ve_cfl=bp;bp+=ns; ve_bfl=bp;bp+=ns; ve_bcl=bp;bp+=ns; ve_u0=bp;bp+=ns; ve_peg=bp;bp+=ns;
      ve_tha=bp;bp+=nth; ve_thc=bp;bp+=nth;
      ve_nseg=ns; ve_nth=nth; ve_root=VE_ROOT_MAP[m]; }   /* per-map actor things now banked too (all 36 maps incl E2-E4 monsters/items) */
    g_nsec=VE_NSEC_MAP[m]; for(int i=0;i<VE_MAXSEC;i++){g_secdc[i]=0; g_secdf[i]=0;} g_doorsec=-1; g_doorstate=0; g_doorstay=0; g_liftsec=-1; g_liftstate=0;   /* DOORS+LIFTS: reset all sector ceiling/floor deltas on map change */
    { int i; for(i=0;i<VE_MAXTH;i++)g_thalive[i]=1; for(i=0;i<NFX;i++)g_fxt[i]=0; }   /* COMBAT: all things alive, no FX, on (re)load */
    vs_bake_pegging();   /* PERF: precompute per-seg pegging now that all seg data is loaded -> lifts 3 %th DIVs/seg out of the per-frame path */
    g_nwalktrig=0; for(int s=0;s<ve_nseg && g_nwalktrig<64;s++) if(ve_flag[s]&64) g_walktrig[g_nwalktrig++]=(unsigned short)s;   /* cache this map's WALK-trigger door segs */
    ve_sx=VE_SX_MAP[m]; ve_sy=VE_SY_MAP[m]; ve_sa=VE_SA_MAP[m];
    vs_camx=ve_sx; vs_camy=ve_sy; vs_camang=(ve_sa*256/360)&255;   /* spawn pose */
    vs_eye=vs_floor_at(ve_sx,ve_sy)+41;                            /* SNAP eye to new floor (no ease from the old map) */
    park_all_sprites(); vs_prevspr=41;                            /* drop old map's strips; force a full re-emit */
    g_pweap=-1; g_faceN=-1;                                       /* MAP CHANGE: invalidate the fix-layer gun + face caches so draw_gun()/draw_face() repaint over the ng_cls'd band (else the change-only guard kept a transition-cleared weapon blank -> "weapon disappears between levels"). g_hidegun intentionally NOT reset (clean-view persists). */
    { int s; for(s=0;s<381;s++) g_scb_hw[s]=16; }                 /* PERF: reset SCB tail high-water -> first emit per slot full-clears boot/old-map-undefined SCB1 */
    vs_upload_tex_pals();                                          /* per-map compacted wall+sky palettes */
}
/* ===== ACTOR BILLBOARD (S1): project a world point (wx,wy on floor floorz) to a hardware-scaled sprite.
   Projection lifted from vs_bbox_vis; feet-anchor from ybFa (line 633); SCB encode from vs_strip. Emits
   wt independent sprite-columns, uniform shrink by 1/depth (rf). Caller passes the sprite's base/wt/ht/
   lo/to/pal (sprites.h SPR_*). No occlusion/sort/budget-partition yet -- those are M1/M2. ===== */
static void vs_billboard(int wx,int wy,int floorz,int base,int wt,int ht,int lo,int to,int pal,int mirror){
    short dx=(short)(wx-vs_px), dy=(short)(wy-vs_py);
    int d=(int)(((long)dx*vs_fcs+(long)dy*vs_fsn)>>8);                       /* depth along view */
    if(d<VS_NEAR) return;                                                    /* behind the near plane */
    int sd=(int)(((long)dx*vs_fsn-(long)dy*vs_fcs)>>8);                     /* lateral offset */
    int rf=(d<VS_RFMAX)?g_rf[d]:(int)(((long)VS_FOCAL<<8)/d);               /* TRUE 1/depth -> drives the POSITIONS (feet + lateral) so a sprite stays glued to the floor at EVERY depth */
    int rfs=rf>512?512:rf;   /* SIZE scale: hardware shrinks only (can't magnify past the SOURCE). Billboards are baked 2x (wad2c HUD1X), so the magnify cap is 2x (512) and the SIZE terms divide by 512 (>>9) -> TRUE perspective size for depth>=80, 1:1-source at depth=80, slightly small only when very near. POSITION (feet/lateral) still uses the uncapped rf (>>8): world-anchored + scale-independent. (3x would need /768.) */
    int sx=VS_HALF+(int)(((long)(short)sd*(short)rf)>>8);                   /* origin screen-x (TRUE position) */
    int feet=VS_HOR-(int)(((long)(short)(floorz-g_flooreye)*(short)rf)>>8); /* origin screen-y on the floor (TRUE position; g_flooreye = static-LUT ref, no stair lag) */
    int sw=(int)(((long)16*rfs)>>9); if(sw<1)sw=1; if(sw>16)sw=16;          /* per-column display width (px) -- 2x source /512 */
    int topy=feet-(int)(((long)to*rfs)>>9);                                 /* sprite TOP = feet up by TO (2x source /512) */
    int leftx=sx-(int)(((long)lo*rfs)>>9);                                  /* sprite LEFT = origin left by LO (2x source /512) */
    int srch=ht*16, scrh=(int)(((long)srch*rfs)>>9);                        /* source (2x) vs on-screen height (/512) */
    int vsh=(srch>0)?(scrh*255)/srch:255; if(vsh<1)vsh=1; if(vsh>255)vsh=255;   /* v-shrink: ht*16 source -> scrh */
    int hsh=sw-1; if(hsh<0)hsh=0; if(hsh>15)hsh=15;
    int t0=0, topy_d=topy;   /* TOP-CLIP: if the sprite top runs above the play band, SKIP the off-top source tiles + drop the y to VS_LBT, so the 9-bit y can't wrap (the residual jump). Feet stay anchored (bottom tiles unchanged). */
    if(topy<VS_LBT){ int pt=(vsh*16)/255; if(pt<1)pt=1; t0=(VS_LBT-topy)/pt; if(t0>ht-1)t0=ht-1; if(t0<0)t0=0; topy_d=topy+t0*pt; }
    int ndraw=ht-t0; if(ndraw<1)ndraw=1;
    { int pt=(vsh*16)/255; if(pt<1)pt=1; int room=(VS_LBB-topy_d)/pt;   /* BOTTOM-CLIP (mirror of the top-clip): bound the sprite to the play band so a low feet -- a very NEAR monster, or a FAR one standing on a much lower floor (down a pit) which projects to the screen bottom -- can't spill under the HUD or wrap the 9-bit sprite-Y. */
      if(room<1) return; if(room<ndraw){ ndraw=room; ht=t0+ndraw; } }
    for(int p=0;p<wt;p++){
        int cx=leftx+p*sw; if(cx<-15||cx>=320) continue;                    /* screen column p (left->right) */
        int srctc=mirror?(wt-1-p):p, flip=mirror?0x01:0;                    /* MIRROR: reverse columns + H-flip each tile (rotations 6/7/8) */
        int wcl=cx, wcr=cx+sw-1;                                            /* OCCLUSION: sample BOTH column edges (centre-only let a sprite col straddling a wall edge draw fully = leak) */
        if(wcl<0)wcl=0; if(wcl>319)wcl=319; if(wcr<0)wcr=0; if(wcr>319)wcr=319;
        int wl=((unsigned short)wcl*(unsigned short)g_colrcp)>>16; if(wl>g_ncol-1)wl=g_ncol-1;
        int wr=((unsigned short)wcr*(unsigned short)g_colrcp)>>16; if(wr>g_ncol-1)wr=g_ncol-1;
        if(g_occl && (vs_wdep[wl] < d-10 || vs_wdep[wr] < d-10
                   || (vs_stepd[wl] < d-10 && vs_stept[wl] <= feet) || (vs_stepd[wr] < d-10 && vs_stept[wr] <= feet))) continue;  /* hidden if EITHER edge is behind a nearer WALL, or below a nearer STEP crest (margin 10). The step term is THE fix for far monsters drawn OVER near steps; feet-aware so ones standing higher than the step still show. */
        if(vs_spr>=379) break;                                              /* sprite-record backstop (<380 HW) */
        int spr=vs_spr++;
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
        for(int r=t0;r<ht;r++){ int T=SPR_TILE0+base+r*wt+srctc;            /* draw source tiles t0..ht-1 (top-clipped) */
            *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((pal<<8)|(((T>>16)&0xF)<<4)|flip); }
        vs_scb_clear_tail(spr,ndraw,pal);   /* PERF: blank only the stale tail past 'ndraw' (was: pad to 16). Same anti-trail safety via the high-water invariant on this shared slot range. */
        int cyf=(496-topy_d)&0x1FF;
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
        *REG_VRAMRW=(u16)((hsh<<8)|vsh); *REG_VRAMRW=(u16)((cyf<<7)|ndraw); *REG_VRAMRW=(u16)((cx&0x1FF)<<7);
    }
}
/* MUZZLE FLASH: SPR_FLASH (C-ROM sprite) drawn at a FIXED screen pos above the gun barrel, in the sprite
   pass, during the firing frames. Sits just above the fix-layer gun so the gun doesn't cover it. */
static void vs_muzzle(void){
    int fx=136, fy=100;   /* top-left of the wt*ht flash, centred over the barrel (x160); +16px with the gun's 2-row (HUD_ROW) shift */
    for(int p=0;p<SPR_FLASH_WT;p++){
        if(vs_spr>=379) break;
        int spr=vs_spr++, cx=fx+p*16;
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
        for(int r=0;r<SPR_FLASH_HT;r++){ int T=SPR_TILE0+SPR_FLASH_BASE+r*SPR_FLASH_WT+p;
            *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((SPR_FLASH_PAL<<8)|(((T>>16)&0xF)<<4)); }
        vs_scb_clear_tail(spr,SPR_FLASH_HT,SPR_FLASH_PAL);   /* PERF: maintain the blank-tail invariant on this shared slot */
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
        *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((((496-fy)&0x1FF)<<7)|SPR_FLASH_HT); *REG_VRAMRW=(u16)((cx&0x1FF)<<7);
    }
}
/* HUD FULL-WIDTH EDGES: gngeo's FIX layer only renders cols 1..38, so the status bar's outer 8px (fix cols
   0 + 39) sit in fix-layer blanking -> the bar read 8px-cropped each side vs the 320px 3D (sprite) view.
   Fill the two gaps with SCB sprites sampled from the ORIGINAL STBAR sprite's END columns (col 0 + col WT-1)
   at the bar's y. The FIX layer is topmost, so it masks the INNER 8px of each 16px edge sprite -> only the
   8px gap shows = seamless full-320 bar. Emitted per frame (SCBs volatile); 2 sprites.
   hwx: left=0 (fb 16..31, col 0 shows); right=304 (fb 320..335, col 39 shows). cyf=512-HUD_ROW*8. */
static void vs_hud_edges(void){
    /* {tile base, palette, row-stride(=src WT), hw-x}. LEFT = raw STBAR col 0 (grey frame; no numerals at x0..15).
       RIGHT = composed HEDGER (its own 1-col sprite) -> the cropped yellow ammo-table numerals show in the gap. */
    static const struct { unsigned short base; unsigned char pal, wt; short hwx; } E[2]={
        {SPR_STBAR_BASE,  SPR_STBAR_PAL,  SPR_STBAR_WT,  0},
        {SPR_HEDGER_BASE, SPR_HEDGER_PAL, SPR_HEDGER_WT, 304} };
    for(int e=0;e<2;e++){
        if(vs_spr>=379) break;
        int spr=vs_spr++;
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
        for(int r=0;r<2;r++){ int T=SPR_TILE0+E[e].base+r*E[e].wt;   /* col 0 of each edge sprite */
            *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((E[e].pal<<8)|(((T>>16)&0xF)<<4)); }
        vs_scb_clear_tail(spr,2,E[e].pal);   /* PERF: maintain the blank-tail invariant on this shared slot */
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
        *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)(((512-HUD_ROW*8)<<7)|2); *REG_VRAMRW=(u16)((E[e].hwx&0x1FF)<<7);
    }
}
/* angle (0..255, 0=+x/east, CCW like the cart heading) of vector (x,y) -- tan-approx, fine enough for 8-way. */
static int vs_atan2(int y,int x){
    if(!x && !y) return 0;
    int ax=x<0?-x:x, ay=y<0?-y:y, a;
    if(ax>=ay) a=(int)(((long)ay<<5)/ax); else a=64-(int)(((long)ax<<5)/ay);   /* 0..64 within a quadrant */
    if(x>=0) return (y>=0)?a:((256-a)&255);
    return (y>=0)?(128-a):(128+a);
}
/* ACTOR CLASSES (CLS_* from vs_e1.h): per-class baked rotation set. Monsters have 5 lumps (R0=A1 front..
   R4=A5 back); the 8 view-rotations map via ROTLUMP + ROTMIR (rots 6/7/8 = lumps 3/2/1 H-flipped). */
static const unsigned char ROTLUMP[8]={0,1,2,3,4,3,2,1}, ROTMIR[8]={0,0,0,0,0,1,1,1};
typedef struct { unsigned short base; unsigned char wt,ht; short lo,to; } sprrot_t;
typedef struct { unsigned char nrot,pal; const sprrot_t *rotA,*rotB,*corpse; } sprclass_t;   /* rotB=0 => 1-frame; corpse=0 => no body (vanish on death) */
#define SR(N) {SPR_##N##_BASE,SPR_##N##_WT,SPR_##N##_HT,SPR_##N##_LO,SPR_##N##_TO}
static const sprrot_t BAR_ROT[1]={ SR(BAR) };
static const sprrot_t IMP_ROTA[5]={ SR(IMP), SR(IMP_R1), SR(IMP_R2), SR(IMP_R3), SR(IMP_R4) };
static const sprrot_t IMP_ROTB[5]={ SR(IMP_B0), SR(IMP_B1), SR(IMP_B2), SR(IMP_B3), SR(IMP_B4) };
static const sprrot_t POSS_ROTA[5]={ SR(POSS), SR(POSS_R1), SR(POSS_R2), SR(POSS_R3), SR(POSS_R4) };
static const sprrot_t POSS_ROTB[5]={ SR(POSS_B0), SR(POSS_B1), SR(POSS_B2), SR(POSS_B3), SR(POSS_B4) };
static const sprrot_t SPOS_ROTA[5]={ SR(SPOS), SR(SPOS_R1), SR(SPOS_R2), SR(SPOS_R3), SR(SPOS_R4) };
static const sprrot_t SPOS_ROTB[5]={ SR(SPOS_B0), SR(SPOS_B1), SR(SPOS_B2), SR(SPOS_B3), SR(SPOS_B4) };
static const sprrot_t IMP_CORPSE[1]={ SR(IMPDM) };    /* imp corpse (TROOM0) */
static const sprrot_t POSS_CORPSE[1]={ SR(POSSD) };   /* zombieman corpse (POSSL0 = DIE5) */
static const sprrot_t SPOS_CORPSE[1]={ SR(SPOSD) };   /* shotgunner corpse (SPOSL0 = DIE5) */
/* ITEM billboards (armour/ammo): 1 frame, no rotation/animation/corpse. The 'pal' field holds the item
   INDEX (SPR_x_PAL = 0..NITEMPAL-1); the draw path adds the per-map base g_itembase to reach the slot. */
static const sprrot_t ARM1_ROT[1]={ SR(ARM1) }; static const sprrot_t ARM2_ROT[1]={ SR(ARM2) }; static const sprrot_t BON2_ROT[1]={ SR(BON2) };
static const sprrot_t CLIP_ROT[1]={ SR(CLIP) }; static const sprrot_t AMMO_ROT[1]={ SR(AMMO) }; static const sprrot_t SHEL_ROT[1]={ SR(SHEL) }; static const sprrot_t SBOX_ROT[1]={ SR(SBOX) };
static const sprrot_t ROCK_ROT[1]={ SR(ROCK) }; static const sprrot_t BROK_ROT[1]={ SR(BROK) };
#undef SR
#define CLS_ITEM0 4   /* classes >= this are visible-billboard items: 'pal' = item index, g_itembase added at draw time; never shootable/killable */
static const sprclass_t SPRCLASS[]={ {1,SPR_BAR_PAL,BAR_ROT,0,0}, {5,SPR_IMP_PAL,IMP_ROTA,IMP_ROTB,IMP_CORPSE}, {5,SPR_POSS_PAL,POSS_ROTA,POSS_ROTB,POSS_CORPSE}, {5,SPR_SPOS_PAL,SPOS_ROTA,SPOS_ROTB,SPOS_CORPSE},
    {1,SPR_ARM1_PAL,ARM1_ROT,0,0}, {1,SPR_ARM2_PAL,ARM2_ROT,0,0}, {1,SPR_BON2_PAL,BON2_ROT,0,0},
    {1,SPR_CLIP_PAL,CLIP_ROT,0,0}, {1,SPR_AMMO_PAL,AMMO_ROT,0,0}, {1,SPR_SHEL_PAL,SHEL_ROT,0,0}, {1,SPR_SBOX_PAL,SBOX_ROT,0,0},
    {1,SPR_ROCK_PAL,ROCK_ROT,0,0}, {1,SPR_BROK_PAL,BROK_ROT,0,0} };
#define NCLS ((int)(sizeof(SPRCLASS)/sizeof(SPRCLASS[0])))
static short CLS_HL[NCLS], CLS_WT[NCLS];   /* per-class MAX source LEFT-offset (lo) and MAX tile-width (wt) over EVERY rotation/frame the class can draw -> the occlusion pre-cull span [lcx=sx-CLS_HL*rfs/512 .. rcx=sx+CLS_WT*sw-1] (sw the SAME >=1-floored per-column width vs_billboard uses) is GUARANTEED to cover this actor's real drawn columns at any depth, so the cull can never hide a visible actor. Filled once by vs_init_clsext. */
static void vs_clsext_lump(int c,const sprrot_t *r){ if(r->lo>CLS_HL[c])CLS_HL[c]=(short)r->lo; if(r->wt>CLS_WT[c])CLS_WT[c]=(short)r->wt; }
static void vs_init_clsext(void){ for(int c=0;c<NCLS;c++){ const sprclass_t *sc=&SPRCLASS[c]; CLS_HL[c]=0; CLS_WT[c]=0;
        for(int k=0;k<sc->nrot;k++){ vs_clsext_lump(c,&sc->rotA[k]); if(sc->rotB) vs_clsext_lump(c,&sc->rotB[k]); } if(sc->corpse) vs_clsext_lump(c,&sc->corpse[0]); } }
static int g_anim=0;   /* free-running actor animation clock (advanced each emitted frame) */
static void vs_render(int px,int py,int ang,int emit){
    vs_px=px; vs_py=py; vs_fcs=VS_CS(ang); vs_fsn=VS_SN(ang); vs_emit=emit; g_vcx=0x40000000;   /* invalidate the per-vertex projection cache each frame (it's camera-dependent) */
    g_frP=vs_fsn-vs_fcs; g_frQ=-(vs_fcs+vs_fsn); g_frR=vs_fsn+vs_fcs;   /* Phase 1: per-frame frustum side-reject coeffs (cheap bbox off-screen cull in vs_bbox_vis) */
    if(emit){ int tgt=vs_floor_at(px,py)+41; g_flooreye=tgt; vs_eye += (tgt-vs_eye)>>2;
              unsigned short cn=ve_root; while(!(cn&0x8000)){ long ccr=(long)((short)(px-ve_nx[cn])*(short)ve_ndy[cn])-(long)((short)(py-ve_ny[cn])*(short)ve_ndx[cn]); cn=(ccr>0)?ve_nr[cn]:ve_nl[cn]; } { int csec=ve_ssf[cn&0x7FFF]; g_cam_ffl=ve_ffl[csec]; g_cam_cfl=ve_cfl[csec]; } }   /* CAMERA-SECTOR floor+ceil flat (same single BSP descent as vs_floor_at; ceil is FREE here -> no second descent) -> the default near-floor / top-band-ceil flats for the stamp */
    { g_vpl=0; g_vpr=g_ncol-1; g_vpt=VS_LBT; g_vpb=VS_LBB; }   /* viewport crop NEUTRALISED (ditched) -> always full; g_vpw/g_vph are now FLOOR/CEILING murk (used in vs_lut) */
    for(int c=0;c<g_ncol;c++){ vs_ct[c]=(c>=g_vpl&&c<=g_vpr)?g_vpt:(g_vpb+1); vs_cb[c]=g_vpb; vs_nstr[c]=0; vs_clY[c]=VS_LBB+1; vs_flY[c]=VS_LBT-1; vs_ctop[c]=VS_LBB+1; vs_cbot[c]=VS_LBT-1; vs_sky[c]=0; vs_skd[c]=0; vs_ffl[c]=0xFF; vs_cfl[c]=0xFF; vs_wdep[c]=0x7FFF; vs_stepd[c]=0x7FFF; }   /* zonal depth reset (vs_fdep + vs_cdep) moved to the 20-wide block loop below (both block-indexed now). ct/cb seed the VIEWPORT band ([g_vpt,g_vpb]); columns outside [g_vpl,g_vpr] start CLOSED (ct>cb) -> walls + BSP skip them = H letterbox */
    if(g_zonal&&!g_generic) for(int bk=0;bk<FLOORLUT_COLS;bk++){ for(int r=0;r<7;r++)vs_cdep[bk][r]=0x7FFF; for(int r=0;r<5;r++){vs_fdep[bk][r]=-1; vs_fbndy[bk][r]=-1;} }   /* zonal depth reset: ceiling(7)+floor(5), BLOCK-indexed 20-wide; vs_fbndy(5) = the zon>=6 sub-tile edge */
    for(int k=0;k<FLOORLUT_COLS;k++)vs_skyblk[k]=0;   /* SKY-IN-OPENING: clear the per-block dedup */
    vs_spr=41; vs_open=g_ncol; g_bbox_n=0; g_seg_n=0; g_seg_clipped=0;
    if(emit){ if(!g_hidehud) vs_hud_edges();   /* full-width HUD edge sprites at slots 41,42 -- BEFORE the wall budget so they're guaranteed */
              else { vs_park(41); vs_park(42); } }   /* cln ON: edges not re-emitted; explicitly PARK both slots (the wall burst only clobbers them when es>=2, so es<2 frames froze a sidebar). Walls/sky reuse 41,42 normally after (vs_spr still 41). */
    if(g_radial){   /* RADIAL far-cull (param 23): only pay the per-column threshold work when it's actually ON */
      if(g_cosrad_n!=g_ncol){ for(int c=0;c<g_ncol;c++){ int dx=c*g_colw+(g_colw>>1)-VS_HALF; g_cosrad[c]=(short)((VS_FOCAL*256)/isqrtI(VS_FOCAL*VS_FOCAL+dx*dx)); } g_cosrad_n=g_ncol; }   /* per-column cos(view angle), only on col-res change */
      for(int c=0;c<g_ncol;c++){ int bcos=256-((g_radial*(256-g_cosrad[c]))>>3); g_murkcol[c]=(int)(((long)g_murk_eff*bcos)>>8); }   /* per-column reach = murk * BLENDED cos: strength g_radial(1..8) lerps the cos pull-in from flat(256) to full. rad=8 -> bcos=cos (full radial); rad=1 -> ~256 (barely). */
    }
    g_vs_tiles=0; g_vs_dmin=9999; g_vs_dmax=0;
#ifdef VS_DIAG
    g_cull_near=g_cull_frus=g_cull_back=g_cull_off=g_cull_bud=g_cull_occ=0;
#endif
    int bxcap=BXCAP[g_bxcapi];                     /* PHASE 2 walk budget (param 44): max node-box tests this frame */
    g_pstepv=PSTEPV[g_pstepi]; g_pstepsh=PSTEPSHV[g_pstepi];   /* PHASE 5: resolve the perspective-subdivide step for vs_render_seg */
    int sp=0; vs_stk[sp++]=ve_root;                /* root is a node index (no bit15) */
    while(sp>0 && vs_open>0 && g_bbox_n<bxcap){     /* ...halt when bx hits the cap: the FAR subtrees still on the stack drop (depth-ordered) */
        unsigned short n=vs_stk[--sp];
        if(n&0x8000){ int ss=n&0x7FFF, cnt=ve_ssc[ss], first=ve_ssf[ss];   /* subsector leaf: render its segs */
            for(int i=0;i<cnt;i++) vs_render_seg(first+i); continue; }
        long cross=(long)((short)(px-ve_nx[n])*(short)ve_ndy[n]) - (long)((short)(py-ve_ny[n])*(short)ve_ndx[n]);  /* which side? FastDoom #1: (long) cast OUTSIDE the product -> 16x16 muls.w, not a __mulsi3 32-bit lib call per node */
        unsigned short nearc,farc; const short *nearbb,*farbb;
        if(cross>0){ nearc=ve_nr[n]; farc=ve_nl[n]; nearbb=&ve_nrb[n*4]; farbb=&ve_nlb[n*4]; }
        else       { nearc=ve_nl[n]; farc=ve_nr[n]; nearbb=&ve_nlb[n*4]; farbb=&ve_nrb[n*4]; }
        if(sp<126 && vs_bbox_vis(farbb))  vs_stk[sp++]=farc;     /* push far first... (guard matches vs_stk[128]) */
        if(sp<126 && vs_bbox_vis(nearbb)) vs_stk[sp++]=nearc;    /* ...near pops first -> front-to-back */
    }
    if(!emit){ g_vs_sink+=vs_spr; return; }
    /* 'ease' (param 6): a DIRECT far-horizon TRIM -- ef = dd minus ease*16 (always on, visible in ef).
       The little far-draw-distance opt: a smaller ef bounds the node far-cull walk + the emit -> perf.
       gov(46)/perfP(12) own the LOAD-ADAPTIVE pull-in; ease is the manual baseline trim on top of dd. */
    int mn=MURKMIN[g_murkmin]; if(mn<0)mn=64;   /* far-cull FLOOR (shuttle 8); off -> a sane min so the trim can't blank the view */
    if(g_govtgt>0){ g_murk_eff=g_govmurk; }   /* GRACEFUL FAR-DROP governor OWNS the horizon when on (overrides ease). */
    else { g_murk_eff = g_vs_murk - g_murkease*16; if(g_murk_eff<mn)g_murk_eff=mn; if(g_murk_eff>g_vs_murk)g_murk_eff=g_vs_murk; }   /* ease trim: ef = dd - ease*16 (ease 0..16 -> 0..256 units), clamped [floor, dd]. Changing ease now visibly moves ef. */
    { int M=vs_spr-41; if(M>VS_SBUFN)M=VS_SBUFN;   /* M = strips RECORDED this frame (dpri records past the budget) */
      int thr=32767;   /* depth cutoff; 32767 = keep all (no depth-priority, or fewer than budget recorded) */
      if(g_dpri && M>g_vs_budget){   /* DEPTH-PRIORITY: keep the NEAREST g_vs_budget strips by depth -> near walls never drop to BSP visit order. Cheap depth histogram (32-unit buckets). */
        for(int b=0;b<64;b++) g_dphist[b]=0;
        for(int i=0;i<M;i++){ vstrip_t *e=&g_sbuf[i]; if(e->sky) continue; int b=(e->d)>>5; if(b<0)b=0; if(b>63)b=63; g_dphist[b]++; }
        int cum=0,tb=63; for(int b=0;b<64;b++){ cum+=g_dphist[b]; if(cum>=g_vs_budget){ tb=b; break; } }
        thr=(tb+1)<<5;   /* keep strips with d <= thr (~the nearest g_vs_budget) */
      }
      int es=0;   /* SCB EMIT index (kept strips, re-packed from slot 41) */
      for(int i=0;i<M;i++){ vstrip_t *e=&g_sbuf[i];
        if(!e->sky && e->d>thr) continue;          /* DEPTH-PRIORITY: drop far non-sky strips beyond the cutoff (sky has no depth -> always kept) */
        if(es>=339) break;                          /* SCB SAFETY: walls cap at slot 41+339=380, the HW sprite ceiling */
        if(g_bspviz && es>=g_bspstep) break;        /* BSP VIZ (37): reveal only the first g_bspstep, front-to-back */
        if(e->sky){ vs_sky_strip_emit(41+es,e->c,e->y0,e->y1,ang); }
        else vs_strip_emit(41+es,e->c,e->y0,e->y1,e->tex,e->tcol,e->d,e->dyt,e->dyb,e->yt0,e->voff,e->poster);
        es++; }
      vs_spr=41+es; }   /* the EMITTED count -> the actor budget check + the parking below see the KEPT strips */
    if(emit) g_drewactors=0;   /* static-skip: assume no actors this frame; the loop below sets it if any get drawn */
    if(emit && g_props){   /* M2: draw this map's THINGS (barrels/enemies) DEPTH-SORTED far->near, occluded per-column by vs_wdep, far-culled with the walls. g_props=0 (param 17) hides them all for geometry-only debug. */
        { static int ce=0; if(!ce){ vs_init_clsext(); ce=1; } }            /* once: per-class occlusion-span extents */
        int bd[MAXACT], bi[MAXACT], nb=0;                                  /* keep the NEAREST MAXACT, sorted by depth DESC (far first) */
        int acull = (g_murk_eff < g_vs_murk) ? g_murk_eff : g_vs_murk;     /* actors cull at the DRAW DISTANCE even when murk is off (g_murk_eff huge) -> no tiny far actors showing through openings */
        if(vs_spr-41 >= g_vs_budget && g_vs_dmax>0 && g_vs_dmax<acull) acull=g_vs_dmax;   /* BUDGET-HALT leak fix: when the draw-count cap halted the BSP walk, far walls weren't drawn (and didn't set vs_wdep) -> tiny far actors leaked through. Cap actors at the deepest DRAWN wall so they can't outrun the budget-culled geometry. */
        if(DD[g_actddi] < acull) acull=DD[g_actddi];   /* ACTOR draw distance (param 41): pull monsters/props in CLOSER than the wall horizon, independently -- the open-arena lever (E2M5) where occlusion can't help */
        for(int i=0;i<ve_nth;i++){
            if(!g_thalive[i]) continue;                                    /* COMBAT: skip killed things */
            short dx=(short)(ve_thx[i]-vs_px), dy=(short)(ve_thy[i]-vs_py);
            if(ve_thc[i]>=CLS_ARM1 && ve_thc[i]<=CLS_BROK && dx>-48 && dx<48 && dy>-48 && dy<48){ SND(5); g_thalive[i]=0; continue; }   /* PICKUP: walk over an item billboard -> itemup SFX (SND 5) + collect (item vanishes) */
            if(dx>acull||dx<-acull||dy>acull||dy<-acull) continue;         /* PERF: cheap BOX reject before the depth MUL+sort -- a thing beyond acull on either axis is either too far (d>acull) or far off-screen to the side (culled anyway). Skips the per-thing cost for the bulk of a 35-159 thing map. */
            int d=(int)(((long)dx*vs_fcs+(long)dy*vs_fsn)>>8);
            if(d<VS_NEAR || d>acull) continue;                             /* behind the near plane, or beyond the actor cull */
            { int sd=(int)(((long)dx*vs_fsn-(long)dy*vs_fcs)>>8);          /* FRUSTUM + OCCLUSION pre-cull (#actor): project the centre, then reject off-screen / fully-behind-wall actors BEFORE they eat a nearest-N slot or pay atan2 + vs_billboard's full projection. */
              int rf=(d<VS_RFMAX)?g_rf[d]:(int)(((long)VS_FOCAL<<8)/d);
              int sx=VS_HALF+(int)(((long)(short)sd*(short)rf)>>8);
              if(sx<-80 || sx>=320+80) continue;                           /* cheap early-out: centre well off-screen */
              if(g_occl){ int cls=ve_thc[i]; if(cls<0||cls>=NCLS)cls=0; int rfs=rf>512?512:rf;   /* OCCLUSION pre-cull: skip an actor whose every drawn column is behind a nearer SOLID wall, before atan2 + vs_billboard. */
                int sw=(16*rfs)>>9; if(sw<1)sw=1;                           /* per-column width: SAME >=1 floor as vs_billboard (~line 1119) so the span covers far actors whose columns each clamp to 1px */
                int lcx=sx-((CLS_HL[cls]*rfs)>>9), rcx=sx+CLS_WT[cls]*sw-1; /* [lcx,rcx] >= vs_billboard's actual drawn px reach (CLS_HL>=lo, CLS_WT>=wt, same sw) -> never misses a visible column */
                if(lcx<0)lcx=0; if(rcx>319)rcx=319;
                if(lcx<=rcx){                                              /* some on-screen extent (fully off-screen actors fall through to vs_billboard, which emits ~0 cheaply) */
                    int rl=((unsigned short)lcx*(unsigned short)g_colrcp)>>16; if(rl>g_ncol-1)rl=g_ncol-1;   /* clamp to [0,319] mirrors vs_billboard's own wcl/wcr clamp, so it samples exactly the render-cols vs_billboard would */
                    int rr=((unsigned short)rcx*(unsigned short)g_colrcp)>>16; if(rr>g_ncol-1)rr=g_ncol-1;
                    int vis=0; for(int c=rl;c<=rr;c++) if(vs_wdep[c]>=d-10){ vis=1; break; }   /* visible if ANY covered render-col is NOT behind a nearer wall (step term omitted = conservative; vs_billboard re-tests survivors) */
                    if(!vis) continue;                                      /* every covered column hidden -> skip the actor entirely */
                } } }
            if(nb<g_maxact){ int j=nb++; while(j>0 && bd[j-1]<d){ bd[j]=bd[j-1]; bi[j]=bi[j-1]; j--; } bd[j]=d; bi[j]=i; }   /* insert (DESC); g_maxact (param 42) <= MAXACT array size */
            else if(d<bd[0]){ int j=0; while(j<g_maxact-1 && bd[j+1]>d){ bd[j]=bd[j+1]; bi[j]=bi[j+1]; j++; } bd[j]=d; bi[j]=i; }   /* full: evict the farthest, keep nearest g_maxact */
        }
        int aframe=(g_anim>>3)&1;                                          /* idle/walk: toggle frame A/B every ~8 emitted frames (g_anim now ticks in the MAIN loop so the clock advances even on skipped frames) */
        g_drewactors=nb;                                                   /* static-skip: actors on-screen this frame? -> only then does the anim phase wake a re-render */
        g_act_sel=nb; g_act_emit=0;                                        /* instrument: selected (post-frustum) vs actually-emitted (>=1 on-screen non-occluded column) */
        for(int k=0;k<nb;k++){ int i=bi[k]; int cls=ve_thc[i]; if(cls<0||cls>=NCLS)cls=0;
            const sprclass_t *sc=&SPRCLASS[cls]; int lump=0,mir=0; const sprrot_t *R;
            if(g_thalive[i]==2 && sc->corpse){ R=&sc->corpse[0]; }   /* CORPSE: static body on the ground, no rotation/animation */
            else { if(sc->nrot>1){ int av=vs_atan2(vs_py-ve_thy[i], vs_px-ve_thx[i]); int ri=((av-ve_tha[i]+16)&255)>>5; lump=ROTLUMP[ri]; mir=ROTMIR[ri]; }   /* 8-way rotation */
                   const sprrot_t *rot=(sc->rotB && aframe)?sc->rotB:sc->rotA; R=&rot[lump]; }   /* animate: A/B frame */
            int pal=(cls>=CLS_ITEM0)?(g_itembase+sc->pal):sc->pal;   /* ITEMS: per-map palette (base + item index); actors: global 244+ slot */
            if(cls<CLS_ITEM0 && bd[k]>g_fog1) pal=14;   /* ENEMY FOG (cheap, 0 new slots): non-item billboards past the far wall-fog band fade to the deep-murk palette (slot 14, idx0 transparent) -> they darken into the fog, cohesive with the wall far-shade. Tracks g_fog1 so fog-off (fogext 0 -> g_fog1 huge) = no darken. */
            int b4=vs_spr; vs_billboard(ve_thx[i],ve_thy[i],ve_thz[i], R->base,R->wt,R->ht,R->lo,R->to, pal, mir);
            if(vs_spr>b4) g_act_emit++;   /* this actor emitted >=1 column (wasn't fully off-screen/occluded) */
        }
        for(int f=0;f<NFX;f++){ if(!g_fxt[f])continue;   /* COMBAT: barrel-explosion FX (E->C->A) then free */
            short dx=(short)(g_fxx[f]-vs_px), dy=(short)(g_fxy[f]-vs_py);
            int d=(int)(((long)dx*vs_fcs+(long)dy*vs_fsn)>>8);
            if(d>=VS_NEAR && d<=acull){ int t=g_fxt[f];
                if(t>6)      vs_billboard(g_fxx[f],g_fxy[f],g_fxz[f], SPR_BEXPE_BASE,SPR_BEXPE_WT,SPR_BEXPE_HT,SPR_BEXPE_LO,SPR_BEXPE_TO, SPR_BEXPC_PAL,0);
                else if(t>3) vs_billboard(g_fxx[f],g_fxy[f],g_fxz[f], SPR_BEXPC_BASE,SPR_BEXPC_WT,SPR_BEXPC_HT,SPR_BEXPC_LO,SPR_BEXPC_TO, SPR_BEXPC_PAL,0);
                else         vs_billboard(g_fxx[f],g_fxy[f],g_fxz[f], SPR_BEXPA_BASE,SPR_BEXPA_WT,SPR_BEXPA_HT,SPR_BEXPA_LO,SPR_BEXPA_TO, SPR_BEXPC_PAL,0); }
            g_fxt[f]--; }
    }
    if(emit && g_fire>=4 && !g_hidegun) vs_muzzle();          /* MUZZLE FLASH for the first few firing frames (over the gun barrel) */
    vs_lut(ang);                                              /* seamless perspective hex floor + ceiling behind the walls */
    for(int i=vs_spr;i<vs_prevspr;i++) vs_park(i);
    vs_prevspr=vs_spr;
}
/* BOUNDARY DETECTION: block movement across SOLID (one-sided) walls; two-sided lines pass. */
static int vs_side(int ax,int ay,int bx,int by,int cx,int cy){
    long v=(long)(short)(bx-ax)*(short)(cy-ay)-(long)(short)(by-ay)*(short)(cx-ax);
    return v>0?1:(v<0?-1:0);
}
#define DOOR_CLEAR 48                              /* a door must open this many world-units before the player can walk through */
static int vs_move_ok(int ax,int ay,int bx,int by){
    for(int s=0;s<ve_nseg;s++){
        int fl=ve_flag[s];
        if(fl&1){                                  /* two-sided */
            if(!(fl&8)) continue;                  /* regular opening -> passable */
            if(g_doorwalk) continue;               /* DEBUG (param 22): doors walk-through without opening */
            int sec=ve_bsec[s]; if(sec>=g_nsec)sec=ve_fsec[s];                       /* the doorway sector */
            int open=(int)ve_bc[s]+(sec<g_nsec?g_secdc[sec]:0)-(int)ve_bf[s];        /* current floor->ceiling gap */
            if(open>=DOOR_CLEAR) continue;         /* DOOR open enough -> passable */
            /* closed/closing door -> fall through to the crossing test (blocks movement) */
        }
        int cx=ve_x0[s],cy=ve_y0[s],dx=ve_x1[s],dy=ve_y1[s];
        int d1=vs_side(ax,ay,bx,by,cx,cy), d2=vs_side(ax,ay,bx,by,dx,dy);
        if(d1==d2) continue;
        int d3=vs_side(cx,cy,dx,dy,ax,ay), d4=vs_side(cx,cy,dx,dy,bx,by);
        if(d3!=d4) return 0;                       /* the move segment crosses this solid wall */
    }
    return 1;
}
/* COMBAT (CNT_C, playing): hitscan the nearest alive THING in the forward corridor, not blocked by a wall/shut door.
   Barrels explode (FX + boom) and splash-chain neighbouring barrels; enemies scream and vanish. */
#define SHOOT_RANGE 1600
#define BLAST_RAD2  (160L*160L)
#ifndef VS_AIM
#define VS_AIM 36          /* aim-cone half-width: s*FOCAL <= VS_AIM*d  (36/160 ~= 12.7deg half -> ~25deg cone), forgiving + range-constant */
#endif
static void vs_fx_spawn(int x,int y,int z){ for(int f=0;f<NFX;f++) if(!g_fxt[f]){ g_fxx[f]=(short)x;g_fxy[f]=(short)y;g_fxz[f]=(short)z;g_fxt[f]=9; return; } }
static unsigned char deadstate(int i){ int c=ve_thc[i]; if(c<0||c>=NCLS)c=0; return (ve_thc[i]!=0 && SPRCLASS[c].corpse)?2:0; }   /* 2 = leave a CORPSE (enemy with a death sprite); 0 = gone (barrel / no corpse frame). Items never reach here (not shootable). */
static void vs_kill_at(int i){
    if(i<0||i>=ve_nth||g_thalive[i]!=1) return;     /* only a LIVING thing can be killed (corpses=2 are inert) */
    g_thalive[i]=deadstate(i);                       /* enemy -> corpse(2); barrel/no-frame -> gone(0) */
    g_redraw=1;                                      /* static-skip: a sprite vanished/changed -> force a render even if the camera is still */
    if(ve_thc[i]!=0) return;                        /* ENEMY: no splash (SFX queued by the caller after the gun pop) */
    /* BARREL: bounded BFS chain (boom SFX queued by the caller) */
    int q[16], qn=0; q[qn++]=i;
    while(qn>0){ int b=q[--qn]; vs_fx_spawn(ve_thx[b],ve_thy[b],ve_thz[b]);
        for(int j=0;j<ve_nth;j++){ if(g_thalive[j]!=1)continue;        /* only LIVING neighbours catch the blast */
            if(ve_thc[j]>=CLS_ITEM0)continue;                          /* ITEMS: visible billboards, immune to barrel blasts */
            short dx=(short)(ve_thx[j]-ve_thx[b]), dy=(short)(ve_thy[j]-ve_thy[b]);
            if((long)dx*dx+(long)dy*dy < BLAST_RAD2){ g_thalive[j]=deadstate(j);   /* blast victim: corpse (enemy) or gone (barrel) */
                if(ve_thc[j]==0 && qn<16) q[qn++]=j; } } }   /* a neighbour barrel chains; enemies in the blast leave bodies */
}
/* POSITIONAL SFX: encode a world-sourced sound's distance->volume (4 levels) + lateral->pan (L/C/R) into ONE
   byte the Z80 decodes (0x80|(idx<<4)|(vol<<2)|pan). idx = 0-based SFX index; dist = forward depth to source;
   lat = lateral offset (>0 = right of the view, matching vs_billboard's sd). pan: 3=both(ahead), 2=left, 1=right
   (YM2610 reg 0x08 bit7=L, bit6=R). Player/UI sounds stay plain (full volume); only world sources use this. */
static unsigned char snd_pos(int idx,int dist,int lat){
    int vol = (dist<400)?3:((dist<900)?2:((dist<1700)?1:0));   /* 4 distance buckets: loud -> faint */
    int al = lat<0?-lat:lat;
    int pan = (al*2 < dist) ? 3 : ((lat>0)?1:2);               /* within ~26deg of ahead -> both; else right(1)/left(2) */
    return (unsigned char)(0x80 | ((idx&7)<<4) | (vol<<2) | pan);
}
static int vs_shoot(int px,int py,int ang){
    int fcs=VS_CS(ang), fsn=VS_SN(ang);
    int best=-1, bestd=SHOOT_RANGE;
    for(int i=0;i<ve_nth;i++){ if(g_thalive[i]!=1)continue;   /* only LIVING things can be shot (corpses are inert) */
        if(ve_thc[i]>=CLS_ITEM0)continue;                     /* ITEMS: visible billboards, not shootable */
        short dx=(short)(ve_thx[i]-px), dy=(short)(ve_thy[i]-py);
        int d=(int)(((long)dx*fcs+(long)dy*fsn)>>8);            /* forward depth */
        if(d<VS_NEAR || d>=bestd) continue;
        int s=(int)(((long)dx*fsn-(long)dy*fcs)>>8); if(s<0)s=-s; /* lateral offset */
        if(s>48 && (long)s*VS_FOCAL > (long)VS_AIM*d) continue; /* hit if close laterally (<=48u, point-blank forgiving) OR within the angular aim cone (far) */
        if(!vs_move_ok(px,py,ve_thx[i],ve_thy[i])) continue;   /* a solid wall / shut door blocks the shot */
        best=i; bestd=d; }
    if(best<0) return 0;
    int idx=(ve_thc[best]==0)?2:((ve_thc[best]==1)?5:6);   /* 0-based SFX index: barrel->boom(2); imp->DSBGDTH1(5); possessed->DSPODTH1(6) */
    short ddx=(short)(ve_thx[best]-px), ddy=(short)(ve_thy[best]-py);
    int lat=(int)(((long)ddx*fsn-(long)ddy*fcs)>>8);       /* lateral offset to the kill (>0 = right) for the pan */
    vs_kill_at(best); g_sndq=snd_pos(idx,bestd,lat); g_sndqt=2;   /* QUEUE the POSITIONAL death SFX (distance volume + L/R pan) ~2 frames after the gun pop */
    return 1;
}
/* DOORS: use-raycast (CNT_D, playing). Cast a ray ~72u ahead; the NEAREST door seg (fl&8) it crosses ->
   open that seg's BACK sector (the doorway) toward the room ceiling. One door at a time; auto-holds + closes. */
static void vs_use_door(int px,int py,int ang){
    if(g_doorstate) return;                                   /* already animating a door */
    int fcs=VS_CS(ang), fsn=VS_SN(ang);
    int ux=px+((fcs*72)>>8), uy=py+((fsn*72)>>8);             /* use-ray endpoint, 72 units ahead */
    int best=-1; long bestd=0x7fffffffL;
    for(int s=0;s<ve_nseg;s++){ if(!(ve_flag[s]&8)) continue; /* DOOR segs only */
        int cx=ve_x0[s],cy=ve_y0[s],ex=ve_x1[s],ey=ve_y1[s];
        if(vs_side(px,py,ux,uy,cx,cy)==vs_side(px,py,ux,uy,ex,ey)) continue;
        if(vs_side(cx,cy,ex,ey,px,py)==vs_side(cx,cy,ex,ey,ux,uy)) continue;   /* the ray crosses this door seg */
        long mx=(cx+ex)/2-px, my=(cy+ey)/2-py, dd=mx*mx+my*my;
        if(dd<bestd){ bestd=dd; best=s; } }
    if(best<0) return;
    int sec=ve_bsec[best]; if(sec>=g_nsec) sec=ve_fsec[best]; /* the doorway (back side); fall back to front */
    if(sec>=g_nsec) return;
    int tgt=ve_fc[best]-ve_bc[best]; if(tgt<8) tgt=64;        /* raise the doorway ceiling to the room ceiling (fallback 64u) */
    g_doorsec=sec; g_doortgt=tgt; g_doorprog=0; g_doorstate=1; g_doorstay=0; SND(6);   /* SND 6 = door open/close; g_doorstay=0 = MANUAL door (holds then auto-closes) */
}
/* LIFT + EXIT use-raycast (CNT_D, playing). Same 72u ray as the door. Returns 2 if the nearest special seg is
   an EXIT (caller advances the map -- it owns px/py/ang), 1 if a LIFT was toggled, 0 if nothing usable. */
static int vs_use_special(int px,int py,int ang){
    int fcs=VS_CS(ang), fsn=VS_SN(ang);
    int best=-1; long bestd=0x7fffffffL;
    for(int s=0;s<ve_nseg;s++){ if(!(ve_flag[s]&(16|32|128))) continue;   /* LIFT (16), EXIT (32), or SWITCH-door (128) segs */
        int mx=((int)ve_x0[s]+ve_x1[s])/2-px, my=((int)ve_y0[s]+ve_y1[s])/2-py;       /* to the seg midpoint */
        if(mx>200||mx<-200||my>200||my<-200) continue;                                /* box reject (keeps the MUL in range + cheap) */
        long along=((long)mx*fcs+(long)my*fsn)>>8; if(along<=0||along>96) continue;    /* must be IN FRONT, within reach -- forgiving (no exact ray-cross needed; was the 'panel doesn't trigger' miss) */
        long perp =((long)mx*fsn-(long)my*fcs)>>8; if(perp<0)perp=-perp; if(perp>48) continue;   /* roughly aligned with the facing */
        long dd=(long)mx*mx+(long)my*my; if(dd<bestd){ bestd=dd; best=s; } }
    if(best<0) return 0;
    if(ve_flag[best]&32) return 2;                          /* EXIT switch -> caller advances the map */
    if(ve_flag[best]&128){ int sec=ve_usesec[best];         /* SWITCH-door: USE raises the TAG-resolved DOOR sector (open-stay) */
        if(sec<g_nsec && g_doorstate==0 && g_secdc[sec]==0){ g_doorsec=sec; g_doortgt=ve_uselo[best]; g_doorprog=0; g_doorstate=1; g_doorstay=1; SND(6); }
        return 1; }
    { int sec=ve_usesec[best]; if(sec>=g_nsec) return 1;   /* LIFT: the TAG-resolved target sector (a remote sector, NOT the panel's own back sector) */
      if(g_liftstate==0){ g_liftsec=sec; g_lifttgt=ve_uselo[best]; g_liftprog=0; g_liftstate=1; SND(10); }   /* at REST (up) -> lower to the low stop (DOOM lower-lift); SND 10 = lift START */
      else if(g_liftstate==2 && g_liftsec==sec){ g_liftstate=3; SND(10); } }            /* at the bottom -> raise back (toggle); SND 10 = lift START */
    return 1;                                               /* mid-animation -> swallow the use */
}
/* WALK-TRIGGER doors (per frame, no button): if the player's move (a->b) crosses a flagged (fl&64) trigger
   seg, open its TAG-resolved door sector in OPEN-STAY mode. One door animates at a time; once open, g_secdc
   persists so the level stays passable. Cheap: only the cached g_walktrig segs are tested. */
static void vs_walk_triggers(int ax,int ay,int bx,int by){
    if((ax==bx && ay==by) || g_doorstate) return;             /* not moving, or a door is already animating */
    for(int k=0;k<g_nwalktrig;k++){ int s=g_walktrig[k];
        int sec=ve_usesec[s]; if(sec>=g_nsec || g_secdc[sec]) continue;        /* unresolved tag, or already open */
        int cx=ve_x0[s],cy=ve_y0[s],ex=ve_x1[s],ey=ve_y1[s];
        if(vs_side(ax,ay,bx,by,cx,cy)==vs_side(ax,ay,bx,by,ex,ey)) continue;   /* seg ends on the same side of the move */
        if(vs_side(cx,cy,ex,ey,ax,ay)==vs_side(cx,cy,ex,ey,bx,by)) continue;   /* move ends on the same side of the seg -> no cross */
        g_doorsec=sec; g_doortgt=ve_uselo[s]; g_doorprog=0; g_doorstate=1; g_doorstay=1; SND(6); return;   /* crossed -> open-stay */
    }
}
#endif

/* ===== STACK HIGH-WATER PROFILER. Work RAM is the binding constraint: statics end at `_end` (~0x10D6C4)
   and the m68k supervisor stack grows DOWN from 0x10F300, so the WHOLE stack budget is ~7.2KB. At boot the
   free region is painted with a sentinel byte; the deepest the stack ever reaches overwrites it, and the
   debug HUD scans for the high-water. `stk` = peak bytes the stack has used, `fr` = bytes still free between
   the high-water and the statics (fr -> 0 means the stack is about to corrupt .bss = the crash under investigation). ===== */
#define STK_TOP  0x10F300u
#define STK_SENT 0xA5
extern char _end[];                                   /* linker symbol: end of .bss/.data in RAM = stack floor */
static void stack_paint(void){
    unsigned long sp; __asm__ volatile("move.l %%sp,%0":"=r"(sp));   /* shallow SP at boot */
    for(volatile unsigned char *p=(unsigned char*)_end; p<(unsigned char*)(sp-256); p++) *p=STK_SENT;   /* fill below the current SP, 256B clear of the live frame */
}
static int stack_free(void){                          /* sentinel-intact bytes from _end up to the high-water */
    unsigned char *p=(unsigned char*)_end;
    while((unsigned long)p<STK_TOP && *p==STK_SENT) p++;
    return (int)((unsigned long)p-(unsigned long)_end);
}
static int stack_peak(void){ return (int)(STK_TOP-(unsigned long)_end)-stack_free(); }   /* peak bytes used = region - free */
#if INTER_HAVE
/* INTERMISSION: the DOOM WIMAP0 level-complete background (a real WAD asset), shown on level EXIT.
   Mirrors the TITLEPIC draw -- per-tile palettes to slots 8.., the tilemap as sprites 1..INTER_COLS,
   y-centred (13 of 14 rows -> 8px letterbox). Blocks until START/fire or a ~3s timeout. */
static void show_interpic(void){
    park_all_sprites(); ng_cls();
    for(int p=0;p<INTER_NPAL && p<248;p++) for(int i=0;i<16;i++) MMAP_PALBANK1[(8+p)*16+i]=INTER_PAL16[p][i];
    { int s=1; int TR=INTER_ROWS-1;
      for(int tx=0;tx<INTER_COLS;tx++){
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+s*64;
        for(int ty=0;ty<TR;ty++){ int T=INTER_TILE0+ty*INTER_COLS+tx;
          *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)(((8+INTER_MAP[ty][tx])<<8)|(((T>>16)&0xF)<<4)); }
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+s;
        *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((((496-((224-TR*16)/2))&0x1FF)<<7)|TR); *REG_VRAMRW=(u16)((tx*16)<<7); s++; }
    }
    { u8 pst=0xff, pa=0xff; u16 t=0;
      for(;;){ ng_wait_vblank();
        u8 st=(u8)~(*REG_STATUS_B), jp=(u8)~(*REG_P1CNT);
        if(((st&CNT_START1)&&!(pst&CNT_START1)) || ((jp&CNT_A)&&!(pa&CNT_A))) break;   /* START or fire advances */
        if(t>=180) break;                                                              /* auto-advance after ~3s */
        pst=st; pa=jp;
        ng_center_text(25,0, ((t>>4)&1) ? "  PRESS START  " : "               "); t++; } }
}
/* EXIT -> intermission -> next map: show WIMAP0, then restore the game palettes + enter map m (mirrors boot). */
static void show_interpic_and_enter(int m){
    show_interpic();
    init_palettes();                                      /* restore base/system palettes the interim clobbered (slots 8..) */
    park_all_sprites();
    MMAP_PALBANK1[255*16+15]=0x0210;                      /* deep-murk backdrop */
    MMAP_PALBANK1[4095]=MURKBG[g_murkbg];                 /* far-field backdrop */
    vs_ceil_pal();   /* INC2: bank 12 = depth-fade ramp (or flat if cmap off) */
    vs_floor_pal();   /* INC2: bank 13 = depth-fade floor ramp, dimmed by cmap */
    g_map=m; vs_set_map(m);                               /* per-map walls/flats/sprites + snap camera to spawn */
    draw_status_bar();
    g_redraw=1;
}
#else
static void show_interpic_and_enter(int m){ g_map=m; vs_set_map(m); g_redraw=1; }   /* no WIMAP0 asset -> instant transition */
#endif
/* ===== CFG PRESETS (CFGP[], Monte-Carlo candidates) -- the SINGLE preset system: gameplay-P AND debug param 26 (cf) both cycle these. Count the framerate by feel.
   Each row sets the full perf-dial vector. Indices: dd->DD[], col->NCOLW[], bud->BUDGET[], mmin->MURKMIN[],
   dc->DC[], ncl->NCLIP[], fl/cl->FCLOD. The rest are direct toggles/levels. (From the dead-governor finding:
   BALANCE/FLOOR drop dd+mmin so g_murk_eff actually pulls in -> the deep-opening WALK gets pruned.) */
typedef struct { const char *nm; unsigned char dd,col,bud,mmin,ease,dci,nclp,flod,clod,vpw,vph,gen,zon,ncul,bclp,occl,skip,hmap,vmap,cap,rad,seam,hwt; } cfgpreset_t;
static const cfgpreset_t CFGP[]={
  /*               dd col bud mmin ea dc ncl fl cl vw vh gn zn nc bc oc sk hm vm cp rd sm hw  (23 fields). Updated 2026-06-22: DEFAULT=the boot config (dd750/mn250/dc7/nclp16/hmap3/vmap3); perf presets carry ease=4 (adaptive far-cull) + FLOOR drops hmap0/vmap0 (no perspective divide). */
  {"DEFAULT",      14, 0, 26,  5, 0, 4, 1, 0, 0, 0, 5, 1, 0, 1, 1, 1, 1, 3, 3, 0, 0, 0, 1},
  {"BALANCE",       9, 0, 18,  4, 4, 6, 2, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 3, 3, 0, 0, 0, 1},
  {"HIGH-DD",      18, 0, 22, 11, 4, 6, 2, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 3, 3, 0, 0, 0, 1},
  {"CLARITY",       9, 1, 14,  4, 4, 6, 2, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 3, 3, 0, 0, 1, 1},
  {"FLATS",         8, 0, 18,  6,12, 6, 2, 2, 2, 2, 2, 0, 1, 1, 1, 1, 1, 3, 3, 0, 0, 0, 1},
  {"MAX-QUAL",     11, 1, 22, 10,16, 6, 2, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 3, 3, 0, 0, 1, 1},
  {"FLOOR",         3, 0,  2, 29, 0, 3, 2, 9, 9, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1},
};
#define NCFGP ((int)(sizeof(CFGP)/sizeof(CFGP[0])))
static int g_cfgsel=0, g_cfgflash=0;
static void cfg_apply(int i){ const cfgpreset_t *P=&CFGP[i];
  g_dd=P->dd; g_ncoli=P->col; g_budgeti=P->bud; g_murkmin=P->mmin; g_dci=P->dci; g_nclip=P->nclp;   /* P->ease no longer applied: ease(6) repurposed to the wall-fog curve, decoupled from the (deprecated) presets */
  g_floorlod=P->flod; g_ceillod=P->clod; g_vpw=P->vpw; g_vph=P->vph;
  g_generic=P->gen; g_zonal=P->zon; g_ncull=P->ncul; g_bandclip=P->bclp; g_occl=P->occl; g_skip=P->skip;
  g_hmap=P->hmap; g_vmap=P->vmap; g_capmode=P->cap; g_radial=P->rad; g_seamover=P->seam; g_hwtail=P->hwt;   /* vmap now preset-driven too -> FLOOR can drop to vmap0 (no perspective divide); quality presets keep vmap3 */
  g_redraw=1; }
/* FINAL-TESTING defaults (2026-06-23): the SINGLE source for the boot config + the R reset.
   Derived state (g_vs_murk/g_dcap/g_vs_budget/g_vs_near) recomputes each frame from these indices (~line 2020). */
static void vs_defaults(void){
    g_dd=14; g_dci=4; g_ncoli=1; g_murkmin=3; g_fogext=6; g_floorlod=0; g_ceillod=1;
    g_vpw=0; g_vph=0; g_murkease=16; g_fogcrv=7; g_fog0=196; g_fog1=350;   /* fog35 (FOGEXT[6]=35 -> g_fog1=350) x fcrv7 (FOGBIAS[7]=56 -> g_fog0=196) */
    g_zonal=0; g_generic=1; g_occl=1; g_capmode=0; g_opensky=1; g_radial=0; g_hclip=0;
    g_lutcull=1; g_hwtail=1; g_ncull=1; g_skip=1; g_hmap=2; g_vmap=2; g_nclip=2; g_bspviz=0;
    g_bandclip=1; g_dpri=1; g_budgeti=23; g_actddi=5; g_maxact=2; g_bxcapi=18; g_pstepi=3;
    g_govtgt=5; g_murkbg=1; g_cmceil=1; g_props=1; g_wbob=1; g_seamover=1; g_cfgsel=0;
}
int main(void){
    vs_defaults();                  /* boot: the final-testing config (overrides the decl initialisers) */
    stack_paint();                  /* PROFILER: sentinel-fill the stack region before anything deepens it */
    ng_cls();
    init_palettes();
    /* (removed: tables_init/map_load/world_init/cam=MAP_START -- the dead on-rails engine's boot. The live
       VSLICE path takes ALL geometry/collision from vs_e1.h via vs_set_map(); none of it read these.) */
#if TITLE_FLOW
    /* ===== TITLE (DOOM TITLEPIC) -> MENU (level select) -> LOADING -> game.  TITLE_FLOW 0 = skip. ===== */
    /* --- TITLE --- */
    park_all_sprites(); ng_cls();
#if TITLE_HAVE
    for(int p=0;p<TITLE_NPAL && p<248;p++) for(int i=0;i<16;i++) MMAP_PALBANK1[(8+p)*16+i]=TITLE_PAL16[p][i];   /* TITLEPIC palettes -> slots 8.. (init_palettes restores them after) */
    { int s=1; int TR=TITLE_ROWS;                            /* draw ALL 14 rows: the title is now baked NATIVE + vertically centred (wad2c.py) -> no stretch, no palette-merge glitch, so no row drop. 14 rows = 224px fills the active area; the baked letterbox shows the full title un-cropped (2026-06-23). */
      for(int tx=0;tx<TITLE_COLS;tx++){
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+s*64;
        for(int ty=0;ty<TR;ty++){ int T=TITLE_TILE0+ty*TITLE_COLS+tx;
          *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)(((8+TITLE_MAP[ty][tx])<<8)|(((T>>16)&0xF)<<4)); }
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+s;
        *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((((496-((224-TR*16)/2))&0x1FF)<<7)|TR); *REG_VRAMRW=(u16)((tx*16)<<7); s++; }   /* y-CENTRED: 13 rows (208px) -> 8px top+bottom letterbox */
    }
#else
    ng_center_text( 6,0, "D O O M - N G");
    ng_center_text( 9,0, "EPISODE 1");
#endif
    ng_center_text(26,0, DNG_VERSION);
    SND(0x10+9);                                        /* TITLE: play the intro track (slot 9, after the 9 maps) -> switches to the map track at level start */
    { u8 pst=0xff, pa=0xff; u16 t=0;                          /* wait for START (or fire); blink the prompt */
      for(;;){ ng_wait_vblank();
#ifdef VS_DIAG
        break;   /* DIAG capture: skip the title wait -> straight to teleported gameplay */
#endif
        if(MENU_CAP){ break; }/*DBG headless capture: skip the title wait*/
        if(t==8) SND(0x10+9);   /* TITLE music re-assert a few frames in, in case the Z80 wasn't yet processing commands at the first send */
        u8 st=(u8)~(*REG_STATUS_B), jp=(u8)~(*REG_P1CNT);
        if(((st&CNT_START1)&&!(pst&CNT_START1)) || ((jp&CNT_A)&&!(pa&CNT_A))){ SND(1); break; }
        pst=st; pa=jp;
        ng_center_text(24,0, ((t>>4)&1) ? "  PRESS START  " : "               "); t++; }
    }
    /* --- NEW GAME: episode select -> skill select. The TITLEPIC left its 242 palettes in slots 8+;
       the graphic menus reuse those slots, then init_palettes() restores the game's before play. --- */
    int skill=2, startmap=0;                                 /* default HURT ME PLENTY; startmap = chosen episode*9 (E1M1=0, E2M1=9, E3M1=18, E4M1=27) */
#ifndef VS_DIAG
#if MENU_HAVE
    { static const int EPI[4]={ML_EPI1,ML_EPI2,ML_EPI3,ML_EPI4};
      static const int SKL[5]={ML_JKILL,ML_ROUGH,ML_HURT,ML_ULTRA,ML_NMARE};
#if MENU_CAP!=2
      startmap = doom_menu(ML_EPISOD,54, -1,0, EPI,4, 4, 0) * 9;   /* ALL 4 Ultimate episodes unlocked -> start map = episode*9 (E1M1/E2M1/E3M1/E4M1) */
#endif
      skill = doom_menu(ML_SKILL,54, ML_NEWG,96, SKL,5, 5, 2); /* skill: all 5, default HURT ME PLENTY */
    }
#else
    menu_select("WHICH EPISODE?", EPISODES, 3, 1, 0);
    skill = menu_select("CHOOSE YOUR SKILL", SKILLS, 5, 5, 2);
#endif
#endif /* !VS_DIAG: diag capture skips title+menus -> straight to teleported gameplay */
    (void)skill;
    init_palettes();                                          /* restore game palettes the title/menu clobbered */
    park_all_sprites(); ng_cls();                             /* brief black, then cut into the level */
    for(int f=0;f<24;f++) ng_wait_vblank();
#endif
    /* BIOS FORCED-START NEUTRALIZER (the "crash at spawn" root cause): nullbios's per-vblank
       SYSTEM_IO has a credit/forced-start routine (BIOS 0xc045f8) that, whenever bios_user_mode!=0
       and it believes START is pressed (or its compulsion countdown expires), JSRs the cart's
       entry (0x128) -> main() RE-RUNS = the deterministic "crash"/reset loop seconds into gameplay
       (traced instruction-by-instruction in the emulator: no exception, no watchdog -- the BIOS
       simply restarts the cart). The cart drives its own start flow, so keep bios_user_mode=0 during gameplay:
       that routine then exits immediately (beq at 0xc04602) and can never yank the cart. */
    bios_user_mode=0;
    SND(0x10+MUSIC[startmap]);                                /* start THIS map's soundtrack (per-map track via MUSIC[]: E1/E2/E3 distinct, E4 reuses); at the level, not the title */
    MMAP_PALBANK1[255*16+15]=0x0210;                          /* BACKDROP = deep murk (was black): undrawn far regions read as atmospheric depth, not void */
#if VSLICE
    {   /* VERTICAL SLICE -- never returns. Free movement in one room, live raycast walls. */
        park_all_sprites();
        MMAP_PALBANK1[4095]=MURKBG[g_murkbg];   /* far-field backdrop (shuttle 9): default black; cycle for atmospheric murk */
        vs_ceil_pal();   /* INC2: bank 12 = depth-fade ramp (or flat if cmap off) -- VS ceiling palette */
        vs_floor_pal();   /* INC2: bank 13 = depth-fade floor ramp, dimmed by cmap (was raw VSFLOOR_PAL16) */
        vs_set_map(startmap);   /* select the chosen episode's first map (episode*9): repoint geometry pointers, snap camera/eye to spawn, upload per-map palettes */
        draw_status_bar();   /* DOOM status bar on the fix layer, rows 24-27 (static; persists across frames + map toggles) */
        /* (P2/NODES bank-switch removed -- VSLICE is pure ROM1 live-BSP, never reads the banked on-rails nodes) */
        for(int d=1;d<VS_RFMAX;d++){ long v=((long)VS_FOCAL<<8)/d; g_rf[d]=(short)(v>32767?32767:v); }   /* reciprocal LUT: 1/depth, once at startup. CLAMP to short-max: d==1 -> 40960 would wrap NEGATIVE (garbage sx/tc/dpix); only reachable via nclip=0, but cheap to harden (audit finding). */
        g_rspan[0]=0; g_rspan[1]=65535; for(int s=2;s<VS_SPANMAX;s++) g_rspan[s]=(unsigned short)(65536U/s);   /* 1/span reciprocal LUT (s=1 clamped to fit u16) */
        int px=vs_camx, py=vs_camy, ang=vs_camang; u16 fc=0;   /* player-1 spawn of the current map (from vs_set_map) */
#ifdef VS_DIAG
        px=VS_DIAG_X; py=VS_DIAG_Y; ang=(VS_DIAG_A)&255;             /* teleport to the coord under test (VS_DIAG_A in 0..255 cart units) */
#ifdef VS_DIAG_ZON
        g_zonal=VS_DIAG_ZON; /* pick the zonal mode under test */
#else
        g_zonal=1;           /* capture the ZONAL render */
#endif
#ifdef VS_DIAG_GEN
        g_generic=VS_DIAG_GEN; /* 0 = real flats, 1 = synthetic (boot default) */
#endif
#ifdef VS_DIAG_LCUL
        g_lutcull=VS_DIAG_LCUL; /* force the LUT-cull on/off for a capture */
#endif
#ifdef VS_DIAG_FB
        g_fb_mode=VS_DIAG_FB; g_untex=(VS_DIAG_FB==2);   /* 2 = unshaded view (the FB mode 1 needs case-47's runtime setup, so it isn't captured this way) */
#endif
        g_dd=10; g_dci=8;    /* dd=950 (max) / dc=16 -- capture config (DD[] is now the 450..950 sweet-spot range) */
#ifdef VS_DIAG_COL
        g_ncoli=VS_DIAG_COL; /* capture at a chosen column-res (0=col20..4=col80) to reproduce high-strip overflow */
#endif
#ifdef VS_DIAG_WPN
        g_weapon=VS_DIAG_WPN; /* capture a chosen weapon (0=fist..5=chainsaw) */
#endif
#ifdef VS_DIAG_BOBC
        g_bobc=VS_DIAG_BOBC;  /* force the bob accumulator to capture a chosen bob phase statically */
#endif
#ifdef VS_DIAG_FCLOD
        g_floorlod=VS_DIAG_FCLOD; g_ceillod=VS_DIAG_FCLOD;  /* force the floor/ceiling crop for a static capture */
#endif
#endif
        int rc_us=0, cad=1, worst=1;     /* raycast cost (vbl per 64 solves), frame cadence, worst */
        u16 prev=g_vbl;
        for(;;){
            ng_wait_vblank();
            u16 now=g_vbl; cad=(int)(u16)(now-prev); prev=now;        /* HONEST cadence: vblanks the last frame's work spanned */
            g_fcost[g_fci]=(unsigned char)(cad>255?255:cad); g_fci=(g_fci+1)&(FCN-1);   /* SCOPE: log every frame's vblank cost (moving + idle); 'worst' is recomputed from this ring at HUD time so it tracks the RECENT peak, not an all-time max that never recovers */
            if(g_govtgt>0 && g_lastrendered){   /* GRACEFUL FAR-DROP governor: drive off g_rcost (vs_render's OWN vblank cost -> EXCLUDES the debug HUD, so it works HUD-up). AIMD: always extend gently, pull PROPORTIONAL to the overshoot -> reaches the target + recovers. */
                int lo=MURKMIN[g_murkmin]; if(lo<64)lo=64; int hi=DD[g_dd]; int gtgt=60/GOVFPS[g_govtgt];   /* vblank target from the fps dial (60fps->1 .. 6fps->10) */
                g_govmurk += 16;                                            /* always extend gently (recovery) */
                if(g_rcost>gtgt) g_govmurk -= (g_rcost-gtgt)*48;            /* over target -> pull HARD in proportion to how far over (fast on a big spike, gentle near target) */
                if(g_govmurk<lo)g_govmurk=lo; if(g_govmurk>hi)g_govmurk=hi;
            }
            if(g_perfen && g_lastrendered){   /* perfP (param 12): STANDALONE auto-LOD governor -- holds ~1 vblank by shedding ceiling-then-floor LOD under sustained load. Independent of gov. Hysteresis (build>1 vbl, release<=0) avoids thrash. */
                if(g_rcost>1){ if(g_perfP<PERFP_MAX)g_perfP++; }            /* render took >1 vblank -> build pressure */
                else if(g_rcost<=0){ if(g_perfP>0)g_perfP--; }             /* fit comfortably under a vblank -> release */
                g_perf_clod=g_perfP>>3;                                      /* ceiling rows shed first (0..3 over perfP 0..24) -- ceiling LOD less noticed */
                g_perf_flod=(g_perfP>12)?((g_perfP-12)>>3):0;              /* floor rows shed later, 0..1 */
            } else { g_perfP=0; g_perf_clod=0; g_perf_flod=0; }            /* perfP OFF -> inert (default) */
            u8 in=(u8)~(*REG_P1CNT);
            int fcs=VS_CS(ang), fsn=VS_SN(ang);
            int dx=0,dy=0, prevx=px, prevy=py;   /* prev pos for the walk-trigger crossing test */
            if(!g_dbg){   /* movement FROZEN while the debug HUD is up -> WASD navigates the param grid instead (shuttle below) */
            if(in&CNT_UP){   dx+=(fcs*14)>>8; dy+=(fsn*14)>>8; }
            if(in&CNT_DOWN){ dx-=(fcs*14)>>8; dy-=(fsn*14)>>8; }
            }
            /* per-axis slide, with a VS_RAD skin: test the destination extended by the radius in the
               travel direction so the camera stops VS_RAD short of the wall (no embedding) */
            if(dx){ int ex=dx+(dx>0?VS_RAD:-VS_RAD); if(vs_move_ok(px,py,px+ex,py)) px+=dx; }
            if(dy){ int ey=dy+(dy>0?VS_RAD:-VS_RAD); if(vs_move_ok(px,py,px,py+ey)) py+=dy; }
            vs_walk_triggers(prevx,prevy,px,py);   /* WALK-trigger doors: crossing a trigger seg opens its tagged door */
            g_fwdph += (int)(((long)(px-prevx)*fcs + (long)(py-prevy)*fsn) >> 8);   /* FLOW PHASE (#40): project the ACTUAL applied move (post-collision) onto the facing unit vector (fcs/fsn = unit*256) -> signed forward distance. Accumulate so floor/ceiling scroll runs forward for every heading + zero on pure turns. */
            if(g_props) for(int i=0;i<ve_nth;i++){   /* ITEM PICKUP: walk over an armour/ammo billboard -> it vanishes + the DSITEMUP chime (SND 5). HUD counters are baked-static, so it's collect+chime only. */
                if(g_thalive[i]!=1 || ve_thc[i]<CLS_ITEM0) continue;   /* alive items (cls>=4) only -- skip barrels/enemies/corpses */
                short ix=(short)(ve_thx[i]-px), iy=(short)(ve_thy[i]-py);
                if((long)ix*ix+(long)iy*iy < 1600L){ g_thalive[i]=0; SND(5); } }   /* within ~40u -> collected (chime; last pickup this frame wins the one SFX channel) */
            if(!g_dbg && (dx||dy||(in&(CNT_LEFT|CNT_RIGHT)))) g_bobc++;   /* advance the gun-bob while walking OR TURNING (bob on rotation too); holds steady only when fully still (and while debugging) */
            if(g_fire>0) g_fire--;                            /* FIRE recoil countdown */
            if(g_sndqt>0 && --g_sndqt==0) SND(g_sndq);        /* delayed death SFX (scream/boom) after the gun pop */
            if(g_doorstate==1){ g_doorprog+=6;
                if(g_doorprog>=g_doortgt){ g_doorprog=g_doortgt; g_secdc[g_doorsec]=(short)g_doortgt;     /* fully open */
                    if(g_doorstay){ g_doorsec=-1; g_doorstate=0; g_doorstay=0; }                          /* TRIGGER door: stay open, free the machine for the next one */
                    else { g_doorstate=2; g_doorhold=80; } }                                              /* MANUAL door: hold, then auto-close */
                else g_secdc[g_doorsec]=(short)g_doorprog; }   /* DOOR opening */
            else if(g_doorstate==2){ if(--g_doorhold<=0){ g_doorstate=3; SND(9); } }   /* hold open, then close (SND 9 = door CLOSE, distinct from the open) */
            else if(g_doorstate==3){ g_doorprog-=6; if(g_doorprog<=0){ g_secdc[g_doorsec]=0; g_doorsec=-1; g_doorstate=0; } else g_secdc[g_doorsec]=(short)g_doorprog; }   /* DOOR closing */
            if(g_liftstate==1){ g_liftprog-=6; if(g_liftprog<=g_lifttgt){g_liftprog=g_lifttgt; g_liftstate=2; SND(11);} g_secdf[g_liftsec]=(short)g_liftprog; }   /* LIFT lowering toward the low stop (drop is <=0); holds DOWN until toggled; SND 11 = lift STOP on arrival */
            else if(g_liftstate==3){ g_liftprog+=6; if(g_liftprog>=0){ g_secdf[g_liftsec]=0; g_liftsec=-1; g_liftstate=0; SND(11); } else g_secdf[g_liftsec]=(short)g_liftprog; }   /* LIFT raising back to rest; SND 11 = lift STOP on arrival */
            if(!g_dbg && (in&CNT_LEFT))  ang=(ang+3)&255;   /* A = turn left (frozen while debug HUD up -> A navigates params left) */
            if(!g_dbg && (in&CNT_RIGHT)) ang=(ang-3)&255;   /* D = turn right */
#ifdef VS_AUTOPAN
            if(!(in&(CNT_UP|CNT_DOWN|CNT_LEFT|CNT_RIGHT))) ang=(ang+1)&255;   /* headless-capture demo only (build -DVS_AUTOPAN); off in play -> no idle drift */
#endif
            /* DEBUG SHUTTLE (edge-detected; WASD=dpad move/turn): SPACE=show/hide HUD, P=cycle param, N=value DOWN, B=value UP. LEVEL is param 15 in the cycle. Dials are EMIT-only -- never override the BSP. */
            { static u8 pin=0; u8 pr=in & ~pin; pin=in;
              if(pr&CNT_A){ g_dbg=!g_dbg;                   /* SPACE: show/hide the debug HUD (clean view + drops the HUD-render overhead) */
                if(!g_dbg){ const char *bl="                                       "; g_vrambusy=1; for(int r=2;r<=15;r++)ng_text(2,r,1,bl); g_vrambusy=0; } }   /* clear the whole debug HUD once on toggle-off (rows 2..15: fps + 9 grid rows + coords/stack/bench/scope) -- was 2..12, so stack/bench/scope "stuck" after toggle-off */
              if(pr&CNT_B){ if(!g_dbg){ g_weapon=(g_weapon+1)%GUNHAND_N; }   /* GAMEPLAY P: cycle WEAPONS (2026-06-23, moved out of the menu). The perf-preset cycle that used to live here is deprecated; presets still reachable via debug param 26 (cf). */
                            else { g_bspviz=!g_bspviz; g_bspstep=0; g_redraw=1; } }   /* DEBUG P: toggle the BSP-WALK viz (separated so cycling presets can't clobber a debug-tuning session) */
              if(g_dbg){                                    /* TUNING: debug keys live ONLY while the HUD is shown (game keys unmapped when debug enabled, debug keys unmapped when playing) */
              { int dp=0; for(int i=0;i<NDISP;i++) if(DSEL[i]==g_sel){ dp=i; break; }   /* WASD grid-nav the param table in DISPLAY order (4 cols): W/S = row up/down (+-4), A/D = col left/right (+-1, linear wrap); P still linear-cycles. N/M adjust the value below. */
                if(pr&CNT_UP)    dp=(dp+NDISP-4)%NDISP;
                if(pr&CNT_DOWN)  dp=(dp+4)%NDISP;
                if(pr&CNT_LEFT)  dp=(dp+NDISP-1)%NDISP;
                if(pr&CNT_RIGHT) dp=(dp+1)%NDISP;
                g_sel=DSEL[dp]; }   /* grid nav = WASD only now; P (CNT_B) is repurposed to toggle the BSP viz */
              { int dd=(pr&CNT_D)?1:((pr&CNT_C)?-1:0);      /* B: value UP / N: value DOWN (bidirectional, wraps) */
                if(dd) switch(g_sel){
                  case 0: g_dd   =(g_dd   +VS_NDD  +dd)%VS_NDD;   break;          /* draw distance */
                  case 1: g_dci  =(g_dci  +VS_NDC  +dd)%VS_NDC;   break;          /* depth cap */
                  case 2: g_ncoli=(g_ncoli+NNCOL   +dd)%NNCOL;    SND(1); break;  /* column res (20/32/40/64/80) */
                  case 3: g_capmode=(g_capmode+NCAPMODE+dd)%NCAPMODE; if(CAP_OPAQUE) vs_upload_cap_pals(); break;      /* cap mode; (re)assert the cap palettes on selecting TB32o so the overlay is coloured without a map reload */
                  case 4: g_zonal=(g_zonal+7+dd)%7; break;                        /* ZONAL flats: 0=off(blanket) /1=zonal /2=bias GREY-into-pit /3=bias PIT-into-grey /4=DETERMINISTIC round boundary /5=round+grey-wins /6=SUB-TILE EXACT floor boundary (per-column overlay -> smooth diagonal, breaks the 16px snap; ~1 slot/boundary). Needs gen=0. */
                  case 5: g_generic=!g_generic; break;                            /* GENERIC mode ON/OFF (synthetic floor/ceil + sky vs real flats) */
                  case 6: g_murkease+=dd*4; if(g_murkease<0)g_murkease=0; if(g_murkease>16)g_murkease=16; break;   /* 'ease': far-horizon TRIM 0..16 step 4 -> ef = dd - ease*16 (terminate far draw distance, always on, visible in ef). */
                  case 7: g_weapon=(g_weapon+GUNHAND_N+dd)%GUNHAND_N; break;      /* WEAPON select: cycle the 6 first-person weapons */
                  case 8: g_murkmin=(g_murkmin+NMURKMIN+dd)%NMURKMIN; break;      /* MURK FLOOR: how far in the far-cull may pull */
                  case 9: g_murkbg=(g_murkbg+NMURKBG+dd)%NMURKBG; MMAP_PALBANK1[4095]=MURKBG[g_murkbg]; break;   /* MURK BACKDROP: far-field colour (apply now) */
                  case 10:g_fogext=(g_fogext+NFOGEXT+dd)%NFOGEXT;                                /* FOG EXTENT: rescale the wall fog-band thresholds; 0 = OFF (push past any depth) */
                          if(FOGEXT[g_fogext]==0){ g_fog0=g_fog1=32767; } else { g_fog1=(1000*FOGEXT[g_fogext])/100; g_fog0=(g_fog1*FOGBIAS[g_fogcrv])/100; } break;   /* fogext sets the far edge g_fog1; fcrv(23) biases the near edge g_fog0 within it */
                  case 11:g_floorlod=(g_floorlod+NFCLOD+dd)%NFCLOD; break;                        /* FLOOR crop (drop FAR/horizon rows, keep bottom) */
                  case 12:g_perfen=!g_perfen; if(!g_perfen)g_perfP=0; break;                       /* perfP: standalone auto-LOD governor on/off (was occl -> now hardwired on). */
                  case 13:g_budgeti=(g_budgeti+NBUDGET+dd)%NBUDGET; break;                        /* DRAW-COUNT CAP: nearest-N wall strips -> flicker/perf lever */
                  case 14:g_cmceil=!g_cmceil; vs_ceil_pal(); vs_floor_pal(); g_redraw=1; break;        /* cmap: A/B the colmap floor+ceiling DIM (on, blends into murk) vs FULL BRIGHT (off), by re-scaling banks 12+13. Both keep the texture + depth fade. */
                  case 15:g_seamover=(g_seamover+4+dd)&3; break;                                   /* SEAM OVERDRAW: 0..3 px strip widen (flicker mask) */
                  case 16:g_ceillod=(g_ceillod+NFCLOD+dd)%NFCLOD; break;                          /* CEILING crop (drop FAR/horizon rows, keep top) */
                  case 17:g_props=!g_props; break;                                                /* PROPS visible: hide actors (enemies/barrels) */
                  case 18:g_vpw=(g_vpw+NVP+dd)%NVP; break;                                        /* FLOOR murk rows (far->horizon) */
                  case 19:g_vph=(g_vph+NVP+dd)%NVP; break;                                        /* CEILING murk rows (far->horizon) */
                  case 20:g_hidehud=g_hidegun=!g_hidegun; g_redraw=1; break;                       /* CLEAN VIEW: hide BOTH weapon + status bar in one toggle (hgun+hhud merged). g_redraw=1 forces a vs_render so the HUD-edge SCB strips (vs_hud_edges, only emitted inside vs_render) restore when unhidden while standing still -- else the static-skip kept them gone. */
                  case 21:g_hidehud=!g_hidehud; g_redraw=1; break;                                 /* (off-grid now; kept for completeness) HIDE player status bar only */
                  case 39:g_bandclip=!g_bandclip; break;                                          /* bclp: VERTICAL BAND-REJECT on/off -- A/B the near-window stutter fix (watch sg/bc + window edges for popping) */
                  case 40:g_dpri=!g_dpri; g_redraw=1; break;                                      /* dpri: DEPTH-PRIORITISED BUDGET on/off -- A/B the dense-courtyard near-wall flicker (on = keep the nearest bud by depth; off = old walk-order budget halt) */
                  case 41:g_actddi=(g_actddi+VS_NDD+dd)%VS_NDD; g_redraw=1; break;                 /* add: ACTOR draw distance (DD[] idx) -- pull monsters/props in independently of the wall dd (E2M5 open-arena lever) */
                  case 42:g_maxact+=dd*2; if(g_maxact<1)g_maxact=1; if(g_maxact>MAXACT)g_maxact=MAXACT; g_redraw=1; break;   /* mxa: runtime nearest-N actor cap (1..MAXACT) */
                  case 43:g_noemit=!g_noemit; g_redraw=1; break;                                  /* nemt: FORENSIC -- skip wall SCB emit (project+clip only). Toggle live: fps jumps => EMIT-bound (VRAM-write floor); fps unchanged => walk/overhead-bound */
                  case 44:g_bxcapi=(g_bxcapi+NBXCAP+dd)%NBXCAP; g_redraw=1; break;                  /* bxc: PHASE 2 walk budget -- cap bx (BSP node-box tests). Dial DOWN on walk-bound (stair/node-dense) maps to bound worst-case; far geometry drops first. 2048=OFF */
                  case 45:g_pstepi=(g_pstepi+NPSTEP+dd)%NPSTEP; g_redraw=1; break;                  /* psd: PHASE 5 perspective-subdivide step (1..32). The hmap3/vmap3 1/z divide runs every PSTEP cols (affine between). 1=exact/most-divides .. 8=default .. 32=fewest. A/B 1 vs 32 to SEE the divide tax in fps */
                  case 46:g_govtgt=(g_govtgt+NGOVFPS+dd)%NGOVFPS; g_redraw=1; break;   /* gov: GRACEFUL FAR-DROP -- TARGET FPS (GOVFPS[]={off,6,10,12,15,30,60}). The far-horizon recedes under load to hold the chosen fps, extends with headroom. Sweep it to read the honest fps<->view-distance shape. */
                  case 47:{ int prevfb=g_fb_mode; g_fb_mode=(g_fb_mode+3+dd)%3;   /* fb now CYCLES: 0=off / 1=FB(Doom64KB chunky framebuffer) / 2=UNSHADED (the old untx untextured-strip debug view, folded onto this toggle) */
                            if(g_fb_mode==1 && prevfb!=1){ g_fb_savecol=g_ncoli; g_ncoli=2; fb_upload_pals(); fb_build_cols(); park_all_sprites(); MMAP_PALBANK1[255*16+15]=0x8000; }   /* enter FB: force col40 + upload the 8 global-palette groups to fix slots 8..15 */
                            else if(g_fb_mode!=1 && prevfb==1){ g_ncoli=g_fb_savecol; g_pweap=-2; g_phidehud=2; g_vrambusy=1; for(int fc2=0;fc2<FBW;fc2++){ *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+fc2*32+FB_ROW0; for(int fr2=0;fr2<FBH;fr2++)*REG_VRAMRW=(u16)SROM_EMPTY_TILE; } g_vrambusy=0; init_palettes(); }   /* leave FB: restore col + clear the FB fix region + redraw HUD/gun over it */
                            g_untex=(g_fb_mode==2); g_redraw=1; } break;   /* mode 2 = unshaded view on the NORMAL vs_render path (sets g_untex); FB-specific paths gate on g_fb_mode==1 */   /* OFF clears the FB's fix-layer region to transparent so VSLICE's SCB 3D shows through (no lingering chunky scene). fb: FRAMEBUFFER-RENDERER SPIKE (Doom64KB-style fix-layer chunky FB) -- A/B vs VSLICE. ON forces col40 + uploads the 8 global-palette groups to fix slots 8..15. OFF restores g_ncoli + init_palettes() + invalidates the gun(g_pweap)/status(g_phidehud) caches so the fix-layer HUD redraws over the FB. The gun/face/status draws are gated on !g_fb_mode so they don't overlay the FB scene. */
                  case 48:g_cmfog=!g_cmfog; vs_fog_bands(); g_redraw=1; break;   /* cmfg: floor/ceiling distance fade via the DOOM COLORMAP curve+hue (1) vs the linear RGB scale (0). Re-uploads ONLY the 5 band slots (vs_fog_bands) -> never touches the gun/HUD. A/B on the gen=1 blanket floor/ceiling. */
                  case 22:g_doorwalk=!g_doorwalk; break;                                          /* DOORS walk-through (no open) */
                  case 23: g_fogcrv+=dd; if(g_fogcrv<0)g_fogcrv=0; if(g_fogcrv>NFOGBIAS-1)g_fogcrv=NFOGBIAS-1;   /* 'fcrv' WALL-FOG CURVE (replaced 'rad', deprecated 2026-06-23): bias the near fog edge front(0)..rear(N-1). */
                          if(FOGEXT[g_fogext]!=0){ g_fog0=(g_fog1*FOGBIAS[g_fogcrv])/100; } break;
                  case 24:g_vmap=(g_vmap+4+dd)%4; g_redraw=1; break;                             /* VERTICAL MAP: 0=stretch all / 1=companion all (1:1+tile+peg, affine d) / 2=selective (companion posters, stretch tiling) / 3=companion all + PERSPECTIVE-CORRECT depth (level tiling on angled walls) */
                  case 26:g_cfgsel=(g_cfgsel+NCFGP+dd)%NCFGP; cfg_apply(g_cfgsel); break;   /* CFG PRESET: cycle the 23-field CFGP[] (DEFAULT..FLOOR) -- SAME selector + apply as the gameplay-P key (cfg_apply sets g_redraw) */
                  case 27: vs_defaults(); vs_ceil_pal(); vs_floor_pal(); g_redraw=1; break;   /* RESET to the final-testing defaults (vs_defaults = the single boot+reset source); re-scale the colmap floor/ceiling banks. */
                  case 28:g_vproj=!g_vproj; break;                                                /* VPROJ: per-vertex projection cache toggle (repurposed from the dead hclp flat-clip) */
                          if(FOGEXT[g_fogext]==0){g_fog0=g_fog1=32767;}else{g_fog1=(1000*FOGEXT[g_fogext])/100;g_fog0=(g_fog1*FOGBIAS[g_fogcrv])/100;} g_murkbg=1; MMAP_PALBANK1[4095]=MURKBG[1]; break;
                  case 29:g_lutcull=!g_lutcull; break;                                            /* LUT-CULL: park floor/ceiling LUT blocks fully hidden behind the nearest wall (A/B the cad/tile delta) */
                  case 30:g_hwtail=!g_hwtail; break;                                              /* SCB TAIL-CLEAR: high-water (1) vs old pad-to-16 (0) -- A/B that high-water isn't slower */
                  case 31:g_untex=!g_untex; break;                                                /* DEBUG: untextured sprite-budget view (see strip count + hw shrink) */
                  case 32:g_wbob=!g_wbob; g_pweap=-1; break;                                      /* WBOB: weapon-bob on/off (g_pweap=-1 forces a gun redraw). Replaced fdbg (flat-source debug deprecated). */
                  case 33:g_bench=g_bench?0:1; break;                                             /* bnch: request ONE on-demand timing burst (1=run next frame, 2=done/showing, 0=clear) */
                  case 34:g_skip=g_skip?0:1; g_redraw=1; break;                                   /* skip: STATIC-FRAME SKIP on/off (A/B the idle-frame win); force one render on toggle so the view is fresh */
                  case 35:g_ncull=!g_ncull; g_redraw=1; break;                                     /* ncul: NODE FAR-CULL on/off -- prune BSP subtrees beyond the far-horizon (cuts the walk/pj). A/B the far visuals + the pj delta (best at playable dd / dense views) */
                  case 36:g_nclip=(g_nclip+NNCLIP+dd)%NNCLIP; break;                               /* nclp: NEAR-CLIP distance 0..200 -- push the near plane out to clip the huge near walls (perf probe) */
                  case 37:g_bspviz=!g_bspviz; g_bspstep=0; g_redraw=1; break;                       /* bspv: BSP TRAVERSAL VIZ -- depth-coloured walls revealed front-to-back, slowly, to show the live BSP walk */
                  case 38:g_hmap=(g_hmap+4+dd)%4; g_redraw=1; break;                               /* HMAP horizontal wall-U: 0=screen-mapped / 1=affine (swims) / 2=selective (posters only) / 3=PERSPECTIVE-CORRECT (glued + swim-free, ~1 divide/col). Separate from vmap(24) again (2026-06-23). */
                  default:g_map=(g_map+VE_NMAP+dd)%VE_NMAP; vs_set_map(g_map); px=vs_camx; py=vs_camy; ang=vs_camang; SND(0x10+MUSIC[g_map]); break;   /* LEVEL: switch the map's soundtrack via MUSIC[] (idx 25 = default) */
                } if(dd) g_redraw=1; }   /* static-skip: any dial adjust forces a fresh render so the change is visible even when standing still */
              } else {                                      /* PLAYING: game keys live while the HUD is hidden */
                if(pr&CNT_C){ g_fire=6; SND(1); vs_shoot(px,py,ang); }   /* C: FIRE -- always the gun pop NOW; a HIT queues the scream/boom ~2 frames later (gun, then scream) */
                if(pr&CNT_D){ int a=vs_use_special(px,py,ang);   /* D: USE -- nearest LIFT/EXIT first, else a door */
                  if(a==2){ show_interpic_and_enter((g_map+1)%VE_NMAP); px=vs_camx; py=vs_camy; ang=vs_camang; SND(0x10+MUSIC[g_map]); }   /* EXIT switch -> INTERMISSION (DOOM WIMAP0) -> next map (resync camera; debug LEVEL dial stays instant) */
                  else if(!a) vs_use_door(px,py,ang); }
              }
              /* DD[] moved to file scope (vs_render's actor-dd cap reads it); g_vs_murk=DD[g_dd] still resolves below */
              static const short DC[VS_NDC]={1,2,3,4,5,6,7,8,16,32,48};       /* per-column see-through DEPTH cap (layers); fine low range 1..8 + originals; 48 ~= effectively unlimited */
              g_vs_murk=DD[g_dd]; g_dcap=DC[g_dci]; g_vs_budget=BUDGET[g_budgeti]; g_vs_near=NCLIP[g_nclip]; }   /* draw-count cap from the dial (param 13); near-clip from the dial (param 36) */
            { int cl=g_ceillod+g_perf_clod; if(cl>NFCLOD-1)cl=NFCLOD-1; int fl=g_floorlod+g_perf_flod; if(fl>NFCLOD-1)fl=NFCLOD-1;
              g_floor_rows=FCLOD_F[fl]; g_ceil_rows=FCLOD_C[cl]; }   /* floor crop (11) + ceiling crop (16) + perfP auto-LOD steps (lever 1/2 of the gov governor) */
#ifdef VS_AUTOFIRE
            if((fc%40)==20){ g_fire=6; if(!vs_shoot(px,py,ang))SND(1); }   /* DBG: headless auto-fire to verify combat/explosion FX */
#endif
            g_anim++;                                                /* advance the actor anim clock EVERY frame (hoisted from vs_render) so it ticks even on skipped frames */
            if(g_bspviz){ g_bspstep+=2; if(g_bspstep>320)g_bspstep=0; }   /* BSP VIZ (37): ramp the front-to-back reveal each frame, then loop the sweep */
            { int anyfx=0; for(int f=0;f<NFX;f++) if(g_fxt[f]){anyfx=1;break;}   /* STATIC-FRAME SKIP (#1, param 34): reuse the SCB already in VRAM when nothing visible changed -> idle frames cost ~0 */
              int aph=(g_anim>>3)&1;
              int stat = g_skip && !g_redraw && !g_bspviz && px==s_px && py==s_py && ang==s_ang
                  && (!g_drewactors || aph==s_aph)        /* actor anim phase only matters when actors are on-screen */
                  && !g_doorstate && !g_liftstate         /* a door/lift mid-cycle moves the geometry every frame */
                  && !g_fire && !anyfx;                   /* muzzle flash + explosion FX both live in the render */
              if(!stat){ u16 rt0=g_vbl; if(g_fb_mode==1){ fb_render(px,py,ang); g_fbcost=(int)(u16)(g_vbl-rt0); } else { vs_render(px,py,ang,1); g_rcost=(int)(u16)(g_vbl-rt0); } s_px=px; s_py=py; s_ang=ang; s_aph=aph; g_redraw=0; g_lastrendered=1; } else { g_skipcnt++; g_lastrendered=0; } }   /* render only on change; gun/HUD are fix-layer so they persist when skipped. skp counter climbs while idle. g_lastrendered gates the far-drop governor (idle frames mustn't push the horizon back out). FB SPIKE: g_fb_mode swaps in the framebuffer renderer (VSLICE path preserved verbatim in the else). */
            if(g_cfgflash>0 && --g_cfgflash==0) ng_center_text(12,1,"                    ");   /* clear the perf-preset name flash after ~2s */
            if(g_fb_mode!=1){                                          /* FB SPIKE: gun/face/status all live on the fix layer the FB writes -> skip them in FB mode (mode 1 only; mode 2 unshaded is the normal render path) (the FB blit overwrites their cells; cleared/restored via the cache invalidation in case 47). */
            draw_gun();                                              /* first-person weapon on the fix layer (redraws only on weapon change) */
            if(g_hidehud!=g_phidehud){ g_phidehud=g_hidehud;        /* HIDE HUD (param 21): the status bar is static -> clear/redraw once on toggle */
                if(g_hidehud){ for(int col=0;col<40;col++){ *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+col*32+HUD_ROW; for(int r=0;r<4;r++)*REG_VRAMRW=(u16)SROM_EMPTY_TILE; } }
                else { draw_status_bar(); g_faceN=-1; } }            /* un-hide: redraw bar + force the face to repaint */
            if(!g_hidehud) draw_face(fc);                            /* HUD face idle look-around (redraws only on frame change) */
            }
            (void)rc_us; fc++;                                        /* (benchmark burst removed -- it WAS the periodic big stall) */
            /* ONE-SHOT perf breakdown (frame 180): isolate CPU (proj) vs LUT vs wall-emit.
               proj32 = 32x vs_render(emit=0) = BSP+projection, NO SCB writes (returns at line ~1500).
               lut32  = 32x vs_lut = the 40 floor/ceiling/sky sprites. strips = this frame's wall count.
               wall-emit/frame ~= cad - proj32/32 - lut32/32. */
#if VS_BENCH
            /* FORENSIC SWEEP (frame 180): real-frame cost (16x vs_render emit=1, EARLY-OUT INTACT) under
               4 configs -> A=all-far, W=walls-off, P=planes-off, B=bare. wall=A-W, planes=A-P, bare=B. */
            static int g_bd=0, g_fa=0,g_fw=0,g_fp=0,g_fb=0,g_fe=0, g_bs=0, g_bbx=0, g_sgn=0;
            if(fc==180 && !g_bd){
                g_bs=vs_spr-41; g_bbx=g_bbox_n; g_sgn=g_seg_n;            /* from the real frame just rendered */
                int sfr=g_floor_rows,scr=g_ceil_rows,sbg=g_vs_budget; u16 t;
                g_floor_rows=4;g_ceil_rows=6;g_vs_budget=48; t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_fa=(int)(u16)(g_vbl-t);
                g_floor_rows=4;g_ceil_rows=6;g_vs_budget=0;  t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_fw=(int)(u16)(g_vbl-t);
                g_floor_rows=0;g_ceil_rows=0;g_vs_budget=48; t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_fp=(int)(u16)(g_vbl-t);
                g_floor_rows=0;g_ceil_rows=0;g_vs_budget=0;  t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_fb=(int)(u16)(g_vbl-t);
                g_floor_rows=0;g_ceil_rows=0;g_vs_budget=48;g_noemit=1; t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_fe=(int)(u16)(g_vbl-t); g_noemit=0;  /* E: walls projected+clipped, NO SCB emit -> wall PROJ only */
                g_floor_rows=sfr;g_ceil_rows=scr;g_vs_budget=sbg; vs_render(px,py,ang,1);  /* restore */
                g_bd=1;
            }
#endif
            if(g_bench==1){    /* bnch: ON-DEMAND per-stage benchmark -- one 16x burst per config at the current view, ~1-2s hitch, then freeze. Mirrors the dev VS_BENCH but runtime. */
                int sfr=g_floor_rows,scr=g_ceil_rows,sbg=g_vs_budget; u16 t;
                if(g_fb_mode==1){ t=g_vbl; for(int i=0;i<16;i++)fb_render(px,py,ang); g_bn_fa=(int)(u16)(g_vbl-t); g_bn_fp=g_bn_fb=g_bn_fe=0; g_bench=2; }   /* FB SPIKE: 16x fb_render -> g_bn_fa = vblank total for 16 frames. cyc/frame = fa*12500; fps = 960/fa. */
                else {
                g_floor_rows=4;g_ceil_rows=6;g_vs_budget=48; t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_bn_fa=(int)(u16)(g_vbl-t);   /* FULL */
                g_floor_rows=0;g_ceil_rows=0;g_vs_budget=48; t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_bn_fp=(int)(u16)(g_vbl-t);   /* walls, NO flats */
                g_floor_rows=0;g_ceil_rows=0;g_vs_budget=0;  t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_bn_fb=(int)(u16)(g_vbl-t);   /* baseline (no walls/flats) */
                g_floor_rows=4;g_ceil_rows=6;g_vs_budget=48;g_noemit=1; t=g_vbl; for(int i=0;i<16;i++)vs_render(px,py,ang,1); g_bn_fe=(int)(u16)(g_vbl-t); g_noemit=0;   /* proj only, NO SCB emit */
                g_floor_rows=sfr;g_ceil_rows=scr;g_vs_budget=sbg; vs_render(px,py,ang,1);   /* restore dials + repaint the real view */
                g_bench=2;
                }
            }
            if(g_dbg){ char tk[NSEL][12]; char ln[44];    /* HUD render gated on g_dbg (P toggles). ALL params laid out -> '>' caret on the P-selected one; screenshot the whole recipe at once. */
              snprintf(tk[0],12,"dd=%d",g_vs_murk);     snprintf(tk[1],12,"dc=%d",g_dcap);
              snprintf(tk[2],12,"col=%s",NCOLW[g_ncoli].name+3);   snprintf(tk[3],12,"cap=%.4s",CAPMODE[g_capmode].name);   /* cap name truncated to 4 -> fits the 4-col layout (full names in CAPMODE[]) */
              snprintf(tk[4],12,"zon=%d",g_zonal);      snprintf(tk[5],12,"gen=%d",g_generic);
              snprintf(tk[6],12,"ease=%d",g_murkease);  snprintf(tk[7],12,"wpn=%d",g_weapon);
              if(MURKMIN[g_murkmin]<0)snprintf(tk[8],12,"mn=of");else snprintf(tk[8],12,"mn=%d",MURKMIN[g_murkmin]);   /* mmin -> mn (fits 4-col) */
              snprintf(tk[9],12,"mbg=%d",g_murkbg);
              if(FOGEXT[g_fogext]==0)snprintf(tk[10],12,"fog=of");else snprintf(tk[10],12,"fog=%d",FOGEXT[g_fogext]);
              snprintf(tk[11],12,"flod=%d",g_floorlod); snprintf(tk[12],12,"perf%d/%d",g_perfen,g_perfP);
              snprintf(tk[13],12,"bud=%d",g_vs_budget); snprintf(tk[14],12,"cmap=%d",g_cmceil);
              snprintf(tk[15],12,"seam=%d",g_seamover); snprintf(tk[16],12,"clod=%d",g_ceillod);
              snprintf(tk[17],12,"prop=%d",g_props);    snprintf(tk[18],12,"fmrk=%d",g_vpw);
              snprintf(tk[19],12,"cmrk=%d",g_vph);       snprintf(tk[20],12,"cln=%d",g_hidegun);
              snprintf(tk[21],12,"hhud=%d",g_hidehud);   snprintf(tk[22],12,"dwlk=%d",g_doorwalk);
              snprintf(tk[23],12,"fcrv=%d",g_fogcrv);    snprintf(tk[24],12,"vmap=%d",g_vmap);   snprintf(tk[25],12,"lvl=%d",g_map+1);
              snprintf(tk[26],12,"cf=%s",CFGP[g_cfgsel].nm); snprintf(tk[27],12,"rset");   snprintf(tk[28],12,"vprj=%d",g_vproj); snprintf(tk[29],12,"lcul=%d",g_lutcull); snprintf(tk[30],12,"hwt=%d",g_hwtail); snprintf(tk[31],12,"untx=%d",g_untex); snprintf(tk[32],12,"wbob=%d",g_wbob); snprintf(tk[33],12,"bnch=%d",g_bench); snprintf(tk[34],12,"skip=%d",g_skip); snprintf(tk[35],12,"ncul=%d",g_ncull); snprintf(tk[36],12,"nclp=%d",NCLIP[g_nclip]); snprintf(tk[37],12,"bspv=%d",g_bspviz); snprintf(tk[38],12,"hmap=%d",g_hmap); snprintf(tk[39],12,"bclp=%d",g_bandclip); snprintf(tk[40],12,"dpri=%d",g_dpri); snprintf(tk[41],12,"add=%d",DD[g_actddi]); snprintf(tk[42],12,"mxa=%d",g_maxact); snprintf(tk[43],12,"nemt=%d",g_noemit); snprintf(tk[44],12,"bxc=%d",BXCAP[g_bxcapi]); snprintf(tk[45],12,"psd=%d",PSTEPV[g_pstepi]); snprintf(tk[46],12,"gov=%d",GOVFPS[g_govtgt]); snprintf(tk[47],12,"fb=%s",FBVNM[g_fb_mode]); snprintf(tk[48],12,"cmfg=%d",g_cmfog);
              int deg=(ang*360)>>8;
              g_vrambusy=1;
              worst=1; for(int i=0;i<FCN;i++) if(g_fcost[i]>worst) worst=g_fcost[i];   /* SCOPE: 'worst' = RECENT peak over the 32-frame ring (was an all-time max that never recovered after one heavy view) */
              { int lp=snprintf(ln,sizeof ln,"rc=%d f=%d w=%d ns=%d sg=%d bx=%d ef=%d",g_rcost,cad?60/cad:60,worst?60/worst:60,vs_spr-41,g_seg_n,g_bbox_n,g_murk_eff);
                while(lp<38 && lp<(int)sizeof(ln)-1) ln[lp++]=' '; ln[lp]=0; ng_text(2,2,1,ln); }   /* perf: fps+worst+strips ns+sg=SEGS PROJECTED+bx=NODE-BBOX WALK+ef=LIVE eased far-horizon g_murk_eff. PADDED to 38 to clear stale chars (the shorter line was leaving a trailing 'ef100[350]' overlay). (dropped t=tiles for room.) */
              for(int row=0;row<((NDISP+3)/4);row++){ int p=0;       /* NDISP params, 4 per row -> ceil(NDISP/4) rows (rows 3..); caret marks g_sel */
                for(int col=0;col<4;col++){ int dpos=row*4+col; if(dpos>=NDISP)break; int idx=DSEL[dpos];   /* DSEL = display order (rset dropped; hclp sits before the pf preset on the last row) */
                  p+=snprintf(ln+p,sizeof(ln)-p,"%c%-8s",(idx==g_sel)?'>':' ',tk[idx]); }   /* caret + 8-char token = 9/col x4 = 36 cols (fits the 1..38 visible fix range) */
                while(p<36 && p<(int)sizeof(ln)-1)ln[p++]=' '; ln[p]=0; ng_text(2,3+row,1,ln); }
              snprintf(ln,sizeof ln,"x%d y%d a%d nb%d ffl=%d bfl=%d ",px,py,deg,g_nfogband,g_dbg_ffl,g_dbg_bfl); ng_text(2,13,1,ln);   /* row 13: WAD coords+deg, nb=adaptive fog-band count this map (3=full fog .. 1=flat), FLAT-DBG front/back flat */
              snprintf(ln,sizeof ln,"stk=%d fr=%d skp=%d act=%d/%d bc=%d ",stack_peak(),stack_free(),g_skipcnt,g_act_emit,g_act_sel,g_seg_clipped); ng_text(2,14,1,ln);   /* STACK PROFILER: peak bytes used / free-to-statics. skp = running SKIPPED-frame count (proof the static-skip fires). bc = segs killed THIS frame by the vertical band-reject (climbs near windows = the stutter fix firing). */
              if(g_bench){ if(g_fb_mode==1) snprintf(ln,sizeof ln,"FB %dvbl/16 ~%dk ~%dfps",g_bn_fa,(g_bn_fa*125)/10,g_bn_fa?960/g_bn_fa:0);   /* FB cost: vblanks for 16 frames -> ~cyc/frame (k) + fps */
                           else snprintf(ln,sizeof ln,"bn fa=%d em=%d fl=%d pj=%d ",g_bn_fa,g_bn_fa-g_bn_fe,g_bn_fa-g_bn_fp,g_bn_fe-g_bn_fb); ng_text(2,15,1,ln); }   /* BENCH (bnch=2): per-stage cost in 16x-vblank units -- fa=FULL, em=emit(SCB writes), fl=flats(floor/ceil LUT), pj=proj(BSP+projection). Lower=cheaper; shows which dominates as the dials change. */
              /* SCOPE digit-strip (row 15) removed from the default debug view (to avoid drawing the long string of numbers). The ring still feeds w= (recent worst) on the fps line; re-add the strip behind a toggle if the timeline is wanted again. */
#ifdef VS_DIAG
              { char d[44]; int p2=snprintf(d,sizeof d,"ns%d sg%d nr%d fr%d bk%d of%d bd%d ",vs_spr-41,g_seg_n,g_cull_near,g_cull_frus,g_cull_back,g_cull_off,g_cull_bud);
                while(p2<40)d[p2++]=' '; d[p2]=0; ng_text(2,12,1,d); }   /* strips / segs-projected / per-cull-path tallies (row 12: below the all-fields HUD + coords) */
#endif
#if VS_BENCH
              { char d[44]; int p2=snprintf(d,sizeof d,"A%d W%d P%d B%d E%d Sg%d ",g_fa,g_fw,g_fp,g_fb,g_fe,g_sgn);   /* forensic 16x: All/noWall/noPlane/Bare/projOnly(noEmit); segs. wall-emit=A-E, wall-proj=E-B */
                while(p2<38)d[p2++]=' '; d[p2]=0; ng_text(2,12,1,d); }
#endif
              g_vrambusy=0; }   /* readout in the bottom letterbox (below the y176 band) -> scene unobstructed for vizdoom compares */
        }
    }
#endif
    return 0;
}
