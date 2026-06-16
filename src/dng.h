#ifndef DNG_H
#define DNG_H
#include <stdint.h>

#define SCREEN_W 320
#define SCREEN_H 224
#define HALF_W (SCREEN_W/2)
#define HALF_H (SCREEN_H/2)

#define NG_MAX_SPR_LINE  96
#define NG_MAX_SPR_FRAME 380
#define NG_SPR_MAXW      16

typedef uint8_t angle_t;
typedef struct { float x, y; } vec2;

typedef struct { float floor, ceil; uint8_t light; int floortex, ceiltex; } sector_t;
typedef struct { int sector, xoff, yoff, uptex, lowtex, midtex; } sidedef_t;  /* tex = id, -1 none */
typedef struct { int v1, v2, flags, special, right, left; } linedef_t;
typedef struct { int v1, v2, line, side, offset; } seg_t;     /* offset = texel U at v1 */
typedef struct { int firstseg, numsegs; } subsector_t;
typedef struct { float x, y, dx, dy; int child[2]; } node_t;  /* child<0 => subsector(-child-1) */
typedef struct { vec2 pos; angle_t ang; int type, tex, sw, sh; } thing_t;

typedef struct {
    const vec2 *verts;           int nverts;
    const sector_t *sectors;     int nsectors;
    const sidedef_t *sides;      int nsides;
    const linedef_t *lines;      int nlines;
    const seg_t *segs;           int nsegs;
    const subsector_t *ssectors; int nssectors;
    const node_t *nodes;         int nnodes;
    const thing_t *things;       int nthings;
} level_t;

typedef struct { vec2 pos; angle_t ang; float z; int sector; float pitch; } camera_t;

/* A draw command. Walls carry texture coords (texel space); the backend mods by
   the texture's dimensions. Flats/things use the procedural pal colour. */
typedef enum { SC_FLAT, SC_WALL, SC_THING } spr_kind;
typedef struct {                 /* packed to fit the 64KB DrawList on the 68000 */
    int16_t sx, sy, w, h;
    uint8_t pal, light, kind;
    int16_t hw, tex, uL, uR, v0, v1;   /* tex<0 = untextured */
    int16_t depth;                     /* world-unit depth of this slice (for node-render Street-View transform) */
    int16_t dtop, dbot;                /* trapezoid: screen-y change of top/bottom edge across the band (L->R); 0 = flat rect */
    int16_t srcid;                     /* CODEC: source identity = (segidx<<2)|edge+1 (1=mid 2=upper 3=lower), 0 = none. Lets the bake correlate the same wall surface across views. */
} SpriteCmd;

#define MAX_CMDS 1024
typedef struct {
    SpriteCmd cmd[MAX_CMDS];
    int n, spr_total, line_max;
    int spr_line[SCREEN_H];
} DrawList;

typedef struct { float fov, nearz, gamma, fisheye; int max_band, constrain; float far; } proj_cfg;
extern proj_cfg PCFG;
extern int dng_flats;            /* 1 = emit per-band floor/ceiling; 0 = backend draws cheap bg */

typedef struct { int segs, divides, toviews, wall_bands, flat_fills,
                 wall_hw, flat_hw, line_walls, line_all; } rstats;
extern rstats RSTAT;

void tables_init(void);
float lut_sin(angle_t a);
float lut_cos(angle_t a);
const level_t *map_load(void);
extern camera_t MAP_START;
extern const int ROT_NV, ROT_NA;        /* rotation LUT dims: verts, angles */
extern const short ROTD[], ROTS[];      /* per-angle per-vertex rotated coords (table-driven to_view) */
extern const int SKY_TEX;        /* texture id of the sky graphic, -1 if none */
void render_world(const level_t *lv, const camera_t *cam, DrawList *dl);
int  point_sector(const level_t *lv, float x, float y);
void move_player(const level_t *lv, camera_t *cam, float dx, float dy);
int  hitscan(const level_t *lv, const camera_t *cam);
void thing_kill(int i);
void world_init(const level_t *lv);          /* copy sector heights into mutable state */
void world_update(const level_t *lv);        /* advance door/lift animations */
int  use_door(const level_t *lv, const camera_t *cam);   /* "use" ray -> open a door */
void open_all_doors(const level_t *lv);                  /* bake/tour: force all doors open (static) */

#endif
