# Read-only stock kernel extraction

The active slot-0 kernel is exposed by the stock system as MTD volume `mtd6`,
named `ubi_k0`, with an exact size of 10,539,008 bytes. Reading this volume does
not modify NAND. Do not substitute `mtd5`, which is the writable configuration
volume and can contain unit-specific provisioning.

The OP2200H stock image has neither a usable `nc` command nor a `dd` applet.
Its `inetd` does expose `/bin/tftpd`, and `/var` is RAM-backed. The successful
read-only procedure was therefore:

```sh
cp /dev/mtd6ro /var/ubi_k0.bin
ls -l /var/ubi_k0.bin
md5sum /var/ubi_k0.bin
```

The expected size and MD5 are:

```text
10539008 bytes
c165b2ca1b8112fee09c9feb89de6ece
```

This old TFTP daemon stalls on a single transfer near 10 MiB and can leave its
`inetd` `wait` child alive. Split the verified RAM copy into two smaller files:

```sh
head -c 5000000 /var/ubi_k0.bin > /var/k0a
tail -c 5539008 /var/ubi_k0.bin > /var/k0b
ls -l /var/k0a /var/k0b
```

Retrieve one part per fresh TFTP child. If a request receives no reply, exit the
Mac client, run `killall tftpd` on UART (leaving `inetd` running), then start a
new client. Do not request RFC 2347 block-size negotiation from this daemon.

```text
tftp 192.168.1.1
tftp> binary
tftp> get /var/k0a k0a
tftp> quit
```

Repeat for `/var/k0b`, then concatenate the two local parts and verify the full
size and MD5 above. Keep all extracted binaries below ignored `captures/`.

## Verified image metadata

The reconstructed slot-0 volume has SHA-256
`e792b9209f2bce61e90b788bbeeeea996336c6724368ddbc201327a03ff84de2`.
It begins with a legacy U-Boot image:

- name: `Linux-4.4.140`;
- architecture: MIPS, kernel, LZMA;
- load address: `0x80010000`;
- entry point: `0x808f1230`;
- compressed payload size: 4,035,345 bytes;
- payload CRC32: `9e709fce`.

The declared payload CRC matches exactly. The vendor LZMA stream expands to a
raw 18,694,208-byte kernel memory image but ends without a standards-compliant
LZMA marker, so `xz` reports a terminal error after producing the usable data.
The expanded image contains the expected Linux build string, kallsyms names and
code at the live addresses reported by `/proc/kallsyms`.

None of these commands writes NAND: `/dev/mtd6ro` is read-only and `/var` is
RAM-backed. Do not substitute `mtd5`, redirect into `/dev/mtd*`, or use the web
interface for extraction.
