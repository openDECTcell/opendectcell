# Roadmap

**First target:** FRITZ!Box 7412. The 7362 SL and 7520/7530 follow, reusing the same stack. Each milestone below maps to a GitHub milestone, and each item to an issue. Items marked **[hw]** need a real box.

## M0: Analysis and tooling (mostly done)
- [x] Extract and document the DECT components of the 7412, 7362 SL and 7520 images
- [x] Reader for AVM's Lantiq NAND filesystem image (`tools/avm_nand_sqfs.py`)
- [x] Headless the analysis tooling pipeline for local analysis
- [x] Control-channel framing (0xAA marker, sequence, length, 16-bit sum)
- [x] 7412 SSC/SPI-TDM register setup, STUB patch mechanism (TLV, tag 1 = RFPI, tag 7 = mode byte)
- [ ] Complete mail header layout (source/destination task, primitive, length)
- [ ] Primitive-ID catalogue host↔STUB (`docs/stub-api.md`)
- [ ] STUB (CR16C) analysis: meaning of mode byte tag 7; channel configuration

## M1: Lab setup on the 7412 **[hw]**
- [ ] OpenWrt on the 7412 with serial console access; document the boot log
- [ ] PCB check: DECT chip marking, what hangs off GPIO10/12 ("PCM-link flash" lines)
- [ ] Locate the factory DECT config block (TLVs incl. RFPI and calibration) in flash; write an extractor
- [ ] Optional: stock-firmware observation with Freetz-NG (`/proc` DECT commands, mail logging, PROTRACE)
- [ ] Console strategy (the DECT UART is the console UART): release the console after boot or move it

## M2: Firmware loader (`dect-fwload`)
- [ ] `dect-fw-extract`: fetch `dectfw_*.hex` from an AVM image or the box (blob policy: never shipped)
- [ ] Intel-HEX → image builder: length header, patch area via descriptor at chip address 6, TLV patching, XOR checksum
- [ ] UART boot: ROM stage (STX/SOH/len16/XOR/ACK) and second stage (len32/CRC16), retries
- [ ] Chip simulator plus unit tests against the real `.hex` files
- [ ] **[hw]** First boot of the STUB on OpenWrt (reset GPIO33), with its UART output captured

## M3: SPI-TDM driver for the VR9 SSC (kernel module)
- [ ] SSC slave-mode streaming: 32-bit words, MSB first, clock polarity 1; RX/TX FIFOs, then DMA
- [ ] Real-time path (dedicated VPE or high-priority IRQ); frame sync on GPIO9, clock enable GPIO7
- [ ] **[hw]** Raw capture tool: dump frame buffers and search for the 0xAA control framing (a software logic analyser)
- [ ] **[hw]** Determine words per frame, control-slot position, voice-slot map and clock master

## M4: Control channel (`dect-stubd`)
- [ ] Framing, checksum and sequence handling; idle pattern; resync
- [ ] STUB liveness and status handshake (`(vendor-internal name)`, version query)
- [ ] Endianness handling for the big-endian MIPS host
- [ ] **[hw]** First mail exchange with the STUB

## M5: MAC interface
- [ ] Start/stop of the FP MAC, RFPI and beacon, paging
- [ ] Connection setup/release, bearer and handover primitives, cipher-key handover
- [ ] Audio-slot binding (`(vendor-internal name)`, PCM slot indication)

## M6: DLC + NWK (GAP)
- [ ] LAPC/Lc DLC (ETSI EN 300 175-4)
- [ ] LCE, MM: access rights (PIN registration), locate, identity, authentication (DSAA, later DSAA2), ciphering
- [ ] CC: setup/alert/connect/release, CLIP, IWU codec lists
- [ ] Evaluate reuse of libdect (OsmocomDECT) NWK code
- [ ] **[hw]** Register a GAP handset; first call

## M7: Telephony
- [ ] `dectd`: handsets as SIP user agents (narrowband G.726↔linear in the STUB, PCM↔RTP on the host)
- [ ] Wideband (G.722, CAT-iq 2.0)
- [ ] Internal calls and multiple handsets

## M8: OpenWrt integration
- [ ] Packages: kernel module, `dect-fwload`, `dect-fw-extract`, `dect-stubd`, `dectd`
- [ ] DTS changes for the 7412 (SSC/TDM pins, DECT reset, console)
- [ ] UCI configuration, LuCI page for handset registration

## M9: Multi-cell DECT over IP
- [ ] Split: box = radio unit (STUB + PCM + mail/RTP tunnel); central controller = DLC/NWK/SIP
- [ ] Idle roaming across cells with a shared registration database
- [ ] Research: inter-cell synchronisation (host-driven FSC on the 7412? STUB sync modes?) for seamless handover
- [ ] Capacity: about 3 calls per cell (STUB PCM channels); verify against the mode byte

## Later
- [ ] Port to the 7362 SL (VR9 TDM) and the 7520/7530 (IPQ40xx ADSS TDM; console shared with DECT)
- [ ] ULE, SUOTA: out of scope for now
