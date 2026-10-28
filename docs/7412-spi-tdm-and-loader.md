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

## 1a. Bench 2026-10-28 (observed on hardware)

Source: *observed on hardware*, FRITZ!Box 7412, OpenWrt 25.12.6 (Linux
6.12.112), openDECTcell 0.3.1 `kmod-opendectcell-lantiq` plus the generic
`kmod-lantiq-ssc-pcm` transport for captures; factory block read from the
box. Radio never switched on (no radio-on mail was accepted).

| Finding | Detail |
|---|---|
| Factory DECT block | In the `urlader` partition (`/dev/mtd0`) at offset **0x1E800**, same format as the 7520's: header `02 01`, BE16 length 17, then TLVs 1 (5 bytes), 2 (2), 3 (1), 4 (1). `dect-fwload` patches it as on the 7520 ("5 TLV patched"). RFPI is not the placeholder. |
| Which STUB clocks PCM | Only the 7412's own **06.88** build (sha256 `72f8e27a…` / `4fabee65…`). The 7520's **07.56 and 08.25** builds upload fine but drive **no bit clock** with mode bytes 0x119, 0x105, 0x101, 0x109, 0x111, 0x11D, 0x104 and either first level. |
| Mode byte = bit clock (06.88) | Words of 32 bit per 125 µs frame: **0x101/0x105 → 4** (1.024 MHz), **0x109 → 8**, **0x111/0x115 → 16**, **0x119/0x11D → 18** (4.608 MHz). The stock value 0x19 gives 18 words (36 slots). |
| GPIO7 | Must stay high from before the chip's reset on; dropping it between `dect-fwload` and the next open silenced the bus (fixed in `260be17`). |
| Bit alignment | The SSC has no frame sync; enabled while the clock runs, its words start at an arbitrary bit offset into the frame (2, 3 and 5 bits seen on different boots, constant within one). Receive and transmit must be aligned in software or the SSC enabled before the chip's clock starts [CR A-0002]. |
| CPU engine vs rate | An interrupt-driven FIFO engine is loss-free at 4 words/frame and near loss-free at 8, but loses 1–3 % of frames at 18 words (8-word FIFO); the stock driver uses DMA [CR A-0002]. openDECTcell's hrtimer engine reads at most 8 words per 125 µs and aligns on `RX_EOM`, which stock never uses [CR A-0001], so at 18 words/frame it lost over half the data. |
| 06.88 control channel | In mode 0x105 the chip's slots carry long runs of **0x7E** (HDLC flag?) rather than the 08.25 `0xAA` framing. A contact `0xF600` in 08.25 framing, sent in every slot, bit shift (0–15) and byte order (512 variants), drew no checksum-valid answer. The 06.88 mail framing is asked in the 7412 clean-room record (Q-0004, Q-0005). |

Consequence: openDECTcell on the 7412 needs (a) the 06.88 STUB's mail framing, (b)
bit alignment, and (c) either a slower mode (0x105 gives the 7520's 8-slot,
1.024 MHz geometry) or a DMA engine for the stock 0x119.

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
