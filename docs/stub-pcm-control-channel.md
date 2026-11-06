# Host ↔ STUB control channel on the PCM bus (working notes)

Status: first written 2026-10-15, **analysed** from the FRITZ!Box 7520 (FRITZ!OS 08.25) modules. **Mail exchange verified on hardware on 2026-10-17 (gate G2)**; **request/answer loop verified on hardware on 2026-10-17 (gate G3)**; see the next sections, which take precedence over older statements below. These are clean-room notes that describe behaviour, not code.

## Audio-path mails: GenDSP connect, test tone, loop-back [CR A-0004]

*Analysed* from the vendor's host and PCM drivers (FRITZ!OS 08.25, 7520); parameter order and a few points marked *assumed*. For gate G4 (test tone in a voice slot; loop-back). The STUB has internal **GenDSP audio channels**: ADPCM 0..3, PCM 4..6, wideband 7..9, **silence 12, test tone 15** (also `docs/models/7520-DECT-analysis.md`).

- **Connect** (AVM's debug `dect_connect chn_dst chn_src`, "[stock log message]"): host → STUB mail
  - source task `0x22`, destination task `0x23`, primitive **`0xFFF1`**
  - parameters (3 bytes): **`0x0A`** (sub-command: connect), **chn_dst**, **chn_src** → `L` = 5, `param_len` = 3
  - chn_dst ∈ {ADPCM 0..3, PCM 4..6, wideband 7..9}; chn_src additionally allows **silence 12** and **test tone 15**
  - so the **test tone** on, say, PCM channel 4 is `0xFFF1` params `0A 04 0F`. No acknowledgement primitive is known; mute by connecting the destination to silence 12 [assumed].
- **Loop-back** (the vendor's debug loop-back for channels 1..3, on or off, PCM → PCM and DECT → DECT): host → STUB mail
  - source `0x22`, destination `0x23`, primitive **`0xFFF1`**, parameters: **`0x09`** (sub-command: loop) then a **32-bit little-endian channel field** (1..3 = loop that channel on, 0 = off) → `L` = 7, `param_len` = 5
  - this is consistent with the earlier note that `0xFFF1` sub-command 9 carries a 32-bit field at mail offset 7.
- **Sample format:** voice channels are **8-bit A-law in one byte half** of the 16-bit slot (observed at rest: the A-law byte is the **low** byte, the other half `0x00`; `0x55`/`0xD5` = A-law zero). Wideband channels (7..9) likely use a linear format [CR A-0004, assumed].
- **Prerequisites:** the send paths check no host-side MAC or connection state; it is a debug path, so try it **right after contact** (whether the STUB runs GenDSP with no MAC is unconfirmed) [CR A-0004].
- **Not the bench path:** the chip's slot-assignment report is host-internal (a mail to the user-interface task `0x14`). The handset-call path -- the protocol-map layer's ADPCM connect and disconnect -- uses primitives near **`0x0230`/`0x0233`** to task `0x02` and needs a call identifier (CCEI). A generic STUB debug mail also exists: primitive **`0x01FE`** to task `0x02`, two parameter bytes ("exec debug cmd in stub"); its commands are unknown.
- **Open:** the STUB PCM-channel → TDM-slot mapping, the test-tone frequency/level, and GenDSP behaviour without a running MAC are not established — find the slot empirically by watching which buffer position changes from idle to a tone.

### G4 bench attempt (observed on hardware, 2026-10-17)

Source: *observed on hardware*, spare 7520, STUB of FRITZ!OS 08.25, `dect-pcmcap -s 6 -k -q -b N -A -m 0x22:0x02:0xf600 -m 0x22:0x23:0xfff1:0a<dst><src>` (contact, then GenDSP connect). The connect mail **is accepted and its effect persists**:

- **Destination → host TDM slot routing works.** Connecting a GenDSP destination changes a specific buffer slot from its rest fill to a destination-driven value, and it stays that way across reconnects. Observed: GenDSP **PCM channel 4 → buffer slot 5**, **PCM channel 6 → buffer slot 0** (slot 0 went from `0xFEFE` at rest to `0x0000`, slot 5 from constant `0x0055` to a low-level varying byte).
- **But the audio source does not generate at the bench.** Connecting **test tone (15)** and **silence (12)** to the same destination give **identical** output (slot 5: low byte wandering `0x51..0xdd`, AC RMS ≈ 9–24 of 32768 ≈ −60 dB; slot 0: constant `0x0000`). No loud, periodic tone appears in any slot for any destination 0..6, and the source channel number makes no difference. Parameter order (chn_dst, chn_src) vs swapped made no difference either.
- **Conclusion (assumed):** the GenDSP audio **source** (test tone, silence) needs the STUB's MAC/DSP in an active call context, which a pure contact bench does not provide — matching A-0004's open point. So the literal "STUB test tone" half of gate G4 probably needs minimal MAC bring-up (M5) first, or a trigger not yet known. Follow-ups: Q-0005 (no host-side start mail found), A-0005.

### Loop-back works (observed on hardware, 2026-10-17, gate G4)

Source: *observed on hardware*, spare 7520, STUB of FRITZ!OS 08.25, `dect-pcmcap -s 6 -k -q -b 800 -A -G <slot>:<freq> -m 0x22:0x02:0xf600 -m 0x22:0x23:0xfff1:09<ch>000000` (contact, then `dect_loop` on, while transmitting an A-law tone in a voice slot). **The STUB loop-back works on a contact-only bench** (no MAC needed), which makes it the usable path for gate G4's audio-path proof.

- **Enable:** the loop-back mail `0xFFF1` sub-command `0x09` with the 32-bit channel field = 1..3 (0 = off) [CR A-0004].
- **Channel → buffer-slot map (observed):** loop **channel 1 ↔ buffer slot 0**, **channel 2 ↔ buffer slot 3**, **channel 3 ↔ buffer slot 2**. With a loop on, a distinctive constant transmitted in that slot comes back in the same slot; with loop off it does not.
- **Real audio, both ways:** transmitting an A-law sine in slot 0 with loop channel 1 on returns the same tone in receive slot 0. The recovered frequency tracks the transmitted one linearly over 400–2400 Hz (RMS ≈ 14600 of 32768, a strong signal); with the loop off there is no tone. The recovered frequency reads a constant ×0.933 of the transmitted value — an artifact of idle (`0xFFFF`) frames in the capture (the contact-mail phase and occasional transmit-queue underruns) diluting the zero-crossing count, not a property of the loop-back.
- So the full voice-slot data path (host transmit → STUB → host receive, 8-bit A-law in the slot's low byte) is proven. The remaining open item is the STUB's own **test-tone source** (channel 15), which stays silent without the MAC (above).

## Verified on hardware: request/answer loop (gate G3, 2026-10-17)

Source: *observed on hardware*, spare 7520, OpenWrt 25.12.5, STUB of FRITZ!OS 08.25, `dect-stubd -v -t 30` (commit after 602f6eb). This run confirms the whole control channel through the daemon (framing + session state machine + slot probe).

- **Slot probe:** `dect-stubd` sends the contact in each transmit slot and looks for a valid answer in any receive slot. This run found **tx slot 1, rx slot 1** (not rotated this time; the probe handles rotation if it occurs).
- **Handshake:** one host contact (`0x22 → 0x02`, `0xF600`, no params) produced **`0xF601`** (src `0x02` → `0x14`, param_len 5) and **`0xF602`** (param_len 23), as in G2.
- **Periodic reports:** after contact the STUB sends **`0xFFF1`** (src `0x23` → `0x22`, **param_len 25**, i.e. the length field L = 27 of the G2 note) each immediately followed by **`0xF618`** (src `0x23` → `0x14`, param_len 2). Observed cadence this run: about **6 pairs in 30 s** (≈ every 5 s), comfortably within a 60 s liveness window.
- **Mail-count note:** the log's `len` is the parameter count (= L − 2). It matches the G2 note's parameter counts (f601 = 5, f602 = 23, f618 = 2) and, for f618, its parameters `37 01`.
- **Quality over 30 s:** 14 mails decoded with **0 sequence gaps, 0 checksum errors, 0 framing errors, 0 resyncs**; the session reached and stayed UP (0 liveness deaths). The host's single contact was enough; no retransmit was needed.
- Parameter bytes of `0xF601`/`0xF602` (box identity, RFPI-like) are not recorded here and are hidden by `dect-stubd` unless `-p` is given.

## Verified on hardware: control slot and mail exchange (gate G2, 2026-10-17)

Source: *observed on hardware*, spare 7520, OpenWrt 25.12.5, STUB of FRITZ!OS 08.25, Phase B module with transmit, `dect-pcmcap -m 0x22:0x02:0xf600` (commit after e9bdb6e). Slot numbers are positions in our DMA buffers with transmit started first and receive 125 µs later; after a STUB restart while the DMA runs, the receive slots can come out rotated (seen once by 4 slots), so a daemon must find the slot rather than hard-code it.

| Item | Observation |
|---|---|
| Control slot | **slot 1 in both directions**; a 16-bit slot carries **2 bytes per frame**, in DMA-buffer order (low byte of the stored word first); 16 kbyte/s each way |
| Idle | host → STUB: `0xFFFF`; STUB → host: `0xFFFF` |
| Who speaks first | **the host** [CR A-0003]: the STUB sends nothing until it gets a contact mail; with idle input it stays silent indefinitely |
| STUB → host framing | exactly as below: `0xAA`, sequence, LE length L, L + 2 body bytes, LE 16-bit sum over length and body. Sequence numbers increment per mail and survive our close/reopen |
| Mail layout (both directions) | `L` (2 bytes LE) = number of bytes after the two task bytes; **source task**, **destination task**, **primitive** (LE, at mail offset 4), parameters |
| Host → STUB element | as below: word count `(n + 5) / 2` (LE), sequence (LE), the n mail bytes, LE 16-bit sum of the mail bytes, 0xFF pad to even length. **No 0xAA header** on transmit (answers the open question below) |
| Contact | host sends source `0x22` (PCM task), destination `0x02`, primitive **`0xF600`**, no parameters (L = 2). The STUB answers within about 14 ms with **`0xF601`** (source 2 → user-interface task `0x14`, 5 parameter bytes) and **`0xF602`** (23 parameter bytes). Both parameter blocks look like box identity/capability data (5 bytes = RFPI size) and are **not recorded here** |
| Other contact variants | with the task bytes `0x02 0x22` (or `0x00 0x00`, `0x02 0x00`) the STUB echoes the mail back as `0xF600` with the same two task bytes |
| First mail after the first contact | primitive `0xFFF2`, source `0x28`, destination `0x22`: an ASCII build stamp of the STUB ("RAM: …" with a date) |
| Periodic status | after contact, about every 10-30 s: **`0xFFF1`** sub-command 4 (source `0x23` PCM task, remote side → `0x22`, 27 bytes) followed by **`0xF618`** (source `0x23` → `0x14`, parameters `37 01`); also once `0xFFF1` sub-command `0x0C` from source `0x27` |
| Slot 4 | **not** the mail slot: a per-frame loop-back. A host word with bit 15 set comes back unchanged; otherwise the STUB sends `0x4008`. Probably AVM's control-slot synchronisation check [CR A-0003] |

A mail element sent in slot 4, or in slots 0, 2, 3, 5, 6, 7, gets no answer; slot 1 does.

## Transport

- The DECT chip (SC144xx running AVM's "STUB" firmware) and the host exchange control "mails" as a **byte stream inside the PCM/TDM bus**. the vendor's PCM driver hands the control-slot bytes of each frame to the vendor host driver's PCM part (the RX/TX handler is called per frame with an RX buffer, a TX buffer and a length).
- When there is nothing to send, TX bytes are **0xFF**. The receive buffer is also pre-filled with 0xFF.
- Multi-byte fields are **little-endian**: the STUB's CPU (CR16C) is LE, and the host converts selected fields on big-endian hosts such as the 7362 SL.

## STUB → host framing (receive side)

| Offset | Size | Meaning |
|---|---|---|
| 0 | 1 | Start marker **0xAA**. The parser discards bytes until it sees 0xAA |
| 1 | 1 | **Sequence number**. The host expects it to increment by 1 per mail and counts mismatches as lost mails (the first mail after a resync is accepted) |
| 2 | 2 | **Length L** (LE), at most 0xFB. Larger values are a framing error and trigger a resync |
| 4 | L + 2 | Mail body |
| 4 + L + 2 | 2 | **Checksum** (LE): the 16-bit sum of all bytes from the length field to the end of the body (L + 4 bytes) |

- On a checksum error the host increments an error counter and restarts the hunt for 0xAA.
- Mails shorter than 10 bytes in total are rejected.
- Valid mails are queued for the stack thread. The queued entry is the length field plus the body.

## Host → STUB framing (transmit side)

The host queues one element per mail, at most 0xFC bytes of mail:

| Offset | Size | Meaning |
|---|---|---|
| 0 | 2 | (mail length + 5) / 2. Looks like a 16-bit-word count |
| 2 | 2 | **TX sequence counter**, incremented per mail |
| 4 | n | Mail bytes. The 16-bit primitive ID sits at mail offset 4; see below |
| 4 + n | 2 | **Checksum** (LE): the 16-bit sum of the n mail bytes |

- The element is padded to an even length and copied into the TX slot bytes of successive frames. Unused bytes are 0xFF.
- **Open question:** whether the vendor's PCM driver adds its own 0xAA/sequence framing on top of this element on the way out, so that both directions look alike on the wire. To be checked in the vendor PCM driver's DECT control path.

## Mail layout (partial)

- The 16-bit **primitive ID** sits at **mail offset 4** (LE).
- The task IDs from the 7520 analysis (e.g. MAC interrupt task = 0x26, PCM task = 0x22) probably fill the bytes before it. To be confirmed on the wire.
- Some primitives get extra field conversion on BE hosts, so their parameter layout is partly known:
  - **0xF626:** a 16-bit field at offset 9.
  - **0xFFF1:** a sub-command byte at offset 6. Sub-commands 7, 9 and 13 carry a 32-bit field at offset 7, and 7 and 13 also a 16-bit field at offset 11.
  - **0x0158:** a 16-bit field at offset 7, a 32-bit field at offset 9 and a 16-bit field at offset 0x38.

## Second checksum type (NWK/DLC level, not the PCM frame)

AVM's mail checksum is a two-byte, Fletcher-style trailer:
- Running sums are kept with end-around carry (an 8-bit add where a carry or a result of 0xFF adds 1).
- The two trailer bytes are chosen so that the block verifies to zero.

This matches the DECT DLC/LAPC checksum style. Its exact use is to be mapped.

## TDM link parameters (IPQ4019)

- the vendor's PCM driver configures the ADSS TDM through the kernel's `tdm_if_config_init(bits, ?, ?, master_flag)` and `tdm_if_dma_init(…)`. The word width is 8 or 16 bits, and the slot count is derived from the link configuration.
- **Clock mastership** is a parameter, so either side can drive the clock. the vendor loader also measures FSC and DCL in software and prints "TDM: FS … Hz CLK … Hz".
- **Pins:** the TDM pins are configured from AVM's board GPIO table for the PCM link (see the 7520 analysis).

## Observed on hardware (2026-10-17)

The SoC receives the chip's TDM frames (8 slots × 16 bit, see `pcm-tdm-ipq4019.md` §13 for the byte table). A running STUB sends **no 0xAA-framed mail on its own**, also not in the first 10 s after an upload: the frame holds one A-law voice channel and constant bytes, the 0xFF positions (bytes 2-3 and 12-15) are candidates for the idle control slot. Next: transmit (host speaks first?) and the answer to clean-room question Q-0001.

## Control slot found by probing (observed on hardware, 2026-10-17)

Source: *observed on hardware*, spare 7520, STUB of FRITZ!OS 08.25 running, Phase B module with transmit (`dect-pcmcap -T -P`). Slot numbers are positions in our DMA buffers (transmit started first, receive 125 µs later, as AVM does); the wire slot number is unknown. Words are 16-bit as the ADSS stores them (little-endian in the buffer).

- **Receive slot 4 carries `0x4008`** while the host sends idle 0xFFFF everywhere. The other receive slots are constant (`0xFEFE` ×3, `0xFFFF` ×3) except one A-law voice slot.
- **Receive slot 4 answers transmit slot 4:** when the host sends a constant word with **bit 15 set** (and not `0xFFFF`) in transmit slot 4, the STUB returns **exactly that word** in receive slot 4 in every frame. Tested: `0x8000`, `0x8080`, `0xFF80`, `0xAA00`, `0xFE00`, `0xAA55`, `0xAAAA`, `0xFEFE`, `0xC3C3`, `0xFFAA`, `0xFFFE`, `0xAAFF`, `0xFEFF` (all echoed). With bit 15 clear (`0x0000`, `0x0080`, `0x00AA`, `0x00FE`, `0x55AA`, `0x7F7F`, `0x1234`, `0x5555`, `0x0201`) or idle `0xFFFF`, it keeps sending `0x4008`.
- The same words in any other transmit slot leave receive slot 4 unchanged; they only show up as noise in the voice slot.
- **Per-frame echo:** a ramp `0x8000, 0x8001, …` written frame by frame into transmit slot 4 (3000 frames, `write()` on `/dev/dect0`) comes back in receive slot 4 complete and in order, one word per frame, then receive slot 4 returns to `0x4008`. So the control slot carries **one 16-bit word per frame in each direction** (2 bytes per 125 µs = 16 kbyte/s). Whether the STUB or the chip's PCM hardware does the echo is unknown.
- **Conclusion (assumed):** slot 4 is the control-slot pair, carrying 16-bit control words with a flag in bit 15. The STUB's echo may be part of the slot synchronisation that the vendor's PCM driver performs ([CR A-0001]: it checks the control-slot value and resyncs on "strange" values). Asked in Q-0003.

## Control-slot facts from the clean room [CR A-0001] (partial answer)

*Analysed* from FRITZ!OS 08.25 (7520): AVM's GPL `ipq-avm.c` API comments and the symbol and string tables of the vendor's PCM driver.
- **16 bits per slot**: the vendor's PCM driver supports no other slot size.
- **The control slot is found by value, not by a fixed index.** the vendor's PCM driver has a control-slot synchronisation: it checks the value in the control slot, resynchronises on "strange" values, and counts sync losses and the time until lock. A module parameter mentions a "slot-shift" workaround.
- The DMA receive and transmit callbacks can **shrink the next descriptor** by a number of bytes. So AVM probably shifts the DMA buffer start until the control slot sits where it expects it. Our buffer start is therefore an arbitrary rotation of the slot order (*assumed*). Our search covers this: the whole-frame stream is tried in every byte and bit variant.
- The control slot is reserved; voice slots form a range whose start depends on the link type.
- Control path: a dedicated DECT-control unit inside the vendor's PCM driver exports a handler-registration entry point, and the vendor's host driver registers its handler there. the vendor's PCM driver also has a "DECT transparent" path with a sine test tone.
- **Still unknown:** who speaks first, the slot number and byte count, the `tdm_if_config_init`/`tdm_if_dma_init` argument values (rxdelay, fs_per_dma, desc_num), and whether transmit adds a 0xAA header. The remaining answers depend on the control flow of the vendor's PCM driver and the vendor's host driver, which are proprietary.

## The stock host's lane map and alignment handshake [CR A-0030]

All `[analysed]` from the stock PCM router and TDM driver unless
marked; the channel count is `[observed]` in a stock 7520 boot log.

**Logical word map** (eight 16-bit words per 8 kHz frame; raw DMA index =
logical index XOR 1, applied identically on receive and transmit):

| logical word | raw DMA word | carries | rate |
|---|---|---|---|
| 0 | 1 | DECT control channel (the mails above) | 2 octets **every frame**, 16 000 octets/s |
| 1 | 0 | DECT voice bearer A | 2 octets per **two** frames (same word twice), 8000 octets/s |
| 2 | 3 | DECT voice bearer B | as A |
| 3 | 2 | DECT voice bearer C | as A |
| 4 | 5 | analog line (SLIC) audio | |
| 5 | 4 | host ↔ chip control/status word; alignment handshake | |

This matches what we found by probing: the control slot at raw word 1 both
ways, the first call's bearer at raw word 0. The control channel and the
voice bearers run at different rates; do not reason from one to the other.
Raw word 4, our long-unexplained "slot-4 word `0x4008`", is the chip's side
of the status word `[assumed]`.

**Idle fill.** Voice words are painted `0xFEFE` in every frame whenever a slot
is closed or muted; the control client fills its own buffer with `0xFF` once
on its first service callback. The host treats a voice word of `0x0000` or
`0xFFFF` as "not a live DECT lane". Our transmit filler is `0xFF`
(`txidle`/`voiceidle`); `voiceidle=0xfe` matches the stock fill.

**Alignment handshake, before any media.** The host paints every word of
every frame with `0xF0F0 | (((5 − i) mod 8) replicated into both bytes)` — word
0 `0xF5F5`, 1 `0xF4F4`, 2 `0xF3F3`, 3 `0xF2F2`, 4 `0xF1F1`, 5 `0xF0F0`, 6
`0xF7F7`, 7 `0xF6F6` — and steps logical word 5 through `0x8081`, `0x8B8C`,
`0x8D8E`; the link counts as synchronised when the chip answers `0x8081` in
its word 5. The chip reports where it saw the ramp by echoing `0xF0F0` with
the discovered word offset in the low nibble (two degenerate forms report
fixed offsets). While scanning, a frame is a candidate only if word 0 is not
`0xFEFE` and the three voice words are neither `0x0000` nor `0xFFFF`. Sync is
retried with a bounded counter; repeated loss reinitialises the PCM bus.
This resolves the **word offset** only; it says nothing about octet order or
pairing phase. We never ran it: our slot search found the control slot by
value and the chip accepted that.

**Service interface above the router.** One callback per 4 ms DMA period
with, per bearer, a 32-octet receive and a 32-octet transmit buffer (64 for
the control channel), plus a first-call flag; no frame counter, no
consumption phase, nothing that distinguishes the downlink boundary from the
uplink one. The transparent (coded G.722 in and out) mode may hold several
bearers; the router's own encode mode is limited to one bearer and encodes
the downlink itself while routing the uplink into the codec/echo chain.

A second, denser map (five bearers, two to a word, one octet per frame,
parity absolute, six channels reported) exists in the driver for other
hardware. The 7520 reports four channels.

## Next steps

1. **TX path in the vendor's PCM driver:** resolve the internal constants (`DAT_…`) to get the exact slot position and size of the control channel, and whether a TX header is added.
2. **Mail OS:** decode the mail send and delivery routines and the per-task dispatch to get the full mail header (source and destination task, primitive, length).
3. **Primitive IDs:** build the primitive-ID table from the per-primitive parser dispatch and the ID→name tables.
4. **Bus configuration:** read the concrete `tdm_if_config_init` arguments for the 7520 from the platform table in the vendor's PCM driver (bits, slots, master/slave, frame rate).
