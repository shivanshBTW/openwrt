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
| PCIe | Alpha `pcie-rtl960x.c`; Realtek GPL source | Do not enable yet. Alpha's current host driver selects RTL9602C and RTL9603CVD only and implements one downstream endpoint. Jameywine's current `rtl9607c-dev` has no PCIe host driver. OP2200H needs both ports and GPIO40/GPIO39 resets. |
| 2.4 GHz | Alpha clean-room `rtl8192fe` | Reuse after RTL9607C PCIe works. Do not copy X111W calibration; extract OP2200H calibration/MAC data first. |
| 5 GHz | Jameywine research; Realtek GPL `rtl8192cd` | No current fetched branch contains an `10ec:f812` driver. Locate the experimental rtw88 work or implement it from a reviewable source before enabling the radio. |
| GPON/OMCI | Alpha shared GPON core plus `rtl9607c_gpon.c` | Defer. Preserve the unit's ONU identity and laser/optics configuration before connecting experimental firmware to the PON. |
| Persistent install | Neither tree | Out of scope. No sysupgrade or UBI image is generated. |

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

The target metadata and `defconfig` were validated on 2026-08-29. The complete
image was not built in the original workspace because its path contains a
space, the host filesystem is case-insensitive, and the host had only about
4.1 GiB free. Reserve substantially more space before starting a toolchain and
kernel build.

## Read-only measurements required next

Before enabling Ethernet:

- complete stock boot log around switch/PHY initialization;
- stock `/proc/rtl865x/` or equivalent switch/port status, if present;
- `dmesg`, `ip link`, `ethtool`, and available Realtek diagnostic procfs output;
- physical LAN-jack-to-switch-port mapping, derived one cable/link transition at
  a time.

Before implementing PCIe:

- stock boot log covering both PCIe controllers and their IRQ mappings;
- read-only PCI enumeration for both `10ec:f812` and `10ec:818c`;
- confirmation that port 0 reset is GPIO40 and port 1 reset is GPIO39 on this
  firmware/board revision;
- the per-port host/config windows, interrupt inputs, SerDes tables, and clock
  gates from the matching RTL9607C GPL code or measured stock state.

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
