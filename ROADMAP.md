# Roadmap

## Where we are (October 2026)

- **Done:** the DECT base works on a FRITZ!Box 7520. Handsets register,
  ring and talk with HD voice, encrypted; calls go out and come in over SIP
  through baresip; everything is set up in LuCI; signed packages are built
  by GitHub Actions. User-level summary: [docs/status.md](docs/status.md).
- **Next (M9b), built 2026-10-29 and waiting for the bench:** one SIP line
  ringing several handsets, outgoing calls on Line 2, narrowband speech to
  SIP with the codecs matched end to end, G.722 passed through, up to three
  SIP calls at once, call waiting, hold and toggle, DTMF, the voicemail
  indicator ([docs/lines-and-calls.md](docs/lines-and-calls.md); cards in
  [docs/bench-7412.md](docs/bench-7412.md) and
  [docs/bench-next.md](docs/bench-next.md)).
- **Then:** call lists and the phonebook on the handset (M9b, optional),
  the FRITZ!Box 7412 as a release target (section Later), the multi-cell
  controller (M10, M11).

The rest of this file is the detailed plan, milestone by milestone, with the
history of each item. `[x]` done, `[~]` partly done, `[ ]` open.

**First target:** FRITZ!Box 7520 (identical hardware to the 7530; OpenWrt image `avm_fritzbox-7530`). Decided 2026-10-17 because a 7520 is available and the 7412 is not. The 7412 and 7362 SL follow with the same stack.

- Each milestone (`## M…`) maps to a GitHub milestone, and each checkbox to an issue. Items marked **[hw]** need a real box.
- **State and gaps as of 2026-10-25:** `docs/review-2026-10-25.md` (what works, what is built but unproven, what the roadmap still lacks, and the order of work).
- **Order of work (maintainer, 2026-10-25, revised the same evening):** finish the DECT basics (the open bench tests), then the handset abstraction layer (M8b), then **a working standalone OpenWrt package with VoIP (M9b, baresip)** together with productising the cell (M9). The controller freeze is **lifted**: the controller works in parallel. Still open: whether the standalone package includes controller functions (a decision for Wednesday; `docs/review-2026-10-25.md`).
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
- [x] Local analysis pipeline for the AVM firmware (output never committed)
- [x] Control-channel framing (0xAA marker, sequence, length, 16-bit sum)
- [x] 7412 SSC/SPI-TDM register setup, STUB patch mechanism (TLV, tag 1 = RFPI, tag 7 = mode byte)
- [x] Stock-firmware observation on a 7520 via the FRITZ!OS support data: PCM timing, STUB patching, channel count, chip SC14448 (`docs/models/7520-DECT-analysis.md` §8)
- [x] Hardware abstraction for several models: kernel board table plus per-model board profiles (`docs/hardware-abstraction.md`)
- [x] Complete mail header layout (source/destination task, primitive, length)
  - `docs/stub-api.md` §1 (2026-10-25)
- [x] Primitive-ID catalogue host↔STUB (`docs/stub-api.md`)
  - 2026-10-25: written by an outside helper from this repository only (brief: `docs/briefs/stub-api-catalogue.md`), and reviewed. Every claim is labelled measured, answered, inferred or unknown. §5 lists stale statements in our own docs and code comments (the `0x0202` "trigger" helper name, the "`0x017C` never sent" comment, the DLC split in `AGENTS.md` §1a) to fix
- [ ] STUB (CR16C) analysis: meaning of mode byte tag 7; channel configuration
- [x] Minimal mail sequence for MAC start (G5): `0x015B:01` + `0x022B:41` [CR COMMAND_SET]. Registration/call-setup sequences (G6/G7) still to establish from COMMAND_SET §6.2-§6.4 + ETSI

## M1: Lab setup on the 7520 **[hw]**
- [x] Full NAND backup of a box still running AVM firmware: RAM-boot OpenWrt (eva_ramboot + initramfs, no flash writes) and dump every MTD partition (raw incl. OOB, plus plain) via SCP into `work/` (never committed)
- [x] OpenWrt on the 7520 (no serial console); document the version and the boot log
- [x] Locate the factory DECT config block (TLVs incl. RFPI and calibration) in flash; write an extractor (7520: boot-loader partition offset 0x3D000, read by `dect-fwload --factory-mtd`)
- [~] Console strategy (the DECT UART is the console UART): free the UART after boot via package config
  - 2026-10-25: the package's uci-defaults script disables the ttyMSM0 getty in `/etc/inittab`, and the init script freezes a running one (`console_claim`, printk 1). Verified by the service test, but the uci-defaults path has not yet run on a fresh install
- [ ] Optional: stock-firmware observation with Freetz-NG (`/proc` DECT commands, mail logging) to record reference traces

## M2: Firmware loader → **Gate G1**
- [x] `tools/dect-fw-extract.py` (PC, Python stdlib only): extract `dectfw_*.hex` from a user-supplied AVM image, verify SHA-256, print upload instructions
  - 2026-10-25: FRITZ!OS `.image` (tar), bare SquashFS (found by magic, any header before it) or an extracted folder; known builds renamed as the service expects, with the `scp`/`uci` lines to run. Tested on synthetic images (`tests/test_fw_extract.py`, part of `make check`); **not yet run on a real AVM image**, and the Lantiq NAND format of the 7412/7362 SL is not wired in
  - **Deferred bench test** (maintainer, 2026-10-25: a working DECT stack comes first): `python3 tools/dect-fw-extract.py FRITZ.Box_7520-07.56.image` on the real image. Pass: `OK` for a first and a second stage, then the `scp`/`uci` lines. Fail: `No DECT firmware found`, then send what `tar tvf` on the image lists. Was test 6 on `docs/bench-2026-10-25.md`
- [x] Image builder: Intel HEX → length header, segments, patch area via descriptor (chip address 0 for `441`), TLV patching, XOR checksum (`src/fwload/fwimage.c`, `docs/stub-image-format.md`)
- [x] `kmod-opendectcell-ipq40xx` skeleton: board table (match by DT compatible), `/dev/dect0` with chip reset and boot strap; no PCM yet (`kernel/`, verified 2026-10-17: 50 consecutive uploads, clean unload)
- [x] Board profile for the 7520/7530 (`boards/<board_name>`, name read on the box) with the loader keys (firmware names, mode word, UART)
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
  - **2026-10-22, retested with everything the 2017 note was missing and it still does not work.** The MAC is up, a wideband call is bound, and the chip has assigned a channel — all the conditions A-0005 suspected. Four probes: `0xFFF1` sub `0x0A` routing source 15 into wideband 7/8/9, then into 0..3, then **all 22 GenDSP sources swept into the chip-assigned channel** (`-a dspsweep=1:1500`). **Silence every time, and no TDM lane moved at all.** This matches the measurement in `dsp_sweep()`'s own comment from 2026-10-19: into destination 6 with no call, no source produces a varying signal — 19 and 20 pin the lane to `0x7FFF`/`0x8000` and the rest to a constant. **Working conclusion: this STUB build has no usable tone generator**, and G4's literal criterion may be unreachable rather than merely unreached. Logs `work/box-tmp/vTone*.log`, `vSweep.log`; detail in `docs/call-g7.md`.

## M5: MAC bring-up → **Gate G5**  ✅ PASSED 2026-10-18
- [x] Start/stop of the FP MAC with RFPI; beacon; paging
  - **Real sequence (verified):** `0x015B:01` (radio/beacon on) then `0x022B:41` (`'A'` = open registration, sets capability a44); park with `0x022C:01` + `0x015B:00`. See `docs/mac-bringup.md` top section [CR COMMAND_SET §6.1/§6.2]. The STUB picks the carrier itself and emits identity from its patched-in factory RFPI; no `0x015C` cell config is involved (the earlier A-0006 "group A/B" reading was wrong). Driven today with `dect-stubd -M`.
- [ ] `dect-probe` skeleton on top of `libopendectcell` (transport and timers injected)
  - 2026-10-25: **probably superseded**. `dect-stubd` grew into this tool, and the controller links libopendectcell directly. Proposed for closing (review §6)
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
- [x] **[hw]** Gate G6: a GAP handset registers — **DLC proven on hardware** (FRITZ!Fon C4): the link establishes, the C4 sends its full ACCESS-RIGHTS-REQUEST and acks our replies. **Open:** the MM exchange doesn't complete (no PIN prompt) — see `docs/registration-g6.md` "Hardware progress". Key hardware fix: never send `0x0202` after a C-plane reply (it is AVM's abnormal-release primitive).
  - **PASSED 2026-10-18**: the C4 enrols, and the UAK is persisted. Since 2026-10-25, all three Gigasets register; PINs of 4 to 8 digits work (8 and 6 tested on the AS690H, bench test 8); a wrong PIN gets reason `0x10`; and registration is closed unless a timed window is open (API 17, 21, 27)

## M7: Minimal call → **Gate G7**
- [x] `libopendectcell`: CC setup/alert/connect/release, connection-to-voice-slot binding
  - `dect_cc` + `dect_link`; FP- and PP-originated calls, transaction matching, setup collisions (§9.5.2.3), F<CC.03>. 2026-10-25: all three Gigasets ring and are answered on transaction value 0 with `<<SIGNAL>>` 0x41 and a conformant SETUP (no codec list); narrowband audio both ways (mode 2, 16-bit linear PCM), runs 21-34 in `docs/call-g7.md`
- [ ] `dect-probe`: ring a handset, play a tone, echo mode
  - 2026-10-25: done in practice by `dect-stubd` (`-a ring=`, `playpcm=`, echo). **Probably superseded**; proposed for closing (review §6)
- [x] **[hw]** **Gate G7 PASSED (2026-10-19)**: the handset rings, it is answered, and audio passes
  - [x] **Audio passes, both directions** (v22): the chip delivers the call's coded G.722 bearer on the TDM slot and accepts it back; echoing it is audible in the handset. See `docs/call-g7.md`
  - [x] **Handset rings** (v27): `-a ring=MS` pages (`0x0300`) and sends an FP-originated CC-SETUP; the handset rang, was answered, and the call bound audio on the same path
    - 2026-10-24: a **Gigaset** rings too (A540, run 15). Until then only the C4 did: every SETUP carried transaction value 1, and EN 300 444 §6.9.2 requires the lowest free value, 0 (`dd5a630`, `docs/call-g7.md` runs 14-16)
  - [x] G.722 codec on the host, so real audio can be sourced and sunk rather than echoed or replayed (not a gate item). `src/libopendectcell/dect_g722.c`, written from ITU-T G.722 and validated against ffmpeg as a black box (`tools/g722-crosscheck.sh`: all four encoder/decoder combinations within 0.05 dB at ~28.6 dB SNR). Sourcing is wired as `-a playpcm=FILE`; sinking is covered offline by `tools/dect_g722_extract.py` plus any G.722 decoder. **[hw]** Neither has been run on the box yet
- [~] After G7: clean-room review of all docs, then make openDECTcell and opendectcell-controller public
  - 2026-10-28: documentation rewritten for a public audience (README, `docs/status.md`, `docs/README.md`, `CONTRIBUTING.md`); a review for private and clean-room-sensitive content is under way before openDECTcell goes public
  - **Rule decided 2026-10-20:** strip **AVM-authored module symbol and source-file names**; keep public SDK/API/primitive names, ETSI names, `/proc` and module-parameter names, filesystem paths of modules, and names that come from AVM's published GPL source (AGENTS.md §2.6 allows naming that file). Rationale: a symbol only readable from a proprietary binary's symbol table carries no fact we need — the role description carries the fact — while an interface name observable on a running box is exactly the "primitive names and IDs" AGENTS.md §2.2 permits. Reversible in one commit if the stricter variant (strip everything non-ETSI) is preferred.
  - Applied to the AVM-authored class: 10 edits in `docs/models/7520-DECT-analysis.md`, 7 in `docs/stub-pcm-control-channel.md`, 1 each in `docs/mac-bringup.md` and `docs/stub-loader-protocol.md`. Nothing in `docs/call-g7.md` carried any.
  - **Still open — the 2026-10-19 scan undercounted.** It found nine names in two files; a repo-wide sweep finds symbol-shaped names in six. The remainder are **not** AVM-authored and so fall outside the rule above, but still need one deliberate call: (a) RTX/Dialog mail-OS and FP-stack internals (`docs/models/7520-DECT-analysis.md` §mail OS, `docs/mac-bringup.md`, `docs/capacity.md`, `docs/models/7362SL-DECT-analysis.md`) — third-party SDK, not AVM's, but equally only readable from a binary; (b) AVM GPIO/clock line names in `docs/7412-spi-tdm-and-loader.md` and `docs/models/7412-DECT-analysis.md`, which plausibly come from the device tree in AVM's GPL kernel source and would then be explicitly allowed. Stripping (a) would cost real traceability in the MAC and capacity notes, so decide before publishing rather than now.

## M8: Full GAP / CAT-iq basics in libopendectcell
- [x] Locate, temporary identities, ciphering (DSC), outgoing calls
  - locate with identity gate, TPUI assignment with `<MM_ident.1>`, PP-originated calls with keypad collection: done. Routing an outgoing call anywhere is the controller's job
  - 2026-10-28: **ciphering works on air** (A540, STUB 07.56): the chip gets the call key in `0x0208` (stock's layout, A-0126) in plain byte order before CIPHER-REQUEST, and the call holds with clear audio (`docs/bench-next.md` test 4). The service ciphers by default again. Other handsets not yet tried ciphered
- [~] CLIP/CNIP: `src/libopendectcell/dect_clip.c` builds CALLING-PARTY-NUMBER (`0x6C`) and CALLING-PARTY-NAME (`0x6D`) from the public ETSI text, wired into the FP-originated CC-SETUP and into `dect-stubd -a clip=/cnip=`. **[hw]** 2026-10-28: the number shows on the C430A and AS690H, and on the A540 once the full <<CALL-INFORMATION>> comes with it ("via Line 1 <number>", sent by default to Part 3 handsets since `2c76ace`); the name element is shown by no bench handset, the Part 1 Gigasets show a name only as vendor display text (a profile item, `docs/bench-next.md` test 1); internal calls carry the caller's terminal number and handset name since `f69b9cf` (untested). Privacy semantics (what to send for a withheld caller) stay deliberately unimplemented: we omit, because the one stock value we have for the restricted case is ambiguous. Since 2026-10-20 the SETUP can also carry `<<CALL-INFORMATION>>` (`src/libopendectcell/dect_callinfo.c`, `-a lineid=`): CAT-iq 2.0 marks an incoming external call by its line identifier, the leading explanation for the C4's "Intern" display (test card #73).
- [x] Codec negotiation (TS 102 527-1 §7.3.3)
  - 2026-10-25: we choose G.726 in `{CC-CONNECT}` for a handset-placed call that negotiates (API 14), and our SETUP carries no list (basic speech = G.726). **Wideband works** (bench tests 10-12, 2026-10-25, all three Gigasets on 07.56): stock's bind (flags 0x41, codec byte 0, no re-assign) gives the coded G.722 lane; our host encoder phase-locked to the chip's two-frame words carries clean wideband speech. Default since API 22 when the call's codec is G.722; `-a narrowband` falls back. Overlap receiving (API 23) and a handset call in steps (API 25) followed; open: the call timer on handset-placed calls (Q-0060)
  - The call timer: A-0122 says stock's CONNECT carries <<CALL-INFORMATION>> "CS call connect". Ours does too since API 26. **[hw]** Untested; bench test 15
- [~] Multiple handsets and parallel calls (STUB limit: about 3 PCM channels)
  - 2026-10-25: **multiple handsets done** — MM holds every subscribed handset and the link adopts the one it serves (API 12); `dect-stubd -s` repeats, installs every UAK in its chip slot, names everyone in the Internal Names List, and rings the first store's handset (runs 29-34). CC rejects strangers with "unknown identity" (API 11); a foreign page response gets `{LCE-PAGE-REJECT}` (API 13). Since `90588c4` a connection that arrives while the main link is busy gets a **side link** (location registration and list access; verified run 35). **Open: parallel calls** — audio, ringing and learning stay on the one main link
  - 2026-10-25 evening: **parallel and internal calls implemented** behind `-a twocalls` (`docs/two-calls.md`). From A-0124: three chip channels; channel k binds flags bit `1<<(k-1)` and mask `1<<k`, and its audio is on TDM word k XOR 1. A second or third call runs on a side link with its own lane, and internal calls (call class 9) are bridged on the host. Reviewed (13 findings, 12 fixed). **[hw]** Untested; bench tests 18 and 19. Open: transcoding for mixed-codec bridges
- [~] **Diagnostics in the library** (`docs/diagnostics.md`): **step 1 built 2026-10-25 (API 16)** — `dect_diag.[ch]` with the protocol half, the `0xF625` decode moved out of the daemon, model/codec capture and JSON; `dect-stubd` prints a `diag:` line per status poll. Hardware check owed (needs an answered call). Original item: a `struct dect_diag` per link plus a base-wide one, carrying identity, subscription slot and whether the chip really holds it, connected state, manufacturer/model from `<<MODEL-IDENTIFIER>>`, codecs offered and in use, slot type, time slot, carrier, field strength, signal quality and encryption state. The library fills the protocol half and the caller fills the radio half — **no I/O in the library**, which has no transport and must keep none. One struct so the LuCI app and the controller cannot drift apart on what "quality" means. *The protocol half needs no new facts and can be built now; the radio half waits on `Q-0054`.*
  - 2026-10-25 evening: **the radio half is built (API 26)**. dBm from the chip's RSSI, a quality percentage from AVM's thresholds plus the CRC and Q-bit counters (A-0121, A-0123), and the counters renamed to their meaning. Shown on the Monitor page as signal bars, dBm and quality
- [~] Golden-trace regression tests in CI
  - 2026-10-30: `tests/test_trace.c` plays a handset through the host stack (`dect_link`) and compares every NWK message the base sends with `tests/traces/*.trace`, byte for byte; four traces (handset call out, the same with Part 3 call status, base call in and release, location registration), made-up identities, recorded with `GOLDEN_UPDATE=1`. CI runs `make check` on every push (`.github/workflows/check.yml`). Still to add: registration with authentication and key allocation, ciphering, list access, call waiting

## M8b: Handset abstraction layer (new 2026-10-25; after the DECT basics, before M9)
- **The maintainer's model** (2026-10-25): layered profiles. A **manufacturer profile** (for example "FRITZ!Fon") builds on the standards (GAP, CAT-iq) and adds manufacturer features and quirks. A **device profile** (for example "FRITZ!Fon C4") builds on the manufacturer profile and adds device quirks. Profiles can be **imported**, so new handsets need no new release. Why: on the Eventphone list, some FRITZ!Fons show incoming numbers and phonebook names and others do not. To refine in detail on Wednesday; points to settle are in the review
- **Decided 2026-10-28 (maintainer):**
  1. **The user picks the handset model**, before starting a registration, so registration quirks can apply too. AVM's own DECT Monitor cannot tell manufacturer and model either, and the user knows them; automatic detection is at most a later suggestion.
  2. A device profile may carry **firmware version ranges**.
  3. **Quirks are data, features are code:** a profile file sets behaviour (send this, omit that, a timer, a limit); feature modules (phonebook, call lists) are code that a profile can only switch on, so an imported profile cannot add code.
  4. **The more specific layer wins:** device over manufacturer over standard. Separately, what a handset reports about itself beats a profile unless the profile marks that report as wrong.
  5. **One profile format** for the cell, the standalone box and the controller.
  6. **Sources:** the standards and our own on-air observations first. For a handset that does not work with our base at all, the firmware of its own base station may be analysed, through the clean-room process only (AGENTS.md §2.9).
  7. **Profiles are files loaded at runtime** (maintainer, 2026-10-28): manufacturer and device files under `/etc/opendectcell/profiles/`, read by `dect-stubd` at startup and again on SIGHUP, so a new or corrected profile is tested by copying the file and reloading, with no rebuild. `dect-stubd --check-profile FILE` validates a file without the radio. The log names the profile and each quirk that took effect. The format is documented for a clean-room agent in `docs/handset-profiles.md` (every key, its values, its effect, examples), so profiles can be built from clean-room facts and tested at once; unknown keys are reported and ignored
  First quirk candidates from the 2026-10-28 bench: the A540 places internal calls through list access plus `0x17` (standard, now handled), rings silently when a call names a line, tries PIN 0000 by itself before asking (confirmed from RES1, bench card test 20 note); the C430A and AS690H start their call timer a few seconds after CONNECT
- [x] Design: a per-handset profile chosen by the user (see above), checked against what the handset tells us (<<MODEL-IDENTIFIER>>, terminal capability, CAT-iq level), carrying (a) **quirks**, such as the A540 showing no call timer, plus the measured rules that once looked like quirks and turned out to be the standard (TI 0, <<SIGNAL>>), and (b) **feature modules** a handset can use. Decide where it lives: libopendectcell, so that the cell, a standalone box and the controller share one table (the controller's AGENTS.md §6 already requires a feature-module framework and a model-quirks table)
- [ ] Handsets outside GAP/CAT-iq: several on the Eventphone compatibility list do not meet the specs. The layer should let a motivated user add a "driver" for such a handset from **on-air captures of their own handset**, and where that is not enough, from facts out of its own base station's firmware, gathered in a separate clean room (decided 2026-10-28; AGENTS.md §2.9). Never vendor code in the library
  - 2026-10-28: built as profile files (`docs/handset-profiles.md`, `profiles/`), chosen in LuCI before registration and applied at enrolment (bench card 28)
- [ ] Vendor features as modules: the FRITZ!Fon phonebook and its online services (RSS and similar). What they use on air is unknown and needs stock captures (the hardware-analysis step / Freetz room questions), then a module written from the facts

## M9: Productising the cell (packages, service, LuCI)
- [x] Feed with `kmod-opendectcell-ipq40xx` and the `opendectcell` package (programs plus board profiles; pulls in the right kmod); CI build with the OpenWrt SDK
  - 2026-10-25: both packages build with SDK 25.12.5 (`opendectcell-0.1-r1.apk`: `dect-stubd`, `dect-fwload`, `dect-pcmcap`, init script, UCI config). The kmod package had gone stale (copied one source file of three) and is fixed. Open: board profiles, a CI build
  - 2026-10-28: board profiles installed; `tools/release/build.sh` and the Packages workflow build all four packages for ipq40xx and lantiq with the official SDK, signed with the project key, as a draft release on every `v*` tag
- [x] Board resources: in-module board table first, then an upstream DTS patch for the 7520/7530
  - The in-module tables exist since 2026-10-17 (ipq40xx) and 2026-10-29 (lantiq), matched by DT compatible with GPIO lookup tables. Open: the DTS patch, which needs the maintainer's decision to go upstream and a flashed image to test
  - *2026-10-30, checked before writing it:* there is nothing upstream for a DTS patch to describe the PCM path with. Neither Linux (6.12 `qcom-ipq4019.dtsi`) nor OpenWrt's ipq40xx target has a node or driver for the IPQ4019 audio block (ADSS) our PCM runs through. The DECT chip's loader UART and boot strap (GPIO16) are the console UART's pins (`blsp1_uart1`, `serial_0_pins` GPIO16/17), which the module re-muxes for the boot. A node would serve only our out-of-tree module, which OpenWrt does not normally take. What could go upstream: (a) a minimal `dect` node (reset GPIO 40, strap GPIO 16, the PCM pins 20-27 as a pinctrl group) used by our module when present, the board table staying as the fallback; (b) nothing until an ADSS binding exists. The maintainer chooses (a) or (b); a flashed 7520 image is needed to test either. *Decided 2026-10-30 (the maintainer left it to us): (b).* The board tables work on stock OpenWrt images today, keyed by the board's compatible string. A minimal node would give users nothing and nothing upstream would read it, yet it would cost a flashed image to test. Revisit if an ADSS binding appears upstream or a second board needs the same resources
- [~] procd init script: board detection, profile plus UCI overrides, clear message on unsupported boxes; console/getty handling
  - 2026-10-25: `/etc/init.d/opendectcell` with `/etc/config/opendectcell`, **disabled by default** (nothing loaded, nothing on air until `enabled '1'`); `insmod`, one firmware upload per module load, every store served, registration closed unless `registration '1'` (`-a noregopen`); refuses with a clear log line if firmware is missing or a getty holds the DECT UART. Start/stop verified on the box under procd (stop parks). Open: board detection, the console strategy itself
  - 2026-10-25 evening: `register` command and timed window (SIGUSR1, `regwindow`, default 120 s), a random PIN per install, the getty handled, procd respawn, options `wideband`, `overlap`/`dialwait`, `cipher`. The service was tested end to end on the box. **[hw]** A ciphered call through it is untested (bench test 17)
- [x] Firmware storage in `/etc/opendectcell/firmware/`: SHA-256 check against the profile, kept across upgrades (`keep.d`)
- [~] LuCI app for non-technical users: status page, firmware upload, basic settings
  - 2026-10-25: status page and register button done. **A settings page is wanted** (maintainer): enable, PIN, registration window, wideband, overlap and dial wait, ciphering, the stores. Every option already exists in `/etc/config/opendectcell`
  - 2026-10-28: Network > DECT with General, Handsets (registration with model and PIN, per-handset pop-up, Line 1 and Line 2) and VoIP Profiles; Status > DECT with DECT, VoIP and Log tabs. Open: firmware upload in the page
- [~] **LuCI DECT Monitor page** (`docs/diagnostics.md`): 2026-10-25 first version, `openwrt/package/luci-app-opendectcell` (Status > DECT: base radio/registration/firmware, per handset name, slot, state, last contact, carrier, level; refreshes every 2 s from the state file). Installed on the lab box by hand, served and readable; seen in a browser at the bench on 2026-10-25. Since then it has had columns named Channel (frequency), Signal Strength (bars and dBm) and Quality, "base off" while parked, a registration countdown, and a "Register a handset" button showing the PIN. Original item: one row per registered handset — identity, in range / connected, model, field strength, signal quality, slot type (long/full), time slot, carrier, codec in use, encryption — plus a base section (RFPI, radio state, dummy-bearer slot, carrier, power). The point is that a non-technical user can answer "is this handset in range and what is it doing" without reading a log. AVM ships exactly this as its DECT Monitor; every hard question of 2026-10-23 was a field on that screen. Deliberately **not** a debugging console: raw mails stay in `dect-stubd -v`.
- [x] Fon/DECT LED (`green:fon`): on while the radio is up, ~5 Hz blink on C-plane traffic or a call, off when parked (`dce3ad7`, verified run 30)
- [~] **Export diagnostics from `dect-stubd`** — 2026-10-25: `/tmp/opendectcell-state.json`, rewritten atomically at most once a second (radio, STUB build, registration, every stored handset with slot/name/link/last seen, a `dect_diag` object per live link; IPUIs only with `-p`). A file rather than a socket: LuCI/rpcd read it without a protocol and the poll loop stays as it is. Verified on the box. Original item:: periodic chip reads into the `dect_diag` struct and a way out of the process — a JSON line on a UNIX socket, since both consumers (LuCI, controller) are out-of-process. The slot audit and UAK read-back already exist and should feed this rather than only printing.
- [~] Cell mode: tunnel mails (TCP, optional TLS) and audio (UDP, optional SRTP/DTLS) to the controller. *2026-10-30:* mails over mutual TLS (`dect-cell`, package `opendectcell-cell`) done; interop with the Go controller passed (`docs/notes-for-the-controller.md`); audio (MEDIA_*) not built

## M9b: Standalone VoIP on one box (**decided 2026-10-25: prioritised**, baresip)
- **Decided 2026-10-28 (maintainer):** a **direct bridge** between `dect-stubd` and baresip, with an option to use a controller instead; that option is a **dummy** until the cell protocol is finished. The bridge carries the events the cell protocol will carry (call setup, digits, alerting, connect, release, audio, diagnostics), so the controller can replace it later without touching `dect-stubd`.
- **MVP: two SIP lines per handset**, as a **line pool**: the box holds SIP lines, and each handset is attached to up to two of them; a line may be attached to several handsets and rings on all of them. This is the CAT-iq multiple-line model (TS 102 527-3 §7.4.7, "attached handsets" in the line list). The chip carries at most three calls at once across all handsets (A-0124; two proven on 2026-10-28).
- **Later, optional:** call lists (outgoing, incoming, missed) and phonebook access from a vCard source over **HTTPS only**, both served through CAT-iq list access. **Contacts are editable on the handset and written back to the remote source** (maintainer, 2026-10-28), as TS 102 527-3 requires for user-defined contacts (spec sweep, §3). *2026-10-30:* the phonebook part is built: the Contact List from one CardDAV address book, both ways (`docs/phonebook.md`, package `opendectcell-carddav`); open: a handset on the air, a real server. Call lists still open.
- [~] Busy handling: an incoming SIP call when all three audio channels are taken, or when the target handset is already in a call (refuse on the SIP side; CAT-iq call waiting for a handset in a call). Handset-placed and internal calls without a free channel are already refused with "insufficient resources"; the exact clauses come from the spec sweep
  - 2026-10-29: built (`87da0f1`, review fixes `45b0264`): a SIP call to a handset in a call waits on a Part 3 handset (TS 102 527-3 §7.4.3.5.2: accept, reject, hold, resume, toggle, release one, a second call out); other handsets, or no free channel, answer 486 Busy Here. **[hw]** Untested; card 9-10 in `docs/bench-7412.md`
- [~] Prove the whole path, handset ↔ cell ↔ SIP ↔ PBX, on one OpenWrt box before committing to the controller: two SIP lines per handset from a line pool (above), calls in and out, caller ID, wideband, configured in LuCI. Candidate SIP stack: baresip from the OpenWrt telephony feed (BSD), driven by `dect-stubd` through a small bridge. This revisits the controller decision "multi-cell only" (the maintainers' notes) for the single-box case only
  - 2026-10-28: **calls work both ways** (bench card 27): A540 to a mobile phone and back through a FRITZ!Box as SIP registrar, G.722, ciphered, number and line shown; SIP accounts are VoIP profiles assigned as a handset's Line 1/Line 2. Open: one line ringing several handsets, Line 2 for outgoing calls, narrowband over the audio socket, more than one bridged call
  - 2026-10-29: all four built (`07a57dc`, `ead1a36`): up to three SIP calls at once, a VoIP profile shared by several handsets rings them all, a handset's chosen line, narrowband resampled to the bridge's 16 kHz; not yet on air
  - 2026-10-29 night: codecs matched end to end instead of converted (`760bb12`): narrowband callers ring the handset narrowband, a narrowband handset's INVITE offers G.711 only, G.722 passes through untouched (`371e783`); DTMF to SIP, the voicemail indicator (`58cf49e`). **[hw]** Untested; cards 5-13 in `docs/bench-7412.md`

## M10: Controller (repo `opendectcell-controller`; implementation starts after G7)
- [x] Architecture, VoIP requirements and DECT feature framework (design docs in opendectcell-controller)
- [ ] Implementation per the controller repo's own roadmap

## M11: Multi-cell
- [x] Research: DECT identities for a multi-cell system (ARI classes, allocation of EMC/EIC/FPN, uniqueness, which classes GAP/CAT-iq handsets accept); define how the controller assigns the system ARI and per-cell RPNs on first run
  - 2026-10-29: `docs/multicell-identities.md`. Class A, at most 7 cells per system (RPN 1-7); we hold no EMC, so the controller adopts one enrolled box's factory ARI as the system identity, forever; LAL 36 makes all cells one location area. Open: Q1 (can the RFPI be set other than by patching the firmware at upload?), and a decision on an EIC/EMC for more than 7 cells
- [~] Idle roaming across cells with a shared registration database (controller)
  - 2026-10-25, one box (`docs/multicell-experiments.md`, 07.56 only): the chip admits, pages and rings handsets with no key in it, so admission is a host decision; deferred location registration (API 18) fetched a roamer's record in 2 s and registered it; a handset moving within one location area does not re-locate, so cells must fetch on any first message from an unknown IPUI or be pushed records ahead of time
  - M4: a held roamer's SETUP is answered at once (API 24), and the A540 waited 17.5 s for a 30 s fetch and connected. M6: Gigasets honour <<DURATION>> in LOCATE-ACCEPT (they re-locate every 41 s at 1 × 40.96 s), so cells could use it for presence
- [~] Research: inter-cell synchronisation (host-driven FSC on the 7412? STUB sync modes?) for seamless handover
  - 2026-10-29, standards side: `docs/multicell-identities.md` §8. One FP's cells shall share frame and multiframe timing (EN 300 175-3 §6.2.2.1.1); across boxes only connection handover is possible (bearer handover needs one MAC). Without sync: idle roaming works with a conformance gap, calls drop at the cell edge. Open: the chip side (questions Q3-Q6 for the clean room) and EN 300 175-2/-7, not on disk
- [~] Capacity per cell (about 3 calls); verify against the mode byte
  - 2026-10-25: **three voice channels per chip**, narrowband or wideband, from stock captures (A-0124). Tick it once bench test 18 shows two calls in parallel on our stack
  - 2026-10-28: **two calls in parallel on our stack** (bench test 18) and internal calls both ways (test 19). Three at once not yet tried. Calls get their own call id since `9a13f74`; the line pool, call waiting and ringing several handsets are designed in `docs/lines-and-calls.md` (steps 1-3 built)

## Later
- [~] Port to the 7412 (VR9 SSC in SPI-slave mode, `kmod-opendectcell-lantiq`) and the 7362 SL (VR9 TDM)
  - 2026-10-29: the 7412 runs the base over the generic `kmod-lantiq-ssc-pcm` transport (its own repository, not `kmod-opendectcell-lantiq`): handsets register, SIP calls in and out with HD voice, internal calls (`docs/bench-7412.md`). Open: the release packaging of the transport, a chip reset when a park cannot be verified, the 7362 SL
- [ ] 7412 PCB check: what hangs off GPIO10/12 ("PCM-link flash" lines)
- [ ] ULE, SUOTA: out of scope for now
