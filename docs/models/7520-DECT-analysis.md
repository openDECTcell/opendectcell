# FRITZ!Box 7520: DECT analysis

Source: `FRITZ.Box_7520-08.25.image` (HW247, FRITZ!OS 08.25, build 132372). The analysis was done read-only and in memory on 2026-10-15.

These notes record **interface facts only**, in our own words: pins, protocols, names and memory layout. They contain no AVM code and no AVM device-tree source. They are the clean-room input for writing a new DECT stack. AVM's firmware blobs are not part of this repository; they are referenced only by hash.

## 1. Image layout and extraction

| Item | Value |
|---|---|
| Container | GNU tar with a kernel image, a filesystem image, an install script and a 128-byte signature, plus a few update helpers |
| Root filesystem | Standard SquashFS 4.0 (little-endian, xz, 64 KiB blocks), tar data offset **724992**. Readable with `unsquashfs -o 724992 <image>` |
| Kernel | AVM EVA format: magic `0xfeed1281`, then total length, then load address **0x80208000**. Then a chunk header (`0x075a0201`, compressed size, uncompressed size, checksum) and the LZMA properties. The raw LZMA stream starts at **+36**. The kernel is Linux 4.4.271, ARMv7 SMP PREEMPT |
| Device trees | 3 DTBs inside the decompressed kernel (`AVM FRITZ!Box 7520`, `qcom,ipq4019`), one per hardware subrevision |

## 2. DECT-related components

| Component | Where | Role |
|---|---|---|
| ROM-loader payload | `lib/firmware/dectfw_firstlevel_441_slow.hex` | Small first-stage loader for the DECT chip. Intel HEX, **618 bytes at 0x10080** (chip RAM) |
| DECT chip firmware ("STUB") | `lib/firmware/dectfw_secondlevel_441.hex` | Firmware for the Dialog SC144xx, about **54 KB** in several regions (0x0–0x3cfd, 0x9000–0x9fff, 0x17300–0x17ff7, 0x20000–0x28dcd). Contains the build-date string "Jan 29 2026" |
| Production-test STUB | `lib/firmware/dectfw_secondlevel_441p2test.hex` | Used instead of the normal STUB when the boot-loader variable `ptest` is set |
| Firmware loader | a kernel module of the vendor | Resets the DECT chip, uploads both stages over UART and patches the RFPI and the mode word |
| PCM driver | a kernel module of the vendor | Drives the TDM/PCM bus on top of the kernel's TDM interface. Carries the DECT control slot and the voice slots, runs the control-slot synchronisation, and handles G.722, echo cancellation and DTMF |
| DECT host stack | a closed-source kernel module of the vendor | DLC, NWK, DSAA/DSAA2, AES, ULE and the EEPROM emulation run on the host |
| Audio and call control | the vendor's audio and VoIP drivers and user-space daemons | Path from the handset audio to VoIP/RTP |
| EEPROM defaults | a factory EEPROM image in the firmware (2551 B) | The persistent copy lives in the box's settings storage (TFFS) |
| Init | the vendor's init script | Loads the loader with the two firmware file names and the loader mode value 0x105 |

Firmware hashes (SHA-256), for verifying later extractions:

| File | SHA-256 |
|---|---|
| `dectfw_firstlevel_441_slow.hex` | `0d855b5929f8b2ad0b8d8fb38065b32e27f9357cd44a2b4e60573b1b5b6da895` |
| `dectfw_secondlevel_441.hex` | `7cac33c72b2222468f34033a15c35487aa35062265ab0013bb480068ebab7b31` |
| `dectfw_secondlevel_441p2test.hex` | `6361384d0e3b717e33ca65111cef82df8952da28e24b9d3b94d055b20dbc4860` |

## 3. Hardware map (from the board's device tree)

| Function | Pin / block |
|---|---|
| DECT reset | GPIO40 |
| DECT "rd" line | GPIO16 |
| DECT UART | **The console UART**: `blsp_uart0`, serial@78af000, GPIO16 (RX) and GPIO17 (TX) |
| PCM link 1 | FSC GPIO22, DCL GPIO21, MCL GPIO20 |
| PCM link 2 | FSC GPIO26, DCL GPIO25, MCL GPIO24 |
| PCM data | DI GPIO23, DO GPIO27 |
| Audio blocks | IPQ4019 ADSS: `qca,ipq4019-audio-adss` (0x7700000), `qca,ipq4019-pcm` (0x7704000), mbox DMA at 0x7706000–0x770e000, stereo blocks at 0x7709000–0x770f000, **`qca,ipq4019-tdm` enabled** |
| Fon/DECT LED | GPIO35 (active low) |
| Buttons | Fon GPIO43, WLAN GPIO42, Connect GPIO41 (all active low) |
| USB power | GPIO49 |

- **Console conflict.** The vendor loader stops the Linux console and talks to the DECT chip over the console UART; afterwards the same UART carries DECT debug and "LED dim" events. **An OpenWrt build with DECT cannot keep a serial console on UART0.**
- **DECT chip type: SC14448** (*observed*: the chip top marking of a 7520 reads "dialog SC14448 B96R101QNC H0MA80.1 1933EHAB", read by the maintainer from a photo on 2026-11-07; the date code 1933 most likely means 2019, week 33. Earlier, a board photo reported on 2026-10-17 had given the type only; matches a 2019 OpenWrt mailing-list post about the 7530). The firmware family is nevertheless named `_441`, while a build string inside the loader module names a 14488 part. At runtime, the vendor's host driver can print the chip's `OTP_VERSION` and `HW_CFG` (`dect_chip_hw_cfg`).

## 4. Firmware boot protocol

1. **Reset and ROM handshake.** Pulse DECT reset (GPIO40). The ROM bootloader sends **STX (0x02)**.
2. **Stage 1.** The host sends **SOH (0x01)** and a 16-bit length, then the first-level loader. The ROM answers with a checksum, and the host confirms with ACK. The vendor loader sends the first level at **115200 baud**, the "slow" variant; a fast mode with a fallback to slow is also coded.
3. **Stage 2.** The first-level loader repeats the STX/SOH handshake with a **32-bit length**, then the second-level image. It answers with a **16-bit CRC**, and NACK carries error codes. The baud rate can be raised to 230400 or 460800. *(Correction 2026-10-17: the check is one XOR byte in both stages, not a CRC16; see `stub-loader-protocol.md`.)*
4. **Patching before upload.** The host converts the Intel HEX into a binary (`the HEX reader`) and patches tagged locations in the second-level image:
   - the **RFPI**, which is derived from the urlader variable `maca`
   - the **DECT mode** word (`the loader mode value`, 0x105 here)

   It also reads a **STUB version tag**.
5. **Boot and supervision.** After the upload the loader reports ready, and the host stack takes over via PCM.

The ROM part matches the public SC144xx UART boot protocol (`TobleMiner/dialog-cr16c-uart-boot`, MIT licence). In that protocol the ROM starts at 9600 baud, takes a 16-bit little-endian length, returns an XOR checksum, and expects `06 00` as confirmation.

## 5. Runtime architecture

```
 handsets ))) SC144xx "STUB" (radio, MAC, ADPCM<->PCM, G.722 wideband, channel switch)
                 |  PCM/TDM bus: control-mail slot + voice slots
            the vendor's PCM driver (IPQ40xx TDM, slot router, sync supervision)
                 |
            the vendor's DECT host stack (DLC, NWK, DSAA/DSAA2, AES, ULE, EEPROM emulation)
                 |
            the vendor's call control, SIP and RTP
```

- **Host↔STUB link.** The link uses the **PCM bus, not the UART**. The host stack sends and receives "mails" through the PCM driver's control-channel interface: register a handler, configure the control channel, open and close slots. The PCM driver part computes **mail checksums**, validates mail lengths, uses an **idle pattern** in the control slot, synchronises the framing and supervises the STUB's liveness.
- **Mail OS.** The stack uses an RTX/Dialog-style mail OS: mails are addressed to numbered tasks (see the task table below).
- **STUB audio channels:** ADPCM 0..3, PCM 4..6, wideband 7..9, silence 12, test tone 15. The host binds handset connections to PCM slots; the chip reports the assignment.
- **Codecs** in the codec-list handling: G.726, G.722, G.711 A-law and µ-law, G.729. G.726 is the fallback.
- **Security:** DSAA and DSAA2 authentication run on the host; the cipher key is handed to the MAC by separate cipher-request and key-allocate mails.
- **Feature extras:** ULE smart-home, SUOTA/FUOTA handset updates, repeater support, and handset-capability adjustments for some handset families.

The NWK layer follows the standard ETSI EN 300 175-5 message-type codes (for example setup = 0x05, release = 0x4d). The MAC/STUB primitive IDs are not table-driven; they are recorded in [stub-api.md](../stub-api.md).

### Mail task IDs (the destination and source fields of a mail)

| ID | Role | ID | Role |
|---|---|---|---|
| 0x01 | authentication | 0x23 | PCM task of the remote side |
| 0x02 | call-control feature layer | 0x25 | timer interrupt (chip side) |
| 0x03 | DLC / LCE | 0x26 | MAC interrupt (chip side) |
| 0x04 | call control | 0x27 | test bus interrupt (chip side) |
| 0x05 | mobility management | 0x28 | DSP interrupt (chip side) |
| 0x06 | base control | 0x29 | upper layer |
| 0x10 | protocol map (audio assignment) | 0x2a | null task |
| 0x14 | user interface | 0x22 | PCM task (host side) |
| 0x15 | production test | | |

## 6. Observation hooks in the stock firmware

The vendor's stack offers debug interfaces (status read-outs through the kernel's proc interface, a MAC/DLC frame trace over a TCP socket, the DECT UART mirrored to a socket, and debug levels of its user interface). They make live observation possible on a box running stock firmware, for example with a Freetz-NG shell. The 7520 is supported by Freetz-NG. Their names are not recorded here.

## 7. What a rewrite needs (7520 view)

1. **Firmware loader** (userspace is fine): GPIO reset, ROM handshake, stage-1 and stage-2 upload, RFPI and mode patching. Blob source: an AVM image or the box, never shipped.
2. **Console handling:** move the OpenWrt console off UART0, or release it after early boot.
3. **TDM driver** for the IPQ4019 ADSS block with mbox DMA. Qualcomm's QSDK has a GPL driver for this block that can be ported. It needs to expose the control slot and the voice slots.
4. **STUB control protocol:** mail framing, checksum, idle pattern, framing sync and status handshake. This is the biggest unknown. Recover it from the PCM driver plus logic-analyser captures of FSC, DCL, DI and DO.
5. **MAC primitive set:** numeric IDs and parameters for the primitives in §5. They are recorded in [stub-api.md](../stub-api.md).
6. **DLC + NWK + security:** per ETSI EN 300 175 (GAP first, then CAT-iq wideband). libdect (OsmocomDECT) is a possible NWK base.
7. **Telephony bridge** from handsets to SIP (Asterisk/FreeSWITCH) with audio between PCM slots and RTP.

## 8. Observations on a running box

Source: *observed on hardware*. FRITZ!Box 7520 running FRITZ!OS 08.25 (175.08.25), with a FRITZ!Fon C4 registered. Kernel log and DECT sections of the FRITZ!OS support data, taken on 2026-10-17. Unit-specific identities (RFPI, IPUI) are deliberately not recorded here.

| Topic | Observation | Notes |
|---|---|---|
| GPIOs | The vendor loader takes DECT reset = GPIO40 and "rd" = GPIO16 from the board's hardware config | Confirms §3 |
| STUB patching | An RFPI TLV is found in the factory DECT config. The loader also asks for **TLV tag 9**, which is absent on this unit and ignored. Mode byte **5** is patched | Confirms tag 7 = low byte of `the loader mode value` (0x105). Meaning of tag 9: *open* |
| Second-level size | Total length **54,119 bytes** (0xD367), followed by a one-byte checksum | The checksum is unit-specific because it covers the RFPI |
| Image layout | The HEX file holds 54,047 data bytes in 9 contiguous runs. 54,119 − 54,047 = 72 = 9 × 8 | *Assumed*: an 8-byte header (address and length) per run. Verify with the image builder and simulator (M2) |
| Upload | "slow" boot mode. From loader start to "second level uploaded": about 6.1 s. No baud-rate change is logged | *Assumed*: 115200 baud throughout (54,119 bytes take about 4.7 s at 115200 8N1) |
| PCM timing | The loader measures **FS = 8000 Hz** and **DCL = 1,025,735 Hz** | Nominal 1.024 MHz (the software measurement reads about 0.17 % high) = **128 DCL cycles per frame**: 16 bytes per frame at one bit per clock, or 8 bytes if DCL runs at double bit rate. Confirm in M3 |
| PCM channels | The vendor's PCM driver reports 9 codec slots, 1 SLIC (the analog port), **DECT = 4** | *Assumed*: 4 DECT voice channels on the PCM link |
| Base settings | ECO mode on, "no emission" active (the base stops transmitting while idle if all handsets support it), CAT-iq 2.0 support flag set, extended security on | Our stack has to decide whether to support no-emission mode |
| Handset (FRITZ!Fon C4) | Offers the codecs **CELT, G.722, G.726**; handset firmware 05.011 | CELT = CAT-iq super-wideband |
| Boot timeline | The loader runs about 15 s after kernel start; upload done at about 21 s; TDM measurement at about 23 s; then the PCM, audio and host-stack drivers load | |

### 8.1 Under OpenWrt

Source: *observed on hardware*. Spare FRITZ!Box 7520, OpenWrt 25.12.5 (r33051-f5dae5ece4) installed on 2026-10-17 with the `avm_fritzbox-7530` image and the 7520's own U-Boot (`uboot-fritz7520.bin`).

| Topic | Observation | Notes |
|---|---|---|
| Board | `board_name` = **`avm,fritzbox-7530`**, model string "AVM FRITZ!Box 7530" | The 7520 runs the 7530 image, so its board profile is `boards/avm,fritzbox-7530` and covers both |
| Kernel, RAM | Linux 6.12.94, 4 cores, about 240 MB usable RAM | |
| Flash | 11 MTD partitions: SBL1, MIBIB, QSEE, CDT, QSEE_B, urlader0/1, `nand-tffs` (8.25 MiB), uboot0/1 (now OpenWrt's U-Boot), `ubi`. UBI volumes after installation: kernel, rootfs, the vendor's two data volumes, rootfs_data (overlay, about 68 MB free) | The vendor's data volumes survive the installation. A full backup from before the installation exists locally (not in git) |
| Console | `ttyMSM0` on `serial@78af000`, 115200 baud | **The same UART that loads the DECT chip** (§3). Our loader must take it over from the console (M1, console strategy) |

### 8.2 PCM bus after the STUB boots

Source: *observed on hardware*, spare 7520, OpenWrt 25.12.5, 2026-10-17. The STUB was uploaded with `dect-fwload`; the pins were then read back-to-back through the sysfs GPIO interface with `tools/gpio-activity.c` (100,000 reads per pin, about 150-220 thousand reads per second). The pin roles are the ones from §3.

| GPIO | Role (§3) | Chip in reset | STUB running |
|---|---|---|---|
| 20 | PCM link 1 MCL | static high | 49 % high, many changes |
| 21 | PCM link 1 DCL | static low | 51 % high, many changes |
| 22 | PCM link 1 FSC | static high | **0.8 % high** |
| 23 | PCM data DI | static high | 75 % high, many changes |
| 24 | PCM link 2 MCL | static high | 49 % high, many changes |
| 25 | PCM link 2 DCL | static low | 51 % high, many changes |
| 26 | PCM link 2 FSC | static high | **0.8 % high** |
| 27 | PCM data DO | static high | static high |

Findings:
- **The DECT chip drives the PCM clocks and the frame sync.** All eight pins are completely static with the chip in reset, and the clock and frame pins start toggling once the STUB runs, while the host does not drive them in OpenWrt (they are plain inputs). So the chip is the **PCM master**; this answers the "clock master" question for the 7520.
- **Both link groups carry the same timing.** The two MCL/DCL/FSC groups behave identically; probably the PCB routes the chip's signals to both groups of the SoC (to be checked in the capture, M3).
- **Frame sync:** a short pulse that is high 0.8 % of the time, about 1/128. With the 8 kHz frame rate and about 1.024 MHz bit clock measured by the stock loader (§8), 128 clocks per frame give exactly that duty for a **one-bit-wide frame pulse**.
- **DI carries data** (mostly high, as an idle 0xFF fill would look, with activity). DO is the host's line and stays idle.
- This is the first sign of life of the STUB. The sampling rate is far below the clock rate, so the change counts are **not frequencies**: they only show that the pins are active.

**Bit clock measured from the SoC** (*observed on hardware*, 2026-10-17, read-only TLMM polling in the `opendectcell` module, `DECT_IOC_CAPTURE` calibration): 1,027,749 / 1,024,590 / 1,019,367 Hz in three runs, i.e. **1.024 MHz** as logged by the stock loader. Without the STUB running, no clock.

**Polling cannot capture the PCM data:** one TLMM register read takes **268 ns** on this SoC (measured), and half a DCL period is about 488 ns, so only one read fits per half period. The capture therefore refuses to run ("too slow"). Capturing the frames needs the SoC's TDM receiver (ADSS block with DMA) or an external logic analyser.

**Gate G1 consequence:** the STUB does not need to print on the UART (nothing was received in 60 s); the PCM clocks are the better criterion, and the vendor's own loader also measures them right after the upload.

### 8.3 Factory DECT block in flash

Source: *observed on hardware*, backup of the spare 7520 (partitions `urlader0`/`urlader1`, taken from OpenWrt 25.12.5 booted into RAM), 2026-10-17. Only the structure is recorded: the values are per-unit identity and calibration data and are not committed.

| Item | Observation |
|---|---|
| Location | **`urlader0` and `urlader1`** (the two boot-loader partitions, `mtd5`/`mtd6`; identical in this unit) at offset **0x3D000**. It is not in the TFFS settings storage (`nand-tffs`), and `wlan_cal` there is empty |
| Header | 4 bytes: type **0x02**, id **0x01**, length as 16-bit big-endian (here 17). Id 1 matches the configuration lookup the vendor loader makes |
| Content | The TLV list that `dect-fwload` applies to the STUB (`stub-image-format.md` §4): **tag 1** (RFPI, 5 bytes, a real RFPI and not the "0270" placeholder), **tag 2** (2 bytes), **tag 3** (1 byte), **tag 4** (1 byte), then erased flash (0xFF) |
| Versus the STUB defaults | Tag 2 and tag 4 differ from the STUB's built-in defaults, tag 3 is equal. They are very likely per-unit radio calibration values. The default tags 6, 7 and 10 are absent from the block (7 is set from the mode word) |
| Effect | `dect-fwload --factory-mtd /dev/mtd5 --factory-offset 0x3d000` patches five entries (tags 1-4 and the mode byte) and the chip accepts the image; the PCM clocks come up as before |

The other data around the block (the first 0x4000 bytes after 0x3C000 hold several unrelated records) is not decoded.

## 9. Open questions

- Frame length in bits (the duty above and §8 point to 128 clocks per frame, i.e. a single-rate DCL), slot count and control-slot position: check the arguments of the kernel's TDM configuration call, plus a capture. FS (8 kHz) and DCL (1.024 MHz) are known (§8).
- Meaning of TLV tags 2, 3, 4 (probably calibration) and tag 10; TLV tag 9 is only a STUB version marker (`stub-image-format.md` §4).
- Exact framing of the second-level patch tags (RFPI and mode).
- Whether the STUB depends on host-side EEPROM content beyond the RFPI.

## Legal note

Observation (EU Directive 2009/24/EC Art. 5(3)) and analysis for interoperability (Art. 6; §69d/§69e UrhG) allow this analysis for building an independent, interoperable implementation. AVM binaries and firmware blobs are not redistributed. New code must be written from these notes, not from analysed code.
