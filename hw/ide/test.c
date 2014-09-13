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
#include "test.h"

#include "testnop.h"
#include "testpwr.h"

unsigned char			cdrom_read_mode = 12;
unsigned char			pio_width_warning = 1;
unsigned char			big_scary_write_test_warning = 1;

char				tmp[1024];
uint16_t			ide_info[256];

#if TARGET_MSDOS == 32
unsigned char			cdrom_sector[512U*120U];/* ~60KB, enough for 30 CD-ROM sector or 120 512-byte sectors */
#else
# if defined(__LARGE__) || defined(__COMPACT__)
unsigned char			cdrom_sector[512U*16U];	/* ~8KB, enough for 4 CD-ROM sector or 16 512-byte sectors */
# else
unsigned char			cdrom_sector[512U*64U];	/* ~32KB, enough for 16 CD-ROM sector or 64 512-byte sectors */
# endif
#endif

/*-----------------------------------------------------------------*/

enum {
	DRIVE_RW_MODE_CHS=0,
	DRIVE_RW_MODE_CHSMULTIPLE,
	DRIVE_RW_MODE_LBA,
	DRIVE_RW_MODE_LBAMULTIPLE,
	DRIVE_RW_MODE_LBA48,
	DRIVE_RW_MODE_LBA48_MULTIPLE,

	DRIVE_RW_MODE_MAX
};

static const char *drive_readwrite_test_modes[] = {
	"Mode: C/H/S",
	"Mode: C/H/S MULTIPLE",
	"Mode: LBA",
	"Mode: LBA MULTIPLE",
	"Mode: LBA48",
	"Mode: LBA48 MULTIPLE"
};

struct drive_rw_test_info {
	unsigned short int	num_cylinder;		/* likely 0-16383 */
	unsigned short int	num_head,num_sector;	/* likely 0-15 and 0-63 */
	unsigned short int	cylinder;
	unsigned short int	head,sector;
	unsigned short int	multiple_sectors;	/* multiple mode sector count */
	unsigned short int	max_multiple_sectors;
	unsigned short int	read_sectors;		/* how many to read per command */
	uint64_t		max_lba;
	uint64_t		lba;
	unsigned char		mode;			/* DRIVE_RW_MODE_* */
	unsigned int		can_do_lba:1;
	unsigned int		can_do_lba48:1;
	unsigned int		can_do_multiple:1;
};

static struct drive_rw_test_info drive_rw_test_nfo;

static char drive_readwrite_test_geo[128];
static char drive_readwrite_test_chs[128];
static char drive_readwrite_test_numsec[128];
static char drive_readwrite_test_mult[128];

int drive_rw_test_mode_supported(struct drive_rw_test_info *nfo) {
	if (!drive_rw_test_nfo.can_do_multiple &&
		(nfo->mode == DRIVE_RW_MODE_CHSMULTIPLE ||
		 nfo->mode == DRIVE_RW_MODE_LBAMULTIPLE ||
		 nfo->mode == DRIVE_RW_MODE_LBA48_MULTIPLE))
		return 0;

	if (!drive_rw_test_nfo.can_do_lba &&
		(nfo->mode == DRIVE_RW_MODE_LBA ||
		 nfo->mode == DRIVE_RW_MODE_LBAMULTIPLE ||
		 nfo->mode == DRIVE_RW_MODE_LBA48 ||
		 nfo->mode == DRIVE_RW_MODE_LBA48_MULTIPLE))
		return 0;

	if (!drive_rw_test_nfo.can_do_lba48 &&
		(nfo->mode == DRIVE_RW_MODE_LBA48 ||
		 nfo->mode == DRIVE_RW_MODE_LBA48_MULTIPLE))
		return 0;

	return 1;
}

static void do_hdd_drive_read_test(struct ide_controller *ide,unsigned char which,unsigned char continuous,struct drive_rw_test_info *nfo) {
	unsigned char user_esc = 0;
	struct vga_msg_box vgabox;
	struct ide_taskfile *tsk;
	unsigned long tlen_sect;
	unsigned int cleared=0;
	unsigned int x,y,i;
	unsigned long tlen;
	int c;

	if (nfo->read_sectors == 0) return;

	if (nfo->mode == DRIVE_RW_MODE_CHSMULTIPLE || nfo->mode == DRIVE_RW_MODE_LBAMULTIPLE || nfo->mode == DRIVE_RW_MODE_LBA48_MULTIPLE) {
		if (nfo->multiple_sectors > ((unsigned long)sizeof(cdrom_sector) / 512UL))
			return;
	}

	idelib_controller_reset_irq_counter(ide); /* IRQ will fire after command completion */
	tsk = idelib_controller_get_taskfile(ide,-1/*selected drive*/);

again:	/* jump point: send execution back here for another sector */
	if (do_ide_controller_user_wait_busy_controller(ide) != 0 || do_ide_controller_user_wait_drive_ready(ide) < 0)
		return;
	idelib_controller_ack_irq(ide); /* <- make sure to ack IRQ */

	tlen_sect = nfo->read_sectors;

	/* C/H/S continuous: limit reads to within track, don't cross. we can't assume the drive will do that. */
	if (continuous && (nfo->mode == DRIVE_RW_MODE_CHS || nfo->mode == DRIVE_RW_MODE_CHSMULTIPLE)) {
		if ((nfo->sector + tlen_sect) > nfo->num_sector)
			tlen_sect = (nfo->num_sector + 1 - nfo->sector);
	}

	if (tlen_sect > ((unsigned long)sizeof(cdrom_sector) / 512UL))
		tlen_sect = ((unsigned long)sizeof(cdrom_sector) / 512UL);
	tlen = tlen_sect * 512UL;

	if (nfo->mode == DRIVE_RW_MODE_LBA48 || nfo->mode == DRIVE_RW_MODE_LBA48_MULTIPLE) {
		tsk->sector_count = tlen_sect;
		tsk->lba0_3 = nfo->lba & 0xFF;
		tsk->lba1_4 = (nfo->lba >> 8) & 0xFF;
		tsk->lba2_5 = (nfo->lba >> 16) & 0xFF;
		tsk->lba0_3 |= ((nfo->lba >> 24) & 0xFF) << 8;
		tsk->lba1_4 |= ((nfo->lba >> 32) & 0xFF) << 8;
		tsk->lba2_5 |= ((nfo->lba >> 40) & 0xFF) << 8;
		tsk->head_select = (which << 4) | 0x40;

		if (nfo->mode == DRIVE_RW_MODE_LBA48)
			tsk->command = 0x24; /* READ SECTORS EXT */
		else if (nfo->mode == DRIVE_RW_MODE_LBA48_MULTIPLE)
			tsk->command = 0x29; /* READ MULTIPLE EXT */

		if (idelib_controller_apply_taskfile(ide,0xFC/*base_io+2-7*/,IDELIB_TASKFILE_LBA48_UPDATE|IDELIB_TASKFILE_LBA48/*set LBA48*/) < 0)
			return;
	}
	else {
		if (tsk->sector_count > 256) return;
		tsk->sector_count = (tlen_sect&0xFF);
		if (nfo->mode == DRIVE_RW_MODE_CHS || nfo->mode == DRIVE_RW_MODE_CHSMULTIPLE) {
			tsk->chs_sector = nfo->sector;
			tsk->chs_cylinder_low = nfo->cylinder & 0xFF;
			tsk->chs_cylinder_high = (nfo->cylinder >> 8) & 0xFF;
			tsk->head_select = (nfo->head & 0xF) | (which << 4) | 0xA0;
		}
		else if (nfo->mode == DRIVE_RW_MODE_LBA || nfo->mode == DRIVE_RW_MODE_LBAMULTIPLE) {
			tsk->lba0_3 = nfo->lba & 0xFF;
			tsk->lba1_4 = (nfo->lba >> 8) & 0xFF;
			tsk->lba2_5 = (nfo->lba >> 16) & 0xFF;
			tsk->head_select = ((nfo->lba >> 24) & 0xF) | (which << 4) | 0xE0;
		}

		if (nfo->mode == DRIVE_RW_MODE_CHS || nfo->mode == DRIVE_RW_MODE_LBA)
			tsk->command = 0x20; /* READ SECTORS */
		else if (nfo->mode == DRIVE_RW_MODE_CHSMULTIPLE || nfo->mode == DRIVE_RW_MODE_LBAMULTIPLE)
			tsk->command = 0xC4; /* READ MULTIPLE */

		if (idelib_controller_apply_taskfile(ide,0xFC/*base_io+2-7*/,IDELIB_TASKFILE_LBA48_UPDATE/*clear LBA48*/) < 0)
			return;
	}

	if (ide->flags.io_irq_enable) { /* NOW we wait for the IRQ */
		if (do_ide_controller_user_wait_irq(ide,1) < 0)
			return;
		idelib_controller_ack_irq(ide); /* <- or else it won't fire again */
	}

	if (do_ide_controller_user_wait_busy_controller(ide) != 0 || do_ide_controller_user_wait_drive_ready(ide) < 0)
		return;

	if (!idelib_controller_is_error(ide)) { /* OK. success. now read the data */
		unsigned int ret_len = 0,drq_len;

		memset(cdrom_sector,0,tlen);
		while (ret_len < tlen) {
			if (idelib_controller_is_error(ide)) {
				vga_msg_box_create(&vgabox,"Error",0,0);
				wait_for_enter_or_escape();
				vga_msg_box_destroy(&vgabox);
				break;
			}

			if (ide->flags.io_irq_enable) { /* NOW we wait for the IRQ */
				if (do_ide_controller_user_wait_irq(ide,1) < 0) {
					user_esc = 1;
					break;
				}
				idelib_controller_reset_irq_counter(ide); /* IRQ will fire after command completion */
				idelib_controller_ack_irq(ide); /* <- or else it won't fire again */
			}

			if (do_ide_controller_user_wait_busy_controller(ide) != 0 || do_ide_controller_user_wait_drive_ready(ide) < 0) {
				user_esc = 1;
				break;
			}

			if (do_ide_controller_user_wait_drive_drq(ide) < 0) {
				user_esc = 1;
				break;
			}

			if (nfo->mode == DRIVE_RW_MODE_CHSMULTIPLE || nfo->mode == DRIVE_RW_MODE_LBAMULTIPLE ||
				nfo->mode == DRIVE_RW_MODE_LBA48_MULTIPLE)
				drq_len = nfo->multiple_sectors * 512;
			else
				drq_len = 512;

			/* OK. read it in and acknowledge */
			idelib_read_pio_general(cdrom_sector+ret_len,drq_len,ide,IDELIB_PIO_WIDTH_DEFAULT);
			ret_len += drq_len;
		}

		/* ---- draw contents on the screen ---- */
		vga_write_color(0x0E);
		if (!cleared) {
			vga_clear();
			cleared = 1;
		}

		vga_moveto(0,0);
		vga_write("Contents of HDD sector ");
		if (nfo->mode == DRIVE_RW_MODE_CHS || nfo->mode == DRIVE_RW_MODE_CHSMULTIPLE) {
			sprintf(tmp,"CHS %u/%u/%u",nfo->cylinder,nfo->head,nfo->sector); vga_write(tmp);
		}
		else {
			sprintf(tmp,"%llu-%llu",nfo->lba,nfo->lba+(unsigned long long)tlen_sect-1ULL); vga_write(tmp);
		}
		sprintf(tmp,"(%lu) bytes  ",(unsigned long)tlen); vga_write(tmp);

		vga_moveto(0,2);
		vga_write_color(0x08);
		vga_write("BYTE ");

		vga_moveto(5,2);
		for (x=0;x < 16;x++) {
			sprintf(tmp,"+%X ",x);
			vga_write(tmp);
		}

		vga_moveto(5+(16*3)+1,2);
		for (x=0;x < 16;x++) {
			sprintf(tmp,"%X",x);
			vga_write(tmp);
		}

		for (i=0;i < (tlen/256UL);i++) { /* 16x16x8 = 2^(4+4+3) = 2^11 = 2048 */
			for (y=0;y < 16;y++) {
				vga_moveto(0,y+3);
				vga_write_color(0x08);
				sprintf(tmp,"%04X ",(i*256)+(y*16));
				vga_write(tmp);
			}

			for (y=0;y < 16;y++) {
				vga_moveto(5,y+3);
				vga_write_color(0x0F);
				for (x=0;x < 16;x++) {
					sprintf(tmp,"%02X ",cdrom_sector[(i*256)+(y*16)+x]);
					vga_write(tmp);
				}

				vga_moveto(5+(16*3)+1,y+3);
				vga_write_color(0x0E);
				for (x=0;x < 16;x++) {
					vga_writec(sanitizechar(cdrom_sector[(i*256)+(y*16)+x]));
				}
			}

			if (continuous && !user_esc) {
				if (kbhit()) {
					c = getch();
					if (c == 0) c = getch() << 8;
				}
				break;
			}
			else {
				if ((c=wait_for_enter_or_escape()) == 27)
					break; /* allow user to exit early by hitting ESC */
			}
		}

		if (c != 27 && !user_esc) {
			/* if the user hit ENTER, then read another sector and display that too */
			if (nfo->mode == DRIVE_RW_MODE_CHS || nfo->mode == DRIVE_RW_MODE_CHSMULTIPLE) {
				nfo->sector += (unsigned long long)tlen_sect;
				while (nfo->sector > nfo->num_sector) {
					nfo->sector -= nfo->num_sector;
					nfo->head++;
				}
				while (nfo->head >= nfo->num_head) {
					nfo->head -= nfo->num_head;
					nfo->cylinder++;
				}

				if (nfo->cylinder >= 16384) {
					nfo->cylinder = 16383;
					nfo->sector = nfo->num_sector;
					nfo->head = nfo->num_head - 1;
				}

				nfo->lba  = (unsigned long long)nfo->cylinder * (unsigned long long)nfo->num_sector * (unsigned long long)nfo->num_head;
				nfo->lba += (unsigned long long)nfo->head * (unsigned long long)nfo->num_sector;
				nfo->lba += (unsigned long long)nfo->sector - 1ULL;
			}
			else {
				nfo->lba += (unsigned long long)tlen_sect;

				nfo->sector = (int)(nfo->lba % (unsigned long long)nfo->num_sector) + 1;
				nfo->head = (int)((nfo->lba / (unsigned long long)nfo->num_sector) % (unsigned long long)nfo->num_head);
				if (nfo->lba >= (16384ULL * (unsigned long long)nfo->num_sector * (unsigned long long)nfo->num_head)) {
					nfo->cylinder = 16383;
					nfo->sector = nfo->num_sector;
					nfo->head = nfo->num_head - 1;
				}
				else {
					nfo->cylinder = (int)((nfo->lba / (unsigned long long)nfo->num_sector) / (unsigned long long)nfo->num_head);
				}
			}

			goto again;
		}
	}
	else {
		common_ide_success_or_error_vga_msg_box(ide,&vgabox);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
	}
}

void do_drive_readwrite_edit_chslba(struct ide_controller *ide,unsigned char which,struct drive_rw_test_info *nfo,unsigned char editgeo) {
	uint64_t lba,tmp;
	int cyl,head,sect;
	struct vga_msg_box box;
	unsigned char redraw=1;
	unsigned char ok=1;
	char temp_str[64];
	int select=0;
	int c,i=0;

	if (editgeo) {
		cyl = nfo->num_cylinder;
		sect = nfo->num_sector;
		head = nfo->num_head;
		lba = nfo->max_lba;
	}
	else {
		cyl = nfo->cylinder;
		sect = nfo->sector;
		head = nfo->head;
		lba = nfo->lba;
	}

	vga_msg_box_create(&box,editgeo ?
		"Edit disk geometry:             " :
		"Edit position:                  ",
		2+4,0);
	while (1) {
		char recalc = 0;
		char rekey = 0;

		if (redraw) {
			redraw = 0;

			vga_moveto(box.x+2,box.y+2+1 + 0);
			vga_write_color(0x1E);
			vga_write("Cylinder:  ");
			vga_write_color(select == 0 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",cyl);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 1);
			vga_write_color(0x1E);
			vga_write("Head:      ");
			vga_write_color(select == 1 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",head);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 2);
			vga_write_color(0x1E);
			vga_write("Sector:    ");
			vga_write_color(select == 2 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%u",sect);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);

			vga_moveto(box.x+2,box.y+2+1 + 3);
			vga_write_color(0x1E);
			vga_write("LBA:       ");
			vga_write_color(select == 3 ? 0x70 : 0x1E);
			i=sprintf(temp_str,"%llu",lba);
			while (i < (box.w-4-11)) temp_str[i++] = ' ';
			temp_str[i] = 0;
			vga_write(temp_str);
		}

		c = getch();
		if (c == 0) c = getch() << 8;

nextkey:	if (c == 27) {
			ok = 0;
			break;
		}
		else if (c == 13) {
			ok = 1;
			break;
		}
		else if (c == 0x4800) {
			if (--select < 0) select = 3;
			redraw = 1;
		}
		else if (c == 0x5000 || c == 9/*tab*/) {
			if (++select > 3) select = 0;
			redraw = 1;
		}

		else if (c == 0x4B00) { /* left */
			switch (select) {
				case 0:
					if (cyl == 0) cyl = editgeo ? 16383 : (nfo->num_cylinder - 1);
					else cyl--;
					break;
				case 1:
					if (head == 0) head = editgeo ? 16 : (nfo->num_head - 1);
					else head--;
					break;
				case 2:
					if (sect <= 1) sect = editgeo ? 256 : nfo->num_sector;
					else sect--;
					break;
				case 3:
					if (lba > 0ULL) lba--;
					break;
			};

			recalc = 1;
			redraw = 1;
		}
		else if (c == 0x4D00) { /* right */
			switch (select) {
				case 0:
					if ((++cyl) >= (editgeo ? 16384 : nfo->num_cylinder)) cyl = 0;
					break;
				case 1:
					if ((++head) >= (editgeo ? 17 : nfo->num_head)) head = 0;
					break;
				case 2:
					if ((++sect) >= (editgeo ? 257 : (nfo->num_sector+1))) sect = 1;
					break;
				case 3:
					lba++;
					break;
			};

			recalc = 1;
			redraw = 1;
		}

		else if (c == 8 || isdigit(c)) {
			unsigned int sy = box.y+2+1 + select;
			unsigned int sx = box.x+2+11;

			switch (select) {
				case 0:	sprintf(temp_str,"%u",cyl); break;
				case 1:	sprintf(temp_str,"%u",head); break;
				case 2:	sprintf(temp_str,"%u",sect); break;
				case 3:	sprintf(temp_str,"%llu",lba); break;
			}

			if (c == 8) {
				i = strlen(temp_str) - 1;
				if (i < 0) i = 0;
				temp_str[i] = 0;
			}
			else {
				i = strlen(temp_str);
				if (i == 1 && temp_str[0] == '0') i--;
				if ((i+2) < sizeof(temp_str)) {
					temp_str[i++] = (char)c;
					temp_str[i] = 0;
				}
			}

			redraw = 1;
			while (1) {
				if (redraw) {
					redraw = 0;
					vga_moveto(sx,sy);
					vga_write_color(0x70);
					vga_write(temp_str);
					while (vga_pos_x < (box.x+box.w-4) && vga_pos_x != 0) vga_writec(' ');
				}

				c = getch();
				if (c == 0) c = getch() << 8;

				if (c == 8) {
					if (i > 0) {
						temp_str[--i] = 0;
						redraw = 1;
					}
				}
				else if (isdigit(c)) {
					if ((i+2) < sizeof(temp_str)) {
						temp_str[i++] = (char)c;
						temp_str[i] = 0;
						redraw = 1;
					}
				}
				else {
					break;
				}
			}

			switch (select) {
				case 0:	tmp=strtoull(temp_str,NULL,0); cyl=(tmp >  16383ULL ? 16383ULL : tmp); break;
				case 1:	tmp=strtoull(temp_str,NULL,0); head=(tmp >    16ULL ?    16ULL : tmp); break;
				case 2:	tmp=strtoull(temp_str,NULL,0); sect=(tmp >   256ULL ?   256ULL : tmp); break;
				case 3:	lba=strtoull(temp_str,NULL,0); break;
			}

			rekey = 1;
			recalc = 1;
		}

		if (recalc) {
			recalc = 0;
			if (sect == 0) sect = 1;
			if (cyl > 16383) cyl = 16383;

			if (editgeo) {
				if (cyl < 1) cyl = 1;
				if (head < 1) head = 1;
				if (head > 16) head = 16;
				if (sect > 256) sect = 256;

				if (select == 3) {
					if (lba >= (16383ULL * 16ULL * 63ULL)) {
						sect = 63;
						head = 16;
						cyl = 16383;
					}
					else {
						cyl = (int)(lba / (unsigned long long)sect / (unsigned long long)head);
						if (cyl < 0) cyl = 1;
						if (cyl > 16383) cyl = 16383;
					}
				}
				else if (cyl < 16383) {
					lba = (unsigned long long)cyl * (unsigned long long)head * (unsigned long long)sect;
				}
			}
			else {
				if (cyl < 0) cyl = 0;
				if (head < 0) head = 0;
				if (head > 15) head = 15;
				if (sect > 255) sect = 255;

				if (select == 3) {
					sect = (int)(lba % (unsigned long long)nfo->num_sector) + 1;
					head = (int)((lba / (unsigned long long)nfo->num_sector) % (unsigned long long)nfo->num_head);
					if (lba >= (16384ULL * (unsigned long long)nfo->num_sector * (unsigned long long)nfo->num_head)) {
						cyl = 16383;
						sect = nfo->num_sector;
						head = nfo->num_head - 1;
					}
					else {
						cyl = (int)((lba / (unsigned long long)nfo->num_sector) / (unsigned long long)nfo->num_head);
					}
				}
				else {
					lba  = (unsigned long long)cyl * (unsigned long long)nfo->num_sector * (unsigned long long)nfo->num_head;
					lba += (unsigned long long)head * (unsigned long long)nfo->num_sector;
					lba += (unsigned long long)sect - 1ULL;
				}
			}

			redraw = 1;
		}

		if (rekey) {
			rekey = 0;
			goto nextkey;
		}
	}
	vga_msg_box_destroy(&box);

	if (ok) {
		if (editgeo) {
			nfo->num_cylinder = cyl;
			nfo->num_sector = sect;
			nfo->num_head = head;
			nfo->max_lba = lba;
		}
		else {
			nfo->cylinder = cyl;
			nfo->sector = sect;
			nfo->head = head;
			nfo->lba = lba;
		}
	}
}

void do_drive_readwrite_test_choose_mode(struct ide_controller *ide,unsigned char which,struct drive_rw_test_info *nfo) {
	int select=drive_rw_test_nfo.mode;
	struct menuboxbounds mbox;
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int c;

	/* UI element vars */
	menuboxbounds_set_def_list(&mbox,/*ofsx=*/4,/*ofsy=*/7,/*cols=*/1);
	menuboxbounds_set_item_strings_arraylen(&mbox,drive_readwrite_test_modes);

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
			vga_write("        IDE r/w test mode ");
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

			sprintf(tmp," lba=%u lba48=%u multiple=%u",
				drive_rw_test_nfo.can_do_lba?1:0,
				drive_rw_test_nfo.can_do_lba48?1:0,
				drive_rw_test_nfo.can_do_multiple?1:0);
			vga_write(tmp);

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

			vga_moveto(mbox.ofsx,mbox.ofsy - 2);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Back");
			while (vga_pos_x < (mbox.width+mbox.ofsx) && vga_pos_x != 0) vga_writec(' ');

			menuboxbound_redraw(&mbox,select);
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			struct vga_msg_box vgabox;

			if (!drive_rw_test_nfo.can_do_multiple &&
				(select == DRIVE_RW_MODE_CHSMULTIPLE ||
				select == DRIVE_RW_MODE_LBAMULTIPLE ||
				select == DRIVE_RW_MODE_LBA48_MULTIPLE)) {

				vga_msg_box_create(&vgabox,
					"The IDE device doesn't seem to support SET MULTIPLE/READ MULTIPLE.\n"
					"Are you sure you want to choose this mode?\n"
					"\n"
					"Hit ENTER to proceed, ESC to cancel"
					,0,0);
				do {
					c = getch();
					if (c == 0) c = getch() << 8;
				} while (!(c == 13 || c == 27));
				vga_msg_box_destroy(&vgabox);
				if (c == 27) continue;
			}

			if (!drive_rw_test_nfo.can_do_lba &&
				(select == DRIVE_RW_MODE_LBA ||
				select == DRIVE_RW_MODE_LBAMULTIPLE ||
				select == DRIVE_RW_MODE_LBA48 ||
				select == DRIVE_RW_MODE_LBA48_MULTIPLE)) {

				vga_msg_box_create(&vgabox,
					"The IDE device doesn't seem to support LBA mode.\n"
					"Are you sure you want to choose this mode?\n"
					"\n"
					"Hit ENTER to proceed, ESC to cancel"
					,0,0);
				do {
					c = getch();
					if (c == 0) c = getch() << 8;
				} while (!(c == 13 || c == 27));
				vga_msg_box_destroy(&vgabox);
				if (c == 27) continue;
			}

			if (!drive_rw_test_nfo.can_do_lba48 &&
				(select == DRIVE_RW_MODE_LBA48 ||
				select == DRIVE_RW_MODE_LBA48_MULTIPLE)) {

				vga_msg_box_create(&vgabox,
					"The IDE device doesn't seem to support 48-bit LBA.\n"
					"Are you sure you want to choose this mode?\n"
					"\n"
					"Hit ENTER to proceed, ESC to cancel"
					,0,0);
				do {
					c = getch();
					if (c == 0) c = getch() << 8;
				} while (!(c == 13 || c == 27));
				vga_msg_box_destroy(&vgabox);
				if (c == 27) continue;
			}

			if (select >= 0)
				drive_rw_test_nfo.mode = select;

			break;
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = mbox.item_max;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > mbox.item_max)
				select = -1;

			redraw = 1;
		}
	}
}

static const char *drive_read_test_menustrings[] = {
	"Show IDE register taskfile",		/* 0 */
	"*mode*",				/* 1 */ /* rewritten (CHS, LBA, CHS MULTI, etc) */
	drive_readwrite_test_geo,
	drive_readwrite_test_chs,
	drive_readwrite_test_numsec,
	drive_readwrite_test_mult,
	"Read sectors",
	"Read sectors continuously"
};

void do_drive_read_test(struct ide_controller *ide,unsigned char which) {
	struct menuboxbounds mbox;
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	int select=-1;
	char redraw=1;
	int c;

	/* UI element vars */
	menuboxbounds_set_def_list(&mbox,/*ofsx=*/4,/*ofsy=*/7,/*cols=*/1);
	menuboxbounds_set_item_strings_arraylen(&mbox,drive_read_test_menustrings);

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
			vga_write("        IDE r/w tests ");
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

			sprintf(tmp," lba=%u lba48=%u multiple=%u",
				drive_rw_test_nfo.can_do_lba?1:0,
				drive_rw_test_nfo.can_do_lba48?1:0,
				drive_rw_test_nfo.can_do_multiple?1:0);
			vga_write(tmp);

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

			drive_read_test_menustrings[1] = drive_readwrite_test_modes[drive_rw_test_nfo.mode];

			sprintf(drive_readwrite_test_geo,"Geometry: C/H/S %u/%u/%u LBA %llu",
				drive_rw_test_nfo.num_cylinder,
				drive_rw_test_nfo.num_head,
				drive_rw_test_nfo.num_sector,
				drive_rw_test_nfo.max_lba);

			sprintf(drive_readwrite_test_chs,"Position: C/H/S %u/%u/%u LBA %llu",
				drive_rw_test_nfo.cylinder,
				drive_rw_test_nfo.head,
				drive_rw_test_nfo.sector,
				drive_rw_test_nfo.lba);

			sprintf(drive_readwrite_test_numsec,"Number of sectors per read: %u",
				drive_rw_test_nfo.read_sectors);

			sprintf(drive_readwrite_test_mult,"Multiple mode: %u sectors (max=%u)",
				drive_rw_test_nfo.multiple_sectors,
				drive_rw_test_nfo.max_multiple_sectors);

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
				case 1: /* Mode */
					do_drive_readwrite_test_choose_mode(ide,which,&drive_rw_test_nfo);
					redraw = backredraw = 1;
					break;
				case 2: /* Edit geometry/max LBA */
					do_drive_readwrite_edit_chslba(ide,which,&drive_rw_test_nfo,/*editgeo*/1);
					redraw = 1;
					break;
				case 3: /* Edit position */
					do_drive_readwrite_edit_chslba(ide,which,&drive_rw_test_nfo,/*editgeo*/0);
					redraw = 1;
					break;
				case 4: /* Number of sectors */
					c = prompt_sector_count();
					if (c >= 1 && c <= 256) {
						drive_rw_test_nfo.read_sectors = c;
						redraw = 1;
					}
					break;
				case 5: /* Multiple mode sector count */
					c = prompt_sector_count();
					if (c >= 0 && c <= 255) {
						do_ide_set_multiple_mode(ide,which,c);
						{
							uint16_t info[256];

							do_ide_identify((unsigned char*)info,sizeof(info),ide,which,0xEC/*ATA IDENTIFY DEVICE*/);
							drive_rw_test_nfo.multiple_sectors = info[59]&0xFF;
						}
						redraw = 1;
					}
					break;
				case 6: /*Read sectors*/
				case 7: /*Read sectors continuously*/
					do_hdd_drive_read_test(ide,which,/*continuous=*/(select==7),&drive_rw_test_nfo);
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
			switch (select) {
				case 1: /* Mode */
					do {
						if (drive_rw_test_nfo.mode == 0)
							drive_rw_test_nfo.mode = DRIVE_RW_MODE_MAX-1;
						else
							drive_rw_test_nfo.mode--;
					} while (!drive_rw_test_mode_supported(&drive_rw_test_nfo));
					break;
				case 4: /* Number of sectors */
					if (drive_rw_test_nfo.read_sectors > 1)
						drive_rw_test_nfo.read_sectors--;
					break;
			};

			redraw = 1;
		}
		else if (c == 0x4D00) { /* right */
			switch (select) {
				case 1: /* Mode */
					do {
						if (++drive_rw_test_nfo.mode >= DRIVE_RW_MODE_MAX)
							drive_rw_test_nfo.mode = 0;
					} while (!drive_rw_test_mode_supported(&drive_rw_test_nfo));
					break;
				case 4: /* Number of sectors */
					if (drive_rw_test_nfo.read_sectors < 256)
						drive_rw_test_nfo.read_sectors++;
					break;
			};

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > mbox.item_max)
				select = -1;

			redraw = 1;
		}
	}
}

/*-----------------------------------------------------------------*/

static const char *drive_readwrite_tests_menustrings[] = {
	"Show IDE register taskfile",		/* 0 */
	"Reading tests >>",
	"Writing tests >>",
	"Read verify tests"
};

void do_drive_readwrite_tests(struct ide_controller *ide,unsigned char which) {
	struct menuboxbounds mbox;
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	int select=-1;
	char redraw=1;
	int c;

	/* get geometry and max sector count *NOW* then the user can tweak them later */
	memset(&drive_rw_test_nfo,0,sizeof(drive_rw_test_nfo));
	{
		uint16_t info[256];

		c = do_ide_identify((unsigned char*)info,sizeof(info),ide,which,0xEC/*ATA IDENTIFY DEVICE*/);
		if (c < 0) return;

		drive_rw_test_nfo.can_do_lba = (info[49] & 0x200) ? 1 : 0;
		drive_rw_test_nfo.can_do_multiple = ((info[47] & 0xFF) != 0) ? 1 : 0;
		drive_rw_test_nfo.can_do_lba48 = drive_rw_test_nfo.can_do_lba && ((info[83] & 0x400) ? 1 : 0);

		/* NTS: Never mind the thousands of OSes out there still using these fields, ATA-8 marks them "obsolete".
		 *      Thanks. You guys realize this is the same logic behind the infuriating number of APIs in Windows
		 *      that are "obsolete" yet everything relies on them?
		 *
		 *      This is why you keep OLDER copies of standards around, guys! */
		drive_rw_test_nfo.num_cylinder = info[54];	/* number of current logical cylinders */
		drive_rw_test_nfo.num_head = info[55];		/* number of current logical heads */
		drive_rw_test_nfo.num_sector = info[56];	/* number of current logical sectors */
		if (drive_rw_test_nfo.num_cylinder == 0 && drive_rw_test_nfo.num_head == 0 && drive_rw_test_nfo.num_sector == 0) {
			drive_rw_test_nfo.num_cylinder = info[1]; /* number of logical cylinders */
			drive_rw_test_nfo.num_head = info[3];	/* number of logical heads */
			drive_rw_test_nfo.num_sector = info[6]; /* number of logical sectors */
		}

		if (drive_rw_test_nfo.can_do_lba48)
			drive_rw_test_nfo.max_lba = ((uint64_t)info[103] << 48ULL) + ((uint64_t)info[102] << 32ULL) +
				((uint64_t)info[101] << 16ULL) + ((uint64_t)info[100]);
		if (drive_rw_test_nfo.max_lba == 0)
			drive_rw_test_nfo.max_lba = ((uint64_t)info[61] << 16ULL) + ((uint64_t)info[60]);
		if (drive_rw_test_nfo.max_lba == 0)
			drive_rw_test_nfo.max_lba = ((uint64_t)info[58] << 16ULL) + ((uint64_t)info[57]);

		drive_rw_test_nfo.sector = 1;
		drive_rw_test_nfo.read_sectors = 1;
		drive_rw_test_nfo.mode = DRIVE_RW_MODE_CHS;
		if (drive_rw_test_nfo.can_do_multiple && (info[59]&0x100))
			drive_rw_test_nfo.multiple_sectors = info[59]&0xFF;
		if (drive_rw_test_nfo.can_do_multiple && (info[47]&0xFF) != 0)
			drive_rw_test_nfo.max_multiple_sectors = info[47]&0xFF;
	}

	/* UI element vars */
	menuboxbounds_set_def_list(&mbox,/*ofsx=*/4,/*ofsy=*/7,/*cols=*/1);
	menuboxbounds_set_item_strings_arraylen(&mbox,drive_readwrite_tests_menustrings);

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
			vga_write("        IDE controller read/write tests ");
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
				case 1: /* Read tests */
					do_drive_read_test(ide,which);
					redraw = backredraw = 1;
					break;
				case 2: /* Write tests */
					redraw = backredraw = 1;
					break;
				case 3: /* Read verify tests */
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
	"Read/Write tests >>"			/* 10 */
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
					/* just in case the IDE controller demands it, wait for not-busy before banging
					 * the host reset bit. if the IDE controller is hung, the user can command us
					 * to continue regardless */
					c = do_ide_controller_user_wait_busy_controller(ide);
					if (c >= 0) { /* if not busy, or busy and user commands us to continue */
						vga_msg_box_create(&vgabox,"Host reset in progress",0,0);

						idelib_device_control_set_reset(ide,1);
						t8254_wait(t8254_us2ticks(1000000));
						idelib_device_control_set_reset(ide,0);

						vga_msg_box_destroy(&vgabox);

						/* now wait for not busy */
						c = do_ide_controller_user_wait_busy_controller(ide);
					}
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
	int i;

	printf("IDE ATA/ATAPI test program\n");

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

	if (int10_getmode() != 3) {
		int10_setmode(3);
		update_state_from_vga();
	}

	do_main_menu();
	free_idelib();
	return 0;
}

