# FRITZ!Box 7412: SPI-TDM link, firmware patching, control channel (working notes)

Status: **derived statically** from the FRITZ!OS 06.88 modules on 2026-10-16 (the analysis tooling 12.1.4, MIPS BE). Not yet verified on hardware. These are clean-room notes: behaviour and parameters only.

## 1. PCM bus = VR9 SSC in SPI-slave mode

- **Controller:** the PCM link to the DECT chip uses the VR9 **SSC** block (physical base 0x1E100800). That is the same controller mainline Linux drives with `spi-lantiq-ssc`, with the same register layout: CLC 0x00, CON 0x10, STAT 0x14, WHBSTATE 0x18, RXFCON 0x30, TXFCON 0x34, BRT 0x40.
- **Register values** programmed when the link is set up, and how they read against the `spi-lantiq-ssc` bit definitions:

| Register | Value | Meaning |
|---|---|---|
| CLC | 0x100 | module clock enabled, divider 1 |
| CON | 0x005F0050 | **32-bit words** (BM = 31), **MSB first** (HB), clock **polarity 1** (PO), phase 0, RX and TX on |
| WHBSTATE | 0xE54 | **clears master mode → SSC runs as SPI slave**, and clears all error flags |
| BRT | 1 | baud-rate value (only relevant in master mode) |
| TXFCON | 0x103 | TX FIFO enabled and flushed, interrupt level 1 |
| RXFCON | (n << 8) \| 3 | RX FIFO enabled and flushed, interrupt level n = words per frame (≤ 63) |

- **Clock and frame sync:** because the SSC is a slave, the bit clock (DCL, GPIO18) comes from outside the SSC. Candidates are the DECT chip or a clock source gated by GPIO7 (`(vendor-internal name)` / `the PCM clock-enable line`). Frame sync is on GPIO9. `PCM-link` also uses GPTU timers. **Who drives DCL and FSC is still to be decided.** This matters for multi-cell synchronisation.
- **Frame handling:** each frame is a fixed number of 32-bit words. The module keeps two copy descriptors (RX/TX double buffering) and per-slot copy routines. The real-time work runs in a MIPS MT **"yield" handler**, with a fall-back to a normal IRQ (`PCMLINK_IRQ`).
- **SSC module clock:** the SSC's power-management domain is enabled before setup (PMU bit 8).
- **GPIO handling:** GPIO33 (DECT reset) is driven low during link reset. The SPI pins are taken over from the generic SPI use: GPIO16/17/18 are shared with the SPI flash signals in the AVM GPIO table.

**Implication for OpenWrt:** an SSC slave-mode streaming driver is needed (the mainline driver only supports master mode). The hardware is the well-understood Lantiq SSC, so a dedicated small driver with FIFO or DMA plus a real-time path is feasible.

## 2. STUB firmware patching (the first-stage loader, `the loader routine`)

- **First-level loader:** loaded from Intel HEX at chip address 0x10080. If the file name contains `_slow`, a slow boot variant is used.
- **Second-level image (STUB):**
  - The image starts with a **4-byte big-endian payload length**, followed by the payload. A **one-byte XOR checksum** of the payload follows it, and the host recomputes it after patching.
  - Chip address **0x0006** holds a 6-byte descriptor: a 32-bit LE address and a 16-bit LE maximum length of the **patch area**.
    **Correction (2026-10-17):** the 08.25 code on the 7520 reads this 6-byte descriptor at chip address **0x0**, not 0x0006; see `stub-image-format.md` §5.
  - Address translation: chip address 0 for the `441` firmware family, 0xF0000 for the `488` family.
- **Patch content:** a **TLV list** (tag u8, length u8, value) written into the patch area:
  - The TLVs come from AVM's per-device factory configuration (`get_wlan_dect_config(1, …)`, up to 0x84 bytes). They likely include per-unit RF calibration in addition to identity. → **On OpenWrt this block must be read from the box's flash**, like Wi-Fi calibration data.
  - **Tag 1, length 5 = RFPI.** If the stored RFPI equals the factory placeholder, the RFPI is derived from a MAC environment variable instead.
  - **Tag 7, length 1 = the low byte of `the loader mode value`**: 0x19 on the 7412, 0x05 on the 7520, 0x01 on the 7362 SL. Its meaning is interpreted by the STUB and still unknown (candidates: PCM interface type, clock role, channel layout).
  - **Bit 8 of `the loader mode value`** (set on all three boxes) is a boot-option flag passed to the first-level upload.
- **Factory test:** if the urlader variable `ptest` contains `dect=cal`, patching is skipped (calibration mode).
- **Upload:** the console UART is paused (unless the hardware profile says otherwise), then first and second level are uploaded, with up to 3 attempts. Afterwards the console is resumed.

## 3. Control channel (comparison with the 7520)

The same design as described in `stub-pcm-control-channel.md`:
- 0xAA start marker, the same checksum validation, idle fill 0xFF
- maximum TX mail length 0xFC
- the 16-bit primitive ID at mail offset 4

**Differences:**
- The 7412 build byte-swaps the parameters of only one primitive (0x0158) on its big-endian host, while the 7520 build handles 0xF626 and 0xFFF1 too. The older (2016) STUB probably lacks those two primitives.
- On the 7412 (big-endian MIPS), all little-endian STUB fields must be converted. A new stack has to do this explicitly.

## 4. Open items

1. Meaning of tag 7 (mode byte) inside the STUB: needs CR16C analysis of the STUB, or an observation.
2. DCL/FSC source and direction on the 7412 (GPIO7 / GPIO9 / GPTU usage).
3. Frame length (words per frame), control-slot position and voice-slot mapping in SPI-TDM mode.
4. The factory DECT config block: where it is stored in flash and its full TLV catalogue.
5. Whether the `PCM-link*` lines (GPIO10/12) are used at runtime.
