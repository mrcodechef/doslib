
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

void vga_clear_npage() {
    unsigned int dof;

    vga_write_sequencer(0x02/*map mask*/,0xF);
    for (dof=0;dof < 0x4000;dof++) vga_state.vga_graphics_ram[dof] = 0;
}

void init_vga256unchained() {
    vga_cur_page=VGA_PAGE_FIRST;
    vga_next_page=VGA_PAGE_SECOND;

    int10_setmode(19);
    update_state_from_vga();
    vga_enable_256color_modex(); // VGA mode X (unchained)
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

#define VRL_IMAGE_FILES             19
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

void seq_intro() {
    uint32_t nf_count,ccount;
    uint16_t vrl_anim_interval;
    struct vrl_image vrl_image[VRL_IMAGE_FILES];
    int vrl_image_select = 0;
    int redraw = 1;
    int c;

    pal_load_to_vga(/*offset*/0,/*count*/32,"sorcwoo.pal");

    {
        unsigned int vrl_image_count = 0;
        for (vrl_image_count=0;vrl_image_count < VRL_IMAGE_FILES;vrl_image_count++) {
            if (load_vrl(&vrl_image[vrl_image_count],seq_intro_sorc_vrl[vrl_image_count]) != 0)
                fatal("seq_intro: unable to load VRL %s",seq_intro_sorc_vrl[vrl_image_count]);
        }
    }

    /* anim1 */
    vrl_anim_interval = (uint16_t)(timer_tick_rate / 30u);
    vrl_image_select = 0;

    nf_count = counter_read() + vrl_anim_interval;

    do {
        if (redraw) {
            redraw = 0;

            vga_clear_npage();

            draw_vrl1_vgax_modex(0,0,
                vrl_image[vrl_image_select].vrl_header,
                vrl_image[vrl_image_select].vrl_lineoffs,
                vrl_image[vrl_image_select].buffer+sizeof(*vrl_image[vrl_image_select].vrl_header),
                vrl_image[vrl_image_select].bufsz-sizeof(*vrl_image[vrl_image_select].vrl_header));

            vga_swap_pages(); /* current <-> next */
            vga_update_disp_cur_page();
            vga_wait_for_vsync(); /* wait for vsync */
        }

        if (kbhit()) {
            c = getch();
            if (c == 27) break;
        }

        ccount = counter_read();
        if (ccount >= nf_count) {
            unsigned int stp = ((ccount - nf_count) / vrl_anim_interval) + 1u;

            if (stp > 16) stp = 16;

            redraw = 1;
            do { /* assume stp != 0 */
                nf_count += vrl_anim_interval;
                if ((++vrl_image_select) >= VRL_IMAGE_FILES)
                    vrl_image_select = 0;
            } while (--stp != 0u);

            if (nf_count < ccount) nf_count = ccount;
        }
    } while (1);

    for (vrl_image_select=0;vrl_image_select < VRL_IMAGE_FILES;vrl_image_select++)
        free_vrl(&vrl_image[vrl_image_select]);
#undef VRL_IMAGE_FILES
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

