# Handset registration (M6 / gate G6) — groundwork

Status: started 2026-10-18, right after G5 passed. Transport facts below are
**observed on hardware** (spare 7520, FRITZ!Fon C4 registration attempt). The
NWK/MM/DSAA protocol itself is **public** (ETSI EN 300 175-4/-5/-7; DSAA was
reverse-engineered and published — deDECTed.org, the CT-RSA 2010 paper, osmocom
`libdect`, and the Linux kernel `net/dect` stack). G6 builds from those public
sources, **not** from AVM material, so it stays outside the clean-room boundary.

Identity note: the captured C-plane bytes, the handset tag and the RFPI are
identity data and are **redacted** here (shown as placeholders). Never commit them.

## What a C4 registration attempt looks like on our interface (observed)

After the base is up (`0x015B:01` + `0x022B:41`, see `mac-bringup.md`) and the
user triggers "register to Basis 3" on the C4, the STUB reports, via the control
slot, a **connection-oriented** access (not the short-form `0xF604`/`0x0E0D`):

```
rx 0x0200  (dst 0x03) CONNECTION_ESTABLISHED  {handle=0, wideband=0}
rx 0x0243  (dst 0x03) CONNECTION_PARAMETER    {handle=0, 0x29, 0x00}
rx 0x0242  (dst 0x10) AUDIO_CHANNEL_STATE     {handle=0, 0,0,0}
rx 0x0203  (dst 0x03) SIGNALLING_SEGMENT      (15 param bytes, see below)  ×N
rx 0x0201  (dst 0x03) CONNECTION_RELEASED     {handle=0}
```

The handset sends its first C_S (Cs) channel segment, then **retransmits the
exact same segment several times** and the connection is released — because our
side never acknowledges it. This is DECT LAPC **acknowledged** C-plane: the PP
will not advance to the next segment until the FP acknowledges the first. So G6
cannot be skipped at the DLC level; we must run the LAPC handshake to even see
the rest of the `ACCESS-RIGHTS-REQUEST`.

### `0x0203` SIGNALLING_SEGMENT param layout (observed, 15 bytes)

| Off | Size | Field | Observed |
|---|---|---|---|
| 0 | 1 | handle | `00` |
| 1 | 1 | marker | `01` |
| 2 | 5 | **C_S segment** (40-bit Cs tail) | `<cs[5]>` (redacted; same in every retransmit) |
| 7 | 2 | zero | `00 00` |
| 9 | 3 | handset tag (PMID-like) | `<tag[3]>` (redacted) |
| 12 | 1 | handle | `00` |
| 13 | 2 | ref/frame (first seg only) | non-zero on seg 1, `00 00` on retransmits |

This matches COMMAND_SET §8.2 `{handle,0x01,data[5],00,00,tag[3],handle}` plus a
2-byte trailer. The **5-byte `data` is one Cs segment**; reassemble consecutive
segments into an LAPC frame (ETSI EN 300 175-4), whose I-field is the NWK message.

## The G6 build

Architecture — new `libopendectcell` modules, injected transport + clock so they
unit-test on the PC like the M4 modules (`dect_mail`/`dect_session`):

0. **`dect_cs` (STUB-facing C-plane transport).** ✅ Built 2026-10-18
   (`src/libopendectcell/dect_cs.{c,h}`, PC-tested). Decodes inbound `0x0203` into
   `{handle, tag, 5-byte Cs segment}`; encodes outbound C-plane as `0x0244`
   APPEND_CONNECTION_DATA (per-connection buffer, ≤205 B) + `0x0202`
   TRIGGER_SIGNALLING [CR COMMAND_SET §6.4/§8.2]. **Outbound path confirmed:**
   C-plane signalling goes through `0x0244`/`0x0202`, *not* `0x015C`
   SEND_ACKED_DATA — the latter is 40-byte **B-field** reliable data addressed
   by tag [CR COMMAND_SET §6.4]. The STUB segments the appended byte stream into
   Cs over the air; inbound it hands us one 5-byte segment per `0x0203`, so
   reassembly is the host's job (next module).

1. **`dect_dlc` (LAPC, C-plane, acknowledged).** Reassemble inbound Cs segments
   (from `dect_cs`) into LAPC frames; generate acknowledgements and outbound
   frames; produce the outbound byte stream for `dect_cs_append_mail`. ETSI
   EN 300 175-4. Reference: kernel `net/dect/dlc_*`, osmocom `libdect` DLC.
2. **`dect_mm` (NWK Mobility Management registration FSM).** Drive
   `ACCESS-RIGHTS-REQUEST` → `AUTHENTICATION-REQUEST` (RAND/RS) →
   `AUTHENTICATION-REPLY` (RES) → `ACCESS-RIGHTS-ACCEPT`. ETSI EN 300 175-5.
   IEs: PORTABLE-IDENTITY (IPUI), FIXED-IDENTITY (ARI/PARK), AUTH-TYPE, RAND, RS,
   RES. Reference: kernel `net/dect/mm_*`, osmocom `libdect` `dect_mm_*`.
3. **`dect_dsaa` (authentication crypto).** A11/A12/A21/A22 + B1/B2 (KSG),
   key allocation from the PIN (AC) and PP authentication. Public: deDECTed.org,
   CT-RSA 2010 paper, osmocom `libdect` DSAA/DSC source.

Daemon wiring (`dect-stubd`): feed inbound `0x0203` (and `0x0200`/`0x0201`) into
`dect_dlc`; send outbound C-plane via host→STUB `0x0202` TRIGGER_SIGNALLING /
`0x0244` APPEND_CONNECTION_DATA / `0x015C` SEND_ACKED_DATA (COMMAND_SET §6.4);
store the resulting subscription with `0xF605` sub-op 2 (slot 0..5). The base
must be beaconing with registration open the whole time.

A background research pass (public DECT FP registration flow + DSAA + the exact
LAPC/Cs framing, from ETSI + kernel `net/dect` + osmocom `libdect`) was started
this session to serve as the implementation spec; re-run it if its report was not
saved. Then: implement 1→3, unit-test on PC, drive on hardware, retry the C4.

## Hardware progress (2026-10-18, FRITZ!Fon C4)

The whole stack is built and PC-tested (`dect_cs`/`dect_lc`/`dect_lapc`/`dect_nwk`/
`dect_dsaa`/`dect_mm`), wired into `dect-stubd -g -P <pin>`. Driven against a real
C4 with instrumented traces. Protocol framing bytes below are safe; handset
identity (IPUI, tag, RFPI) is redacted and must never be committed.

**Solved — the DLC link now works end to end:**
- The STUB reports the incoming connection as `0x0200` → `0x0243` → `0x0242`,
  then delivers the handset's Cs signalling as `0x0203` segments.
- The handset opens the link with an **empty LAPC I-frame on LLN=2** (a class-B
  request, `a1 00 01` + Lc checksum). We **refuse class B → class A (LLN 1)** and
  confirm establishment with an RR carrying **NLF set and N(R)=1** (`91 21`).
- **Critical fix:** do **not** send `0x0202` after a C-plane reply — in AVM's host
  driver `0x0202` is the *abnormal link release* primitive, and sending it dropped
  the connection after ~40 ms [CR AVM/Q-0010 analysis]. Reply with `0x0244` only.
  With that, the connection stays up (60 s+).
- Our own I-frames use **NLF clear + P/F=0** (class A, EN 300 175-4 §9.2.1.1).
- Result: the C4 sends its **full ACCESS-RIGHTS-REQUEST** (MM msg `0x44`, a 49-byte
  NWK message with PORTABLE-IDENTITY etc., reassembled from 11 Cs segments) and
  **acknowledges our replies** (`13 21` = RR N(R)=1, a PT response).

**Open — the MM exchange does not complete (no PIN prompt):**
- We have tried replying to ACCESS-RIGHTS-REQUEST with **AUTHENTICATION-REQUEST**
  (`0x40`) and with **KEY-ALLOCATE** (`0x42`). In both cases the handset
  **LAPC-acks** our I-frame but sends **no MM response** and shows **no PIN
  prompt**, then times out ("Anmeldung gescheitert").
- So it is an MM-content / transaction-level issue, not DLC. Leads for next
  session:
  1. **Transaction identifier.** The request uses TV=0, F=0 (originator). We reply
     TV=0, F=1 (responder). KEY-ALLOCATE may need to be a *new* FP-initiated
     transaction (own TV, F=0) rather than a response in the PP's transaction.
  2. **Decode the captured 49-byte ACCESS-RIGHTS-REQUEST** fully (it is in the
     `-p` trace) — especially its AUTH-TYPE IE (observed content `02 48 00`;
     `0x48` = key type AC + IPUI/PARK flag, but octet 3 = `0x02`, not DSAA `0x01`)
     and any SETUP/TERMINAL-CAPABILITY / IWU IEs — to match the exact algorithm
     id, key/code numbers and IE set the C4 expects.
  3. Try replying with **ACCESS-RIGHTS-ACCEPT directly** (PIN-less GAP accept) as
     an A/B test, to see whether the C4 registers without authentication.
  4. Get the **exact AVM MM response** (message type, IEs, TI handling) after
     ACCESS-RIGHTS-REQUEST via the an external review/hardware-analysis route (the clean-room
     Q-0010 was blocked by the safety filter; the user relayed the DLC answer as
     `the clean-room record/Q-0010_AVM_Analyse.md`, which did not fully trace the MM
     sequence — ask specifically for it).
- The crypto is ready and Annex-K-verified; `dect_mm` implements the
  key-allocation flow (ACCESS-RIGHTS-REQUEST → KEY-ALLOCATE → PP
  AUTHENTICATION-REQUEST → AUTHENTICATION-REPLY → ACCESS-RIGHTS-ACCEPT) and the
  daemon drains its message queue one per free LAPC window.

## AVM host-side MM facts to match (from A-0011/A-0012/A-0014) — not yet coded

These are host-driver (the vendor's host driver) facts to align our `dect_mm` with. All are
clean-room answers; cite `[CR A-00NN]`. No identity bytes here.

**Choosing KEY-ALLOCATE vs AUTHENTICATION-REQUEST.** On ACCESS-RIGHTS-REQUEST
(`0x44`) AVM reads the AUTH-TYPE IE's first payload octet (the "mode"); it accepts
`0x01` and `0x02`. Mode `0x02` → the FP sends **KEY-ALLOCATE** (`0x42`) first;
other modes → an authentication path. The C4's request carried mode `0x02`, so
KEY-ALLOCATE-first is correct for it. [CR A-0012 F1] (AVM routes this via an
internal host mail `0x0F9C` to task 5 — that is host-internal, **never** sent to
the STUB.)

**KEY-ALLOCATE exact bytes (mode 2), 34 NWK bytes** — our current frame differs:
| IE | AVM | ours now | fix |
|---|---|---|---|
| ALLOCATION-TYPE | `0B 02 02 88` | `0B 02 01 11` | payload → `02 88` |
| RAND | `0C 08` + 8 | `0C 08` + 8 | ok |
| RS | `0E 10` + **16** | `0E 08` + 8 | **RS must be 16 bytes** |
[CR A-0011, A-0012 F5]. AUTHENTICATION-REQUEST's AUTH-TYPE uses a constant 2nd
byte `0x18` and `0A 03`/`0A 05` variants [CR A-0011].

**ACCESS-RIGHTS-ACCEPT (what we already build correctly).** `85 45 | 05 07 80 A8
<5 id bytes> | 06 07 A0 A5 <5 RFPI bytes>`; FIXED-IDENTITY is PARK type with the
RFPI copied verbatim — **no `>>1` transform** [CR A-0011, A-0012]. Transaction
octet: incoming `0x05` → reply `0x85` (same TV, responder F-bit) [CR A-0011/A-0012].

**Validation is stateful and IE-dependent.** AVM's `0x44` handler inspects
PORTABLE-IDENTITY (`0x05`), AUTH-TYPE (`0x0A`), FIXED-IDENTITY (`0x06`) and
`0x63`/`0x77`/`0x78`/`0x7B`/`0x7C`; a syntactically valid LAPC I-frame can still
be rejected silently at MM. A LAPC RR is **not** MM acceptance. [CR A-0012 F5]

**Storing the subscription after ACCESS-RIGHTS-ACCEPT.** On enrol AVM calls, in
host order, `0x45` ACCESS-RIGHTS-ACCEPT → then `0x0247` (task 2: {slot 1-based,
op 1, state byte}) → `0x020A` (task 2: {index 1..6, 16-byte record, 1 byte}).
[CR A-0012 F3] The `0xF605` sub-op 2 producer layout is {`F605` LE, sub-op 2,
`00`, index 0..5, 5-byte identity, 4-byte aux, 1 flag} = 15 bytes [CR A-0013 F3].

**Next-session order:** fix KEY-ALLOCATE (RS 16 bytes, ALLOCATION-TYPE `02 88`),
retry the C4, watch for the PIN prompt; then wire AUTHENTICATION-REPLY /
ACCESS-RIGHTS-ACCEPT and the `0x0247`+`0x020A` store.

## G6 ACHIEVED — a FRITZ!Fon C4 registers (2026-10-18)

The C4 registered successfully and shows "connected to Basis 3". The full,
hardware-verified MM sequence (identities redacted) is:

```
C4 → FP  05 44  ACCESS-RIGHTS-REQUEST   (AUTH-TYPE proposes DSAA2, algo 02)
FP → C4  05 42  KEY-ALLOCATE            ALLOCATION-TYPE 01 88, RAND 0C 08, RS 0E 08
C4 → FP  85 40  AUTHENTICATION-REQUEST  (AUTH-TYPE algo 01, RAND_P, RES1)
FP → C4  05 41  AUTHENTICATION-REPLY    (RES2)                 -- RES1 verified, DSAA1
FP → C4  85 45  ACCESS-RIGHTS-ACCEPT    (PORTABLE-IDENTITY, FIXED-IDENTITY=PARK)
C4 → FP  05 54  LOCATE-REQUEST          (IPUI, PARK, LOCATION-AREA, caps)
FP → C4  85 55  LOCATE-ACCEPT           05 05 A0 94 <TPUI> + 07 01 64
C4 → FP  05 5d  TEMPORARY-IDENTITY-ASSIGN-ACK                  -- registration complete
```

**Corrections to the pre-hardware assumptions above:**

- **It is DSAA1, not DSAA2.** The C4 *proposes* DSAA2 (AUTH-TYPE algo `02`) but
  accepts a DSAA1 KEY-ALLOCATE and then authenticates with DSAA1 (its
  AUTHENTICATION-REQUEST comes back with algo `01`). So ALLOCATION-TYPE is
  `01 88` and RS is **8 bytes** (`0E 08`), *not* `02 88` / 16-byte RS. Our
  existing DSAA1 crypto verifies RES1 and produces RES2. [hardware]
- **The decisive fix was the transaction F-bit.** KEY-ALLOCATE and
  AUTHENTICATION-REPLY are FT-*initiated* (`05 ..`, F=0); ACCESS-RIGHTS-ACCEPT and
  LOCATE-ACCEPT are responders (`85 ..`, F=1). KEY-ALLOCATE as `85 42` was
  LAPC-acked but ignored at MM; `05 42` made the C4 proceed. [CR A-0022, hardware]
- **RS must be a fresh random seed** (was zeros).
- **LOCATE-ACCEPT must assign a TPUI** via PORTABLE-IDENTITY
  (`05 05 A0 94 {0, 0x11, slot}`) plus LOCATION-AREA; a LOCATION-AREA-only reply
  drew `05 5f` (ASSIGN-REJECT). [CR A-0023, hardware]
- No PIN prompt appears when the base PIN is the C4 factory default `0000`.

**Subscription persistence (done).** After LOCATE-ACCEPT the daemon installs the
handset in the chip the way AVM does [CR A-0026]: `0x0247 {slot, op=1, status}`
then `0x020A {slot, UAK[16], extra}` to STUB task 2, then **closes registration**
with `0x022C 01` (AVM opens registration with `0x022B 'A'` and closes it
afterwards, COMMAND_SET §6.2/§9.7). The key finding: the 16-byte `0x020A` record
is **the UAK itself** (the 128-bit `A21(K,RS)` the key-allocation procedure
agrees on), not a packed identity — so we keep that UAK from the authentication
step and write it verbatim. `0xF605`/`0x015F`/`0x017C` are **not** on the
ordinary voice-handset path and are not sent. The `status`/`extra` security-mode
bytes have no documented enumeration [CR A-0026]; we send `0x00` and will revisit
if G7 paging needs a specific value.

**Result — G6 finished.** A factory-reset FRITZ!Fon C4 enrolls and holds stable
standby: its home screen shows the base name and full signal with no errors, and
the chip keeps beaconing autonomously even after the daemon exits. The handset
can originate a dial, but the call drops after ~3 s with no audio, and no time is
shown — i.e. the remaining connect/release activity is the handset attempting
**normal operation** (time/service fetch, call origination), which needs call
control and the PCM audio path. That is **G7**, not registration.

**Reassembly hardening (done).** Class-A (modulus-2) reassembly cleared its
partial buffer only implicitly; it now drops any partial NWK message on a
new-link I-frame (NLF) or DISC, so a link restart mid-message cannot prepend
stale bytes [CR A-0023 Q5].

## Stock registration, naming and clock, observed over the air (2026-10-22)

`[CR A-StockTrace-4]`, from an over-the-air trace of a stock 7520 (7.56 host,
Mar 31 2022 chip) registering a **factory-reset** third-party handset. The
factory reset matters: a handset that already knows its name and time gives the
base nothing to push, so a plain re-registration would have hidden the very
steps wanted. The protocol was the maintainer's suggestion.

**The registration sequence is standard DECT MM** `[observed]`:
`ACCESS-RIGHTS-REQUEST` → `KEY-ALLOCATE` → `ACCESS-RIGHTS-ACCEPT`, with
`AUTHENTICATION-REQUEST`/`-REPLY` pairs interleaved. That matches what this
project already implements (§ above) and is the conformance reference we wanted.

**A handset has no intrinsic name: the base pushes it.** `[observed]` three
times in one session — at registration, on an isolated rename in the stock web
interface with nothing else happening, and again when the handset re-attached
after a power cycle. The base also **sets the handset's clock**, seen on the
same re-attach, and it is **not** the standard `<<MMS date/time>>` element.
This is distinct from `<<CALLING-PARTY-NAME>>` in a call setup: that is a
*caller's* name, this is the handset's *own* name.

**Open, and challenged by the clean side the same day.** The answer attributes
both pushes to an "AVM-proprietary message", evidenced by an `"AVM "` tag on
the frame. Two reasons that reading may be wrong, both put back to the room:

1. The same room earlier described its tracer as delivering `"AVM "`-framed
   DLC and MAC frames, i.e. `"AVM "` looks like the **capture tool's container
   marker on every frame**, which would make it evidence about the tracer and
   not about the protocol.
2. The handset that accepted the name and the clock was a **factory-reset
   Gigaset**. A third-party handset will not implement an AVM-proprietary
   protocol, so a mechanism both vendors honour is far more likely standard or
   industry de-facto, most plausibly CAT-iq. "Not the `<<MMS date/time>>`
   element" is a real finding but is not the same as "proprietary".

`[not established]` which it is. The distinction decides real work: if it is
CAT-iq we implement naming and clock-setting from specifications we already
cite; if it is genuinely proprietary we have a format to decode and a harder
interoperability question. ~~**Do not build either feature until this is
settled.**~~ **Settled 2026-10-22 -- see the next section; the hold is
lifted.** The raw capture is archived in the analysis side, so settling it needed no
hardware.

Also present in that capture and archived: the idle beacon, the paging that
precedes an incoming call, and six release messages.

### Settled: both pushes ride `<<ESCAPE-TO-PROPRIETARY>>` under EMC 0x0002 (2026-10-22)

`[CR A-StockTrace-4 §4]` decoded the clock push from the archived capture, so
the question above is answered and the "do not build either feature" hold is
lifted.

Both the name and the clock travel in the **standard** `<<ESCAPE-TO-
PROPRIETARY>>` IE, tag `0x7B` (EN 300 175-5), with the proprietary header
`81 00 02` — discriminator type EMC, **EMC = 0x0002** — and then a sub-record:

| what | sub-tag | payload |
|---|---|---|
| handset name | `0x54` | `02` then ASCII (observed: `C430A`) |
| date and time | `0x3B` | len 9, `DD MM YY` BCD, a byte, weekday, `HH MM` BCD |

The records as captured, **complete** `[CR A-StockTrace-4 §3/§4, corrected
2026-10-22]` -- an earlier transcription here dropped the trailing sub-record,
which is why the length octet did not match the content:

```
time:  7b 11 | 81 00 02 | 3b 09 03 22 10 26 00 04 09 12 01 | 5b 01 90
       |  |    |          |  |  |  |__|__|  |  |  |__|  |     |  |  +- sequence
       |  |    |          |  |  |  DD MM YY |  wd HH MM  01    |  +- len 1
       |  |    |          |  |  +- 03                          +- sub-tag 0x5B
       |  |    |          |  +- len 9
       |  |    |          +- sub-tag 0x3B (date/time, BCD)
       |  |    +- EMC header: discriminator type EMC, EMC = 0x0002
       |  +- length 0x11 = 17 = 3 + 11 + 3
       +- <<ESCAPE-TO-PROPRIETARY>>

name:  7b 0e | 81 00 02 | 54 06 02 43 34 33 30 41 | 5b 01 91
                          |  |  |  |___________|     sub-tag 0x5B, len 1, seq
                          |  |  +- 02               "C430A"
                          |  +- len 6
                          +- sub-tag 0x54 (handset name)
       length 0x0e = 14 = 3 + 8 + 3
```

Two corrections to the earlier reading in this file. **Name and time are not
alternative sub-records in one IE**: they are separate IEs in separate frames,
each its own `0x7B` / EMC-`0x0002` wrapper carrying exactly one content record.
And **every such IE also carries a `0x5B` sub-record, length 1, a sequence
counter** -- `[observed]` 0x90 (time), 0x91 (name), 0x92 (name on re-attach),
0x94 (time on re-attach), interleaved across both kinds and advancing. It is
always present and is counted by the IE length, so **it must be emitted or the
length will not match the content**.

**The counter is settled** `[CR A-StockTrace-4 §3, full sweep]`: it is **one
monotonic byte per subscription**, `+1` on **every** proprietary push, shared
across content types -- the observed run is strictly consecutive
`8e` name, `8f` settings, `90` time, `91` name, `92` name, `93` settings,
`94` time. Not one counter per message type. It wraps at `0xFF`, and the
starting value is wherever the subscription's counter already stands (the
capture begins mid-range at `0x8e`), so starting anywhere and incrementing is
safe.

**And the handset acknowledges it.** `[observed]` after each base push with
`0x5B` = X the handset sends its `0x59` record carrying **the same X** --
`8e->8e`, `8f->8f`, `90->90`, `91->91`, through `94`. So this is a
**sequenced, acknowledged channel**, and re-emitting a value the handset has
already seen will almost certainly be deduped: the push would land once and
then silently stop updating. **Never reuse a counter value.**

**Reinterpretation of §4a above.** That section recorded the clock push as
being *preceded* by a handset frame carrying PORTABLE-IDENTITY and an escape
sub-record `0x59`, and read it as an identify that triggers the push. With the
acknowledgement now understood, the `0x59` frames are **acks of the previous
push**, and the apparent "precedes" relationship is an artefact of the
interleaving (push N, ack N, push N+1, ack N+1). **The push is not triggered by
a handset identify.** This is a positive reason for the gating already chosen:
fire on our own LOCATE-ACCEPT, never on `0x59`/`0x64`, which are responses and
not requests.

**The channel carries more than name and time.** `[observed]` on the same
counter: settings lists (`0x58`) and softkey labels (`0x26`, holding the German
UI strings "Still" and "Ablehnen"). So `0x7B` / EMC `0x0002` is a general
sequenced handset-configuration push channel, of which name and clock are two
message types. Recorded for shape; not needed now.

Two captures are byte-identical **except the minute** (09:12 and 09:15), which
is what pins the field layout rather than merely fitting it, and the `04`
matches the ISO weekday of 2026-10-22 (Thursday), an independent cross-check
that the field really is a date. Offsets `0x7df5d` and `0x11cac9` in the AVM
room's `registration.raw`.

A second, fuller timestamp (sub-tag `0xC0`, `YY MM DD HH MM SS`, 2026-10-22
09:11:21, at `0x212d3`) is **not** wrapped in the escape IE and appears once.
`[assumed]` the base's own system timestamp rather than a handset-facing push.
The three stamps advance (09:11:21 -> 09:12 -> 09:15), so it is a live clock.

**How to read this.** The earlier dispute was framed as "AVM-proprietary
versus CAT-iq" and the truth is neither pole. The *container* is a standard
ETSI IE we already cite, so there is no proprietary protocol to reverse and
no new framing to implement -- that part of the challenge in the section above
was right. But `<<ESCAPE-TO-PROPRIETARY>>` is by definition the IE whose
*content* is vendor-scoped, so the sub-records are not CAT-iq elements either,
and the first reading was right that the payload is not standardised.

`[assumed, not observed]` that EMC 0x0002 is an industry de-facto value rather
than AVM's own: the handset that accepted both pushes was a factory-reset
Gigaset, which is unlikely to implement an AVM-only EMC. This matters for
**our** base: emitting EMC 0x0002 means claiming someone else's manufacturer
code. Before we ship it, check EMC 0x0002 against the ETSI EMC assignment and
test whether the Gigasets also accept the same sub-records under a different
EMC. If they do, use ours; if only 0x0002 works, that is an interoperability
fact to document, not to hide.

**Work this unblocks:** naming and clock-setting are now ordinary
implementation -- build the IE, fill a BCD payload. No further capture needed.
The the analysis side holds the raw frames if a conformance test wants exact byte
windows.

#### EMC ownership and the trigger, answered (`[CR A-StockTrace-4 §4a]`, 2026-10-22)

**EMC 0x0002 is not AVM-exclusive, so emitting it is interoperation, not
impersonation.** `[observed]` all 28 escape IEs in the capture carry the single
value 0x0002 and they travel in **both** directions -- 15 base->handset and 13
handset->base -- and the **factory-reset third-party Gigaset emits its own**
proprietary records under EMC 0x0002 (sub-tag `0x59`, seven times,
handset-originated). A third-party handset stamping its own records with 0x0002
is the evidence that settles the practical question: both sides already treat it
as a shared interop value. This retires the worry logged in the section above.

`[not established]` who owns 0x0002 in the ETSI registry. The room did not
decode the FP's broadcast ARI/RFPI EMC (hard from the OTA MAC layer) and is
holding the handset IPUI EMC on the clean-room boundary as a device identity.
So "safe to emit" rests on observed bidirectional use, not on a registry check.
If we ever publish, check the registry; for interoperation the question is
closed.

**The push is attach-triggered: implement a responder, not a sender.**
`[observed]` both base->handset clock pushes are preceded, within ~3 DLC frames,
by a handset-originated frame carrying the handset's PORTABLE-IDENTITY plus a
proprietary escape sub-record (tag `0x59`). No clock push occurs without a
preceding handset identify, and the capture holds no spontaneous or periodic
push. `[not established]` that `0x59` *means* "send me the time" -- its
semantics were not decoded -- only that the push follows the identify.

Direction is trustworthy here: the tracer's DLC frame carries a direction bit at
header offset `0x1a` (0 = hs->base, 1 = base->hs) that splits the capture
147/147; every clock push is dir=1 and every preceding identify is dir=0.

**Where this lands in our stack.** Our bench on 2026-10-22 showed a
factory-reset-era Gigaset re-attaching to our base with **LOCATE-REQUEST**
(`0x54`), which our `dect_mm.c` already answers with **LOCATE-ACCEPT** (`0x55`).
That is the same attach exchange the stock base pushes name and time after. So
the implementation point is immediately after we send LOCATE-ACCEPT, and we
already have the hook. Note the stock trigger frame carries PORTABLE-IDENTITY
*plus* sub-record `0x59`, which our handset's LOCATE-REQUEST may or may not
include -- check a real trace before gating the push on seeing `0x59`, or gate
it on the attach alone.

`[not established]` whether the `0xC0` record is once per boot: the build-stamp
boot marker lives on the host/chip mail interface, not the OTA trace, so a
single capture cannot distinguish once-per-boot from once-per-capture. A kernel
log across a chip reboot would settle it.

### First non-AVM handset registered: Gigaset A540 CAT (2026-10-22)

`[observed]` A factory-reset **Gigaset A540 CAT** completed a full authenticated
registration against our base, on the **08.25** STUB, with `dect-stubd -g -P
0000 -p -t 600`. The handset displayed "MT angemeldet". Every earlier
registration in this project was a FRITZ!Fon C4, so this is the first evidence
that our MM implementation is not AVM-specific.

```
PP -> FP   05 44   ACCESS-RIGHTS-REQUEST
FP -> PP   05 42   KEY-ALLOCATE
PP -> FP   85 40   AUTHENTICATION-REQUEST     (the PIN is entered here)
FP -> PP   05 41   AUTHENTICATION-REPLY
FP -> PP   85 45   ACCESS-RIGHTS-ACCEPT
PP -> FP   05 54   LOCATE-REQUEST
FP -> PP   85 55   LOCATE-ACCEPT
PP -> FP   05 5d   (two octets, no IEs -- see below)
```

`sub: subscription saved to /etc/opendectcell-sub` (84 bytes) and `reg: subscription
persisted + registration closed (slot 1)`. We hold a real UAK for this handset,
so later daemon starts reload it into the chip and the handset re-attaches by
itself within a second or two.

**`0x5D` is undocumented here.** `[observed]` the only handset message after
LOCATE-ACCEPT is MM type `0x5D` with `nwk_len=2`: no IEs at all. Our
`dect_nwk.h` defines MM types only to `0x55`, and the type is recorded by its
observed properties rather than named. `[not established]` what it means.

**`0x5D` is shared, not A540-specific** `[CR A-StockTrace-4 §4a]`: a **C430A**
in the stock capture sends the same bare `05 5d`, `nwk_len=2`, no IEs, three
times, each ~20-25 frames after an `85 55` LOCATE-ACCEPT -- byte-identical in
shape, decoded with the same calibration on both sides. The contrast that
matters: the C430A emits **both** the bare `0x5D` **and** the `0x64`/`0x59`
frame, while the A540 emits **only** `0x5D`. So `0x5D` is the common post-locate
signal and `0x64`/`0x59` is the C430A-specific extra.

It matters for the name/clock work: `[CR A-StockTrace-4 §4a]` found that the
stock base pushes name and time after a handset-originated `0x64` message
carrying PORTABLE-IDENTITY and escape sub-record `0x59`. **Our A540 emits no
such message** -- `0x5D` is empty -- so the push must be gated on
**LOCATE-ACCEPT**, which we already send, and *not* on `0x59`/`0x64`. Nor on
`0x5D`, shared though it is: LOCATE-ACCEPT is **our own outgoing** message, so
it is the cleanest hook and it sits before the point where the two handsets'
post-locate behaviour diverges. Pushing there fires slightly earlier than
stock, which the room assesses as harmless.

#### Two traps this session walked into, recorded so they are not repeated

1. **A re-attach is not a registration.** Earlier the same day a **C430A**,
   still subscribed from when this box ran FRITZ!OS, attached to our base on its
   own and originated calls, while the A540 in the maintainer's hand showed
   "Anmeldung"/"Synchronisiere" and never completed. The logs showed a healthy
   base and an attached handset, which read as success and was not. The
   discriminator is the message type: **`0x44` means registration, `0x54` means
   re-attach**; and `/etc/opendectcell-sub` is written only on the former. Our base
   advertises the box's **real RFPI** (patched from the factory block at `mtd5`
   `0x3d000`), so to any handset ever registered to this box under FRITZ!OS we
   are indistinguishable from the stock base.
2. **Restarting the daemon de-syncs every handset.** `-g` parks the radio on
   exit, so each restart drops the base off the air and handsets fall back to
   "Synchronisiere". A `ring=MS` of 20 s then pages a handset that is not yet
   listening. Start the daemon **once** and give the handset at least ~60-90 s
   before the page.

#### Still open

`[not established]` whether the A540 can register on the **07.56** STUB. Three
windows on 07.56 produced **no handset activity whatsoever** -- not one bearer
-- while 08.25 worked on the next attempt with no other change. That is
suggestive but it is two conditions with one trial each, and the handset was
also factory-reset in between, so it is **not** a controlled comparison. If the
Mar-2022 chip build is needed for the audio work, re-test registration on it
before assuming it is usable.
