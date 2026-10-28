# Host ↔ STUB mail API

Status: repository-only catalogue, 2026-10-25. Source snapshot: `openDECTcell/opendectcell`, `main` at `428c872de7969254436139afada77acfb62b7a2d`. Prepared for maintainer review; no firmware, binaries, external clean-room answer files or external sources were opened.

## Reading this catalogue

Evidence labels apply to individual claims, not automatically to every field in a row:

- **measured**: the repository records an observation on hardware; this catalogue did not repeat the experiment.
- **answered**: a repository document or code comment records a clean-room answer, or a static finding. Existing `[CR …]` citations are retained; they are not claims that the answer file was consulted.
- **inferred**: an interpretation, or behaviour established only from openDECTcell's current encoder/decoder. An encoder proves what we send, not what the chip does.
- **unknown**: the available repository evidence does not establish the fact.

`H→C` means host → chip; `C→H` means chip → host. `p[i]` is a zero-based **parameter** offset, excluding length, tasks and primitive. `h` is a connection handle, `s` a subscription slot, and `k` an audio channel; they are different namespaces. Integers are little-endian unless stated otherwise. Hex byte strings give bytes in transmission order. `u16@i` and `u32@i` occupy two and four consecutive octets respectively. An observed zero is not necessarily a reserved field.

Build columns refer to the STUB's FRITZ!OS source build on a 7520, not the OpenWrt host version. **Unknown build** means the cited passage does not pin that particular fact to 07.56 or 08.25. Addresses from 08.25 must not be reused on 07.56 without separate evidence. [B, tests 10–11; G, runs 21–22 result]

Source abbreviations below identify repository files; each use also names a section, function or line range:

| Key | Repository source |
| --- | --- |
| P | `docs/stub-pcm-control-channel.md` |
| L | `docs/stub-loader-protocol.md` |
| I | `docs/stub-image-format.md` |
| M | `docs/mac-bringup.md` |
| R | `docs/registration-g6.md` |
| G | `docs/call-g7.md` |
| T | `docs/two-calls.md` |
| D | `docs/diagnostics.md` |
| C | `docs/capacity.md` |
| F | `docs/models/7520-DECT-analysis.md` |
| B | `docs/bench-2026-10-25.md` |
| V | `docs/verification-gap.md` |
| H | `src/libopendectcell/dect_proto.h` |
| MAIL | `src/libopendectcell/dect_mail.c` and `dect_mail.h` |
| SESSION | `src/libopendectcell/dect_session.c` |
| CS | `src/libopendectcell/dect_cs.c` |
| LINK | `src/libopendectcell/dect_link.c` |
| DIAG | `src/libopendectcell/dect_diag.c` and `dect_diag.h` |
| S | `src/stubd/dect-stubd.c` |

## 1. Transport, framing and complete mail header

### PCM control slot

**Measured, 08.25:** the control stream carries two bytes per 8 kHz PCM frame, low byte of the stored DMA word first: 16 kbyte/s in each direction. Idle is `FF FF`. G2/G3 found raw slot 1 in both directions, but receive alignment can rotate after a chip restart. Discover the slot rather than treating that number as a board-independent constant. Slot 4's per-frame echo is not the mail interface. [P, “Verified on hardware: control slot and mail exchange”, “Verified on hardware: request/answer loop”]

### Common mail header

**Measured, 08.25; implemented in MAIL:** both directions carry this mail, beginning at its length field:

| Mail offset | Size | Meaning |
| --- | --- | --- |
| 0 | 2 | `L = 2 + parameter_count`, little-endian; counts primitive and parameters, excluding both task bytes |
| 2 | 1 | Source task |
| 3 | 1 | Destination task |
| 4 | 2 | Primitive ID, little-endian |
| 6 | `L - 2` | Parameters |

Thus the complete mail has `n = L + 4 = 6 + parameter_count` bytes. The daemon's logged `len` is **parameter_count**, not `L`. Some older clean-room excerpts count from the primitive; their `mail[2]` is this catalogue's `p[0]`. Convert offsets only where the source establishes that convention. [P, G2/G3; MAIL, `dect_mail_encode()`, `dect_rx_pull()`; D, “Revised field map”, `[CR A-0088]`]

### Chip → host frame

**Measured, 08.25; implemented in MAIL:**

| Frame offset | Size | Meaning |
| --- | --- | --- |
| 0 | 1 | Start marker `AA` |
| 1 | 1 | Sequence, incrementing modulo 256 |
| 2 | 2 | `L`, little-endian |
| 4 | 1 | Source task |
| 5 | 1 | Destination task |
| 6 | 2 | Primitive, little-endian |
| 8 | `L - 2` | Parameters |
| `L + 6` | 2 | Little-endian sum of the bytes from offset 2 through offset `L + 5`, modulo 65536 |

Total length is `L + 8`. **Inferred from the parser:** it accepts `2 <= L <= 0xFB`, hence at most 249 parameter bytes. It skips idle bytes, retains an incomplete tail, and resynchronises after a bad length or checksum. Sequence discontinuity is counted, not itself used to reject a checksum-valid mail; the first valid mail after resynchronisation is exempt. [MAIL, `dect_rx_pull()`, `DECT_MAIL_L_MAX`]

### Host → chip element

**Measured, 08.25; implemented in MAIL:** transmit does **not** use the receive-side `AA` prefix.

| Element offset | Size | Meaning |
| --- | --- | --- |
| 0 | 2 | Word count `floor((n + 5) / 2)`, little-endian; counts words after this count word, including any final padding |
| 2 | 2 | Transmit sequence, little-endian |
| 4 | `n` | Complete common mail, including its length field |
| `4 + n` | 2 | Little-endian sum of all `n` mail bytes, modulo 65536 |
| `6 + n` | 0 or 1 | `FF` padding if needed to make the element even-sized |

**Inferred from the encoder:** the mail limit is `n <= 0xFC`, giving a transmit parameter limit of **246**, smaller than the receive limit. Sequence, word count and padding are excluded from the checksum. [P, G2; MAIL, `dect_mail_encode()`, `DECT_MAIL_TX_MAX`]

### Task routing

**Measured/answered, 08.25; current code uses these routes:**

| Task | Catalogue description | Evidence/source |
| --- | --- | --- |
| `0x02` | Chip command/contact destination | **measured**, P G2/G3; **answered**, `[CR A-0003]`, H task constants |
| `0x03` | Host C-plane event destination | **measured**, R first registration attempt; H |
| `0x05` | Host access/authentication event destination | **answered**, H `DECT_PRIM_ACCESS_CHAL`, `[CR COMMAND_SET §8.2]` |
| `0x10` | Host audio event destination | **measured**, R first registration attempt; H |
| `0x14` | Host management task; also source of the later startup configuration replay | **measured**, P contact replies; S `main()` startup mail list, 07.56 capture comment |
| `0x22` | Host PCM/control endpoint | **measured**, P G2/G3; H |
| `0x23` | Chip PCM/debug endpoint | **measured**, P periodic reports and loop-back; H |
| `0x26` | Chip MAC event source | **answered**, F §5 task-ID table; **measured**, G link-control reports |
| `0x27` | Source of an observed debug report | **measured**, P G2, `0xFFF1` subcommand `0x0C` |
| `0x28` | Source of the build-stamp report | **measured**, P G2 |

These are routing IDs, not evidence that every task in the historical host task table is a remotely addressable chip service. [F §5; S startup list and debug senders]

## 2. Primitive catalogue

One row is provided per ID found as a mail primitive in the cited material. Historical receiver candidates are included explicitly as **unknown**; they are not verified commands or guaranteed chip-originated messages. `0x0204` is retained as an explicitly rejected candidate. NWK message types and host-internal messages are not STUB primitives.

Unless a row says otherwise, current host command encoders use source `0x22`, destination `0x02`; debug commands use `0x22→0x23`. This default is **inferred from S/CS**, not a claim about every historical candidate.

| ID | Direction | Our name | Parameter layout | Meaning / use | Evidence | Build | Sources |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `0x0152` | C→H candidate; wire direction unknown | Unclassified 0152 | Unknown | Listed by an early host-receiver analysis only | **unknown** | 08.25 host analysis; chip use unknown | M, “Likely STUB answers”, `[CR A-0006]` |
| `0x0153` | H→C, `14→02` | Eco configuration replay | `p[0]=00` in current startup sequence; other values unknown | Replays one of the stock startup settings; exact 07.56 semantics unknown | **measured** stock bytes; **inferred** current send; **unknown** mechanism | 07.56; cited address interpretation came from 08.25 | S `main()` startup list; G runs 4 and 4 result |
| `0x0154` | C→H candidate; wire direction unknown | Unclassified 0154 | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis; chip use unknown | M, “Likely STUB answers”, `[CR A-0006]` |
| `0x0156` | C→H candidate; wire direction unknown | Unclassified 0156 | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis; chip use unknown | M, “Likely STUB answers”, `[CR A-0006]` |
| `0x0157` | H→C, `14→02` | Capability-profile replay | `p[0]=00` in startup; full value map unknown | Earlier answer calls zero the unrestricted profile; current code explicitly withdraws an understood 07.56 mechanism | **answered** older profile interpretation; **measured** replay bytes; **unknown** full semantics | 08.25 interpretation; 07.56 replay | G, “What A-0027 established”, `[CR A-0027]`; S `main()` startup list |
| `0x0158` | Unknown | Extended-data candidate | `u16@1`, `u32@3`, `u16@50`; intervening fields and total size unknown | Only endian-converted fields and an extended-data association are recorded | **answered** partial field widths; **unknown** meaning/direction | 08.25 context; other builds unknown | P, “Mail layout (partial)”; G, TPUI-range discussion |
| `0x0159` | Unknown | Extended-data candidate | Unknown | Mentioned with `0x0158` in the extended-data/TPUI discussion | **unknown** layout/direction | Unknown | G, TPUI-range discussion, lines 636–684 |
| `0x015A` | C→H candidate; wire direction unknown | Unclassified 015A | Unknown | Proposed radio reply in an early analysis; not an established radio-on acknowledgement | **unknown** | 08.25 host analysis | M, “Likely STUB answers” and group-A replay, `[CR A-0006]` |
| `0x015B` | H→C | Radio on/off | `p[0]=01` on, `00` off; one byte used | Starts beaconing or parks the radio | **measured**, **answered** | 08.25 G5; 07.56 later call runs | M G5; H radio constants; S startup/shutdown; `[CR COMMAND_SET §6.1]` |
| `0x015C` | H→C | Acknowledged B-field data | Contains a 40-byte B-field addressed by a tag; full offsets/header unknown here | Reliable B-field data; not cell configuration and not the ordinary Cs transmit path | **answered** purpose; **unknown** full layout | Early 08.25 work; exact answer build not stated | R “The G6 build”; M G5 and A-0036 correction; H; `[CR COMMAND_SET §6.4]`, `[CR A-0036]` |
| `0x015D` | C→H, destination `14` | Acknowledged-data confirmation | Unknown | Confirmation associated with `0x015C`, not general MAC startup success | **answered** association; **unknown** payload | Unknown; early 08.25 context | H `DECT_PRIM_ACKED_CONFIRM`; R G6 build; `[CR COMMAND_SET §6.4]` |
| `0x015E` | H→C candidate | Paging/queue-management candidate | Unknown; a bearer-type field is mentioned without a complete layout | Not a verified prerequisite to ordinary paging; later ordinary-call capture reports it absent | **answered** limited role; **unknown** complete contract | 08.25 interpretation; 07.56 absence in cited capture | M A-0036 correction; G A-0076 §4 discussion and A-0119/run 11; `[CR A-0036]`, `[CR A-0076]`, `[CR A-0119]` |
| `0x015F` | H→C | Extended admission | Numeric admission namespace recorded; byte offsets unknown | Extended/ULE admission, separate from six ordinary subscription records | **answered** role; **unknown** layout | Unknown; 7520 review context | C “The two different sixes” and “Host-driver confirmations”; `[CR SC14441_Firmware_Review §5]`, `[CR A-0014 F1/F2]` |
| `0x0165` | C→H candidate; wire direction unknown | Unclassified 0165 | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0x0169` | H→C, startup `14→02` | Feature-setting replay | Startup `p[0]=07`; older experiment used `1F`; full bit meanings unknown | Startup configuration replay; not a radio switch | **measured** replay/memory changes; **unknown** complete semantics | 07.56 replay; 08.25 earlier interpretation | S `main()`; M final survey; G run 4 discussion |
| `0x016A` | H→C | Handover request | `p[0]=h`, one byte in current sender | Requests a bearer move; the cited experiments did not isolate a successful forced handover | **inferred** encoder; **answered** intended role; **unknown** demonstrated effect | 08.25 G7 experiments; 07.56 unestablished | S `send_handover()`; G v24/v25 and opening unknowns |
| `0x017A` | H→C according to header; command validity unresolved | Legacy release candidate | Header says `{h, reason}`; reason values unknown | Legacy release definition; later documented teardown uses `0x0202` | **answered** old declaration; **unknown** validity/semantics against later command map | Unknown | H `DECT_PRIM_RELEASE_CONN`, `[CR COMMAND_SET §6.4]`; G A-0065; §5 below |
| `0x017B` | C→H candidate; wire direction unknown | Unclassified 017B | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0x017C` | H→C established; historical C→H listing unresolved | Bearer-slot availability | `p[0]=00` unused by cited handler; `p[1]=mask`; current default `09` | Sets global bearer-slot availability; does not enlarge the connection pool | **answered** handler; **measured** state change; **inferred** default | 07.56 run 7; do not generalise addresses | S `send_subscription_tail()`; G runs 7–8; C capacity table; `[CR A-0114]`, `[CR A-0116]` |
| `0x01FE` | H→C, destination `02` | Chip debug command | Two parameter bytes; command/value meanings unknown | Generic debug operation, no established command list | **answered** envelope; **unknown** commands | 08.25 early context | P “Audio-path mails”, `[CR A-0004]` |
| `0x0200` | C→H, destination `03` | Connection up | `p[0]=h`; two-byte examples have `p[1]=00` or `01`; complete second-byte semantics unknown | MAC connection established/re-established; not proof that a speech bearer or audio route exists | **measured** examples; **unknown** full flag map | 08.25 G5/G7; 07.56 call runs | R first attempt; G v11, run A2 and signalling-only-link discussion; H |
| `0x0201` | C→H, destination `03` | Connection down | `p[0]=h`; additional fields, if any, unknown | Connection released | **measured** | 08.25; 07.56 | R first attempt; G run 1 collision result; H |
| `0x0202` | H→C | Connection teardown; legacy “trigger” name | `p[0]=h`, exactly one byte; no mode/reason in the later answer | Immediate teardown after DLC is quiescent. The retained helper encodes the same bytes under an obsolete signalling-trigger name | **answered**, **measured** later teardown; **inferred** helper bytes | 08.25-era 2026-10-23 work; 07.56 semantics not separately demonstrated here | G A-0065; S `send_audio_disconnect()` comment; CS `dect_cs_trigger_mail()`; `[CR A-0065]`, `[CR A-0070 §4]` |
| `0x0203` | C→H, destination `03` | Cs signalling segment | 13-byte core: `p[0]=h`, `[1]=01`, `[2..6]=Cs[5]`, `[7..8]=00 00` observed, `[9..11]=tag[3]`, `[12]=h`; 15-byte form adds `[13..14]` opaque trailer | Host reassembles Cs into Lc/LAPC/NWK. Tag is PMID-like; exact trailer meaning unknown | **measured** layout; **answered** core; **unknown** trailer/zero-field semantics | 08.25 G6; later corpus build coverage not individually pinned | R “0x0203 … param layout”; CS decoder; `[CR COMMAND_SET §8.2]` |
| `0x0204` | H→C rejected candidate | Not a chip command | No valid command payload established | Later chip-command audit says this ID is not a command; use `0x0244` for Cs data | **answered** negative | 08.25-era answer | G A-0065, `[CR A-0065]` |
| `0x0208` | H→C established; historical C→H listing unresolved | Cipher material | Short payload 28 bytes, optional long payload 31; detailed map in §2.1 | Supplies security material; queued mail alone does not prove cipher activation | **answered** provenance; **inferred** encoder; **unknown** selector mapping/activation | 08.25 early cipher work; 07.56 effect unestablished here | G A-0028; S `send_chip_cipher()`; `[CR A-0027]`, `[CR A-0028]` |
| `0x0209` | C→H candidate; transport scope not fully established | Authentication-trigger indication | Unknown | Stock timeline places it at bearer-up; do not promote the old “missing trigger causes refusal” theory to fact | **answered** timeline; **unknown** layout/routing | 07.56 stock comparison | G run 9 result, `[CR A-0116]`; later runs 14–19 |
| `0x020A` | H→C established; historical C→H listing unresolved | Write subscription credential | 18 bytes: `p[0]=s` (1..6), `[1..16]=UAK[16]` unchanged, `[17]=config` | Stores key plus a separate per-handset flag. Current config defaults to zero; observed stock examples include Gigaset `01`, C4 `02`; complete enumeration unknown | **answered**, **measured** registration; **inferred** current default | 08.25 record address; 07.56 stock/config tests | R “G6 achieved”; S `install_subscription()`; G runs 5–8, 21–22; `[CR A-0026]`, `[CR A-0085]`, `[CR A-0113]` |
| `0x020C` | C→H candidate; wire direction unknown | Unclassified 020C | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0x020E` | C→H candidate; wire direction unknown | Unclassified 020E | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0x022B` | H→C | Open registration / configuration token | Current payload `[0]=41` only. Experimental `[1..2]=00 10` was stored, without the proposed extra beacon changes | Exact `41` token enables the documented registration behaviour; not a freely writable capability bitfield | **measured** registration/a44 effect; **answered** exact-token test; **unknown** general trailing-data contract | 08.25 G5 and later capability work; 07.56 current use | M G5 and long-form correction; G A-0065; `[CR COMMAND_SET §6.2]`, `[CR A-0065]` |
| `0x022C` | H→C | Close registration | `[0]=01` used; nonzero clears a44 per original answer; zero's complete behaviour unknown | Ends registration advertisement; does not itself park the radio | **measured**, **answered** | 08.25; 07.56 | M G5; R G6 achieved; H; `[CR COMMAND_SET §6.2]` |
| `0x022D` | H→C | Audio path control | `[0]=h`, `[1]=flags`; `00` connects in our working sequence; bit 7 set is described as disconnect/mute; other bits unknown | Turns on the assigned path in current working bind; audio release uses `0x0233` | **measured** working connect; **answered** bit-7 interpretation | 08.25 earlier work; 07.56 runs 26–28 and bench 10–11 | G “U-plane bind”, A-0046, runs 26–28; S `send_audio_connect()`; `[CR COMMAND_SET §6.5]`, `[CR A-0040]`, `[CR A-0046]` |
| `0x0230` | H→C | Assign audio channel | Six bytes: `[0]=h`, `[1]=flags`, `u16@2=mask`, `[4]=master`, `[5]=extra`. For channels 1..3, low flags are `01/02/04`; bit 6 selects the demonstrated WB bind; remaining bit/extra combinations incompletely known | Binds the connection to an audio lane; full recipes in §3.5–3.7 | **measured** channel-1 recipes; **answered** multi-channel mapping; **inferred** current encoders | 08.25 and 07.56 channel 1; A-0124 build unspecified in T | S `send_audio_bind()`, `xlane_bind()`; G runs 26–28; B 10–11; T; `[CR A-0029]`, `[CR A-0124]` |
| `0x0233` | H→C | Disconnect audio | `[0]=h`, `[1]=01` (nonzero required by cited handler) | Releases channel configuration and clears path-on state; not the obsolete one-byte recipe | **answered** semantics; **inferred** current encoder | 08.25-era answer; 07.56 independent teardown measurement not isolated | S `send_audio_disconnect()`; `[CR A-0070 §4]` |
| `0x0242` | C→H, destination `10` | Audio assignment/state report | Four-byte form: `[0]=h`, `[1]=channel flags`, `u16@2=mask`; shorter state reports also exist; full short-form semantics unknown | “Processed”, not “assignment applied”; signalling-only links can report zero channel | **measured** forms; **answered** no-act reply; **unknown** full flag map | 08.25 evidence; 07.56 working bind replies | R first attempt; S `handle_reg_mail()`; G A-0075 discussion; `[CR COMMAND_SET §8.4]`, `[CR A-0075]` |
| `0x0243` | C→H, typically `26→03` | Link-control update | `[0]=h`, `[1..2]=opaque numeric pair`; examples `29 00`, `29 01` | Link-control/window bookkeeping, potentially resumes buffered signalling; not an audio marker or a `0x29` enumeration | **answered** corrected role; **measured** examples; **unknown** numeric units | 08.25-era work; other-build contract not pinned | H `DECT_PRIM_LINK_CTRL`; G link-control discussion; `[CR A-0072-2]` |
| `0x0244` | H→C | Append connection data | `[0]=h`, `[1]=n`, `[2..n+1]=n` bytes; current encoder accepts 1..205 | Sends the framed C-plane byte stream. Per-connection buffer is 205 bytes; cited answer says data is dropped if it will not fit. Current LINK path does not follow each append with `0x0202` | **answered** capacity; **measured** working Cs path; **inferred** encoding/call path | 08.25 G6; 07.56 calls | CS `dect_cs_append_mail()`; LINK `dect_link_poll_append_mail()`; R G6; `[CR COMMAND_SET §6.4]` |
| `0x0247` | H→C | Subscription slot operation | `[0]=s`, `[1]=op` (`00` clear, `01` set), `[2]=flag`; current default flag `00` | Clears or installs/refreshes slot state. Later answer: with op 1, flag bit 0 clears a per-slot mask bit when set, sets it when clear; other bits unknown | **answered** semantics; **measured** clearing effect; **inferred** default | 08.25-era clear test; 07.56 flag tests | S `clear_foreign_slots()`, `install_subscription()`; G vF426/vF427 and runs 6–8; `[CR A-0065]`, `[CR A-0113]` |
| `0x0248` | H→C candidate | Repeater/peer configuration candidate | Historical trial `00 07 01 05` followed by five identity-sized bytes; complete layout unknown | Later notes associate it with repeater configuration, not ordinary boot/audio setup | **answered** broad role; **measured** trial changed RAM; **unknown** field meanings | 08.25-context notes | M RFPI section, group-A replay and final survey; G A-0036 correction; `[CR A-0006]`, `[CR A-0036]` |
| `0x024E` | C→H candidate; wire direction unknown | Unclassified 024E | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0x0250` | H→C candidate | Channel-timer candidate | Historical trial `00 00`; field meanings unknown | Named as a channel timer in early notes; no proven ring/tone recipe | **answered** tentative name; **unknown** contract/effect | 08.25-context notes | G “U-plane: binding a connection to audio”; M group-A replay |
| `0x0251` | H→C | Set channel parameter | `[0]=h`, `[1]=selector`, `[2]=value`; selectors 1..6 described; normal channels use `k=1..3`, value `2B` | Sent before assignment; value stored shifted left by 8 per answer. Physical meaning is not established by the CLI name “gain” | **answered** layout/storage; **measured** working recipe; **unknown** parameter units | 08.25 and 07.56 channel-1 recipes; multi-channel build unspecified | S `send_audio_bind()`, defaults, `xlane_bind()`; T; `[CR A-0029]`, `[CR A-0036]`, `[CR A-CodecModes-3]`, `[CR A-0124]` |
| `0x0252` | C→H candidate; wire direction unknown | Unclassified 0252 | Unknown | Early proposed reply to channel settings; not an established success acknowledgement | **unknown** | 08.25 host analysis | M “Likely STUB answers” and replay result, `[CR A-0006]` |
| `0x0300` | H→C | Page handset | Three bytes: `[0]=page-info`, `[1]=TPUI middle byte`, `[2]=TPUI low byte`; optional experimental `[3]=header input` | Short page for assigned TPUI. Code derives `0C`/`2C`; later answer says first byte is ignored. Optional byte supplies header bits 1–4 per answer; other semantics unknown | **measured** successful paging; **answered** ignored byte; **inferred** optional encoder | 08.25 and 07.56 paging; optional-byte build not pinned | S `send_page()`, `ring_target_tpui()`; G A-0076 and run 32 correction; `[CR A-0076]`, `[CR A-0091]` |
| `0x0E0D` | C→H, destination `05` | Short-form access challenge | Unknown | Alternative access/authentication event; C4 G6 used the connection-oriented path instead | **answered** name/routing; **unknown** layout | Unknown; early 08.25 context | H `DECT_PRIM_ACCESS_CHAL`; M G5; R first attempt |
| `0xF600` | H→C normally `22→02`; C→H echo for tested alternate tasks | Contact | No parameters in normal request | Establishes host contact; normal replies `F601`, `F602`; alternate task combinations were echoed | **measured**, **answered** | 08.25 G2/G3; 07.56 later operation | P G2/G3; SESSION; `[CR A-0003]` |
| `0xF601` | C→H, `02→14` | Contact identity answer | Five bytes; identity/RFPI-like, exact interpretation not fully established in P | First normal contact answer; identity bytes intentionally omitted | **measured** length/routing; **inferred** identity interpretation | 08.25; later 07.56 contact use | P G2/G3; H |
| `0xF602` | C→H | Contact/capability report | 23 bytes; `[0..2]` state-like prefix, `[3..22]` four five-byte groups; exact field map incomplete | Contact answer and report following registration/radio state changes; groups correlate with beacon tails | **measured** lengths/changes; **inferred** beacon-content interpretation; **unknown** full map | 08.25 initial contact; 2026-10-22 corpus | P G2/G3; M “0xF602 carries the beacon broadcast content”; C host-driver confirmations; `[CR A-0013 F2]` |
| `0xF603` | C→H candidate; wire direction unknown | Unclassified F603 | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0xF604` | C→H | Short-form handset access | Unknown | Alternative access indication; timing/identity sufficiency for subscription virtualisation unknown | **answered** broad role; **unknown** layout/timing | Unknown; early review context | H `DECT_PRIM_ACCESS_EVENT`; C open questions; R first attempt |
| `0xF605` | H→C | Alternate subscription-table operation | Known sub-op 2 payload: `[0]=02`, `[1]=00`, `[2]=index 0..5`, `[3..7]=identity[5]`, `[8..11]=aux[4]`, `[12]=flag`; aux/flag meanings and other sub-ops unknown | Separate table operation; not the current ordinary voice credential-write sequence | **answered** producer layout; **unknown** remaining semantics | 7520 host answer; exact build not pinned in R | R host-side MM facts and G6 achieved; `[CR A-0013 F3]`, `[CR A-0026]` |
| `0xF606` | C→H candidate; wire direction unknown | Unclassified F606 | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0xF60F` | C→H candidate; wire direction unknown | Unclassified F60F | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0xF611` | C→H candidate; wire direction unknown | Unclassified F611 | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0xF613` | C→H candidate; wire direction unknown | Unclassified F613 | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0xF618` | C→H, `23→14` | Periodic companion report | Two bytes; example `37 01`; field meaning unknown | Immediately follows periodic `FFF1/04` in G3 | **measured** | 08.25 | P G2/G3; H |
| `0xF623` | C→H | Radio scan/status report | Observed 14 bytes: `[0]=09`, `[1]=0A`, `[2..11]` ten levels, `[12..13]=37 00` example; exact first/trailing fields unknown | Streams while radio is on; ten-level per-carrier RSSI interpretation remains an inference in the cited source | **measured** bytes/activity; **inferred** RSSI scan | 08.25 G5; later scan example not independently build-pinned | M G5; V “One thing we already receive and have never used” |
| `0xF624` | H→C | Request connection status | `[0]=h`, `[1]=0A`; two bytes | Polls the per-link status row; answer `F625` | **answered**, **measured** | 08.25 G7; exact cross-build field equivalence unproven | S `send_conn_status_req()`; D “0xF625 decoded”; `[CR A-0087]` |
| `0xF625` | C→H | Connection status | Observed 45-byte payload; detailed octet map in §2.2 | Carrier, slot-related value, RSSI and interval/link counters; not a slot-width indicator | **measured** shape/corpus; **answered** counter names; **unknown** residual fields | 08.25 corpus context; A-0121/A-0123 host-side mappings not explicitly build-pinned in cited passages | D revised map and counter section; DIAG decoder; `[CR A-0088]`, `[CR A-0089]`, `[CR A-0090]`, `[CR A-0121]`, `[CR A-0123]` |
| `0xF626` | H→C | DSP route control | `[0]=h`, `[1]=enable`, `[2]=channel configuration`, `u16@3=parameter`; current optional sender uses `01`, returned channel, `0000` | Experimental route command; absent from the normal bind in later answers; no established readable success indication | **answered** layout; **inferred** sender; **unknown** full field map/effect | 08.25 experiments; 07.56 not separately established | G “U-plane bind” and A-0029 bind; S `send_audio_connect()`; `[CR COMMAND_SET §6.5]`, `[CR A-0029]` |
| `0xF627` | C→H candidate; wire direction unknown | Unclassified F627 | Unknown | Early receiver candidate | **unknown** | 08.25 host analysis | M “Likely STUB answers”, `[CR A-0006]` |
| `0xFFF1` | H→C and C→H | Debug, memory and periodic status | `[0]=subcommand`; remaining fields depend on subcommand, §2.3 | Shared primitive; direction/task/subcommand must all be retained | **measured**, **answered**, **inferred** per subcommand | 08.25 established debug path; 07.56 memory observations do not establish equal addresses | P; S memory/debug senders; §2.3 |
| `0xFFF2` | C→H, `28→22` | Build stamp | Variable ASCII payload; observed `RAM: …` date string; exact length/termination contract unknown | First-contact report after chip boot; may be absent when reusing a loaded chip | **measured**; **inferred** logger treatment | 08.25 G2; S uses it to distinguish later loaded builds | P G2; S `log_mail()` |

### 2.1 Cipher-material payload (`0x0208`)

**Answered** field provenance `[CR A-0028]`; **inferred** exact current bytes from S `send_chip_cipher()`. These do not establish successful radio encryption.

| Parameter bytes | Current encoding | Evidence limit |
| --- | --- | --- |
| 0 | Explicit user-supplied selector | Live-call routing nibble is not proven equal to `h`; subscription-oriented selectors 7..12 are described, but complete consumer semantics remain **unknown** |
| 1..8 | DCK, reversed as an eight-byte group | Computed-result position/reversal **answered**; identification as the relevant DCK representation partly **inferred** |
| 9..12 | RES, reversed as a four-byte group | Computed-result position/reversal **answered** |
| 13..20 | RAND_F unchanged; optional test selects RS instead | Second half of a 16-byte input **answered**; input-half interpretation remains **inferred** |
| 21..23 | `00 00 00` | Producer constants **answered** |
| 24..25 | `FF FF` | Producer constants **answered** |
| 26..27 | `00 00` | Recorded configuration defaults **answered**; full meanings **unknown** |
| 28..30 | Optional `40 00 00` | Long-form encoder **inferred**; last two bytes' status/config meaning incomplete |

“30-byte” and “33-byte” descriptions in the older discussion include the two primitive bytes: parameter lengths are 28 and 31. Neither variant proves cipher-on, and the selector, input choice and activation indication remain open. [G A-0028; S `send_chip_cipher()`]

**Link release, from the stock captures `[CR A-0127]` (2026-10-28, observed, 07.56).** After the last over-air {CC-RELEASE-COM} stock sends **`0x0202 <link>`** (one byte) 28-32 ms later; the chip answers **`0x0201 <link>`**. There is no idle timer: it follows the last transaction. After a voice call no LAPC DISC goes over the air (the bearer is dropped by `0x0202`); after a list-access session stock sends a FP-to-PP LAPC DISC first. `dect-stubd -a nlr` does this (off by default until bench-tested).

**Location area `[CR A-0127]`:** stock's {LOCATE-ACCEPT} always carries `07 01 64`: LI-type 1, location area level 36, the PARI length of a class A ARI (this box). The broadcast PARK length is not in any host-to-chip mail; the chip builds the beacon from the RFPI and EEPROM.

**Stock call layout, from the encrypted and unencrypted stock calls `[CR A-0126]` (2026-10-28, observed, 07.56).** This supersedes the selector and the RES/RAND fields above for calls:

| Parameter bytes | Stock content on a call |
| --- | --- |
| 0 | the link (connection handle) |
| 1..8 | the 8-byte per-call key; varies per call, matches no over-air RAND, RS or RES, so consistent with the DCK; byte order not stated |
| 9..23 | `00` |
| 24..25 | `FF FF` |
| 26..27 | `00 00` |

- Sent on the connecting link **after** {AUTHENTICATION-REPLY} and **before** the over-air {CIPHER-REQUEST} (0.2 to 760 ms before it); one per link per authentication. Also sent on unencrypted calls: it loads the authentication result, it does not switch ciphering on.
- The chip answers with **`0x0209`, one byte: the link**, 100-180 ms after {CIPHER-REQUEST}. No key in it. **It is the cipher-on indication** `[CR A-0128]`: present per link in the encrypted call, absent from the whole unencrypted one (where `0x0208` still fires), and nothing else follows before the audio bind; stock takes it as the end of the cipher procedure. dect-stubd feeds it to `dect_mm_cipher_on()`, which stops <MM_cipher.1>.
- There is **no separate cipher-enable mail**: the over-air {CIPHER-REQUEST}, which the host builds, is the trigger; the chip's MAC switches by itself. With no key loaded it drops the link, which is bench test 17's 212 ms drop.
- The 31-byte form (`40`, a varying byte, a selector `00..03` appended) is for registration and location registration only.
- An internal call between registered handsets sends no `0x0208` at all (the key is reused).
- `0x1082` is not ciphering: it is in unencrypted calls and registrations too, routed to host task `0x14`, and carries an <<ESCAPE-TO-PROPRIETARY>> broadcast element `[CR A-0126]`, confirming `[CR A-0072]`.

**Capability bits a24, a29, a34** `[CR A-0128]` (observed): the chip reports its Q-channel messages to the host in **`0xF602`** (len 29); the host shapes them with `0x0153`/`0x0169`/`0x0157` at bring-up, never as literal bytes. The "extended FP capabilities part 2" (Qh 12) body is `c8 01 85 11 34`, **identical in ours and stock's**: a24 (NG-DECT wideband voice) = 1, a29 (extended wideband voice) = 1, a31 (third-party conference) = 1, **a34 (multiple lines) = 0** (body byte 3 bit 5). `0x0157` only changes byte 4 (a42/a43). Stock leaves a34 at 0 although its box offers two lines (A-0125 saw line ids 1 and 2): it assigns lines in its own configuration, not by TS 102 527-3 §7.4.7.

### 2.2 Connection-status payload (`0xF625`)

Offsets below are parameter offsets, not offsets from the primitive. **Measured** corpus shape is 45 bytes; shorter lengths accepted by the library are parser capabilities, not independently measured firmware variants. [D “Revised field map”; DIAG `dect_diag_decode_status()`]

| Bytes | Interpretation | Evidence |
| --- | --- | --- |
| 0 | Echoed link handle | **measured**, D |
| 1 | Observed `0A`; interpreted as ten carrier entries | **measured** value, **inferred** count meaning, D |
| 2 | Slot-related value; only even examples recorded; raw slot versus twice-slot remains **unknown** | **measured** movement with carrier; DIAG header retains ambiguity |
| 3 | Carrier index 0..9 | **measured**, D corpus; **answered**, `[CR A-0088]` |
| 4 | Observed zero; meaning **unknown** | **measured**, D revised map |
| 5 | `(connection record +0x116 bit 1) ? 2 : 1`; not full/long slot | **answered**, `[CR A-0089]`; higher-level state meaning **unknown** |
| 6..15 | Ten RSSI octets, carrier order 0..9; larger is stronger | **measured**, **answered**, `[CR A-0088]` |
| 16..17 | ACRC, u16 LE | **answered**, `[CR A-0123]`; counter shape **measured** |
| 18..19 | XCRC, u16 LE | **answered**, `[CR A-0123]` |
| 20..21 | ZCRC, u16 LE | **answered**, `[CR A-0123]` |
| 22..23 | QBIT1, u16 LE | **answered**, `[CR A-0123]` |
| 24..25 | QBIT2, u16 LE | **answered**, `[CR A-0123]` |
| 26..27 | RXCOUNT, u16 LE | **answered**, `[CR A-0123]`; DIAG calls it `interval_frames` |
| 28..29 | HANDOVER, u16 LE | **answered**, `[CR A-0123]`; complete increment condition **unknown** |
| 30..31 | SLIDEERR, u16 LE | **answered**, `[CR A-0123]` |
| 32..33 | SYNCERR, u16 LE | **answered**, `[CR A-0123]` |
| 34..35 | CONNECTTIME, u16 LE, this interval | **answered**, `[CR A-0123]`; DIAG `connect_frames` |
| 36..39 | Link uptime in frames, u32 LE | **answered**, `[CR A-0123]`; low half advancing at 100 Hz **measured** |
| 40..43 | BER field | **answered** label/extent, `[CR A-0123]`; encoding, units and denominator **unknown**; not decoded by DIAG |
| 44 | Observed zero; meaning **unknown** | **measured**, D corpus map |

**Answered:** the eight event counters are interval counters cleared by a read, not monotonic lifetime totals. **Inferred from code:** DIAG accepts a 16-byte header, decodes counters at length 38 or greater, and the full uptime at length 40 or greater. Its separate frame accumulator still unwraps the low 16 bits across polls; reset it for a new link and poll more often than the approximately 655.36 s wrap. [DIAG header, `dect_diag_decode_status()`, `dect_diag_frames_update()`; D counter sections]

**Answered, `[CR A-0121]`:** the recorded RSSI conversion is `dBm = (raw - 20) * 80 / 37 - 110`; raw 0 and 255 mean no value. DIAG computes tenths of a dBm using integer arithmetic. Quality is a host calculation from the current carrier's RSSI and ACRC+XCRC+ZCRC+QBIT1+QBIT2 divided by CONNECTTIME, not an extra on-wire quality byte. [D revised map; DIAG `dect_diag_rssi_dbm10()`, `dect_diag_quality()`]

### 2.3 Shared debug/status primitive (`0xFFF1`)

This subcommand table expands the single catalogue row; it does not assign new primitive IDs.

| Subcommand | Direction / parameters | Meaning and evidence |
| --- | --- | --- |
| `01` | H→C; `p[0]=01`, `u16@1=fill` | Idle-fill setting **answered** in G “The stock host's 0x0230 layout”; complete effects/build portability **unknown** |
| `04` | C→H, normally `23→22`; 25 parameter bytes total, `[0]=04`, `[1..24]` opaque here | Periodic status **measured**, 08.25 G3; exact counter/field map **unknown** [P G3] |
| `05` | H→C candidate to `23`; `[0]=05`, remaining layout **unknown** | Historical keep-alive hypothesis only; not a demonstrated contact prerequisite [M group-A replay, `[CR A-0005]`] |
| `07` | H→C `22→23`; `[0]=07`, `u32@1=address`, `u16@5=count` | Memory read **answered/measured** in repository work; exact encoder **inferred** [S `mem_dump_tick()`, `uak_verify_send()`; V answered audit, `[CR A-0085]`] |
| `08` | C→H; `[0]=08`, `u32@1=address`, `[5..6]` length/count field, `[7..]` data | Read reply; address/data offsets **inferred** from S `uak_verify_reply()`; full length-field validation contract **unknown** |
| `09` | H→C `22→23`; original form `[0]=09`, `u32@1=channel` | Channels 1..3 enable loop-back, zero disables: **answered** `[CR A-0004]`, **measured** 08.25 G4. Later S sweep sends only `{09, channel}` for 1..6; short-form equivalence is **unknown** [P G4; S `dsp_sweep()`, `send_gendsp()`] |
| `0A` | H→C `22→23`; `[0]=0A`, `[1]=destination`, `[2]=source` | DSP route **answered**, `[CR A-0004]`; not a required ordinary call bind. Source/destination numbering and silence/test-tone claims need the caveats in §5 [P audio-path mails; S `send_audio_connect()`] |
| `0C` | C→H, observed source `27`; `[0]=0C`, remainder **unknown** | One report **measured**, 08.25 G2 [P G2] |
| `0D` | Direction **unknown**; `[0]=0D`, `u32@1`, `u16@5`; remaining bytes **unknown** | Only endian-converted field widths recorded; no established semantics [P “Mail layout (partial)”] |

**Answered/measured, 08.25 only:** the UAK read-back address in S is `0x9518 + (s - 1) * 0x11`, covering 16 key bytes and a flag. The code skips this read for a known different STUB build. Reusing the address on 07.56 produced meaningless mismatch reports; the read primitive itself does not make an address portable. [S `uak_verify_send()`; G runs 21–22 result; `[CR A-0085]`]

## 3. Sequences

These are mail sequences, not replacements for the host's MM/CC/LAPC state machines. All payloads use the catalogue's offsets. Runtime state changes generally have no negative acknowledgement; sending a mail is not evidence of application. **Answered**, `[CR A-0070]`, `[CR A-0086]`; **measured** no-act `0x0242` replies, `[CR A-0075]`. [V answered audit; S verification table]

### 3.1 Bring-up and contact

1. **Answered/measured, 08.25:** patch the supplied image's factory TLVs and mode byte, then upload stage 1 and stage 2 over UART. Each stage receives `02`, sends `01` and the length header, receives `06`, sends payload, receives the chip's XOR, and sends `06` for a match or `15` for a mismatch. This is loader traffic, not PCM mail traffic. [L §1 and hardware observations; I §§2–4]
2. **Measured, 08.25:** discover the PCM control slot and send `0xF600 {}` from `22→02`. Normal answers are `0xF601` and `0xF602`; the first contact after chip boot can also produce `0xFFF2`. [P G2/G3]
3. **Inferred from code:** SESSION retries contact every 2000 ms by default and uses a 60000 ms liveness window. Any received mail can move SESSION from CONTACTING to UP; the daemon's slot probe separately requires a contact answer rather than accepting unrelated periodic traffic. These are host policies, not chip timing guarantees. [SESSION `dect_session_tick()`, `dect_session_rx()`; S slot-probe logic; G “And the slot search was broken”]
4. **Measured stock order, 07.56; inferred current replay:** send `14→02: 0x0153 {00}`, `0x0169 {07}`, `0x0157 {00}` before radio-on when the daemon's normal `-g` startup applies. Their complete 07.56 effects remain **unknown**; `nostockcfg` omits them. [S `main()` startup list; G run 4]

### 3.2 Radio on and park

1. **Measured/answered:** `22→02: 0x015B {01}` starts the radio; `0xF623` reports then appear. [M G5, `[CR COMMAND_SET §6.1]`]
2. **Inferred current default:** follow with `0x022C {01}` to keep registration closed, or explicitly open it as below. [S `main()`]
3. **Measured/answered park:** send `0x022C {01}`, then `0x015B {00}`. Ending the daemon alone does not stop an autonomously beaconing chip. [M G5; R G6 achieved; S shutdown path; `AGENTS.md` §6]

### 3.3 Open and close registration

1. **Measured/answered:** after contact/radio-on, send `0x022B {41}`. A capability/state `0xF602` report is observed; the registration advertisement changes. [M G5 and capability correction; `[CR COMMAND_SET §6.2]`]
2. **Measured:** the C4 connection-oriented path delivers `0x0200`, `0x0243`/`0x0242`, then `0x0203` segments. Service MM through the host stack; send its Lc/LAPC output with `0x0244`. [R G6 achieved; CS; LINK]
3. **Measured/inferred:** persist the resulting host subscription, write the chip record as below, and send `0x022C {01}`. Closing advertisement and enforcing host registration policy are separate operations; API 17 adds the host policy. [R G6 achieved; H API 17; S registration handling]

### 3.4 Write or restore a subscription

1. **Measured/answered:** clear unowned chip slots with `0x0247 {s,00,00}`; preserve all slots held by loaded stores. Chip slot state survives daemon restarts. [S `clear_foreign_slots()`, `send_subscription()`; G vF426/vF427; `[CR A-0065]`]
2. **Answered/inferred:** for each owned slot 1..6, send `0x0247 {s,01,flag}`, then `0x020A {s,UAK[16],config}`. Current bare defaults for flag/config are zero; do not silently substitute stock examples as universal requirements. [S `install_subscription()` and zero-initialised configuration; `[CR A-0026]`, `[CR A-0113]`]
3. **Answered/inferred:** current code sends global `0x017C {00,09}` after installing subscriptions, unless disabled. Its presence is not proof that it solved the historical ringing failure. [S `send_subscription_tail()`; G runs 7–8, 14–19; `[CR A-0114]`]
4. **Answered/inferred:** when the build/address is established, read the UAK record with `0xFFF1/07`, compare `0xFFF1/08` data without logging the key, and report an unverifiable result for an unsupported build. Current code sends the UAK verification read before the global tail. [S `send_subscription()`, `uak_verify_send()`, `uak_verify_reply()`; `[CR A-0085]`, `[CR A-0086]`]

**Clearing a slot on deregistration, stock** `[CR A-0128]` (observed, 07.56, log 06, three handsets deleted from the web UI, the same sequence each): page the handset (`0x0300 08 11 <slot>`), bearer up, over air {ACCESS-RIGHTS-TERMINATE-REQUEST} (MM 48H, the IPUI), the handset's {ACCESS-RIGHTS-TERMINATE-ACCEPT} (MM 49H; **no authentication either way**), then `0x0247 {s,00,00}`, **`0x020A {s, 17 zero octets}`** (the A-0026 record overwritten with zeros, no separate delete prim), `0x017C {00,mask}` with the slot's bit cleared (bit n = slot n+1), and `0x0202`/`0x0201` releasing the link. The record is zeroed only **after** the ACCEPT; the whole tail follows within ~40 ms. A handset deregistering itself is not in the captures.

Do not probe slot 7: the current code cites an unchecked writer into adjacent storage and rejects `0x020A` indices outside 1..6. The older capacity document's “try a seventh slot” proposal is superseded. **Answered**, `[CR A-SubscriberCapacity-7]`; **inferred** host check. [S `install_subscription()`; C empirical-test section]

### 3.5 Paging

1. **Inferred current policy:** select the subscription being called and its assigned TPUI, rather than the identity most recently adopted by a reusable link. The run-32 bug demonstrated why these differ. [S `ring_target_tpui()`; G runs 31–34]
2. **Measured/inferred:** send `0x0300 {page-info,TPUI[1],TPUI[2]}`. Current code chooses `0C` or `2C` from the offered basic service, but **answered** `[CR A-0076]` says this first byte does not alter the page on the examined build. Do not infer long-slot selection from it. [S `send_page()`; G A-0076]
3. **Measured/inferred:** receive `0x0200`, reassemble `0x0203`, and check the identity in `{LCE-PAGE-RESPONSE}` before offering the call. It is a NWK response, not a standalone mail acknowledgement. Reject a mismatched responder through the host LCE path. [LINK `dect_link_rx_segment()`, `dect_link_page_reject()`; H API 10/13]
4. **Measured, 07.56:** carry the CC setup over `0x0244`. The A540 rang with transaction value 0; without `<<SIGNAL>>` it displayed the call silently. These were NWK fixes, not new STUB primitives. [G runs 14–19; H API 10]

**Inferred policy:** S uses a 3000 ms page interval and up to 20 pages. These retry choices are not a firmware queue contract. [S `RING_PAGE_INTERVAL`, `RING_PAGE_RETRIES`]

### 3.6 Narrowband audio bind

**Measured, 07.56:** the channel-1 recipe below worked on A540, AS690H and C430A without the earlier startup noise burst. The host lane carries signed 16-bit linear PCM at one sample per 8 kHz frame. [G runs 26–28; 08.25 PCM evidence in G vF413/vF411]

1. `0x0251 {h,01,2B}`.
2. `0x0230 {h,01,02,00,00,03}`: channel bit 1, mask `0002`, master 0, extra 3.
3. On the matching long `0x0242`, send `0x022D {h,00}`. The reply is **not** proof that assignment applied; assess audio independently.
4. Feed/read raw TDM word 0. Do not send a second assignment in this measured recipe.

**Inferred implementation distinction:** the executable's bare option defaults are not identical to this recorded recipe: mask and extra begin at zero unless options change them. `send_audio_bind()` forwards those fields. Do not label the measured `{…02 00 00 03}` tuple as the unconditional raw executable default. [S `main()`, `send_audio_bind()`, usage defaults; G runs 26–28]

**Measured historical fallback, 08.25:** a first assignment with bit 6 set followed by a bit-6-clear reassignment 500 ms after path-on yielded narrowband audio; 150 ms did not reproduce it. Later 07.56 runs established that a bit-6-clear first assignment avoids that extra transition. This is history, not the preferred sequence. [G vF411/vF414 and runs 26–28]

### 3.7 Wideband audio bind

**Measured:** coded G.722 worked on 08.25/C4 and, in bench tests 10–11, on 07.56/Gigasets. The tested channel-1 shape is:

1. Negotiate G.722 in CC; this is host protocol work carried by `0x0244`, not a change to the primitive ID.
2. Send `0x0251 {h,01,2B}`.
3. Send `0x0230 {h,41,02,00,00,00}`.
4. Follow the working path-on sequence with `0x022D {h,00}` after the assignment report; no narrowband reassignment.
5. Exchange coded G.722 on raw word 0: a 16-bit word held for two 8 kHz frames, 8000 coded octets/s. Host-generated downlink must align to that two-frame cadence.

Sources: B tests 10–11 and 12a; G v31/v32 discussion; S `send_audio_bind()`, `send_audio_connect()`, negotiated-codec handling. **Inferred from code:** wideband forces bit 6 in the assignment and extra byte zero. `dect_audio.c` supplies A-law/tone utilities, not a separate mail encoder or evidence that every call lane is A-law.

### 3.8 Audio channels 1 to 3

**Answered, `[CR A-0124]`; implemented in S `xlane_bind()`:** channel ordinal, assignment bit, slot mask and raw DMA word are related as follows. T does not independently pin the answer to a firmware build, and its multi-call test plan is not a completed openDECTcell hardware result.

| Channel `k` | `0251` selector | NB assignment flags | WB assignment flags | `0230` mask bytes | Raw TDM word, both directions |
| --- | --- | --- | --- | --- | --- |
| 1 | `01` | `01` | `41` | `02 00` | 0 |
| 2 | `02` | `02` | `42` | `04 00` | 3 |
| 3 | `03` | `04` | `44` | `08 00` | 2 |

1. **Answered/inferred:** allocate a free channel, then `0x0251 {h,k,2B}`.
2. **Answered/inferred:** send full `0x0230` with that channel's one-hot flag and `1 << k` mask. For the demonstrated WB shape, master/extra are `00 00`. Generalising the measured NB extra `03` to other channels is **inferred**, not a separately reported hardware test.
3. **Inferred current implementation:** send `0x022D {h,00}` when the associated audio report is handled.
4. **Answered:** use raw word `k XOR 1`; WB still uses one word. Three voice channels are described, not six. Host bridging is required for an internal call between two handsets. [T “What the chip side needs”; `[CR A-0124]`]

### 3.9 Audio and connection release

1. **Answered/inferred:** finish the host CC/DLC exchange as appropriate, and send `0x0233 {h,01}` to release audio. [S `send_audio_disconnect()`; `[CR A-0070]`]
2. **Answered:** when DLC is quiescent and an explicit connection teardown is required, the later command audit specifies `0x0202 {h}`. It is abrupt if sent prematurely. [G A-0065; S release-order comment; `[CR A-0070 §4]`]
3. **Measured:** `0x0201` reports connection loss. Do not use the legacy `0x0202` helper as a mandatory flush after every `0x0244`. [R connection events; LINK outbound path; §5]

### 3.10 Status polling

1. **Measured/inferred:** while a link is active and bound, send `0xF624 {h,0A}` at the configured interval; `status=8000` is an established 8-second example. [S `send_conn_status_req()`; D first decode]
2. **Measured/answered:** correlate `0xF625 p[0]` to the handle and decode §2.2. Treat interval counters separately from uptime. [DIAG; `[CR A-0123]`]
3. **Measured:** independently, contact is followed by periodic `0xFFF1/04` and `0xF618`; radio-on also produces `0xF623`. They are not replies to the per-link poll. [P G3; M G5]

## 4. Mode word and factory TLVs

### Mode word `0x105`

**Answered/measured, 7520/08.25:** the loader mode word is `0x105`. Its low byte `05` is written into image TLV tag 7, length 1, when nonzero. Bit 8 controls the preference order of stage-1 slow/fast boot attempts; it is not part of the one-byte TLV. With bit 8 set, the documented sequence tries slow first for attempts 1–11 and fast for 12–16. Other boards' recorded words include `0x119` and `0x101`; their low bytes are `19` and `01`. [L §2; I §4; F §§4,8]

**Unknown:** the individual chip-side meanings of tag-7 bits in `05`, and the rest of the mode word beyond the documented loader selection. Do not equate this boot/configuration byte with the per-call `0x0230` flags, DSP mode numbers, or the `0xF625` state byte.

### Factory block and patch list

**Measured, 7520:** the factory block is at offset `0x3D000` in both boot-loader partitions on the examined spare unit. Its four-byte header is type `02`, ID `01`, and a big-endian 16-bit length; the observed length was 17. Values are per-device and are not reproduced here. [F §8.3]

**Answered, 08.25:** image TLVs have an octet tag, octet length and value, with tag 0 ending the image list. Replacement requires both tag and length to match an existing entry; unknown tags and mismatched lengths are skipped rather than appended. The documented factory input limit is 64 bytes. The 441-family descriptor is six bytes at chip address 0: little-endian 32-bit patch address and 16-bit size; on the analysed image it points to `0xA0`, length 53. Recompute the upload payload XOR after patching. [I §§3–4]

| Tag | Value layout | Established meaning / limit | Evidence and build |
| --- | --- | --- | --- |
| 1 | Five octets, RFPI | Factory identity replaces the image placeholder. A placeholder beginning ASCII `0270` has a documented host-derived fallback; real factory RFPI should be preserved | **answered**, I §4; **measured**, F §8.3, 7520/08.25 |
| 2 | Two octets; numeric endianness/units **unknown** | Factory value differs from image default on the examined box; calibration interpretation only **inferred** | **measured** length/difference, F §8.3; **unknown** meaning, I §6 |
| 3 | One octet | Factory value equals image default on that box; meaning **unknown**, calibration only **inferred** | **measured**, F §8.3, 7520/08.25 |
| 4 | One octet | Factory value differs from image default; meaning **unknown**, calibration only **inferred** | **measured**, F §8.3, 7520/08.25 |
| 6 | One octet in default image | Absent from the measured factory block; meaning **unknown** | **answered** default shape, I §4; **measured** absence, F §8.3 |
| 7 | One octet; `05` on 7520 | Low mode byte; detailed bit meanings **unknown** | **answered/measured**, I §4; L hardware observations; F §8 |
| 9 | 22 bytes when present | Version-information TLV; not patched; absent from analysed 08.25 image | **answered**, I §4 |
| 10 | One octet in default image | Absent from measured factory block; meaning **unknown** | **answered**, I §4; **measured**, F §8.3 |

**Measured:** applying tags 1–4 plus tag 7 was accepted by the chip and brought up PCM clocks. This does not decode the calibration fields. **Unknown:** factory-block locations for the other boards in these sources, and the exact factory-test selector that disables patching. [F §8.3; I §6]

## 5. Contradictions and open questions

### Superseded statements and remaining conflicts

| Subject | Earlier statement | Preferred evidence / disposition |
| --- | --- | --- |
| Mail header / transmit prefix | P's old “partial” section leaves task offsets and an extra transmit `AA` unresolved | **Measured** G2/G3 and MAIL settle the header and asymmetric framing. Use §1 |
| Periodic status length/cadence | P G2 says “27 bytes” and 10–30 s | G3 explicitly distinguishes `L=27` from 25 parameters and measures about 5 s. Cadence varies; neither is a fixed protocol timer |
| Boot sequence | M's “groups A/B” were treated as ordered initialisation; `015B` seven zeros and `022C` as a radio switch | **Measured** G5 and **answered** `[CR A-0036]` retract this. `015B 00` is off, `022C 01` closes registration, and `015C` is B-field data |
| Current startup replay | Minimal G5 uses radio-on/open-registration only | S later replays `0153`, `0169`, `0157` from a 07.56 capture, then radio-on. Its comments explicitly leave the 07.56 mechanism unknown. G run 4 did not establish those mails as the ringing fix |
| `0202` / `017A` | H and CS retain “trigger signalling” and a separate release declaration; R says append then trigger | G's later `[CR A-0065]` identifies `0202` as immediate teardown; `[CR A-0070]` supplies release order. Current LINK sends `0244` without the trigger. The helper name/comment is stale; `017A` remains unresolved, not a substitute recipe |
| C-plane ownership | H's old command comment says all DLC is host-side; `AGENTS.md` §1a says MAC and DLC run in chip | Current code plainly performs host Lc/LAPC framing/reassembly and NWK. The exact division of every DLC function remains **unknown**; describe the concrete interface instead of claiming either broad statement is complete |
| `022B` capability interpretation | A general capability bitfield or longer payload would enable missing capabilities | M's measured short/long comparison does not support that; later `[CR A-0065]` says `41` is an exact enable token. Retain measured a44/registration behaviour, not arbitrary capability control |
| `0243` | Fixed `29` tag, connection parameter selector, bearer heartbeat or audio readiness | H records later `[CR A-0072-2]`: numeric link-control inputs, no special `29` branch, no audio gate |
| Subscription writes | Early R proposed `F605`; later R/S comments say `017C` is never sent | Current ordinary path is `0247`, `020A`, then `017C` in `send_subscription_tail()`. The old “never sent” comment is stale |
| Subscription flag “fixes” | G runs 5–8 predicted flag/config/mask changes would cure A540 refusal | Their recorded results still refused. Runs 14–19 establish transaction value 0 and audible alerting via `SIGNAL`; keep chip field findings without the disproven causal claims |
| Seventh subscription experiment | C proposes writing a seventh slot | S cites `[CR A-SubscriberCapacity-7]` and rejects out-of-range `020A`; adjacent-storage overwrite risk supersedes that proposal |
| `0242` as success | Channel-report receipt proves the assignment landed | Later `[CR A-0075]` and no-act examples refute this. It proves processing, not successful routing |
| `0230` field names | Low nibble is a channel ordinal; final byte is simply a codec ID | `[CR A-0124]`/T and `xlane_bind()` use one-hot bits `1,2,4` for channels `1,2,3`. Full layout is handle/flags/mask/master/extra; CLI names do not establish semantics |
| `022D` and audio release | Early G says never send `022D`; `0233` is one byte or unnecessary | Current encoders and measured later recipes retain `022D {h,00}`. `0233 {h,01}` follows `[CR A-0070]`. A no-`022D` narrowband test was explicitly left unrun in G run 26 |
| `0251` parameter | Called gain, later packet-loss concealment threshold | S's later default comment still says DSP semantics are unproven. Preserve selector/value and `2B` provenance; units remain **unknown** |
| PCM format / second assignment | Early G headings alternately declare coded G.722 or linear PCM universal; a delayed reassign is mandatory | Later measured recipes distinguish NB linear PCM from WB coded G.722. G runs 26–28 remove the NB reassign; B tests 10–11 demonstrate WB on 07.56 |
| GenDSP route numbering | P names PCM 4..6, silence 12, test tone 15; G discusses silence 21 and wider ranges | `0A` envelope is known; a complete build/mode-specific route map is **unknown**. A route command is not a substitute for the measured call bind |
| GenDSP loop encoding | P describes `09 + u32 LE`; later S sweep emits `09 + u8` | Both encodings are documented as sent; equivalence and accepted short-form contract remain **unknown** |
| Page first byte | `0C/2C` was treated as an on-air NB/WB selector | Later `[CR A-0076]` says it is ignored. Current code retains derivation, which does not restore the claimed on-air effect |
| Page identity | Old examples equate page suffix, slot and link handle | Current `ring_target_tpui()` targets the called subscription. G run 32 measured the wrong-handset page when link-adopted identity was used |
| Page/header construction | `[CR A-0076]` describes page-builder writes; later `[CR A-0109]` says the examined table is built at bring-up and unchanged by paging | S startup comment preserves the later finding; exact build-specific table lifecycle remains **unknown**. Do not use the old address narrative as a portable page confirmation |
| `F625` byte 5 | Called codec mode, 64-byte mode or long-slot flag | D's later `[CR A-0089]` says it encodes record bit 1 and does not carry slot width; DIAG preserves it as `b5` |
| `F625` counters | D calls counters unnamed; bytes 38..44 are zero; uptime is only 16 bits | Later `[CR A-0123]`, H API 26 and DIAG name counters, distinguish RXCOUNT/CONNECTTIME, and decode 32-bit uptime. BER remains undecoded. Zeros in a short corpus are not reserved-field proof |
| Memory addresses | UAK read-back on 07.56 used the 08.25 address | G runs 21–22 retract the mismatch inference. Current code skips known-other-build reads; unknown-build addresses are still unverified |
| Concurrent-call count | C leaves roughly three versus six unresolved | T `[CR A-0124]` answers three voice channels and their mapping. This is **answered**, not a completed three-call openDECTcell hardware test in the cited snapshot |
| Image descriptor / version TLV | Earlier notes locate descriptor at `0006`; F's early table leaves tag 9 open | I §§4–5 specifies a six-byte descriptor at address 0 and identifies tag 9 as version information; its absence on 08.25 is not an error |

### Open questions: all unresolved catalogue fields

The following inventory includes the unknowns in the main and subsidiary tables. No unknown byte should be filled from an example value alone.

| Primitive(s) | Still unknown |
| --- | --- |
| `0152`, `0154`, `0156`, `015A`, `0165`, `017B`, `020C`, `020E`, `024E`, `0252`, `F603`, `F606`, `F60F`, `F611`, `F613`, `F627` | Whether each historical receiver candidate crosses the chip boundary, direction/routing, payload, semantics, actual reply relationship and supported builds |
| `0153`, `0157`, `0169` | Complete parameter maps and chip-side effects on 07.56; allowed values; build-dependent differences |
| `0158`, `0159` | Direction, complete layouts and extended-data semantics; meaning of the known-width fields in `0158` |
| `015C`, `015D` | Full tag/B-field envelope offsets and confirmation layout; precise supported-build coverage |
| `015E` | Complete layout, bearer-type offset, queue-management contract and any legitimate ordinary-call use; superseded boot-order guesses do not answer this |
| `015F` | Wire layout, precise admission namespace bounds and applicability beyond the documented extended/ULE mechanism |
| `016A` | Isolated hardware proof of requested handover; prerequisites, result indication and cross-build behaviour |
| `017A` | Whether the legacy declaration denotes a valid chip command; reason map and relationship to documented `0202` teardown |
| `017C` | Complete mask policy, all bit meanings and any genuine reverse-direction form; default/example masks are not a full specification |
| `01FE` | Debug command enumeration and both parameter meanings |
| `0200`, `0201` | Complete event flag/optional-field maps and lengths beyond recorded examples; `0200` byte 1 is not established as an audio-readiness test |
| `0202` | Resolution of retained trigger/release declarations across builds; no evidence here justifies append-then-teardown as ordinary signalling |
| `0203` | Meaning of bytes 7–8 and optional 13–14 trailer, complete tag interpretation, allowed variants and per-build coverage |
| `0204` | No positive command contract; later evidence explicitly says not a command |
| `0208` | Selector-to-connection mapping, full configuration-byte meanings, input-half choice, representation assumptions, long-form status bytes, activation/confirmation and any real C→H form |
| `0209` | Payload, task routing, whether every cited occurrence is a transported chip indication, and precise trigger conditions |
| `020A`, `0247` | Complete config/flag enumerations, remaining flag bits, per-build record addresses; historical reverse-direction `020A` listing remains unresolved |
| `022B`, `022C` | General trailing-data contract of `022B`, exact relationship between configuration-token handling and advertised bits across builds, and complete zero-mode behaviour of `022C` |
| `022D`, `0230`, `0233`, `0242` | Unenumerated flags, all master/extra combinations, short report variants, dependable applied-state confirmation and cross-build release coverage |
| `0243` | Units and complete meaning of the two numeric link-control inputs |
| `0248` | Complete peer/repeater configuration payload, trial-byte meanings and prerequisites |
| `0250` | Parameter meanings, timer units and any proven tone/ring use |
| `0251` | Physical meaning/units of the stored value, selectors beyond demonstrated channels and build-specific effects |
| `0300` | Optional fourth-byte contract and unmentioned bits; build-specific header/table lifecycle; behaviour outside documented TPUI examples |
| `0E0D`, `F604` | Complete short-access payloads, identity content, timing and suitability for just-in-time subscription loading |
| `F601`, `F602` | Complete identity/capability field maps, prefix interpretation and exact beacon/TPUI-field offsets; variable values must not be inferred from private identities |
| `F605` | Auxiliary-field and flag meanings, other sub-operations and exact build coverage |
| `F618` | Meaning and endianness, if numeric, of its two parameter bytes |
| `F623` | Full field map, first/trailing fields, proof of per-carrier interpretation, scale and per-build equivalence |
| `F624`, `F625` | Other request selectors; slot versus twice-slot numbering; bytes 4/44; higher-level meaning of byte 5; HANDOVER increment condition; BER encoding/units; field equivalence between builds |
| `F626` | Full channel-configuration/parameter semantics, reliable effect verification and 07.56 behaviour |
| `FFF1/01`, `/04`, `/05`, `/08`, `/09`, `/0A`, `/0C`, `/0D` | Complete fill effects; periodic-status fields; keep-alive contract; memory-reply length semantics; short/full loop equivalence; route map; report/subcommand semantics and missing directions/build coverage |
| `FFF1/07` | Build-specific readable address maps and validity of all addresses outside those explicitly established; wire-layout knowledge does not validate memory contents |
| `FFF2` | Exact string-length/termination contract and coverage across builds |

Additional open items: tag-7 bit meanings; TLVs 2–4, 6 and 10; the two-byte numeric interpretation of tag 2; other boards' factory locations; the exact factory-test patch-disable selector; and the detailed host/chip DLC split. Channels 2–3 and a three-call openDECTcell run still need hardware results attached to a named build. [I §6; F §9; T test plan; H/CS/LINK ownership discrepancy above]

This catalogue specifies the complete observed mail header. It does not claim a complete chip command set or a fully decoded payload for every known ID.
