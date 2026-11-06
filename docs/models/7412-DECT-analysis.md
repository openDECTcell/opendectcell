# FRITZ!Box 7412: DECT analysis

Source: `FRITZ.Box_7412.137.06.88.image` (HW209, FRITZ!OS 06.88; kernel built 2021-10). The analysis was done read-only on 2026-10-16.

These notes record **interface facts only**, in our own words. They contain no AVM code and no AVM device-tree source. AVM firmware blobs are referenced by hash only. The DECT design is the same as on the 7362 SL and 7520 (see `7362SL-DECT-analysis.md` and `7520-DECT-analysis.md`). This document gives the 7412 specifics.

## 1. Image layout and extraction

| Item | Value |
|---|---|
| Container | GNU tar: `var/tmp/kernel.image` (tar offset 315392), `var/tmp/filesystem.image` (tar offset 2822144, 17.9 MB), `var/install`, `var/chksum`, `var/regelex`, `var/signature` |
| Kernel | AVM EVA-LZMA (magic `0xfeed1281`, LZMA at +36). Linux **3.10.73**, MIPS 34K, big-endian, SMP with **MT_SMTC** |
| Device tree | One small DTB inside the kernel with the hardware revision (209) and the board's GPIO table |
| Root filesystem | Same AVM NAND format as the 7362 SL: big-endian SquashFS 4, LE metadata headers, xz, interleaved 1 KiB sector-index headers. Additionally: **short, zero-padded index headers**, **non-data gaps** (0x00/0xFF filler, up to ~50 KB) inside file data, and a **circular layout** (data near the end of the image continues near its start). 3399 inodes |

Extraction tool: `tools/avm_nand_sqfs.py`. It handles all of the above by:
- detecting the index headers
- choosing the superblock by plausibility
- validating the metadata base by walking the directory tree (the base can be negative)
- locating file blocks by size chain
- repairing interrupted xz blocks by searching for the resume point, with wrap-around, accepted only if the xz CRC verifies

All DECT-relevant files extract, except the factory DECT EEPROM default, whose fragment block has not been recovered yet.

## 2. DECT-related components

| Component | Path | Notes |
|---|---|---|
| First-level loader | `/lib/modules/dectfw_firstlevel_441.hex` | **Identical to the 7362 SL's** (same SHA-256) |
| STUB firmware | `/lib/modules/dectfw_secondlevel_441.hex` (126 KB HEX, about 44 KB binary) | Regions 0x0–0x3cff, 0x9000–0x9ffd, 0x1138a–0x17057, 0x1030000–0x10307f7. Build-date string **"Oct 29 2016"**, the oldest of the three boxes |
| Firmware loader | a kernel module of the vendor | Same loader state machine (first and second level, RFPI and mode patch) |
| PCM driver | a kernel module of the vendor | Contains both the VR9 TDM link and the **SPI-TDM** link, uses hardware timers and MIPS "yield" realtime handling. Almost all of it is shared with the 7362 SL |
| DECT host stack | a closed-source kernel module of the vendor | Almost all of it (450 of 456 functions) is shared with the 7362 SL and most of it with the 7520, so findings transfer |
| Userspace | the vendor's DECT application layer | As on the other boxes |
| Init | the vendor's init script | Loader mode value **0x119**. For comparison: 0x101 on the 7362 SL and 0x105 on the 7520 |

Firmware hashes (SHA-256):

| File | SHA-256 |
|---|---|
| `dectfw_firstlevel_441.hex` | `72f8e27a0d01c301a05a8b373a71887af882a6cebcb1a30bd4a435580e1045f2` |
| `dectfw_secondlevel_441.hex` | `4fabee65d07a3214f9083de81ae54089f01bbfccb4cdbb672f37487a09cdfaa6` |

## 3. Hardware map (from the board's GPIO table)

| Function | GPIO | Notes |
|---|---|---|
| DECT reset | 33 | output |
| DECT "rd" line | 8 | |
| TDM / PCM-link FSC | 9 | frame sync |
| TDM / PCM-link DCL | 18 | **same pin as `spi_clk`** |
| PCM-link DO | 16 | **same pin as `spi_di`** |
| PCM-link DI | 17 | **same pin as `spi_do`** |
| DCL enable (clock-enable line of the PCM link) | 7 | |
| PCM-link "flash CS" / "flash WP" | 10 / 12 | Also listed as the SPI flash chip select (10) |
| DECT LED | 15 | (OpenWrt's 7412 DTS uses the same pin) |
| DECT button | 2 | (likewise in OpenWrt's DTS) |
| WLAN button | 1 | |
| Other LEDs | power 6, power-red 5, internet 34, WLAN 0, info 35 | |
| PCIe reset | 11 | |

- **PCM over SPI.** On the 7412 the DECT PCM bus runs over the **VR9 SSC (SPI) pins** in AVM's "SPI-TDM" mode. A GPIO-driven frame sync (FSC, GPIO9) is generated with GPTU timers, plus a clock-enable line (GPIO7). This is the main platform difference from the 7362 SL, which uses the dedicated VR9 TDM pins.
- **DECT UART = console UART.** The table has no separate DECT UART pins. The vendor loader uses the ASC console UART (it stops the console while it loads and follows the baud rate). As on the 7520, **a DECT-enabled OpenWrt build cannot keep the serial console on that UART**.
- **"Flash" lines on the PCM link.** The board table's flash chip-select and write-protect entries on the PCM link suggest an SPI flash device on the same SSC bus, possibly a flash attached to the DECT chip. To be clarified (PCB check and static RE).

## 4. Boot protocol and runtime architecture

Identical to the other boxes:
- **Boot:** GPIO reset of the DECT chip, then the ROM STX/SOH UART handshake for the first-level loader, then the first level's handshake with a 32-bit length and CRC16 for the STUB. The RFPI (derived from `maca`) and the mode word (0x119) are patched in before upload. *(Correction 2026-10-17: the check is one XOR byte in both stages, not a CRC16; see `stub-loader-protocol.md`.)*
- **Runtime:** checksummed control mails plus voice in PCM slots (see [stub-pcm-control-channel.md](../stub-pcm-control-channel.md)).
- **Host stack:** the vendor's DECT host stack runs DLC, NWK and security on the host.

## 5. Assessment as the first target

**In favour:**
- Cheapest DECT-capable box, already supported by OpenWrt (lantiq/xrx200, `avm_fritz7412`).
- Same host stack and STUB protocol family as the other boxes. The vendor's host stack is 98 % shared with the 7362 SL, so findings transfer directly.
- The PCM bus runs on the **SSC/SPI controller**, which has a mainline Linux driver (`spi-lantiq-ssc`). That is better documented than a proprietary TDM block, and SPI captures are a software-only observation path.
- Oldest and smallest STUB (2016), which may mean fewer features to handle.

**Challenges:**
- **Timing:** continuous PCM streaming over SPI with a timer-generated frame sync needs real-time behaviour (AVM uses MIPS MT "yield" handlers). Under OpenWrt this needs a careful driver: DMA plus a hardware timer for FSC, or a dedicated VPE/TC.
- **Console:** the serial console is shared with the DECT UART.
- **Resources:** a single-core MIPS 34K and limited RAM. That is enough for the DECT stack plus a SIP bridge, but less headroom for codecs and Asterisk than the 7520.
- **Unknowns:** what the mode word 0x119 encodes, and whether the "flash" lines matter at runtime.

## 6. Open questions

- What does mode 0x119 select (channel count, wideband, SPI-TDM framing)?
- SPI-TDM framing: word size, slot layout, and which side is clock master (GPTU-generated FSC suggests the host).
- The role of the "flash" lines on the PCM link.
- The factory DECT EEPROM default (fragment not yet recovered; the persistent copy on a running box is in TFFS).

## Legal note

Observation (EU Directive 2009/24/EC Art. 5(3)) and analysis for interoperability (Art. 6; §69d/§69e UrhG) allow this analysis for building an independent, interoperable implementation. AVM binaries and firmware blobs are not redistributed. New code must be written from these notes, not from analysed code.
