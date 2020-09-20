
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>
#include <malloc.h>
#include <fcntl.h>
#include <math.h>
#include <dos.h>

#include <hw/cpu/cpu.h>
#include <hw/dos/dos.h>
#include <hw/dos/emm.h>
#include <hw/dos/himemsys.h>
#include <hw/vga/vga.h>
#include <hw/vga/vrl.h>
#include <hw/8254/8254.h>
#include <hw/8259/8259.h>
#include <fmt/minipng/minipng.h>

#include "timer.h"
#include "vmode.h"
#include "fonts.h"
#include "vrlimg.h"
#include "freein.h"
#include "dbgheap.h"
#include "fontbmp.h"
#include "unicode.h"
#include "commtmp.h"
#include "sin2048.h"
#include "vrldraw.h"
#include "seqcomm.h"
#include "keyboard.h"
#include "dumbpack.h"
#include "fzlibdec.h"
#include "fataexit.h"
#include "sorcpack.h"
#include "rotozoom.h"
#include "seqcanvs.h"
#include "cutscene.h"

#include <hw/8042/8042.h>

struct game_2dvec_t {
    int32_t         x,y;    /* 16.16 fixed point */
};

struct game_2dvec_t         game_position;
uint64_t                    game_angle;

#define GAME_VERTICES       128
struct game_2dvec_t         game_vertex[GAME_VERTICES];
struct game_2dvec_t         game_vertexrot[GAME_VERTICES];
unsigned                    game_vertex_max;

/* from start to end, the wall is a line and faces 90 degrees to the right from the line, DOOM style.
 *
 *       ^
 *       |
 *   end +
 *       |
 *       |
 *       |-----> faces this way (sidedef 0)
 *       |
 *       |
 * start +
 */
struct game_2dlineseg_t {
    uint16_t                start,end;                  /* vertex indices */
    uint16_t                flags;
    uint16_t                sidedef[2];                 /* [0]=front side       [1]=opposite side */
};

#define GAME_LINESEG        128
struct game_2dlineseg_t     game_lineseg[GAME_LINESEG];
unsigned                    game_lineseg_max;

struct game_2dsidedef_t {
    uint8_t                 texture;
    unsigned                texture_render_w;
    int8_t                  xoffset,yoffset;
    uint8_t                 sector;                     /* which sector this faces */
};

#define GAME_SIDEDEFS       128
struct game_2dsidedef_t     game_sidedef[GAME_SIDEDEFS];

struct game_2dsector_t {
    int32_t                 top,bottom;
    uint8_t                 floor,ceiling;
};

#define GAME_SECTORS        16
struct game_2dsector_t      game_sector[GAME_SECTORS];

/* No BSP tree, sorry. The 3D "overworld" is too simple and less important to need it.
 * Also no monsters and cacodemons to shoot. */

#define GAME_TEXTURE_W      64
#define GAME_TEXTURE_H      64
struct game_2dtexture_t {
    unsigned char*          tex;        /* 64x64 texture = 2^6 * 2^6 = 2^12 = 4096 bytes = 4KB */
};

#define GAME_TEXTURES       8
struct game_2dtexture_t     game_texture[GAME_TEXTURES] = { {NULL} };

struct game_vslice_t {
    int16_t                 top,bottom;         /* total slice including floor to ceiling */
    int16_t                 floor,ceil;         /* wall slice (from floor to ceiling) */
    unsigned                sidedef;
    unsigned                flags;
    unsigned                next;               /* next to draw or ~0u */
    uint8_t                 tex_n;
    uint8_t                 tex_x;
    int32_t                 dist;
};

#define VSF_TRANSPARENT     (1u << 0u)

#define GAME_VSLICE_MAX     2048
struct game_vslice_t        game_vslice[GAME_VSLICE_MAX];
unsigned                    game_vslice_alloc;

#define GAME_VSLICE_DRAW    320
unsigned                    game_vslice_draw[GAME_VSLICE_DRAW];

#define GAME_MIN_Z          (1l << 12l)

#define GAMETEX_LOAD_PAL0   (1u << 0u)

void game_texture_load(const unsigned i,const char *path,const unsigned f) {
    struct minipng_reader *rdr;
    unsigned char *row;
    unsigned int ri;

    if (game_texture[i].tex != NULL)
        return;

    if ((rdr=minipng_reader_open(path)) == NULL)
        fatal("gametex png error %s",path);
    if (minipng_reader_parse_head(rdr) || rdr->plte == NULL || rdr->plte_count == 0 || rdr->ihdr.width != 64 || rdr->ihdr.height != 64 || rdr->ihdr.bit_depth != 8)
        fatal("gametex png error %s",path);
    if ((game_texture[i].tex=malloc(64*64)) == NULL)
        fatal("gametex png error %s",path);
    if ((row=malloc(64)) == NULL)
        fatal("gametex png error %s",path);

    for (ri=0;ri < 64;ri++) {
        minipng_reader_read_idat(rdr,row,1); /* pad byte */
        minipng_reader_read_idat(rdr,row,64); /* row */

        {
            unsigned int x;
            unsigned char *srow = row;
            unsigned char *drow = game_texture[i].tex + ri;

            for (x=0;x < 64;x++) {
                *drow = *srow++;
                drow += 64;
            }
        }
    }

    if (f & GAMETEX_LOAD_PAL0) {
        unsigned char *pal = (unsigned char*)rdr->plte;
        unsigned int x;

        vga_palette_lseek(0);
        for (x=0;x < rdr->plte_count;x++) vga_palette_write(pal[x*3+0]>>2,pal[x*3+1]>>2,pal[x*3+2]>>2);
    }

    minipng_reader_close(&rdr);
    free(row);
}

int32_t game_3dto2d(struct game_2dvec_t *d2) {
    const int32_t dist = d2->y >> 2l;

    d2->x = ((int32_t)(160l << 16l)) + (int32_t)(((int64_t)d2->x << (16ll + 6ll)) / (int64_t)dist); /* fixed point 16.16 division */
    d2->y = 100l << 16l;

    return dist;
}

#define TEXPRECIS               (0)
#define ZPRECSHIFT              (8)
void game_project_lineseg(const unsigned int i) {
    struct game_2dlineseg_t *lseg = &game_lineseg[i];
    struct game_2dsidedef_t *sdef;

    /* 3D to 2D project */
    /* if the vertices are backwards, we're looking at the opposite side */
    {
        unsigned sidedef;
        struct game_2dvec_t pr1,pr2;
        int32_t od1,od2;
        int32_t  u1,u2;
        int32_t d1,d2;
        unsigned side;
        int x1,x2,x;
        int ix,ixd;

        u1 = 0;
        u2 = 0x10000ul;
        pr1 = game_vertexrot[lseg->start];
        pr2 = game_vertexrot[lseg->end];
        side = 0;

        if (pr1.y < GAME_MIN_Z && pr2.y < GAME_MIN_Z) {
            return;
        }
        else if (pr1.y < GAME_MIN_Z || pr2.y < GAME_MIN_Z) {
            const int32_t dx = pr2.x - pr1.x;
            const int32_t dy = pr2.y - pr1.y;

            if (dx == 0l) {
                if (pr1.y < GAME_MIN_Z) pr1.y = GAME_MIN_Z;
                if (pr2.y < GAME_MIN_Z) pr2.y = GAME_MIN_Z;
            }
            else {
                if (pr2.y < GAME_MIN_Z) {
                    const int32_t cdy = GAME_MIN_Z - pr1.y;
                    const int32_t cdx = (int32_t)(((int64_t)cdy * (int64_t)dx) / (int64_t)dy);
                    u2 = (int32_t)(((int64_t)u2 * (int64_t)cdx) / (int64_t)dx);
                    pr2.x = pr1.x + cdx;
                    pr2.y = GAME_MIN_Z;
                }
                else if (pr1.y < GAME_MIN_Z) {
                    const int32_t cdy = GAME_MIN_Z - pr2.y;
                    const int32_t cdx = (int32_t)(((int64_t)cdy * (int64_t)dx) / (int64_t)dy);
                    u1 = 0x10000l - (int32_t)(((int64_t)(0x10000l - u1) * (int64_t)-cdx) / (int64_t)dx);
                    pr1.x = pr2.x + cdx;
                    pr1.y = GAME_MIN_Z;
                }
                else {
                    return;
                }
            }
        }

        d1 = game_3dto2d(&pr1);
        d2 = game_3dto2d(&pr2);

        if (pr1.x > pr2.x) {
            struct game_2dvec_t tpr;
            uint16_t tu;
            int32_t td;

            tpr = pr1;
            td = d1;
            tu = u1;

            pr1 = pr2;
            d1 = d2;
            u1 = u2;

            pr2 = tpr;
            d2 = td;
            u2 = tu;

            side = 1;
        }

        if ((sidedef=lseg->sidedef[side]) == (~0u))
            return;

        sdef = &game_sidedef[sidedef];

        od1 = d1;
        d1 = (1l << (32l - (int32_t)ZPRECSHIFT)) / (int32_t)d1;         /* d1 = 1/z1 */

        od2 = d2;
        d2 = (1l << (32l - (int32_t)ZPRECSHIFT)) / (int32_t)d2;         /* d2 = 1/z2 */

        ix = x1 = (int)(pr1.x >> 16l);
        if (x1 < 0) x1 = 0;

        x2 = (int)(pr2.x >> 16l);
        ixd = x2 - ix;
        if (x2 > 320) x2 = 320;

        u1 = (int32_t)((((int64_t)sdef->texture_render_w << (int64_t)TEXPRECIS) * (int64_t)u1) / (int64_t)od1);
        u2 = (int32_t)((((int64_t)sdef->texture_render_w << (int64_t)TEXPRECIS) * (int64_t)u2) / (int64_t)od2);

        for (x=x1;x < x2;x++) {
            if (game_vslice_alloc < GAME_VSLICE_MAX) {
#if 1/*ASM*/
                const unsigned pri = game_vslice_draw[x];
                int32_t id,d;

                /* id = d1 + (((d2 - d1) * (x - ix)) / ixd); */     /* interpolate between 1/z1 and 1/z2 */
                __asm {
                    .386
                    mov     ax,x
                    sub     ax,ix
                    movsx   eax,ax                  ; eax = x - ix
                    mov     ebx,d2
                    sub     ebx,d1                  ; ebx = d2 - d1
                    imul    ebx                     ; edx:eax = (x - ix) * (d2 - d1)
                    mov     bx,ixd
                    movsx   ebx,bx                  ; ebx = ixd
                    idiv    ebx                     ; eax = ((x - ix) * (d2 - d1)) / ixd
                    add     eax,d1                  ; eax = u1 + (((x - ix) * (d2 - d1)) / ixd)
                    mov     id,eax
                }
                /* d = (1l << (32l - (int32_t)ZPRECSHIFT)) / id; */ /* d = 1 / id */
                __asm {
                    .386
                    xor     edx,edx
                    mov     eax,0x1000000           ; 1 << (32 - 8) = 1 << 24 = 0x01000000
                    idiv    id
                    mov     d,eax
                }
#else
                const int32_t id = d1 + (((d2 - d1) * (x - ix)) / ixd);     /* interpolate between 1/z1 and 1/z2 */
                const int32_t d = (1l << (32l - (int32_t)ZPRECSHIFT)) / id; /* d = 1 / id */
                const unsigned pri = game_vslice_draw[x];
#endif

                if (pri != (~0u)) {
                    if (d > game_vslice[pri].dist) {
                        if (!(game_vslice[pri].flags & VSF_TRANSPARENT))
                            continue;
                    }
                }

                {
#if 1/*ASM*/
                    const unsigned vsi = game_vslice_alloc++;
                    struct game_vslice_t *vs = &game_vslice[vsi];
                    int32_t tid,tx;
                    int h;

                    /* tid = u1 + (((u2 - u1) * (x - ix)) / ixd); */     /* interpolate between 1/u1 and 1/u2 (texture mapping) */
                    __asm {
                        .386
                        mov     ax,x
                        sub     ax,ix
                        movsx   eax,ax                  ; eax = x - ix
                        mov     ebx,u2
                        sub     ebx,u1                  ; ebx = u2 - u1
                        imul    ebx                     ; edx:eax = (x - ix) * (u2 - u1)
                        mov     bx,ixd
                        movsx   ebx,bx                  ; ebx = ixd
                        idiv    ebx                     ; eax = ((x - ix) * (u2 - u1)) / ixd
                        add     eax,u1                  ; eax = u1 + (((x - ix) * (u2 - u1)) / ixd)
                        mov     tid,eax
                    }
                    /* tx = (tid << (16l - (int32_t)ZPRECSHIFT)) / id; */ /* texture map u coord = 1 / tid */
                    __asm {
                        .386
                        xor     edx,edx
                        mov     eax,tid
                        mov     cl,8                ; (16 - 8)
                        shl     eax,cl
                        idiv    id
                        mov     tx,eax
                    }
                    /* h = (64l << 16l) / d; */
                    __asm {
                        .386
                        xor     edx,edx
                        mov     eax,0x400000        ; (64l << 16l) = 0x40 << 16l = 0x400000
                        idiv    d
                        mov     h,ax
                    }
#else/*C*/
                    const int32_t tid = u1 + (((u2 - u1) * (x - ix)) / ixd);      /* interpolate between 1/u1 and 1/u2 (texture mapping) */
                    const int32_t tx = (tid << (16l - (int32_t)ZPRECSHIFT)) / id; /* texture map u coord = 1 / tid */
                    const int h = (int)((64l << 16l) / d);
                    const unsigned vsi = game_vslice_alloc++;
                    struct game_vslice_t *vs = &game_vslice[vsi];
#endif

                    vs->top = 0;
                    vs->bottom = 0;
                    vs->flags = 0;
                    vs->sidedef = sidedef;
                    vs->ceil = (int)(((100 << 1) - h) >> 1);
                    vs->floor = (int)(((100 << 1) + h) >> 1);

                    if (vs->flags & VSF_TRANSPARENT)
                        vs->next = pri;
                    else
                        vs->next = (~0u);

                    vs->dist = d;
                    vs->tex_n = sdef->texture;
                    vs->tex_x = (tx >> TEXPRECIS) & 0x3Fu;

                    game_vslice_draw[x] = vsi;
                }
            }
        }
    }
}
#undef ZPRECSHIFT

void game_texture_free(struct game_2dtexture_t *t) {
    if (t->tex != NULL) {
        free(t->tex);
        t->tex = NULL;
    }
}

void game_texture_freeall(void) {
    unsigned int i;

    for (i=0;i < GAME_TEXTURES;i++)
        game_texture_free(&game_texture[i]);
}

static inline void game_set_vertexfip(const unsigned i,int32_t x,int32_t y) {
    game_vertex[i].x = x;
    game_vertex[i].y = y;
}

static inline void game_set_linedef_ss(const unsigned i,const unsigned s,const unsigned e,const uint16_t flags,const unsigned sd) {
    game_lineseg[i].start = s;
    game_lineseg[i].end = e;
    game_lineseg[i].flags = flags;
    game_lineseg[i].sidedef[0] = sd;
    game_lineseg[i].sidedef[1] = ~0u;
}

static inline void game_set_linedef_sd(const unsigned i,const unsigned s,const unsigned e,const uint16_t flags,const unsigned sd,const unsigned sd2) {
    game_lineseg[i].start = s;
    game_lineseg[i].end = e;
    game_lineseg[i].flags = flags;
    game_lineseg[i].sidedef[0] = sd;
    game_lineseg[i].sidedef[1] = sd2;
}

static inline void game_set_sidedef(const unsigned i,const unsigned tex,const int8_t xoff,const int8_t yoff,const unsigned int sector,const unsigned int texw) {
    game_sidedef[i].texture_render_w = texw;
    game_sidedef[i].texture = tex;
    game_sidedef[i].xoffset = xoff;
    game_sidedef[i].yoffset = yoff;
    game_sidedef[i].sector = sector;
}

static inline void game_set_sector(const unsigned i,const int32_t top,const int32_t bottom,const unsigned floor,const unsigned ceil) {
    game_sector[i].top = top;
    game_sector[i].bottom = bottom;
    game_sector[i].floor = floor;
    game_sector[i].ceiling = ceil;
}

#define MAX_VSLICE_DRAW     8

void game_loop(void) {
    unsigned int vslice_draw_count;
    uint16_t vslice_draw[MAX_VSLICE_DRAW];
    struct game_vslice_t *vsl;
    uint32_t prev,cur;
    unsigned int x2;
    unsigned int o;
    unsigned int i;
    unsigned int x;

    /* seqanim rotozoomer needs sin2048 */
    if (sin2048fps16_open())
        fatal("cannot open sin2048");

    game_texture_load(0,"watx0001.png",GAMETEX_LOAD_PAL0);
    game_texture_load(1,"watx0002.png",0);
    game_texture_load(2,"watx0003.png",0);
    game_texture_load(3,"watx0004.png",0);

    game_vertex_max = 0;
    game_lineseg_max = 0;

    /* init pos */
    game_position.x = 0;
    game_position.y = 0;
    game_angle = 0; /* looking straight ahead */

    /*    0------------>1           point 0 at -4, 4 | point 1 at  4, 4
     *   /|\     |      |
     *    |             |
     *    |--    x    --|           x at 0, 0        | y increases ahead of user at angle == 0, x increases to the right
     *    |             |
     *    |      |     \|/
     *    3<------------2           point 3 at -4,-4 | point 2 at  4,-4
     */
    game_set_vertexfip(0,   -4l << 16l,     4l << 16l); /* -4, 4 */
    game_set_vertexfip(1,    4l << 16l,     4l << 16l); /*  4, 4 */
    game_set_vertexfip(2,    4l << 16l,    -4l << 16l); /*  4,-4 */
    game_set_vertexfip(3,   -4l << 16l,    -4l << 16l); /* -4,-4 */

    /*           |
     *    4<------------7
     *    |            /|\
     *    |             |
     *  --|      x      |--
     *    |             |
     *   \|/            |
     *    5------------>6
     *           |
     */

    game_set_vertexfip(4,   -1l << 16l,     1l << 16l); /* -1, 1 */
    game_set_vertexfip(5,   -1l << 16l,    -1l << 16l); /* -1,-1 */
    game_set_vertexfip(6,    1l << 16l,    -1l << 16l); /*  1,-1 */
    game_set_vertexfip(7,    1l << 16l,     1l << 16l); /*  1, 1 */

    game_vertex_max = 8;

    game_set_linedef_ss(0,  0,      1,  0x0000/*flags*/,            0/*sidedef*/);
    game_set_linedef_ss(1,  1,      2,  0x0000/*flags*/,            0/*sidedef*/);
    game_set_linedef_ss(2,  2,      3,  0x0000/*flags*/,            0/*sidedef*/);
    game_set_linedef_ss(3,  3,      0,  0x0000/*flags*/,            0/*sidedef*/);

    game_set_linedef_ss(4,  4,      5,  0x0000/*flags*/,            1/*sidedef*/);
    game_set_linedef_ss(5,  5,      6,  0x0000/*flags*/,            1/*sidedef*/);
    game_set_linedef_ss(6,  6,      7,  0x0000/*flags*/,            1/*sidedef*/);
    game_set_linedef_ss(7,  7,      4,  0x0000/*flags*/,            1/*sidedef*/);

    game_lineseg_max = 8;

    game_set_sidedef(0,     0/*texture*/,   0/*xoff*/,  0/*yoff*/,  0/*sector*/,    64*8/*texture w*/);
    game_set_sidedef(1,     1/*texture*/,   0/*xoff*/,  0/*yoff*/,  0/*sector*/,    64*2/*texture w*/);

    game_set_sector(0,       1l << 16l/*top*/,      -1l << 16l/*bottom*/,       0/*floor*/,     1/*ceiling*/);

    init_keyboard_irq();

    cur = read_timer_counter();

    while (1) {
        prev = cur;
        cur = read_timer_counter();

        if (kbdown_test(KBDS_ESCAPE)) break;

        if (kbdown_test(KBDS_UP_ARROW)) {
            const unsigned ga = game_angle >> 5u;
            game_position.x += ((int32_t)sin2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 30l;
            game_position.y += ((int32_t)cos2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 30l;
        }
        if (kbdown_test(KBDS_DOWN_ARROW)) {
            const unsigned ga = game_angle >> 5u;
            game_position.x -= ((int32_t)sin2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 60l;
            game_position.y -= ((int32_t)cos2048fps16_lookup(ga) * (int32_t)(cur - prev)) / 60l;
        }
        if (kbdown_test(KBDS_LSHIFT) || kbdown_test(KBDS_RSHIFT)) {
            if (kbdown_test(KBDS_LEFT_ARROW)) {
                const unsigned ga = game_angle >> 5u;
                game_position.x -= ((int32_t)sin2048fps16_lookup(ga + 0x800) * (int32_t)(cur - prev)) / 60l;
                game_position.y -= ((int32_t)cos2048fps16_lookup(ga + 0x800) * (int32_t)(cur - prev)) / 60l;
            }
            if (kbdown_test(KBDS_RIGHT_ARROW)) {
                const unsigned ga = game_angle >> 5u;
                game_position.x += ((int32_t)sin2048fps16_lookup(ga + 0x800) * (int32_t)(cur - prev)) / 60l;
                game_position.y += ((int32_t)cos2048fps16_lookup(ga + 0x800) * (int32_t)(cur - prev)) / 60l;
            }
        }
        else {
            if (kbdown_test(KBDS_LEFT_ARROW))
                game_angle -= (((int32_t)(cur - prev)) << 15l) / 60l;
            if (kbdown_test(KBDS_RIGHT_ARROW))
                game_angle += (((int32_t)(cur - prev)) << 15l) / 60l;
        }

        /* clear screen */
        vga_write_sequencer(0x02/*map mask*/,0xF);
        vga_rep_stosw(vga_state.vga_graphics_ram,0,((320u/4u)*200)/2u);

        /* project and render */
        game_vslice_alloc = 0;
        for (i=0;i < GAME_VSLICE_DRAW;i++) game_vslice_draw[i] = ~0u;

        for (i=0;i < game_vertex_max;i++) {
            /* TODO: 2D rotation based on player angle */
            /* TODO: Perhaps only the line segments we draw */
            game_vertexrot[i].x = game_vertex[i].x - game_position.x;
            game_vertexrot[i].y = game_vertex[i].y - game_position.y;

            {
                const unsigned ga = game_angle >> 5u;
                const int64_t inx = ((int64_t)game_vertexrot[i].x * (int64_t)cos2048fps16_lookup(ga)) - ((int64_t)game_vertexrot[i].y * (int64_t)sin2048fps16_lookup(ga));
                const int64_t iny = ((int64_t)game_vertexrot[i].y * (int64_t)cos2048fps16_lookup(ga)) + ((int64_t)game_vertexrot[i].x * (int64_t)sin2048fps16_lookup(ga));
                game_vertexrot[i].x = (int32_t)(inx >> 15ll);
                game_vertexrot[i].y = (int32_t)(iny >> 15ll);
            }
        }

        for (i=0;i < game_lineseg_max;i++)
            game_project_lineseg(i);

        for (i=0;i < GAME_VSLICE_DRAW;i++) {
            uint16_t vslice,vi;

            vslice_draw_count = 0;
            vslice = game_vslice_draw[i];
            while (vslice != (~0u) && vslice_draw_count < MAX_VSLICE_DRAW) {
                vslice_draw[vslice_draw_count++] = vslice;
                vslice = game_vslice[vslice].next;
            }

            vga_write_sequencer(0x02/*map mask*/,1u << (i & 3u));

            for (vi=vslice_draw_count;vi != 0;) {
                __segment vs = FP_SEG(vga_state.vga_graphics_ram);
                __segment texs;
                unsigned texo;
                uint16_t tf,ts,tw;

                vslice = vslice_draw[--vi];
                vsl = &game_vslice[vslice];
                x2 = (unsigned int)((vsl->floor) < 0 ? 0 : vsl->floor);
                x = (unsigned int)((vsl->ceil) < 0 ? 0 : vsl->ceil);
                if (x2 > 200) x2 = 200;
                if (x > 200) x = 200;
                if (x >= x2) continue;

                texs = FP_SEG(game_texture[vsl->tex_n].tex);
                texo = FP_OFF(game_texture[vsl->tex_n].tex) + (vsl->tex_x * 64u);
                {
                    const uint32_t s = (0x10000ul * 64ul) / (uint32_t)(vsl->floor - vsl->ceil);
                    tw = (uint16_t)(s >> 16ul);
                    ts = (uint16_t)(s & 0xFFFFul);
                    tf = 0;
                }

                o = (i >> 2u) + (x * 80u) + FP_OFF(vga_state.vga_graphics_ram);
                x2 -= x;

                if (vsl->ceil < 0) {
                    uint32_t adv = ((uint32_t)(-vsl->ceil) * (uint32_t)(ts)) + (uint32_t)(tf);
                    texo += (uint16_t)(-vsl->ceil) * (uint16_t)(tw);
                    texo += (uint16_t)(adv >> 16ul);
                    tf = (uint16_t)(adv);
                }

                /* do not access data local variables between PUSH DS and POP DS.
                 * local stack allocated variables are fine until the PUSH BP because most compilers
                 * including Open Watcom code locals access as some form of MOV ...,[BP+n] */
                __asm {
                    mov         cx,x2
                    mov         si,texo
                    mov         di,o
                    mov         dx,tw
                    mov         ax,tf
                    mov         bx,ts
                    push        ds
                    mov         es,vs
                    mov         ds,texs
                    push        bp
                    mov         bp,bx
yal1:               ; CX = x2  DS:SI = texs:texo  ES:DI = vs:o  DX = tw  AX = tf  BP = ts  BX = (left aside for pixel copy)
                    mov         bl,[si]
                    mov         es:[di],bl
                    add         di,80               ; o += 80
                    add         ax,bp               ; ts += tf
                    adc         si,dx               ; texo += tw + CF
                    loop        yal1
                    pop         bp
                    pop         ds
                }

                vslice = vsl->next;
            }
        }

        /* present to screen, flip pages, wait for vsync */
        vga_swap_pages(); /* current <-> next */
        vga_update_disp_cur_page();
        vga_wait_for_vsync(); /* wait for vsync */
    }

    restore_keyboard_irq();
    game_texture_freeall();
}

/*---------------------------------------------------------------------------*/
/* main                                                                      */
/*---------------------------------------------------------------------------*/

int main(int argc,char **argv) {
    probe_dos();
	cpu_probe();
    if (cpu_basic_level < 3) {
        printf("This game requires a 386 or higher\n");
        return 1;
    }

	if (!probe_8254()) {
		printf("No 8254 detected.\n");
		return 1;
	}
	if (!probe_8259()) {
		printf("No 8259 detected.\n");
		return 1;
	}
    if (!probe_vga()) {
        printf("VGA probe failed.\n");
        return 1;
    }
    if (!(vga_state.vga_flags & VGA_IS_VGA)) {
        printf("This game requires VGA\n");
        return 1;
    }
    detect_keyboard();

#if TARGET_MSDOS == 16
# if 0 // not using it yet
    probe_emm();            // expanded memory support
    probe_himem_sys();      // extended memory support
# endif
#endif

    if (argc > 1 && !strcmp(argv[1],"KBTEST")) {
        printf("Keyboard test. Hit keys to see scan codes. ESC to exit.\n");

        init_keyboard_irq();
        while (1) {
            int k = kbd_buf_read();
            if (k >= 0) printf("0x%x\n",k);
            if (k == KBDS_ESCAPE) break;
        }

        printf("Dropping back to DOS.\n");
        unhook_irqs(); // including keyboard
        return 0;
    }

    init_timer_irq();
    init_vga256unchained();

    seq_intro();
    game_loop();

    gen_res_free();
    check_heap();
    unhook_irqs();
    restore_text_mode();

    //debug
    dbg_heap_list();

    return 0;
}

