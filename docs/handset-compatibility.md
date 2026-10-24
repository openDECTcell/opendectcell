# Handset compatibility

One table, kept current, of what each handset has actually been observed to do
against **our** base. It exists to answer a question that keeps coming up --
"should `libopendectcell` grow per-handset support?" -- with evidence instead of
impressions.

**The finding so far is that it should not, yet.** Every difference below that
looked like a vendor quirk turned out to be **our** non-conformance, which the
FRITZ!Fon C4 tolerated and the Gigasets did not. Writing a per-device
abstraction now would freeze our own bugs as "quirks". The working assumption
is therefore: **implement the standard, keep this table, and revisit only when
two demonstrably *conformant* handsets need genuinely different FP behaviour.**

## The handsets

| | FRITZ!Fon C4 | Gigaset A540 CAT | Gigaset C430A |
|---|---|---|---|
| vendor | AVM | Gigaset | Gigaset |
| role here | the handset G1-G7 were developed against | current bench handset | arrived with the A540; used by the the hardware-analysis step for stock captures |
| registered to our base | yes (2026-10-18) | **yes, 2026-10-22**; again after factory reset, slot 1 | **yes, 2026-10-24**, after factory reset, slot 2 |
| local status | a handset, on the a second unit, **off-limits for experiments** | spare | spare |

The **Gigaset AS690H** joined on 2026-10-24: registered in slot 3 after a
factory reset, and it rings aloud and is answered on the default build (runs
22b, 23). Unlike the A540 it placed no list-access call of its own in any run
we hold.

## Observed behaviour

| behaviour | C4 | A540 CAT | C430A | reading |
|---|---|---|---|---|
| GAP registration against our MM stack | passes | **passes** | not attempted | our MM is not AVM-specific |
| post-LOCATE message | not recorded | bare `0x5D`, `nwk_len=2`, no IEs | bare `0x5D` **and** `0x64` + escape `0x59` | `0x5D` is common; `0x64`/`0x59` is C430A-specific |
| incoming `{CC-SETUP}` on transaction value 1 | **rings** | **rejects**, `0x32` "Insufficient resources" | not attempted | the C4 is *tolerant*; the A540 enforces EN 300 444 §6.9.2, lowest free value = 0 (runs 14, 16) |
| incoming `{CC-SETUP}` on transaction value 0 | **rings** (v28 and every completed C4 call) | **rings**, G.722 chosen in `{CC-ALERTING}` (run 15, 2026-10-24); default build, answered (runs 19, 20, 24) | **rings, answered** (run 21, 2026-10-24) | the default since `dd5a630`; the earlier reading "no bearer, §9.3.2.3" was wrong |
| PP-originated call binds audio | yes | yes | yes | the handset builds the bearer, so this path always worked |
| opens a periodic CAT-iq service call unprompted | no | **yes** -- `03 05`, then `03 60 77 04 c3 00 NN 00` with an advancing counter, then `03 4d` normal release, ~13 s cycle | not measured | likely LiA list access; harmless, but it misled the collision investigation |
| re-sync time after our base leaves the air | not measured | **~30 s**; 162 s after 17 min off untouched, ~2 s after a key press (runs 20, 23, 24) | 11 s in hand (run 21) | the handset backs off its search the longer the base is gone; a key press restarts it |
| display with call class 8 (`0x88`) | "Intern", no number | not reached | n/a | call class, not CLIP encoding, drives the C4 display |
| display with call class 3 (`0x38`) | shows the number, call dies ~4 s | not reached | n/a | the dying call is our `CALL_PRESENT` hole, not a handset trait |

## Differences that are genuinely per-vendor

Only one so far, and it is vendor-scoped **by definition** rather than by
quirk: the handset **name and clock push**, carried in `<<ESCAPE-TO-
PROPRIETARY>>` (`0x7B`) under **EMC `0x0002`**, sub-tag `0x54` (name) and
`0x3B` (BCD date/time). See `registration-g6.md`. `[observed]` EMC `0x0002` is
used in both directions and by a factory-reset third-party Gigaset, so emitting
it is interoperation rather than impersonation.

When this is implemented it should be a **feature flag**, not a device
abstraction: gate it on our own `LOCATE-ACCEPT`, which every handset reaches,
and not on `0x59`/`0x64`. There is now a positive reason for that and not just
a cautious one: `0x59` is the handset's **acknowledgement** of the base's
`0x5B` sequence counter `[CR A-StockTrace-4 §3]`, so it is a response, never a
request. The earlier reading of it as an identify that triggers the push was an
artefact of the interleaving (push N, ack N, push N+1, ack N+1).

The channel is **sequenced and acknowledged**: one monotonic byte per
subscription, `+1` on every push, shared across name (`0x54`), time (`0x3B`),
settings (`0x58`) and softkey labels (`0x26`). A value the handset has already
seen will be deduped, so a base that reuses one would push once and then
silently stop updating.

**The answer is capability detection, not a per-model allowlist.**
`[CR A-StockTrace-5]` makes the point that settles the design question at the
top of this file: **whether a handset implements the EMC `0x0002` channel is
observable at registration.** A handset that sends EMC `0x0002` escape records
during ACCESS-RIGHTS / LOCATE implements it; one that sends none does not. So
gate the push on "did this subscription use the `0x0002` channel while
registering", recorded per subscription at runtime -- never on a model string.

That is the clean form of per-handset support: not a table of devices, which
ages badly and encodes our own bugs as quirks, but a capability learned from
what the handset actually did. It also degrades correctly for handsets nobody
has ever seen.

`[observed]` the C430A uses the channel **proactively, not only to
acknowledge**: six handset-originated EMC `0x0002` escape IEs that are not
acks -- capability records `0x06`/`0x29` and `0xa1` during registration, and a
`0x58` settings record with its own counter at re-attach. Our A540 sends
**none of these, ever**.

**Open, and the first real candidate for per-handset behaviour:** `[observed]`
our **A540 CAT has never sent a single `0x7B` escape IE** -- not in
registration, locate, or any call, across every run we hold. The C430A sends
them. **This does not establish that the A540 cannot receive a push**: the
C430A's frames are acks, and our base has never pushed anything, so the A540
has had nothing to acknowledge. The test is cheap once the push is implemented
-- send a name and watch for a display change and a `0x59` carrying our
counter. If the A540 ignores it, this becomes the first genuine case of two
conformant handsets needing different FP behaviour -- but it is handled by the
capability detection above rather than by a device abstraction, so the
recommendation at the top of this file stands.

`[assessed, CR A-StockTrace-5]` the ack is integral to the channel, so a
handset that cannot emit `0x7B` almost certainly cannot be a receive-only
participant either: it could not acknowledge a push, which would stall the
stock flow. `[not established]` the receive direction for the A540 -- there is
no capture of one being pushed to, and our own test is still the definitive
check.

`[not established]` who owns EMC `0x0002`. It is used by the AVM base and by
the C430A but apparently not by the A540 -- **both Gigasets** -- so it reads as
a model/firmware-specific implementation of an AVM extension rather than a
universal Gigaset feature. Registry ownership remains unchecked.

## What would change the recommendation

Add a per-handset mechanism when, and only when, **two handsets that both
conform to EN 300 175 require different FP behaviour**. The one candidate is
the call-class/display interaction in the table above, and it is not proven:
the C4's class-3 call died from a defect of ours, so that row does not yet
describe handset behaviour at all.

Until then, every entry above is a to-do against the **standard**, not against
a device.

## The handsets declare different things about themselves

`[observed]` parsed from our own registration and locate captures. This is the
concrete, byte-level difference between the handset that has completed an
FP-originated call and the one that refuses every one.

| element | FRITZ!Fon C4 | Gigaset A540 CAT |
|---|---|---|
| `<<TERMINAL-CAPABILITY>>` `0x63` | **17 octets** `25 15 0a 00 30 09 10 81 02 00 00 00 00 40 92 c0 a0` | **15 octets** `25 35 0a 00 0c 01 8e 02 00 00 00 00 40 96 82` |
| `<<MODEL-IDENTIFIER>>` `0x78` | **`0b 74 08`** | **absent** |
| `<<ESCAPE-TO-PROPRIETARY>>` `0x7B` | present, EMC **`0x0B74`** | **absent, in every message we hold** |
| `<<CODEC-LIST>>` `0x7C` | `90 03 00 01 02 00 84` | identical |

Two things follow.

**The A540 declares no model identifier and uses no proprietary channel.**
Across every message in every log we hold it sends `0x05`, `0x0A`, `0x63` and
`0x7C` and nothing else. The C4 additionally declares a MODEL-IDENTIFIER of
`0b 74 08` and speaks `<<ESCAPE-TO-PROPRIETARY>>` under EMC `0x0B74` --
evidently AVM's own code, since `0b 74` appears as both its MANIC and its EMC.

**Stock keys per-handset behaviour on exactly that element.**
`[CR A-StockTrace-5 §16]`: the stock FP reads `IE_MODEL_ID` from
ACCESS-RIGHTS-REQUEST, matches it with a 7-byte `memcmp` against a stored
signature plus a model-digit check, and for matching Siemens/Gigaset terminals
`a stock capability adjustment` rewrites a capability field from 12 to
14 when a sub-capability exceeds 13. So AVM's firmware **does** special-case
handsets by declared model, at registration time.

`[not established]` that this is why the A540 refuses our calls. The adjustment
is a registration-time terminal-capability tweak and the causal chain to the
missing traffic bearer is unproven -- the room is explicit about that. But the
two handsets declare **different-length capability structures**, one of them is
the family stock adjusts, and only one of them completes an FP-originated call.
That is worth testing before anything more speculative.

### This changes the recommendation at the top of this file

The earlier conclusion was "implement the standard, not a device table, because
every apparent quirk so far has been our own non-conformance". The first half
stands. The second half is now **contradicted by evidence**: stock ships a
model-keyed capability adjustment, so at least one real FP in the field found
the standard insufficient and special-cased handsets by model.

That does not resurrect a device table. The better form is still the one in the
section above -- **detect, do not hardcode** -- and `<<MODEL-IDENTIFIER>>` is
itself a declared capability, observable at registration, which fits that shape
exactly. What changes is the expectation: **we should plan for per-model
adjustment to be necessary eventually**, rather than treating it as a smell to
be avoided. AVM did not do it for fun.

## `<<TERMINAL-CAPABILITY>>` decoded (EN 300 175-5 §7.7.41)

`[observed]`, decoded from our own captures against the standard. This kills
the capability lead and corrects two readings of it.

| | C4 | C430A | A540 |
|---|---|---|---|
| tone / display | dial tone only / full display | same | same |
| echo | minimum TCLw | minimum TCLw | **TCLw > 55 dB (VoIP)** |
| **slot types (octet 3c)** | **`0x0a` long j=640 + full** | **`0x0a` identical** | **`0x0a` identical** |
| stored display chars | 48 | 48 | **12** |
| display lines | 9 | 3 | **1** |
| characters per line | 16 | 16 | **14** |
| PI_1 | GAP | GAP | GAP |
| **PI_7** | Part 1 wideband, re-keying | same | **Part 1 wideband + NG-DECT Part 3, re-keying** |

**1. All three declare the same slot capability.** Octet 3c is `0x0a` on every
handset: long slot j=640 **and** full slot. **The A540 declares exactly the slot
a wideband G.722 call needs.** So "the handset cannot do the slot" is false, and
offering it something lesser is not the fix.

**2. The octets stock keys on are display geometry, not service capability.**
`[CR A-StockTrace-5 §17]` found `a stock capability adjustment`
rewriting a field 12 -> 14 when `content[4]` exceeds 13, and the two Gigasets
differ at `content[4]` (`0x30` = 48 vs `0x0c` = 12). Decoded, `content[3..4]` is
**"number of stored display characters"** and `content[5]`/`content[6]` are
**lines** and **characters per line**. The A540 is a 1-line, 14-character
handset; the C430A is 3 lines of 16. So AVM's model-keyed adjustment is a
**display-geometry fix** -- plausibly so their UI strings fit -- and has no
visible connection to bearers or services.

**3. The A540 declares MORE than the others, not less.** `[CR A-StockTrace-5
§17]` described it as declaring "materially lower capabilities", and that
reading came from `content[4]`, which is the display field. On the only octet
that carries protocol support, PI_7, the **A540 is the only one of the three
that declares NG-DECT Part 3** (TS 102 527-3, the CAT-iq 2.0 feature set); the
C430A and the C4 declare Part 1 wideband voice only. It also declares the
strictest echo class. **The A540 is the most capable handset we own on paper.**

**So the capability lead is dead too.** Nothing in what the A540 declares
explains a missing traffic bearer: it claims the right slot, more profile
support than the handsets that work, and an identical codec list. What remains
unexplained is still why the **bearer** never appears -- which `[CR
A-StockTrace-5 §16]` established is **MAC-initiated**, with no DLC frame
triggering it.

**The caveat that should have been raised sessions ago, and is now recorded:**
we have **never established that stock rings an A540.** Every "stock rings it,
we do not" comparison in this project is C430A-on-stock against A540-on-ours --
different handsets. It remains possible that no base rings this handset the way
a C4 or C430A is rung, and that its refusal is correct behaviour we have
misread as our bug. Nothing yet distinguishes those two worlds.

### Settled: stock rings the A540 (2026-10-22, maintainer)

`[observed by the maintainer]` The Gigaset A540 CAT **rang without trouble**
during the Freetz / FRITZ!OS sessions on **this same spare 7520**. So a stock
base on this very hardware pages that handset and it alerts.

That closes the doubt raised above. The comparison is no longer
C430A-on-stock against A540-on-ours: it is **the same handset, on the same
box, ringing under stock and refusing under ours**. The handset is not the
problem and its capability declaration is not the problem. **Our base is.**

Two consequences. There is no case for developing against the C4 instead, and
no case for "offering the A540 what it can do" -- it demonstrably takes the
ordinary incoming call. And the remaining difference must lie where the
measurement already points: `[CR A-StockTrace-5 §15/§16]` a connection bearer
exists ~360 ms before stock's CC-SETUP and its onset is **MAC-initiated**,
while on our base **no traffic bearer ever appears at all**.

## 2026-10-24: the comparison that mattered was never run

Everything in this file about the A540 refusing FP-originated calls was
measured while our `{CC-SETUP}` reached the air **3 860 ms after it was
queued** (see `spec-conformance.md`). In that window the A540 opened a
service call of its own, so the refusal was EN 300 175-5 §9.5.2.3 collision
resolution, not a verdict on anything we sent.

That changes how the C4 row should be read. The C4 "accepts our 3-IE
CC-SETUP and rings" and the A540 "rejects" — but the C4 **does not open a
periodic CAT-iq service call** (the row two above says so), so it had nothing
to collide with. The two handsets were never being asked the same question.
The C4's tolerance and the A540's strictness may both be artefacts of that.

With the latency fixed the A540 still refuses, now cleanly: 0 ms queue-to-wire,
no collision, no call in progress, and nineteen variables swept with one
identical answer — message content including the exact EN 300 444 table 21
message, narrowband and wideband connections, and an audio channel assigned
before the setup.

**So the open question is no longer "why does the A540 differ from the C4".
It is "does our base ring anything at all, now that it can actually deliver a
setup on time".** The C4 result predates the fix and the C4 is local
equipment. The test that settles it is the **C430A** — a spare, never
registered to our base, never offered an incoming call, and the handset the
clean room's stock captures were taken with. `work/7520/lab/c430a.sh`.

Until that runs, no row in this file about FP-originated calls should be
treated as a property of a handset.

## Multi-handset: one slot each, and why that was broken `[2026-10-24]`

A third spare arrived (Gigaset **AS690H**), so the base now has to hold more
than one handset at a time. It could not.

**`dect-stubd` never set `mcfg.sub_slot`.** Every handset therefore took chip
subscription slot 1, and the slot decides two things at once:

| what | where | consequence of everyone being slot 1 |
| --- | --- | --- |
| chip UAK slot | `0x9518 + (slot−1) × 0x11` | a second handset **overwrites the first's key** |
| assigned TPUI | `dect_mm_tpui()` → `{0x00, 0x11, slot}` | every handset is given **TPUI `0x1101`** |

The second is the one that would have quietly ruined the whole multi-handset
comparison: with two handsets both believing they are `0x1101`, a page for
`0x1101` can be answered by either, and a per-handset result means nothing.
It would have looked like data.

The subscription store has **always** carried the slot — `sub_save()` writes
`r.slot` — and `sub_load()` printed it and threw it away. Both ends are now
connected, so a slot round-trips, and `-a subslot=N` overrides it.

| handset | slot | store |
| --- | --- | --- |
| Gigaset A540 CAT | 1 | `/etc/opendectcell-sub.a540` |
| Gigaset C430A | 2 | `/etc/opendectcell-sub.c430a` |
| Gigaset AS690H | 3 | `/etc/opendectcell-sub.as690h` |

The A540 needs nothing redone: its existing record was saved with slot 1 and
now restores into it.

**Registration is still sequential, and that is a real limit, not a
convenience.** `dect-stubd` holds one C-plane link, one MM state machine, one
UAK and one `-s` store, so two handsets cannot register inside a single run —
the second would overwrite the first in the store. `work/7520/lab/regall.sh`
gives each handset its own run and its own store; `both` only saves you
invoking it twice. Making registration genuinely concurrent means giving the
daemon a link per handset, which is `opendectcell#67` territory and is not needed
to answer the question in front of us.

**The question in front of us** is narrow: nineteen variables have been swept
against the A540 and the handset itself has never been varied.
`work/7520/lab/ringall.sh` rings each registered handset in turn and collects,
in the same capture, the connection record at `0x1AB9 + handle × 0x148`,
`0x965d` and `0x96ad` — so one run answers the handset question and feeds the
six-connection filter and the record diff at once.

- **all three refuse identically** → it is our base, and the stock capture is
  the only thing left worth doing;
- **one rings** → the FP-originated path works and the A540's refusal is a
  device trait, which is a different project entirely.

### The first three-handset run was void, and the script said "refused" `[2026-10-24]`

All three handsets were rung in turn with all of them powered on. None rang,
and the summary reported "refused" for each. **That verdict was wrong and the
run proves nothing.**

| handset | `0x32` received | replies on our TI | C-plane re-arms |
| --- | --- | --- | --- |
| A540 | **0** | **0** | 28 |
| C430A | **0** | **0** | 26 |
| AS690H | **0** | **0** | 30 |

Not one "insufficient resources" — which is what every previous refusal
looked like — and not one message of any kind on our transaction. The
handsets did not refuse. **They never answered at all.**

The A540's window shows why:

```
106435  hold start (setupdelay 1600 ms)
107999  connection up (handle 0), C-plane armed   <- link re-armed
108035  {CC-SETUP} queued and sent
110035  tx cplane 37 bytes                        <- LAPC retransmit: no ack
111815  connection up (handle 0)                  <- re-armed, retransmit abandoned
```

**Our base has one C-plane link.** Every connection from any handset calls
`reg_start()`, which re-initialises LAPC, MM and CC. With three handsets and a
FRITZ!Fon C4 in range there were 26–30 re-arms in 170 s — one every six
seconds — so a `{CC-SETUP}` was sent onto a link that was torn down underneath
it before it could be acknowledged. The right handset answered the page and
the setup carried the right identity; it simply never survived to be read.

`setupdelay=1600` made it worse by holding the setup for 1.6 s in exactly that
window. It is removed: the delay sweep had already shown it changes nothing
from 120 ms to 1 720 ms.

**So the handset question is still open.** The fix is procedural, not code:
**run one handset at a time with the others powered off.** The script now
counts re-arms and reports `VOID -- CONFOUNDED` rather than inventing a
refusal, and distinguishes a real `0x32` from silence.

This is the third time this week a confounded null has been presented as a
clean negative — see also the eighteen void theories and the `0x3D4`/`0xEF4`
regions. The pattern is the same every time: the instrument could not have
shown a positive, and nothing in the output said so.

### First clean A540 measurement `[2026-10-24]`

With every other handset powered off, and with the handle routing and
connection-up guard in place:

```
page responses:   1   (from the A540)
foreign traffic:  0
locates rejected: 0   (nobody else on air)
C-plane re-arms:  5   (normal for one handset)

ring: paged the handset, attempt 1 of 20
ring: CC-SETUP queued (page response received)
nwk out --> 13 05 ... e0 80 7c 04 90 02 00 84
nwk in  <-- 93 5a e2 32
```

**The A540 genuinely refuses.** Not the deadline fallback — the setup went
out on a real page response, to the handset that answered it, with nothing
else on the air. `re-arms=0` in the summary, and the verdict logic now
requires a page response before it will call a run a measurement at all.

This is the first result in the FP-originated investigation that can be
trusted. It does not change what we thought the answer was; it changes
whether we were entitled to think it. Every earlier `0x32` was measured
through an instrument that could not have shown a positive.

**The handset question is still open**, and is now one command each: the same
run against the C430A and the AS690H, each with the others off. If both also
refuse, it is our base and the stock capture is the only thing left worth
doing. If either rings, the FP-originated path works and the A540's refusal
is a device trait.

### Settled: it is our base `[2026-10-24]`

Three handsets, three models, three separate runs, each with every other
handset powered off:

| handset | page response | foreign traffic | rejected locates | setup | `0x32` |
| --- | --- | --- | --- | --- | --- |
| Gigaset A540 CAT | 1 | 0 | 0 | 1 | **1** |
| Gigaset C430A | 1 | 0 | 0 | 1 | **1** |
| Gigaset AS690H | 1 | 0 | 0 | 1 | **1** |

Every run had a real `{LCE-PAGE-RESPONSE}` rather than our deadline fallback,
no competing C-plane traffic, and a genuine `{CC-RELEASE-COM}` /
"insufficient resources".

**The handset hypothesis is dead.** The FP-originated path does not work
against anything we own, and no device profile, capability quirk or
per-handset adjustment is going to explain it. Every row in this file about
FP-originated calls describes our base, not a handset.

That also retires the question this file opened with. The C4 "rings" in the
old notes, but that result predates the drain fix and is not comparable: the
C4 opens no periodic service call, so it never had the collision the others
had. It has not been re-tested since, and it is local equipment.

**Next and only step: the stock capture.** See `docs/test-card-2026-10-25.md`.

## Solved: the A540 rings on transaction value 0 `[2026-10-24 night]`

The defect this file spent two days on was ours, as its opening paragraph
predicts. The daemon rotated the CC transaction value per call from
2026-10-20, so every incoming call a Gigaset was ever offered carried value 1
or higher. EN 300 444 §6.9.2 requires the lowest free value, 0. The C4 accepts
any value; the A540 refuses anything but 0 with `0x32`. Measured in one
session (`call-g7.md`, runs 14-16): value 0 rang, value 1 was refused twice,
once with stock's full SETUP content and once as a control.

So the recommendation at the top stands, now with its strongest example: no
device profile, because the A540 was conformant and we were not. Still to
measure on the default build: the C430A and the AS690H, after re-registering
them.
