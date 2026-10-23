# Conformance audit: our C-plane against EN 300 175

Specs are local, fetched from ETSI (free), in `docs/specs/`:
`en_30017503v020901p.pdf` (MAC), `en_30017504v020901p.pdf` (DLC),
`en_30017505v020901p.pdf` (NWK), all **V2.9.1 (2022-03)**.

Scope of this audit: the layers **we** implement, which is the host-side
C-plane -- `dect_cc.c`, `dect_mm.c`, `dect_lapc.c`, `dect_nwk.c` (1 394 lines
in total). The MAC and PHY are the chip's, and Part 3 is read here only for the
vocabulary needed to ask the chip for the right thing.

Started 2026-10-22, after a session in which three separate defects were found
to be ordinary non-conformance that one handset tolerated and another did not.

## Call Control, clause 9

### What is already right

`[verified against §9.3.2]` The FT-initiated (incoming call) path is
**substantially conformant**, which is better than the project's running
narrative suggested. We model seven states --

    NULL, PROCEEDING, CALL_PRESENT, RINGING, CONNECT_PENDING, ACTIVE, RELEASED

-- against the spec's ten per side, and the incoming-call sequence maps
correctly:

| spec (§9.3.2) | ours |
|---|---|
| send {CC-SETUP}, enter "CALL PRESENT" | `dect_cc_setup_id()` -> `CALL_PRESENT` |
| receive {CC-ALERTING}, enter "CALL RECEIVED" | -> `RINGING` |
| receive {CC-CONNECT}, send {CC-CONNECT-ACK}, enter "ACTIVE" | exactly this |

`<<BASIC-SERVICE>>` `0x88` is correct per §7.6.4: call class `1000` "Normal
call setup", basic service `1000` "Wideband speech default setup attributes".
No `<<IWU-ATTRIBUTES>>` is required, as that is mandatory only when the basic
service is "other" (`1111`).

**A correction recorded deliberately:** an earlier note in this session claimed
we modelled "4 of 10 states". That was taken from a partial grep and is wrong.
The states were there; the defect was elsewhere. This is the same
reason-from-one-sample error the project has hit before.

### ~~Gap 1 -- no CC timers at all~~ **FIXED: F<CC.03> implemented**

`[observed]` `dect_cc.c` contains no timer of any kind. The spec requires:

| timer | side | role | ours |
|---|---|---|---|
| `F<CC.03>` | FT | started on {CC-SETUP}; on expiry the FT **shall** reject the call with {CC-RELEASE-COM}, reason "TIMER-EXPIRY", and enter NULL (§9.3.2.2) | **missing** |
| `F<CC.04>` | FT | started on {CC-ALERTING}; **optional** for both sides (§9.3.2.10 NOTE) | missing, allowed |
| `P<CC.05>` | PT | PT-side only | n/a |

Consequence today: an FP-originated call that is never answered leaves our CC
entity in `CALL_PRESENT` indefinitely rather than releasing. That is a real
defect and it is cheap to fix, but it is **not** why the A540 refuses calls.

### Gap 2 -- the one that matters: no lower-layer connection

`[CR EN 300 175-5 §9.3.2.3]` A {CC-SETUP} that omits `<<CONNECTION-IDENTITY>>`
and `<<CONNECTION-ATTRIBUTES>>` declares a **basic connection**, and

> if this basic connection is not already established when the {CC-SETUP}
> message is received, the call shall be rejected.

We omit both and establish nothing, so the handset rejects with "Insufficient
resources". The full argument, the stock-trace agreement and the rejected
alternatives are in `call-g7.md`. **This is not a CC-layer defect** -- the CC
layer is doing the right thing with the wrong lower layer underneath it, which
is why auditing CC alone would never have found it.

**2026-10-23: this converges with `[CR A-0076]`, and they are the same
mechanism seen from two layers.** The chip side found that `0x0300` builds a
fixed **short** page whose LCE header is compiled in, and that EN 300 444
table 80b maps LCE header `000` ("no U-plane") to **"MAC service = no MAC
service"** -- a PT that establishes its link that way has no U-plane resource
at all. §14.2.3 says the same thing from the other end: without service detail
in the page, "the responding PT shall only establish the minimum MAC layer
service needed for a single C-plane link".

So: our page may be telling the handset to bring up a C-plane-only link, and
our `{CC-SETUP}` then declares a basic connection that §9.3.2.3 requires to be
already established. Two readings, one failure, and the same observable --
`{CC-RELEASE-COM}` with "insufficient resources".

It also explains the handset split without needing either handset to be at
fault: the C4 tolerates being asked to find a U-plane late, the A540 does not.
"Lenient versus strict" is a description, not a cause; this is the cause it
describes.

**`F4.36` tests it with one dump.** Read the compiled LCE header at
`0x96ad+4` while paging: if it decodes to `000`, both readings are confirmed
at once and the fix is to make the page ask for a U-plane rather than to keep
adjusting the SETUP. Four of our attempted fixes so far -- codec list, basic
service in three forms -- were all inside the SETUP, which is the layer that
was never wrong.

## DLC / LAPC, Part 4

### ~~Gap 3 -- the retransmission timer is half the specified value~~ **FIXED**

`[CR EN 300 175-4 §A.1 Table A.1.1]` `<DL.04>`, the retransmission timer, is
**2 s** for both FT and PT, started when an I-frame is transmitted and stopped
when that frame is acknowledged. `dect_lapc.c` uses
`DEFAULT_RETRY_MS = 1000u`. **We retransmit at twice the specified rate.**

Not causal for the A540 reject -- a conformant exchange is acknowledged in
~150 ms and never reaches either value -- but it is a real deviation, and
retransmitting early risks duplicate I-frames on a slow link. The other
Part 4 timers (`<DL.00>`, `<DL.01>`, `<DL.02>`, `<DL.03>`, `<DL.07>` all 2 s;
`<DL.05>` 10 s; `<DL.06>` 4 s) are not implemented at all; most are for
procedures we do not perform (suspend, resume, connection handover).

### Gap 4 -- CC `IWU-INFO` is never sent and never answered

`[observed]` `dect_cc_rx()` has no `case DECT_CC_IWU_INFO`; the constant is
defined and the message falls into `default: break;`, silently dropped --
together with `CALL_PROC`, `SETUP_ACK`, `NOTIFY` and `INFO`. Nothing in the
codebase sends `0x60`, and nothing builds a `<<CODEC-LIST>>`.

This is currently the **best-supported** explanation for the A540 refusing
FP-originated calls, on four independent lines (see `call-g7.md`): the air
trace shows stock negotiating IWU-INFO between SETUP and ALERTING; stock's own
release guard is named "MAC layer and/or IWU-Info not in time -> release
connection"; `<<BASIC-SERVICE>> 0x88` selects wideband, for which §7.6.4
NOTE 3 requires TS 102 527-1 codec negotiation that we never perform; and the
A540 sent us 20 `IWU-INFO` frames in one run, all unanswered.

**Implementation points, both in `dect_cc.c`:**

1. a `case DECT_CC_IWU_INFO` in `dect_cc_rx()` to **answer** the handset;
2. a sender invoked **after** our CC-SETUP on an FP-originated call to
   **initiate** the exchange -- stock sends its first IWU-INFO seven frames
   after the SETUP and alerts only once the exchange completes.

`[blocked]` on the record layout: the content leads `c3`, carries an advancing
sequence byte and a `91`, inside `<<IWU-TO-IWU>>` (`0x77`). The the hardware-analysis step is
decoding it. **Do not invent the payload** -- a wrong reply is likely worse
than no reply, and the handset's own frames give us the shape but not the
semantics.

## Gap 5 -- the page never says what the call needs `[2026-10-23 night]`

**Audited:** clause 8.2 (the `{LCE-REQUEST-PAGE}` message) and clause 14.2.3
(indirect FT-initiated link establishment). This is the first audit entry that
may explain a live defect, so it is written out in full.

### What the standard provides

`{LCE-REQUEST-PAGE}` has a **short** format and a **full** format, and the
full format *with TPUI address structure* (W = 1) carries three fields the
other forms do not (§8.2.4.2):

| field | octet | values |
|---|---|---|
| **1 -- slot type** | 2, bits 8..5 | `0000` half · `0001` **long slot j=640** · `0010` **long slot j=672** · `0100` full · `0101` double |
| **2 -- setup info** | 5, bits 8..5 | `0000` none · `0001` MT basic, attributes negotiation optional · `0010` MT basic, mandatory · `0011` MT advanced, mandatory · `0100`/`0101` B-field signalling |
| 3 | 5, bits 4..1 | reserved, or the LCN for a DPRS resume |

§8.2.2: with a nonzero info field 2, *"the PP shall use the setup method and
the slot type indicated in the paging message"*. Without one -- short page,
full-with-IPUI page, or info field 2 = `0000` -- the PP falls back to the
default table 80b, keyed on the **3-bit LCE header**:

| LCE header | default behaviour on a short page |
|---|---|
| `000` no U-plane | MT basic, full slot, no negotiation, **MAC service = no MAC service** |
| `001` unknown + ringing | MT basic, full slot, PP **does** negotiate attributes |
| `010`/`011` general purpose | MT basic, full slot, PP **does** negotiate attributes |
| `100` voice -- the GAP code | MT basic, full slot, no negotiation, MAC service = IN_min_delay |
| `101` auxiliary | MT advanced, full slot, no negotiation, IN_normal_delay |

And §14.2.3, on what the PT does when the page carries no service detail:
*"In all other cases, the responding PT shall only establish the minimum MAC
layer service needed for a single C-plane link (i.e. a single duplex
bearer)."*

### Why this is a gap for us

We page with `0x0300 {page info, TPUI hi, TPUI lo}` -- three bytes, page info
`0x0C`, or `0x2C` when the `{CC-SETUP}` will carry `<<BASIC-SERVICE>>` service
8. The chip builds the actual message. **We have never known which format it
builds or which LCE header `0x0C` becomes**, so we have never known which row
of table 80b every handset we page has been following.

Two things follow if the answer is the unfavourable one, and both are open:

- **The A540 refusal.** Under header `000` the PT establishes a link with *no
  MAC service*. A speech `{CC-SETUP}` on that link, answered with
  `{CC-RELEASE-COM}` / `<<RELEASE-REASON>>` `0x32` "insufficient resources",
  is then **the correct behaviour**, not a fault. It is the first hypothesis
  that fits the whole shape: page answered, SETUP delivered and ACKed at DLC,
  call refused on resources, and only by the stricter handset. The four
  hypotheses excluded by experiment (stale bearer, missing `<<CODEC-LIST>>`,
  wideband basic service, external-handover call class) do not fit it.
- **Wideband.** §8.2.4.1 on the GAP voice code `100`: *"full paging format is
  mandatory if the slot is different of a full slot."* A short page can only
  ask for a full slot. Put that beside `[CR A-0069]` -- the chip does not
  choose the slot type, it reads it from the handset's own bearer-request
  attributes -- and **nobody in the system is choosing a long slot**: not us,
  not the chip, and the handset was never told to ask for one.

### Status

**Not a finding yet.** We have not verified that our page is short format or
that its header is `000`; that is `Q-0051` to the clean room, which asks what
the chip builds from `0x0300`, how the page-info byte maps to the LCE header,
what bit `0x20` changes on the air, and whether any path exists to a
full-format TPUI page with info field 1 = long slot. A plain "there is none"
is a complete answer and retires the wideband thread on this STUB.

## Still to audit

- **Clause 14 (LCE).** Link establishment and the "short LCE confirm" that
  stock sends between the page response and the SETUP, which we do not send.
  Bytes are in hand from the the hardware-analysis step (`00 91 21 01 45`). Clause 8.2 and
  §14.2.3 are now audited -- see Gap 5.
- **Part 4 (DLC / LAPC).** Timer values done (Gap 3). Still to check: the
  class A/B establishment procedures and the NLF handling.
- ~~**Clause 13 (MM).** … the `0x5D` message we receive and ignore after
  LOCATE-ACCEPT is unidentified.~~ **IDENTIFIED AND FIXED 2026-10-23** — it is
  `{TEMPORARY-IDENTITY-ASSIGN-ACK}`, the handset confirming the TPUI our
  `{LOCATE-ACCEPT}` assigned (EN 300 175-5 §13.2.2). Three agreeing lines of
  evidence: the capture is `05 5d`, two octets with no elements; table 62
  gives the ack as P⇒F with only PD, TI and type mandatory, the only MM
  message of that shape here; and `reg4.log` shows it arriving right after our
  `85 55 … a0 94 00 11 01` LOCATE-ACCEPT. **The numeric value is not read off
  the coding table** — pdftotext renders it column-wise and two
  reconstructions failed their own calibration — so treat it as very probable
  rather than certain.

  **It was a real gap, not just an unknown.** §13.2.2 requires the FT to start
  `<MM_ident.1>` (10 s) when it assigns a TPUI, stop it on the ack, and abort
  the procedure on expiry. We ran no timer and ignored the ack, so an
  assignment counted as successful the moment we sent it — **and we page by
  that TPUI.** A handset that never took it would be paged on an identity it
  is not listening for, silently. Implemented in API 7 with
  `dect_mm_tpui_confirmed()`, and `dect-stubd` now prints
  `WARNING: TPUI never acknowledged` on any page in that state.
- **Clause 9.5 (release).** We have `RELEASE` and `RELEASE_COM`; the normal vs
  abnormal release distinction and the LCE "NLR" notifications are not
  implemented.

## Method note

Audit against the **standard**, not against a handset. Every difference between
the C4 and the Gigasets examined so far has been our own non-conformance that
the C4 tolerated. See `handset-compatibility.md`.


## Gaps 1 and 3 closed (2026-10-22)

**Gap 3**, the easy one: `DEFAULT_RETRY_MS` was `1000`, where EN 300 175-4 §A.1
specifies `<DL.04>` as **2 s** for both FT and PT. Now 2000. A healthy exchange
never reaches either value -- stock is acknowledged in about 150 ms -- so this
only changes behaviour when something is already wrong, which is also why it
was invisible.

**Gap 1**, `F<CC.03>`: implemented as `dect_cc_tick()`, with
`dect_cc_next_deadline()` beside it so a caller can arm one timer across LAPC
and CC together. 20 s, FT and PT alike (EN 300 175-5 §A.1). On expiry it sends
`{CC-RELEASE-COM}` carrying `<<RELEASE-REASON>>` **"timer expiry"** (`0x0D`)
and enters NULL, as §9.3.2.2 requires.

Three details worth recording:

- **The state is the stop condition.** §A.1 says the timer stops when "a
  response message has been received", and every response moves the entity out
  of `CALL_PRESENT` -- to `RINGING` on CC-ALERTING, `ACTIVE` on CC-CONNECT,
  `RELEASED` on a release. So the tick clears the deadline whenever the state
  is not `CALL_PRESENT`, and no separate stop is needed.
- **It is armed in the tick, not where the SETUP is queued**, so
  `dect_cc_setup_id()` keeps its signature. The tick runs every loop pass, so
  the start is late by well under a millisecond against 20 seconds.
- **It only runs for FP-originated calls** (`cc->incoming`), because `F<CC.03>`
  starts when *we* send a SETUP.

**Why it mattered in practice:** every refused ring in the 2026-10-22 tests
left a transaction stuck in `CALL_PRESENT`, and only a daemon restart cleared
it. That is also why `ring_queue_setup()` could be refused by a busy CC entity
-- the entity was busy with a call that had already failed.

Still open: **Gap 2** (no lower-layer connection, the live defect) and **Gap 4**
(`IWU-INFO` never sent or answered, deferred since the registration-time codec
list turned out not to be the cause).

## CLIP / CNIP audit `[2026-10-23]`

Audited `dect_clip.c` and the `{CC-SETUP}` builder against EN 300 175-5 §7.7.9
(figure 33), §7.7.53 (figure 98b) and §7.6, because no handset has ever been
shown either element and we wanted to know whether what we emit is even right.

**The encoder is correct.** Every field checked against the figures:

| field | spec | ours |
|---|---|---|
| `<<CALLING-PARTY-NUMBER>>` id | `0110 1100` = `0x6C` | `0x6C` |
| `<<CALLING-PARTY-NAME>>` id | `0110 1101` = `0x6D` | `0x6D` |
| number octet 3 | ext bit 8, type bits 7–5, NPI bits 4–1 | `(ton & 7) << 4 \| (npi & 15)` |
| number octet 3a | ext bit 8 set, presentation bits 7–6, spare 5–3 zero, screening 2–1 | `0x80 \| (pres & 3) << 5 \| (scr & 3)` |
| name octet 3 | bit 8 zero, presentation 7–6, alphabet 5–3, screening 2–1 | `(pres & 3) << 5 \| (alpha & 7) << 2 \| (scr & 3)` |
| enums | TON 0/1/2/3/4/6, NPI 0/1/3/7/8/9/10/11/12/13/14/15, presentation 0–3, screening 0–3, alphabet 0/1/7 | all present, all matching |

Our omit-rather-than-guess policy also matches §7.7.53 note 3: when the
network provides no CNIP indication the element "should not be used", and the
"name not available" indicator "should not be used to cover this case".

**A defect found, and it was ours and one day old.** §7.6:

> "The variable-length information elements are transmitted over the air
> according to the ascending numerical order of their identifiers. This allows
> receiving equipment to detect the presence or absence of a particular
> information element without scanning through an entire message."

A receiver may therefore **stop looking once it has passed** the identifier it
wants. Our `{CC-SETUP}` was emitting `<<CODEC-LIST>>` (`0x7C`) **before** the
caller identity (`0x6C`, `0x6D`) — a descending step immediately before the
caller ID, which is exactly the shape that makes a conforming handset skip it.

Introduced by `893a70e` earlier the same day, when the codec list was added to
fix the A540 refusal. Fixed: the identity now precedes the codec list, and
`tests/test_cc.c` carries an **invariant** over the whole built message rather
than a check of one pair — verified by reinstating the bug, watching the test
fail, and restoring the fix.

**It is not the explanation for the original symptom**, and the comment in
`dect_clip.h` that implied it has been corrected. The C4 ignored byte-exact
`0x6C`/`0x6D` in v28/v29, which predate the codec list by days; the order was
ascending then. That remains open, and the leading candidate is still that the
call was not marked external (`<<CALL-INFORMATION>>`, test card #73) — the C4
displayed "Intern".

**Two notes for whoever runs the bench test:**

- Figure 98b numbers the name's last octet `L+1`, where figure 33 numbers the
  number's `L+2`. §7.6 settles it — "the length is calculated starting from
  octet 3" — so `L+2` is right and the figure is a typo. We follow the general
  rule, which is what `dect_nwk_put_ie()` does for every element.
- §7.7.53 note 1 is a feature we do not have yet: if the calling number matches
  an entry in the FP contact list, the name from that list should be sent with
  screening "user-provided, verified and passed". That belongs with the
  controller's contact list, not here.
