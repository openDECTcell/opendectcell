# FP MAC bring-up on the STUB (M5 / gate G5 notes)

## ✅ GATE G5 PASSED (2026-10-18) — and the real bring-up is trivial

**Not enough on the 7362 SL's 07.18 STUB (2026-11-01, *observed on hardware*):** radio on and open registration alone gave no base a searching handset found; stock's three configuration mails first (`0x0153 00`, `0x0169 07`, `0x0157 00` from task `0x14`, as `dect-stubd -g` sends them) did ([bench-7362sl-g5.md](journals/bench-7362sl-g5.md)).

A second clean-room pass reverse-engineered the **STUB firmware itself** (the
SC14441 second-level images), not the host driver, and wrote it up as
`the clean-room record/answers/COMMAND_SET.md` [CR COMMAND_SET]. That document is
the authority on what each mail does, and it shows the earlier host-driver
reading (A-0006/A-0007/A-0008, "group A/B", the 55-byte `0x015C` cell config)
was **wrong about which mail does what**. The real bring-up is two 1-byte mails:

| Step | Mail (src:dst:prim:hex) | Effect [CR COMMAND_SET §6.1/§6.2] |
|---|---|---|
| contact | `0x22:0x02:0xF600` | link up; STUB replies `0xF601` (identity) + `0xF602` |
| **radio on** | `0x22:0x02:0x015B:01` | `0x015B` = RADIO_ON_OFF; byte 0 = 1 → **beacon on**. STUB picks the quietest carrier itself; `0xF623` then streams while on |
| **open reg** | `0x22:0x02:0x022B:41` | `0x022B` byte 0 = `'A'` → sets capability bit **a44 "access-rights-requests supported"**; STUB sends `0xF602` |
| (park) | `0x22:0x02:0x022C:01` then `0x015B:00` | close registration, radio off |

**Verified on the spare 7520 (2026-10-18):** after those two mails, `0xF623`
RADIO_STATUS began streaming (the firmware only sends it while the radio is on),
and a **FRITZ!Fon C4** in registration mode **listed our base as "Basis 3" and
attempted to register** (display: "Anmeldung/Registrierung an Basis 3
gescheitert"). That is the G5 criterion — *a handset sees the base* — met.

**Why the earlier attempts failed:** we sent `0x015B` with seven 0x00 bytes
(from the host-driver "group A" guess). The firmware reads `0x015B` byte 0 as the
on/off mode, so seven zeros = **radio OFF**. We were turning the radio off while
believing we were loading cell config. No `0x015C` cell-config mail is involved
in bring-up at all (in the firmware, `0x015C` = SEND_ACKED_DATA, a per-connection
data mail). The Freetz-NG live-capture plan (Q-0009) is therefore **not needed**.

**Next: G6 (registration).** During the C4's attempt the STUB reported
`0x0200` CONNECTION_ESTABLISHED → `0x0243`/`0x0242` → **four `0x0203`
SIGNALLING_SEGMENTs** (the handset's NWK ACCESS-RIGHTS-REQUEST, carried on the
Cs-channel) → `0x0201` RELEASED. The handset ran the whole procedure; our daemon
never answered the signalling, so it timed out. G6 = implement the DECT MM
access-rights + authentication exchange on the host: reassemble the inbound
`0x0203` segments, run the registration/auth procedure (DSAA, PIN-derived key),
send MM responses back via `0x0202`/`0x0244`/`0x015C`, and store the handset with
`0xF605` sub-op 2. Note: the C4 used the *connection-oriented* access path
(`0x0200`+`0x0203`), not the short-form access block (`0xF604`/`0x0E0D`).

Everything below is the earlier **host-driver** analysis, kept for history. Where
it conflicts with COMMAND_SET, COMMAND_SET wins.

---

Status: first written 2026-10-17, **analysed** from the vendor's host driver (FRITZ!OS 08.25, 7520), clean-room answers A-0005 and A-0006. Nothing here is verified on hardware yet. Clean-room notes describing behaviour, not code. Mail layout per `stub-pcm-control-channel.md` (primitive at mail offset 4, little-endian; host→STUB uses source task `0x22`, destination task `0x23`).

## Safety: what the MAC mails can and cannot do [CR A-0006]

- **They cannot change the box's persistent configuration.** AVM's EEPROM read and write routines act on a **host-side emulated EEPROM** (a RAM image saved to a file in the box's settings storage), entirely inside the vendor's host driver. Our daemon does not run it, and the STUB (uploaded into RAM every start) has no persistent store our mails could change. So no mail we send can brick or reconfigure the box.
- **The one real side effect is radio emission** once the MAC is switched on. The spare 7520 is a certified DECT device; emitting a DECT beacon is its normal function. DECT is designed for multiple fixed parts to coexist, so a test beacon should not disturb a nearby local base — but treat "radio on" as an outward-facing, on-air action and verify with a handset in search mode (gate G5).

## RFPI / identity [CR A-0006]

- The base RFPI/ARI is **EEPROM item 0x10 (5 bytes)**. Evidence: read by `a stock MM routine` and `the stock MM task`; the driver warns "[stock log message] (please do DECT config of board, first)" and logs an "RFPI_IND" line.
- Likely the **STUB reports its RFPI to the host** (an RFPI indication), and the host stores it — rather than the host sending the RFPI down. This fits the STUB taking its identity from the **factory data we already patch into the STUB image** (`docs/stub-image-format.md`, tag 1 = RFPI). So MAC start probably needs no RFPI parameter from us. *(assumed)*
- Item `0x415` (5 bytes) is a second identity-sized item used for a repeater "fixed-part lock" -- an MM-side lock on a repeated fixed part -- sent in the `0x0248` mail — probably a peer/repeater identity, not our own RFPI. **Never commit any of these identity bytes.**

## Candidate MAC bring-up sequence [CR A-0006] — **superseded by A-0036**

> **Correction (2026-10-19, A-0036).** The "Group A / Group B" below is the
> *address order of branches inside a message dispatcher*, not an execution
> trace: each branch runs in response to a different incoming message, so this
> is **not a boot sequence** and must not be replayed as one. The paragraph's
> own caveat ("not proven to be execution order") was right. Specific
> misreadings: the 55-byte `0x015C` is `SEND_ACKED_DATA` (a ULE handset-data
> envelope, `COMMAND_SET.md` §6.4), **not** cell/MAC configuration; `0x022C 01`
> *closes registration* and is not a radio switch; `0x0300`/`0x015E` are paging
> and queue management. The two-mail bring-up at the top of this file remains
> the verified one. The text is kept below for the record only.

`FpUI` (the host task that handles the STUB's contact answers) sends two contiguous groups of mails to task `0x23`. Call-site order, **not proven to be execution order**; parameter layouts beyond the listed constant bytes are unknown.

**Group A — configuration download (radio off), in order:**
`0x015B` (built from config items 0x3F1-0x3F4, 0x413, 0x414) → `0x0248` (`[2]=00 [3]=07 [4]=01` + the 5 bytes of item 0x415) → `0x0169` (param = config item 0x23) → `0x016A` → `0x022B` → **`0x022C` param 0** → `0x0250` → `0x0251`.

**Group B — activation (radio on), in order:**
`0x022B` (`[2]=0x41 [4]=0x10`) → timer 0x40 (3000 ms) → **`0x022C` param 1** → `0x015C` (55-byte mail, the largest; MAC/cell parameters) → `0x0300` (`[2]=0x0A`) → `0x015E` ×2 → `0x0300` (`[2]=0x0A`).

- **`0x022C` is the on/off switch** (param 0 in group A, param 1 in group B) — the best candidate for "MAC/radio on". `0x0169` looks like a prerequisite setting, not a switch. *(assumed)*
- Reading: group A loads configuration with the radio off; group B turns it on and sends the cell parameters (`0x015C`, 55 bytes). *(assumed)*
- Resolved raw-mail primitives: `0x0247`, `0x015B`, `0x0248`, `0x022B`, `0x015C`, `0x0300`, `0x015E`.

### Blocker for a faithful replay

Group A builds its mails from host config items (0x23, 0x3F1-0x3F4, 0x413, 0x414, 0x415) that live in AVM's emulated EEPROM defaults — **we do not have those values** (AVM config, clean-room). And `0x015C`'s 55-byte layout is unknown. So a faithful group-A/B replay is not yet possible. Open options: (a) ask the analysis side for the default item values and the `0x015C` layout; (b) test whether `0x022C(1)` alone starts the MAC on the STUB's own patched-in defaults (minimal, but emits); (c) derive the parameters empirically.

## Likely STUB answers [CR A-0006]

16-bit values `FpUI` treats as STUB→host primitives (besides the ones it sends): `0x0152/0x0154/0x0156/0x015A/0x015D/0x0165/0x017B/0x017C`, `0x0208/0x020A/0x020C/0x020E/0x024E/0x0252`, and `0xF601-0xF604/0xF606/0xF60F/0xF611/0xF613/0xF618/0xF623/0xF625/0xF627`. Request/answer pairs line up as `0x015B↔0x015A`, `0x015C↔0x015D`, `0x0250/0x0251↔0x0252`. So **`0x015D` or `0x0252`** are candidates for a "MAC configured/started" confirmation our daemon could wait for. *(assumed)*

## Registration (later, gate G6) [CR A-0006]

Registration is handled at the NWK layer **on the host** (`the stock MM task`, `a stock MM routine`, which reads RFPI item 0x10). The STUB delivers the handset's NWK messages (ACCESS-RIGHTS-REQUEST, …) as link-layer data to the host MM task; it does not answer them itself. The primitive carrying that NWK data is not yet established.

## Observed on hardware (2026-10-17)

- **`0x022C(1)` alone does nothing.** After contact, sending `0x022C` param 1 by itself produced no new mails versus the contact-only baseline (only the usual `0xF601`/`0xF602`/`0xFFF1`/`0xF618`). So the switch needs the group-A configuration first, as A-0006 expected — the MAC does not come up from the bare switch. (A brief radio-on was attempted; no emission could be confirmed, consistent with the MAC not starting.)
- Next faithful step therefore needs the group-A config values (0x23, 0x3F1-0x3F4, 0x413, 0x414, 0x415) and the `0x015C` layout — asked in Q-0007 (answered: stock defaults are mostly 0x00, item 0x23 = 0x1F).
- **Group A replay produced no observable answer** [CR A-0007 defaults]. Sending the suggested group-A order with stock defaults (`0x015B` seven 0x00 → `0x0248` `00 07 01 05` + five 0x00 → `0x0169` 0x1F → `0x016A` 00 → `0x022B` → `0x022C` 00 → `0x0250` 0000 → `0x0251` 000000) after contact gave **no new STUB mails** — only the usual `0xF601`/`0xF602`/`0xFFF1`/`0xF618` — whether sent to destination task **0x02** (the contact task) or **0x23** (the GenDSP task). So either the STUB accepts config mails silently (no ACK, so the expected `0x015A`/`0x0252` answers do not appear), or the guessed variable parameters are wrong, or some dynamic prerequisite is missing (candidate: the `0xFFF1`/`0x05` "keep-alive" that AVM's `pcm_Task` sends to task 0x23, per A-0005 — our daemon never sends it).
- **To make progress, the beacon must be verified with a handset** (the FRITZ!Fon C4 in registration/search mode = gate G5). Without that, "no answer" cannot be told apart from "silently working". The next on-air step therefore needs the user's handset. Open options: (a) try the full group A + `0x022C(1)` (+ `0x015C`) and check whether the C4 lists the base; (b) ask the analysis side whether the config mails are expected to be silent and whether the `0xFFF1`/`0x05` keep-alive is a prerequisite; (c) record stock reference traces with Freetz-NG (ROADMAP M1) to see the real bring-up order and answers.

## Gate G5 attempt (observed on hardware, 2026-10-17): no beacon

Ran `dect-stubd` with the full candidate bring-up as extra mails (`-M`, one per ~100 ms, all to dst `0x02`): group A with stock defaults → `0x022C(1)` → `0x015C` (53 zero parameter bytes) → `0x0300` (`0A` + zeros), then kept the link up for 10 min. Result:
- All 11 mails were sent and the control channel stayed healthy, but the STUB produced **no new mail** of any kind — only the usual `0xF601`/`0xF602`/`0xFFF1`/`0xF618`. No confirmation primitive (`0x015D`/`0x0252`) appeared.
- A **FRITZ!Fon C4** in registration/search mode reported **"Keine aktive Basis"** (no active base) after timeout.

So the guessed config does not start a detectable beacon. Most likely the `0x015C` cell parameters (sent as all zero; real layout unknown) are essential and must carry a valid carrier/capability/cell configuration, and/or a registration-enable step is missing. Guessing the 55-byte `0x015C` and the other variable fields is not tractable.

**Conclusion: the MAC bring-up needs the real mail sequence, captured from AVM's stock firmware.** The clean-room dataflow route is **exhausted** [CR A-0008]: the `0x015B`/`0x015C`/`0x0300` field sources need a analyser-style data-flow trace through `FpUI`, which the analysis side does not do (a prior attempt hit the safety filter; it is outside the clean-room rules). So static analysis cannot give us the cell-config contents.

### What A-0008 did establish

- **Registration is accepted on the host at the NWK/MM layer** — `MM_ACCESS_RIGHTS_REQ/ACC/REJ`, `a stock MM routine`, the subscription device-type and restriction settings. There is **no "registration-mode" primitive**; the STUB only has to beacon and pass NWK data through, and the host decides whether to accept a handset. So *our* stack must implement the MM access-rights answer (that is gate G6 territory), but for G5 (handset *sees* the base) the base just needs to beacon with the right advertised capability.
- **[public, ETSI EN 300 175-5]** a GAP handset searching for a base to register with looks for the broadcast higher-layer capability **"access rights requests supported"**; bases that do not advertise it are not offered. Where the AVM stack sets this bit (likely inside `0x015C`) is unknown — **find it by diffing a capture with vs. without the stock "register handset" action.**
- The test box's NAND backup of the factory DECT EEPROM could **not** be located (needs a TFFS3 parser; and it holds identity/subscription data anyway). Alternative: read the box's own `/var/flash/dect_eeprom` under stock firmware and compare only the non-identity group-A items (0x23, 0x3F1-0x3F4, 0x413, 0x414) against the defaults.

### Capture plan [CR A-0008]

1. Run **stock FRITZ!OS (or Freetz-NG) on a spare box** and log the control-slot byte stream (buffer position 1, both directions) from STUB upload until the base beacons. Freetz-NG exposes the vendor driver's mail logging and DECT debug read-outs; use RAM-boot (no flashing), as the M1 NAND backup was done.
2. Capture **once while triggering "register handset"** and once without; **diff** the two to locate the registration/access-rights-capability bit.
3. Map each host→STUB mail (`0x015B`, `0x0248`, `0x0169`, `0x016A`, `0x022B`, `0x022C`, `0x0250`, `0x0251`, `0x015C`, `0x0300`, `0x015E`) against the STUB→host answers that preceded it; byte-identical fields reveal their source directly (e.g. identity bytes the STUB reported earlier).
4. **Strip identity bytes** before recording anything in the repo.
5. Replay the reconstructed sequence with our `dect-stubd -M`, then retry the C4.

## Suggested first hardware step (needs a handset to verify)

After contact: either try `0x022C(1)` alone, or group A then `0x022C(1)`, and watch for a candidate confirmation (`0x015D`/`0x0252`) and for a **GAP handset in registration/search mode listing the base** (gate G5). Verifying G5 needs a handset (the FRITZ!Fon C4 is local equipment — coordinate with the user). Only then try `0x015C`/`0x0300` (unknown parameter layouts).

## Our `0x022B` is shorter than stock's, and it is the FP-capability mail (2026-10-22)

`[observed]` We send `0x022B` with **one** parameter byte:

```
tx M src 22 dst 02 prim 022b len 1 41
```

`[CR A-0006]`, recorded above, has stock sending it in group B as
`0x022B (​[2]=0x41 [4]=0x10)` -- **two meaningful bytes, not one**. And this
file already records what the mail does: byte 0 = `0x41` (`'A'`) sets FP
capability bit **a44, "access-rights-requests supported"**.

So `0x022B` is **the mail that sets the fixed-part capability bits the base
broadcasts in its beacon**, and we set exactly one of them.

**Why this now matters.** `[CR A-StockTrace-5 §18]` confirms stock maintains an
**extended** fixed-part capability set -- a capability block and a second one, with strings for "CATiq list access setup", "Wideband
speech defaults" and "Light Data Service Class 3/4". Those are the higher-layer
capabilities a CAT-iq base advertises. A base that advertises only a44 is
announcing a basic FP, and a CAT-iq handset deciding whether to open a traffic
bearer when paged is exactly the kind of decision that could key on them.

That is consistent with the measurement: `[CR §15/§16]` stock has a connection
bearer ~360 ms before its CC-SETUP, MAC-initiated with no DLC trigger, and we
**never raise one at all** -- while the same handset rings fine on stock on
this same box.

**The cheap test, and it is the next bench action.** Send `0x022B` with the
longer parameter list rather than one byte. `[not established]` the exact
layout: `[CR A-0006]` gives `[2]=0x41` and `[4]=0x10` in AVM's own mail
indexing, which we have not mapped to our `param[]` offsets, so the middle byte
is unknown. Try `41 00 10` first and watch for a bearer.

`[not established]` that this controls the bearer. It is a hypothesis supported
by stock having the structure, by our mail being demonstrably shorter, and by
the capability bit being exactly what a beacon advertises -- not by anything
that proves causation.

> **RESOLVED 2026-10-22, SAME DAY: the long `0x022B` is DEAD, twice over.**
> Answered from logs already on disk -- no bench run, no radio, no handset.
>
> **(1) The bits it was supposed to switch on are already set.** Decoding the
> QH=3 group against a bit we *know* we set is what makes this safe: `0x022B
> 41` sets a44, and a44 lands at `0x08` in the last octet, which fixes the
> numbering (MSB = lowest a-number) from a known value instead of a guess. QH
> then reads 3, as it must. Applying that convention to `38 49 94 ee`:
>
> | bit | meaning | ours | stock |
> |---|---|---|---|
> | a12 | extended FP info (QH=4 present) | 1 | 1 |
> | a17 | full slot | 1 | 1 |
> | a20 | C/O set-up on dummy allowed | 1 | 1 |
> | a23 | basic A-field set-up | 1 | 1 |
> | a24 | advanced A-field set-up | 1 | 1 |
> | a44 | access-rights-requests supported | 1 | 0 |
>
> **a44 is the only bit that differs from stock, and we set it deliberately.**
> The handset is already being told the base supports C/O set-up on the dummy
> bearer and basic/advanced A-field set-up. The premise was false.
>
> **(2) The mail cannot reach anything else anyway.** `tools/dect_qh7.py`
> extracts the broadcast block from any memdump log. Comparing a run with no
> `0x022B` (`cap_A`), the short form (`vF5`), and the long form (`cap_C`):
>
> ```
> no 0x022B    38 49 94 ee 00 | 40 11 10 20 04 | c8 01 85 11 34 | 73 07 00 00 00
> short '41'   38 49 94 ee 08 | 40 11 10 20 04 | c8 01 85 11 34 | 73 07 00 00 00
> long  '41 00 10'
>              38 49 94 ee 08 | 40 11 10 20 04 | c8 01 85 11 34 | 73 07 00 00 00
> ```
>
> Short and long are **byte-identical**. The extra parameter bytes change
> nothing; `0x022B` reaches exactly one bit.
>
> **Consequence for the QH=7 lead.** The only remaining difference between our
> beacon and stock's is the QH=7 manufacturer escape -- ours `73 07 00 00 00`,
> stock `70 53 41 47 48` `[CR A-StockTrace-5 §19]`. `0x022B` demonstrably
> cannot write it, so we have **no known mail that reaches QH=7**. Finding one
> (or establishing there is none) is a question for the Saturday stock capture,
> not for the bench.
>
> Superseded note, kept for the record: *launched once, never actually tested.* `work/box-tmp/cap_C.log` is the only run in the whole corpus
> that sends the long form (`prim 022b len 3 41 00 10`; every other run sends
> `len 1 41`). But that run was a **memory sweep with the radio off** -- it
> never sends `0x015B 01`, no handset ever attaches, and it contains zero page
> attempts, zero `0x0242`, zero releases. So the long `0x022B` has never been
> put in front of a handset. The hypothesis is **untested, not disproved**, and
> it is still the one that accounts for every measurement at once.
>
> Checked and rejected while writing this: the `0xF602` capability report does
> *not* show an effect from the long form. `cap_C` reports `ee 00 02 40 11` /
> `ee 08 02 40 11`, but so do a dozen runs that sent only the short form --
> the byte tracks the `ee 00` vs `ee 08` block offset, not our parameter.
> Printing the whole series killed a false positive that one line supported.
>
> **The run to do:** long `0x022B`, radio on, A540 attached, then an
> FP-originated call -- and watch for a traffic bearer. The parameter layout is
> still `[not established]`: `[CR A-0006]` gives `[2]=0x41` and `[4]=0x10` in
> AVM's mail indexing, unmapped to our `param[]` offsets, so sweep a few
> variants rather than trusting `41 00 10`.
>
> `[CR A-StockTrace-5 §18]` above is **retracted at source** (the room banner-ed
> it on our report): `0x015C` is `SEND_ACKED_DATA` per A-0036, not cell config.
> The capability blocks still exist in stock -- only
> the claim that `0x015C` carries them is void. If anything that strengthens
> this test: `0x022B` is the mail we *know* sets beacon capability bits.

**Still blocked:** `0x015C`, the 55-byte cell-configuration mail, is never sent
by us and its layout remains unknown. `[CR A-StockTrace-5 §18]` reports stock's
capability values are runtime/EEPROM-driven through `FpUI` and cannot be read
statically, so the analysis side cannot supply them either. If the `0x022B` test
fails, `0x015C` is the next thing and it needs a different approach entirely.

### `0xF602` carries the beacon broadcast content (2026-10-22)

`[observed]` The chip's `0xF602` answer is 23 octets and only **two** of them
ever vary across every run we hold:

```
ee | 00 02 | 40 11 10 20 04  c8 01 85 11 34  14 00 10 00 86  01 96 01 28 00
     ^^ ^^   |<------------- constant in every capture ------------------>|
```

| seen | when |
|---|---|
| `ee 00 02 ...` | after `0x015B 01`, before `0x022B` |
| `ee 08 01 ...` | after `0x022B 41` |
| `ee 00 01 ...` | later, steady state |

So byte 1 goes `00` -> `08` when we send `0x022B 41`, which is the mail our
notes already record as setting FP capability bit **a44**. `[assumed]` byte 1
is a capability or state field and `0x08` is what a44 looks like there.

**The constant block is four 5-octet groups -- the size of a DECT A-field
tail -- and two of them appear verbatim in the stock beacon** `[CR
A-StockTrace-5 §16]`:

| our `0xF602` block | in stock's captured beacon? |
|---|---|
| `40 11 10 20 04` | **yes** |
| `c8 01 85 11 34` | **yes** |
| `14 00 10 00 86` | not in their sample |
| `01 96 01 28 00` | not in their sample |

Their beacon lines take the form `23 00 10 18 80 10 <tag> <5 octets>`, and one
of the payloads is `de ad 00 00 b8` -- **our own RFPI**, confirming their
capture is from this same box.

Two exact five-octet matches are not coincidence, so `[assumed]` `0xF602`
reports the Qt content the chip broadcasts, in tail-sized groups.

**This weakens the thin-beacon theory rather than supporting it.** If our chip
broadcasts the same `40 11 10 20 04` and `c8 01 85 11 34` that stock does, our
beacon is not obviously missing those messages. `[not established]` whether the
sets are equal: their sample is "distinct A-fields" and may not be exhaustive,
we hold two groups they do not show, and they hold `0e 24 b8 18 aa` which we do
not. A complete comparison needs either their full beacon set or a way to read
ours over the air, which we do not have.

So the discriminating test is the capability mail itself, not the beacon
content -- see the run below.

### Located: where `0x022B` lands in chip memory (2026-10-22)

`[observed]` Three runs with the **radio off** -- no `-g`, no `0x015B`, nothing
transmitted -- each preceded by a fresh STUB load so the chip starts from the
same state, each sweeping 48 KB of chip memory (`0x0000-0x3fff` and
`0x10000-0x17fff`) with the `0xFFF1`/`0x07` read mail. Run **A** sent no
`0x022B`; run **B** sent `0x022B 41`, which is what we send today.

`tools/dect_memcmp.py` compares two runs and **excludes every address that
moves within a run**, which matters: 469 of 49 152 addresses are counters and
would otherwise drown the result.

**29 of 48 656 stable addresses differ, and three of them are unambiguous:**

| address | A | B | reading |
|---|---|---|---|
| `0x002459` | `00` | **`41`** | the `0x022B` parameter byte, stored verbatim |
| `0x002289` | `00` | **`08`** | the capability flag -- this is the `00` -> `08` seen in `0xF602` byte 1 |
| `0x002477` | `00` | `01` | `[assumed]` a "capability set" marker |

So **`0x002289` is where the fixed-part capability bits live** and
`0x002459` is where the mail's parameter is kept. The remaining 26 differences
are scattered singles in `0x003d..`, `0x0109..`, `0x0113..` and `0x0117..` and
are `[not established]` -- plausibly bring-up bookkeeping that differs because
one run did one more thing than the other.

**Why this is worth having.** It gives a direct, radio-free read of what the
base is advertising: set a capability, read `0x002289`, and see it. No handset,
no transmission, no air trace. Any future question of the form "does this mail
change what we advertise?" is now a two-run experiment of about two minutes.

The method is also reusable as it stands -- `dect_memcmp.py A.log B.log` for
any pair of runs differing in one mail.

### Tested: a longer `0x022B` does not change what the base advertises

`[observed]` Run **C** sent `0x022B 41 00 10` -- the three-byte form that
`[CR A-0006]` suggests stock uses -- against run **B**'s `0x022B 41`.

**The chip accepts and stores it.** Thirteen stable addresses differ, two of
them clearly:

| address | B (`41`) | C (`41 00 10`) | reading |
|---|---|---|---|
| `0x00245b` | `00` | **`10`** | the third parameter byte, stored |
| `0x002477` | `01` | **`03`** | the marker advances |

`0x002289`, the capability flag, is **unchanged** at `0x08` -- expected, since
`0x41` sets it in both.

**But the advertisement does not change.** The `0xF602` payload is
**byte-identical** between B and C:

```
A   ee 00 02 | 40 11 10 20 04 c8 01 85 11 34 14 00 10 00 86 01 96 01 28 00
B   ee 00 02   and   ee 08 02 | ... same 20 octets ...
C   ee 00 02   and   ee 08 02 | ... same 20 octets ...     <- identical to B
```

**So the "advertise more capabilities through a longer `0x022B`" idea is
dead.** The extra byte is accepted, stored, and has no effect on what the base
broadcasts.

`[not established]`, and worth keeping in view before anyone revives this: the
middle byte was a **guess** (`[CR A-0006]` gives `[2]=0x41` and `[4]=0x10` in
AVM's own mail indexing, which we never mapped to our `param[]` offsets), so a
different third byte or a different length might behave differently. And
`0xF602` may not report everything the beacon carries -- we know it carries
four tail-sized groups, not that it carries all of them.

**What this run does leave us**, and it outlasts the negative: a **radio-free,
handset-free, two-minute test** for "does this mail change what we advertise?"
Load the STUB, send the mail, sweep memory, compare with `dect_memcmp.py`, and
read `0xF602`. Every remaining candidate in the stock bring-up we skip --
`0x0169`, `0x016A`, `0x0248`, `0x0250`, `0x0251`, `0x022C 00`, and `0x015C`
itself -- can be tested this way without touching the radio or waiting for a
handset.

## ~~We never broadcast `QH=3`~~ **RETRACTED -- we do hold it** (2026-10-22)

`[CR A-StockTrace-5 §19]` supplied the **complete** set of stock's static
capability-type beacon tails (§16 had been a sample, not the whole set), and
decoding the `QH` nibble of each against EN 300 175-3 Table 7.3 gives:

| tail | QH | message | stock | us |
|---|---|---|---|---|
| `40 11 10 20 04` | 4 | extended fixed part capabilities | yes | yes |
| `c8 01 85 11 34` | C | extended FP capabilities part 2 | yes | yes |
| `14 00 10 00 86` | 1 | static system info | -- | yes |
| `01 96 01 28 00` | 0 | static system info | -- | yes |
| **`38 49 94 ee 00`** | **3** | **fixed part capabilities** | **yes** | **NO** |
| `70 53 41 47 48` | 7 | escape | yes | no |
| `73 07 00 00 00` | 7 | escape | yes | no |

**`QH=3` is marked MAN = Yes in Table 7.3 -- mandatory transmission.** We do
not broadcast it, and we *do* broadcast `QH=4`, which Note 2 says shall be sent
in the multiframe following every transmission of the fixed part capabilities
information. So we send the extension without the thing it extends.

**What stock declares in it**, decoded against Table 7.14 (`38 49 94 ee 00`,
`a8..a11` = `0011` = QH 3, then `a12..a31` physical/MAC, `a32..a47` higher
layer):

| bit | capability |
|---|---|
| `a12` | **extended FP Info (QH = 4)** -- the bit that announces the message we send unannounced |
| `a17` | full slot |
| `a20` | **C/O setup on dummy allowed** |
| `a23` | basic A-field set-up |
| `a24` | advanced A-field set-up |

**Why this is the best candidate we have had.** A paged PT decides for itself
whether to open a traffic bearer, and `[CR §16]` established that onset is
**MAC-initiated** with no DLC frame triggering it. A PT that has never been
told the FP supports **basic/advanced A-field set-up** or **connection-oriented
set-up on the dummy bearer** has no grounds to try. That accounts for every
measurement at once:

- no traffic bearer ever appears on our FP-originated calls, while stock raises
  one ~360 ms before its SETUP;
- the same handset rings on stock on this same box;
- six hypotheses about the **SETUP's contents** all failed, because the SETUP
  was never the problem;
- `<<CODEC-LIST>>`, `<<SIGNAL>>`, the transaction identifier and the basic
  service are all downstream of a bearer that was never going to exist.

`[not established]`, and it matters: **`0xF602` is the chip's capability
report, not an air capture.** It holds four groups; stock broadcasts five
static capability tails plus static system info. It is possible the chip
transmits `QH=3` and simply does not report it in `0xF602`. We cannot capture
our own air, so this cannot be confirmed from our side. What is established is
that the two sets are not equal and differ **in both directions** -- we hold
two groups stock never broadcasts, stock holds three we do not.

**Next, and it is a chip-side question:** which mail makes the chip broadcast
`QH=3`? `0x015C`, the 55-byte cell-configuration mail we never send, is the
obvious candidate and its layout is unknown. The radio-free memory-sweep method
established above can test any candidate in about two minutes -- send the mail,
sweep, compare, and watch whether a fifth group appears in `0xF602`.


### RETRACTION: the chip does hold `QH=3`, and its content matches stock

`[observed]` The section above concluded, from `0xF602` not reporting a `QH=3`
group, that we never broadcast the mandatory fixed part capabilities. **That is
wrong.** Searching the swept chip memory for stock's tail finds the whole
capability block, twice, contiguous and in order:

```
config copy  0x00028f   38 49 94 ee 00 | 40 11 10 20 04 | c8 01 85 11 34 | 73 07 00 00 00
live copy    0x002285   38 49 94 ee 08 | 40 11 10 20 04 | c8 01 85 11 34 | 73 07 00 00 00
                                    ^^ the a44 bit 0x022B 41 sets
                        QH=3           QH=4             QH=C             QH=7 escape
```

`[CR A-StockTrace-5 §20]` had said the three capability messages are one
contiguous 15-byte block in stock; our chip holds the same block **plus** an
escape tail, and `QH=3`'s content is **byte-identical to stock's**
(`38 49 94 ee 00`) apart from that a44 bit, which we set deliberately to open
registration.

This also explains `0x002289` exactly: it is the **last octet of the `QH=3`
group**, which is why `0x022B 41` flips it `00` -> `08` and why that shows in
`0xF602` byte 1. The localisation was right; the interpretation of its absence
was not.

**So `0xF602` is not a complete beacon report** -- it starts mid-block, at the
`ee` of `QH=3`, and its last two groups (`14 00 10 00 86`, `01 96 01 28 00`)
are the static system info from somewhere else entirely. Reasoning from what it
does *not* contain was the error, and `[CR §19]` warned of exactly this:
"`0xF602` is your chip's capability report, not a direct OTA beacon capture".
The warning was recorded and then not applied.

**What actually differs, after this correction:** of stock's static capability
tails, we hold `38 49 94 ee 00`, `40 11 10 20 04`, `c8 01 85 11 34` and
`73 07 00 00 00`. The one we do **not** hold anywhere in 48 KB of swept memory
is the other escape, **`70 53 41 47 48`** (`QH=7`, manufacturer-specific). That
is now the only known difference in the static beacon set, and it is a
proprietary escape rather than a standard capability -- so it is a much weaker
candidate for gating bearer establishment than `QH=3` would have been.

`[not established]` whether the chip broadcasts everything it holds, and
whether that escape matters at all.

## Survey: what the bring-up mails we skip actually configure (2026-10-22)

`[observed]` One mail per run, **radio off**, fresh STUB each time, 48 KB of
chip memory swept and compared against a baseline that sent no extra mail.

**First, the noise floor.** 18 addresses differ from the baseline in **every**
run regardless of which mail was sent -- `0x00e2`, `0x00e4-5`, `0x02c6`,
`0x3deb`, and a scatter in `0x109xx`-`0x116xx`. They are run-to-run variation,
not mail effects. **Any future comparison must exclude them**, and the earlier
"A vs B: 29 differ" figure in this file was inflated for want of them: the real
signal was 11.

| mail | addresses unique to it | reading |
|---|---|---|
| `0x022B 41` | `0x002289` -> `08`, `0x002459` -> `41`, `0x002477` -> `01` | the capability block; **confirmed real**, unique to the `0x022B` runs |
| `0x022B 41 00 10` | adds `0x00245b` -> `10`, `0x002477` -> `03` | stores the extra byte, as recorded above |
| `0x016A 00` | `0x0002d8`, `0x003fe6` -> `01`, `0x010e19`, `0x010e35` | touches `0x010e..`, which nothing else does |
| `0x022C 00` | `0x003e00`-`0x003e09`, `0x011334` | |
| `0x0169 1f` | `0x003e00`-`0x003e0f` with different values, `0x011332/4` | |
| `0x0248 ...` | `0x003e00`-`0x003e09`, **`0x010d00/01`** (`aa aa` -> `00 ff`) | touches `0x010d..`, which nothing else does |

**All four are accepted and all four change chip state**, so none is a no-op --
worth knowing, since a previous attempt to replay group A concluded it produced
"no observable answer" from the mail log alone. They do take effect; the
evidence is in memory, not in answering mails.

**But nothing here looks like bearer or MAC configuration.** `0x022C 00` and
`0x0248` write **identical** values to `0x003e00`-`0x003e09`, and `0x0169`
writes different values to the same bytes, which reads as a shared scratch or
record area rather than per-mail configuration -- and `0x003df4` and its
neighbours are already flagged in `docs/journals/call-g7.md` as connection-record regions to
leave alone. So `[not established]` that any of this bears on the missing
traffic bearer, and **no candidate emerged.**

**What the survey is worth anyway:** the noise floor is now measured, so any
future "does this mail change X?" question is a clean two-minute experiment;
and three of these mails have had their effects located for the first time.
