# OP2200H bring-up status

Research snapshot: 2026-08-29.

The proven ESP32 bridge wiring and capture procedure imported from the UART
task are recorded in [uart-console.md](uart-console.md).

## Source revisions inspected

- `alphaonex86/openwrt` `main`: `2d744e904b192a6235309f162cce3821f88702ed`
  (2026-08-28, `gpon: the driver that serves three chips stops carrying one
  chip's name`). This is the working base.
- `jameywine/openwrt` `rtl9607c-dev`:
  `46dec5c00c4f24519afa455b129fabd94c0b63a8` (2026-08-18,
  `realtek: add a new device BT-PON BT-G711AX`). This is a reference tree, not
  the base.

## Reuse decisions

| Area | Reuse | OP2200H decision |
| --- | --- | --- |
| CPU, IRQ, timer, UART | Alpha `realtek-luna` RTL9607C platform | Reuse. Override the reference board's 288 MiB memory with the confirmed 256 MiB. |
| Clock, GPIO, SPI NAND/ECC | Jameywine RTL9607C platform and BT-G711AX DTS | Reference only. Its flash offsets differ, and milestone 1 declares no flash node. |
| Ethernet | Alpha `rtl960x_eth.c` | Do not enable yet. It currently assumes CPU port 9, PON port 5, and five copper ports. OP2200H Ethernet/switch mapping is unmeasured. |
| PCIe | Two-host RTL9607C bring-up proven by UART RAM boot | Both Gen1 links and endpoints enumerate with non-overlapping resources. The RTL8192F driver successfully requested GIC input 57 as Linux IRQ 57; GIC56 remains unrequested until an RTL8812FE driver binds. |
| 2.4 GHz | Alpha clean-room `rtl8192fe` | Reuse after RTL9607C PCIe works. Do not copy X111W calibration; extract OP2200H calibration/MAC data first. |
| 5 GHz | Jameywine research; Realtek GPL `rtl8192cd` | No current fetched branch contains an `10ec:f812` driver. Locate the experimental rtw88 work or implement it from a reviewable source before enabling the radio. |
| GPON/OMCI | Alpha shared GPON core plus `rtl9607c_gpon.c` | Defer. Preserve the unit's ONU identity and laser/optics configuration before connecting experimental firmware to the PON. |
| Persistent install | Neither tree | Out of scope. No sysupgrade or UBI image is generated. |

## Confirmed from the stock slot-0 boot

The private capture `captures/stock-slot0-boot.log` was inspected on
2026-08-29. It is intentionally ignored by Git because the stock userspace
prints credentials, MAC addresses, and PON identity to the console.

- SoC: RTL9607Cv2, revision C; four MIPS interAptiv VPEs; 256 MiB DDR3.
- Flash: 128 MiB SPI NAND, 2 KiB pages and 128 KiB eraseblocks.
- Stock partitions: `boot` 768 KiB, two 128 KiB environment partitions,
  `static_conf` 128 KiB, then `ubi_device` from `0x120000` to `0x7d80000`.
- The bootloader reported ECC errors while reading two pages in its protected
  boot area, although its U-Boot CRC still passed. Preserve a verified backup
  before any future flash work.
- PCIe port 0 trained at 2.5 GT/s and enumerated `10ec:f812`, RTL8812FE
  (stock `wlan0`, 5 GHz). Its PERST# is GPIO40.
- PCIe port 1 trained at 2.5 GT/s and enumerated `10ec:818c`, RTL8192F
  (stock `wlan1`, 2.4 GHz). Its PERST# is GPIO39.
- The active stock system maps `wlan0` to Linux IRQ 72 / MIPS GIC input 56 and
  `wlan1` to Linux IRQ 73 / MIPS GIC input 57. Both counters incremented on
  separate CPUs while the radios were active.
- Stock `rtl8192cd` 4.0.8.4 is built into the kernel and registered both
  interfaces successfully. This disproves the idea that only one radio can run
  at a time.
- The vendor driver does not register a conventional Linux PCI bus: the stock
  image has no `/sys/bus/pci`, and `/proc/iomem` contains no PCIe windows. The
  two radios are driven through hard-coded controller/config windows. OpenWrt
  must register two proper host controllers and expose each endpoint to the
  normal PCI core.
- The unstripped stock symbol table exposes `PCIE_reset_pin`,
  `PCIE1_reset_pin`, `PCIE_reset_procedure`, `rtl8192cd_init_hw_PCI`, and the
  global two-entry `wlan_device` table. RTL8812F-specific PCIe PHY symbols are
  also present. The SoC SerDes constants themselves are compiled into static
  code/data, so the active `ubi_k0` kernel volume must be disassembled rather
  than guessed.
- The stock log shows board-specific power, crystal, thermal and RF tables
  being applied separately to both radios. Those values must be extracted
  without copying credentials into the source tree.

## Recovered RTL9607C PCIe implementation

The active `ubi_k0` volume was read through `/dev/mtd6ro`, reconstructed and
verified against the router on 2026-08-29. `PCIE_reset_procedure` at
`0x805f42f4` and `wlan_device` at `0x80c25f74` establish two independent host
controllers, not one controller multiplexed between two radios:

| Stock port | Endpoint | Host / extension / endpoint config | CPU MMIO | IRQ | PERST# |
| --- | --- | --- | --- | --- | --- |
| 0 | `10ec:f812` RTL8812FE | `b8b20000` / `b8b21000` / `b8b30000` | `ba000000` (phys `1a000000`) | GIC 56 / Linux 72 | GPIO40 |
| 1 | `10ec:818c` RTL8192F | `b8b00000` / `b8b01000` / `b8b10000` | `b9000000` (phys `19000000`) | GIC 57 / Linux 73 | GPIO39 |

The stock port-0 raw SerDes table is:

```text
00=8a50 02=26f9 03=6bcd 06=104a 09=6307 0b=0009 0c=0800
20=0105 21=1000
```

The stock port-1 raw SerDes table is:

```text
00=8a50 02=26f9 03=6bcd 04=8049 06=1088 07=52b3 08=5285
09=6300 0b=0009 0c=0800 0e=0093 20=0105 21=1000
```

Both raw tables are terminated by `ff=ffff`. They are not, however, the final
values used on this revision-C board. The stock boot identifies `IC-C v006`,
and `__pcie_param_fixup()` at `0x807335d0` replaces the port-1 table in memory
before `PCIE_reset_procedure()` performs its MDIO loop. The effective port-1
recipe is:

```text
01=a852 06=0017 08=3591 09=520c 0a=f670 0b=a90d 0d=e720
0e=1000 1c=2001 1e=66eb 20=d4a4 21=485a 23=0b66 24=4f0c
29=f0f3 2b=a0a1 09=500c 09=520c
```

The reset routine also confirms:

- `SOC_IP_SEL` (`0xb8000600`) uses bit 7 for port 0 and bit 6 for port 1;
- `SOC_PCI_MISC` (`0xb8000504`) gives port 0 its bit-24 reset strobe and port 1
  its bit-21 strobe, with additional per-port reset-control differences;
- each host extension writes LTSSM control `0x01`, then `0x81`;
- link-up is host-config `0x728 & 0x1f == 0x11`, with up to four full attempts;
- each endpoint receives IO BAR `0x18c00001`, MEM BAR `0x19000004`, and command
  `0x00180007`; each host receives command `0x00100007`, 128-byte max payload,
  and the bit-17 config-forwarding strobe.

`pcie-rtl960x.c` now models the two RTL9607C hosts independently: each has its
own config windows, CPU memory aperture, bus identity and IRQ mapping while the
architecture's required global `pcibios_*` hooks dispatch through per-bus host
state. Port 0 translates PCI bus address `0x19000000` to CPU physical
`0x1a000000`; port 1 is identity-mapped at `0x19000000`. The shared stock legacy
IO BAR is retained as the programmed bus address, but its otherwise-unused host
resources are split into two non-overlapping 32 KiB CPU bookkeeping ranges.

RTL9607C register access remains disabled unless the OP2200H DT root explicitly
contains the boolean `realtek,enable-pcie`. The normal milestone-1 DTS does not
set it. Add it only to a RAM-only test DT/image; do not use the property as a
reason to create a persistent flash recipe.

## Milestone 1 profile

The `ovt_op2200h` profile supplies:

- model/compatible strings specific to OP2200H;
- 256 MiB RAM;
- UART0 at 115200 through the shared RTL9607C DTSI;
- no NAND node;
- disabled `rtl960x_eth` platform device;
- no `mtd` or `uboot-envtools` user-space package;
- no persistent image recipe.

Expected artifact after a successful build:

`bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h-initramfs-kernel.bin`

The name must be confirmed from `sha256sums`; do not type it into U-Boot from
this document alone.

## Reproducible configuration

Use a case-sensitive filesystem and a source path with no spaces. Seed the
configuration with the tracked fragment:

```sh
cp configs/op2200h-initramfs.config .config
make defconfig
make -j"$(nproc)"
```

On macOS use GNU make (`gmake`) and a case-sensitive APFS volume, or build in a
Linux VM/container whose source tree is stored in a case-sensitive volume. The
macOS build commands are `gmake defconfig` and
`gmake -j"$(sysctl -n hw.logicalcpu)"`. A Docker bind mount of a normal
case-insensitive macOS directory does not change the underlying filename
semantics.

The target metadata and `defconfig` were validated on 2026-08-29. A complete
build was then run from a case-sensitive APFS volume because the original
workspace path contains a space and resides on a case-insensitive filesystem.
On macOS, Linux 6.18's host `gen_init_cpio` also needs the portability additions
in `target/linux/generic/hack-6.18/200-tools_portability.patch`; these retain the
existing read/write fallback when `copy_file_range` and `O_LARGEFILE` are not
provided by the host. Reserve substantially more space than the source checkout
before starting a toolchain and kernel build.

For the PCIe experiment, select the separate non-persistent test profile:

```sh
cp configs/op2200h-pcie-initramfs.config .config
make defconfig
make -j"$(nproc)"
```

`ovt_op2200h_pcie_test` shares the normal board DTS through
`rtl9607c_ovt_op2200h-common.dtsi` and adds only the PCIe safety-gate boolean.
It still has no NAND node, excludes `mtd`/`uboot-envtools`, and declares no
persistent `KERNEL` or `IMAGE` recipe. Confirm the actual initramfs filename and
hash from the build's `sha256sums` before typing any U-Boot command.

The full profile build completed successfully on 2026-08-29. It produced only
the RAM-boot artifact below (plus metadata), not a sysupgrade or flash image:

```text
bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-initramfs-kernel.bin
SHA256 de63f6cc6a6440f901eda9d8e0c8c5acec52a732786e8da304c1bb0fc45b1804
```

The result is a legacy MIPS/Linux LZMA uImage with load and entry address
`0x80000000`; its payload is 4,278,589 bytes. Decompressing the payload and
comparing its tail against the compiled test DTB confirmed that it embeds
`rtl9607c_ovt_op2200h_pcie_test.dtb`, including `realtek,enable-pcie`.

The first UART RAM boot (artifact SHA256
`6cf809033d01423492aa8e0e24715117927f1367e79b51a2504c4360b0a74e27`)
reached a stable OpenWrt console but both links remained in LTSSM state `0x2`
for all four attempts. That result exposed a sibling-layout error in the
endpoint reset mapping: GPIO39/40 are RTL9607C bank 1, whose SWCORE pad-enable
word is physical `0x1b00003c`; the initial table incorrectly used
`0x1b000040`, claiming bank 2 while writing bank-1 direction/data registers.
The second UART test image contained the corrected bank-1 address.

The second UART RAM boot confirmed that correction: PCIe0 trained at Gen1,
enumerated root port `10ec:8196` and endpoint `10ec:f812`, translated PCI bus
BAR `0x19000000` to CPU resource `0x1a000000`, and reached a stable OpenWrt
console without a panic or reset. PCIe1 still remained in LTSSM state `0x2`
through four attempts, so the remaining work is isolated to the GPIO39/port-1
reset or PHY sequence; do not treat the dual-port implementation as complete
until `10ec:818c` enumerates independently as well.

The third RAM-test image added driver diagnostics. Because the initramfs kernel
intentionally has no `CONFIG_DEVMEM` and therefore no
`/dev/mem`, the driver now prints a read-only snapshot after each successful or
timed-out training attempt: the shared GPIO
pad-enable/direction/data words, decoded PERST# level, PCI reset and IP-gate
registers, LTSSM control/state, and final PHY-MDIO command. This permits a
direct comparison between working PCIe0 and failing PCIe1 without enabling an
arbitrary physical-memory write interface.

That diagnostic boot showed PCIe0 at L0 (`state=11`) with endpoint
`10ec:f812`. PCIe1 consistently reached only `state=02`, but its snapshot proved
that GPIO39 was claimed and driven as an output, PERST# was released high, the
port-1 reset strobe and MAC gate were set, and LTSSM control was `0x81`. This
ruled out the corrected GPIO mapping and exposed the raw-versus-effective PHY
table discrepancy above. The current artifact is the fourth RAM-test image and
changes only PCIe1 to stock's effective revision-C PHY recipe; PCIe0 retains its
already-proven sequence.

The fourth UART RAM boot completed the dual-host link/resource milestone. PCIe0
trained at Gen1 and enumerated `10ec:f812` at `0000:00:01.0`, with its 64-bit
MMIO BAR translated to CPU `0x1a000000-0x1a00ffff`. PCIe1 applied the expected
final MDIO register (`mdio=00000931`), reached L0 on its first attempt, and
enumerated `10ec:818c` at `0000:02:01.0`, with MMIO at
`0x19000000-0x1900ffff`. Both root bridges and their split IO resources
registered without overlap, and the system reached a stable OpenWrt console.
The in-tree `rtl8192fe` driver then bound to `10ec:818c`, but correctly warned
that this unit's factory calibration has not been supplied. Do not transmit
with generic calibration; IRQ routing and unit-specific calibration are the
next read-only checks.

The follow-up sysfs/procfs inventory confirmed that `rtl8192fe` owns
`0000:02:01.0` and requested GIC input 57 as Linux IRQ 57. The IRQ number differs
from stock's Linux IRQ 73 because the Linux 6.18 GIC domain uses a different
virtual-IRQ allocation; the hardware input is unchanged. Endpoint `10ec:f812`
correctly remains at the PCI core's unassigned IRQ value 255 because no
RTL8812FE driver is present to request GIC input 56. `phy0` exposes the expected
2.4 GHz HT20/HT40, two-stream MCS set, but its interface is down with a generic
Realtek MAC and no OP2200H calibration, so it must not transmit yet.

The subsequent read-only stock `/proc/wlan1/mib_rf` capture identified the
2.4 GHz radio as RTL8192FnB, 2T2R, with `rfe_type=3`, `pa_type=0`,
`trswitch=0`, thermal meter 42 (`0x2a`), XCAP 16 (`0x10`) and regulatory
domain 1. It also confirmed the four 14-channel base-power arrays already seen
in the stock startup commands. No unit MAC or other identity-bearing value is
stored in the source profile.

The fifth RAM-test image adds an OP2200H-only board-calibration profile using
those base-power, thermal, crystal and RF-front-end values. It also refactors
the previous calibration helper so RFE and external-PA selection belong to each
board profile instead of being hardcoded globally. Stock's nonzero HT20/OFDM
delta arrays are packed per channel, whereas the current rtlwifi efuse model
stores per-rate-count deltas. R5 therefore has an explicit OP2200H TX lock in
`rtl92fe_hw_init()`: it can prove the correct profile was selected at PCI probe,
but any attempt to open the interface is refused until the per-channel deltas
are represented exactly.

The R5 UART boot passed. Both PCIe links again reached L0 on their first
attempt, both endpoints enumerated, and the driver logged the exact OP2200H
profile (`xtal=0x10`, `thermal=0x2a`, `cck[A1]=0x20`, `ht40[A1]=0x27`,
`rfe=3`, `ext_pa=0`, `reg=1`). `wlan0` remained `state DOWN` and cfg80211
reported 0.00 dBm. The zero MAC in the profile was deliberately ignored; the
core supplied its temporary generic fallback rather than embedding a unit
identity. The same capture exposed two independent follow-ups: CPU1 still
fails to start, and cfg80211 rejected the compiled-in `regulatory.db` because
the database did not match its maintainer signature. Neither failure prevented
the locked, down WiFi device from probing, but the regulatory failure must be
closed before a transmission test.

```text
bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-initramfs-kernel.bin
SHA256 89a64c42ee1093b0c0ba53aec037f1da8704c311d9ae39425c868f2789e4516f
```

The sixth RAM-test image implements the stock packed delta format exactly.
The GPL rtl8192cd implementation confirms that each channel byte stores path A
in its low signed nibble and path B in its high signed nibble, and applies the
OFDM, HT20 and HT40-2S deltas independently. For example, nibble `f` is -1,
not +15. R6 preserves the three stock arrays byte-for-byte and decodes them in
the tx-power-index path. Transmission still fails closed after every boot:
the root-only `op2200h_allow_tx` module parameter must be explicitly set before
the interface-open hardware initialization is permitted, and that opt-in is
lost on reboot.

Source reference: [Realtek GPL `PHY_RF6052SetOFDMTxPower()`](https://github.com/cgoder/openwrt_rtk/blob/master/rtk_openwrt_sdk/target/linux/rtkmips/files/drivers/net/wireless/rtl8192cd/8192cd_hw.c).

The first R6 build (`b206d66893dd8da48d527e70ab5a38cb2c4a1074cad6be18f280075adfef9dde`)
was not sent to the device: a post-build audit found that its compiled-in
`regulatory.db` was an older 6,292-byte file paired with the current signature.
The database was replaced from the official `wireless-regdb-2026.03.18`
kernel.org archive after its archive hash matched the hash pinned by OpenWrt.
The exact database and signature extracted back out of the rebuilt `vmlinux`
have SHA256 `3d437be973206ca41b7f4e8bb6c3da66f9ef17a760763d974fce7812944f36f3`
and `138cd89205b9612ea3df9eacf2672e5586a08aea986c677d22c5d71ea35774de`;
OpenSSL CMS verification succeeds on that embedded pair. The TX lock remains
enabled in this corrected image.

```text
bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-initramfs-kernel.bin
SHA256 e37b145f45a7e63767a68cb797ab48df2efab322b67ecd775f9ea15283310e18
```

The corrected R6 UART boot passed. U-Boot received exactly 4,278,794 bytes,
validated the uImage CRC, and booted the expected 4,278,730-byte payload. Both
PCIe links reached L0/Gen1 on their first attempt, the endpoints remained
`10ec:f812` and `10ec:818c`, and RTL8192FE again selected the exact OP2200H
profile and firmware without an Oops or panic. Crucially, cfg80211 loaded its
compiled-in certificate and no longer reported a malformed or invalidly signed
regulatory database. `iw reg get` now returns a populated database. The
per-phy `country 99` is rtlwifi's intentional alpha2 for its custom world
domain, not another database failure; the real operating-country hint must
still be set before transmission. `wlan0` remained managed, DOWN and 0.00 dBm
with a new temporary Realtek fallback MAC. CPU1 still failed to start, unchanged
from the earlier images. Because this capture never attempted to open wlan0,
the next no-transmit gate is to prove that an interface-open request is refused
while `op2200h_allow_tx` remains false.

That fail-closed gate passed. Sysfs reported `op2200h_allow_tx=N`; an
`ip link set dev wlan0 up` request produced the driver's `OP2200H TX LOCKED`
message, and the final link state contained no `UP` flag and remained
`state DOWN`. The shell later printed `up_rc=0`, but the UART input around the
request was interleaved and that value is not the result criterion: the driver
message plus final kernel link state prove that hardware initialization was
refused. No explicit unlock has yet been performed.

The pre-unlock runtime preparation also passed. The signed database accepted
the `IN` user hint and exposed India's rules globally while rtlwifi retained
its expected custom `99` per-phy ceiling. The 2.4 GHz endpoint was assigned
this unit's stock `wlan1` address (`08:63:32:61:11:ae`) at runtime; the address
is deliberately not compiled into the image. No hostapd or wpa_supplicant
process was present, and wlan0 remained down. The next experiment may therefore
open the managed interface briefly to exercise hardware/firmware initialization,
but must not request a scan, association or AP mode.

The first controlled open exposed an IRQ-domain bug and was stopped. With the
runtime lock explicitly set to `Y`, RTL8192FE completed RF/PHY initialization:
the readback showed 2T2R, RFE type 3, XCAP `0x10`, both RF paths on channel 7,
and populated IQK results. As soon as the endpoint asserted PCIe1 INTx, however,
Linux reported that IRQ 57's flow handler was `handle_bad_irq` even though the
`_rtl_pci_interrupt` action was attached, then entered a continuous
`unexpected IRQ #57` storm. No scan, association, AP or data-frame test was
reached.

The cause is in the PCI host's arch hook, not the WiFi handler. RTL9607C uses
the MIPS GIC's three-cell specifier `<GIC_SHARED input flags>`; shared input N
maps to the GIC domain hwirq `GIC_NUM_LOCAL_INTRS + N`. The hook instead passed
the table's raw input 57 to `irq_create_mapping()`, bypassing the GIC xlate
step. The next image uses `irq_create_of_mapping()` with the controller's
declared `#interrupt-cells`: the older Luna one-cell INTCs retain their native
mapping, while RTL9607C inputs 56/57 take the GIC shared/level-high path. The
radio must remain locked until a new UART boot proves the resulting descriptor
uses the GIC level handler before any interface-open attempt.

R7 contains that IRQ-domain correction and retains the default-off OP2200H TX
lock. The linked `vmlinux` contains `irq_create_of_mapping()`, both new INTx
mapping diagnostics, and the locked/unlocked guard strings. The regulatory
database/signature extracted from this rebuilt kernel still match the verified
R6 pair and pass OpenSSL CMS verification. It is a legacy MIPS/LZMA uImage with
load/entry `0x80000000`, total size 4,279,879 bytes and payload 4,279,815 bytes.

```text
bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-initramfs-kernel.bin
SHA256 f5e6e8854f443c7dfff38954d44ba78db3c374fa363bc00b24c77d608366fd45
```

The locked R7 UART boot confirms the corrected mapping before radio start.
PCIe1 shared input 57 now maps to Linux IRQ 15, whose `/proc/interrupts` entry
is `MIPS GIC 64 rtl_pci`. Hardware IRQ 64 is correct: this kernel's MIPS GIC
has seven local interrupt slots, so shared input 57 is domain hwirq `7 + 57`.
The descriptor is therefore owned by the GIC level controller instead of
`handle_bad_irq`; its action is the rtlwifi PCI handler and its count remains
zero while the locked interface is down. Linux IRQ 15 is a dynamically
allocated virtual number and need not match stock's display number 73.

The first controlled R7 interface-open test then passed the point that stopped
R6. With the India regulatory domain, the unit's factory MAC and the RAM-only
TX gate set explicitly, `ip link set dev wlan0 up` returned normally. The 2.4
GHz LED illuminated, RF/PHY initialization reported 2T2R, RFE type 3, XCAP
`0x10`, both RF paths on channel 7 and populated IQK results; there was no IRQ
storm. No scan, association, AP or data-frame test was attempted.

`ip link set dev wlan0 down` turned the 2.4 GHz LED off but did not return to
the shell. That places the stall late in shutdown, after the RTL8192F power-off
sequence. The remaining rtlwifi stop path tries to restore PCIe ASPM after
powering the endpoint down. RTL9607C provides fixed configuration-space MMIO
windows and does not yet have a proven safe abort path for a transaction to a
powered-down endpoint, making that access the leading cause. R8 disables ASPM
only for the `ovt,op2200h` machine and adds begin/complete markers around the
card-disable sequence. R7 must not be unlocked again; repeat the controlled
open/close test only with R8 after its locked boot is verified.

R8 builds successfully as a legacy MIPS/LZMA uImage with load/entry
`0x80000000`, total size 4,278,946 bytes and payload 4,278,882 bytes. Its linked
kernel contains the OP2200H ASPM-disable and both card-disable diagnostics, the
corrected INTx mapping diagnostic and the default-off TX guard. The embedded
regulatory database/signature retain the previously verified hashes and pass
OpenSSL CMS verification. The canonical test filename now refers to R8; R7 is
preserved separately for diagnosis and must not be radio-unlocked again.

```text
bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-initramfs-kernel.bin
SHA256 d46edd37d2a3701f2c721a7f20fd8c7a376e2d1d88a63c5519acb3f786815672

bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-r7-initramfs-kernel.bin
SHA256 f5e6e8854f443c7dfff38954d44ba78db3c374fa363bc00b24c77d608366fd45
```

The locked R8 UART boot passed on 2026-08-30. Both endpoints again reached
L0/Gen1; PCIe1 input 57 mapped to Linux IRQ 15 / MIPS GIC hwirq 64, with a zero
counter while wlan0 remained down. The driver selected the exact OP2200H board
profile and logged the new board-specific ASPM disable before loading
`rtl8192fefw.bin`. Sysfs reported `op2200h_allow_tx=N`; wlan0 retained its
temporary generic Realtek address and no `UP` flag. `gpon-wan-recover` remained
disabled as intended.

The controlled R8 open then matched R7: after country `IN`, the unit MAC and
an explicit TX unlock, `ip link set dev wlan0 up` returned. RF/PHY reported
2T2R, RFE type 3, XCAP `0x10`, both paths on channel 7 (`RF_*[0x18]=0x03c07`)
and populated IQK. IRQ 15 stayed `MIPS GIC 64 rtl_pci` at count 0. `wlan0` was
administratively `UP` with `NO-CARRIER` / operstate `DOWN`, as expected with no
scan or association. No scan, AP or data traffic was attempted.

`ip link set dev wlan0 down` hung. UART printed `OP2200H card disable begin`
(console-duplicated) and then stopped mid-line at `[ 1075.`; `complete` never
appeared and the 2.4 GHz LED stayed on. R8 therefore did not reach
`rtl_pci_enable_aspm()`. The stall is inside `rtl92fe_card_disable()` /
`_rtl92fe_poweroff_adapter()`, and the still-on LED is consistent with never
finishing the NIC disable flow that deasserts the LED pad (R7's LED-off was
likely that hardware side-effect, not a completed `rtl_pci_stop()`). Do not
unlock R8 again.

R9 keeps the default-off TX gate and ASPM disable, and adds staged
`OP2200H card disable <step>` markers with a 50 ms UART drain after each
print so a hung BAR access cannot swallow the last completed step. The first
hardware access after `begin` is `_rtl92fe_set_media_status()` (`MSR` read).
The NIC disable flow later requests MAC-off and polls `0x05` bit 1; a poll
timeout cannot save a `rtl_read_byte()` that never returns on this host.

R9 built on 2026-08-30 as a legacy MIPS/LZMA uImage with load/entry
`0x80000000`, total size 4,280,789 bytes and payload 4,280,725 bytes. The
linked kernel contains the staged card-disable format string, every step
name, the TX lock/unlock guards and the ASPM-disable message. R8 is preserved
separately and must not be radio-unlocked again.

The controlled R9 up/down test passed every marker through
`before nic disable flow`, then hung (~50 ms later, truncated `[  127.6`).
`complete` never appeared. That isolates the stall to
`rtl_hal_pwrseqcmdparsing(RTL8192F_NIC_DISABLE_FLOW)`. That table begins with
`ACT_TO_CARDEMU`, which writes `0x05` bit 1 (MAC off by hardware state
machine) and polls the same bit. A `rtl_read_byte()` that never returns
cannot be bounded by the 5000-iteration poll cap. R7's LED-off then hang is
the same point: the MAC/RF pad dropped, then the next BAR access froze the
CPU. Do not unlock R9 again.

R10 skips `RTL8192F_NIC_DISABLE_FLOW` only on `ovt,op2200h`. LPS enter, RF
off and MCU reset already completed on R9; CARDDIS/PDN is not required for a
RAM-boot interface down. The TX gate and ASPM disable remain. Success is
`skipping NIC_DISABLE_FLOW`, then `before rsv ctrl`, `poweroff adapter done`,
`complete`, and a returned shell. If it hangs on `before rsv ctrl`, the BAR
is already dead before that flow.

R10 built on 2026-08-30 as a legacy MIPS/LZMA uImage with load/entry
`0x80000000`, total size 4,279,768 bytes and payload 4,279,704 bytes.

```text
bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-initramfs-kernel.bin
SHA256 7e7431b217d4a53e304f67963429008ad8fe7622e4a9bbae9addfe0ffe77319b

bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-r9-initramfs-kernel.bin
SHA256 5212e05234691f0fb5cf9517bf277eb019fcbd2d9af2565c2a9e20af2f6ad82f
```

The controlled R10 up/down test passed on 2026-08-30. Every marker through
`skipping NIC_DISABLE_FLOW`, `before rsv ctrl`, `poweroff adapter done` and
`complete` printed, `ip link set dev wlan0 down` returned to the shell, and
the 2.4 GHz LED turned off. Software LED-off was still skipped
(`unload=0 rfoff=0`); the LED going off is the RF/LPS side-effect, matching
the R7 observation. This is the first controlled hardware shutdown that did
not hang. It is still not a scan, association, AP or data-path pass. The
next check on this same RAM boot is whether a second `wlan0 up`/`down` still
returns after the skipped CARDDIS, then relock.

```text
bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-initramfs-kernel.bin
SHA256 5212e05234691f0fb5cf9517bf277eb019fcbd2d9af2565c2a9e20af2f6ad82f

bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-r8-initramfs-kernel.bin
SHA256 d46edd37d2a3701f2c721a7f20fd8c7a376e2d1d88a63c5519acb3f786815672

bin/targets/realtek-luna/rtl9607x/openwrt-realtek-luna-rtl9607x-ovt_op2200h_pcie_test-r7-initramfs-kernel.bin
SHA256 f5e6e8854f443c7dfff38954d44ba78db3c374fa363bc00b24c77d608366fd45
```

After a UART-observed RAM boot, collect these before trying to load a WiFi
driver:

```sh
dmesg | grep -Ei 'pci|rtl9607'
cat /proc/iomem
ls -l /sys/bus/pci/devices
for d in /sys/bus/pci/devices/*; do
    echo "$d"
    cat "$d/vendor" "$d/device" "$d/irq" "$d/resource"
done
```

Expected endpoint IDs are `10ec:f812` and `10ec:818c`. Hardware interrupt inputs
are GIC56 and GIC57; Linux virtual IRQ numbers are kernel-domain allocations and
need not equal stock's 72/73. Stop after capturing UART if either link retries
continuously, a resource conflict is reported, the kernel faults, or the board
resets. Do not run a flash-write command as part of this test.

## Read-only measurements required next

Before enabling Ethernet:

- complete stock boot log around switch/PHY initialization;
- stock `/proc/rtl865x/` or equivalent switch/port status, if present;
- `dmesg`, `ip link`, `ethtool`, and available Realtek diagnostic procfs output;
- physical LAN-jack-to-switch-port mapping, derived one cable/link transition at
  a time.

Before testing PCIe:

- enable `realtek,enable-pcie` only in a RAM-only test DT/image and verify both
  endpoint IDs, memory resources and GIC mappings from UART;
- keep persistent image generation disabled until both links have survived the
  RAM-only test without resets, hangs or resource conflicts.

Before enabling either radio:

- per-radio MAC addresses;
- efuse contents or positive confirmation that efuse is blank;
- board-specific power, crystal, thermal, regulatory, and RF-front-end data
  from `static_conf`/the stock MIB, read only.

Before GPON work:

- GPON serial/vendor ID, PLOAM password, LOID/password if used;
- ONU model and OMCI identifiers;
- laser driver identity and stock calibration;
- stock PON/OMCI configuration and MAC addresses.

Do not publish identity-bearing values in commits or logs.
