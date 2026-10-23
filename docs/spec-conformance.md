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
- **Clause 13 (MM).** Registration passes on two vendors' handsets, so this is
  lower priority, but the `0x5D` message we receive and ignore after
  LOCATE-ACCEPT is unidentified.
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
