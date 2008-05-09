/*
 * hfc_multi.c  low level driver for hfc-4s/hfc-8s/hfc-e1 based cards
 *
 * Author	Andreas Eversberg (jolly@eversberg.eu)
 * ported to mqueue mechanism:
 * 		Peter Sprenger (sprengermoving-bytes.de)
 *
 * inspired by existing hfc-pci driver:
 * Copyright 1999  by Werner Cornelius (werner@isdn-development.de)
 * Copyright 2001,2007  by Karsten Keil (kkeil@suse.de)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Thanks to Cologne Chip AG for this great controller!
 */

/*
 * module parameters:
 * type:
 * 	By default (0), the card is automatically detected.
 * 	Or use the following combinations:
 *	Bit 0-7   = 0x00001 = HFC-E1 (1 port)
 * or	Bit 0-7   = 0x00004 = HFC-4S (4 ports)
 * or	Bit 0-7   = 0x00008 = HFC-8S (8 ports)
 *	Bit 8     = 0x00100 = uLaw (instead of aLaw)
 *	Bit 9     = 0x00200 = Disable DTMF detection on all B-channels via hardware
 *	Bit 10    = spare
 *	Bit 11    = 0x00800 = Force PCM bus into slave mode. (otherwhise auto)
 * or   Bit 12    = 0x01000 = Force PCM bus into master mode. (otherwhise auto)
 *	Bit 13    = 0x02000 = Use direct RX clock for PCM sync rather than PLL. (E1 only)
 *	Bit 14    = 0x04000 = Use external ram (128K)
 *	Bit 15    = 0x08000 = Use external ram (512K)
 *	Bit 16    = 0x10000 = Use 64 timeslots instead of 32
 * or	Bit 17    = 0x20000 = Use 128 timeslots instead of anything else
 *	Bit 18    = 0x40000 = Use crystal clock for PCM and E1, for autarc clocking.
 *	Bit 19    = 0x80000 = Send the Watchdog a Signal (Dual E1 with Watchdog)
 * (all other bits are reserved and shall be 0)
 *	example: 0x20204 one HFC-4S with dtmf detection and 128 timeslots on PCM bus (PCM master)
 *
 * port: (optional or required for all ports on all installed cards)
 *	HFC-4S/HFC-8S only bits:
 *	Bit 0	  = 0x001 = Use master clock for this S/T interface (ony once per chip).
 *	Bit 1     = 0x002 = transmitter line setup (non capacitive mode) DONT CARE!
 *	Bit 2     = 0x004 = Disable E-channel. (No E-channel processing)
 *	example: 0x0001,0x0000,0x0000,0x0000 one HFC-4S with master clock received from port 1
 *
 *	HFC-E1 only bits:
 *	Bit 0     = 0x001 = interface: 0=copper, 1=optical
 *	Bit 1     = 0x002 = reserved (later for 32 B-channels transparent mode)
 *	Bit 2     = 0x004 = Report LOS
 *	Bit 3     = 0x008 = Report AIS
 *	Bit 4     = 0x010 = Report SLIP
 *	Bit 5     = 0x020 = Report RDI (not yet supported)
 *	Bit 6-7   = 0x0X0 = elastic jitter buffer (1-3), Set both bits to 0 for default.
 *	Bit 8     = 0x100 = Turn off CRC-4 Multiframe Mode, use double frame mode instead.
 * (all other bits are reserved and shall be 0)
 *
 * debug:
 *	NOTE: only one debug value must be given for all cards
 *	enable debugging (see hfc_multi.h for debug options)
 *
 * poll:
 *	NOTE: only one poll value must be given for all cards
 *	Give the number of samples for each fifo process.
 *	By default 128 is used. Decrease to reduce delay, increase to
 *	reduce cpu load. If unsure, don't mess with it!
 *	Valid is 8, 16, 32, 64, 128, 256.
 *
 * pcm:
 *	NOTE: only one pcm value must be given for every card.
 *	The PCM bus id tells the mISDNdsp module about the connected PCM bus.
 * 	By default (0), the PCM bus id is 100 for the card that is PCM master.
 * 	If multiple cards are PCM master (because they are not interconnected),
 * 	each card with PCM master will have increasing PCM id.
 *	All PCM busses with the same ID are expected to be connected and have
 *	common time slots slots.
 *	Only one chip of the PCM bus must be master, the others slave.
 *	-1 means no support of PCM bus not even.
 *	Omit this value, if all cards are interconnected or none is connected.
 *	If unsure, don't give this parameter.
 *
 * dslot:
 *	NOTE: only one poll value must be given for every card.
 *	Also this value must be given for non-E1 cards. If omitted, the E1
 *	card has D-channel on time slot 16, which is default.
 *	If 1..15 or 17..31, an alternate time slot is used for D-channel.
 *	In this case, the application must be able to handle this.
 *	If -1 is given, the D-channel is disabled and all 31 slots can be used
 *	for B-channel. (only for specific applications)
 *	If you don't know how to use it, you don't need it!
 * 	
 */

/* debug using register map (never use this, it will flood your system log) */
//#define HFC_REGISTER_MAP

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/mISDNhw.h>
#include <linux/mISDNdsp.h>

// #define IRQCOUNT_DEBUG

#include "hfc_multi.h"
#ifdef ECHOPREP
#include "gaintab.h"
#endif

/* NOTE: MAX_PORTS must be 8*MAX_CARDS */
#define	MAX_CARDS	8
#define	MAX_PORTS	64

static LIST_HEAD(HFClist);
static rwlock_t HFClock = RW_LOCK_UNLOCKED;

static void ph_state_change(struct dchannel *);

extern const char *CardType[];

static const char *hfcmulti_revision = "$Revision: 2.00 $";

extern void ztdummy_extern_interrupt(void);
static void (* hfc_interrupt)(void);
extern void ztdummy_register_interrupt(void);
static void (* register_interrupt)(void);
extern int ztdummy_unregister_interrupt(void);
static int (* unregister_interrupt)(void);
static int interrupt_registered = 0;

#define	VENDOR_CCD	"Cologne Chip AG"
#define	VENDOR_BN	"beroNet GmbH"
#define	VENDOR_DIG	"Digium Inc."
#define VENDOR_JH	"Junghanns.NET GmbH"
#define VENDOR_PRIM	"PrimuX"

#ifndef CCAG_VID
#define	CCAG_VID	0x1397	/* Cologne Chip Vendor ID */
#define	HFC4S_ID	0x08B4
#define	HFC8S_ID	0x16B8
#define	HFCE1_ID	0x30B1
#endif

#ifndef PRIM_VID
#define PRIM_VID	0x0301  /* PrimuX Vendor ID */
#endif


#define	TYP_E1		1
#define	TYP_4S		4
#define TYP_8S		8

static int poll_timer = 6;	/* default = 128 samples = 16ms */
/* number of POLL_TIMER interrupts for G2 timeout (ca 1s) */
static int nt_t1_count[] = { 3840, 1920, 960, 480, 240, 120, 64, 32  };
#define	CLKDEL_TE	0x0f	/* CLKDEL in TE mode */
#define	CLKDEL_NT	0x0c	/* CLKDEL in NT mode (0x60 MUST not be included!) */
static u_char silence =	0xff;	/* silence by LAW */

#define	DIP_4S	0x1		/* DIP Switches for Beronet 1S/2S/4S cards */
#define	DIP_8S	0x2		/* DIP Switches for Beronet 8S+ cards */
#define	DIP_E1	0x3		/* DIP Switches for Beronet E1 cards */
/* enable 32 bit fifo access (PC usage) */
#define	FIFO_32BIT_ACCESS

/*
 * module stuff
 */

static uint	type[MAX_CARDS];
static uint	pcm[MAX_CARDS];
static uint	dslot[MAX_CARDS];
static uint	port[MAX_PORTS];
static uint	debug;
static uint	poll;
static uint	timer;

static int	HFC_cnt, Port_cnt, PCM_cnt = 99;

#ifdef MODULE
MODULE_AUTHOR("Andreas Eversberg");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif
module_param(debug, uint, S_IRUGO | S_IWUSR);
module_param(poll, uint, S_IRUGO | S_IWUSR);
module_param(timer, uint, S_IRUGO | S_IWUSR);
module_param_array(type, uint, NULL, S_IRUGO | S_IWUSR);
module_param_array(pcm, uint, NULL, S_IRUGO | S_IWUSR);
module_param_array(dslot, uint, NULL, S_IRUGO | S_IWUSR);
module_param_array(port, uint, NULL, S_IRUGO | S_IWUSR);
#endif


#define	HFCM_FIX_IRQS

static void
enable_hwirq(struct hfc_multi *hc)
{
	hc->hw.r_irq_ctrl |= V_GLOB_IRQ_EN;
	HFC_outb(hc, R_IRQ_CTRL, hc->hw.r_irq_ctrl);
}

static void
disable_hwirq(struct hfc_multi *hc)
{
	hc->hw.r_irq_ctrl &= ~((u_char)V_GLOB_IRQ_EN);
	HFC_outb(hc, R_IRQ_CTRL, hc->hw.r_irq_ctrl);
}

#ifndef CONFIG_HFCMULTI_PCIMEM
#define	B410P_CARD
#endif

#define	NUM_EC 2
#define	MAX_TDM_CHAN 32


#ifdef B410P_CARD
inline void
enablepcibridge(struct hfc_multi *c)
{
	HFC_outb(c, R_BRG_PCM_CFG, (0x0 << 6) | 0x3); /* was _io before */
}

inline void
disablepcibridge(struct hfc_multi *c)
{
	HFC_outb(c, R_BRG_PCM_CFG, (0x0 << 6) | 0x2); /* was _io before */
}

inline unsigned char
readpcibridge(struct hfc_multi *c, unsigned char address)
{
	unsigned short cipv;
	unsigned char data;

	/* slow down a PCI read access by 1 PCI clock cycle */
	HFC_outb(c, R_CTRL, 0x4); /*was _io before*/

	if (address == 0)
		cipv = 0x4000;
	else
		cipv = 0x5800;

	/* select local bridge port address by writing to CIP port */
	// data = HFC_inb(c, cipv); /* was _io before */
	outw(cipv, c->pci_iobase + 4);
	data = inb(c->pci_iobase);

	/* restore R_CTRL for normal PCI read cycle speed */
	HFC_outb(c, R_CTRL, 0x0); /* was _io before */

	return data;
}

inline void
writepcibridge(struct hfc_multi *hc, unsigned char address, unsigned char data)
{
	unsigned short cipv;
	unsigned int datav;

	if (address == 0)
		cipv = 0x4000;
	else
		cipv = 0x5800;

	/* select local bridge port address by writing to CIP port */
	outw(cipv, hc->pci_iobase + 4);

	/* define a 32 bit dword with 4 identical bytes for write sequence */
	datav=data | ((__u32) data << 8) | ((__u32) data << 16) |
	    ((__u32) data << 24);

	/*
	 * write this 32 bit dword to the bridge data port
	 * this will initiate a write sequence of up to 4 writes to the same
	 * address on the local bus interface the number of write accesses
	 * is undefined but >=1 and depends on the next PCI transaction
	 * during write sequence on the local bus
	 */
	outl(datav, hc->pci_iobase);
}

inline void
cpld_set_reg(struct hfc_multi *hc, unsigned char reg)
{
	/* Do data pin read low byte */
	HFC_outb(hc, R_GPIO_OUT1, reg);
}

inline void
cpld_write_reg(struct hfc_multi *hc, unsigned char reg, unsigned char val)
{
	cpld_set_reg(hc, reg);

	enablepcibridge(hc);
	writepcibridge(hc, 1, val);
	disablepcibridge(hc);

	return;
}

inline unsigned char
cpld_read_reg(struct hfc_multi *hc, unsigned char reg)
{
	unsigned char bytein;

	cpld_set_reg(hc, reg);

	/* Do data pin read low byte */
	HFC_outb(hc, R_GPIO_OUT1, reg);

	enablepcibridge(hc);
	bytein = readpcibridge(hc, 1);
	disablepcibridge(hc);

	return bytein;
}

inline void
vpm_write_address(struct hfc_multi *hc, unsigned short addr)
{
	cpld_write_reg(hc, 0, 0xff & addr);
	cpld_write_reg(hc, 1, 0x01 & (addr >> 8));
}

inline unsigned short
vpm_read_address(struct hfc_multi *c)
{
	unsigned short addr;
	unsigned short highbit;

	addr = cpld_read_reg(c, 0);
	highbit = cpld_read_reg(c, 1);

	addr = addr | (highbit << 8);

	return addr & 0x1ff;
}

inline unsigned char
vpm_in(struct hfc_multi *c, int which, unsigned short addr)
{
	unsigned char res;

	vpm_write_address(c, addr);

	if (!which)
		cpld_set_reg(c, 2);
	else
		cpld_set_reg(c, 3);

	enablepcibridge(c);
	res = readpcibridge(c, 1);
	disablepcibridge(c);

	cpld_set_reg(c, 0);

	return res;
}

inline void
vpm_out(struct hfc_multi *c, int which, unsigned short addr,
    unsigned char data)
{
	vpm_write_address(c, addr);

	enablepcibridge(c);

	if (!which)
		cpld_set_reg(c, 2);
	else
		cpld_set_reg(c, 3);

	writepcibridge(c, 1, data);

	cpld_set_reg(c, 0);

	disablepcibridge(c);

	{
	unsigned char regin;
	regin = vpm_in(c, which, addr);
	if (regin != data)
		printk("Wrote 0x%x to register 0x%x but got back 0x%x\n",
		    data, addr, regin);
	}

}


void
vpm_init(struct hfc_multi *wc)
{
	unsigned char reg;
	unsigned int mask;
	unsigned int i, x, y;
	unsigned int ver;

	for (x = 0; x < NUM_EC; x++) {
		/* Setup GPIO's */
		if (!x) {
			ver = vpm_in(wc, x, 0x1a0);
			printk("VPM: Chip %d: ver %02x\n", x, ver);
		}

		for (y = 0; y < 4; y++) {
			vpm_out(wc, x, 0x1a8 + y, 0x00); /* GPIO out */
			vpm_out(wc, x, 0x1ac + y, 0x00); /* GPIO dir */
			vpm_out(wc, x, 0x1b0 + y, 0x00); /* GPIO sel */
		}

		/* Setup TDM path - sets fsync and tdm_clk as inputs */
		reg = vpm_in(wc, x, 0x1a3); /* misc_con */
		vpm_out(wc, x, 0x1a3, reg & ~2);

		/* Setup Echo length (256 taps) */
		vpm_out(wc, x, 0x022, 1);
		vpm_out(wc, x, 0x023, 0xff);

		/* Setup timeslots */
		vpm_out(wc, x, 0x02f, 0x00);
		mask = 0x02020202 << (x * 4);

		/* Setup the tdm channel masks for all chips */
		for (i = 0; i < 4; i++)
			vpm_out(wc, x, 0x33 - i, (mask >> (i << 3)) & 0xff);

		/* Setup convergence rate */
		printk("VPM: A-law mode\n");
		reg = 0x00 | 0x10 | 0x01;
		vpm_out(wc, x, 0x20, reg);
		printk("VPM reg 0x20 is %x\n", reg);
		//vpm_out(wc, x, 0x20, (0x00 | 0x08 | 0x20 | 0x10));

		vpm_out(wc, x, 0x24, 0x02);
		reg = vpm_in(wc, x, 0x24);
		printk("NLP Thresh is set to %d (0x%x)\n", reg, reg);

		/* Initialize echo cans */
		for (i = 0; i < MAX_TDM_CHAN; i++) {
			if (mask & (0x00000001 << i))
				vpm_out(wc, x, i, 0x00);
		}

		/*
		 * ARM arch at least disallows a udelay of
		 * more than 2ms... it gives a fake "__bad_udelay"
		 * reference at link-time.
		 * long delays in kernel code are pretty sucky anyway
		 * for now work around it using 5 x 2ms instead of 1 x 10ms
		 */

		udelay(2000);
		udelay(2000);
		udelay(2000);
		udelay(2000);
		udelay(2000);

		/* Put in bypass mode */
		for (i = 0; i < MAX_TDM_CHAN; i++) {
			if (mask & (0x00000001 << i)) {
				vpm_out(wc, x, i, 0x01);
			}
		}

		/* Enable bypass */
		for (i = 0; i < MAX_TDM_CHAN; i++) {
			if (mask & (0x00000001 << i))
				vpm_out(wc, x, 0x78 + i, 0x01);
		}
      
	} 
}

void
vpm_check(struct hfc_multi *hctmp)
{
	unsigned char gpi2;

	gpi2 = HFC_inb(hctmp, R_GPI_IN2);

	if ((gpi2 & 0x3) != 0x3) {
		printk("Got interrupt 0x%x from VPM!\n", gpi2);
	}
}


/*
 * Interface to enable/disable the HW Echocan
 *
 * these functions are called within a spin_lock_irqsave on
 * the channel instance lock, so we are not disturbed by irqs
 *
 * we can later easily change the interface to make  other
 * things configurable, for now we configure the taps
 *
 */

void
vpm_echocan_on(struct hfc_multi *hc, int ch, int taps)
{
	unsigned int timeslot;
	unsigned int unit;
	struct bchannel *bch = hc->chan[ch].bch;
#ifdef TXADJ
	int txadj = -4;
#endif
	if (hc->chan[ch].protocol != ISDN_P_B_RAW)
		return;

	if (!bch)
		return;

#ifdef TXADJ
	_queue_data(bch->ch, PH_CONTROL_IND, HFC_VOL_CHANGE_TX, sizeof(int),
		&txadj, GFP_KERNEL);
#endif

	timeslot = ((ch/4)*8) + ((ch%4)*4) + 1;
	unit = ch % 4;

	printk(KERN_NOTICE "vpm_echocan_on called taps [%d] on timeslot %d\n",
	    taps, timeslot);

	vpm_out(hc, unit, timeslot, 0x7e);
}

void
vpm_echocan_off(struct hfc_multi *hc, int ch)
{
	unsigned int timeslot;
	unsigned int unit;
	struct bchannel *bch = hc->chan[ch].bch;
#ifdef TXADJ
	int txadj = 0;
#endif

	if (hc->chan[ch].protocol != ISDN_P_B_RAW)
		return;

	if (!bch)
		return;

#ifdef TXADJ
	_queue_data(bch->ch, PH_CONTROL_IND, HFC_VOL_CHANGE_TX, sizeof(int),
		&txadj, GFP_KERNEL);
#endif

	timeslot = ((ch/4)*8) + ((ch%4)*4) + 1;
	unit = ch % 4;

	printk(KERN_NOTICE "vpm_echocan_off called on timeslot %d\n",
	    timeslot);
	/* FILLME */
	vpm_out(hc, unit, timeslot, 0x01);
}

#endif  /* B410_CARD */



/*
 * free hardware resources used by driver
 */

static void
release_io_hfcmulti(struct hfc_multi *hc)
{
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: entered\n", __FUNCTION__);

	/* soft reset also masks all interrupts */
	hc->hw.r_cirm |= V_SRES;
	HFC_outb(hc, R_CIRM, hc->hw.r_cirm);
	udelay(1000);
	hc->hw.r_cirm &= ~V_SRES;
	HFC_outb(hc, R_CIRM, hc->hw.r_cirm);
	udelay(1000); /* instead of 'wait' that may cause locking */

	/* disable memory mapped ports / io ports */
	pci_write_config_word(hc->pci_dev, PCI_COMMAND, 0);
#ifdef CONFIG_HFCMULTI_PCIMEM
	if (hc->pci_membase) iounmap((void *)hc->pci_membase);
	if (hc->plx_membase) iounmap((void *)hc->plx_membase);
#else
	if (hc->pci_iobase)
		release_region(hc->pci_iobase, 8);
#endif

	if (hc->pci_dev) {
		pci_disable_device(hc->pci_dev);
		pci_set_drvdata(hc->pci_dev, NULL);
	}
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: done\n", __FUNCTION__);
}

/*
 * function called to reset the HFC chip. A complete software reset of chip
 * and fifos is done. All configuration of the chip is done.
 */

static int
init_chip(struct hfc_multi *hc)
{
	u_long 	flags, val, val2 = 0, rev;
	int	i, err = 0;
	u_char	r_conf_en, rval;

	spin_lock_irqsave(&hc->lock, flags);
	/* reset all registers */
	memset(&hc->hw, 0, sizeof(struct hfcm_hw));

	/* revision check */
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: entered\n", __FUNCTION__);
	val = HFC_inb(hc, R_CHIP_ID)>>4;
	if (val != 0x8 && val != 0xc && val != 0xe) {
		printk(KERN_INFO "HFC_multi: unknown CHIP_ID:%x\n", (u_int)val);
		err = -EIO;
		goto out;
	}
	rev = HFC_inb(hc, R_CHIP_RV);
	printk(KERN_INFO
	    "HFC_multi: detected HFC with chip ID=0x%lx revision=%ld%s\n",
	    val, rev, (rev == 0) ? " (old FIFO handling)" : "");
	if (rev == 0) {
		test_and_set_bit(HFC_CHIP_REVISION0, &hc->chip);
		printk(KERN_WARNING
		    "HFC_multi: NOTE: Your chip is revision 0, "
		    "ask Cologne Chip for update. Newer chips "
		    "have a better FIFO handling. Old chips "
		    "still work but may have slightly lower "
		    "HDLC transmit performance.\n");
	}
	if (rev > 1) {
		printk(KERN_WARNING "HFC_multi: WARNING: This driver doesn't "
		    "consider chip revision = %ld. The chip / "
		    "bridge may not work.\n", rev);
	}

/* set s-ram size */
	hc->Flen = 0x10;
	hc->Zmin = 0x80;
	hc->Zlen = 384;
	hc->DTMFbase = 0x1000;
	if (test_bit(HFC_CHIP_EXRAM_128, &hc->chip)) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: changing to 128K extenal RAM\n",
			    __FUNCTION__);
		hc->hw.r_ctrl |= V_EXT_RAM;
		hc->hw.r_ram_sz = 1;
		hc->Flen = 0x20;
		hc->Zmin = 0xc0;
		hc->Zlen = 1856;
		hc->DTMFbase = 0x2000;
	}
	if (test_bit(HFC_CHIP_EXRAM_512, &hc->chip)) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: changing to 512K extenal RAM\n",
			    __FUNCTION__);
		hc->hw.r_ctrl |= V_EXT_RAM;
		hc->hw.r_ram_sz = 2;
		hc->Flen = 0x20;
		hc->Zmin = 0xc0;
		hc->Zlen = 8000;
		hc->DTMFbase = 0x2000;
	}

	/* we only want the real Z2 read-pointer for revision > 0 */
	if (!test_bit(HFC_CHIP_REVISION0, &hc->chip))
		hc->hw.r_ram_sz |= V_FZ_MD;

	/* select pcm mode */
	if (test_bit(HFC_CHIP_PCM_SLAVE, &hc->chip)) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: setting PCM into slave mode\n",
			    __FUNCTION__);
	} else
	if (test_bit(HFC_CHIP_PCM_MASTER, &hc->chip)) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: setting PCM into master mode\n",
			    __FUNCTION__);
		hc->hw.r_pcm_md0 |= V_PCM_MD;
	} else {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: performing PCM auto detect\n",
			    __FUNCTION__);
	}

	/* soft reset */
	HFC_outb(hc, R_CTRL, hc->hw.r_ctrl);
	HFC_outb(hc, R_RAM_SZ, hc->hw.r_ram_sz);
	HFC_outb(hc, R_FIFO_MD, 0);
	hc->hw.r_cirm = V_SRES | V_HFCRES | V_PCMRES | V_STRES | V_RLD_EPR;
	HFC_outb(hc, R_CIRM, hc->hw.r_cirm);
	udelay(100);
	hc->hw.r_cirm = 0;
	HFC_outb(hc, R_CIRM, hc->hw.r_cirm);
	udelay(100);
	HFC_outb(hc, R_RAM_SZ, hc->hw.r_ram_sz);


	/* PCM setup */
	HFC_outb(hc, R_PCM_MD0, hc->hw.r_pcm_md0 | 0x90);
	if (hc->slots == 32)
		HFC_outb(hc, R_PCM_MD1, 0x00);
	if (hc->slots == 64)
		HFC_outb(hc, R_PCM_MD1, 0x10);
	if (hc->slots == 128)
		HFC_outb(hc, R_PCM_MD1, 0x20);
	HFC_outb(hc, R_PCM_MD0, hc->hw.r_pcm_md0 | 0xa0);
	HFC_outb(hc, R_PCM_MD2, 0x00);
	HFC_outb(hc, R_PCM_MD0, hc->hw.r_pcm_md0 | 0x00);
	for (i = 0; i < 256; i++) {
		HFC_outb_(hc, R_SLOT, i);
		HFC_outb_(hc, A_SL_CFG, 0);
		HFC_outb_(hc, A_CONF, 0);
		hc->slot_owner[i] = -1;
	}

	/* set clock speed */
	if (test_bit(HFC_CHIP_CLOCK2, &hc->chip)) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: setting double clock\n", __FUNCTION__);
		HFC_outb(hc, R_BRG_PCM_CFG, V_PCM_CLK);
	}

#ifdef B410P_CARD
	if (test_bit(HFC_CHIP_B410P, &hc->chip)) {
		printk(KERN_NOTICE "Setting GPIOs\n");
		HFC_outb(hc, R_GPIO_SEL, 0x30);
		HFC_outb(hc, R_GPIO_EN1, 0x3);
		udelay(1000);
		printk(KERN_NOTICE "calling vpm_init\n");
		vpm_init(hc);
	}
#endif	

	/* check if R_F0_CNT counts (8 kHz frame count) */
	val = HFC_inb(hc, R_F0_CNTL);
	val += HFC_inb(hc, R_F0_CNTH) << 8;
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG
		    "HFC_multi F0_CNT %ld after reset\n", val);
	spin_unlock_irqrestore(&hc->lock, flags);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((HZ/100)?:1); /* Timeout minimum 10ms */
	spin_lock_irqsave(&hc->lock, flags);
	val2 = HFC_inb(hc, R_F0_CNTL);
	val2 += HFC_inb(hc, R_F0_CNTH) << 8;
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "HFC_multi F0_CNT %ld after 10 ms (1st try)\n",
		    val2);
	if (val2 >= val+8) { // 1 ms */
		/* it counts, so we keep the pcm mode */
		if (test_bit(HFC_CHIP_PCM_MASTER, &hc->chip))
			printk(KERN_INFO "controller is PCM bus MASTER\n");
		else
		if (test_bit(HFC_CHIP_PCM_SLAVE, &hc->chip))
			printk(KERN_INFO "controller is PCM bus SLAVE\n");
		else {
			test_and_set_bit(HFC_CHIP_PCM_SLAVE, &hc->chip);
			printk(KERN_INFO "controller is PCM bus SLAVE "
				"(auto detected)\n");
		}
	} else {
		/* does not count */
		if (test_bit(HFC_CHIP_PCM_MASTER, &hc->chip)) {
controller_fail:
			printk(KERN_ERR "HFC_multi ERROR, getting no 125us "
			    "pulse. Seems that controller fails.\n");
			err = -EIO;
			goto out;
		}
		if (test_bit(HFC_CHIP_PCM_SLAVE, &hc->chip)) {
			printk(KERN_INFO "controller is PCM bus SLAVE "
				"(ignoring missing PCM clock)\n");
		} else
		{
			/* retry with master clock */
			hc->hw.r_pcm_md0 |= V_PCM_MD;
			HFC_outb(hc, R_PCM_MD0, hc->hw.r_pcm_md0 | 0x00);
			spin_unlock_irqrestore(&hc->lock, flags);
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout((HZ/100)?:1); /* Timeout min. 10ms */
			spin_lock_irqsave(&hc->lock, flags);
			val2 = HFC_inb(hc, R_F0_CNTL);
			val2 += HFC_inb(hc, R_F0_CNTH) << 8;
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG "HFC_multi F0_CNT %ld after "
					"10 ms (2nd try)\n", val2);
			if (val2 >= val+8) { // 1 ms */
				test_and_set_bit(HFC_CHIP_PCM_MASTER,&hc->chip);
				if (!hc->pcm)
					PCM_cnt++;
				printk(KERN_INFO "controller is PCM bus MASTER "
					"(auto detected)\n");
			} else
				goto controller_fail;
		}
	}

	/* pcm id */
	if (hc->pcm)
		printk(KERN_INFO "controller has given PCM BUS ID %d\n",
			hc->pcm);
	else {
		hc->pcm = PCM_cnt;
		printk(KERN_INFO "controller has PCM BUS ID %d "
			"(auto selected)\n", hc->pcm);
	}

	/* set up timer */
	HFC_outb(hc, R_TI_WD, poll_timer);
	hc->hw.r_irqmsk_misc |= V_TI_IRQMSK;

	/*
	 * set up 125us interrupt, only if function pointer is available
	 * and module parameter timer is set
	 */
	if (timer && hfc_interrupt && register_interrupt) {
		/* only one chip should use this interrupt */
		timer = 0;
		interrupt_registered = 1;
		hc->hw.r_irqmsk_misc |= V_PROC_IRQMSK;
		/* deactivate other interrupts in ztdummy */
		register_interrupt();
	}

	/* set E1 state machine IRQ */
	if (hc->type == 1)
		hc->hw.r_irqmsk_misc |= V_STA_IRQMSK;

	/* set DTMF detection */
	if (test_bit(HFC_CHIP_DTMF, &hc->chip)) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: enabling DTMF detection "
			    "for all B-channel\n", __FUNCTION__);
		hc->hw.r_dtmf = V_DTMF_EN | V_DTMF_STOP;
		if (test_bit(HFC_CHIP_ULAW, &hc->chip))
			hc->hw.r_dtmf |= V_ULAW_SEL;
		HFC_outb(hc, R_DTMF_N, 102 - 1);
		hc->hw.r_irqmsk_misc |= V_DTMF_IRQMSK;
	}

	/* conference engine */
	if (test_bit(HFC_CHIP_ULAW, &hc->chip))
		r_conf_en = V_CONF_EN | V_ULAW;
	else
		r_conf_en = V_CONF_EN;
	HFC_outb(hc, R_CONF_EN, r_conf_en);

	/* setting leds */
	switch (hc->leds) {
	case 1: /* HFC-E1 OEM */
		if (test_bit(HFC_CHIP_WATCHDOG, &hc->chip))
			HFC_outb(hc, R_GPIO_SEL, 0x32);
		else
			HFC_outb(hc, R_GPIO_SEL, 0x30);

		HFC_outb(hc, R_GPIO_EN1, 0x0f);
		HFC_outb(hc, R_GPIO_OUT1, 0x00);

		HFC_outb(hc, R_GPIO_EN0, V_GPIO_EN2 | V_GPIO_EN3);
		break;

	case 2: /* HFC-4S OEM */
	case 3:
		HFC_outb(hc, R_GPIO_SEL, 0xf0);
		HFC_outb(hc, R_GPIO_EN1, 0xff);
		HFC_outb(hc, R_GPIO_OUT1, 0x00);
		break;
	}

	/* set master clock */
	if (hc->masterclk >= 0) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: setting ST master clock "
			    "to port %d (0..%d)\n",
			    __FUNCTION__, hc->masterclk, hc->ports-1);
		hc->hw.r_st_sync = hc->masterclk | V_AUTO_SYNC;
		HFC_outb(hc, R_ST_SYNC, hc->hw.r_st_sync);
	}

	/* setting misc irq */
	HFC_outb(hc, R_IRQMSK_MISC, hc->hw.r_irqmsk_misc);
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "r_irqmsk_misc.2: 0x%x\n",
		    hc->hw.r_irqmsk_misc);

	/* RAM access test */
	HFC_outb(hc, R_RAM_ADDR0, 0);
	HFC_outb(hc, R_RAM_ADDR1, 0);
	HFC_outb(hc, R_RAM_ADDR2, 0);
	for (i = 0; i < 256; i++) {
		HFC_outb_(hc, R_RAM_ADDR0, i);
		HFC_outb_(hc, R_RAM_DATA, ((i*3)&0xff));
	}
	for (i = 0; i < 256; i++) {
		HFC_outb_(hc, R_RAM_ADDR0, i);
		HFC_inb_(hc, R_RAM_DATA);
		rval = HFC_inb_(hc, R_INT_DATA);
		if(rval != ((i * 3) & 0xff)) {
			printk(KERN_DEBUG
			    "addr:%x val:%x should:%x\n", i, rval,
			    (i * 3) & 0xff);
			err++;
		}
	}
	if (err) {
		printk(KERN_DEBUG "aborting - %d RAM access errors\n", err);
		err = -EIO;
		goto out;
	}

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: done\n", __FUNCTION__);
out:
	spin_unlock_irqrestore(&hc->lock, flags);
	return err;
}


/*
 * control the watchdog
 */
static void
hfcmulti_watchdog(struct hfc_multi *hc)
{
	hc->wdcount++;

	if (hc->wdcount > 10) {
		hc->wdcount = 0;
		hc->wdbyte = hc->wdbyte == V_GPIO_OUT2 ?
		    V_GPIO_OUT3 : V_GPIO_OUT2;

	/* printk("Sending Watchdog Kill %x\n",hc->wdbyte); */
		HFC_outb(hc, R_GPIO_EN0, V_GPIO_EN2 | V_GPIO_EN3);
		HFC_outb(hc, R_GPIO_OUT0, hc->wdbyte);
	}
}



/*
 * output leds
 */
static void
hfcmulti_leds(struct hfc_multi *hc)
{
	unsigned long lled;
#ifndef CONFIG_HFCMULTI_PCIMEM
	unsigned long leddw;
#endif
	int i, state, active, leds;
	struct dchannel *dch;
	int led[4];

	hc->ledcount += poll;
	if (hc->ledcount > 4096) {
		hc->ledcount -= 4096;
		hc->ledstate = 0xAFFEAFFE;
	}

	switch(hc->leds) {
	case 1: /* HFC-E1 OEM */
		/* 2 red blinking: NT mode deactivate
		 * 2 red steady:   TE mode deactivate
		 * left green:     L1 active
		 * left red:       frame sync, but no L1
		 * right green:    L2 active
		 */
		if (hc->chan[hc->dslot].sync != 2) { /* no frame sync */
			if (hc->chan[hc->dslot].dch->dev.D.protocol
				!= ISDN_P_NT_E1)
				led[0] = led[1] = 1;
			else if (hc->ledcount>>11)
				led[0] = led[1] = 1;
			else
				led[0] = led[1] = 0;
			led[2] = led[3] = 0;
		} else { /* with frame sync */
			/* TODO make it work */
			led[0] = led[1] = 0;
			led[2] = 0;
			led[3] = 1;
		}
		leds = (led[0] | (led[1]<<2) | (led[2]<<1) | (led[3]<<3))^0xF; /* leds are inverted */
		if (leds != (int)hc->ledstate) {
			HFC_outb(hc, R_GPIO_OUT1, leds);
			hc->ledstate = leds;
		}
		break;

	case 2: /* HFC-4S OEM */
		/* red blinking = PH_DEACTIVATE NT Mode
		 * red steady   = PH_DEACTIVATE TE Mode 
		 * green steady = PH_ACTIVATE
		 */
		for (i=0; i < 4; i++) {
			state = 0;
			active = -1;
			dch = hc->chan[(i << 2) | 2].dch;
			if (dch) {
				state = dch->state;
				if (dch->dev.D.protocol == ISDN_P_NT_S0)
					active = 3;
				else
					active = 7;
			}
			if (state) {
				if (state == active) {
					led[i] = 1; /* led green */
				} else
					if (dch->dev.D.protocol == ISDN_P_TE_S0)
						/* TE mode: led red */
						led[i] = 2;
					else
						if (hc->ledcount>>11)
							/* led red */
							led[i] = 2;
						else
							/* led off */
							led[i] = 0;
			} else
				led[i] = 0; /* led off */
		}
#ifdef B410P_CARD
		if (test_bit(HFC_CHIP_B410P, &hc->chip)) {
			leds = 0;
			for (i = 0; i < 4; i++) {
				if (led[i]==1) {
					/*green*/
					leds |= (0x2 << (i * 2));
				} else if (led[i] == 2) {
					/*red*/
					leds |= (0x1 << (i * 2));
				}
			}
			if (leds != (int)hc->ledstate) {
				vpm_out(hc, 0, 0x1a8 + 3, leds);
				hc->ledstate = leds;
			}
		} else 
#endif
		{
			leds = ((led[3] > 0) << 0) | ((led[1] > 0) <<1 ) |
			    ((led[0] > 0) << 2) | ((led[2] > 0) << 3) |
			    ((led[3] & 1) << 4) | ((led[1] & 1) << 5) |
			    ((led[0] & 1) << 6) | ((led[2] & 1) << 7);
			if (leds != (int)hc->ledstate) {
				HFC_outb(hc, R_GPIO_EN1, leds & 0x0F);
				HFC_outb(hc, R_GPIO_OUT1, leds >> 4);
				hc->ledstate = leds;
			}
		}
		break;

	case 3: /* HFC 1S/2S Beronet */
		/* red blinking = PH_DEACTIVATE NT Mode
		 * red steady   = PH_DEACTIVATE TE Mode 
		 * green steady = PH_ACTIVATE
		 */
		for (i = 0; i < 2; i++) {
			state = 0;
			active = -1;
			dch = hc->chan[(i << 2) | 2].dch;
			if (dch) {
				state = dch->state;
				if (dch->dev.D.protocol == ISDN_P_NT_S0)
					active = 3;
				else
					active = 7;
			}
			if (state) {
				if (state == active) {
					led[i] = 1; /* led green */
				} else
					if (dch->dev.D.protocol == ISDN_P_TE_S0)
						/* TE mode: led red */
						led[i] = 2;
					else
						if (hc->ledcount >> 11)
							/* led red */
							led[i] = 2;
						else
							/* led off */
							led[i] = 0;
			} else
				led[i] = 0; /* led off */
		}


		leds = (led[0]>0) | ((led[1]>0)<<1) | ((led[0]&1)<<2) | ((led[1]&1)<<3);
		if (leds != (int)hc->ledstate) {
			HFC_outb(hc, R_GPIO_EN1,
			    ((led[0] > 0) << 2) | ((led[1] > 0) << 3));
			HFC_outb(hc, R_GPIO_OUT1,
			    ((led[0] & 1) << 2) | ((led[1] & 1) << 3));
			hc->ledstate = leds;
		}
		break;
	case 8: /* HFC 8S+ Beronet */
		lled = 0;

		for (i = 0; i < 8; i++) {
			state = 0;
			active = -1;
			dch = hc->chan[(i << 2) | 2].dch;
			if (dch) {
				state = dch->state;
				if (dch->dev.D.protocol == ISDN_P_NT_S0)
					active = 3;
				else
					active = 7;
			}
			if (state) {
				if (state==active) {
					lled |= 0 << i;
				} else 
					if (hc->ledcount >> 11)
						lled |= 0 << i;
					else
						lled |= 1 << i;
			} else
				lled |= 1 << i;
		}
#ifndef CONFIG_HFCMULTI_PCIMEM
		leddw = lled << 24 | lled << 16 | lled << 8 | lled;
		if (leddw != hc->ledstate) {
			// HFC_outb(hc, R_BRG_PCM_CFG, 1);
			// HFC_outb(c, R_BRG_PCM_CFG, (0x0 << 6) | 0x3);
			/* was _io before */
			HFC_outb(hc, R_BRG_PCM_CFG, 1 | V_PCM_CLK);
			outw(0x4000, hc->pci_iobase + 4);
			outl(leddw, hc->pci_iobase);
			HFC_outb(hc, R_BRG_PCM_CFG, V_PCM_CLK);
			hc->ledstate = leddw;
		}
#endif
		break;
	}
}
/*
 * read dtmf coefficients
 */

static void
hfcmulti_dtmf(struct hfc_multi *hc)
{
	signed int	coeff[16];
	u_int		mantissa;
	int		co, ch;
	struct bchannel	*bch = NULL;
	u8		exponent;
	int		dtmf = 0;
	int		addr;
	u16		w_float;
	struct sk_buff	*skb;
	struct mISDNhead *hh;

	if (debug & DEBUG_HFCMULTI_DTMF)
		printk(KERN_DEBUG "%s: dtmf detection irq\n", __FUNCTION__);
	for (ch = 0; ch < 32; ch++) {
		/* only process enabled B-channels */
		bch = hc->chan[ch].bch;
		if (!bch) {
			continue;
		}
		if (!hc->created[hc->chan[ch].port]) {
			continue;
		}
		if (!test_bit(FLG_TRANSPARENT, &bch->Flags)) {
			continue;
		}
		if (debug & DEBUG_HFCMULTI_DTMF)
			printk(KERN_DEBUG "%s: dtmf channel %d:",
				__FUNCTION__, ch);
		dtmf = 1;
		for (co = 0; co < 8; co++) {
			/* read W(n-1) coefficient */
			addr = hc->DTMFbase + ((co<<7) | (ch<<2));
			HFC_outb_(hc, R_RAM_ADDR0, addr);
			HFC_outb_(hc, R_RAM_ADDR1, addr>>8);
			HFC_outb_(hc, R_RAM_ADDR2, (addr>>16) | V_ADDR_INC);
			w_float = HFC_inb_(hc, R_RAM_DATA);
#ifdef CONFIG_HFCMULTI_PCIMEM
			w_float |= (HFC_inb_(hc, R_RAM_DATA) << 8);
#else
			w_float |= (HFC_getb(hc) << 8);
#endif
			if (debug & DEBUG_HFCMULTI_DTMF)
				printk(" %04x", w_float);

			/* decode float (see chip doc) */
			mantissa = w_float & 0x0fff;
			if (w_float & 0x8000)
				mantissa |= 0xfffff000;
			exponent = (w_float>>12) & 0x7;
			if (exponent) {
				mantissa ^= 0x1000;
				mantissa <<= (exponent-1);
			}

			/* store coefficient */
			coeff[co<<1] = mantissa;

			/* read W(n) coefficient */
			w_float = HFC_inb_(hc, R_RAM_DATA);
#ifdef CONFIG_HFCMULTI_PCIMEM
			w_float |= (HFC_inb_(hc, R_RAM_DATA) << 8);
#else
			w_float |= (HFC_getb(hc) << 8);
#endif
			if (debug & DEBUG_HFCMULTI_DTMF)
				printk(" %04x", w_float);

			/* decode float (see chip doc) */
			mantissa = w_float & 0x0fff;
			if (w_float & 0x8000)
				mantissa |= 0xfffff000;
			exponent = (w_float>>12) & 0x7;
			if (exponent) {
				mantissa ^= 0x1000;
				mantissa <<= (exponent-1);
			}

			/* store coefficient */
			coeff[(co<<1)|1] = mantissa;
		}
		skb = mI_alloc_skb(sizeof(coeff), GFP_ATOMIC);
		if (!skb) {
			printk(KERN_WARNING "%s: No memory for skb\n",
			    __FUNCTION__);
			continue;
		}
	        hh = mISDN_HEAD_P(skb);
	        hh->prim = PH_CONTROL_IND;
	        hh->id = DTMF_HFC_COEF;
		memcpy(skb_put(skb, sizeof(coeff)), coeff, sizeof(coeff));
		if (debug & DEBUG_HFCMULTI_DTMF) {
			printk("\n");
			printk("%s: DTMF ready %08x %08x %08x %08x "
			    "%08x %08x %08x %08x len=%d\n", __FUNCTION__,
			    coeff[0], coeff[1], coeff[2], coeff[3],
			    coeff[4], coeff[5], coeff[6], coeff[7], skb->len);
		}
		skb_queue_tail(&bch->rqueue, skb);
		schedule_event(bch, FLG_RECVQUEUE);
	}

	/* restart DTMF processing */
	hc->dtmf = dtmf;
	if (dtmf)
		HFC_outb_(hc, R_DTMF, hc->hw.r_dtmf | V_RST_DTMF);
}

#ifdef CONFIG_HFCMULTI_PCIMEM
/*
 * write fifo data
 */
void
write_fifo_data(struct hfc_multi *hc, BYTE *dest, int len)
{
	int i, remain;

	remain = len;

#ifdef FIFO_32BIT_ACCESS
	for (i = 0; i < len/4; i++) {
		HFC_outl_(hc, A_FIFO_DATA0, *((DWORD *)dest));
		remain -= 4;
		dest += 4;
	}
#endif

#ifdef FIFO_16BIT_ACCESS
	for (i = 0; i < len/2; i++) {
		HFC_outw_(hc, A_FIFO_DATA0, *((WORD *)dest));
		remain -= 2;
		dest += 2;
	}
#endif

	for (i = 0; i < remain; i++)
		HFC_outb_(hc, A_FIFO_DATA0, *dest++);

}
#endif

#ifndef CONFIG_HFCMULTI_PCIMEM
/*
 * write fifo data
 */
void
write_fifo_data(struct hfc_multi *hc, BYTE *dest, int len)
{
	int i, remain;

	remain = len;
	HFC_set(hc, A_FIFO_DATA0);

#ifdef FIFO_32BIT_ACCESS
	for (i = 0; i < len/4; i++) {
		HFC_putl(hc, *((DWORD *)dest));
		remain -= 4;
		dest += 4;
	}
#endif

#ifdef FIFO_16BIT_ACCESS
	for (i = 0; i < len / 2; i++) {
		HFC_putw(hc, *((WORD *)dest));
		remain -= 2;
		dest += 2;
	}
#endif

	for (i = 0; i < remain; i++)
		HFC_putb(hc, *dest++);
}
#endif

/*
 * fill fifo as much as possible
 */

static void
hfcmulti_tx(struct hfc_multi *hc, int ch)
{
	int i, ii, temp, len = 0;
	int Zspace, z1, z2;
	int Fspace, f1, f2;
	BYTE *d;
	int *txpending, slot_tx;
	struct	bchannel *bch;
	struct  dchannel *dch;
	struct  sk_buff **sp = NULL;
	int *idxp;

	bch = hc->chan[ch].bch;
	dch = hc->chan[ch].dch;
	if ((!dch) && (!bch))
		return;
	f1 = HFC_inb_(hc, A_F1);
	f2 = HFC_inb_(hc, A_F2);
	// printk(KERN_DEBUG "txf12:%x %x\n",f1,f2);
	/* get skb, fifo & mode */

	txpending = &hc->chan[ch].txpending;
	slot_tx = hc->chan[ch].slot_tx;
	if (dch) {
		sp = &dch->tx_skb;
		idxp = &dch->tx_idx;
	} else {
		sp = &bch->tx_skb;
		idxp = &bch->tx_idx;
	}
	if (*sp)
		len = (*sp)->len;

	if ((!len) && *txpending != 1)
		return; /* no data */

	// printk("debug: data: len = %d, *txpending = %d!!!!\n",
	// *len, *txpending);
	/* lets see how much data we will have left in buffer */
	if (test_bit(HFC_CHIP_B410P, &hc->chip) &&
	    (hc->chan[ch].protocol == ISDN_P_B_RAW) &&
	    (hc->chan[ch].slot_rx < 0) &&
	    (hc->chan[ch].slot_tx < 0))
		HFC_outb_(hc, R_FIFO, 0x20 | (ch << 1));
	else
		HFC_outb_(hc, R_FIFO, ch << 1);
	HFC_wait_(hc);

	if (*txpending == 2) {
		/* reset fifo */
		HFC_outb_(hc, R_INC_RES_FIFO, V_RES_F);
		HFC_wait_(hc);
		HFC_outb(hc, A_SUBCH_CFG, 0);
		*txpending = 1;
	}
next_frame:
	if (dch || test_bit(FLG_HDLC, &bch->Flags)) {
		f1 = HFC_inb_(hc, A_F1);
		f2 = HFC_inb_(hc, A_F2);
		while (f2 != (temp = HFC_inb_(hc, A_F2))) {
			if (debug & DEBUG_HFCMULTI_FIFO)
				printk(KERN_DEBUG
				    "%s: reread f2 because %d!=%d\n",
				    __FUNCTION__, temp, f2);
			f2 = temp; /* repeat until F2 is equal */
		}
		Fspace = f2 - f1 - 1;
		if (Fspace < 0)
			Fspace += hc->Flen;
		/*
		 * Old FIFO handling doesn't give us the current Z2 read
		 * pointer, so we cannot send the next frame before the fifo
		 * is empty. It makes no difference except for a slightly
		 * lower performance.
		 */
		if (test_bit(HFC_CHIP_REVISION0, &hc->chip)) {
			if (f1 != f2)
				Fspace = 0;
			else
				Fspace = 1;
		}
		/* one frame only for ST D-channels, to allow resending */
		if (hc->type != 1 && dch) {
			if (f1 != f2)
				Fspace = 0;
		}
		/* F-counter full condition */
		if (Fspace == 0)
			return;
	}
	z1 = HFC_inw_(hc, A_Z1) - hc->Zmin;
	z2 = HFC_inw_(hc, A_Z2) - hc->Zmin;
	while (z2 != (temp = (HFC_inw_(hc, A_Z2) - hc->Zmin))) {
		if (debug & DEBUG_HFCMULTI_FIFO)
			printk(KERN_DEBUG "%s: reread z2 because %d!=%d\n",
				__FUNCTION__, temp, z2);
		z2 = temp; /* repeat unti Z2 is equal */
	}
	Zspace = z2 - z1 - 1;
	if (Zspace < 0)
		Zspace += hc->Zlen;
	/* buffer too full, there must be at least one more byte for 0-volume */
	if (Zspace < 4) /* just to be safe */
		return;

	/* if no data */
	if (!len) {
		if (z1 == z2) { /* empty */
			/* if done with FIFO audio data during PCM connection */
			if (bch && (!test_bit(FLG_HDLC, &bch->Flags)) &&
			    *txpending && slot_tx >= 0) {
				if (debug & DEBUG_HFCMULTI_MODE)
					printk(KERN_DEBUG
					    "%s: reconnecting PCM due to no "
					    "more FIFO data: channel %d "
					    "slot_tx %d\n",
					    __FUNCTION__, ch, slot_tx);
				/* connect slot */
				HFC_outb(hc, A_CON_HDLC, 0xc0 | 0x00 |
				    V_HDLC_TRP | V_IFF);
				HFC_outb_(hc, R_FIFO, ch<<1 | 1);
				HFC_wait_(hc);
				HFC_outb(hc, A_CON_HDLC, 0xc0 | 0x00 |
				    V_HDLC_TRP | V_IFF);
				HFC_outb_(hc, R_FIFO, ch<<1);
				HFC_wait_(hc);
			}
			*txpending = 0;
		}
		return; /* no data */
	}

	/* if audio data and connected slot */
	if (bch && (!test_bit(FLG_HDLC, &bch->Flags)) && (!*txpending) && slot_tx >= 0) {
		if (debug & DEBUG_HFCMULTI_MODE)
			printk(KERN_DEBUG "%s: disconnecting PCM due to "
			    "FIFO data: channel %d slot_tx %d\n",
			    __FUNCTION__, ch, slot_tx);
		/* disconnect slot */
		HFC_outb(hc, A_CON_HDLC, 0x80 | 0x00 | V_HDLC_TRP | V_IFF);
		HFC_outb_(hc, R_FIFO, ch<<1 | 1);
		HFC_wait_(hc);
		HFC_outb(hc, A_CON_HDLC, 0x80 | 0x00 | V_HDLC_TRP | V_IFF);
		HFC_outb_(hc, R_FIFO, ch<<1);
		HFC_wait_(hc);
	}
	*txpending = 1;

	/* show activity */
	hc->activity[hc->chan[ch].port] = 1;

	/* fill fifo to what we have left */
	ii = len;
	if (dch || test_bit(FLG_HDLC, &bch->Flags))
		temp = 1;
	else
		temp = 0;
	i = *idxp;
	d = (*sp)->data + i;
	if (ii - i > Zspace)
		ii = Zspace + i;
	if (debug & DEBUG_HFCMULTI_FIFO)
		printk(KERN_DEBUG "%s: fifo(%d) has %d bytes space left "
		    "(z1=%04x, z2=%04x) sending %d of %d bytes %s\n",
			__FUNCTION__, ch, Zspace, z1, z2, ii-i, len-i,
			temp ? "HDLC":"TRANS");

	/* Have to prep the audio data */
	write_fifo_data(hc, d, ii - i);
	*idxp = ii;

	/* if not all data has been written */
	if (ii != len) {
		/* NOTE: fifo is started by the calling function */
		return;
	}

	/* if all data has been written, terminate frame */
	if (dch || test_bit(FLG_HDLC, &bch->Flags)) {
		/* increment f-counter */
		HFC_outb_(hc, R_INC_RES_FIFO, V_INC_F);
		HFC_wait_(hc);
	}

	/* check for next frame */
	dev_kfree_skb(*sp);
	if (bch && get_next_bframe(bch))
		goto next_frame;
	if (dch && get_next_dframe(dch))
		goto next_frame;

	/*
	 * now we have no more data, so in case of transparent,
	 * we set the last byte in fifo to 'silence' in case we will get
	 * no more data at all. this prevents sending an undefined value.
	 */
	if (bch && !test_bit(FLG_HDLC, &bch->Flags))
		HFC_outb_(hc, A_FIFO_DATA0_NOINC, silence);
}


#ifdef CONFIG_HFCMULTI_PCIMEM
/*
 * read fifo data
 */
void
read_fifo_data(struct hfc_multi *hc, BYTE *dest, int len)
{
	int i, remain;

	remain = len;

#ifdef FIFO_32BIT_ACCESS
	for (i = 0; i < len / 4; i++) {
		*((DWORD *)dest) = HFC_inl_(hc, A_FIFO_DATA0);
		remain -= 4;
		dest += 4;
	}
#endif

#ifdef FIFO_16BIT_ACCESS
	for (i = 0; i < len / 2; i++) {
		*((WORD *)dest) = HFC_inw_(hc, A_FIFO_DATA0);
		remain -= 2;
		dest += 2;
	}
#endif

	for (i = 0; i < remain; i++)
		*dest++ = HFC_inb_(hc, A_FIFO_DATA0);

}
#endif

#ifndef CONFIG_HFCMULTI_PCIMEM
/*
 * read fifo data
 */
void
read_fifo_data(struct hfc_multi *hc, BYTE *dest, int len)
{
	int i, remain;

	remain = len;
	HFC_set(hc, A_FIFO_DATA0);

#ifdef FIFO_32BIT_ACCESS
	for (i = 0; i < len / 4; i++) {
		*((DWORD *)dest) = HFC_getl(hc);
		remain -= 4;
		dest += 4;
	}
#endif

#ifdef FIFO_16BIT_ACCESS
	for (i = 0; i < len / 2; i++) {
		*((WORD *)dest) = HFC_getw(hc);
		remain -= 2;
		dest += 2;
	}
#endif

	for (i = 0; i < remain; i++)
		*dest++ = HFC_getb(hc);
}
#endif


static void
hfcmulti_rx(struct hfc_multi *hc, int ch)
{
	int temp;
	int Zsize, z1, z2 = 0; /* = 0, to make GCC happy */
	int f1 = 0, f2 = 0; /* = 0, to make GCC happy */
	struct	bchannel *bch;
	struct  dchannel *dch;	
	struct sk_buff 	*skb, **sp = NULL;
	int	maxlen;

	/* lets see how much data we received */
	if (test_bit(HFC_CHIP_B410P, &hc->chip) &&
	    (hc->chan[ch].protocol == ISDN_P_B_RAW) &&
	    (hc->chan[ch].slot_rx < 0) &&
	    (hc->chan[ch].slot_tx < 0))
		HFC_outb_(hc, R_FIFO, 0x20 | (ch<<1) | 1);
	else
		HFC_outb_(hc, R_FIFO, (ch<<1)|1);
	HFC_wait_(hc);
	bch = hc->chan[ch].bch;
	dch = hc->chan[ch].dch;
	if ((!dch) && (!bch))
		return;
next_frame:
	if (dch || test_bit(FLG_HDLC, &bch->Flags)) {
		f1 = HFC_inb_(hc, A_F1);
		while (f1 != (temp = HFC_inb_(hc, A_F1))) {
			if (debug & DEBUG_HFCMULTI_FIFO)
				printk(KERN_DEBUG
				    "%s: reread f1 because %d!=%d\n",
				    __FUNCTION__, temp, f1);
			f1 = temp; /* repeat until F1 is equal */
		}
		f2 = HFC_inb_(hc, A_F2);
	}
	// if(f1!=f2) printk(KERN_DEBUG
	//    "got a chan:%d framef12:%x %x!!!!\n",ch,f1,f2);
	// printk(KERN_DEBUG "got a chan:%d framef12:%x %x!!!!\n",ch,f1,f2);
	z1 = HFC_inw_(hc, A_Z1) - hc->Zmin;
	while (z1 != (temp = (HFC_inw_(hc, A_Z1) - hc->Zmin))) {
		if (debug & DEBUG_HFCMULTI_FIFO)
			printk(KERN_DEBUG "%s: reread z2 because %d!=%d\n",
			    __FUNCTION__, temp, z2);
		z1 = temp; /* repeat until Z1 is equal */
	}
	z2 = HFC_inw_(hc, A_Z2) - hc->Zmin;
	Zsize = z1 - z2;
	if ((dch || test_bit(FLG_HDLC, &bch->Flags)) && f1 != f2)
		/* complete hdlc frame */
		Zsize++;
	if (Zsize < 0)
		Zsize += hc->Zlen;
	/* if buffer is empty */
	if (Zsize <= 0)
		return;

	if (dch) {
		sp = &dch->rx_skb;
		maxlen = dch->maxlen;
	} else {
		sp = &bch->rx_skb;
		maxlen = bch->maxlen;
	}
	if (*sp == NULL) {
		*sp = mI_alloc_skb(maxlen + 3, GFP_ATOMIC);
		if (*sp == NULL) {
			printk(KERN_DEBUG "%s: No mem for rx_skb\n",
			    __FUNCTION__);
			return;
		}
	}
	/* show activity */
	hc->activity[hc->chan[ch].port] = 1;

	/* empty fifo with what we have */
	if (dch || test_bit(FLG_HDLC, &bch->Flags)) {
		if (debug & DEBUG_HFCMULTI_FIFO)
			printk(KERN_DEBUG "%s: fifo(%d) reading %d bytes (z1="
			    "%04x, z2=%04x) HDLC %s (f1=%d, f2=%d) got=%d\n",
				__FUNCTION__, ch, Zsize, z1, z2,
				(f1 == f2) ? "fragment" : "COMPLETE",
				f1, f2, Zsize + (*sp)->len);
		/* HDLC */
		if ((Zsize + (*sp)->len) > (maxlen + 3)) {
			if (debug & DEBUG_HFCMULTI_FIFO)
				printk(KERN_DEBUG
				    "%s: hdlc-frame too large.\n",
				    __FUNCTION__);
			skb_trim(*sp, 0);
			HFC_outb_(hc, R_INC_RES_FIFO, V_RES_F);
			HFC_wait_(hc);
			return;
		}

		read_fifo_data(hc, skb_put(*sp, Zsize), Zsize);

		if (f1 != f2) {
			/* increment Z2,F2-counter */
			HFC_outb_(hc, R_INC_RES_FIFO, V_INC_F);
			HFC_wait_(hc);
			/* check size */
			if ((*sp)->len < 4) {
				if (debug & DEBUG_HFCMULTI_FIFO)
					printk(KERN_DEBUG
					    "%s: Frame below minimum size\n",
					    __FUNCTION__);
				skb_trim(*sp, 0);
				goto next_frame;
			}
			/* there is at least one complete frame, check crc */
			if ((*sp)->data[(*sp)->len - 1]) {
				if (debug & DEBUG_HFCMULTI_CRC)
					printk(KERN_DEBUG
					    "%s: CRC-error\n", __FUNCTION__);
				skb_trim(*sp, 0);
				goto next_frame;
			}
			/* only send dchannel if in active state */
			if (dch && hc->type == 1 && 
			    hc->chan[ch].e1_state != 1) {
				skb_trim(*sp, 0);
				goto next_frame;
			}
			skb_trim(*sp, (*sp)->len - 3);
			if ((*sp)->len < MISDN_COPY_SIZE) {
				skb = *sp;
				*sp = mI_alloc_skb(skb->len, GFP_ATOMIC);
				if (*sp) {
					memcpy(skb_put(*sp, skb->len),
					    skb->data, skb->len);
					skb_trim(skb, 0);
				} else {
					skb = NULL;
				}
			} else {
				skb = NULL;
			}
			if (debug & DEBUG_HFCMULTI_FIFO) {
				temp = 0;
				while (temp < (*sp)->len)
					printk("%02x ", (*sp)->data[temp++]);
				printk("\n");
			}
			if (dch)
				recv_Dchannel(dch);
			if (bch)
				recv_Bchannel(bch);
			*sp = skb;
			goto next_frame;
		}
		/* there is an incomplete frame */
	} else {
		/* transparent */
		if (Zsize > skb_tailroom(*sp))
			Zsize = skb_tailroom(*sp);
		if (((*sp)->len + Zsize) < MISDN_COPY_SIZE) {
			skb = *sp;
			*sp = mI_alloc_skb(skb->len, GFP_ATOMIC);
			if (*sp) {
				memcpy(skb_put(*sp, skb->len),
				    skb->data, skb->len);
				skb_trim(skb, 0);
			} else {
				skb = NULL;
			}
		} else {
			skb = NULL;
		}
		if (debug & DEBUG_HFCMULTI_FIFO)
			printk(KERN_DEBUG
			    "%s: fifo(%d) reading %d bytes "
			    "(z1=%04x, z2=%04x) TRANS\n",
				__FUNCTION__, ch, Zsize, z1, z2);
		read_fifo_data(hc, skb_put(*sp, Zsize), Zsize);
		/* only bch is transparent */
		recv_Bchannel(bch);
		*sp = skb;
	}
}


/*
 * Interrupt handler
 */
static void
signal_state_up(struct dchannel *dch, int info, char *msg)
{
	struct sk_buff	*skb;
	int		id, data = info;

	if (debug & DEBUG_HFCMULTI_STATE)
		printk(KERN_DEBUG "%s: %s\n", __FUNCTION__, msg);

	id = TEI_SAPI | (GROUP_TEI << 8); /* manager address */

	skb = _alloc_mISDN_skb(MPH_INFORMATION_IND, id, sizeof(data), &data,
		GFP_ATOMIC);
	if (!skb)
		return;
	skb_queue_tail(&dch->rqueue, skb);
	schedule_event(dch, FLG_RECVQUEUE);
}

static inline void
handle_timer_irq(struct hfc_multi *hc)
{
	int		ch, temp;
	struct dchannel	*dch;

	for (ch = 0; ch < 32; ch++) {
		if (hc->created[hc->chan[ch].port]) {
			hfcmulti_tx(hc, ch);
			/* fifo is started when switching to rx-fifo */
			hfcmulti_rx(hc, ch);
			if (hc->chan[ch].dch &&
			    hc->chan[ch].nt_timer > -1) {
				dch = hc->chan[ch].dch;
				if (!(--hc->chan[ch].nt_timer)) {
					schedule_event(dch,
					    FLG_PHCHANGE);
					if (debug &
					    DEBUG_HFCMULTI_STATE)
						printk(KERN_DEBUG
						    "%s: nt_timer at "
						    "state %x\n",
						    __FUNCTION__,
						    dch->state);
				}
			}
		}
	}
	if (hc->type == 1 && hc->created[0]) {
		dch = hc->chan[hc->dslot].dch;
		if (test_bit(HFC_CFG_REPORT_LOS, &hc->chan[hc->dslot].cfg)) {
			if (debug & DEBUG_HFCMULTI_SYNC)
				printk(KERN_DEBUG
				    "%s: (id=%d) E1 got LOS\n",
				    __FUNCTION__, hc->id);
			/* LOS */
			temp = HFC_inb_(hc, R_RX_STA0) & V_SIG_LOS;
			if (!temp && hc->chan[hc->dslot].los)
				signal_state_up(dch, L1_SIGNAL_LOS_ON,
				    "LOS detected");
			if (temp && !hc->chan[hc->dslot].los)
				signal_state_up(dch, L1_SIGNAL_LOS_OFF,
				    "LOS gone");
			hc->chan[hc->dslot].los = temp;
		}
		if (test_bit(HFC_CFG_REPORT_AIS, &hc->chan[hc->dslot].cfg)) {
			if (debug & DEBUG_HFCMULTI_SYNC)
				printk(KERN_DEBUG
				    "%s: (id=%d) E1 got AIS\n",
				    __FUNCTION__, hc->id);
			/* AIS */
			temp = HFC_inb_(hc, R_RX_STA0) & V_AIS;
			if (!temp && hc->chan[hc->dslot].ais)
				signal_state_up(dch, L1_SIGNAL_AIS_ON,
				    "AIS detected");
			if (temp && !hc->chan[hc->dslot].ais)
				signal_state_up(dch, L1_SIGNAL_AIS_OFF,
				    "AIS gone");
			hc->chan[hc->dslot].ais = temp;
		}
		if (test_bit(HFC_CFG_REPORT_SLIP, &hc->chan[hc->dslot].cfg)) {
			if (debug & DEBUG_HFCMULTI_SYNC)
				printk(KERN_DEBUG
				    "%s: (id=%d) E1 got SLIP (RX)\n",
				    __FUNCTION__, hc->id);
			/* SLIP */
			temp = HFC_inb_(hc, R_SLIP) & V_FOSLIP_RX;
			if (!temp && hc->chan[hc->dslot].slip_rx)
				signal_state_up(dch, L1_SIGNAL_SLIP_RX,
				    " bit SLIP detected RX");
			hc->chan[hc->dslot].slip_rx = temp;
			temp = HFC_inb_(hc, R_SLIP) & V_FOSLIP_TX;
			if (!temp && hc->chan[hc->dslot].slip_tx)
				signal_state_up(dch, L1_SIGNAL_SLIP_TX,
				    " bit SLIP detected TX");
			hc->chan[hc->dslot].slip_tx = temp;
		}
		temp = HFC_inb_(hc, R_JATT_DIR);
		switch (hc->chan[hc->dslot].sync) {
		case 0:
			if ((temp & 0x60) == 0x60) {
				if (debug & DEBUG_HFCMULTI_SYNC)
					printk(KERN_DEBUG
					    "%s: (id=%d) E1 now "
					    "in clock sync\n",
					    __FUNCTION__, hc->id);
				HFC_outb(hc, R_RX_OFF,
				    hc->chan[hc->dslot].jitter | V_RX_INIT);
				HFC_outb(hc, R_TX_OFF,
				    hc->chan[hc->dslot].jitter | V_RX_INIT);
				hc->chan[hc->dslot].sync = 1;
				goto check_framesync;
			}
			break;
		case 1:
			if ((temp & 0x60) != 0x60) {
				if (debug & DEBUG_HFCMULTI_SYNC)
					printk(KERN_DEBUG
					    "%s: (id=%d) E1 "
					    "lost clock sync\n",
					    __FUNCTION__, hc->id);
				hc->chan[hc->dslot].sync = 0;
				break;
			}
check_framesync:
			temp = HFC_inb_(hc, R_RX_STA0);
			if (temp == 0x27) {
				if (debug & DEBUG_HFCMULTI_SYNC)
					printk(KERN_DEBUG
					    "%s: (id=%d) E1 "
					    "now in frame sync\n",
					    __FUNCTION__, hc->id);
				hc->chan[hc->dslot].sync = 2;
			}
			break;
		case 2:
			if ((temp & 0x60) != 0x60) {
				if (debug & DEBUG_HFCMULTI_SYNC)
					printk(KERN_DEBUG
					    "%s: (id=%d) E1 lost "
					    "clock & frame sync\n",
					    __FUNCTION__, hc->id);
				hc->chan[hc->dslot].sync = 0;
				break;
			}
			temp = HFC_inb_(hc, R_RX_STA0);
			if (temp != 0x27) {
				if (debug & DEBUG_HFCMULTI_SYNC)
					printk(KERN_DEBUG
					    "%s: (id=%d) E1 "
					    "lost frame sync\n",
					    __FUNCTION__, hc->id);
				hc->chan[hc->dslot].sync = 1;
			}
			break;
		}
	}

	if (test_bit(HFC_CHIP_WATCHDOG, &hc->chip))
		hfcmulti_watchdog(hc);

	if (hc->leds)
		hfcmulti_leds(hc);
}

static void
ph_state_irq(struct hfc_multi *hc, u_char r_irq_statech)
{
	struct dchannel	*dch;
	int		ch;
	int		active;

	/* state machine */
	for (ch = 0; ch < 32; ch++) {
		if (hc->chan[ch].dch) {
			dch = hc->chan[ch].dch;
			if (r_irq_statech & 1) {
				HFC_outb_(hc, R_ST_SEL, hc->chan[ch].port);
				dch->state = HFC_inb(hc, A_ST_RD_STATE) & 0x0f;
				if (dch->dev.D.protocol == ISDN_P_NT_S0)
					active = 3;
				else
					active = 7;
				if (dch->state == active) {
					HFC_outb_(hc, R_FIFO, (ch << 1) | 1);
					HFC_wait_(hc);
					HFC_outb_(hc, R_INC_RES_FIFO,V_RES_F);
					HFC_wait_(hc);
					dch->tx_idx = 0;
				}
				schedule_event(dch, FLG_PHCHANGE);
				if (debug & DEBUG_HFCMULTI_STATE)
					printk(KERN_DEBUG 
					    "%s: S/T newstate %x port %d\n",
					    __FUNCTION__, dch->state,
					    hc->chan[ch].port);
			}
			r_irq_statech >>= 1;
		}
	}
}

static void
fifo_irq(struct hfc_multi *hc, int block)
{
	int	ch, j;
	struct dchannel	*dch;
	struct bchannel	*bch;
	u_char r_irq_fifo_bl;

	r_irq_fifo_bl = HFC_inb_(hc, R_IRQ_FIFO_BL0 + block);
	j = 0;
	while (j < 8) {
		ch = (block << 2) + (j >> 1);
		dch = hc->chan[ch].dch;
		bch = hc->chan[ch].bch;
		if (((!dch) && (!bch)) || (!hc->created[hc->chan[ch].port])) {
			j += 2;
			continue;
		}
//if (r_irq_fifo_bl & (1<<j)) printk(KERN_DEBUG "tx-fifo %d ready\n", ch);
		if (dch && (r_irq_fifo_bl & (1 << j)) &&
		    test_bit(FLG_ACTIVE, &dch->Flags)) {
			hfcmulti_tx(hc, ch);
			/* start fifo */
			HFC_outb_(hc, R_FIFO, 0);
			HFC_wait_(hc);
		}
		if (bch && (r_irq_fifo_bl & (1 << j)) &&
		    test_bit(FLG_ACTIVE, &bch->Flags)) {
			hfcmulti_tx(hc, ch);
			/* start fifo */
			HFC_outb_(hc, R_FIFO, 0);
			HFC_wait_(hc);
		}
		j++;
//if (r_irq_fifo_bl & (1<<j)) printk(KERN_DEBUG "rx-fifo %d ready\n", ch);
		if (dch && (r_irq_fifo_bl & (1 << j)) &&
		    test_bit(FLG_ACTIVE, &dch->Flags)) {
			hfcmulti_rx(hc, ch);
		}
		if (bch && (r_irq_fifo_bl & (1 << j)) &&
		    test_bit(FLG_ACTIVE, &bch->Flags)) {
			hfcmulti_rx(hc, ch);
		}
		j++;
	}
}

//int irqsem=0;
static irqreturn_t
hfcmulti_interrupt(int intno, void *dev_id)
{
#ifdef IRQCOUNT_DEBUG
	static int iq1 = 0, iq2 = 0, iq3 = 0, iq4 = 0,
	    iq5 = 0, iq6 = 0, iqcnt = 0;
#endif
	static int count = 0;
	struct hfc_multi	*hc = dev_id;
	struct dchannel	*dch;
	u_char 		r_irq_statech, status, r_irq_misc, r_irq_oview;
	int		i;
	// u_char bl1,bl2;
#ifdef CONFIG_PLX_PCI_BRIDGE
	u_short *plx_acc, wval;
#endif

#ifdef CONFIG_PLX_PCI_BRIDGE
	plx_acc = (u_short*)(hc->plx_membase + 0x4c);
	wval = *plx_acc;
	if (!(wval & 0x04)) {
		if (wval & 0x20) {
			// printk(KERN_WARNING "NO irq  LINTI1:%x\n",wval);
			printk(KERN_WARNING "got irq  LINTI2\n");
		}
		return IRQ_NONE;
	}
#endif

	spin_lock(&hc->lock);
//	if (irqsem) printk(KERN_DEBUG "oops... irq for card %d during irq from card %d", hc->id, irqsem);
//	irqsem = hc->id;

	if (!hc) {
		printk(KERN_WARNING "HFC-multi: Spurious interrupt!\n");
		irq_notforus:

#ifdef CONFIG_PLX_PCI_BRIDGE
	// plx_acc=(u_short*)(hc->plx_membase+0x4c);
	// *plx_acc=0xc00;  // clear LINTI1 & LINTI2
	// *plx_acc=0xc41;
#endif
//		irqsem = 0;
		spin_unlock(&hc->lock);
		return IRQ_NONE;
	}
	status = HFC_inb_(hc, R_STATUS);
	r_irq_statech = HFC_inb_(hc, R_IRQ_STATECH);
#ifdef IRQCOUNT_DEBUG
	if (r_irq_statech)
		iq1++;
	if (status & V_DTMF_STA)
		iq2++;
	if (status & V_LOST_STA)
		iq3++;
	if (status & V_EXT_IRQSTA)
		iq4++;
	if (status & V_MISC_IRQSTA)
		iq5++;
	if (status & V_FR_IRQSTA)
		iq6++;
	if (iqcnt++ > 5000) {
		printk(KERN_ERR "iq1:%x iq2:%x iq3:%x iq4:%x iq5:%x iq6:%x\n",
		    iq1, iq2, iq3, iq4, iq5, iq6);
		iqcnt = 0;
	}
#endif
	if (!r_irq_statech &&
	    !(status & (V_DTMF_STA | V_LOST_STA | V_EXT_IRQSTA |
	    V_MISC_IRQSTA | V_FR_IRQSTA))) {
		/* irq is not for us */
		// if(status) printk(KERN_WARNING "nofus:%x\n",status);
		goto irq_notforus;
	}
	hc->irqcnt++;
	if (r_irq_statech) {
		if (hc->type != 1)
			ph_state_irq(hc, r_irq_statech);
	}
	if (status & V_EXT_IRQSTA) {
		/* external IRQ */
	}
	if (status & V_LOST_STA) {
		/* LOST IRQ */
		HFC_outb(hc, R_INC_RES_FIFO, V_RES_LOST); /* clear irq! */
	}
	if (status & V_MISC_IRQSTA) {
		/* misc IRQ */
		r_irq_misc = HFC_inb_(hc, R_IRQ_MISC);
		if (r_irq_misc & V_STA_IRQ) {
			if (hc->type == 1) {
				/* state machine */
				dch = hc->chan[hc->dslot].dch;
				dch->state = HFC_inb_(hc, R_E1_RD_STA) & 0x7;
				schedule_event(dch, FLG_PHCHANGE);
				if (debug & DEBUG_HFCMULTI_STATE)
					printk(KERN_DEBUG
					    "%s: E1 (id=%d) newstate %x\n",
					    __FUNCTION__, hc->id ,dch->state);
			}
		}
		if (r_irq_misc & V_TI_IRQ)
			handle_timer_irq(hc);

		if (r_irq_misc & V_DTMF_IRQ) {
			/* -> DTMF IRQ */
			hfcmulti_dtmf(hc);
		}
#warning V_IRQ_PROC 125 us interrupt is not acceptable chip allows 1 khz timer
		if (r_irq_misc & V_IRQ_PROC) { /* TODO: REPLACE !!!! 125 us Interrupts are not acceptable  */
			/* IRQ every 125us */
			count++;
			/* generate 1kHz signal */
			if (count == 8) {
				if (hfc_interrupt)
					hfc_interrupt();
				count = 0;
			}
		}

	}
	if (status & V_FR_IRQSTA) {
		/* FIFO IRQ */
		r_irq_oview = HFC_inb_(hc, R_IRQ_OVIEW);
		// if(r_irq_oview) printk(KERN_DEBUG "OV:%x\n",r_irq_oview);
		for (i = 0; i < 8; i++) {
			if (r_irq_oview & (1 << i))
				fifo_irq(hc, i);
		}
	}

#ifdef CONFIG_PLX_PCI_BRIDGE
	// plx_acc=(u_short*)(hc->plx_membase+0x4c);
	// *plx_acc=0xc00;  // clear LINTI1 & LINTI2
	// *plx_acc=0xc41;
#endif
//	irqsem = 0;
	spin_unlock(&hc->lock);
	return IRQ_HANDLED;
}


/*
 * timer callback for D-chan busy resolution. Currently no function
 */

static void
hfcmulti_dbusy_timer(struct hfc_multi *hc)
{
}


/*
 * activate/deactivate hardware for selected channels and mode
 *
 * configure B-channel with the given protocol
 * ch eqals to the HFC-channel (0-31)
 * ch is the number of channel (0-4,4-7,8-11,12-15,16-19,20-23,24-27,28-31
 * for S/T, 1-31 for E1)
 * the hdlc interrupts will be set/unset
 */
static int
mode_hfcmulti(struct hfc_multi *hc, int ch, int protocol, int slot_tx,
    int bank_tx, int slot_rx, int bank_rx)
{
	int flow_tx = 0, flow_rx = 0, routing = 0;
	int oslot_tx = hc->chan[ch].slot_tx;
	int oslot_rx = hc->chan[ch].slot_rx;
	int conf = hc->chan[ch].conf;

	if (debug & DEBUG_HFCMULTI_MODE)
		printk(KERN_DEBUG
		    "%s: card %d channel %d protocol %x slot old=%d new=%d"
		    "bank new=%d (TX) slot old=%d new=%d bank new=%d (RX)\n",
		    __FUNCTION__, hc->id, ch, protocol, oslot_tx, slot_tx,
		    bank_tx, oslot_rx, slot_rx, bank_rx);

	if (oslot_tx >= 0 && slot_tx != oslot_tx) {
		/* remove from slot */
		if (debug & DEBUG_HFCMULTI_MODE)
			printk(KERN_DEBUG "%s: remove from slot %d (TX)\n",
			    __FUNCTION__, oslot_tx);
		if (hc->slot_owner[oslot_tx<<1] == ch) {
			HFC_outb(hc, R_SLOT, oslot_tx << 1);
			HFC_outb(hc, A_SL_CFG, 0);
			HFC_outb(hc, A_CONF, 0);
			hc->slot_owner[oslot_tx<<1] = -1;
		} else {
			if (debug & DEBUG_HFCMULTI_MODE)
				printk(KERN_DEBUG
				    "%s: we are not owner of this tx slot "
				    "anymore, channel %d is.\n",
				    __FUNCTION__, hc->slot_owner[oslot_tx<<1]);
		}
	}

	if (oslot_rx >= 0 && slot_rx != oslot_rx) {
		/* remove from slot */
		if (debug & DEBUG_HFCMULTI_MODE)
			printk(KERN_DEBUG
			    "%s: remove from slot %d (RX)\n",
			    __FUNCTION__, oslot_rx);
		if (hc->slot_owner[(oslot_rx << 1) | 1] == ch) {
			HFC_outb(hc, R_SLOT, (oslot_rx << 1) | V_SL_DIR);
			HFC_outb(hc, A_SL_CFG, 0);
			hc->slot_owner[(oslot_rx << 1) | 1] = -1;
		} else {
			if (debug & DEBUG_HFCMULTI_MODE)
				printk(KERN_DEBUG
				    "%s: we are not owner of this rx slot "
				    "anymore, channel %d is.\n",
				    __FUNCTION__,
				    hc->slot_owner[(oslot_rx << 1) | 1]);
		}
	}

	if (slot_tx < 0) {
		flow_tx = 0x80; /* FIFO->ST */
		/* disable pcm slot */
		hc->chan[ch].slot_tx = -1;
		hc->chan[ch].bank_tx = -1;
	} else {
		/* set pcm slot */
		if (hc->chan[ch].txpending)
			flow_tx = 0x80; /* FIFO->ST */
		else
			flow_tx = 0xc0; /* PCM->ST */
		/* put on slot */
		routing = bank_tx ? 0xc0 : 0x80;
		if (conf >= 0 || bank_tx > 1)
			routing = 0x40; /* loop */
		if (debug & DEBUG_HFCMULTI_MODE)
			printk(KERN_DEBUG "%s: put channel %d to slot %d bank"
			    "%d flow %02x routing %02x conf %d (TX)\n",
			    __FUNCTION__, ch, slot_tx, bank_tx,
			    flow_tx, routing, conf);
		HFC_outb(hc, R_SLOT, slot_tx << 1);
		HFC_outb(hc, A_SL_CFG, (ch<<1) | routing);
		HFC_outb(hc, A_CONF, (conf < 0) ? 0 : (conf | V_CONF_SL));
		hc->slot_owner[slot_tx << 1] = ch;
		hc->chan[ch].slot_tx = slot_tx;
		hc->chan[ch].bank_tx = bank_tx;
	}
	if (slot_rx < 0) {
		/* disable pcm slot */
		flow_rx = 0x80; /* ST->FIFO */
		hc->chan[ch].slot_rx = -1;
		hc->chan[ch].bank_rx = -1;
	} else {
		/* set pcm slot */
		if (hc->chan[ch].txpending)
			flow_rx = 0x80; /* ST->FIFO */
		else
			flow_rx = 0xc0; /* ST->(FIFO,PCM) */
		/* put on slot */
		routing = bank_rx?0x80:0xc0; /* reversed */
		if (conf >= 0 || bank_rx > 1)
			routing = 0x40; /* loop */
		if (debug & DEBUG_HFCMULTI_MODE)
			printk(KERN_DEBUG "%s: put channel %d to slot %d bank"
			    "%d flow %02x routing %02x conf %d (RX)\n",
			    __FUNCTION__, ch, slot_rx, bank_rx,
			    flow_rx, routing, conf);
		HFC_outb(hc, R_SLOT, (slot_rx<<1) | V_SL_DIR);
		HFC_outb(hc, A_SL_CFG, (ch<<1) | V_CH_DIR | routing);
		hc->slot_owner[(slot_rx<<1)|1] = ch;
		hc->chan[ch].slot_rx = slot_rx;
		hc->chan[ch].bank_rx = bank_rx;
	}

	switch (protocol) {
		case (ISDN_P_NONE):
			/* disable TX fifo */
			HFC_outb(hc, R_FIFO, ch << 1);
			HFC_wait(hc);
			HFC_outb(hc, A_CON_HDLC, flow_tx | 0x00 | V_IFF);
			HFC_outb(hc, A_SUBCH_CFG, 0);
			HFC_outb(hc, A_IRQ_MSK, 0);
			HFC_outb(hc, R_INC_RES_FIFO, V_RES_F);
			HFC_wait(hc);
			/* disable RX fifo */
			HFC_outb(hc, R_FIFO, (ch<<1)|1);
			HFC_wait(hc);
			HFC_outb(hc, A_CON_HDLC, flow_rx | 0x00);
			HFC_outb(hc, A_SUBCH_CFG, 0);
			HFC_outb(hc, A_IRQ_MSK, 0);
			HFC_outb(hc, R_INC_RES_FIFO, V_RES_F);
			HFC_wait(hc);
			if (hc->chan[ch].bch && hc->type != 1) {
				hc->hw.a_st_ctrl0[hc->chan[ch].port] &=
				    ((ch & 0x3) == 0)? ~V_B1_EN: ~V_B2_EN;
				HFC_outb(hc, R_ST_SEL, hc->chan[ch].port);
				HFC_outb(hc, A_ST_CTRL0,
				    hc->hw.a_st_ctrl0[hc->chan[ch].port]);
			}
			if (hc->chan[ch].bch) {
				test_and_clear_bit(FLG_HDLC,
				    &hc->chan[ch].bch->Flags);
				test_and_clear_bit(FLG_TRANSPARENT,
				    &hc->chan[ch].bch->Flags);
			}
			break;
		case (ISDN_P_B_RAW): /* B-channel */

#ifdef B410P_CARD
			if (test_bit(HFC_CHIP_B410P, &hc->chip) &&
			    (hc->chan[ch].slot_rx < 0) &&
			    (hc->chan[ch].bank_rx == 0) &&
			    (hc->chan[ch].slot_tx < 0) &&
			    (hc->chan[ch].bank_tx == 0)) {

				printk(KERN_DEBUG
				    "Setting B-channel %d to echo cancelable "
				    "state on PCM slot %d\n", ch,
				    ((ch / 4) * 8) + ((ch % 4) * 4) + 1);
				printk(KERN_DEBUG
				    "Enabling pass through for channel\n");
				vpm_out(hc, ch, ((ch / 4) * 8) +
				    ((ch % 4) * 4) + 1, 0x01);
				/* rx path */
				/* S/T -> PCM */
				HFC_outb(hc, R_FIFO, (ch << 1));
				HFC_wait(hc);
				HFC_outb(hc, A_CON_HDLC, 0xc0 | V_HDLC_TRP |
				    V_IFF);
				HFC_outb(hc, R_SLOT, (((ch / 4) * 8) +
				    ((ch % 4) * 4) + 1) << 1);
				HFC_outb(hc, A_SL_CFG, 0x80 | (ch << 1));

				/* PCM -> FIFO */
				HFC_outb(hc, R_FIFO, 0x20 | (ch << 1) | 1);
				HFC_wait(hc);
				HFC_outb(hc, A_CON_HDLC, 0x20 | V_HDLC_TRP |
				    V_IFF);
				HFC_outb(hc, A_SUBCH_CFG, 0);
				HFC_outb(hc, A_IRQ_MSK, 0);
				HFC_outb(hc, R_INC_RES_FIFO, V_RES_F);
				HFC_wait(hc);
				HFC_outb(hc, R_SLOT, ((((ch / 4) * 8) +
				    ((ch % 4) * 4) + 1) << 1) | 1);
				HFC_outb(hc, A_SL_CFG, 0x80 | 0x20 |
				    (ch << 1) | 1);

				/* tx path */
				/* PCM -> S/T */
				HFC_outb(hc, R_FIFO, (ch << 1) | 1);
				HFC_wait(hc);
				HFC_outb(hc, A_CON_HDLC, 0xc0 | V_HDLC_TRP |
				    V_IFF);
				HFC_outb(hc, R_SLOT, ((((ch / 4) * 8) +
				    ((ch % 4) * 4)) << 1) | 1);
				HFC_outb(hc, A_SL_CFG, 0x80 | 0x40 |
				    (ch << 1) | 1);

				/* FIFO -> PCM */
				HFC_outb(hc, R_FIFO, 0x20 | (ch << 1));
				HFC_wait(hc);
				HFC_outb(hc, A_CON_HDLC, 0x20 | V_HDLC_TRP |
				    V_IFF);
				HFC_outb(hc, A_SUBCH_CFG, 0);
				HFC_outb(hc, A_IRQ_MSK, 0);
				HFC_outb(hc, R_INC_RES_FIFO, V_RES_F);
				HFC_wait(hc);
				/* tx silence */
				HFC_outb_(hc, A_FIFO_DATA0_NOINC, silence);
				HFC_outb(hc, R_SLOT, (((ch / 4) * 8) +
				    ((ch % 4) * 4)) << 1);
				HFC_outb(hc, A_SL_CFG, 0x80 | 0x20 | (ch << 1));
			} else 
#endif	
			{
				/* enable TX fifo */
				HFC_outb(hc, R_FIFO, ch << 1);
				HFC_wait(hc);
				HFC_outb(hc, A_CON_HDLC, flow_tx | 0x00 |
				    V_HDLC_TRP | V_IFF);
				HFC_outb(hc, A_SUBCH_CFG, 0);
				HFC_outb(hc, A_IRQ_MSK, 0);
				HFC_outb(hc, R_INC_RES_FIFO, V_RES_F);
				HFC_wait(hc);
				/* tx silence */
				HFC_outb_(hc, A_FIFO_DATA0_NOINC, silence);
				/* enable RX fifo */
				HFC_outb(hc, R_FIFO, (ch<<1)|1);
				HFC_wait(hc);
				HFC_outb(hc, A_CON_HDLC, flow_rx | 0x00 |
				    V_HDLC_TRP);
				HFC_outb(hc, A_SUBCH_CFG, 0);
				HFC_outb(hc, A_IRQ_MSK, 0);
				HFC_outb(hc, R_INC_RES_FIFO, V_RES_F);
				HFC_wait(hc);
			}
			if (hc->type != 1) {
				hc->hw.a_st_ctrl0[hc->chan[ch].port] |=
				    ((ch & 0x3) == 0) ? V_B1_EN : V_B2_EN;
				HFC_outb(hc, R_ST_SEL, hc->chan[ch].port);
				HFC_outb(hc, A_ST_CTRL0,
				    hc->hw.a_st_ctrl0[hc->chan[ch].port]);
			}
			if (hc->chan[ch].bch)
				test_and_set_bit(FLG_TRANSPARENT,
				    &hc->chan[ch].bch->Flags);
			break;
		case (ISDN_P_B_HDLC): /* B-channel */
		case (ISDN_P_TE_S0): /* D-channel */
		case (ISDN_P_NT_S0):
		case (ISDN_P_TE_E1):
		case (ISDN_P_NT_E1):
			/* enable TX fifo */
			HFC_outb(hc, R_FIFO, ch<<1);
			HFC_wait(hc);
			if (hc->type == 1) { /* E1 */
				HFC_outb(hc, A_CON_HDLC, flow_tx | 0x04);
				HFC_outb(hc, A_SUBCH_CFG, 0);
			} else {
				HFC_outb(hc, A_CON_HDLC,
				    flow_tx | 0x04 | V_IFF);
				HFC_outb(hc, A_SUBCH_CFG, 2);
			}
			HFC_outb(hc, A_IRQ_MSK, V_IRQ);
			HFC_outb(hc, R_INC_RES_FIFO, V_RES_F);
			HFC_wait(hc);
			/* enable RX fifo */
			HFC_outb(hc, R_FIFO, (ch<<1)|1);
			HFC_wait(hc);
			HFC_outb(hc, A_CON_HDLC, flow_rx | 0x04);
			if (hc->type == 1)
				HFC_outb(hc, A_SUBCH_CFG, 0);
			else
				HFC_outb(hc, A_SUBCH_CFG, 2);
			HFC_outb(hc, A_IRQ_MSK, V_IRQ);
			HFC_outb(hc, R_INC_RES_FIFO, V_RES_F);
			HFC_wait(hc);
			if (hc->chan[ch].bch) {
				test_and_set_bit(FLG_HDLC,
				    &hc->chan[ch].bch->Flags);
				if (hc->type != 1) {
					hc->hw.a_st_ctrl0[hc->chan[ch].port] |=
					    ((ch&0x3) == 0) ? V_B1_EN : V_B2_EN;
					HFC_outb(hc, R_ST_SEL, hc->chan[ch].port);
					HFC_outb(hc, A_ST_CTRL0,
					    hc->hw.a_st_ctrl0[hc->chan[ch].port]);
				}
			}
			break;
		default:
			printk(KERN_DEBUG "%s: protocol not known %x\n",
			    __FUNCTION__, protocol);
			hc->chan[ch].protocol = ISDN_P_NONE;
			return -ENOPROTOOPT;
	}
	hc->chan[ch].protocol = protocol;
	return 0;
}


/*
 * connect/disconnect PCM
 */

static void
hfcmulti_pcm(struct hfc_multi *hc, int ch, int slot_tx, int bank_tx,
    int slot_rx, int bank_rx)
{
	if (slot_rx < 0 || slot_rx < 0 || bank_tx < 0 || bank_rx < 0) {
		/* disable PCM */
		mode_hfcmulti(hc, ch, hc->chan[ch].protocol, -1, -1, -1, -1);
		return;
	}

	/* enable pcm */
	mode_hfcmulti(hc, ch, hc->chan[ch].protocol, slot_tx, bank_tx,
		slot_rx, bank_rx);
}

/*
 * set/disable conference
 */

static void
hfcmulti_conf(struct hfc_multi *hc, int ch, int num)
{
	if (num >= 0 && num <= 7)
		hc->chan[ch].conf = num;
	else
		hc->chan[ch].conf = -1;
	mode_hfcmulti(hc, ch, hc->chan[ch].protocol, hc->chan[ch].slot_tx,
	    hc->chan[ch].bank_tx, hc->chan[ch].slot_rx,
	    hc->chan[ch].bank_rx);
}


/*
 * set/disable sample loop
 */

/* NOTE: this function is experimental and therefore disabled */
static void
hfcmulti_splloop(struct hfc_multi *hc, int ch, u_char *data, int len)
{
	struct bchannel *bch = hc->chan[ch].bch;

	if (!bch)
		return;
	/* flush and confirm pending TX data, if any */
	get_next_bframe(bch);

	/* prevent overflow */
	if (len > hc->Zlen-1)
		len = hc->Zlen-1;

	/* select fifo */
	HFC_outb_(hc, R_FIFO, ch<<1);
	HFC_wait_(hc);

	/* reset fifo */
	HFC_outb(hc, A_SUBCH_CFG, 0);
	udelay(500);
	HFC_outb_(hc, R_INC_RES_FIFO, V_RES_F);
	HFC_wait_(hc);
	udelay(500);

	/* if off */
	if (len <= 0) {
		HFC_outb_(hc, A_FIFO_DATA0_NOINC, silence);
		if (hc->chan[ch].slot_tx >= 0) {
			if (debug & DEBUG_HFCMULTI_MODE)
				printk(KERN_DEBUG "%s: connecting PCM due to "
				    "no more TONE: channel %d slot_tx %d\n",
				    __FUNCTION__, ch, hc->chan[ch].slot_tx);
			/* connect slot */
			HFC_outb(hc, A_CON_HDLC, 0xc0 | 0x00 |
			    V_HDLC_TRP | V_IFF);
			HFC_outb(hc, R_FIFO, ch<<1 | 1);
			HFC_wait(hc);
			HFC_outb(hc, A_CON_HDLC, 0xc0 | 0x00 |
			    V_HDLC_TRP | V_IFF);
		}
		hc->chan[ch].txpending = 0;
		return;
	}

	/* loop fifo */

	/* set mode */
	hc->chan[ch].txpending = 2;

// printk("len=%d %02x %02x %02x\n", len, data[0], data[1], data[2]);
	/* write loop data */
	write_fifo_data(hc, data, len);

	udelay(500);
	HFC_outb(hc, A_SUBCH_CFG, V_LOOP_FIFO);
	udelay(500);

	/* disconnect slot */
	if (hc->chan[ch].slot_tx >= 0) {
		if (debug & DEBUG_HFCMULTI_MODE)
			printk(KERN_DEBUG
			    "%s: disconnecting PCM due to TONE: channel %d "
			    "slot_tx %d\n", __FUNCTION__,
			    ch, hc->chan[ch].slot_tx);
		HFC_outb(hc, A_CON_HDLC, 0x80 | 0x00 | V_HDLC_TRP | V_IFF);
		HFC_outb(hc, R_FIFO, ch<<1 | 1);
		HFC_wait(hc);
		HFC_outb(hc, A_CON_HDLC, 0x80 | 0x00 | V_HDLC_TRP | V_IFF);
		HFC_outb(hc, R_FIFO, ch<<1);
		HFC_wait(hc);
	} else {
		/* change fifo */
		HFC_outb(hc, R_FIFO, ch<<1);
		HFC_wait(hc);
	}

// udelay(300);
}

/*
 * Layer 1 callback function
 */
static int
hfcm_l1callback(struct dchannel *dch, u_int cmd)
{
	struct hfc_multi	*hc = dch->hw;

	switch (cmd) {
	case INFO3_P8:
	case INFO3_P10:
		break;
	case HW_RESET_REQ:
		/* start activation */
		if (hc->type == 1) {
			if (debug & DEBUG_HFCMULTI_MSG)
				printk(KERN_DEBUG
				    "%s: HW_RESET_REQ no BRI\n",
				    __FUNCTION__);
		} else {
			HFC_outb(hc, R_ST_SEL, hc->chan[dch->slot].port);
			HFC_outb(hc, A_ST_WR_STATE, V_ST_LD_STA | 3); /* F3 */
			udelay(6); /* wait at least 5,21us */
			HFC_outb(hc, A_ST_WR_STATE, 3);
			/* activate */
			HFC_outb(hc, A_ST_WR_STATE, 3 | (V_ST_ACT*3));
		}
		l1_event(dch->l1, HW_POWERUP_IND);
		break;
	case HW_DEACT_REQ:
		/* start deactivation */
		if (hc->type == 1) {
			if (debug & DEBUG_HFCMULTI_MSG)
				printk(KERN_DEBUG
				    "%s: HW_DEACT_REQ no BRI\n",
				    __FUNCTION__);
		} else {
			HFC_outb(hc, R_ST_SEL, hc->chan[dch->slot].port);
			HFC_outb(hc, A_ST_WR_STATE, V_ST_ACT*2);
			/* deactivate */
		}
		skb_queue_purge(&dch->squeue);
		if (dch->tx_skb) {
			dev_kfree_skb(dch->tx_skb);
			dch->tx_skb = NULL;
		}
		dch->tx_idx = 0;
		if (dch->rx_skb) {
			dev_kfree_skb(dch->rx_skb);
			dch->rx_skb = NULL;
		}
		test_and_clear_bit(FLG_TX_BUSY, &dch->Flags);
		if (test_and_clear_bit(FLG_BUSY_TIMER, &dch->Flags))
			del_timer(&dch->timer);
		break;
	case HW_POWERUP_REQ:
		if (hc->type == 1) {
			if (debug & DEBUG_HFCMULTI_MSG)
				printk(KERN_DEBUG
				    "%s: HW_POWERUP_REQ no BRI\n",
				    __FUNCTION__);
		} else {
			HFC_outb(hc, R_ST_SEL, hc->chan[dch->slot].port);
			HFC_outb(hc, A_ST_WR_STATE, 3 | 0x10); /* activate */
			udelay(6); /* wait at least 5,21us */
			HFC_outb(hc, A_ST_WR_STATE, 3); /* activate */
		}
		break;
	case PH_ACTIVATE_IND:
		test_and_set_bit(FLG_ACTIVE, &dch->Flags);
		_queue_data(&dch->dev.D, cmd, MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
		break;
	case PH_DEACTIVATE_IND:
		test_and_clear_bit(FLG_ACTIVE, &dch->Flags);
		_queue_data(&dch->dev.D, cmd, MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
		break;
	default:
		if (dch->debug & DEBUG_HW)
			printk(KERN_DEBUG "%s: unknown command %x\n",
			    __FUNCTION__, cmd);
		return -1;
	}
	return 0;
}

/*
 * Layer2 -> Layer 1 Transfer
 */

static int
handle_dmsg(struct mISDNchannel *ch, struct sk_buff *skb)
{
	struct mISDNdevice	*dev = container_of(ch, struct mISDNdevice, D);
	struct dchannel		*dch = container_of(dev, struct dchannel, dev);
	struct hfc_multi	*hc = dch->hw;
	struct mISDNhead	*hh = mISDN_HEAD_P(skb);
	int			ret = -EINVAL;
	u_long			id;
	u_long			flags;

	switch (hh->prim) {
	case PH_DATA_REQ:
		spin_lock_irqsave(&hc->lock, flags);
		ret = dchannel_senddata(dch, skb);
		if (ret > 0) { /* direct TX */
			id = hh->id; /* skb can be freed */
			hfcmulti_tx(hc, dch->slot);
			/* start fifo */
			HFC_outb(hc, R_FIFO, 0);
			HFC_wait(hc);
			spin_unlock_irqrestore(&hc->lock, flags);
			queue_ch_frame(ch, PH_DATA_CNF, id, NULL);
			ret = 0;
		} else
			spin_unlock_irqrestore(&hc->lock, flags);
		return ret;
	case PH_ACTIVATE_REQ:
		spin_lock_irqsave(&hc->lock, flags);
		if (dch->dev.D.protocol != ISDN_P_TE_S0) {
			ret = 0;
			if (debug & DEBUG_HFCMULTI_MSG)
				printk(KERN_DEBUG
				    "%s: PH_ACTIVATE port %d (0..%d)\n",
				    __FUNCTION__, hc->chan[dch->slot].port,
				    hc->ports-1);
			/* start activation */
			if (hc->type == 1) {
				ph_state_change (dch);
				if (debug & DEBUG_HFCMULTI_STATE)
					printk(KERN_DEBUG
					    "%s: E1 report state %x \n",
					    __FUNCTION__, dch->state);
			} else {
				HFC_outb(hc, R_ST_SEL,
				    hc->chan[dch->slot].port);
				HFC_outb(hc, A_ST_WR_STATE, V_ST_LD_STA | 1);
				    /* G1 */
				udelay(6); /* wait at least 5,21us */
				HFC_outb(hc, A_ST_WR_STATE, 1);
				HFC_outb(hc, A_ST_WR_STATE, 1 |
				    (V_ST_ACT*3)); /* activate */
				dch->state = 1;
			}
		} else {
			ret = l1_event(dch->l1, hh->prim);
		}
		spin_unlock_irqrestore(&hc->lock, flags);
		break;
	case PH_DEACTIVATE_REQ:
		test_and_clear_bit(FLG_L2_ACTIVATED, &dch->Flags);
		spin_lock_irqsave(&hc->lock, flags);
		if (dch->dev.D.protocol != ISDN_P_TE_S0) {
			if (debug & DEBUG_HFCMULTI_MSG)
				printk(KERN_DEBUG
				    "%s: PH_DEACTIVATE port %d (0..%d)\n",
				    __FUNCTION__, hc->chan[dch->slot].port,
				    hc->ports-1);
			/* start deactivation */
			if (hc->type == 1) {
				if (debug & DEBUG_HFCMULTI_MSG)
					printk(KERN_DEBUG
					    "%s: PH_DEACTIVATE no BRI\n",
					    __FUNCTION__);
			} else {
				HFC_outb(hc, R_ST_SEL,
				    hc->chan[dch->slot].port);
				HFC_outb(hc, A_ST_WR_STATE, V_ST_ACT * 2);
				    /* deactivate */
				dch->state = 1;
			}
			skb_queue_purge(&dch->squeue);
			if (dch->tx_skb) {
				dev_kfree_skb(dch->tx_skb);
				dch->tx_skb = NULL;
			}
			dch->tx_idx = 0;
			if (dch->rx_skb) {
				dev_kfree_skb(dch->rx_skb);
				dch->rx_skb = NULL;
			}
			test_and_clear_bit(FLG_TX_BUSY, &dch->Flags);
			if (test_and_clear_bit(FLG_BUSY_TIMER, &dch->Flags))
				del_timer(&dch->timer);
#ifdef FIXME
			if (test_and_clear_bit(FLG_L1_BUSY, &dch->Flags))
				dchannel_sched_event(&hc->dch, D_CLEARBUSY);
#endif
			ret = 0;
		} else {
			ret = l1_event(dch->l1, hh->prim);
		}
		spin_unlock_irqrestore(&hc->lock, flags);
		break;
	}
	if (!ret)
		dev_kfree_skb(skb);
	return ret;
}

static void
deactivate_bchannel(struct bchannel *bch)
{
	struct hfc_multi	*hc = bch->hw;
	u_long			flags;

	spin_lock_irqsave(&hc->lock, flags);
	if (test_and_clear_bit(FLG_TX_NEXT, &bch->Flags)) {
		dev_kfree_skb(bch->next_skb);
		bch->next_skb = NULL;
	}
	if (bch->tx_skb) {
		dev_kfree_skb(bch->tx_skb);
		bch->tx_skb = NULL;
	}
	bch->tx_idx = 0;
	if (bch->rx_skb) {
		dev_kfree_skb(bch->rx_skb);
		bch->rx_skb = NULL;
	}
	test_and_clear_bit(FLG_ACTIVE, &bch->Flags);
	test_and_clear_bit(FLG_TX_BUSY, &bch->Flags);
	hc->chan[bch->slot].conf = -1;
	mode_hfcmulti(hc, bch->slot, ISDN_P_NONE, -1, -1, -1, -1);
	spin_unlock_irqrestore(&hc->lock, flags);
}

static int
handle_bmsg(struct mISDNchannel *ch, struct sk_buff *skb)
{
	struct bchannel		*bch = container_of(ch, struct bchannel, ch);
	struct hfc_multi	*hc = bch->hw;
	int			ret = -EINVAL;
	struct mISDNhead	*hh = mISDN_HEAD_P(skb);
	u_long			id;
	u_long			flags;

	switch (hh->prim) {
	case PH_DATA_REQ:
		spin_lock_irqsave(&hc->lock, flags);
		ret = bchannel_senddata(bch, skb);
		if (ret > 0) { /* direct TX */
			id = hh->id; /* skb can be freed */
			hfcmulti_tx(hc, bch->slot);
			/* start fifo */
			HFC_outb(hc, R_FIFO, 0);
			HFC_wait(hc);
			spin_unlock_irqrestore(&hc->lock, flags);
			queue_ch_frame(ch, PH_DATA_CNF, id, NULL);
			ret = 0;
		} else
			spin_unlock_irqrestore(&hc->lock, flags);
		return ret;
	case PH_ACTIVATE_REQ:
		if (debug & DEBUG_HFCMULTI_MSG)
			printk(KERN_DEBUG "%s: PH_ACTIVATE ch %d (0..32)\n",
				__FUNCTION__, bch->slot);
		spin_lock_irqsave(&hc->lock, flags);
		/* activate B-channel if not already activated */
		if (!test_and_set_bit(FLG_ACTIVE, &bch->Flags)) {
			ret = mode_hfcmulti(hc, bch->slot,
				ch->protocol,
				hc->chan[bch->slot].slot_tx,
				hc->chan[bch->slot].bank_tx,
				hc->chan[bch->slot].slot_rx,
				hc->chan[bch->slot].bank_rx);
			if (!ret) {
				if (ch->protocol == ISDN_P_B_RAW && !hc->dtmf
 				  && test_bit(HFC_CHIP_DTMF, &hc->chip)) {
					/* start decoder */
					hc->dtmf = 1;
					if (debug & DEBUG_HFCMULTI_DTMF)
						printk(KERN_DEBUG
						    "%s: start dtmf decoder\n",
							__FUNCTION__);
					HFC_outb(hc, R_DTMF, hc->hw.r_dtmf |
					    V_RST_DTMF);
				}
			}
		} else
			ret = 0;
		spin_unlock_irqrestore(&hc->lock, flags);
		if (!ret)
			_queue_data(ch, PH_ACTIVATE_IND, MISDN_ID_ANY, 0, NULL, GFP_KERNEL);
		break;
	case PH_CONTROL_REQ:
		spin_lock_irqsave(&hc->lock, flags);
		switch (hh->id) {
		case HFC_SPL_LOOP_ON: /* set sample loop */
			if (debug & DEBUG_HFCMULTI_MSG)
			printk(KERN_DEBUG
			    "%s: HFC_SPL_LOOP_ON (len = %d)\n",
			    __FUNCTION__, skb->len);
			hfcmulti_splloop(hc, bch->slot, skb->data, skb->len);
			ret = 0;
			break;
		case HFC_SPL_LOOP_OFF: /* set silence */
			if (debug & DEBUG_HFCMULTI_MSG)
				printk(KERN_DEBUG "%s: HFC_SPL_LOOP_OFF\n",
				    __FUNCTION__);
			hfcmulti_splloop(hc, bch->slot, NULL, 0);
			ret = 0;
			break;
		default:
			printk(KERN_ERR
			     "%s: unknown PH_CONTROL_REQ info %x\n",
			     __FUNCTION__, hh->id);
			ret = -EINVAL;
		}
		spin_unlock_irqrestore(&hc->lock, flags);
		break;
	case PH_DEACTIVATE_REQ:
		deactivate_bchannel(bch);
		_queue_data(ch, PH_DEACTIVATE_IND, MISDN_ID_ANY, 0, NULL, GFP_KERNEL);
		ret = 0;
		break;
	}
	if (!ret)
		dev_kfree_skb(skb);
	return ret;
}

/*
 * bchannel control function
 */
static int
channel_bctrl(struct bchannel *bch, struct mISDN_ctrl_req *cq)
{
	int			ret = 0;
	struct dsp_features	*features = (struct dsp_features *)cq->p1;
	struct hfc_multi	*hc = bch->hw;
	int			slot_tx;
	int			bank_tx;
	int			slot_rx;
	int			bank_rx;
	int			num;

	switch(cq->op) {
	case MISDN_CTRL_GETOP:
		cq->op = MISDN_CTRL_HFC_OP | MISDN_CTRL_HW_FEATURES_OP;
		break;
	case MISDN_CTRL_HW_FEATURES: /* fill features structure */
		if (debug & DEBUG_HFCMULTI_MSG)
			printk(KERN_DEBUG "%s: HW_FEATURE request\n",
			    __FUNCTION__);
		/* create confirm */
		features->hfc_id = hc->id;
		if (test_bit(HFC_CHIP_DTMF, &hc->chip))
			features->hfc_dtmf = 1;
		features->hfc_loops = 0;
		features->pcm_id = hc->pcm;
		features->pcm_slots = hc->slots;
		features->pcm_banks = 2;
		if (test_bit(HFC_CHIP_B410P, &hc->chip))
			features->hfc_echocanhw = 1;
		break;
	case MISDN_CTRL_HFC_PCM_CONN: /* connect interface to pcm timeslot (0..N) */
		slot_tx = cq->p1 | 0xff;
		bank_tx = cq->p1 >> 8;
		slot_rx = cq->p2 | 0xff;
		bank_rx = cq->p2 >> 8;
		if (debug & DEBUG_HFCMULTI_MSG)
			printk(KERN_DEBUG
			    "%s: HFC_PCM_CONN slot %d bank %d (TX) "
			    "slot %d bank %d (RX)\n",
			    __FUNCTION__, slot_tx, bank_tx,
			    slot_rx, bank_rx);
		if (slot_tx <= hc->slots && bank_tx <= 2 &&
		    slot_rx <= hc->slots && bank_rx <= 2)
			hfcmulti_pcm(hc, bch->slot,
			    slot_tx, bank_tx, slot_rx, bank_rx);
		else {
			printk(KERN_WARNING
			    "%s: HFC_PCM_CONN slot %d bank %d (TX) "
			    "slot %d bank %d (RX) out of range\n",
			    __FUNCTION__, slot_tx, bank_tx,
			    slot_rx, bank_rx);
			ret = -EINVAL;
		}
		break;
	case MISDN_CTRL_HFC_PCM_DISC: /* release interface from pcm timeslot */
		if (debug & DEBUG_HFCMULTI_MSG)
			printk(KERN_DEBUG "%s: HFC_PCM_DISC\n",
			    __FUNCTION__);
		hfcmulti_pcm(hc, bch->slot, -1, -1, -1, -1);
		break;
	case MISDN_CTRL_HFC_CONF_JOIN: /* join conference (0..7) */
		num = cq->p1 | 0xff;
		if (debug & DEBUG_HFCMULTI_MSG)
			printk(KERN_DEBUG "%s: HFC_CONF_JOIN conf %d\n",
			    __FUNCTION__, num);
		if (num <= 7)
			hfcmulti_conf(hc, bch->slot, num);
		else {
			printk(KERN_WARNING
			    "%s: HW_CONF_JOIN conf %d out of range\n",
			    __FUNCTION__, num);
			ret = -EINVAL;
		}
		break;
	case MISDN_CTRL_HFC_CONF_SPLIT: /* split conference */
		if (debug & DEBUG_HFCMULTI_MSG)
			printk(KERN_DEBUG "%s: HFC_CONF_SPLIT\n", __FUNCTION__);
		hfcmulti_conf(hc, bch->slot, -1);
		break;
	case MISDN_CTRL_HFC_ECHOCAN_ON:
#ifdef B410P_CARD		
		vpm_echocan_on(hc, bch->slot, cq->p1);
#endif
		break;

	case MISDN_CTRL_HFC_ECHOCAN_OFF:
#ifdef B410P_CARD		
		vpm_echocan_off(hc, bch->slot);
#endif
		break;
	default:
		printk(KERN_WARNING "%s: unknown Op %x\n",
		    __FUNCTION__, cq->op);
		ret= -EINVAL;
		break;
	}
	return ret;
}

static int
hfcm_bctrl(struct mISDNchannel *ch, u_int cmd, void *arg)
{
	struct bchannel	*bch = container_of(ch, struct bchannel, ch);
	int		err = -EINVAL;

	if (bch->debug & DEBUG_HW)
		printk(KERN_DEBUG "%s: cmd:%x %p\n",
		    __FUNCTION__, cmd, arg);
	switch(cmd) {
	case CLOSE_CHANNEL:
		test_and_clear_bit(FLG_OPEN, &bch->Flags);
		if (test_bit(FLG_ACTIVE, &bch->Flags))
			deactivate_bchannel(bch);
		ch->protocol = ISDN_P_NONE;
		ch->peer = NULL;
		module_put(THIS_MODULE);
		err = 0;
		break;
	case CONTROL_CHANNEL:
		err = channel_bctrl(bch, arg);
		break;
	default:
		printk(KERN_WARNING "%s: unknown prim(%x)\n",
			__FUNCTION__, cmd);
	}
	return err;
}

/*
 * handle D-channel events
 *
 * handle state change event
 */
static void
ph_state_change(struct dchannel *dch)
{
	struct hfc_multi *hc = dch->hw;
	int ch;

	if (!dch) {
		printk(KERN_WARNING "%s: ERROR given dch is NULL\n",
		    __FUNCTION__);
		return;
	}
	ch = dch->slot;

	if (hc->type == 1) {
		if (dch->dev.D.protocol == ISDN_P_TE_E1) {
			if (debug & DEBUG_HFCMULTI_STATE)
				printk(KERN_DEBUG
				    "%s: E1 TE (id=%d) newstate %x\n",
				    __FUNCTION__, hc->id, dch->state);
		} else {
			if (debug & DEBUG_HFCMULTI_STATE)
				printk(KERN_DEBUG
				    "%s: E1 NT (id=%d) newstate %x\n",
				    __FUNCTION__, hc->id, dch->state);
		}
		switch (dch->state) {
			case (1):
				test_and_set_bit(FLG_ACTIVE, &dch->Flags);
				_queue_data(&dch->dev.D, PH_ACTIVATE_IND,
				    MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
				break;

			default:
				if (hc->chan[ch].e1_state != 1)
					return;
				test_and_clear_bit(FLG_ACTIVE, &dch->Flags);
				_queue_data(&dch->dev.D, PH_DEACTIVATE_IND,
				    MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
		}
		hc->chan[ch].e1_state = dch->state;
	} else {
		if (dch->dev.D.protocol == ISDN_P_TE_S0) {
			if (debug & DEBUG_HFCMULTI_STATE)
				printk(KERN_DEBUG
				    "%s: S/T TE newstate %x\n",
				    __FUNCTION__, dch->state);
			switch (dch->state) {
			case (0):
				l1_event(dch->l1, HW_RESET_IND);
				break;
			case (3):
				l1_event(dch->l1, HW_DEACT_IND);
				break;
			case (5):
			case (8):
				l1_event(dch->l1, ANYSIGNAL);
				break;
			case (6):
				l1_event(dch->l1, INFO2);
				break;
			case (7):
				l1_event(dch->l1, INFO4_P8);
				break;
			}
		} else {
			if (debug & DEBUG_HFCMULTI_STATE)
				printk(KERN_DEBUG "%s: S/T NT newstate %x\n",
				    __FUNCTION__, dch->state);
			switch (dch->state) {
			case (2):
				if (hc->chan[ch].nt_timer == 0) {
					hc->chan[ch].nt_timer = -1;
					HFC_outb(hc, R_ST_SEL,
					    hc->chan[ch].port);
					HFC_outb(hc, A_ST_WR_STATE, 4 |
					    V_ST_LD_STA); /* G4 */
					udelay(6); /* wait at least 5,21us */
					HFC_outb(hc, A_ST_WR_STATE, 4);
					dch->state = 4;
				} else {
					/* one extra count for the next event */
					hc->chan[ch].nt_timer =
					    nt_t1_count[poll_timer] + 1;
					HFC_outb(hc, R_ST_SEL,
					    hc->chan[ch].port);
					/* allow G2 -> G3 transition */
					HFC_outb(hc, A_ST_WR_STATE, 2 |
					    V_SET_G2_G3);
				}
				break;
			case (1):
				hc->chan[ch].nt_timer = -1;
				test_and_clear_bit(FLG_ACTIVE, &dch->Flags);
				_queue_data(&dch->dev.D, PH_DEACTIVATE_IND,
				    MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
				break;
			case (4):
				hc->chan[ch].nt_timer = -1;
				break;
			case (3):
				hc->chan[ch].nt_timer = -1;
				test_and_set_bit(FLG_ACTIVE, &dch->Flags);
				_queue_data(&dch->dev.D, PH_ACTIVATE_IND,
				    MISDN_ID_ANY, 0, NULL, GFP_ATOMIC);
				break;
			}
		}
	}
}

/*
 * called for card mode init message
 */

static void
hfcmulti_initmode(struct dchannel *dch)
{
	struct hfc_multi *hc = dch->hw;
	BYTE		a_st_wr_state, r_e1_wr_sta;
	int		i, pt;

	if (debug & DEBUG_HFCMULTI_INIT)
		printk("%s: entered\n", __FUNCTION__);

	if (hc->type == 1) {
		hc->chan[hc->dslot].slot_tx = -1;
		hc->chan[hc->dslot].slot_rx = -1;
		hc->chan[hc->dslot].conf = -1;
		if (hc->dslot) {
			mode_hfcmulti(hc, hc->dslot, dch->dev.D.protocol,
				-1, -1, -1, -1);
			dch->timer.function = (void *) hfcmulti_dbusy_timer;
			dch->timer.data = (long) dch;
			init_timer(&dch->timer);
		}
		for (i = 1; i <= 31; i++) {
			if (i == hc->dslot)
				continue;
			hc->chan[i].slot_tx = -1;
			hc->chan[i].slot_rx = -1;
			hc->chan[i].conf = -1;
			mode_hfcmulti(hc, i, ISDN_P_NONE, -1, -1, -1, -1);
		}
		/* E1 */
		if (test_bit(HFC_CFG_REPORT_LOS, &hc->chan[hc->dslot].cfg)) {
			HFC_outb(hc, R_LOS0, 255); /* 2 ms */
			HFC_outb(hc, R_LOS1, 255); /* 512 ms */
		}
		if (test_bit(HFC_CFG_OPTICAL, &hc->chan[hc->dslot].cfg)) {
			HFC_outb(hc, R_RX0, 0);
			hc->hw.r_tx0 = 0 | V_OUT_EN;
		} else {
			HFC_outb(hc, R_RX0, 1);
			hc->hw.r_tx0 = 1 | V_OUT_EN;
		}
		hc->hw.r_tx1 = V_ATX | V_NTRI;
		HFC_outb(hc, R_TX0, hc->hw.r_tx0);
		HFC_outb(hc, R_TX1, hc->hw.r_tx1);
		HFC_outb(hc, R_TX_FR0, 0x00);
		HFC_outb(hc, R_TX_FR1, 0xf8);

		if (test_bit(HFC_CFG_CRC4, &hc->chan[hc->dslot].cfg))
			HFC_outb(hc, R_TX_FR2, V_TX_MF | V_TX_E | V_NEG_E);

		HFC_outb(hc, R_RX_FR0, V_AUTO_RESYNC | V_AUTO_RECO | 0);

		if (test_bit(HFC_CFG_CRC4, &hc->chan[hc->dslot].cfg))
			HFC_outb(hc, R_RX_FR1, V_RX_MF | V_RX_MF_SYNC);

		if (test_bit(HFC_CHIP_PCM_SLAVE, &hc->chip)) {
			/* SLAVE (clock master) */
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG
				    "%s: E1 port is clock master "
				    "(clock from PCM)\n", __FUNCTION__);
			HFC_outb(hc, R_SYNC_CTRL, V_EXT_CLK_SYNC | V_PCM_SYNC);
		} else {
			if (test_bit(HFC_CHIP_CRYSTAL_CLOCK, &hc->chip)) {
				/* MASTER (clock master) */
				if (debug & DEBUG_HFCMULTI_INIT)
					printk(KERN_DEBUG "%s: E1 port is "
					    "clock master "
					    "(clock from crystal)\n",
					    __FUNCTION__);
				HFC_outb(hc, R_SYNC_CTRL, V_EXT_CLK_SYNC |
				    V_PCM_SYNC | V_JATT_OFF);
			} else {
				/* MASTER (clock slave) */
				if (debug & DEBUG_HFCMULTI_INIT)
					printk(KERN_DEBUG
					    "%s: E1 port is clock slave "
					    "(clock to PCM)\n", __FUNCTION__);
				HFC_outb(hc, R_SYNC_CTRL, V_SYNC_OFFS);
			}
		}
		if (dch->dev.D.protocol == ISDN_P_NT_E1) {
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG "%s: E1 port is NT-mode\n",
				    __FUNCTION__);
			r_e1_wr_sta = 0; /* G0 */
		} else {
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG "%s: E1 port is TE-mode\n",
				    __FUNCTION__);
			r_e1_wr_sta = 0; /* F0 */
		}
		HFC_outb(hc, R_JATT_ATT, 0x9c); /* undoc register */
		if (test_bit(HFC_CHIP_RX_SYNC, &hc->chip)) {
			HFC_outb(hc, R_SYNC_OUT, V_SYNC_E1_RX | V_IPATS0 |
			    V_IPATS1 | V_IPATS2);
		} else {
			HFC_outb(hc, R_SYNC_OUT, V_IPATS0 |
			    V_IPATS1 | V_IPATS2);
		}
		HFC_outb(hc, R_PWM_MD, V_PWM0_MD);
		HFC_outb(hc, R_PWM0, 0x50);
		HFC_outb(hc, R_PWM1, 0xff);
		/* state machine setup */
		HFC_outb(hc, R_E1_WR_STA, r_e1_wr_sta | V_E1_LD_STA);
		udelay(6); /* wait at least 5,21us */
		HFC_outb(hc, R_E1_WR_STA, r_e1_wr_sta);
	} else {
		i = dch->slot;
		hc->chan[i].slot_tx = -1;
		hc->chan[i].slot_rx = -1;
		hc->chan[i].conf = -1;
		mode_hfcmulti(hc, i, dch->dev.D.protocol, -1, -1, -1, -1);
		dch->timer.function = (void *)hfcmulti_dbusy_timer;
		dch->timer.data = (long) dch;
		init_timer(&dch->timer);
		hc->chan[i - 1].slot_tx = -1;
		hc->chan[i - 1].slot_rx = -1;
		hc->chan[i - 1].conf = -1;
		mode_hfcmulti(hc, i - 1, ISDN_P_NONE, -1, -1, -1, -1);
		hc->chan[i - 2].slot_tx = -1;
		hc->chan[i - 2].slot_rx = -1;
		hc->chan[i - 2].conf = -1;
		mode_hfcmulti(hc, i - 2, ISDN_P_NONE, -1, -1, -1, -1);
		/* ST */
		pt = hc->chan[i].port;
		/* select interface */
		HFC_outb(hc, R_ST_SEL, pt);
		if (dch->dev.D.protocol == ISDN_P_NT_S0) {
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG 
				    "%s: ST port %d is NT-mode\n",
				    __FUNCTION__, pt);
			/* clock delay */
			HFC_outb(hc, A_ST_CLK_DLY, CLKDEL_NT | 0x60);
			a_st_wr_state = 1; /* G1 */
			hc->hw.a_st_ctrl0[pt] = V_ST_MD;
		} else {
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG
				    "%s: ST port %d is TE-mode\n",
				    __FUNCTION__, pt);
			/* clock delay */
			HFC_outb(hc, A_ST_CLK_DLY, CLKDEL_TE);
			a_st_wr_state = 2; /* F2 */
			hc->hw.a_st_ctrl0[pt] = 0;
		}
		if (!test_bit(HFC_CFG_NONCAP_TX, &hc->chan[i].cfg)) {
			hc->hw.a_st_ctrl0[pt] |= V_TX_LI;
		}
		/* line setup */
		HFC_outb(hc, A_ST_CTRL0,  hc->hw.a_st_ctrl0[pt]);
		/* disable E-channel */
		if ((dch->dev.D.protocol == ISDN_P_NT_S0) ||
		    test_bit(HFC_CFG_DIS_ECHANNEL, &hc->chan[i].cfg))
			HFC_outb(hc, A_ST_CTRL1, V_E_IGNO);
		else
			HFC_outb(hc, A_ST_CTRL1, 0);
		/* enable B-channel receive */
		HFC_outb(hc, A_ST_CTRL2,  V_B1_RX_EN | V_B2_RX_EN);
		/* state machine setup */
		HFC_outb(hc, A_ST_WR_STATE, a_st_wr_state | V_ST_LD_STA);
		udelay(6); /* wait at least 5,21us */
		HFC_outb(hc, A_ST_WR_STATE, a_st_wr_state);
		hc->hw.r_sci_msk |= 1 << pt;
		/* state machine interrupts */
		HFC_outb(hc, R_SCI_MSK, hc->hw.r_sci_msk);
	}
	if (debug & DEBUG_HFCMULTI_INIT)
		printk("%s: done\n", __FUNCTION__);
}


static int
open_dchannel(struct hfc_multi *hc, struct dchannel *dch,
    struct channel_req *rq)
{
	int err = 0;

	if (debug & DEBUG_HW_OPEN)
		printk(KERN_DEBUG "%s: dev(%d) open from %p\n", __FUNCTION__,
		    dch->dev.id, __builtin_return_address(0));
	if (rq->protocol == ISDN_P_NONE)
		return -EINVAL;
	if ((dch->dev.D.protocol != ISDN_P_NONE) &&
	    (dch->dev.D.protocol != rq->protocol)) {
	    if (debug & DEBUG_HFCMULTI_MODE)
		printk(KERN_WARNING "%s: change protocol %x to %x\n",
		    __FUNCTION__, dch->dev.D.protocol, rq->protocol);
	}
	if ((dch->dev.D.protocol == ISDN_P_TE_S0)
	 && (rq->protocol != ISDN_P_TE_S0))
		l1_event(dch->l1, CLOSE_CHANNEL);
	if (dch->dev.D.protocol != rq->protocol) { 
		if (rq->protocol == ISDN_P_TE_S0) {
			err = create_l1(dch, hfcm_l1callback);
			if (err)
				return err;
		}
		dch->dev.D.protocol = rq->protocol;
		hfcmulti_initmode(dch);
	}
	
	if (((rq->protocol == ISDN_P_NT_S0) && (dch->state == 3)) ||
	    ((rq->protocol == ISDN_P_TE_S0) && (dch->state == 7)) ||
	    ((rq->protocol == ISDN_P_NT_E1) && (dch->state == 1)) ||
	    ((rq->protocol == ISDN_P_TE_E1) && (dch->state == 1))) {
		_queue_data(&dch->dev.D, PH_ACTIVATE_IND, MISDN_ID_ANY,
		    0,NULL, GFP_KERNEL);
	}
	rq->ch = &dch->dev.D;
	if (!try_module_get(THIS_MODULE))
		printk(KERN_WARNING "%s:cannot get module\n", __FUNCTION__);
	return 0;
}

static int
open_bchannel(struct hfc_multi *hc, struct dchannel *dch,
    struct channel_req *rq)
{
	struct bchannel	*bch;
	int		ch;	

	if (!test_bit(rq->adr.channel, &dch->dev.channelmap[0]))
		return -EINVAL;
	if (rq->protocol == ISDN_P_NONE)
		return -EINVAL;
	if (hc->type == 1) {
		ch = rq->adr.channel;
	} else {
		ch = rq->adr.channel - 1;
		ch += hc->chan[dch->slot].port << 2;
	}
	bch = hc->chan[ch].bch;
	if (!bch) {
		printk(KERN_ERR "%s:internal error ch %d has no bch\n",
		    __FUNCTION__, ch);
		return -EINVAL;  
	}
	if (test_and_set_bit(FLG_OPEN, &bch->Flags))
		return -EBUSY; /* b-channel can be only open once */
	bch->ch.protocol = rq->protocol;
	rq->ch = &bch->ch;
	if (!try_module_get(THIS_MODULE))
		printk(KERN_WARNING "%s:cannot get module\n", __FUNCTION__);
	return 0;
}

/*
 * device control function
 */
static int
channel_dctrl(struct dchannel *dch, struct mISDN_ctrl_req *cq)
{
	int	ret = 0;

	switch(cq->op) {
	case MISDN_CTRL_GETOP:
		cq->op = 0; // TODO
		break;
	default:
		printk(KERN_WARNING "%s: unknown Op %x\n",
		    __FUNCTION__, cq->op);
		ret= -EINVAL;
		break;
	}
	return ret;
}

static int
hfcm_dctrl(struct mISDNchannel *ch, u_int cmd, void *arg)
{
	struct mISDNdevice	*dev = container_of(ch, struct mISDNdevice, D);
	struct dchannel		*dch = container_of(dev, struct dchannel, dev);
	struct hfc_multi	*hc = dch->hw;
	struct channel_req	*rq;
	int			err = 0;

	if (dch->debug & DEBUG_HW)
		printk(KERN_DEBUG "%s: cmd:%x %p\n",
		    __FUNCTION__, cmd, arg);
	switch (cmd) {
	case OPEN_CHANNEL:
		rq = arg;
		if ((rq->protocol == ISDN_P_TE_S0) ||
		    (rq->protocol == ISDN_P_NT_S0) ||
		    (rq->protocol == ISDN_P_TE_E1) ||
		    (rq->protocol == ISDN_P_NT_E1))
			err = open_dchannel(hc, dch, rq);
		else
			err = open_bchannel(hc, dch, rq); 
		break;
	case CLOSE_CHANNEL:
		if (debug & DEBUG_HW_OPEN)
			printk(KERN_DEBUG "%s: dev(%d) close from %p\n",
			    __FUNCTION__, dch->dev.id,
			    __builtin_return_address(0));
		module_put(THIS_MODULE);
		break;
	case CONTROL_CHANNEL:
		err = channel_dctrl(dch, arg);
		break;
	default:
		if (dch->debug & DEBUG_HW)
			printk(KERN_DEBUG "%s: unknown command %x\n",
			    __FUNCTION__, cmd);
		err = -EINVAL;
	}
	return err;
}

/*
 * initialize the card
 */

/*
 * start timer irq, wait some time and check if we have interrupts.
 * if not, reset chip and try again.
 */
static int
init_card(struct hfc_multi *hc)
{
	int 	err = -EIO;
	u_long		flags;
#ifdef CONFIG_PLX_PCI_BRIDGE
	u_short	*plx_acc;
#endif

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: entered\n", __FUNCTION__);

	spin_lock_irqsave(&hc->lock, flags);
	/* set interrupts but let global interrupt disabled */
	hc->hw.r_irq_ctrl = V_FIFO_IRQ;
	disable_hwirq(hc);
	spin_unlock_irqrestore(&hc->lock, flags);

	if (request_irq(hc->pci_dev->irq, hfcmulti_interrupt, IRQF_SHARED,
	    "HFC-multi", hc)) {
		printk(KERN_WARNING "mISDN: Could not get interrupt %d.\n",
		    hc->pci_dev->irq);
		return -EIO;
	}
	hc->irq = hc->pci_dev->irq;

#ifdef CONFIG_PLX_PCI_BRIDGE
	plx_acc = (u_short*)(hc->plx_membase+0x4c);
	*plx_acc = 0x41;  /* enable PCI & LINT1 irq */
#endif

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: IRQ %d count %d\n",
		    __FUNCTION__, hc->irq, hc->irqcnt);
	if ((err = init_chip(hc))) {
		goto error;
	}
	/*
	 * Finally enable IRQ output
	 * this is only allowed, if an IRQ routine is allready
	 * established for this HFC, so don't do that earlier
	 */
	spin_lock_irqsave(&hc->lock, flags);
	enable_hwirq(hc);
	spin_unlock_irqrestore(&hc->lock, flags);
	// printk(KERN_DEBUG "no master irq set!!!\n");
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout((100*HZ)/1000); /* Timeout 100ms */
	/* turn IRQ off until chip is completely initialized */
	spin_lock_irqsave(&hc->lock, flags);
	disable_hwirq(hc);
	spin_unlock_irqrestore(&hc->lock, flags);
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: IRQ %d count %d\n",
		    __FUNCTION__, hc->irq, hc->irqcnt);
	if (hc->irqcnt) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: done\n", __FUNCTION__);

		return 0;
	}
	if (test_bit(HFC_CHIP_PCM_SLAVE, &hc->chip)) {
		printk(KERN_INFO "ignoring missing interrupts\n");
		return 0;
	}

	printk(KERN_ERR "HFC PCI: IRQ(%d) getting no interrupts during init.\n",
		hc->irq);


#ifdef CONFIG_PLX_PCI_BRIDGE
	plx_acc = (u_short*)(hc->plx_membase + 0x4c);
	*plx_acc = 0x00;  /* disable PCI & LINT1 irq */
#endif
	err = -EIO;

error:
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_WARNING "%s: free irq %d\n", __FUNCTION__, hc->irq);
	if (hc->irq) {
		free_irq(hc->irq, hc);
		hc->irq = 0;
	}

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: done (err=%d)\n", __FUNCTION__, err);
	return err;
}

/*
 * find pci device and set it up
 */

static int
setup_pci(struct hfc_multi *hc, struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct hm_map	*m = (struct hm_map *)ent->driver_data;	

	printk(KERN_INFO
	    "HFC-multi: card manufacturer: '%s' card name: '%s' clock: %s\n",
	    m->vendor_name, m->card_name, m->clock2 ? "double" : "normal");

	hc->pci_dev = pdev;
	if (m->clock2)
		test_and_set_bit(HFC_CHIP_CLOCK2, &hc->chip);

	if (ent->device == 0xB410)
	{
		test_and_set_bit(HFC_CHIP_B410P, &hc->chip);
		test_and_set_bit(HFC_CHIP_PCM_MASTER, &hc->chip);
		test_and_clear_bit(HFC_CHIP_PCM_SLAVE, &hc->chip);
		hc->slots = 32;
	}

	if (hc->pci_dev->irq <= 0) {
		printk(KERN_WARNING "HFC-multi: No IRQ for PCI card found.\n");
		return -EIO;
	}
	if (pci_enable_device(hc->pci_dev)) {
		printk(KERN_WARNING "HFC-multi: Error enabling PCI card.\n");
		return -EIO;
	}
	hc->leds = m->leds;
	hc->ledstate = 0xAFFEAFFE;
	hc->opticalsupport = m->opticalsupport;

#ifdef CONFIG_HFCMULTI_PCIMEM
	hc->pci_membase = NULL;
	hc->plx_membase = NULL;

#ifdef CONFIG_PLX_PCI_BRIDGE
	hc->plx_origmembase =  hc->pci_dev->resource[0].start;
	/* MEMBASE 1 is PLX PCI Bridge */

	if (!hc->plx_origmembase) {
		printk(KERN_WARNING
		    "HFC-multi: No IO-Memory for PCI PLX bridge found\n");
		pci_disable_device(hc->pci_dev);
		return -EIO;
	}

	if (!(hc->plx_membase = ioremap(hc->plx_origmembase, 128))) {
		printk(KERN_WARNING
		    "HFC-multi: failed to remap plx address space. "
		    "(internal error)\n");
		hc->plx_membase = NULL;
		pci_disable_device(hc->pci_dev);
		return -EIO;
	}
	printk(KERN_WARNING "HFC-multi: plx_membase:%#x plx_origmembase:%#x\n",
	    (u_int) hc->plx_membase, (u_int)hc->plx_origmembase);

	hc->pci_origmembase =  hc->pci_dev->resource[2].start;
	    /* MEMBASE 1 is PLX PCI Bridge */
	if (!hc->pci_origmembase) {
		printk(KERN_WARNING
		    "HFC-multi: No IO-Memory for PCI card found\n");
		pci_disable_device(hc->pci_dev);
		return -EIO;
	}

	if (!(hc->pci_membase = ioremap(hc->pci_origmembase, 0x400))) {
		printk(KERN_WARNING "HFC-multi: failed to remap io "
		    "address space. (internal error)\n");
		hc->pci_membase = NULL;
		pci_disable_device(hc->pci_dev);
		return -EIO;
	}

	printk(KERN_INFO
	    "%s: defined at MEMBASE %#x (%#x) IRQ %d HZ %d leds-type %d\n",
	    hc->name, (u_int) hc->pci_membase, (u_int) hc->pci_origmembase,
	    hc->pci_dev->irq, HZ, hc->leds);
	pci_write_config_word(hc->pci_dev, PCI_COMMAND, PCI_ENA_MEMIO);
#else /* CONFIG_PLX_PCI_BRIDGE */
	hc->pci_origmembase = hc->pci_dev->resource[1].start;
	if (!hc->pci_origmembase) {
		printk(KERN_WARNING
		    "HFC-multi: No IO-Memory for PCI card found\n");
		pci_disable_device(hc->pci_dev);
		return -EIO;
	}

	if (!(hc->pci_membase = ioremap(hc->pci_origmembase, 256))) {
		printk(KERN_WARNING
		    "HFC-multi: failed to remap io address space. "
		    "(internal error)\n");
		hc->pci_membase = NULL;
		pci_disable_device(hc->pci_dev);
		return -EIO;
	}
	printk(KERN_INFO "card %d: defined at MEMBASE %#x (%#x) IRQ %d HZ %d "
	    "leds-type %d\n", hc->id, (u_int) hc->pci_membase,
	    (u_int) hc->pci_origmembase, hc->pci_dev->irq, HZ, hc->leds);
	pci_write_config_word(hc->pci_dev, PCI_COMMAND, PCI_ENA_MEMIO);
#endif /* CONFIG_PLX_PCI_BRIDGE */
#else
	hc->pci_iobase = (u_int) hc->pci_dev->resource[0].start;
	if (!hc->pci_iobase) {
		printk(KERN_WARNING "HFC-multi: No IO for PCI card found\n");
		pci_disable_device(hc->pci_dev);
		return -EIO;
	}

	if (!request_region(hc->pci_iobase, 8, "hfcmulti")) {
		printk(KERN_WARNING "HFC-multi: failed to request address "
		    "space at 0x%04x (internal error)\n", hc->pci_iobase);
		hc->pci_iobase = 0;
		pci_disable_device(hc->pci_dev);
		return -EIO;
	}

	printk(KERN_INFO
	    "%s %s: defined at IOBASE %#x IRQ %d HZ %d leds-type %d\n",
	    m->vendor_name, m->card_name, (u_int) hc->pci_iobase,
	    hc->pci_dev->irq, HZ, hc->leds);
	pci_write_config_word(hc->pci_dev, PCI_COMMAND, PCI_ENA_REGIO);
#endif

	pci_set_drvdata(hc->pci_dev, hc);

	/* At this point the needed PCI config is done */
	/* fifos are still not enabled */
	return 0;
}


/*
 * remove port
 */

static void
release_port(struct hfc_multi *hc, struct dchannel *dch)
{
	int	pt, ci, i = 0;
	u_long	flags;
	struct bchannel *pb;

	ci = dch->slot;
	pt = hc->chan[ci].port;

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: entered for port %d\n",
			__FUNCTION__, pt + 1);

	if (pt >= hc->ports) {
		printk(KERN_WARNING "%s: ERROR port out of range (%d).\n",
		     __FUNCTION__, pt + 1);
		return;
	}

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: releasing port=%d\n",
		    __FUNCTION__, pt + 1);

	if (dch->dev.D.protocol == ISDN_P_TE_S0)
		l1_event(dch->l1, CLOSE_CHANNEL);

	hc->chan[ci].dch = NULL;

	if (hc->created[pt]) {
		hc->created[pt] = 0;
		mISDN_unregister_device(&dch->dev);
	}

	spin_lock_irqsave(&hc->lock, flags);

	if (dch->timer.function) {
		del_timer(&dch->timer);
		dch->timer.function = NULL;
	}

	if (hc->type == 1) { /* E1 */
		/* free channels */
		for (i = 0; i < 32; i++) {
			if (hc->chan[i].bch) {
				if (debug & DEBUG_HFCMULTI_INIT)
					printk(KERN_DEBUG
					    "%s: free port %d channel %d\n",
					    __FUNCTION__, hc->chan[i].port+1,i);
				pb = hc->chan[i].bch;
				hc->chan[i].bch = NULL;
				spin_unlock_irqrestore(&hc->lock, flags);
				mISDN_freebchannel(pb);
				kfree(pb);
				spin_lock_irqsave(&hc->lock, flags);
			}
		}
	} else {
		if (hc->chan[ci - 2].bch) {
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG
				    "%s: free port %d channel %d\n",
				    __FUNCTION__, hc->chan[ci - 2].port+1,
				    ci - 2);
			pb = hc->chan[ci - 2].bch;
			hc->chan[ci - 2].bch = NULL;
			spin_unlock_irqrestore(&hc->lock, flags);
			mISDN_freebchannel(pb);
			kfree(pb);
			spin_lock_irqsave(&hc->lock, flags);
		}
		if (hc->chan[ci - 1].bch) {
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG
				    "%s: free port %d channel %d\n",
				    __FUNCTION__, hc->chan[ci - 1].port+1,
				    ci - 1);
			pb = hc->chan[ci - 1].bch;
			hc->chan[ci - 1].bch = NULL;
			spin_unlock_irqrestore(&hc->lock, flags);
			mISDN_freebchannel(pb);
			kfree(pb);
			spin_lock_irqsave(&hc->lock, flags);
		}
	}

	spin_unlock_irqrestore(&hc->lock, flags);

	mISDN_freedchannel(dch);
	kfree(dch);

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: done!\n", __FUNCTION__);
}

static void
release_card(struct hfc_multi *hc)
{
	u_long	flags;
	int	ch;

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_WARNING "%s: release card (%d) entered\n",
		    __FUNCTION__, hc->id);

	spin_lock_irqsave(&hc->lock, flags);
	disable_hwirq(hc);
	spin_unlock_irqrestore(&hc->lock, flags);

	udelay(1000);

	/* dimm leds */
	if (hc->leds)
		hfcmulti_leds(hc);

	/* disable D-channels & B-channels */
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: disable all channels (d and b)\n",
		    __FUNCTION__);
	for (ch = 0; ch < 32; ch++) {
		if (hc->chan[ch].dch)
			release_port(hc, hc->chan[ch].dch);
	}

	/* release hardware & irq */
	release_io_hfcmulti(hc);
	if (hc->irq) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_WARNING "%s: free irq %d\n",
			    __FUNCTION__, hc->irq);
		free_irq(hc->irq, hc);
		hc->irq = 0;

	}

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_WARNING "%s: remove instance from list\n",
		     __FUNCTION__);
	list_del(&hc->list);

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_WARNING "%s: delete instance\n", __FUNCTION__);
	kfree(hc);
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_WARNING "%s: card successfully removed\n",
		    __FUNCTION__);
}

static int
init_e1_port(struct hfc_multi *hc, struct hm_map *m)
{
	struct dchannel	*dch;
	struct bchannel	*bch;
	int		ch, ret = 0;
	char		name[MISDN_MAX_IDLEN];

	dch = kzalloc(sizeof(struct dchannel), GFP_KERNEL);
	if (!dch)
		return -ENOMEM;
	dch->debug = debug;
	mISDN_initdchannel(dch, MAX_DFRAME_LEN_L1, ph_state_change);
	dch->hw = hc;
	dch->dev.Dprotocols = (1 << ISDN_P_TE_E1) | (1 << ISDN_P_NT_E1);
	dch->dev.Bprotocols = (1 << (ISDN_P_B_RAW & ISDN_P_B_MASK)) |
	    (1 << (ISDN_P_B_HDLC & ISDN_P_B_MASK)); 
	dch->dev.D.send = handle_dmsg;
	dch->dev.D.ctrl = hfcm_dctrl;
	dch->dev.nrbchan = (hc->dslot)?30:31;
	dch->slot = hc->dslot;
	hc->chan[hc->dslot].dch = dch;
	hc->chan[hc->dslot].port = 0;
	hc->chan[hc->dslot].nt_timer = -1;
	for (ch = 1; ch <= 31; ch++) {
		if (ch == hc->dslot) // skip dchannel
			continue;
		bch = kzalloc(sizeof(struct bchannel), GFP_KERNEL);
		if (!bch) {
			printk(KERN_ERR "%s: no memory for bchannel\n",
			    __FUNCTION__);
			ret = -ENOMEM;
			goto free_chan;
		}
		bch->nr = ch;
		bch->slot = ch;
		bch->debug = debug;
		mISDN_initbchannel(bch, MAX_DATA_MEM);
		bch->hw = hc;
		bch->ch.send = handle_bmsg;
		bch->ch.ctrl = hfcm_bctrl;
		bch->ch.nr = ch;
		list_add(&bch->ch.list, &dch->dev.bchannels);
		hc->chan[ch].bch = bch;
		hc->chan[ch].port = 0;
		test_and_set_bit(bch->nr, &dch->dev.channelmap[0]);
	}
	/* set optical line type */
	if (port[Port_cnt] & 0x001) {
		if (!m->opticalsupport)  {
			printk(KERN_INFO
			    "This board has no optical "
			    "support\n");
		} else {
			if (debug & DEBUG_HFCMULTI_INIT)
				printk(KERN_DEBUG
				    "%s: PROTOCOL set optical "
				    "interfacs: card(%d) "
				    "port(%d)\n",
				    __FUNCTION__,
				    HFC_cnt + 1, 1);
			test_and_set_bit(HFC_CFG_OPTICAL,
			    &hc->chan[hc->dslot].cfg);
		}
	}
	/* set LOS report */
	if (port[Port_cnt] & 0x004) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: PROTOCOL set "
			    "LOS report: card(%d) port(%d)\n",
			    __FUNCTION__, HFC_cnt + 1, 1);
		test_and_set_bit(HFC_CFG_REPORT_LOS,
		    &hc->chan[hc->dslot].cfg);
	}
	/* set AIS report */
	if (port[Port_cnt] & 0x008) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: PROTOCOL set "
			    "AIS report: card(%d) port(%d)\n",
			    __FUNCTION__, HFC_cnt + 1, 1);
		test_and_set_bit(HFC_CFG_REPORT_AIS,
		    &hc->chan[hc->dslot].cfg);
	}
	/* set SLIP report */
	if (port[Port_cnt] & 0x010) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: PROTOCOL set SLIP report: "
			    "card(%d) port(%d)\n",
			    __FUNCTION__, HFC_cnt + 1, 1);
		test_and_set_bit(HFC_CFG_REPORT_SLIP,
		    &hc->chan[hc->dslot].cfg);
	}
#if 0
	/* set RDI report */
	if (port[Port_cnt] & 0x020) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: PROTOCOL set RDI report: "
			    "card(%d) port(%d)\n",
			    __FUNCTION__, HFC_cnt + 1, 1);
		test_and_set_bit(HFC_CFG_REPORT_RDI,
		    &hc->chan[hc->dslot].cfg);
	}
#endif
	/* set elastic jitter buffer */
	if (port[Port_cnt] & 0x0c0) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: PROTOCOL set elastic "
			    "buffer to %d: card(%d) port(%d)\n",
			    __FUNCTION__, hc->chan[ch].jitter,
			    HFC_cnt + 1, 1);
		hc->chan[hc->dslot].jitter =
		    (port[Port_cnt]>>6) & 0x3;
	} else
		hc->chan[ch].jitter = 2; /* default */
	/* set CRC-4 Mode */
	if (! (port[Port_cnt] & 0x100)) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: PROTOCOL turn on CRC4 report:"
				" card(%d) port(%d)\n",
				__FUNCTION__, HFC_cnt + 1, 1);
		test_and_set_bit(HFC_CFG_CRC4,
		    &hc->chan[hc->dslot].cfg);
	} else {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG "%s: PROTOCOL turn off CRC4"
		   		" report: card(%d) port(%d)\n",
		   		__FUNCTION__, HFC_cnt + 1, 1);
	}
	snprintf(name, MISDN_MAX_IDLEN - 1, "hfc-e1.%d", HFC_cnt + 1);
	ret = mISDN_register_device(&dch->dev, name);
	if (ret)
		goto free_chan;
	hc->created[0] = 1;
	return ret;
free_chan:
	release_port(hc, dch);
	return ret;	
}

static int
init_multi_port(struct hfc_multi *hc, int pt)
{
	struct dchannel	*dch;
	struct bchannel	*bch;
	int		ch, i, ret = 0;
	char		name[MISDN_MAX_IDLEN];

	dch = kzalloc(sizeof(struct dchannel), GFP_KERNEL);
	if (!dch)
		return -ENOMEM;
	dch->debug = debug;
	mISDN_initdchannel(dch, MAX_DFRAME_LEN_L1, ph_state_change);
	dch->hw = hc;
	dch->dev.Dprotocols = (1 << ISDN_P_TE_S0) | (1 << ISDN_P_NT_S0);
	dch->dev.Bprotocols = (1 << (ISDN_P_B_RAW & ISDN_P_B_MASK)) |
	    (1 << (ISDN_P_B_HDLC & ISDN_P_B_MASK)); 
	dch->dev.D.send = handle_dmsg;
	dch->dev.D.ctrl = hfcm_dctrl;
	dch->dev.nrbchan = 2;
	i = pt << 2;
	dch->slot = i + 2;
	hc->chan[i + 2].dch = dch;
	hc->chan[i + 2].port = pt;
	hc->chan[i + 2].nt_timer = -1;
	for (ch = 0; ch < dch->dev.nrbchan; ch++) {
		bch = kzalloc(sizeof(struct bchannel), GFP_KERNEL);
		if (!bch) {
			printk(KERN_ERR "%s: no memory for bchannel\n",
			    __FUNCTION__);
			ret = -ENOMEM;
			goto free_chan;
		}
		bch->nr = ch + 1;
		bch->slot = i + ch;
		bch->debug = debug;
		mISDN_initbchannel(bch, MAX_DATA_MEM);
		bch->hw = hc;
		bch->ch.send = handle_bmsg;
		bch->ch.ctrl = hfcm_bctrl;
		bch->ch.nr = ch + 1;
		list_add(&bch->ch.list, &dch->dev.bchannels);
		hc->chan[i + ch].bch = bch;
		hc->chan[i + ch].port = pt;
		test_and_set_bit(bch->nr, &dch->dev.channelmap[0]);
	}
	/* set master clock */
	if (port[Port_cnt] & 0x001) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: PROTOCOL set master clock: "
			    "card(%d) port(%d)\n",
			    __FUNCTION__, HFC_cnt + 1, pt + 1);
		if (dch->dev.D.protocol != ISDN_P_TE_S0) {
			printk(KERN_ERR "Error: Master clock "
			    "for port(%d) of card(%d) is only"
			    " possible with TE-mode\n",
			    pt + 1, HFC_cnt + 1);
			ret = -EINVAL;
			goto free_chan;
		}
		if (hc->masterclk >= 0) {
			printk(KERN_ERR "Error: Master clock "
			    "for port(%d) of card(%d) already "
			    "defined for port(%d)\n",
			    pt + 1, HFC_cnt + 1, hc->masterclk+1);
			ret = -EINVAL;
			goto free_chan;
		}
		hc->masterclk = pt;
	}
	/* set transmitter line to non capacitive */
	if (port[Port_cnt] & 0x002) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: PROTOCOL set non capacitive "
			    "transmitter: card(%d) port(%d)\n",
			    __FUNCTION__, HFC_cnt + 1, pt + 1);
		test_and_set_bit(HFC_CFG_NONCAP_TX,
		    &hc->chan[i + 2].cfg);
	}
	/* disable E-channel */
	if (port[Port_cnt] & 0x004) {
	if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: PROTOCOL disable E-channel: "
			    "card(%d) port(%d)\n",
			    __FUNCTION__, HFC_cnt + 1, pt + 1);
		test_and_set_bit(HFC_CFG_DIS_ECHANNEL,
		    &hc->chan[i + 2].cfg);
	}
#if 0
	/* register E-channel */
	if (protocol[Port_cnt] & 0x008) {
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: PROTOCOL register E-channel: "
			    "card(%d) port(%d)\n",
			    __FUNCTION__, HFC_cnt + 1, pt + 1);
		test_and_set_bit(HFC_CFG_REG_ECHANNEL,
		    &hc->chan[i + 2].cfg);
	}
#endif
	snprintf(name, MISDN_MAX_IDLEN - 1, "hfc-%ds.%d/%d", 
		hc->type, HFC_cnt + 1, pt + 1);
	ret = mISDN_register_device(&dch->dev, name);
	if (ret)
		goto free_chan;
	hc->created[pt] = 1;
	return ret;
free_chan:
	release_port(hc, dch);
	return ret;	
}

static int
hfcpci_init(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct hm_map	*m = (struct hm_map *)ent->driver_data;	
	int		ret_err = 0;
	int		pt;
	struct hfc_multi	*hc;
	u_long		flags;
	u_char		dips = 0, pmj = 0; /* dip settings, port mode Jumpers */

	if (HFC_cnt >= MAX_CARDS) {
		printk(KERN_ERR "too many cards (max=%d).\n",
			MAX_CARDS);
		return -EINVAL;
	}
	if (type[HFC_cnt] && (type[HFC_cnt] & 0xff) != m->type) {
		printk(KERN_WARNING "HFC-MULTI: Card '%s:%s' type %d found but "
		    "type[%d] %d was supplied as module parameter\n",
		    m->vendor_name, m->card_name, m->type, HFC_cnt,
		    type[HFC_cnt] & 0xff);
		printk(KERN_WARNING "HFC-MULTI: Load module without parameters "
			"first, to see cards and their types.");
		return -EINVAL;
	}
	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: Registering %s:%s chip type %d (0x%x)\n",
		    __FUNCTION__, m->vendor_name, m->card_name, m->type,
		    type[HFC_cnt]);

	/* allocate card+fifo structure */
	if (!(hc = kzalloc(sizeof(struct hfc_multi), GFP_KERNEL))) {
		printk(KERN_ERR "No kmem for HFC-Multi card\n");
		return -ENOMEM;
	}
	spin_lock_init(&hc->lock);
	hc->mtyp = m;
	hc->type =  m->type;
	hc->ports = m->ports;
	hc->id = HFC_cnt;
	hc->pcm = pcm[HFC_cnt];
	if (dslot[HFC_cnt] < 0) {
		hc->dslot = 0;
		printk(KERN_INFO "HFC-E1 card has disabled D-channel, but "
			"31 B-channels\n");
	} if (dslot[HFC_cnt] > 0 && dslot[HFC_cnt] < 32) {
		hc->dslot = dslot[HFC_cnt];
		printk(KERN_INFO "HFC-E1 card has alternating D-channel on "
			"time slot %d\n", dslot[HFC_cnt]);
	} else
		hc->dslot = 16;

	/* set chip specific features */
	hc->masterclk = -1;
	if (type[HFC_cnt] & 0x100) {
		test_and_set_bit(HFC_CHIP_ULAW, &hc->chip);
		silence = 0xff; /* ulaw silence */
	} else
		silence = 0x2a; /* alaw silence */
	if (!(type[HFC_cnt] & 0x200))
		test_and_set_bit(HFC_CHIP_DTMF, &hc->chip);

	if (type[HFC_cnt] & 0x800)
		test_and_set_bit(HFC_CHIP_PCM_SLAVE, &hc->chip);
	if (type[HFC_cnt] & 0x1000) {
		test_and_set_bit(HFC_CHIP_PCM_MASTER, &hc->chip);
		test_and_clear_bit(HFC_CHIP_PCM_SLAVE, &hc->chip);
	}
	if (type[HFC_cnt] & 0x2000)
		test_and_set_bit(HFC_CHIP_RX_SYNC, &hc->chip);
	if (type[HFC_cnt] & 0x4000)
		test_and_set_bit(HFC_CHIP_EXRAM_128, &hc->chip);
	if (type[HFC_cnt] & 0x8000)
		test_and_set_bit(HFC_CHIP_EXRAM_512, &hc->chip);
	hc->slots = 32;
	if (type[HFC_cnt] & 0x10000)
		hc->slots = 64;
	if (type[HFC_cnt] & 0x20000)
		hc->slots = 128;
	if (type[HFC_cnt] & 0x40000)
		test_and_set_bit(HFC_CHIP_CRYSTAL_CLOCK, &hc->chip);
	if (type[HFC_cnt] & 0x80000) {
		test_and_set_bit(HFC_CHIP_WATCHDOG, &hc->chip);
		hc->wdcount = 0;
		hc->wdbyte = V_GPIO_OUT2;
		printk(KERN_NOTICE "Watchdog enabled\n");
	}

	/* setup pci */
	ret_err = setup_pci(hc, pdev, ent);
	if (ret_err) {
		kfree(hc);
		return ret_err;
	}

	/* crate channels */
	for (pt = 0; pt < hc->ports; pt++) {
		if (Port_cnt >= MAX_PORTS) {
			printk(KERN_ERR "too many ports (max=%d).\n",
				MAX_PORTS);
			ret_err = -EINVAL;
			goto free_card;
		}
		if (hc->type == 1)
			ret_err = init_e1_port(hc, m);
		else
			ret_err = init_multi_port(hc, pt);
		if (debug & DEBUG_HFCMULTI_INIT)
			printk(KERN_DEBUG
			    "%s: Registering D-channel, card(%d) port(%d)"
			    "result %d\n",
			    __FUNCTION__, HFC_cnt + 1, pt, ret_err);

		if (ret_err) {
			while (pt) { /* release already registered ports */
				pt--;
				release_port(hc, hc->chan[(pt << 2) +2].dch);
			}
			goto free_card;
		}
		Port_cnt++;
	}

	/* disp switches */
	switch (m->dip_type) {
	case DIP_4S:
		/*
		 * get DIP Setting for beroNet 1S/2S/4S cards
		 *  check if Port Jumper config matches
		 * module param 'protocol'
		 * DIP Setting: (collect GPIO 13/14/15 (R_GPIO_IN1) +
		 * GPI 19/23 (R_GPI_IN2))
		 */
		dips = ((~HFC_inb(hc, R_GPIO_IN1) & 0xE0) >> 5) |
			((~HFC_inb(hc, R_GPI_IN2) & 0x80) >> 3) |
			(~HFC_inb(hc, R_GPI_IN2) & 0x08);

		/* Port mode (TE/NT) jumpers */
		pmj = ((HFC_inb(hc, R_GPI_IN3) >> 4)  & 0xf);

		if (test_bit(HFC_CHIP_B410P, &hc->chip))
			pmj = ~pmj & 0xf;

		printk(KERN_INFO "%s: %s DIPs(0x%x) jumpers(0x%x)\n",
			m->vendor_name, m->card_name, dips, pmj);
		break;
	case DIP_8S:
		/*
		 * get DIP Setting for beroNet 8S0+ cards
		 *
		 * enable PCI auxbridge function
		 */
#ifndef CONFIG_HFCMULTI_PCIMEM
		HFC_outb(hc, R_BRG_PCM_CFG, 1 | V_PCM_CLK);
		/* prepare access to auxport */
		outw(0x4000, hc->pci_iobase + 4);
		/*
		 * some dummy reads are required to
		 * read valid DIP switch data
		 */
		dips = inb(hc->pci_iobase);
		dips = inb(hc->pci_iobase);
		dips = inb(hc->pci_iobase);
		dips = ~inb(hc->pci_iobase) & 0x3F;
		outw(0x0, hc->pci_iobase + 4);
		/* disable PCI auxbridge function */
		HFC_outb(hc, R_BRG_PCM_CFG, V_PCM_CLK);
		printk(KERN_INFO "%s: %s DIPs(0x%x)\n", 
		    m->vendor_name, m->card_name, dips);
#endif
		break;
	case DIP_E1:
		/*
		 * get DIP Setting for beroNet E1 cards
		 * DIP Setting: collect GPI 4/5/6/7 (R_GPI_IN0)
		 */
		dips = (~HFC_inb(hc, R_GPI_IN0) & 0xF0)>>4;
		printk(KERN_INFO "%s: %s DIPs(0x%x)\n",
		    m->vendor_name, m->card_name, dips);
		break;
	}

	/* add to list */
	write_lock_irqsave(&HFClock, flags);
	list_add_tail(&hc->list, &HFClist);
	write_unlock_irqrestore(&HFClock, flags);

	/* initialize hardware */
	ret_err = init_card(hc);
	if (ret_err) {
		printk(KERN_ERR "init card returns %d\n", ret_err);
		release_card(hc);
		return ret_err;
	}

	/* start IRQ and return */
	spin_lock_irqsave(&hc->lock, flags);
	enable_hwirq(hc);
	spin_unlock_irqrestore(&hc->lock, flags);
	return 0;

free_card:
	release_io_hfcmulti(hc);
	kfree(hc);		
	return ret_err;
}

static void __devexit hfc_remove_pci(struct pci_dev *pdev)
{
	struct hfc_multi	*card = pci_get_drvdata(pdev);
	u_long			flags;

	if (debug)
		printk(KERN_INFO "removing hfc_multi card vendor:%x "
		    "device:%x subvendor:%x subdevice:%x\n",
		    pdev->vendor, pdev->device,
		    pdev->subsystem_vendor, pdev->subsystem_device);

	if (card) {
		write_lock_irqsave(&HFClock, flags);
		release_card(card);
		write_unlock_irqrestore(&HFClock, flags);
	}  else {
		if (debug)
			printk(KERN_WARNING "%s: drvdata allready removed\n",
			    __FUNCTION__);
	}
}

static const struct hm_map hfcm_map[] =
{
	{VENDOR_BN, "HFC-1S Card (mini PCI)", 4, 1, 1, 3, 0, DIP_4S},
	{VENDOR_BN, "HFC-2S Card", 4, 2, 1, 3, 0, DIP_4S},
	{VENDOR_BN, "HFC-2S Card (mini PCI)", 4, 2, 1, 3, 0, DIP_4S},
	{VENDOR_BN, "HFC-4S Card", 4, 4, 1, 2, 0, DIP_4S},
	{VENDOR_BN, "HFC-4S Card (mini PCI)", 4, 4, 1, 2, 0, 0},
	{VENDOR_CCD, "HFC-4S Eval (old)", 4, 4, 0, 0, 0, 0},
	{VENDOR_CCD, "HFC-4S IOB4ST", 4, 4, 1, 2, 0, 0},
	{VENDOR_CCD, "HFC-4S", 4, 4, 1, 2, 0, 0},
	{VENDOR_DIG, "HFC-4S Card", 4, 4, 0, 2, 0, 0},
	{VENDOR_CCD, "HFC-4S Swyx 4xS0 SX2 QuadBri", 4, 4, 1, 2, 0, 0},
	{VENDOR_JH, "HFC-4S (junghanns 2.0)", 4, 4, 1, 2, 0, 0},
	{VENDOR_PRIM, "HFC-2S Primux Card", 4, 2, 0, 0, 0, 0},

	{VENDOR_BN, "HFC-8S Card", 8, 8, 1, 0, 0, 0},
	{VENDOR_BN, "HFC-8S Card (+)", 8, 8, 1, 8, 0, DIP_8S},
	{VENDOR_CCD, "HFC-8S Eval (old)", 8, 8, 0, 0, 0, 0},
	{VENDOR_CCD, "HFC-8S IOB4ST Recording", 8, 8, 1, 0, 0, 0},

	{VENDOR_CCD, "HFC-8S IOB8ST", 8, 8, 1, 0, 0, 0},
	{VENDOR_CCD, "HFC-8S", 8, 8, 1, 0, 0, 0},
	{VENDOR_CCD, "HFC-8S", 8, 8, 1, 0, 0, 0},

	/* E1 only supports single clock */
	{VENDOR_BN, "HFC-E1 Card", 1, 1, 0, 1, 0, DIP_E1},
	/* E1 only supports single clock */
	{VENDOR_BN, "HFC-E1 Card (mini PCI)", 1, 1, 0, 1, 0, 0},
	/* E1 only supports single clock */
	{VENDOR_BN, "HFC-E1+ Card (Dual)", 1, 1, 0, 1, 0, DIP_E1},
	/* E1 only supports single clock */
	{VENDOR_BN, "HFC-E1 Card (Dual)", 1, 1, 0, 1, 0, DIP_E1},

	{VENDOR_CCD, "HFC-E1 Eval (old)", 1, 1, 0, 0, 0, 0},
	/* E1 only supports single clock */
	{VENDOR_CCD, "HFC-E1 IOB1E1", 1, 1, 0, 1, 0, 0},
	/* E1 only supports single clock */
	{VENDOR_CCD, "HFC-E1", 1, 1, 0, 1, 0, 0},

	/* PLX PCI-Bridge */
	{VENDOR_CCD, "HFC-4S PCIBridgeEval", 4, 4, 0, 0, 0, 0},
#if 0
	{VENDOR_CCD, "HFC-4S CCAG Eval", 4, 4, 0, 0, 0, 0},
	{VENDOR_CCD, "HFC-8S CCAG Eval", 8, 8, 0, 0, 0, 0},
	/* E1 only supports single clock */
	{VENDOR_CCD, "HFC-E1 CCAG Eval", 1, 1, 0, 0, 0, 0},
#endif
};

#undef H
#define H(x)	(unsigned long)&hfcm_map[x]
static struct pci_device_id hfmultipci_ids[] __devinitdata = {

	/* Cards with HFC-4S Chip */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB567, 0, 0, H(0)}, /* BN1S mini PCI */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB566, 0, 0, H(1)}, /* BN2S */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB569, 0, 0, H(2)}, /* BN2S mini PCI */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB560, 0, 0, H(3)}, /* BN4S */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB568, 0, 0, H(4)}, /* BN4S mini PCI */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0x08B4, 0, 0, H(5)}, /* Old Eval */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB520, 0, 0, H(6)}, /* IOB4ST */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB620, 0, 0, H(7)}, /* 4S */
	{ 0xD161, 0xB410, 0xD161, 0xB410, 0, 0, H(8)}, /* 4S - Digium */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB540, 0, 0, H(9)}, /* 4S Swyx */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0xB550, 0, 0, H(10)},
	    /* 4S junghanns 2.0 */
	{ CCAG_VID, 0x08B4, CCAG_VID, 0x1234, 0, 0, H(11)}, /* Primux */

	/* Cards with HFC-8S Chip */
	{ CCAG_VID, 0x16B8, CCAG_VID, 0xB562, 0, 0, H(12)}, /* BN8S */
	{ CCAG_VID, 0x16B8, CCAG_VID, 0xB56B, 0, 0, H(13)}, /* BN8S+ */
	{ CCAG_VID, 0x16B8, CCAG_VID, 0x16B8, 0, 0, H(14)}, /* old Eval */
	{ CCAG_VID, 0x16B8, CCAG_VID, 0xB521, 0, 0, H(15)},
	    /* IOB8ST Recording */
	{ CCAG_VID, 0x16B8, CCAG_VID, 0xB522, 0, 0, H(16)}, /* IOB8ST  */
	{ CCAG_VID, 0x16B8, CCAG_VID, 0xB552, 0, 0, H(17)}, /* IOB8ST  */
	{ CCAG_VID, 0x16B8, CCAG_VID, 0xB622, 0, 0, H(18)}, /* 8S */


	/* Cards with HFC-E1 Chip */
	{ CCAG_VID, 0x30B1, CCAG_VID, 0xB563, 0, 0, H(19)}, /* BNE1 */
	{ CCAG_VID, 0x30B1, CCAG_VID, 0xB56A, 0, 0, H(20)}, /* BNE1 mini PCI */
	{ CCAG_VID, 0x30B1, CCAG_VID, 0xB565, 0, 0, H(21)}, /* BNE1 + (Dual) */
	{ CCAG_VID, 0x30B1, CCAG_VID, 0xB564, 0, 0, H(22)}, /* BNE1 (Dual) */

	{ CCAG_VID, 0x30B1, CCAG_VID, 0x30B1, 0, 0, H(23)}, /* Old Eval */
	{ CCAG_VID, 0x30B1, CCAG_VID, 0xB523, 0, 0, H(24)}, /* IOB1E1 */
	{ CCAG_VID, 0x30B1, CCAG_VID, 0xC523, 0, 0, H(25)}, /* E1 */

	{ 0x10B5, 0x9030, CCAG_VID, 0x3136, 0, 0, H(26)},
	    /* PLX PCI Bridge */
	{ CCAG_VID, 0x08B4, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{ CCAG_VID, 0x16B8, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{ CCAG_VID, 0x30B1, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
#if 0 
	/* you cannot claim all PLX to this driver */
	{ 0x10B5, 0x9030, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	    /* PLX PCI Bridge */
#endif
	{0, }
};
#undef H

MODULE_DEVICE_TABLE(pci, hfmultipci_ids);

static int
hfcpci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct hm_map	*m = (struct hm_map *)ent->driver_data;
	int		ret;

	if (m == NULL) {
		if (ent->vendor == CCAG_VID)
			if (ent->device == HFC4S_ID ||
			    ent->device == HFC8S_ID ||
			    ent->device == HFCE1_ID)
				printk(KERN_ERR
				    "unknown HFC multiport controller "
				    "(vendor:%x device:%x subvendor:%x "
				    "subdevice:%x) Please contact the "
				    "driver maintainer for support.\n",
				    ent->vendor, ent->device,
				    ent->subvendor, ent->subdevice);
		return -ENODEV;
	}
	ret = hfcpci_init(pdev, ent);
	if (ret) {
		return ret;
	}
	HFC_cnt++;
	printk(KERN_INFO "%d devices registered\n", HFC_cnt);
	return 0;
}

static struct pci_driver hfcmultipci_driver = {
	name:		"hfc_multi",
	probe:		hfcpci_probe,
	remove:		__devexit_p(hfc_remove_pci),
	id_table:	hfmultipci_ids,
};

static void __exit
HFCmulti_cleanup(void)
{
	struct hfc_multi *card, *next;

	/* unload interrupt function symbol */
	if (hfc_interrupt) {
		symbol_put(ztdummy_extern_interrupt);
	}
	if (register_interrupt) {
		symbol_put(ztdummy_register_interrupt);
	}
	if (unregister_interrupt) {
		if (interrupt_registered) {
			interrupt_registered = 0;
			unregister_interrupt();
		}
		symbol_put(ztdummy_unregister_interrupt);
	}

	list_for_each_entry_safe(card, next, &HFClist, list) {
		release_card(card);
	}
	/* get rid of all devices of this driver */
	pci_unregister_driver(&hfcmultipci_driver);
}

static int __init
HFCmulti_init(void)
{
	int err;

	if (debug & DEBUG_HFCMULTI_INIT)
		printk(KERN_DEBUG "%s: init entered\n", __FUNCTION__);

#ifdef __BIG_ENDIAN
#error "not running on big endian machines now"
#endif
	hfc_interrupt = symbol_get(ztdummy_extern_interrupt);
	register_interrupt = symbol_get(ztdummy_register_interrupt);
	unregister_interrupt = symbol_get(ztdummy_unregister_interrupt);
	printk(KERN_INFO "mISDN: HFC-multi driver %s\n",
	    hfcmulti_revision);

	switch (poll) {
	case 0:
		poll_timer = 6;
		poll = 128;
		break;
		/*
		 * wenn dieses break nochmal verschwindet,
		 * gibt es heisse ohren :-)
		 * "without the break you will get hot ears ???"
		 */
	case 8:
		poll_timer = 2;
		break;
	case 16:
		poll_timer = 3;
		break;
	case 32:
		poll_timer = 4;
		break;
	case 64:
		poll_timer = 5;
		break;
	case 128:
		poll_timer = 6;
		break;
	case 256:
		poll_timer = 7;
		break;
	default:
		printk(KERN_ERR
		    "%s: Wrong poll value (%d).\n", __FUNCTION__, poll);
		err = -EINVAL;
		return err;

	}

	err = pci_register_driver(&hfcmultipci_driver);
	if (err < 0) {
		printk(KERN_ERR "error registering pci driver: %x\n", err);
		if (hfc_interrupt) {
			symbol_put(ztdummy_extern_interrupt);
		}
		if (register_interrupt) {
			symbol_put(ztdummy_register_interrupt);
		}
		if (unregister_interrupt) {
			if (interrupt_registered) {
				interrupt_registered = 0;
				unregister_interrupt();
			}
			symbol_put(ztdummy_unregister_interrupt);
		}
		return err;
	}
	return 0;
}


#ifdef MODULE
module_init(HFCmulti_init);
module_exit(HFCmulti_cleanup);
#endif
