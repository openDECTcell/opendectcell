# FRITZ!Box 7362 SL: DECT analysis

Source: `FRITZ.Box_7362_SL-07.18.image` (HW203, FRITZ!OS 07.18, build 114995). The analysis was done read-only and in memory on 2026-10-15.

These notes record **interface facts only**, in our own words. They contain no AVM code and no AVM device-tree source. They are the clean-room input for a new DECT stack. AVM firmware blobs are referenced by hash only.

The DECT design is the same as on the FRITZ!Box 7520 (see `7520-DECT-analysis.md`). This document gives the 7362 SL specifics and the differences.

## 1. Image layout and extraction (unusual)

| Item | Value |
|---|---|
| Container | GNU tar. `var/tmp/kernel.image` (tar offset 25048576), `var/tmp/filesystem.image` (tar offset 619008, 24.4 MB), `var/install`, `var/chksum`, `var/regelex`, `var/signature` |
| Kernel | AVM EVA format: magic `0xfeed1281`, load address **0x80002000**, LZMA stream at +36. Linux 3.10.107, MIPS 34K, 32-bit **big-endian** SMP |
| Device tree | One small DTB inside the kernel that holds only the hardware revision (203) and the board's GPIO table |
| Root filesystem | AVM **big-endian SquashFS 4.0**, xz, 64 KiB blocks, 4248 inodes. Superblock, inodes, directories and the fragment table are big-endian, but the **metadata block headers are little-endian**. Standard `unsquashfs` refuses it |

The `filesystem.image` is **not a plain SquashFS**:
- It starts with a 0x500-byte area that is zero except for the `sqsh` magic.
- It is interleaved with **1 KiB sector-index headers**. Each header holds 256 consecutive little-endian u32 sector numbers. A header with index *n* sits at file offset *n*·1024 − 0x300. There are 81 headers. They usually follow each other every 257 sectors, with larger gaps in between.
- After the headers are removed, the real superblock is found in the middle of the image, at offset 0x7bf500 of the linear data. The data and metadata regions don't share a single base offset.
- Files were therefore recovered by:
  1. reading directories and inodes, with the metadata region at linear offset 0x9100
  2. locating each file's data blocks by matching its block-size chain against the self-delimiting xz streams
  3. resolving fragments by decompressing and checking the content

A small in-memory reader was used for this. A future `dect-fw-extract` tool has to implement the same logic, or read the firmware from the running box instead.

## 2. DECT-related components

| Component | Path | Notes |
|---|---|---|
| First-level loader | `/lib/modules/dectfw_firstlevel_441.hex` | Intel HEX, **612 bytes at 0x10080** |
| STUB firmware | `/lib/modules/dectfw_secondlevel_441.hex` | About **48 KB**. Regions 0x0–0x3cfd, 0x9000–0x9ffd, 0x1138a–0x17d45, 0x1030000–0x10307f7. Contains the build-date string "Feb 25 2019" |
| Production-test STUB | `/lib/modules/dectfw_secondlevel_441p2test.hex` | Selected when `ptest` is set |
| Firmware loader | a kernel module of the vendor (92 KB) | Same loader state machine as on the 7520 (first/second level, RFPI patch, mode patch). It uses the VR9 **USIF UART** and the console stop/start hooks |
| PCM driver | a kernel module of the vendor | VR9 TDM link with an alternative **SPI-TDM** link. Realtime work runs as MIPS 34K "yield" handlers. Includes DECT hum cancellers |
| DECT host stack | a closed-source kernel module of the vendor | The same code base as on the 7520 (451 shared function names); the 7362 SL build additionally has a DSP-to-host mail routine |
| Audio and call control | the vendor's audio and VoIP drivers | As on the 7520 |
| EEPROM defaults | a factory EEPROM image in the firmware (2551 B) | Same size as on the 7520 |
| Init | the vendor's init script | Loader mode value 0x101 (the 7520 uses 0x105). Firmware paths are under `/lib/modules/` |

Firmware hashes (SHA-256):

| File | SHA-256 |
|---|---|
| `dectfw_firstlevel_441.hex` | `72f8e27a0d01c301a05a8b373a71887af882a6cebcb1a30bd4a435580e1045f2` |
| `dectfw_secondlevel_441.hex` | `8aae3b5e3450fd2c9999e003068e786096c11d8ef53f7d47c25cebd057a797d3` |

## 3. Hardware map (from the board's GPIO table in the kernel DTB)

| Function | GPIO | Notes |
|---|---|---|
| DECT reset | 30 | output |
| DECT UART RX | 11 | VR9 USIF |
| DECT UART TX | 12 | VR9 USIF |
| TDM FSC | 0 | alternate function |
| TDM DCL | 40 | alternate function |
| PCM-link FSC | 0 | input |
| PCM-link DCL | 40 | input |
| PCM-link DO | 25 | output |
| PCM-link DI | 41 | input |
| DECT LED | 35 | output |
| DECT button | 1 | input |
| WLAN button | 29 | input |
| LEDs | power 32, power-red 33, internet 38, WLAN 36, info 47, info-red 34 | |
| USB power enable / fault | 14 / 9 | |

- **DECT chip:** SC14441, per the OpenWrt 7362 SL commit and Osmocom's chip list. Confirm on the PCB.
- **DECT UART:** its own UART pins (USIF on GPIO11/12). Unlike the 7520, the DECT UART is **not the board console** pin-wise. The vendor loader still uses the console stop/start hooks, so check on hardware whether the console is paused while firmware loads.
- **PCM direction:** FSC and DCL are inputs on the PCM-link side, and the vendor's PCM-link driver has a "Host-CPU is master" check. Clock mastership (VR9 vs STUB) must be confirmed with a capture.

## 4. Boot protocol and runtime architecture

These are identical to the 7520 (`7520-DECT-analysis.md` §4–§5):
- **Boot:** GPIO reset, then the ROM STX/SOH handshake with a 16-bit length for the first level, then the first level's STX/SOH with a 32-bit length and CRC16 for the STUB. RFPI (from `maca`) and the mode word are patched in before upload. *(Correction 2026-10-17: the check is one XOR byte in both stages, not a CRC16; see `stub-loader-protocol.md`.)*
- **Runtime:** the STUB on the SC14441 handles the radio, MAC and ADPCM/G.722, and talks to the host over the **PCM bus**, using checksummed mails in a control slot plus voice slots.
- **Host stack:** the vendor's DECT host stack implements DLC, NWK, DSAA/DSAA2 and the EEPROM emulation.

The 7362 SL STUB is an older build (2019) with mode 0x101. Its primitive set should be checked against the 7520's for differences.

## 5. Differences from the 7520 that matter for a rewrite

| Topic | 7362 SL | 7520 |
|---|---|---|
| CPU / endianness | MIPS 34K, big-endian, Linux 3.10 | ARMv7, little-endian, Linux 4.4 |
| DECT UART | VR9 USIF, GPIO11/12, separate pins | IPQ4019 UART0, GPIO16/17, **shared with the console** |
| PCM hardware | VR9 TDM (no public documentation) or SPI-TDM | IPQ4019 ADSS TDM (Qualcomm QSDK has a GPL driver) |
| Realtime | MIPS MT "yield" interrupts | FIQ / dedicated CPU path |
| STUB build | 2019-02, mode 0x101 | 2026-01, mode 0x105 |
| Extraction | Fragmented BE SquashFS (custom tool needed) | Plain LE SquashFS |

## 6. What a rewrite needs (7362 SL view)

1. **The 7520 work first.** Same STUB protocol and same host stack, on a simpler and better documented platform.
2. **A VR9 PCM/TDM driver.** This is the main platform-specific gap. Candidate sources of register knowledge:
   - Lantiq/Intel UGW GPL drops (the VMMC/TAPI voice stack drives the same PCM interface through the voice CPU)
   - observation of the vendor's driver's register accesses on a running box
   - ~~the SPI-TDM alternative, which may be simpler (SSC-based)~~ ruled out on this box (§7): the PCM pins cannot reach the SSC, and the SSC carries the SPI NOR flash
   The driver also has to set the pin mux itself: `pinctrl-xway` has no TDM group (§7).
3. **The firmware loader** using the USIF UART and GPIO30 for reset. OpenWrt has **no USIF UART driver** (§7), so this needs a new kernel module.
4. **Firmware extraction** from this image format (see §1), or a copy from a running stock box.

## 7. OpenWrt on the box (2026-11-01)

Source: a FRITZ!Box 7362 SL running **OpenWrt 25.12.6** (Linux 6.12.112), *observed on hardware*; and the public sources of that release, *analysed*: OpenWrt `v25.12.6` `target/linux/lantiq/` (`vr9.dtsi`, `vr9_avm_fritz736x.dtsi`, `vr9_avm_fritz7362sl.dts`), Linux `v6.12.112` `drivers/pinctrl/pinctrl-xway.c` and `drivers/tty/serial/lantiq.c`, and the 25.12.6 `lantiq/xrx200` SDK's kernel `.config` and `modules.builtin`. No AVM material.

| Item | Finding |
|---|---|
| Board name | `avm,fritz7362sl` (`/tmp/sysinfo/board_name`, observed). DT compatible `"avm,fritz7362sl", "avm,fritz736x", "lantiq,xway", "lantiq,vr9"` |
| Flash layout | Observed `/proc/mtd`: `mtd0` kernel and `mtd1` ubi on NAND (128 KiB erase); `mtd2` **urlader** (256 KiB), `mtd3`/`mtd4` tffs on the **SPI NOR** (64 KiB erase). Unlike the 7412, where `mtd0` is the urlader, so the 7412's `factory_mtd=/dev/mtd0` does not carry over |
| SSC | **In use**: the DTS enables `spi@e100800` with a `jedec,spi-nor` flash on chip select 4 holding urlader and tffs. A transport that takes the SSC (`kmod-lantiq-ssc-pcm`) would cut the box off its NOR flash. That module only instantiates itself on `avm,fritz7412`, so it does not touch this box |
| USIF UART | No driver. The kernel has only the ASC serial driver (`lantiq.ko`: `lantiq,asc`, `intel,lgm-asc`). `vr9.dtsi` has a `usif@da00000` node (`lantiq,usif`, IRQs 29 125 107 108 109 110), `status = "disabled"`, which nothing binds; no OpenWrt patch adds one |
| USIF pin mux | Supported: groups `usif uart_rx` = GPIO11 and `usif uart_tx` = GPIO12 |
| PCM pin mux | GPIO0: GPIO/EXIN/SDIO/**TDM**; GPIO25: GPIO/**TDM**/SDIO/USIF; GPIO40 and GPIO41: GPIO/MII/**TDM**. None has an SSC function, so the PCM pins cannot be routed to the SSC |
| TDM in pinctrl | `XWAY_MUX_TDM` appears only in the per-pin table; no SoC defines a `tdm` group or function, so a device tree **cannot request it**. A transport must set the pins' alternate-function registers itself, or rely on a pinctrl patch (own kernel builds only) |
| LEDs, buttons | DTS: green `dect` LED on GPIO35 (active low), DECT button GPIO1 (`KEY_PHONE`), WLAN button GPIO29; matches §3. Also a green `fon` LED on GPIO38 |
| Free pins | Nothing in the DTS claims GPIO30 (DECT reset), 11/12, 0, 25, 40 or 41 |
| Serial console | `console=ttyLTQ0,115200` on the ASC; the DECT UART is separate, so the console stays usable |

**Factory DECT block** (*observed on hardware*, one box, 2026-11-01): in the urlader (`mtd2`) at offset **0x1985**, with the same header as on the 7520 and 7412: `02 01`, BE16 length 17 (`00 11`), then tag 1 length 5 (the RFPI, not the placeholder). Found by searching the partition for that header; 0x1E800, the 7412's offset, is erased (`0xFF`) here. The offset is not aligned and was seen on one box only, so a second 7362 SL should confirm it before a profile relies on it. `dect-fwload --factory-mtd /dev/mtd2 --factory-offset 0x1985` reads it as it stands (`read_factory()` takes any offset). The RFPI's value is not recorded here.

## 7a. Gate G1 (2026-11-01)

*Observed on hardware* ([bench-7362sl.md](../journals/bench-7362sl.md)): the 07.18 STUB
boots over the USIF UART (`kmod-lantiq-usif-uart`, `/dev/ttyUSIF0`) with mode
0x101, stage 1 at 9600 Bd, stage 2 at 230400 Bd. The ROM answers **only with
the boot strap**: the chip's TX line (GPIO 11) held low while reset is
released, as on the 7520. Afterwards the chip drives DCL (GPIO 40) and FSC
(GPIO 0) and sends on DI (GPIO 41): it is the PCM clock master.

## 8. Open questions

- ~~Who is PCM clock master?~~ The STUB (§7a). Open: the frame and slot layout, and DCL's rate. (Mode 0x101 gave 4 words of 32 bit per frame, 1.024 MHz, on the 7412's 06.88 STUB, `7412-spi-tdm-and-loader.md` §1a; *assumed* to be the same here until measured.)
- ~~Is the VR9 USIF UART driver in OpenWrt usable at the baud rates the vendor loader uses (up to 460800)?~~ There is none (§7). Open instead: the USIF's register layout and baud-rate generation, for a new driver.
- The VR9 TDM block: register base, setup sequence, DMA channels, and the pin alternate-function settings for GPIO0/25/40/41.
- Is the factory block at urlader 0x1985 on every 7362 SL (§7)? Seen on one box.
- Does the 2019 STUB differ in its mail or primitive set from the 2026 STUB on the 7520?

## Legal note

Observation (EU Directive 2009/24/EC Art. 5(3)) and analysis for interoperability (Art. 6; §69d/§69e UrhG) allow this analysis for building an independent, interoperable implementation. AVM binaries and firmware blobs are not redistributed. New code must be written from these notes, not from analysed code.
