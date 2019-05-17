
#ifndef __DOSLIB_HW_86DUINO_86DUINO_H
#define __DOSLIB_HW_86DUINO_86DUINO_H

#define VTX86_SYSCLK            (100)

#define VTX86_INPUT             (0x00)
#define VTX86_OUTPUT            (0x01)
#define VTX86_INPUT_PULLUP      (0x02)
#define VTX86_INPUT_PULLDOWN    (0x03)

#define VTX86_LOW               (0x00)
#define VTX86_HIGH              (0x01)
#define VTX86_CHANGE            (0x02)
#define VTX86_FALLING           (0x03)
#define VTX86_RISING            (0x04)

#define VTX86_MCM_MC            (0)
#define VTX86_MCM_MD            (1)

#define VTX86_GPIO_PINS         (45)

/* northbridge 0:0:0 */
#define VORTEX86_PCI_NB_BUS         (0)
#define VORTEX86_PCI_NB_DEV         (0)
#define VORTEX86_PCI_NB_FUNC        (0)

uint8_t vtx86_nb_readb(const uint8_t reg);
uint16_t vtx86_nb_readw(const uint8_t reg);
uint32_t vtx86_nb_readl(const uint8_t reg);
void vtx86_nb_writeb(const uint8_t reg,const uint8_t val);
void vtx86_nb_writew(const uint8_t reg,const uint16_t val);
void vtx86_nb_writel(const uint8_t reg,const uint32_t val);

/* southbridge 0:7:0 */
#define VORTEX86_PCI_SB_BUS         (0)
#define VORTEX86_PCI_SB_DEV         (7)
#define VORTEX86_PCI_SB_FUNC        (0)

uint8_t vtx86_sb_readb(const uint8_t reg);
uint16_t vtx86_sb_readw(const uint8_t reg);
uint32_t vtx86_sb_readl(const uint8_t reg);
void vtx86_sb_writeb(const uint8_t reg,const uint8_t val);
void vtx86_sb_writew(const uint8_t reg,const uint16_t val);
void vtx86_sb_writel(const uint8_t reg,const uint32_t val);

extern uint8_t vtx86_mc_md_inuse[VTX86_GPIO_PINS];

#endif //__DOSLIB_HW_86DUINO_86DUINO_H

