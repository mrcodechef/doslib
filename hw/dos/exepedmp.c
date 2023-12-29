
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>

#include <hw/dos/exehdr.h>
#include <hw/dos/exenehdr.h>
#include <hw/dos/exenepar.h>

#ifndef O_BINARY
#define O_BINARY (0)
#endif

#define EXE_PE_SIGNATURE                                                        (0x4550U)

#pragma pack(push,1)
struct exe_pe_coff_file_header {
        uint16_t                                Machine;                        /* +0x00 target machine */
        uint16_t                                NumberOfSections;               /* +0x02 number of PE sections */
        uint32_t                                TimeDateStamp;                  /* +0x04 Time/Date timestamp */
        uint32_t                                PointerToSymbolTable;           /* +0x08 File offset of COFF symbol table or 0 */
        uint32_t                                NumberOfSymbols;                /* +0x0C Number of entries in symbol table */
        uint16_t                                SizeOfOptionalHeader;           /* +0x10 Size of the optional header */
        uint16_t                                Characteristics;                /* +0x12 Flags indicating attributes of the file */
};                                                                              /* =0x14 */
#pragma pack(pop)

#pragma pack(push,1)
/* This is the initial header. It does not define anything more because fields past this struct do vary */
struct exe_pe_header {
        uint32_t                                signature;                      /* +0x00 PE\0\0 */
        struct exe_pe_coff_file_header          fileheader;                     /* +0x04 COFF File Header (0x14 bytes) */
};                                                                              /* =0x18 */
/* optional header follows including section table */
#pragma pack(pop)

const char *exe_pe_fileheader_machine_to_str(const uint16_t Machine) {
        switch (Machine) {
                case 0x0000:    return "UNKNOWN";
                case 0x014C:    return "I386";
                default:        break;
        }

        return "?";
}

static unsigned char            opt_sort_ordinal = 0;
static unsigned char            opt_sort_names = 0;

static char*                    src_file = NULL;
static int                      src_fd = -1;

static struct exe_dos_header    exehdr;
static struct exe_dos_layout    exelayout;

static void help(void) {
    fprintf(stderr,"EXENEDMP -i <exe file>\n");
    fprintf(stderr," -sn        Sort names\n");
    fprintf(stderr," -so        Sort by ordinal\n");
}

int main(int argc,char **argv) {
    struct exe_pe_header pe_header;
    uint32_t pe_header_offset;
    uint32_t file_size;
    char *a;
    int i;

    memset(&exehdr,0,sizeof(exehdr));

    for (i=1;i < argc;) {
        a = argv[i++];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"h") || !strcmp(a,"help")) {
                help();
                return 1;
            }
            else if (!strcmp(a,"sn")) {
                opt_sort_names = 1;
            }
            else if (!strcmp(a,"so")) {
                opt_sort_ordinal = 1;
            }
            else if (!strcmp(a,"i")) {
                src_file = argv[i++];
                if (src_file == NULL) return 1;
            }
            else {
                fprintf(stderr,"Unknown switch %s\n",a);
                return 1;
            }
        }
        else {
            fprintf(stderr,"Unknown switch %s\n",a);
            return 1;
        }
    }

    assert(sizeof(exehdr) == 0x1C);

    if (src_file == NULL) {
        fprintf(stderr,"No source file specified\n");
        return 1;
    }

    src_fd = open(src_file,O_RDONLY|O_BINARY);
    if (src_fd < 0) {
        fprintf(stderr,"Unable to open '%s', %s\n",src_file,strerror(errno));
        return 1;
    }

    file_size = lseek(src_fd,0,SEEK_END);
    lseek(src_fd,0,SEEK_SET);

    if (read(src_fd,&exehdr,sizeof(exehdr)) != (int)sizeof(exehdr)) {
        fprintf(stderr,"EXE header read error\n");
        return 1;
    }

    if (exehdr.magic != 0x5A4DU/*MZ*/) {
        fprintf(stderr,"EXE header signature missing\n");
        return 1;
    }

    printf("File size:                        %lu bytes\n",
        (unsigned long)file_size);
    printf("MS-DOS EXE header:\n");
    printf("    last_block_bytes:             %u bytes\n",
        exehdr.last_block_bytes);
    printf("    exe_file_blocks:              %u bytes\n",
        exehdr.exe_file_blocks);
    printf("  * exe resident size (blocks):   %lu bytes\n",
        (unsigned long)exe_dos_header_file_resident_size(&exehdr));
    printf("                                  ^  x  = %lu x 512 = %lu\n",
        (unsigned long)exehdr.exe_file_blocks,
        (unsigned long)exehdr.exe_file_blocks * 512UL);
    if (exehdr.last_block_bytes != 0U && exehdr.exe_file_blocks != 0U) {
        printf("                                  ^ (x -= 512) = %lu, last block not full 512 bytes\n",
            (unsigned long)exehdr.exe_file_blocks * 512UL - 512UL);
        printf("                                  ^ (x += %lu) = %lu, add last block bytes\n",
            (unsigned long)exehdr.last_block_bytes,
            ((unsigned long)exehdr.exe_file_blocks * 512UL) + (unsigned long)exehdr.last_block_bytes - 512UL);
    }
    printf("    number_of_relocations:        %u entries\n",
        exehdr.number_of_relocations);
    printf("  * size of relocation table:     %lu bytes\n",
        (unsigned long)exehdr.number_of_relocations * 4UL);
    printf("    header_size:                  %u paragraphs\n",
        exehdr.header_size_paragraphs);
    printf("  * header_size:                  %lu bytes\n",
        (unsigned long)exe_dos_header_file_header_size(&exehdr));
    printf("    min_additional_paragraphs:    %u paragraphs\n",
        exehdr.min_additional_paragraphs);
    printf("  * min_additional:               %lu bytes\n",
        (unsigned long)exe_dos_header_bss_size(&exehdr));
    printf("    max_additional_paragraphs:    %u paragraphs\n",
        exehdr.max_additional_paragraphs);
    printf("  * max_additional:               %lu bytes\n",
        (unsigned long)exe_dos_header_bss_max_size(&exehdr));
    printf("    init stack pointer:           base_seg+0x%04X:0x%04X\n",
        exehdr.init_stack_segment,
        exehdr.init_stack_pointer);
    printf("    checksum:                     0x%04X\n",
        exehdr.checksum);
    printf("    init instruction pointer:     base_seg+0x%04X:0x%04X\n",
        exehdr.init_code_segment,
        exehdr.init_instruction_pointer);
    printf("    relocation_table_offset:      %u bytes\n",
        exehdr.relocation_table_offset);
    printf("    overlay number:               %u\n",
        exehdr.overlay_number);

    if (exe_dos_header_to_layout(&exelayout,&exehdr) < 0) {
        fprintf(stderr,"EXE layout not appropriate for Windows NE\n");
        return 1;
    }

    if (!exe_header_can_contain_exe_extension(&exehdr)) {
        fprintf(stderr,"EXE header cannot contain extension\n");
        return 1;
    }

    /* go read the extension */
    if (lseek(src_fd,EXE_HEADER_EXTENSION_OFFSET,SEEK_SET) != EXE_HEADER_EXTENSION_OFFSET ||
        read(src_fd,&pe_header_offset,4) != 4) {
        fprintf(stderr,"Cannot read extension\n");
        return 1;
    }
    printf("    EXE extension (if exists) at: %lu\n",(unsigned long)pe_header_offset);

    /* go read the extended header */
    if (lseek(src_fd,pe_header_offset,SEEK_SET) != pe_header_offset ||
        read(src_fd,&pe_header,sizeof(pe_header)) != sizeof(pe_header)) {
        fprintf(stderr,"Cannot read PE header\n");
        return 1;
    }
    if (pe_header.signature != EXE_PE_SIGNATURE) {
        fprintf(stderr,"Not an PE executable\n");
        return 1;
    }

    printf("* PE header at %lu\n",(unsigned long)pe_header_offset);
    printf("    Machine:                        0x%04x (%s)\n",
        pe_header.fileheader.Machine,
        exe_pe_fileheader_machine_to_str(pe_header.fileheader.Machine));
    printf("    NumberOfSections:               %u\n",
        pe_header.fileheader.NumberOfSections);
    printf("    TimeDateStamp:                  %lu (0x%08lx)\n",
        (unsigned long)pe_header.fileheader.TimeDateStamp,
        (unsigned long)pe_header.fileheader.TimeDateStamp);
    printf("    PointerToSymbolTable:           %lu (0x%08lx) (file offset)\n",
        (unsigned long)pe_header.fileheader.PointerToSymbolTable,
        (unsigned long)pe_header.fileheader.PointerToSymbolTable);
    printf("    NumberOfSymbols:                %lu\n",
        (unsigned long)pe_header.fileheader.NumberOfSymbols);
    printf("    SizeOfOptionalHeader:           %lu\n",
        (unsigned long)pe_header.fileheader.SizeOfOptionalHeader);
    printf("    Characteristics:                0x%04x\n",
        pe_header.fileheader.Characteristics);

    close(src_fd);
    return 0;
}
