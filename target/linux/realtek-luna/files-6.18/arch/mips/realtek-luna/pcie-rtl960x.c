// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek "Luna" RTL960x PCIe host controller driver.
 *
 * ONE driver, THREE chips, selected at runtime from the device-tree root
 * compatible. It has to be one file: `pcibios_map_irq()` and
 * `pcibios_plat_dev_init()` are GLOBAL arch hooks with exactly one definition
 * per kernel, so two per-chip host drivers cannot coexist in one image even
 * when only one of them would ever register.
 *
 * ★★ THE ACCEPTANCE TEST FOR THE RTL9602C IS A BYTE-IDENTICAL REGISTER
 * SEQUENCE. Its table entry below is the previous file's constants value for
 * value, and every field the RTL9602C does not have is 0 with the caller
 * SKIPPING it -- so any behaviour change on the X111W is a defect of this
 * refactor and not of the new chip.
 *
 * Independent implementation from each SoC's register interface. The RTL9602C
 * facts were established by direct hardware probing of an X111W-A10 (reading
 * back live controller state, bisecting the bring-up, confirming each field
 * against observed link behaviour). The RTL9603CVD facts were established from
 * the LANLY G24W's own stock kernel image -- its `bsp_pcie_reset`,
 * `pci1_controller`, `pcie1_phy_params`, `PCIE_reset_pin` and built-in DTB --
 * and cross-checked against that chip's own 4.4 SDK C source and against the
 * board's own boot console.
 *
 * ┌─ per chip ────────────────┬─ RTL9602C (X111W) ─┬─ RTL9603CVD (G24W) ─────┐
 * │ live PCIe port            │ 0                  │ 1 (port 0 not wired)    │
 * │ hostcfg / devcfg / hostext│ b8b00000 / b8b10000 / b8b01000  (SAME)       │
 * │ MEM / IO window           │ 19000000+16M / 18c00000+64K     (SAME)       │
 * │ SOC_PINMUX 0xb800004c     │ |= 0x10000000      │ REGISTER DOES NOT EXIST │
 * │ SOC_PCI_MISC 0xb8000504   │ strobe BIT(24)|BIT(21) │ strobe BIT(21) only │
 * │ SOC_IP_SEL   0xb8000600   │ MAC BIT(7), plus    │ MAC BIT(6), NO extras  │
 * │                           │ BIT(26)+0,2,11,12   │                        │
 * │ SerDes ePHY table         │ 23 pairs, 25 MHz    │ 10 pairs, no refclk    │
 * │                           │ refclk block        │ variant on this part   │
 * │ endpoint PERST#           │ not software-driven │ GPIO 40, ACTIVE LOW    │
 * │ INTC input for INTx       │ 15                  │ 16                     │
 * └───────────────────────────┴────────────────────┴─────────────────────────┘
 *
 * ⚠ THE THREE 9602C CONSTANTS THAT WOULD HAVE BEEN CARRIED OVER SILENTLY. On
 * the RTL9603CVD, `SOC_PINMUX` is not a register at all (nothing in that chip's
 * stock kernel touches offset 0x4c, and its chipdef names 0x48/0x4c
 * CFG_PCSXF / CFG_PHY_CTRL); `IP_SEL` bit 7 is the OTHER port's MAC gate; and
 * bits 26/0/2/11/12 are never set by stock and enable unidentified IP blocks.
 * This is the same defect class that put BASE_PHYAD=25 on the G24W's PHY bus
 * and a 9602C SerDes window into 9603CVD silicon -- a sibling literal on the
 * wrong die, consumed by the hardware with nothing to read back.
 *
 * Bring-up, per chip, in the order that chip's own boot does it: (optionally)
 * assert PERST#, MDIO reset, MAC-enable pulse, PHY reset + SerDes tuning,
 * (optionally) release PERST#, then poll the LTSSM for L0 (0x728[4:0] == 0x11).
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/types.h>

#include <asm/addrspace.h>

/* SoC system controller registers (KSEG1, uncached). Present on BOTH chips
 * except where the table says otherwise -- see SOC_PINMUX. */
#define SOC_PINMUX	((void __iomem *)0xb800004cul)	/* 9602C only          */
#define SOC_PCI_MISC	((void __iomem *)0xb8000504ul)	/* PCIe MDIO reset     */
#define SOC_IP_SEL	((void __iomem *)0xb8000600ul)	/* per-IP MAC enable   */

#define PINMUX_PCIE		0x10000000u	/* PCIe mux bit in SOC_PINMUX     */
#define PCI_MISC_MDIO_CLR	BIT(14)		/* MDIO reset: cleared in reset   */
#define PCI_MISC_MDIO_P0	BIT(24)		/* MDIO reset strobe, port 0      */
#define PCI_MISC_MDIO_P1	BIT(21)		/* MDIO reset strobe, port 1      */
#define IP_SEL_EN_PCIE0		BIT(7)		/* port-0 PCIe MAC enable (gate)  */
#define IP_SEL_EN_PCIE1		BIT(6)		/* port-1 PCIe MAC enable (gate)  */
/* 9602C ONLY: bits the operational config sets alongside the PCIe MAC
 * (IP_SEL = 0x04001887). Without them the endpoint's config space never decodes
 * (reads the 0xeeeeeeee abort pattern) even though the link trains.
 * ⚠ The RTL9603CVD's stock kernel NEVER sets any of them; leave its ip_pre_or 0. */
#define IP_SEL_EN_PCIE_PHY	BIT(26)		/* 9602C PCIe SerDes/PHY enable   */
#define IP_SEL_EN_EXTRA		(BIT(0) | BIT(2) | BIT(11) | BIT(12))

/* hostext offsets, identical on both chips. */
#define HOSTEXT_MDIO	0x000	/* PHY MDIO write: [31:16]=val [15:8]=reg bit0=go */
#define HOSTEXT_LTSSM	0x008	/* bit7 = PHY_RST_N, bit0 = LTSSM enable */
#define HOSTEXT_FN	0x00c	/* PCI function-number select            */

/* hostcfg offsets, identical on both chips. */
#define HOSTCFG_CMD	0x004	/* command/status                        */
#define HOSTCFG_PAYLOAD	0x078	/* MAX_PAYLOAD_SIZE in bits [7:5]        */
#define HOSTCFG_SPEED	0x082	/* [3:0] link speed: 1 = 2.5 GT/s        */
#define HOSTCFG_LINK	0x728	/* LTSSM state; [4:0]==0x11 => link up   */
#define HOSTCFG_CFGCTL	0x80c	/* bit17 enables endpoint config access  */
#define CFGCTL_FWD_EN	BIT(17)	/* write-enable; reads back as 0x100 once active */

#define LINK_UP_STATE	0x11u

struct luna_pcie_phy { u8 reg; u16 val; };

/*
 * RTL9602C PCIe SerDes PHY tuning, written over the host MDIO before link
 * training. These are the revB analog values that match this RTL9602C (rev A)
 * SerDes. The match is load-bearing beyond just training: regs 0x20/0x21 set the
 * SerDes PLL and clock divider that also clock the downstream endpoint's config
 * core. A sibling part's revC table (0x20=0xd4a4/0x21=0x485a) still bit-locks
 * the lane to L0, but leaves the endpoint's config-core clock mistuned so every
 * config TLP aborts (reads back the 0xeeeeeeee pattern). {reg, val} pairs,
 * terminated by reg 0xff.
 */
static const struct luna_pcie_phy luna_pcie_phy_9602c[] = {
	{ 0x01, 0xa852 }, { 0x06, 0x0017 }, { 0x08, 0x3591 }, { 0x09, 0x520c },
	{ 0x0a, 0xf670 }, { 0x0b, 0xa90d }, { 0x0d, 0xe720 }, { 0x0e, 0x1000 },
	{ 0x1c, 0x2001 }, { 0x1e, 0x66eb }, { 0x20, 0xd4a4 }, { 0x21, 0x485a },
	{ 0x23, 0x0b66 }, { 0x24, 0x4f0c }, { 0x29, 0xf0f3 }, { 0x2b, 0xa0a1 },
	{ 0x09, 0x500c }, { 0x09, 0x520c },
	/* 25 MHz reference-clock SerDes values (the board has a 25 MHz crystal at
	 * the WiFi PCIe PHY). This is the vendor "9602C 25M clk" ePHY table; reg
	 * 0x03 is the refclk PLL multiplier (0x3031 for 25 MHz, vs 0x7b31 for
	 * 40 MHz) and reg 0x06 = 0xe0b8 (vs 0xe2b8). A 40 MHz PLL on a 25 MHz
	 * refclk mistunes the config-core clock -> marginal high-offset access. */
	{ 0x03, 0x3031 }, { 0x06, 0xe0b8 }, { 0x0e, 0x98c5 },
	{ 0x0f, 0x400f }, { 0x19, 0xfc70 },
	{ 0xff, 0xffff },
};

/*
 * RTL9603CVD PCIe SerDes ePHY tuning -- port 1. TEN pairs, and NOT a subset of
 * the 9602C's: reg 0x20/0x21, the pair the comment above calls load-bearing for
 * the endpoint's config-core clock, are 0x0105/0x1000 here against 0xd4a4/0x485a
 * there. Carrying the sibling table over is exactly the failure that comment
 * describes.
 *
 * ★ AND THERE IS NO REFERENCE-CLOCK VARIANT ON THIS PART. The 9602C table above
 * carries a second 25 MHz block because that chip's vendor code selects between
 * a 25 MHz and a 40 MHz ePHY table. The RTL9603CVD's bring-up has ONE table, no
 * strap, no DT property and no branch -- so this board's crystal frequency is
 * NOT NEEDED to train the link, and it is also NOT ESTABLISHED. It is named here
 * rather than inferred from the sibling.
 *
 * Established two independent ways that agree pair for pair: read as bytes from
 * `pcie1_phy_params` in the G24W's own stock kernel image (tier 2), and from
 * that chip's own 4.4 SDK C source (tier 3).
 */
static const struct luna_pcie_phy luna_pcie_phy_9603cvd[] = {
	{ 0x00, 0x8a50 }, { 0x02, 0x26f9 }, { 0x03, 0x6bcd }, { 0x06, 0x1088 },
	{ 0x08, 0x4a45 }, { 0x09, 0x6303 }, { 0x0b, 0x0009 }, { 0x0c, 0x0800 },
	{ 0x20, 0x0105 }, { 0x21, 0x1000 },
	{ 0xff, 0xffff },
};

/*
 * RTL9607C has two independent PCIe hosts.  The port-0 recipe below is the raw
 * table selected by PCIE_reset_procedure() and is proven to train the OP2200H's
 * RTL8812FE.  Port 1 is subtler: this revision-C SoC prints "IC-C v006", and
 * the stock __pcie_param_fixup() at 0x807335d0 replaces its raw 13-pair table
 * with the effective 18-pair recipe below immediately before the MDIO loop.
 * Using the unpatched raw table leaves the RTL8192F in LTSSM state 0x2.
 */
static const struct luna_pcie_phy luna_pcie_phy_9607c_p0[] = {
	{ 0x00, 0x8a50 }, { 0x02, 0x26f9 }, { 0x03, 0x6bcd }, { 0x06, 0x104a },
	{ 0x09, 0x6307 }, { 0x0b, 0x0009 }, { 0x0c, 0x0800 }, { 0x20, 0x0105 },
	{ 0x21, 0x1000 },
	{ 0xff, 0xffff },
};

static const struct luna_pcie_phy luna_pcie_phy_9607c_p1[] = {
	{ 0x01, 0xa852 }, { 0x06, 0x0017 }, { 0x08, 0x3591 }, { 0x09, 0x520c },
	{ 0x0a, 0xf670 }, { 0x0b, 0xa90d }, { 0x0d, 0xe720 }, { 0x0e, 0x1000 },
	{ 0x1c, 0x2001 }, { 0x1e, 0x66eb }, { 0x20, 0xd4a4 }, { 0x21, 0x485a },
	{ 0x23, 0x0b66 }, { 0x24, 0x4f0c }, { 0x29, 0xf0f3 }, { 0x2b, 0xa0a1 },
	{ 0x09, 0x500c }, { 0x09, 0x520c },
	{ 0xff, 0xffff },
};

/*
 * The per-chip table. A field that is 0 is NOT PRESENT on that chip and the
 * caller SKIPS the step -- never writes zero to the register. That idiom is the
 * whole reason this refactor is safe for the RTL9602C: every field the 9602C
 * has keeps its previous value, and every field only the RTL9603CVD has is 0
 * there, so the X111W's emitted register sequence does not move.
 */
struct luna_pcie_chip {
	const char *name;
	const char *root_compat;	/* DT root compatible selecting this entry */
	const char *intc_compat;	/* which INTC maps the aggregated INTx	 */
	unsigned int hwirq;		/* that INTC's INPUT number for PCIe INTx */

	unsigned long hostcfg;		/* root-bridge config window, KSEG1	 */
	unsigned long devcfg;		/* endpoint config window, KSEG1	 */
	unsigned long hostext;		/* host-controller extension, KSEG1	 */
	u32 mem_phys, mem_size;
	u32 io_phys, io_size;
	u32 mem_cfg_phys;		/* bridge register value; 0 = mem_phys */
	u32 io_cfg_phys;		/* bridge register value; 0 = io_phys  */
	u32 dev_cmd;			/* downstream command/status value      */

	u32 pinmux_bit;			/* SOC_PINMUX bit; 0 = no such register	 */
	u32 misc_clear;			/* SOC_PCI_MISC bits cleared before strobe */
	u32 misc_set;			/* SOC_PCI_MISC bits held set during strobe */
	u32 misc_strobe;		/* SOC_PCI_MISC reset bit(s) for THIS port */
	u32 ip_mac_bit;			/* SOC_IP_SEL MAC gate for THIS port	 */
	u32 ip_pre_or;			/* extra SOC_IP_SEL bits; 0 = none	 */

	/* Endpoint PERST#, active LOW. All four 0 = not software-driven here. */
	unsigned long perst_pad_en;	/* IO_GPIO_EN word holding this pad	 */
	unsigned long perst_dir;	/* GPIO direction word (1 = output)	 */
	unsigned long perst_data;	/* GPIO data word			 */
	u8 perst_bit;

	const struct luna_pcie_phy *phy;
	unsigned int retries;		/* full reset+train attempts		 */
	unsigned int link_polls;	/* 10 ms polls per attempt		 */
	unsigned int ltssm_delay_ms;	/* 0 = legacy 50 ms settle		 */
};

static const struct luna_pcie_chip luna_pcie_9602c = {
	.name = "RTL9602C", .root_compat = "realtek,rtl9602c",
	.intc_compat = "realtek,rtl9602c-intc", .hwirq = 15,
	.hostcfg = 0xb8b00000ul, .devcfg = 0xb8b10000ul, .hostext = 0xb8b01000ul,
	.mem_phys = 0x19000000u, .mem_size = 0x01000000u,
	.io_phys = 0x18c00000u, .io_size = 0x00010000u,
	.dev_cmd = 0x00180007u,
	.pinmux_bit = PINMUX_PCIE,
	.misc_clear = PCI_MISC_MDIO_CLR,
	/* This board trains only with BOTH port reset bits strobed. */
	.misc_strobe = PCI_MISC_MDIO_P0 | PCI_MISC_MDIO_P1,
	.ip_mac_bit = IP_SEL_EN_PCIE0,
	.ip_pre_or = IP_SEL_EN_PCIE_PHY | IP_SEL_EN_EXTRA,
	/* PERST# is not driven by an SoC GPIO here: probing shows the candidate
	 * lines left as inputs while the link is up, so the endpoint is tied
	 * released. All four PERST fields stay 0 and the two steps are skipped. */
	.phy = luna_pcie_phy_9602c, .retries = 3, .link_polls = 10,
};

static const struct luna_pcie_chip luna_pcie_9603cvd = {
	.name = "RTL9603CVD", .root_compat = "realtek,rtl9603cvd",
	/* Aggregator input 16. TIER 1, from the board's own boot console: its
	 * vendor kernel prints the whole translate table, and `16=>50` is the
	 * PCIe row (`26=>36` is GMAC0, which our own eth driver already uses).
	 * 50 + the GISR domain's base 23 = Linux virq 73, and the vendor WiFi
	 * driver prints `b8b10000/b9000000/73` on this board. Three numbers close. */
	.intc_compat = "realtek,rtl9603cvd-intc", .hwirq = 16,
	/* PORT 1, not port 0. Port 0's constants exist in the stock code and are
	 * dead on this product -- its CPU-side interrupt number has no aggregator
	 * input at all (the translate table's row for it is -1). */
	.hostcfg = 0xb8b00000ul, .devcfg = 0xb8b10000ul, .hostext = 0xb8b01000ul,
	.mem_phys = 0x19000000u, .mem_size = 0x01000000u,
	.io_phys = 0x18c00000u, .io_size = 0x00010000u,
	.dev_cmd = 0x00180007u,
	/* ⚠ 0: SOC_PINMUX IS NOT A REGISTER ON THIS DIE. Nothing in this chip's
	 * stock kernel touches 0xb800004c, and its own chipdef names 0x48/0x4c
	 * CFG_PCSXF / CFG_PHY_CTRL -- the same pair that, written as the 9602C's
	 * IO_GPIO_EN, put BASE_PHYAD=25 on this board's PHY bus for weeks. */
	.pinmux_bit = 0,
	.misc_clear = PCI_MISC_MDIO_CLR,
	.misc_strobe = PCI_MISC_MDIO_P1,
	.ip_mac_bit = IP_SEL_EN_PCIE1,
	/* ⚠ 0 DELIBERATELY: stock never sets BIT(26) or BIT(0|2|11|12) here, and
	 * they enable IP blocks nobody has identified on this silicon. */
	.ip_pre_or = 0,
	/* PERST# = GPIO 40 = bank 1 bit 8, ACTIVE LOW. The pin comes from stock's
	 * own device tree (`board_setting` / `pci0_gpio_rst = <&bank1 8 0>`) and
	 * the board prints `PCIE0 reset pin is set to GPIO 40`. The three words are
	 * the SWCORE pad-enable IO_GPIO_EN+4 and the bank-1 direction/data pair. */
	.perst_pad_en = 0xbb000040ul, .perst_dir = 0xb8003324ul,
	.perst_data = 0xb8003328ul, .perst_bit = 8,
	.phy = luna_pcie_phy_9603cvd, .retries = 4, .link_polls = 9,
};

/*
 * OP2200H / RTL9607C, stock port 0: RTL8812FE (10ec:f812).
 *
 * The stock bridge writes the same legacy IO BAR value on both ports. Linux's
 * global resource tree cannot register overlapping host IO resources, so the
 * otherwise-unused 64 KiB window is split into two 32 KiB bookkeeping ranges.
 * io_cfg_phys preserves the exact stock BAR write. The WiFi drivers use the
 * separate MMIO BARs below, not this legacy IO BAR.
 */
static const struct luna_pcie_chip luna_pcie_9607c_p0 = {
	.name = "RTL9607C PCIe0", .root_compat = "ovt,op2200h",
	.intc_compat = "mti,gic", .hwirq = 56,
	.hostcfg = 0xb8b20000ul, .devcfg = 0xb8b30000ul, .hostext = 0xb8b21000ul,
	.mem_phys = 0x1a000000u, .mem_size = 0x01000000u,
	.io_phys = 0x18c00000u, .io_size = 0x00008000u,
	.mem_cfg_phys = 0x19000000u, .io_cfg_phys = 0x18c00000u,
	.dev_cmd = 0x00180007u,
	.misc_clear = BIT(15), .misc_set = BIT(30),
	.misc_strobe = PCI_MISC_MDIO_P0,
	.ip_mac_bit = IP_SEL_EN_PCIE0,
	/* RTL9607C GPIO bank 1 uses the second SWCORE pad-enable word:
	 * IO_GPIO_EN+4 = phys 0x1b00003c. The sibling RTL9603CVD layout above
	 * uses 0x1b000040; carrying that address here claims bank 2 instead and
	 * leaves GPIO40 unable to drive the endpoint's PERST# pin. */
	.perst_pad_en = 0xbb00003cul, .perst_dir = 0xb8003324ul,
	.perst_data = 0xb8003328ul, .perst_bit = 8,
	.phy = luna_pcie_phy_9607c_p0, .retries = 4, .link_polls = 100,
	.ltssm_delay_ms = 100,
};

/* OP2200H / RTL9607C, stock port 1: RTL8192F (10ec:818c). */
static const struct luna_pcie_chip luna_pcie_9607c_p1 = {
	.name = "RTL9607C PCIe1", .root_compat = "ovt,op2200h",
	.intc_compat = "mti,gic", .hwirq = 57,
	.hostcfg = 0xb8b00000ul, .devcfg = 0xb8b10000ul, .hostext = 0xb8b01000ul,
	.mem_phys = 0x19000000u, .mem_size = 0x01000000u,
	.io_phys = 0x18c08000u, .io_size = 0x00008000u,
	.mem_cfg_phys = 0x19000000u, .io_cfg_phys = 0x18c00000u,
	.dev_cmd = 0x00180007u,
	.misc_clear = BIT(14), .misc_strobe = PCI_MISC_MDIO_P1,
	.ip_mac_bit = IP_SEL_EN_PCIE1,
	.perst_pad_en = 0xbb00003cul, .perst_dir = 0xb8003324ul,
	.perst_data = 0xb8003328ul, .perst_bit = 7,
	.phy = luna_pcie_phy_9607c_p1, .retries = 4, .link_polls = 100,
	.ltssm_delay_ms = 100,
};

/* ★★★ THE CHIP TABLE MUST SURVIVE INIT -- IT IS NOT INIT DATA (measured
 * 2026-08-27 on the G24W, one panic, one boot lost to stock fallback).
 * `host->chip` is dereferenced by luna_pcie_access() on EVERY config-space access
 * for the life of the system, and config space is touched long after boot:
 * the first wlan0 open runs rtl92fe_hw_init -> pcie_capability_*() at ~21 s,
 * AFTER free_initmem(). With the tables (and the phy arrays their .phy
 * members point at) marked __initconst, that access read a recycled page:
 *   BadVA df414f10, epc luna_pcie_access+0xb8 (the hostext function-select
 *   write), Kernel panic, SoC watchdog full-chip reset, U-Boot autoboots
 *   the committed STOCK image -- a crash that presents as "the board came
 *   back on the wrong firmware". Boot-time enumeration worked because it
 *   runs BEFORE the free, which is exactly what kept this latent until
 *   wifi-scripts landed and something finally opened wlan0.
 * The original pcie-rtl9602c.c had no chip table and marked only its
 * init-walked PHY array __initconst; the rewrite introduced the defect.
 * Cost of residency: ~0.5 KB. Do not "optimise" it back. */
static DEFINE_SPINLOCK(luna_pcie_lock);

/* Runtime state belongs to a host controller, never to the SoC globally.
 * Keeping struct pci_controller first is intentional: MIPS stores its address
 * in pci_bus::sysdata, so config access and pcibios_map_irq() can recover the
 * enclosing host directly. This is equivalent to the old single global for
 * RTL9602C/RTL9603CVD and is the prerequisite for RTL9607C's two live hosts. */
struct luna_pcie_host {
	struct pci_controller controller;
	struct resource mem;
	struct resource io;
	const struct luna_pcie_chip *chip;
	u8 busnr;
	int virq;
};

static struct luna_pcie_host luna_pcie_hosts[2];

static inline void __iomem *pcie_hostcfg(const struct luna_pcie_host *host)
{
	return (void __iomem *)host->chip->hostcfg;
}

static inline void __iomem *pcie_devcfg(const struct luna_pcie_host *host)
{
	return (void __iomem *)host->chip->devcfg;
}

static inline void __iomem *pcie_hostext(const struct luna_pcie_host *host)
{
	return (void __iomem *)host->chip->hostext;
}

/* ---------- config-space accessors ----------
 *
 * Only two devices exist on this single-port root complex: the root bridge at
 * PCI slot 0 and the downstream endpoint at slot 1; their config windows sit at
 * fixed MMIO bases (a hardware fact). A small table maps slot -> window so a
 * single helper serves both reads and writes. The controller targets a function
 * by latching PCI_FUNC(devfn) into the host-extension function register just
 * before the access, so reads and writes are serialised against that latch.
 */

static int luna_pcie_access(struct pci_bus *bus, unsigned int devfn, int where,
			    int size, u32 *val, bool is_write)
{
	struct luna_pcie_host *host = bus->sysdata;
	unsigned int slot = PCI_SLOT(devfn);
	void __iomem *reg;
	unsigned long flags;

	if (!host || !host->chip)
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (host->busnr == 0xff)
		host->busnr = bus->number;
	if (bus->number != host->busnr || slot > 1)
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (size != 1 && size != 2 && size != 4)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* slot 0 = root bridge, slot 1 = the single downstream endpoint. */
	reg = (slot ? pcie_devcfg(host) : pcie_hostcfg(host)) + where;

	spin_lock_irqsave(&luna_pcie_lock, flags);
	writel(PCI_FUNC(devfn), pcie_hostext(host) + HOSTEXT_FN);
	mb();			/* order the function-select latch before the access */
	if (is_write) {
		if (size == 4)
			writel(*val, reg);
		else if (size == 2)
			writew(*val, reg);
		else
			writeb(*val, reg);
	} else if (size == 4) {
		*val = readl(reg);
	} else if (size == 2) {
		*val = readw(reg);
	} else {
		*val = readb(reg);
	}
	spin_unlock_irqrestore(&luna_pcie_lock, flags);
	return PCIBIOS_SUCCESSFUL;
}

static int luna_pcie_read(struct pci_bus *bus, unsigned int devfn,
			  int where, int size, u32 *val)
{
	int ret = luna_pcie_access(bus, devfn, where, size, val, false);

	if (ret != PCIBIOS_SUCCESSFUL)
		*val = ~0u;	/* absent device reads as all-ones */
	return ret;
}

static int luna_pcie_write(struct pci_bus *bus, unsigned int devfn,
			   int where, int size, u32 val)
{
	return luna_pcie_access(bus, devfn, where, size, &val, true);
}

static struct pci_ops luna_pcie_ops = {
	.read  = luna_pcie_read,
	.write = luna_pcie_write,
};

/* ---------- arch hooks ---------- */

int pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct luna_pcie_host *host = dev->bus->sysdata;
	const struct luna_pcie_chip *chip;

	/* The endpoint's INTx is aggregated by the SoC INTC onto a single input
	 * line; map that hwirq through the INTC's (linear) irq_domain to obtain
	 * the Linux virq the PCI core hands to the endpoint driver. Returning the
	 * raw hwirq would yield an unmapped virq (no_irq_chip) so request_irq()
	 * fails with -ENOSYS. The INTC drives handle_level_irq, so the line is
	 * already level-triggered. */
	if (!host || !host->chip)
		return 0;
	chip = host->chip;
	if (!host->virq) {
		struct device_node *np;

		np = of_find_compatible_node(NULL, NULL, chip->intc_compat);
		if (np) {
			struct irq_domain *domain = irq_find_host(np);

			of_node_put(np);
			if (domain)
				host->virq = irq_create_mapping(domain,
							      chip->hwirq);
		}
	}
	return host->virq;
}

int pcibios_plat_dev_init(struct pci_dev *dev)
{
	return 0;
}

/* ---------- bring-up ---------- */

/* Drive the endpoint PERST# pin. ACTIVE LOW: assert=0 holds the endpoint in
 * reset, release=1 lets it come up. A no-op on a chip that declares no pin.
 *
 * ★ THE DRIVER THAT NEEDS THE PAD OWNS THE MUX WRITE, so the pad-function
 * enable is done here and as a READ-MODIFY-WRITE of one bit. The alternative --
 * a gpio_chip for the whole SoC -- would be the larger and more useful piece of
 * work, and it is not what this driver needs to train a link.
 *
 * ⚠ THE PAD-ENABLE WORD IS SHARED, AND THIS IS THE ONE CROSS-DRIVER HAZARD HERE.
 * `IO_GPIO_EN` word 1 (SWCORE 0x40) is the same word the GPON driver's Board-C
 * optical-pad recipe writes WHOLESALE as SOC_IO_GPIO_EN_W1. That recipe is
 * skipped on the RTL9603CVD today ("GPIO optical-SD pad recipe skipped ... not
 * Board C's pinout"), so nothing clobbers this bit -- but a future change that
 * un-skips it would de-claim the PERST pad, and because PERST is a one-shot at
 * boot the link would already be trained and only a RE-train would fail. Named
 * here so it is not re-derived from a symptom.
 */
static void __init luna_pcie_perst(struct luna_pcie_host *host, bool assert)
{
	const struct luna_pcie_chip *chip = host->chip;
	u32 bit;

	if (!chip->perst_data)
		return;
	bit = 1u << chip->perst_bit;
	/* claim the pad for GPIO, then drive it as an output */
	writel(readl((void __iomem *)chip->perst_pad_en) | bit,
	       (void __iomem *)chip->perst_pad_en);
	writel(readl((void __iomem *)chip->perst_dir) | bit,
	       (void __iomem *)chip->perst_dir);
	if (assert)
		writel(readl((void __iomem *)chip->perst_data) & ~bit,
		       (void __iomem *)chip->perst_data);
	else
		writel(readl((void __iomem *)chip->perst_data) | bit,
		       (void __iomem *)chip->perst_data);
	mb();
}

/*
 * RAM-test diagnostics for boards whose user space has no /dev/mem.  Keep the
 * reads here, after the MAC gate has been enabled: touching a gated PCIe window
 * can otherwise stall the CPU bus.  Printing the full shared words lets one
 * successful controller act as a reference for the failing controller while
 * the decoded PERST level makes the active-low endpoint reset easy to verify.
 */
static void __init luna_pcie_diag(struct luna_pcie_host *host,
				  const char *result)
{
	const struct luna_pcie_chip *chip = host->chip;
	u32 pad_en = 0, dir = 0, data = 0;
	unsigned int perst = 1;

	if (chip->perst_data) {
		pad_en = readl((void __iomem *)chip->perst_pad_en);
		dir = readl((void __iomem *)chip->perst_dir);
		data = readl((void __iomem *)chip->perst_data);
		perst = !!(data & BIT(chip->perst_bit));
	}

	pr_info("realtek-pcie: %s diag %s: pad_en=%08x dir=%08x data=%08x perst=%u misc=%08x ip_sel=%08x ltssm_ctl=%08x state=%02x mdio=%08x\n",
		chip->name, result, pad_en, dir, data, perst,
		readl(SOC_PCI_MISC), readl(SOC_IP_SEL),
		readl(pcie_hostext(host) + HOSTEXT_LTSSM),
		readl(pcie_hostcfg(host) + HOSTCFG_LINK) & 0x1f,
		readl(pcie_hostext(host) + HOSTEXT_MDIO));
}

/*
 * Full PCIe host bring-up, in the controller's own reset order and timing.
 * Returns 0 once the LTSSM reaches L0 (state 0x11), -ETIMEDOUT otherwise. No
 * access to the 0xb8b0xxxx window happens before the MAC gate is set in step 2,
 * or the CPU bus stalls on an un-acked target.
 */
static int __init luna_pcie_reset(struct luna_pcie_host *host)
{
	const struct luna_pcie_chip *chip = host->chip;
	u32 v;
	int i;

	/* 0. Hold the endpoint in reset for the whole bring-up (RTL9603CVD), or
	 *    just spend the bare timing budget where PERST is tied (RTL9602C). */
	luna_pcie_perst(host, true);
	mdelay(10);

	/* 1. PCIe pin mux where the chip has one, then MDIO reset: clear the
	 *    reset-hold bit and this port's reset bit, then strobe it. */
	if (chip->pinmux_bit)
		writel(readl(SOC_PINMUX) | chip->pinmux_bit, SOC_PINMUX);
	v = readl(SOC_PCI_MISC) & ~(chip->misc_clear | chip->misc_strobe);
	v |= chip->misc_set;
	writel(v, SOC_PCI_MISC);
	mb();
	writel(v | chip->misc_strobe, SOC_PCI_MISC);
	mdelay(1);

	/* 2. Where the chip needs them, set the PHY + operational gates first;
	 *    then pulse ONLY this port's MAC enable (clear, then set) as the MAC
	 *    reset. On the RTL9602C the PHY-enable bit is left set throughout --
	 *    clearing it mid-bring-up resets the SerDes. */
	if (chip->ip_pre_or)
		writel(readl(SOC_IP_SEL) | chip->ip_pre_or, SOC_IP_SEL);
	v = readl(SOC_IP_SEL) & ~chip->ip_mac_bit;
	writel(v, SOC_IP_SEL);
	mb();
	writel(v | chip->ip_mac_bit, SOC_IP_SEL);
	mdelay(100);

	/* 3. Arm the LTSSM with the PHY held in reset, then release the PHY reset. */
	writel(0, pcie_hostext(host) + HOSTEXT_FN);
	writel(0x01, pcie_hostext(host) + HOSTEXT_LTSSM);	/* PHY in reset, LTSSM en */
	mb();
	writel(0x81, pcie_hostext(host) + HOSTEXT_LTSSM);	/* release PHY reset      */
	mdelay(chip->ltssm_delay_ms ? chip->ltssm_delay_ms : 50);

	/* 4. SerDes PHY tuning over MDIO -- required before POLLING can complete. */
	for (i = 0; chip->phy[i].reg != 0xff; i++) {
		writel(((u32)chip->phy[i].val << 16) |
		       ((u32)chip->phy[i].reg << 8) | 1,
		       pcie_hostext(host) + HOSTEXT_MDIO);
		mdelay(1);
	}
	mdelay(20);

	/* 5. Let the endpoint out of reset, immediately before training. */
	luna_pcie_perst(host, false);

	/* 6. Poll for link-up (L0). */
	for (i = 0; i < (int)chip->link_polls; i++) {
		if ((readl(pcie_hostcfg(host) + HOSTCFG_LINK) & 0x1f) == LINK_UP_STATE) {
			luna_pcie_diag(host, "link-up");
			return 0;
		}
		mdelay(10);
	}
	luna_pcie_diag(host, "timeout");
	return -ETIMEDOUT;
}

/*
 * Select the host(s) from the DEVICE TREE root, never from a Kconfig symbol:
 * one image serves multiple boards on the rtl9607x subtarget, and only the DT
 * identifies the actual board. RTL9607C access is additionally gated by an
 * explicit root boolean. The OP2200H DTS deliberately does not set it yet, so
 * ordinary milestone-1 images cannot touch untested PCIe registers.
 */
static unsigned int __init
luna_pcie_select(const struct luna_pcie_chip **selected)
{
	struct device_node *root;
	bool enabled = false;

	if (of_machine_is_compatible(luna_pcie_9602c.root_compat)) {
		selected[0] = &luna_pcie_9602c;
		return 1;
	}
	if (of_machine_is_compatible(luna_pcie_9603cvd.root_compat)) {
		selected[0] = &luna_pcie_9603cvd;
		return 1;
	}
	if (!of_machine_is_compatible(luna_pcie_9607c_p0.root_compat))
		return 0;

	root = of_find_node_by_path("/");
	if (root) {
		enabled = of_property_read_bool(root, "realtek,enable-pcie");
		of_node_put(root);
	}
	if (!enabled) {
		pr_info("realtek-pcie: OP2200H dual PCIe support present but disabled by DT safety gate\n");
		return 0;
	}

	selected[0] = &luna_pcie_9607c_p0;
	selected[1] = &luna_pcie_9607c_p1;
	return 2;
}

static int __init luna_pcie_init_host(struct luna_pcie_host *host,
				      const struct luna_pcie_chip *chip)
{
	u32 io_cfg_phys = chip->io_cfg_phys ? chip->io_cfg_phys : chip->io_phys;
	u32 mem_cfg_phys = chip->mem_cfg_phys ? chip->mem_cfg_phys : chip->mem_phys;
	unsigned int attempt;
	int ret = -ETIMEDOUT;

	host->chip = chip;
	host->busnr = 0xff;
	host->virq = 0;

	for (attempt = 0; attempt < chip->retries; attempt++) {
		ret = luna_pcie_reset(host);
		if (!ret)
			break;
		pr_info("realtek-pcie: %s link not trained (state=0x%x), retry %u/%u\n",
			chip->name,
			readl(pcie_hostcfg(host) + HOSTCFG_LINK) & 0x1f,
			attempt + 1, chip->retries);
	}
	if (ret) {
		pr_warn("realtek-pcie: %s link did not train after %u attempts (state=0x%x)\n",
			chip->name, chip->retries,
			readl(pcie_hostcfg(host) + HOSTCFG_LINK) & 0x1f);
		return ret;
	}

	/* Configuration-retry settle before any config/BAR access. */
	mdelay(100);

	/* Program the downstream endpoint's BARs + command register, then enable the
	 * host bridge (written twice), set 128 B max payload, and enable config
	 * forwarding. The forwarding-enable is the bit17 write-strobe -- the register
	 * then reads back the operational value 0x100, but writing 0x100 does not
	 * enable it. Values are table-driven because RTL9607C port 0's CPU MMIO
	 * aperture differs from the bus address written into the endpoint BAR. */
	writel(io_cfg_phys | 1u, pcie_devcfg(host) + 0x10);
	writel(mem_cfg_phys | 4u, pcie_devcfg(host) + 0x18);
	writel(chip->dev_cmd, pcie_devcfg(host) + 0x04);
	writel(0x00100007, pcie_hostcfg(host) + HOSTCFG_CMD);
	writel(0x00100007, pcie_hostcfg(host) + HOSTCFG_CMD);
	writeb(readb(pcie_hostcfg(host) + HOSTCFG_PAYLOAD) & ~0xe0,
	       pcie_hostcfg(host) + HOSTCFG_PAYLOAD);
	writel(readl(pcie_hostcfg(host) + HOSTCFG_CFGCTL) | CFGCTL_FWD_EN,
	       pcie_hostcfg(host) + HOSTCFG_CFGCTL);
	mb();

	/* Wait for the endpoint's config space to answer before the bus scan so the
	 * device is not skipped. Function 0 is selected on the host window. */
	for (attempt = 0; attempt < 20; attempt++) {	/* up to ~1 s */
		u32 id;

		writel(0, pcie_hostext(host) + HOSTEXT_FN);
		mb();
		id = readl(pcie_devcfg(host));
		if ((id & 0xffff) == PCI_VENDOR_ID_REALTEK)
			break;
		mdelay(50);
	}

	pr_info("realtek-pcie: %s link up at gen%u, bridge 0x%08x, endpoint 0x%08x\n",
		chip->name, readb(pcie_hostcfg(host) + HOSTCFG_SPEED) & 0xf,
		readl(pcie_hostcfg(host)), readl(pcie_devcfg(host)));

	host->mem.name = chip->name;
	host->mem.start = chip->mem_phys;
	host->mem.end = chip->mem_phys + chip->mem_size - 1;
	host->mem.flags = IORESOURCE_MEM;
	host->io.name = chip->name;
	host->io.start = chip->io_phys;
	host->io.end = chip->io_phys + chip->io_size - 1;
	host->io.flags = IORESOURCE_IO;
	host->controller.pci_ops = &luna_pcie_ops;
	host->controller.mem_resource = &host->mem;
	host->controller.io_resource = &host->io;
	/* MIPS resources are CPU addresses; BARs contain bus addresses. */
	host->controller.mem_offset = chip->mem_phys - mem_cfg_phys;
	host->controller.io_offset = chip->io_phys - io_cfg_phys;
	host->controller.io_map_base = CKSEG1ADDR(chip->io_phys);
	register_pci_controller(&host->controller);
	return 0;
}

static int __init luna_pcie_init(void)
{
	const struct luna_pcie_chip *selected[ARRAY_SIZE(luna_pcie_hosts)];
	unsigned int i, count, registered = 0;
	int first_err = 0;

	count = luna_pcie_select(selected);
	if (!count) {
		/* NOT an error and NOT a silence: a board whose root compatible
		 * names no enabled host has no bridge THIS DRIVER may touch. Accessing
		 * an undecoded 0xb8b0xxxx window can stall the CPU bus. */
		pr_info("realtek-pcie: no enabled PCIe host declared for this board -- not registering\n");
		return 0;
	}

	for (i = 0; i < count; i++) {
		int ret;

		ret = luna_pcie_init_host(&luna_pcie_hosts[i], selected[i]);
		if (ret) {
			if (!first_err)
				first_err = ret;
			continue;
		}
		registered++;
	}

	/* One absent or failed radio must not suppress the other controller. */
	return registered ? 0 : first_err;
}

/*
 * late_initcall: the SoC clock/IP_SEL setup must run after core platform init;
 * the PCI core scan that follows enumerates the endpoint.
 */
late_initcall(luna_pcie_init);
