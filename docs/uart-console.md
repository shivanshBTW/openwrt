# OP2200H UART console

Imported from Codex task `01a04a03-bf3a-7eb3-89cd-d38ae76b6066` on
2026-08-29.

## Proven bridge

An ESP32 WROOM-32 is flashed as a bidirectional 115200 8N1 USB/UART bridge.
The final source is `/Users/shivanshtyagi/Codebase/esp32-uart/esp32-uart.ino`.

Final wiring:

- router TX -> ESP32 GPIO23;
- router RX <- ESP32 GPIO22;
- router GND -> ESP32 GND;
- router VDD remains disconnected;
- router and ESP32 use their own power sources.

The macOS serial device is `/dev/cu.usbserial-0001`. A stale detached `screen`
session previously held this device; exit future sessions cleanly with
`Ctrl-A`, then `\\`, then `y`.

## Capture a complete stock boot

From the repository root:

```sh
cd captures
screen -L /dev/cu.usbserial-0001 115200
```

Only after logging starts, power-cycle the router. For the first capture, do
not press a key during U-Boot: allow untouched slot 0 to boot normally. After
the stock system settles, exit `screen` cleanly. The log will be
`captures/screenlog.0`; rename it locally before the next capture.

Do not commit captures. They may contain MAC addresses, GPON serials,
credentials, or other unit-specific provisioning data.

## Safety boundary

Typing through this bridge reaches the router directly. Until a reviewed step
requires otherwise, do not run `saveenv`, `erase`, `nand write`, `ubi write`,
`upk`, `upr`, `updev`, `upt`, or `upv`.
