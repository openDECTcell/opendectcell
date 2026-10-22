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

### Gap 1 -- no CC timers at all (conformance, not today's bug)

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

### Gap 3 -- the retransmission timer is half the specified value

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

## Still to audit

- **Clause 14 (LCE).** Link establishment and the "short LCE confirm" that
  stock sends between the page response and the SETUP, which we do not send.
  Bytes are in hand from the the hardware-analysis step (`00 91 21 01 45`).
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
