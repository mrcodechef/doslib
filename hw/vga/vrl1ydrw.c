
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <dos.h>

#include <hw/cpu/cpu.h>
#include <hw/dos/dos.h>
#include <hw/vga/vga.h>
#include <hw/vga/vrl.h>

static inline void draw_vrl_modex_stripystretch(unsigned char far *draw,unsigned char *s,unsigned int ystretch/*10.6 fixed pt*/) {
	unsigned char run,skip,b,fy=0;

	do {
		run = *s++;
		if (run == 0xFF) break;
		skip = *s++;
		while (skip > 0) {
			while (fy < (1 << 6)) {
				draw += vga_stride;
				fy += ystretch;
			}
			fy -= 1 << 6;
			skip--;
		}

		if (run & 0x80) {
			b = *s++;
			while (run > 0x80) {
				while (fy < (1 << 6)) {
					*draw = b;
					draw += vga_stride;
					fy += ystretch;
				}
				fy -= 1 << 6;
				run--;
			}
		}
		else {
			while (run > 0) {
				while (fy < (1 << 6)) {
					*draw = *s;
					draw += vga_stride;
					fy += ystretch;
				}
				fy -= 1 << 6;
				run--;
				s++;
			}
		}
	} while (1);
}

void draw_vrl_modexystretch(unsigned int x,unsigned int y,unsigned int xstretch/*1/64 scale 10.6 fixed pt*/,unsigned int ystretch/*1/6 scale 10.6*/,struct vrl1_vgax_header *hdr,vrl1_vgax_offset_t *lineoffs/*array hdr->width long*/,unsigned char *data,unsigned int datasz) {
	unsigned int vram_offset = (y * vga_stride) + (x >> 2),fx=0;
	unsigned char vga_plane = (x & 3);
	unsigned int limit = vga_stride;
	unsigned char far *draw;
	unsigned char *s;

	if (limit > hdr->width) limit = hdr->width;

	/* draw one by one */
	do {
		draw = vga_graphics_ram + vram_offset;
		vga_write_sequencer(0x02/*map mask*/,1 << vga_plane);
		{
			unsigned int x = fx >> 6;
			if (x >= hdr->width) break;
			s = data + lineoffs[x];
		}
		draw_vrl_modex_stripystretch(draw,s,ystretch);

		/* end of a vertical strip. next line? */
		fx += xstretch;
		if ((++vga_plane) == 4) {
			if (--limit == 0) break;
			vram_offset++;
			vga_plane = 0;
		}
	} while(1);
}

