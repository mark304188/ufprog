# Universal Flash Programmer

This project aims at developing a flash programmer software supports various of flash chips through various controllers on various platforms.

Currently this project is at an early developing stage.

Pull requests are welcome.

## Features
* Controller plugin support. Easy to add new controller support.
* Controller configuration. Supports configuration-specific features (multiple USB interface/drive strength/chip select/...).
* Abstraction Layer: Controller <- -> Interface <- -> Flash device. Easy to add multiple interface support.
* External Flash ID list support. Easy to add new flash without recompiling the project.
* Cross-platform. Supports Windows and Linux.

## SPI-NOR Features
* SFDP parsing for new flashes.
* Supports OTP regions.
* Supports non-volatile register modification.
* Supports write-protection modification.

## Generic NAND Features
* Supports ECC plugin to replace NAND's on-die ECC
* Supports Bad Block Table (BBT) plugin
* Supports simple Flash Translation Layer (FTL) plugin
* Supports OTP regions.
* Supports reading unique ID.

## SPI-NAND Features
* ONFI parameter page parsing for new flashes.

## Supported controllers
* WCH CH341 (100KB/s read)
* WCH CH347 (4.9MB/s read)
* WCH CH552 USB-SPI bridge (≈0.4MB/s read, board-side flash accelerators)
* FTDI MPSSE chips (FT232H/FT2232H/FT4232H) (3MB/s read)
* FTDI FT4222H (6MB/s read using QSPI)
* Programmer using serprog protocol (860KB/s read using USB FS)

## Supported interfaces
* SPI (Single/Dual/Quad/QPI using SPI-MEM)
* TBD: I2C/Parallel NAND

## Supported flash types
* SPI-NOR
* SPI-NAND
* TBD: Parallel NAND/24 EEPROM

## Supported SPI-NOR flashes
* Atmel/Adesto/Dialog/Renesas
* EON
* ESMT
* GigaDevice
* Intel
* ISSI
* Macronix
* Microchip/SST
* Micron
* Spansion/Cypress/Infineon
* Winbond
* XMC
* XTX
* TBD: FudanMicro/Puya/Zbit/Zetta...

## Supported SPI-NAND flashes
* Alliance Memory
* ATO Solution
* CoreStorage
* Dosilicon
* ESMT
* Etron
* Fidelix
* FORESEE
* Fudan Microelectronics
* GigaDevice
* HeYangTek
* ISSI
* Macronix
* Micron
* MK Founder
* Paragon
* Toshiba/Kioxia
* Winbond
* XTX
* Zetta

## Build Prerequisites
* libusb
* hidapi-libusb
* json-c

## CH552 USB-SPI bridge notes

The CH552 controller (`controller/wch/ch552-*`) drives the small CH552
USB-SPI bridge documented in `docs/protocol.md` of the parent project:
a vendor specific interface, two 64 byte bulk endpoints (0x03 OUT /
0x82 IN), and a command protocol on top of them.

It is a full-speed (12 Mbit/s) device, so the USB bus, not the SPI
clock, is the limit.  The bridge firmware compensates by running whole
flash operations itself, and this driver uses them whenever the
operation matches what the device implements:

| Flash operation | Device command | Saves |
|---|---|---|
| SPI-NOR page program | `CMD_NOR_PROGRAM` | WREN, PP and the WIP poll per page |
| SPI-NOR 4 KiB erase | `CMD_NOR_ERASE` | WREN, SE and the WIP poll per sector |
| SPI-NAND page read | `CMD_NAND_READ_PAGES` | PAGE READ plus the OIP poll per page |
| SPI-NAND page program | `CMD_NAND_PROGRAM_PAGES` | WREN, LOAD, EXECUTE and the OIP poll |

The firmware advertises these through `CMD_GET_CAPS`; when a build does
not, or the operation is outside the supported subset (dual/quad I/O,
4-byte addressing, a different page or erase opcode, multi-plane NAND),
the driver transparently falls back to the generic SPI-MEM path.

Device configuration (`share/ufprog/device/ch552-libusb.json`):

```json
{
	"driver": "ch552-libusb",
	"if_type": [ "spi" ],
	"config": {
		"match": [ { } ],
		"spi": { "sysclk-hz": 24000000 }
	}
}
```

`sysclk-hz` is the CH552 system clock the firmware was built for
(24 MHz by default, 12 MHz for a 3.3 V-only board).  The SPI divider is
relative to it, so the reported SCK frequency follows from it.

Usage:

```bash
ufsnorprog dev=ch552-libusb probe
ufsnorprog dev=ch552-libusb read dump.bin 0 0x100000
ufsnorprog dev=ch552-libusb write firmware.bin 0
ufsnorprog dev=ch552-libusb erase 0 0x10000

ufsnandprog dev=ch552-libusb probe
ufsnandprog dev=ch552-libusb write raw oob image.bin 0
```

On Linux the interface is claimed through usbfs; install the udev rule
from the parent project (`host/99-ch552-spi.rules`) or run as root.  On
Windows, bind interface 0 to WinUSB with Zadig first.

## Supported compilers
* MSVC (Windows only. Suggested to use with vcpkg)
* GCC (Windows (mingw) and Linux)

## Install

### Linux

```bash
git clone https://github.com/hackpascal/ufprog.git

cd ufprog

cmake -DCMAKE_BUILD_TYPE=None \
    -DBUILD_PORTABLE=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -B build \
    -G Ninja

ninja -C build

ninja -C build install
```

### Arch Linux

Installation can be done via [AUR ufprog-git](https://aur.archlinux.org/pkgbase/ufprog-git).

```bash
yay -Syu ufprog
```

## License
* For executable programs, GPL-2.0-only
* For libraries, LGPL-2.1-only
