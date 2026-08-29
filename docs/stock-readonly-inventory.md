# Stock slot-0 read-only inventory

Run this only after the untouched stock system has finished booting and a
shell has been obtained on `ttyS0`. Every command below is read-only. Keep the
terminal capture private: interface addresses and provisioning details may be
unit-specific.

```sh
echo '=== IDENTITY ==='
uname -a
cat /proc/cmdline
cat /proc/cpuinfo

echo '=== FLASH AND MOUNTS ==='
cat /proc/mtd
cat /proc/mounts

echo '=== INTERRUPTS AND MMIO ==='
cat /proc/interrupts
cat /proc/iomem

echo '=== LOADED DRIVERS ==='
cat /proc/modules

echo '=== PCI SYSFS ==='
ls -la /sys/bus/pci/devices
for d in /sys/bus/pci/devices/*; do
    [ -d "$d" ] || continue
    echo "--- $d ---"
    for f in vendor device subsystem_vendor subsystem_device class irq resource; do
        [ -r "$d/$f" ] && { echo "$f"; cat "$d/$f"; }
    done
    readlink "$d/driver"
    hexdump -C "$d/config" | head -n 16
done
if command -v lspci >/dev/null 2>&1; then
    lspci -nn -vv
fi

echo '=== NETWORK DEVICES ==='
ifconfig -a
cat /proc/net/dev
if command -v iwconfig >/dev/null 2>&1; then
    iwconfig
fi
for n in /sys/class/net/*; do
    echo "--- $n ---"
    cat "$n/ifindex"
    cat "$n/address"
    readlink "$n/device"
done

echo '=== REALTEK PROCFS INVENTORY ==='
find /proc -maxdepth 2 -type d | grep -Ei 'rtl|eth|switch|phy|wlan|pci'

echo '=== LIVE DEVICE TREE INVENTORY ==='
ls -la /proc/device-tree
find /proc/device-tree -maxdepth 4 -type f | grep -Ei 'pci|gpio|wlan|wifi|eth|switch|phy|board'
find /proc/device-tree/board_setting -maxdepth 3 -type f 2>/dev/null

echo '=== KERNEL LOG ==='
if command -v dmesg >/dev/null 2>&1; then
    dmesg
fi

echo '=== END OF READ-ONLY INVENTORY ==='
```

Do not run `flash set`, `mtd`, `nandwrite`, `ubiformat`, `saveenv`, or any
command that redirects output into `/proc`, `/sys`, `/dev/mtd*`, or a block
device.

## Follow-up for this stock image

The first inventory proved that this firmware has no PCI sysfs and lacks both
`sort` and `dmesg`. Run this shorter block to capture the live device-tree
values and any unstripped PCIe symbols in the stock kernel:

```sh
echo '=== BOARD SETTING FILES ==='
find /proc/device-tree/board_setting -maxdepth 3 -type f 2>/dev/null
for f in /proc/device-tree/board_setting/*; do
    [ -f "$f" ] || continue
    echo "--- $f ---"
    hexdump -C "$f"
done

echo '=== PCIE-RELATED DEVICE TREE FILES ==='
find /proc/device-tree -maxdepth 5 -type f | grep -Ei 'pci|gpio|wlan|wifi|board'
for f in $(find /proc/device-tree -maxdepth 5 -type f | grep -Ei 'pci|wlan|wifi|board'); do
    echo "--- $f ---"
    hexdump -C "$f"
done

echo '=== PCIE-RELATED KERNEL SYMBOLS ==='
grep -Ei 'pcie|pci0|pci1|rtl8192cd|wlan_device' /proc/kallsyms

echo '=== WLAN PROCFS FILES ==='
find /proc/wlan0 -maxdepth 2 -type f
find /proc/wlan1 -maxdepth 2 -type f
ls -la /proc/wlan0
ls -la /proc/wlan1

echo '=== KERNEL CONFIG PCI/WLAN ==='
if [ -r /proc/config.gz ] && command -v zcat >/dev/null 2>&1; then
    zcat /proc/config.gz | grep -Ei 'CONFIG_(PCI|PCIE|RTL.*(8192|8812)|WLAN)'
fi

echo '=== END FOLLOW-UP ==='
```
