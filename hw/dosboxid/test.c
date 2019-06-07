
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/dos/dos.h>
#include <hw/dos/doswin.h>
#include <hw/8254/8254.h>		/* 8254 timer */

#include <hw/dosboxid/iglib.h>

#ifdef TARGET_WINDOWS
# define WINFCON_STOCK_WIN_MAIN
# include <hw/dos/winfcon.h>
#endif

int main(int argc,char **argv,char **envp) {
	char tmp[128];

	probe_dos();
	detect_windows();

    if (windows_mode == WINDOWS_NT) {
        printf("This program is not compatible with Windows NT\n");
        return 1;
    }

	if (!probe_dosbox_id()) {
		printf("DOSBox integration device not found\n");
		return 1;
	}
	printf("DOSBox integration device found at I/O port %xh\n",dosbox_id_baseio);

	if (probe_dosbox_id_version_string(tmp,sizeof(tmp)))
		printf("DOSBox version string: '%s'\n",tmp);
	else
		printf("DOSBox version string N/A\n");

	dosbox_id_debug_message("This is a debug message\n");
	dosbox_id_debug_message("This is a multi-line debug message\n(second line here)\n");

    {
        uint32_t mixq;

		dosbox_id_write_regsel(DOSBOX_ID_REG_MIXER_QUERY);
		mixq = dosbox_id_read_data();

        printf("Mixer: %u-channel %luHz mute=%u sound=%u swapstereo=%u\n",
            (unsigned int)((mixq >> 20ul) & 0xFul),
            (unsigned long)(mixq & 0xFFFFFul),
            (unsigned int)((mixq >> 30ul) & 1ul),
            ((unsigned int)((mixq >> 31ul) & 1ul)) ^ 1,
            (unsigned int)((mixq >> 29ul) & 1ul));
    }

	return 0;
}

