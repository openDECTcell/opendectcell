# Roadmap

**First target:** FRITZ!Box 7520 (identical hardware to the 7530; OpenWrt image `avm_fritzbox-7530`). Decided 2026-10-17 because a 7520 is available and the 7412 is not. The 7412 and 7362 SL follow with the same stack.

- Each milestone (`## M…`) maps to a GitHub milestone, and each checkbox to an issue. Items marked **[hw]** need a real box.
- **Gates G1–G7 are go/no-go points on real hardware.** Work that depends on a gate starts only after the gate has passed (see `AGENTS.md` §6). The controller (separate repo `opendectcell-controller`) is designed in parallel but **implemented only after G7**.

| Gate | Proves | Pass criterion |
|---|---|---|
| G1 | STUB boots | Both upload stages are acknowledged, and the PCM clocks (DCL, FSC) start |
| G2 | PCM link alive | Captured frames contain 0xAA-framed STUB mails with valid checksums |
| G3 | Control loop | A request mail (version/status/capabilities) gets the matching answer |
| G4 | Audio path | The STUB test tone (channel 15) is captured in a voice slot on the host; loopback works |
| G5 | Base on air | A GAP handset in registration/search mode sees the base (RFPI) |
| G6 | Handset registers | Minimal DLC + MM via `dect-probe`: the handset shows "registered" |
| G7 | Handset rings and talks | Minimal CC via `dect-probe`: the handset rings, you answer, and audio passes (tone or echo) |

## M0: Analysis and tooling (mostly done)
- [x] Extract and document the DECT components of the 7412, 7362 SL and 7520 images
- [x] Reader for AVM's Lantiq NAND filesystem image (`tools/avm_nand_sqfs.py`)
- [x] Headless the analysis tooling pipeline for local analysis
- [x] Control-channel framing (0xAA marker, sequence, length, 16-bit sum)
- [x] 7412 SSC/SPI-TDM register setup, STUB patch mechanism (TLV, tag 1 = RFPI, tag 7 = mode byte)
- [x] Stock-firmware observation on a 7520 via the FRITZ!OS support data: PCM timing, STUB patching, channel count, chip SC14448 (`docs/models/7520-DECT-analysis.md` §8)
- [x] Hardware abstraction for several models: kernel board table plus per-model board profiles (`docs/hardware-abstraction.md`)
- [ ] Complete mail header layout (source/destination task, primitive, length)
- [ ] Primitive-ID catalogue host↔STUB (`docs/stub-api.md`)
- [ ] STUB (CR16C) analysis: meaning of mode byte tag 7; channel configuration
- [x] Minimal mail sequence for MAC start (G5): `0x015B:01` + `0x022B:41` [CR COMMAND_SET]. Registration/call-setup sequences (G6/G7) still to establish from COMMAND_SET §6.2-§6.4 + ETSI

## M1: Lab setup on the 7520 **[hw]**
- [x] Full NAND backup of a box still running AVM firmware: RAM-boot OpenWrt (eva_ramboot + initramfs, no flash writes) and dump every MTD partition (raw incl. OOB, plus plain) via SCP into `work/` (never committed)
- [x] OpenWrt on the 7520 (no serial console); document the version and the boot log
- [x] Locate the factory DECT config block (TLVs incl. RFPI and calibration) in flash; write an extractor (7520: boot-loader partition offset 0x3D000, read by `dect-fwload --factory-mtd`)
- [ ] Console strategy (the DECT UART is the console UART): free the UART after boot via package config
- [ ] Optional: stock-firmware observation with Freetz-NG (`/proc` DECT commands, mail logging) to record reference traces

## M2: Firmware loader → **Gate G1**
- [ ] `tools/dect-fw-extract.py` (PC, Python stdlib only): extract `dectfw_*.hex` from a user-supplied AVM image, verify SHA-256, print upload instructions
- [x] Image builder: Intel HEX → length header, segments, patch area via descriptor (chip address 0 for `441`), TLV patching, XOR checksum (`src/fwload/fwimage.c`, `docs/stub-image-format.md`)
- [x] `kmod-opendectcell-ipq40xx` skeleton: board table (match by DT compatible), `/dev/dect0` with chip reset and boot strap; no PCM yet (`kernel/`, verified 2026-10-17: 50 consecutive uploads, clean unload)
- [ ] Board profile for the 7520/7530 (`boards/<board_name>`, name read on the box) with the loader keys (firmware names, mode word, UART)
- [x] `dect-fwload` bring-up version: UART boot, both stages, retries; reset and strap through sysfs GPIO given on the command line (`src/fwload/`, `docs/stub-loader-protocol.md` §7)
- [x] `dect-fwload` on `/dev/dect0` instead of command-line GPIOs (the board profile is a separate item)
- [ ] Unit tests for the upload engine (`loader.c`) against the real `.hex` files; the image builder already has them
- [x] **[hw]** Gate G1: STUB boots, with the PCM clocks present
  - 2026-10-17, 7520: both upload stages acknowledged with matching checksums; after the upload the chip drives DCL and FSC (`docs/models/7520-DECT-analysis.md` §8.2). The STUB prints nothing on the UART

## M3: PCM kernel module → **Gate G2**
- [x] IPQ4019 ADSS TDM streaming (port from Qualcomm's GPL QSDK driver): FS 8 kHz, DCL 1.024 MHz, mbox DMA
  - 2026-10-17: written from `docs/pcm-tdm-ipq4019.md` (not ported); receive and transmit DMA, 10 minutes without gap or overrun
- [ ] Real-time path (dedicated CPU core or high-priority IRQ/FIQ)
  - so far a plain interrupt at 250/s is enough; revisit with voice
- [x] `/dev/dect0` interface: control-slot and voice-slot data, frame-layout configuration; identical for every transport
  - raw frames: `read()` blocks with header, `write()` frame queue, step-wise bring-up ioctls; slot demultiplexing stays in userspace for now
- [x] Capture tool (a "software logic analyser"): dump frames and search for the 0xAA framing
  - `src/tools/dect-pcmcap.c` (also sends a mail with `-m` and decodes the answers)
- [x] **[hw]** Determine words per frame, control-slot position, voice-slot map and clock master
  - 8 × 16-bit slots, chip is clock master, control slot 1 (2 bytes per frame), slot 4 loop-back, one A-law voice slot seen; full voice map in M4
- [x] **[hw]** Gate G2: valid STUB mails captured
  - 2026-10-17, 7520: after the host's contact mail (`0xF600`) the STUB answers and sends periodic status mails, all with valid checksums and consecutive sequence numbers (`docs/stub-pcm-control-channel.md`)

## M4: Control channel → **Gates G3, G4**
- [x] `dect-stubd`: framing, checksum, sequence, idle pattern, resync; big-endian host conversion
  - 2026-10-17: `src/libopendectcell/` (framing codec + session state machine, PC unit tests) and `src/stubd/dect-stubd.c` (slot probe + decode/session loop). Endian-clean (byte-wise LE)
- [~] STUB status/liveness and version/capability requests
  - liveness done (0xFFF1 tracked, re-contact on silence); explicit version/capability request mails still to add
- [x] **[hw]** Gate G3: request/answer loop works
  - 2026-10-17, 7520: `dect-stubd` found the control slot (tx 1, rx 1), one 0xF600 contact got 0xF601 + 0xF602, then periodic 0xFFF1/0xF618; 14 mails in 30 s with 0 seq gaps / csum / framing errors / resyncs; session stayed UP (`docs/stub-pcm-control-channel.md`)
- [~] Audio test: test tone and loopback commands; capture of voice slots
  - 2026-10-17: voice slots captured and the sample format found (8-bit A-law in the slot's low byte); the GenDSP connect/loop mails from [CR A-0004] work. **Loop-back works on a contact-only bench** (dect_loop channel 1/2/3 = buffer slot 0/3/2): a transmitted A-law tone returns in the mapped receive slot, frequency tracking the transmit. The STUB's own **test tone (channel 15) stays silent without the MAC** — it routes to a slot but generates nothing (A-0005; likely needs MAC/DSP active)
- [ ] **[hw]** Gate G4: test tone received in a voice slot
  - loop-back half proven 2026-10-17 (`docs/stub-pcm-control-channel.md`); the literal STUB test tone needs minimal MAC bring-up (M5) or a trigger not yet known

## M5: MAC bring-up → **Gate G5**  ✅ PASSED 2026-10-18
- [x] Start/stop of the FP MAC with RFPI; beacon; paging
  - **Real sequence (verified):** `0x015B:01` (radio/beacon on) then `0x022B:41` (`'A'` = open registration, sets capability a44); park with `0x022C:01` + `0x015B:00`. See `docs/mac-bringup.md` top section [CR COMMAND_SET §6.1/§6.2]. The STUB picks the carrier itself and emits identity from its patched-in factory RFPI; no `0x015C` cell config is involved (the earlier A-0006 "group A/B" reading was wrong). Driven today with `dect-stubd -M`.
- [ ] `dect-probe` skeleton on top of `libopendectcell` (transport and timers injected)
- [x] **[hw]** Gate G5: a handset sees the base — FRITZ!Fon C4 listed the base ("Basis 3") and attempted registration

## M6: Minimal registration → **Gate G6**
All host-side modules below are built and PC-unit-tested (golden vectors / loopback). Sources are PUBLIC (ETSI EN 300 175-4/-5/-7, EN 300 444, E. Tews DSAA thesis, osmocom libdect as cross-check only — no code copied; GPL-2.0-or-later so the AGPL controller can link libopendectcell). The STUB runs only radio/MAC; DLC/NWK/MM/auth all run on the host [CR COMMAND_SET line 68].
- [x] `dect_cs` STUB-facing C-plane transport — inbound `0x0203`, outbound `0x0244`+`0x0202` [CR COMMAND_SET §6.4/§8.2]
- [x] `dect_lc` Lc layer — Cs reassembly/segmentation + ISO 8073 checksum (Annex B vectors); LSIG=0 during registration
- [x] `dect_lapc` LAPC class-A acknowledged link — I/RR, mod-2/window-1, segmentation + reassembly (loopback-tested); the ACK the handset waited for at G5
- [x] `dect_nwk` S-format header + variable-length IE codec
- [x] `dect_dsaa` DSAA + A11/A12/A21/A22 + B1/B2 + PIN→AC — verified against ETSI EN 300 175-7 Annex K
- [x] `dect_mm` MM registration FSM — access rights + authentication (correct/​wrong PIN tested); FIXED-IDENTITY/PARK from RFPI is the hardware-iteration point
- [x] `dect-stubd -g` wires the stack over the STUB (auto bring-up, RFPI capture, C-plane routing, radio parked on exit); host + ARM builds clean
- [~] **[hw]** Gate G6: a GAP handset registers — **DLC proven on hardware** (FRITZ!Fon C4): the link establishes, the C4 sends its full ACCESS-RIGHTS-REQUEST and acks our replies. **Open:** the MM exchange doesn't complete (no PIN prompt) — see `docs/registration-g6.md` "Hardware progress". Key hardware fix: never send `0x0202` after a C-plane reply (it is AVM's abnormal-release primitive).

## M7: Minimal call → **Gate G7**
- [ ] `libopendectcell`: CC setup/alert/connect/release, connection-to-voice-slot binding
- [ ] `dect-probe`: ring a handset, play a tone, echo mode
- [x] **[hw]** **Gate G7 PASSED (2026-10-19)**: the handset rings, it is answered, and audio passes
  - [x] **Audio passes, both directions** (v22): the chip delivers the call's coded G.722 bearer on the TDM slot and accepts it back; echoing it is audible in the handset. See `docs/call-g7.md`
  - [x] **Handset rings** (v27): `-a ring=MS` pages (`0x0300`) and sends an FP-originated CC-SETUP; the handset rang, was answered, and the call bound audio on the same path
  - [x] G.722 codec on the host, so real audio can be sourced and sunk rather than echoed or replayed (not a gate item). `src/libopendectcell/dect_g722.c`, written from ITU-T G.722 and validated against ffmpeg as a black box (`tools/g722-crosscheck.sh`: all four encoder/decoder combinations within 0.05 dB at ~28.6 dB SNR). Sourcing is wired as `-a playpcm=FILE`; sinking is covered offline by `tools/dect_g722_extract.py` plus any G.722 decoder. **[hw]** Neither has been run on the box yet
- [ ] After G7: clean-room review of all docs, then make openDECTcell and opendectcell-controller public
  - **Rule decided 2026-10-20:** strip **AVM-authored module symbol and source-file names**; keep public SDK/API/primitive names, ETSI names, `/proc` and module-parameter names, filesystem paths of modules, and names that come from AVM's published GPL source (AGENTS.md §2.6 allows naming that file). Rationale: a symbol only readable from a proprietary binary's symbol table carries no fact we need — the role description carries the fact — while an interface name observable on a running box is exactly the "primitive names and IDs" AGENTS.md §2.2 permits. Reversible in one commit if the stricter variant (strip everything non-ETSI) is preferred.
  - Applied to the AVM-authored class: 10 edits in `docs/models/7520-DECT-analysis.md`, 7 in `docs/stub-pcm-control-channel.md`, 1 each in `docs/mac-bringup.md` and `docs/stub-loader-protocol.md`. Nothing in `docs/call-g7.md` carried any.
  - **Still open — the 2026-10-19 scan undercounted.** It found nine names in two files; a repo-wide sweep finds symbol-shaped names in six. The remainder are **not** AVM-authored and so fall outside the rule above, but still need one deliberate call: (a) RTX/Dialog mail-OS and FP-stack internals (`docs/models/7520-DECT-analysis.md` §mail OS, `docs/mac-bringup.md`, `docs/capacity.md`, `docs/models/7362SL-DECT-analysis.md`) — third-party SDK, not AVM's, but equally only readable from a binary; (b) AVM GPIO/clock line names in `docs/7412-spi-tdm-and-loader.md` and `docs/models/7412-DECT-analysis.md`, which plausibly come from the device tree in AVM's GPL kernel source and would then be explicitly allowed. Stripping (a) would cost real traceability in the MAC and capacity notes, so decide before publishing rather than now.

## M8: Full GAP / CAT-iq basics in libopendectcell
- [ ] Locate, temporary identities, ciphering (DSC), outgoing calls
- [~] CLIP/CNIP: `src/libopendectcell/dect_clip.c` builds CALLING-PARTY-NUMBER (`0x6C`) and CALLING-PARTY-NAME (`0x6D`) from the public ETSI text, wired into the FP-originated CC-SETUP and into `dect-stubd -a clip=/cnip=`. **[hw]** No handset has been shown either IE. Privacy semantics (what to send for a withheld caller) stay deliberately unimplemented: we omit, because the one stock value we have for the restricted case is ambiguous Since 2026-10-20 the SETUP can also carry `<<CALL-INFORMATION>>` (`src/libopendectcell/dect_callinfo.c`, `-a lineid=`): CAT-iq 2.0 marks an incoming external call by its line identifier, the leading explanation for the C4's "Intern" display (test card #73).
- [ ] Wideband (G.722) codec negotiation
- [ ] Multiple handsets and parallel calls (STUB limit: about 3 PCM channels)
- [ ] Golden-trace regression tests in CI

## M9: OpenWrt integration
- [ ] Feed with `kmod-opendectcell-ipq40xx` and the `opendectcell` package (programs plus board profiles; pulls in the right kmod); CI build with the OpenWrt SDK
- [ ] Board resources: in-module board table first, then an upstream DTS patch for the 7520/7530
- [ ] procd init script: board detection, profile plus UCI overrides, clear message on unsupported boxes; console/getty handling
- [ ] Firmware storage in `/etc/opendectcell/firmware/`: SHA-256 check against the profile, kept across upgrades (`keep.d`)
- [ ] LuCI app for non-technical users: status page, firmware upload, basic settings
- [ ] Cell mode: tunnel mails (TCP, optional TLS) and audio (UDP, optional SRTP/DTLS) to the controller

## M10: Controller (repo `opendectcell-controller`; implementation starts after G7)
- [x] Architecture, VoIP requirements and DECT feature framework (design docs in opendectcell-controller)
- [ ] Implementation per the controller repo's own roadmap

## M11: Multi-cell
- [ ] Research: DECT identities for a multi-cell system (ARI classes, allocation of EMC/EIC/FPN, uniqueness, which classes GAP/CAT-iq handsets accept); define how the controller assigns the system ARI and per-cell RPNs on first run
- [ ] Idle roaming across cells with a shared registration database (controller)
- [ ] Research: inter-cell synchronisation (host-driven FSC on the 7412? STUB sync modes?) for seamless handover
- [ ] Capacity per cell (about 3 calls); verify against the mode byte

## Later
- [ ] Port to the 7412 (VR9 SSC in SPI-slave mode, `kmod-opendectcell-lantiq`) and the 7362 SL (VR9 TDM)
- [ ] 7412 PCB check: what hangs off GPIO10/12 ("PCM-link flash" lines)
- [ ] ULE, SUOTA: out of scope for now
