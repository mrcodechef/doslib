/* THINGS TO DO:
 *
 *      - Modularize copy-pasta'd code such as:
 *         * The "if you value your data" warning
 *         * ATAPI packet commands
 *         * EVERYTHING---This code basically works on test hardware, now it's time to clean it up
 *           modularize and refactor.
 *      - Add menu item where the user can ask this code to test whether or not the IDE controller
 *        supports 32-bit PIO properly.
 *      - Start using this code for reference and implementation of IDE emulation within DOSBox-X
 *      - Add menu items to allow the user to play with SET FEATURES command
 *      - Add submenu where the user can play with the S.M.A.R.T. ATA commands
 *      - **TEST THIS CODE ON AS MANY MACHINES AS POSSIBLE** The IDE interface is one of those
 *        hardware standards that is conceptually simple yet so manu manufacturers managed to fuck
 *        up their implementation in some way or another.
 *      - Cleanup this code, move library into idelib.c+idelib.h
 *      - Fix corner cases where we're IDE busy waiting and user commands to break free (ESC or spacebar
 *        do not break out of read/write loop, forcing the user to CTRL+ALT+DEL or press reset.
 *
 * Also don't forget:
 *   - Test programs for specific IDE chipsets (like Intel PIIX3) to demonstrate IDE DMA READ/WRITE commands
 *
 * Interesting notes:
 *   - Toshiba Satellite Pro 465CDX/2.1
 *      - Putting the hard drive to sleep seems to put the IDE controller to sleep too. IDE controller
 *        "busy" bit is stuck on when hard drive asleep. Only way out seems to be doing a "host reset"
 *        on that IDE controller. Note that NORMAL behavior is that the IDE controller remains not-busy
 *        but the device is not ready.
 *
 *      - The CD-ROM drive (or secondary IDE?) appears to ignore 32-bit I/O to port 0x170 (base_io+0).
 *        If you attempt to read a sector or identify command results using 32-bit PIO, you get only
 *        0xFFFFFFFF. Reading data only works when PIO is done as 16-bit. NORMAL behavior suggests
 *        either 32-bit PIO gets 32 bits at a time (PCI based IDE) or 32-bit PIO gets 16 bits of IDE
 *        data and 16 bits of the adjacent 16-bit I/O register due to 386/486-era ISA subdivision of
 *        32-bit I/O into two 16-bit I/O reads. Supposedly, on pre-PCI controllers, 32-bit PIO could
 *        be made to work if you tell the card in advance using the "VLB keying sequence", but I have
 *        yet to find such a card.
 *
 * Known hardware this code has trouble with (so far):
 * 
 *   - Ancient (1999-2002-ish) DVD-ROM drives. They generally work with this code but there seem to be a lot
 *     of edge cases. One DVD-ROM drive I own will not raise "drive ready" after receipt of a command it doesn't
 *     recognize. */

#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/vga/vga.h>
#include <hw/pci/pci.h>
#include <hw/dos/dos.h>
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/vga/vgagui.h>
#include <hw/vga/vgatty.h>
#include <hw/ide/idelib.h>

#if TARGET_MSDOS == 16 && (defined(__COMPACT__) || defined(__SMALL__))
  /* chop features out of the Compact memory model build to ensure all code fits inside 64KB */
#else
# define ISAPNP
#endif

#ifdef ISAPNP
#include <hw/isapnp/isapnp.h>
#include <hw/sndsb/sndsbpnp.h>
#endif

#include "testutil.h"
#include "testmbox.h"
#include "testcmui.h"
#include "testbusy.h"
#include "testpiot.h"
#include "testrvfy.h"
#include "testrdwr.h"
#include "testidnt.h"
#include "testcdej.h"
#include "testpiom.h"
#include "testtadj.h"
#include "testcdrm.h"
#include "testmumo.h"
#include "testmisc.h"
#include "test.h"

#include "testnop.h"
#include "testpwr.h"

/* TODO: add to DOS library */
unsigned short			smartdrv_version = 0xFFFF;
int				smartdrv_fd = -1;

int smartdrv_close() {
	if (smartdrv_fd >= 0) {
		close(smartdrv_fd);
		smartdrv_fd = -1;
	}

	return 0;
}

int smartdrv_flush() {
	if (smartdrv_version == 0xFFFF || smartdrv_version == 0)
		return 0;

	if (smartdrv_version >= 0x400) { /* SMARTDRV 4.xx and later */
#if TARGET_MSDOS == 32
		__asm {
			push	eax
			push	ebx
			mov	eax,0x4A10		; SMARTDRV
			mov	ebx,0x0001		; FLUSH BUFFERS (COMMIT CACHE)
			int	0x2F
			pop	ebx
			pop	eax
		}
#else
		__asm {
			push	ax
			push	bx
			mov	ax,0x4A10		; SMARTDRV
			mov	bx,0x0001		; FLUSH BUFFERS (COMMIT CACHE)
			int	0x2F
			pop	bx
			pop	ax
		}
#endif
	}
	else if (smartdrv_version >= 0x300 && smartdrv_version <= 0x3FF) { /* SMARTDRV 3.xx */
		char buffer[0x1];
#if TARGET_MSDOS == 32
#else
		char far *bptr = (char far*)buffer;
#endif
		int rd=0;

		if (smartdrv_fd < 0)
			return 0;

		buffer[0] = 0; /* flush cache */
#if TARGET_MSDOS == 32
		/* FIXME: We do not yet have a 32-bit protected mode version.
		 *        DOS extenders do not appear to translate AX=0x4403 properly */
#else
		__asm {
			push	ax
			push	bx
			push	cx
			push	dx
			push	ds
			mov	ax,0x4403		; IOCTL SMARTAAR CACHE CONTROL
			mov	bx,smartdrv_fd
			mov	cx,0x1			; 0x01 bytes
			lds	dx,word ptr [bptr]
			int	0x21
			jc	err1
			mov	rd,ax			; CF=0, AX=bytes written
err1:			pop	ds
			pop	dx
			pop	cx
			pop	bx
			pop	ax
		}
#endif

		if (rd == 0)
			return 0;
	}

	return 1;
}

int smartdrv_detect() {
	unsigned int rvax=0,rvbp=0;

	if (smartdrv_version == 0xFFFF) {
		/* Is Microsoft SMARTDRV 4.x or equivalent disk cache present? */

#if TARGET_MSDOS == 32
		__asm {
			push	eax
			push	ebx
			push	ecx
			push	edx
			push	esi
			push	edi
			push	ebp
			push	ds
			mov	eax,0x4A10		; SMARTDRV 4.xx INSTALLATION CHECK AND HIT RATIOS
			mov	ebx,0
			mov	ecx,0xEBAB		; "BABE" backwards
			xor	ebp,ebp
			int	0x2F			; multiplex (hope your DOS extender supports it properly!)
			pop	ds
			mov	ebx,ebp			; copy EBP to EBX. Watcom C uses EBP to refer to the stack!
			pop	ebp
			mov	rvax,eax		; we only care about EAX and EBP(now EBX)
			mov	rvbp,ebx
			pop	edi
			pop	esi
			pop	edx
			pop	ecx
			pop	ebx
			pop	eax
		}
#else
		__asm {
			push	ax
			push	bx
			push	cx
			push	dx
			push	si
			push	di
			push	bp
			push	ds
			mov	ax,0x4A10		; SMARTDRV 4.xx INSTALLATION CHECK AND HIT RATIOS
			mov	bx,0
			mov	cx,0xEBAB		; "BABE" backwards
			xor	bp,bp
			int	0x2F			; multiplex
			pop	ds
			mov	bx,bp			; copy BP to BX. Watcom C uses BP to refer to the stack!
			pop	bp
			mov	rvax,ax			; we only care about EAX and EBP(now EBX)
			mov	rvbp,bx
			pop	di
			pop	si
			pop	dx
			pop	cx
			pop	bx
			pop	ax
		}
#endif

		if ((rvax&0xFFFF) == 0xBABE && (rvbp&0xFFFF) >= 0x400 && (rvbp&0xFFFF) <= 0x5FF) {
			/* yup. SMARTDRV 4.xx! */
			smartdrv_version = (rvbp&0xFF00) + (((rvbp>>4)&0xF) * 10) + (rvbp&0xF); /* convert BCD to decimal */
		}

		if (smartdrv_version == 0xFFFF) {
			char buffer[0x28];
#if TARGET_MSDOS == 32
#else
			char far *bptr = (char far*)buffer;
#endif
			int rd=0;

			memset(buffer,0,sizeof(buffer));

			/* check for SMARTDRV 3.xx */
			smartdrv_fd = open("SMARTAAR",O_RDONLY);
			if (smartdrv_fd >= 0) {
				/* FIXME: The DOS library should provide a common function to do IOCTL read/write character "control channel" functions */
#if TARGET_MSDOS == 32
				/* FIXME: We do not yet have a 32-bit protected mode version.
				 *        DOS extenders do not appear to translate AX=0x4402 properly */
#else
				__asm {
					push	ax
					push	bx
					push	cx
					push	dx
					push	ds
					mov	ax,0x4402		; IOCTL SMARTAAR GET CACHE STATUS
					mov	bx,smartdrv_fd
					mov	cx,0x28			; 0x28 bytes
					lds	dx,word ptr [bptr]
					int	0x21
					jc	err1
					mov	rd,ax			; CF=0, AX=bytes read
err1:					pop	ds
					pop	dx
					pop	cx
					pop	bx
					pop	ax
				}
#endif

				/* NTS: Despite reading back 0x28 bytes of data, this IOCTL
				 *      returns AX=1 for some reason. */
				if (rd > 0 && rd <= 0x28) {
					if (buffer[0] == 1/*write through flag*/ && buffer[15] == 3/*major version number*/) { /* SMARTDRV 3.xx */
						smartdrv_version = ((unsigned short)buffer[15] << 8) + ((unsigned short)buffer[14]);
					}
					else {
						close(smartdrv_fd);
					}
				}
			}
		}

		/* didn't find anything. then no SMARTDRV */
		if (smartdrv_version == 0xFFFF)
			smartdrv_version = 0;
	}

	return (smartdrv_version != 0);
}
/* END TODO */

unsigned char			cdrom_read_mode = 12;
unsigned char			pio_width_warning = 1;
unsigned char			big_scary_write_test_warning = 1;

char				tmp[1024];
uint16_t			ide_info[256];

#if TARGET_MSDOS == 32
unsigned char			cdrom_sector[512U*256U];/* ~128KB, enough for 64 CD-ROM sector or 256 512-byte sectors */
#else
# if defined(__LARGE__) || defined(__COMPACT__)
unsigned char			cdrom_sector[512U*16U];	/* ~8KB, enough for 4 CD-ROM sector or 16 512-byte sectors */
# else
unsigned char			cdrom_sector[512U*64U];	/* ~32KB, enough for 16 CD-ROM sector or 64 512-byte sectors */
# endif
#endif

/*-----------------------------------------------------------------*/

static const char *drive_main_menustrings[] = {
	"Show IDE register taskfile",		/* 0 */
	"Identify (ATA)",
	"Identify packet (ATAPI)",
	"Power states >>",
	"PIO mode >>",				/* 4 */ /* rewritten */
	"No-op",				/* 5 */
	"Tweaks and adjustments >>",
	"CD-ROM eject/load >>",
	"CD-ROM reading >>",
	"Multiple mode >>",
	"Read/Write tests >>",			/* 10 */
	"Miscellaneous >>"
};

void do_ide_controller_drive(struct ide_controller *ide,unsigned char which) {
	struct menuboxbounds mbox;
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	int select=-1;
	char redraw=1;
	int c;

	/* UI element vars */
	menuboxbounds_set_def_list(&mbox,/*ofsx=*/4,/*ofsy=*/7,/*cols=*/1);
	menuboxbounds_set_item_strings_arraylen(&mbox,drive_main_menustrings);

	/* most of the commands assume a ready controller. if it's stuck,
	 * we'd rather the user have a visual indication that it's stuck that way */
	c = do_ide_controller_user_wait_busy_controller(ide);
	if (c != 0) return;

	/* select the drive we want */
	idelib_controller_drive_select(ide,which,/*head*/0,IDELIB_DRIVE_SELECT_MODE_CHS);

	/* in case the IDE controller is busy for that time */
	c = do_ide_controller_user_wait_busy_controller(ide);
	if (c != 0) return;

	/* read back: did the drive select take effect? if not, it might not be there. another common sign is the head/drive select reads back 0xFF */
	c = do_ide_controller_drive_check_select(ide,which);
	if (c < 0) return;

	/* it might be a CD-ROM drive, which in some cases might not raise the Drive Ready bit */
	do_ide_controller_atapi_device_check_post_host_reset(ide);

	/* wait for the drive to indicate readiness */
	/* NTS: If the drive never becomes ready even despite our reset hacks, there's a strong
	 *      possibility that the device doesn't exist. This can happen for example if there
	 *      is a master attached but no slave. */
	c = do_ide_controller_user_wait_drive_ready(ide);
	if (c < 0) return;

	/* for completeness, clear pending IRQ */
	idelib_controller_ack_irq(ide);

	while (1) {
		if (backredraw) {
			vga = vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_height;y++) {
				for (x=0;x < vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        IDE controller ");
			sprintf(tmp,"@%X",ide->base_io);
			vga_write(tmp);
			if (ide->alt_io != 0) {
				sprintf(tmp," alt %X",ide->alt_io);
				vga_write(tmp);
			}
			if (ide->irq >= 0) {
				sprintf(tmp," IRQ %d",ide->irq);
				vga_write(tmp);
			}
			vga_write(which ? " Slave" : " Master");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');

			vga_write_color(0xC);
			vga_write("WARNING: This code talks directly to your hard disk controller.");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
			vga_write_color(0xC);
			vga_write("         If you value the data on your hard drive do not run this program.");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			/* update a string or two: PIO mode */
			if (ide->pio_width == 33)
				drive_main_menustrings[4] = "PIO mode (currently: 32-bit VLB) >>";
			else if (ide->pio_width == 32)
				drive_main_menustrings[4] = "PIO mode (currently: 32-bit) >>";
			else
				drive_main_menustrings[4] = "PIO mode (currently: 16-bit) >>";

			vga_moveto(mbox.ofsx,mbox.ofsy - 2);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Back to IDE controller main menu");
			while (vga_pos_x < (mbox.width+mbox.ofsx) && vga_pos_x != 0) vga_writec(' ');

			menuboxbound_redraw(&mbox,select);
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1)
				break;

			switch (select) {
				case 0: /* show IDE register taskfile */
					do_common_show_ide_taskfile(ide,which);
					redraw = backredraw = 1;
					break;
				case 1: /*Identify*/
				case 2: /*Identify packet*/
					do_drive_identify_device_test(ide,which,select == 2 ? 0xA1/*Identify packet*/ : 0xEC/*Identify*/);
					redraw = backredraw = 1;
					break;
				case 3: /* power states */
					do_drive_power_states_test(ide,which);
					redraw = backredraw = 1;
					break;
				case 4: /* PIO mode */
					do_drive_pio_mode(ide,which);
					redraw = backredraw = 1;
					break;
				case 5: /* NOP */
					do_ide_controller_drive_nop_test(ide,which);
					break;
				case 6: /* Tweaks and adjustments */
					do_drive_tweaks_and_adjustments(ide,which);
					redraw = backredraw = 1;
					break;
				case 7: /* CD-ROM start/stop/eject/load */
					do_drive_cdrom_startstop_test(ide,which);
					redraw = backredraw = 1;
					break;
				case 8: /* CD-ROM reading */
					do_drive_cdrom_reading(ide,which);
					redraw = backredraw = 1;
					break;
				case 9: /* multiple mode */
					do_drive_multiple_mode(ide,which);
					redraw = backredraw = 1;
					break;
				case 10: /* read/write tests */
					do_drive_readwrite_tests(ide,which);
					redraw = backredraw = 1;
					break;
				case 11: /* misc */
					do_drive_misc_tests(ide,which);
					redraw = backredraw = 1;
					break;
			};
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = mbox.item_max;

			redraw = 1;
		}
		else if (c == 0x4B00) { /* left */
			redraw = 1;
		}
		else if (c == 0x4D00) { /* right */
			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > mbox.item_max)
				select = -1;

			redraw = 1;
		}
	}
}

static void (interrupt *my_ide_old_irq)() = NULL;
static struct ide_controller *my_ide_irq_ide = NULL;
static unsigned long ide_irq_counter = 0;
static int my_ide_irq_number = -1;

static void interrupt my_ide_irq() {
	int i;

	/* we CANNOT use sprintf() here. sprintf() doesn't work to well from within an interrupt handler,
	 * and can cause crashes in 16-bit realmode builds. */
	i = vga_width*(vga_height-1);
	vga_alpha_ram[i++] = 0x1F00 | 'I';
	vga_alpha_ram[i++] = 0x1F00 | 'R';
	vga_alpha_ram[i++] = 0x1F00 | 'Q';
	vga_alpha_ram[i++] = 0x1F00 | ':';
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((ide_irq_counter / 100000UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((ide_irq_counter /  10000UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((ide_irq_counter /   1000UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((ide_irq_counter /    100UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((ide_irq_counter /     10UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((ide_irq_counter /       1L) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ' ';

	ide_irq_counter++;
	if (my_ide_irq_ide != NULL) {
		my_ide_irq_ide->irq_fired++;

		/* ack PIC */
		if (my_ide_irq_ide->irq >= 8) p8259_OCW2(8,P8259_OCW2_NON_SPECIFIC_EOI);
		p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);
	}
}

void do_ide_controller_hook_irq(struct ide_controller *ide) {
	if (my_ide_irq_number >= 0 || my_ide_old_irq != NULL || ide->irq < 0)
		return;

	/* let the IRQ know what IDE controller */
	my_ide_irq_ide = ide;

	/* enable on IDE controller */
	p8259_mask(ide->irq);
	idelib_enable_interrupt(ide,1);

	/* hook IRQ */
	my_ide_old_irq = _dos_getvect(irq2int(ide->irq));
	_dos_setvect(irq2int(ide->irq),my_ide_irq);
	my_ide_irq_number = ide->irq;

	/* enable at PIC */
	p8259_unmask(ide->irq);
}

void do_ide_controller_unhook_irq(struct ide_controller *ide) {
	if (my_ide_irq_number < 0 || my_ide_old_irq == NULL || ide->irq < 0)
		return;

	/* disable on IDE controller, then mask at PIC */
	idelib_enable_interrupt(ide,0);
	p8259_mask(ide->irq);

	/* restore the original vector */
	_dos_setvect(irq2int(ide->irq),my_ide_old_irq);
	my_ide_irq_number = -1;
	my_ide_old_irq = NULL;
}

void do_ide_controller_enable_irq(struct ide_controller *ide,unsigned char en) {
	if (!en || ide->irq < 0 || ide->irq != my_ide_irq_number)
		do_ide_controller_unhook_irq(ide);
	if (en && ide->irq >= 0)
		do_ide_controller_hook_irq(ide);
}

void do_ide_controller(struct ide_controller *ide) {
	struct vga_msg_box vgabox;
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int select=-1;
	int c;

	/* we're taking a drive, possibly out from MS-DOS.
	 * make sure SMARTDRV flushes the cache so that it does not attempt to
	 * write to the disk while we're controlling the IDE controller */
	if (smartdrv_version != 0) {
		for (c=0;c < 4;c++) smartdrv_flush();
	}

	/* most of the commands assume a ready controller. if it's stuck,
	 * we'd rather the user have a visual indication that it's stuck that way */
	c = do_ide_controller_user_wait_busy_controller(ide);
	if (c < 0) return;

	/* if the IDE struct says to use interrupts, then do it */
	do_ide_controller_enable_irq(ide,ide->flags.io_irq_enable);

	while (1) {
		if (backredraw) {
			vga = vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_height;y++) {
				for (x=0;x < vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        IDE controller ");
			sprintf(tmp,"@%X",ide->base_io);
			vga_write(tmp);
			if (ide->alt_io != 0) {
				sprintf(tmp," alt %X",ide->alt_io);
				vga_write(tmp);
			}
			if (ide->irq >= 0) {
				sprintf(tmp," IRQ %d",ide->irq);
				vga_write(tmp);
			}
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');

			vga_write_color(0xC);
			vga_write("WARNING: This code talks directly to your hard disk controller.");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
			vga_write_color(0xC);
			vga_write("         If you value the data on your hard drive do not run this program.");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Main menu");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 0) ? 0x70 : 0x0F);
			vga_write("Host Reset");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 1) ? 0x70 : 0x0F);
			vga_write("Tinker with Master device");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 2) ? 0x70 : 0x0F);
			vga_write("Tinker with Slave device");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 3) ? 0x70 : 0x0F);
			vga_write("Currently using ");
			vga_write(ide->flags.io_irq_enable ? "IRQ" : "polling");
			vga_write(", switch to ");
			vga_write((!ide->flags.io_irq_enable) ? "IRQ" : "polling");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select == 0) { /* host reset */
				if (ide->alt_io != 0) {
					vga_msg_box_create(&vgabox,"Host reset in progress",0,0);

					idelib_device_control_set_reset(ide,1);
					t8254_wait(t8254_us2ticks(1000000));
					idelib_device_control_set_reset(ide,0);

					vga_msg_box_destroy(&vgabox);

					/* now wait for not busy */
					do_ide_controller_user_wait_busy_controller(ide);
				}
			}
			else if (select == 1) {
				do_ide_controller_drive(ide,0/*master*/);
				redraw = backredraw = 1;
			}
			else if (select == 2) {
				do_ide_controller_drive(ide,1/*slave*/);
				redraw = backredraw = 1;
			}
			else if (select == 3) {
				if (ide->irq >= 0)
					ide->flags.io_irq_enable = !ide->flags.io_irq_enable;
				else
					ide->flags.io_irq_enable = 0;

				do_ide_controller_enable_irq(ide,ide->flags.io_irq_enable);
				redraw = backredraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = 3;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > 3)
				select = -1;

			redraw = 1;
		}
	}

	do_ide_controller_enable_irq(ide,0);
	idelib_enable_interrupt(ide,1); /* NTS: Most BIOSes know to unmask the IRQ at the PIC, but there might be some
					        idiot BIOSes who don't clear the nIEN bit in the device control when
						executing INT 13h, so it's probably best to do it for them. */
}

void do_main_menu() {
	char redraw=1;
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	struct ide_controller *ide;
	unsigned int x,y,i;
	int select=-1;
	int c;

	while (1) {
		if (backredraw) {
			vga = vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_height;y++) {
				for (x=0;x < vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        IDE controller test program");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');

			vga_write_color(0xC);
			vga_write("WARNING: This code talks directly to your hard disk controller.");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
			vga_write_color(0xC);
			vga_write("         If you value the data on your hard drive do not run this program.");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Exit program");
			y++;

			for (i=0;i < MAX_IDE_CONTROLLER;i++) {
				ide = idelib_get_controller(i);
				if (ide != NULL) {
					vga_moveto(8,y++);
					vga_write_color((select == (int)i) ? 0x70 : 0x0F);

					sprintf(tmp,"Controller @ %04X",ide->base_io);
					vga_write(tmp);

					if (ide->alt_io != 0) {
						sprintf(tmp," alt %04X",ide->alt_io);
						vga_write(tmp);
					}

					if (ide->irq >= 0) {
						sprintf(tmp," IRQ %2d",ide->irq);
						vga_write(tmp);
					}
				}
			}
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select >= 0 && select < MAX_IDE_CONTROLLER) {
				ide = idelib_get_controller(select);
				if (ide != NULL) do_ide_controller(ide);
				backredraw = redraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (select <= -1)
				select = MAX_IDE_CONTROLLER - 1;
			else
				select--;

			while (select >= 0 && idelib_get_controller(select) == NULL)
				select--;

			redraw = 1;
		}
		else if (c == 0x5000) {
			select++;
			while (select >= 0 && select < MAX_IDE_CONTROLLER && idelib_get_controller(select) == NULL)
				select++;
			if (select >= (MAX_IDE_CONTROLLER-1))
				select = -1;

			redraw = 1;
		}
	}
}

int main(int argc,char **argv) {
	struct ide_controller *idectrl;
	struct ide_controller *newide;
	int do_pause=0;
	int i;

	printf("IDE ATA/ATAPI test program\n");

	if (smartdrv_detect()) {
		printf("WARNING: SMARTDRV %u.%02u or equivalent disk cache detected!\n",smartdrv_version>>8,smartdrv_version&0xFF);
		printf("         Running this program with SMARTDRV enabled is NOT RECOMMENDED,\n");
		printf("         especially when using the snapshot functions!\n");
		printf("         If you choose to test anyway, this program will attempt to flush\n");
		printf("         the disk cache as much as possible to avoid conflict.\n");
		do_pause=1;
	}

	/* we take a GUI-based approach (kind of) */
	if (!probe_vga()) {
		printf("Cannot init VGA\n");
		return 1;
	}
	/* the IDE code has some timing requirements and we'll use the 8254 to do it */
	/* I bet that by the time motherboard manufacturers stop implementing the 8254 the IDE port will be long gone too */
	if (!probe_8254()) {
		printf("8254 chip not detected\n");
		return 1;
	}
	/* interrupt controller */
	if (!probe_8259()) {
		printf("8259 chip not detected\n");
		return 1;
	}
	if (!init_idelib()) {
		printf("Cannot init IDE lib\n");
		return 1;
	}
	if (pci_probe(-1/*default preference*/) != PCI_CFG_NONE) {
		printf("PCI bus detected.\n");
	}
#ifdef ISAPNP
	if (!init_isa_pnp_bios()) {
		printf("Cannot init ISA PnP\n");
	}
	if (find_isa_pnp_bios()) {
		printf("ISA PnP BIOS detected\n");
		/* TODO */
	}
#endif

	printf("Probing standard IDE ports...\n");
	for (i=0;(idectrl = (struct ide_controller*)idelib_get_standard_isa_port(i)) != NULL;i++) {
		printf("   %3X/%3X IRQ %d: ",idectrl->base_io,idectrl->alt_io,idectrl->irq); fflush(stdout);

		if ((newide = idelib_probe(idectrl)) != NULL) {
			printf("FOUND: alt=%X irq=%d\n",newide->alt_io,newide->irq);
		}
		else {
			printf("\x0D                             \x0D"); fflush(stdout);
		}
	}

	if (do_pause) {
		printf("Hit ENTER to continue, ESC to cancel\n");
		i = wait_for_enter_or_escape();
		if (i == 27) {
			smartdrv_close();
			free_idelib();
			return 0;
		}
	}

	if (int10_getmode() != 3) {
		int10_setmode(3);
		update_state_from_vga();
	}

	smartdrv_close();
	do_main_menu();
	free_idelib();
	return 0;
}

