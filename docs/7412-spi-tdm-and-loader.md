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
| 06.88 control channel | **Same framing and primitives as 08.25** [CR A-0004]; **observed answering**: in mode 0x119, with the SSC running before the chip boots, the STUB sends periodic `0xF618` (`23→14`, L 4), `0xF623` (L 16) and `0xFFF1` (`23→22`, L 27) about every 5 s, and answered a contact `0xF600` with **`0xF601`** (`02→14`, L 7). The probe that hit used a transmit bit shift equal to the measured receive lane offset and the **low byte of the 16-bit slot first** (as on the 7520). |
| Modes without control channel | No mail at all in 0x105, 0x109 or 0x111 (12 s each); in 0x105 the slots carry runs of 0x7E, which belong to the handset-update (UOTA) channel's HDLC framing, not to control [CR A-0005]. The stock 0x119 (18 words/frame) is required. |

Consequence: openDECTcell on the 7412 needs (a) the stock mode 0x119 and its
18-word frame (36 slots), (b) bit alignment (receive and transmit shifted by
the same lane offset), and (c) a word-loss-free transport at 4.6 Mbit/s —
the CPU FIFO path loses 1–3 % of frames, each loss moving the control slot
by a word. (c) is the generic transport's DMA work (`kmod-lantiq-ssc-pcm`,
clean-room Q-0006). The mail stack itself needs no change.

**Update, same night (observed on hardware):** `kmod-lantiq-ssc-pcm`'s DMA
engine carries the 18-word frame with no lost word (125,599 frames). Started
before the chip boots, with the contact sent at TX bit shift = measured lane
offset, it drew the first-contact sequence **`0xFFF2`** (`28→22`, L 28),
**`0xF601`** (L 7), **`0xF602`** (L 25), `0xFFF1`, then the periodic reports
every 5 s — all in one fixed receive slot (that session: lane offset 6 bits,
TX slot 29, RX slot 9 of 36, low byte of each slot first). Slot numbers
depend on the session's alignment and must be probed, as on the 7520. The
path for openDECTcell: use `/dev/ssc-pcm0` (36 slots) for the 7412 instead of its
own SSC engine, start it before `dect-fwload`, align by the measured offset.

## 1b. dect-stubd on the SSC transport (2026-10-29, observed on hardware)

Same box and software as §1a, 06.88 STUB, mode 0x119.

| Finding | Detail |
|---|---|
| Start order | **Not needed.** Started *after* `dect-fwload`, with the chip already clocking, the transport carried the contact answer (`0xFFF2`, `0xF601`, `0xF602`) as before; only the lane offset was new (6 bits that run). The earlier "start before `dect-fwload`" was the order that happened to be used, not a requirement. What is required: `lantiq_ssc_pcm` loaded before the chip leaves reset, since it holds GPIO7. |
| Closing `/dev/dect0` | Switched the SSC off under the transport (STATE bit 0 cleared) until `4602f60`; `dect-fwload` exits right before the daemon starts. |
| Idle lanes | Three adjacent 16-bit lanes read `0xFEFE` in every frame, followed by `0xD5D5` and `0x0C08`. Their count peaks at the true lane offset (1536 per 512 frames) and again 8 bits later (1024), where two idle lanes are adjacent. |
| Byte order | `read()` delivers bus order. Each slot's two bytes are swapped against the 7520's DMA buffer layout, in which an element's first byte is a slot's first byte. |
| `dect-stubd` | `-d /dev/ssc-pcm0 -c 36`: measures the offset, aligns both directions, swaps each slot, and probes the control slot as on the 7520. First run: offset 3, control slot TX 32 / RX 13, contact answered, periodic `0xFFF1`/`0xF618`/`0xF623` every 5 s for 20 s, 0 sequence gaps and 0 checksum errors (`5667930`). |
| STUB name | The profile now names the 7412's stages `*-06.88-7412.hex`. Under the old names, `dect-stubd` would have taken 06.88 for 08.25 and read the 08.25 UAK table address (`05c903d`). |
| Service run (R1) | `/etc/init.d/opendectcell start` with the 7412 profile (`56a2d33`): `lantiq_ssc_pcm` and `opendectcell` loaded, 06.88 uploaded, `dect-stubd -d /dev/ssc-pcm0 -c 36 -g` started. Control slot found on the third pass, 6 s after the upload, as on the 7520 ("the chip may still be starting"). Bring-up mails `0x0153`, `0x0169`, `0x0157` (src 0x14), `0x015B 01`, `0x022C 01` sent. Session up for 39 s, 0 deaths, 0 sequence gaps or checksum errors; stop parked the radio (`0x022C 01`, `0x015B 00`, on the wire within 6 ms). |
| Radio run (R2) | The same by hand with `-v`, 30 s: after the bring-up mails one `0xF602` (23 octets), and one after each park mail; periodic reports throughout. The chip then went back into reset. **Not verified:** that the radio beacons. `0xF602` carries the beacon content, not the radio state (docs/mac-bringup.md), and no handset was at hand. |

### Command check (2026-10-29, observed on hardware)

One short `dect-stubd -p` run per command, 06.88 STUB, mode 0x119, no handset.

| Command | Answer on 06.88 | Against 08.25 / A-0007 |
|---|---|---|
| `0xF600` contact | `0xF601` (5 octets) and `0xF602` (23) | same. The chip answered only 30 to 60 s after its upload in several runs; the `0xFFF2` build stamp of the first contact was not caught |
| `0xFFF1/07` memory read | `0xFFF1/08` echoing address and count, 32 bytes; repeated reads identical; a full 64 KiB sweep answered 5489 of 5490 requests | same |
| `0xF624 {h,0A}` status poll | `0xF625`, **44** octets, for handles 0 and 1 with no link; ten RSSI octets at offsets 6-15 | 45 octets on 08.25 |
| `0x0169 07`, `0x0157 00`, `0x0153 00` (src 0x14), radio off | **no reply** | A-0007 expected `0xF602`/`0xF623` (*derived*) |
| `0x022B 41`, then `0x022C 01` | `0xF602` byte 1 `00` -> `08` -> `00` | same (capability a44) |
| `0x020A` idx 1 with a marker record | the marker appears at **`0x067F`**, flag at +0x10, idx 2 at `0x0690` | 08.25: `0x9518`. Confirms A-0007's displacement `0x067F` |
| `0x015B 00` | `0xF602` byte 2 `01` -> `02`; `0xF623` stops | byte 2 reads `01` from boot on |

**Radio start.** In the 7520's bring-up order the first radio-on after a boot
left the `0xF623` scan report flat (every carrier `0x1B`) for 26, 30 and 120 s,
header `00 02`/`01 02`. With `0x015B 00` before the configuration (or after a
park in an earlier run) it showed levels up to `0x3B`, strongest on carriers
5-7, within 1-2 s, five runs of five; radio-off sent after the configuration
did not help. `dect-stubd` now starts the 06.88 bring-up with radio-off
(`143c5fd`). In the validation run the levels fell back to the floor after
about 15 s while the header stayed `04 02`. Whether the levels are our own
beacon or a neighbour that the scan sees only while it runs is **open**
(asked as Q-0008); a handset decides (`bench-7412.md`).

**Field meanings [CR A-0008]** (the room's 08.25 model; our runs fit it
except where noted):

| Field | Meaning | Our runs |
|---|---|---|
| `0xF602` byte 2 | radio state, `01` on, `02` off; `01` from boot | fits |
| `0xF602` byte 1 bit 3 | a44, registration open | fits (`022B`/`022C`) |
| `0xF623[0]` | carrier of the beacon, `0xFF` none | `ff` from boot until a mail; see below |
| `0xF623[1]` | beacon bearer, nonzero = present | `02`, `04` after a park |
| `0xF623[2..11]` | receive peak-hold level per carrier; decays once the beacon has settled | explains the fall to the floor after ~15 s |
| `0xF623[12]` | temperature, as `0xF618` | `22`-`26`, rising with run time |
| `0xF623[13]` bit 6 | no emission | always `00` here |
| `0x015B` | acts only on a change of the radio state | the edge alone did not help (X2) |
| `0x0153`/`0x0157`/`0x0169` | latch silently; report only when the broadcast changes | no reply with the radio off, as seen |
| `0xF625` | 44 octets: 08.25's trailing constant octet dropped | — |

**RAM addresses [CR A-0009], checked on the box** (fresh boot, memory sweeps
every 0.6 s next to `0xF623`): `0x010A` radio switch (`01` from boot, `00`
after `015B 00`, `01` after radio-on, `00` after the park); `0x010C` and
`0x010B` equal `0xF623[0]` and `[1]`; `0x0188 + 2n` equals the ten levels
byte for byte, decay included. After a park, though, `0x010C`/`0x010B` keep
the carrier and bearer while `0x010A` is 0 and `0xF623` stops, so it is open
whether the park ends the transmission or the bytes go stale (Q-0010;
`bench-7412.md` test 3 checks it with a handset). **[CR A-0010]:** stale --
the `0x015B 00` path clears `0x010A`, removes both beacon bearers (the
inverse of the radio-on path) and stops the MAC, but never writes
`0x010B`/`0x010C`. Use `0x010A` == 0 plus the absence of `0xF623` as "parked".
It does not rewrite the RF registers the radio-on path sets, so "no RF" rests
on no bearer being scheduled; the handset test stays. Our chip-side
cross-check was inconclusive: `0x08A0`-`0x08DF` looks like a timer list (a
100 Hz counter at `0x08B1` runs at boot and while on, frozen after the park),
not the bearer table A-0010 named (Q-0011). `0x0776` read
`0x80` throughout: bit 7 is set at boot, and bit 0 (eco on) follows bit 7 of
the `0x0153` parameter, which our `00` leaves clear [CR A-0010].
A-0009 also explains X2: in the boot state (`0x010A` = 1) every
configuration mail brings the beacon up on a default carrier, after which
radio-on does not scan. So **any configuration or registration mail in the
boot state transmits** on 06.88; `dect-stubd` sends `0x015B 00` first, and
subscription restores wait for it.

Answered by A-0009 (kept for the record): why X2 (configuration, then a real off->on edge)
stalled while Z1 (radio-off before the configuration) worked; and whether
`0xF623[0]` changing `ff` -> `05` within 5 s of a lone `0x0169`, with no
radio-on ever sent, means the chip beacons by itself. With no mail after the
boot it stays `ff`. Since `143c5fd` the 7412 bring-up begins with radio-off,
and every run ends parked, so this does not change what the service sends.

The daemon's log masks key bytes of memory reads in the 08.25 table range
(`0x9518`) whatever the build; on 06.88 the table is at `0x067F` and is not
masked, so do not memory-dump that range with `-p` once handsets are
registered.

### 06.88 against 08.25 [CR A-0007]

*Derived statically* from both STUB blobs' mail dispatch tables:

- Every bring-up, registration and subscription mail `dect-stubd` sends has a 06.88 handler with the same dispatch flags. Dispatch is by primitive, not by source task. The `0x020A` record handler is the same routine (16-byte record plus flag, stride 0x11, six slots).
- The UAK record table is **not** at 0x9518 on 06.88; A-0007 gave only the displacement `0x067F`, and the command check found it there. `dect-stubd` still treats 06.88 as an unknown build and skips its table reads; using `0x067F` is open.
- Missing on 06.88: `0x0251` (channel parameter, sent at call set-up, presumably ignored), `0x015E` and `0x015F` (sent only by the optional paging sequence or not at all), `0x0159`, `0x015C`.

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
