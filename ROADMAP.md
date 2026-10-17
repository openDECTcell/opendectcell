# Roadmap

**First target:** FRITZ!Box 7412. The 7362 SL and 7520/7530 follow with the same stack.

- Each milestone (`## M…`) maps to a GitHub milestone, and each checkbox to an issue. Items marked **[hw]** need a real box.
- **Gates G1–G7 are go/no-go points on real hardware.** Work that depends on a gate starts only after the gate has passed (see `AGENTS.md` §6). The controller (separate repo `opendectcell-controller`) is designed in parallel but **implemented only after G7**.

| Gate | Proves | Pass criterion |
|---|---|---|
| G1 | STUB boots | Both upload stages are acknowledged, and the STUB's own UART output appears |
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
- [x] Hardware abstraction for several models: kernel board table plus per-model board profiles (`docs/hardware-abstraction.md`)
- [ ] Complete mail header layout (source/destination task, primitive, length)
- [ ] Primitive-ID catalogue host↔STUB (`docs/stub-api.md`)
- [ ] STUB (CR16C) analysis: meaning of mode byte tag 7; channel configuration
- [ ] Minimal mail sequences for MAC start, registration and call setup, as used by the stock stack (input for G5–G7)

## M1: Lab setup on the 7412 **[hw]**
- [ ] Full NAND backup of a box still running AVM firmware: RAM-boot OpenWrt (eva_ramboot + initramfs, no flash writes) and dump every MTD partition (raw incl. OOB, plus plain) via SCP into `work/` (never committed)
- [ ] OpenWrt on the 7412 with serial console access; document the boot log
- [ ] PCB check: DECT chip marking, what hangs off GPIO10/12 ("PCM-link flash" lines)
- [ ] Locate the factory DECT config block (TLVs incl. RFPI and calibration) in flash; write an extractor
- [ ] Console strategy (the DECT UART is the console UART): free the UART after boot via package config
- [ ] Optional: stock-firmware observation with Freetz-NG (`/proc` DECT commands, mail logging) to record reference traces

## M2: Firmware loader → **Gate G1**
- [ ] `tools/dect-fw-extract.py` (PC, Python stdlib only): extract `dectfw_*.hex` from a user-supplied AVM image, verify SHA-256, print upload instructions
- [ ] Image builder: Intel HEX → length header, patch area via descriptor at chip address 6, TLV patching, XOR checksum
- [ ] `kmod-opendectcell-lantiq` skeleton: board table (match by DT compatible), `/dev/dect0` with chip reset; no PCM yet
- [ ] Board profile `boards/avm,fritz7412` with the loader keys (firmware names, mode word, UART)
- [ ] `dect-fwload`: UART boot, ROM stage (STX/SOH/len16/XOR/ACK) and second stage (len32/CRC16), retries; chip reset via `/dev/dect0`
- [ ] Chip simulator plus unit tests against the real `.hex` files
- [ ] **[hw]** Gate G1: STUB boots, with its UART output captured

## M3: SPI-TDM kernel module → **Gate G2**
- [ ] SSC slave-mode streaming: 32-bit words, MSB first, clock polarity 1; RX/TX FIFOs, then DMA
- [ ] Real-time path (dedicated VPE or high-priority IRQ); frame sync on GPIO9, clock enable GPIO7
- [ ] `/dev/dect0` interface: control-slot and voice-slot data, frame-layout configuration; identical for every transport
- [ ] Capture tool (a "software logic analyser"): dump frames and search for the 0xAA framing
- [ ] **[hw]** Determine words per frame, control-slot position, voice-slot map and clock master
- [ ] **[hw]** Gate G2: valid STUB mails captured

## M4: Control channel → **Gates G3, G4**
- [ ] `dect-stubd`: framing, checksum, sequence, idle pattern, resync; big-endian host conversion
- [ ] STUB status/liveness and version/capability requests
- [ ] **[hw]** Gate G3: request/answer loop works
- [ ] Audio test: test tone and loopback commands; capture of voice slots
- [ ] **[hw]** Gate G4: test tone received in a voice slot

## M5: MAC bring-up → **Gate G5**
- [ ] Start/stop of the FP MAC with RFPI; beacon; paging
- [ ] `dect-probe` skeleton on top of `libopendectcell` (transport and timers injected)
- [ ] **[hw]** Gate G5: a handset sees the base

## M6: Minimal registration → **Gate G6**
- [ ] `libopendectcell`: LAPC/Lc DLC minimum (ETSI EN 300 175-4)
- [ ] `libopendectcell`: MM access rights with PIN, identity, authentication (DSAA), key storage
- [ ] **[hw]** Gate G6: a GAP handset registers

## M7: Minimal call → **Gate G7**
- [ ] `libopendectcell`: CC setup/alert/connect/release, connection-to-voice-slot binding
- [ ] `dect-probe`: ring a handset, play a tone, echo mode
- [ ] **[hw]** Gate G7: a handset rings and audio passes
- [ ] After G7: clean-room review of all docs, then make openDECTcell and opendectcell-controller public

## M8: Full GAP / CAT-iq basics in libopendectcell
- [ ] Locate, temporary identities, ciphering (DSC), outgoing calls, CLIP
- [ ] Wideband (G.722) codec negotiation
- [ ] Multiple handsets and parallel calls (STUB limit: about 3 PCM channels)
- [ ] Golden-trace regression tests in CI

## M9: OpenWrt integration
- [ ] Feed with `kmod-opendectcell-lantiq` and the `opendectcell` package (programs plus board profiles; pulls in the right kmod); CI build with the OpenWrt SDK
- [ ] Board resources: in-module board table first, then an upstream DTS patch for the 7412
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
- [ ] Port to the 7362 SL (VR9 TDM) and the 7520/7530 (IPQ40xx ADSS TDM)
- [ ] ULE, SUOTA: out of scope for now
