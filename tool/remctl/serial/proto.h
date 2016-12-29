
#pragma pack(push,1)
/* this is loosely based on Kermit, but not the same protocol */
struct remctl_serial_packet_header {
    unsigned char       mark;
    unsigned char       length;
    unsigned char       sequence;
    unsigned char       type;
    unsigned char       chksum;
};

struct remctl_serial_packet {
    struct remctl_serial_packet_header      hdr;
    unsigned char                           data[256];
};
#pragma pack(pop)

#define REMCTL_SERIAL_MARK      0x01

enum {
    REMCTL_SERIAL_TYPE_DOS=0x44,        /* MS-DOS specific */
    REMCTL_SERIAL_TYPE_ERROR=0x45,
    REMCTL_SERIAL_TYPE_HALT=0x48,       /* halt/un-halt system */
    REMCTL_SERIAL_TYPE_INPORT=0x49,     /* input from port */
    REMCTL_SERIAL_TYPE_OUTPORT=0x4F,    /* output to port */
    REMCTL_SERIAL_TYPE_MEMREAD=0x52,    /* read from memory */
    REMCTL_SERIAL_TYPE_PING=0x50,
    REMCTL_SERIAL_TYPE_MEMWRITE=0x57    /* write to memory */
};

/* REMCTL_SERIAL_TYPE_DOS */
enum {
    REMCTL_SERIAL_TYPE_DOS_LOL=0x4C,    /* return List of Lists pointer */
    REMCTL_SERIAL_TYPE_DOS_INDOS=0x49   /* return InDOS pointer */
};

