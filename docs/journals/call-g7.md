# G7: a handset rings and talks (minimal Call Control + audio)

**Status: GATE PASSED, 2026-10-19.** A handset rings, is answered, and audio
passes in both directions.

This document is the running record of getting there, oldest first, including
the readings that turned out to be wrong and the corrections to them. It is
long, so the conclusions are collected here; the dated sections below are the
evidence for them.

## What is true, as of the end of 2026-10-19

1. **`0x0230` does not put decoded PCM on the TDM slot. It puts the call's
   coded G.722 bearer there** — one 16-bit word every *two* 8 kHz frames, so
   4000 distinct words per second = 8000 bytes/s = 64 kbit/s. This is the
   single fact the whole gate turned on. Every test tone before it wrote A-law
   or linear PCM into a slot carrying coded bits, which is why none was ever
   audible, and it retired the companding, byte-lane, 8-versus-16-bit and
   two-slot questions at a stroke.
   - Read a capture: `tools/dect_g722_extract.py call.raw --wav out.wav`.
   - Transmit: each coded word into two consecutive frames (`-a play=FILE`,
     `-a loopback`).
2. **The audio bind is `0x0251` then `0x0230`, and nothing else.** The chip
   assigns the channel itself and reports it in `0x0242`; the mode nibble is
   not ours to choose. `0x022D` is a mute request, not an enable. `0x0251` is
   a packet-loss concealment threshold, not gain.
3. **The lane is 16-bit linear and reaches both rails.** On that lane,
   `0x0000` means routed-but-silent and `0xFEFE` means unrouted.
4. **Incoming calls**: page with `0x0300 {0x2C, 0x11, slot}`, where the last
   two bytes are the low 16 bits of the TPUI our own LOCATE-ACCEPT assigns;
   then an FP-originated CC-SETUP carrying PORTABLE-IDENTITY, FIXED-IDENTITY
   and BASIC-SERVICE. A paged handset answers in about 200 ms.
5. **Working command line** for a call with audio:
   `-g -P 0000 -C -a flags=0x41,mask=0x0002,status=8000,dspsweep=6:30000,loopback`.
   Park afterwards, always.

## What is still unknown

- **What wires the chip's audio route.** It comes alive only when the chip
  re-establishes the connection *after* the channel is assigned. The recipe
  above is a recipe, not an explanation. `-a handover=MS` sends `0x016A`
  (`a stock handover hook`) to provoke it and is **still untested**,
  because in both runs that used it the chip re-established by itself first.
- **The chip's uplink bearer underruns**, about three short stalls per second
  on DECT frame boundaries. Proved not to be ours: the driver reports
  `dropped 0` every run, and a locally generated file plays cleanly to the
  handset.
- A **host-side G.722 codec** is not written. Audio today is echoed or
  replayed from a file.

**Q-0027** asks the clean room about all three, and leads with the one that
changes the most: whether the stock system's TDM slot carries the coded
bearer as ours does, or decoded PCM. If decoded, there is a mode we are not
selecting and no codec is needed at all.

---

## Original plan (2026-10-19, superseded by the record below)

G6 (registration) is finished on hardware; the FRITZ!Fon C4 enrolls, holds
standby, and already *initiates* a call when you dial (the bearer comes up and
drops after ~3 s for lack of call handling and audio). Sources are public ETSI
(CC is a standard protocol) and the clean-room `COMMAND_SET.md` (STUB audio
primitives); no AVM material was read.

## The gate

ROADMAP M7 / Gate G7: **"the handset rings, you answer, and audio passes (tone or
echo)."** Minimal Call Control in `libopendectcell` plus binding a connection to a
voice slot.

We tackle it in two milestones, **outgoing first** (lowest risk — the handset
drives it), then incoming:

- **Milestone A — outgoing dial + audio.** You dial on the handset; we accept the
  call and loop/echo audio back. Proves the whole C-plane + U-plane path with the
  handset doing the initiating.
- **Milestone B — incoming ring.** We page the handset so it *rings*; you answer;
  audio passes. This is the literal gate wording and reuses everything from A.

## Two planes

A DECT call runs on two parallel planes over the same connection (handle):

1. **C-plane — Call Control (CC).** A NWK protocol, like MM but for calls, carried
   over the same DLC/LAPC/Lc/Cs path we already have. New module `dect_cc`.
2. **U-plane — audio.** The voice bearer: we bind the connection's handle to one
   of the chip's six DSP audio channels and route it to a PCM voice slot, then
   capture/play A-law samples on `/dev/dect0`.

Both must reach "active" for the handset to pass sound.

### NWK protocol dispatch (prerequisite)

Today the daemon feeds every reassembled NWK message to `dect_mm`. CC messages
carry a different protocol discriminator, so the C-plane path must dispatch by PD:

- `PD = MM (0x5)` -> `dect_mm` (existing)
- `PD = CC (0x3)` -> `dect_cc` (new)

`dect_nwk_parse()` already returns the PD; the daemon's `reg_segment()` (rename to
something call-agnostic, e.g. `cplane_segment()`) routes on it.

## What we already have

- **DLC/MM/registration** (G5/G6): the link, reassembly (hardened), and the
  subscription are done. The handset is enrolled and reachable at the DLC level.
- **Audio U-plane, largely working** (G4/M4): PCM voice slots are captured on the
  host, the sample format is 8-bit A-law in the slot's low byte, and **loopback
  works on the bench** (the GenDSP connect/loop mails from `[CR A-0004]`): a
  transmitted tone returns in the mapped receive slot. `dect_audio.{c,h}`
  (tone analysis, A-law decode) and `dect-pcmcap` exist.
- **The STUB audio commands** are documented in the clean room (below).

So G7 is mostly the **CC protocol** (new, but public-spec) plus **wiring the
existing audio path to a live connection**.

## Call Control module (`dect_cc`)

Same shape as `dect_mm`: transport-agnostic (feed decoded NWK messages in, pull
NWK messages out), unit-tested, no I/O. Specification: ETSI EN 300 175-5 §9,
EN 300 444 (GAP). Message types (PD = CC = `0x3`; values cross-checked against
libdect, to confirm against ETSI while coding):

| Message | Type | Direction (our FP view) |
|---|---|---|
| `CC-SETUP` | `0x05` | PP -> FP (outgoing) / FP -> PP (incoming) |
| `CC-SETUP-ACK` | `0x0d` | FP -> PP (ask for more digits) |
| `CC-CALL-PROC` | `0x02` | FP -> PP (call proceeding, en-bloc) |
| `CC-ALERTING` | `0x01` | either (ringing) |
| `CC-CONNECT` | `0x07` | either (answered) |
| `CC-CONNECT-ACK` | `0x0f` | either |
| `CC-RELEASE` | `0x4d` | either |
| `CC-RELEASE-COM` | `0x5a` | either |
| `CC-INFO` | `0x7b` | either (more digits / display) |

Key IEs (EN 300 175-5 §7.7): BASIC-SERVICE (`0x02` — basic speech), PORTABLE- and
FIXED-IDENTITY (`0x05`/`0x06`, as in MM), CALLED-PARTY-NUMBER / MULTI-KEYPAD
(the dialled digits), RELEASE-REASON, and the CC-specific CALL-ATTRIBUTES.

FP-side state machine (subset of the EN 300 175-5 §9.3 states we need):
`NULL -> {CALL_INITIATED | CALL_PRESENT} -> ... -> ACTIVE -> RELEASE_PENDING ->
NULL`.

### Milestone A — outgoing call flow (the handset dials)

```
PP -> FP  CC-SETUP        BASIC-SERVICE=speech, PORTABLE-IDENTITY, called number
FP -> PP  CC-CALL-PROC    (accept en-bloc; or SETUP-ACK + INFO for overlap dialling)
FP        << bind audio: assign channel, path on >>   (see U-plane below)
FP -> PP  CC-CONNECT      we "answer" immediately (we are the far end for the test)
PP -> FP  CC-CONNECT-ACK
--- state ACTIVE: audio passes (loop/echo) ---
either    CC-RELEASE / CC-RELEASE-COM   on hang-up or our timer
```

For the first test we shortcut the far end: on `CC-SETUP` we send `CALL-PROC`
then `CONNECT` right away and loop the audio back, so a dial produces audible
echo/tone instead of dropping after 3 s. Overlap vs en-bloc dialling and the
exact called-number IE are the main things to pin down against the C4's real
`CC-SETUP` (capture it first, as we did for the MM messages).

### Milestone B — incoming call flow (make it ring)

```
FP        page the handset   (0x0300 PAGE_HANDSET by handset number/TPUI) [CR COMMAND_SET §6.3]
PP -> FP  (connection set-up by the paged PP)
FP -> PP  CC-SETUP           BASIC-SERVICE=speech, our identity, (ringing cause)
PP -> FP  CC-ALERTING        handset is ringing
--- user answers ---
PP -> FP  CC-CONNECT
FP -> PP  CC-CONNECT-ACK
FP        << bind audio >>
--- ACTIVE: audio passes ---
```

Paging relies on the handset being recognised, which ties back to the G6 chip
subscription (`0x020A`) and the handset number/TPUI space. Whether our
subscription store makes the C4 answer a page is an **open hardware question**
(and a good check of whether the `status`/`extra` bytes we guessed in G6 matter).

## U-plane: binding a connection to audio `[CR COMMAND_SET §6.5]`

Six DSP audio channels (1..6) connect a radio connection (by handle) to the
DSP/codec/PCM side. To make a connected call carry voice:

1. **Assign a channel:** `0x0230 ASSIGN_AUDIO_CHANNEL {handle, chan=1, ...}`.
   Reply `0x0242 {handle, chan_flags, chan_mask}` confirms the channel.
2. **Turn the path on:** `0x022D AUDIO_PATH_ON_OFF` for that connection.
3. **Route the DSP path:** `0xF626 SET_AUDIO_ROUTE {..., chan_cfg}` (off / mode 1 /
   mode 2).
4. Voice then flows as 8-bit A-law in the channel's PCM voice slot; we read/write
   it on `/dev/dect0` using the existing voice-slot map and `dect_audio`.
   **Measured: channel 1 is TDM slot 0** (see the voice-slot probe result below).
5. **Release:** `0x0233 RELEASE_AUDIO_CHANNEL` + `0x022D` off on hang-up.

First audio target is a **loopback/echo** (route the received voice slot back to
the transmit slot, as the M4 bench loop already does) or a **test tone**
(`0x0250 START_CHANNEL_TIMER` is a candidate per-line tone/ring action — unproven).
Real telephony audio (SIP bridge) is the controller's job, later.

## Clean-room status and gaps

- **CC protocol: public.** ETSI EN 300 175-5 / EN 300 444 — implemented from the
  spec like MM/DLC/DSAA. No clean-room dependency.
- **STUB audio commands: clean-room, already documented** in `COMMAND_SET.md`
  §6.5 (the `0x0230`/`0x0242`/`0x022D`/`0xF626` set) and proven in part by the M4
  loopback.
- **Gap — real-voice PCM transport.** Robust production voice (vs. the bench
  loopback) touches the vendor's PCM driver PCM/DMA/TDM details analysed in A-0020/A-0021.
  Those are **hardware-analysis** reports; their clean-room *handoff* files are **not in
  the clean room yet**, so we do not implement from them. The loopback/tone path
  is enough to pass G7; request the handoffs when we need production voice.

## Open questions

- The C4's exact `CC-SETUP`: overlap vs en-bloc dialling, and the called-number IE
  encoding. Capture it first (unfiltered NWK log, like the MM bring-up).
- Does paging (`0x0300`) + our G6 subscription actually make the C4 ring, or do
  the `status`/`extra` bytes (sent as `0x00`) need a real value for reachability?
- Which audio-channel mode / codec byte the C4 expects for basic speech (the
  `0x0230` codec field, default 0).
- ~~Which TDM slot carries a call's audio~~ — **answered**: channel 1 → slot 0.
- Whether the STUB test tone / `0x0250` can produce audible output now that the
  MAC is up (it was silent pre-MAC in M4).

## Hardware capture — outgoing call works at the C-plane (2026-10-19)

A registered FRITZ!Fon C4 dialled `12345`; the full CC exchange now decodes and
the handset shows the call connected ("12345" with "HD"). Decoded (identities
redacted):

```
C4 → us  03 05  05 07 <IPUI>  06 07 <PARK>  e0 88    CC-SETUP, BASIC-SERVICE e0 88
us → C4  83 02                                       CALL-PROC
us → C4  83 07                                       CONNECT
C4 → us  03 7b  2c 05 31 32 33 34 35                 CC-INFO, MULTI-KEYPAD "12345"
C4 → us  03 4d  e2 00                                CC-RELEASE, RELEASE-REASON 0
us → C4  83 5a                                       RELEASE-COM
```

What this taught us:

- **The LSIG fix was the unlock.** Registered connections sign Lc with
  LSIG = low 16 bits of the PMID (the assigned TPUI, `0x1101` here); learning it
  made the whole call decode and also ended the post-registration churn.
- **BASIC-SERVICE `e0 88`** = call class 8, service 8 = **wideband speech (HD)**.
  The C4 defaults to HD. Our audio path so far is narrowband A-law (M4); codec
  negotiation / wideband is an open item.
- **Overlap dialling.** The C4 sends `CC-SETUP` *without* the number, then the
  digits in a `CC-INFO` **MULTI-KEYPAD** IE (`0x2c`, ASCII). A cleaner FP would
  answer `SETUP-ACK` to invite digits; our immediate `CALL-PROC`+`CONNECT` still
  got the handset to show the call connected.
- **It released itself** after a few seconds with *"Unverschlüsselte Verbindung
  abgebaut"* ("unencrypted connection released"): no audio was bound and the call
  may also want DSC ciphering. Which it is will become clear once audio is bound.

## U-plane bind — exact mails `[CR COMMAND_SET §6.5]`

- `0x0230 ASSIGN_AUDIO_CHANNEL {handle, chan_flags, [chan_mask LE], [codec]}` —
  chan_flags low nibble = channel 1..6. Reply `0x0242 {handle, chan_flags,
  chan_mask}`.
- `0xF626 SET_AUDIO_ROUTE {handle, enable≠0, chan_cfg (low nibble = channel),
  param LE}` — programs the DSP path.
- `0x022D AUDIO_PATH_ON_OFF {handle, flags}` — flags bit 7: 0 = connect, 1 =
  disconnect. On disconnect the voice buffers fill with `0xFE`.
- Release: `0x0233 RELEASE_AUDIO_CHANNEL` + `0x022D` disconnect.

## Hardware result — audio binds, but the call needs encryption (2026-10-19)

Binding the voice channel works: on the dial, `0x0230 {01,01}` was answered by
`0x0242 {01, 01, 00, 00}` — channel 1 assigned. But the C4 **still** released the
call with "Unverschlüsselte Verbindung abgebaut". So the teardown is not "no
audio": **the C4 requires DSC ciphering for voice** (CAT-iq enforces encryption).

### Hardware result — ciphering negotiated, DCK verified (2026-10-19)

With the corrected `CIPHER-INFO` and call-time authentication wired:

```
C4 → us  03 05 ...                                   CC-SETUP
us → C4  83 02                                       CALL-PROC
us → C4  05 40 0a 03 01 18 10 0c 08 <RAND_F> 0e 08 <RS>   AUTHENTICATION-REQUEST
C4 → us  85 41 0d 04 <RES>                           AUTHENTICATION-REPLY
us → C4  83 07                                       CONNECT
us → C4  05 4c 19 02 81 98                           CIPHER-REQUEST
```

- **The DCK derivation is verified on real hardware.** The C4's `RES` matched our
  `A12(A11(UAK,RS),RAND_F)`, so both sides derived the same DCK. The stored UAK,
  the AUTH-TYPE (`01 18 10`, UAK #8) and our DSAA are correct.
- **The "unencrypted connection" complaint is gone**, and the call now holds
  **~17 s** instead of ~3–5 s. The handset shows "12345 / HD" and returns to the
  home screen cleanly.
- **The C4 no longer sends `CC-RELEASE`**: the bearer itself drops, which points
  at a lower-layer timeout (no audio, or the radio cipher never engaging) rather
  than a call-control hangup.
- The MM authentication naturally sequences **before** `CONNECT`, because the
  shared drain empties the MM queue ahead of CC — which matches the expected
  order.

Remaining candidates for the ~17 s drop: **(a)** nothing is fed into the bound
voice channel, **(b)** the radio cipher never actually switches on because the
`0x0208` chip enable is withheld. (a) is safe host-side work and needed for G7
anyway, so it is tried first.

### Hardware result — voice slot identified, and (a) is ruled out (2026-10-19)

A read-only probe (`voice_slot_probe()`) counted, for every TDM slot in the
receive block, how often its 16-bit sample changes from one frame to the next,
and printed a per-slot rate once a second while a call was bound. Two dialled
calls gave the same picture:

```
control slot found: tx 1, rx 1
call: slot activity (changes/s): 0:7954 1:114 2:0 3:0 4:0 5:1890 6:0 7:0   (call up)
call: slot activity (changes/s): 0:   0 1:  0 2:0 3:0 4:0 5:1831 6:0 7:0   (call gone)
```

- **The call's audio is on TDM slot 0.** At ~7950 changes/s out of 8000 frames/s
  essentially every sample differs, which is speech; it drops to exactly `0` the
  moment the bearer goes down. So **chip audio channel 1 (`0x0242 … 01 01`) maps
  to TDM slot 0** — the mapping the U-plane section lists as open.
- Slot 1 is the C-plane control slot (tx/rx 1, matching the probe result) and only
  ticks with signalling traffic.
- Slot 5 churns at ~1850/s continuously, including with no call at all, so it is
  unrelated background, not a second voice channel.

> **Corrected on 2026-10-19 — see "the voice slot carried ciphertext" below.**
> The full-rate activity in slot 0 was the *ciphered* bearer, not voice. The
> conclusion that the handset delivers audio into the bound channel does **not**
> hold. The call-timeline and C-plane findings in this section are unaffected,
> because they rest on LAPC evidence rather than on the slot contents.

**Candidate (a) looks dead.** The handset appears to be delivering voice into
the bound channel; the connection is not starving for audio. The timeline reads:

| event | call 1 (ms) | call 2 (ms) |
|---|---:|---:|
| audio channel bound | 3176 | 17140 |
| `CIPHER-REQUEST` sent | ~3600 | ~17600 |
| **chip reports link gone (`0x0201`)** | 12896 | 27900 |

After `CIPHER-REQUEST` the C4 sends **nothing at all** — no `CIPHER-REJECT`, no
`CC-RELEASE` — and then roughly ten seconds later the **whole MAC link** goes
down, not just the call. That is the GAP cipher-switching timeout: per
EN 300 175-5 a successful switch has no NWK response, the confirmation *is* the
MAC changing to ciphered operation. We announce ciphering and never deliver it,
so the handset abandons the link. The earlier *"unverschlüsselt"* text is absent
now precisely because we claim encryption rather than omitting it.

**So the blocker is (b): the `0x0208` chip cipher-enable.**

#### `0x0208` is implemented, but opt-in only (`-k`)

`send_chip_cipher()` builds the mail from the A-0027 Q1 layout and is sent right
before the `CIPHER-REQUEST` goes over the air (A-0027 Q5 orders key loading and
cipher negotiation as separate steps, in that order). It is **never sent unless
`-k SEL[,rs][,33]` names a selector**, because two things remain guesses:

- **Which bytes are the DCK.** We put the DCK in the 8-byte field (offsets
  15..22) and challenge + verified `RES` in the 12-byte field (offsets 3..14,
  `RAND_F||RES` by default, `RS||RES` with `,rs`), and leave the EEPROM field
  zero. `RAND_F(8) + RES(4) = 12` fits the field exactly, which is suggestive but
  not proof. A-0027 explicitly declines to map the DCK to offsets, so the daemon
  must not pick a layout on its own.
- **Which selector.** Low selectors address a link/terminal number; `7..12` are
  the subscription-record selectors, so a wrong guess there can overwrite the
  handset's stored record. Since the host-side store (`-s`) now holds the UAK,
  such damage is repairable by restarting the daemon — but low selectors are
  tried first regardless.

The mail's parameters are never logged: they carry the cipher key.

### Hardware result — the PP ciphers, we do not (2026-10-19)

Three dial cycles with `-k 1`, `-k 1,rs` and `-k 1,33`. The decisive observation
is not whether the call survived but **what happens to the inbound C-plane the
instant the `CIPHER-REQUEST` goes out**:

```
before:  reg: cs in 13 21 01 ... lc_rc=3     valid LAPC frames
after:   reg: cs in 29 36 8d ... lc_rc=0     nothing decodable
         reg: cs in 77 90 56 ... lc_rc=0
         reg: cs in 6f 28 41 ... lc_rc=-1    checksum failure
```

Counted over one call: 14 fragments before the request, 5 of which decode; 18
fragments after, of which **not one** decodes. The bytes are high-entropy.

**The C4 switches its transmitter to ciphered operation as soon as it receives
our `CIPHER-REQUEST`** — which is exactly what EN 300 175-5 prescribes, and
confirms our MM layer, AUTH-TYPE, CIPHER-INFO and DCK are all correct. Our radio
never follows, so the two sides go deaf to each other: we retransmit the
`CIPHER-REQUEST` in plaintext once a second (eleven times, `addr 13 ctrl 02`),
it is never acknowledged, and the MAC link times out ~10 s later. That fully
explains the drop, including why no `CC-RELEASE` ever arrives — by then the
handset cannot hear us at all.

**All three `0x0208` variants were inert.** The logs with `-k` are
indistinguishable from the baseline without it: same garbage after the request,
same 1 Hz retransmits, same timeout. The chip also sent **no error mail** in
reply, so we cannot tell from the outside whether it rejected the mail, ignored
it, or accepted it without acting. Conclusions:

- Selector 1 with the DCK at offsets 15..22 does not switch the radio, in either
  the 30- or the 33-byte form, with `RAND_F` or `RS` in the 12-byte field.
- Blind layout search has low yield from here: the outcome is binary and silent,
  and the hypothesis is compound (selector × field mapping × whether `0x0208`
  even is the enable — A-0027 never established that last point).

**Better detector for any future attempt:** "does any valid LAPC frame arrive
after the `CIPHER-REQUEST`?" It answers within ~2 s instead of waiting out the
10 s timeout, and it distinguishes *the chip did something* from *the chip did
nothing*, which call-survival alone cannot.

**Open and now sharper:** whether `0x0208` is the enable at all. A-0027 says a
definitive STUB `ENABLE_DSC` flag was never isolated. Our evidence is consistent
with `0x0208` being key *material* only, with a separate start trigger (or a
per-connection flag in `0x0203`/`0x0200`) still missing. Q-0016 Q3 asks exactly
this; the addendum below carries these results into it.

### A-0028 — the layout was wrong, and in a specific way

A-0028 traced `0x0208` one stage upstream, to the authentication worker that
builds the 47-byte internal event feeding it, and that **refutes the layout the
three cycles above tested**. Field provenance, not guesswork:

| mail offset | provenance `[CR A-0028 §1]` | our mapping |
|---|---|---|
| `3..10` | 8-byte *computed* result, byte-reversed as a group before the event | **DCK** (candidate) |
| `11..14` | 4-byte *computed* result, likewise byte-reversed | **RES** |
| `15..22` | last 8 bytes of a 16-byte *input*, copied intact | **RAND_F** (or RS) |
| `23..25` / `26..27` | zeroed / `FF FF` | as before |
| `28..29` | EEPROM `0x51`/`0x52`, **defaults `00 00`** | zeros are fine |

Two corrections matter:

1. **`15..22` is an input, not a derived value.** Putting the DCK there — which
   is what all three cycles did — contradicts the producer. The computed results
   live at `3..14`, and the DCK candidate is `3..10`.
2. **The computed results are byte-reversed** by the worker before the event,
   and the event→mail copy adds no further transformation. So the DCK and RES go
   on the wire reversed.

Mapping onto DSAA1: the call-time calculation's two 8-byte inputs are `RS` and
`RAND_F`, so the 16-byte input is that pair and its last half is `RAND_F`. The
mail is therefore `DCK(8, reversed) ‖ RES(4, reversed) ‖ RAND_F(8)`. Our DSAA
already matches the public DCK test vector (`tests/test_dsaa.c:94`), so A-0028
§6.1's vector check is satisfied for the abstract key; only the wire order was
open, and A-0028 answers it.

`send_chip_cipher()` now builds exactly this. `-k SEL[,rs][,norev][,33]` keeps
the two residual unknowns switchable: `,rs` if the 16-byte input's halves are
the other way round, `,norev` if the reversal does not apply to our
representation.

**Two cautions carried forward from A-0028:**

- **Selector is still unknown, and low selectors are no longer "the safe ones".**
  The live-call branch takes its selector from the authentication event's routing
  nibble, which is *not* proven equal to our MAC handle, and A-0028 withdraws any
  guarantee that selectors `0..6` are volatile-only. Neither family is certified
  harmless without a consumer audit. It does note the state lives in RAM-loaded
  firmware, so persistence across a chip power cycle is unproven either way.
- **`0x0208` may still not be the enable.** Q3 remains unresolved: it is not safe
  to conclude that a queued `0x0208` means cipher on, and no separate
  radio-cipher-on primitive or confirmation bit has been isolated. A perfect
  layout may therefore still produce no visible change.

### What A-0027 established

- **`CIPHER-REQUEST` is exact:** AVM's over-air content is `4C` + CIPHER-INFO IE
  **`19 02 81 98`** — `0x81` enables the standard cipher, `0x98` identifies the
  **DCK / key number 8**. (That key number corroborates the `0x18` = UAK #8 second
  octet in AUTH-TYPE.) The preceding PD/transaction octet is taken from the active
  MM transaction, not hard-coded.
- **`0x0E04` is host-local only** (AVM host task `0x03`, its DLC handler) and
  carries the MM cipher request. It is **not** a chip command — never send it to
  task 2. This corrects A-0014's open point.
- **The chip security command is `0x0208` to task `0x02`**, emitted on AVM's
  internal authentication-result event, **30 or 33 bytes** incl. the 2-byte
  primitive: selector at offset 2 (link/terminal number, or **terminal slot + 7**
  matching the `0x07..0x0C` subscription-record selectors), 12 + 8 bytes of
  security material copied from the authentication result, 3 zero bytes,
  `FF FF`, 2 EEPROM config bytes, and in the 33-byte form a trailing `0x40` plus
  two per-terminal status bytes.
- **Still unknown — and the reason `0x0208` stays behind `-k`:** *which* 8 of those 20
  security bytes are the 64-bit DCK, whether byte order is transformed, and
  whether a later MAC/DIP step is also needed. A-0027 warns against labelling a
  guessed offset "DCK". Worse, `0x0208` selectors `7..12` **write subscription
  records**, so a malformed payload could corrupt the handset's subscription.
  Resolving this needs a redacted stock-FRITZ!OS capture or a STUB-side trace.
- **Capability:** `0x0157` restrict=0 (full profile, incl. a37 "standard ciphering
  supported") is the **power-on default**, so our base should already advertise
  a37. The derived **a42** additionally needs `0x0169` bit 1 *and* per-subscription
  `0xF605` flags; A-0027 says do **not** force that for a normal C4, and whether
  the C4 requires a42 is unverified.

So the G7 blocker is **DSC (DECT Standard Cipher)**, which has three layers
(A-0014 F7, refined by A-0027):

1. **MM negotiation (over-air, public ETSI):** authenticate the PP at call time
   (FP-initiated AUTHENTICATION-REQUEST using the stored UAK, deriving a fresh
   DCK), then `CIPHER-REQUEST` (`0x4c`). We can build these.
2. **Chip cipher-enable (STUB, unknown):** load the DCK and turn on DSC on the
   connection. A-0014 F7 found AVM's *host-local* `0x0E04` (to host task `0x03`)
   but the actual chip/radio primitive is **not established** — asked in Q-0015.
3. **Capability:** the base must advertise "standard ciphering supported" (a37);
   check whether our bring-up sets it (via `0x0157`/`0x0169`).

Deferred until the call holds (encrypted):

- **Wideband vs narrowband:** the C4 asks for HD (service 8). Which `0x0230`
  codec byte selects narrowband so our A-law path matches, or must we carry G.722?
- **Echo vs tone:** cheapest "audio passes" proof once the call stays up.

## First implementation steps (Milestone A)

1. `dect_cc.{c,h}` — CC message codec (header + IEs) and a minimal FP state
   machine, with host unit tests (mirror `dect_mm`/`test_mm`).
2. NWK dispatch in the daemon: route by PD to `dect_mm` or `dect_cc`.
3. Capture the C4's real `CC-SETUP` on a dial (add CC logging; one hardware run).
4. Answer the call (`CALL-PROC` + `CONNECT`) and bind audio (`0x0230` + `0x022D`
   + `0xF626`), looping the voice slot back.
5. Hardware test: dial on the C4, confirm the call stays up and audio echoes.

### Hardware result — the voice slot carried ciphertext (2026-10-19)

A capture of the bound call's TDM slot (`-w`) showed **no reading of the bytes
is audio**: adjacent-sample correlation was 0.005..0.086 for every candidate
format, where speech sits above 0.9. Scanning correlation over time found no
window that looked like voice either.

The explanation is that the handset ciphers **the whole bearer, voice
included**, the moment it sees our `CIPHER-REQUEST` — which goes out about
400 ms after the audio binds. We cannot decipher, so from 400 ms on the slot
carries DSC ciphertext. The capture was ciphertext almost end to end.

`-C` was added to suppress call ciphering so the call stays in the clear. With
it, a **15.7 s** bound call produced:

```
call: slot activity (changes/s): 0:0 1:64 2:0 3:0 4:0 5:1866 6:0 7:0
```

and a capture that is **98 % `00 00`** with 82 distinct byte pairs in total and
1–2 zero crossings per second. Slot 1 is the control slot; slot 5 churns at
~1850/s with or without a call and is unrelated background.

**So the earlier conclusion was wrong in both directions:**

- Slot 0 is where the chip delivers the bearer's decoded PCM — that part holds,
  since ciphertext appeared there at the full 8 kHz frame rate.
- But **we have never actually seen voice in it.** "The handset is delivering
  voice into the bound channel" was a misreading of ciphertext.

Two explanations remain, and the captures cannot separate them:

1. **The C4 withholds voice on an unencrypted bearer.** It kept the call up for
   15.7 s and then released it with its unencrypted-connection complaint, so
   refusing to transmit audio meanwhile would be consistent policy.
2. **Our audio path does not actually deliver voice.** `0x0230` is acknowledged
   by `0x0242` reporting channel 1, and `0xF626`/`0x022D` are accepted, but an
   acknowledged setup is not a working path — the same lesson `0x0208` taught.

**What separates them: transmit.** Feed a tone into the slot on an unencrypted
call. If it is audible in the handset, the U-plane works and the uplink silence
is C4 policy (1). If it is not, the path is not carrying audio at all (2). That
work is needed for G7 regardless, so it is the next step either way.

Note that sending needs a continuous transmit stream: `send_mail()` currently
writes only short bursts when there is a mail to send, filling non-control slots
with idle `0xFF`. Carrying voice means writing a block per received block, with
the voice slot filled and mail traffic interleaved.

**Unaffected:** the C-plane diagnosis stands. The link dies because the handset
ciphers, we never follow, our retransmitted `CIPHER-REQUEST` is never
acknowledged, and the MAC times out. That rests on LAPC decode failures, not on
the slot contents.

### Hardware result — "Abhörsicher" explains the drops; audio is the real gate (2026-10-19)

The C4 has a handset setting **"Abhörsicher"** (secure / eavesdropping-proof).
It was on. With it **off**, an unencrypted call **holds indefinitely** — we ran
one for 78 s and ended it manually. No release, no complaint.

That retires the whole ~10 s drop investigation: the handset was enforcing its
own encryption policy. The cipher work (A-0027, A-0028, `-k`) stays valid and we
still want DSC, but **it no longer blocks a working call**.

With the call held open and ciphering suppressed (`-C`), the voice-slot capture
is decisive. Over **78 seconds** it contains **five distinct 16-bit values**:

| pair | count | share |
|---|---:|---:|
| `00 00` | 367045 | 59 % |
| `FE FE` | 257333 | 41 % |
| 3 others | 6 | — |

`COMMAND_SET.md` §9.8 gives **`0xFE` as the idle/mute pattern in voice buffers**,
and §6.5 says the buffers are filled with it **on disconnect**. So:

- We are reading the **right slot** — it is a real voice buffer.
- The chip is filling it with the **disconnected/mute pattern**, not with audio.
- Therefore the audio path **never actually connects**, even though `0x0230`
  returns `0x0242 {01,01,00,00}` and `0xF626`/`0x022D` are accepted without
  error.

That is the same lesson `0x0208` taught: an accepted command is not a working
path. Both of our "it is acknowledged, so it works" assumptions have now failed.

Leads to chase, from `COMMAND_SET.md`:

- **`0x0251 SET_CHANNEL_PARAM`** (per-channel DSP gain) is in §8.4's flow sketch
  — "assign channel, on/off, **gain**" — and we never send it. A zero default
  would explain silence, though not obviously the `0xFE` *disconnect* pattern.
- **DSP routing.** §7.1 documents `0xFFF1` sub `0x0A DSP_ROUTE` (dest 0..17,
  source 0..22, source `0x15` = silence) and sub `0x09` AUDIO_MUTE/RESTORE,
  where `n = 1..6` restores a channel's **default route**. If a call's path
  depends on a DSP route the host must establish, that fits the symptom exactly.
- **Codec sub-mode.** The second `0x0242` carries `chan_flags = 0x40`, and §6.5
  makes bit 6 the sub-mode flag. The C4 asks for HD. A mode mismatch holding the
  path disconnected would tie both observations together.
- **Ordering.** We send `0xF626` between `0x0230` and `0x022D`; §8.4's sketch
  lists `0x0230`, `0x022D`, `0x0251` and does not place `0xF626` at all.

Also unresolved: §9.8's frame sizes (40 bytes per 10 ms standard, 80 wideband,
20 narrow) match none of the **160 bytes per 10 ms** our TDM slot supplies, so
how the two relate — and hence the sample format — is still open.

Q-0017 asks all of this. Note that the audio commands carry no key material, so
a stock-FRITZ!OS capture of one answered call could be handed over essentially
unredacted, which makes it a much cheaper ask than the cipher captures.

### ~~Latent bug~~ — our assigned TPUI sits in the chip's reserved range (2026-10-19)

> **Retracted by A-0031 (same day).** `00 11 <slot>` is **AVM's own regular
> TPUI scheme** — its ordinary six-slot `LOCATE-ACCEPT` constructor assigns
> exactly `00 11 <one-based slot>`, and its normal paging helper sends `0x0300`
> with `N.high = 0x11, N.low = local link id`. The `COMMAND_SET.md` §9.3/§6.3
> warning against `0x11xx` is "at least too broad"; A-0031 proposes amending
> it. **Do not change the TPUI and do not add `0x015F`.** What remains open is
> only whether the STUB's `0x11xx` consumer branch emits the page — settled by
> one instrumented incoming-call attempt, not by re-registering. The section
> below is kept as written for the record.

Found while preparing the incoming-call questions, in a document we already had.

At registration we assign a TPUI whose contents are `00 11 01`. `COMMAND_SET.md`
§9.3 describes a 3-byte handset tag `{A0, A1, A2}` and says:

- if **A0 = 0**, the device reads **N = A1·256 + A2** as a handset number;
- tags **`{0x00, 0x11, handle}` are reserved internally** for handle-addressed
  data (`0x0158`/`0x0159`), with the explicit warning **"Don't use numbers
  `0x11xx`"**;
- and §6.3 says `0x0300 PAGE_HANDSET` **ignores numbers of the form `0x11xx`**.

Our tag is exactly `{0x00, 0x11, 0x01}`, so N = `0x1101`. The link signature we
independently derived from the air for this handset is **`0x1101`** — which is
A1·256 + A2, corroborating §9.3's reading that the tag is the PMID, and
confirming the chip really does see our handset under that number.

**If the `0x0300` rule applies as written, this handset can never be paged, so
incoming calls could never ring** — no matter what else we get right. That would
be a dead end we walked into ourselves at G6, and it would not show up until
milestone B.

It may also explain why registration and outgoing calls work without us ever
touching the access lists: §9.4 checks the admission list only for N in
`1..439`, and `0x1101` is far outside it, so the checks are skipped.

Not fixed yet, because the fix is not obvious:

- moving N into `1..439` would bring the admission list into play, so we would
  then have to admit the number with `0x015F`;
- a tag whose A0 low nibble is non-zero **skips** the access-list checks per
  §9.4, which may be what AVM does;
- and changing the TPUI means re-registering every handset, so we want to change
  it once, to the right thing.

Q-0019 asks what AVM actually puts in `0x0300`'s N and what TPUI structure it
assigns. Until that is answered, note that **G7 milestone B is probably blocked
on this**, independently of the audio problem in Q-0017.

### Hardware result — A-0029's bind applied; a real A-law channel found (2026-10-19)

The bind now follows A-0029 §1: `0x0251 {handle, 1, gain}` **before** a
**six-parameter** `0x0230`, and no `0xF626`. All accepted; `0x0242 {01,01,00,00}`.

Captured **all eight TDM words** for a 37 s unencrypted call with continuous
speech, and scored every word and both byte lanes as A-law, mu-law and linear:

| word | contents | reading |
|---|---|---|
| 0 | `FE` / `00` | idle/mute fill — **never was the voice** |
| 1 | `FF` + mail bytes | our control slot |
| 4 | constant `08 40` | static |
| **5** | low lane A-law codes at zero (`55`,`54`,`D5`,`57`); high lane all `00` | **live A-law channel at digital silence** |
| 2,3,6,7 | constant | unused |

Word 5 matches A-0029 §4's predicted layout exactly — **one companded sample per
byte lane**, the other lane unused — and carries the lowest-magnitude A-law
codes, i.e. a *connected* channel rather than an idle buffer.

It is still **not the call's voice**. Across 37 s of speech the decoded peak
stays pinned at the smallest A-law magnitude for 35 of them. Word 5 behaves the
same with or without a call, and did so in every earlier capture — it is the
slot the probe used to report as "~1850 changes/s background". That churn was
A-law silence dither, not noise.

So all sixteen word/lane combinations have now been checked in one capture, and
none holds the call's audio. The audio is therefore **not merely in a position
we had not looked at**: either it is not produced, or it is produced somewhere
our DMA configuration does not expose.

Two leads remain:

1. **Slot/lane mapping (asked as Q-0023).** A-0029 §4's the vendor's PCM driver table encodes
   channel positions as `0x0091`/`0x0092`/`0x0093`/`0x00A1`/`0x00A2`, with
   `0x9x`/`0xAx` selecting sub-byte configurations of the same physical words.
   We cannot decode that, and our own module finds the control slot at word 1
   while the vendor's PCM driver puts the control resource at `0x0000` — so our physical
   numbering differs from theirs and we need to know whether it is configurable.
2. **Continuous transmit DMA (A-0029 §5).** Our host writes TDM frames only when
   it has a mail. The stock driver runs a continuously clocked, idle-filled
   transmit ring. If the chip needs ongoing transmit frames before producing
   receive audio, that explains everything above.

Still worth one bench sweep: the **codec sub-mode**. The C4 asks for HD and we
sent `flags=1, codec=0` (standard narrowband). A-0029 §7 says explicitly that it
**cannot** supply the right `{chan_flags, codec}` pair — so the clean room has
declined this one, the space is three combinations, and the signal is
unambiguous. That makes it a bench question rather than a search.

### Continuous TX ring implemented; transport starvation ruled out (2026-10-19)

A-0030 §10 named this the "highest-confidence actionable correction", and §4 Q8
named the specific bug: *"Avoid writing entire TDM buffers from a control-mail
sender: that can overwrite voice lanes."* That is exactly what `send_mail()`
did — `memset(frames, 0xff, ...)` across the whole block, so **every mail blasted
`0xFF` over every voice lane**.

Now mails are a queue of control-slot bytes that the block loop drains, and
`tx_block()` writes one block per received block, filling the control slot from
the queue and leaving the voice lanes their own idle value. Result:

```
tx: blocks 14387 queue_drops 0 idle ff voice_idle ff
```

The ring runs clean, the control channel is unaffected, and the call behaves as
before — so the architecture is right. **But the audio is unchanged**: still four
distinct A-law codes, peak pinned at the smallest magnitude. So *transport
starvation is eliminated* as the explanation. A useful negative.

#### A slot model that finally fits

A-0030 §2 Q4 warns that "DMA word order may not match a naive array of
host-native 16-bit samples: the AVM packer compensates for 32-bit word
organization when locating 16-bit slots." If 16-bit slots are swapped within
each 32-bit word, our index *i* maps to physical *i*⊕1, and everything lines up:

| physical | the vendor's PCM driver role (A-0030 §3) | our index | what we see |
|---:|---|---:|---|
| 0 | `dect0`, control | 1 | control slot ✔ |
| 1 | `dect1` low byte — **the voice** | 0 | `FE`/`00` mute fill |
| 2 | `dect2` low | 3 | `FE` |
| 3 | `dect3` low | 2 | `FE` |
| 4 | not a DECT channel | 5 | live A-law channel at silence |

That resolves two puzzles at once. Our control slot probes to index 1 while
the vendor's PCM driver puts control at 0 — the swap explains it. And our index 5, the
permanently live A-law channel that exists with or without a call, is then
physical 4, which is no DECT channel at all: almost certainly the **analogue
(FXS) port codec**, which is exactly what a permanently-connected line at
digital silence would look like.

Under this model the DECT voice lane is our index 0, low byte — and it is
carrying the mute fill. So the question is no longer *where* the audio is but
**why that lane is muted**.

Remaining candidates, in order:

1. **`0x0251` gain.** We send `1`; A-0030 §6 Q13 says AVM loads it from EEPROM
   at `0x2A + connection index` and explicitly warns against guessing. There is
   **no EEPROM read primitive** in `COMMAND_SET.md` — only raw `0xFFF1` sub
   `0x07` MEM_READ, which needs an address we do not have. So this needs the
   room.
2. **Codec sub-mode.** The C4 asks HD; we send `flags=1, codec=0`. A-0029 §7
   could not supply the right pair, so this one is a bench sweep: `-a` makes it
   a flag, not a rebuild.

#### Confirmed by A-0035: `raw = logical ^ 1`

A-0035 establishes the slot model above from the stock driver itself: the
interleaver and deinterleaver **swap the two 16-bit halves of every 32-bit
group** before each access (`address ^= 2`, i.e. `raw_word = logical_slot ^ 1`
for a 32-bit-aligned frame). The full enhanced-profile mapping, in raw DMA
words as our captures hold them:

| logical | raw word | lane | role |
|---:|---:|---|---|
| `dect0` | **1** | full word | control — our probed control slot, explained |
| `dect1` | **0** | low | first voice route |
| `dect4` | 0 | high | fourth voice route (shares the word) |
| `dect3` | 2 | low | |
| `dect2` | 3 | low | |
| `dect5` | 3 | high | |

Two other things settled by the stock *idle* snapshot: raw word 5 alternates
`0x00D5`/`0x0055` and raw word 4 reads `0x4008` **in stock, with no call** — so
they are background transport pattern, not DECT audio (source device unknown,
"analogue port" was a guess and stays one). No TDM register or device-tree
change is needed to reconcile the control position.

What A-0035 does **not** settle, stated carefully: that `0x0230` channel 1 is
`dect1` (names alone do not prove it), and whether the chip gates RX audio on
continuous TX (we have since tried it: no change). Its recommended path is the
one we are on — hold the setup constant, change one variable, and validate the
host↔STUB binding and **audio mute state** rather than searching for signal.

### Deferred `0x022D` — falsified (2026-10-19)

`0x022D` now goes out only after the chip's `0x0242` confirms the channel
(`-a nowait` restores the old order). The run did exactly that:

```
call: audio assign: 0x0251 (sel 1 gain 0x01), 0x0230 {01 0000 00 00} -- 0x022D waits for 0x0242
[16923] rx  prim 0242 len 4 01 01 00 00
call: audio connect: 0x022D on channel 1
```

and the `dect1` lane (raw word 0, low byte) is unchanged: two values, `00` for
39 of 40 s and `fe` in ~1.4 s of edges. **The timing hypothesis is wrong.** A
cheap negative; the deferred order stays because it is the documented
precondition and costs nothing.

One detail is new. `0xFE` is the STUB's documented disconnect fill, but the lane
is `0x00` almost throughout. That reads less like *a connected channel that is
muted* and more like *nothing driving this TDM position at all* — consistent
with A-0035's explicit non-confirmation that STUB audio channel 1 is routed to
`dect1`'s position. The STUB may need to be told which PCM slot a channel uses,
and we have never configured that: our bring-up sends only `0x015B` and
`0x022B`, while the stock boot includes a configuration download that A-0033
notes contains audio-related mails (`0x0250` among them).

#### The lead: the boot configuration download we never send (asked as Q-0024)

`docs/mac-bringup.md` has had the stock boot sequence since A-0006. Group A,
the *configuration download*, is `0x015B → 0x0248 → 0x0169 → 0x016A → 0x022B →
0x022C 00 → 0x0250 → 0x0251`; Group B, activation, adds the 55-byte `0x015C`
cell configuration and paging setup. We send `0x015B 01` and `0x022B 41` — and
stopped there because the radio came up and handsets registered without the
rest. `0x0250`/`0x0251`, the per-channel DSP parameters, are therefore sent at
**boot** on stock, and `0x015C` never goes out from us at all.

A DSP/PCM path that was never initialised would read as `0x00` at its TDM
position — nothing driving it — which is precisely the `dect1` lane. This fits
the symptom better than any per-call parameter we have varied, and it is a
question about AVM's default configuration values, which A-0028 showed the room
can extract. Q-0024 asks for the boot mail list with parameters; none of those
mails carry keys, so it is the cheapest capture requested so far.

Until it is answered, the codec sweep is parked: varying `0x0230`'s codec byte
cannot help if the path behind it was never configured.

#### Retracted by A-0036: there is no boot configuration download

A-0036 shows A-0006's "Group A / Group B" is the **address order of branches in
a message dispatcher**, not an execution trace — each runs in response to a
different incoming message. The lead above was built on a sequence whose own
caveat said "not proven to be execution order", and should not have been. In
particular the 55-byte `0x015C` is `SEND_ACKED_DATA`, a ULE data envelope, not
cell configuration; `0x022C 01` closes registration; `0x0300`/`0x015E` are
paging and queue management. None of them initialises audio, and `0x0248` is
not even in the 2026 STUB's main dispatcher table. `docs/mac-bringup.md` carries
the same correction.

What A-0036 does supply — the only exact new byte — is the **`0x0251`
fresh-default value `0x2B`**: AVM's per-call producer reads EEPROM
`0x2A + channel index`, and the driver initialises all six of those records to
`0x2B`. We had been sending `1` as a placeholder. The default is now `0x2B`
(`-a gain=N` still overrides). Its DSP semantics remain unproven, and a
provisioned box may hold a different persisted value — but it is a value with
provenance, which `1` never was, so it gets one dial.

The cause of the silence is, per A-0036, **not established**. Its ranked next
steps after this test: a redacted stock *answered-call* capture (host→STUB
`0x0251`/`0x0230`/`0x022D`/`0x0233` with synchronised PCM samples), a real
stock boot trace to replace A-0006's chronology, and only then a STUB consumer
trace of `0x0230`/`0x0251` for the DSP mux. All three are room work.

### `0x0251` gain `0x2B` — falsified; per-call parameters exhausted (2026-10-19)

```
call: audio assign: 0x0251 (sel 1 gain 0x2b), 0x0230 {01 0000 00 00} -- 0x022D waits for 0x0242
call: audio connect: 0x022D on channel 1
raw0/log1 (dect1) low lane: 00 ×287970, fe ×400   (36 s, continuous speech)
```

No change. That closes the list of per-call parameters for which a value with
provenance exists:

| variable | source | result |
|---|---|---|
| `0x0251` before full `0x0230`, no `0xF626` | A-0029 | no audio |
| continuous idle-filled TX ring | A-0030 | no audio |
| `0x022D` after `0x0242` | COMMAND_SET §6.5 | no audio |
| `raw = logical ^ 1`, `dect1` = raw 0 low | A-0035 | lane identified; silent |
| `0x0251` value `0x2B` | A-0036 | no audio |

What remains unvaried is the codec/sub-mode pair — but A-0029 §7 and A-0036
both decline to supply it, and with the lane reading `0x00` rather than any
mute or silence code, a codec mismatch is not the shape of the symptom. The
symptom is **nothing drives the position**, and the only way left to learn how
the STUB routes an audio channel to a TDM position is the one A-0036 ranks
first: **a stock answered-call capture with synchronised PCM samples**. That is
room work, and the dials stop here until it exists (Q-0025).

Method note for the record: today's bench runs that paid off were the ones
that falsified a single prediction with a sourced value (`-C`, the TX ring,
deferred `0x022D`, `0x2B`). The ones that did not were searches (`-k` ×3) or
rested on a doc's unverified ordering (the boot "download"). Ask the room for
the former kind of fact; use the bench only for the latter kind of test.

### Transmit test tone — nothing heard; `dect1` is dead in both directions (2026-10-19)

First test of the host → chip direction. A 1 kHz A-law tone written into raw
word 0, low byte (the transmit side of `dect1` under A-0035's mapping) on every
frame of a 45 s unencrypted call:

```
call: tx tone: 1000 Hz A-law into raw word 0 low byte
tx: blocks 13566 queue_drops 0
```

The handset heard **nothing**. No received lane carried the tone either, so
there is no in-chip loopback to that position. Together with every RX result,
the position is **dead in both directions**: the STUB is not routing audio
channel 1 to this TDM word, or no audio path is configured.

One documented degree of freedom remains, with provenance on both sides. A-0030
§3 Q3 and A-0035 §Q1 both describe **two host transport profiles** — the
*enhanced* one (8-bit A-law in a byte lane, `0x91`…, tested above) and the
*default* one (`0x81`/`0x82`/`0x83`, a separate **16-bit** path) — and both say
which is active is **not established**. Testing the other documented mode is a
falsification, not a search: `-a tone=1000,tone16` writes 16-bit linear into
the whole word (`tone16be` for the other byte order, which A-0030 says not to
infer from the platform). If that is silent too, the per-call and transport
space is exhausted and only Q-0025's stock capture can say where the chip puts
its audio.

### 16-bit transport and wideband channel mode — both silent (2026-10-19)

Two more single-variable dials, tone kept on so both directions test at once:

| run | change | chip `0x0242` reply | RX `dect1` | handset |
|---|---|---|---|---|
| v9 | tone as **16-bit linear LE** in the whole raw word 0 (default transport profile) | `01 01 00 00` | `00`/`fe` | nothing heard |
| v10 | `0x0230 {41 0000 00 01}` — **sub-mode bit + codec 1 = wideband** (§9.8), matching the C4's BASIC-SERVICE `0x88` HD bearer | **`01 41 00 00`** | `00`/`fe` | *(awaiting report)* |

v10 is the first time a change of ours altered the chip's reply: it echoes the
sub-mode bit, so the wideband mode was applied. The position stays dead in both
directions regardless.

Why this run happened despite the codec sweep being parked: the handset shows
HD, and its CC-SETUP carries BASIC-SERVICE `0x88` = wideband speech, which we
accept without a codec IE — so the air bearer is G.722 while `0x0230 codec=0`
configured the chip channel narrowband. A STUB that refuses to run its DSP path
on a bearer/channel mode conflict would look exactly like "dead both ways", so
the earlier argument that `0x00` "is not the shape of a codec mismatch" no
longer held once TX was shown dead too. §9.8's "flag set with codec 1 →
wideband" is a documented value marked "needs checking on hardware", i.e. a
falsification, not a search.

Still untested, deliberately: `tone16be` (other byte order; low value now that
the lane is dead under three write formats), and the opposite resolution of the
mode conflict — answering CC-SETUP with a **CODEC-LIST selecting G.726** so the
C4 drops to narrowband (EN 300 175-5 §7.7.54; "narrowband first" per A-0029 §8
and A-0030 §8). That one is spec work on `dect_cc`, not a flag.

### The STUB-side slot map was measured at G4 — and the DSP has a routing matrix (2026-10-19)

A-0035 left open how the STUB routes an audio channel to a TDM position.
`docs/stub-pcm-control-channel.md` has had the answer since 2026-10-17, from
the G4 bench: the STUB's **GenDSP** has channels ADPCM 0..3, PCM 4..6, wideband
7..9, silence 12 and test tone 15; `0xFFF1` sub `0x0A {dst, src}` connects
them, and on hardware **"the connect mail is accepted and its effect
persists"**, with the measured map

| GenDSP PCM channel | raw DMA word |
|---:|---:|
| 4 | 5 — the permanently "silent A-law" word |
| 6 | 0 — where the vendor's PCM driver puts `dect1` |

So raw word 0 is PCM channel 6 on the STUB side. What never worked at G4 was
the *source*: test tone 15 and silence 12 gave identical output, and the note
concluded the sources need an active call context — which we now have.

Also confirmed from every call log: `0x0200 CONNECTION_ESTABLISHED {01, 01}`,
i.e. **`wideband = 1`** [CR COMMAND_SET §8.2]. The connection is wideband at
MAC level, matching BASIC-SERVICE `0x88`.

This gives a mechanism for "dead both ways" that none of the per-call mails
touched: an audio path is a pair of GenDSP routes, codec ↔ PCM channel, and
nothing we send establishes one for PCM 6. The next test is on the bench, with
provenance from our own measurements: during a live call, route **tone 15 →
PCM 6** (expect a tone on RX raw word 0) and **tone 15 → the call's codec
channel** (expect it audible on the handset). Two routes, two independent
observables, one dial, reversible (route to silence 12, or `0xFFF1 09 n`).
`-a dsp=DST:SRC` sends them after the audio path is up. The codec channel is
the one unknown — wideband 7..9 for an HD call, ADPCM 0..3 for narrowband —
which is why a narrowband call (a handset HD setting, if the C4 has one) would
be worth having first.

### v11: GenDSP routes in a live call — silent, and the source number had no provenance (2026-10-19)

The C4 has no voice-quality setting, so the call stayed HD. Run (commit
`b09f470`): `-g -P 0000 -p -t 150 -C -a tone=1000,dsp=6:15,dsp=7:15 -w v11.raw`.
Unencrypted call of 54.7 s, `0x0200 {01 01}`, our `0x0251 {01 01 2b}` and
`0x0230 {01 01 00 00 00 00}` → `0x0242 {01 01 00 00}` → `0x022D {01 00}`, then
`0xFFF1 0A 06 0F` and `0xFFF1 0A 07 0F`, both accepted silently. **The handset
heard nothing.** Capture (839,616 frames from the bind): raw word 0 was `FEFE`
for 400 frames (50 ms), `0000` for the whole call, and `FEFE` again after
`0x0201`; raw 2 `FEFE`, raw 3 `0000`, raw 5 the usual A-law dither, 6/7 `FFFF`.
No tone on any word in either direction.

What the run established, and what it corrects:

1. **The bind does drive raw word 0.** The `FEFE → 0000` edge sits 50 ms after
   the bind, i.e. at `0x0242`/`0x022D`, and the `0000 → FEFE` edge at release.
   That is exactly the G4 observation: connecting a GenDSP destination to PCM 6
   turns raw word 0 from the `0xFEFE` fill to `0x0000`. So Q-0025's "nothing
   drives the position" was the wrong reading. **PCM 6 is connected to raw
   word 0 and delivers linear zero** — the source upstream of it is silent.
2. **"Test tone 15" was never a chip-side number.** A-0004's list (ADPCM
   0..3, PCM 4..6, wideband 7..9, silence 12, tone 15) is the *host driver's*
   debug vocabulary. `COMMAND_SET.md` §7.1, which describes `0xFFF1` sub `0x0A`
   from the *chip's* side, says: dest 0..17, source 0..22, out-of-range values
   ignored, **source `0x15` = silence** — and names no tone source. Whether the
   two numberings coincide has never been stated. So routing "15" to dest 6 and
   dest 7 falsified nothing; it only showed that source 15 into dest 6 yields
   the same zeros as the call path. That is a question for the room, not a
   dial, and it is the first item of Q-0026.
3. **`0x0242 {01 40 00 00}` arrives unsolicited, twice, at connection-up**
   (3.5 s, the C4's first link, 10 s before any bind), and `0x0242 {01 01}`
   short plus `{01 01 00 00}` long arrive 32 ms after our routes. Noted before
   at the second `0x0242` with `chan_flags = 0x40`; A-0037 asks the same thing
   ("what does the STUB do differently for `0x41` vs `0x01`"). Still open.

A-0037 (answer to Q-0025) says the stock captures A and B cannot be produced by
the room: no running stock box. It offers a GUI-only in-call support-data
export from a stock box (Tier A0) and, as the escalation, the STUB-side
consumer trace of `0x0230`/`0x0251`/`0x022D` to the DSP mux with only the
behavioural mapping handed over. Both A and B being unavailable is the
condition Q-0025 set for that escalation, so Q-0026 asks for it, together with
the enumeration of the chip's routing matrix. No further audio dials until it
is answered.

### The stock host's `0x0230` layout — and v12, the slot-mask dial (2026-10-19)

A separate analysis of the stock host module, `the hardware-analysis notes.md` in the
clean-room `answers/` folder (banner: "the analysis side" analysis, behaviour and
constants only, symbol names included; only its audio-path, PCM-task, debug
and re-implementation sections were read here), gives the audio primitives as
the stock host sends them [CR the hardware-analysis notes §6 1.6, §6 5]:

| mail | layout as sent by the stock host |
|---|---|
| `0x0230` | `{link, audio mode, u16 PCM slot mask LE, master flag, extra}` — mode bits 0..3 = chip channel (the GenDSP numbers ADPCM 0..3 / PCM 4..6 / wideband 7..9, `0xF` = not connected), bit 6 = wideband codec, `0x80` = no channel check |
| `0x0242` | `{link, audio type (bit 6 wideband), u16 PCM slot}` — the reply |
| `0x022D` | `{link, mute mode}`, `0x80` = mute during a codec change — a **mute request**, not an enable |
| `0x0251` | `{link, 1..3 channel class or host value, EEPROM 0x2A+ccei (default 0x2B)}` |
| `0x0233` | `{link}` disconnect; not sent at release (the chip drops audio with the link) |

Audio path: the host (user-space daemon) chooses the PCM slot via a
PCM_SLOT_REQ carrying mode, codec and slot mask; the module sends `0x0251`
(if a channel is given) and then `0x0230` with mode / slot mask / master /
extra, and waits for `0x0242`. Nothing else. The wideband bit is derived from
the negotiated codec. Also: `0xFFF1` sub `0x01` sets the chip's idle fill
pattern (u16), and `dect_connect` is confirmed as sub `0x0A {dst, src}` with
the GenDSP numbers above (so A-0004's numbering is the chip's; the
`COMMAND_SET` §7.1 ranges 0..17 / 0..22 are the firmware's bounds checks on
the same space — source 0x15 = 21 being silence there and 12 in the help text
stays unexplained).

So every bind we ever sent had **slot mask `0x0000`** — "no TDM slot" — and the
chip echoed slot 0 each time. That is a per-call parameter with provenance
from the stock host, and one dial falsifies it.

**v12:** `-a flags=0x41,mask=0x0002,tone=1000 -w v12.raw` (mode `0x41` =
channel 1 + wideband, as stock derives it for a G.722 call; mask bit 1 =
logical slot 1 = raw word 0, the `dect1` position). Unencrypted HD call,
26 s. The chip accepted and **echoed the slot: `0x0242 {01 41 02 00}`**.
Handset: nothing audible, TX tone inaudible. Capture (292,192 frames): raw
word 0 `0000` for the whole call apart from a handful of one-frame blips and
two 30 ms `FEFE` dropouts (no mail coincides with them); raw 3 `0000`
throughout, raw 2 `FEFE`, raw 4 `0840`, raw 5 the A-law dither, 6/7 `FFFF`.
**No change from mask 0.**

What that leaves: the chip drives raw word 0 with linear zero whether the
mask is 0 or 2, and echoes 2 when asked. Either the mask's bit numbering is
not "logical 16-bit slot" (the chip may count 8-bit timeslots, or count from
after the control slot, or expect two slots for wideband), or the position we
watch is not where this chip channel lands. Both are facts about the vendor's PCM driver
(`dectN` ↔ TDM slot / lane / width) and the STUB's slot-mask consumer, so
they go into Q-0026 rather than into further dials.

### The stock *user-space* side: what sends `0x0230`, and a live diagnostic (2026-10-19)

A second analysis of the same kind, `the hardware-analysis notes.md` (stock
DECT manager daemon, the character device below it, and the ctlmgr plugin;
same "the analysis side" banner, behaviour and constants only), closes the chain from
the stock telephony stack down to the mails we send. Only its audio, PCM-slot,
primitive-name, monitor and codec sections were read here.

**The chain, end to end** [CR the hardware-analysis notes §6 0.6, §6 1.6;
the hardware-analysis notes §5, §6 5]:

| step | who | message |
|---|---|---|
| 1 | stock daemon → host module | `SET_PCM_SLOT` `{link, audio mode, codec-present, codec byte, u16 slot mask, extra}` |
| 2 | host module → chip | the same body, as a mail to the chip's call-switch task |
| 3 | still in the host module, protocol-map layer | derives the wideband bit from the codec byte and emits `0x0251` (when a channel is given) and then `0x0230 {link, mode, u16 mask, master, extra}` |
| 4 | chip → host | `0x0242`, relayed up as a PCM-slot indication |

**Correction (A-0038 §3).** An earlier version of this section said `0x0230`
is produced inside the chip. That is wrong, and A-0038 caught it: the task
table in the same analysis puts the whole call stack — the host API gateway,
the switch, the protocol map, call control and the data link — **on the host,
inside the module**; only three task ids (the chip's MAC/CCF, the production
test task and the TDM peer) run on the chip. So `0x0230` is a host→chip mail,
exactly as A-0029 found, and the producer we stand in for is a host-side
layer, not a chip-internal one.

What survives the correction is the part that matters: the stock host emits
`0x0251` then `0x0230` and waits for `0x0242`, which is what we send, in that
order, with that layout. The useful consequence is stronger than before —
because every layer above the chip is host-side, **the complete set of
host→chip mails is enumerable from the same analysis**, and a mail we never
send would have to appear in it.

Three further facts, each of which moves something:

1. **`0x0251` is not gain.** The host primitive that produces it is named
   *error-concealment threshold*: a per-handset packet-loss concealment level,
   persisted at EEPROM `0x2A + handset` [CR the hardware-analysis notes §4].
   That is the same byte A-0029 found, so the value `0x2B` was right, but it
   has nothing to do with audibility. `-a gain=` is not an audio lever.
2. **Mode and mask come from one number on the host side.** The stock daemon
   allocates a PCM slot, hands it to its telephony stack, and when that stack
   answers it sends `mode = slot` and `mask |= 1 << slot`
   [CR the hardware-analysis notes §6 1.6]. Our v12 (`mode` nibble 1, mask bit
   1) satisfies that rule; every run before it did not.
3. **The 7520's bus carries four DECT slots: 0 = control, 1..3 = audio**, and
   the chip-side channel names ("ADPCM 0..3, PCM 4..6, wideband 7..9") are
   *chip channel numbers, not TDM slots* [CR the hardware-analysis notes §12 1].

Items 2 and 3 cannot both be literally true for a wideband call: a chip
channel of 7..9 cannot also be a TDM slot on a bus with four of them. Which
of the two the mode nibble really is, is now the question — and it has a
concrete consequence, because **every bind we have sent used mode nibble 1**,
i.e. ADPCM channel 1, while the call is G.722. If the nibble is the chip
channel, a wideband call belongs on 7..9 and ADPCM channel 1 is exactly the
empty channel one would expect to read out as linear zero.

**A read-only diagnostic we did not have.** The stock host polls the chip for
a per-link connection status: `0xF624 {link, 0x0A}`, answered by `0xF625` with
a 0x2D-byte record [CR the hardware-analysis notes §6 1.5, host mail for the connection
monitor]. The record begins with the MAC slot, the RF carrier, a codec byte
and the link type (1 voice / 2 data), followed by ten RSSI bytes, frame
counters and a BER word [CR the hardware-analysis notes §5 4.4]. The codec
byte doubles as the bearer's **slot type: 0 = full slot (G.726 narrowband),
1..3 = long slot (G.722 or the proprietary wideband codecs)**
[CR the hardware-analysis notes §8 3.3]. A G.722 call running on a full-slot
bearer would explain silence in both directions on an otherwise correctly
bound channel, and this read says so directly.

It is a pure read and changes nothing the radio transmits, so it is usable
during a live call: `-a status=MS` polls it every MS milliseconds while a call
is bound and prints slot, carrier, slot type, link type and state.

Also fixed while here: the `0x0242` handler only connected channels 1..6, so a
wideband channel 7..9 would have had its `0x022D` silently skipped.

### v13–v15: the bearer is right, the chip picks the channel, the DSP routes do nothing (2026-10-19)

Three runs on the spare box, each one variable, radio parked after every one.

**v13 — the connection-status read.** `-a flags=0x41,mask=0x0002,status=2000,
tone=1000`, i.e. v12's bind with the new `0xF624` poll. The answer
(`0xF625`, 45 bytes) decodes cleanly once the ten RSSI bytes are used as the
anchor — they sit at the same place as in the chip's DECT-status answer
`0xF623`, which fixes the offsets:

```
01 0a | 08 07 00 02 | 1c 1f 1a 1a 1b 1b 19 36 1c 22 | 00 00 ...
link sel  slot carrier ? codec   <--- 10 RSSI bytes --->
```

MAC slot 8, carrier 7, **codec byte 2**, stable for the whole call. Codec 2 is
a **long slot** bearer, i.e. wideband [CR the hardware-analysis notes §8 3.3],
and under the codec reading of the same byte it is AVM's wideband id `0x0A`,
not G.722 — the chip knows the handset's capabilities from registration and
picks by its own preference order. Either way the bearer is a wideband long
slot, so **the "G.722 call on a full-slot bearer" explanation is dead**, and
with it the idea that the fix is at CC level. Audio: nothing, either way.

**v14 — the chip channel.** Same run with `flags=0x47` (wideband bit +
channel 7, the GenDSP wideband range). We sent `0x0230 {01 47 02 00 00 00}`
and the chip answered **`0x0242 {01 41 02 00}`: channel 1**. It does not take
the channel we ask for; it assigns one and reports it back. So the mode nibble
is not a dial, every bind we have sent has been on the channel the chip itself
chose, and the "wideband call belongs on channel 7..9" reading is falsified.
Audio: nothing. (MAC slot 4, carrier 4 this time, codec byte 2 again.)

**v15 — where does the decoded audio come out?** With the bind back at
`flags=0x41`, three GenDSP cross-connects sent after `0x022D`, each into a
different PCM port, so one call tests three candidate sources and the capture
is the readout rather than a person's ear:

| route | port | raw word | candidate source |
|---|---|---|---|
| `dsp=6:1` | PCM 6 | 0 (measured G4) | ADPCM channel 1 — the channel the chip reports |
| `dsp=4:7` | PCM 4 | 5 (measured G4) | wideband channel 7 |
| `dsp=5:8` | PCM 5 | unknown | wideband channel 8 |

All three mails went out. **No raw word changed behaviour**: word 0 stayed at
linear zero, word 5 kept exactly the A-law dither it has when idle, every
other word stayed constant, for a call the user spoke into continuously.

That last point is the informative one. At G4 we measured that connecting a
destination to PCM 4 *does* change raw word 5. Here it did not. So either the
cross-connect is being rejected during a live call (the firmware ignores
out-of-range values silently, and we have never seen an acknowledgement for
sub `0x0A`), or all three sources are genuinely silent.

**Where that leaves the problem.** Everything on our side of the interface is
now confirmed correct against the stock implementation: the two mails, their
order, their layout, the channel (the chip's own choice), the slot mask (the
chip echoes it), and the bearer (long slot, wideband). The chip drives the TDM
word we watch, and writes linear zero into it for the whole call. The missing
link is inside the chip: nothing connects the call's decoder to the PCM port,
and the debug cross-connect does not appear to install one either.

Two readings remain, and neither is reachable by dialling:

1. **A wideband channel needs more than one TDM slot.** The mask is a 16-slot
   mask and we have only ever set one bit. The 7520's bus carries four DECT
   slots (0 control, 1..3 audio), so a two-slot wideband channel would be
   mask `0x0006`. This is the one dial still open with zero code changes.
2. **The U-plane carries no audio.** If the bearer has no voice data, a
   correctly routed decoder outputs exactly the linear zero we see, and no
   cross-connect can produce sound. What establishes the U-plane on this chip,
   and whether anything beyond `0x0230` is needed for it, is not in either
   analysis.

No further dials on reading 2 until the room answers; it is a search space,
not a prediction.

### v16: the two-slot mask, negative — and the end of the dialling (2026-10-19)

`-a flags=0x41,mask=0x0006,status=2000,tone=1000`: mask bits 1 and 2, i.e.
both of the first two audio slots, on the hypothesis that a wideband channel
needs two. The chip **accepted and echoed it — `0x0242 {01 41 06 00}`** — and
reported the usual long-slot wideband bearer (MAC slot 2, carrier 5, codec
byte 2). Raw word 0 and raw word 3, the two words those slots map to, both
stayed flat for the whole call: zero changes per second, every second.
Nothing audible in either direction.

So the chip accepts any slot mask we offer, echoes it back, drives the first
word of it, and fills it with linear zero regardless of how many slots we give
it. Reading 1 from the previous section is falsified; **reading 2 — that the
U-plane carries no voice data at all — is the only one left standing.**

That is the end of what dialling can decide. Every parameter of the bind is
now either confirmed against the stock implementation or demonstrably not
ours to choose:

| parameter | status |
|---|---|
| mails sent, and their order (`0x0251`, `0x0230`, `0x022D`) | identical to the stock host's producer (host-side; see the A-0038 correction above) |
| `0x0230` layout | confirmed [CR the hardware-analysis notes §6 1.6] |
| chip channel (mode nibble) | **assigned by the chip**, not by us (v14) |
| slot mask | accepted and echoed for 0, 2 and 6 (v12, v13, v16) |
| bearer | long slot, wideband, confirmed live by the chip (v13–v16) |
| codec | the chip's own choice from the handset's registered capabilities |
| `0x0251` | packet-loss concealment, not an audio lever |
| GenDSP cross-connect | no observable effect during a call, never acknowledged (v15) |

What remains is a question about the chip, not about our parameters: what
makes an established DECT link actually carry voice on this firmware, and
whether a host that drives the MAC directly must do something the stock host
never does, because the chip's own call-control layers do it for it. That is
in Q-0026 §1c.

### A-0038, and the one observable we already had (2026-10-19)

A-0038 answers Q-0026 with a careful "not established" on the DSP matrix: the
room located the dispatcher branches for `0x0230`, `0x0251`, `0x022D`,
`0x0233` and `0xF626` in the production firmware, but did not trace them to
the DSP mux, and it refuses to turn the host's debug vocabulary into chip
channel definitions. It also says plainly: **do not sweep the 23×18 matrix**,
and do not read a route command's silence as success — sub `0x0A` has no
acknowledgement, so a rejected route and a silent source look identical.

It corrects one of our own claims (see the correction above) and it reframes
the problem usefully, by splitting what we had been treating as one question
into five observables:

| stage | our evidence |
|---|---|
| DECT link established | yes — `0x0200`, and the status read |
| logical audio assignment | yes — `0x0230` → `0x0242`, channel and mask echoed |
| TDM endpoint opened | yes — raw word 0 goes `FEFE → 0000 → FEFE` with the call |
| **voice B-field reaches the chip** | **never observed** |
| decoder output reaches the PCM port | never observed |

The fourth row is the one A-0038 calls the crucial missing stage, and its
decision test turns on it: if B-field frames arrive while the decoder stays
empty, the fault is in the mux; if no B-field frames arrive at all, the fault
is upstream, in whatever makes the MAC carry voice on an established link.

**We can read that row without any new primitive.** The connection-status
answer we added for v13 is 45 bytes, and only its first 24 were being logged.
Behind the ten RSSI bytes sit ten `u16` counters and a BER word, and the host
names them in this order for this record: duration, **received B-field
frames**, Q1/Q2 errors, slide errors, sync errors, handovers, and the A/X/Z
CRC error counts [CR the hardware-analysis notes §5 4.4]. The B-field frame
counter is precisely the fourth row.

So the daemon now prints all ten counters and their delta since the previous
poll. No new mail, nothing extra transmitted — the poll was already being
sent and the chip was already answering with these numbers; we were throwing
them away 24 bytes in.

### v17: the B-field counters — the U-plane is alive (2026-10-19)

`-a flags=0x41,mask=0x0002,status=2000`, the v13 bind with the status answer
decoded in full. Nine polls over a ~20 s call, MAC slot 10, carrier 5, the
usual long-slot wideband bearer. The ten `u16` counters, printed by index
because the host is known to reorder them for its own view and the wire order
is therefore not established:

```
c0=0  c1=0  c2=0  c3=0  c4=0  c5=200  c6=0  c7=0/1  c8=0  c9=200   ber=0
```

The first poll after the bind read 621 in both c5 and c9; every later poll
read 199–200 in both. The polls are 2 s apart and DECT runs 100 frames per
second, so **both counters count exactly one per frame** over the window
(621 = the 6.2 s from call start to the first poll), and both reset on read.
Every other counter is zero except c7, which went 0 → 1 → 0 once.

Two readings are possible, and they are not close:

- **c5 and c9 are the window duration and the received B-field frame count.**
  Then the chip is receiving a B-field in every single frame, with no CRC and
  no sync errors, and c7 is a rare error counter behaving exactly as one
  should.
- **c5 and c9 are sync errors and Z-CRC errors.** Then every frame of the
  call fails both checks.

The second cannot be true. A bearer failing every frame does not hold a call
for a minute, keep the handset registered, report healthy RSSI on ten
carriers, or leave the other eight counters at zero. So:

**The U-plane is alive. Voice B-field frames arrive at the full frame rate
with no errors, and the PCM port the chip binds still carries linear zero.**

That resolves A-0038 §5's decision test onto its first branch — *"B-field
buffers contain valid payloads while decoder input is empty or constant →
investigate MAC-to-decoder handoff and DSP routing"* — and collapses its
five-stage table to a single unknown:

| stage | status |
|---|---|
| DECT link established | confirmed |
| logical audio assignment | confirmed, channel and mask echoed |
| TDM endpoint opened | confirmed, tracks the call |
| voice B-field reaches the chip | **confirmed (v17)** |
| decoder output reaches the PCM port | **the only remaining gap** |

The caveat worth keeping: a frame counter shows that a B-field arrives, not
that its content is speech rather than filler. But at full rate with zero CRC
errors it is the strongest evidence obtainable from the host side, and it
removes the entire "the bearer carries no voice" branch that we could not
otherwise have excluded.

What this is worth to the room: A-0038's item E (what enables the U-plane)
can be dropped, and its item D (the DSP matrix and its default routes) plus
items A and B (what `0x0230` and `0x022D` actually switch) are now the whole
question. The gap is between a working decoder input and a bound PCM output.

### The GenDSP output lane, mapped with the radio off (2026-10-19)

A-0038 says a route command has no acknowledgement, so a rejected route and a
silent source look identical — which is why v15 told us so little. That is
testable without transmitting anything: the GenDSP lives on the chip and takes
routing commands with the radio parked, so `-a capall` was added to make `-w`
capture from start-up rather than only during a call, and the probing was done
with no radio and no handset.

**Route writes are observable.** With nothing else running, `dest 6 ← src 1`
moves raw DMA word 0 from the `0xFEFE` idle fill to `0x0000` and holds it
there; `dest 4 ← src 1` and `dest 5 ← src 1` move nothing on any of the eight
words. So destination 6 drives word 0 (reproducing the G4 measurement), and
destinations 4 and 5 drive no lane we capture — **v15's negative was a real
negative, not a refused write.** The G4 note that destination 4 drives raw
word 5 did not reproduce and should be treated as unconfirmed.

**What the lane carries.** Sweeping every source 0..22 into destination 6,
one short radio-off run each, and reading word 0:

| source | word 0 | reading |
|---|---|---|
| 0, 21, 22 | `fefe` | idle fill — the destination is left disconnected |
| 1–5, 11–15, 18 | `0000` | connected, source silent |
| 6, 8, 9, 10, 16, 17 | `ffff` | connected, a different constant |
| **19** | **`7fff`** | **+full scale, 16-bit linear** |
| **20** | **`8000`** | **−full scale, 16-bit linear** |

Source 7 was `ffff` apart from 32 frames of `00a3`, the only transient in the
whole sweep.

Three things follow, all `[observed]`:

1. **The lane works and it is 16-bit linear.** It reaches both rails. Nothing
   about the TDM position, the byte lane or the companding is wrong.
2. **`0x0000` means connected-to-a-silent-source; `0xFEFE` means
   disconnected.** A bound call leaves `0x0000` on word 0, so the call's PCM
   port *is* connected and its source is silent. That is a much stronger
   statement than "the word is zero".
3. **The host debug-help numbering is not the chip's**, confirming A-0038's
   refusal to adopt it: 12 ("silence") and 15 ("test tone") are both silent
   here, and the only non-zero sources are 19 and 20.

This also gives us a reference signal. Any future claim that "the output path
is broken" can be checked in one radio-off run by routing source 19 or 20 and
looking for `0x7FFF` on the lane.

**What it sets up.** Sources 1–5 and 11–15 are silent with no call — exactly
what an idle decoder should be. If one of them is the live call's decoder, it
becomes non-constant during a call. `-a dspsweep=DST[:MS]` walks the sources
into one destination during a bound call, one per second, and the daemon's
existing per-second lane-activity line lines up against it, so **one call
covers the whole source range**. The sweep restores the default routes of
channels 1..6 when it finishes.

### v19: data on the lane at last — but it is not audio (2026-10-19)

**Correction first.** This section originally reported v19 as "real wideband
speech on the TDM". That was wrong. The maintainer listened to the exported
audio and heard white noise, and the measurements agree with him once the
right ones are taken: the 100 ms RMS blocks of the loudest second run 16859,
18774, 19327, 18460, 19597, 17832, 19327, 18258, 18081, 19823 — dead flat.
Speech has pauses and syllables. "Varies a lot and reaches full scale" is not
evidence of voice, and it should not have been reported as such.

What is nonetheless true, and new: **this is the only run in which raw DMA
word 0 ever carried data at all.** In v13–v17 it held digital silence for the
whole call. So the route does get wired under some condition, and we now have
a sample of what comes through it.

**What differed about v19.** The bind was byte-identical to the silent runs —
`0x0251 {01 01 2b}`, `0x0230 {01 41 02 00 00 00}`, `0x0242 {01 41 02 00}`,
`0x022D {01 00}`. 120 ms later the chip reported connection-up *again* for the
same handle and re-confirmed `0x0242 {01 41 02 00}`; the lane went live in
that same second (100 ms RMS 211, 125, 0, 4951, 16396, …, so it starts about
300–400 ms in). No silent run ever showed that repeat, and their only
connection-time `0x0242` carried audio type `0x40` with **no channel**. The
working theory is that the chip wires its audio route when a connection is
established, using the channel assignment that exists at that moment — bind
afterwards and the channel is recorded but never wired.

**What the data is.** With the sweep walking sources into destination 6:

| source | content |
|---|---|
| bind's own route | live, entropy 5.9 bits/byte |
| 1 and 11 | live, full scale, entropy 6.9–7.0 bits/byte |
| 2 and 12 | 97.8 % repeat at every byte lag — a near-constant slow value, not audio |
| 19, 20, 21, 22 | the radio-off reference values, exactly as measured |

The structural finding is in sources 1 and 11: **byte `i` equals byte `i-2`
51 % of the time, and essentially nothing repeats at any other lag.** Each
16-bit word is held for exactly two TDM frames, so the lane carries 4000
distinct words per second — **8000 bytes/s, 64 kbit/s** — duplicated to fill
the 128 kbit/s slot. That is exactly the B-field rate of the long slot the
status read reports, and exactly G.722's rate.

So the port is being fed a 64 kbit/s high-entropy stream rather than decoded
PCM. Decoding the de-duplicated bytes as G.722 (both byte phases, which are
identical because the duplication is exact) produces a saturating, diverging
output rather than speech, so either the framing is wrong or the content is
not plain G.722.

**Two readings, both specific:**

1. **The mux is routing the coded bearer rather than the decoder output.**
   Then the question is which `0x0230` parameter or which GenDSP source
   selects decoded audio — A-0038's items B and D, now with a much sharper
   symptom than "silence".
2. **The bearer is ciphered.** The handset may encrypt the U-plane even
   though `-C` suppresses our side and the handset's own setting allows the
   call to hold. Against this: the byte distribution is clearly not uniform
   (top codes at 2–4 % where ciphertext would be 0.39 %, entropy 7.0 rather
   than ~8.0). For it: nothing else explains high-entropy data at exactly the
   bearer rate.

Reading 1 is the better fit for the measured distribution. Either way the
useful new fact for the room is the rate and the duplication, because it says
what the port receives, not merely that it is not speech.

### CONFIRMED: the lane carries coded G.722, and the uplink audio is recoverable (2026-10-19)

The 64 kbit/s stream from v19 **is** the G.722 bearer. De-duplicated — one
frame in two — and fed to a G.722 decoder it produces the handset's speech,
confirmed by ear on `source 1`. This supersedes the "white noise" reading of
the previous section, which was correct about the samples not being PCM and
wrong about there being no audio in them.

So the chip's answer to `0x0230` is not "decode this call to PCM and put it on
your TDM slot". It is **"put the call's coded bearer on your TDM slot"**:

- 4000 distinct 16-bit words per second, each repeated for two 8 kHz frames
- = 8000 bytes/s = 64 kbit/s, the long slot's B-field rate and G.722's rate
- both byte phases give the same stream, because the duplication is exact

`tools/dect_g722_extract.py` does the extraction, and with `--wav` the decode
as well.

**This explains every inaudible test tone in this document.** v5 through v12
wrote A-law or 16-bit linear PCM into raw word 0. The lane does not carry PCM;
it carries coded bits. A sine wave encoded as PCM is, to a G.722 decoder,
noise at best — so the handset was right to play nothing. The transmit
direction was never tested by those runs at all.

It also retires several things we chased: the companding question, the byte
lane question, the 16-bit-versus-8-bit question, and the "wideband needs two
slots" question. One slot at 128 kbit/s carries a 64 kbit/s coded stream with
room to spare, which is why the duplication is there.

**The next step needs no codec on our side.** `-a loopback` copies the
received bearer word straight back into the transmit frame, byte for byte,
frame for frame: a valid coded stream handed back to the chip, with the
two-frame cadence preserved for free. If the handset hears itself, the
transmit direction is proved and G7's remaining half is a G.722 encoder, not
a protocol unknown.

Still open, and now the only protocol unknown in the audio path: **what makes
the chip wire the route at all.** v19 is the only run where the lane carried
anything, and the one thing that distinguished it was the chip repeating
connection-up 120 ms after the bind and re-confirming the channel. The
`bindearly` option (assign the channel as soon as the chip reports a
connection, rather than when a call connects) is the test for that.

### v20: loopback, and the lane stayed dead — v19 is not yet reproducible (2026-10-19)

`-a flags=0x41,mask=0x0002,status=8000,loopback`, no sweep, so the mux stays
where the bind puts it for the whole call. Result: **raw word 0 held `0x0000`
for the entire call** — connected, silent, exactly as v13–v17. Nothing audible
at the handset, and the capture contains no bearer.

Everything that could explain it was checked and is identical to v19:

| | v19 (lane live) | v20 (lane dead) |
|---|---|---|
| bind mails | `0251 {01 01 2b}`, `0230 {01 41 02 00 00 00}` | identical |
| chip's answer | `0242 {01 41 02 00}`, then `022D {01 00}` | identical |
| connection-up repeat after the bind | yes, +120 ms | yes, +104 ms |
| follow-up `0242` forms (long, long, short) | yes | yes |
| C-plane after the bind | keypad `2c 05 "12345"`, then AVM proprietary IEs | identical |
| bearer counters | 800 per 8 s poll = 100/s | identical |
| bearer type | long slot, wideband, codec 1 then 2 | identical |

So the connection-up repeat is **not** the trigger — it happened in both. That
theory, and the `bindearly` test built on it, are weakened accordingly.

One difference remains and it is ours: what we transmit on that word during
the call. v19 sent the voice-lane filler `0xFFFF` throughout; v20's loopback
sent back whatever it received, which while the lane is silent is `0x0000`.
If the chip reads the downlink side of that slot and treats an all-zero or
idle-fill input as "this bearer is not in use", the loopback would suppress
the very path it was meant to test. That is a guess, but it is the only
difference left between a run that worked and a run that did not.

The honest position: **we have one success and one failure with the same
protocol exchange**, so the first thing to establish is whether v19 is
reproducible at all. Repeating v19's exact command line answers that, and
distinguishes "the loopback suppressed it" from "the chip wires the route
only sometimes".

### v22: AUDIO PASSES, BOTH DIRECTIONS (2026-10-19)

`-a flags=0x41,mask=0x0002,status=8000,dspsweep=6:30000,loopback`: the
configuration that worked in v19 and v21, with the sweep's first step pushed
out to 30 s so the mux stays on the bind's own route, plus the bearer
loopback.

**The maintainer heard his own voice back through the handset.** The lane ran
at ~4000 changes/s for the whole call — the full 50 s, uninterrupted — and the
capture decodes to clean speech.

That is G7's audio criterion met, by the echo route the gate explicitly
allows: the chip hands us the uplink G.722 bearer on the TDM slot, we hand the
same coded words straight back, and the handset plays them. No codec on our
side, so this proves the transport in both directions rather than any codec
work of ours.

| direction | how it is proved |
|---|---|
| handset → base | the captured bearer decodes to the maintainer's speech (v19, v21, v22) |
| base → handset | the echoed bearer is audible in the handset's earpiece (v22) |

What is left for a real call is a **G.722 codec on the host side** — an ITU
standard, public, and plain engineering — plus the frame cadence we already
know: write each coded 16-bit word into two consecutive TDM frames.

**Still unexplained, and now the one open item in the audio path.** The lane
comes alive in some runs and not others on a byte-identical protocol exchange.
Live: v19, v21, v22. Dead: v13–v17, v20. The only configuration difference
that separates them is `dspsweep` being *configured* — but its first mail goes
out long after the lane is already running at full rate, so it cannot be the
cause, and the real variable is still hidden. v20's failure is no longer
attributable to the loopback either, because v22 used the loopback and worked.
Until that is understood, the working recipe is the v22 command line.

### Audio quality: the bearer stalls, and it is not the capture (2026-10-19)

The v22 export is intelligible but has short interruptions and level
fluctuations. Measuring the run lengths of identical consecutive words in the
capture shows why. Normally a coded word occupies exactly two frames — 201295
such pairs in the 50 s call — but there are also runs of 4 (230), 10 (82),
40 (10) and one each of 47 and 48, plus four odd-length runs.

Those lengths are the tell: 10 and 40 TDM frames are exactly 1/8 and 1/2 of a
DECT frame (80 TDM frames = 10 ms). The stalls therefore sit on DECT frame
structure, which places them on the chip's side of the slot — bearer
underruns, not an artefact of how we read it.

Confirmed by comparing runs with and without the transmit loopback, so the
extra per-frame work we do is not the cause:

| capture | window | stall events | frames held over |
|---|---|---|---|
| v19 (no loopback) | 0–3 s | 9.2/s | 11.4 % |
| v21 (no loopback) | 0–3 s | 11.5/s | 11.9 % |
| v22 (loopback) | 0–3 s | 8.5/s | 11.8 % |
| v22 (loopback) | 10–50 s | **3.1/s** | **0.10 %** |

So the path is rough for the first few seconds while it settles — about an
eighth of all frames held over — and then steady at roughly three short
stalls per second. The de-duplication itself is sound: four odd-length runs
in 50 s is the entire phase-slip budget, and because both frames of a pair
are identical a slip costs at most one sample.

`tools/dect_g722_extract.py --max-run N` caps a held word at N frames, so the
repeats are not fed to the decoder to re-adapt through. It trades a little
timing drift for fewer artefacts: `--max-run 2` shortened the 50.9 s call to
50.4 s.

Not yet established: whether the underruns are the chip waiting on bearer
data, or the DSP emitting before the next coded word is ready. Both would
look like this from the host. It matters for call quality but not for the
transport, which is proved.

### What separates a live lane from a dead one (2026-10-19)

> **Correction, 2026-10-20 [CR A-0072-2].** `0x0243` is *not* a connection
> parameter and `0x29` is *not* a selector. The stock receiver handles it in the
> connection's link-control / data-link path: it updates a counter/window value
> and may resume buffered signalling fragments, and never touches audio
> assignment or DSP routing. Its two trailing bytes are ordinary numeric inputs
> to that procedure. The correlation recorded below stands as an *observation* —
> the mail does accompany live lanes — but read it as link-control activity that
> co-occurs with the re-established connection, not as an audio-ready marker,
> and do not gate on it. See `work/an external review/01-audio-route-and-pcm.md` §5.

Comparing the `0x0243` traffic in the two seconds after
the bind across every run to date gives a rule that fits five of six:

| run | connection-up repeated after the bind | `0x0243 {01 29 00}` | lane |
|---|---|---|---|
| v13, v17 | no | no | dead |
| v19, v21, v22 | yes | yes | **live** |
| v20 | yes | yes | dead |

So the audio path appears to be wired when the chip **re-establishes the
connection after the channel has been assigned** — visible as the repeated
`0x0200` together with a `0x0243 {01 29 00}` (bytes kept opaque). The runs that
never saw either never got audio. This revives the theory v20 appeared to
kill, and makes `bindearly` worth testing again: if the channel is already
assigned when a connection comes up, the first connection-up should wire it
and no re-establishment would be needed.

**v20 is the lone exception, and it looks self-inflicted.** Its lane did start
to come alive — 26 distinct values on the word in the first second — and then
went flat. v20 is also the only run that echoed the lane back while the lane
was still silent, i.e. handed the chip's downlink a stream of `0x0000`. v22
used the same loopback but its lane came up at once, so the echo carried valid
coded words from the first frame.

The loopback now only echoes real bearer data: a word of `0x0000` (routed but
silent) or `0xFEFE` (unrouted) is replaced by the transmit filler. That
removes the hazard without changing the proven case.

### v23: bindearly does not do it — and the lever that should (2026-10-19)

`-a flags=0x41,mask=0x0002,status=8000,bindearly,loopback`. The early
assignment fired as intended, on the bare link before any call:

```
reg: connection up (handle 1), C-plane armed
call: bindearly: assigning the audio channel on handle 1 before any call
call: audio assign: 0x0251 {01 01 2b}, 0x0230 {01 41 02 00 00 00}   <- early
call: audio assign: 0x0251 {01 01 2b}, 0x0230 {01 41 02 00 00 00}   <- the call
call: audio connect: 0x022D on channel 1
```

The lane stayed dead, and **there was no connection-up repeat**. So assigning
the channel in advance does not make the chip wire the route, and the rule
survives intact: audio has flowed in exactly those runs where the chip
re-established the connection *after* the channel was assigned, and in no
others.

We cannot be early enough to catch the first establishment — the chip tells us
about it after the fact. So the move is to provoke a second one.

**`0x016A {link}` is `a stock handover hook`**, named from the stock
daemon's own primitive table (host primitive `0x2FEA`), and described there as
a hook for forcing a bearer or base handover
[CR the hardware-analysis notes §4, §5 2.5; the hardware-analysis notes §5]. Our
`COMMAND_SET` §6.4 had this id as a frame-credit counter, `[M]/[L]`
confidence; that reading is superseded.

A forced bearer handover is precisely the event that accompanied every
successful run. `-a handover=MS` sends it MS ms after the audio bind.

If it works, the audio path stops being a 3-in-8 accident and becomes
something we invoke — and the sweep correlation, which never had a mechanism,
is explained away as coincidence.

### v24: audio again, the handover test inconclusive, and the stalls are the chip's

`-a flags=0x41,mask=0x0002,status=8000,handover=1500,loopback`. Audio worked
in both directions again. But the log order matters:

```
call: audio assign ...
call: audio connect: 0x022D on channel 1
reg: connection up (handle 1) repeated while a call is bound   <- spontaneous
call: handover: asked the chip to move the bearer (0x016A)     <- ours, at +1.5 s
reg: connection up (handle 1) repeated while a call is bound   <- after ours
```

The spontaneous re-establishment happened first and the lane was already live
in the first second (2413 changes/s), so **this run does not test the
handover**. To test it the trigger has to be late enough that the lane can be
observed dead before it and live after — `handover=15000` on a run that does
not re-establish on its own would do it. We cannot force the spontaneous case
not to happen, so this may take a few attempts.

**The audio stalls are not ours.** The driver's own bookkeeping, now printed
at the end of every run, says the transport is clean:

```
pcm: blocks 5080 dropped 0 multi 0 empty 0 errors 1 overruns 1107197 ...
```

`dropped` counts receive blocks lost because userspace was too slow: zero, in
every run. `multi` and `empty` are zero too. (`overruns` reads ~1.107 M
whatever the run length — 5 s, 10 s and 20 s give the same figure — so it is a
latched register, not a live error count.) So no block is lost between the
chip and the capture, and the words held over in the capture were held over by
the chip.

That matches the stall lengths sitting on DECT frame structure, and it means
the chip is not producing a fresh coded word for some frames: a bearer
underrun above the TDM slot.

**And the glitches the handset plays are probably our own echo of them.** The
loopback re-sends exactly what it received, stalls included, so an uplink
underrun is handed straight back to the earpiece. The downlink may well be
clean; we have never fed it anything that was not already glitchy. Telling
the two apart needs a locally generated source — i.e. the G.722 encoder —
which is the next piece of work and needs no radio time to build.

### v25: the downlink is clean — the glitches were our own echo

`-a flags=0x41,mask=0x0002,status=8000,handover=1500,play=/tmp/tone440.g722`.

`-a play=FILE` transmits a raw G.722 stream to the handset while a call is up,
one coded word per two TDM frames, looping. The file was made on the
workstation with `ffmpeg -f lavfi -i sine=frequency=440:sample_rate=16000
-f g722`, which produces exactly 8000 bytes/s — the lane's rate — so no
encoder of ours is involved.

**The handset played a clean, steady 440 Hz tone.** So the transmit direction
carries audio without the stalls, and the interruptions heard during the
loopback runs were the loopback re-sending the *uplink's* underruns. The
downlink was never the problem, and the audio defect is confined to the chip's
uplink bearer.

That also means a file can be played into a live call today, with no codec on
our side at all.

Standing after this run:

| | state |
|---|---|
| downlink (base → handset) | **clean**, verified with a locally generated source |
| uplink (handset → base) | works, with ~3 short bearer underruns per second |
| our transport | clean: `dropped 0`, `multi 0`, `empty 0` in every run |
| wiring the audio path | still depends on the chip re-establishing the connection |

The handover trigger is still untested: in both v24 and v25 the chip
re-established the connection on its own before our `0x016A` went out.

## Milestone B: incoming calls (the handset rings)

G7's remaining criterion. The audio path is the same one we now have working,
so this is purely call control plus one new chip mail.

**The sequence**, FP to PP:

| step | what | where |
|---|---|---|
| 1 | page the handset | chip mail `0x0300 {page info, 0x11, link}` |
| 2 | the PP sets up a link; the chip reports it | `0x0200` connection up |
| 3 | CC-SETUP with the mandatory IEs | C-plane, originator side |
| 4 | the handset rings, answers CC-ALERTING | C-plane |
| 5 | the user accepts, the handset sends CC-CONNECT | C-plane |
| 6 | we confirm with CC-CONNECT-ACK | C-plane |
| 7 | bind the audio exactly as today | `0x0251`, `0x0230`, `0x022D` |

**The page.** `0x0300` carries `{page info, 0x11, link}` where `[3..4]` is the
low 16 bits of the handset's TPUI/PMID. The page info byte is `0x08` for an
MM-triggered page, `0x0C` for a CC page, and **`0x2C` when the SETUP will
carry `<<BASIC-SERVICE>>` with service value 8** — wideband, which is what our
C4 uses. A 3 s timer guards it [CR the hardware-analysis notes §6; COMMAND_SET §6.3
`0x0300`]. Note the two sources disagree on the handset number: COMMAND_SET
reads numbers of the form `0x11xx` as ignored, while the stock host sends
exactly that form. The stock behaviour wins; our reading of the pending-list
rule is probably what is wrong.

**Call control** is implemented and unit-tested (`dect_cc_setup()`): an
FP-originated SETUP carrying `<<PORTABLE-IDENTITY>>` (from the subscription,
`dect_mm.port_id`), `<<FIXED-IDENTITY>>` and `<<BASIC-SERVICE>>`, which
EN 300 444 makes mandatory for this direction; then ALERTING moves us to
`RINGING`, CONNECT to `ACTIVE` with a CONNECT-ACK queued. Both transaction
directions now coexist: the flag distinguishes a call the handset places from
one we place, so the two cannot be confused.

**Resolved since, without radio time:**

- **The page's identity bytes.** Our own LOCATE-ACCEPT assigns the TPUI, and
  it packs the 20-bit value as `{0, 0x11, slot}` [CR A-0023]. The low 16 bits
  are therefore `0x11, slot` — exactly the `[3..4]` the stock host sends. For
  our one enrolled handset the page is `0x0300 {0x2C, 0x11, 0x01}`.
- **The `<<FIXED-IDENTITY>>` encoding.** Rather than derive it, take the
  handset's own: a CC-SETUP it sends carries both the identities an
  FP-originated SETUP needs, in the authoritative encoding. The daemon now
  keeps the first pair it sees and persists them with the subscription
  (store version 2; version 1 files still load and simply learn the
  identities on the next call the handset places). So the type, bit length
  and padding never have to be guessed.

**Still open:** whether the chip expects anything between the page and the
SETUP, which only a hardware run will show.

`-a ring=MS` does the whole sequence: MS ms after start-up it pages the
handset, and when the paged handset brings up a link it queues the
FP-originated SETUP. A page is a broadcast the handset can miss, so it is
repeated every 3 s — the interval the stock host guards each page with — up
to four times before giving up. If a link happens to be up already the SETUP
goes straight out with no page at all.

The LAPC side needs nothing extra: a fresh link sets `nlf_pending`, so our
first I-frame carries the new-link flag and establishes the link implicitly,
which is exactly how an FP-originated SETUP is supposed to open one. If the handset answers, the call reaches ACTIVE and the
existing audio bind runs unchanged — so an answered incoming call should have
working audio immediately.

It refuses to page when no identities are stored, and says so, rather than
sending a SETUP the handset would reject.

### v26: the page works — and two bugs of ours behind it (2026-10-19)

First run of `-a ring=MS`. The handset learned its identities from a call it
placed (7 + 7 bytes, exactly the two IEs it puts in its own SETUP) and they
were persisted. Then:

```
[45002] tx M prim 0300 {2c 11 01}
ring: paged the handset, attempt 2
[45226] rx  prim 0200 {01 01}        <- the handset answered the page, +224 ms
ring: CC-SETUP queued -- the handset should ring
[45270..45350] rx 0x0203 x5          <- its page response
reg: cs in 0e 24 b8 ... lc_rc=-1     <- we dropped it
```

**The paging works.** The handset answers in about a fifth of a second, and
the retry mattered: it took the second page, so the single attempt the first
implementation made would have failed outright.

The handset did not ring because of two faults on our side, both now fixed
and both with tests:

1. **The link signature could not be learned from a multi-fragment first
   frame.** A PP signs its Cs frames with the low 16 bits of its PMID, and we
   learn that signature from the first frame of a link. The code only accepted
   the lesson from a *single-fragment* frame, which is what registration opens
   with. A paged handset opens the link with its **page response** — a full
   five-fragment message — so the signature was never learned, the checksum
   could not pass, and every retransmission was dropped the same way.
   `derive_lsig()` solves for the signature from the frame itself and never
   cared about its length; the restriction was the bug.

   Consequence for the contract, stated plainly: corruption in the *first*
   frame of a link cannot be detected, because the signature is exactly what
   that frame teaches us. It is detected from the second frame on. The test
   that asserted otherwise was asserting something we cannot provide.

2. **A dropped segment blocked our own queued messages.** The segment handler
   returned early when reassembly failed, skipping the drain that puts queued
   NWK messages on the wire. An incoming call queues the CC-SETUP *before* the
   handset's first frame arrives, so one unusable segment stranded it for
   good. The drain now runs on those paths too.

Neither bug could show up before: every call so far was placed by the handset,
which opens its link with an establishment frame and sends before we do.

### v27: GATE G7 PASSED — the handset rings, answers, and talks (2026-10-19)

With the two Lc/drain fixes in place and the identities already in the
subscription store, the ring path worked end to end on the first attempt:

```
reg: nwk out --> 85 55 05 05 a0 94 00 11 01 07 01 64   <- LOCATE-ACCEPT (handset checked in)
ring: a link is already up -- sending CC-SETUP without paging
reg: nwk out --> 03 05 05 07 ... 06 07 ... e0 88       <- our FP-originated CC-SETUP
reg: nwk out --> 03 0f                                 <- CONNECT-ACK: the user answered
call: connected (handle 1) -- audio channel assigned
```

**The handset rang, was answered, and the call bound audio on exactly the
same path as a call the handset places** — the lane ran at 2400–3700
changes/s throughout, and the capture decodes to speech
(`work/audio/v27-INCOMING-call-g722.wav`).

Worth noting: the handset had just done a location update, so a link was
already up and the "no page needed" shortcut fired. The page itself was
proved in v26, where the handset answered one in 224 ms.

That closes G7: **a handset rings and talks.**

| criterion | evidence |
|---|---|
| handset rings | v27, FP-originated CC-SETUP after a page or on an existing link |
| it is answered | v27, the handset's CONNECT and our CONNECT-ACK |
| audio passes | v22 both directions by echo; v25 a clean tone to the handset; v27 on the incoming call |

What is *not* done, and should not be confused with the gate: a host-side
G.722 codec. Audio today is echoed (`-a loopback`) or replayed from a file
(`-a play=FILE`). Sourcing and sinking real audio needs the codec, which is a
public ITU standard and can be built and verified offline against the
captures in `work/audio/`.

Still open from the day's work, neither a gate blocker:

- **What wires the chip's audio route.** It comes alive only when the chip
  re-establishes the connection after the channel is assigned. `-a
  handover=MS` sends `0x016A` to provoke that and remains untested, because
  in every run that used it the chip re-established on its own first.
- **The chip's uplink bearer underruns**, ~3 short stalls per second.

### v28-v34: the downlink was never proven, and "Intern" was a call class (2026-10-20)

Seven attended runs on the spare 7520 with a C4, testing the host G.722 codec
and the new `0x6C`/`0x6D` caller identity. Two root causes found, one of them
correcting a claim made here on 2026-10-19.

#### The downlink: a free-running cadence, not a codec fault

| run | downlink source | heard |
|---|---|---|
| v28 | `playpcm`, 440/1500/5000 Hz tone ladder | "absolutely weird" |
| v29 | `playpcm`, 12 s of real speech | "weird noises" |
| v30 | `playraw`, the chip's own v22 lane replayed verbatim | "like a microphone in the wind" |
| v31 | `loopdelay=4000`, the lane echoed through a 0.5 s ring | **clear speech** |
| v32 | `playpcm` + `playsync` | **worked, with glitches** |

v30 is the one that matters: *no encoder of ours was involved*. The bytes were
the chip's own, at its own rate, byte order and two-frame hold pattern, and
they still came back as noise. Our encoder was independently exonerated
offline -- on the v29 material its output is byte-identical to ffmpeg's, 0 of
95936 octets differ.

v31 then separated cadence from content: a loopback delayed by half a second
is clean, so the content may be arbitrarily old. What the chip needs is that
**our transmitted word changes on the same frame boundary its own does**. The
file-playback path free-ran its own two-frame cadence and slipped against the
chip's. A steady sine survives that; speech does not.

**This corrects the v25 entry above.** "The downlink is clean, verified with a
locally generated source" was too strong: v25 established that the downlink
carries a *constant tone*, which is invariant to exactly the defect that was
present. It did not establish that the downlink carries arbitrary content, and
it did not.

`-a playsync` advances the coded stream on the received lane's transitions.
v32 proved it and also showed its first form was wrong: slaving the *rate* to
those transitions drags the chip's uplink underruns (the known holds of 4, 10
and 40 frames) into the downlink and stretches the audio, which is what v32's
glitches were. The shipped form uses a received transition only to realign the
*phase*, and keeps our own two-frame counter for the rate. **That refinement is
not yet verified on hardware** -- v33 and v34 ended before any audio.

#### Caller identity: the C4 parses it, and the call class decides the display

| run | `<<BASIC-SERVICE>>` | `<<SIGNAL>>` | result |
|---|---|---|---|
| v28, v29 | `0x88` | none | rang, non-standard tone, no number |
| v30 | `0x88` | none | same, with IEs before BASIC-SERVICE and `01 81` |
| v31 | `0x88` | none | no identity sent: **standard** ringtone |
| v32 | `0x88` | none | rang, showed **"Intern"** |
| v33 | `0x38` | none | **showed the number, correctly** -- but silent, gone in ~4 s |
| v34 | `0x38` | `0x40` | rang ~4 s, no number, gone before it could be answered |

The ringtone difference between v31 and the rest is the key observation: the
handset **was** parsing our IEs all along and re-categorising the call. The
failure was never the encoding. Three variants that changed nothing confirm
it: screening `00` vs the stock `01`, and placing both IEs before
`<<BASIC-SERVICE>>` rather than after.

`[observed]` With call class 3 the C4 displays the calling number from `0x6C`
correctly. With call class 8 -- what we had sent since G7 -- it labels the call
"Intern" and shows no number. The high nibble of `<<BASIC-SERVICE>>` is
therefore what decides whether this handset treats an FP-originated call as
internal, and our long-standing `0x88` was the reason CLIP appeared to be
ignored.

`[not established]` A call class that both rings and displays. Class 3 calls
are released at about +3.5 s: the log shows the handset acknowledging our
SETUP at the LAPC layer and then sending **no CC-ALERTING and no CC-CONNECT**,
after which the chip reports the connection released. Adding `<<SIGNAL>> 0x40`
makes it alert but suppressed the number, which is unexplained.

**Next step, and it is ours rather than the room's:** the link dies because
nothing keeps it alive once the SETUP is acknowledged. Our CC state machine
sends the SETUP and then goes quiet in `CALL_PRESENT`. Before sweeping more
call classes, give the FP side a keepalive while a call is pending, then
re-test class 3 with and without `<<SIGNAL>>`. Only after a class-3 call
survives long enough to answer is a call-class sweep worth a person's evening.

Also untested: the name IE (`0x6D`) has never been seen displayed, because
every run that displayed anything carried only a number, and `ÄÖÜß` has not
been tried at all.

### Runs A and A2 (2026-10-20): the phase refinement was wrong, and a re-establishment eats the call

**Run A — `playsync` with phase-only realignment: back to noise.** The
refinement described in the v28-v34 section above is wrong and is reverted.
Advancing on our own two-frame rate and merely realigning the phase on each
received transition sounds exactly like the unsynchronised case.

The reason is the stalls themselves. The chip consumes the downlink at the rate
it *produces* the uplink, so during a hold of 4, 10 or 40 frames it consumes
fewer words than a free-running 8000 octets/s supplies, and the surplus is
dropped. Advancing strictly on a received transition cannot over-supply. The
price is that the audio stretches slightly through a stall — and **that is what
v32's glitches were**: the chip's uplink underrun showing through on the
downlink, the same defect Q-0028 §2 asks about, not a separate fault of ours.

`[observed]` Transition-driven advance is required for anything but a steady
tone, and is now what `-a playsync` does.

**Run A2 — a repeated `0x0200` destroys an unanswered call.** The handset rang,
sent CC-ALERTING, and the user pressed accept; no audio followed and the C4 hung
in its accept/reject screen until its battery was pulled. The log:

```
[14501] nwk in <-- 83 01 ...     CC-ALERTING from the handset
[15697] rx prim 0200 {01 00}
        reg: connection up (handle 1), C-plane armed    <- re-initialised
[17677] nwk in <-- 83 07 ...     CC-CONNECT: the user answered
        (no CONNECT-ACK)
        nwk in <-- 83 4d         the handset releases
```

That second `0x0200` is the chip re-establishing the connection after the
channel was assigned — the event this document already credits with wiring the
audio route. `reg_start()` treated it as a new link and re-ran `dect_lapc_init`,
`dect_mm_init` and `dect_cc_init`, so the CC transaction and the LAPC sequence
numbers were thrown away between ALERTING and CONNECT.

A guard for this existed since v18 but tested `call_bound`, which an incoming
call only sets when it *connects*. The whole ringing window was unprotected. It
now also tests `dect_cc_busy()`, true from the moment a SETUP is sent or
received until the call is released.

**This is a plausible contributor to earlier flakiness** and should be kept in
mind when re-reading the v13-v27 runs: any of them whose re-establishment landed
inside a call-setup window would have lost state the same way.

`[not established]` Whether this alone explains the class-3 releases at ~3.5 s
recorded above. Those runs showed the handset sending *no* CC message at all,
which is a different signature from this one, where it sent both ALERTING and
CONNECT. Q-0029 still stands.

### Runs A3-A6 (2026-10-20): the downlink measured, not guessed

The mid-call re-establishment fix works: A3 shows `nwk out --> 03 0f`
(CONNECT-ACK), `call: connected`, `0x0242 {01 41 02 00}` and the new guard
firing — *"connection up repeated while a call is in progress — keeping call
state"*. A call now survives the chip re-establishing underneath it.

The downlink is a separate fault, and `-a playlog` plus the cadence counters
replaced listening with measurement.

**A4, advance on every received transition.** The counters:

```
8000 frames, 3998 advances, 3998 rx transitions -> 7996 octets/s
8000 frames, 1186 advances, 1186 rx transitions -> 2372 octets/s
8000 frames,  153 advances,  153 rx transitions ->  306 octets/s
8000 frames,    0 advances,    0 rx transitions ->    0 octets/s
```

`[observed]` **The uplink lane carries the handset's microphone**, so when the
user stops talking its coded words stop changing. Slaving our rate to those
transitions starves the downlink: over a ~30 s call we delivered **63 596
octets, 7.9 s of audio, 26 % of a real-time stream**. The recorded stream
decodes to intelligible speech, so the content and the encoder were never the
problem — only the rate was. This also explains v32's "worked with glitches":
the user was talking.

**A5, constant rate with the phase locked once** from the first received
transition after the bind. The counters are then exactly right —
`8000 frames, 4000 advances -> 8000 octets/s (phase locked)`, independent of
whether anyone is speaking — and the handset hears **loud** noise.

That is informative. Full-scale broadband output is what a G.722 decoder
produces from structurally wrong octets, so our bytes reach the decoder and are
mis-parsed rather than lost or mis-timed.

**Standing after the session:**

| property | state |
|---|---|
| our encoder | `[observed]` correct — byte-identical to ffmpeg on this material |
| transmitted content | `[observed]` correct — the logged stream decodes to speech |
| transmitted rate | `[observed]` correct — 8000 octets/s, measured, steady |
| frame parity / octet order on the lane | `[not established]` — the remaining unknown |

`-a playsweep=MS` walks the four (parity, octet-order) combinations during a
call and prints which is active. A6 attempted it; the call was released before
it reached the bound state and produced no data, so **the sweep is written and
staged but has never run**. It is the cheapest remaining experiment.

`[assumed]` The model that fits every observation is that the chip consumes the
lane as **one octet per frame** — one byte of the 16-bit word on each parity —
rather than one 16-bit word every two frames. Writing the same word into both
frames is then correct, parity matters absolutely, and getting it wrong swaps
adjacent octets: noise for speech, still a tone for a sine, which is exactly
what v25 through A5 showed. Unverified.

### Correction: `0x38` is the ULE call class, not an external-call setting (2026-10-20)

`[CR A-FPCallSetup-3]`, answering Q-0029, overturns the reading recorded in the
v28-v34 section above. **Do not use `-a basicsvc=0x38`.**

`[analysed]` The stock FP-originated SETUP constructor picks the call
class high nibble from a three-way selector: **0 → `0x80` normal**, **1 →
`0x90` internal**, **2 → `0xB0`** (a third class whose product use is not
established). The low nibble is a *separate* runtime input masked to four bits,
so the emitted octet is `class_base | (service_attributes & 0x0F)`. The
ordinary incoming telephone-call producer initialises the selector to **0**, and
only a further guarded branch — tied to CAT-iq eligibility and internal-call
state — moves it to 1.

`[public standard]` High nibble 3, our `0x38`, is the **ULE** call class. It was
never an external-call setting, and the number appearing on the C4 under it is
not a result we can build on.

So `E0 88` was right all along and **the "Intern" display has another cause**.
Our IE ordering is confirmed correct against the stock builder, which emits
identities, `<<BASIC-SERVICE>>`, then conditionally `<<SIGNAL>>` (`0xE4`),
`0x6C`, `0x6D`, `<<DISPLAY>>` (`0x70`), `<<CODEC-LIST>>` (`0x7C`) and
`<<CALL-INFORMATION>>` (`0x7E`).

`[analysed]` Three candidates we do not send at all now have verified
stock producers: `0x7E` CALL-INFORMATION, `0x70` DISPLAY and `0x7C` CODEC-LIST.
`[not established]` whether any of them is what makes the C4 show a caller
rather than "Intern" — the answer explicitly declines to attribute it.

`[analysed]` `<<SIGNAL>>` is conditional on a request byte and is
**not** intrinsically `0x40`; it is not a keepalive.

`[not established]` Who releases the class-3 call at ~3.5 s. The stock CC task
has timers started with 20000, 30000 and 36000 units, and the normative CC.03
setup timer is 20 s, but none of that identifies the actor. The answer is
explicit that we should **not** add a keepalive or a repeated SETUP on this
evidence, and should instead capture the first release-producing action.

**Next bench steps, in the order the room proposes them**, one variable each:

1. `E0 80` versus `E0 88`, media held constant — does the service-attribute
   nibble affect the C4's display or alerting?
2. A single valid `<<CALL-INFORMATION>>` subrecord versus its omission, with
   `E0 88` and `0x6C` otherwise unchanged.

Only values whose semantics we actually know; no synthetic line identifiers
invented from a table.

### Run B3 (2026-10-20): the whole parity/order space is ruled out

Two faults were live going into this run; one is fixed and one is now much
better bounded.

**Call setup: fixed.** Runs A6, B1 and B2 all failed the same way — our
CC-SETUP went out, the C4 acknowledged it at the link layer, sent **neither**
CC-ALERTING nor CC-CONNECT, and the chip reported the link released about two
seconds later. Byte-identical SETUPs had rung the same handset minutes
earlier, so it was not the message.

We had hardcoded **transaction value 0** for every FP-originated call. A call
that ends without a clean release — A5 was cut off mid-media — can leave the
portable part believing that transaction is still open, and it then
acknowledges the next SETUP and discards it. `dect_cc_set_next_ti()` now
rotates 0..6 per call. After that change and a handset power cycle the call
established, was answered, and ran for two minutes. `[not established]` which
of the two fixed it; both were applied together, and the rotation is correct
either way.

**Downlink: the parity/order space is empty.** `-a playsweep=6000` walked all
four (frame parity, octet order) combinations twice during one call. **All
four sound like noise.** So the model behind them is wrong:

`[observed]` The chip does **not** take the lane as a 16-bit word held across
two frames, under any parity or octet order. That retires the hypothesis
recorded in the A3-A6 section.

Reviewing what the sweep actually covered shows the search was narrower than
intended: every one of the four variants still wrote **both** octets into
**both** frames of a pair. A convention where the chip takes one octet per
frame from a single byte position would read `octet0, octet0, octet2, octet2`
under all four — duplicates — and would sound identical in each. The layout
that convention needs is **one octet per frame, advancing every frame**, which
none of the four did.

`-a playbyte=0|1|2` now selects the lane layout (0 = a held word, 1 = one octet
per frame in the low byte, 2 = the high byte) and `-a playsweep` walks all six.
**Written and built, never run.**

**Where the downlink stands:**

| property | state |
|---|---|
| our encoder | `[observed]` correct, byte-identical to ffmpeg |
| the content we transmit | `[observed]` correct, the logged stream decodes to speech |
| the rate we transmit | `[observed]` correct, 8000 octets/s measured |
| a held 16-bit word, any parity, any octet order | `[observed]` **ruled out** |
| one octet per frame | `[not established]` — the next thing to try |
| the lane index for the downlink of a channel | `[not established]` — Q-0030 Q7 |

Clean-room Q-0030 carries all of this and is pointed at the PCM transport,
which A-FPCallSetup-3 §6 identifies as the only place the answer can be.

### A-0030 received, and the noise sorted by supply rate (2026-10-20)

`[CR A-0030]` answers Q-0030 from the stock host's PCM transport. The
answer and a desk pass over our own recordings change the plan: the lane
layout was never the fault, and the one-octet-per-frame layouts queued in the
B3 section are the convention of a slot map this box does not run.

#### What the stock host does on the downlink voice lane

All `[analysed]` unless marked.

| property | stock host |
|---|---|
| octets per 16-bit lane word | two, the **earlier octet at the lower byte address** |
| frames per word | the **same word written into both frames** of a pair, every frame, no gaps |
| rate | 32 coded octets per 4 ms DMA period, unconditionally: 8000 octets/s, **not adapted** to the uplink, no notion of the chip's holds (Q8) |
| pairing phase | anchored to the host's own 4 ms period; **not** derived from the chip, a frame counter, the MAC frame or any status bit (Q2). 4 ms is not a multiple of the 10 ms MAC frame, so the stock phase drifts against it — read as evidence, not proof, that the chip does not care `[assumed]` |
| direction symmetry | uplink and downlink use the same word, cadence, repeat and byte order (Q3); raw index = logical index XOR 1, applied identically both ways, so **no RX/TX index transformation** (Q7) |
| idle fill | `0xFEFE` in every voice word of every frame when a slot is closed or muted; `0x0000` and `0xFFFF` are treated by the host's own alignment logic as "not a live lane" (Q5) |
| priming | a word-level alignment handshake over logical word 5 before any media (§5 of the answer), a configurable mute ramp of `0xFEFE`, and a first-call flag that the control client uses to fill its buffer with `0xFF` once; nothing establishes an octet phase (Q6) |
| slot map | logical words 0 control, 1-3 voice bearers, 4 analog line, 5 host↔chip status; four DECT PCM channels, `[observed]` in a stock boot log |

The **dense** map (five bearers, two to a word, one octet per frame, parity
absolute) exists in the stock driver for other hardware and reports six
channels. That is exactly the `playbyte=1|2` model, and it is the map this
box does **not** run `[CR A-0030 §9]`. The layouts stay selectable; they are
no longer the next experiment.

Logical word 5 is raw word 4 under the XOR-1 rule, which makes the long-open
"slot-4 word `0x4008`" (the maintainers' notes) the chip's side of the status word.
`[assumed]` until read against the handshake values in the answer.

#### The answer's own tests, against what has already run

- **T1** (feed the writer into the reader at the desk): passes by inspection.
  `tx_block()` puts the earlier octet at the lower address; the extractor
  reads both bytes in address order and that decodes to speech.
- **T2** (parity × octet order): that was B3. All four were noise.
- **T3** (write all three voice words): moot. The loopback writes raw word 0
  alone and the handset hears it, so the chip reads this channel's downlink
  from the word its uplink arrives on.
- **T4** (`0xFEFE` idle fill from driver start): cheap, `-a voiceidle=0xfe`,
  can ride along with anything.

So the answer's §8 closing request applies to us: a byte-exact replay of the
chip's own words (v30) and the stock convention at a steady rate (A5, B3)
both fail, while the chip's own words echoed back (v22, v31) pass. That is
the contradiction a Q-0031 now carries.

#### Desk checks on the recordings

- **The chip's uplink holds are all even.** Hold-length histogram of raw word
  0 over the whole v22 capture: 201 113 words held 2 frames, 230 held 4, 82
  held 10, one 8, one 100, and four 1-frame runs at the idle boundaries.
  `[observed]` The pairing parity of the uplink never flips mid-call, and the
  deficit is 606 words in 50.5 s of live lane: **7976 octets/s, 0.3 % below
  8000**. Q-0028 §2.1 already recorded that the chip reports B-fields at the
  full rate with zero CRC errors while this happens, so the deficit is the
  chip's media path, not the air.
- **The `overruns` figure in the `pcm:` exit line is sticky, not a rate.** It
  reads ~1.108 M in every run since v25 whether the run had 19 212 or 38 076
  blocks, so it is a register value accumulated before DMA ran. `dropped 0`
  and the template-frame arithmetic (`tx_frames` equals the daemon's blocks
  × 32 minus a fifo remainder) stand: nothing was inserted mid-call.
- **v30 was 12.0 s of the raw lane** (192 000 bytes at 16 000 B/s), so it did
  carry the chip's own hold pattern, free-running.
- **The source was not time-stretched.** Voice pitch of the A4 transmit log
  decoded: median 160 Hz; of the v22 uplink decode, the same voice: 158 Hz.
  Both directions carry normal-rate G.722 and the PCM source was mono.
- **ITU silence octets match the chip's.** `ffmpeg` encodes silence as
  `F8 FA FB DE F3 …`; the chip's uplink during silence shows `F8F8 FBFB FAFA
  FBDF DADA FADE`. The lane's octet layout is ITU G.722 on the uplink; the
  stock host's `0xFE` substitute is the same family.
- **Which mis-readings produce "loud" while a sine survives.** Each
  transformation was applied to the A4 speech stream and to 440/1500/5000 Hz
  tones, decoded with ffmpeg, and measured (level against the clean decode,
  spectral centroid, flatness, peak):

  | mis-reading | speech | 440 Hz tone |
  |---|---|---|
  | octet order swapped in the word (A-0030 §7 item 1) | −0.7 dB, intelligible | tone intact |
  | pairing offset by one octet (§7 item 2) | 0 dB, intelligible | tone intact |
  | swap + offset | +1 dB | tone intact |
  | one lost word every ~3 s (a 0.3 % surplus dropped cleanly) | 0 dB, inaudible | intact |
  | each octet read twice (one octet per frame from a held word) | **+12 dB, centroid 520 Hz: loud rumble** | clean tone at 219 Hz |
  | each pair read twice (a word read every frame) | **+10 dB, rumble** | clean tone at 219 Hz |
  | sub-band bit fields swapped / upper-band bits swapped | +16 to +20 dB | tone intact or shifted |
  | bits reversed, random octets | +12 to +15 dB, flat spectrum | **noise** (flatness 0.96) |

  `[observed]` at the desk: the two candidates the answer ranks first cannot
  make "loud noise", a sine does not survive bit-level garbage (v25 heard a
  clean tone, so that is out), and the signatures that fit — loud, rumbling,
  tone preserved — are **duplication**-like. No sampling model of a held
  word yields duplication for our stream while sparing the echoed one, so
  the duplication is not in how the chip reads a frame.
- **Correction to A5 and B3 as logged.** The "phase locked" message fired on
  the first frame after a reset, because the sentinel differs from any word:
  the lock was to a DMA block boundary, not to a chip transition. Both
  parities were still covered (blocks are 32 frames), so the sweep's result
  stands; the label did not. Fixed: the lock needs a change between two live
  words.

#### Sorting every downlink run by the rate it supplied

| run | source | rate relative to the chip's own uplink rate | heard |
|---|---|---|---|
| v22, v24 | loopback | exact, hold for hold | clean |
| v31 | loopdelay 4000 | exact, hold for hold, 0.5 s late | clean |
| v32 | playpcm, advance per received transition | at or below (pauses with the uplink) | clean while the far end talks |
| A4 | same, instrumented | at or below: 26 % of real time | correct while the far end talks |
| v25 | play, 440 Hz file, free-running | steady 8000, above | "clean steady tone" |
| v28 | playpcm tone ladder, free-running | above | "absolutely weird" |
| v29 | playpcm speech, free-running | above | "weird noises" |
| v30 | the chip's own lane, free-running | above (its holds in the wrong places) | "microphone in the wind" |
| run A | playpcm, own rate, phase realigned | above | noise |
| A5 | playpcm, own rate, phase locked once | above, 8000 measured | **loud** noise |
| B3 | all four parity/order variants | above | noise, all four |

Every source that was heard clean supplied the chip at or below its own
rate; every source heard as noise supplied a steady 8000 octets/s, which is
above the chip's measured 7976. The one steady-rate source that sounded
acceptable was a pure tone, which survives chunk repeats and skips.

`[assumed]` **The chip's downlink consumption follows its own media clock,
the one whose deficit shows as the uplink holds, and over-supplying it is
handled badly** (a writer lapping a reader in the 80-byte, two-half buffer
Q-0028 §2.2 sketches would re-present or skip 40-byte halves, which is what a
tone shrugs off and speech does not). Under-supply is benign: A4 held words
for as long as 7 s and the handset simply waited.

This sits awkwardly with A-0030 Q8, where the stock host writes at a constant
rate and never adapts. Either the chip has no deficit under the stock host
(the holds would then be ours to find, in bring-up), or the stock host's
constant rate is tolerated for a reason we cannot see from the host side.
Q-0031 asks both.

#### Test card: one falsification, two outcomes that each say something

`-a playslow=N` holds every N-th word for four frames — a shape the chip's
own uplink produces — so the lane carries `8000 × (1 − 1/N)` octets/s with
the parity untouched. N = 100 under-supplies by 1 %, three times the measured
deficit. `-a playphase=MS` flips only the pair boundary every MS ms, so the
same call also answers whether parity matters once the rate is right.

```
dect-stubd -g -P 0000 -p -t 180 -C \
  -a flags=0x41,mask=0x0002,status=8000,dspsweep=6:30000,playpcm=/tmp/speech.pcm,playslow=100,playphase=10000,playlog=/tmp/txlog-C1.g722 \
  -w /tmp/vC1.raw | tee /tmp/vC1.log
```

Then park: `dect-stubd -M 0x22:0x02:0x022c:01 -M 0x22:0x02:0x015b:00 -t 8`.

Prediction from the model: **speech, clean throughout**, 1 % slow (inaudible),
with the cadence line reading about 7920 octets/s and ~40 held per second.

| heard | meaning |
|---|---|
| clean for the whole call | rate was the fault; parity is irrelevant, as A-0030 says. Next: find the smallest margin and a way to pace from the chip |
| clean in alternate 10 s halves | rate was the fault **and** parity matters; the stock pairing phase would then be a real requirement |
| noise throughout | the model is wrong: the fault is not rate. Nothing on the host side is left to vary; the answer is in the chip image |

Logs to send back: `/tmp/vC1.log` (the `play phase` and `play cadence`
lines), `/tmp/vC1.raw`.

### "Intern": the incoming SETUP carries no line identifier (2026-10-20)

A desk finding from the public CAT-iq text, not a bench result. It turns the
open "Intern" question into one prediction a single call can falsify.

`[public standard]` ETSI TS 102 527-3 V1.7.1 (CAT-iq 2.0, "NG-DECT Part 3"):

| clause | requirement |
|---|---|
| §7.4.5.3.2 | For a first incoming **external** call, the FP shall send the line identifier in a `<<CALL-INFORMATION>>` IE in the `{CC-SETUP}`, "even if there is only one line in the system, and even if the PP is attached to only one line". |
| §7.4.5.3.1, §7.4.5.1 | That line identifier "shall always" travel with a *line type information* subrecord carrying at least the network-delay and second-call-type flags. |
| §7.4.5 (line selection, last paragraph) | "In case of internal call, no line identifier shall be sent." |
| §7.4.5.1 | "Typically, a FP would use line identifiers in the 0..n interval, where n+1 represents the number of lines it handles." |
| §7.4.6.3 | Under the separate *call identification* feature, the FP also sends a call identifier in the same IE of the incoming `{CC-SETUP}`; Figures 42 and 50 add a call status of *CS call setup*. |
| Table 17, note 3 | CLIP for an internal call uses NPI "private numbering plan"; any other NPI marks an external call. We send E.164 (`clipnpi` default 1), which already says external. |

Our FP-originated SETUP has never carried `<<CALL-INFORMATION>>`. To a CAT-iq
2.0 handset an incoming call without a line identifier is shaped exactly like
an internal call, which is what the C4 displays. It also fits A-FPCallSetup-3:
the stock constructor has a `0x7E` producer, conditional on line or call
parameters being present. That answer declined to say whether the ordinary
external path populates it; the profile says it must.

`[public standard]` Encoding, EN 300 175-5 V2.9.1 §7.7.56:

```
7E  L  { type  subtype  value... }...
```

Each identifier value octet carries 7 data bits; bit 8 set marks the last
octet. All values below fit in a single octet.

| subrecord | type | subtype | value octet |
|---|---|---|---|
| line identifier for external call | `00` | `00` | `80 | line` (0..126; 127 is "None", which a PP sends for FP-managed line selection) |
| line type information | `00` | `05` | `80 | SCT<<1 | NDT`; bits 7-3 reserved |
| call identifier | `01` | `00` | `80 | call id` |
| call status | `02` | `01` | `80 | status`; *CS call setup* = 1, *CS call connect* = 5 |

NDT = 1 means a network with significant delay (VoIP); SCT = 0 means second
calls use the common parallel call procedures (VoIP lines). Both are true of a
line that ends at the SIP controller, so our line type octet is `0x81`. The
standard's own examples: line 2 is `00 00 82`, call 2 is `01 00 82`.

A receiver ignores an identifier type it does not understand and carries on
with the next (§7.7.56), so the IE cannot make a GAP-only handset reject the
call.

`[not established]` Whether the C4 applies the CAT-iq 2.0 rules to our base
at all. That depends on the higher-layer capabilities the base advertises,
which the STUB broadcasts and we have not decoded (the maintainers' notes). Also
unknown: what it displays for a line it has no line-settings list for. We do
not implement list access.

**Prediction:** with line identifier 0 and line type `0x81` in the SETUP, and
everything else as in v32, the C4 shows the calling number instead of
"Intern".

`-a lineid=N,linetype=N,callid=N,callstatus=N` adds the subrecords
(`src/libopendectcell/dect_callinfo.c`); none of them means no IE, so the G7 SETUP
is unchanged by default. The test card is GitHub issue #73: D1 is the
baseline, D2 adds `lineid=0,linetype=1`, and D3 adds `callid=1,callstatus=1`,
run only if D2 still says "Intern". Note the ringtone as well as the display;
v31 showed that the C4 picks its ringtone by how it categorises the call.

### Run C1 (2026-10-20): 1 % under-supply, both parities — still noise

Issue #72, run as written except `play=/tmp/speech.g722` (the A4 transmit
log, 7.9 s of the same speech, byte-identical to what A5 sent) instead of the
PCM file, and `ring=6000`, which the card had omitted; the first attempt
without it waited for the handset and was parked.

Timeline: the call bound at 14.9 s, in phase B; phase A from 24.9 s; the
maintainer ended the call after about 23 s because the sound was dangerously
loud even at the lowest volume. Cadence, every second of the bound call:

```
call: play cadence: 8000 frames, 3960 advances, 0 rx transitions, 40 held -> 7920 octets/s
```

(`rx transitions` is only counted with `playsync`, which was not used; the
slot-activity line shows the uplink word changing at ~3950/s, so the far
end's microphone was live.) The transmit log decodes to normal speech, pitch
157 Hz, first loop byte-identical to the A4 source.

`[observed]` **Supplying the chip 1 % below 8000 octets/s, in either frame
parity, is loud "microphone in the wind" noise — the same sound as v30 and
A5.** The supply-rate model of the previous section is falsified; so is the
idea that the parity flip at 24.9 s would change anything.

What has now been varied on the host side with the content held constant
and proven correct: lane word, octet order, frame parity, rate (8000, 7920,
and paced from the uplink), hold shape, and the chip's own bytes replayed.
Every steady-cadence variant is noise; only sources whose word changes
coincide with the chip's own uplink transitions (loopback, loopdelay, A4,
v32) are clean. **Nothing on the host side is left to vary.** The chip's
downlink consumption is tied to its uplink transitions in a way the host
cannot see, and only the chip image can say how. Q-0032 carries this result
to the room as an addendum to Q-0031.

One lead that is cheap to keep in mind: A4 was clean *while the far end
talked*, so a transition-paced source works whenever the uplink changes.
Whether the chip reads the downlink at all while its uplink word is constant
is unknown; under A4 the handset heard pauses, not noise, during those
stretches. That is a question for the room, not a dial.

**Safety note for future runs:** every noise variant has been loud enough to
worry about the handset's speaker. Start the next downlink run at the lowest
C4 volume and end the call at the first sign of noise; the cadence lines are
the measurement, not the ear.

### Runs C2-C5 (2026-10-20): the codec byte of `0x0230` — lane liveness yes, downlink no

`[CR A-0032]` read our bind `0x0230 {41 0002 00 00}` (six parameter bytes:
handle, flags, mask LE, "master", "extra") as selecting a 40-byte media mode
because the last byte, the codec, is 0; the documented wideband selection is
codec 1, with 3 or 4 as "a second wideband-type mode". Four short calls,
loopback recipe unless stated, verdicts from the counters first:

| run | `0x0230` | chip reply | lane (raw word 0) | heard |
|---|---|---|---|---|
| C2 | `{41 0002 00 01}` | `0x0242 {01 41 02 00}` | **dead**: constant `0x0000` for 100 s, although the chip re-established right after the bind as in every live run | nothing |
| C3 | `{41 0002 00 01}` | same | **dead** again, 60 s | nothing |
| C4 | `{41 0002 00 03}` | same | **live at once**, ~3950 changes/s | **loopback works** |
| C5 | `{41 0002 00 03}`, `play=` the A4 speech log at a plain 8000 octets/s | same | live | **"microphone in the wind", loud** — the same as A5 and C1 |

`[observed]`
- **Codec 1 gives no voice lane at all** (two of two calls), while codec 0
  gave a live lane in the four preceding calls and codec 3 in two of two. The
  chip echoes the same `0x0242 {01 41 02 00}` for all three.
- **Codec 3 does not fix the downlink.** A steady 8000 octets/s playback is
  noise with codec 3 exactly as with codec 0. The codec byte is retired as the
  downlink suspect.
- **Codec 3 changes the uplink's hold structure.** In the codec-0 capture
  v22, every 10-frame hold started at the same position within the 10 ms DECT
  frame and recurred every 80 TDM frames for 0.82 s (82 in a row), and 84 of
  the 230 4-frame holds did the same at another fixed position: a reader
  catching up with a writer at a fixed phase. In the codec-3 capture C4 the
  holds start at scattered positions and show no 80-frame period. The
  word-rate figures of C4 (2300-3500 words/s) are not comparable to v22
  because the loopback's change rate drops whenever the speaker is silent.
- **Codec 3's lane came alive without waiting** for the re-establishment, in
  the one sample we have. `[not established]` whether the codec byte is the
  long-open "wiring problem" (lane dead in v13-v17, v20, v23 on codec 0).

`[observed]` at the desk, for A-0032's remaining leads:
- The v22 hold burst (t = 50.1-50.9 s) is **not** a burst of chip mails: the
  log holds no `0x024E` debug-log mails at all, and the only mails within
  ±300 ms of the burst are one instance of the periodic status set (`0xFFF1`,
  `0xF618`, `0xF623`, our `0xF624` poll and its `0xF625` reply) that recurs
  every ~4.7 s throughout the call without a hold burst at the other 18
  occurrences. It also does not coincide with a reported bearer move: the
  status reply shows the same MAC slot and carrier from 19 s to 59 s.
- The frame-rate measurement of A-0031 §7 ran for 500 s mail-only
  (125 150 blocks) but was interrupted without wall-clock stamps when the
  device was needed; A-0032 §2 demotes it, and C1 did not test it either
  (its 4-frame holds are 79 phase steps per second, as A-0032 §2a notes).

**Where the downlink stands after tonight.** Sources whose word changes
coincide with the chip's own uplink transitions are clean (loopback,
loopdelay, A4/v32); every source with a cadence of its own is loud noise, at
8000 and at 7920 octets/s, in both parities, with codec 0 and codec 3. The
chip's uplink word rate tracked our supply in C1 (3960 supplied → 3950-3959
changes/s; 4000 → 3997-3999). The room cannot read the DSP microcode that
services the lane. What is left is (a) a stock lane dump taken during a
connected call, which would show whether a stock chip holds at all, (b) the
chip's per-terminal statistics (`64-byte mode` flag, 28 unknown counters
cleared on read) diffed between a loopback call and a playback call, and (c)
the audio-route primitive's second "on" mode `[CR A-0032 §5.2]`, whose
selecting bit the room has not named. None of these is a dial on a
hypothesis of ours.

### The per-terminal counters were being thrown away in the logger (2026-10-21)

`0xF625` carries, at payload offsets 16..43, **28 per-terminal counters whose
meanings are unknown and which are cleared by reading** `[CR COMMAND_SET §8.1]`,
and at offset 5 the connection's **64-byte-mode flag**. `A-0031` §7 names that
block as the most likely home of anything media-related and suggests diffing it
between a known-good and a known-bad call.

`[observed]` **Our mail logger cut every payload at 24 bytes**, so every run
ever made logged 8 of those 28 counters and discarded the other 20. Fixed: the
logger now prints the whole payload.

What the 8 logged bytes already show, from the codec-3 loopback call C4, with
the hold counts measured from the capture over the same intervals:

| t (s) | long holds since the previous read | counter bytes 16..23 |
|---|---|---|
| 9.3 | 260 | `00 00 00 00 00 00 01 00` |
| 17.3 | 317 | `01 00 01 00 00 00 01 00` |
| 25.3 | 312 | `00 00 00 00 00 00 00 00` |
| 33.3 | 322 | `00 00 00 00 00 00 00 00` |
| 41.3 | 410 | `00 00 00 00 00 00 00 00` |
| 49.3 | 407 | `00 00 00 00 00 00 00 00` |
| 65.3 | 179 | `00 00 00 00 00 00 00 00` |

`[observed]` **None of those eight bytes counts uplink holds**: they read 0 or 1
while several hundred holds occurred in the same window. The remaining **20
counters have never been looked at** and cost nothing to collect on the next
bench run.

**Audit: what else the 24-byte cut damaged.** Across every run ever logged,
exactly two primitives exceeded 24 bytes: `0xFFF1` at 25 bytes (213
occurrences, one byte lost) and `0xF625` at 45 bytes (43 occurrences, 21 bytes
lost). Every conclusion in this record that mentions `0xFFF1` concerns mails we
*send* (sub `0x01` idle fill, sub `0x09`, sub `0x0A` DSP route) or the mere
presence of the received one in the periodic set, never the content of its
25-byte payload, so **no earlier conclusion rests on a truncated byte**. The
received `0xFFF1` is a periodic chip→host status with sub-code `0x04`; its byte
24 is now collected too and has never been seen.

**Why this matters more than it looks.** If one of the 20 tracks holds, we gain
a direct instrument on our own box, and the same counter read once during a
stock call gives the stock hold count exactly — which is the question the whole
stock-capture exercise is being run to answer, obtained without any sampling.
~~Offset 5 is separately worth comparing: ours reads `01` (not 64-byte mode)
in every call.~~ **Wrong, corrected the same day — see "every one of our
connections runs in 64-byte mode" below.** That sentence generalised from the
*first* statistics poll of a run to all of them. The first poll lands about
30 ms after the call binds and reads `01`; every poll after it reads `02`.

### Why a two-frame-per-dump hold measurement cannot settle the hold question

The stock on-demand lane dump prints two consecutive frames per buffer, so each
dump is one sample of "was this word held". For a lane whose words nominally
occupy two frames, the probability that two adjacent frames carry the same word
is `(1 + d) / 2`, where `d` is the rate deficit. Our box measures `d = 0.30 %`,
so the figure to resolve is 0.5015 against a nominal 0.5000.

| deficit | P(two adjacent frames identical) | samples needed (95 % confidence, 80 % power) |
|---|---|---|
| 0.30 % (ours) | 0.50150 | 871,000 |
| 1.00 % | 0.50500 | 78,400 |
| 4.00 % | 0.52000 | 4,900 |

| samples | smallest deficit resolvable |
|---|---|
| 5,000 | 3.96 % |
| 50,000 | 1.25 % |
| 870,000 | 0.30 % |

`[observed]` arithmetic. A few thousand dumps exclude only a **gross** stock
deficit above about 4 %, which is implausible a priori because stock audio is
good. Reaching our own 0.3 % would need of the order of a million samples, i.e.
many hours of a *connected* call, which is also many hours of DECT
transmission and is excluded by the project's radio rule. **The quantitative
half of that measurement is therefore not worth session time**; the qualitative
half (what the downlink lane carries while media flows, and at bearer open)
is reachable and still wanted.

### The captures already contained the full mail stream (2026-10-21)

`dect-stubd -w` records **every** TDM slot of every frame, and the control slot
is one of them, so every capture ever taken already holds the complete chip→host
mail stream — including the payload bytes the daemon's own log line cut at 24.
`src/tools/dect-maildump.c` re-frames the control slot offline using
libopendectcell's own decoder, so it agrees with the daemon by construction:

```
dect-maildump call.raw -p f625        # just the terminal statistics
```

It recovers 437 mails from the v22 capture with zero checksum, framing or
sequence errors. **Every past capture is now a full mail log, retroactively and
without the radio.**

#### Negative: no per-terminal counter tracks the uplink holds

Decoding `0xF625` offsets 16..43 as 14 little-endian 16-bit counters, against
hold counts measured from the same capture over the same intervals. The codec-3
call C4, which holds hundreds of times per interval, is the decisive one:

| t (s) | long holds since the previous read | extra frames | the only non-trivial counters |
|---|---|---|---|
| 8 | 174 | 614 | offs 26 = 808, offs 34 = 808, offs 36 = 1348 |
| 16 | 368 | 1366 | 800, 800, 2148 |
| 24 | 274 | 1166 | 800, 800, 2948 |
| 32 | 355 | 1742 | 800, 800, 3748 |
| 40 | 364 | 1344 | 800, 800, 4548 |
| 48 | 433 | 1486 | 800, 800, 5348 |
| 56 | 399 | 1578 | 800, 800, 6148 |
| 64 | 243 | 696 | 800, 800, 6948 |

`[observed]` Every other counter reads 0 or 1 throughout. Offsets 26 and 34
sit at **800 per 8 s = 100/s**, which is the DECT MAC frame rate, not a media
counter. Offset 36 is their free-running accumulator, rising by exactly 800 per
read, which confirms `COMMAND_SET` §8.1's note that bytes 36..39 are **not**
cleared by reading while the rest are.

**So none of the 14 counters tracks uplink holds**, and the idea of using this
block as a hold instrument is dead. Reported to the analysis side as a clean
negative so they do not spend a stock call reading it.

#### Positive, and unexpected: every one of our connections runs in 64-byte mode

`0xF625` payload **byte 5** is documented as `0` = no connection, `1` = normal,
**`2` = the connection is in 64-byte mode** `[CR COMMAND_SET §8.1]`.

`[observed]` Across every capture we hold — v22 (codec byte 0), C4 and C5
(codec byte 3) — byte 5 reads `01` on the first poll, before the call is up,
and **`02` on every poll thereafter**, in all three runs and under both codec
bytes:

```
v22   01 02 02 02 02 02 02
C4    01 02 02 02 02 02 02 02 02
C5    01 02 02 02
```

**We never asked for that mode and did not know we were in it.** `A-0031` §6
named this very flag as "obviously relevant to a buffer-lap theory" and
recommended reading it in a known-bad call "and comparing it against what you
think you asked for". Done, and the answer is that we are in a non-default
buffer geometry on every call.

What the reference says the mode changes, all `[CR COMMAND_SET §6.4, §8.1,
§12]` and none of it independently established here: a data primitive takes two
queue entries and the radio sends them as one 64-byte block only on
connections in this mode; **there is no per-frame credit message in 64-byte
mode**; a different event is sent in place of `0x0156`; and *what switches a
connection into the mode* is listed there as an open question.

That last point joins a thread: `A-0031` §6 flagged a per-connection "frame
credit" primitive adding ten units, where the transmit path consumes one unit
per transmitted block, and called it "if anything in the command set is a slip
knob, this is it" — and ten is one of our observed hold lengths. If 64-byte
mode suppresses exactly that credit mechanism, the two observations are one
thread. `[assumed]`, and not to be dialled on that basis.

**The cheap stock question is now one byte:** what does `0xF625` byte 5 read on
a stock call? The stock host polls the statistics itself every 30 s, so the
value appears in the C1 trace with nothing injected. If stock reads `1` where
we read `2`, we have a real geometry difference between stock and us on exactly
the axis the downlink symptom lives on, and it is the first such difference
found.

### The uplink holds, re-measured after A-RouteMedia-FINAL (2026-10-22)

`[CR A-RouteMedia-FINAL]` closes the static investigation of both audio defects
without finding either root cause, and hands back one testable warning: a run
of identical coded words is **not** by itself a stall, because "equal genuine
coded words are possible" (its §4.3 and Test 2 item 4). It also asks us to
prove where the *first* stale word appears rather than assume the chip.

Both points are answerable from captures we already hold.

#### How much of the "hold" count is coincidence

For each capture: collapse the lane into maximal runs, take the distinct coded
words actually emitted, and compute from their own frequency distribution how
often two *fresh* consecutive words would be equal by chance.

| capture | distinct word values | P(two fresh words equal) | adjacent-equal observed | expected by chance |
|---|---|---|---|---|
| v22 (codec byte 0) | 21 854 | 0.00028 | 610 | 56 |
| C4 (codec byte 3) | 9 537 | 0.00892 | 4 996 | 1 494 |

Broken down by run length, against the chance expectation for a run of that
length:

| run | v22 observed | v22 by chance | C4 observed | C4 by chance |
|---|---|---|---|---|
| 4 ticks | 230 | 56 | 1 626 | 1 480 |
| 6 ticks | 0 | 0 | 502 | 13 |
| 8 ticks | 1 | ~0 | 189 | 0.1 |
| 10 ticks | 82 | ~0 | 111 | ~0 |
| 12+ ticks | 1 (at 100) | ~0 | 171 | ~0 |

`[observed]` **Four-tick runs are largely or entirely coincidence.** In C4 they
run at 1.1x the chance rate, i.e. essentially all of them are two genuinely
equal adjacent coded words. In v22 they are at 4x, so roughly a quarter are
coincidental. **Runs of six ticks and longer are real** in both captures: the
chance expectation is zero to three decimal places.

**This corrects a count given to the analysis side on 2026-10-21.** The figure of
"174 to 433 long holds per 8 s interval" in C4 counted every run over two
ticks; about 1 626 of that capture's 2 610 runs are the 4-tick kind, so the
real anomaly is nearer 1 000 than 2 610. The conclusion drawn from it — that
no `0xF625` counter tracks holds — is unaffected, because no counter tracked
either figure.

#### The holds are on the wire, not in our receive path

`A-RouteMedia-FINAL` Test 2 asks which stage first presents a stale word, and
explicitly includes our own driver as a candidate. Settled from v22:

`[observed]` The long runs fall into **two clusters locked to the 10 ms DECT
frame**, not to anything of ours:

| cluster | count | start position in the 80-frame DECT frame | when |
|---|---|---|---|
| 4-tick runs | 84 of 230 | frame 67, always | spread over the whole call |
| 10-tick runs | 82 of 82 | frame 55, always | one 0.8 s burst at t = 50.1-50.9 s |

**Corrected 2026-10-22, same day:** an earlier version of this table swapped
the two clusters, putting the 10-tick runs at frame 67 and the 4-tick ones at
frame 55. It is the other way round, and the 10-tick cluster is a single
0.8-second burst rather than a feature of the whole call. The the analysis side was
sent the wrong version and has been told.

**And the locking is not general.** Applying the >=6-tick filter (which removes
the coincidental runs) to the codec-3 captures shows their ~1000 real holds
spread across the 80 frame positions, with no position holding more than about
4 % of them. So "the holds are locked to the DECT frame" is true of the
specific v22 clusters above and **false as a general property**. Any scheme
that paces the downlink from a predicted stall position is therefore not
supported by the evidence.

Our DMA block is **32 frames**, and the holds do show mod-32 structure — but it
is entirely derivative. Because 80 ≡ 16 (mod 32), anything pinned to one
residue mod 80 must alternate between exactly two residues mod 32, and that is
what is there: the frame-67 cluster splits 43/41 between mod-32 positions 3 and
19, and the frame-55 cluster sits at 23 and 7. Per degree of freedom the
mod-80 concentration is the stronger of the two (chi-square/dof 29.7 against
19.3).

**So the repeats are present on the TDM lane as delivered, aligned to the radio
frame, and our DMA block boundary does not explain them.** That removes our own
receive path from the list of candidates in that report's Test 2 table, using
data already on disk and no bench time. What it does not do is say which stage
*before* the lane introduces them: that remains `[not established]`, and the
report is explicit that B-field counters and zero DMA drops do not prove fresh
speech.

#### What we take from the report

- **Stop expecting a `0xF625` underrun counter.** Its §4.5 finds every one of
  the ten positions has a writer and none is identified as an underrun; our own
  measurement on 2026-10-21 already found none tracks holds. Both agree: drop
  it.
- **The 80-byte regions are signalling, not a speech ping-pong buffer.** Its
  §4.1 traces a 40-byte *zero clear* and a 78+2 integrity-checked record feeding
  the `0x0203` signalling event. The `40+40` media-buffer idea of Q-0028 §2.2 is
  withdrawn; the numeric match to 5 ms of G.722 was a coincidence.
- **`0xFEFE` has a verified host-side producer** in the stock receive path when
  the source pointer is absent (§4.2), so seeing it does not prove the chip's
  `0xFE`-filled region is being presented. Our own idle handling has to stand on
  its own evidence.
- **`0x0242` is an assignment report, not a speech confirmation** (§3.1, §8).
  Our log line already says "audio channel assigned" rather than claiming audio.
- **Do not** add a deactivate/activate workaround, a post-`0x0200` delay, or
  tune `0x0251` on this evidence; the report rules all three out explicitly.

### The A540 CAT rejects our CC-SETUP (2026-10-22, run E1b)

`[observed]` An FP-originated call to the newly registered **Gigaset A540 CAT**
pages correctly and the handset answers, then **rejects the call** before
ringing. The handset never rang.

```
FP -> PP   13 05 05 07 80 a8 02 de ad 00 01 06  07 a0 a5 de ad 00 00 b8  e0 88
                 |<-- PORTABLE-IDENTITY ----->|  |<-- FIXED-IDENTITY -->|  ^BASIC-SERVICE 0x88
PP -> FP   00 71 05 07 80 a8 02 ...        (identity echo)
PP -> FP   93 5a e2 32                     CC-RELEASE-COM, RELEASE-REASON 0x32
```

Both sides address the transaction correctly (we send F=0 TI=1, it answers F=1
TI=1), so this is not a transaction-identifier fault: the handset understood the
SETUP and refused its **content**.

**The contrast that makes this actionable:** the FRITZ!Fon C4 accepts this very
SETUP -- G7 passed with it, the C4 rang and answered -- and where the C4 does
release, it releases with `e2 00`, RELEASE-REASON **0** (recorded earlier in
this file). The A540 releases with reason **0x32**. So the defect is in what our
SETUP omits, not in the call machinery.

**The `<<SIGNAL>>` hypothesis, and why it is not yet the answer**
`[CR A-StockTrace-5-Ringing]` proposes the cause is the missing `<<SIGNAL>>`
IE: every stock **external** incoming SETUP carries `<<SIGNAL>>` (`0xE4`)
`0x40`/`0x41` (alerting on, pattern 0/1) right after `<<BASIC-SERVICE>>`
`0x88`, while stock **internal** uses `0x80` with `0x4F` (alerting off). Our
SETUP is confirmed from the wire to carry **no `0xE4` at all** -- three IEs
only -- and our builder already places SIGNAL immediately after BASIC-SERVICE,
so testing it is one flag: `-a signal=0x40`. Worth running.

But two things on our side argue against it being the cause, and it should not
be recorded as the fix until tested:

1. **The signature does not match.** The room's model is a handset that accepts
   the call object and stays silent for want of an alerting command. The A540
   did not stay silent: it sent `CC-RELEASE-COM` with a **non-zero** reason
   ~1.3 s after the SETUP. That is a refusal on content, not a call waiting to
   be told to ring.
2. **`<<SIGNAL>>` is not required to ring.** The table above is our own
   evidence: v28, v29, v30 and v32 all used `<<BASIC-SERVICE>> 0x88` with **no
   `<<SIGNAL>>`** and the C4 **rang** every time. And v34 (class `0x38` +
   `<<SIGNAL>> 0x40`) rang but **suppressed the number**, so SIGNAL is not a
   free addition -- it interacts with the display path.

So the A540 may simply be stricter than the C4, which the flag will show, but
the C4 data falsifies "SIGNAL is a precondition for alerting" outright.

**`0x32` = "Insufficient resources"** `[CR A-StockTrace-5]`, verified
independently here against Wireshark's `packet-dect-nwk.c`
(`DECT_NWK_S_IE_FL_RELEASE_REASON_INSUFFICIENT_RESOURCES = 0x32`; neighbours
`0x31` Overload, `0x33` Insufficient bearers available, `0x34` IWU congestion).

**This reframes the problem away from SETUP content.** The content-rejection
codes are `0x03` "Mandatory information element missing" and `0x04` "Invalid
information element contents". `0x32` is neither, so the A540 is not telling us
an IE is missing or wrong -- which also demotes the `<<SIGNAL>>` hypothesis
above to a cheap side-test rather than the fix, and is consistent with the C4
ringing on the same three-IE SETUP.

**What our base actually does after the SETUP** `[observed]`, run E1b:

| t (ms) | event |
|---|---|
| 90166 | we send CC-SETUP (`tx cplane 25 bytes`) |
| 90246 | the handset's **LCE page response** (`00 71`) is parsed -- *after* our SETUP |
| 90326 | handset supervisory frame `13 01 01`, decoded `nr=0` |
| 91166 | **we retransmit the same 25 bytes** -- our DLC saw no acknowledgement |
| 91366 | handset sends `93 5a e2 32`, RELEASE-COM / Insufficient resources |
| 91426 | chip reports the connection released |

Three things to take from it. **We send no traffic-bearer or audio mail on this
call at all** -- no `0x0230`, nothing; the FP side sends the SETUP and then only
LAPC retransmissions, which is the "goes quiet in `CALL_PRESENT`" problem
already identified above and never fixed. **We send the SETUP before the
handset's page response arrives.** And **the SETUP is not being acknowledged**
at the DLC layer, or we would not retransmit at exactly +1000 ms.

`[not established]` which of those is causal, and `[not established]` whether
the `nr=0` reading is right -- the LAPC field decode should be checked against
the registration flow, where the acknowledging frame reads `13 21 01` with
`nr=1`, before any weight is put on it.

**A unifying hypothesis, and why it does not hold.** `[CR A-StockTrace-5]`
suggests one broken U-plane/bearer path could explain both this reject and the
project's audio defect. It cannot: **our loopback produces working audio**, so
the traffic bearer does come up and carry media on a C4 call. The downlink
defect is that a *host-supplied* source is noise while a *transition-paced* one
is clean -- a media-ingestion problem on an established bearer, not a missing
bearer. The two should stay separate until something ties them together. We hold no copy of
EN 300 175-5 locally and the code is **not** guessed here; fetch the spec
(see the note on etsi.org needing a browser User-Agent) and decode it before
changing the SETUP, because the reason code almost certainly names the missing
element outright and would save a round of blind IE-adding.

**Where this meets the "Intern" work.** Our SETUP carries exactly three
elements: PORTABLE-IDENTITY, FIXED-IDENTITY, BASIC-SERVICE `0x88`. It carries
no `<<CALL-INFORMATION>>` (`0x7E`), no `<<CALLING-PARTY-NAME>>` (`0x6D`), no
`<<SIGNAL>>`. The "Intern" section of this file established that stock internal
calls carry `0x6C`+`0x6D` and external calls carry `0x7E`, and that stock sends
`<<SIGNAL>>` in both. A handset that refuses a SETUP with no signalling
instruction and no call information is consistent with that reading. The queued
D2/D3 caller-identity runs therefore now have a second reason to run, and
should be run against the **A540**, where the difference is a hard accept/reject
rather than a cosmetic display difference.

**Note on the C430A.** Earlier the same day a C430A -- re-attached on its
FRITZ!OS-era subscription -- originated calls to us repeatedly and we bound
audio each time (`0x0230 {41 0002 00 03}`, `0x022D on channel 1`). So
PP-originated calls work and bind audio; only the FP-originated direction is
refused. That also means the audio experiments can be run on a
**handset-originated** call if the SETUP problem takes time to fix.

#### Fix applied: the CC-SETUP now waits for the page response (2026-10-22)

`[observed]` The failure is a lost frame, not a rejected one. Our SETUP went
out as an I-frame with `ctrl 00` (N(S)=0); no N(R)-advancing acknowledgement
followed; we retransmitted at +1000 ms; the handset then answered `ctrl 21` ->
**N(R)=1** and released ~200 ms later.

**Correction to an earlier reading here** `[CR A-StockTrace-5 §6]`: the single
`13 01 01` (N(R)=0) frame is **not** by itself proof of non-acknowledgement --
both `13 01 01` and `13 21 01` occur in *healthy* stock exchanges, `nr=0` being
normal where the PP is expecting a fresh I-frame 0. The conclusion stands but
rests on the pair of facts, not that one frame: **no N(R)-advancing ack
appeared, and we retransmitted.** Stock shows the healthy signature --
acknowledgement, no retransmission -- in every captured call. So the handset only ever saw the SETUP
a second late, and `RELEASE-REASON 0x32` is what it says when a call arrives
after it has given up waiting -- a resource/timing reason, consistent with the
code meaning and with the C4 ringing on the identical SETUP.

**Cause:** we queued the SETUP on `DECT_PRIM_CONN_UP`, the chip's link-up
event, which fires *before* the handset answers the page. In run E1b we
transmitted at t=90166 and the handset's LCE page response was parsed at
t=90246.

**Change** (`src/stubd/dect-stubd.c`): a new `RING_WAIT_PAGE_RESP` state. On
link-up the ring no longer sends; it waits. The SETUP is queued from the NWK
dispatch when a message with `pd == DECT_PD_LCE` arrives -- the page response,
which until now fell through to `dect_mm_rx` and was discarded. A
`RING_PAGE_RESP_MS` (1500 ms) deadline sends it anyway, so a handset that
answers differently still gets a call instead of silence. The three places
that built a SETUP are now one `ring_queue_setup()`.

**Stock confirms the ordering** `[CR A-StockTrace-5 §6]`, decoded from four
external incoming calls, frame order unambiguous (the tracer has no wall clock,
so the millisecond figures below are a 10 ms-frame estimate, not measured):

```
MAC paging                     FP
LCE page response   00 51 00 71  PP
short LCE confirm              FP      <-- we do not send this
CC-SETUP                       FP      <-- after the page response, 4/4 calls
[~15 MAC frames, FP holds the MAC connection up]
DLC ack  13 21 01 (nr=1)       PP      (~150 ms after SETUP)
[~7 MAC]  DLC ack 13 21 01     PP
[~5 MAC]  CC-ALERTING          PP      (~270 ms after SETUP)
```

A healthy exchange is acknowledged well inside our 1000 ms retransmit timer, so
stock never retransmits a SETUP -- none of the five captured SETUPs is
duplicated.

`[not established]` that our change is sufficient. Two differences from stock
remain. **We do not send the short LCE confirm** between the page response and
the SETUP. And in the gap **stock keeps continuous MAC frames flowing** to hold
the connection up while the call is pending, where our FP side still emits
nothing -- the "goes quiet in `CALL_PRESENT`" hole. If the A540 still releases
after this change, those two are the next suspects, in that order.

#### Run F2: the ordering fix works, and is not the cause (2026-10-22)

`[observed]` With `RING_WAIT_PAGE_RESP` in place the DLC fault is gone and the
handset **still refuses the call**:

```
60546  page (answered on attempt 1 of 20)
60666  LCE page response   00 71
60667  our CC-SETUP                 <-- now after the response, as stock does
60847  13 21 01  nr=1               <-- handset ACKNOWLEDGES, 180 ms, no retransmit
60887  93 5a e2 32                  <-- RELEASE-COM / Insufficient resources
```

The acknowledgement now lands at 180 ms against stock's ~150 ms estimate and
**we no longer retransmit at all**, so the lost-frame mechanism is genuinely
fixed. But the reject survives it. **The lost frame was a real bug and not the
cause**, and the expectation recorded in the previous section -- that fixing
the ordering might be sufficient -- is falsified.

The 40 ms between the acknowledgement and the release is the useful number: it
is an **immediate refusal**, not a timeout, and it falls well before stock's
~270 ms alerting point.

**The strongest remaining signal, stated as fact rather than inference: on an
FP-originated call our base sends zero bearer or audio mails.** Grepping the
whole call window gives no `0x0230`, no `0x0251`, nothing. We offer the handset
a call with no traffic bearer behind it, and "insufficient resources" is what a
PP would reasonably say when asked to take a call it has no U-plane for.

Two things are being chased from here, and they are different sizes of work:
`-a bindearly` (assign the audio channel on link-up, before any call) is the
cheap test of the bearer reading; and `[CR A-StockTrace-5 §6]` has been asked
whether stock's "~15 MAC frames" in the SETUP->ALERTING gap are bearer
establishment or merely keepalive, which decides between "send a keepalive" and
"establish a U-plane bearer".

**Unrelated bug found while fixing the counter:** paging gave up after
`RING_PAGE_RETRIES` = 4 pages in 12 s, but the A540 needs ~30 s to
re-synchronise after our base goes off air between runs (run F1: paged
45/48/51/54 s, gave up 57 s, handset's first connection 75.8 s). That was
masking the real failure in every earlier run. Now 20 attempts over 60 s, and
the handset answers on attempt 1. The attempt counter also printed one too high.

#### Run F3 and the spec: the bearer is a MAC-level object the chip is not being told to build

`[observed]` `-a bindearly` **does not fix the reject.** It fired on the ring
link and the chip was told to assign the audio channel 128 ms *before* the
SETUP went out:

```
60351  connection up (handle 1)
60351  bindearly: tx 0x0251 {01 01 2b}, tx 0x0230 {01 41 02 00 00 03}
60479  LCE page response, then our CC-SETUP
60699  93 5a e2 32   RELEASE-COM / Insufficient resources
```

So **a chip-side audio-channel assignment is not an over-the-air traffic
bearer.** That is the useful negative result: the two were being conflated.

**What stock does in the gap** `[CR A-StockTrace-5 §7]`, from MAC frame
*size* rather than density (the tracer logs every slot, so density is 100 %
even at idle): idle is **36-byte** frames (dummy bearer / beacon); from the
page response onward, through the SETUP->ALERTING gap and into the call, the
frames are **52-67 bytes, A-field plus B-field**. A full duplex bearer with an
allocated B-field (the U-plane slot) is therefore up and transmitting *before
the call is answered*, while only three C-plane frames cross in that window --
which a keepalive on an idle link could not produce. `[not established]`
whether FP or PP initiates it; both transmit on it, and OTA cannot say.

**A collision theory, raised and rejected.** EN 300 175-5 §9.5.2.3 describes a
PT releasing an FT-initiated call with exactly our signature -- `(F, TV) =
(1, TV1)` and reason "Insufficient resources" -- when the PT is simultaneously
establishing its own outgoing call, explicitly including **an LiA service
call**, and this handset *does* open a periodic service call (`03 05`, then
`03 60 77 04 c3 00 NN 00` with an advancing counter, then a normal `03 4d`
release). It fits the symptom well. **It is not what happened:** in both F2 and
F3 the only messages in the window are the page response, our SETUP and the
reject -- **no competing `03 05` from the handset** -- and its service call had
released 1.1 s (F3) and 2.1 s (F2) before we paged. §17.3.3 (a call already in
progress) is unsupported for the same reason. Recorded because the clause fits
the bytes so exactly that it will be proposed again.

**Where this leaves it.** The handset acks a well-formed SETUP and refuses
40 ms later because there is no U-plane behind the call, and the thing that
builds the U-plane is a **MAC traffic bearer**, not `0x0230`. The open question
is now **which host->chip primitive makes the chip establish a duplex traffic
bearer for an FP-originated call** -- and it cannot come from the air trace,
because that interface is not exposed on the 7.56 box. On a PP-originated call
the handset establishes the bearer, which is why every working call so far has
been handset-originated and why this never surfaced before.

#### ANSWERED by the specification: the basic connection must exist before the SETUP

`[CR EN 300 175-5 V2.9.1 §9.3.2.3]`, "Selection of lower layer resources",
closes this. The spec is explicit and it matches every observation:

> Both the `<<CONNECTION-IDENTITY>>` and the `<<CONNECTION-ATTRIBUTES>>`
> elements shall be omitted from all messages for a call establishment relating
> to a **basic connection**. **If this basic connection is not already
> established when the {CC-SETUP} message is received, the call shall be
> rejected.**

and, earlier in the same clause, "If suitable resources are not available and
cannot be established the PT shall reject the call."

Our CC-SETUP omits both elements, which *is* the declaration of a basic
connection. The handset therefore looks for an already-established basic
connection, finds none, and rejects -- which is exactly what we see, with
`RELEASE-REASON 0x32` "Insufficient resources", 40 ms after it acknowledges a
syntactically perfect SETUP. It also explains why the handset acks first and
refuses second: the SETUP is well formed, the *resources* are not there.

This agrees with `[CR A-StockTrace-5 §7]` independently: stock has a full duplex
A+B bearer (52-67 byte MAC frames against 36-byte idle) running **from the page
response onward, before the SETUP**. Stock establishes the connection first and
sends the SETUP into it. We page, take the C-plane link, and send a SETUP into
nothing.

**Required order, and what we do:**

| step | stock | us |
|---|---|---|
| page | yes | yes |
| LCE page response | yes | yes |
| short LCE confirm | yes | **no** |
| **establish the basic connection (MAC traffic bearer)** | **yes** | **NO** |
| CC-SETUP | yes | yes |

**The "advanced connection" shortcut does not apply.** §9.3.2.3 also says that
if `<<CONNECTION-ATTRIBUTES>>` marks a connection identifier "unknown", the PT
shall itself initiate the establishment before its first response -- which would
have been attractive, since PP-initiated bearers demonstrably work here. But the
clause opens "The following procedures shall only be used for **advanced
connections**", and a basic speech call is a basic connection; the element is
7-61 octets of slot types, MAC services and bearer counts besides. Not the route.

`<<BASIC-SERVICE>> 0x88` is **correct** and is not implicated: call class
`1000` = "Normal call setup", basic service `1000` = "Wideband speech default
setup attributes" (§7.6.4), which is what stock sends for an external incoming
call. No `<<IWU-ATTRIBUTES>>` is needed, because that is only required when the
basic service is "other" (`1111`).

**So the whole problem reduces to one unknown:** which host->chip primitive
makes the chip establish a basic connection / duplex traffic bearer. Confirmed
from the stock side too `[CR A-StockTrace-5 §8]`: in AVM's own driver the DSP
media cross-connect (`dect_connect <chn_dst> <chn_src>` within GenDSP, the
analogue of our `0x0230`) is a **separate step** from the OTA bearer. The chip
mail is not in the analysis side's archive -- the traced window contains no
FP-originated call, and at debug level `0x0f` the driver logs only primitive id
and length, not payload.

#### The mechanism, found in the paging message (EN 300 175-5 §14.2.3 / §8.2.4)

The missing connection is **requested in the page**, not established by a
separate primitive. `[CR EN 300 175-5 §14.2.3]`:

> The {LCE-REQUEST-PAGE} message **may contain extended details of the required
> MAC layer service**. In this event the responding PT **may use these service
> details to start immediate establishment of the required service at the MAC
> layer**. **In all other cases, the responding PT shall only establish the
> minimum MAC layer service needed for a single C-plane link.**

So the sequence is not "FT builds a bearer": the **PT** builds it, on being
paged, and the page tells it what to build. This also matches the MAC: basic
connection control messages (§7.2.5.2, Part 3) use the "first PT transmission"
code -- `access_request` comes from the PT and the FT answers `bearer_confirm`.

A **full format** page with TPUI carries (§8.2.4.2):

| field | octet | meaning |
|---|---|---|
| 1 | 2 | slot type: `0100` full slot, `0101` double slot, ... |
| 2 | 5 | setup info: `0000` none, `0001` **basic connection, attributes negotiation optional**, `0010` basic connection, negotiation **mandatory**, `0011` advanced connection |
| 3 | 5 | additional discriminator, `0000` default |

And the default when setup info is `0000` (Table 80b, LCE code `000`): "MT
basic connection setup on full slot, PP does not initiate attributes
negotiation. **MAC service = no MAC service**." With LCE codes `010`/`011`, or
with setup info `0001`/`0010`, the PP *does* negotiate and the "MAC service =
as negotiated in attributes".

**That is the defect in one line: the handset is building a C-plane link with
no MAC service, because our page never asks for one.** A basic connection
provides "one full slot duplex bearer" (Part 3 §5.6.1.1) -- the 52-67 byte A+B
frames the air trace sees on stock and never on us.

**What this changes.** The fix is no longer an unknown bearer primitive; it is
the **content of our page**. We send `0x0300 {0x2C, 0x11, slot}`, three bytes,
where `0x2C` is **AVM's own page-info encoding** (`0x0C` = CC page, `+0x20` =
wideband SETUP to follow) taken from `[CR the hardware-analysis notes §6; COMMAND_SET
§6.3]`, not the raw DECT paging field. The chip builds the actual
{LCE-REQUEST-PAGE} from it.

**Answered from the stock implementation** `[CR A-StockTrace-5 §9]`, by
analysing the driver's page path rather than reading the document:

- **Stock's `0x0300` page is three payload bytes, `{page-info, 0x11, slot}` --
  byte-for-byte the form we already send.** Both senders (`the stock paging routine`,
  `the stock paging routine`) build it through `a stock mail routine`; the mail-length immediate is
  5 (2-byte opcode + 3 payload). **There is no longer form and no extra
  setup-info octet.** A four-payload-byte sender (`a stock mail routine4`) exists in the
  driver but **no page call site uses it**.
- **The page-info byte is computed per connection, not a constant.** So if any
  MAC-service selection rides in the page at all, it is in the **bits of that
  one byte**. Ours is the fixed constant `0x2C`; stock's is computed. That
  difference alone is worth a diff.
- `[ruled out]` Immediately before each page the driver sets a global to 3 or 7
  from a capability bit `0x40`. It resolves to `a stock retry counter`, the
  paging **retry count** N300 -- **not** a setup-info field. Do not chase it.

So the "longer page mail" hypothesis is dead, and two possibilities remain:
the selection is in the page-info **bits**, or **the MAC service is established
on a different path entirely**. Candidate stock symbols on that path, offered
by the analysis side: **`a stock MAC routine`** -- the name alone reads like
a MAC-layer slot request, i.e. the thing we have been looking for -- and
a stock codec-wait routine, the guard behind a previously
noted "[stock log message]" error,
which implies the stock FP *waits* for the MAC layer to come up after paging.
That error is the FT-side mirror of the handset's "insufficient resources".

#### Page and audio-assign both ruled out; the live lead is the codec (2026-10-22)

`[CR A-StockTrace-5 §10]`, from the stock driver on disk:

- **The page is identical to ours, confirmed at the source.** Stock computes
  the page-info byte in a stock paging routine as `0x0C | (wideband ? 0x20 :
  0)`, selected purely on the `<<BASIC-SERVICE>>` low nibble. For a wideband
  external call that is exactly **`0x2C`** -- our hardcoded constant. There is
  no hidden setup-info selector. With §9's three-byte mail, **the whole page
  matches**. Ruled out.
- **`a stock MAC routine` is not bearer creation.** Its callers are
  a stock codec-change routine and the stock chip-event
  dispatcher, so it runs *in reaction to* a chip "MAC slot up"
  indication, and it calls a stock audio-connect routine, which sends `0x0251` and
  `0x0230` -- **the same two mails our `-a bindearly` already sends**. The name
  misled both rooms: it is "handle MAC-slot-ready", not "request MAC slot".
  **AVM's own code therefore confirms the FT does not create the bearer**,
  exactly as `§14.2.3` says. Our side simply never receives that indication,
  because the A540 never builds the slot.

**So the remaining difference is the codec exchange, and our own SETUP is
internally inconsistent about it.** `[observed]` in our code:

| | stock | us |
|---|---|---|
| sends CC `IWU-INFO` (`0x60`) | yes -- 21 base->hs frames in `trace.raw`, via `a stock IWU routine` | **never** -- the constant is defined and no code path sends it |
| sends `<<CODEC-LIST>>` | -- | **never** |
| answers the handset's `IWU-INFO` | -- | **never**: in run F2 the A540 sent us **20** `03 60 77 ...` (`<<IWU-TO-IWU>>`) messages and we replied to **none** |

And we declare `<<BASIC-SERVICE>>` `0x88`, whose low nibble `1000` is
**"Wideband speech default setup attributes"** -- for which §7.6.4 NOTE 3 says
the codec negotiation mechanism of ETSI TS 102 527-1 applies. **We ask for a
wideband call and then never negotiate a codec.** Stock ties connection
survival to exactly this: a stock codec-wait routine guards
the error "[stock log message]", the
FT-side mirror of the handset's "insufficient resources".

**The cheap test, and it should be the next bench action.** Basic service
`0000` is "Basic speech default setup attributes" (§7.6.4, clause E.1) --
narrowband, **no codec negotiation required**. So:

```
-a basicsvc=0x80     # normal call class + basic speech, instead of 0x88
```

is a one-flag test of the whole hypothesis. If the A540 rings on `0x80` and
refuses on `0x88`, the missing codec negotiation is the cause and the fix is to
implement `<<CODEC-LIST>>` / `IWU-INFO` for wideband. If it refuses on both,
the codec is not it either. `[hypothesis]`, not a conclusion -- but it is the
only remaining structural difference between our call setup and stock's, and
it costs one run.

#### The IWU-INFO negotiation happens during setup, before alerting

`[CR A-StockTrace-5 §11]`, from the air trace. On the FP-originated legs:

```
#16526  B->H  CC-SETUP
#16533  B->H  IWU-INFO      (7 frames after the SETUP)
#16549  H->B  IWU-INFO      (the handset replies)
#16551  B->H  IWU-INFO      (the base replies again)
#16583  H->B  CC-ALERTING   <-- the handset alerts only after this
 ...
#17303        CONNECT, with audio
```

The same shape repeats at `#21292` SETUP -> `#21301`/`#21303` IWU-INFO ->
`#21343` ALERTING.

Three things follow. It is **part of call setup, not mid-call traffic**; it is
**bidirectional**, so we must both **send** it and **answer** the handset's;
and it completes **before the handset alerts** -- which is exactly the window
in which our A540 gives up instead.

`[observed correlation]` the two stock calls that negotiated IWU-INFO reached
CONNECT with audio; the two that did not (`#1292`, `#3201`) were short and
never brought up wideband media. Correlation rather than proof, but it points
the same way as stock's own release guard.

**The structure matches what our handset already sends us.** Stock:

```
B->H   83 60 77 04 c3 12 91 01
H->B   03 60 77 03 c3 02 91
B->H   83 60 77 03 c3 03 91
```

against the A540's `03 60 77 04 c3 00 NN 00`: same message type `0x60`, same
IE `<<IWU-TO-IWU>>` `0x77`, same leading content octet `c3`, middle byte an
advancing sequence. **The handset is speaking this protocol at us and we drop
every frame.** `[not established]` what `c3`, the sequence and `91` mean; the
the analysis side can decode the record fields from the stock exchange when needed.

`[CR, AVM-internal, for completeness]` `a stock IWU routine` builds
a host->chip mail (`byte0 0x8A`, `byte1 0x0C` = task) whose length scales with
the codec-list size, called from
a stock audio-connect routine. That is AVM's chip API,
not a path we call; **our equivalent is constructing the NWK IWU-INFO /
IWU-TO-IWU ourselves and both sending and answering it during setup.**

**Why this is needed even if `-a basicsvc=0x80` makes the handset ring.**
Narrowband avoids the negotiation, so it may well ring -- but every audio
result in this project is on the **wideband G.722 bearer**, which is where the
downlink defect lives. Falling back to basic speech would change the media path
underneath the audio investigation and confound it. So `0x80` is the
**diagnostic**, not the fix: wideband needs this negotiation regardless of how
the test comes out.

#### `<<CODEC-LIST>>` decoded from the standard (2026-10-22)

`[CR EN 300 175-5 §7.7.54]`, verified locally against the downloaded spec, with
`docs/specs/ts_10252701v010501p.pdf` (TS 102 527-1 V1.5.1) alongside.

```
7C | L | 1 nnn rrrr | <codec entry> ...
                 octet 3: bit 8 = 1, bits 7-5 = negotiation indicator, 4-1 reserved
                          000 = negotiation not possible, 001 = codec negotiation
each codec entry, three octets:
   octet 4   0 + codec identifier (7 bits)
   octet 4a  0 + reserved + direction (2) + MAC and DLC service (4)
   octet 4b  ext + C-plane routing (3) + slot size (4)
```

Codec identifiers (§7.7.54): `0000001` user specific 32k, **`0000010` G.726
ADPCM 32 kbit/s**, **`0000011` G.722 64 kbit/s**, `0000100` G.711 A-law,
`0000101` G.711 u-law, `0000110` G.729.1, then the MPEG-4 and LC3plus range.
C-plane routing: `000` CS only, `001` CS preferred/CF accepted, `010` CF
preferred/CS accepted, `100` CF only. Slot size: `0000` half slot, `0001` long
slot j=640, `0010` long slot j=672, `0100` full slot, `0101` double slot.

**The two stock entries therefore decode as:**

| observed | codec id | codec | C-plane routing | slot size |
|---|---|---|---|---|
| `7c 04 90 02 00 84` | 2 | **G.726**, 32 kbit/s | CS only | `0100` **full slot** |
| `7c 04 90 03 00 81` | 3 | **G.722**, 64 kbit/s | CS only | `0001` **long slot j=640** |

and `0x90` is octet 3 with bits 7-5 = `001`, "Codec negotiation" -- not an
opaque constant. The pairing is self-consistent: G.722 at 64 kbit/s takes the
larger slot, G.726 at 32 kbit/s fits a full slot.

**Correction to `[CR A-StockTrace-5 §12]`.** That answer stated that AVM's
codec enum (2 = G.726, 3 = G.722) is "AVM's INTERNAL enum" and
that "TS 102 527-1, whose on-air ids put G.722=2", so `02 00 84` would be the
wideband G.722 entry. **That is backwards.** EN 300 175-5 §7.7.54 is the IE
coding and it gives **G.726 = 2, G.722 = 3** -- so AVM's enum *is* the standard
enum, and `02 00 84` is the **G.726** entry. Had we implemented the inferred
reading we would have advertised G.726 while calling it wideband, and the slot
sizes would have contradicted it. `[verified]` from the spec text, not inferred.

**Do not conflate this with the chip's `0x0230` codec byte** (`-a codec=0/1/3`),
which is AVM's chip-API field and has its own meaning; the two numbering spaces
are unrelated.

#### ROOT CAUSE, and the fix: we never answer the handset's codec list

`[CR ETSI TS 102 527-1 V1.5.1 §7.3.1]`, now local in `docs/specs/`, is a
**shall**:

> Equipment supporting New Generation DECT wideband speech **shall** add the IE
> `<<CODEC-LIST>>` indicating the supported codecs in the following messages:
> {ACCESS-RIGHTS-REQUEST}, **{ACCESS-RIGHTS-ACCEPT}**, {LOCATE-REQUEST},
> **{LOCATE-ACCEPT}**. The IE shall contain at least G.722 and G.726. The
> transmitting side shall always indicate "Codec Negotiation possible".

`[observed]` **The A540 CAT has been doing its half since the day it
registered.** Its ACCESS-RIGHTS-REQUEST and its LOCATE-REQUEST both carry:

```
7c 07 90 03 00 01 02 00 84
      |  |           +-- G.726 (2), 32 kbit/s, full slot, last entry
      |  +-------------- G.722 (3), 64 kbit/s, long slot j=640
      +----------------- negotiation indicator, "codec negotiation"
```

-- which is also independent confirmation of the identifier decoding above,
from a handset rather than from stock. **We replied with no codec list in
either message, because we never implemented the element.**

§7.3.3 then supplies the symptom exactly: "Sending the IE `<<CODEC-LIST>>` in
CC-SETUP is **not necessary in case the most recent list sent during
registration/location registration is still the valid one**." Stock omits it
from the SETUP because the exchange already happened. **We never had that
exchange at all**, so our wideband SETUP (`<<BASIC-SERVICE>> 0x88`) arrives
with no agreed codec behind it, the PT cannot bring up the U-plane, and it
rejects the call -- "insufficient resources", 40 ms after acknowledging a
syntactically perfect message.

`[fix applied 2026-10-22]` `dect_mm.c` now emits `<<CODEC-LIST>>` (`0x7C`) in
**both** ACCESS-RIGHTS-ACCEPT and LOCATE-ACCEPT, offering G.722 then G.726
with the negotiation indicator set -- the same list the handset offers us.
`put_codec_list()` returns the original offset if the element would not fit,
because the callers in that file commit `off` unchecked and a 0 would commit a
zero-length message.

**No re-registration is needed to test it.** The subscription is stored, so the
handset re-attaches with LOCATE-REQUEST at every daemon start and will receive
the list in the LOCATE-ACCEPT. The next ring test therefore tests this
directly. `-a basicsvc=0x80` is now the **fallback** diagnostic, to be run only
if the handset still refuses.

§7.3.3 also confirms two things we had right: a SETUP offering more than G.726
"shall contain the basic service wideband speech default setup attributes"
(our `0x88`), and "`<<IWU-attributes>>`, `<<CALL-attributes>>` and
`<<CONNECTION-attributes>>` shall not be used in the CC-SETUP" -- so omitting
them was correct, and the earlier `§9.3.2.3` reading that we should have sent
`<<CONNECTION-ATTRIBUTES>>` is superseded.

#### Confirmed byte-exact against stock (2026-10-22)

`[CR A-StockTrace-5 §13]` The stock base sends `<<CODEC-LIST>>` in **exactly
the two messages patched here**, with identical content:

```
#12820  B->H  ACCESS-RIGHTS-ACCEPT   7c 07 90 03 00 01 02 00 84
#3612   B->H  LOCATE-ACCEPT          7c 07 90 03 00 01 02 00 84
#12951  B->H  LOCATE-ACCEPT          7c 07 90 03 00 01 02 00 84
#30274  B->H  LOCATE-ACCEPT          7c 07 90 03 00 01 02 00 84
```

Our `put_codec_list()` emits `7c` + length `07` + `90 03 00 01 02 00 84` --
**byte-identical**. That is the fourth independent confirmation of the
identifier decoding (the spec table, the A540's own frames, the stock call
frames, and now stock's FP registration frames).

**Loose end closed: the single-entry lists are the *chosen codec*.** The
`7c 04 90 02 00 84` and `7c 04 90 03 00 81` elements recorded earlier in this
file have length 4, i.e. **one** codec entry, and `[CR A-StockTrace-5 §13]`
places them in CC-SETUP and CC-CALL-PROC. TS 102 527-1 §7.3.3 identifies them:
"The response message which chooses the codec uses the same IE
`<<CODEC-LIST>>`, but **only one codec shall be in the list**." So they are the
optional per-call restatement / selection, not the primary exchange -- which is
why `7c 04 ... 03 00 81` picks G.722 on the long slot and `7c 04 ... 02 00 84`
picks G.726 on the full slot. Our per-entry decoding of both stands; only their
role was misread.

**We will need that mechanism** when the handset places a wideband call to us:
on receiving a CC-SETUP with a codec list we must choose one and restate it in
a single-entry list in our response, falling back to G.726 where there is no
overlap. Not on the critical path for ringing, and not yet implemented.

**The chain, end to end:** paging gave up before the handset re-synced ->
SETUP was sent before the page response -> (page byte-identical to stock,
`bindearly` a no-op, IWU-INFO only a slot-modification fallback) -> **no codec
was ever agreed at registration** -> the PT cannot bring up the U-plane ->
"insufficient resources". The first two were real defects and are fixed; the
third is the root cause. **Untested on hardware as of this writing.**

### Six hypotheses tested on hardware, six negatives (2026-10-22, runs F4.1-F4.1d)

`[observed]` The A540 CAT refuses every FP-originated call with `CC-RELEASE-COM
/ RELEASE-REASON 0x32` "insufficient resources", ~240 ms after acknowledging a
syntactically correct CC-SETUP. **None of the following is the cause.** Each was
tested on hardware; do not re-test them.

| tested | result |
|---|---|
| SETUP sent before the LCE page response | real defect, **fixed**; still refused |
| paging gave up after 12 s vs a ~30 s re-sync | real defect, **fixed**; answers on attempt 1 |
| chip audio assign before the call (`-a bindearly`) | no effect |
| `<<CODEC-LIST>>` missing at registration/locate | **fixed**, delivered byte-exact; still refused |
| wideband `0x88` vs narrowband `0x80` | **both refused**, with the page byte made consistent |
| `<<SIGNAL>> 0x40` absent | sent; still refused |
| transaction identifier 1 vs 0 | **both refused** |

Our SETUP now matches stock structurally:

```
ours    03 05 | 05 07 .. | 06 07 .. | e0 88 | e4 40
stock   03 05 | 05 07 .. | 06 07 .. | e0 88 | e4 41 | 6c 0d .. | 7e 0c ..
```

Only the `<<SIGNAL>>` **pattern** (0 vs 1) and the display elements differ, and
neither gates resources. **So the cause is almost certainly not in the SETUP.**
`0x32` is a *resource* reason and it arrives after the handset has
acknowledged the message, which points at the state of the **link** when the
SETUP lands rather than at its content. That is the layer our host-side logs do
not show; `[CR A-StockTrace-5]` has been asked for the MAC-level frame sequence
between the page and CC-ALERTING.

**Two methodological notes, recorded because they cost real bench time.**

The transaction-identifier lead came from **diffing stock's bytes against ours**
rather than from a spec clause, and it was the best-looking lead of the day:
every call this project has ever completed used TI 0 (`v28`, `v30`, `v34`, A2,
A3, B2) and every refused one used TI 1. It was **confounded** -- the TI-0 runs
are all the FRITZ!Fon C4 -- and `-a ti=0` on the A540 killed it. Byte-diffing
was still the right method; it was reached six hypotheses too late.

`v28` is worth reading for a different reason: it is a **complete working
FP-originated call**, with `83 01` CC-ALERTING, `83 07` CC-CONNECT carrying the
chosen codec `7c 04 90 02 00 84` (G.726, single-entry list exactly as
TS 102 527-1 §7.3.3 prescribes), our `03 0f` CONNECT-ACK and audio connected.
**The FP-originated path has worked, on the C4.** Whatever the A540 wants is
something the C4 did not require.

#### The RAM disk, and why several earlier results were not what they looked like

`[observed]` `/tmp` on OpenWrt is a **RAM disk**, and `-w` writes
`slots x 2 x 8000` = **128 KB/s** into it, so a 240 s run costs ~30 MB. Seven
accumulated captures filled it completely. A full `/tmp` does **not** fail
usefully: the capture's own writes return `ENOSPC` and are handled, but the
**log's writes do not**, so the daemon goes quiet mid-line while the radio keeps
running. On the bench that reads exactly like *a handset that will not
synchronise* -- which is what it was called for part of the afternoon.

Guards added so it cannot recur: `dect-stubd` now computes what a run will need
(`slots x 2 x 8000 x seconds` plus 16 MB headroom) and **refuses to open a
capture** that will not fit, naming both figures; and it re-checks every
megabyte, closing the capture cleanly at an 8 MB floor rather than filling the
disk. `work/7520/lab/f4.sh` additionally takes no capture at all for the ring
tests, clears stale `.raw` files before a capturing run, and refuses below
40 MB free.

Also recorded, because both wasted a run: **`pgrep -x` is unreliable on this
box** (it reports a live daemon as absent; verified against `/proc` and `ps`),
so the scripts use `ps` now; and **editing a shell script while it is running**
corrupts the running copy, because `sh` reads it incrementally.

#### MEASURED: no traffic bearer ever comes up on our FP-originated calls

`[CR A-StockTrace-5 §15]` resolved the stock side by MAC frame size (36 =
beacon/dummy, >=50 = connection bearer), and stated its limit honestly: the
tracer's MAC header is constant, so **per-direction attribution is impossible**
-- it cannot say which side sends a given MAC frame. Size and order it can do.
Stop asking for per-direction MAC from that capture.

What it shows for a stock incoming call:

```
#3150-3180   36/41 only        beacon and paging
#3181        54                <<< connection bearer appears, abrupt 36->54
#3199        LCE page response (H->B)
#3201        CC-SETUP (B->H)
#3202-3246   52-67             bearer never drops
#3247        CC-ALERTING (H->B)
```

**The bearer is up ~360 ms before the SETUP** and runs unbroken to alerting.
Stock does not build it at SETUP time; the SETUP lands on a connection that
already exists. Also `[observed]`, from the DLC frames which *do* carry
direction: between the page response and the SETUP the FP sends **nothing**
except the short LCE confirm.

**Our side, measured from logs we already held** -- no new bench run needed.
`dect-stubd` logs a chip status line naming the MAC slot and slot type
whenever a bearer exists. Across runs F4.1c and F4.1d:

| event | status line? |
|---|---|
| our FP-originated CC-SETUP (`13 05`/`03 05`) | **never** |
| the handset's own CC-SETUP (`03 05 ... e0 28`) | **always**, ~40 ms later |

Every `call: status: mac slot N carrier M codec/slottype 0x02 (long slot,
wideband)` in those logs follows a **handset-originated** call. Not one follows
ours. In F4.1d our SETUP goes out at t=65099 and the first bearer status in the
whole run is at t=87875, immediately after the handset originated its own call
at t=87839.

**So: on an FP-originated call we never get a traffic bearer at all.** Stock has
one ~360 ms before the SETUP; we have none, ever. `RELEASE-REASON 0x32`
"insufficient resources" is then an accurate description rather than a puzzle,
and the fault is **upstream of the CC-SETUP entirely** -- which is consistent
with six SETUP-content hypotheses all failing.

`[not established]` why the bearer does not come up. The page is byte-identical
to stock's `[CR A-StockTrace-5 §9/§10]`, and per EN 300 175-5 §14.2.3 a paged PT
builds only "the minimum MAC layer service needed for a single C-plane link"
unless the page carries extended MAC service details -- which stock's does not
either. So stock gets a bearer the spec does not obviously require it to get,
and that is the next thing to understand. **This is the question for the next
session, and it is now a specific one rather than a search.**

#### The page and the TPUI are identical to stock, measured on both sides

`[CR A-StockTrace-5 §21]` plus our own capture. This closes the last thing the
air traces could compare at the page/NWK layer.

**The page.** Stock's `0x0300` is `{page-info, 0x11, slot}`, the same three
bytes we send. The `0x11` is a **hardcoded constant** in stock's `the stock paging routine`
(`mov r3,#17`), not a TPUI byte as our own comment assumed; `page-info` is
`0x2C` wideband / `0x0C` narrowband, which matches the derivation we adopted.

**The TPUI assigned in LOCATE-ACCEPT**, full `<<PORTABLE-IDENTITY>>`:

```
stock  05 05 a0 94 00 11 01        (#3612; also 00 11 02 at #12951, #30274)
ours   05 05 a0 94 00 11 01        (reg5.log)
```

**Byte-identical**, and identical in every field that carries meaning: type
octet `0xa0` = TPUI (against `0x80` = IPUI in ACCESS-RIGHTS-ACCEPT, so stock
does assign a distinct TPUI at locate, as we do), length `0x94` = ext + 20
bits, leading value bit `0` = individual-assigned class.

So the `§9` worry that the page might be "identical in form only" is closed: it
is identical in form, in the page-info derivation, **and in the TPUI it
addresses**. The TPUI-class hypothesis is dead before it cost a bench run.

**This exhausts the page.** Every layer the captures can reach now matches, and
the difference is below them, in whatever makes the handset open a traffic
bearer on being paged. `docs/test-card-F5.md` is aimed at that: it reads what
bearer state looks like in chip memory during the handset's own calls, then
asks whether any of it moves during our failed page.

---

## The downlink defect as a clock question, and how to read the next run

`[CR A-DSP-1]`, 2026-10-22. Stock's downlink is a **clock-domain crossing**:
audio moves in 4 ms blocks, and a frame the host cannot supply in time is sent
as an idle pattern. The failure is named in stock's own strings
(`[stock log message]`).

### Why our fix is simpler than stock's

Stock crosses between an independent VoIP clock and the DECT clock, so it
carries a dejitter buffer and a correction law keyed on a measured host↔DECT
millisecond delta. **We have no such crossing.** On the 7520 the DECT chip is
the TDM clock master and the SoC its slave — `docs/pcm-tdm-ipq4019.md` §2,
measured: the clocks only run while the STUB runs, and the bit clock reads
1,019,367–1,027,749 Hz *at* the SoC, the wobble of a clock being observed
rather than generated. AVM's own board tree marks GPIO 20–26 (both clock
groups, both frame syncs) as inputs and only GPIO 27, the data out, as an
output.

The room corroborated this from the other side: the SoC-side TDM endpoint has
no clock or PLL generation at all, consumes the frame rate as an input to size
DMA descriptors, and its clock-change notification handler is a no-op — the
signature of a follower.

**So there is one clock domain and nothing to correct.** No dejitter buffer, no
correction law. The fix is to drive transmit from the hardware DMA refill
instead of a userspace timer. "Transition-paced playback is clean" has been
saying exactly this all along: it is the DECT clock, borrowed by hand.

### The number, and why it was never measured

`pcm_tx_fill()` in `kernel/opendectcell_pcm.c` drains the transmit fifo frame by
frame and memcpy's the idle template into any frame the fifo cannot supply. So

```
idle-filled frames = tx_blocks × frames_per_block − tx_frames
```

is the count of frames the handset heard as idle pattern instead of audio —
our equivalent of stock's trigger-violation count. Both counters have existed
all along, but were printed **once, at exit**, summed over the whole run:
every capture in `work/box-tmp/` has exactly one such line, so starvation
during playback could never be separated from priming at start-up. Commit
`c8e0fd2` prints it once a second, next to the slot-activity and play-cadence
lines.

One independent agreement worth noting: our DMA block is 32 frames at an 8 kHz
frame sync = **4 ms**, the same block size the room found on stock's host side,
and the `default 32 (4 ms)` comment in `kernel/include/uapi/opendectcell.h` predates
that answer.

### How to read the next bench run

Take the **steady-state per-second rate during playback**, not the cumulative
line. The cumulative figures in existing logs suggest 0.3–0.6 % idle fill —
25–45 discontinuities per second, which would sound like continuous noise
rather than clicks — but they are confounded by start-up priming and should be
treated as unmeasured.

| idle-fill rate during playback | audio | reading |
|---|---|---|
| collapses toward ~0 once TX is refill-driven | clean | clock reading **confirmed** |
| stays non-zero once TX is refill-driven | noise | not drift but **fifo supply** — a software timer still in the loop. Different fix, same counter finds it |
| already near zero | still noise | the clock reading is **wrong**. Worth knowing fast, and this run is what tells us |

The third row is the one to watch for. It is the cheapest way this hypothesis
can be falsified, and it costs nothing extra to look.

---

## Bench session, 2026-10-22 evening: the third row fired

Six runs on the spare 7520 with the maintainer present. Logs in
`work/box-tmp/`: `vIWU1`, `vIWU2` (A540), `vC4T4c`, `vC4T4d`, `vC4sync2` (C4).
Radio parked after every run.

### The result in one table

Same binary, same evening, same `-a flags=0x41,mask=0x0002,codec=3,status=8000`
and the same `/tmp/speech.g722`.

| run | handset | call direction | audio | starvation | uplink holds/s | extra frames/s | slot 0 changes/s |
|---|---|---|---|---|---|---|---|
| `vIWU1` | A540 | handset-originated | **clean** | 0.00 % | — | — | — |
| `vIWU2` | A540 | handset-originated | **clean** | 0.00 % | — | — | — |
| `vC4T4c` | C4 | handset-originated | **clean** | 0.00 % | **7** | **14** | 3992–3996 |
| `vC4T4d` | C4 | **base-originated** | **wind** | 0.00 % | **152–153** | **532–636** | 3432–3535 |
| `vC4sync2` | C4 | **base-originated**, `playsync` | **wind** | 0.00 % | 47–103 | 122–320 | 3743–3786 |

**The defect is call DIRECTION, not the handset.** Nobody had separated those
before, because until tonight a base-originated call either never rang on the
handset we had, or was never played into.

### The clock-crossing reading is wrong

`[CR A-DSP-1]` read the noise as a host↔DECT clock crossing, and the outcome
table above this section named the falsifying case: *near-zero idle fill while
the audio is still bad*. **That case fired.** `vC4T4d` ran twelve consecutive
seconds at `0 of 8000 frames idle-filled` with a perfect `8000 frames, 4000
advances` cadence, and the handset played wind. The feeder supplies flawlessly
and the audio is still broken, so the fault is **downstream of the fifo**. A
reclocker would have been the wrong thing to build.

### My own instrument was half-blind, and I over-read it

`idle-filled = tx_blocks × frames_per_block − tx_frames` counts frames the
queue could not supply. **It cannot see over-supply at all**: writing too fast
never leaves a frame unfilled, so it reads a clean `0.00 %` either way. Four
"zero starvation" runs were reported here as evidence against a supply fault.
They ruled out exactly half of one. The number was right; the reading of what
it excluded was not.

### What the lane says instead

The chip **holds** uplink words far longer on a base-originated bearer:
**7 holds/s on a handset-originated call against 152/s on a base-originated
one**, a twenty-fold difference, costing 14 extra frames per second against
532–636. We advance playback at a fixed 4000 words/s in both cases, so on a
base-originated call we write into a lane that is not consuming at that rate.

`playsync` (pace to the chip's own transitions) **halved the holds but did not
fix the audio**. Its cadence line is revealing: `4000 advances, 3040–3164 rx
transitions, (phase locked)`. It locks **phase** and keeps advancing at the
nominal **rate** — so a rate mismatch survives it.

**Working reading, not yet proven:** on a base-originated bearer the chip's
audio lane runs slower than our nominal 4000 words/s, we over-supply, and the
lane corrupts. The fix direction is to derive the advance rate from the lane
rather than from the file's nominal bit rate.

**Confounds that are not yet excluded**, and no new dial should be turned
before they are:

- Slot-0 change counts fall with silence, and the maintainer spoke during the
  clean call. Part of 3995 vs 3500 may be content, not rate.
- The MAC slot differed every time (0/carrier 4 clean; 4/carrier 7 and
  2/carrier 7 noisy) while our TDM route is pinned at `mask=0x0002`. A wrong
  lane mapping would also produce this, and is not ruled out by anything here.
- One clean base-originated call has never been observed, so "direction" and
  "whatever else differs about these bearers" are not yet separable.

### The counter needs an over-supply side

`report_tx_starvation()` should also report the lane's own consumption, so the
two rates can be compared directly instead of inferred from hold counts. Until
then, read `uplink holds` as the real instrument and the starvation line only
as a floor.

### Correction, same evening: the over-supply reading does not survive the code

The section above offered "the chip's lane runs slower than our nominal
4000 words/s on a base-originated bearer" as a working reading, resting on
slot-0 change counts (3995 clean against 3430-3790 noisy) and uplink holds
(7/s against 152/s). It named the silence confound first. **The confound is
almost certainly the whole effect**, and the answer was already written in our
own source from 2026-10-20, in the `playsync` comment in `dect-stubd.c`:

> Advancing on every transition (v32, A3, A4) starves: the uplink carries the
> handset's microphone, so when the user stops talking the lane stops changing
> and the downlink falls from 8000 octets/s to zero.

The uplink lane carries the **handset's microphone**. Its word-change rate is
an audio-content measure, not a clock measure — and `uplink holds` counts
repeated consecutive words, so a quiet microphone inflates it for exactly the
same reason. In `vC4T4c` the maintainer was speaking (a dialled call); in
`vC4T4d` and `vC4sync2` he was listening to the handset's earpiece. That alone
predicts the direction and rough size of both gaps.

So the proposed fix — advance from observed transitions — is not merely
unproven, it is **already measured and already failed**, for a documented
reason. It must not be retried.

**What still stands:**

- The audio outcome split: clean on three handset-originated calls across two
  handsets, wind on two base-originated calls. That is an observation, not an
  inference, and no confound touches it.
- Starvation is zero in every run, so the fault is not under-supply and the
  clock-crossing reading stays falsified.

**What falls:** the rate-mismatch diagnosis, and with it the idea that the
instrument should report "lane consumption" derived from the receive lane.
Any such counter would measure the microphone.

**What is left as the leading candidate**, and it was already listed as an
unexcluded confound: the **MAC slot/carrier differed in every run** —
`slot 0 / carrier 4` on the clean call, `slot 4 / carrier 7` and
`slot 2 / carrier 7` on the two noisy ones — while our TDM route is pinned at
`mask=0x0002`. A route that is correct for one slot assignment and wrong for
another produces exactly this: clean when the chip happens to land where we
point, noise when it does not. **Direction may be a proxy for slot assignment,
not a cause.**

That is testable without guessing: ring repeatedly, record the MAC slot each
time, and see whether a base-originated call that lands on slot 0 is clean and
a handset-originated call that lands on slot 4 is noisy. If audio tracks the
slot rather than the direction, the fix is the route, not the rate.

**Lesson, the second time in two days:** read what an instrument physically
measures before reasoning from its numbers. The playback code carried the
answer in a comment for two days.

### We have never routed the audio at all `[2026-10-22, from CR A-DSP-1 §3]`

`[CR A-DSP-1 §3]` separates two things our stack has always treated as one:

> `0x0251` codec selector and `0x0230` audio assign **SELECT** the
> codec/channel; the `F1 FF 0A` connect **ROUTES** it in the matrix.
> "routing audio for a call is: pick the DECT codec channel and the PCM channel
> and cross-connect them with this command."

The cross-connect is `send_stub_connect_cmd(chn_dst, chn_src)`, emitted as
`header 0x22 | F1 FF 0A | <chn_dst> | <chn_src>` (mail type `0x23`,
payload length 5) — the STUB/DSP opcode space, distinct from the CC-mail space
the whole audio bind lives in.

**We have never sent it.** Not in `src/`, not in any capture in
`work/box-tmp/`. We select a channel and never connect it to anything.

That is consistent with the evening's actual finding — that audio is
**non-deterministic across calls with identical assign bytes**, arriving as a
clear recording, as wind, or as nothing at all. With no cross-connect of our
own, what the matrix happens to hold from a previous state decides the
outcome, which is exactly the shape of a defect that has resisted every
host-side variable for three days.

It also explains why every one of our instruments pointed the wrong way: they
all measure the **uplink**, and the uplink is whatever the matrix was already
routing. We have no measurement of the downlink at all.

**Not yet established, and the reason this is a question and not a patch:**
which channel numbers a wideband call should use (`[CR A-DSP-1 §2]` gives
ADPCM 0-3, PCM 4-6, WIDEBAND 7-9, Ruhe 12, Testton 15, but not how a live call
picks its pair), whether the connect is sent once or once per direction, and
where in the sequence it belongs. Guessing channel numbers into a routing
matrix is not a cheap experiment. Asked as `Q-0036`.

`[CR A-DSP-1 §2]` also offers a free bring-up test we have never run:
cross-connect a DECT codec channel to **channel 15 (Testton)** and the handset
should hear a tone **with no host audio in the path at all**. That isolates
chip→air from host→chip in one run, which nothing we have done so far does.

### The GenDSP route probes: a clean negative `[2026-10-22, vTone/vTone2/vTone3/vSweep]`

`[CR A-DSP-2]` retracted the idea that stock routes a live call with
`F1 FF 0A`: that opcode is reached only from a `/dev/debug` command, the whole
`F1 FF xx` space is debug, and the live bind is `0x0251` + `0x0230` with the
route riding in them as the link number, the chip allocating the matrix
channel. **So "we never route the audio" was wrong**, and it was wrong twice:
we have had `-a dsp=DST:SRC` and `-a dspsweep=DST[:MS]` since gate G4, both
emitting exactly `0xFFF1` sub `0x0A {dst, src}` to task `0x23`.

A-DSP-2 still proposed the Testton probe as a cheap way to separate chip→air
from host→chip. It was run four ways and **produced nothing**:

| run | route attempted | result |
|---|---|---|
| `vTone` | `0xF1FF` sub-`0A` into 7, 8, 9 | silence — **wrong framing**, see below |
| `vTone2` | `0xFFF1` sub-`0A` into 7, 8, 9 | silence |
| `vTone3` | `0xFFF1` sub-`0A` into 1, 0, 2, 3 | silence, two calls |
| `vSweep` | `dspsweep=1:1500`, **all 22 sources** into channel 1 | silence, and **no TDM lane moved at all** |

Three mistakes worth recording, since each cost a call:

1. **Byte order.** `A-DSP-2` gave the on-wire bytes `F1 FF 0A`, which is
   `0xFFF1` little-endian plus sub-command `0x0A` — the same primitive we have
   always used for `MEM_READ` to task `0x23`. The first probe sent primitive
   `0xF1FF`, which is not a primitive at all.
2. **Channel number.** 7-9 is the *wideband class*, but `0x0242` reports the
   channel the chip actually assigned, and for every call here it is
   **channel 1** (`01 41 02 00`, low nibble of `0x41`), with our own log line
   `audio connect: 0x022D on channel 1`. The first two probes routed into
   channels the call never used. **Read the assigned channel from `0x0242`;
   do not assume the class.**
3. **Timing.** `-N` (added this session, fires at call-bind) lands in the
   middle of our own `0x0251`/`0x0230`/`0x022D` sequence, which plausibly
   overwrites it. `-a dsp=` exists precisely because it fires *inside*
   `send_audio_connect()`, after `0x022D`. The purpose-built option was always
   the right one.

**What the clean negative says:** with a live wideband call on chip-assigned
channel 1, routing each of the 22 GenDSP sources into that channel in turn
moved **no TDM lane** and produced **no audible output**. This is consistent
with the 2026-10-19 measurement in `dsp_sweep()`'s own comment — into
destination 6, with no call, every source pinned the lane to a constant and
none produced a varying signal. **On this evidence there may be no usable tone
source in this STUB build**, and the probe cannot separate chip→air from
host→chip. It should not be retried in this form.

Note also that in `vSweep` TDM slot 0 showed **zero** changes for the whole
call, where the one clean call (`vC4T4c`) showed 3992/s. That call appears to
have carried no audio in either direction.

### On `[CR A-DSP-3]`: it describes the vendor's PCM driver, which we do not run

A-DSP-3 traces a host-side mechanism in AVM's the vendor's PCM driver — a codec slot with
a "transparent" flag at byte[3] that bypasses the encoder, set true iff the
source resolves to G.722, with an unbound slot (`0xFF`) skipping the route
entirely — and shows it can produce wind, clear and silence from one racy
input. It is a careful piece of work and the three-outcome match is striking.

**It cannot be our mechanism.** We do not run the vendor's PCM driver. Our kernel module
(`kernel/opendectcell_pcm.c`) does TDM DMA and nothing else: no PCM router, no
codec slots, no transparent flag, no encoder to bypass. There is nothing in
our stack that could be "wrongly transparent".

**The question underneath it may still be ours**, and it is one we have never
asked: *what does the lane expect?* We play `/tmp/speech.g722` — already
G.722-encoded — straight into the lane. If the chip expects linear PCM there
and encodes it itself, we have been handing it encoded data to encode again,
which would sound like noise. The `dsp_sweep()` comment's 2026-10-19
measurement is evidence on this point: sources 19 and 20 drove the lane to
`0x7FFF` and `0x8000`, "the extremes of 16-bit linear", which reads as a
**linear** lane, not a G.722 one.

That is a concrete, cheap, untried experiment: play 16-bit linear PCM instead
of G.722 and listen. It needs no new code (`play_raw` exists) and no clean-room
input.

### THE LANE CARRIES SIGNED 16-BIT LINEAR PCM `[2026-10-22 20:40, vLin]`

> **Partly retracted 2026-10-23.** The tone test rules out G.722 on the
> lane; it does **not** rule out A-law or µ-law, which reproduce a sine's
> pitch exactly. See "Desk session, 2026-10-23 morning" at the end of this
> file.

A signed 16-bit little-endian 400 Hz sine, generated at 8000 samples/s and
written **verbatim** onto the lane with `-a playraw=`, produced a **clear,
steady tone at the handset** — reported as "permanent tone... close to
400 Hz". No G.722 anywhere in the path.

**A coded lane fed a linear sine cannot produce a tone.** It produces hash.
So the lane carries **linear PCM and the chip encodes to G.722 itself**, and
feeding it a pre-encoded `.g722` stream — which is what `-a play=` has always
done — is a **double encode**. That is what the "wind" has been.

This is the first thing in three days that is both audible and in the right
direction, and it was predicted in advance by `[CR A-DSP-4]` from stock's own
host-side lane tone generator (signed 16-bit linear, `0x7fff` saturation),
plus our own 2026-10-19 sweep, which pinned the lane to `0x7FFF`/`0x8000` —
the extremes of signed 16-bit linear, values G.722 cannot produce. **We had
measured the lane format on 2026-10-19 and read it as "no varying source".**

The reported pitch also says the lane consumes **one word per 8 kHz frame**:
the sine was generated at 8000 samples/s and came back at its generated pitch.
At 16 kHz it would have sounded an octave high.

### CONFIRMED by a discriminating test `[2026-10-22 20:50, vAB]`

> **Partly retracted 2026-10-23.** The tone test rules out G.722 on the
> lane; it does **not** rule out A-law or µ-law, which reproduce a sine's
> pitch exactly. See "Desk session, 2026-10-23 morning" at the end of this
> file.

The maintainer asked the right question — "did I hear a tone or a glitch?" —
and proposed the right test: two different frequencies. Better still in one
call: a file alternating **5 s of 400 Hz with 5 s of 1000 Hz**, looping.

**Result: "It alternates!"** Two distinct pitches, over an octave apart,
following our samples on a 10-second cycle. A glitch cannot do that, and
neither can a latched or free-running source. **The lane is signed 16-bit
linear PCM, reproducibly, and the chip encodes to G.722 for the air.**

This also demonstrates the lane follows our *content*, not merely our
presence: the pitch changes when and only when the file says so.

### What this does NOT yet establish, and the blocker

- **It was observed once.** Every follow-up was blocked, see below.
- **It contradicts our own v19 measurement**, which decoded the lane's words
  as G.722 by ear and is quoted in `dect-stubd.c` to this day. Both cannot be
  true of the same lane in the same direction. Either the directions differ,
  or v19 was wrong. Unresolved — do not delete the v19 note yet.
- **Speech was not confirmed.** `vLin2` played the same recording decoded to
  linear and produced silence; `vLin3` then replayed the *sine that had just
  worked* and also produced silence. So both were **dead calls**, and `vLin2`
  tells us nothing about speech.

**The blocker is now the intermittency, not the format.** The same
configuration produced a tone at 20:40 and silence at 20:45. MAC slot does not
separate them — slot 8 appears in both the tone run and a silent one. Until a
call can be made to carry audio *reliably*, no format or rate change can be
confirmed, because a negative result is indistinguishable from a dead call.

**That makes the intermittency the top audio task**, ahead of finishing the
format fix.

### A bug in the test tooling, for whoever repeats this

`scratchpad/g722dec.c` decoded the speech to 16 kHz and then took **every
second sample with no low-pass filter**. That aliases everything above 4 kHz
back into the band and would wreck speech even on a working path. A 400 Hz
sine is immune, which is why the sine test was still valid. **Decimate
properly, or decode to 8 kHz, before judging speech.**

### Speech still inaudible — and the most likely reason is level, not format `[vSpeech]`

With the anti-aliasing fixed (average each pair before the 2:1 decimation,
not discard every second sample), the decoded recording was played onto the
lane twice, minutes after the alternating tone had worked on the same setup.
**Nothing audible, both calls.**

Before reading that as "speech does not work", note the levels:

| file | peak | RMS | RMS vs full scale |
|---|---|---|---|
| `ab.raw` (alternating tone, **audible**) | 12000 | 8485 | **-11.7 dB** |
| `speech2.lin` (recording, **inaudible**) | 32767 | 2202 | **-23.5 dB** |

The speech is **~12 dB quieter in RMS** than the tone, and every call this
evening was made at *minimum* earpiece volume, deliberately, because the
historic failure mode was loud noise. A 12 dB deficit on top of a minimum
volume setting is a mundane and entirely sufficient explanation for "the tone
was obvious and the speech was not".

**So this is not yet evidence against the linear-lane finding.** The next
attempt should:

1. **Normalise the speech** to roughly the tone's RMS (around -12 dBFS) before
   playing it, and
2. **raise the handset volume** — the double-encode that made this dangerous
   is exactly what we are no longer sending.

Only if a normalised recording at normal volume is still inaudible does the
format conclusion come back into question. The intermittency remains a
confound for any negative result here (`vLin3` showed the sine itself going
silent between runs), so a negative must be repeated against a tone control in
the same run before it means anything.

### `[CR A-DSP-5]`: the rate reconciles, and the intermittency has a name

**1. Rate — wideband wants TWO slots, and ours opens one.** The room owned an
under-specification in A-DSP-4 and the reconciliation is clean:

- The TDM **frame** rate is fixed at 8 kHz (our DMA block is 32 frames x 125 us
  = 4 ms — both sides measured this independently).
- One 16-bit word per **slot** per frame = **8000 samples/s per slot**.
- So **narrowband = 1 slot at 8000 Sa/s**, which is exactly our
  correct-pitch tone result; and **wideband = 16000 Sa/s = TWO slots**, two
  words per 8 kHz frame. Not a 16 kHz single slot.

A-DSP-4's "16 kHz" was the wideband *audio* rate, not a slot rate. Our
`0x0230` mask is `0x0002` — **one slot**. Stock's slot-opening call
counts the slots in the mask and supports several.

**So for a wideband call we may be supplying half the samples the chip
expects.** Two things to test, in this order:

1. **Does our call believe it is wideband?** `0xF625` says
   `codec/slottype 0x02 (long slot, wideband)` and we configure `codec=3`,
   yet one 8 kHz slot gave a **pitch-correct** tone. If the chip genuinely
   wanted 16000 Sa/s and got 8000, the tone should have come back an **octave
   low**. It did not. So either the call is narrowband in a way the status
   byte does not say, or the chip resamples a single slot. **Generate a tone
   at 16000 Sa/s, play it on one slot, and listen for an octave shift** — the
   two-pitch rig already built answers this in one call.
2. **Only if step 1 says the chip wants two slots:** open two
   (`mask=0x0003` and the neighbouring word) and interleave two 8 kHz streams.

Do **not** read the assigned channel number as narrow/wide — it is the
host-facing link channel, not the GenDSP matrix channel. **The slot count and
the pitch are the reliable tells.**

**2. The intermittency has a named mechanism: TDM slot-sync loss.** Stock
carries it explicitly: a slot-sync routine, a sync-mode setup
routine, a log line giving the time to slot sync, and two failure messages.

The chip is TDM master and emits the frame/slot sync; our DMA must lock its
slot **phase** to it. Sync held → samples land on the right frame → tone. Sync
never established or lost → the lane and the bearer do not line up → silence,
**invisible in the upper-layer log because the miss is at slot-phase level**.
A probabilistic lock at call start is exactly "same settings, different
result", which is what we measured (`vLin` tone at 20:40, `vLin3` silence at
20:45).

**This is why the in-call tone control is not optional.** A dead call and a
real negative are indistinguishable without one.

**3. Uplink format: probably linear too, but argued rather than proven.** The
room finds no support for a coded uplink: a GenDSP codec channel is symmetric,
the lane is 16-bit both ways, and `0x0230` carries **one** codec type with a
direction selector — not a codec per direction — so the format cannot differ
per direction within one call. the vendor PCM driver's `g722_decode_lin16` is host-side
processing for VoIP legs and the echo canceller, not a DECT-lane RX decode.

On our v19 "G.722 by ear": most likely a misread, and there is a plausible
mechanism — **G.722's lower subband is ADPCM, so arbitrary input decodes to
something voiced and "G.722-ish"**. Feeding signed 16-bit linear into a G.722
decoder does not give silence, it gives plausible-sounding rubbish.

**Decisive test, mirroring the one that worked:** record the uplink lane while
a known pitch is played at the handset's microphone, and interpret the words
as **signed 16-bit linear**. If the pitch comes back, the uplink is linear and
v19 was wrong. If only a G.722 decode recovers it, the asymmetry is real.
One call, with an in-call tone control.

---

## Desk session, 2026-10-23 morning: the tone test was half as strong as recorded, and the uplink is settled

No radio was used. Everything below comes from recordings we already had
(`work/box-tmp/v22.raw`, 53.1 s of RX DMA from the v22 call) and from
arithmetic on `scratchpad/sine400.raw`, the exact file the 2026-10-22 test
played. Prompted by `[CR A-DSP-8]`, which predicted the opposite of what we
had written down in both directions and so forced the check.

### 1. Correction: "CONFIRMED by a discriminating test" claims more than the test showed

The two-tone run (`vAB`, above) is still good evidence, but it does not
establish what that heading says. The question it answers is **what pitch the
lane carries**, and pitch survives most wrong readings, because almost any
misinterpretation of a periodic byte stream is still periodic with the same
period.

Measured on the file we actually sent (`scratchpad/discrim.py`), peak
frequency and the share of energy in the fundamental:

| how the 400 Hz linear sine is read | peak | fundamental |
|---|---|---|
| as signed 16-bit linear (what we meant) | 400.0 Hz | 100 % |
| **low byte taken as A-law** | **400.0 Hz** | **24.7 %** |
| **high byte taken as A-law** | **400.0 Hz** | **37.7 %** |
| as G.722 octets | 3800.0 Hz | 71 % |

`[derived]` So:

- **G.722 on the downlink lane is genuinely ruled out** by the tone test. A
  linear sine read as G.722 comes out at 3800 Hz, not 400 Hz, and the
  alternating file would have produced two *wrong* pitches. It did not.
- **A-law and µ-law are not ruled out.** A linear sine misread as A-law lands
  on exactly 400 Hz, with only a quarter to a third of its energy in the
  fundamental and roughly 45 % spread broadband. That is a **buzzy tone at the
  right pitch** — and the report we have is "permanent tone... close to
  400 Hz", not "a clean sine".

**This matters because it fits the symptom we cannot otherwise explain.** A
law-vs-linear mismatch passes tones through at the correct pitch while
turning speech into noise, which is precisely what we see: every tone we have
sent has been recognisable, and no speech ever has. The heading above should
be read as *"the lane is not G.722 and the pitch tracks our content"*, which
is what was shown.

`[CR A-DSP-8]` §2 supplies the independent reason to take this seriously: in
the 08.25 STUB the per-channel transmit chain — the one that feeds the air —
**begins with an A/µ-law-to-linear step**. A chip that converts law to linear
on the way in is a chip that expects law on the lane.

### 2. The uplink is G.722-coded. `[CR A-DSP-8]`'s U1 prediction is falsified

U1 predicted the uplink words decode as **A-law in one byte, the other byte
constant**. They do not, and the capture settles it without a call.

`[observed, v22.raw, 425 008 live frames = 53.1 s]`

- **Both bytes of the bearer word vary**, each changing about 3930 times per
  second. Byte 1 is not constant, so "A-law in one byte half" is excluded by
  inspection.
- **98.9 % of consecutive bytes differ from their neighbour**, giving ~14 800
  distinct octets/s out of 16 000. There is no duplication and no hold
  structure at the byte level, so the lane is not one octet repeated into both
  halves of the word either.
- The **loudness envelope** is decisive. A 53-second recording of someone
  talking has pauses; a wrong decode does not invent them. Per 100 ms window,
  5th and 95th percentile, and the share of windows more than 20 dB below the
  loud percentile:

| reading of the uplink | range | windows >20 dB down |
|---|---|---|
| signed 16-bit linear LE | 11.5 dB | 4.5 % |
| signed 16-bit linear BE | 11.9 dB | 4.5 % |
| A-law, both bytes | 17.6 dB | 0.0 % |
| µ-law, both bytes | 24.9 dB | 5.6 % |
| **G.722, both bytes in order** | **52.0 dB** | **61.4 %** |
| G.722, bytes swapped | 52.2 dB | 59.4 % |
| G.722, byte 0 only | 42.3 dB | 48.4 % |

  Only the G.722 readings show a speech envelope. The byte-level readings are
  a continuous signal with no silence in 53 seconds, which no one produces by
  talking.

**So the 2026-10-19 finding stands: the uplink lane carries G.722 code
octets.** The v19 note quoted in `dect-stubd.c` was right and should not be
deleted. `[observed]`

Tooling: `scratchpad/ulfmt.py` (which byte positions are live),
`scratchpad/uldec.py`, `scratchpad/pairing.py`, `scratchpad/env2.py`.

### 3. The two directions really are asymmetric, and there may be two lanes

Taking §1 and §2 together, the chip hands us **coded octets** on the uplink
while `[CR A-DSP-8]` says the downlink chain wants **law-coded PCM**. Those
cannot both describe one symmetric lane, and the room's own reading has a
matching tension: it found **no G.722 encoder in either STUB build**, yet the
uplink we record is G.722.

The resolution that fits every measurement we have is that **we are reading a
transparent bearer slot, not the DSP's PCM voice channel**:

- **Slot 0** (bytes 0–1) carries the coded air bearer, passed through without
  decoding. This is the slot we have used for everything.
- **Slot 5** (byte 10, with byte 11 constant `0x00`) is an **A-law PCM voice
  channel** — exactly what `docs/stub-pcm-control-channel.md` §17 and
  `docs/pcm-tdm-ipq4019.md` §13 recorded in 2026-10-17, and what the DSP
  chains in `[CR A-DSP-8]` §2 describe.

`[observed]` In the whole 53 s of the v22 call, **slot 5 took only four
distinct values** — `0x55` (278 306), `0x54` (140 664), `0xd5` (5 749),
`0x57` (817) — at −66.6 dBFS. That is A-law silence with dither: the channel
is present and clocked throughout a live call, but **nothing is routed to
it**. A silent voice channel running beside a busy bearer slot is what an
unrouted PCM path looks like.

If this is right, the downlink question is not "which format on slot 0" but
"which slot", and `0x0251` / the matrix routes in `[CR A-DSP-8]` §9 are how
the choice is made. Unverified — it is a hypothesis built to fit, and it
needs the bench.

### 4. What is now worth a transmit window

The pairing in §1 and §3 costs one call each and both have pre-committed
readings. See `docs/tasks-2026-10-23.md` A0.

**And a standing rule, earned twice now: a single tone proves pitch, nothing
more.** A format claim needs either speech or a signal whose *shape* is
checked, because every candidate format reproduces a sine's fundamental. The
alternating two-tone rig is a fine control for "is this call alive" and should
keep being used for exactly that.

### Desk session part 2, 2026-10-23: the hold is at WORD level, and that is the rate error

**A0 came back negative on all four formats** (`vF46b`, 79.6 s of connected
call, the 38 s cycle heard just over twice): no segment was clean, "everything
was garbled, some sounded more like wind than others". The beep markers were
not perceived either, except the very first one — a design fault in the rig,
since a misread 150 ms sine does not stand out against continuous garble.

That negative is the useful kind, because it sent us back to the capture.

**This morning's §2 measured the right thing and drew the wrong conclusion.**
We reported "98.9 % of consecutive bytes differ from their neighbour, so there
is no duplication and no hold structure". True at byte level — and irrelevant,
because a word held for two frames produces the byte sequence `X Y X Y`, in
which no byte ever equals its neighbour. The test could not see the structure
it was used to rule out.

`[observed, v22.raw, 425 008 live frames]` At **word** level:

| measure | value |
|---|---|
| consecutive words equal | 52.6 % |
| word runs of **exactly length 2** | **99.8 %** |
| distinct words/s | 3795 (≈4000, the deficit being the chip's known 4/10/40-frame holds) |
| **octets/s** | **8000** |
| held pairs starting on an even frame | 0.0 % |

**8000 octets/s is G.722 at 64 kbit/s**, which is exactly what the 80-byte
long slot carries — and the chip reported `codec/slottype 0x01 (long slot,
wideband)` in this very call. Every number closes.

**So the lane convention is: two G.722 code octets per 16-bit word, each word
written into two consecutive frames.** Deduplicated, the uplink decodes with a
41 dB speech envelope.

**The error was rate, not format.** `playraw` writes a new word every frame —
16000 bytes/s into a lane that consumes 8000 octets/s, a factor of two. And
this explains the whole three-day symptom set exactly:

- **A tone survives it.** Take every second sample of a 400 Hz sine at 8000
  Sa/s and it is still 400 Hz at 4000 Sa/s, well under Nyquist. Both tones in
  the alternating test survive for the same reason. That is why every tone we
  sent was recognisable *and why none of them told us anything*.
- **Speech does not survive it.** Half the code octets of an adaptive
  sub-band coder are discarded, which is garble, not wind.
- **`-a play=` had the right format all along.** It feeds pre-encoded G.722,
  which is what a wideband call wants; it fed it at twice the rate. The
  "double encode" reading of 2026-10-22 was wrong, and so is task A4 as
  written.

`[derived]` Octet order within the word is not settled: deduplicated and
decoded, the natural order gives a 41.1 dB envelope and the swapped order
36.8 dB. Too close to call offline, so the probe tests both.

**Rig:** `tools/make-lane-g722.py`, and `work/audio/g722probe.raw` =
19.68 s in the natural order followed by 19.68 s swapped, looping. Verified
offline: deduplicating the generated lane reproduces the source G.722 stream
**exactly**, and does so on **either** sampling parity — duplicating each word
makes the result robust against the pairing phase, so A-DSP-7's byte-equality
test is not needed to get audio.

**Pre-committed readings** (`f4.sh 7`):

- **Intelligible on the first pass, garbled on the second** → natural octet
  order; the lane is solved, and the fix is to hold each word two frames in
  the normal `play=` path.
- **Garbled first, intelligible second** → same, swapped order.
- **Both intelligible** → octet order does not matter; solved either way.
- **Both garbled** → the rate is still wrong or the hold is not the whole
  story. Next step is then the slot question (§3 above), not the format.

### And the slot search was broken the same morning

Separately, `find_control_slot()` accepted a transmit slot on *any* mail
arriving in its 500 ms probe window. The chip emits unsolicited `0xFFF1`
statistics and `0xF618` every 5 s and `0xF623` scan reports, so one of those
could land in the window and lock the search onto a slot the chip never reads.
The daemon then ran with a transmit slot nothing listens to: the radio was
never brought up and the handset could not see the base, while the log said
"control slot found" and the chip's periodic reports kept arriving.

`[observed, 2026-10-23]` Failing run `vF46`: `tx 0, rx 1 (answer prim fff1)`,
session `contacts 3 answers 0`. After requiring `0xF601`/`0xF602`: `tx 1,
rx 1 (answer prim f601)`, `contacts 1 answers 3`, `overruns 0`, and the
handset rang. Same box, minutes apart, nothing else changed.

**This is the first concrete mechanism found behind the intermittency.** A
race against the chip's own five-second reports is exactly the kind of fault
that appears and disappears between otherwise identical runs, and it would
never show in our logs as anything but a successful start-up.

### Desk session part 3, 2026-10-23: the handset's audio is DECOUPLED from ours

`vF47` fed the correctly rated G.722 lane — each word held two frames,
8000 octets/s, both octet orders, 19.68 s per pass — into a connected
wideband call (`codec/slottype 0x02 (long slot, wideband)`, mac slot 4,
carrier 1). The maintainer recorded the earpiece acoustically for 154.6 s.

**This is the first objective measurement of the downlink we have ever had,
and it says something no amount of listening would have.**

`[observed, acoustic recording of the earpiece, 70–150 s of connected call,
50 ms windows]`

| | envelope range (p95−p5) | sd |
|---|---|---|
| what the handset played | **3.8 dB** | 2.93 dB |
| the speech we sent | **52.9 dB** | 15.69 dB |

Best correlation of the two envelopes, searched over every offset:
**−0.307** — no relationship at any alignment.

**A wrong format or a wrong rate garbles speech. It does not decouple the
output from the input.** Half-rate G.722 is noise, but noise whose *level*
still rises and falls with the energy we feed in. What the handset produced
was a constant-level source for 87 seconds while we sent a recording with
53 dB of dynamics. The spectrum is broadband (centroid 1307 Hz, 6.2 % below
500 Hz, 7.3 % above 3 kHz), which is the "wind" of every previous run.

**So the chip was not reading our data at all, and every format and rate
conclusion of the last three days was drawn from a path that was never
carrying our content.** That includes this morning's §1 and §2: the word-hold
measurement stands on the chip's own uplink and is unaffected, but the
inference that rate was *the blocker* does not follow.

It also retires, for now, the question the whole A0 probe was built to answer.
Those four segments were never compared by the chip.

**What this points at: the slot.** `audio.tone_word` has defaulted to **0**
since the first tone test, on the strength of slot 0 carrying the bearer in
the chip's **own** frames (`v22.raw`). That is the receive direction and says
nothing about which transmit slot the chip reads. The assumption has never
been tested, and `[CR A-DSP-8]` §9 says the mask's low three bits index a
13-entry table of radio-slot buffer addresses, with **the order of the two
mask arguments unresolved** — so which of our slots the chip is told to read
is exactly the thing the room could not recover statically.

`playslot=MS` (added today) walks the lane across every transmit slot during
one call, skipping the control slot, logging each change. Test card F4.8.

**Pre-committed readings:**

- **Speech appears during one slot's window** → that is the transmit bearer
  slot; set `toneword` and the downlink is solved, with the word-hold rate
  from part 2 already correct.
- **Nothing anywhere, all slots noise** → the slot is not the variable
  either. The remaining candidate is the matrix routing of `[CR A-DSP-8]` §9
  — which endpoint the channel configurator connects to the PCM lane — and
  that is a question for the room, not the bench. See `Q-0038`.
- **Level changes but no speech** → the chip reads that slot but interprets
  it differently; then format and rate come back, with a known-good slot.

**Method note, for whoever repeats this.** The acoustic recording settled in
one run what five attended calls of listening could not. Record the earpiece
by default from now on: an envelope correlation against the known source
separates "wrong format" from "not our data" instantly, and neither ear nor
log could tell those apart. Our logs show a perfectly healthy call in both
cases.

### `[CR A-0039]`: the chip was routed to silence, and the slot was right all along

The room answered `Q-0038` the same day. The decoupling has a mechanism, and
it is not the slot.

**The slot was never wrong.** DSP channel k uses PCM channel k in both
directions; PCM channel 0 is the control slot. `0x0242` returns channel 1, so
the lane we write is the lane the chip reads. The `0x0230` **mask is not a
slot selector** — it is an allowed-channel bitmask — and the two "mask"
arguments A-DSP-8 could not order turn out to be per-connection **radio**
slot bytes from the MAC, nothing to do with the TDM side.

**What blocks us is the route state.** The chip's channel configurator picks a
mode from the assign's flags and the path state:

| mode | when | routes | chain |
|---|---|---|---|
| **1** | **first `0x0230`, and the `0x022D` connect** | PCM out(k) ← **silence**; **sink** ← PCM in(k) | **disabled** |
| 2 | second `0x0230`, bit 6 **clear**, path on | PCM out(k) ← channel k; channel k ← PCM in(k) | **enabled** |
| 3/9 | second `0x0230`, bit 6 set, path on | variant-B input port (11+k−1) | per mode |
| 0 | release / disconnect | torn down | disabled |

**Our sequence `0x0251`, `0x0230`, `0x022D` runs mode 1 twice.** PCM output is
the silence constant, PCM input goes to a sink, the transmit chain is switched
off. Nothing we write could ever have reached the air.

**That is exactly what we measured** — and it is why the measurement looked
like nothing we could explain from format or rate. A lane routed to a constant
produces a constant-level output uncorrelated with our content. Our 3.8 dB
envelope against the source's 52.9 dB, and the −0.307 correlation, are what
"PCM out ← silence" looks like from the earpiece.

**Why stock does not have this problem:** stock never sends `0x022D`
(`[CR A-0034]`), and its live routes come from the MAC-side bearer handler,
which calls the same configurator with mode 2/3/8/9 when the bearer comes up
— **but only if a channel is already assigned at that moment**. We bind on our
own CC state, after CONNECT, so the handler finds no channel and our later
`0x0230` only ever runs mode 1. **This is defect C2** — "the audio bind fires
blindly, with no reference to any chip indication" — and it turns out to be
the cause, not a tidiness complaint.

**`0xF625` §4 is resolved and was our misreading.** Byte 5 is the MAC's
long-slot flag, set from the bearer the handset asked for (2 = long slot),
**independent of `0x0230`**. So "long slot, wideband" alongside a 40-byte DSP
configuration was never a contradiction: the air bearer is long while the DSP
chain sits parked in mode 1. A-DSP-8 §3 stands.

**Implemented: `-a remode=MS`** sends a second `0x0230` with flag bit 6 clear
MS ms after `0x022D`, which is the room's T2 and the one-call fix candidate.
Test card **F4.9** runs it together with T1, the read-only matrix read, so the
route state is recorded whether or not we hear anything.

**Pre-committed readings** (the room's, adopted verbatim):

- **T2 works** → envelope correlation with the source rises from −0.3 to well
  above 0.5, and T1 reads `0x085D` at dest 6 (PCM out ← channel 1) and
  `0x0BC7` at dest 1.
- **T1 confirms mode 1 and T2 does not move it** → dest 6 reads `0x0410`
  (silence) and dest 16 reads `0x0BC7` (PCM in parked in the sink), channel-1
  enable word at `0x10802` reads 0. Then the re-assign is not reaching the
  configurator's mode-2 branch and T3 (bind before bearer-up, stock ordering)
  is next.
- **T1 shows a live route but no audio** → the route is right and the lane
  content is wrong; format and rate come back, now on a known-good path.

**Do not re-run the old GenDSP source probes.** The "test tone 15 / silence
12" ids were AVM's *host-side* names; in this chip's table 12–15 are idle
channel ports and silence is 21, which is why every one of them produced
identical silence. `[CR A-0039]`

### T1/T2 result, 2026-10-23: the mode-1 diagnosis is falsified by its own test

`vF49` ran `[CR A-0039]`'s T2 and T1 together. The new mail went out as
designed — `0x0230 {01 41 02 00 00 03}` at 50289, `0x022D` at 50317, then the
re-assign `0x0230 {01 01 02 00 00 03}` at 50817 — and the read-only matrix
read ran 397 times across the call.

`[observed]` Matrix destination table at `0x1168A`, channel-1 enable word at
`0x10802`:

| moment | dest 1 | dest 6 | dest 11 | dest 16 | enable |
|---|---|---|---|---|---|
| idle, before the bind | 085d | **0410** (silence) | 0bc5 | 0bc5 | **0000** |
| after `0x022D`, before the re-assign | 085d | **085d** | 0bc5 | 0bc5 | **0003** |
| after the mode-2 re-assign | **0bc5** | 085d | 0bc5 | 0bc5 | **0001** |

**`[CR A-0039]`'s central claim does not hold.** It predicted our sequence
leaves the chip in mode 1 — `dest 6 = 0410` (routed from the silence
constant) and the channel chain disabled. Our own bind already moves
`dest 6` to `085d` and the enable word from 0 to 3. The silence routing is
the **idle** state, which we leave as soon as we bind.

So the decoupling we measured was never "routed to silence".

**What T2 did do** is install `dest 1 = 0bc5` — PCM-in(1) into channel 1,
which is the **downlink** direction and genuinely was not routed before. The
pre-committed value `0x0BC7` is `0x0BC5` here, two low; the room's other
values match exactly, so this looks like a small offset in their reading
rather than a different state.

**Both directions are now routed and the chain is enabled, and the audio is
still white noise.** That is the room's third branch: *route right, lane
content wrong.* Format and rate are back — but now on a path verified live by
direct memory read rather than assumed.

### The next candidate is specific, not a shrug

`[CR A-0039]` marks `dest 1` (variant A) as the likely **G.726 encoder**
input and `dest 11` (variant B, selected with flag bit 6 **set**, mode 9) as
the likely **transparent** already-coded path. It could not prove which DSP
routine consumes which.

`[CR A-DSP-8]` found **no G.722 encoder in either STUB build**. Our call is
long-slot wideband. A wideband downlink therefore *cannot* be encoded by the
chip and must arrive already coded — so wideband has to use the transparent
input, variant B.

**We routed variant A and fed it G.722.** White noise is what an encoder fed
coded octets produces. The two readings agree.

`-a remodebit6` keeps bit 6 set in the re-assign, asking for mode 9 and
`dest 11 ← PCM in(1)`. Test card **F4.10**, same G.722 lane whose rate is
already verified.

**Pre-committed:**

- **Speech** → variant B is the transparent path, wideband is solved, and the
  rate work of part 2 was necessary after all.
- **Noise, but `dest 11` changes from 0bc5** → routed, still wrong content;
  then the lane wants PCM after all and `fmtprobe.raw` becomes the next run,
  now on a route we can verify.
- **`dest 11` does not change** → the mode-9 branch is not reached from our
  state, and T3 (bind before bearer-up, stock's ordering) is the remaining
  test.

**Method note that earned its keep:** T1 was read-only, cost nothing, and
answered what no amount of listening could — it falsified the room's
diagnosis and redirected the next run in the same call that tested it. Pair
every audio test with a read-only state read from now on.

### Mode 9 is worse, and that identifies the direction of `dest 6`

> **Contested 2026-10-23 by `[CR A-0041]`, and our inference was thin.** The
> room says `dest 6` is PCM channel 1's *output* register — the word the host
> **reads**, i.e. the **uplink** — and that the earpiece is driven by what we
> write into PCM-in, consumed at `dest 11` (variant B) or `dest 1` (variant
> A). Our reading rested on **two** runs in which `dest 6` and the channel's
> direction words both differed, and we had not read the direction words at
> all, so the correlation is confounded. The matrix run (F4.13) reads
> `dest 1`, `dest 6`, `dest 11`, both direction words and the enable word
> across eight windows with an acoustic recording, which settles it properly.
> Do not cite the conclusion below until it does.


`vF410` kept flag bit 6 set in the re-assign, asking for mode 9. Result at the
handset: **white noise for about a second, then silence** — the noise stopping
when the re-assign landed, 500 ms after `0x022D`.

`[observed]` | run | re-assign | `dest 6` after it | handset |
|---|---|---|---|
| `vF49` | mode 2, bit 6 clear | **085d** (channel 1) | white noise |
| `vF410` | mode 9, bit 6 set | **0410** (silence) | silence |

**That correlation settles the direction we had been guessing at.** `dest 6`
is the route feeding the **air**, not the host: when it holds the silence
constant the handset is silent, when it holds channel 1's output the handset
gets whatever channel 1 produces. So the downlink chain is

> our lane → `dest 1` (channel 1 codec input) → channel 1 → `dest 6` → air

and `vF49`'s mode 2 had **both** links live (`dest 1 = 0bc5`, `dest 6 =
085d`). **Mode 9 breaks the chain**; it is not the wideband path we hoped for.

`[observed]` Note also that `vF410`'s *idle* read already showed `dest 1 =
0bc5`: the route `vF49` installed **persisted across runs**, so idle state is
not a clean baseline between calls.

### So the lane wants PCM, and `[CR A-DSP-8]` §2 said so all along

With the chain verified live and fed G.722, we got noise. `[CR A-DSP-8]` §2:
**every per-channel transmit chain toward the air is a G.726 encode**,
preceded by a law-to-linear step. An encoder's input is PCM. We fed coded
octets into an encoder.

The wideband reasoning that sent us to mode 9 was wrong in one step: `0xF625`
byte 5 is the **air slot size** (`[CR A-0039]` §4, which we confirmed), not a
statement that the lane carries G.722. The chip encodes G.726 into the long
slot; the lane upstream of that encoder is PCM at **one sample per frame**.

The two-frame word hold we measured is a property of the chip's **uplink**,
which is a different path and still stands for that direction.

**This also explains the 2026-10-22 tone.** A linear sine, one sample per
frame, is the right format for this chain — the tone was audible because the
format was right, and speech failed because `dest 1` was not routed until the
re-assign existed. Both halves were needed and we never had them at once.

**F4.11**: mode 2 routing, `work/audio/linlane.raw` — the same speech,
normalised to −12 dBFS, linear 16-bit LE, one sample per frame.

**Pre-committed:**

- **Speech** → the downlink is solved: mode-2 re-assign plus linear PCM.
- **Noise with `dest 1 = 0bc5` and `dest 6 = 085d`** → chain live, format
  still wrong; then A-law in one byte half is next (`fmtprobe.raw` segments
  2 and 3), since the chain begins with law-to-linear.
- **Silence** → the re-assign did not take this time; compare the route reads
  against `vF49` before concluding anything about format.

### `[CR A-0040]`: the bind already ends in mode 9, and the untested state is mode 8

The room explained T1 and withdrew A-0039's mode-1 claim. Two of its three
checkable predictions match our readings exactly:

| claim | predicted | measured | |
|---|---|---|---|
| the bind ends in **mode 9** (the MAC-side bearer handler re-runs the configurator once the path is on, choosing by codec byte) | dest 6 = 085d, dest 11 = 0bc5, enable 3 | dest 6 085d, dest 11 0bc5, enable 0003 (`vF49` after `0x022D`) | ✅ |
| **mode 2 = the narrowband G.726 encoder**, enable word → 1 | dest 1 ← PCM-in, enable 1 | dest 1 0bc5, enable 0001 (`vF49` after the re-assign) | ✅ |
| an explicit **mode 9** re-assign gives dest 6 ← channel 1 | dest 6 = 085d | dest 6 **0410** (`vF410`), and the handset went silent | ❌ |

So our white noise was G.722 octets fed into a **G.726 encoder** — which is
what we had concluded independently. `[CR A-0040]` §2 adds the part we had
wrong: mode 2's chain starts **law→linear**, so that lane wants A-law or
µ-law, not the 16-bit linear we were about to send. **F4.11 as built would
have failed**, and for a reason we would have misread as "format still wrong".

**The reported mode-9 mismatch is left open.** Our explicit re-assign plainly
does not land where the MAC handler's mode 9 lands; reported back.

### Why mode 8 is the candidate, and why we nearly missed it twice

`[CR A-0040]` §5: **mode 8 is the only mode that configures the 80-byte
B-field** — the one our long-slot bearer actually carries. It sets direction 1
(transparent downlink) on, direction 0 (uplink) **off by design**, enable word
0, PCM-out(1) ← constant 0.

**`A-0033` rejected codec byte 1 because the uplink lane stayed `0x0000`.**
That is precisely what mode 8 does on purpose, and the downlink was never
checked acoustically. We dismissed the right configuration on evidence from
the direction it deliberately disables.

That is the second time today a conclusion rested on an instrument pointed at
the wrong direction — the first being the uplink-derived counters of
2026-10-20.

**F4.12** = T5: re-assign `{41, 0002, 00, 01}` after `0x022D`
(`-a remode=500,remodebit6,remodecodec=1`), G.722 lane unchanged, plus reads
of the two channel-1 direction words.

**Pre-committed** (the room's, adopted):

- **Envelope correlation > 0.5** → mode 8 is the wideband downlink
  configuration. **The uplink lane reading `0x0000` is expected here, not a
  failure.**
- **Correlation but garbled** → the transparent block takes one octet per
  frame, not two; repeat with one octet in the low byte.
- **No correlation** → mode 8's downlink is decoupled too, and the suspect
  becomes B-field/bearer timing rather than routing.

**Mode-8 signature to confirm we got the mode:** dest 6 = `0x040E`, dest 11 =
`0x0BC5`, enable = **0**, direction-0 word at `0x116FA` = **0**, direction-1
word at `0x11712` = **1**. (Mode 9 reads 2 and 1; mode 2 reads 0 and 0.)


### `[CR A-0041]`: T5 stands, but we were watching the wrong word

The room corrected its own A-0040 before we scored T5, and the correction
matters more than the confirmation.

- **T5 is correctly specified.** The dispatcher's mode selector for an
  explicit `0x0230` re-assign is the **codec byte**: 1 → mode 8, 2–4 → mode 9,
  else mode 3. `{41, 0002, 00, 01}` does select mode 8.
- **`dest 6` is the uplink port.** `[CR A-0040]` led us to watch it to judge
  the earpiece; `[CR A-0041]` withdraws that. In mode 8, `dest 6` reading
  silence is **expected** — the uplink is off by design — and is not a fault.
- **The dest-6 divergence between the two callers is real but is an uplink
  difference**, and was never what silenced the handset.
- **What to judge by instead:** the earpiece, `dest 11`, the two channel-1
  direction words (`0x116FA`, `0x11712`) and the enable word.
- **Our delta-baseline point is adopted**: route and block state persist
  across calls and daemon restarts, so a pre-bind read is not a clean zero and
  every signature must be read as a within-session delta. The room notes its
  own "idle" columns are unreliable for the same reason.
- **The A-0033 re-open is confirmed as the only one needed.** Codec 1 was the
  single configuration rejected on uplink-only evidence; the codec 0 and 3
  results were judged on direction-neutral lane liveness.

**`[CR A-0041]` §4 names the likely failure mode**, and it is one our reads
can see: a channel's transparent downlink block has a **mode word separate
from its matrix route**, so the route can read live (`dest 11 = 0bc5`) while
the block is switched off. That is what `vF410` probably was. The room could
not establish statically why an explicit re-assign leaves it off where the
MAC handler leaves it on, and declined to guess.

**None of this changes the matrix run**, which already reads every word in
question. Windows 1 and 2 *are* T5, with both G.722 packings, and windows 0
and 3–7 cost nothing extra. One call now answers T5, the gauge question and
the format question together, each window carrying its own proof of state.

---

## SOLVED: mode 2 plus signed 16-bit linear PCM, one sample per frame `[vF413, 2026-10-23]`

The matrix run worked. Eight (chip mode × lane format) windows in one call,
each with its own route reads, and an acoustic recording of the earpiece.

**Envelope cross-correlation of the recording against our 7.5 s source,
searched over the whole recording: `r = +0.922` at recording t = 42.04 s.**
That lands 0.5 s into **window 5** — exactly past its zero gap. The next
peaks are +0.55 and below, so the result is not a near-tie.

| win | mode | lane format | dest1 | dest6 | dest11 | en | dir0 | dir1 | earpiece |
|---|---|---|---|---|---|---|---|---|---|
| 0 | bind default | G.722 held | 0bc5 | 085d | 0bc5 | 0003 | 0002 | 8001 | noise |
| 1 | mode 8 `41:01` | G.722 held | 0bc5 | 040e | 0bc5 | 0000 | 0000 | 8001 | noise |
| 2 | mode 8 `41:01` | G.722 one/frame | 0bc5 | 040e | 0bc5 | 0000 | 0000 | 8001 | silence |
| 3 | mode 2 `01:03` | A-law low | 0bc5 | 085d | 0bc5 | 0001 | 0000 | 0000 | silence |
| 4 | mode 2 `01:03` | A-law high | 0bc5 | 085d | 0bc5 | 0001 | 0000 | 0000 | noise |
| **5** | **mode 2 `01:03`** | **linear 16-bit LE** | 0bc5 | 085d | 0bc5 | 0001 | 0000 | 0000 | **SPEECH r=0.922** |
| 6 | mode 9 `41:03` | G.722 held | 0bc5 | 085d | 0bc5 | 0003 | 0002 | 8001 | noise |
| 7 | mode 9 `41:03` | A-law low | 0bc5 | 0410 | 0bc5 | 0000 | 0000 | 0000 | cut short |

**Windows 3, 4 and 5 are a controlled experiment.** Identical chip state in
every word we read — `dest1 0bc5, dest6 085d, dest11 0bc5, enable 0001,
dir0 0000, dir1 0000` — and only the lane format differs. A-law in either
byte half fails; **signed 16-bit linear PCM, one sample per frame, works.**

### The answer

**Downlink audio needs two things, and we never had both at once:**

1. **A second `0x0230` with flag bit 6 clear, after `0x022D`** — mode 2. The
   bind alone leaves the chip in mode 9, where a linear lane is noise.
2. **Signed 16-bit linear PCM on the lane, one sample per frame** (8000 Sa/s).

### What this means for the retraction of this morning

The 2026-10-22 conclusion — "the lane carries signed 16-bit linear PCM" — was
**correct**. This morning's retraction of its *evidence* still stands: the
two-tone test ruled out G.722 and nothing more, and a linear sine misread as
A-law does peak at 400.0 Hz. The reasoning was unsound and the conclusion was
true, which is exactly the case a retraction is supposed to catch and did.
What was missing was never the format; it was the mode-2 re-assign.

The rate work of part 2 (the two-frame word hold, 8000 octets/s) remains
correct **for the uplink**, which is a different path and a different format.

### What the clean room got right and wrong

`[CR A-0039]`/`[CR A-0040]`/`[CR A-0041]` supplied the mode table, the
`0x0230` mode selector, the direction words and the memory addresses, without
which none of this was observable. Scoring:

- **Right:** mode 2 exists and is reached by a bit-6-clear re-assign after
  `0x022D`; the enable word reads 1 there; the bind lands in mode 9; the mode
  table's structure; every memory address.
- **Wrong:** `[CR A-0040]` §2's "the mode-2 chain starts law→linear", which
  implied the lane wants A-law. A-law was tested in both byte halves against
  an identical chip state and failed; linear worked.
- **Wrong:** `[CR A-0040]` §5's mode 8 as the wideband candidate. Mode 8 was
  reached (windows 1 and 2, `dest6 040e`, `enable 0000`) and produced noise
  and silence.
- **`dest 6` is not the earpiece gauge**, as `[CR A-0041]` said: windows 3–5
  share `dest6 = 085d` and differ entirely in what was heard. Our contested
  claim from `66be325` is now **withdrawn** — the room was right and our
  two-point correlation was coincidence.

### Next: reproducibility, which is now the whole question

One window of one call. The bind is known to be unreliable (`vF49` vs
`vF412`), so the open question is no longer *what* but *how often*. **F4.11**
replays `linlane.raw` for a whole call with `remode=500` and the route reads.

**Pre-committed:** speech for the whole call, repeatably → the downlink is
done and the work moves to `-a play=` emitting linear by default, then the
uplink. Intermittent speech with the route words identical across good and
bad calls → the intermittency is below the route layer and A5 is back, now
with a known-good configuration to compare against.

### `[CR A-0042]`: what `dir1 = 0x8001` means, and the risk to reproducibility

The room accepted the mode-2 + linear-16-bit result as the settled
**narrowband** downlink configuration and withdrew both falsified claims:
`[CR A-0040]` §2 (the LAW2LIN chain head is a parameterised routine that can
be configured to pass linear through, so the G.726 encoder receives linear as
it should) and §5 (mode 8 is not the wideband path).

**`dir1 = 0x8001` answered.** `0x8001` is never written as a constant: the
configurator writes only bit 15 into that per-channel descriptor for the
wideband/transparent classes, and the DSP sets the low bit at run time. So
`0x8001` means *coded block armed and active* — a descriptor for the coded
passthrough, **not** an on/off flag and **not** our working path. In mode 2 it
correctly reads 0, and for narrowband it should stay 0.

**The named risk to reproducibility:** the MAC bearer handler may re-run
**mode 9** mid-call — its default for our bytes — which would re-arm `dir1`
and break the linear path. The silent-transition signature to watch for is
**`enable` → 3 and `dir1` → `0x8001`**.

F4.11 therefore does two things in one call: it plays the linear lane for the
whole call *and* re-asserts mode 2 at t+20 s and t+40 s.

- **Audio holds throughout** → mode 2 is stable; the downlink is done.
- **Audio dies, then returns at 20 s or 40 s** → the handler interferes *and*
  re-asserting is the fix, both established from one call.
- **Audio dies and stays dead with the route words unchanged** → the
  intermittency is below the route layer; A5 returns, now with a known-good
  configuration to diff against.

**Wideband is deferred, and the room flags it as hard:** the coded block wants
G.722 octets and `[CR A-DSP-8]` found the chip has no G.722 encoder, so the
host would have to encode them. Narrowband reproducibility first.

### Reproduced for a whole call `[vF411, 2026-10-23]`

`-a remode=500` plus `linlane.raw` (signed 16-bit linear, one sample per
frame, −12 dBFS) played for the **whole call**, reported audible throughout.
The downlink is reproducible, not a one-window fluke.

`[observed]` Route words after the bind at 47874 ms, read ~4×/s for 73 s:

| word | behaviour |
|---|---|
| `dir1` (`0x11712`) | `8001` at the bind → **`0000`** within 1 s, then unchanged for the rest of the call |
| `dir0` (`0x116FA`) | `0002` → **`0000`** within 1 s, unchanged |
| `enable` (`0x10802`) | **`0001`** from the bind until the call ended |
| `dest1` (`0x1168C`) | **`0bc5`** throughout, no transition |

**`[CR A-0042]`'s predicted break never happened.** The MAC bearer handler did
not re-run mode 9 — no `enable → 3`, no `dir1 → 0x8001`. Mode 2 is **stable
once set**, and the two re-asserts at t+20 s and t+40 s were unnecessary.
Worth keeping as a cheap guard until more calls confirm it.

### Two things the maintainer noticed, both expected and one now fixed

**1. A burst of white noise at the start of every call.** That is the ~500 ms
between `0x022D` and the re-assign, during which the chip is still in mode 9
and our linear lane decodes as noise — visible in the table above as the
one-second `dir1 8001 → 0000` transition. There is nothing useful to send in
that window, so the lane is now **held silent until the re-assign has gone
out** (`remode_done`). Fixed in code, not worked around in the test rig.

**2. No HD symbol: the call is narrowband.** Expected and correct. Mode 2 is
the **narrowband G.726** path — the chip encodes G.726 from our linear PCM.
Wideband is a different configuration and is the next real question.

### Wideband: what it needs, and the one piece of good news

`[CR A-0042]`: the coded block (`dir1` = `0x8001`, "coded block armed and
active" — bit 15 written by the configurator for the wideband/transparent
classes, the low bit set by the DSP at run time) wants **G.722 octets**, and
`[CR A-DSP-8]` found the chip has **no G.722 encoder**. So for wideband the
host must encode.

**We already can:** `dect_g722_encode()` exists in `src/libopendectcell/dect_g722.h`
and `-a playpcm=` has been using it to encode at start-up since before any of
this. So wideband is not blocked on an encoder; it is blocked on finding the
configuration that arms the coded block *and* reads our lane — which is the
mode-8/mode-9 territory that produced noise and silence today, now
re-approachable with a known-good narrowband baseline to diff against.

**Order of work:** narrowband end to end first (make `-a play=` emit linear
by default, then the uplink), wideband after.

### The re-assign delay matters, and 150 ms is too early `[vF414, 2026-10-23]`

`vF414` was meant to capture the uplink in mode 2. It did not get mode 2.

| run | re-assign sent | `enable` | `dest 6` | heard |
|---|---|---|---|---|
| `vF411` | `0x022D` + **500 ms** | `0001` for 73 s | `085d`, steady | speech, whole call |
| `vF414` | `0x022D` + **152 ms** | **never reached 1** | `085d` → **`0410`** at t+4 s | constant buzzing |

**The two runs differ in nothing else.** The MAC-side bearer handler evidently
runs its own configurator pass some time after `0x022D`, and a re-assign that
arrives before it is simply overwritten — the handler puts the channel back to
its default for our bytes and the mode-2 state is lost.

This was a regression we introduced by defaulting `remode_ms` to 150 ms on the
reasoning that it is "comfortably after the `0x0242` that triggers `0x022D`".
That reasoning was about the wrong event. **500 ms is the only value with
evidence behind it**, and the default is back to 500.

`[observed]` It also explains the buzzing: with mode 2 lost, the lane's idle
fill is being read by whatever path mode 9 leaves active.

**The uplink question is therefore still open** — `vF414` captured 34.3 s of a
call that was not in the configuration we meant to measure. For the record, in
that (mode-9-ish) state the capture showed slot 0 almost dead (85 changes in
34 s, mostly `0xFEFE`) and slot 5 holding four values around A-law zero at
−67.7 dBFS with 0.7 dB of envelope range: idle dither, not voice. Neither is
evidence about mode 2.

**Standing lesson, the third of the day:** a configuration that is known to
work has a *value*, not just a shape. Changing a proven constant for a
plausible reason is a change to the experiment, and it needs its own
confirmation run before anything else is measured on top of it.

---

## THE INTERMITTENCY HAS A NAME: a SETUP sent on a lingering link `[2026-10-23]`

The long duplex run (`vF415`) failed completely — `enable 0000` and
`dest 6 0410` in all eight windows, through eight separate mode-2 re-assigns,
with the maintainer hearing only a quiet ~400 Hz tone. Tabulating it against
every run of the day produced a **7/7 split with no exceptions**:

| run | how the SETUP went out | first `0xF625` byte 5 | mode 2 reached? |
|---|---|---|---|
| `vF49` | page response received | `0x02` | **yes** |
| `vF413` | page response received | `0x02` | **yes** |
| `vF411` | page response received | `0x02` | **yes** |
| `vF410` | a link was already up | `0x01` | no |
| `vF412` | a link was already up | `0x01` | no |
| `vF414` | a link was already up | `0x01` | no |
| `vF415` | a link was already up | `0x01` | no |

**Read byte 5 narrowly: only the first record splits.** The full series per
run, in order, shows the value flipping mid-call in *both* classes:

```
vF49    02 01 01 01 01 01 01
vF413   02 02 02 02 01 01 01 02 02 02 02 02 02 02 02 02 02 02 02
vF411   02 01 01 01 01 01 01 01 01 01
vF410   01 02 02 02 02 02 02 02 02 02 02 02 02 02
vF412   01 02 02 02 02 02
vF414   01 01 01 01 01
vF415   01 01 01 01 01 01 01 01 01 01 01 01 01 02 02 02 01 01 ...
```

`vF49` and `vF411` carried working audio while byte 5 read `0x01` for most of
the call; `vF413` was audible in a window where it read `0x02`. So **byte 5 is
not a property of the call and it does not track audio** — only the value in
the *first* record after the `{CC-SETUP}` splits with the ring path, 7/7.
An earlier version of this table also claimed a 0-vs-4 split in codec lists
sent. **That claim has no support in the logs** — no codec-list IE is logged
at all — and it is withdrawn. Printing the whole series, again, moved a clean
two-value split to a much narrower claim.

**Every call that paged the handset got working audio. Every call that reused
an existing link got none.** Nothing else separates them: identical `-a`
options, identical mails, the same re-assign at the same +500 ms offset.

### What was happening

`ring_tick()` took a shortcut: if a link was already up, the handset does not
need paging, so the `{CC-SETUP}` went straight out on the existing link. That
is reasonable DECT and it is where the audio died. A SETUP sent on a lingering
link brings the bearer up as **slot type `0x01`** in a state whose audio path
we cannot configure — the chip's enable word never reaches 1 no matter how
often mode 2 is requested. **`vF415` asked eight times and never got it.**

**This is the intermittency we have chased since 2026-10-20.** "Tone at 20:40,
silence at 20:45, same settings, nothing in the log" was never random: it
tracked whether the handset's previous link had cleared. It is also why
`vF414` looked like a 150 ms timing regression — that run *was* a lingering
link too, so the delay change and the real cause were confounded. The 500 ms
default is still right (`vF411` proves it), but it was not what `vF414`
demonstrated.

### The fix

`ring_tick()` now **waits up to 20 s for the link to clear**, then pages
normally. If it does not clear it falls back to the old behaviour and logs
`EXPECT NO AUDIO`, since a call with broken audio still beats no call.

`[observed]` It also explains the maintainer's own remark before `vF411` —
*"the old call on the handset was still open, sorry"*. That was the signal,
and we both read it as an aside.

### What this does to today's conclusions

- **The downlink answer stands**: mode 2 plus linear PCM. Proved on `vF413`
  and reproduced on `vF411`, both page-response calls.
- **The uplink is still unmeasured.** `vF414` and `vF415` are both lingering-
  link calls, so neither says anything about mode 2's uplink. The capture in
  `vF415` shows slot 5 at −67.6 dBFS with 0.7 dB of envelope range in every
  window — idle dither, from a call whose audio path was never configured.
- **The mode-9 detour and recovery test in `vF415` are void** for the same
  reason.

**Standing method note:** before reading anything into a bench result, check
`ring:` in the log. If it says *"a link was already up"*, the run carries no
audio information at all. That check now costs one grep and would have saved
four runs today.

---

## THE the analysis session ROUND: `[CR A-0043]` … `[CR A-0046]`, 2026-10-23 evening

Four briefs went to the analysis side (`Q-0039` … `Q-0042`) and all four came back
the same evening. The headline is that **most of what we built today is
scaffolding around a handshake the chip does by itself**, and that the
lingering-link defect has a cause-level fix we can make in our own code.

### `[CR A-0046]` — the sequence we should have written

Stock's connect step is **`0x0251` then `0x0230`, and nothing else.** It never
sends `0x022D` and it never re-assigns. It does not need to, because the chip's
own MAC-side bearer handler runs the channel configurator on bearer-active
event `0x70` using the flags and codec byte from that single `0x0230`.

| we send | stock sends | A-0046's verdict |
|---|---|---|
| `0x0230` flags **`0x41`** (bit 6 **set**) | flags with bit 6 **clear** for narrowband | bit 6 is the sub-mode flag; clear it in the **first** assign and the chip lands mode 2 on its own |
| `0x022D` connect | **never** | drop it |
| second `0x0230` at +500 ms | **never** | drop it; the timer exists only to beat a race |
| audio-up on a wall clock | on `0x0242` + the bearer indication | key off the event |
| `0x0251` with a fixed selector/value | selector from the flags' low three bits, value from EEPROM `0x2A + link index`, default **`0x2B`** | align it |
| `0x0230` offset-4 byte constant 0 | 1 when a per-connection state byte is `0x10`, else 0 | cheap to try |

**This is directly testable and needs no code**: `-a flags=0x01,noconnect,
noremode` already expresses it. `./f4.sh 16`.

**There is a tension worth naming before we run it.** `A-0040` §1 said the
`0x022D` connect's **path-on flag** is what *lets* the handler run the
configurator again. `A-0046` says the handler runs on event `0x70` regardless.
Both can be true only if path-on is not a precondition for `0x70`. If step 16
fails, `./f4.sh 17` (same first assign, `0x022D` kept) separates the two
readings in one more call. **Do not guess between them.**

### `[CR A-0045]` — the lingering link, explained and fixed at the cause

`dest 6 = 0x0410` with `enable = 0` is **not a mute. It is the unconfigured
channel default** — what the channel holds before anything binds it.

The configurator runs from exactly one site: bearer-active event **`0x70`**,
gated on the connection record's assigned-channel field `record[0x137]` being
non-zero and not `0xF`. Event `0x20` (bearer allocation) is what sets that
field. A `{CC-SETUP}` on an existing link brings the traffic bearer up
**without a fresh allocation**, so `record[0x137]` is never set, the `0x70`
guard fails, and the configurator returns without touching the channel. Every
re-assign we send writes mode words for a channel that was never bound —
which is why `vF415`'s eight attempts changed nothing.

Two consequences, both ours to act on:

1. **The 20-second wait is the wrong fix.** `A-0046` §2: stock releases the
   traffic bearer at the end of each call, so the next call always allocates a
   fresh one. Our lingering link is a **teardown gap** — we keep the bearer up
   and ride it. Releasing it at end of call is the cause-level fix.
2. **A one-read bind-time test replaces the wait entirely.** Read `0x10802`
   after the first `0x0230`: **1** = configured, proceed; **0** (with
   `dest 6 = 0x0410`) = this bearer can never carry audio, page instead. The
   earlier tell is `record[0x137] = 0`.

And `0xF625` **byte 5 is the air bearer's long-slot flag** (2 = long slot,
1 = normal), renegotiated during a call. That is why it splits 7/7 on the
first record and then wanders. It is not an audio signal; we stop reading one
out of it. The reliable observable is the tuple (`enable`, `dest 6`, `dir0`,
`dir1`), which we already read.

### `[CR A-0044]` — the mode-2 uplink is full duplex

`dir0` does **not** need arming. In the working mode-2 state the matrix already
routes `dest 6` from the channel's decoded output (`0x085D`) and `enable = 1`
opens that output stage, so the air→host path is wired and gated on in the
state that already carries our downlink. It is the far end's audio, not a
loopback or a sidetone.

The format is the open part, and it is the `LIN2LAW` counterpart of the
`LAW2LIN` stage that `A-0042` found passing linear on the downlink:

1. **signed 16-bit linear, one sample per frame** — symmetric with the
   downlink, the leading candidate;
2. **A-law in the low byte**, 8000 octets/s — `A-DSP-8`'s U1 again. U1 was
   falsified in **mode 9**, which carries G.722; mode 2 is the narrowband path
   U1 actually described, so it is live again here.

Not G.726 code words, and **not** the two-frame word hold we measured — that
is the wideband coded packing. One capture, decoded under both.

### `[CR A-0043]` — wideband: the full mode table, and a reachability verdict

The configurator's host-reachable modes are **{0, 1, 2, 3, 8, 9} and that is
the complete set**. Modes 4–7 exist in the code and **no host→chip mail
computes them** — including 6 and 7, the true two-slot wideband classes. Since
we do not write chip RAM, those are **out of reach, permanently**.

| mode | reached by | B-field | dir0 | dir1 | enable | dest 6 | coded block |
|---|---|---|---|---|---|---|---|
| 0 | `0x0233`, `0x022D` disconnect | — | 0 | 0 | 0 | silence | off |
| 1 | first `0x0230`, `0x022D` connect | — | writer | writer | 0 | silence | off |
| **2** | `0x0230` bit 6 **clear** | std | 0 | 0 | **1** | `0x085D` | off |
| 3 | bit 6 set, codec ≠1, not 2–4 | std | 0/1 | 1 | 1 | channel | armed |
| **8** | bit 6 set, codec byte **1** | **80 (long slot)** | 0 | 1 | 0 | const 0 | **armed** |
| **9** | handler default, codec 2–3 | std | 2 | 1 | 3 | `0x085D` | **armed** |

So wideband downlink, if it exists for us at all, is **mode 8 or mode 9 and
nothing else**. The good news: for channel 1 the direction-1 block head
(`0x0B89` / CPU `0x11712`) is referenced from the microcode's **host→air**
region as a circular-buffer parameter — the coded block is a **TX-side ring**,
so a transparent downlink path is structurally present, not merely an uplink
one. What the room could not settle is the packing the ring consumes, or
whether a host route actually fills it.

#### We disagree with the room's leading packing candidate, on arithmetic

`A-0043` leads with "two octets per 16-bit word, consumed every frame =
16 000 octets/s, the natural match for a 64 kbit/s coded air bearer". **It is
not a match; it is exactly twice one.**

```
80-byte long-slot B-field, one per 10 ms DECT frame = 8000 octets/s = 64 kbit/s
TDM lane:                        8000 frames/s  ->  one octet/frame = 8000 octets/s
                                                    two octets/frame = 16000 octets/s
```

One octet per frame is the rate-matched packing, and it is also the rate we
**measured** on the uplink (two octets per word with the word held two frames
= 8000 octets/s). So `./f4.sh 18` tests **one octet per frame first** and keeps
the room's candidate as the second window, because it costs one window — not
because the arithmetic supports it. The word-held-two-frames packing is not
retried: `vF413` falsified it.

`tools/make-lane-matrix.py --plan wideband` builds the lane and the matching
`seq=` fragment together, as the format plan does.

**If every window is silence or noise with the mode signatures confirmed**,
the room will trace the ring's fill edge to confirm it is air-fed only, and
**wideband downlink closes as unreachable on this STUB**. That is a real
outcome and we should report it as one, not keep probing.

### What this round changes in our code

| change | status |
|---|---|
| first `0x0230` with bit 6 clear, no `0x022D`, no re-assign | **testable today**, `./f4.sh 16`; no code needed |
| release the traffic bearer at end of call | the cause-level fix for the lingering link; needs the release ordering (`A-0046` deferred item) |
| read `0x10802` after the first `0x0230`, page if 0 | replaces `RING_LINK_WAIT_MS`; the one observable worth building |
| `0xFFF1` sub `0x04` PCM-phase poller (`A-DSP-8` §4) | **do not build it** — it was for the TDM-sync hypothesis the lingering-link finding replaced |
| `0x0251` selector/value alignment, `0x0230` offset-4 boolean | cheap, untested, fold into the next build |

### The idle baseline `[vF419, 2026-10-23 12:06, no ring, no call]`

`[CR A-0041]` warned that idle is not a clean zero. It is worse than that:
**most of the words we have been quoting as a mode signature read the same
value at idle, with no call up at all.** Sixty reads over 75 seconds, every
one identical:

| word | idle value | what we had been calling it |
|---|---|---|
| dest 0 | `0x0991` | — |
| **dest 1** (`0x1168C`) | **`0x0BC5`** | "mode 2: dest 1 = PCM-in(1)" |
| dest 2, 3 | `0x0991` | — |
| dest 4, 5 | `0x040E` | — |
| **dest 6** (`0x11696`) | **`0x0410`** | "the silence constant / the failure signature" |
| dest 7, 8, 9 | `0x0991` | — |
| dest 10 | `0x0410` | — |
| **dest 11** (`0x116A0`) | **`0x0BC5`** | "modes 8/9: variant B ← PCM-in" |
| dest 12–15 | `0x0410` | — |
| dest 16, 17 | `0x0BC5`, `0x0416` | — |
| **enable** (`0x10802`) | **`0x0000`** | "the failure signature" |
| **dir0** (`0x116FA`) | **`0x0000`** | "mode 2: dir0 = 0" |
| **dir1** (`0x11712`) | **`0x0000`** | "mode 2: dir1 = 0" |

**Consequences, and they are not small:**

1. **`dest 1 = 0x0BC5` proves nothing.** It is the idle value. Every time we
   have written "dest 1 = PCM-in(1), so the mode-2 route is in place" we were
   reading a word that was already there before the call. Same for
   `dest 11 = 0x0BC5`, which `A-0040` and `A-0043` both give as the mode-8/9
   variant-B route signature.
2. **`dir0 = dir1 = 0` proves nothing** either — also idle. The mode-2 tuple
   as we have been quoting it (`dest 1 = 0x0BC5`, `enable = 1`,
   `dir0 = dir1 = 0`) therefore has exactly **two** informative words:
   **`enable = 1`** and **`dest 6 = 0x085D`**. The rest is wallpaper.
3. **The failure tuple is literally the idle state.** `enable = 0`,
   `dest 6 = 0x0410`, `dir0 = dir1 = 0` is what the chip reads with no call at
   all. So "the lingering-link bearer reads the unconfigured default"
   (`A-0045`) and "no path-on, so mode 1 park" (`A-0047`) and "no call is up"
   are **three readings of one indistinguishable state**. We cannot currently
   tell them apart from chip memory, which matters because `A-0045`'s
   bind-time test rests on exactly this word.
4. Non-zero `dir0`/`dir1` and `dest 6 ≠ 0x0410` remain informative, because
   they are never the idle value.

Everything positive we concluded survives: `vF413`'s windows 3/4/5 were
separated by `dest 6 = 0x085D` **and** `enable` moving, and the audio result
itself is acoustic. But any claim that rested on `dest 1` or on the direction
words alone has to be re-read as "consistent with", not "shows".

`[observed]` Also confirmed by the same run: the slot search now locks via a
real `0xF601` contact answer (`control slot found: tx 1, rx 0 (answer prim
f601)`), the stored subscription reloads into the chip, and 927 mails arrived
with zero sequence gaps, checksum errors, framing errors or resyncs. The rig
is sound for the attended runs.

`[rig defect, fixed]` The run also exposed that `f4.sh` pulled its log to a
**relative** `work/box-tmp/`, so running it from `work/7520/lab` silently
dropped the log — the `scp` failure was swallowed by `2>/dev/null`. Now
absolute, and a failed pull says so.

### The 20-second link wait does not work `[vF420, vF420b, 2026-10-23 12:1x]`

The fix in `8aaadb3` had been deployed but **never exercised on hardware**:
no run since had taken its branch. Two unattended runs, nobody answering:

| run | page sent at | what `ring:` said |
|---|---|---|
| `vF420` | t+15 s | *a link is still up — waiting* → **did not clear in 20 s** → SETUP sent anyway |
| `vF420b` | t+70 s | *a link is still up — waiting* → **did not clear in 20 s** → SETUP sent anyway |

`[observed]` `reg.active` is set by `reg_start()`, which runs on the chip's
CONN_UP — i.e. **the handset holds a link**, not "a previous call of ours is
lingering". `vF420`'s log shows `reg: connection up (handle 0), C-plane armed`
about one second after the daemon starts. The C4 attaches as soon as the base
is up and keeps the link; it was still up 70 seconds later, and still up 20
seconds after that.

**So the wait is the wrong shape of fix.** It assumed a leftover link that
would time out. What is actually there is a handset attachment that does not.
On this handset, every call we place would take the fallback path — which is
exactly the path that cannot carry audio. The three runs that *did* get
`page response received` caught a window where the handset happened to have
released; that is luck, not a mechanism we control.

**What replaces it** (`[CR A-0048]`, from the stock host trace): stock calls a
**link release** at the end of every call, so the next call always starts from
no link and pages. The fix is in our teardown, not in a wait. Until that is
implemented, the honest status of `RING_LINK_WAIT_MS` is: it detects the
condition correctly and reports it loudly, and its remedy does not work.

### The connection-record read: addresses in hand, **not yet verified**
`[vF421, vF421b]`

`[CR A-0049]` supplied the map we asked for: per-connection record base
`0x1AB9`, stride `0x148`, six records; `+0x116` bit 6 = path-on, `+0x137` =
assigned channel, `+0x13b` = codec byte. Read-only, config fields only — we do
not dump the rest of the record, which has not been audited for identity
content.

Both runs read cleanly and returned **all zeros**: link 0's three fields in
`vF421`, and `+0x137` across all six records in `vF421b`.

**That is consistent with the room's pre-commit (`+0x137 = 0`, no channel
allocated) and it is not evidence for it.** An all-zero read is equally
consistent with the addresses being wrong for this build. Neither run had an
answered call, so there was no positive control — and `vF421b` did not even
reach `{CC-ALERTING}`. **The map stays unverified until one read shows
`+0x137` non-zero during a call that carries audio.** That is an attended
test, and it is now the first thing to check in the next working call.

This is the third time this week that a measurement without a positive
control has looked like a result. The rule stands: a reading that cannot
come out any other way has not measured anything.

### The connection-record map: our negative was real, our headline was wrong
`[vF422, 2026-10-23; corrected the same day by [CR A-0051]]`

`[CR A-0050]` supplied the positive control we asked for: record `+0x116`
**bit 5** (`0x20`) is a "a link exists" flag, set by the MAC on link
activation, present with or without a call. Since the C4 attaches a second
after the base comes up and holds the link (`vF420`, `vF420b`), a silent run
with no ring should show bit 5 set on exactly one of the six records — which
would verify base, stride and offset in one go.

It does not. Seventy seconds, no ring, the daemon logging `connection up`
twice during the run:

| record | address of `+0x116` | reads | value | bit 5 |
|---|---|---|---|---|
| link 0 | `0x1BCF` | 24 | `0x0000` | 0 |
| link 1 | `0x1D17` | 24 | `0x0000` | 0 |
| link 2 | `0x1E5F` | 23 | `0x0000` | 0 |
| link 3 | `0x1FA7` | 23 | `0x0000` | 0 |
| link 4 | `0x20EF` | 23 | `0x0000` | 0 |
| link 5 | `0x2237` | 23 | `0x0000` | 0 |

**The control for the control:** in `vF421` the *same* `0xFFF1` sub `0x07`
mechanism, in the same run, read `0x11696` as `0x0410` — real data — while
`0x1BCF`, `0x1BF0` and `0x1BF4` all returned `0x0000`. So the read path was
alive and the record region genuinely reads zero. This is not a failed read.

**What this falsifies is `A-0050`'s bit-5 claim, not the map.** `[CR A-0051]`
owned the error the same day: `+0x116` bit 5 is set by the MAC **bearer**
handler on the `0x20`/`0x50`/`0x70` events — that is, on a **call** — not at
MM attachment. "Non-zero whenever a link exists, with or without a call" was
wrong, and **our 70-second no-call run read zero correctly.**

Of the three explanations we listed, the third is the right one:

1. base or stride wrong — not confirmed and less likely. `0x1AB9` is runtime
   BSS, inside the image gap `0x1AAD`…`0x24F8`, so it is zero until the
   firmware populates it. A zero read with no call is a **correct read of an
   unpopulated record**.
2. **unreachable — ruled out.** Stock's own dump reads CPU low RAM with the
   same `0xFFF1` sub `0x07` 32-byte mechanism and gets real data. Our read
   path reaches that space. *(Our note: the ranges cited as precedent,
   `0x0003D4` and `0x000EF4`…`0x001754`, do not themselves contain `0x1AB9`,
   so this is an argument about the address space, not a direct precedent for
   this address.)*
3. **call-only — this is it.** The records are populated for call
   connections; a bare attachment is held elsewhere, in structures that are
   identity-bearing and therefore out of bounds for us.

**So there is no attachment-only positive control**, and the map can only be
verified during a live call. That verification now rides along on the next
attended run rather than costing one of its own: `./f4.sh 17` reads `+0x116`
and `+0x137` alongside `enable` and `dest 6`. Bit 5 set and `+0x137` holding
an assigned channel (1…6) while audio is up confirms base `0x1AB9` and stride
`0x148`.

If it still reads zero during a live call, the fallback is empirical: the
fingerprint of a populated record is **tx-slot (`+0x104`) = rx-slot (`+0xFD`)
+ 6**, repeating at a `0x148` stride. Diff a connected-call sweep of
`0x0000`…`0x3FFF` against the idle one and look for that pattern.

**Nothing in our code depends on the map.** The bind-time test uses `enable`
(`0x10802`) in shared RAM, which we read reliably; the record fields were a
bonus. The cleaner signal for "has a bearer been allocated" is the
`0x20` → `0x70` event instrumentation, which is ours to build.

`[method]` Worth noting what made this cheap: we asked for an observable that
would be non-zero in a state we could reach **with nobody present**, rather
than one that needed an attended call. The answer arrived, the run took 70
silent seconds, and it came back negative — which is still four minutes of the
maintainer's time not spent.

### The page is the bearer trigger — but an attached handset ignores the page
`[vF423, 2026-10-23]`

`[CR A-0052]` traced the recipe: stock sets a call's traffic bearer up by
**paging** (`0x0300`, page type `0x11`), and the causal chain is
page → handset answers over the air → chip allocates the traffic bearer →
MAC event `0x20` → event `0x70` → the configurator binds audio. The link that
is "already up" is the **attachment/signalling link**, not a traffic bearer,
which is why a SETUP sent on it never gets an audio path.

That explains our 7/7 split exactly, and the recommended fix was: page for
every call, even when attached — "light, not heavy", no release needed.

**We implemented it and it does not work on this handset.** `ring_tick()` now
pages unconditionally. With the C4 attached:

| run | link up at ring? | pages sent | page response | handset rang? |
|---|---|---|---|---|
| `vF411` | no | 1 | **yes, attempt 1** | yes, with audio |
| `vF423` | yes | **16** | **none** | **no — never alerted** |

`[observed]` Same build, same mails, same `0x0300 {2c 11 01}` basic service
`88`. The record fields and the matrix words stayed at their idle values
throughout, which is consistent: no page response, so no bearer, so no
configurator.

**The C4 ignores our page while it holds a link.** So on this handset the two
options are mutually exclusive as things stand:

- **page** → no response, no ring at all;
- **SETUP on the attachment link** → rings, no audio.

A fallback is therefore in place: page first, and if the pages go unanswered
while a link is up, send the SETUP on it and say `EXPECT NO AUDIO`. A call
that rings and is silent beats a handset that does not ring.

**The hypothesis this points at, which we have not tested:** the handset holds
that link because **we never release it**. `[CR A-0048]` found stock calls a
link release at the end of every call, and our daemon has no equivalent — the
C4 attaches a second after the base comes up and the link is still there 70 s
later (`vF420b`). If stock's handsets return to idle-locked because stock
releases them, then "the handset is permanently attached" is **our defect, not
the handset's behaviour**, and the page would be answered normally once we
stop holding the link open.

That makes the link release the next thing to implement, and it reframes it:
not a workaround for a lingering link, but the thing that keeps a handset
pageable. **We do not yet know which primitive releases the link** — asked.

`[method note]` This is the second recommendation this round that the bench
overturned (after the `0x022D` drop, which the room caught itself). Both were
sound readings of the firmware; both missed how this handset behaves in the
state we are actually in. The pattern is worth naming: **static analysis tells
us what the chip will do with a given input, and cannot tell us which inputs
this handset will produce.**

### Release-then-page: the page half works, the release half does not
`[vF423 second and third runs, 2026-10-23 13:0x]`

`[CR A-0053]` resolved the thread: stock never pages a linked handset, it
releases first and pages from idle-locked, and the release does **not**
un-attach — attachment and location registration are persistent and separate
from the CC/DLC link. Our daemon had no release at all, which is why the C4
sat linked from the moment it attached.

We added one (`dect_lapc_disconnect()` — we have parsed DISC since forever and
never had a way to send it) and made `ring_tick()` release, wait for the
chip's `CONN_DOWN`, then page. Two runs, and they separate cleanly:

| run | link up at ring? | what happened |
|---|---|---|
| 2nd | **no** | release branch never ran; page answered on **attempt 1**; `{CC-SETUP}` on a page response; handset rang |
| 3rd | **yes** | release branch ran, DISC transmitted (`tx cplane 3 bytes, addr 13 ctrl 53`), **link did not go down in 3 s**, pages unanswered again |

`[observed]` So the second run re-confirms what `vF411` already showed (page
from idle-locked works, first attempt) and says nothing about the release. The
third run is the test, and **the DLC DISC does not release this link**: the
frame went out, the handset kept talking — 300 further C-plane frames from it
after the DISC — and the chip never reported `CONN_DOWN`.

**Why this is the expected failure in hindsight.** `A-0053` said the release
is a **NWK release message** carried over the signalling path, with the
host-side trigger `0x0202` (TRIGGER_SIGNALLING), and explicitly that it is
*not* a dedicated chip "release link" primitive. We implemented a layer below
that — a DLC disconnect — and sent no `0x0202`. The handset is entitled to
ignore it, and did.

`[no harm]` The handset stayed fully responsive throughout; nothing wedged,
and no battery pull was needed. The local side effect to watch is that
`dect_lapc_disconnect()` clears our `established` flag while the peer keeps
sending I-frames, which can desync the link state. Worth a guard before this
is used anywhere but a test.

**What we still need is one fact:** which NWK release message, or which chip
mail, actually takes the link down. `A-0053` noted this is a NWK-capture
comparison rather than a host trace — which puts it on the Saturday stock
capture (`T5`/Capture C0) unless the room can name the message from the DLC
layer directly. Asked.

**Status of the ring path as it stands:** release-then-page is in, the release
is a no-op in practice, and the fallback still rings the handset over the old
link with `EXPECT NO AUDIO`. So audio is still only reachable on a call that
happens to find the handset idle-locked — exactly where we were this morning,
but now with the mechanism understood and one missing primitive named.

### Why the link never releases: an unanswered NWK transaction `[vF423, re-analysed]`

`[CR A-0054]` gave three ranked candidates for the failed release. Checking
our own code and log against them moved the answer to the third one, and
produced a correction to what we told the room.

**Correction first.** We reported "300 further C-plane frames from the handset
after our DISC". That was wrong: 270 of those lines are C-plane fill that
never parsed as a frame (`lc_rc=0`). The real figure is **30 LAPC I-frames**.
The inflated number came from counting log lines instead of parsed frames —
the same mistake class as the `0xF625` byte-5 table this morning.

**Candidate #1 is falsified in our source.** `A-0054` predicted we were not
acknowledging the handset's I-frames. We are: `dect_lapc.c` queues an RR with
`N(R) = V(R)` on every in-sequence I-frame, and re-acknowledges out-of-sequence
ones. The log confirms it — 15 RRs with `nr=0` and 15 with `nr=1` after the
DISC, alternating correctly.

**Candidate #3 is the answer, and the flood predates the DISC.** The handset
repeatedly sends a **segmented 85/86-octet NWK message** that we never answer:

| | before our DISC | after |
|---|---|---|
| `ns=1 more=1 infolen=63` (first segment) | 6 | 15 |
| `ns=0 infolen=22/23` (second segment) | 3 | 15 |

`[observed]` Twelve complete 85-octet messages and eight 86-octet ones in one
60-second run. We reassemble them correctly — `reg: nwk_len=85`, `nwk in <--`
— and then **do nothing**. The handset, having had no reply, retransmits.

So the link is held open by an **open transaction at the NWK layer**, not by a
DLC problem, and our DISC could never have worked: `A-0054` says stock's DLC
completes a disconnect only when the link is quiescent (TX queue empty and
every I-frame acknowledged), and a transaction the peer keeps retrying is the
definition of not quiescent.

**What the message is, we do not know.** Its header is `63 62 …`:
protocol discriminator **`0x3` = CC**, transaction identifier 6, F=0, and
**message type `0x62`** — which is not one our stack handles and not one of
the CC types we implement (`SETUP`, `CONNECT`, `RELEASE`, `RELEASE-COM`,
`INFO`). One further message in the same run carries PD `0x5` = MM, type
`0x54`, also unanswered.

**Hypothesis, not a claim:** `0x62` may be an escape-to-proprietary message —
in which case the C4 is asking the base something AVM-specific every few
seconds, our silence keeps a CC transaction open forever, and the link can
never go quiet enough to release. That would make this question and the
GAP/CAT-iq map (`Q-0045`) the same question. Asked; we are not guessing
further, since the whole point of the room is that we do not have to.

**No ciphering was active in this run**, so the payload is not encrypted and
the parse is not a decryption failure.

**What this reframes.** We have been treating "the handset holds the link" as
a property of the handset. It is a consequence of our own silence. Every
downstream symptom — pages unanswered, no traffic bearer, no configurator run,
no audio on any call that finds the handset linked — follows from one
unanswered message type.

`[privacy]` The payload is not reproduced here and was not sent to the clean
room; only the two protocol header octets, which carry no identity, crossed.

### Closing {CC-FACILITY} releases the link — and the handset comes straight back
`[vF423 fourth run, 2026-10-23 13:36]`

`[CR A-0055]` identified both unanswered messages from AVM's own debug name
table: MM `0x54` = `{LOCATE-REQUEST}`, CC `0x62` = `{CC-FACILITY}`, the
supplementary-services message, carrying a `<<FACILITY>>` IE of AVM
proprietary data that stock forwards to a FRITZ userspace app. We have no such
app, so the prescription was: close what we cannot answer, with
`{CC-RELEASE}` (`0x4d`) + `<<RELEASE-REASON>>` `[0xe2][0x01]`.

**One half of the diagnosis was already false.** `A-0055` predicted we answer
registration but not the follow-up location update. We do answer it: the log
shows `nwk out --> 85 55 …` — `{LOCATE-ACCEPT}`, MM type `0x55` — sent once,
early. The `{LOCATE-REQUEST}` was never the problem. **Independent
confirmation in the other direction:** our `<<RELEASE-REASON>>` IE id was
already `0xE2` in `dect_nwk.h`, derived from the standard long before the
room named it, so the two agree.

**Implemented** as a generic close (`queue_release_other()`): any CC
transaction we have no state for is released with reason `0x01`, in the
peer's transaction with the flag inverted, without touching our own call
state. Two tests cover it, including that a live call survives the close.

**The result is real progress and not a fix.**

| | before | after |
|---|---|---|
| `{CC-RELEASE}` sent | 0 | **20** (`e3 4d e2 01`) |
| link released | **never** | **19 times** |
| `ring: link released -- paging now` | never seen | **first time** |
| `{CC-FACILITY}` received | 12 | **20** |
| handset alerted | no | no |

`[observed]` The release works — the link goes quiescent and the disconnect
completes, which is exactly what `A-0054` said would happen once the link was
quiet. **But the handset immediately re-establishes and re-sends the same
`{CC-FACILITY}`**, 20 times in 60 seconds, so the link cycles
released → up → released instead of staying down.

**The room's falsifier does not fire.** `A-0055` said: if the handset keeps
re-sending `0x62` after a `{CC-RELEASE}`, the message is CISS (PD `0x2`), not
CC, and should be closed with `{CISS-RELEASE-COM}`. Every one of the 20
retransmits carries **PD `0x3` (CC)**, TI 6, type `0x62` — unchanged. It is
CC. Also notable: **the handset never sends `{CC-RELEASE-COM}`**; it drops the
link and reconnects instead.

**Our reading:** the handset has something it genuinely wants delivered and
will retry until it gets a *proprietary* answer, not a release. Closing the
transaction is strictly better than silence — the link can now go down at all
— but it converts a stuck link into a cycling one, and a page still has to win
a race against the next re-establishment.

**The question we think matters now:** *why is the handset asking at all?*
Stock answers through a userspace app, so the C4 presumably only raises this
facility because the base advertises something that invites it. If the
invitation is in our beacon capability bits or in an IE we emit during
registration, **not advertising it** would stop the question being asked —
which is a far better fix than answering a proprietary protocol we cannot
see. Asked.

### The AVM capability audit: we advertise nothing, and the C4 probes anyway
`[2026-10-23, A-0056 / A-0057 / A-0058]`

`[CR A-0058]` answered "why is the handset asking at all?": stock advertises an
AVM-proprietary capability in **both** its `{ACCESS-RIGHTS-ACCEPT}` and its
`{LOCATE-ACCEPT}` — IE identifier **`0x78`**, content `0B 74 01`, where
`0x0B74` is AVM's manufacturer code. It is a mutual "I speak AVM" handshake,
emitted only for handsets stored as AVM-family devices, and an AVM handset
that sees it raises `{CC-FACILITY}` to use FRITZ features. The clean fix would
be to stop advertising it.

**We never advertised it.** Two checks, both negative:

- `DECT_IE_MODEL_IDENTIFIER` (`0x78`) is **defined in `dect_nwk.h` and used
  nowhere** in the source.
- **Zero** IE-`0x78` bytes on the wire. Our `{LOCATE-ACCEPT}` is
  `85 55 05 05 a0 94 …`; no `0x78`, no `0b 74` anywhere in any run.

`A-0058` pre-committed this case: *"if you already advertise none of that and
the C4 still probes, it's probing UNCONDITIONALLY, and not-advertising can't
stop it."* **That is our situation.** The C4 raises `{CC-FACILITY}` at a base
that has made no AVM claim of any kind, so the decline path is closed before
we try it. Path (1) of three is eliminated, by two greps and no bench time.

What remains: **(2)** win the race by paging on the `link released` edge —
available today, and our fourth `vF423` run already pages there, so the race
is real but currently lost; **(3)** a standard ASN.1 ROSE Reject echoing the
invoke-id, which `A-0058` notes is the standard decline but cannot confirm the
C4 accepts, because stock answers from a userspace app and there is no
in-kernel reject to read. Whether the C4 takes a Reject is a genuine capture
question for Saturday.

Also settled, and worth recording:

- **The A540 is not special-cased.** `[CR A-0056]`: stock's FP-call rejects
  are codec-driven (reason `0x05` for an incompatible codec list), never
  manufacturer-driven; the EMC is read in the call path only to forward a
  manufacturer-specific IWU payload and never gates admission; the
  Siemens/Gigaset capability adjustment is in **registration**, not call
  setup. So `0x32` is the **A540's own verdict**, consistent with our
  bearer-less reframe. Retake the corpus after the link fix, with a
  compatible codec offer.
- **Ringing without a call is possible**: chip mail **`0x0304`**, an alert
  broadcast from the FpUI task, rings handsets with no bearer and no call.
  This is the no-human liveness check we have wanted all week — *is the base
  alive and the handset reachable* — though it does **not** exercise the audio
  path.
- **Audio without a call is not.** The configurator runs only on bearer event
  `0x70`, so a traffic bearer is required. The one no-human audio source is a
  **handset-originated** bearer, which also fires `0x70` and is feedable. And
  there is **no usable chip tone generator** in this build, so we cannot
  synthesise a liveness tone in the chip.

#### The `0x0304` probe: two false starts, now correctly staged `[vF424]`

Worth recording because both failures were ours, not the chip's:

1. First run sent the three candidate payloads at **t = 28–36 ms** — before
   the handset could lock.
2. Adding `-a xmailms=` to delay them exposed a second ordering bug: the
   stored-subscription reload is gated on *all extra mails having been sent*,
   so delaying the mails delayed the subscription too and the alert still went
   out before the chip knew the handset. (A code comment claiming `0x0247` /
   `0x020a` "bring the radio up" was also wrong — they are the subscription
   reload. Corrected.)

Third run is correctly staged: subscription reloaded at **t = 27 ms**, the
three `0x0304` candidates at **t = 25.0 s**, radio up, handset known.
**Whether the handset chirped is a question only a human in the room can
answer**, and that is the one thing this test cannot do for itself.

### The spec-conformant close changes nothing — and that closes the thread
`[vF423, 2026-10-23 13:58]`

`[CR A-0060]` conceded the `{CC-RELEASE}` / `{CC-RELEASE-COM}` point and
explained its own slip: stock emits **both** forms by context, and
spec-conformantly — `{CC-RELEASE}` for `{CC-SERVICE-CHANGE}`, which is the one
case EN 300 175-5 §17.3.2.1 **excludes**, and `{CC-RELEASE-COM}` for the
general unknown-transaction case. `A-0055` had taken its template from the
excluded handler. It also corrected the reason code: `0x01` is "unexpected
message"; the one the standard names here is `0x02`.

**We verified the reason table ourselves before implementing**, per the new
rule: EN 300 175-5 V2.9.1 gives `0x01` Unexpected Message, `0x02` Unknown
Transaction Identifier, `0x05` Incompatible service. All three of the room's
values check out.

Implemented, tested, deployed, run. **No change whatsoever:**

| | `{CC-RELEASE}` + `0x01` | `{CC-RELEASE-COM}` + `0x02` |
|---|---|---|
| closes sent | 20 (`e3 4d e2 01`) | 20 (`e3 5a e2 02`) |
| `{CC-FACILITY}` received | 20 | 20 |
| link releases | 19 | 19 |
| reconnects | 20 | 20 |

`[observed]` A fully spec-conformant decline does not stop the C4 either. It
probes unconditionally, as `A-0058` predicted, and only a proprietary reply
would satisfy it.

**And under the scope rule written today (`AGENTS.md` §1a), that is where the
thread ends for us.** We decline correctly per the standard, and we stop. The
facility exchange is AVM-proprietary; it belongs in a device profile later, if
ever, and not in `libopendectcell`. The ROSE-reject experiment and the Saturday
capture of a FRITZ!Box's facility reply are **parked**.

What this leaves as the real blocker is a chip question, not a protocol one:
**the standard permits an FP to set up a call on an existing link, and we
cannot, because we cannot get a traffic bearer allocated on one.** That is
`Q-0046` §1.1, and it is now the top ask.

### Task 6 is not on the chip: `0xf839` is unreachable `[vF425, 2026-10-23 14:09]`

`[CR A-0061]` answered the blocker: **there is no "allocate a traffic bearer"
chip mail.** In DECT the PP sets the bearer up and the FP only triggers it;
for an idle-locked PP the only trigger is the page. But stock *does* place a
call on an existing link without paging, by a different route: on the
existing-link branch it sends `{CC-ALERTING}` + `{CC-CONNECT}` and then
`a stock mail routine(6, 0xf839, link_number)`, which connects the audio switch onto
the bearer the link **already has** — no fresh allocation, so no `0x20`/`0x70`.

The answer described task 6 as "host-side (the vendor's PCM driver)" while also
listing 6 among the chip mail destinations, so we tested which it is.

`[observed]` Three `0xf839` mails to **dst 06**, each came straight back:

```
[25003] tx M src 22 dst 06 prim f839 len 1 00
[25027] rx   src 22 dst 06 prim f839 len 1 00      <- same src AND dst: our own mail
```

**The control for it:** our `0x0304` mails to **dst 02** in `vF424` were
**never** echoed (0 of 3), and no other run in the whole corpus contains a
single `rx` with `src 22`. So the echo is specific to destination 6: the
chip's mail router has nothing at task 6 and bounces what it cannot deliver.

**Conclusion: task 6 is AVM host software, not a chip task, and `0xf839` is
not a mail we can send.** `A-0061`'s own description was right.

**What that means for us is better than it sounds.** If `0xf839` only
manipulates host-side switch state and confirms host-side (`0xf83b`), then
**we are the host** — the equivalent is something we do in our own code, not
a mail we lack. The chip-facing part of an existing-link call would then be
just the NWK messages we already build plus our known audio mails
(`0x0251`, `0x0230`) bound to the existing link's handle.

**Untested, and the run that would have tested it was confounded:** no
handset was attached during `vF425` (`connection up` count 0), so there was
no existing link to bind to. The experiment to run is: on a held link, send
`0x0251` + `0x0230` against that link's handle with no page and no `0x022D`
timer, and read `enable` / `dest 6`. `A-0061`'s caveat applies — it can only
work if the held link is a traffic-capable bearer rather than a
signalling-only association, and that is precisely what the test decides.

### The A540 did not attach — and we accept location registration from a handset we did not load `[vF426, 2026-10-23 14:19]`

The maintainer brought the A540 online and we ran with **its** subscription
loaded (`-s /etc/opendectcell-sub.a540`, `sub: restored subscription for slot 1`).

**The A540 never appeared. The C4 did.** One portable identity in the whole
90-second run, and it matches the stored C4 record byte for byte:

```
seen on the air        80 a8 02 de ad 00 02
/etc/opendectcell-sub.c4    80 a8 02 de ad 00 02     <- match
/etc/opendectcell-sub.a540  80 a8 02 de ad 00 01     <- the record we loaded
```

Confirmed independently by the maintainer. **So `.a540` and `.c4` really do
hold different handsets** — the `C1` identity collision is not present in
these two files — and the A540 simply did not try. The likely reasons are that
it is no longer subscribed to our base or is out of range; it needs
re-enrolling in registration mode to find out.

#### The defect this exposed

**We loaded the A540's subscription and then accepted a location registration
from the C4.** Our MM answers `{LOCATE-REQUEST}` with `{LOCATE-ACCEPT}` without
checking the requesting handset's identity against the subscription we hold.
Thirty connections came up in ninety seconds on a record that was not the
handset's.

That is wrong under the standard, not merely untidy: a GAP FP accepts location
registration from **subscribed** handsets. Ours would accept any handset locked
to our RFPI, which is a security property as much as a correctness one. It also
explains why handset identity has been confusing all week — the store told us
one thing and the air another, and nothing checked.

**Fix:** `on_locate_request()` must compare the `<<PORTABLE-IDENTITY>>` against
the stored subscription and reply `{LOCATE-REJECT}` (MM type `0x57`) when it
does not match. Filed as a task; it is squarely inside the new scope rule —
this is the standard, not the chip.

`[observed]` Also recorded, because it settles a question we asked the clean
room: the C4's `{LOCATE-REQUEST}` carries **`<<MODEL-IDENTIFIER>>` (IE `0x78`)
= `0b 74 08`** — AVM's manufacturer code, announced by the **handset**. So the
C4 declares itself an AVM device to any base, which is consistent with
`[CR A-0058]`'s conclusion that its `{CC-FACILITY}` probe is unconditional and
is not something our base invites.

### The held link is signalling-only: `0x0242` reports a channel of zero
`[vF426 vs vF411, 2026-10-23, no new bench time]`

`[CR A-0062]` gave the go/no-go for binding audio onto an existing link:
*"instrument `0x0242` on the held link — if you never see a channel-assigned
report for it, it's signalling-only and no audio mail will bind; if you do,
`0x0230` binds."* That test was answerable from logs we already had.

**We do see `0x0242` on a held link — thirty of them in ninety seconds, with
no call at all. But the channel they report is zero.**

| run | state | `0x0242` payload | count |
|---|---|---|---|
| `vF426` | held link, **no call** | `01 00 00 00` | 29 |
| `vF426` | " | `00 00 00 00` | 1 |
| `vF411` | **call with working audio** | `01 40 00 00` | 6 |
| `vF411` | " | `01 01 02 00` | 4 |
| `vF411` | " | `01 41 02 00` | 1 |

The payload is `{handle, chan_flags, mask…}`. On a held link `chan_flags` is
**`0x00`**; on a call that carried audio it is `0x40`, `0x01` or `0x41`, and
the mask becomes non-zero too.

**So the criterion is not the presence of the report, it is the channel field
in it.** A signalling-only bearer still produces `0x0242` — it just announces
no channel. Refinement sent back to the room, since their phrasing would have
had us read these thirty reports as a green light.

**Conclusion: the held link is signalling-only and no audio mail can bind to
it.** `0x0251`/`0x0230` by that handle would be writing to a channel that does
not exist, which is the same class of mistake as the lingering-link re-assigns
that cost us four days. We are not going to run it.

**Which settles the existing-link question in the negative, for this handset
in this state:** release-then-page remains the only route to audio, and
`[CR A-0061]`'s existing-link path depends on the link already having a
traffic-capable bearer — one the C4 does not give us by merely attaching.

`[method]` Worth noting what this cost: nothing. Two greps over logs already
on disk answered a question we were about to spend an attended call on. That
is the third time this week that tabulating the existing corpus beat running a
new experiment.

`[open]` `0x0243` (`src 26 dst 03`) arrives constantly — 145 times as
`01 29 01`, 29 as `01 29 00`, 13 as `00 29 01` in one 90-second run — and we
have no idea what it is. Asked the room; it is the most frequent in-mail we
ignore.

### `[CR A-0065]`: the authoritative chip command set, and what `0x022B` really is

Part 3 of the task-2 sweep went into the **chip** firmware rather than the
host module, and it changes two things we believed.

**`0x022B = 0x41` is not a capability bitfield.** The chip tests the payload
byte for **exactly `0x41`**, rejects anything else, and sets a "configured"
flag. It is an **enable token**. There are no GAP or CAT-iq bits in it.

Two consequences, one reassuring and one not:

- **We have no capability-mismatch risk from it.** We are not claiming
  anything we do not implement, which was the worry behind `Q-0048`. The chip
  broadcasts its **built-in default FP capabilities**, a valid GAP
  announcement — which is why handsets lock to us at all.
- **We also cannot announce anything.** Real capability data would be the
  bytes *after* `0x41`, into a chip config buffer the MAC bearer handler
  consumes. **We send none.** So if CAT-iq support has to be announced, we are
  not announcing it, and that is now the live question (`Q-0048` proper, the
  room's part 4).

**The other bring-up mails, decoded:** `0x015B` is radio on (`01`) / park
(`00`), not capability. `0x0247` is `{slot, op (0 clear / 1 set), flag}`.
`0x020A` is `{index, 16-byte UAK, config flag}` — and the trailing byte is a
per-handset config flag stored separately from the key, so it is safe to vary.

**`0x0243`, our most frequent unknown in-mail, is a bearer-status heartbeat**
from the MAC bearer handler to LCE: `{handle, 0x29 fixed tag, status}`. Safe
to ignore for audio. 288 of them in a 150-second run and they were telling us
nothing we needed.

**Both of our bench corrections are confirmed against the chip firmware:**

- `0x0202` is an **immediate** connection teardown with a single connection-id
  parameter and **no mode or reason field**. Our ~40 ms abrupt drop was right,
  and there is no orderly/abrupt flag we could have been getting wrong:
  orderliness is entirely "only send it once the DLC is quiescent".
- `0x0242`'s go/no-go is the **channel field**, not the report's existence,
  exactly as we found. Byte[1] is the assigned channel; **bind audio only when
  `(chan & 0x0F) != 0`**, low nibble = channel 1..6, and the `0x40` bit is a
  transient setup/direction flag. A signalling-only bearer still reports with
  channel 0.

Also confirmed: `0x0204` is **not** a chip command at all — `0x0244` is the
C-plane path, which is what we already use.

### The A540 is not reaching us at all `[vF426, 150 s]`

With the A540's subscription loaded and the handset powered on, **one portable
identity appeared in 150 seconds and it was the C4's**. 51 bearer setups, 848
C-plane segments, 288 heartbeats — every one of them the C4's facility loop.

**The A540 is not transmitting to us.** This is not a refusal we can see, a
failed authentication or a rejected link; there is simply nothing. Since the
chip demonstrably does not gate link establishment on the stored subscription
(the C4 attached while the A540's record was loaded), the A540 ought to be
able to reach us whatever our store holds.

That points away from our base and at the handset's own base selection: a
Gigaset holds several base registrations and locks to the selected one. The
a FRITZ!Box is in range and is the likely incumbent. Until the handset
is told to use our base — or re-registered to it — there is nothing on our
side to test.

### The chip keeps subscriptions across runs — and we were admitting a handset we had no record of
`[vF426 vs vF427, 2026-10-23]`

The maintainer put it exactly right: *"there is definitely an error, since the
C4 shouldn't be able to attach when the A540 data is loaded."*

The stores are not mislabelled — we checked the identity bytes:

| file | IPUI prefix | handset |
|---|---|---|
| `/etc/opendectcell-sub` (default) | `80 a8 02 58 f4 …` | C4 |
| `/etc/opendectcell-sub.c4` | `80 a8 02 58 f4 …` | C4 |
| `/etc/opendectcell-sub.a540` | `80 a8 02 b6 de …` | **A540** |
| `/etc/opendectcell-sub.bak-c1` | `80 a8 02 b6 de …` | A540 |

So we really did load the A540's record, and the C4 really did attach anyway.

**The cause: the chip's subscription table is its own and it survives our
daemon.** We write exactly one slot (`DECT_MM_REG_SLOT` = 1) and never touch
the others, so a handset enrolled in an earlier session stays admissible for
ever. A clean A/B, same subscription file in both:

| run | slot 1 | slots 2–6 | connections | C-plane |
|---|---|---|---|---|
| `vF426` | A540 | **untouched** | **51, all C4** | 848 segments |
| `vF427` | A540 | **cleared** | **0** | **0** |

Clearing slots 2–6 stopped the C4 dead. That is the mechanism, confirmed.

**Why it matters beyond this experiment.** A GAP FP serves **subscribed**
handsets. Ours was admitting one it had no record of — a correctness and
security defect, and the reason every identity question this week was
ambiguous: the store said one thing and the air said another, and nothing
reconciled them. It is very likely the hidden variable behind some of the
"which handset was that?" confusion in the older captures.

**Fixed:** `clear_foreign_slots()` now clears every slot but ours at bring-up,
using `0x0247 {slot, op 0, flag}` per `[CR A-0065]`. This pairs with the
`{LOCATE-REJECT}` identity check still to be written — the chip gate stops a
link, the MM gate stops a location registration, and a base should have both.

**The A540 is still silent.** With its record in slot 1 and no competing
slots, 180 seconds produced **no connections and no C-plane traffic at all**.
The maintainer reports the handset was reset and still holds this base, and
attaches in about 60 s when it does. It is not locking to our beacon, which is
a layer below subscription — so the next step is a fresh registration rather
than more store surgery.

## WIDEBAND: mode 8 reached for the first time, and it does not read our lane
`[vF418, 2026-10-23 14:45, attended, earpiece recorded]`

`[CR A-0068]` resolved the codec-byte conflict from the chip firmware: the
chip compares `0x0230[7]` against **1** with no masking, so a bare `0x01` is
the byte for mode 8, and `A-0059`'s packed `0x41` is a **host-internal** form
that never reaches the wire. We sent `0x01`.

**It worked. Window 1 is the first time we have ever confirmed mode 8:**

| window | assign | `dest 6` | `enable` | `dir0` | `dir1` | earpiece |
|---|---|---|---|---|---|---|
| 0 bind | (default, remode to mode 2) | `085d` | `0001`/`0003` | `0000`/`0002` | `0000` | 40.9 dB |
| **1 mode 8, one octet/frame** | `41:01` | **`040e`** | **`0000`** | **`0000`** | **`8001`** | **60.1 dB** |
| 2 mode 8, octet high | `41:01` | `0410` | `0000` | `0000` | `0000` | 14.3 dB |
| 3 mode 8, swap | `41:01` | `0410` | `0000` | `0000` | `0000` | 11.5 dB |
| 4 mode 8, held | `41:01` | `0410` | `0000` | `0000` | `0000` | 11.9 dB |
| 5 mode 8, two/frame | `41:01` | `0410` | `0000` | `0000` | `0000` | 11.1 dB |
| 6 mode 9 | `41:03` | `0410` | `0000` | `0000` | `0000` | 15.6 dB |
| 7 mode 9 held | `41:03` | `0410` | `0000` | `0000` | `0000` | 12.1 dB |
| 8 mode 3 | `41:05` | `0410` | `0000` | `0000` | `0000` | 16.3 dB |

Window 1 matches `[CR A-0043]`'s mode-8 signature exactly — `dest 6 = 0x040E`,
`enable = 0`, `dir0 = 0`, **`dir1 = 0x8001`, the coded block armed**.

### Two findings, and the second spoils the run

**1. In mode 8 the handset hears loud noise that has nothing to do with our
lane.** Window 1 is **60.1 dB**, the loudest window by 20 dB — the maintainer
reported *"no HD Voice sign, no speech, just noise on one window"*, and this is
that window. But its envelope **range is 6.1 dB** and its correlation against
the source envelope is **−0.237**.

Speech has ~50 dB of envelope range. 6 dB of range with no correlation is the
signature we have seen before for *the chip is not reading our bytes* — not
for "reading them and decoding them wrongly". So with the coded block armed
and one octet per frame, **the ring is not being fed from our lane.**

That falsifies the rate-matched packing that both we and `[CR A-0047]` had as
the leading candidate. It does **not** yet falsify wideband, because:

**2. The configuration collapsed after one window and never came back.**
Windows 2–8 all read `dest 6 = 0x0410`, `enable = 0`, both direction words
zero — the unconfigured/park state — and are 11–16 dB, i.e. silent. Five
further `0x0230` re-assigns, three of them byte-identical to the one that
worked in window 1, changed nothing.

**So only one of the nine windows tested anything.** The other three mode-8
packings, both mode-9 windows and the mode-3 check were all run against a dead
channel and say nothing at all.

### What this means for the method

**The matrix approach does not work for mode 8.** Mode 8 sets `enable = 0` by
design, and something then tears the channel down within ten seconds; once
down, no re-assign recovers it — the same "cannot reconfigure once lost"
behaviour we saw with the lingering link. Each remaining packing needs its own
call, or we need to find what collapses the state and prevent it.

Pre-committed for the next attempt: if a fresh call in mode 8 with the
**high-byte** packing again gives loud uncorrelated noise, and the same for the
swap and held packings, then the coded ring is air-fed only and wideband
downlink closes on this chip — which `[CR A-0043]` §4 has already committed to
confirming from the firmware if we report the null.

### CORRECTION: clearing the chip's slots does not gate attachment `[vF418]`

An hour ago this file said, of clearing subscription slots 2–6:
*"Clearing slots 2–6 stopped the C4 dead. That is the mechanism, confirmed."*

**It is not confirmed, and `vF418` falsifies it.** That run loaded the **C4's**
record into slot 1 and cleared slots 2–6 with the new `clear_foreign_slots()`
(the log shows it ran), and **both** handsets appeared on the air:

```
nwk in <-- 05 54 05 07 80 a8 02 de ad 00 02     the C4   (slot 1's record)
nwk in <-- 05 54 05 07 80 a8 02 de ad 00 01     the A540 (no record loaded)
```

So a handset with **no subscription record in the chip at all** attached and
completed a location registration. Clearing the slots does not prevent it.

**What the evidence actually supports, with the A/B now at three runs:**

| run | slot 1 | slots 2–6 | attached |
|---|---|---|---|
| `vF426` | A540 | untouched | C4 only |
| `vF427` | A540 | cleared | **nobody** |
| `vF418` | C4 | cleared | **C4 and A540** |

`vF427`'s zero is the odd one out and is **unexplained**. It is not the
clearing, because `vF418` cleared too and got both handsets. The likeliest
reading is that nothing happened to try during those 180 seconds — which,
given the A540 has now been seen attaching spontaneously, is entirely
plausible and means that run never had the discriminating power I gave it.

**The lesson is the one already in our rules and I broke it anyway:** I
declared a mechanism confirmed from a single pair of runs, and committed a fix
on the strength of it. One A and one B is not a controlled result when the
thing being measured is intermittent, and handset attachment plainly is.

**What stands:** `clear_foreign_slots()` stays — not clearing subscriptions we
do not own is right regardless, and `[CR A-0065]`'s decode of `0x0247` is
sound. **What falls:** the claim that it is the gate. It is not.

**The real gate is the one we still do not have.** The chip admits any handset
locked to our RFPI, and our MM then answers `{LOCATE-REQUEST}` without
checking the identity against our store. **That** is the defect, it is
host-side and ours, and the fix is the `{LOCATE-REJECT}` check already filed.

`[good news]` The A540 **is** reachable. It attached spontaneously during
`vF418` and the maintainer heard it make noises, so the handset is alive, in
range, and locked to this base. The earlier silence was not a dead handset.

### The 14:50 recording: a call in mode 2 fed the wrong lane, and a flapping enable
`[vF418 tail, A540 recording 14:50:07, 51.2 s]`

The maintainer heard the A540 "randomly connect and make strange noises", then
"some speech, but garbled", and recorded it. What the run shows:

- **A second call bound at t = 233 s on handle 0**, and unlike the held links
  it got a **real channel**: `0x0242` reports `00 41 02 00`, then `00 01 02 00`
  — low nibble 1, mask `0x0002`. By `[CR A-0065]`'s gate (`chan & 0x0F != 0`)
  that bearer is traffic-capable.
- **Mode 2 was reached on it** — `enable = 0x0001` through much of t = 230–290 s
  — so our lane was being read into the narrowband chain.
- **But `enable` flaps**: `0001` and `0000` alternate across every 10-second
  bucket, and `dir1` flickers `0000`/`8001`/`8000`.
- The call was **handset-originated**: three `{CC-SETUP}` (PD 3, type `0x05`)
  arrive from the handset side during the run; we never rang anything after the
  first call.
- **The A540's location registration is at t = 264 s** — *after* that call
  bound. So the 233 s call was very likely the C4's, not the A540's.

**What the audio says.** 51.2 s, level 30.1 dB, envelope range **60.5 dB** —
speech-like dynamics, not noise. But the decisive test fails: our lane has a
**0.5 s silent gap every 10 s** at each window boundary, and the recording has
**no 10-second cadence**. Its quiet stretches are at 4.3, 9.4, 21.0 and 27.7 s
— gaps of 5.1, 11.6 and 6.7 s. Best envelope correlation against the lane read
as linear is **0.490**, at a lane offset inconsistent with the predicted one.

**So: consistent with, but not proof of, our audio.** A call in mode 2 being
fed the *wideband* rig's G.722-octet lane would sound exactly like garbled
speech — mode 2 reads linear, and G.722 code octets read as linear are noise
carrying the speech's envelope — and a flapping `enable` would chop it
irregularly, which is why no clean 10 s structure survives. That is a coherent
story and the numbers fit it, but the structural evidence that would settle it
is absent, so it stays a hypothesis.

**What this does establish, regardless of whose earpiece it was:**

1. A handset-originated call **does** get a traffic-capable bearer and reaches
   mode 2 without any of our paging machinery. That is the no-human audio path
   `[CR A-0056]` predicted, and it works.
2. `enable` is **not stable** across a call in this state. Every audio result
   we have taken over a long call needs re-reading with that in mind.

**The test that settles it, and it is cheap:** place a handset-originated call
from the A540 with `linlane.raw` — the **known-good linear lane** — loaded
instead of the wideband rig. If the A540's earpiece plays clean speech, the
A540 has working downlink audio, the "A540 refuses calls" defect was about
FP-originated calls only, and the bearer-less reframe is confirmed from the
other side. One call, no paging, and the maintainer only has to dial.

## THE MODE-8 WINDOW HAD THE AUDIO PATH SWITCHED OFF `[corpus re-read, 2026-10-23 night]`

No radio for this. It is a re-read of every log we already hold, prompted by
the §4 note *"`enable` flaps across a long call"*. That note is wrong, and
chasing down why it is wrong undoes a headline.

### 1. `enable` does not flap. It has eleven clean edges in 300 s

`vF418`, 238 samples of `0x10802` at 250 ms:

| t (s) | `enable` |
|---|---|
| 0.55 | `0000` |
| 50.95 → 52.21 | `0003` → `0001` |
| 61.03 | `0000` |
| 233.65 | `0001` |
| 244.99 | `0000` |
| 248.77 → 250.03 | `0003` → `0001` |
| 263.89 | `0000` |
| 272.71 | `0001` |
| 282.79 | `0000` |

Three audio episodes of 11.3 s, 13.9 s and 10.1 s, each with a clean arm and a
clean disarm — the handset's own short service calls. The earlier reading came
from 10-second buckets, which contain both values at an edge and so report
"alternating" for what is in fact one transition. **Bucketed summaries cannot
see edges; go back to the samples before calling something unstable.**

### 2. Every edge is explained by a mail we sent, and `codec = 1` is the one that switches audio OFF

Each `0x0230` in the corpus, against `0x10802` before and over the next 3 s:

| run | t | handle | conn up? | flags | codec | enable before → after |
|---|---|---|---|---|---|---|
| `vF413` | 49.36 | 01 | yes | `41` | **3** | 0 → **3** |
| `vF413` | 57.36 | 01 | yes | `41` | **1** | 3 → **0** |
| `vF413` | 73.36 | 01 | yes | `01` | **3** | 0 → **1** |
| `vF413` | 97.36 | 01 | yes | `41` | **3** | 1 → **3** |
| `vF418` | 50.81 | 01 | yes | `41` | **3** | 0 → **3** → 1 |
| `vF418` | 60.81 | 01 | yes | `41` | **1** | 1 → **0** |
| `vF418` | 232.49 | 00 | yes | `41` | **3** | 0 → **1** |
| `vF418` | 248.69 | 00 | yes | `41` | **3** | 0 → **3** → 1 |
| `vF418` | 272.05 | 01 | yes | `41` | **3** | 0 → **1** |
| `vF418` | 282.05 | 01 | yes | `41` | **1** | 1 → **0** |

**Correctly addressed, with audio already armed, `codec = 1` took `enable` to
zero in 3 of 3 cases.** `codec = 3` armed it in 6 of 6. Two further `codec = 1`
sends went to handle 01 while the call was on handle 00 — the chip answered
`0242 01 0f 00 00` both times and `enable` fell once and held once, so the
mis-addressed pair is not evidence either way. (That mis-addressing is a rig
bug of ours: the periodic re-assert uses a fixed handle, not the active one.)

Every remaining `codec = 1` send landed with `enable` already 0 and changed
nothing.

### 3. The mode-8 signature and a live audio path have never co-occurred

Cross-tabulating `dest 6` against `enable` over every sample in every log:

| `dest 6` | `enable` | samples |
|---|---|---|
| `0410` (idle) | `0000` | 1960 |
| `085d` (narrowband call) | `0001` | **221** |
| `040e` (read as mode 8) | `0000` | **42** |
| `085d` | `0003` | 33 |
| `040e` | `0003` / `0001` | 1 / 1 |

The two non-zero `040e` samples are single, and `dest 6` and `enable` are
separate reads ~250 ms apart, so an edge produces exactly one mismatched pair.

**So in 42 of 44 samples where the chip was in the configuration we have been
calling mode 8, the audio-enable global read zero.** `vF418`'s one "wideband
window" is the clearest case: `enable` went to 0 at t = 61.0, `dest 6` became
`040e` at t = 61.8, and it decayed back to the idle `0410` by t = 71.9. One
window, ten seconds, path off throughout.

### CORRECTION, one hour later: §4 as first written was wrong twice

I wrote that the mode-8 state *"decayed by itself ten seconds later with no
host mail in between"* and that the chip then *"stopped responding to the
command"*. Both are false, and interleaving the mails with the `dest 6`
samples — which is what I should have printed in the first place — shows it:

```
vF413                                   vF418
49.4  >>> 0x0230 codec=03               50.8  >>> 0x0230 codec=03
49.6  dest6 -> 085d                     51.3  >>> 0x0230 codec=03
57.4  >>> 0x0230 codec=01               51.7  dest6 -> 085d
57.6  dest6 -> 040e                     60.8  >>> 0x0230 codec=01
65.4  >>> 0x0230 codec=01  (no change)  61.8  dest6 -> 040e
73.4  >>> 0x0230 codec=03               70.8  >>> 0x0230 codec=01
73.6  dest6 -> 085d                     71.9  dest6 -> 0410
                                        80.8  >>> 0x0230 codec=01  (no change)
                                        90.8  >>> 0x0230 codec=01  (no change)
                                       100.8  >>> 0x0230 codec=01  (no change)
                                       110.8  >>> 0x0230 codec=03  (no change)
                                       120.8  >>> 0x0230 codec=03  (no change)
                                       130.8  >>> 0x0230 codec=05  (no change)
```

- **It did not decay by itself.** There is a mail at 70.8 in the gap. In
  `vF413` the same state held **16 seconds** across a second `codec = 1` that
  changed nothing, and reversed only when we asked for `codec = 3`.
- **The chip did not stop responding.** It answered **every one** of those
  six ignored sends with `0242 01 41 02 00` — channel `0x41`, low nibble 1,
  byte-identical to the answer it gave at 60.8 when the mail *did* configure
  the matrix.

**What is actually there is better than what I claimed.** The same mail, to
the same handle, acknowledged with the same `0x0242`, configured the matrix
once and did nothing the next six times. The acknowledgement carries no
information about whether anything happened. That is the "same settings,
different result" we have been chasing all week, cornered in one run.

It also means **the hold on F4.30–F4.33 does not rest on "the first window is
all you get"** — in `vF413` the command kept working. It rests on this: we do
not know what gates the downlink, we have never confirmed audio in mode 8, and
`0x0242` cannot tell us whether a send landed. Four three-minute calls is a
poor way to find that out; `F4.34` is a two-minute way.

### 4. `0x040e` is not a mode-8 marker — two table entries hold it permanently

Printing all 16 words of the matrix table at `0x1168A` on every change:

```
t          d0   d1   d2   d3   d4   d5   d6   d7  ...
 0.05    0991 0bc5 0991 0991 040e 040e 0410 0991 ...   (idle, no radio)
51.71    ...                      040e 040e 085d ...   (narrowband call)
61.79    ...                      040e 040e 040e ...   ("mode 8")
71.87    ...                      040e 040e 0410 ...   (decayed)
```

**`dest 6` is the only word that ever moves**, in `vF413` and `vF418` alike.
And `0x040e` is what `d4` and `d5` hold from t = 0, with no radio up and no
call — it is not a value the wideband configuration produced, it is a value
two other entries hold at rest.

So "`dest 6` = `040e` ⇒ the chip is in mode 8" rests on `dest 6` matching the
resting value of its neighbours. That may still be right — if `d4`/`d5` are
the long-slot lanes, they would sit at that destination idle — but it is a far
weaker observation than it was recorded as, and it arrived together with an
audio path that was switched off.

### What this does to the wideband thread

**`./f4.sh 30`–`33` as built would have tested four lane packings against a
disabled audio path, four times, with the maintainer listening.** Each sends
`seqstep=41:01` five times at 10 s — the mail that in 3 of 3 clean cases took
`enable` to zero. The lane would have been fed into a chain that is not
reading it, exactly as in `vF418`.

**Both readings are still open and only one is bad news:**

- **(a)** `codec = 1` switches the audio path off on this STUB. Wideband
  downlink is then not reachable this way at all, and the four packings are
  irrelevant.
- **(b)** `0x10802` gates the narrowband chain only, and the coded-block path
  has its own gate we have never watched. Then the path may well have been
  live and the packings are still the right question.

Nothing in the logs separates them: the whole rest of the matrix is constant,
so there is no second gate visible at any address we dump. That makes it a
chip question, and it is asked as `Q-0050`.

**Held, not run:** the four packing calls are off the list until the gate
question has an answer. What replaces them is one call, in §F4.34 — arm
narrowband, confirm by ear that something is playing, then send `codec = 1`
once and listen for whether it stops. That is the discriminator, it costs one
minute of a maintainer's attention instead of twelve, and its outcome is
audible rather than inferred.

## THE CALLS WE HAVE BEEN MEASURING WERE PHONEBOOK READS `[2026-10-23 night]`

Still no radio. This came out of reading `{CC-SETUP}` bytes we had been
logging for weeks without decoding the one element that says what the call is.

### `<<BASIC-SERVICE>>`, read out of the spec rather than taken on trust

EN 300 175-5 §7.7.5, figure 18 — high nibble call class, low nibble basic
service:

| call class | | basic service | |
|---|---|---|---|
| `0010` | **LiA service setup** (list access) | `0000` | basic speech |
| `0011` | ULE service call | `1000` | **wideband speech** |
| `0100` | **Message call setup** | `1001`/`1010` | light data (SUOTA) |
| `0111` | DECT/ISDN IIP | `1011` | DTAM wideband speech |
| `1000` | **Normal call setup** | `1111` | other |
| `1001` | Internal call | | |
| `1010` | Emergency call | | |
| `1011` | Service call | | |
| `1100` | **External handover** | | |
| `1101` | Supplementary service | | |
| `1110` | OA&M | | |

**`[CR A-0069]` had the class numbering wrong, and we adopted it.** It read
`0x88` as "call class 8 = external handover" and prescribed `0x40` as "call
class 4 = normal call". The table says class 8 **is** normal call setup,
external handover is `1100` = `0xC0`, and `0x40` is **message call setup** —
so this afternoon's fix replaced a correct byte with a worse one. We now send
`0x80` (normal call, basic speech). The nibble split A-0069 gave was right;
the class names were not. None of the three values changes the A540 refusal —
`0x88`, `0x80` and `0x40` all drew `0x32`.

### What the handsets actually send

Every handset-originated `{CC-SETUP}` in the corpus, by portable identity and
`<<BASIC-SERVICE>>`:

| identity | `<<BASIC-SERVICE>>` | reading | count |
|---|---|---|---|
| `…de ad 00 01` (A540) | `0x28` | **LiA service call**, wideband speech | **145** |
| `…de ad 00 01` (A540) | `0x88` | normal call, wideband speech | 2 |
| `…de ad 00 02` (C4) | `0x88` | normal call, wideband speech | 21 |

Two things fall out of that table.

**First: the A540 is wideband-capable.** It asks for service 8 in all 147 of
its setups. "Narrowband-only and strict, so it refuses a wideband offer" was
never the explanation for its `0x32`, which the `0x40` run had already shown
experimentally.

**Second, and this one is ours: 145 of those 147 calls were phonebook reads.**
Call class `0010` is list access (TS 102 527-3). The log confirms it directly
— each one is followed by a `{CC-IWU-INFO}` exchange carrying
`<<IWU-TO-IWU>>` (IE `0x77`), which is the list-access protocol, and there is
no U-plane anywhere in it.

### And we bound the audio path to every one of them

`call_bound` was set whenever CC reached connect, with no look at what kind of
call it was. So each time the A540 read its phonebook we armed the audio
channel, sent `0x0230`, started playing the lane into it, and ran the memory
sweeps.

**This is the context for the whole mode-8 analysis above.** `vF413` and
`vF418` — the two runs the codec/enable table comes from — contain **no voice
calls at all**. Every call in them is `e0 28`. So "what gates the coded-block
audio path" was asked of connections that carried no speech service, and if
the chip declines to configure a coded audio block on a list-access call, that
is a complete and very ordinary explanation for all of it. `Q-0050` has been
corrected to say so; the room was told the same night.

It is also, almost certainly, *"the A540 just randomly connected and is making
strange noises"*: the handset opened a list-access call, we answered it as a
voice call and played the wideband rig's lane into it.

### Fixed

`dect_cc_peer_is_speech()` (`dect_cc.c`), and the bind in `dect-stubd.c` now
asks it. **Both nibbles have to agree** — the service nibble alone is exactly
the trap, because a list-access call carries class `0010` with service `1000`
"wideband speech", so reading the service by itself calls a phonebook read a
wideband voice call. Speech classes are normal, internal, emergency and
external handover; everything else is declined, whatever its service nibble
says.

The call itself is still answered and list access still works — we only
decline to arm audio, and the log says so once per call:
`call: not a speech call (<<BASIC-SERVICE>> 28) -- no audio path bound`.

### What this does to the corpus

**Every audio result taken from a handset-originated call needs its
`<<BASIC-SERVICE>>` checked before it is read as anything.** The 14:50
recording analysis above says the 233 s call "was very likely the C4's" and
treats it as a voice call; it was `e0 28`, a list-access call, on the A540.
The conclusion of that section — that the recording is consistent with but not
proof of our audio — is unchanged, but its reasoning about which handset and
which kind of call was wrong.

The two `e0 88` A540 voice calls are now the most interesting rows in the
corpus: they are the only handset-originated speech calls that handset has
placed to us.

## `[CR A-0075]` AND `[CR A-0076]`: WE HAVE NEVER INSTRUMENTED THE DOWNLINK

Two answers came back the same night, and between them they retire more of our
own work than anything else this week. Read this before trusting any earlier
section that argues from `enable`, `dest 6` or `0x0242`.

### 1. `0x10802` and `dest 6` are the **uplink**

`[CR A-0075]` §1, asked bluntly and answered bluntly: the matrix routes
`dest 6` from the channel's **decoded output** and `0x10802` opens **that
output stage**. Both belong to the **air→host** direction. `[CR A-0044]` said
as much in passing and we did not hear it.

**The host→air path has never appeared in anything we dump.** Every
"configuration" we have measured all week — the working tuple, the idle
baseline that is not zero, the `0x0230` correlations, tonight's 3-of-3 — is
the direction we were *not* trying to fix. It also explains why the idle
baseline is not zero: the uplink stage is wired up whether or not we are
feeding anything the other way.

**What this does not invalidate:** anything measured by ear or by capture.
The uplink *works* and was measured on its own terms. What it invalidates is
every inference of the form "the chip is configured, so our bytes must be
going somewhere".

**We currently have no instrument for the downlink at all.** Asked the room
for one by address; until we have it, no bench run can report anything about
the downlink except through a human ear.

### 2. `0x0242` is "mail processed", not "matrix changed"

`[CR A-0075]` §0: it is emitted on every path including the no-act early-out.
The act/no-act gate is the connection's byte 0/byte 1 plus record `+0x116`
bit 5 — state outside the mail. **That is exactly the "same mail, same
acknowledgement, different effect" we cornered in `vF418` tonight**, and we
have been reading `0x0242` as a landed-confirmation for weeks. We stop.

### 3. The page-info byte we have been computing is never read by the chip

`[CR A-0076]`: `mail[2]` of `0x0300` — our `0x0C`/`0x2C` — is **not
referenced anywhere in the page path**. Bit `0x20` is inert on the air; the
two values are identical to the chip. `0x0300` reads only the TPUI, builds a
fixed **short** page into chip RAM at `0x96ad`, and writes exactly one header
byte at `0x96ad+4`. The LCE header is **compiled in**.

**This retires four of our own runs.** Today's A540 series varied
`<<BASIC-SERVICE>>` across `0x88`, `0x80` and `0x40`, and because
`ring_page_info()` derives the page byte from the service nibble, three of
those runs also changed the page from `0x2C` to `0x0C`. We read them as four
experiments over two variables. They were four runs of the same page.

### 4. There is no path to a long-slot page on this STUB

`[CR A-0076]` §4, which is the answer we asked for plainly and got plainly:
not `0x0300` (short format only), not `0x015E` (its bearer-type field is read
by the MAC config and never reaches the page builder), not `0x015C`
(connection setup). No full-format builder exists.

Put beside `[CR A-0069]` — the chip does not choose the slot type either —
**nothing in the FP ever asks for or selects a long slot.** And a short page
can only ask for a full slot (EN 300 175-5 §8.2.4.1).

`[CR A-0075]` §8 then closes the loop: `codec = 1` is mode 8, mode 8 is the
only long-slot path, and it needs the handset-granted long-slot bit.
**`./f4.sh 30`–`33` are retired, not held.** Four attended calls not spent.

### 5. The one avenue left is a handset asking on its own — and we have one

A PP may request a long slot by itself from a negotiated wideband service
(§8.2.4.2). We cannot force it; a handset can choose it. **The A540 asks for
wideband service in all 147 of its own setups**, which makes the
already-planned A540 dial-out test the wideband test as well, at no extra
cost. One call, the handset dials, we listen and we dump.

### 6. The A540 has a one-dump test now

`[CR A-0076]`: read `0x96ad+4` while a page is in flight and the compiled LCE
header is there to be decoded. Row `000` of table 80b is "MAC service = no
MAC service", which would make the A540's `0x32` **correct behaviour** — and
that is the only hypothesis left standing that fits a wideband-capable
handset answering our page and refusing our call.

**Run tonight, radio down (`F4.35`):** `0x96a0..0x96bf` reads **all zero**,
80 samples. The builder runs on demand, so the buffer has to be read while
paging. `F4.36` is written and pre-committed for that; it rings, so it is
attended, but it is log-only and nobody has to answer.

## `[CR A-0077]`: THE LONG-SLOT FLAG WAS NOT A FLAG, AND THE SECTION ABOVE IS TOO STRONG

Third correction of the night, and this one lands on what I wrote two hours
earlier. It started from a number I sent the room as an open observation
rather than a finding — `0xF625` byte 5 reads `02` in **471 of 554** samples
across the whole corpus, during our calls, during the handset's list-access
calls, and with no call up at all — with the comment that a field reading `2`
in 85 % of samples does not behave like a flag for something we supposedly
cannot obtain.

### What the room found when it went to check

- **Record `+0x116` bit 1 has two setters, not one.** `[CR A-0069]` found the
  classifier path (the handset's own bearer request) and missed an
  ordinary-bearer-flow setter in the bearer handler that sets the bit with a
  word-OR — which is why a bit-set-instruction search never saw it. It runs in
  **normal operation, with no long-slot request anywhere.**
- **Bits 1 and 2 are a bearer/slot state machine**, not clean flags: set bit 1,
  add bit 2, clear bit 1 on bearer events. Bit 1 is a central running-state
  bit.
- **`0xF625` byte 5 encodes only bit 1**, as `(bit1) ? 2 : 1`. So it reads `2`
  whenever that common state holds, including at idle because the record
  retains it. Our 471/554 is exactly that.

**Withdrawn as a result:** `[CR A-0069]`'s "bit 1 = long slot, set only by the
handset", `[CR A-0075]` §8's "a narrowband connection lacks it", and our own
note at the top of this file that byte 5 is "the air bearer's long-slot flag
(2 = long slot, 1 = normal), renegotiated during a call". **We have no
reliable long-slot indicator at all.**

**What survives, because it is physical rather than a flag:** mode 8
configures an **80-byte** B-field and modes 9/3/2 configure 40-byte. An
80-byte B-field cannot be carried on a normal slot — that is capacity, and it
holds whatever `+0x116` turns out to mean. So mode 8 does need a real long
slot. What can no longer be said is whether our connections ever have one.

### So the hold on F4.30–F4.33 changes its reason, again

It was "mode 8 is unreachable by construction". That rested on the withdrawn
reading. **The honest reason now is that we have no reliable long-slot
indicator, so a null result from those four calls could not be told from a
dead channel** — which is the same trap `vF418` fell into. They stay off the
list, and the question moves to `F4.37`, where the handset does the asking and
we dump the record through the whole call.

### And my own section above overstates it

"WE HAVE NEVER INSTRUMENTED THE DOWNLINK" is too strong, and `[CR A-0077]`
says so directly: **`0x10802` is written by both the rx and tx branches of the
configurator** — a shared per-channel enable, not a direction-specific one.
`[CR A-0075]` §1 over-simplified, `[CR A-0044]`'s reading was right for the
mode-2 state it analysed, and the transmit side writes a separate pointer that
is not yet pinned to an absolute address.

**What is actually true:** we have no instrument that reports the host→air
direction *on its own*, so a reading of `0x10802` cannot distinguish "the
downlink is configured" from "the uplink is". Every inference of the form
"the chip reads as configured, so our bytes must be arriving" is still gone.
But the stronger claim — that the downlink has never appeared in our dumps at
all — is not established, and I should not have written it as though it were.

### The better instrument, and its idle baseline

`[CR A-0077]`: watch the **whole word** at record `+0x116`, not `0xF625`
byte 5. `0x1AB9 + handle × 0x148 + 0x116`:

| field | handle 0 | handle 1 | meaning |
|---|---|---|---|
| bytes 0, 1 | `0x1AB9` | `0x1C01` | connection state; byte 1 is their candidate for a service classifier |
| `+0x116` word | `0x1BCF` | `0x1D17` | bit 6 coded-mode enable, **bit 5 the act/no-act gate**, bits 1/2 the slot state machine |
| `+0x137`…`+0x13C` | `0x1BF0` | `0x1D38` | saved-config fields |

Bit 5 is the answer to the `0x0242` problem: it is the gate that decides
whether a `0x0230` does anything, so it is the confirmation signal we have
been missing all week.

**Baseline measured tonight, radio parked, nothing transmitted** (`F4.35`
rerun with the record dumps, 80 replies, all stable):

```
0x1AB9 +0..3   00 ff ff ff        0x1C01 +0..3   00 ff ff ff
0x1BCF  word   00 00              0x1D17  word   00 00
0x1BF0 +6      00 00 00 00 00 00  0x1D38 +6      00 00 00 00 00 00
```

So the records are empty at rest and `byte 1 = 0xff` on both handles. Every
reading from a live call is a delta against this.

The dumps are wired into `F4.36` and `F4.37` as `$RECORD` in `f4.sh`, so both
attended runs produce the sequence the room asked for whether or not their
primary question answers.

## `[CR A-0078]`: THE DOWNLINK INSTRUMENT WAS TWO ADDRESSES WE ALREADY DUMP

`[CR A-0078]` gave us what this project has never had: a **per-direction**
indicator. The configurator programs a separate slot descriptor for rx and for
tx, and the B-field size at `descriptor+6` is direction-specific — `0x28`
normal slot, `0x50` long slot, `0x14` short. Unlike `0x10802`, which both
branches write, the **tx** descriptor's `+6` reports the host→air lane on its
own. A downlink mode change becomes a dumped byte instead of a person
listening.

The room named five candidate descriptor bases but **declined to say which is
tx**, giving us a pointer table at `0x11DDA` to resolve it from a live dump
instead of guessing. That was the right call, and the dump says something
neither of us expected.

### Measured tonight, radio parked, nothing transmitted

The five named bases all read the same and carry **no B-field size**:

```
0x10F3A / 0x10F8C / 0x10FDE / 0x11B06 / 0x11B58
    04 00 00 00 10 00 00 60      +6 = 00
```

But the pointer table decodes as `{ptr, ptr, two 16-bit fields}` per entry, and
**its pointers go somewhere else**:

```
0x11DDA  +00: 0x000116FA   +04: 0x00011712   +08: {0x000b, 0x0006}
         +12: 0x0001172A   +16: 0x00011742   +20: {0x000c, 0x0007}
         +24: 0x0001175A   +28: 0x00011772
         +32: {0x000d, 0x0008}  0x00011BAA  0x00011BC2 …
```

**`0x116FA` and `0x11712` are `dir0` and `dir1`** — the two addresses we have
been dumping in every run since F4.9, as bare two-byte words. Read as
descriptors they are exactly the right shape:

```
dir0  0x116FA   00 00 00 00 86 0b 28 00      +6 = 0x28   normal slot
dir1  0x11712   00 80 93 0b ff ff 28 00      +6 = 0x28   normal slot
```

So **we have been reading the first two bytes of the rx and tx slot
descriptors for weeks and never looked at offset 6.** The `dir1 = 0x8000` /
`0x8001` values that `[CR A-0042]` went to the trouble of explaining are the
first word of a descriptor, not a standalone flag.

By the room's rule — the second pointer of a channel's pair is the tx
descriptor — **`dir1` is tx**, which also fits the behaviour: `dir1` is the
one that moved to `0x8001` during `vF418`'s mode-8 window while `dir0` carried
`0x0002` at the narrowband bind. Sent to them for labelling rather than
asserted here.

### What it changes

- **`0x11718` (`dir1+6`) is the downlink instrument.** `0x28` → `0x50` is a
  long slot configured on the host→air lane, reported on its own, with no ear
  involved.
- **It settles the dead-channel-vs-null problem** that the four-call hold
  rests on. `F4.37` can now distinguish "mode 8 configured on the downlink"
  from "nothing happened", which is precisely what `vF418` could not do.
- **Idle baseline: both read `0x28`.** Measured with the radio parked, so
  normal slot is the resting state and any `0x50` is a real change.

Dumped in all three rigs as `$LANES` and `$DIRS`.

### A rig bug this found, which would have cost an attended run

The daemon's `memdump=` list was capped at **8 regions**, and exceeding it
failed the whole `-a` string — which printed sixty lines of usage with no
indication of why, and looked exactly like a missing argument. A run now
carries 13 regions before anything specific to its own question. Cap raised to
24, and a bad `-a` now says which string was rejected before the usage dump.

It cost one unattended run tonight. It would have cost an attended one.

### `[CR A-0079]`: labelled, and a live slot map in a record we already dump

The room confirmed `dir1` = tx three ways (the `0x11DDA` entry is
`{rx_ptr, tx_ptr}` and `0x11712` is the second; `A-0042`/`A-0044` already had
`dir0` = air→host and `dir1` = host→air; and `vF418`'s own behaviour). It also
owns the `A-0078` address error: **the five bases are slot-type templates**,
selected by a slot-type switch and carrying fixed geometry, which is why they
read identically with `+6 = 00`. The `+6` rule was right; the addresses were
not, and the live dump corrected both.

**So `0x11718` is the load-bearing byte** and the rest of that address list is
not dumped any more.

And the static `0x11DDA` pairs are not a slot map — the table is built once at
init and never written at runtime. **The live map is in the connection record
we have been dumping all along:**

| field | handle 0 | handle 1 | |
|---|---|---|---|
| `+0xFD` | `0x1BB6` | `0x1CFE` | **rx (air→host) slot** — what the `0x0203` builder reads to place a received C-plane |
| `+0x104` | `0x1BBD` | `0x1D05` | **tx (host→air) slot** — its companion, armed with the connection |

These update across handovers, so this is "which physical slot is this
connection on", live and per direction — something we have never had.

**Idle baseline, radio parked, nothing transmitted:**

```
handle 0  +0xFD = 03   +0x104 = 03      dir0 +6 = 0x28
handle 1  +0xFD = 04   +0x104 = 04      dir1 +6 = 0x28
```

**One thing to resolve on a live call, flagged rather than assumed:** the
slot numbers are **already populated with the radio down and no connection**,
and **rx and tx read the same value** on both handles. The room described
`+0x104` as copied when a connection is armed, so either these are per-handle
defaults that a real connection overwrites, or the field carries the base slot
with the duplex half-frame offset implied. `F4.37` will show which; the
baseline above is what any live reading is a delta against.

### The assembled instrument for F4.37

Two bytes decide the question the four retired calls were stuck on:

- **`0x11718` (`dir1+6`)** — `0x28` normal, `0x50` long. The host→air lane
  width, on its own.
- **`0x1BBD` / `0x1D05` (record `+0x104`)** — the tx air slot.

`0x50` plus a long-slot number on the A540's own wideband call means the
packings are worth running, and we know it from a dump. `0x28` throughout
means the downlink never went long and the packings would test nothing.
Either way, no ear required.

### `[CR A-0080]` and the slot arithmetic: the `+6` does not match DECT numbering

`[CR A-0080]` withdrew the `+0x104` half of its own previous answer, which I
had queried before the run rather than after: **`+0xFD`/`+0x104` are slot
*numbers*, never shifted or masked into a width field, so there is no
"long-slot number" to watch there.** For F4.37's long-slot question the sole
instrument is **`0x11718` (`dir1+6`)** — `0x28` normal, `0x50` long. The two
counters at `+0xFE` and `+0x102` are bearer-handler state and a tick; ignore
both, confirmed.

It also answered the idle question: option (1), per-handle defaults that a
real connection overwrites. On a live connection the offset **is** stored —
the bearer handler sets `+0x104 = +0xFD + 6`, subtracting 6 instead when the
rx value exceeds 7. So on a live call the two should **differ**, which makes
`+0x104` a useful liveness check even though it says nothing about width.

**The mapping of that `+6` onto DECT slot numbers was handed to us, and it
does not fit.** EN 300 175-3 §4.2.2:

> "Full-slots are numbered from K = 0 to 23 … Double slots are numbered from
> K = 0 to 22; there is no K = 11. Normally full slots K = 0 to 11 are used in
> the FP to PP direction, and slots K = 12 to 23 in the PP to FP direction.
> Normally double slots K = 0 to 10 are used in the FP to PP direction, and
> double slots K = 12 to 22 in the PP to FP direction."

**The duplex offset is +12 in both numberings**, and half-slot numbering is
`L = 0 or 1` within a full slot, which does not admit a +6 either. A wrap at
"rx > 7" fits none of them: the full-slot FP→PP range runs to 11, the
double-slot range to 10.

So one of these is true, and we cannot tell which from here:

- the field is an **internal index**, not a DECT air slot — in which case
  "which physical slot is this connection on" is not yet answered and
  `+0x104` is only a liveness flag;
- or there is a scaling between the chip's index and the air numbering that
  neither side has seen.

Worth noting alongside: the static table at `0x11DDA` pairs its two pointers
with fields whose offset the room reports as **5**, not 6 — two different
offsets for what is described as the same rx/tx relationship. Raised with
them rather than reconciled here.

**Measured, so F4.37 settles it for free:** the idle values are `h0 = 3/3`
and `h1 = 4/4`, and the prediction is that a live call makes them differ by 6.
If it does, the arithmetic is confirmed and the numbering question stays open;
if they stay equal, `A-0080`'s live-offset claim does not hold on our
hardware. Either way the long-slot answer comes from `0x11718` alone and does
not depend on this.

### `[CR A-0081]`: they are DSP lane indices, and there is no slot map after all

The arithmetic check paid for itself. `[CR A-0081]` withdraws the slot-map
reading entirely: **`+0xFD`/`+0x104` are internal DSP lane indices, not air
slots.** The field is used only as an index into the chip's DSP config
structures, resolved against the DSP bases — the same `0x10000` region that
holds the `0x10802` enable. The values `3`/`4` select lanes, of which there
are a handful, not timeslots, of which there are 24. **No verified
chip-index→air-slot translation exists on these fields.**

So the `±6` is internal bookkeeping, not a duplex offset, and the fold at
index > 7 is an internal range wrap — which is exactly why it could not be
made to match a boundary that lives at 11/12 and 10/12. The `+5` of the static
`0x11DDA` table is a third thing again (per-DSP-channel resource indices,
written once at init). `[CR A-0079]` called two different index spaces "the
tx/rx offset"; both are withdrawn.

**What we lose:** the live per-direction slot map, which we never actually
had. "Which physical slot is this connection on" is **still unanswered**, and
should go back on the list as open rather than quietly solved.

**What we keep, and it is the whole point:** `0x11718` (`dir1+6`) is a
**B-field size**, verified, with no air-numbering dependency anywhere in it.
`0x28` normal, `0x50` long. The one load-bearing byte for `F4.37` is
untouched by any of this.

**One thing worth keeping from the wreckage.** The room says `+0x104` is
rewritten by `±6` *when the connection is on the coded path*. That makes a
divergence from `+0xFD` weak corroboration that the coded-path bookkeeping
fired — not a long-slot indicator, not an air-slot reading, but a second,
independent trace of the same event `0x11718` reports. Worth recording in
`F4.37` for that and nothing more, which costs us one dump we are already
taking.

**The pattern, now three for three.** `0xF625` byte 5, the `A-0078` template
addresses, and now the slot map: each time a chip-internal field was mapped
onto a DECT air concept, and each time our own numbers caught it before a
bench run did. The check that worked every time was the cheapest one — take
the arithmetic to the standard and see whether it fits any numbering the
standard defines. It costs minutes and it has now saved three runs.

### `[CR A-0082]`: one of its two instruments fails our own idle check

`[CR A-0082]` offered the transmit counterpart of `A-0044`'s receive tuple —
**`dest 1` (`0x1168C`) + `enable`** as the host→air input stage — and
**`dir1` bit 0** as a DSP-set liveness bit for the coded transmit block. Both
were checked against the corpus before being written into `F4.37`. Neither
survives as stated.

**`dest 1` never transitions.** Across every log we hold:

| `dest 1` | samples |
|---|---|
| `0x0bc5` | 2210 |
| `0x085d` | **68, all in one run** |

All 68 are `vF49`, and they are present from its **first sample at
t = 0.05 s**, with `dest 6` still at the idle `0410` and nothing configured.
So `dest 1` does not open and close with a call — it is almost certainly
retained chip state from a previous run, which we know the chip does. **In
2278 samples it has never once changed in response to anything we sent.**
That is the idle-baseline check from the lesson above, applied immediately:
a field that already reads "configured" before anything is configured is not
reporting configuration.

**`dir1` bit 0 tracks the flags byte, not the codec.** Every transition we
have, against the `0x0230` that preceded it:

| run | t | mail | `dir1` |
|---|---|---|---|
| `vF413` | 49.4 | `fl=41 codec=3` | → `8001` |
| `vF413` | 73.4 | `fl=01 codec=3` | → `0000` |
| `vF413` | 97.4 | `fl=41 codec=3` | → `8001` |
| `vF413` | 105.4 | `fl=41 codec=3` | → `0000` ✗ |
| `vF418` | 60.8 | `fl=41 codec=1` | → `8001` |
| `vF418` | 70.8 | `fl=41 codec=1` | → `0000` ✗ |
| `vF418` | 232.5 | `fl=41 codec=3` | → `8001` |
| `vF418` | 233.0 | `fl=01 codec=3` | → `0000` |

**Flags bit 6 (`0x40`) sets it and its absence clears it, in 6 of 8
transitions.** The two exceptions are both a *second identical mail* and both
coincide with the whole configuration collapsing to idle — the teardown case.
**The codec byte does not predict it at all:** bit 0 was set by `codec = 3`
three times and by `codec = 1` once.

In `vF413` it was set at 49.4 by a **narrowband** mail, eight seconds before
any `codec = 1` was sent, and stayed set across the entire mode-8 window. So
"bit 0 means the DSP is running the **coded** TX block" does not hold here —
whatever it reports, a narrowband configuration with flags bit 6 produces it.

**What this leaves for `F4.37`.** `0x11718` alone, exactly as `[CR A-0080]`
said: `0x28` normal, `0x50` long, no air-numbering and no codec dependency.
The proposed conjunction with `dir1` bit 0 is **not** added, because bit 0
can be 1 for reasons that have nothing to do with mode 8, and a second number
in the brief that means less than it looks like is worse than one clean one.

**And one thing that does survive, which is not nothing.** `A-0082`'s
architectural answer stands: there is **no CPU-side "audio is flowing" byte**,
because the lane is a continuous TDM stream rather than a buffered FIFO and
the DSP, not the CPU, runs the loop. So "configured" is readable and "working"
is not, by construction — the ear stays irreplaceable for "is the audio
*right*", and that is a property of the hardware rather than a gap in our
instrumentation. Worth knowing we can stop looking.

## FIRST HARDWARE TEST OF THE DATE AND TIME `[vF438, vF439, 2026-10-23 evening]`

Run on the maintainer's explicit authorisation while he was away. Silent
throughout: ring set past the run length, nothing paged, nothing rang. Radio
up at t = 0, parked at t = 150 s, in both runs.

### It works, and the message is byte-correct

```
nwk out --> 64 62 23 08 c0 26 10 23 18 44 26 00
             |  |  |  |  |  |  |  |  |  |  |  '-- time zone: GMT
             |  |  |  |  |  |  |  |  |  |  '----- second 26
             |  |  |  |  |  |  |  |  |  '-------- minute 44
             |  |  |  |  |  |  |  |  '----------- hour 18
             |  |  |  |  |  |  |  '-------------- day 23
             |  |  |  |  |  |  '----------------- month 10
             |  |  |  |  |  '-------------------- year 26
             |  |  |  |  '----------------------- coding 11 "time and date",
             |  |  |  |                           interpretation 0 "current"
             |  |  |  '-------------------------- length 8
             |  |  '----------------------------- <<TIME-DATE>>
             |  '-------------------------------- {FACILITY}
             '----------------------------------- TI 6, PD 4 (CISS)
```

**The C4 sent nothing back on PD 4** — no `{CISS-RELEASE-COM}`, no rejection —
and the link stayed up. So the transaction was accepted. **Whether its display
changed is unverified** and needs human eyes; this establishes the protocol
half only.

**Handset substitution, stated plainly:** the maintainer asked for the A540.
In `vF438` the A540 never appeared at all, so that run was void for time, as
its card pre-committed. `vF439` loaded the C4's record instead, because the C4
was the handset actually present. (The A540 *did* show up in the second run,
locating once and placing six list-access calls — so it is alive; it was
simply not the admitted handset.)

### A defect found by running it, which no unit test would have caught

The first `vF439` sent the time **14 times for 7 location registrations**, the
extras landing immediately after `connection up` with no `{LOCATE-ACCEPT}`
between them. Cause: `dect_link_init()` zeroes `mm.stats.located`, while the
daemon's watermark `time_sent_for` kept its old value, so `0 != N` fired a
send on every connection setup.

**A counter compared across a re-initialisation.** Both sides are correct in
isolation and the unit tests of each pass; only running it showed the seam.
Fixed by clearing the watermark where the link is created. Re-run: **1 locate,
1 send, 1 ack.**

### Three other things the same run confirmed, all from tonight's work

- **`{TEMPORARY-IDENTITY-ASSIGN-ACK}` is real.** `05 5d` came back after the
  `{LOCATE-ACCEPT}`, exactly as the identification earlier tonight predicted
  from three lines of circumstantial evidence. The `<MM_ident.1>` timer now
  has its stop condition confirmed on hardware.
- **The speech gate works.** The A540 placed **6 list-access calls**
  (`<<BASIC-SERVICE>> 0x28`), the daemon declined all six with
  `not a speech call ... no audio path bound`, and **0 audio channels were
  bound**. Before tonight all six would have armed the audio path and played
  a lane into a phonebook read.
- **`reg: nwk out` is back**, which is the only reason any of the above could
  be read. See the note below.

### The log line that nearly cost an hour

`vF438` showed 20 `{LOCATE-REQUEST}`s arriving with **no reply logged** —
which reads as a serious protocol defect. It was not. The `reg: nwk out -->`
line had been removed by `9f08622`, the `#67` refactor, when the drain moved
into the library. We were answering and had stopped saying so; 149 outbound
C-plane frames in the same log said as much, if you looked.

**Reading a log for an absence only works if the thing would have been
printed.** Restored as an optional `tx_log` hook on `dect_link` — logging
only, the library still does no I/O of its own.

## THE REFUSAL IS A {CC-SETUP} COLLISION, AND WE NEVER RAN OUR HALF OF §9.5.2.3 `[2026-10-24 evening, desk]`

Found by tabulating the corpus instead of dialling: 93 logs with an
FP-originated `{CC-SETUP}`, every one classified by what the handset's own
CC transaction was doing at that moment (`tools` in the scratchpad; the table
is reproduced in `spec-conformance.md`).

| outcome | handset's own CC state at our SETUP | runs |
|---|---|---|
| ALERTING / CONNECT | no handset-originated call anywhere near | **26** |
| `0x32` | its `{CC-SETUP}` crossed ours by 100–300 ms, or its call was active | **33** |
| `0x32` | no handset SETUP visible before the refusal | 25 (+2 void identity runs) |
| no reply | mixed | 9 |

**Not one successful ring coincides with a handset-originated call; not one
refusal is free of one, except the 25 where nothing of the handset's is
visible on the wire.** The 33 are EN 300 175-5 §9.5.2.3 verbatim — the
spec's own figure for the clause shows a PT's *LiA service call* crossing an
FT's *normal call setup*, and the mandated PT answer is exactly our
signature: `{CC-RELEASE-COM}` (1, TV1), reason "insufficient resources",
~40 ms after the DLC ack. The earlier rejection of this reading
(`call-g7.md`, 2026-10-22, and `spec-conformance.md`, 2026-10-24) rested on
the 25: *"no competing `03 05` from the handset"*. But the clause applies
from **CALL INITIATED**, which is the PT's state the moment its CC accepts
an MNCC_SETUP-req — before its SETUP is on the air — and a PT that detects
the crossing there "shall not setup any new outgoing call", i.e. it holds
its own SETUP back. A handset with a permanently pending list sync (it
displays "Synchronisiere"; our LiA replies never satisfy it) initiates that
call on every link that comes up, which is why it is always there to collide
with. The 25 are the same mechanism with the handset's half suppressed.

What we never did is **the FT's half** of §9.5.2.3: release the PT's crossing
call with "insufficient resources", and "re-present the incoming call ...
immediately". The PT then "shall wait for the re-presentation ... and not
setup any new outgoing call" for P<CC.06> = 1 500 ms. We ignored the crossing
SETUP (the handset's call then hung for its full 20 s P<CC.03>) and treated
the 0x32 as final.

Implemented in `dect_cc.c` (unit-tested, four new tests): a peer SETUP on a
different transaction while a call is in progress is released with 0x32
(§17.3.3, and §9.5.2.3 when ours is in CALL PRESENT); a 0x32 release of our
unanswered incoming call re-presents the identical SETUP at once, at most
`DECT_CC_REPRESENT_MAX` = 2 times, with F<CC.03> restarted.

### Pre-committed outcomes for the first bench run (A540, `./ringall.sh a540`)

| log shows | meaning |
|---|---|
| SETUP → `0x32` → **re-presented SETUP → ALERTING (`93 01`)** | the collision was the whole refusal; defect 1 is closed at the protocol level. The unsatisfied list sync stays a separate conformance defect and is the reason the collision was so reliable. |
| SETUP → `0x32` → re-presented SETUP → `0x32` again, twice | the handset refuses with no call of its own left in CALL INITIATED. Then the 0x32 is not §9.5.2.3 and the remaining candidate is the handset's list-sync state itself; next step is TS 102 527-3 conformance of our LiA replies, not another SETUP variable. |
| SETUP → ALERTING with no 0x32 at all | consistent (no crossing this time) but does not exercise the fix; run again. |
| VOID (no page response / handset never spoke) | the handset is asleep; not a measurement. |
| the handset's crossing SETUP is NOT released by us (no `83 5a e2 32` out) | our half did not run; read the log before believing anything else. |

The falsifying case is the second row. Nothing else in the daemon changed;
the A540 keeps slot 1 and its existing store.

### Run 1 of the collision fix: refused, re-presented, and the handset dropped the link `[vCOLLIDE-20261024-183515]`

`-a ringattach=15000` (new: ring N ms after the handset's first C-plane
contact, because a parked base is re-found anywhere between 2 s and never
within 170 s, and `RINGA540-20261024-182814` was VOID for exactly that).

| t (ms) | event |
|---|---|
| 5 712 | handset attaches, runs lists 00 / 06 / 08, releases at 19 392 |
| 21 072 | page answered, our `{CC-SETUP}` out |
| 21 272 | DLC ack |
| 21 292 | `{CC-RELEASE-COM}` 0x32 — **no handset SETUP anywhere** |
| 21 312 | our re-presented SETUP queued and on the wire (37 bytes, ~80 ms of air) |
| **21 372** | **the handset releases the MAC connection** (`0x0201`), before the re-presentation could even finish transmitting |
| 23 484 | the handset connects again and runs lists 00 / 06 / 08 once more |

Row 2 of the pre-commit did not fire as written (the handset never got to
refuse the re-presentation), but its meaning stands: **the 0x32 is not a
§9.5.2.3 crossing in this instance** — nothing of the handset's was in CALL
INITIATED on the air, and a PT resolving a crossing keeps the link for the
re-presentation instead of dropping it. What the handset did is refuse,
drop, reconnect, **and re-run its list sync**. That is a handset that
treats "list sync pending" as a reason to decline a call, and goes back to
the sync. Our sync never completes, so the refusal never ends.

Why the sync never completes, read against TS 102 527-3 (`docs/specs/`):

- our "start session confirm" said **2 entries** for every list, and
  **1 sorting field** even for the one-entry List of Supported Lists
  (7.4.10.4.1.2 says 0);
- our "read entries confirm" announced **3 entries delivered** and was
  followed by **no data packet**; 7.4.10.4.3.2: "Content of list entry is
  transmitted in data packets". The handset waits ~3.3 s, ends the session,
  tries the next list, and after list 08 releases and starts over;
- we answered the LiA service call with `{CC-CALL-PROC}` **and**
  `{CC-CONNECT}`; 7.4.10.6.1: CALL-PROC only, "no {CC-CONNECT} is used".

The §9.5.2.3 handling stays in: it is mandatory FT behaviour, unit-tested,
and it did exactly what it should on the wire. It was not sufficient.

### Pre-committed outcomes for run 2: real list data (`dect_iwu.c` rewritten against 7.4.10)

Lists 00, 06 and 08 served to the byte of tables 43, 49, 62, 64, 70 and
72; CALL-PROC only on the LiA call; ring 40 s after first contact so the
sync has finished (or shown that it has not).

| log shows | meaning |
|---|---|
| the handset reads 00 / 06 / 08, gets data packets, ends each session, **releases and stays quiet**; later our SETUP → **ALERTING** | the sync was the whole refusal; defect 1 closed |
| the sync completes, the handset goes quiet, our SETUP → 0x32 anyway | the refusal is not the sync either; back to the handset-state question with two mechanisms eliminated |
| the handset keeps re-running the sync every ~15 s although every read is answered with data | our data content is still wrong somewhere, or it wants a list we do not serve (it would show in which list it loops on, or in a start-session reject) — fix that first, the ring result is then moot |
| the handset reads list 00 and immediately asks for a list we reject (01, 05, 07 …) and loops | advertise-and-serve that list next; same as the row above |
| VOID | asleep; `ringattach` only rings after contact, so this would show as no contact at all in 15 min |

### Run 2: list sync completes cleanly — and the refusal survives it `[vCOLLIDE-20261024-184852]`

First uncontaminated FP-originated refusal in the project. All four known
confounds are gone at once:

- drain latency: fixed (queue-to-wire 0 ms);
- identity poisoning: the A540 is on its own slot 1 with its own store;
- `{CC-SETUP}` collision: **verified absent** — the only inbound CC in the
  window around the refusal is the `93 5a e2 32` itself; the `11 22 11` /
  `13 21 01` fragments are Lc/LAPC control, not CC messages;
- list sync: **complete and quiet** — the handset read lists 00, 06 and 08,
  got real data packets for each, closed every session, and released the
  LiA service call with reason `00`. It did **not** re-run the sync after
  the refusal (`cc: setups_in 0` for the rest of the run).

The list dialog, byte for byte, worked:

```
hs  c3 00 00 00                              start session, list 00
us  c3 01 00 81 81 00 00 00 ff 00            confirm: session 1, 1 entry, 0 sort
hs  c3 06 81 81 01 00 01                      read entry 1, field 01
us  c3 07 81 81 01                            read confirm: 1 delivered
us  c3 14 81 81 86 01 84 80 00 06 08          DATA PACKET LAST: lists {00,06,08}
hs  c3 02 81     us c3 03 81                  end session / confirm
... same for list 06 (DECT 1) and list 08 (Line 1, attached bitmap) ...
hs  03 4d e2 00                               LiA service call released, reason 0
```

That `c3 14` data packet is exactly what the old responder never sent. The
sync is fixed, and it is a real TS 102 527-3 conformance gain regardless of
the ring.

**And the ring is still refused.** Paged at t=45 s (sync long finished),
page answered, our SETUP out, `93 5a e2 32` at +240 ms, handset drops the
MAC link. Our SETUP was the default `e0 80` (narrowband basic speech) with
`7c 04 90 02 00 84` (G.726 32k full slot only). The A540 registered as a
**wideband** handset (`7c 07 90 03 00 01 02 00 84`, G.722 64k long slot
first), and stock's incoming-call SETUP to these handsets carries
`<<BASIC-SERVICE>>` **0x88** (wideband), not 0x80 (`[CR A-StockTrace-1]`:
both stock calls were 0x88). We offer the wideband handset a narrowband
call with only its fallback codec. That difference was previously tested
only under collision/broken-sync (content sweep phase 7, now void), so it
is back in play on a clean instrument for the first time.

### Pre-committed outcomes for run 3: match stock's wideband offer

`EXTRA="basicsvc=0x88,codeclist=90030001020084"` — `<<BASIC-SERVICE>>`
0x88 (which also flips the page-info byte to the wideband form `0x2c`,
`ring_page_info()`), and the full registered codec list. Control is run 2
(default `e0 80`, clean `0x32`).

| run 3 shows | meaning |
|---|---|
| **ALERTING** | the narrowband/codec mismatch was the refusal; the A540 needs the wideband offer stock sends, the C4 tolerates narrowband. Defect 1 solved; narrowband becomes a per-device concern for a profile. |
| `0x32` again | wideband negotiation is **not** it either. With collision, sync and codec all eliminated on a clean instrument, the surviving cause is the one the chip owns: no duplex **traffic bearer** is established for an FP-originated call (Q-0047). That is the hardware-analysis handoff, not a clean-side dial. |
| the handset's own SETUP crosses ours | discard; the ring landed on a list-access call; re-run with a longer `ringattach`. |

### Run 3 and the headline: the refusal is the chip's paged bearer, and it is handset-specific `[vCOLLIDE-20261024-185625]`

Run 3 offered the A540 exactly what stock offers a wideband handset:
`<<BASIC-SERVICE>>` 0x88, the full codec list `7c 07 90 03 00 01 02 00 84`
(G.722 long slot then G.726), and the wideband page byte `2c`. On the wire,
verified. The sync completed first, no collision. **Still `93 5a e2 32`.**
So the codec/wideband offer is not the cause either. Collision, list sync
and codec are now all eliminated on a clean instrument — the first three
rows of the project's "eighteen theories void" list that have been re-tested
without any confound.

Then the decisive measurement, read from the chip's own `0x0242`
(channel-assigned) reports, which `[CR A-0062]` names as the go/no-go for a
bindable traffic bearer — a non-zero channel means the bearer is
traffic-capable, zero means signalling-only:

| handset | page sent | `0x0242` at the page response | result |
|---|---|---|---|
| **FRITZ!Fon C4** | `0x0300 {2c 11 01}`, bsvc 88 | **`01 40 00 00`** (byte 1 = 0x40, traffic) | `{CC-ALERTING}` — rings |
| **Gigaset A540** | `0x0300 {2c 11 01}`, bsvc 88 | **`01 00 00 00`** (byte 1 = 0x00, signalling only) | `{CC-RELEASE-COM}` 0x32 |

**Same base, same code, same page bytes, same SETUP form.** The C4's paged
connection comes up with a traffic bearer (`0x40`) before it even alerts;
the A540's comes up signalling-only (`0x00`), and it then refuses the call
with "insufficient resources" — which is literally true, there is no
traffic bearer behind the call. On answer the C4's `0x0242` goes to
`01 41 02 00` (channel 1 assigned); the A540 never gets that far.

Three C4 logs (v28, vC4T4d, vC4sync2) all read `01 40 00 00` at ring; two
A540 runs read `01 00 00 00`. The split is clean and it is **upstream of
NWK entirely** — it is which MAC bearer the chip and the handset negotiate
when the handset answers the identical page.

**Why this is now a chip question and not a clean-side one.** The page the
chip transmits is identical for both handsets, so the page content is not
the lever — and `[CR A-0076]` already established the chip ignores our
page-info byte and offers no long-slot-page path, so we cannot change the
transmitted page from the host anyway. `[CR A-0062]` established there is
no host→chip bearer-allocation mail: `0x0242` is the chip's own report, and
`0x0230` can only bind onto a channel the chip already assigned (which is
why `-a bindearly` could never help — a signalling-only link has no channel
to bind). On **stock** the A540 rings (maintainer, 2026-10-22), so stock's
chip does give the A540 a `0x40` bearer from its page; ours does not. What
in the chip's page/bearer path makes a paged connection traffic-capable,
and why it differs between the C4 and the A540 under our driving, is
`Q-0055` to the analysis side — with `0x0242` byte 1 as the exact observable to
trace against, measurable on our own box with no stock capture.

**Clean-side status of defect 1:** the protocol layer is now correct and
tested — collision resolution (§9.5.2.3/§17.3.3), a conformant list-access
responder (TS 102 527-3 §7.4.10, sync completes), and the wideband offer
stock sends. None of them makes the A540 ring, because the refusal is below
them, at the chip's MAC bearer for a paged connection. That is the one
remaining cause and it is the chip's to answer.

### Run 4: the C4 theory, applied to the A540 — drop the stock bring-up config `[pre-commit]`

Comparing the host→chip mail streams of a C4 ring (`vC4T4d`, `0x0242` = `0x40`,
rings) against the A540 runs (`0x0242` = `0x00`, refuses) surfaced one
controllable difference in bring-up: the C4 run sent a plain bring-up
(`0x022b`/`0x022c`/`0x015b`/`0x020a`/`0x0247`), while the A540 run additionally
sends `0x0153`/`0x0157`/`0x0169` — the stock-config replay added 2026-10-24,
whose effect on the 07.56 chip is a "faithful replay, not an understood
mechanism" (`docs/journals/why-the-handsets-never-rang.md`). The C4 captures predate
that replay, so the C4 rang on a daemon that did **not** send those three
mails.

They write chip state nobody has decoded on 07.56 (`0x0153`→`0x16c`/`0xdd8`,
`0x0157`→`0x989b` bit 4 + `0x1e6` bit 0, `0x0169`→`0x154 |= 0x05`). If one of
them puts the chip in a state where a paged connection comes up signalling-only
(`0x0242` = `0x00`), that is a cause the A540 enforces and the C4 tolerates.

Run 4 = run 3 (wideband `e0 88`, full codec list) **plus `-a nostockcfg`** —
the exact bring-up the C4 rang on. Single variable from run 3.

| run 4 shows | meaning |
|---|---|
| `0x0242` → `01 40 00 00` and `{CC-ALERTING}` | the stock-config replay was breaking the A540's paged bearer; drop it (or fix which of the three) and the ring works. The whole 2026-10-24 "replay stock's bring-up" direction is then the regression, not the fix. |
| `0x0242` → `01 40 00 00` but still `0x32` | the bearer came up traffic-capable yet the handset still refused — then the bearer is necessary but not sufficient, and something in the SETUP/CC path is the second cause (now testable, bearer finally present). |
| `0x0242` stays `01 00 00 00`, `0x32` | the stock config is not the differentiator; the `0x40`/`0x00` split is handset-intrinsic under identical driving, and it is purely Q-0055's chip question. |
| VOID (no contact) | nostockcfg changed the beacon enough that the A540 will not attach; re-run with the config on, note it, and the answer is "the config is needed for attach" — a different constraint. |

### Run 4 result: nostockcfg eliminated — the bearer split is handset-intrinsic `[vCOLLIDE-20261024-191251]`

Stock config confirmed off (zero `0x0153`/`0x0157`/`0x0169` in the log), the
A540's bring-up now identical to the C4's. The paged connection still came up
**`0x0242 = 01 00 00 00`** (signalling-only) and the call was still refused
`93 5a e2 32`. Outcome row 3: the stock-config replay is **not** the
differentiator.

So the C4 theory is fully tested from the host side and the result is a clean
negative. The A540's paged bearer has now been driven three ways, all giving
`0x00`:

| run | A540 driving | `0x0242` | result |
|---|---|---|---|
| 2 | clean baseline, narrowband `e0 80` | `01 00 00 00` | 0x32 |
| 3 | wideband `e0 88` + full codec list | `01 00 00 00` | 0x32 |
| 4 | run 3 **+ nostockcfg** (exact C4 bring-up) | `01 00 00 00` | 0x32 |

The C4 gets `01 40 00 00` from the same page and bring-up; the A540 gets
`01 00 00 00`. **Nothing the host sends — page, bring-up config, SETUP
service/codec — moves the A540's paged bearer off signalling-only.** The
bearer type is the output of the chip↔handset MAC negotiation and it differs
by handset. On stock the A540 gets a traffic bearer (it rings), so the lever,
if any, is in the chip's per-subscription/registration handling of that
handset, not in the call path. That is exactly what `Q-0055` asks, and run 4
removes one candidate (the stock config) from its question 2.

### Hardware confirmation: the A540 displays "DECT 1" from our Internal Names List `[observed by the maintainer, 2026-10-24 evening]`

During/after the run-4 session the A540 showed **"DECT 1"** on its display.
That string is not a handset default — it is exactly the Internal Names List
entry our responder serves for slot 1 (`dect-stubd` installs the handset's
subscription slot as terminal id with name "DECT n"; slot 1 → "DECT 1"). So
the handset **read our list and adopted the name**, which is independent,
on-handset proof that the CAT-iq list-access sync now works end to end: the
handset consumes our list data and provisions itself as an internal party.
The old responder, which never sent a data packet, could never have produced
this — the handset would show its own fallback, not ours.

**What it does and does not mean.** It confirms the provisioning layer is
fixed and the A540 now treats us as a proper CAT-iq base for lists. It does
**not** unblock the ring: the refusal is below NWK, at the chip's paged
bearer (`0x0242 = 0x00`, runs 2–4), which no host-side change moves. The two
are separate layers. The encouraging reading is that everything above the
bearer — registration, attach, list sync, provisioning — is now demonstrably
right, which is what makes the surviving `0x40`/`0x00` bearer split the single
clean remaining cause for `Q-0055`.

### Run 5: the real fix candidate — the 0x020A config byte `[CR A-0113]`

The the analysis side answered Q-0056 (A-0113) and corrected the whole bearer premise:
stock's A540 **rings** with `0x0242 = 03 00 00 00` — chan byte `00`,
signalling-only, identical to our refusing A540. The `0x40`/`0x00` split is
handset-class (CAT-iq C4 opens a traffic bearer at once; a GAP Gigaset is
signalling-only until answer), **not** ring-vs-refuse. So runs 2–4's bearer
reading was a red herring, and there is no missing per-call mail — stock sends
nothing between the page and the `{CC-SETUP}` that we don't.

The difference is two attach bytes we send as `0x00` and stock sends non-zero:

- **Primary: the `0x020A` config byte** after the 16-octet UAK. Stock sends
  `0x01` for every Gigaset (`0x02` for the C4). Firmware: stored at
  UAK-slot+`0x10`, and the CC state machine copies it into the connection
  record's auth/capability word (`conn+0x21`) that the ring progression
  (`0x0209`/auth) reads. Our `0x00` seeds the wrong security class; a GAP
  handset gates call acceptance on it and refuses with `0x32`
  ("insufficient bearers available" = the PP cannot get its own bearer
  granted). The C4 does not depend on this GAP state, so it rings regardless.
- **Secondary: the `0x0247` install flag byte**, bit 0. Stock sends bit 0 = 1
  (A540 = `0x03`); our `0x00` leaves a per-slot mask bit set where stock
  clears it.

Implemented as one-byte flips on mails we already send: `-a attcfg=N`
(the `0x020A` byte) and `-a attflag=N` (the `0x0247` byte), applied in
`send_subscription()`, which runs on reload so the A540's existing store picks
them up with no re-registration.

**Run 5 = `attcfg=0x01` (the A540's stock value), default call otherwise.**

| run 5 shows | meaning |
| --- | --- |
| **`{CC-ALERTING}` — it rings** | the `0x020A` config byte was the cause; set `0x01` for Gigasets (`0x02` for the C4) and defect 1 is solved |
| still `0x32` | the config byte alone is not enough; run 6 adds `attflag=0x03`, then both |
| the handset will not attach | `0x01` disturbed attachment (unlikely — it is stock's own value); note and revert |

### Run 5 result + Run 6: attcfg=0x01 alone did not ring; try both bytes

Run 5 (`vCOLLIDE-20261024-195552`): the `0x020A` went out ending in `01`
(`...de ad be 01`), confirmed on the wire, so the config byte reached the chip.
The A540 still refused with `93 5a e2 32`. So the `0x020A` config byte alone is
not sufficient. Per A-0113's test order, next is the `0x0247` flag byte, and
since the room expects "(1) or (1)+(2)", run 6 sends **both**:
`attcfg=0x01,attflag=0x03` (the A540's stock values for both).

| run 6 shows | meaning |
| --- | --- |
| `{CC-ALERTING}` | the two attach bytes together were the cause; set both to stock's per-handset values and defect 1 is solved |
| still `0x32` | neither attach byte, alone or together, flips it; A-0113's model is incomplete, and the next step is the room tracing what `conn+0x21` (seeded by the `0x020A` byte) gates downstream in the CC state machine |

### Run 6 result: both attach bytes on the wire, A540 still refuses `[vCOLLIDE-20261024-195840]`

`attcfg=0x01,attflag=0x03` both confirmed on the wire:
`0x020A` ends in `01` (`...de ad be 01`), `0x0247` install for slot 1 is
`01 01 03`. The A540 still answered `93 5a e2 32`, and `0x0242` stayed
`00 00 00` throughout — consistent with A-0113's correction that a ringing
Gigaset also shows `0x0242 = 00`, so the bearer byte is not the signal.

So **neither A-0113 attach byte flips the refusal**, alone (run 5) or together
(run 6). The attach-byte values now match stock for the A540
(`0x020A` cfg `01`, `0x0247` flag `03`) and the handset still refuses. That
exhausts A-0113's two candidates on the bench. Per the answer's own next step,
the question returns to the room: what does `conn+0x21` — the per-connection
auth/capability word the `0x020A` config byte seeds — gate downstream in the
CC state machine `a stock routine`, such that a GAP handset refuses the call. The
attach bytes are kept at stock's values (they are correct regardless and cost
nothing), and `-a attcfg`/`-a attflag` remain for whatever value the trace
names next.

### Run 7: THE FIX — 0x017c 00 09 sets the bearer-slot mask `[CR A-0114]`

The room traced it to the end (A-0114): `0xdb3` is a 6-bit traffic-bearer
slot-availability mask, init `0x3f` (all six slots in-use). The CC state
machine allocates an incoming-call bearer only from a CLEAR bit, finds none,
and the handset refuses with `0x32` "insufficient bearers available". Stock
sends `0x017c 00 09` at attach (`0xdb3 = 0x09`, four slots free); we never sent
`0x017c` at all. The C4 rings regardless because CAT-iq opens its own bearer
PP-side — which is exactly why all three Gigasets failed and the C4 did not.

Implemented: `send_subscription()` now sends `0x017c 00 09` after `0x020A`
(default on, `-a slotmask=N` / `-a noslotcfg`). Run 7 is the default build
with a read-back of `0xdb3` in the same run, attach bytes left at `0x00`
(isolating `0x017c`).

| run 7 shows | meaning |
| --- | --- |
| `0xdb3` reads `0x09` and the A540 sends `{CC-ALERTING}` | solved — `0x017c` was the missing mail; defect 1 closed |
| `0xdb3` reads `0x09` but still `0x32` | the mask landed but something else also gates it; report to the room |
| `0xdb3` still `0x3f` | the mail did not take (wrong addressing/handler); report the bytes |

### Run 7 result: 0xdb3 confirmed 0x3f -> 0x09, and the A540 STILL refuses `[vCOLLIDE-20261024-201602]`

`0x017c 00 09` went out (`tx M ... prim 017c len 2 00 09`) and the read-back
proves it took: `0xdb3` read `0x3f` on the first sweep (t=56 ms, before the
mail was processed) and `0x09` on every sweep after (304 reads at `0x09`,
`0xdb0:16` = `ff 00 00 09 80 80 00 02 03 07 05 04 09 ff ff ff`). So the
slot-availability mask now matches stock exactly throughout the ring.

**And the A540 still refused.** At the page (t=60239): `0x0242 = 01 00 00 00`,
`0x0243` progresses `2b 00` -> `2b 01`, our SETUP goes out, **`0x0209` never
fires**, and `93 5a e2 32` comes back. Identical to before the mask was set.

So A-0114's `0xdb3` was a genuine gap — we were leaving it all-blocked — but
setting it to stock's `0x09` is **not sufficient**: with four slots free, the
CC state machine's slot-allocation branch still does not run (`0x0209` absent,
`0x0242` stays `00`). Either the free-slot search has a second gate besides
`0xdb3`, or the branch reads the mask from a per-connection copy, or the search
runs but its result is not reached on this path. The `0x017c 00 09` mail is
kept (it matches stock and is correct regardless); the question returns to the
room with this read-back as the new constraint: `0xdb3 = 0x09` at ring time,
`0x0209` still absent.

### A-0115: the slot-search is behind a multi-gate bearer-validation the connection never reaches

The room answered run 7's constraint. `0x017c`/`0xdb3` was a real gap (keep
`0x017c 00 09`) but necessary-not-sufficient: the free-slot search sits behind
a gate chain in the CC state machine, and our incoming connection never reaches
it (hypothesis c). The loop reads `*0xdb3` directly (no per-connection copy),
so the mask is fine; the problem is upstream. Gate chain to the slot-search,
each gate rejecting if it fails, `param` = the per-connection record the frame
pump passes (`0x10000 + idx<<6`, field `+0x17` = the PP's received MAC/bearer
attributes):

1. record `+0x12` bit 6 set
2. event marker byte `R6 == 0xe0` (A-field active-slot / bearer event)
3. `param[1]` in {0,1,0x10,0x11,0xa0,0xa1}
4. `param[2] == *0x989a` (stock `0x4b`)
5. `param[3] & 0xe0 == *0x989b & 0xe0` (stock `0x989b = 0x80`, so top 3 bits `100`)
6. `param[3] & 0x0f > 0x0c`
7. `param[4] != 0x11`

The capability bytes `0x989a/0x989b` are built once at init from firmware
power-on defaults, so on the same STUB ours are very likely `4b/80` and gates
4-5 pass (consistent with nostockcfg changing nothing). The failing gate is
likely 1, 2, 3, 6 or 7 — the PP's bearer message is not arriving/matching, or
the connection is not in the state that validates it. We get `0243/0205`
(the alerting heartbeat, a different state on the page-response handle) but
never transition into this bearer-setup-validation state.

### Plan (maintainer's call): replay stock's exact sequence rather than chase gates

Asked the room (follow-up to A-0115) for the complete stock sequence to replay
verbatim: (1) the full host->chip mail stream from bring-up through
**registration** to the ringing call, marking every mail we don't send;
(2) the over-the-air NWK/DLC sequence the handset receives during registration
and the call; (3) which host mail or OTA message puts stock's connection into
the bearer-validation state above. Replay it, re-register the A540 if the
registration half differs, and page it. Rings on a faithful replay -> we had a
sequence/completeness gap. Still refuses when we send byte-for-byte what stock
sends -> it is registration-time handset config, not the call sequence. The two
A-0115 localization reads (confirm `0x989a/0x989b = 4b/80`; dump the
per-connection param record during the ring) are the fallback if the replay
does not ring.

### A-0116 + Run 8: the 0x0247 flag and 0x017c together, with a chip-RAM diff

A-0116 gave the full stock host->chip sequence. The call itself is just page +
auth-response, so replaying the call adds nothing; the replayable gap is in
registration, and the mechanism is a MAC state machine: the slot-search gate is
reached only when `0xdd5 ∉ {4,5,6,7}`, and `0xdd5` is driven by `0xdd8`
(our `0x0153`) and **`0xdd9` which must be `00`** (set by the `0x0247` flag:
bit0=1 clears the slot bit, our `0x00` sets it). Stock idle-one-registered:
`0xdb3=01 0xdd5=00 0xdd8=80 0xdd9=00`, `0x989a/b=4b/80`.

The never-run combination: the `0x0247` flag = `03` (clears `0xdd9`) AND the
`0x017c` mask in the same run. Run 6 had the flag but no `0x017c`; run 7 had
`0x017c` but flag `00`. Run 8 sends the A540's stock values together on reload
(no re-registration): `attcfg=0x01`, `attflag=0x03`, `slotmask=0x0f`, and dumps
`0xdb0..0xddf` to diff against stock.

| run 8 shows | meaning |
| --- | --- |
| `0xdd9=00`, `0xdd5 ∉ {4-7}`, and `{CC-ALERTING}` | the `0xdd9`/`0xdd5` state was the gate; clearing `0xdd9` via the `0x0247` flag (plus the mask) opens it — solved |
| `0xdd9=00`, `0xdd5 ∉ {4-7}`, still `0x32` | state matches stock but another gate remains; send the RAM diff to the room |
| `0xdd9` still non-zero | the flag on reload did not clear it (clear-foreign-slots sets it, or the double round / `0x022c` commit is needed); then the attended full-registration replay is next |

### Run 8 result: the chip state now MATCHES stock — and the A540 still refuses `[vCOLLIDE-20261024-203718]`

With `attcfg=0x01,attflag=0x03,slotmask=0x0f` on reload, the config region now
matches stock where it matters:

```
         0xdb3  0xdd5  0xdd8  0xdd9   0x989a/b
stock     01     00     80     00      4b/80
run 8     0f     00     80     00      4b/80
```

`0xdd9` cleared to `00` (the `0x0247` flag `03` did it, as the room said) and
`0xdd5` is `00` (out of the gate-blocking {4-7}). So the MAC state machine that
gated the slot-search (A-0115/A-0116) is now open, on our box, confirmed by
read-back. **And the A540 still answered `0x32`.**

Remaining RAM differences are the ones the room called downstream: `0xdb3` `0f`
vs stock idle `01` (dynamic slot mask; both have free slots), the `0xdb7..dbc`
slot-assignment array (ours `03 01 05 06 08 01` vs `07 04 03 01 05 02`), and the
`0xdc0` region. None of these is the gate state.

So clearing `0xdd5/0xdd9` was necessary progress but the refusal survives it,
and by A-0115's gate chain the live blocker is now gates 1-3/6-7 — the
**per-connection record** `param` (`0x10000+idx<<6 +0x17`), i.e. the PP's
received MAC/bearer attributes. That is set by what the handset does over the
air when paged, which is governed by the OTA exchange and the handset's
enrolment — the registration-config hypothesis. The host→chip chip-state is now
replicated; the next layer is the air/registration, which A-0116 #2 says is our
own NWK to diff against stock's C-plane.

`attcfg=0x01`/`attflag=0x03`/`slotmask=0x0f` are now the right per-A540 values
and should become the defaults for a Gigaset; kept as options meanwhile.

### A-0117 + Run 9: the registration ACCEPTs are identical — the gap is the call SETUP

A-0117 falsified the registration hypothesis: stock's ACCESS-RIGHTS-ACCEPT and
LOCATE-ACCEPT are byte-identical to ours (identities + codec list, nothing
else). The live difference is the `{CC-SETUP}`. Stock's SETUP vs ours:

| IE | stock | ours (default) |
|---|---|---|
| `<<BASIC-SERVICE>>` | `e0 88` | `e0 80` |
| `<<SIGNAL>>` (`0xe4`) | `e4 41` | **omitted** |
| `<<CALLING-NUMBER>>` `6c` | present | optional (tested) |
| `<<CALL-INFORMATION>>` `7e` | `7e 0c ...` | `7e 06` (tested) |
| `<<CODEC-LIST>>` `7c` | **absent in the call** | `7c 04 90 02 00 84` |

`0xe4` is our `DECT_IE_SIGNAL` — the standard GAP alerting-pattern element
(table 21), value `0x41` = external-call alert. We have it (`-a signal=`) but
omit it by default. The earlier `<<SIGNAL>> 0x40` sweep was void (pre-clean
instrument, wrong value). So three untested SETUP differences, all `-a` flags:
add `<<SIGNAL>> e4 41`, set `<<BASIC-SERVICE>> e0 88`, drop the call codec-list.

Run 9 replays stock's exact SETUP on top of run 8's matching chip state:
`attcfg=0x01,attflag=0x03,slotmask=0x0f,signal=0x41,basicsvc=0x88,nocodeclist`.

| run 9 shows | meaning |
| --- | --- |
| `{CC-ALERTING}` | the SETUP content (likely the omitted `<<SIGNAL>>`) was the final gap — defect 1 solved; then isolate which of the three |
| still `0x32` | not the SETUP content either; fall back to A-0117 step 4 (per-connection param dump during the ring) with the room |

### Run 9 result: SETUP now matches stock, A540 still refuses — and the gate is BEFORE the SETUP

Our SETUP went out as `13 05 <ids> e0 88 e4 41` — stock's exact IE set
(`<<BASIC-SERVICE>>` 88, `<<SIGNAL>>` e4 41, no codec-list). Still `93 5a e2 32`,
no `0x0209` (zero in the whole log).

Decisive realisation from A-0116's stock timeline: `0x0209` (the auth trigger
that leads to alerting) fires **before** stock's `{CC-SETUP}` (`0x0244`), at
bearer-up. So the SETUP content was never the gate — run 9 confirms it. The
blocker is the pre-SETUP bearer event that arms `0x0209` / passes A-0115's
gates 1-7 on the per-connection record.

**We have now matched stock on every host-controllable layer and the A540 still
refuses:**
- registration ACCEPTs — byte-identical (A-0117);
- chip config state — `0xdd5/0xdd9/0xdd8/0x989a-b` match (run 8);
- call SETUP content — `e0 88 e4 41`, no codec-list (run 9).

The one thing still different is what the handset emits on the bearer after the
page: stock's A540 sends MAC data that arms `0x0209`; ours does not, so the gate
never opens. Same handset, same 07.56 STUB. That is a chip-MAC / over-the-air
bearer-negotiation question — A-0117 step 4 — not a host content gap. Handed to
the room for the upstream trace: which inbound MAC bearer event arms `0x0209`
(gate 1 `record+0x12` bit6, gate 2 `R6=0xe0`), and what makes the handset send
it on stock but not here.

The stock-correct SETUP values (`signal=0x41`, `basicsvc=0x88`, drop call
codec-list) and attach values are kept as options; they match stock and are
right for a Gigaset regardless.

### A-0118 + Run 10: the decisive per-connection param dump

A-0118 pinned the arm: `0x0209` / `record+0x12` bit6 is set (a stock routine
@0x23730) only when the PP's inbound bearer event has `param[1]` bit3 (`0x08`)
set — `param` = the per-connection record `record+0x17..`. No host mail arms it
in a call; it's chip-autonomous off the handset's air event. So the read that
forks the whole problem: during the ring, is the A540 setting `param[1]` bit3?

Record pointer is at `0x168` (frame pump writes `0x10000+idx<<6` there each
frame); records are `0x10000+idx<<6`. Run 10 keeps run 9's stock-exact
SETUP+state and dumps `0x168:4` + `0x10000:256` through the ring (idle sweeps
before the page = churn baseline, A-0110).

| run 10 shows (rec = the A540's connection record) | meaning |
| --- | --- |
| `rec+0x18` (param[1]) bit3 **clear** | the A540 is not emitting the bearer-establishment attribute — not attempting the traffic bearer — cause is upstream on the air (beacon); room traces the 07.56 beacon builder |
| `rec+0x18` bit3 **set**, `rec+0x12` bit6 never set | arm path skipped for another reason (the `0xff600c` MAC-HW wait); room traces that branch |

### Run 10 inconclusive + the encryption lead

Run 10's param dump missed the active record: the frame-pump pointer (`0x168`)
during the ring was dominantly `0x10140` (idx 5, the likely A540 connection),
outside the `0x10000:256` range I dumped; the `0x10000` records captured are too
churny (A-0110 volatile region) to read `param[1]` bit3 cleanly. So the dump did
not fork it. A re-dump covering `0x10140` is possible but fragile.

**Encryption lead (maintainer).** In the Freetz test the DECT chip appeared to
RESET when encryption was toggled, and the handset may demand an encrypted
connection our chip is not configured to run. Confirmed on our side: the
incoming-call bearer is **never encrypted** — NWK CIPHER-REQUEST goes out only
at CC-CONNECT (never reached by a call refused at bearer-up), the chip
cipher-enable `0x0208` only with `-k` (field mapping unproven), and no chip
cipher-MODE config mail is sent at bring-up. Our own GAP audit
(`gap-conformance.md`): ciphering is optional for the FP, "but an individual
handset may refuse to operate without it — a compatibility question." This fits
A-0118's upstream/beacon fork: if the A540 will not attempt the bearer, a
missing encryption capability/mode in the beacon is a prime reason. Asked the
room (their encryption-toggle capture) for the chip cipher-mode mail behind the
reset and whether the A540 gates the bearer on advertised encryption.

### A-0119 + Run 11: encryption eliminated; the param dump targeted at 0x10140

A-0119 traced the encryption lead out: the chip reset on toggle is a hardware
GPIO reset (no cipher mail), `0x015F`/`0x015E` are never sent by stock, the only
cipher-linked bring-up config `0x0157 00` we already send, and the beacon
templates + `0x989b/0x98ae` already match stock. So encryption is not a
missing-config cause. Every host layer and the beacon/encryption config now
match stock.

The one instrument left is the per-connection param dump, now with the address:
record `0x10140` (idx 5). Run 11 keeps the stock-matching config and dumps
`0x10100:128` (covers `0x10140`) + `0x168:4`, through the ring:
`0x10152`=+0x12 (arm bit6=0x40), `0x10157`=+0x17=param[0], `0x10158`=+0x18=
param[1] (**bit3 0x08 = the arm gate**), `0x10159..5c`=param[2..5]
(A-0115 gates: param[2]==4b, param[3]&e0==80, param[3]&f>c, param[4]!=11).

| run 11 shows | meaning |
| --- | --- |
| `param[1]` bit3 **clear** | A540 not emitting the bearer attribute; with config/beacon/encryption all matching stock, room compares param[2..5] to the gate constants and traces what in the page-response makes stock's A540 set bit3 |
| bit3 **set**, `+0x12` bit6 never set | arm skipped at the `0xff600c` MAC-HW wait / record mismatch; room traces that branch |

### Runs 11-12: the param dump is not resolvable with memdump (instrument wall)

The connection record re-indexes per run (0x10140 in run 10, 0x10080 in run 11,
0x10140 again in run 12 but split with 0x100c0/0x10040). Using 0x168 to pick the
live record and a wide 0x10000:512 dump, the record the frame pump uses during
the ring is **pure churn**: a stability scan of all 16 chunks shows the active
records (0x10040/0x10080/0x10140) with only 9-18 of 32 bytes constant across
ring samples, while the only fully-stable chunks (0x10180/0x101e0, 32/32) are
idle fill that 0x168 never points to. param[2..5] differs every sample;
param[1] bit3 is set in ~half of both idle and ring samples (random).

This is exactly A-0110's warning: the U-plane records at 0x10000+idx<<6 are
per-frame structures, and memdump revisits each 32-byte chunk ~every 176 ms
against a 10 ms frame, so it samples ~1 frame in 17 and the content aliases to
noise. **The memdump instrument cannot read the live per-connection bearer-event
record.** So A-0118's decisive param[1] bit3 is not obtainable this way.

### Where the investigation stands (honest)

Every host-controllable layer now matches stock, hardware-confirmed, and the
A540 still refuses:
- list sync completes (handset shows "DECT 1"); {CC-SETUP} collision handled;
- attach bytes (0x020A cfg 01, 0x0247 flag 03) match; 0x017c slot mask sent;
- chip gate state 0xdd5=0xdd9=00, 0xdd8=80, 0x989a/b=4b/80 match (read back);
- registration ACCEPTs byte-identical (A-0117);
- call SETUP content e0 88 + e4 41 + no codec-list match (run 9);
- encryption/beacon config all match; the chip reset was a GPIO reset (A-0119).

We KNOW the chip's bearer gate never passes (0x0209 never fires). We cannot see
WHY with our instruments: the per-connection event record is per-frame (memdump
aliases it), and we have no over-the-air DECT monitor to see what the A540
actually emits on the bearer vs on stock. Static RE (the room) has traced the
gate chain to that inbound air event; the host side is exhausted. The honest
next instrument is an over-the-air DECT sniffer pointed at our base ringing the
A540 (or the stock base, to diff). This is a new instrument the project does not
have (no serial/sniffer by project constraint).

### A-0120: the divergence is real-time MAC/RF, below every instrument either room has

The room answered all three:
- **No non-per-frame arm observable.** The `0x0209` mail is the observable; our
  count=0 is the stable "arm never fired" signal we already have. The upstream
  `param[1]` bit3 is written only into the per-frame `0x10000` record (no `0xF6xx`
  stat latches it, and the stable `0x1AB9` record isn't built during an
  unanswered ring). So memdump genuinely cannot see it. Confirmed.
- **Cannot be statically traced.** `param[1]` is the chip's MAC RX of the A540's
  over-air A-field, written by the chip's autonomous bearer handshake below the
  task-2 mail interface. Every host-reachable input matches stock (page
  `mail[2..4]`, beacon `0x14d`, `0xdd5/0xdd9/0x989a-b`). So why stock's A540 sets
  bit3 and ours doesn't is in real-time MAC/RF, not a missing mail.
- **AVM on-box tooling can't log it.** The DECT Monitor (`0xF624/0xF625`) is
  chip status (slot/mode/RSSI/counters), not raw OTA. The DLC C-plane is visible
  (we both have it) but the MAC A-field is below the mail interface. That layer
  needs an RF sniffer.

**Conclusion.** Every host layer, chip-state input, registration, SETUP,
encryption and beacon capability matches stock, and the A540 still refuses; the
one remaining difference is in the chip's real-time MAC/RF bearer handshake,
which neither chip memory (per-frame aliasing) nor AVM on-box tooling can
observe. The honest next instrument is an over-the-air DECT sniffer (or a
reference base to A/B at RF).

The room offered one last cheap host-side catch-all before the sniffer: a full
whole-RAM diff of our refusing-ring chip against stock's working-ring snapshot
(it holds stock's 07.56 ring snapshot + churn mask). Any stable non-identity
byte that differs would be a chip-state divergence not yet checked. It is a
large chip-RAM dump and would need identity/key regions redacted before sharing
with the analysis side; decision pending with the maintainer.

### Run 13: whole-RAM sweeps through the refusing ring (the room's last host-side catch-all, A-0120)

Maintainer approved the whole-RAM diff before conceding an over-air sniffer. The
room holds stock's 07.56 ring snapshot and idle churn mask; our side supplies
full `0x0..0x17fff` sweeps of the refusing A540 ring. Two ranges
(`memdump=0x0:65536`, `memdump=0x10000:32768`; the daemon caps one range at
64 KiB), 10 ms per request = ~31 s per sweep. The ring is placed 65 s after
first contact so two full idle sweeps come first (our own churn mask) and a
post-ring tail (`TAIL=80`) completes the sweep that spans it. Config: the A540's
stock-matching attach values, `signal=0x41`, `basicsvc=0x88`, no call codec-list
(run 9), unchanged.

Identities and keys are redacted locally before anything leaves this repo's
side (IPUI/PARK/RFPI/UAK byte patterns from the corpus, plus the known identity
and UAK-table regions); the room gets a range list of what was blanked so it can
exclude those bytes from its diff.

### Run 13 result: whole-RAM sweeps captured and handed to the room (Q-0057)

`vCOLLIDE-20261024-213717`. The A540 woke late (first contact ~110 s), so the run
holds 14 complete sweeps of `0x00000-0x17fff`: sweeps 1-8 idle, sweep 9 spans the
refused ring (SETUP at t=294987 ms, `93 5a e2 32` again), sweeps 10-14 after.

Instrument check: two idle sweeps differ by ~550-770 bytes, the churn mask over
sweeps 1-8 is 844 bytes (A-0110 measured 985), so the sweeps are sound. Gate bytes
read as stock in every sweep, before/during/after the ring: `0xdb3=0f 0xdd5=00
0xdd8=80 0xdd9=00 0x989a/b=4b/80 0x98ae=00`. Our ring footprint (stable bytes that
change in reads taken after the SETUP) is **66 bytes**; the room's working-ring
delta is 40.

Redaction was done by byte pattern (IPUI, PARK/RFPI, every UAK in the corpus, both
byte orders) plus the UAK table, 11 ranges / 184 bytes, and verified independently
with zero residual matches. The first pass was wrong and caught by that check: the
IPUI regex also matched padded raw C-plane fragment lines, producing 131 junk
patterns that over-blanked RAM and re-matched their own zeroing. Patterns now come
only from complete NWK message lines, with padding-like content excluded. The UAK
turned out to live in a second RAM location (`0xffc-0x100c`) besides the table.

Images, chunk times, blanked ranges and our footprint are in
`the clean-room record/questions/Q-0057_attachments/`; question `Q-0057`. The diff
against stock's ring snapshot minus churn is the room's to run.

| room's diff shows | meaning |
| --- | --- |
| short list of stable non-identity differences | chip-state inputs not yet matched; set the host-reachable ones one at a time |
| differences, none host-reachable | chip-internal state fed by the air; sniffer territory |
| none | every host and chip-state input matches stock; divergence is real-time MAC/RF |

### External review: two eliminations were void, and stock's full SETUP was never sent `[2026-10-24 late, desk]`

An external review of commit `e899958` (`the clean-room record/an external review-24102026.md`)
was checked finding by finding against the code, `docs/specs/` and the
thirteen clean `vCOLLIDE` logs. Three corrections to the record above:

- **"Run 9 sent stock's exact IE set" is wrong.** `[CR A-0117]` lists stock's
  ring SETUP as `e0 88`, `e4 41`, `<<CALLING-PARTY-NUMBER>>` `6c 0d 01 80
  <11 digits>` and a 12-octet `<<CALL-INFORMATION>>` `7e 0c 00 00 82 00 05
  81 01 00 81 02 01 ..` (line 2, line type 1, call id 1, call status with
  its value octet missing from the transcription). Runs 9-13 sent only
  `e0 88 e4 41`. The calling-party sweep (`vCLIP1-3`, 17:56-18:03) ran
  **before** the collision fix (18:35) and the list-sync fix (18:48), so it
  is void by the same rule that reopened the codec test in run 3; it also
  sent `e0 80`, a codec list, `6d`, no `e4`, and a 6-octet `7e` with no call
  id or status. TS 102 527-3 §7.4.5.3.2 and §7.4.6.3 make the line
  identifier and the call identifier mandatory in an incoming `{CC-SETUP}`,
  and figure 50 puts call status "CS call setup" (`0x01`) there too.
- **Transaction value 0 was never tested clean.** EN 300 444 §6.9.2: "the
  lowest available free number". `collide.sh` pins `ti=1`. The only `ti=0`
  run is F4.1 on 2026-10-22, before the drain fix.
- **`0x32` is "insufficient resources"**, not "insufficient bearers
  available" (`0x33`), per the EN 300 175-5 release-reason table.
  `[CR A-0113/A-0114]` used the wrong label.

Fixed on the way, found by the same review: the mail parser's underlength
overflow (`aa41389`) and the end-of-run park that never left the queue
(`8ded31a`). Not causes of the clean refusals, checked against the logs:
the CC transaction-matching defects (`closed_unknown 0`; the handset only
ever sent its LiA SETUP, the list dialog, its RELEASE and the refusal), the
missing LiA-to-voice transition (the LiA call ended before our SETUP in
every run from run 2 on), and the Class B DISC (the release branch ran in
none of the clean runs).

### Runs 14-16: stock's full SETUP, then transaction value 0 `[pre-commit]`

`work/7520/lab/stockset.sh`, one script, each phase a fresh daemon with
`ringattach=40000` and the run-9 configuration (`attcfg=0x01,attflag=0x03,
slotmask=0x0f,signal=0x41,basicsvc=0x88,nocodeclist`) as the base. The
script prints the SETUP's element list from the wire, identities stripped,
so each phase proves its variable arrived. Build: `aa41389` + `8ded31a`.

- **Run 14 (phase A): stock's full SETUP.** Base + `clip=01701234567,
  lineid=1,linetype=1,callid=1,callstatus=1`, `ti=1`. Line 1, not stock's 2,
  because line 1 is the line our list access serves the handset. Expected on
  the wire: `13 05 <ids> e0 88 e4 41 6c 0d 01 80 <11 digits> 7e 0c 00 00 81
  00 05 81 01 00 81 02 01 81`, in whatever IE order our builder uses.
- **Run 15 (phase B): transaction value 0.** Base only, `ti=0`. Expected:
  `03 05 <ids> e0 88 e4 41`; the handset's replies then start `83`.
- **Run 16 (phase C, control): run 9 exactly**, `ti=1`. **Only if A or B
  rings.** Without it a ring could not be attributed: the build changed and
  the handset's state may have.

| phase A shows | meaning |
|---|---|
| **`0x32` with `6c` and `7e` proven on the wire** | **falsifying row**: stock's SETUP content is excluded, on the clean instrument this time |
| `{CC-ALERTING}` | the CAT-iq call information or the calling number was the refusal; C decides attribution; a follow-up separates `6c` from `7e` |
| a release reason other than `0x32` | the content changed the handset's decision; read the reason against EN 300 175-5 before anything else |
| VOID | no contact, no page response or no SETUP; not a result |

| phase B shows | meaning |
|---|---|
| **`0x32` with `03 05` proven on the wire** | **falsifying row**: the transaction value is excluded |
| `{CC-ALERTING}` | GAP §6.9.2 was the refusal; C decides attribution |
| VOID | as above |

| phase C shows | meaning |
|---|---|
| `0x32` | the ring in A or B belongs to that phase's variable |
| `{CC-ALERTING}` | something else changed; the A/B ring is not attributable, and the next step is to find what |

Also read from the same logs: the daemon's new park line, `reg: radio
parked -- park mails on the wire N ms after queueing`, is the hardware check
of `8ded31a`. Its absence, or `RADIO NOT PARKED`, means the fix failed; the
script's separate park still runs either way.

### Runs 14-16 result: THE A540 RINGS on transaction value 0 `[vSTOCKSET-20261024-224320-A/B/C]`

One script, one session, build `aa41389`+`8ded31a`, the A540 on slot 1, each
phase paged ~40 s after the handset's first contact, its LiA call released
before our SETUP in all three (checked from the logs).

| phase | SETUP on the wire (identities stripped) | handset's answer |
|---|---|---|
| A, run 14, `ti=1` | `13 05 <ids> e0 88 e4 41 6c 0d 01 80 <11 digits> 7e 0c 00 00 81 00 05 81 01 00 81 02 01 81` | `93 5a e2 32`, re-presented, refused again |
| **B, run 15, `ti=0`** | **`03 05 <ids> e0 88 e4 41`** | **`83 01 7c 04 90 03 00 81`: `{CC-ALERTING}`, the A540 rang** |
| C, run 16, `ti=1`, control | `13 05 <ids> e0 88 e4 41` | `93 5a e2 32` |

Phase B timeline: page at 47 131 ms, page response and SETUP at 47 735 ms,
`{CC-ALERTING}` at 47 995 ms, 260 ms after the SETUP. The alerting carries a
one-entry `<<CODEC-LIST>>`, G.722 on a long slot: the PT choosing the codec
in `{CC-ALERTING}`, exactly as TS 102 527-1 §7.3.3 NOTE 1 recommends. The
maintainer confirmed the ring at the handset, and confirmed that the control
did not ring.

Read against the pre-commit:

- **Phase B row 2 fired:** EN 300 444 §6.9.2 was the refusal. **Phase C row 1
  fired:** the control, identical to B but for the transaction value, was
  refused in the same session, so the ring belongs to the value. Two
  separate readings agree: B rang and C did not, and all thirteen clean runs
  before them used value 1 and were refused.
- **Phase A row 1 fired (the falsifying row):** with value 1, stock's full
  SETUP content is refused too. The calling number and the CAT-iq call
  information are not what the handset gates on. They remain mandatory for a
  CAT-iq Part 3 external call (TS 102 527-3 §7.4.5.3.2, §7.4.6.3), so they
  are a conformance item for the controller, not a ring item.
- **The chip-level model is falsified.** Phase B rang with **zero `0x0209`**,
  no `0x0208`, and `0x0242` = `01 00 00 00` (signalling-only), exactly as in
  A and C. Neither the `0x0242` byte-1 split (runs 2-4), nor the `0x0209` arm
  `[CR A-0118]`, nor `param[1]` bit 3 `[CR A-0120]` was a precondition for
  ringing. The "real-time MAC/RF, below every instrument" conclusion and the
  sniffer it called for are withdrawn. `Q-0057` (the whole-RAM diff) is moot.
- **`8ded31a` verified on hardware:** every phase logged `reg: radio parked --
  park mails on the wire 4 / 2 / 0 ms after queueing`, the daemon's own park
  now reaching the wire before teardown.

**Why it took this long.** The daemon rotated the transaction value per call
from 2026-10-20 (`a16ec41`), two days before the A540 arrived, so no Gigaset
was ever offered value 0 by default. The `-a ti=` comment noted on
2026-10-22 that every completed call had used 0 and every refused one 1; the
one `ti=0` run that day (F4.1) was under the drain defect, its result void,
and the lab scripts pinned `ti=1` afterwards. The C4 accepts value 1, which
is why the C4 always rang.

**Fixed:** `dd5a630` makes 0 the default (`ring_queue_setup()`), corrects
the library comment that called rotation "what the standard expects", and
adds `test_transaction_value_is_lowest_free()`. The lab scripts' `ti=1`
pins are removed.

**Still to confirm:** one ring on the fixed default build with no `ti=`
option. Run 15 is one sample of a ring; the fix should make it the default
behaviour, and that wants a second sample before defect 1 is called closed.

### Runs 17-18: confirm the default, then ask whether the value alone suffices `[pre-commit]`

Build `dd5a630` on the box (md5 `b9937eb4…`), `work/7520/lab/confirm.sh`,
same harness as runs 14-16 (`flags=0x41,mask=0x0002,codec=3,status=8000,
noremode,setuptries=40,ringattach=40000`), **no `ti=` in either phase**.
The maintainer is at the handset and reports whether it rings audibly.

- **Run 17:** run 9's settings (`attcfg=0x01,attflag=0x03,slotmask=0x0f,
  signal=0x41,basicsvc=0x88,nocodeclist`). Expected on the wire:
  `03 05 <ids> e0 88 e4 41`. This is run 15 again, on the default value.
- **Run 18:** the daemon's plain defaults. Expected: `03 05 <ids> e0 80 7c
  04 90 02 00 84` (narrowband, G.726 only, **no `<<SIGNAL>>`**), attach bytes
  `0x00`, slot mask `0x09`.

| run 17 shows | meaning |
|---|---|
| `{CC-ALERTING}` (`83 01`) | second sample; the fix works as the default. Defect 1 closed |
| **`0x32` with `03 05` on the wire** | **falsifying row**: run 15 did not reproduce; the value is not the whole story and defect 1 reopens |
| `13 05` on the wire | the default did not take; read the build and the option string before anything else |
| VOID | not a result |

| run 18 shows | meaning |
|---|---|
| `{CC-ALERTING}` and an audible ring | the transaction value alone was the defect; the run-9 extras are not needed to ring |
| `{CC-ALERTING}`, no audible ring | the handset accepts but needs `<<SIGNAL>>` to alert (EN 300 444 §8.14); make it the default |
| `0x32` | something in the run-9 extras is needed too; bisect, `<<SIGNAL>>` first |
| another release reason | read it against EN 300 175-5 before anything else |

### Runs 17-18 result: the default rings, and without `<<SIGNAL>>` it rings silently `[vCONFIRM-20261024-230218-17/18]`

| run | SETUP on the wire (identities stripped) | handset | at the handset (maintainer) |
|---|---|---|---|
| 17, run-9 settings, no `ti=` | `03 05 <ids> e0 88 e4 41` | `83 01` `{CC-ALERTING}` | **rang aloud** |
| 18, plain defaults, no `ti=` | `03 05 <ids> e0 80 7c 04 90 02 00 84` | `83 01` `{CC-ALERTING}` | **ringer icon, green button blinking, no sound** |

- **Run 17, row 1:** second sample of a ring on value 0, this time from the
  default rather than an override. **Defect 1 is closed.**
- **Run 18, row 2:** the transaction value alone makes the A540 accept the
  call, with narrowband basic service, a G.726-only codec list and zero
  attach bytes. None of the run-9 extras is needed to be accepted. But with
  no `<<SIGNAL>>` the handset alerts silently. EN 300 444 §8.14 makes
  alerting the FT's job; the library's comment that "the handset picks" was
  wrong. Fixed in `9c4414c`: `<<SIGNAL>>` 0x41 by default, `-a nosignal` for
  control runs.
- Also on the box since this session, not yet on hardware: `0ac7e38`, CC
  messages matched to their transaction.

### Run 19: the new default build, answered `[pre-commit]`

Build `9c4414c` (md5 `866fdd06…`), plain defaults, no options beyond the
harness. Expected SETUP: `03 05 <ids> e0 80 e4 41 7c 04 90 02 00 84`. The
maintainer answers when it rings; the script holds the call for that.

| run 19 shows | meaning |
|---|---|
| audible ring, `{CC-CONNECT}` answered with our `{CC-CONNECT-ACK}` | both changes work on hardware; the transaction matching passes the handset's real CONNECT |
| audible ring, CONNECT but **no** CONNECT-ACK from us | **falsifying row** for `0ac7e38`: the matching rejects our own call's CONNECT |
| silent ring with `e4 41` on the wire | `<<SIGNAL>>` 0x41 is not enough; try a `{CC-INFO}` alert |
| no answer within the hold | ring checked, connect path untested; not a failure |

### Run 19 result: rings aloud on the plain default build, and is answered `[vANSWER-20261024-230837-19]`

SETUP `03 05 <ids> e0 80 e4 41 7c 04 90 02 00 84`, `{CC-ALERTING}`, the
maintainer answered, `{CC-CONNECT}` from the handset and our
`{CC-CONNECT-ACK}` (1 each), no release from us, `closed_unknown 0`; the
handset showed a running call timer. Row 1: `9c4414c` (default
`<<SIGNAL>>`) and `0ac7e38` (transaction matching) both work on hardware.
The call ended when the script parked the radio after its 10 s hold, as
designed. **First answered FP-originated call on a Gigaset.**

### Run 20: the review-fix batch on hardware `[pre-commit]`

Build `02b9039`, plain defaults, `work/7520/lab/batch.sh` (run 19's script,
relabelled). New since run 19: the LOCATE-REQUEST identity check
(`7e7e5f4`), atomic store (`c615aa9`), TPUI ACK check (`edaaeb3`), SAPI
check (`2fd24ae`), reassembly cut (`e5bc10b`), mail-spec validation
(`ea52963`), segment marker/handle check (`398d1f7`), transmit commit
(`35a7c20`), release-handle check (`12cb980`), page-response identity check
(`02b9039`). Checked at the desk: the A540's page-response identity equals
the identity we ring, so `02b9039` should accept it.

| run 20 shows | meaning |
|---|---|
| `{LOCATE-ACCEPT}` to the A540, page response accepted, audible ring, CONNECT and our CONNECT-ACK, `write_short 0`, park line present | the batch works on hardware |
| **a `{LOCATE-REJECT}` to the A540, or no attach** | **falsifying row for `7e7e5f4`**: the identity check rejects our own handset |
| **"page response from a handset we did not page"** | **falsifying row for `02b9039`** |
| "release of handle N ignored" | `12cb980` acted; read the context before trusting the run |
| `write_short` > 0 | the driver refused a write; check the mail stream stayed intact |
| no answer within the hold | ring checked, connect path untested; not a failure |

### Run 20 result: the review-fix batch works on hardware `[vBATCH-20261024-232726-20]`

Row 1. Build `02b9039`, plain defaults. The A540 took 135 s to make contact
(its usual slow re-lock), then: `{LOCATE-ACCEPT}` 1 and `{LOCATE-REJECT}` 0
(the new identity check passes our handset), page response accepted (no
"did not page" line), SETUP `03 05 <ids> e0 80 e4 41 7c 04 90 02 00 84`,
`{CC-ALERTING}` at 202.5 s with an audible ring, `{CC-CONNECT}` at 229.2 s
answered with our `{CC-CONNECT-ACK}`, no release from us, `closed_unknown 0`,
no ignored chip release, `queue_drops 0`, `write_short 0` over 61 557 blocks,
and the daemon's own park on the wire 3 ms after queueing.

**The call lasted 22 s at the handset, not the script's 10 s hold.** Our
radio-off went out 16.9 s after the handset's CONNECT: the script counts
5 s per poll but each poll also makes several SSH calls, so its "10 s" is
about 13 real seconds, plus up to one poll of detection delay. The
remaining ~5 s is the A540's own link supervision noticing the base had
gone. Nothing is wrong; the hold is looser than its label.

### Runs 21-24: register and ring the C430A and the AS690H `[pre-commit]`

Both factory-reset by the maintainer. Build `35ef02b` (adds arming the
identity gate when a handset registers, so a later link's LOCATE-REQUEST
is not refused). `regall.sh c430a` (slot 2) and `regall.sh as690h`
(slot 3), each followed by `ringone.sh <name>`: plain defaults, answered
if the maintainer can. Their earlier refusals (2026-10-24 morning) are
void: their stores carried the A540's identity. These are their first
valid tests.

| registration shows | meaning |
|---|---|
| "handset registered", identities derived, store saved, `{LOCATE-ACCEPT}` | enrolled; ring next |
| `{LOCATE-REJECT}` after the accept of access rights | the identity check rejects a fresh registration: falsifies `7e7e5f4`/`35ef02b` |
| no registration | read the log before retrying |

| ring shows | meaning |
|---|---|
| audible ring, CONNECT and our CONNECT-ACK | the fix generalises: three Gigaset models, one rule |
| **`0x32` with `03 05` and `e4 41` on the wire** | **falsifying row**: model-specific; the transaction value is not the whole story for this handset |
| silent ring | `<<SIGNAL>>` not honoured by this model; try a `{CC-INFO}` alert |
| another release reason | read it against EN 300 175-5 first |

### Runs 21-22 result: the C430A rings; the AS690H's run is void `[2026-10-24 23:38-23:51]`

Registration: both enrolled on the first attempt, `{ACCESS-RIGHTS-ACCEPT}`,
`{LOCATE-ACCEPT}`, `{TEMPORARY-IDENTITY-ASSIGN-ACK}`, store saved, radio
parked (`vREGC430A233816`, `vREGAS690H234204`). The handsets already
enrolled were turned away at location registration with "IPUI unknown",
which EN 300 444 §8.28.2.1 recommends; the handset keeps its LAL and
retries later, and nothing in the clause makes it drop its subscription.

| run | handset | log | outcome |
|---|---|---|---|
| 21 | C430A | `vRINGC430A-20261024-234854-1` | **rang, answered**: ALERTING, CONNECT, our CONNECT-ACK; SETUP `03 05 05 <id> 06 <id> e0 80 e4 41 7c 04 90 02 00 84`, the A540's bytes |
| 22 | AS690H | `vRINGAS690H-20261024-235046-1` | **void**: released `0x08` "invalid identity"; our SETUP addressed the A540 |

Run 22, by identity (hashes, not bytes): the AS690H enrolled as `deadbe01`;
our SETUP carried `deadbe02`, which is the A540 (run 20 paged and rang it
under that identity). The registration log has the daemon "learned the
identities for calling the handset" at line 92, from the A540's
list-access `{CC-SETUP}`, while the AS690H's `{ACCESS-RIGHTS-REQUEST}`
arrives at line 443. With no identity armed yet, `dect_cc_peer_is()` said
yes to anyone. `35ef02b` armed the gate after registration and left the
window before it open. **This is the same fault that voided both handsets'
morning runs**; the C430A escaped it tonight only because the A540's call
came after its registration.

Neither pre-committed ring row applies to run 22: the variable never
reached the handset. On the re-run list.

The same log explains the maintainer's report that the A540 showed
"DECT 1", "DECT 3", then "DECT 2": the daemon served the A540's
list-access call the Internal Names List of whichever handset's store was
loaded, own-flag set. One cause for both, the identity gate covering
`{LOCATE-REQUEST}` and nothing else. Fixed in `bc64850` (API 11): `dect_link`
gates every PT `{CC-SETUP}` on `mm.port_id`, answering others with
`{CC-RELEASE-COM}` "unknown identity" `0x0A` (EN 300 444 table 17); the
daemon learns call identities only once an identity is armed, and drops
stored ones that are not the subscribed handset's.

**Also found: the UAK read-back has been reading the wrong memory since the
07.56 STUB went in** (~17:20). A-0085's `0x9518` is the 08.25 build's
address ("Sources: chip firmware (08.25, CR16C)"). Every read since returns
the same table, `0c 84 02 c9`, `0c 87 02 c9`, ... rising by 3, in every
slot, so every `MISMATCH` and `FOREIGN SLOT(S) STILL HELD` since then is
meaningless, not a finding. The handsets attaching shows the keys land.
Asked as Q-0059. And `-p` printed the UAK in the clear, contrary to what
`verification-gap.md` promised; masked in `509ea8a`.

### Run 22b: the AS690H again, on `bc64850` `[pre-commit]`

Store repaired at load, identities derived from the subscription. Expected
in the log first: `stored call identities belong to ANOTHER handset --
dropped`, then `derived the identities ... from the subscription`.

| ring shows | meaning |
|---|---|
| SETUP portable identity is not `deadbe01` | void again: the repair did not take; stop and read |
| audible ring, CONNECT and our CONNECT-ACK | run 22 was the poisoned store alone; three Gigaset models ring on one rule |
| **`0x08` again, SETUP carrying `deadbe01`** | **falsifying row**: the derived identity encoding is wrong for this handset, not the store |
| `0x32` with `e4 41` on the wire | the original falsifying row: model-specific |

And the name check, same run: if the A540 calls, the log shows `turned away
a call from a handset we are not subscribed to`, and the A540 does **not**
change its name to "DECT 3". If it does, the name reaches it by a path
other than its `{CC-SETUP}` (an `{IWU-INFO}` outside a call, say), and the
fix is incomplete.

### Run 22b result: the AS690H rings `[2026-10-25 00:00, vRINGAS690H-20261024-235929-1]`

| check | result |
|---|---|
| store repair at load | `stored call identities belong to ANOTHER handset -- dropped`, then `derived the identities ... from the subscription, saving` |
| SETUP portable identity | `deadbe01`, the AS690H's own |
| SETUP on the wire | `03 05 05 <id> 06 <id> e0 80 e4 41 7c 04 90 02 00 84`, the same bytes that ring the A540 and the C430A |
| outcome | **ALERTING, CONNECT, our CONNECT-ACK**: rang and answered; parked 2 ms after queueing |
| location registration | the AS690H accepted, the C430A turned away (`IPUI unknown`) |

Pre-committed row: "run 22 was the poisoned store alone; three Gigaset
models ring on one rule". **The A540, C430A and AS690H all ring on
transaction value 0 with `<<SIGNAL>>` 0x41.** The falsifying rows (`0x08`
on the right identity, `0x32` with `e4 41`) did not occur.

**The name check did not run:** the A540 never contacted the base in this
run (no `{LOCATE-REQUEST}` from `deadbe02`), so nothing was turned away. It
stays owed. Its `{LOCATE-REJECT}` in the runs before tells it not to try
again until the conditions of EN 300 175-5 §13.2 are met, which would
explain the silence. Nor has a subscribed handset's own `{CC-SETUP}` passed
the new gate on hardware yet: neither the C430A nor the AS690H placed one
in its run. Both are owed by the next A540 run, whose list-access call
precedes every ring.

### Runs 23-24: the name check and the gate's other side `[pre-commit]`

Build `bc64850`, one attended session. The A540 shows "DECT 2" (given by
the old build during a C430A run). Before each phase the maintainer notes
its display, and presses a key on the A540 the moment the radio is up, which
also tests the maintainer's suggestion that a Gigaset searches for a lost
base on its own only slowly, but at once when woken. The A540's first
contact is its first `{LOCATE-REQUEST}` (`deadbe02`).

**Run 23, the AS690H's store (slot 3), the AS690H rings as control.** It
has to be slot 3: under the C430A's store the old build would also have
said "DECT 2", and the check would not discriminate.

| shows | meaning |
|---|---|
| A540's `{CC-SETUP}` answered `{CC-RELEASE-COM}` `0x0A`, the A540 still says "DECT 2", the AS690H rings | the gate works on hardware, and the SETUP was the only path to the name |
| **the A540 says "DECT 3"** | **falsifying row**: the name reaches it another way (an `{IWU-INFO}` outside a call, say); the fix is incomplete |
| the A540 never contacts within the run | void for the name check, and evidence against "a key press makes it search" |
| the AS690H does not ring | a regression in `bc64850`; read that before anything else |

**Run 24, the A540's store (slot 1), the A540 rings.**

| shows | meaning |
|---|---|
| its list-access `{CC-SETUP}` taken (CALL-PROC, no "turned away" for it), it shows "DECT 1", rings and is answered | the gate admits the subscribed handset; the first hardware proof of that side |
| **its `{CC-SETUP}` turned away** | **falsifying row**: the IPUI in its SETUP does not equal the one it located with, so GAP table 10's premise fails on this handset |
| rings, name unchanged | its list sync did not finish; read the `{IWU-INFO}` exchange |

Contact time in both: the A540's first `{LOCATE-REQUEST}` within ~15 s of
the key press supports the suggestion; 60 s or more argues against it.

### Runs 23-24 result: the gate holds on both sides; a held handset still suffers `[2026-10-25 00:04-00:08]`

| run | store | log | result |
|---|---|---|---|
| 23 | AS690H (slot 3) | `vRINGAS690H-20261025-000412-1` | AS690H **rang, answered**; the A540 placed **20** list-access `{CC-SETUP}`s, all 20 answered `{CC-RELEASE-COM}` `0x0A`; zero `{IWU-INFO}`, zero CALL-PROC sent; two foreign link releases ignored |
| 24 | A540 (slot 1) | `vRINGA540-20261025-000628-1` | the A540's `{CC-SETUP}` **admitted** (CALL-PROC), identities learned and matching, Internal Names served (`44 45 43 54 20 31`, "DECT 1"); the A540 shows **"DECT 1"**; **rang, answered** |

The maintainer pressed a key on the A540 as the radio came up: first contact
2.6 s (run 23) and 1.7 s (run 24), against 162 s for run 20, where nobody
touched it after 17 minutes off. That supports the suggestion that a Gigaset
searches for a lost base slowly on its own and at once when woken. Two
samples, both with a key press; a run without one, after the same time off,
would make it a comparison.

Pre-committed rows: run 23 did not show "DECT 3", so the `{CC-SETUP}` was
the only path to the name. Run 24 admitted the subscribed handset's own
`{CC-SETUP}`, the first hardware proof of that side of the gate; GAP table
10's premise holds on the A540 (same IPUI in `{LOCATE-REQUEST}` and
`{CC-SETUP}`). **All four Gigaset rings of runs 21-24 used the same SETUP
bytes, `03 05 05 <id> 06 <id> e0 80 e4 41 7c 04 90 02 00 84`.**

**One row the pre-commit got wrong.** It said the A540 would *keep* "DECT 2"
in run 23. It dropped the name instead and showed "Synchronisiere"
roughly every 4 s, its list sync failing over and over: 20 attempts in one
minute. Turning it away is right for a handset the base does not know. But
the A540 *is* subscribed to this base; only the daemon, which loads one
store at a time, does not know that. **The one-store daemon is wrong for any
base with more than one handset**: every handset whose store is not loaded
is locked out of location registration and list access, and burns air time
and battery retrying. The next structural step is to load every store: the
gate admits any held identity, the Internal Names List lists every handset
with the reader's own entry flagged, and the chip holds every UAK instead of
one.

### Run 25: audio on a Gigaset, the C4's recipe unchanged `[pre-commit]`

Defect 2 was solved on the C4 (2026-10-23, `vF413`/`vF411`): mode 2 via the
bit-6-clear `0x0230` re-assign 500 ms after `0x022D`, and signed 16-bit
linear PCM on the lane, one sample per frame. It has never run on a Gigaset,
and never on the 07.56 STUB: all of that work was on 08.25.

Build `bc64850`, 07.56, the A540 with its own store, our default SETUP
(G.726 only, so narrowband, which is what mode 2 is). Downlink
`playraw=/tmp/linlane.raw` (19.7 s of speech) for the whole call; uplink
recorded with `-w`. The maintainer answers, listens for ~20 s, then talks
continuously for ~30 s; the call holds 60 s. No memory dumps: the route-word
addresses are the 08.25 build's, like the UAK table's.

Pre-flight: the log must say `CC-SETUP queued (page response received)`. If
it says `a link was already up`, the run carries no audio information.

| earpiece | meaning |
|---|---|
| the speech sample, intelligible | the recipe carries over to a Gigaset and to 07.56; defect 2 closes for narrowband |
| noise | mode 2 or the lane format is not what this build or handset wants; next, the same run on 08.25 to separate the two |
| silence, call up | the path is not bound; read the `0x022D`/`0x0230` order and timing |

**Falsifying row for "the recipe is handset-independent":** no speech on
07.56 *and* on 08.25.

Uplink: decode the talk window as signed 16-bit linear and as A-law in the
low byte (`tools/voice-decode.py`). `A-0044` leads with linear; speech under
A-law instead would correct it.

### Run 25 result: audio on a Gigaset, both directions `[2026-10-25 00:18, vRINGA540-20261025-001831-1]`

First attempt (`-001617`) void: with `-w` the daemon budgets capture space
for the whole `-t 900` run (109 MiB against 93 free) and refused to start,
before the radio loop. Correct refusal; rerun with `MAXSECS=300`.

Pre-flight passed (`CC-SETUP queued (page response received)`); the box
reported `STUB second stage dectfw_secondlevel_441-07.56.hex` and skipped
the 08.25-only read-back (`c96dd40`).

| direction | result |
|---|---|
| **downlink** | **the maintainer heard the speech sample clearly** for the whole call |
| **uplink** | slot 0 (channel 1) is **signed 16-bit linear PCM, little-endian, one sample per frame**: voice-decode scores it 2.60 against <0.8 for every other reading; quiet near −70 dBFS for 1-20 s while the maintainer listened, −16 to −30 dBFS from 21 s when they talked (they said "20+ seconds") |
| **first ~0.5 s** | a burst of white noise in the earpiece; the uplink shows a constant −42.1 dBFS floor with bursts until ~550 ms, then the room floor |

Timeline: CONNECT and our first `0x0230 {41 0002 00 03}` at 43653 ms, `0x022D`
at 43681 (lane live, playback restarts), re-assign `{01 0002 00 03}` at
44181. **For 500 ms after `0x022D` the chip is in mode 9**, where the C4
taught us a linear lane is noise, and the uplink is in a different state
too. The noise is ours, not the recording's.

Pre-committed row: "the recipe carries over to a Gigaset and to 07.56;
defect 2 closes for narrowband". **It does, both directions.** `A-0044`'s
leading uplink candidate, linear, is right; A-law did not score.

### Run 26: does the chip reach mode 2 without our re-assign? (`F4.17` on the A540) `[pre-commit]`

`A-0046`/`A-0047`: with bit 6 clear in the **first** `0x0230`, the chip's
bearer-active event configures mode 2 itself once `0x022D` sets path-on,
and the 500 ms re-assign exists only to win a race. `F4.17` was written on
2026-10-23 and never run. Same as run 25 except `flags=0x01`, `noremode`.
No route words (08.25 addresses), so the ear and the uplink capture decide.
The maintainer listens closely to the first two seconds.

| shows | meaning |
|---|---|
| speech from the first moment, no burst; uplink without the 550 ms floor | the chip self-configures mode 2; the re-assign and its timer cause the burst and go (after a confirming run) |
| burst, then speech | it configures mode 2 itself, but later; time the burst against `0x022D` |
| **noise or silence for the whole call** | **falsifying row**: only the explicit re-assign reaches mode 2; it stays, and the burst is fixed by holding the lane until it has gone out |

### Run 26 result: one `0x0230` with bit 6 clear reaches mode 2, and the burst is gone `[2026-10-25 00:24, vRINGA540-20261025-002335-1]`

Mails: `0x0251 {01 01 2b}`, `0x0230 {01 0002 00 03}` at 60821 ms, `0x022D`
at 60857, **no re-assign**. The maintainer heard the speech sample **from
the first moment, no noise burst**, and hung up after ~10 s (handset
`{CC-RELEASE}` "normal", our `{CC-RELEASE-COM}`).

Uplink, slot 0 as linear PCM: a ~100 ms bearer-start transient no louder
than −41.5 dBFS, the room floor by 300 ms. Run 25 had a constant floor with
bursts up to −11 dBFS until ~550 ms.

Pre-committed row: "the chip self-configures mode 2; the re-assign and its
timer cause the burst and go (after a confirming run)". **That is what
happened.** `A-0046`/`A-0047` were right: path-on from `0x022D`, then the
chip's own bearer-active event configures the channel from the first
assign's flags. `F4.16` (no `0x022D` at all) is not needed and stays unrun.

### Run 27: the same shape on the AS690H `[pre-commit]`

The confirming run, on a second handset model. Identical options to run 26.

| shows | meaning |
|---|---|
| speech from the first moment, no burst | confirmed on two models: `flags=0x01` and no re-assign become the defaults |
| **burst or noise** | **falsifying row**: model-dependent; keep both shapes and find out what the AS690H needs |
| silence | the AS690H's bearer does not trigger the configurator the same way; keep the re-assign as a fallback |

### Run 27 result: confirmed on the AS690H `[2026-10-25 00:27, vRINGAS690H-20261025-002644-1]`

One `0x0230 {03 01 02 00 00 03}` (handle 3) and `0x022D` 32 ms later, no
re-assign. **Speech from the first moment, no burst**, by the maintainer's
ear. Uplink slot 0, linear PCM (score 2.53): a ~100 ms transient peaking at
−34 dBFS, quiet while they listened, their voice at −13 to −21 dBFS from
8 s. They also remarked that the handset's call timer runs: that is the
handset's own, started at our CONNECT-ACK; we send nothing for it.

Pre-committed row: "confirmed on two models: `flags=0x01` and no re-assign
become the defaults". Done in `7093ae3`: the re-assign now defaults on only
after a bit-6 bind; `ringone.sh` stopped passing `flags=0x41,noremode`.

### Run 28: the new default, no audio options, on the C430A `[pre-commit]`

Build `7093ae3`, `ringone.sh c430a` with only `playraw` and `-w`. The first
audio test of the third model, and the check that the default sends what
runs 26-27 sent.

| shows | meaning |
|---|---|
| one `0x0230` with flags `01`, `0x022D`, no re-assign; speech from the first moment; uplink speech in slot 0 | the default is right, and narrowband audio works on all three Gigasets |
| a second `0x0230` in the log | the default did not take; the run says nothing about the C430A |
| **burst, noise or silence with the right mails** | **falsifying row**: model-dependent after all |

### Run 28 result: the default is right; narrowband audio on all three Gigasets `[2026-10-25 00:32, vRINGC430A-20261025-003106-1]`

No audio options but `playraw` and `-w`. The daemon sent one
`0x0230 {02 01 02 00 00 03}` (handle 2, flags `01`), `0x022D` 36 ms
later, no re-assign: the default arrived. The maintainer: **"Audio is
clean"**, no burst. Uplink slot 0, linear PCM (score 2.43): 100 ms at
−47 dBFS, then exact zeros and the room floor; their voice from 8 s at
−5 to −20 dBFS.

Pre-committed row: "the default is right, and narrowband audio works on all
three Gigasets". **Defect 2 is closed for narrowband on the A540, C430A and
AS690H**, both directions, on the 07.56 STUB and the default build.

Open, from the maintainer's ear in runs 27 and 28: "sounds like loopback is
on". We put only the file on the downlink lane, so it is the handset's
sidetone or a path inside the chip. Immediate means sidetone; a delay of
tens of milliseconds would mean the chip mixes the uplink into the downlink.

**Settled:** the maintainer heard their voice immediately, not delayed. It is
the handset's sidetone; nothing in the base loops the uplink back.

### Runs 29-30: every store loaded, and the LED `[2026-10-25 ~01:10, unattended, vVFYMULTI-*, vVFYLED-*]`

The maintainer allowed unattended verification runs (handset ringers at
minimum, park checked after each). Builds `dce3ad7`/`e2b3417`, all three
stores loaded (`-s a540 -s c430a -s as690h`), no ring.

Run 29 (120 s):

| check | result |
|---|---|
| chip | `cleared chip subscription slots other than 1 2 3`; C430A and AS690H installed in slots 2 and 3, the A540 in 1 |
| location registration | **all three accepted**, each with its own TPUI (`00 11 01/02/03`); **zero** `{LOCATE-REJECT}` |
| adoption | `now serving ...as690h (slot 3)`, back to the A540 on release, `now serving ...c430a (slot 2)` |
| A540 list-access call | admitted; identities learned into **its own** store; Internal Names List with **three** entries: `1` "DECT 1" own (`b0`), `2` "DECT 2", `3` "DECT 3" (`a0`) |
| gate events | zero turned-away calls, zero ignored connection-ups |
| park | parked 4 ms after queueing; no daemon left; LED 0 |

Against run 23, where the A540 was turned away twenty times a minute, this
is the fix working: the base serves every handset it holds.

Run 30 (90 s, LED count added): `led: 41 changes, 33 blink ticks of 100 ms`
for 84 received segments in four bursts (the three handsets locating, the
A540's list access), LED 0 after the park. On while up, blinking during
traffic, off when parked: as the maintainer asked.

Still unverified on hardware: `{LCE-PAGE-REJECT}` (needs a foreign page
response) and the codec in our CONNECT (needs a person to dial out).

### Runs 31-33: the conformant SETUP, no codec list `[pre-commit, unattended]`

TS 102 527-1 §7.3.3: a <<CODEC-LIST>> in `{CC-SETUP}` shall contain at least
G.722 and G.726, and a list of only the preferred codec is not allowed. Our
default sends G.726 alone (`7c 04 90 02 00 84`). The conformant narrowband
form is no list and basic speech (`e0 80`), whose default is G.726. Build
`e2b3417`, every store loaded, `-a nocodeclist`, one ring per handset,
~5 s of ringing (ringers at minimum), nobody answers: ALERTING decides.

| shows | meaning |
|---|---|
| ALERTING from all three, SETUP without `7c` | the conformant form rings; the G.726-only list leaves the default |
| **a refusal from any of them** | **falsifying row**: that handset needs a list; the default keeps it and the non-conformance is recorded as interoperability |
| no SETUP sent / not paged | void; read the ring path before anything else |

### Runs 31-34 result: the conformant SETUP rings all three; a paging bug found on the way `[2026-10-25 01:00-01:20, unattended]`

| run | handset | build | result |
|---|---|---|---|
| 31 | A540 | `e2b3417` | **ALERTING** to `03 05 05 <id> 06 <id> e0 80 e4 41` (no `7c`) |
| 32 | C430A | `e2b3417` | **void**: paged as `0c 11 01`, the A540's TPUI |
| 33 | AS690H | `e2b3417` | **void**: same |
| 32b | C430A | `3f91e4c` | paged `0c 11 02`; **ALERTING**, no list |
| 33b | AS690H | `3f91e4c` | paged `0c 11 03`; **ALERTING**, no list |
| 34 | A540 | `9bfd0a4` | the new default, no options: **ALERTING**, SETUP without `7c` |

Each rang ~5 s at minimum volume, nobody answered, every run parked and
checked (no daemon left, LED 0).

**Runs 32-33, the bug multi-store introduced:** the page took its TPUI from
the link's MM, which had adopted the A540 when it placed its list-access
call; switching the daemon back to the ring target on release left the link
MM alone. The A540 answered a page for its own TPUI, correctly, and we
answered with `80 72 05 07 <its IPUI>` and released the link: **the first
`{LCE-PAGE-REJECT}` on hardware**, right message for a wrong page. Fixed in
`3f91e4c` (the page uses the target's TPUI by MM's rule for its slot).

Pre-committed row: "ALERTING from all three, SETUP without `7c` -- the
conformant form rings; the G.726-only list leaves the default". Done in
`9bfd0a4`, verified by run 34.

Owed by a person: audio on a call with the new SETUP (basic speech, no
list, should be the same G.726 mode 2 as runs 25-28).

### Run 35: side links during a call `[2026-10-25 01:22, unattended, vRINGA540-20261025-012157-1]`

Build `90588c4`. Ring the A540 3 s after first contact, so the other two
handsets are still locating while our call holds the main link (handle 1).

| t | event |
|---|---|
| line 449 | `CC-SETUP queued (page response received)` on handle 1 |
| 480 | `side: connection up (handle 0) -- handle 1 is busy, so it gets a side link` |
| 498 | `side: handle 0 serves ...as690h (slot 3)`; its `{LOCATE-ACCEPT}` on the side link |
| 519 | side link released |
| 535-553 | the same for the C430A (slot 2) |
| | the A540 **ALERTING**; parked, no daemon left, LED 0 |

Zero ignored connection-ups (runs 25-28 ignored 1-6 per call). Both
handsets located during our call, each as itself. Still one call at a time.
