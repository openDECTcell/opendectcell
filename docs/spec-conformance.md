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
- §7.7.53 note 1 is a feature we do not have yet: if the calling number matches
  an entry in the FP contact list, the name from that list should be sent with
  screening "user-provided, verified and passed". That belongs with the
  controller's contact list, not here.

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
