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
