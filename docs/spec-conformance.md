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

    NULL, PROCEEDING, OVERLAP, CALL_PRESENT, RINGING, ACTIVE, RELEASED

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
| `F<CC.03>` | FT | started on {CC-SETUP}; on expiry the FT **shall** reject the call with {CC-RELEASE-COM}, reason "TIMER-EXPIRY", and enter NULL (§9.3.2.2) | **implemented** (`dect_cc_tick()`, 20 s) |
| `F<CC.04>` | FT | started on {CC-ALERTING}; **optional** for both sides (§9.3.2.10 NOTE) | missing, allowed |
| `P<CC.05>` | PT | PT-side only | n/a |

Consequence **before the fix**: an FP-originated call that is never answered
left our CC entity in `CALL_PRESENT` indefinitely rather than releasing. Real,
cheap to fix, and never the reason the A540 refuses calls. Closed on
2026-10-22 -- see "Gaps 1 and 3 closed" below for the three details worth
knowing about how it arms and stops.

### Gap 2 -- the one that matters: no lower-layer connection

`[CR EN 300 175-5 §9.3.2.3]` A {CC-SETUP} that omits `<<CONNECTION-IDENTITY>>`
and `<<CONNECTION-ATTRIBUTES>>` declares a **basic connection**, and

> if this basic connection is not already established when the {CC-SETUP}
> message is received, the call shall be rejected.

We omit both and establish nothing, so the handset rejects with "Insufficient
resources". The full argument, the stock-trace agreement and the rejected
alternatives are in `docs/journals/call-g7.md`. **This is not a CC-layer defect** -- the CC
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

### 2026-10-23, later: the standard names the value we should be sending

Everything above was argued from one NWK clause plus a chip inference. The
profile and the LCE clause settle it directly, and they correct one of our own
sentences while they are at it. All of this is from
`docs/specs/`, re-extracted with `pdftotext -raw` and calibrated row by row.

**EN 300 444 Table 66** -- the GAP profile's own table for
`{LCE-REQUEST-PAGE}` -- permits exactly two values of `<LCE-header>`:

> `0, 4`. "Indicates the U-plane services (MAC) required. **The '0' value
> shall be used when only C-plane is required (e.g. MM procedures).** The PT
> shall support a follow on call on the same link even if value '0' was used
> during initial paging."

**EN 300 175-5 §8.2.4.1** gives `4` = `100` its name:

> Code "100" = Voice Service code (former IN_min_delay) **is the traditional
> code used in GAP voice devices.** When used as short paging, this code shall
> cause a **Basic connection MT bearer setup with full slot and MAC service
> IN_min_delay.**

So `F4.36` is no longer "read it and see". **The expected value for a voice
call is `100`; `000` is the value reserved for C-plane-only procedures.**

**Table 80b itself was already on this page** -- Gap 5 decoded it on
2026-10-23 night, including the `100` voice row. This section re-derived it
from the PDFs instead of reading Gap 5 first, which is the mistake
`read-our-own-notes-first` exists to prevent. What is genuinely new here is
Table 66 above, its follow-on-call sentence, and GAP §8.35; the rest points at
Gap 5.

**Correction to this page.** Gap 2 above says we "omit both and establish
nothing". That contradicts Gap 5's own table 80b in this same document, where
both relevant rows read *"MT basic connection setup on full slot"* and differ
only in MAC service (`none` vs `IN_min_delay`). A basic connection is
established either way. What `000` withholds is the
**MAC service** -- there is no U-plane on the connection, not no connection.
That matters because it is the only reading consistent with what we actually
observe: we exchange LAPC frames with the handset perfectly well, which a
link that was never established could not do. "No bearer at all" never fitted
that and should not have been written.

**And it explains the handset split without either handset being at fault.**
GAP requires the PT to "support a follow on call on the same link even if
value '0' was used" -- i.e. to acquire a U-plane later, by MAC service
modification, on a link paged as C-plane-only. The C4 does that. The A540
apparently does not, which makes the A540 the stricter reading of a
requirement it arguably fails. Either way the remedy on our side is the same
and is not a workaround: **page with `4` for a voice call**, which is what the
profile calls the traditional GAP value and what makes the U-plane present
from the start instead of depending on behaviour one of our two handsets does
not implement.

**A second thing falls out, about wideband.** §8.2.4.1 adds that `100` "is the
standard code for Wideband voice ... but **full paging format is mandatory if
the slot is different of a full slot**", while GAP §8.35 says "FT and PT shall
**only support short format** for the {LCE-REQUEST-PAGE} message". Those
combine into a profile-level fact we had been attributing to AVM:

> A GAP FP cannot page for a long slot **at all**. Short paging can only ask
> for a full slot, and GAP permits nothing but short paging.

`[CR A-0076]` §4 found no full-format page builder on this STUB and we recorded
that as a limitation of the chip. It is not -- it is GAP conformance. The
conclusion (`F4.30`–`F4.33` retired, the handset must ask for wideband itself)
is unchanged and was right; the reason is better than the one we wrote down.

**What this does not settle.** Whether the compiled header at `0x96ad+4` is
`000` or `100` is still a measurement, and whether anything host-side selects
it is still unknown -- `[CR A-0076]` found `mail[2]` of `0x0300` unreferenced
in the page path. If `F4.36` reads `100`, this whole line of reasoning is
wrong and the A540's refusal is something else. That is the useful shape of
the test.

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

### ~~Gap 4 -- CC `IWU-INFO` is never sent and never answered~~ **CLOSED as a responder; the hypothesis attached to it is WITHDRAWN**

**What was written here** `[2026-10-22]`: `dect_cc_rx()` had no
`case DECT_CC_IWU_INFO`, the message fell into `default: break;` alongside
`CALL_PROC`, `SETUP_ACK`, `NOTIFY` and `INFO`, and this was called "the
**best-supported** explanation for the A540 refusing FP-originated calls" on
four independent lines. Implementation was `[blocked]` on the record layout.

**Both halves of that are now out of date, and the second one matters more.**

*Implemented.* `src/libopendectcell/dect_iwu.{c,h}` is the `<<IWU-TO-IWU>>` session
responder, dispatched from `case DECT_CC_IWU_INFO` in `dect_cc_rx()` and
covered by `tests/test_iwu.c` (45 checks). The record framing came from
`[CR A-StockTrace-5 §23]`, 40 records across 6+ complete dialogs, so nothing
was invented. `case DECT_CC_INFO` also exists now (GAP N.4, dialled digits),
so the list of silently-dropped messages in the old text is wrong twice over.

*Only a responder, and deliberately.* The old implementation point 2 -- a
sender to **initiate** the exchange after our `{CC-SETUP}` -- **is retired, not
outstanding.** `[CR A-StockTrace-5 §24]` corrects §11a: in stock captures
**every** open is handset→base, the base only ever responds, and
base-originated calls to Gigaset handsets ring with **no** `{CC-IWU-INFO}`
exchange at all.

*Hypothesis withdrawn.* That same correction removes the ground this gap's
headline claim stood on. The dialog's absence during our failed calls is a
**consequence of the early release, not its cause**. `dect_iwu.h` says so in
its own header comment; this page had not caught up. The live explanation for
the A540 is Gap 2 / Gap 5, not this.

**Why it was still worth doing:** the peer opens sessions we were dropping,
which is a defect whatever else it causes, and TS 102 527-3 list access is
built on this dialog.

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

**`Q-0051` is answered; the measurement is not taken.** `[CR A-0076]` replied
on every part of it: `0x0300` builds a fixed **short** page into chip RAM at
`0x96ad` and writes exactly one header byte at `0x96ad+4`; the page-info byte
(`mail[2]`, our `0x0C`/`0x2C`) is **never referenced in the page path**, so bit
`0x20` is inert on the air and the LCE header is **compiled in**; and there is
**no full-format page builder** on this STUB -- not `0x0300`, not `0x015E`, not
`0x015C`. That last answer retires the wideband-by-paging thread, exactly as
the question anticipated.

So the only thing still unmeasured is **the value of the compiled header**,
which `F4.36` reads in one dump while a page is in flight. `F4.35` has already
shown `0x96a0..0x96bf` is all zero with the radio down, so it must be read
during paging.

**What the profile adds, and it was not in the question** `[2026-10-23]`:
**EN 300 444 Table 66** permits a GAP FP exactly two header values, `0` and
`4`, with `0` "used when only C-plane is required (e.g. MM procedures)" -- so
the table above is not just a menu, it is a two-way choice and `100` is the
voice half of it. Table 66 also says **"The PT shall support a follow on call
on the same link even if value '0' was used during initial paging"**, which
is the first thing we have found that explains the handset split without
either handset being at fault: acquiring a U-plane later by service
modification is required of the PT, the C4 does it, the A540 appears not to.

And GAP §8.35 -- *"FT and PT shall only support short format for the
{LCE-REQUEST-PAGE} message"* -- means the missing full-format builder
`A-0076` found is **profile conformance, not a chip limitation**. A GAP FP
cannot page for a long slot at all. The wideband conclusion is unchanged; its
reason is better.

## Still to audit

- **Clause 14 (LCE).** Link establishment and the "short LCE confirm" that
  stock sends between the page response and the SETUP, which we do not send.
  Bytes are in hand from the analysis side (`00 91 21 01 45`). Clause 8.2 and
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
the C4 tolerated. See `journals/handset-compatibility-2026-10.md`.


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

Still open: **Gap 2 / Gap 5** -- the lower-layer connection and the LCE header
that selects its MAC service, which are one defect seen from two clauses and
are the live one. **Gap 4 is closed** (responder implemented, initiator
retired, hypothesis withdrawn).

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
- §7.7.53 note 1: if the calling number matches an entry in the FP contact
  list, the name from that list should be sent with screening "user-provided,
  verified and passed". Done since 2026-10-30 for a SIP call that brings no
  name of its own (`docs/phonebook.md`).

## Audit of this page against the code `[2026-10-23]`

The conformance tables are read by people and by the controller agent as the
interface to this stack, so they were checked line by line against the source.
**Three entries had drifted, and the document contradicted itself once.**

| entry | said | actually |
|---|---|---|
| `F<CC.03>` row | **missing** | implemented 2026-10-22 as `dect_cc_tick()`. The *heading* above the table said "FIXED" and the closure section below it said "implemented" — only the row, the thing a reader scans, was stale |
| Gap 4 | "never sent and never answered", `[blocked]` on the record layout, and "the **best-supported** explanation for the A540 refusing FP-originated calls" | implemented as a responder (`dect_iwu.{c,h}`, 45 checks) and the hypothesis **withdrawn** by `[CR A-StockTrace-5 §24]` — `dect_iwu.h` says so in its own header; this page had not caught up |
| Gap 5 status | "Not a finding yet … that is `Q-0051` to the clean room" | `Q-0051` was answered in full by `[CR A-0076]`; only the header's *value* is unmeasured |
| Gap 2 vs Gap 5 | Gap 2: "we omit both and **establish nothing**" | Gap 5's own table 80b, on this page, says *"MT basic connection setup on full slot"* for both rows. A basic connection is established either way; `000` withholds the **MAC service** |

A stale "missing" is the expensive kind of wrong: it sends the next session —
or the controller agent — to build something that already exists.

### `F<CC.03>` had no test, and now has three

The timer was written on 2026-10-22 and nothing exercised it. A 20-second fuse
whose only job is to fire when nothing else happens is exactly what stops
working in silence. `tests/test_cc.c` now covers expiry, the stop-on-answer
path and a PP-originated call, 33 checks.

**They were mutation-tested rather than trusted**, and that caught two faults
in the tests themselves:

- changing the timer to 10 s, changing the release reason, and removing the
  deadline-clear on answer are each caught now — but the first revision read
  `out[n-2]` without checking `n`, so the "wrong timer" mutation turned a
  clean failure into an ASan stack-buffer-overflow. Guarded.
- the PP-originated test **did not discriminate**: deleting `!cc->incoming`
  from `dect_cc_tick()` changed nothing. A PP SETUP goes to `CONNECT PENDING`
  (`on_setup()` queues CALL-PROC and CONNECT at once) and never sits in
  `CALL PRESENT`, so the state guard alone already excludes it.

**That last one is a small finding about the code, not just the test:**
`incoming == 0 && state == CALL_PRESENT` is **unreachable through the public
API**, so the `!cc->incoming` term is defensive and currently redundant. Left
in place — it is free and it states the intent — but recorded so nobody
"simplifies" it believing a test guards it. The test pins the behaviour (a
handset-originated call is never torn down by our timer), which is what we
actually care about.

## The LCE header is ring noise, and our own logs predict its value `[CR A-0091, A-0092]` `[2026-10-23]`

`[CR A-0091]` corrects `[CR A-0076]` §2: the page header byte at `0x96ad+4` is
**not compiled in**. The builder makes it from

```
bit0 | ((mail[5] & 0x0F) << 1) | (bits 5-7 preserved)
```

and `mail[5]` is the `0x0300` body byte after the TPUI. **Neither we nor stock
has ever written it** — stock's sender emits three payload bytes exactly as
ours does. `[CR A-0092]` then explains what the builder reads instead: the
chip's incoming mails sit in a **queue ring, dispatched in place**, with no
copy to a scratch buffer and nothing zeroing the region. So `mail[5]` is the
byte left at that ring offset by whatever mail last occupied it.

### Our own mail log predicts the header, before the bench run

If the leftover is a previous host→chip mail, its value is **ours**, and we
have 198 960 of them in the corpus. `mail[5]` is `param[3]`, so:

| what we send | count | `param[3]` |
|---|---|---|
| `fff1` memory read | **197 190 (99.1 %)** | the address high byte |
| `f624` status poll | 554 | — |
| everything else (`0230`, `0251`, `022d`, `015b`, `0247`, `0300`) | ~1 200 | assorted |

And `param[3]` of an `fff1` read is exactly the address high byte, verified
across all 197 190: **`0x00` for any address below `0x010000`, `0x01` for the
`0x01xxxx` range.** Nothing else occurs.

So the ring is, to three nines, full of `0x00` and `0x01`. Taking
`header = mail[5] & 0x07`:

| header | share of host→chip mails | table 80b behaviour |
|---|---|---|
| **`000`** | 68.7 % | basic connection, full slot, **MAC service = no MAC service** |
| **`001`** | 31.3 % | basic connection, full slot, **PP does negotiate attributes**, MAC service as negotiated |
| 3, 5, 7 | 0.04 % combined | — |
| **`100` (the GAP voice code)** | **never** | — |

**Pre-committed prediction: `F4.36` reads header `0` or `1`, and never `4`.**

### This is a mechanism for the intermittency, not just a value

`000` gives the handset no MAC service; `001` makes it negotiate attributes
and would **work** for a voice call. A page whose header is a leftover ring
byte would land on one or the other depending on preceding traffic — which is
intermittent by construction and correlates with nothing we control. That is
the signature we have been unable to explain all week, and it is the first
account of it with a mechanism behind it.

### The instrument changes the measurement, and `F4.36` is not exempt

`F4.36` polls `0x96a0` plus `$RECORD` (eleven low addresses, `param[3]=0x00`)
and `$LANES` (two `0x01xxxx` addresses, `param[3]=0x01`) every 50 ms. **It is
injecting `0x00` into the ring far faster than anything else happens**, and
`0x96a0` — the read we need in order to see the header at all — is itself a
low address. So a header of `0` in `F4.36` is partly our own doing and does
**not** establish that an ordinary ring test sees `0`.

There is no way to read `0x96a0` without an `fff1` at a low address, so this
cannot be designed away. It can be worked around two ways, and both are worth
having:

1. **`36b` (`pagehdr=4`) is immune**, because it writes `mail[5]` explicitly.
   If the header follows our byte, the ring no longer matters.
2. **`36c` changes the ring composition instead of the page**: `$LANES` only,
   polled slowly, so `0x01xxxx` reads dominate. If the header moves toward `1`
   with no change to the page at all, that is a **second, independent lever on
   the same byte** — confirming A-0091/A-0092 by a different mechanism, the
   way `dir1+6` and the corpus confirmed each other over the RSSI.

### How to decode `0x96b1`, and why a raw read would mislead `[CR A-0093]`

The byte at `0x96ad+4` is **composed, not copied**:

| bits | contents |
|---|---|
| 0 | **always 1**, set unconditionally. Not part of the header |
| 1–4 | `mail[5]` bits 0–3 — **the only part that moves** |
| 5–7 | a fixed init constant; the page path never writes them |

So the header to compare against table 80b is **`(byte >> 1) & 0x07`**, and
the low five bits we expect are `((mail[5] & 0x0F) << 1) | 1`:

| `mail[5]` | low 5 bits | header |
|---|---|---|
| `0x00` | `0 0001` | `000` no MAC service |
| `0x01` | `0 0011` | `001` PP negotiates attributes |
| `0x04` | `0 1001` | `100` GAP voice code |

Two consequences worth stating:

- **A non-zero high nibble is expected and means nothing.** Reading the byte
  as a header directly would have made every run look like a null result,
  which is why this is recorded before the run rather than after it.
- **Bits 5–7 being constant rules them out as an intermittency source.** Only
  bits 1–4 track the ring, so the variable header lives exactly where our
  `0x00`/`0x01` prediction puts it.

A-0093 also confirms the ceiling on `pagehdr`: the builder forces the header
to 0 when `mail[5] & 0xC0 != 0`, and capping the option at `0x0F` means we can
never trip that path by accident. It also closes "never 4" from the firmware
side — reaching `4` needs `mail[5]` bit 2 set with bits 6–7 clear, which
nothing in our traffic produces, including the rare `3/5/7` tail.

## Theory sweep: ten causes eliminated in one sitting `[2026-10-23 night]`

The maintainer's rule, applied for the first time: **gather every plausible
theory and test them all in one attended run**, with the script announcing
which is live. `work/7520/lab/sweep.sh`, eleven phases, ~11 minutes.

**The handset showed "Synchronisiere" throughout and never rang — but the
sweep was sound**, which the logs establish and the display does not:

| phase | theory | page resp | our SETUP | reply |
|---|---|---|---|---|
| 1 | baseline (`pagehdr=4`) | — | yes | `0x32` |
| 2 | page header **1** (PP negotiates attributes) | yes | yes | `0x32` |
| 3 | page header **3** (general purpose) | yes | yes | `0x32` |
| 4 | page header **0** (known-bad) | yes | yes | `0x32` |
| 5 | `<<BASIC-SERVICE>>` `0x88` wideband | yes | yes | `0x32` |
| 6 | `<<BASIC-SERVICE>>` `0x28` (the A540's own value) | yes | yes | `0x32` |
| 7 | `<<SIGNAL>>` alerting pattern 0 | yes | yes | `0x32` |
| 8 | `<<CALL-INFORMATION>>` external line | no | **no setup** | — |
| 9 | CLIP number present | yes | yes | `0x32` |
| 10 | **ciphering ON** (`-C` dropped) | yes | yes | `0x32` |
| 11 | baseline again | yes | yes | `0x32` |

Phases 1 and 11 agree, so the sweep is internally consistent. Phase 8 sent no
SETUP and tested nothing. **Ten theories, one identical refusal.**

`e2 32` appears three times per log because one event is logged at three
layers (raw mail, Cs decode, NWK) — one refusal per phase, not three.

### What that eliminates

**The LCE header is not the cause.** Headers `0`, `1`, `3` and `4` produce
byte-identical outcomes. Whatever `[CR A-0091]`'s `mail[5]` mechanism does,
varying it changes nothing the handset reacts to — and combined with the whole
40-slot table at `0x96ad` reading zero through 12 496 reads while pages
demonstrably work, the page-header line of inquiry is **closed, not merely
unproven**. Gap 2/Gap 5 as stated was wrong.

Also eliminated: basic service (three values), `<<SIGNAL>>`, CLIP presence,
and **ciphering** — a CAT-iq handset refusing an unencrypted call was the best
remaining alternative and it refuses either way.

### What is left, and it was in our own notes

The refusal is **insensitive to everything we can put in the air messages.**
That points at the chip, not the protocol — and the inventory of primitives we
send has a hole in exactly the right shape:

> `0xFFF1` memory read · `0xF624` status · `0x020A` subscription · `0x0247`
> slot op · `0x0230` assign audio channel · `0x016A` channel config · `0x0251`
> channel params · `0x022D` audio path on · `0x0233` audio disconnect ·
> `0x0208` cipher · `0x0300` page · `0xF626` DSP route · `0x015B` radio ·
> `0x022B` enable token · `0x022C` close registration · `0x0244` transmit
> C-plane

**There is no connection- or bearer-setup primitive anywhere in that list.**
Everything is audio routing (after a call binds) or C-plane. `dect-stubd`
asserts in a comment that "the page is what makes the chip allocate a traffic
bearer"; the sweep is evidence that assumption is false.

And `docs/mac-bringup.md` §80 has had the stock sequence written down since
the MAC bring-up work:

> `0x022C` param 1 → `0x015C` (cell parameters) → **`0x0300`** → **`0x015E` ×2**
> → **`0x0300`**

**Stock sends `0x015E` twice around every page. We have never sent it once.**
`[CR A-0076]` notes in passing that `0x015E` has a **bearer-type field read by
the MAC config** — it was mentioned only to rule it out as a *page* builder,
and nobody asked what it does as a *bearer* request.

That is the live hypothesis now: we page, a C-plane link comes up, we send a
speech `{CC-SETUP}` over it, and **nothing has ever asked the chip for a
traffic bearer** — so "insufficient resources" is exactly right, and no air-
side field could have fixed it. `Q-0055` asks the room for `0x015E`'s layout.
**Do not invent the payload.**

### `0x015E` is the missing bearer allocation `[CR A-0095]` `[2026-10-23]`

`Q-0055` answered, and question 3 came back **yes**.

**Mechanism, verified from the chip:** `0x015E`'s arm bit sets `0x8000` in the
bearer table at **`0x965d + index × 2`**. That table is read by the MAC bearer
handler, indexed by connection, and **nothing else in the task-2 set arms
`0x965d`** — only `0x015E` does.

So the §9.3.2.3 chain is:

> `0x015E` arms the bearer → the MAC allocates it → a speech `{CC-SETUP}` has
> a U-plane to bind to.

Skip it, and `0x965d` is never armed, the MAC never allocates, and a handset
offered speech with no bearer underneath refuses — **insensitive to everything
in the air messages, because the problem is below them.** That is exactly the
shape of the theory sweep: ten air-side variables, one identical `0x32`.

**`dect-stubd`'s comment that "the page is what makes the chip allocate a
traffic bearer" is false**, and it is the `"compiled in"` of this thread — a
sentence both rooms accepted without asking whether anything we send actually
does it.

#### Field map

`mail[0..1]` is the primitive, so `param[N]` = `mail[N+2]`:

| field | bytes | meaning |
|---|---|---|
| handle | `mail[2]` | |
| TPUI | `mail[3:4]` | only read when flags bit 3 is set |
| **flags** | `mail[5]` | **`0x10` ARM**, `0x20` DISARM, `0x08` also build a page |
| opaque | `mail[6:7]` | 16-bit, **not pinned by the room** |
| bearer id | `mail[8:9]` | index = value − `0x196`, armed iff index ≤ `0x27` |

#### What we hold, and what we do not

**We have no stock capture of `0x015E`.** A-0095 suggested reading the values
from "your A-StockTrace capture"; that series is the room's evidence, not
ours, and our own stock-firmware mail capture is still scheduled for the
weekend. So stock's actual bytes remain unknown.

But the field map leaves **exactly two unknowns** for an ARM-only mail:

1. the endianness of `mail[8:9]`;
2. the value of `mail[6:7]`.

That is a **structured sweep, not a guess**, and it is a different position
from the one `f4.sh 41` still refuses — every other byte comes from A-0095's
map. `work/7520/lab/sweep15e.sh` runs eight phases including a **DISARM
control** (`flags 0x20`) which must *not* ring; if it does, the flag map is
wrong and nothing else in the sweep counts.

#### `[CR A-0096]`: both axes resolved, and the sweep re-aimed

- **`mail[8:9]` is LITTLE-endian** — a native word load on a little-endian
  chip. Index 0 (`0x0196`) is bytes **`96 01`**.
- **`mail[3:4]` TPUI is BIG-endian**, assembled by hand as
  `(mail[3] << 8) | mail[4]`. **The two 16-bit fields in the same mail have
  opposite byte order**; copying one to the other is a trap.
- **The ordering is already right.** Stock sends `0x015E` around every page,
  *before* the handset responds and before any link exists — exactly where we
  send it. `0x965d` is not cleared per connection, so an arm persists. Moving
  it later would diverge from stock, not match it. This was the failure mode
  I thought most likely, and it is ruled out.
- Unarmed, the MAC reader "writes a refuse-shaped `0x10`" — which is our
  `0x32` arriving from below the air layer. So the arm is genuinely required.

**What A-0096 declined to assert**, and it is the right call: that arming
index 0 hits the slot the MAC later reads for *our* connection. The range
checks on both sides are branches that room has misread repeatedly this week.
So the index is the real unknown, and the sweep now spends its phases there
rather than on endianness.

**The sweep is self-verifying now.** It dumps `0x965d:80` — all 40 indices —
at 20 ms, so each phase reports whether the arm bit actually took hold,
independent of whether the handset rings. A silent phase that nonetheless
armed a slot is still a result; before this change it would have looked
identical to a phase that did nothing.

#### Result: the arm works, and it is not the cause `[2026-10-24]`

The sweep ran all eight phases. **Every one produced the same three
`e2 32` refusals.** But the `0x965d` dump turned a silent sweep into a
decisive one, and two facts came out of it.

**1. The `0x015E` field map is confirmed on hardware.** Phase 8 sent
`flags 0x20` (DISARM) with bearer id `96 01` at `t=15003 ms`:

| t (ms) | `0x965d` word 0 |
| --- | --- |
| 55 … 15015 | `00 80` (armed) |
| 15075 … end | `00 00` (cleared) |

One mail, one write, 72 ms later, exactly the word A-0095 named. Phases 2
and 3 left indices 1 and 2 armed and they were **still armed when phase 8
started**, confirming A-0096's "an arm persists across connections" —
these are chip-RAM writes that survive a daemon restart. Little-endian
`mail[8:9]`, `index = value - 0x196`, `0x10` arm / `0x20` disarm: all four
now rest on observation rather than on a read of a branch.

**2. The premise was wrong.** Index 0 reads `00 80` at **`t=56 ms`** in
phase 1 — before the first page, and before we had ever sent `0x015E` in
the history of this project. **The chip arms index 0 by itself during
bring-up.** So the model's central claim — "nothing else arms `0x965d`,
so skipping `0x015E` leaves the MAC with no bearer" — is false for index
0, which is the one index that matters. Our ARM in phase 1 was a no-op on
an already-armed slot, which is why it changed nothing.

Phase 8 is the sharper result. It *cleared* the bit the chip had set, and
the two pages after the disarm were still answered and still refused with
`e2 32` — identical to the armed case. **Arming state at index 0 does not
influence the refusal in either direction.**

**`0x015E` is closed as the cause.** Twelve air-side theories and six
bearer-table theories now produce one byte-identical outcome. That
consistency is itself the finding: we are not varying the thing that
matters.

## 2026-10-24: the {CC-SETUP} was 3.9 seconds late, and everything before this is void

### The defect

`dect-stubd`'s poll loop open-coded `dect_link_tick()` as `dect_lapc_tick()`
+ `dect_cc_tick()`. Those are two of that function's four duties; it silently
dropped `dect_mm_tick()` and, decisively, `drain_nwk()` -- the step that moves
a queued NWK message from the CC output queue into LAPC.

`dect_link_rx_segment()` was then the only remaining caller of the drain. So a
message queued **outside** an inbound-segment context had nothing to push it.
An FP-originated `{CC-SETUP}` is exactly that: `reg_segment()` calls
`ring_queue_setup()` *after* `rx_segment()` has already drained.

Measured in `vBR01`:

| t (ms) | event |
| --- | --- |
| 21464 | page response parsed, `{CC-SETUP}` queued |
| 21464 | `tx cplane 7 bytes` -- the LAPC ack, and nothing else |
| 22892 … 25300 | **silence: no C-plane transmit for 3 860 ms** |
| 25324 | the handset sends a `{CC-SETUP}` of its own |
| 25324 | our stale `{CC-SETUP}` finally goes out, woken by that inbound frame |
| 25744 | `93 5a e2 32` |

Every `reg: tx cplane` line in the corpus immediately follows an inbound
`prim 0203`. The transmit path was **purely reactive**: nothing left the base
until something arrived.

### Why that produced exactly the symptom we chased

EN 300 175-5 §9.5.2.3 "Abnormal release in case of call setup collision":

> The PT shall perform abnormal release of the FT-initiated call (incoming
> call), by sending a {CC-RELEASE-COM} … **using release reason 'Insufficient
> resources'**.

So `0x32` was not a complaint about our message. It was the **spec-mandated
answer to a collision**, and the collision was manufactured by our own
latency. The corpus agrees without exception:

- every log with a refusal has a handset `{CC-SETUP}` within **100 ms** of
  ours, and the refusal lands **220 ms** after ours -- the same two numbers in
  every run;
- every log where the handset's own setup is seconds away has **no refusal**;
- `vSW08`, which never sent a setup, has no refusal.

### Consequence: eighteen theories are void, not eliminated

Page header 0/1/3/4, `<<BASIC-SERVICE>>` 0x88/0x80/0x28/0x40, `<<SIGNAL>>`,
`<<CALL-INFORMATION>>`, CLIP, ciphering, the retry budget, TPUI
acknowledgement, and the six `0x015E` bearer-arm phases were **all** measured
while the setup arrived ~3.9 s late and was answered by a collision. None of
them was ever read by the handset's attribute check. They are back in play.

Two of them were worse than void:

- **the ciphering phase tested nothing.** Call ciphering is started in the
  `{CC-CONNECT}` branch, which an unanswered incoming call never reaches, so
  `-C` and no `-C` produce an identical incoming call.
- **the `<<BASIC-SERVICE>>` phases also moved the page byte**, because
  `ring_page_info()` keys on the service nibble. That was already recorded.

### The fix

`dect_link_tick()` in the poll loop, as `dect_link.h` has always said
("Call once per poll loop"), plus a drain in `reg_segment()` right after
`ring_queue_setup()` so the setup leaves in the same breath as the page
response rather than up to `POLL_MS` later.

`vFIX` confirms it: queued at `t=76514`, on the wire at `t=76514`. **Latency
3 860 ms → 0 ms.** The handset's own setup came 2 660 ms later, so there was
no collision, and no call was in progress at the time (the previous service
call closed at `t=74654`, the next opened at `t=79274`).

**And the handset still refused, in 240 ms.** So the collision was real, was
ours, and was not the whole story. What it bought us is a clean instrument:
for the first time the handset's attribute check is actually reading what we
send.

### Corrected along the way

- `<<BASIC-SERVICE>>` call class **8 is "Normal call setup"** (EN 300 175-5
  §7.6.4 figure 18); external handover is **12**. Our
  `docs/autonomous-2026-10-23.md` table mislabelled every row, and A-0069's
  prescribed `0x40` is call class 4, **"Message call setup"** -- not a voice
  call. `RING_BASIC_SVC` was already `0x80` and is correct.
- `0x015E` is **ACCESS_MANAGEMENT** `[CR COMMAND_SET §6.3]`, not bearer
  allocation; `0x196–0x1BD` is a special **handset-number** range with an
  enable flag, and the MAC reject codes `0x0D`/`0x10`/`0x12` it gates are
  MAC-layer codes, not the NWK `<<RELEASE-REASON>>` we receive. A-0096's
  identification of the two was a layer confusion.

### What EN 300 444 actually requires of the message

Table 21, the mandatory content of an FT `{CC-SETUP}` for an incoming call:

| IE | value | ours |
| --- | --- | --- |
| `<<PORTABLE-IDENTITY>>` | type 0 IPUI | ✓ |
| `<<FIXED-IDENTITY>>` | type 32 PARK, whole PARK | ✓ `a0 a5 …` |
| `<<BASIC-SERVICE>>` | call class 8, basic service 0 | ✓ `e0 80` |
| `<<SIGNAL>>` | 40H–47H, 48H, 4FH | **omitted** |
| `<<CALLING-PARTY-NUMBER>>` | only with feature [N.30] | optional |

`<<CODEC-LIST>>` is **not** in table 21, and EN 300 175-5 §9.3.2.1 says that
when `<<BASIC-SERVICE>>` indicates default attributes the service is defined
by the annex E defaults and no further attribute elements shall be included.
We send one anyway: `90 02 00 84` = codec negotiation, G.726 32 kbit/s,
LU1/INA, full slot, CS only. The A540's own list is `90 03 00 01 02 00 84` --
G.722 64 kbit/s on a **long slot** first, G.726 full slot second. We offer
only its fallback.

### The content sweep: it is not the message `[2026-10-24]`

Eight phases with the timing fixed. Phases 1–2 were void (the handset had not
re-attached inside a 60 s settle); the rest all refused identically.

| # | variable | result |
| --- | --- | --- |
| 3 | no `<<CODEC-LIST>>` | `0x32` |
| 4 | **exactly EN 300 444 table 21**: `<<SIGNAL>>` 40H, no codec list | `0x32` |
| 5 | page header 4 (GAP voice code) | `0x32` |
| 6 | page header 1 (PP negotiates attributes) | `0x32` |
| 7 | the handset's own codec list + `<<BASIC-SERVICE>>` 0x88 | `0x32` |
| 8 | baseline again | `0x32` |

Phase 8 matches the earlier baseline, so the sweep is sound. **The GAP
mandatory message, with nothing added and nothing missing, is refused.**

### `[CR A-0076]` corrected: the page info byte is not inert

A-0076 said the chip never reads `mail[2]` of `0x0300`, so `0x0C` and `0x2C`
produce byte-identical pages, and told us never to treat it as an experimental
variable again. That is wrong, and phase 7 shows it. `COMMAND_SET §8.2` gives
`0x0200` CONNECTION_ESTABLISHED as `{handle, wideband}`:

| phase | page byte | page-response connection |
| --- | --- | --- |
| 3, 4, 5, 6, 8 and vFIX | `0c` | `0x0200 {01, **00**}` |
| **7** (`basicsvc=0x88`) | `2c` | `0x0200 {01, **01**}` |

One byte in the page, one bit in the connection the handset then establishes.
**We can steer the connection type from the page after all** — which is a
capability we thought we did not have, even though it did not make the call
succeed.

### The U-plane sweep: the audio channel is not it either

`earlybind` assigns the audio channel on the page response instead of on
`{CC-CONNECT}` — which an unanswered incoming call never reaches — following
`COMMAND_SET §9.2`, where the host's assignment comes straight after "radio
connection up", and `[CR A-StockTrace-5 §15]`, where stock has a bearer
~360 ms before its setup.

It works, and it does not help. In `vBIND01` the connection came up at
`t=75459` as `{01, 00}`, our early `0x0230` went out, and the chip confirmed
at `t=75607` with `0x0242 {01, 41, 02, 00}` — channel 1, wideband bit set,
slot mask 2. **The connection had an assigned audio channel before the
`{CC-SETUP}` and the handset still refused.**

So "the handset is being offered speech on a connection with no U-plane" is
not the explanation, or at least not one the host can fix with `0x0230`.

### The timing sweep: not that either, and the knob was verified `[2026-10-24]`

`[CR A-StockTrace-5 §15]` measured a connection bearer on stock ~360 ms before
its `{CC-SETUP}`. We send ours 120 ms after connection-up. Five phases, with
the gap from connection-up to the setup on the wire measured in each log
rather than assumed:

| phase | `setupdelay` | measured gap | result |
| --- | --- | --- | --- |
| 5 (control) | 0 | **120 ms** | `0x32` |
| 1 | 360 | **480 ms** | `0x32` |
| 2 | 800 | **912 ms** | `0x32` |
| 4 | 1600 | **1 720 ms** | `0x32` |
| 3 | 360 + wideband connection | 480 ms | `0x32` |

Stock's 360 ms is bracketed in both directions and nothing moves. The delay
was confirmed to have taken effect in every phase — the daemon logged the hold
and the measured gap matches the requested value plus the ~120 ms
connection-up-to-page-response interval. **Timing is not the axis.**

## Where this leaves it

Nineteen variables, one handset, one answer. With the transport defect fixed
and two agreeing baselines, the following are measured and refused: message
content including the exact EN 300 444 table 21 message; page headers 0, 1, 3,
4; narrowband and wideband page-response connections; the handset's own
preferred codec list; an audio channel configured before the setup and
confirmed by the chip; `0x015E` arming at any index on either handle; and the
setup-to-bearer gap from 120 ms to 1 720 ms.

**The uniformity is the finding.** Nineteen phases agreeing is not nineteen
eliminations of a shared cause — it is evidence that the varied thing is not
in the causal path. Every theory this week has asked "which of the mails we
send is wrong", and the answer keeps coming back "none of them". The question
that fits the data is **"which mail are we not sending at all"**, and no
amount of reasoning from the chip's receive side answers it.

Two things settle it, in order, and both are cheap:

1. **Ring a second handset** (`docs/test-card-2026-10-25.md`). Nineteen
   variables have been varied against one A540; the handset itself never has.
   Ten minutes, and it decides whether we are debugging our base or a device
   quirk.
2. **The stock capture.** What is wanted is the complete ordered host→chip
   mail list between the page and the `{CC-SETUP}`, with timings, including
   mails we have never sent. The clean room holds the chip's full host→chip
   dispatcher and has offered to diff stock's sequence against our inventory
   and name anything missing.

Until one of those runs, further sweeps are not worth their bench time.

### The DSP U-plane diff: the reference regions are static here `[2026-10-24]`

`[CR A-0098]` mined a working-call chip dump and named two regions that are
empty at idle and populated during a live call: `0x3D4`–`0x414` (slot /
U-plane buffer) and `0xEF4`–`0x1694` (Gen2DSP lane-descriptor table, said to
be short at idle and rebuilt and grown during a call). The proposal was to
dump them on our failing page-response connection and diff.

Run: 210 s, `0x3D4:64` and the **whole** `0xEF4:1952` at 15 ms, settle long
enough for a deep-sleeping A540 (the maintainer confirmed the handset was
alive, just asleep). 394 inbound C-plane segments and **seven** connections,
six of them the handset's own working ones and one ours:

| t (ms) | `0x0200` wideband byte | whose |
| --- | --- | --- |
| 5679 | 0 | — |
| 26799, 50411, 81990, 129550, 177109 | 1 | the handset's |
| **146330** | **0** | **ours, from the page** |

**Of the 63 chunks covering both regions, exactly one ever changed, and it
changed at `t=1063` — during start-up, 4.6 s before the first connection and
then never again for 208 seconds.** A boot artefact. Every other chunk is
byte-identical across all dumps, through idle, through six working
connections and through ours.

**So these addresses carry no connection state on this chip.** The regions are
not unreadable and not zero — they hold stable content, and the first attempt
at a narrow 128-byte window was correctly widened to the full 1 952 bytes in
case the growth happened past it. It did not.

The instrument is sound: the same `0xFFF1` read is address-sensitive (the six
chunks differ from each other) and demonstrably tracks live state elsewhere —
it caught the `0x015E` arm bit flipping at `0x965d` 72 ms after the mail,
earlier the same night.

The likeliest reading is that the room's `cd3` tool and our host-side read do
not address the same space, or the idle/active dump it mined came from a
different firmware. **The diff is closed as inconclusive-by-address**, not as
evidence either way about our U-plane — and it does not need re-running
without a corrected address.

Two earlier attempts were void because the A540 had gone into deep sleep and
answered no pages; that is a handset state, not a fault, and the fix is simply
a longer settle.

## 2026-10-24, evening: the corpus, tabulated — every refusal sits on a handset-originated call

The question that had not been asked: not *what is in our SETUP*, but
*what was the handset's own CC transaction doing when it arrived*. Every
FP-originated `{CC-SETUP}` in `work/box-tmp/*.log` (93 logs), classified
by the peer's state:

| outcome | handset's own CC at our SETUP | runs |
|---|---|---|
| ALERTING / CONNECT (the C4) | no handset-originated call near | **26** |
| `{CC-RELEASE-COM}` 0x32 | its own SETUP crossed ours by 100–300 ms, or its call was active | **33** |
| `{CC-RELEASE-COM}` 0x32 | nothing of the handset's visible before the refusal | 25 |
| `{CC-RELEASE-COM}` 0x32 | poisoned-identity runs (C430A, AS690H) | 2, void |
| no reply | mixed | 9 |

No successful ring coincides with a handset-originated call; no refusal
is free of one on the wire except the 25, and in those the handset's
next list-access call follows the refusal by 1–7 s in most runs. The
33 are EN 300 175-5 §9.5.2.3 verbatim — the clause's own figure shows a
PT's *LiA service call* crossing an FT's *normal call setup*, with
exactly our signature as the mandated PT answer.

Two readings were left. (1) A pure §9.5.2.3 crossing, with the PT's half
sometimes suppressed before its SETUP reached the air; our half (release
its call with 0x32, re-present ours immediately) was never implemented.
(2) The handset refuses any incoming call while its list sync is pending,
and the sync is pending for ever because our list-access replies never
delivered an entry. Both were implemented; the bench separates them —
`docs/journals/call-g7.md`, "THE REFUSAL IS A {CC-SETUP} COLLISION" and the two runs
after it.

### Earlier rejections of the collision reading, re-read

Both earlier rejections (2026-10-22, and "The content sweep: it is not the
message" above) rested on *"no competing `03 05` from the handset"* in a
few traces. The absence was real and the inference was not: a PT in CALL
INITIATED has accepted an MNCC_SETUP-req and not necessarily transmitted
anything yet, and a handset with a permanently pending sync enters that
state on every link-up. Absence of the handset's SETUP on the air was
never evidence that the handset had no call pending.

### List access, audited against TS 102 527-3 V1.7.1

What the first responder sent, and what the standard says:

| item | ours (until 2026-10-24) | TS 102 527-3 |
|---|---|---|
| LiA service call answer | `{CC-CALL-PROC}` + `{CC-CONNECT}` | CALL-PROC only, "no {CC-CONNECT} is used" (7.4.10.6.1) |
| start session confirm, total entries | always 2 | the list's count (table 43) |
| start session confirm, sorting fields | always 1 field | 0 for the one-entry lists, the default field otherwise (7.4.10.4.1.2) |
| read entries confirm | "3 entries" | the count actually delivered (table 49) |
| data packets after the confirm | **none, ever** | mandatory: "Content of list entry is transmitted in data packets" (7.4.10.4.3.2) |
| unsupported list | confirmed like any other | session 0, reason "list not supported" |
| search entries | NACK reason 01 "entry not available" | NACK 06 "procedure not supported" |

Now served: List of Supported Lists, Internal Names List, Line Settings
List, with the field codings of 7.4.10.5.1.1/2/3/13 and 7.4.11.4.1–3, and
query supported entry fields. Not served, and said so in list 00: the
call lists (01–04, 09), the Contact List (05) and the DECT System
Settings List (07). Table 41 makes 01, 03, 05 and 07 mandatory for an FP,
so that is the next conformance gap, in that order of handset impact.

## The review of 2026-10-24: one cause found, the rest still open `[2026-10-24 night]`

An external review of commit `e899958` (`the clean-room record/an external review-24102026.md`)
listed ten findings. All ten were checked against the code, `docs/specs/` and
the thirteen clean `vCOLLIDE` logs (`docs/journals/call-g7.md`, "External review").

**Fixed:**

| finding | clause | commit |
|---|---|---|
| transaction value rotated per call | EN 300 444 §6.9.2, lowest free value | `dd5a630`. **This was the Gigaset refusal**: run 15 (value 0) rang the A540, run 16 (value 1) was refused |
| mail parser copied 64 KiB on a length field below 2 | host/chip framing | `aa41389`, with an ASan-reproduced test |
| end-of-run park queued after the loop, never sent | AGENTS.md §6 | `8ded31a`, verified on hardware in runs 14-16 |

**Open, none of them a cause of the refusal (each checked against the logs):**

| finding | clause | where |
|---|---|---|
| a `{CC-RELEASE}` / `{CC-RELEASE-COM}` on any transaction releases our call; the reply goes out on our transaction | EN 300 175-5 §17.3.2.1: no action for an unknown transaction | `dect_cc_rx()` |
| the default branch tests the TI flag the opposite way from the SETUP branch, so an unhandled message on our own call is closed as "unknown transaction"; `{CC-CALL-PROC}` has no case | §17.3.2.1; EN 300 444 §6.9.4: ignore | `dect_cc_rx()` |
| `{CC-ALERTING}` / `{CC-CONNECT}` change our call without checking the transaction | §17.4.1 | `dect_cc_rx()` |
| link release sends a Class B DISC with P/F set on a Class A link; the bench saw it fail on 2026-10-23 | EN 300 175-4 §7.11.8, §9.2.1.1, §9.2.3.7 | `dect_lapc_disconnect()` |
| no alerting by default | EN 300 444 §8.14 | `-a signal=` |
| default call codec list is G.726 only; our CONNECT for PP-originated calls names no chosen codec | TS 102 527-1 §7.3.3 | daemon default, `on_setup()` |
| no line identifier / call identifier in the incoming SETUP by default | TS 102 527-3 §7.4.5.3.2, §7.4.6.3 | controller-owned data, `-a lineid=,callid=` |
| no pseudo call waiting when a voice call meets an open LiA session | TS 102 527-3 §7.4.10.6.3 | `ring_queue_setup()` |
| any LCE message counts as the page response; `CONN_DOWN` is not checked against its handle | TS 102 527-1 §7.3.8.2.1, §7.3.8.3.1 | `dect_link_rx_segment()`, daemon |

## Second review (an external review), verified `[2026-10-24 night]`

`the clean-room record/an external review-1-24102026.md` audited the same commit
(`e899958`) and listed eight findings that do not overlap the first review.
Each was checked against the current source (`dect_mm.c`, `dect_lapc.c`,
`dect_cs.c` and `dect_link.c` are unchanged since `e899958`) and, where a
clause applies, against `docs/specs/`. **All eight hold in the code; two need
a narrower fix than proposed. None touches the A540 refusal, which was the
transaction value.** All open.

| # | finding | verdict | the correct fix |
|---|---|---|---|
| 1 | `on_locate_request()` accepts a `{LOCATE-REQUEST}` with no `<<PORTABLE-IDENTITY>>`; with no stored subscription it accepts any IPUI | **confirmed**. The element is M in EN 300 175-5 table 56 | §17.6.4: return `{LOCATE-REJECT}` "information element error" when it is missing; "IPUI unknown" when we hold no subscription. `test_locate()` encodes the second case as an accept and must change |
| 2 | `pump_lapc()` ignores `send_mail()`'s result; `tx_block()` dequeues control bytes before the block write succeeds | **confirmed**, never fired: `queue_drops 0` in all 162 logs. LAPC retransmits a lost I-frame by timer, but not a lost RR or UA | dequeue only after a full `write()`; on a full queue, stop polling LAPC rather than drop the frame |
| 3 | an oversized reassembly resets `rxmsg_len` and carries on, so the tail of the message is delivered as a new one | **confirmed in part**. Acknowledging is allowed: EN 300 444 §6.9.3 lets the receiver discard further segments | discard until the segment with M = 0, and deliver at most the first part; add a test with a message over 256 octets |
| 4 | `{TEMPORARY-IDENTITY-ASSIGN-ACK}` sets `tpui_confirmed` with no assignment pending | **confirmed** | accept it only while `ident_pending`, on the MM transaction that sent the assign |
| 5 | `dect_lapc_rx()` checks neither LLN nor SAPI | **confirmed in part**. Accepting any LLN is deliberate: the PT opens on a Class B LLN and EN 300 175-4 §9.2.4.3 lets us treat it as Class A, so strict LLN checking would break attach | check the SAPI (0, connection-oriented) only |
| 6 | `sub_save()` truncates and rewrites the store in place | **confirmed**. A crash or full overlay mid-write loses the UAK, and re-registration needs the user | write a temporary file, `fsync()`, `rename()` over the store; `fchmod(0600)` an existing file |
| 7 | `dect_cs_decode_segment()` does not check the `0x01` marker or the trailing copy of the handle | **confirmed**, defensive only | reject a segment whose marker is not `0x01` or whose two handles differ |
| 8 | an odd number of hex digits in `-M` steps `par` past the NUL | **confirmed**, local command line only | reject odd-length parameter strings before the loop |

## Status of both reviews after the fix batch `[2026-10-24 night]`

All verified on the A540 in runs 19 and 20 unless marked "unit-tested".

| finding (review, #) | commit | status |
|---|---|---|
| transaction value rotated (an external review #9) | `dd5a630` | **fixed; was the Gigaset refusal** (runs 15-17) |
| no alerting by default (an external review #5) | `9c4414c` | **fixed**, `<<SIGNAL>>` 0x41 default; silent ring without it (run 18) |
| CC transaction matching (an external review #3) | `0ac7e38` | **fixed**; CONNECT/CONNECT-ACK verified (runs 19, 20) |
| mail parser underlength (an external review #1) | `aa41389` | **fixed**, unit-tested (ASan) |
| end-of-run park never sent (an external review #2) | `8ded31a` | **fixed**, park line in every run since 14 |
| any LCE message counted as the page response (an external review #10) | `02b9039`, `{LCE-PAGE-REJECT}` in API 13 | **fixed**; A540 accepted (run 20). `{LCE-PAGE-REJECT}` plus link release on a mismatch (TS 102 527-1 §7.3.8.3.1): unit-tested, not yet seen on hardware |
| CONN_DOWN not checked against its handle (an external review #10) | `12cb980` | **fixed**; 102 misattributed releases in the corpus |
| LOCATE-REQUEST identity (an external review #1) | `7e7e5f4` | **fixed**, plus the pre-authentication overwrite; A540 accepted (run 20) |
| transmit loss on failed write (an external review #2) | `35a7c20` | **fixed**; `write_short 0` (run 20) |
| over-long reassembly (an external review #3) | `e5bc10b` | **fixed**, unit-tested |
| unsolicited TPUI ACK (an external review #4) | `edaaeb3` | **fixed**, unit-tested |
| LAPC SAPI (an external review #5) | `2fd24ae` | **fixed** (SAPI only, LLN deliberately not), unit-tested |
| non-atomic store (an external review #6) | `c615aa9` | **fixed**; save path host-checked, not yet exercised by a registration |
| segment marker/handle (an external review #7) | `398d1f7`, `981f546` | **fixed**; zero violations in 42 252 corpus segments |
| odd hex in `-M` (an external review #8) | `ea52963` | **fixed**, checked at startup |
| Class B DISC on a Class A link (an external review #4) | — | **open**: Class A release is the MAC connection release; which chip mail does it is a question for the room |
| CONNECT names no chosen codec (an external review #8) | API 14 | **fixed** for handset-placed calls: G.726 alone in {CC-CONNECT} when the SETUP negotiates (TS 102 527-1 §7.3.3); unit-tested, not yet on hardware |
| our {CC-SETUP} lists G.726 alone (an external review #8) | see call-g7 runs 31-33 | **fixed**: no list by default, basic speech means G.726 (TS 102 527-1 §7.3.3); all three Gigasets ring without it |
| no CAT-iq line/call identifier by default (an external review #6) | — | **open**, controller-owned data; the A540 rings without it |
| no pseudo call waiting into an open LiA session (an external review #7) | — | **open** |
