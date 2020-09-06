
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <assert.h>
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

struct seq_anim_i {
    unsigned int        duration;       // in ticks
    unsigned int        frame_rate;     // in Hz
    int                 init_frame;
    int                 min_frame;
    int                 max_frame;
    unsigned int        flags;
    signed char         init_dir;
};

#define SEQANF_PINGPONG     (0x01u)
#define SEQANF_OFF          (0x02u)

uint32_t counter;
uint16_t timer_irq_interval; /* PIT clock ticks per IRQ */
uint16_t timer_irq_count;
uint16_t timer_tick_rate = 120;

/* must disable interrupts temporarily to avoid incomplete read */
uint32_t counter_read() {
    uint32_t tmp;

    SAVE_CPUFLAGS( _cli() ) {
        tmp = counter;
    } RESTORE_CPUFLAGS();

    return tmp;
}

void (__interrupt __far *prev_timer_irq)() = NULL;
static void __interrupt __far timer_irq() { /* IRQ 0 */
    counter++;

    /* make sure the BIOS below our handler still sees 18.2Hz */
    {
        const uint32_t s = (uint32_t)timer_irq_count + (uint32_t)timer_irq_interval;
        timer_irq_count = (uint16_t)s;
        if (s >= (uint32_t)0x10000)
            _chain_intr(prev_timer_irq);
        else
            p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | 0);
    }
}

void init_timer_irq() {
    if (prev_timer_irq == NULL) {
        p8259_mask(0);
        p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | 0);
        prev_timer_irq = _dos_getvect(irq2int(0));
        _dos_setvect(irq2int(0),timer_irq);
        timer_irq_interval = T8254_REF_CLOCK_HZ / timer_tick_rate;
	    write_8254_system_timer(timer_irq_interval);
        timer_irq_count = 0;
        counter = 0;
        p8259_unmask(0);
    }
}

void restore_timer_irq() {
    if (prev_timer_irq != NULL) {
        p8259_mask(0);
        p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | 0);
        _dos_setvect(irq2int(0),prev_timer_irq);
	    write_8254_system_timer(0); /* normal 18.2Hz timer tick */
        prev_timer_irq = NULL;
        p8259_unmask(0);
    }
}

struct vrl_image {
    struct vrl1_vgax_header*        vrl_header;
    vrl1_vgax_offset_t*             vrl_lineoffs;
    unsigned char*                  buffer;
    unsigned int                    bufsz;
};

void free_vrl(struct vrl_image *img) {
    if (img->buffer != NULL) {
        free(img->buffer);
        img->buffer = NULL;
    }
    if (img->vrl_lineoffs != NULL) {
        free(img->vrl_lineoffs);
        img->vrl_lineoffs = NULL;
    }
    img->vrl_header = NULL;
    img->bufsz = 0;
}

int load_vrl(struct vrl_image *img,const char *path) {
    struct vrl1_vgax_header *vrl_header;
    vrl1_vgax_offset_t *vrl_lineoffs;
    unsigned char *buffer = NULL;
    unsigned int bufsz = 0;
    int fd = -1;

    fd = open(path,O_RDONLY|O_BINARY);
    if (fd < 0) {
        fprintf(stderr,"Unable to open '%s'\n",path);
        goto fail;
    }
    {
        unsigned long sz = lseek(fd,0,SEEK_END);
        if (sz < sizeof(*vrl_header)) goto fail;
        if (sz >= 65535UL) goto fail;

        bufsz = (unsigned int)sz;
        buffer = malloc(bufsz);
        if (buffer == NULL) goto fail;

        lseek(fd,0,SEEK_SET);
        if ((unsigned int)read(fd,buffer,bufsz) < bufsz) goto fail;

        vrl_header = (struct vrl1_vgax_header*)buffer;
        if (memcmp(vrl_header->vrl_sig,"VRL1",4) || memcmp(vrl_header->fmt_sig,"VGAX",4)) goto fail;
        if (vrl_header->width == 0 || vrl_header->height == 0) goto fail;
    }
    close(fd);

    /* preprocess the sprite to generate line offsets */
    vrl_lineoffs = vrl1_vgax_genlineoffsets(vrl_header,buffer+sizeof(*vrl_header),bufsz-sizeof(*vrl_header));
    if (vrl_lineoffs == NULL) goto fail;

    img->vrl_header = vrl_header;
    img->vrl_lineoffs = vrl_lineoffs;
    img->buffer = buffer;
    img->bufsz = bufsz;

    return 0;
fail:
    if (buffer != NULL) free(buffer);
    if (fd >= 0) close(fd);
    return 1;
}

void pal_buf_to_vga(unsigned int offset,unsigned int count,unsigned char *palette) {
    unsigned int i;

    vga_palette_lseek(offset);
    for (i=0;i < count;i++) vga_palette_write(palette[(i*3)+0]>>2,palette[(i*3)+1]>>2,palette[(i*3)+2]>>2);
}

int pal_load_to_vga(unsigned int offset,unsigned int count,const char *path) {
    unsigned char *palette;
    int fd,ret=-1;

    /* load color palette */
    fd = open(path,O_RDONLY|O_BINARY);
    if (fd >= 0) {
        palette = malloc(3u*count);
        if (palette != NULL) {
            read(fd,palette,3u*count);
            close(fd);

            pal_buf_to_vga(offset,count,palette);

            free(palette);
            ret = 0;
        }
    }

    return 0;
}

#define VGA_PAGE_FIRST          0x0000
#define VGA_PAGE_SECOND         0x4000

/* VGA unchained page flipping state for all code here */
VGA_RAM_PTR orig_vga_graphics_ram;
unsigned int vga_cur_page,vga_next_page;

void vga_swap_pages() {
    vga_cur_page = vga_next_page;
    vga_next_page = (vga_next_page ^ 0x4000) & 0x7FFF;
    vga_state.vga_graphics_ram = orig_vga_graphics_ram + vga_next_page;
}

void vga_update_disp_cur_page() {
    /* make sure the CRTC is not in the blanking portions or vsync
     * so that we're not changing offset during a time the CRTC might
     * latch the new display offset.
     *
     * caller is expected to wait for vsync start at some point to
     * keep time with vertical refresh or bad things (flickering)
     * happen. */
    vga_wait_for_vsync_end();
    vga_wait_for_hsync_end();
    vga_set_start_location(vga_cur_page);
}

static unsigned int vga_rep_stosw(const unsigned char far * const vp,const uint16_t v,const unsigned int wc);
#pragma aux vga_rep_stosw = \
    "rep stosw" \
    parm [es di] [ax] [cx] \
    modify [di cx] \
    value [di];

void vga_clear_npage() {
    vga_write_sequencer(0x02/*map mask*/,0xF);
    vga_rep_stosw(vga_state.vga_graphics_ram,0,0x4000u/2u); /* 16KB (8KB 16-bit WORDS) */
}

void init_vga256unchained() {
    vga_cur_page=VGA_PAGE_FIRST;
    vga_next_page=VGA_PAGE_SECOND;

    int10_setmode(19);
    update_state_from_vga();

    /* Real hardware testing results show that the VGA BIOS will set up chained 256-color mode
     * and only clear the bytes drawn and accessible by that. If you switch on unchained mode X,
     * the junk between the chained DWORDs become visible (not cleared by VGA BIOS). So before
     * we switch to it, use the DAC mask to blank the display first, then switch on unchained
     * mode, clear video memory, then unblank the display by restoring the mask. */
    outp(0x3C6,0x00); // DAC mask: set to 0 to blank display

    vga_enable_256color_modex(); // VGA mode X (unchained)

    vga_write_sequencer(0x02/*map mask*/,0xF); // zero the video memory
    vga_rep_stosw(vga_state.vga_graphics_ram,0,0x8000u/2u);

    outp(0x3C6,0xFF); // DAC mask: restore display

    orig_vga_graphics_ram = vga_state.vga_graphics_ram;
    vga_state.vga_graphics_ram = orig_vga_graphics_ram + vga_next_page;
    vga_set_start_location(vga_cur_page);
}

void restore_text_mode() {
    int10_setmode(3);
}

void unhook_irqs() {
    restore_timer_irq();
}

void fatal(const char *msg,...) {
    va_list va;

    unhook_irqs();
    restore_text_mode();

    printf("FATAL ERROR: ");

    va_start(va,msg);
    vprintf(msg,va);
    va_end(va);
    printf("\n");

    exit(1);
}

/*---------------------------------------------------------------------------*/
/* introductory sequence                                                     */
/*---------------------------------------------------------------------------*/

#define ANIM_SEQS                   4
#define VRL_IMAGE_FILES             19
#define ATOMPB_PAL_OFFSET           0x00
#define SORC_PAL_OFFSET             0x40
const char *seq_intro_sorc_vrl[VRL_IMAGE_FILES] = {
    "sorcwoo1.vrl",             // 0
    "sorcwoo2.vrl",
    "sorcwoo3.vrl",
    "sorcwoo4.vrl",
    "sorcwoo5.vrl",
    "sorcwoo6.vrl",             // 5
    "sorcwoo7.vrl",
    "sorcwoo8.vrl",
    "sorcwoo9.vrl",
    "sorcuhhh.vrl",
    "sorcbwo1.vrl",             // 10
    "sorcbwo2.vrl",
    "sorcbwo3.vrl",
    "sorcbwo4.vrl",
    "sorcbwo5.vrl",
    "sorcbwo6.vrl",             // 15
    "sorcbwo7.vrl",
    "sorcbwo8.vrl",
    "sorcbwo9.vrl"
                                // 19
};
/* anim1: ping pong loop 0-8 (sorcwoo1.vrl to sorcwoo9.vrl) */
/* anim2: single frame 9 (sorcuhhh.vrl) */
/* anim3: ping pong loop 10-18 (sorcbwo1.vrl to sorcbwo9.vrl) */

struct seq_anim_i anim_seq[ANIM_SEQS] = {
    /*  dur     fr      if      minf    maxf    fl,                 init_dir */
    {   120*8,  15,     0,      0,      8,      SEQANF_PINGPONG,    1}, // 0
    {   120*6,  1,      0,      0,      0,      SEQANF_OFF,         0}, // 1
    {   120*4,  1,      9,      9,      9,      0,                  0}, // 2
    {   120*8,  30,     10,     10,     18,     SEQANF_PINGPONG,    1}  // 3
};

static uint32_t atpb_init_count;

// TODO: Replace with fixed pt sin lookup table
#include <math.h>
// END TODO

/* "Second Reality" style rotozoomer because Woooooooooooo */
void atomic_playboy_zoomer(unsigned int w,unsigned int h,__segment imgseg,uint32_t count) {
    const uint32_t rt = count - atpb_init_count;
    const __segment vseg = FP_SEG(vga_state.vga_graphics_ram);
// TODO: Replace with fixed pt sin lookup table
    const double a = ((double)rt * 3.14159 * 2.0) / (timer_tick_rate * 8);
    const double sc = 1.5 + (sin(a) * 1.2);
    const uint16_t sx1 = (uint16_t)(cos(a) *  0x400 * sc);
    const uint16_t sy1 = (uint16_t)(sin(a) * -0x400 * sc);
    const uint16_t sx2 = (uint16_t)(cos(a) *  0x133 * sc);
    const uint16_t sy2 = (uint16_t)(sin(a) * -0x133 * sc);
// END TODO
    unsigned cvo = FP_OFF(vga_state.vga_graphics_ram);
    uint16_t fcx,fcy;

// make sure rotozoomer is centered on screen
    fcx = 0 - ((w / 2u / 4u) * sx1) - ((h / 2u) *  sy2);
    fcy = 0 - ((w / 2u / 4u) * sy1) - ((h / 2u) * -sx2);

// NTS: Because of VGA unchained mode X, this renders the effect in vertical columns rather than horizontal rows
    while (w >= 4) {
        vga_write_sequencer(0x02/*map mask*/,0x0F);

        // WARNING: This loop temporarily modifies DS. While DS is altered do not access anything by name in the data segment.
        //          However variables declared locally in this function are OK because they are allocated from the stack, and
        //          are referenced using [bp] which uses the stack (SS) register.
        __asm {
            mov     cx,h
            mov     es,vseg
            mov     di,cvo
            inc     cvo
            push    ds
            mov     ds,imgseg
            mov     dx,fcx          ; DX = fractional X coord
            mov     bx,fcy          ; BX = fractional Y coord

crloop:
            ; SI = (BX & 0xFF00) + (DX >> 8)
            ;   but DX/BX are the general registers with hi/lo access so
            ; SI = (BH << 8) + DH
            ; by the way on anything above a 386 that sort of optimization stuff doesn't help performance.
            ; later (Pentium Pro) processors don't like it when you alternate between DH/DL and DX because
            ; of the way the processor works internally.
            mov     ah,bh
            mov     al,dh
            mov     si,ax
            mov     al,[si]
            stosb
            add     di,79           ; stosb / add di,79  is equivalent to  mov es:[di],al / add di,80

            add     dx,sy2
            sub     bx,sx2

            loop    crloop
            pop     ds
        }

        fcx += sx1;
        fcy += sy1;
        w -= 4;
    }
}

void seq_intro() {
    uint32_t nanim_count = 0;
    uint16_t vrl_anim_interval = 0;
    uint32_t nf_count = 0,ccount = 0;
    struct vrl_image vrl_image[VRL_IMAGE_FILES];
    signed char vrl_image_dir = 0;
    int vrl_image_select = 0;
    unsigned char anim;
    int redraw = 1;
    int c;

    unsigned atpbseg; /* atomic playboy 256x256 background DOS segment value */

    /* atomic playboy background 256x256 */
    {
        struct minipng_reader *rdr;

        /* WARNING: Code assumes 16-bit large memory model! */

        if ((rdr=minipng_reader_open("atmpbrz.png")) == NULL)
            fatal("seq_intro: failed atmpbrz.png");
        if (minipng_reader_parse_head(rdr) || rdr->plte == NULL || rdr->plte_count < 64)
            fatal("seq_intro: failed atmpbrz.png");
        if (_dos_allocmem(0x1000/*paragrahs==64KB*/,&atpbseg) != 0)
            fatal("seq_intro: failed atmpbrz.png");

        {
            unsigned int i;

            for (i=0;i < 256;i++) {
                unsigned char far *imgptr = (unsigned char far*)MK_FP(atpbseg + (i * 16u/*paragraphs=256b*/),0);
                minipng_reader_read_idat(rdr,imgptr,1); /* pad byte */
                minipng_reader_read_idat(rdr,imgptr,256); /* row */
            }

            {
                const unsigned char *p = (const unsigned char*)(rdr->plte);
                vga_palette_lseek(ATOMPB_PAL_OFFSET);
                for (i=0;i < 64;i++)
                    vga_palette_write(p[(i*3)+0]>>2,p[(i*3)+1]>>2,p[(i*3)+2]>>2);
            }
        }

        minipng_reader_close(&rdr);
    }

    pal_load_to_vga(/*offset*/SORC_PAL_OFFSET,/*count*/32,"sorcwoo.pal");

    {
        unsigned int vrl_image_count = 0;
        for (vrl_image_count=0;vrl_image_count < VRL_IMAGE_FILES;vrl_image_count++) {
            if (load_vrl(&vrl_image[vrl_image_count],seq_intro_sorc_vrl[vrl_image_count]) != 0)
                fatal("seq_intro: unable to load VRL %s",seq_intro_sorc_vrl[vrl_image_count]);

                vrl_palrebase(
                    vrl_image[vrl_image_count].vrl_header,
                    vrl_image[vrl_image_count].vrl_lineoffs,
                    vrl_image[vrl_image_count].buffer+sizeof(*(vrl_image[vrl_image_count].vrl_header)),
                    SORC_PAL_OFFSET);
        }
    }

    atpb_init_count = nanim_count = ccount = counter_read();
    anim = -1; /* increment to zero in loop */

    vga_clear_npage();

    do {
        if (ccount >= nanim_count) {
            if ((++anim) >= ANIM_SEQS) break;
            vrl_anim_interval = (uint16_t)(timer_tick_rate / anim_seq[anim].frame_rate);
            vrl_image_select = anim_seq[anim].init_frame;
            vrl_image_dir = anim_seq[anim].init_dir;
            nanim_count += anim_seq[anim].duration;
            if (nanim_count < ccount) nanim_count = ccount;
            nf_count = ccount + vrl_anim_interval;
            redraw = 1;
        }
        else if (anim == 0 || anim == 3) {
            redraw = 1; /* always redraw for that super awesome rotozoomer effect :) */
        }

        if (redraw) {
            redraw = 0;

            if (anim == 0 || anim == 3)
                atomic_playboy_zoomer(320/*width*/,168/*height*/,atpbseg,ccount);
            else {
                vga_write_sequencer(0x02/*map mask*/,0xF);
                vga_rep_stosw(vga_state.vga_graphics_ram,0,((320u/4u)*168u)/2u);
            }

            if (!(anim_seq[anim].flags & SEQANF_OFF)) {
                draw_vrl1_vgax_modex(70,10,
                    vrl_image[vrl_image_select].vrl_header,
                    vrl_image[vrl_image_select].vrl_lineoffs,
                    vrl_image[vrl_image_select].buffer+sizeof(*vrl_image[vrl_image_select].vrl_header),
                    vrl_image[vrl_image_select].bufsz-sizeof(*vrl_image[vrl_image_select].vrl_header));
            }

            vga_swap_pages(); /* current <-> next */
            vga_update_disp_cur_page();
            vga_wait_for_vsync(); /* wait for vsync */
        }

        if (kbhit()) {
            c = getch();
            if (c == 27) break;
        }

        while (ccount >= nf_count) {
            if (!(anim_seq[anim].flags & SEQANF_OFF)) {
                redraw = 1;
                if (vrl_image_select >= anim_seq[anim].max_frame) {
                    if (anim_seq[anim].flags & SEQANF_PINGPONG) {
                        vrl_image_select = anim_seq[anim].max_frame - 1;
                        vrl_image_dir = -1;
                    }
                }
                else if (vrl_image_select <= anim_seq[anim].min_frame) {
                    if (anim_seq[anim].flags & SEQANF_PINGPONG) {
                        vrl_image_select = anim_seq[anim].min_frame + 1;
                        vrl_image_dir = 1;
                    }
                }
                else {
                    vrl_image_select += (int)vrl_image_dir; /* sign extend */
                }
            }

            nf_count += vrl_anim_interval;
        }

        ccount = counter_read();
    } while (1);

    /* atomic playboy image seg */
    _dos_freemem(atpbseg);
    /* VRLs */
    for (vrl_image_select=0;vrl_image_select < VRL_IMAGE_FILES;vrl_image_select++)
        free_vrl(&vrl_image[vrl_image_select]);
#undef ATOMPB_PAL_OFFSET
#undef SORC_PAL_OFFSET
#undef VRL_IMAGE_FILES
#undef ANIM_SEQS
}

/*---------------------------------------------------------------------------*/
/* main                                                                      */
/*---------------------------------------------------------------------------*/

int main() {
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

#if TARGET_MSDOS == 16
    probe_emm();            // expanded memory support
    probe_himem_sys();      // extended memory support
#endif

    init_timer_irq();
    init_vga256unchained();

    seq_intro();

    unhook_irqs();
    restore_text_mode();

    return 0;
}

