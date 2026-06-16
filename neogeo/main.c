/* DOOM-NG -- Neo Geo backend, NODE-RENDER runtime (the 60fps path).
 *
 * The BSP renderer is too heavy for the FPU-less 68000 to run per-frame. So we
 * PRE-RENDER the level off-line: flood-fill the walkable floor into a 120-unit
 * grid, render each (cell x 12 angles) viewpoint on the host, and store the
 * emit-ready sprite records in P-ROM (neogeo/nodes_data.h).
 *
 * At runtime we do NO 3D math: snap the camera to the nearest grid node + angle,
 * look up that view's records, and blit them as hardware-scaled sprites. That's
 * all a normal Neo Geo game does -- push pre-made sprites -- so we get 60fps.
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
#include "floorlut.h"
#include "ceillut.h"
#include "sprites.h"
#include "ramps.h"
#include "hudfix.h"          /* RAMP_TILE0 + RAMP_OFF[tex][drop][edge]: texture-baked smooth-wall edge tiles */
#include "gunhand.h"         /* GUNHAND[] + GUNHAND_PAL16[]: the 6 first-person weapons as fix-layer tiles (gunbake.py) */
#include "titlepic.h"       /* TITLE_TILE0 + TITLE_PAL16[][] + TITLE_MAP[][]: DOOM title screen tilemap */
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

static camera_t cam;
static int g_nodebank=-1;
static int g_view_sky=0;
static int g_vbob=0;   /* HEAD BOB: vertical view offset folded into every sprite Y at emit time (quantized 2px so the view-cache still skips when standing) */
static angle_t g_binang=0;   /* angle of the view ON SCREEN (its 8-degree bin centre). Billboards and the sky
                                rotate with THIS, not the continuous cam.ang: smooth sprites against snapped
                                walls read as "barrels yoking wildly" -- the whole world snaps as ONE. */
static int g_ceildark=0;   /* this node's room uses ceiling LUT B (dark) -- from the baked PATHCEIL table */   /* HEAD BOB: vertical view offset folded into every sprite Y at emit time (quantized 2px so the view-cache still skips when standing) */    /* last emitted view contains sky records -> the view-cache must also key on the sky-scroll angle bin */   /* currently-mapped P2 bank for NODES. Cached: re-issuing P_ROM_SWITCH_BANK every frame makes gngeo re-map/flush -> the BK1 slideshow. emit_nodeview is the only switcher. */

static void init_palettes(void){
    MMAP_PALBANK1[1*16]=0x8000; for(int i=1;i<16;i++) MMAP_PALBANK1[1*16+i]=0x7FFF;   /* fix palette 1 = WHITE (idx0 transparent) -> the debug HUD readout */
    for(int i=0;i<16;i++){ MMAP_PALBANK1[2*16+i]=HUDFIX_PAL2[i]; MMAP_PALBANK1[3*16+i]=HUDFIX_PAL3[i];
        MMAP_PALBANK1[4*16+i]=HUDFIX_PAL4[i]; MMAP_PALBANK1[5*16+i]=HUDFIX_PAL5[i]; MMAP_PALBANK1[6*16+i]=HUDFIX_PAL6[i]; }   /* fix palettes 2-5: the status bar (HUD lives on the FIX LAYER now -- ~30 sprites freed for world records) */
    MMAP_PALBANK1[255*16+15]=0x0111;   /* hardware backdrop (0x401FFE) -> near-black murk: far walls + horizon gaps recede into it */
    MMAP_PALBANK1[15*16]=0x8000; MMAP_PALBANK1[14*16]=0x8000;     /* slots 14/15 REPURPOSED (were unused Doom gray fallbacks): the 3rd (deep) murk band -- 14=shared deepest, 15=ceiling mid. Colours filled in the murk loop below. */
    MMAP_PALBANK1[13*16]=FLOORLUT_PAL16[0];                        /* floor LUT palette, brightened toward the OG mid-grey */
    for(int i=1;i<16;i++){ unsigned short c=FLOORLUT_PAL16[i];
        int r=(c>>8)&0xF, g=(c>>4)&0xF, b=c&0xF;
        r=(r*11)>>4; g=(g*11)>>4; b=(b*11)>>4;                     /* ~0.69x: uniform DARKER floor (the shade you liked), one palette = no banding */
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
    for(int i=1;i<16;i++){ unsigned short c=MMAP_PALBANK1[13*16+i];      /* from the FLOOR shade (slot 13), so the fade continues the floor, not the raw flat */
        int r=(c>>8)&0xF, g=(c>>4)&0xF, b=c&0xF;
        MMAP_PALBANK1[11*16+i]=(unsigned short)((((r*15)>>4)<<8)|(((g*15)>>4)<<4)|((b*15)>>4));    /* floor band 1: 0.94x (subtle) */
        MMAP_PALBANK1[10*16+i]=(unsigned short)((((r*13)>>4)<<8)|(((g*13)>>4)<<4)|((b*13)>>4));    /* floor band 2: 0.81x */
        MMAP_PALBANK1[14*16+i]=(unsigned short)((((r*11)>>4)<<8)|(((g*11)>>4)<<4)|((b*11)>>4));    /* band 3 (DEEP, shared floor+ceiling): 0.69x -- gentle, was 0.45x (too dark); keeps 3 distinct steps */
        unsigned short cc=CEILLUT_PAL16[i]; int cr=(cc>>8)&0xF, cg=(cc>>4)&0xF, cb=cc&0xF;
        MMAP_PALBANK1[9*16+i]=(unsigned short)((((cr*15)>>4)<<8)|(((cg*15)>>4)<<4)|((cb*15)>>4));  /* ceiling band 1: 0.94x */
        MMAP_PALBANK1[15*16+i]=(unsigned short)((((cr*13)>>4)<<8)|(((cg*13)>>4)<<4)|((cb*13)>>4)); }   /* ceiling band 2: 0.81x (band 3 = shared slot 14, 0.69x) */
    MMAP_PALBANK1[11*16]=0x8000; MMAP_PALBANK1[10*16]=0x8000; MMAP_PALBANK1[9*16]=0x8000;
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

/* OUR vblank handler (overrides ngdevkit's rom_handler_VBlank_default). The default jsr's the
   BIOS SYSTEM_IO every interrupt; nullbios's credit/forced-start logic in there re-enters the
   cart's start vector seconds into gameplay (the deterministic "crash at spawn" reboot loop) and
   its FIX writes can race our VRAM sequences. We need none of its services after boot: ack the
   IRQ, kick the watchdog, run ngdevkit's callback (the ng_wait_vblank counter) -- and never call
   the BIOS again. */
static volatile unsigned short g_vbl=0;
extern void rom_callback_VBlank(void);
/* VBLANK-DRIVEN FLOOR ("interstitial frames"): the floor phase ticks inside the vblank IRQ,
   decoupled from the main loop -- in heavy rooms the geometry updates at 20-30Hz but the floor
   keeps flowing every refresh the main loop isn't mid-VRAM-write. the author's doctrine: "smoother
   baseline and some jumping geometry beats jumping everything." g_vrambusy brackets every
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
   palettes) + the near-black backdrop, which read better than fogging the planes underfoot. */
static const unsigned char FLOORPAL[7]={10,11,13,13,13,13,13};   /* DISTANT MURK, subtle: the two farthest floor rows ease into darkness (10=0.78x, 11=0.90x of the floor shade) -- a gradient, not stripes; rows 2+ stay uniform */
/* DISTANCE FOG -- keyed on DEPTH, not on-screen height. (Height-keyed left tall far walls bright,
   so there was no consistent gradient.) dep = depth/4 (the wall record byte). <=FOGD0 clean (L0);
   FOGD0..FOGD1 = L1 (~0.5); beyond FOGD1 = L2 (~0.25, fading into the 0x0111 murk). So EVERYTHING
   at a given distance darkens the same, tall or short -> a real depth gradient. */
#define FOGD0 56    /* dep -> world depth ~224: murk introduced NEARER (the author: "nearer field as subtly as possible") -- the L1 step is gentle (0.81) so the onset reads as atmosphere, not a dim wall */
#define FOGD1 135   /* ~world depth 540: middle ground -- the 480/0.375 combo crushed mid-distance walls to black blobs (the author: "visually degraded") */
static const unsigned char CEILPAL5[5]={12,7,8,8,8};   /* per-room ceiling palette: A grey, B dark, C spotted (red/amber rooms ride the white spots -- their slots went to murk; the red ROOM character comes from the dome records the author liked) */
#define CEILPAL_NOW (CEILPAL5[g_ceildark])
#define CEILROWPAL(r,re) (((r)==(re)-1) ? 9 : CEILPAL_NOW)   /* ceiling murk matches the floor: every room type eases its horizon row into the fade (dark rooms previously never eased). ONE row -- two pulled the murk line visibly too close (the author). */
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

#define SND(n) (*((volatile unsigned char*)0x320000)=(unsigned char)(n))   /* sound-code latch: 1=pistol/menu blip, 2=imp death (music loops from boot) */
#if !MENU_HAVE
static const char *const EPISODES[]={ "E1  KNEE-DEEP IN THE DEAD", "E2  THE SHORES OF HELL (LOCKED)", "E3  INFERNO (LOCKED)" };
static const char *const SKILLS[]  ={ "I'M TOO YOUNG TO DIE", "HEY, NOT TOO ROUGH", "HURT ME PLENTY", "ULTRA-VIOLENCE", "NIGHTMARE!" };

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
   drawn over black. DOOM's 320x200 menu coords, shifted +12y to centre in our 224 viewport. ===== */
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
   sprite hardware draws. We measure how many of these passes fit in a second (MAXFPS). */
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
#include "vsflat.h"              /* per-flat perspective LUT bank: real DOOM floor/ceiling per sector */
#define VS_FLATS 1               /* 1 = real per-flat floor/ceiling; 0 = synthetic hex/grey LUT (instant revert) */
#define VS_SKYWIN 0              /* 1 = WINDOW-SKY (front OR back F_SKY1) -- TESTED Jun14: produced garbage/bleed, the other fixes did NOT brute-force it (needs the parked 2-span). 0 = front-sky only (a42eec2 anti-bleed). */
#define VS_EYE  41
#define VS_NCOL 20
#define VS_NCOL_MAX 80   /* widest column-res mode (button C: 20/40/80). Per-column arrays sized to this; live count = g_ncol. */
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
static int g_bbox_n, g_seg_n;                        /* per-frame perf counters: vs_bbox_vis calls, segs projected */
#ifdef VS_DIAG
static int g_cull_near,g_cull_frus,g_cull_back,g_cull_off,g_cull_bud;   /* per-frame seg early-return tallies (sign-inversion diag) */
#define VSCULL(x) (g_cull_##x++)
#else
#define VSCULL(x) ((void)0)
#endif
static short vs_fcs, vs_fsn;
static unsigned short vs_stk[128];                   /* BSP node/subsector traversal stack (was 80; deeper so a big map never silently prunes a far subtree at the push guard) */
static int vs_eye=41;                                /* eased eye z (floor+41) -> walking up stairs raises the view */
static int g_flooreye=41;                            /* INSTANT player-floor eye ref (floor+41), NOT eased. Actor/FX feet anchor to THIS so they stay glued to the static LUT floor grid; using the eased vs_eye made far baddies float in front of the floor while climbing stairs. */
static unsigned char vs_nstr[VS_NCOL_MAX];               /* strips drawn per column (depth-complexity cap) */
static int g_dcap=16;                                /* per-column see-through layer cap -- RUNTIME dial (button B). Was a hidden #define (3); now crankable. The global sprite budget is the real bound. */
static short vs_clY[VS_NCOL_MAX], vs_flY[VS_NCOL_MAX];       /* per-column highest wall-top / lowest wall-bottom -> ceiling/floor fill extents */
static unsigned char vs_sky[VS_NCOL_MAX];                /* column sees a sky sector -> leave ceiling as backdrop sky */
static unsigned char vs_skd[VS_NCOL_MAX];                /* sky DECIDED: the nearest seg owns each column ceiling so recursed far sky cannot bleed the room ceiling above a window */
static unsigned char vs_ffl[VS_NCOL_MAX];                /* nearest-seg FRONT FLOOR flat slot per column (0xFF unset -> synthetic) */
static unsigned char vs_cfl[VS_NCOL_MAX];                /* nearest-seg FRONT CEIL flat slot per column (0xFF sky/unset) */
static unsigned char vs_ffr[VS_NCOL_MAX][5];             /* ZONAL: per-(col,row) FLOOR flat, stamped front-to-back like a DOOM visplane -- each sector painted by its OWN segs as the BSP recurses through openings. Rows below the frontier = synthetic. */
static unsigned char vs_cfr[VS_NCOL_MAX][7];             /* ZONAL: per-(col,row) CEIL flat */
static short vs_wdep[VS_NCOL_MAX];                       /* per-column nearest SOLID-wall depth (for actor occlusion); 0x7FFF = no solid wall (open/capped) */
static short vs_stepd[VS_NCOL_MAX];                      /* per-column nearest STEP/lower-wall (riser) depth; 0x7FFF = none. With vs_stept -> occludes a far actor whose feet sit BELOW a nearer step crest (floor geometry never used to occlude actors). */
static short vs_stept[VS_NCOL_MAX];                      /* that nearest step's CREST screen-row (ybO). Actor hidden in this column when (vs_stepd nearer) AND (vs_stept <= feet) -> feet are behind/below the step. */
static unsigned char vs_skyblk[FLOORLUT_COLS];           /* SKY-IN-OPENING dedup: 1 if this 16px block already got an opening-sky strip this frame (so one even strip per block, not per wall column) */
static short vs_fdep[VS_NCOL_MAX][5];                    /* ZONAL: per-(col,row) FLOOR owner = the winning seg's floor-edge row fr (LINE-priority: largest fr = lowest edge = nearest wins, = DOOM visplane occlusion). -1 = unstamped -> synthetic. */
static short vs_cdep[VS_NCOL_MAX][7];                    /* ZONAL: per-(col,row) CEIL depth (nearest wins) */
static signed short  g_flatpal[VSFLAT_NFLAT];        /* per-map: flat slot -> compacted HW palette slot (-1 = not uploaded) */
/* emit ONE 16px wall/cap column strip [y0..y1] of texture tex (depth d -> fog shade) */
#define VS_MURK 800           /* draw distance: walls beyond are simply not drawn (floor/ceiling fill + backdrop carry it) */
/* DEBUG DIALS (rising-edge): A=draw distance, B=tile/strip budget, C=cap mode; D reserved (map toggle).
   EMIT-only -- they never override the BSP. */
#define VS_NDD 20
#define VS_NDC 11
static int g_dd=3, g_dci=5;   /* DIAGNOSTIC dials: draw distance (DD[] idx) + per-column DEPTH CAP (DC[] idx -> g_dcap); default DD[3]=600 (the author's sweet spot; dd is a HARD wall clamp) / DC[5]=6. */
/* DEBUG SHUTTLE: SPACE (CNT_A) = show/hide HUD; P (CNT_B) = cycle WHICH param; N (CNT_C) = value DOWN, B (CNT_D) = value UP; LEVEL is param 15 in the cycle (no longer a dedicated button). */
#define NSEL 28
static int g_sel=13;           /* selected debug param: 0=dd 1=dc 2=col 3=cap 4=zon 5=gen 6=ease 7=wpn 8=mmin 9=mbg 10=fog 11=flod 12=occl 13=bud 14=sky 15=seam 16=clod 17=prop 18=fmrk(floor murk) 19=cmrk(ceil murk) 20=hgun 21=hhud 22=dwlk(door walk-through) 23=LEVEL. '>' caret. Default 13. */
static int g_dbg=0;            /* debug HUD on/off (P toggles). OFF = clean view + skips the per-frame snprintf/ng_text overhead ("schrodinger's debug"). */
static int g_zonal=1;          /* ZONAL flats: per-row visplane (correct floor/ceil flat per depth band, each sector painted by its own segs through openings). DEFAULT OFF -> single-flat blanket; shuttle param 4 A/Bs it. Flip default once the author confirms the ride. */
static int g_generic=1;        /* GENERIC mode: force the synthetic floor/ceiling LUT + sky (the pre-real-flats look) instead of any real flat; shuttle param 5. A/B vs the flats (and a flicker probe). */
static int g_murkease=16;       /* far-cull EASING (shuttle param 6) = the per-frame gain as a SHIFT: 0=OFF (constant murk, no wink); 1=>>1 snappiest .. 16=>>16 ultra-gentle. Higher = gentler/slower. Default 4 (>>4). */
#define NEASE 65   /* ease dial 0..64 (the author: test the limit). NOTE: it's a bit-SHIFT gain -> the per-frame step floors at 1 unit by ~ease 12, so >=12 are all "maximally gentle" (visually identical); kept the wide range anyway. */
static int g_weapon=1, g_pweap=-1; /* current weapon (0=fist..5=chainsaw; default pistol=1) + last-drawn (for change-only redraw); shuttle param 7 cycles it. */
/* MURK EXTENT controls (the author's "leave no stone unturned"), shuttle params 8/9/10: */
#define NMURKMIN 20
static int g_murkmin=6;   /* (8) far-cull FLOOR: min g_murk_eff under load. Default idx6 = 600. LAST idx = -1 = OFF. */
static const short MURKMIN[NMURKMIN]={0,100,200,300,400,500,600,700,800,900,1000,1500,2000,2500,3000,3500,4000,4500,5000,-1};   /* 100-steps to 1000, then 500-steps to 5000 (matches DD's upper bound); -1 (last) = OFF: far-cull disabled (draw to the strip budget) */
#define NMURKBG 8
static int g_murkbg=1;    /* (9) far-field BACKDROP colour (undrawn regions) = the FURTHEST murk layer. Default idx1=0x0111 near-black (the author: deepest murk; floor/ceiling cull to it via the gradient layers). */
static const unsigned short MURKBG[NMURKBG]={0x0000,0x0111,0x0222,0x0333,0x0444,0x0555,0x0210,0x0421};
#define NFOGEXT 16
static int g_fogext=14;   /* (10) wall fog-band EXTENT %: scales the depth thresholds. Default idx14=75%. LAST = 0 = OFF (no wall fog). LOW % = fog onset near the camera (heavy fog). */
static const unsigned char FOGEXT[NFOGEXT]={5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,0};   /* 5..75% in steps of 5 (low = heavy/near fog) + OFF; % of the 400/1000 base depth thresholds */
static int g_fog0=300, g_fog1=750;   /* live wall fog-band thresholds (init = 75% for default g_fogext=1; recomputed from g_fogext on change) */
/* PERF PRESETS (param 26): each row = dial INDICES {col,bud,dd,dc,mmin,flod,clod}. LOW=fastest/leanest .. ULTRA=heaviest/best. */
typedef struct { unsigned char col,bud,dd,dc,mmin,flod,clod; } perfpreset_t;
static const perfpreset_t PERF[4]={ {0,2,0,1,2,6,6}, {0,6,3,3,5,3,3}, {1,12,7,5,8,0,0}, {2,22,11,8,19,0,0} };
static int g_perf=1;                              /* (26) preset selector; APPLIED on change (sets the 7 dials above) */
static const char *const PERFNM[4]={"lo","md","hi","ul"};
#define NFCLOD 10
static int g_floorlod=0, g_ceillod=0;   /* (11) FLOOR crop + (16) CEILING crop, INDEPENDENT now (the author): each drops FAR (horizon) rows one at a time (FCLOD_F/FCLOD_C, 0=full .. 9=off). Floor keeps NEAR=bottom, ceiling keeps top -> both recede from the horizon (centre) OUTWARD. Default 3 = floor 4 / ceil 5 (old combined look). */
static int g_props=1;     /* (17) PROPS visible: 1=draw actors (baddies/barrels), 0=hide them (debug A/B for geometry-only views). */
static int g_hidegun=0, g_hidehud=0;     /* (20) hide weapon / (21) hide player status bar -- clean-view debug toggles */
static int g_phidegun=-1, g_phidehud=-1; /* last-drawn states: the fix layer persists, so toggling must clear/redraw once */
static int g_doorwalk=1;                 /* (22) doors walk-through without opening collision (the author: default ON -- frictionless flow while the trigger system settles) */
static int g_vpw=0, g_vph=0;   /* (18) FLOOR + (19) CEILING murk: # of FAR rows (toward the horizon) darkened to the murk shade (slot 9). Applies to the SYNTHETIC planes (gen=1 -- "the two basic ones, not flats"). 0=off. Repurposed from the ditched viewport crop. */
#define NVP 6                  /* murk levels 0..5 */
static int g_vpl=0, g_vpr=9999, g_vpt=VS_LBT, g_vpb=VS_LBB;   /* computed each frame: H column range [g_vpl,g_vpr] + V band [g_vpt,g_vpb] */
static int g_occl=1;      /* (12) actor occlusion vs walls (M2): 1=barrels hidden behind nearer solid walls (per-column, VS_RAD fuzz); 0=off (draw over walls). Per-column can over-cull a barrel past a wall edge -> toggle to A/B. */
#define MAXACT 32         /* per-view cap: draw at most the NEAREST 32 actors (bounds the sort + protects the sprite/scanline budget); the rest fog out */
static unsigned char g_thalive[VE_MAXTH];   /* COMBAT: runtime per-thing ALIVE flag (the const ve_th* tables can't be mutated); reset each map load */
#define NFX 6             /* barrel-explosion FX pool */
static short g_fxx[NFX], g_fxy[NFX], g_fxz[NFX]; static unsigned char g_fxt[NFX];   /* world pos + frames-left (0=free) */
static const unsigned char FCLOD_F[NFCLOD]={5,5,4,4,3,3,2,2,1,0}, FCLOD_C[NFCLOD]={7,6,6,5,5,4,4,3,2,0};   /* fine: each step drops one far row */
static int g_capmode=0;        /* wall cap MODE: index into CAPMODE[]. Default OFF (idx 0, the author): the cap quantization made step-riser diagonals snap frame-to-frame. idx 1 TB32f = flat picket-proof bevel if re-enabled. */
/* COLUMN-RESOLUTION toggle (button C): trade wall-strip width for serration. ncol*colw=320 always.
   20x16 (baseline ~40 spr/scanline), 32x10 (~52, the author's sweet-spot pick), 40x8 (~60, serration halved),
   64x5 (~84 wall+lut, ~at the limit), 80x4 (~100 -> HW sprite dropout: the "watch it break" rung).
   colw = strip px width (HW h-shrink 0..15 = colw-1). screen-x -> column index = (sx*colrcp)>>16
   (colrcp=ceil(2^16/colw)) -- a MULS.W reciprocal instead of a shift, so colw need NOT be a power of 2
   (that's how 32x10 / 64x5 are possible). EXACT for sx in [0,320] (brute-verified); for pow2 widths it
   equals the old sx>>log2(colw) bit-for-bit. */
static int g_ncoli=0;   /* default col20 (the author's pick) */
static const struct { short ncol,colw,colrcp; const char *name; } NCOLW[]={{20,16,4096,"col20"},{32,10,6554,"col32"},{40,8,8192,"col40"},{64,5,13108,"col64"},{80,4,16384,"col80"}};
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
static int g_vs_murk=VS_MURK, g_vs_budget=VS_BUDGET;  /* wall draw-distance (A dial) + strip budget (now driven by the BUDGET dial below, param 13) */
static const short BUDGET[]={24,36,48,60,72,84,96,108,120,132,144,156,168,180,192,204,216,228,240,252,264,276,288};   /* DRAW-COUNT CAP steps (12 apart); 41+288 walls + 32 actors < 380 HW slots = safe */
#define NBUDGET ((int)(sizeof(BUDGET)/sizeof(BUDGET[0])))
static int g_budgeti=8;   /* draw-count cap dial (param 13): nearest-N wall strips; default = max (288). Dial DOWN to shrink the flicker window + speed dense rooms. */
static int g_opensky=1;   /* SKY-IN-OPENING (param 14): draw sky through windows/openings whose BACK ceiling is sky (#42). 1=on. */
static int g_seamover=0;  /* SEAM OVERDRAW (param 15): widen each wall strip by N px to the right so neighbours OVERLAP -> a column mid-rebuild is masked by its neighbour's overdraw instead of a blank gap (the author's temporal-parallax flicker mask). 0=off; only bites at col>=32 (col20 strips are already 16px=max width). */
static int g_murk_eff=VS_MURK;   /* FLICKER FIX: effective far-horizon, eased per frame by budget pressure -> the far-cull drops by DEPTH (stable) not strip-emit-order (which reshuffled frame-to-frame = the wink) */
/* RADIAL draw-distance: the cull is on PERPENDICULAR depth (a flat plane), so the screen edges reach ~1/cos
   farther in euclidean distance -> eccentric far-geometry over-included (the wide-view cost). g_murkcol[c] =
   g_murk_eff * cos(view-angle of column c) carves the slab into a uniform-radius cone-sector (edges cull sooner). */
static short g_cosrad[VS_NCOL_MAX]; static int g_cosrad_n=-1;   /* per-column cos(angle)*256; recomputed only on column-res change */
static int g_murkcol[VS_NCOL_MAX];                              /* per-column radial far-cull threshold (= g_murk_eff*cos), refreshed each frame */
static int g_radial=0;   /* (23) RADIAL far-cull ON/OFF. OFF = flat perpendicular slab (dC<=g_murk_eff, skip-emit only). ON = RADIAL v2: uniform euclidean reach (per-column g_murkcol) + the column is CLOSED when its nearest wall is beyond reach (vs_open--), so the BSP walk TERMINATES early at the eccentric edges instead of just hiding strips. v1 only skip-emitted (deepening the walk, net-slower); v2 should actually save the walk. Default OFF pending the author's A/B. */
static int g_vmap=1;     /* (24) VERTICAL MAP: ON = V companion (1:1 world-scale + vertical tiling + DOOM peg). OFF = ORIGINAL (stretch the whole texture to the wall height, no tiling/peg). A/B the close-up look the companion changed. */
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
static void vs_upload_tex_pals(void){
    for(int t=0;t<NTEXTILE;t++) vs_lpal[t]=-1;
    int n=0;
    for(int s=0;s<ve_nseg;s++){
        int e[3]; e[0]=ve_mt[s]; e[1]=ve_ut[s]; e[2]=ve_lt[s];
        for(int k=0;k<3;k++){ int t=e[k]; if(t>=0 && t<NTEXTILE && vs_lpal[t]<0) vs_lpal[t]=n++; }
    }
    if(SKY_TEX>=0 && SKY_TEX<NTEXTILE && vs_lpal[SKY_TEX]<0) vs_lpal[SKY_TEX]=n++;
    if(n<1) n=1;
    if(3*n>228) n=76;                     /* wall pal range = slots 16..243 = 228 = 3*76; clamp (won't trigger for E1 maps) */
    vs_nlpal=n;
    for(int t=0;t<NTEXTILE;t++){ int L=vs_lpal[t]; if(L<0||L>=vs_nlpal) continue;
        int s0=(TEXBASE+L)*16, s1=(TEXBASE+vs_nlpal+L)*16, s2=(TEXBASE+2*vs_nlpal+L)*16;
        MMAP_PALBANK1[s0]=0x8000; MMAP_PALBANK1[s1]=0x8000; MMAP_PALBANK1[s2]=0x8000;
        for(int i=1;i<16;i++){ unsigned short c=TEXPAL16[t][i];
            int r=(c>>8)&0xF, g=(c>>4)&0xF, b=c&0xF;
            MMAP_PALBANK1[s0+i]=c;                                                          /* L0 full */
            MMAP_PALBANK1[s1+i]=(unsigned short)(((r*13/16)<<8)|((g*13/16)<<4)|(b*13/16));  /* L1 0.81 */
            MMAP_PALBANK1[s2+i]=(unsigned short)(((r*7/16)<<8)|((g*7/16)<<4)|(b*7/16)); }   /* L2 0.44 */
    }
    /* per-map FLAT palettes: ONE band each (flats need no distance fog), packed AFTER the 3 wall bands
       at FLATBASE; first-seen distinct flat slots only (E1M2 worst = 16+3*57+28 = 215 <= 243). */
    for(int i=0;i<VSFLAT_NFLAT;i++) g_flatpal[i]=-1;
    { int fn=0, FLATBASE=TEXBASE+3*vs_nlpal;
      for(int s=0;s<ve_nseg;s++){
        int fe[2]; fe[0]=ve_ffl[s]; fe[1]=ve_cfl[s];
        for(int k=0;k<2;k++){ int fsl=fe[k];
            if(fsl<VSFLAT_NFLAT && VSFLAT_BASE[fsl]>=0 && g_flatpal[fsl]<0){
                int slot=FLATBASE+fn++; if(slot>243){ g_flatpal[fsl]=243; continue; }   /* clamp: degrade colour, never overflow palette RAM */
                g_flatpal[fsl]=slot; MMAP_PALBANK1[slot*16]=0x8000;
                for(int i=1;i<16;i++) MMAP_PALBANK1[slot*16+i]=VSFLAT_PAL16[fsl][i]; } }
      } }
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
#define VS_SBUFN 300   /* >= max draw-count budget (the dial caps at 288); slots 41..41+N-1 */
typedef struct { short y0,y1,tex,tcol,d,dyt,dyb,yt0,voff; unsigned char c,sky; } vstrip_t;   /* yt0 = UNCLIPPED wall top (clip-aware vertical peg); voff = texture-TILE offset at yt0 (DOOM pegging: 0 = peg top, >0 = peg bottom for uppers); sky=1 -> screen-anchored window-opening sky strip */
static vstrip_t g_sbuf[VS_SBUFN];
static void vs_strip(int spr,int c,int y0,int y1,int tex,int tcol,int d,int dyt,int dyb,int yt0,int voff){
    int i=spr-41; if(i<0||i>=VS_SBUFN) return;   /* record into the burst buffer (replayed post-walk) */
    vstrip_t *e=&g_sbuf[i]; e->sky=0; e->c=(unsigned char)c; e->y0=(short)y0; e->y1=(short)y1; e->tex=(short)tex; e->tcol=(short)tcol; e->d=(short)d; e->dyt=(short)dyt; e->dyb=(short)dyb; e->yt0=(short)yt0; e->voff=(short)voff;
}
static void vs_strip_sky(int spr,int c,int y0,int y1){   /* SKY-IN-OPENING: record a screen-anchored sky strip (two-pass; bursts via vs_sky_strip_emit post-walk) */
    int i=spr-41; if(i<0||i>=VS_SBUFN) return;
    vstrip_t *e=&g_sbuf[i]; e->sky=1; e->c=(unsigned char)c; e->y0=(short)y0; e->y1=(short)y1; e->voff=0;
}
static void vs_strip_emit(int spr,int c,int y0,int y1,int tex,int tcol,int d,int dyt,int dyb,int yt0,int voff){
    int topclip=(y0<VS_LBT), botclip=(y1>VS_LBB);   /* edge runs OFF the play band -> no real visible edge there -> no cap (the author: clean diagonal where the edge shows, no picket where it's off-screen) */
    if(y0<VS_LBT)y0=VS_LBT; if(y1>VS_LBB)y1=VS_LBB; if(y1<=y0||tex<0) return;
    int wt=TEXWT[tex], th=TEXHT[tex]; tcol=((tcol%wt)+wt)%wt;
    int plvl=(d<g_fog0)?0:((d<g_fog1)?1:2);   /* wall fog band; thresholds scaled by g_fogext (shuttle 10) */
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
       drop the cap (no lookup, no raise) and fall back to the plain 16px staircase the user accepts. */
    /* GATE widened to the full +-32 the cap tiles represent: now that there is NO raise, caps are
       picket-proof at ANY slope (they only cut INTO the wall, never protrude), so we can bevel the
       grazing walls that used to fall back to the 16px staircase (the residual zigzag/picket). Only
       truly edge-on walls (|slope|>32, unrepresentable) stay blunt. */
    int tg=CAPMODE[g_capmode].tg, bg=CAPMODE[g_capmode].bg;   /* per-edge slope gates (0 = edge off) */
    int rtop=(tg&&textured&&!topclip&&(dyt<=tg&&dyt>=-tg))?RAMP_OFF[tex][dtc-RAMP_DMIN][0]:-1;   /* TOP cap: edge on + IN-band + within its gate */
    int rbot=(bg&&textured&&!botclip&&(dyb<=bg&&dyb>=-bg))?RAMP_OFF[tex][dbc-RAMP_DMIN][0]:-1;   /* BOTTOM cap: edge on + IN-band + within its gate */
    int rtfl=(dtc>0)?0x01:0x00, rbfl=(dbc>0)?0x02:0x03;   /* only TOP stacks baked: bottom(d)==vh-flip(top(d)) */
    int adt=dtc<0?-dtc:dtc, adb=dbc<0?-dbc:dbc;
    int ntt=(adt+15)>>4, ntb=(adb+15)>>4; if(ntt<1)ntt=1; if(ntb<1)ntb=1;
    /* RAISE the capped strip top to the column's HIGH corner (y0 = wall-top at column CENTRE; the high
       corner sits adt/2 above it). Without this, no-raise left each column's LEFT HALF flat at the centre
       height -> a flat-then-diagonal SAWTOOTH per column (= the close-up pickets). Raising makes the
       diagonal span the FULL column (high corner -> low corner) so it CHAINS with the neighbour = smooth
       silhouette. Residual: convex corners bump up <= adt/2 (<=8px) -- localized, far milder than the
       sawtooth. (gate is +-16, so adt<=16, bump<=8.) */
    if(rtop>=0 && CAPMODE[g_capmode].tr){ y0-=adt/2; if(y0<VS_LBT)y0=VS_LBT; }   /* TOP raise (per-edge); FLAT edges cut into the wall (picket-proof) */
    if(rbot>=0 && CAPMODE[g_capmode].br){ y1+=adb/2; if(y1>VS_LBB)y1=VS_LBB; }   /* BOTTOM raise/extend (per-edge) */
    if(y1<=y0) return;
    int cot=(y1-y0+15)>>4; if(cot<1)cot=1; if(cot>32)cot=32;
    g_vs_tiles+=cot; if(d<g_vs_dmin)g_vs_dmin=d; if(d>g_vs_dmax)g_vs_dmax=d;   /* debug tally */
    int cyf=(496-y0)&0x1FF;
    int vsh=((y1-y0)*255)/(cot*16); if(vsh<1)vsh=1; if(vsh>255)vsh=255;
    int capbot=(rbot>=0)?(ntb<cot?ntb:cot):0;            /* bottom cap (against the floor) has tile priority */
    int captop=(rtop>=0)?ntt:0; if(captop>cot-capbot)captop=cot-capbot; if(captop<0)captop=0;
    *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
    int st8, racc;
    if(g_vmap){                                                    /* V companion (param 24 ON): 1:1 world-scale vertical map, tiles when taller, DOOM-pegged */
        int Hvis=(int)(((long)(y1-y0)*d*410)>>16);                 /* visible WORLD height in texture px (1:1; d/FOCAL per screen-px, FOCAL=160 ~= *410>>16) */
        int Vtop=(int)(((long)(y0-yt0)*d*410)>>16); if(Vtop<0)Vtop=0;   /* texture px at the visible top -> clip-aware peg from the UNCLIPPED wall top yt0 */
        st8=(cot>1)?(int)(((long)(Hvis>>4)*g_rspan[cot])>>8):0; racc=(Vtop<<4)+(voff<<8);   /* anchored at (Vtop+voff tiles); voff = DOOM peg offset */
    } else {                                                       /* ORIGINAL (param 24 OFF): stretch the FULL texture to the wall height -- no 1:1, no tiling, no peg */
        st8=(cot>1)?(int)(((long)th*g_rspan[cot])>>8):0; racc=0;
    }
    for(int r=0;r<cot;r++){ int T, rfl=0;
        if(captop && r<captop){          T=RAMP_TILE0+rtop+r*wt+tcol; rfl=rtfl; }            /* top cap stack (high corner down) */
        else if(capbot && r>=cot-capbot){ T=RAMP_TILE0+rbot+(cot-1-r)*wt+tcol; rfl=rbfl; }   /* bottom cap = top stack vh-flipped */
        else { int srow=(th>0)?(racc>>8)%th:0; T=FIRST_TEX_TILE+TEXTBASE[tex]+srow*wt+tcol; }   /* V companion: tile vertically (mod th) instead of clamping -> tall walls repeat, not stretch */
        racc+=st8;
        *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((pal<<8)|(((T>>16)&0xF)<<4)|rfl); }
    if(vsh<255 && cot<16){ int pe=cot+4; if(pe>16)pe=16; for(int r=cot;r<pe;r++){ *REG_VRAMRW=(u16)(BLANK_TILE&0xFFFF); *REG_VRAMRW=(u16)(pal<<8); } }   /* PERF #1: pad only ~2 rows past content (was the full 16) -> ~halves the frame's SCB1 blank-writes, the single biggest avoidable cost. If a 1-row garbage sliver appears under shrunk strips on cart, raise the +2. */
    *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
    int hsh=g_colw-1+g_seamover; if(hsh>15)hsh=15;   /* SEAM OVERDRAW (g_seamover px): widen right so neighbours overlap -> a column mid-rebuild is masked by the overlap, not a blank seam */
    *REG_VRAMRW=(u16)((hsh<<8)|vsh); *REG_VRAMRW=(u16)((cyf<<7)|cot); *REG_VRAMRW=(u16)((c*g_colw)&0x1FF)<<7;
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
static int g_sndq=0, g_sndqt=0;   /* delayed SFX queue: a HIT plays the gun pop NOW, then this (scream/boom) a couple frames later (one SFX channel -> sequence them) */
/* BOB: gunbake bakes GUNHAND_NBOB vertical phases per weapon (stride = wt*ht tiles). Cart picks the phase
   from g_bobc via a 0-1-2-1 triangle -> a gentle ~4px up/down as you walk; holds steady when still. */
static void draw_gun(void){
    static const unsigned char BOBSEQ[4]={0,1,2,1};
    int phase = (g_bobc>>1)&3; phase=BOBSEQ[phase];
    int fire=(g_fire>0);
    if(g_weapon==g_pweap && phase==g_pphase && fire==g_pfire && g_hidegun==g_phidegun) return;
    g_pweap=g_weapon; g_pphase=phase; g_pfire=fire; g_phidegun=g_hidegun;
    for(int col=8;col<32;col++){ *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+col*32+15;   /* clear the weapon band (cols 8..31, rows 15..HUD_ROW-1). Tall guns (CHAINGUN/ROCKET) are top-clipped by 2 so even firing they top at row 15; all other guns top lower. Rows 11-14 are NEVER weapon pixels -> free for the debug HUD. */
        for(int r=15;r<HUD_ROW;r++) *REG_VRAMRW=(u16)SROM_EMPTY_TILE; }
    if(g_hidegun) return;                         /* HIDE WEAPON (param 20): band cleared above, draw nothing */
    if(g_weapon<0||g_weapon>=GUNHAND_N) return;
    const gunhand_t *gw=&GUNHAND[g_weapon];
    int wt=gw->wt, ht=gw->ht, leftcol=(40-wt)/2, toprow=HUD_ROW-ht-(fire?1:0); if(toprow<0)toprow=0;   /* bottom at row HUD_ROW-1, just above the bar. FIRE: kick up 1 tile */
    int pbase=gw->base + phase*(wt*ht);                                    /* this phase's tile block */
    int clip=(g_weapon==3||g_weapon==4)?2:0;   /* CHAINGUN(3=minigun) + ROCKET(4=bazooka) are the tall guns -> drop their top 2 (empty-headroom) tile-rows so they sit 2 lines LOWER, freeing space above for the debug HUD. Bottom stays anchored just above the bar. */
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
    int phase=(((vs_px+vs_py)>>3)&(VSFLOOR_NPHASE-1));     /* periodic floor flow with translation (approx; heading-independent = no turn-pop) */
    int fset=(ang>>1)%VSFLOOR_NA;                          /* fold: 128 half-units / VSFLOOR_NA sets. NA=64 -> 2-fold (180deg period); NA=32 was 4-fold (90deg). No mirror -- 180deg ROTATIONAL period (so an h/v-asymmetric base flat repeats every half-turn, not every quarter). */
    int cset=(ang>>1)%VSCEIL_NA;                           /* same fold for the ceiling (NA=64 -> 180deg period) */
    int fbase0=VSFLOOR_TILE0 + (fset*VSFLOOR_NPHASE+phase)*VSFLOOR_ROWS*VSFLOOR_COLS;    /* synthetic floor fallback */
    int cbase0=VSCEIL_TILE0 + (cset*VSCEIL_NPHASE + phase)*VSCEIL_ROWS*VSCEIL_COLS;  /* synthetic ceiling fallback -- phase (was phase>>2, which collapsed to 0 with NPHASE=4 = no scroll); now scrolls like the floor (gen=1) */
    int a64=ang&63, sph=((vs_px+vs_py)>>3);   /* per-column flat 90deg fold + scroll-phase source */
    int cskip=g_vpt>>4;                                   /* LETTERBOX: skip ceiling tiles above the band top (g_vpt = full 0, or the V-viewport top) */
    int fkeep=g_floor_rows;                                /* floor rows drawn (LOD NEAR/MID/FAR; 0 => parked) */
    int crow=g_ceil_rows; if(crow>VSCEIL_ROWS-cskip)crow=VSCEIL_ROWS-cskip;   /* ceiling rows drawn (LOD) */
    int ccyf=(496-cskip*16)&0x1FF;
    for(int k=0;k<FLOORLUT_COLS;k++){
        int sx=k*16; int wc=((short)(sx+8)*(short)g_colrcp)>>16; if(wc>g_ncol-1)wc=g_ncol-1;   /* map each 16px LUT block to the wall sub-column at its CENTRE (sx+8), not the left edge -> halves the flat-overspill at sector boundaries when colw<16 (col32/64/80). No-op at col20 (16px blocks map 1:1). */
        if(wc<g_vpl||wc>g_vpr){ vs_park(1+k); vs_park(21+k); continue; }   /* H VIEWPORT: this block falls outside the column window -> backdrop (letterbox left/right) */
        int fbase=fbase0, fpal=13, cbase=cbase0, cpal=12;     /* SINGLE-flat (non-zonal) bases = nearest seg's front flat, blanketed; default synthetic floor(13)/ceiling(12) */
#if VS_FLATS
        if(!g_generic){   /* GENERIC mode: skip real flats -> fbase/cbase stay synthetic (fbase0/cbase0) */
        fbase=vs_flatres(vs_ffl[wc],a64,sph,1,fbase0,13,&fpal);   /* nearest-seg FRONT floor flat (blanket) */
        cbase=vs_flatres(vs_cfl[wc],a64,sph,1,cbase0,12,&cpal);   /* nearest-seg FRONT ceil flat (scroll=1: ceiling scrolls with the view, like the floor) */
        }
#endif
        if(g_ceil_rows>0){
#ifdef VS_NOSKY
        if(1){                                             /* DIAG: force-draw ceiling, ignore vs_sky (isolate the black-ceiling cause) */
#else
        if(!vs_sky[wc]){                                    /* ceiling 1+k: rows cskip.., top-anchored at the band top */
#endif
            *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+(1+k)*64;
            for(int r=cskip;r<cskip+crow;r++){ int cb_=cbase, ww=r-(cskip+crow-g_vph)+1, cp=(r>=cskip+crow-g_vph)?((ww*3>2*g_vph)?14:(ww*3>g_vph?15:9)):cpal;   /* CEILING MURK (param 19): bottom g_vph rows = 3-band gradient -> 9 (0.70x) -> 15 (0.55x) -> 14 (0.45x deep) -> backdrop. Crank cmrk to 3..5 rows to see all three bands. */
                if(g_zonal && !g_generic) cb_=vs_flatres((vs_cdep[wc][r]<0x7FFF)?vs_cfr[wc][r]:0xFF,a64,sph,1,cbase0,12,&cp);   /* ZONAL: per-row visplane ceil flat (scroll=1: ceiling scrolls like the floor); unstamped -> synthetic */
                int T=cb_+(VSCEIL_ROWS-1-r)*VSCEIL_COLS+k;   /* square fold: no mirror */
                *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((cp<<8)|(((T>>16)&0xF)<<4)|0x02); }   /* per-column ceil pal + VFLIP (ceiling = floor-cast flipped) */
            *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+(1+k);
            *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((ccyf<<7)|crow); *REG_VRAMRW=(u16)(sx<<7);
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
        if(g_floor_rows>0 && fhi>fskip){
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+(21+k)*64;
        for(int r=fskip;r<fhi;r++){ int fb=fbase, vv=fskip+g_vpw-r, fp=(r<fskip+g_vpw)?((vv*3>2*g_vpw)?14:(vv*3>g_vpw?10:11)):fpal;   /* FLOOR MURK (param 18): far g_vpw rows = 3-band gradient -> 11 (0.90x) -> 10 (0.78x) -> 14 (0.45x deep) -> backdrop. Crank fmrk to 3..5 rows to see all three bands. */
            if(g_zonal && !g_generic) fb=vs_flatres((vs_fdep[wc][r]>=0)?vs_ffr[wc][r]:0xFF,a64,sph,1,fbase0,13,&fp);   /* ZONAL: per-row visplane floor flat (unstamped rows -> synthetic; vs_fdep now holds the winning fr, -1=unstamped); GENERIC forces synthetic */
            int T=fb+r*VSFLOOR_COLS+k;     /* square fold: no mirror */
            *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((fp<<8)|(((T>>16)&0xF)<<4)); }   /* per-column floor pal (real flat) */
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+(21+k);
        *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((((384-fskip*16)&0x1FF)<<7)|(fhi-fskip)); *REG_VRAMRW=(u16)(sx<<7);
        } else vs_park(21+k);                              /* floor toggle OFF / clipped away -> backdrop below walls */
    }
}
/* render ONE BSP seg into the per-column clip (called front-to-back from the traversal) */
static void vs_render_seg(int s){
    if(vs_emit && vs_spr-41>=g_vs_budget){ VSCULL(bud); vs_open=0; return; }   /* DEPTH-CULL budget hit: stop projecting+emitting (front-to-back => near drew first, far becomes fog); vs_open=0 halts the BSP walk */
    short ax=(short)(ve_x0[s]-vs_px), ay=(short)(ve_y0[s]-vs_py), bx=(short)(ve_x1[s]-vs_px), by=(short)(ve_y1[s]-vs_py);
    int ad=(int)(((long)ax*vs_fcs+(long)ay*vs_fsn)>>8), bd=(int)(((long)bx*vs_fcs+(long)by*vs_fsn)>>8);
    if(ad<VS_NEAR && bd<VS_NEAR){ VSCULL(near); return; }
    int aS=(int)(((long)ax*vs_fsn-(long)ay*vs_fcs)>>8), bS=(int)(((long)bx*vs_fsn-(long)by*vs_fcs)>>8);
    if((long)(short)aS*VS_FOCAL >  160L*ad && (long)(short)bS*VS_FOCAL >  160L*bd){ VSCULL(frus); return; }
    if((long)(short)aS*VS_FOCAL < -160L*ad && (long)(short)bS*VS_FOCAL < -160L*bd){ VSCULL(frus); return; }
    if(g_radial){ long R2=(long)g_murk_eff*g_murk_eff; if((long)ad*ad+(long)aS*aS>R2 && (long)bd*bd+(long)bS*bS>R2){ VSCULL(frus); return; } }   /* RADIAL (param 23): skip segs WHOLLY beyond the EUCLIDEAN far-cull BEFORE projection. NOTE: costs 4 MULS on EVERY seg, only culls the wholly-far minority -> net-slower in practice (see g_radial). */
    int Ua=ve_u0[s], Ub=ve_u0[s]+ve_ulen[s];   /* WALL-U (tex px) at vertex A (v1) and B (v2); near-clip slides them with aS/bS */
    if(ad<VS_NEAR){ int den=bd-ad; if(den<=0)return; int t=((VS_NEAR-ad)<<8)/den; aS+=(int)(((long)(short)(bS-aS)*(short)t)>>8); Ua+=(int)(((long)(Ub-Ua)*t)>>8); ad=VS_NEAR; }
    else if(bd<VS_NEAR){ int den=ad-bd; if(den<=0)return; int t=((VS_NEAR-bd)<<8)/den; bS+=(int)(((long)(short)(aS-bS)*(short)t)>>8); Ub+=(int)(((long)(Ua-Ub)*t)>>8); bd=VS_NEAR; }
    int rfa=(ad<VS_RFMAX)?g_rf[ad]:(int)(((long)VS_FOCAL<<8)/ad);   /* 1/depth: LUT (near) or divide (far, rare/undrawn) */
    int rfb=(bd<VS_RFMAX)?g_rf[bd]:(int)(((long)VS_FOCAL<<8)/bd);
    int sxa=VS_HALF+(int)(((long)(short)aS*(short)rfa)>>8), sxb=VS_HALF+(int)(((long)(short)bS*(short)rfb)>>8);
    if(sxa>=sxb){ VSCULL(back); return; }         /* back-facing seg (BSP segs are one-directional) */
    g_seg_n++;                                     /* perf: a seg that reaches the per-column projection */
    if(sxb<0 || sxa>319){ VSCULL(off); return; }
    int fc=ve_fc[s]+g_secdc[ve_fsec[s]], ff=ve_ff[s]+g_secdf[ve_fsec[s]], bc=ve_bc[s]+(ve_bsec[s]<g_nsec?g_secdc[ve_bsec[s]]:0), bff=ve_bf[s]+(ve_bsec[s]<g_nsec?g_secdf[ve_bsec[s]]:0), fl=ve_flag[s];   /* DOORS: +per-sector ceiling raise; LIFTS: +per-sector FLOOR raise (g_secdf); 0 = identical */
    int two=fl&1, mt=ve_mt[s], ut=ve_ut[s], lt=ve_lt[s];
    short scA=(short)rfa, scB=(short)rfb;          /* = (FOCAL<<8)/depth, exact via the reciprocal LUT (was 2 DIVS.W) */
    short fcE=(short)(fc-vs_eye), ffE=(short)(ff-vs_eye), bcE=(short)(bc-vs_eye), bfE=(short)(bff-vs_eye);
    int ytFa=VS_HOR-(int)(((long)fcE*scA)>>8), ybFa=VS_HOR-(int)(((long)ffE*scA)>>8);
    int ytFb=VS_HOR-(int)(((long)fcE*scB)>>8), ybFb=VS_HOR-(int)(((long)ffE*scB)>>8);
    int ytOa=0,ybOa=0,ytOb=0,ybOb=0;
    if(two){ ytOa=VS_HOR-(int)(((long)bcE*scA)>>8); ybOa=VS_HOR-(int)(((long)bfE*scA)>>8);
             ytOb=VS_HOR-(int)(((long)bcE*scB)>>8); ybOb=VS_HOR-(int)(((long)bfE*scB)>>8); }
    int sa=sxa<0?0:sxa, sb=sxb>320?320:sxb;                 /* clamp to [0,320] so the (short) reciprocal-MUL can't overflow (sxa/sxb are off-screen-unbounded ints; line 570 only culled wholly-off segs) */
    int ca=((short)sa*(short)g_colrcp)>>16; if(ca>g_ncol-1)ca=g_ncol-1; int cb=((short)sb*(short)g_colrcp)>>16; if(cb>g_ncol-1)cb=g_ncol-1;
    int span=sxb-sxa; if(span<1)span=1;
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
    int peg=ve_peg[s], yo_t=ve_yoff[s]>>4, mvoff=0, uvoff=0, lvoff=0;
    /* bottom-peg b = texture tile-row at the section TOP = floor((th*16 - sectionHeight)/16) mod th. NEGATE then FLOOR (>>4): flooring the
       height to a tile first would mis-peg by one 16px tile whenever the height isn't a 16-multiple (most DOOM walls). top-peg b = 0. */
    if(mt>=0){ int th=TEXHT[mt]; if(th>0){ int b=(peg&2)?((((th<<4)-(fc-ff))>>4)%th):0; mvoff=(((b+yo_t)%th)+th)%th; } }            /* one-sided middle: peg TOP by default; DONTPEGBOTTOM -> texture bottom at the floor */
    if(two && ut>=0){ int th=TEXHT[ut]; if(th>0){ int b=(peg&1)?0:((((th<<4)-(fc-bc))>>4)%th); uvoff=(((b+yo_t)%th)+th)%th; } }    /* upper: peg BOTTOM by default (texture bottom at opening ceiling); DONTPEGTOP -> peg top */
    if(two && lt>=0){ int th=TEXHT[lt]; if(th>0){ int b=(peg&2)?(((fc-bff)>>4)%th):0; lvoff=(((b+yo_t)%th)+th)%th; } }            /* lower: peg TOP at the opening floor by default; DONTPEGBOTTOM -> hang from the front ceiling (continuous stairs) */
    for(int c=ca;c<=cb;c++, f+=df){
        if(vs_ct[c]>vs_cb[c]) continue;
        short ff2=(short)f; if(ff2<0)ff2=0; if(ff2>256)ff2=256;
        int ytF=ytFa+(((long)(short)(ytFb-ytFa)*ff2)>>8), ybF=ybFa+(((long)(short)(ybFb-ybFa)*ff2)>>8);
        int dC=ad+(((long)(short)(bd-ad)*ff2)>>8); int U=Ua+(int)(((long)(Ub-Ua)*ff2)>>8), tc=U>>4;   /* WALL-U interpolated like dC (affine), >>4 = texture-tile column -> textures GLUE to the wall (was tc=c = screen-mapped) */
        if(g_radial && dC>g_murkcol[c]){ vs_ct[c]=vs_cb[c]+1; vs_open--; continue; }   /* RADIAL v2: nearest wall in this column is beyond the radial reach -> CLOSE the column (end its walk) instead of just hiding the strip. Front-to-back guarantees nothing nearer follows; vs_lut (floor/ceil) is independent of vs_ct/vs_cb so the near floor/ceil still draw + fade via the murk gradient. THIS is what makes radial save the walk (v1 only skip-emitted, deepening it). */
        int vis = vs_emit && !g_noemit && dC<=(g_radial?g_murkcol[c]:g_murk_eff) && vs_spr<41+g_vs_budget;        /* far-cull: RADIAL per-column (g_murkcol=g_murk_eff*cos) when param 23 ON, else flat perpendicular g_murk_eff; budget = hard backstop */
        int just=!vs_skd[c];                          /* this seg is the NEAREST to latch column c (owns its FRONT flat + the BACK zone if two-sided) */
#if VS_SKYWIN
        if(just){ int sk=(fl&6)?1:0; vs_sky[c]=sk; vs_skd[c]=1; vs_ffl[c]=ve_ffl[s]; vs_cfl[c]=sk?0xFF:ve_cfl[s]; }   /* WINDOW-SKY test: front OR back F_SKY1 -> sky (suppression off) */
#else
        if(just){ vs_sky[c]=(fl&2)?1:0; vs_skd[c]=1; vs_ffl[c]=ve_ffl[s]; vs_cfl[c]=(fl&2)?0xFF:ve_cfl[s]; }   /* NEAREST seg owns the column ceiling sky + FRONT floor/ceil flat (sky=>0xFF) */
#endif
        if(g_zonal && !g_generic){   /* VISPLANE STAMP -- only when the LUT actually READS it (gen=1 uses the synthetic blanket, so the stamp was pure wasted memory-bound work = the perf win). */
            short dep=(short)dC; int ffl_s=ve_ffl[s], cfl_s=ve_cfl[s];
            int fr=(ybF-VS_HOR)>>4; if(fr<0)fr=0; if(fr>4)fr=4;
            unsigned char *ffr_c=vs_ffr[c]; short *fdep_c=vs_fdep[c];   /* floor: LINE-priority (largest fr=nearest owns the rows below). fdep non-decreasing in r -> break once a >=fr seg owns the row. */
            for(int r=fr; r<5 && fr>fdep_c[r]; r++){ ffr_c[r]=(unsigned char)ffl_s; fdep_c[r]=(short)fr; }
            int cr=ytF>>4; if(cr<0)cr=0; if(cr>VSCEIL_ROWS-1)cr=VSCEIL_ROWS-1;
            unsigned char *cfr_c=vs_cfr[c]; short *cdep_c=vs_cdep[c];   /* ceil: DEPTH-priority, nearest wins per row */
            for(int r=0;r<=cr;r++) if(dep<cdep_c[r]){ cfr_c[r]=(unsigned char)cfl_s; cdep_c[r]=dep; }
        }
        if(!two){
            int y0=ytF<vs_ct[c]?vs_ct[c]:ytF, y1=ybF>vs_cb[c]?vs_cb[c]:ybF;
            if(vis && y1-y0>=4) vs_strip(vs_spr++,c,y0,y1,mt,tc,dC,dyt,dyb,ytF,mvoff);   /* one-sided: top=ceiling slope, bottom=floor slope; voff=mvoff (peg top default, peg bottom if DONTPEGBOTTOM) */
            if(vs_ct[c]<=vs_cb[c]) vs_open--;
            if(dC<vs_wdep[c])vs_wdep[c]=(short)dC;   /* nearest solid wall in this column -> actor occlusion threshold */
            vs_ct[c]=vs_cb[c]+1;                  /* solid wall: close the column (drives early-out) */
        } else {
            int ytO=ytOa+(((long)(short)(ytOb-ytOa)*ff2)>>8), ybO=ybOa+(((long)(short)(ybOb-ybOa)*ff2)>>8);
            if(ytO>=ybO){                                  /* OPAQUE: opening collapsed (closed door, back ceil<=floor) -> draw solid + CLOSE the column (no see-through, no traversal beyond) */
                int dtex=(ut>=0)?ut:((lt>=0)?lt:mt);
                int y0=ytF<vs_ct[c]?vs_ct[c]:ytF, y1=ybF>vs_cb[c]?vs_cb[c]:ybF;
                if(vis && y1-y0>=4 && dtex>=0) vs_strip(vs_spr++,c,y0,y1,dtex,tc,dC,dyt,dyb,ytF,0);
                if(vs_ct[c]<=vs_cb[c]) vs_open--;
                if(dC<vs_wdep[c])vs_wdep[c]=(short)dC;   /* opaque (closed door) wall -> actor occlusion threshold */
                vs_ct[c]=vs_cb[c]+1;
            } else {
            if(ytO>ytF && ut>=0){ int y0=ytF<vs_ct[c]?vs_ct[c]:ytF, y1=ytO>vs_cb[c]?vs_cb[c]:ytO; if(vis&&y1-y0>=4){ vs_strip(vs_spr++,c,y0,y1,ut,tc,dC,dyt,dytO,ytF,uvoff); vs_nstr[c]++; } }   /* upper: top=front ceiling, bottom=opening ceiling; voff=uvoff -> peg the texture's BOTTOM to the opening (DOOM default) */
            if(ybF>ybO && lt>=0){ int y0=ybO<vs_ct[c]?vs_ct[c]:ybO, y1=ybF>vs_cb[c]?vs_cb[c]:ybF;
                if(dC<vs_stepd[c]){ vs_stepd[c]=(short)dC; vs_stept[c]=(short)(ybO<VS_LBT?VS_LBT:ybO); }   /* STEP OCCLUSION: nearest riser crest (back/upper floor edge) -> hides far actors standing below it */
                if(vis&&y1-y0>=4){ vs_strip(vs_spr++,c,y0,y1,lt,tc,dC,dybO,dyb,ybO,lvoff); vs_nstr[c]++; } }   /* lower: top=opening floor, bottom=front floor; voff=lvoff (peg top default, hang-from-ceiling if DONTPEGBOTTOM) */
            if((fl&4) && !(fl&2) && g_opensky && SKY_TEX>=0){   /* SKY-IN-OPENING: back ceiling is sky AND front is NOT (i.e. you're INDOORS looking out -> draw sky; suppresses the from-OUTSIDE-into-an-indoor-window case where front=sky) */
                int sk=(c*g_colw+(g_colw>>1))>>4; if(sk<0)sk=0; if(sk>FLOORLUT_COLS-1)sk=FLOORLUT_COLS-1;   /* the 16px sky BLOCK at this column's centre */
                if(!vs_skyblk[sk]){ int sy0=ytO<vs_ct[c]?vs_ct[c]:ytO, sy1=ybO>VS_HOR?VS_HOR:ybO; if(sy1>vs_cb[c])sy1=vs_cb[c];   /* ABOVE the horizon only (far walls, higher slot, overdraw it); one even strip per 16px block */
                    if(vis&&sy1-sy0>=4){ vs_strip_sky(vs_spr++,sk,sy0,sy1); vs_skyblk[sk]=1; } } }
            if(ytO>vs_ct[c]) vs_ct[c]=(short)ytO;
            if(ybO<vs_cb[c]) vs_cb[c]=(short)ybO;
            if(vs_nstr[c]>=g_dcap && vs_ct[c]<=vs_cb[c]){ if(dC<vs_wdep[c])vs_wdep[c]=(short)dC; vs_open--; vs_ct[c]=vs_cb[c]+1; }   /* DEPTH CAP: fog beyond g_dcap layers; the fog-out depth now also OCCLUDES far actors behind it (was: only solid walls set vs_wdep -> far baddies leaked through fogged columns) */
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
    int cx[4]={bb[0],bb[2],bb[2],bb[0]}, cy[4]={bb[1],bb[1],bb[3],bb[3]};
    int anyfront=0, anyback=0, allR=1, allL=1, sxmin=320, sxmax=-1;
    for(int i=0;i<4;i++){
        short dx=(short)(cx[i]-vs_px), dy=(short)(cy[i]-vs_py);
        int d=(int)(((long)dx*vs_fcs+(long)dy*vs_fsn)>>8);
        if(d<VS_NEAR){ anyback=1; continue; }
        int sd=(int)(((long)dx*vs_fsn-(long)dy*vs_fcs)>>8); anyfront=1;
        if(!((long)(short)sd*VS_FOCAL >  160L*d)) allR=0;
        if(!((long)(short)sd*VS_FOCAL < -160L*d)) allL=0;
        int rf=(d<VS_RFMAX)?g_rf[d]:(int)(((long)VS_FOCAL<<8)/d);
        int sx=VS_HALF+(int)(((long)(short)sd*(short)rf)>>8);
        if(sx<sxmin)sxmin=sx; if(sx>sxmax)sxmax=sx;
    }
    if(!anyfront) return 0;                 /* wholly behind the near plane */
    if(anyback)   return 1;                 /* straddles the near plane -> span unreliable, keep (safe) */
    if(allR||allL) return 0;                /* wholly off one side of the frustum */
    int smin=sxmin<0?0:(sxmin>320?320:sxmin), smax=sxmax<0?0:(sxmax>320?320:sxmax);   /* clamp to [0,320] for the (short) reciprocal-MUL (corner sx are off-screen-unbounded ints) */
    int c0=((short)smin*(short)g_colrcp)>>16; if(c0>g_ncol-1)c0=g_ncol-1; int c1=((short)smax*(short)g_colrcp)>>16; if(c1>g_ncol-1)c1=g_ncol-1;
    for(int c=c0;c<=c1;c++) if(vs_ct[c]<=vs_cb[c]) return 1;   /* a covered column is still open -> visible */
    return 0;                               /* every covered column already solid -> OCCLUDED, prune */
}
/* locate the subsector containing (px,py) by descending the BSP -> its sector floor */
static int vs_floor_at(int px,int py){
    unsigned short n=ve_root;
    while(!(n&0x8000)){
        long cross=(long)(short)(px-ve_nx[n])*ve_ndy[n]-(long)(short)(py-ve_ny[n])*ve_ndx[n];
        n=(cross>0)?ve_nr[n]:ve_nl[n];
    }
    { int sg=ve_ssf[n&0x7FFF], sc=ve_fsec[sg]; return ve_ff[sg]+(sc<g_nsec?g_secdf[sc]:0); }   /* first seg's front floor + LIFT raise -> the eye rides a raising platform */
}
/* MAP TOGGLE: reseat all geometry pointers to map m, snap the camera/eye to its spawn, drop the old map's
   sprites, and upload its compacted palettes. Per-frame cost unchanged; the array-of-arrays index is paid
   once here, never per seg. */
static void vs_set_map(int m){
    if(m<0||m>=VE_NMAP) m=0;
    g_map=m;
    ve_x0=VEX0_MAP[m]; ve_y0=VEY0_MAP[m]; ve_x1=VEX1_MAP[m]; ve_y1=VEY1_MAP[m];
    ve_ff=VEFF_MAP[m]; ve_fc=VEFC_MAP[m]; ve_bf=VEBF_MAP[m]; ve_bc=VEBC_MAP[m];
    ve_mt=VEMT_MAP[m]; ve_ut=VEUT_MAP[m]; ve_lt=VELT_MAP[m]; ve_flag=VEFLAG_MAP[m];
    ve_ffl=VEFFL_MAP[m]; ve_cfl=VECFL_MAP[m]; ve_bfl=VEBFL_MAP[m]; ve_bcl=VEBCL_MAP[m];   /* per-seg FRONT + BACK floor/ceil flat slots for this map */
    ve_ssc=VESSC_MAP[m]; ve_ssf=VESSF_MAP[m]; ve_fsec=VEFSEC_MAP[m]; ve_bsec=VEBSEC_MAP[m];
    ve_usesec=VEUSESEC_MAP[m]; ve_uselo=VEUSELO_MAP[m];   /* per-seg LIFT target sector + drop */
    ve_u0=VEU0_MAP[m]; ve_ulen=VEULEN_MAP[m];   /* per-seg wall-U base + span (perspective texture mapping) */
    ve_yoff=VEYOFF_MAP[m]; ve_peg=VEPEG_MAP[m];   /* per-seg vertical peg: rowoffset + DONTPEG* flags (V companion 2b) */
    g_nsec=VE_NSEC_MAP[m]; for(int i=0;i<VE_MAXSEC;i++){g_secdc[i]=0; g_secdf[i]=0;} g_doorsec=-1; g_doorstate=0; g_doorstay=0; g_liftsec=-1; g_liftstate=0;   /* DOORS+LIFTS: reset all sector ceiling/floor deltas on map change */
    ve_thx=VETHX_MAP[m]; ve_thy=VETHY_MAP[m]; ve_thz=VETHZ_MAP[m]; ve_tha=VETHA_MAP[m]; ve_thc=VETHC_MAP[m]; ve_nth=VE_NTH_MAP[m];   /* per-map actor things (S2: barrels) */
    { int i; for(i=0;i<VE_MAXTH;i++)g_thalive[i]=1; for(i=0;i<NFX;i++)g_fxt[i]=0; }   /* COMBAT: all things alive, no FX, on (re)load */
    ve_nx=VENX_MAP[m]; ve_ny=VENY_MAP[m]; ve_ndx=VENDX_MAP[m]; ve_ndy=VENDY_MAP[m];
    ve_nr=VENR_MAP[m]; ve_nl=VENL_MAP[m]; ve_nrb=VENRB_MAP[m]; ve_nlb=VENLB_MAP[m];
    ve_nseg=VE_NSEG_MAP[m]; ve_root=VE_ROOT_MAP[m];
    g_nwalktrig=0; for(int s=0;s<ve_nseg && g_nwalktrig<64;s++) if(ve_flag[s]&64) g_walktrig[g_nwalktrig++]=(unsigned short)s;   /* cache this map's WALK-trigger door segs */
    ve_sx=VE_SX_MAP[m]; ve_sy=VE_SY_MAP[m]; ve_sa=VE_SA_MAP[m];
    vs_camx=ve_sx; vs_camy=ve_sy; vs_camang=(ve_sa*256/360)&255;   /* spawn pose */
    vs_eye=vs_floor_at(ve_sx,ve_sy)+41;                            /* SNAP eye to new floor (no ease from the old map) */
    park_all_sprites(); vs_prevspr=41;                            /* drop old map's strips; force a full re-emit */
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
    int rfs=rf>256?256:rf;   /* SIZE scale: the Neo Geo can't MAGNIFY (vsh caps at 255) -> cap the sprite SIZE + its TO/LO offsets at 1:1. Do NOT cap the feet/lateral: capping rf there pinned close baddies' feet at y=112+41=153 = the float. The 1:1 TO offset keeps topy from wrapping. */
    int sx=VS_HALF+(int)(((long)(short)sd*(short)rf)>>8);                   /* origin screen-x (TRUE) */
    int feet=VS_HOR-(int)(((long)(short)(floorz-g_flooreye)*(short)rf)>>8); /* origin screen-y on the floor (TRUE -> on the floor at all depths; g_flooreye = static-LUT ref, no stair lag) */
    int sw=(int)(((long)16*rfs)>>8); if(sw<1)sw=1; if(sw>16)sw=16;          /* per-column display width (px, 1:1) */
    int topy=feet-(int)(((long)to*rfs)>>8);                                 /* sprite TOP = origin up by TO (1:1 -> no 9-bit y wrap) */
    int leftx=sx-(int)(((long)lo*rfs)>>8);                                  /* sprite LEFT = origin left by LO (1:1) */
    int srch=ht*16, scrh=(int)(((long)srch*rfs)>>8);                        /* source vs on-screen height (1:1) */
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
        int wl=((short)wcl*(short)g_colrcp)>>16; if(wl>g_ncol-1)wl=g_ncol-1;
        int wr=((short)wcr*(short)g_colrcp)>>16; if(wr>g_ncol-1)wr=g_ncol-1;
        if(g_occl && (vs_wdep[wl] < d-10 || vs_wdep[wr] < d-10
                   || (vs_stepd[wl] < d-10 && vs_stept[wl] <= feet) || (vs_stepd[wr] < d-10 && vs_stept[wr] <= feet))) continue;  /* hidden if EITHER edge is behind a nearer WALL, or below a nearer STEP crest (margin 10). The step term is THE fix for far monsters drawn OVER near steps; feet-aware so ones standing higher than the step still show. */
        if(vs_spr>=379) break;                                              /* sprite-record backstop (<380 HW) */
        int spr=vs_spr++;
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+spr*64;
        for(int r=t0;r<ht;r++){ int T=SPR_TILE0+base+r*wt+srctc;            /* draw source tiles t0..ht-1 (top-clipped) */
            *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)((pal<<8)|(((T>>16)&0xF)<<4)|flip); }
        if(vsh<255 && ndraw<16){ for(int r=ndraw;r<16;r++){ *REG_VRAMRW=(u16)(BLANK_TILE&0xFFFF); *REG_VRAMRW=(u16)(pal<<8); } }   /* FULL clear ndraw..15 (NOT the +4 wall-strip trim): actor SCB slots inherit TALLER stale chains from wall strips, so a heavily-shrunk far actor vshrink-over-reads into them = the 'large garbage trail under baddies'. Actors are few (~50) -> clearing the whole chain is cheap. */
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
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+spr;
        *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((((496-fy)&0x1FF)<<7)|SPR_FLASH_HT); *REG_VRAMRW=(u16)((cx&0x1FF)<<7);
    }
}
/* HUD FULL-WIDTH EDGES: gngeo's FIX layer only renders cols 1..38, so the status bar's outer 8px (fix cols
   0 + 39) sit in fix-layer blanking -> the bar read 8px-cropped each side vs the 320px 3D (sprite) view.
   Fill the two gaps with SCB sprites sampled from the ORIGINAL STBAR sprite's END columns (col 0 + col WT-1)
   at the bar's y. The FIX layer is topmost, so it masks the INNER 8px of each 16px edge sprite -> only the
   8px gap shows = seamless full-320 bar (the author's call). Emitted per frame (SCBs volatile); 2 sprites.
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
#undef SR
static const sprclass_t SPRCLASS[4]={ {1,SPR_BAR_PAL,BAR_ROT,0,0}, {5,SPR_IMP_PAL,IMP_ROTA,IMP_ROTB,IMP_CORPSE}, {5,SPR_POSS_PAL,POSS_ROTA,POSS_ROTB,POSS_CORPSE}, {5,SPR_SPOS_PAL,SPOS_ROTA,SPOS_ROTB,SPOS_CORPSE} };
static int g_anim=0;   /* free-running actor animation clock (advanced each emitted frame) */
static void vs_render(int px,int py,int ang,int emit){
    vs_px=px; vs_py=py; vs_fcs=VS_CS(ang); vs_fsn=VS_SN(ang); vs_emit=emit;
    if(emit){ int tgt=vs_floor_at(px,py)+41; g_flooreye=tgt; vs_eye += (tgt-vs_eye)>>2; }   /* ease the eye up/down with the floor -> climb stairs; g_flooreye = the INSTANT (un-eased) ref the static LUT floor is drawn at, for actor feet */
    { g_vpl=0; g_vpr=g_ncol-1; g_vpt=VS_LBT; g_vpb=VS_LBB; }   /* viewport crop NEUTRALISED (ditched) -> always full; g_vpw/g_vph are now FLOOR/CEILING murk (used in vs_lut) */
    for(int c=0;c<g_ncol;c++){ vs_ct[c]=(c>=g_vpl&&c<=g_vpr)?g_vpt:(g_vpb+1); vs_cb[c]=g_vpb; vs_nstr[c]=0; vs_clY[c]=VS_LBB+1; vs_flY[c]=VS_LBT-1; vs_sky[c]=0; vs_skd[c]=0; vs_ffl[c]=0xFF; vs_cfl[c]=0xFF; vs_wdep[c]=0x7FFF; vs_stepd[c]=0x7FFF; if(g_zonal&&!g_generic){ for(int r=0;r<5;r++)vs_fdep[c][r]=-1; for(int r=0;r<7;r++)vs_cdep[c][r]=0x7FFF; } }   /* zonal reset (skipped when gen=1: the stamp is unread); ct/cb seed the VIEWPORT band ([g_vpt,g_vpb]); columns outside [g_vpl,g_vpr] start CLOSED (ct>cb) -> walls + BSP skip them = H letterbox */
    for(int k=0;k<FLOORLUT_COLS;k++)vs_skyblk[k]=0;   /* SKY-IN-OPENING: clear the per-block dedup */
    vs_spr=41; vs_open=g_ncol; g_bbox_n=0; g_seg_n=0;
    if(emit && !g_hidehud) vs_hud_edges();   /* full-width HUD edge sprites at slots 41,42 -- BEFORE the wall budget so they're guaranteed */
    if(g_radial){   /* RADIAL far-cull (param 23): only pay the per-column threshold work when it's actually ON */
      if(g_cosrad_n!=g_ncol){ for(int c=0;c<g_ncol;c++){ int dx=c*g_colw+(g_colw>>1)-VS_HALF; g_cosrad[c]=(short)((VS_FOCAL*256)/isqrtI(VS_FOCAL*VS_FOCAL+dx*dx)); } g_cosrad_n=g_ncol; }   /* per-column cos(view angle), only on col-res change */
      for(int c=0;c<g_ncol;c++) g_murkcol[c]=(int)(((long)g_murk_eff*g_cosrad[c])>>8);   /* threshold per column = g_murk_eff*cos (refresh as the eased murk moves) */
    }
    g_vs_tiles=0; g_vs_dmin=9999; g_vs_dmax=0;
#ifdef VS_DIAG
    g_cull_near=g_cull_frus=g_cull_back=g_cull_off=g_cull_bud=0;
#endif
    int sp=0; vs_stk[sp++]=ve_root;                /* root is a node index (no bit15) */
    while(sp>0 && vs_open>0){
        unsigned short n=vs_stk[--sp];
        if(n&0x8000){ int ss=n&0x7FFF, cnt=ve_ssc[ss], first=ve_ssf[ss];   /* subsector leaf: render its segs */
            for(int i=0;i<cnt;i++) vs_render_seg(first+i); continue; }
        long cross=(long)(short)(px-ve_nx[n])*ve_ndy[n] - (long)(short)(py-ve_ny[n])*ve_ndx[n];  /* which side is the camera on? */
        unsigned short nearc,farc; const short *nearbb,*farbb;
        if(cross>0){ nearc=ve_nr[n]; farc=ve_nl[n]; nearbb=&ve_nrb[n*4]; farbb=&ve_nlb[n*4]; }
        else       { nearc=ve_nl[n]; farc=ve_nr[n]; nearbb=&ve_nlb[n*4]; farbb=&ve_nrb[n*4]; }
        if(sp<126 && vs_bbox_vis(farbb))  vs_stk[sp++]=farc;     /* push far first... (guard matches vs_stk[128]) */
        if(sp<126 && vs_bbox_vis(nearbb)) vs_stk[sp++]=nearc;    /* ...near pops first -> front-to-back */
    }
    if(!emit){ g_vs_sink+=vs_spr; return; }
    /* FLICKER: ease the effective far-horizon from this frame's budget pressure (hysteresis).
       Near the cap -> pull the horizon IN; comfortable slack -> push OUT toward the A-dial (g_vs_murk).
       Because the gate (line 554) now drops by DEPTH (dC<=g_murk_eff), a wall at depth D has the same
       fate every frame -> the far band fogs consistently instead of winking. */
    int mn=MURKMIN[g_murkmin];   /* far-cull FLOOR (shuttle 8); -1 = OFF */
    if(mn<0){ g_murk_eff=g_vs_murk; }   /* MURK FLOOR OFF: no easing pull-in, but dd (g_vs_murk) is STILL a hard far clamp (constant -> no wink). Was 32767 = walls ignored dd entirely. */
    else if(!g_murkease){ g_murk_eff=g_vs_murk; }   /* EASING OFF (param 6 = 0): constant far-cull at the dial -> no easing wink */
    else { int sh=g_murkease; if(sh>30)sh=30; int used=vs_spr-41;   /* ease the far-horizon by budget pressure; gain = >>sh (bigger = gentler). Clamp sh<=30: a >>32+ shift is UB on the 68000, and the step already floors at 1 unit by ~12 anyway. */
      if(used >= g_vs_budget-16){ int s=(g_murk_eff-mn)>>sh; g_murk_eff -= (s>1?s:1); }
      else if(used < g_vs_budget-48){ int s=(g_vs_murk-g_murk_eff)>>sh; g_murk_eff += (s>1?s:1); }
      if(g_murk_eff<mn) g_murk_eff=mn; if(g_murk_eff>g_vs_murk) g_murk_eff=g_vs_murk; }   /* CLAMP ORDER: floor (mn) first, then dd ceiling LAST -> dd is ALWAYS the hard ceiling, even when mn>dd (dd wins -> dial always bites) */
    { int nw=vs_spr-41; if(nw>VS_SBUFN)nw=VS_SBUFN;   /* TWO-PASS BURST: write the recorded wall + sky strips NOW (post-walk) so the SCB set is in flux only briefly = dense-room flicker fix */
      for(int i=0;i<nw;i++){ vstrip_t *e=&g_sbuf[i];
        if(e->sky){ vs_sky_strip_emit(41+i,e->c,e->y0,e->y1,ang); }   /* e->c = the 16px sky BLOCK (set at record time) -> drawn on the ceiling band's grid */
        else vs_strip_emit(41+i,e->c,e->y0,e->y1,e->tex,e->tcol,e->d,e->dyt,e->dyb,e->yt0,e->voff); } }
    if(emit && g_props){   /* M2: draw this map's THINGS (barrels/baddies) DEPTH-SORTED far->near, occluded per-column by vs_wdep, far-culled with the walls. g_props=0 (param 17) hides them all for geometry-only debug. */
        int bd[MAXACT], bi[MAXACT], nb=0;                                  /* keep the NEAREST MAXACT, sorted by depth DESC (far first) */
        int acull = (g_murk_eff < g_vs_murk) ? g_murk_eff : g_vs_murk;     /* actors cull at the DRAW DISTANCE even when murk is off (g_murk_eff huge) -> no tiny far actors showing through openings */
        if(vs_spr-41 >= g_vs_budget && g_vs_dmax>0 && g_vs_dmax<acull) acull=g_vs_dmax;   /* BUDGET-HALT leak fix: when the draw-count cap halted the BSP walk, far walls weren't drawn (and didn't set vs_wdep) -> tiny far actors leaked through. Cap actors at the deepest DRAWN wall so they can't outrun the budget-culled geometry. */
        for(int i=0;i<ve_nth;i++){
            if(!g_thalive[i]) continue;                                    /* COMBAT: skip killed things */
            short dx=(short)(ve_thx[i]-vs_px), dy=(short)(ve_thy[i]-vs_py);
            if(dx>acull||dx<-acull||dy>acull||dy<-acull) continue;         /* PERF: cheap BOX reject before the depth MUL+sort -- a thing beyond acull on either axis is either too far (d>acull) or far off-screen to the side (culled anyway). Skips the per-thing cost for the bulk of a 35-159 thing map. */
            int d=(int)(((long)dx*vs_fcs+(long)dy*vs_fsn)>>8);
            if(d<VS_NEAR || d>acull) continue;                             /* behind the near plane, or beyond the actor cull */
            if(nb<MAXACT){ int j=nb++; while(j>0 && bd[j-1]<d){ bd[j]=bd[j-1]; bi[j]=bi[j-1]; j--; } bd[j]=d; bi[j]=i; }   /* insert (DESC) */
            else if(d<bd[0]){ int j=0; while(j<MAXACT-1 && bd[j+1]>d){ bd[j]=bd[j+1]; bi[j]=bi[j+1]; j++; } bd[j]=d; bi[j]=i; }   /* full: evict the farthest, keep nearest N */
        }
        g_anim++; int aframe=(g_anim>>3)&1;                                /* idle/walk: toggle frame A/B every ~8 emitted frames */
        for(int k=0;k<nb;k++){ int i=bi[k]; int cls=ve_thc[i]; if(cls>3)cls=0;
            const sprclass_t *sc=&SPRCLASS[cls]; int lump=0,mir=0; const sprrot_t *R;
            if(g_thalive[i]==2 && sc->corpse){ R=&sc->corpse[0]; }   /* CORPSE: static body on the ground, no rotation/animation */
            else { if(sc->nrot>1){ int av=vs_atan2(vs_py-ve_thy[i], vs_px-ve_thx[i]); int ri=((av-ve_tha[i]+16)&255)>>5; lump=ROTLUMP[ri]; mir=ROTMIR[ri]; }   /* 8-way rotation */
                   const sprrot_t *rot=(sc->rotB && aframe)?sc->rotB:sc->rotA; R=&rot[lump]; }   /* animate: A/B frame */
            vs_billboard(ve_thx[i],ve_thy[i],ve_thz[i], R->base,R->wt,R->ht,R->lo,R->to, sc->pal, mir);
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
#define DOOR_CLEAR 48                              /* a door must open this many world-units before you can walk through */
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
   Barrels explode (FX + boom) and splash-chain neighbouring barrels; baddies scream and vanish. */
#define SHOOT_RANGE 1600
#define BLAST_RAD2  (160L*160L)
#ifndef VS_AIM
#define VS_AIM 36          /* aim-cone half-width: s*FOCAL <= VS_AIM*d  (36/160 ~= 12.7deg half -> ~25deg cone), forgiving + range-constant */
#endif
static void vs_fx_spawn(int x,int y,int z){ for(int f=0;f<NFX;f++) if(!g_fxt[f]){ g_fxx[f]=(short)x;g_fxy[f]=(short)y;g_fxz[f]=(short)z;g_fxt[f]=9; return; } }
static unsigned char deadstate(int i){ int c=ve_thc[i]; if(c>3)c=0; return (ve_thc[i]!=0 && SPRCLASS[c].corpse)?2:0; }   /* 2 = leave a CORPSE (baddie with a death sprite); 0 = gone (barrel / no corpse frame) */
static void vs_kill_at(int i){
    if(i<0||i>=ve_nth||g_thalive[i]!=1) return;     /* only a LIVING thing can be killed (corpses=2 are inert) */
    g_thalive[i]=deadstate(i);                       /* baddie -> corpse(2); barrel/no-frame -> gone(0) */
    if(ve_thc[i]!=0) return;                        /* BADDIE: no splash (SFX queued by the caller after the gun pop) */
    /* BARREL: bounded BFS chain (boom SFX queued by the caller) */
    int q[16], qn=0; q[qn++]=i;
    while(qn>0){ int b=q[--qn]; vs_fx_spawn(ve_thx[b],ve_thy[b],ve_thz[b]);
        for(int j=0;j<ve_nth;j++){ if(g_thalive[j]!=1)continue;        /* only LIVING neighbours catch the blast */
            short dx=(short)(ve_thx[j]-ve_thx[b]), dy=(short)(ve_thy[j]-ve_thy[b]);
            if((long)dx*dx+(long)dy*dy < BLAST_RAD2){ g_thalive[j]=deadstate(j);   /* blast victim: corpse (baddie) or gone (barrel) */
                if(ve_thc[j]==0 && qn<16) q[qn++]=j; } } }   /* a neighbour barrel chains; baddies in the blast leave bodies */
}
static int vs_shoot(int px,int py,int ang){
    int fcs=VS_CS(ang), fsn=VS_SN(ang);
    int best=-1, bestd=SHOOT_RANGE;
    for(int i=0;i<ve_nth;i++){ if(g_thalive[i]!=1)continue;   /* only LIVING things can be shot (corpses are inert) */
        short dx=(short)(ve_thx[i]-px), dy=(short)(ve_thy[i]-py);
        int d=(int)(((long)dx*fcs+(long)dy*fsn)>>8);            /* forward depth */
        if(d<VS_NEAR || d>=bestd) continue;
        int s=(int)(((long)dx*fsn-(long)dy*fcs)>>8); if(s<0)s=-s; /* lateral offset */
        if(s>48 && (long)s*VS_FOCAL > (long)VS_AIM*d) continue; /* hit if close laterally (<=48u, point-blank forgiving) OR within the angular aim cone (far) */
        if(!vs_move_ok(px,py,ve_thx[i],ve_thy[i])) continue;   /* a solid wall / shut door blocks the shot */
        best=i; bestd=d; }
    if(best<0) return 0;
    int sfx=(ve_thc[best]==0)?4:2;                 /* barrel -> boom, baddie -> scream */
    vs_kill_at(best); g_sndq=sfx; g_sndqt=2;       /* QUEUE the death SFX ~2 frames after the gun pop (sequence on the one channel) */
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
      if(g_liftstate==0){ g_liftsec=sec; g_lifttgt=ve_uselo[best]; g_liftprog=0; g_liftstate=1; SND(6); }   /* at REST (up) -> lower to the low stop (DOOM lower-lift) */
      else if(g_liftstate==2 && g_liftsec==sec){ g_liftstate=3; SND(6); } }            /* at the bottom -> raise back (toggle) */
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

int main(void){
    ng_cls();
    init_palettes();
    tables_init();
    const level_t*lv=map_load();
    world_init(lv);                 /* sector heights for collision */
    cam=MAP_START;
#if TITLE_FLOW
    /* ===== TITLE (DOOM TITLEPIC) -> MENU (level select) -> LOADING -> game.  TITLE_FLOW 0 = skip. ===== */
    /* --- TITLE --- */
    park_all_sprites(); ng_cls();
#if TITLE_HAVE
    for(int p=0;p<TITLE_NPAL && p<248;p++) for(int i=0;i<16;i++) MMAP_PALBANK1[(8+p)*16+i]=TITLE_PAL16[p][i];   /* TITLEPIC palettes -> slots 8.. (init_palettes restores them after) */
    { int s=1;                                               /* 20 column-sprites x 13 tiles, per-tile palette */
      for(int tx=0;tx<TITLE_COLS;tx++){
        *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_SCB1+s*64;
        for(int ty=0;ty<TITLE_ROWS;ty++){ int T=TITLE_TILE0+ty*TITLE_COLS+tx;
          *REG_VRAMRW=(u16)(T&0xFFFF); *REG_VRAMRW=(u16)(((8+TITLE_MAP[ty][tx])<<8)|(((T>>16)&0xF)<<4)); }
        *REG_VRAMMOD=0x200; *REG_VRAMADDR=ADDR_SCB2+s;
        *REG_VRAMRW=(u16)((15<<8)|255); *REG_VRAMRW=(u16)((((496-((224-TITLE_ROWS*16)/2))&0x1FF)<<7)|TITLE_ROWS); *REG_VRAMRW=(u16)((tx*16)<<7); s++; }   /* y-CENTRED in the 224 active area (488 for 13 rows = 8px top+bottom); HW can't magnify 208->224, so centring is the faithful height fix */
    }
#else
    ng_center_text( 6,0, "D O O M - N G");
    ng_center_text( 9,0, "KNEE-DEEP IN THE DEAD");
#endif
    ng_center_text(26,0, DNG_VERSION);
    SND(0x10+VE_NMAP);                                        /* TITLE: play the intro track (slot 9, after the 9 maps) -> switches to the map track at level start */
    { u8 pst=0xff, pa=0xff; u16 t=0;                          /* wait for START (or fire); blink the prompt */
      for(;;){ ng_wait_vblank();
#ifdef VS_DIAG
        break;   /* DIAG capture: skip the title wait -> straight to teleported gameplay */
#endif
        if(MENU_CAP){ break; }/*DBG headless capture: skip the title wait*/
        if(t==8) SND(0x10+VE_NMAP);   /* TITLE music re-assert a few frames in, in case the Z80 wasn't yet processing commands at the first send */
        u8 st=(u8)~(*REG_STATUS_B), jp=(u8)~(*REG_P1CNT);
        if(((st&CNT_START1)&&!(pst&CNT_START1)) || ((jp&CNT_A)&&!(pa&CNT_A))){ SND(1); break; }
        pst=st; pa=jp;
        ng_center_text(24,0, ((t>>4)&1) ? "  PRESS START  " : "               "); t++; }
    }
    /* --- NEW GAME: episode select -> skill select. The TITLEPIC left its 242 palettes in slots 8+;
       the graphic menus reuse those slots, then init_palettes() restores the game's before play. --- */
    int skill=2;                                             /* default HURT ME PLENTY */
#ifndef VS_DIAG
#if MENU_HAVE
    { static const int EPI[3]={ML_EPI1,ML_EPI2,ML_EPI3};
      static const int SKL[5]={ML_JKILL,ML_ROUGH,ML_HURT,ML_ULTRA,ML_NMARE};
#if MENU_CAP!=2
      doom_menu(ML_EPISOD,54, -1,0, EPI,3, 1, 0);              /* episode: only E1 unlocked (shareware) */
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
       simply restarts us). We drive our own start flow, so keep bios_user_mode=0 during gameplay:
       that routine then exits immediately (beq at 0xc04602) and can never yank the cart. */
    bios_user_mode=0;
    SND(0x10+g_map);                                          /* start THIS map's soundtrack (cmd 0x10+map -> ADPCM-B track); at the level, not the title */
    MMAP_PALBANK1[255*16+15]=0x0210;                          /* BACKDROP = deep murk (was black): undrawn far regions read as atmospheric depth, not void */
#if VSLICE
    {   /* VERTICAL SLICE -- never returns. Free movement in one room, live raycast walls. */
        park_all_sprites();
        MMAP_PALBANK1[4095]=MURKBG[g_murkbg];   /* far-field backdrop (shuttle 9): default black; cycle for atmospheric murk */
        for(int i2=0;i2<16;i2++) MMAP_PALBANK1[12*16+i2]=VSCEIL_PAL16[i2];   /* VS ceiling palette (its own colour order) */
        for(int i2=0;i2<16;i2++) MMAP_PALBANK1[13*16+i2]=VSFLOOR_PAL16[i2];   /* VS floor synth palette (slot 13) -- was left as the OLD FLOORLUT_PAL16; the vsfloor LUT bank expects VSFLOOR_PAL16 */
        vs_set_map(0);   /* select E1M1: repoint geometry pointers, snap camera/eye to spawn, upload per-map palettes */
        draw_status_bar();   /* DOOM status bar on the fix layer, rows 24-27 (static; persists across frames + map toggles) */
        /* (P2/NODES bank-switch removed -- VSLICE is pure ROM1 live-BSP, never reads the banked on-rails nodes) */
        for(int d=1;d<VS_RFMAX;d++) g_rf[d]=(short)(((long)VS_FOCAL<<8)/d);   /* reciprocal LUT: 1/depth, once at startup */
        g_rspan[0]=0; g_rspan[1]=65535; for(int s=2;s<VS_SPANMAX;s++) g_rspan[s]=(unsigned short)(65536U/s);   /* 1/span reciprocal LUT (s=1 clamped to fit u16) */
        int px=vs_camx, py=vs_camy, ang=vs_camang; u16 fc=0;   /* player-1 spawn of the current map (from vs_set_map) */
#ifdef VS_DIAG
        px=VS_DIAG_X; py=VS_DIAG_Y; ang=(VS_DIAG_A)&255;             /* teleport to the coord under test (VS_DIAG_A in 0..255 cart units) */
        g_zonal=1;           /* capture the ZONAL render */
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
            if(cad>worst) worst=cad;
            u8 in=(u8)~(*REG_P1CNT);
            int fcs=VS_CS(ang), fsn=VS_SN(ang);
            int dx=0,dy=0, prevx=px, prevy=py;   /* prev pos for the walk-trigger crossing test */
            if(in&CNT_UP){   dx+=(fcs*14)>>8; dy+=(fsn*14)>>8; }
            if(in&CNT_DOWN){ dx-=(fcs*14)>>8; dy-=(fsn*14)>>8; }
            /* per-axis slide, with a VS_RAD skin: test the destination extended by the radius in the
               travel direction so the camera stops VS_RAD short of the wall (no embedding) */
            if(dx){ int ex=dx+(dx>0?VS_RAD:-VS_RAD); if(vs_move_ok(px,py,px+ex,py)) px+=dx; }
            if(dy){ int ey=dy+(dy>0?VS_RAD:-VS_RAD); if(vs_move_ok(px,py,px,py+ey)) py+=dy; }
            vs_walk_triggers(prevx,prevy,px,py);   /* WALK-trigger doors: crossing a trigger seg opens its tagged door */
            if(dx||dy||(in&(CNT_LEFT|CNT_RIGHT))) g_bobc++;   /* advance the gun-bob while walking OR TURNING (the author: bob on rotation too); holds steady only when fully still */
            if(g_fire>0) g_fire--;                            /* FIRE recoil countdown */
            if(g_sndqt>0 && --g_sndqt==0) SND(g_sndq);        /* delayed death SFX (scream/boom) after the gun pop */
            if(g_doorstate==1){ g_doorprog+=6;
                if(g_doorprog>=g_doortgt){ g_doorprog=g_doortgt; g_secdc[g_doorsec]=(short)g_doortgt;     /* fully open */
                    if(g_doorstay){ g_doorsec=-1; g_doorstate=0; g_doorstay=0; }                          /* TRIGGER door: stay open, free the machine for the next one */
                    else { g_doorstate=2; g_doorhold=80; } }                                              /* MANUAL door: hold, then auto-close */
                else g_secdc[g_doorsec]=(short)g_doorprog; }   /* DOOR opening */
            else if(g_doorstate==2){ if(--g_doorhold<=0){ g_doorstate=3; SND(6); } }   /* hold open, then close (SND on close-start) */
            else if(g_doorstate==3){ g_doorprog-=6; if(g_doorprog<=0){ g_secdc[g_doorsec]=0; g_doorsec=-1; g_doorstate=0; } else g_secdc[g_doorsec]=(short)g_doorprog; }   /* DOOR closing */
            if(g_liftstate==1){ g_liftprog-=6; if(g_liftprog<=g_lifttgt){g_liftprog=g_lifttgt; g_liftstate=2;} g_secdf[g_liftsec]=(short)g_liftprog; }   /* LIFT lowering toward the low stop (drop is <=0); holds DOWN until toggled */
            else if(g_liftstate==3){ g_liftprog+=6; if(g_liftprog>=0){ g_secdf[g_liftsec]=0; g_liftsec=-1; g_liftstate=0; } else g_secdf[g_liftsec]=(short)g_liftprog; }   /* LIFT raising back to rest */
            if(in&CNT_LEFT)  ang=(ang+3)&255;   /* A = turn left */
            if(in&CNT_RIGHT) ang=(ang-3)&255;   /* D = turn right */
#ifdef VS_AUTOPAN
            if(!(in&(CNT_UP|CNT_DOWN|CNT_LEFT|CNT_RIGHT))) ang=(ang+1)&255;   /* headless-capture demo only (build -DVS_AUTOPAN); off in play -> no idle drift */
#endif
            /* DEBUG SHUTTLE (edge-detected; WASD=dpad move/turn): SPACE=show/hide HUD, P=cycle param, N=value DOWN, B=value UP. LEVEL is param 15 in the cycle. Dials are EMIT-only -- never override the BSP. */
            { static u8 pin=0; u8 pr=in & ~pin; pin=in;
              if(pr&CNT_A){ g_dbg=!g_dbg;                   /* SPACE: show/hide the debug HUD (clean view + drops the HUD-render overhead) */
                if(!g_dbg){ const char *bl="                                       "; g_vrambusy=1; for(int r=2;r<=12;r++)ng_text(2,r,1,bl); g_vrambusy=0; } }   /* clear the (now 9-row) HUD once on toggle-off */
              if(g_dbg){                                    /* TUNING: debug keys live ONLY while the HUD is shown (the author: game keys unmapped when debug enabled, debug keys unmapped when playing) */
              if(pr&CNT_B) g_sel=(g_sel+1)%NSEL;            /* P: cycle WHICH debug param */
              { int dd=(pr&CNT_D)?1:((pr&CNT_C)?-1:0);      /* B: value UP / N: value DOWN (bidirectional, wraps) */
                if(dd) switch(g_sel){
                  case 0: g_dd   =(g_dd   +VS_NDD  +dd)%VS_NDD;   break;          /* draw distance */
                  case 1: g_dci  =(g_dci  +VS_NDC  +dd)%VS_NDC;   break;          /* depth cap */
                  case 2: g_ncoli=(g_ncoli+NNCOL   +dd)%NNCOL;    SND(1); break;  /* column res (20/32/40/64/80) */
                  case 3: g_capmode=(g_capmode+NCAPMODE+dd)%NCAPMODE; break;      /* cap mode (caps back under control via the shuttle) */
                  case 4: g_zonal=!g_zonal; break;                                /* ZONAL flats ON/OFF (A/B per-row visplane vs single-flat blanket) */
                  case 5: g_generic=!g_generic; break;                            /* GENERIC mode ON/OFF (synthetic floor/ceil + sky vs real flats) */
                  case 6: g_murkease=(g_murkease+NEASE+dd)%NEASE; break;          /* far-cull EASING gain 0(off)..16(ultra-gentle) */
                  case 7: g_weapon=(g_weapon+GUNHAND_N+dd)%GUNHAND_N; break;      /* WEAPON select: cycle the 6 first-person weapons */
                  case 8: g_murkmin=(g_murkmin+NMURKMIN+dd)%NMURKMIN; break;      /* MURK FLOOR: how far in the far-cull may pull */
                  case 9: g_murkbg=(g_murkbg+NMURKBG+dd)%NMURKBG; MMAP_PALBANK1[4095]=MURKBG[g_murkbg]; break;   /* MURK BACKDROP: far-field colour (apply now) */
                  case 10:g_fogext=(g_fogext+NFOGEXT+dd)%NFOGEXT;                                /* FOG EXTENT: rescale the wall fog-band thresholds; 0 = OFF (push past any depth) */
                          if(FOGEXT[g_fogext]==0){ g_fog0=g_fog1=32767; } else { g_fog0=(400*FOGEXT[g_fogext])/100; g_fog1=(1000*FOGEXT[g_fogext])/100; } break;
                  case 11:g_floorlod=(g_floorlod+NFCLOD+dd)%NFCLOD; break;                        /* FLOOR crop (drop FAR/horizon rows, keep bottom) */
                  case 12:g_occl=!g_occl; break;                                                  /* ACTOR OCCLUSION on/off */
                  case 13:g_budgeti=(g_budgeti+NBUDGET+dd)%NBUDGET; break;                        /* DRAW-COUNT CAP: nearest-N wall strips -> flicker/perf lever */
                  case 14:g_opensky=!g_opensky; break;                                            /* SKY-IN-OPENING on/off (#42) */
                  case 15:g_seamover=(g_seamover+4+dd)&3; break;                                   /* SEAM OVERDRAW: 0..3 px strip widen (flicker mask) */
                  case 16:g_ceillod=(g_ceillod+NFCLOD+dd)%NFCLOD; break;                          /* CEILING crop (drop FAR/horizon rows, keep top) */
                  case 17:g_props=!g_props; break;                                                /* PROPS visible: hide actors (baddies/barrels) */
                  case 18:g_vpw=(g_vpw+NVP+dd)%NVP; break;                                        /* FLOOR murk rows (far->horizon) */
                  case 19:g_vph=(g_vph+NVP+dd)%NVP; break;                                        /* CEILING murk rows (far->horizon) */
                  case 20:g_hidegun=!g_hidegun; break;                                            /* HIDE WEAPON */
                  case 21:g_hidehud=!g_hidehud; break;                                            /* HIDE player status bar */
                  case 22:g_doorwalk=!g_doorwalk; break;                                          /* DOORS walk-through (no open) */
                  case 23:g_radial=!g_radial; break;                                              /* RADIAL far-cull ON/OFF (uniform euclidean reach vs flat perpendicular slab) */
                  case 24:g_vmap=!g_vmap; break;                                                  /* VERTICAL MAP: V companion (1:1+tile+peg) vs original stretch */
                  case 26:g_perf=(g_perf+4+dd)%4; { const perfpreset_t *P=&PERF[g_perf]; g_ncoli=P->col; g_budgeti=P->bud; g_dd=P->dd; g_dci=P->dc; g_murkmin=P->mmin; g_floorlod=P->flod; g_ceillod=P->clod; } break;   /* PERF PRESET: apply the lo/md/hi/ul dial set */
                  case 27:g_dd=3;g_dci=5;g_ncoli=0;g_murkmin=6;g_budgeti=8;g_fogext=14;g_floorlod=0;g_ceillod=0;g_vpw=0;g_vph=0;g_murkease=16;g_zonal=1;g_generic=1;g_occl=1;g_capmode=0;g_opensky=1;g_seamover=0;g_radial=0;g_vmap=1;g_perf=1;   /* RESET all tunables to defaults */
                          if(FOGEXT[g_fogext]==0){g_fog0=g_fog1=32767;}else{g_fog0=(400*FOGEXT[g_fogext])/100;g_fog1=(1000*FOGEXT[g_fogext])/100;} g_murkbg=1; MMAP_PALBANK1[4095]=MURKBG[1]; break;
                  default:g_map=(g_map+VE_NMAP+dd)%VE_NMAP; vs_set_map(g_map); px=vs_camx; py=vs_camy; ang=vs_camang; SND(0x10+g_map); break;   /* LEVEL: E1M1..E1M9 -> switch the map's soundtrack too (idx 25 = default) */
                } }
              } else {                                      /* PLAYING: game keys live while the HUD is hidden */
                if(pr&CNT_C){ g_fire=6; SND(1); vs_shoot(px,py,ang); }   /* C: FIRE -- always the gun pop NOW; a HIT queues the scream/boom ~2 frames later (gun, then scream) */
                if(pr&CNT_D){ int a=vs_use_special(px,py,ang);   /* D: USE -- nearest LIFT/EXIT first, else a door */
                  if(a==2){ g_map=(g_map+1)%VE_NMAP; vs_set_map(g_map); px=vs_camx; py=vs_camy; ang=vs_camang; SND(0x10+g_map); }   /* EXIT switch -> next map (resync camera like the debug LEVEL path) */
                  else if(!a) vs_use_door(px,py,ang); }
              }
              static const short DD[VS_NDD]={450,500,550,600,650,700,750,800,850,900,950,1000,1500,2000,2500,3000,3500,4000,4500,5000};   /* draw distance, world units; 50-step sweet-spot to 1000, then 500-steps to 5000 (long vistas). default DD[3]=600 */
              static const short DC[VS_NDC]={1,2,3,4,5,6,7,8,16,32,48};       /* per-column see-through DEPTH cap (layers); fine low range 1..8 + originals; 48 ~= effectively unlimited */
              g_vs_murk=DD[g_dd]; g_dcap=DC[g_dci]; g_vs_budget=BUDGET[g_budgeti]; }   /* draw-count cap from the dial (param 13); 41+288 walls + actors < 380 HW slots = safe */
            g_floor_rows=FCLOD_F[g_floorlod]; g_ceil_rows=FCLOD_C[g_ceillod];   /* floor crop (param 11) + ceiling crop (param 16), independent */
#ifdef VS_AUTOFIRE
            if((fc%40)==20){ g_fire=6; if(!vs_shoot(px,py,ang))SND(1); }   /* DBG: headless auto-fire to verify combat/explosion FX */
#endif
            vs_render(px,py,ang,1);                                   /* real frame: raycast + SCB emit (at vblank) */
            draw_gun();                                              /* first-person weapon on the fix layer (redraws only on weapon change) */
            if(g_hidehud!=g_phidehud){ g_phidehud=g_hidehud;        /* HIDE HUD (param 21): the status bar is static -> clear/redraw once on toggle */
                if(g_hidehud){ for(int col=0;col<40;col++){ *REG_VRAMMOD=1; *REG_VRAMADDR=ADDR_FIXMAP+col*32+HUD_ROW; for(int r=0;r<4;r++)*REG_VRAMRW=(u16)SROM_EMPTY_TILE; } }
                else { draw_status_bar(); g_faceN=-1; } }            /* un-hide: redraw bar + force the face to repaint */
            if(!g_hidehud) draw_face(fc);                            /* HUD face idle look-around (redraws only on frame change) */
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
            if(g_dbg){ char tk[NSEL][12]; char ln[44];    /* HUD render gated on g_dbg (P toggles). ALL params laid out -> '>' caret on the P-selected one; screenshot the whole recipe at once. */
              snprintf(tk[0],12,"dd=%d",g_vs_murk);     snprintf(tk[1],12,"dc=%d",g_dcap);
              snprintf(tk[2],12,"col=%s",NCOLW[g_ncoli].name+3);   snprintf(tk[3],12,"cap=%.4s",CAPMODE[g_capmode].name);   /* cap name truncated to 4 -> fits the 4-col layout (full names in CAPMODE[]) */
              snprintf(tk[4],12,"zon=%d",g_zonal);      snprintf(tk[5],12,"gen=%d",g_generic);
              snprintf(tk[6],12,"ease=%d",g_murkease);  snprintf(tk[7],12,"wpn=%d",g_weapon);
              if(MURKMIN[g_murkmin]<0)snprintf(tk[8],12,"mn=of");else snprintf(tk[8],12,"mn=%d",MURKMIN[g_murkmin]);   /* mmin -> mn (fits 4-col) */
              snprintf(tk[9],12,"mbg=%d",g_murkbg);
              if(FOGEXT[g_fogext]==0)snprintf(tk[10],12,"fog=of");else snprintf(tk[10],12,"fog=%d",FOGEXT[g_fogext]);
              snprintf(tk[11],12,"flod=%d",g_floorlod); snprintf(tk[12],12,"occl=%d",g_occl);
              snprintf(tk[13],12,"bud=%d",g_vs_budget); snprintf(tk[14],12,"sky=%d",g_opensky);
              snprintf(tk[15],12,"seam=%d",g_seamover); snprintf(tk[16],12,"clod=%d",g_ceillod);
              snprintf(tk[17],12,"prop=%d",g_props);    snprintf(tk[18],12,"fmrk=%d",g_vpw);
              snprintf(tk[19],12,"cmrk=%d",g_vph);       snprintf(tk[20],12,"hgun=%d",g_hidegun);
              snprintf(tk[21],12,"hhud=%d",g_hidehud);   snprintf(tk[22],12,"dwlk=%d",g_doorwalk);
              snprintf(tk[23],12,"rad=%d",g_radial);      snprintf(tk[24],12,"vmap=%d",g_vmap);   snprintf(tk[25],12,"lvl=%d",g_map+1);
              snprintf(tk[26],12,"pf=%s",PERFNM[g_perf]); snprintf(tk[27],12,"rset");
              int deg=(ang*360)>>8;
              g_vrambusy=1;
              snprintf(ln,sizeof ln,"fps=%-3d w=%-3d ns=%-3d t=%-4d ",cad?60/cad:60,worst?60/worst:60,vs_spr-41,g_vs_tiles); ng_text(2,2,1,ln);   /* perf: fps + worst + strips + tile-rows */
              for(int row=0;row<7;row++){ int p=0;       /* 26 params, 4 per row -> 7 rows (rows 3..9, clear of the gun band); caret marks g_sel */
                for(int col=0;col<4;col++){ int idx=row*4+col; if(idx>=NSEL)break;
                  p+=snprintf(ln+p,sizeof(ln)-p,"%c%-8s",(idx==g_sel)?'>':' ',tk[idx]); }   /* caret + 8-char token = 9/col x4 = 36 cols (fits the 1..38 visible fix range) */
                while(p<36 && p<(int)sizeof(ln)-1)ln[p++]=' '; ln[p]=0; ng_text(2,3+row,1,ln); }
              snprintf(ln,sizeof ln,"x%d y%d a%d d%d-%d        ",px,py,deg,g_vs_dmin>g_vs_dmax?0:g_vs_dmin,g_vs_dmax); ng_text(2,11,1,ln);   /* WAD coords+deg for refcap; d = drawn depth range (row 11: below the 8 param rows) */
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
