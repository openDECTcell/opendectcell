# More than six handsets on stand-by in one base

Status: design and test plan, 2026-10-31. Nothing here is built. Written
from our own notes (`capacity.md`, `multicell-experiments.md`,
`stub-api.md`), the clean-room answers A-0025, A-0041 and A-0131, and the
maintainer's question of the same day. The chip firmware stays unchanged.

## 1. The question and the short answer

One chip holds six subscriptions (slots 1..6: `0xF605` and the `0x020A`
key records) [CR SC14441_Firmware_Review §5, A-0085]. Can one base keep
more than six handsets locked and reachable?

**Probably yes, if a chip slot is a cache and not the register.** The host
keeps every registration; a chip slot holds the handsets that need the chip
right now. Two measured facts make this plausible, one unmeasured fact
decides it:

| Fact | Source |
|---|---|
| The chip admits a handset it holds no key for; our host's identity gates decide who may open a link. Measured on 07.56: A540 and AS690H located and connected with the chip's table empty | `multicell-experiments.md` M1, 2026-10-25 |
| A keyless handset is paged and rings (it answers the page with a location registration first) | M2', same day |
| **Measured 2026-10-31 (08.25, warm chip):** a changed key written with `0x020A` reads back correctly 20 ms later (36 of 36 from 20 to 800 ms); a read in the same instant gets no answer. A slot clear (`0x0247` op 0) leaves the key bytes in the table | §4b |
| **Unmeasured:** whether a call with ciphering works when the key is written just before it; whether a cleared slot still admits its handset | T-C, T-D, Q-0073 |
| **Open contradiction:** on 08.25 clearing the other slots stopped a foreign handset dead (`vF427`, 2026-10-23); on 07.56 it did not. Every claim here is 07.56 until T-B on 08.25 | M1 |

A stand-by handset needs nothing from the chip: it is locked to the RFPI
and listens for pages. The chip needs its key only for ciphering (and
possibly for what it does with the slot's TPUI).

## 2. Design

Three layers; none touches the chip firmware.

1. **The host store has no slot limit.** Today `DECT_MM_MAX_SLOT = 6`
   (`dect_mm.h`) is both the number of records the host holds and the chip's
   slot index, and `DECT_BASE_HS_MAX` follows it. The split: a *handset
   record* (identity, key, TPUI, capabilities) is host data, any number; a
   *chip slot* 1..6 is a cache entry that points at a record. An API change
   in `dect_base`, not a protocol change.
2. **The TPUI is the record's, not the slot's.** Today it is derived as
   `{0x00, 0x11, slot}` unless `dect_mm_cfg.tpui` is set, and every caller
   that builds an MM leaves it zero (`dect_base.c`, about ten sites). With
   more handsets than slots, slot-derived TPUIs collide; the TPUI must come
   from a pool, as the controller design already says (`cell-protocol.md`
   §7.4, openDECTcell#65 and #76). The library supports it (`dect_mm_hold()` takes
   a TPUI, `dect_mm_tpui()` uses it verbatim); the base does not pass one.
   AVM's host does the same: it keeps TPUI pools and assigns the TPUI
   itself, the chip does not derive it from the IPUI [CR A-0131].
3. **The slot cache.** Before the chip must use a record (an incoming call
   we page, an outgoing call's first message, ciphering), the host makes
   sure a slot holds it, evicting an idle one. Eviction rules are the
   controller's (`cell-protocol.md` §7.4): never a pinned slot or one with a
   live connection, 5 minutes of hysteresis, then least recently seen. An
   incoming call is the easy case, because we page: load, wait until the
   write is effective, page. An outgoing call is the hard one: the first
   message is a location registration or a CC-SETUP from an IPUI the chip
   holds no slot for; the host holds the message while it loads the key
   (MM already holds a `{LOCATE-REQUEST}` this way; the handset's CC timer
   is 20 s).

Rule: **a call whose key could not be loaded is refused**, never made
unciphered.

Not changed by this: concurrent calls stay at about three or four voice
channels per chip (`DECT=4` in the host's `PCM-link` [CR A-0013 F6]). More
stand-by handsets do not buy more simultaneous calls; that is what more
cells are for.

## 3. Open questions, and who answers them

| Question | Route |
|---|---|
| Does anything confirm that a `0x020A` / `0x0247` write has taken effect? | room, Q-0072 |
| May a slot be rewritten while another slot has a live connection, and does a slot carry state beyond the 17-byte record? | room, Q-0072 |
| How long until a written key is effective (our 7412 audit waits 1 s) | bench, T-A |
| Does a non-slot TPUI work end to end (locate accept, page, link signature)? | bench, T-C |
| Does a keyless handset still work on 08.25? | bench, T-B (on the lab 7520 with the 08.25 blob) |
| What does ciphering need, and when does it fetch the key? | room (answered in part by A-0041), bench T-D |

## 4. Tests, outcomes written before the run

Every test needs the radio on; each is bounded and the box is parked or
restored afterwards. Tests that need a person at a handset (answer a call,
dial out) are marked **[person]** and wait for a bench day.

| Test | What | Pass | Fail, and what it means |
|---|---|---|---|
| **T-A** key-write delay, 08.25 (lab 7520, `second=dectfw_secondlevel_441.hex`) | start the daemon with stores installed, read the UAK table back at 0, 50, 100, 200, 400, 800, 1000 ms after the writes, ten starts | a delay below which the read-back shows the old content and above which the new, stable over ten starts | no stable step: the write is not a simple delay; ask the room for the completion rule |
| **T-B** M1 again, 08.25: handsets with no chip key | `-a nochipkeys`, handsets alive and in range (switch-on needs **[person]** unless already on) | a locate or a link attempt from a handset the chip holds no key for | no contact at all with an empty table: world B on 08.25, so records must be in the chip before the handset arrives, and the cache needs `0xF604`; the design in §2 falls back to "six per cell" |
| **T-C** non-slot TPUI | the base sets the TPUI of one record to a value that is not `0x11xx` of its slot; the handset locates (after a daemon restart or a `locvalid` re-locate), acks the assignment, is paged by it and rings | `TEMPORARY-IDENTITY-ASSIGN-ACK`, the page answered, ALERTING | assign rejected, or a page unanswered: the chip or the link signature relies on the slot-derived value; the pool design needs another route |
| **T-D** key loaded just before a call | evict a handset's slot, then ring it; load on the page | ringing and (answered **[person]**) a ciphered call | the call ends at ciphering: the key was not effective yet, widen the wait from T-A |
| **T-E** eviction under a live call **[person]** | one handset in a call; another's slot rewritten | the call is undisturbed | a drop: never rewrite while any connection is live, not only the target's |

## 4a. Knobs built for the tests (2026-10-31)

Both are `-a` options of `dect-stubd` (LuCI: Network > DECT, Extra options;
uci `opendectcell.main.options`), off by default, no behaviour change when unset:

| Option | What it does | Test |
|---|---|---|
| `keycheckms=MS` | how long the UAK read-back and the slot audit wait after the key writes (default 0; 1000 on the 7412's 06.88). The verdict lines carry no key bytes | T-A |
| `tpuioffset=N` | the TPUI assigned in LOCATE-ACCEPT and used for pages is `{0x00, 0x11, slot + N}` instead of `{0x00, 0x11, slot}`; `dect_base_cfg.tpui_offset`, applied at every site that builds an MM from a slot (`tpui_cfg_for_slot()` in `dect_base.c`) | T-C |

With `tpuioffset=0x40` slot 1's handset gets TPUI `0x1141`. A handset that
already holds `0x1101` takes the new one when it next locates (a daemon
restart does not make it; a power-cycle of the handset, or `locvalid=1`,
does). Revert by removing the option and locating once more. Unit test:
`tests/test_base.c` (`dect_base_tpui`). Not an API bump: a test knob in the
config, the TPUI per record for the controller's pool is the later change.

## 4b. Results, 2026-10-31 (lab 7520, 08.25 chip, unattended)

| Test | Result |
|---|---|
| **T-A** key-write delay | Two different handsets' keys swapped through slot 2 (`subslot=2`, stores `as*` and `c*` alternately, chip warm), read back after `keycheckms=` N, 3 repeats each way: **N = 0: no answer, 6 of 6. N = 20, 50, 100, 200, 400, 800: MATCHES, 36 of 36.** So a key write is effective within about 20 ms; the host can wait 50 ms and not the 1000 ms the 7412's 06.88 needs. Raw: bench-later U8f-1d. *Earlier sweeps of the same day were void:* they rewrote identical content (the read-back matches whatever the delay) or the init rejected the store glob |
| **T-A on the 7412 (06.88)** | The same swap through slot 2 (stores `1*` and `2*`, delays 0 to 1500 ms, two repeats each way, no void): **0 ms: no answer, 4 of 4. 20, 50, 100, 200, 1000, 1500 ms: MATCHES, 24 of 24. 400 and 800 ms: MATCHES in 6 of 8, no answer in 2 of 8.** Never a MISMATCH, i.e. never the old key read back. A key write is effective within 20 ms on 06.88 too; the 1000 ms wait in `dect-stubd` is not needed for the write to land, but a read-back can occasionally get no answer (2 of 36 here), so the host should ask again once after a missing answer. Raw: bench-later U8f-1d |
| **T-B on 08.25, 2026-10-31 (lab 7520, maintainer at the handsets)** | **Retracted, see the correction below: first read as world B, but the runs had an empty key table on a cold chip.** Fresh chip each time (service stopped, `stub.second` removed, started), the three handsets power-cycled after the radio was up, 3-4 minutes watched. **Run 1** `nochipkeys` (no keys, mask `00 00`): no `connection up` from any handset. That run was confounded (the mask was zero as well). **Run 2** `nochipkeys,slotmask=0x07` (no keys, mask `00 07`): no `connection up` in 4 minutes, the handsets show "not registered". **Control after each**, keys written, handsets untouched: all three connected within 30 s (`MATCHES`, audit `1=OURS 2=ours 3=ours`). So the key, not the mask, gates admission on 08.25; `vF427` (2026-10-23) was right, and M1's world A holds for 07.56 only. Consequence: on 08.25 the key must be in the chip **before** the handset arrives; locate-time loading is not possible. A page-driven load (incoming call) is untested on 08.25. 06.88 (the 7412, the controller's cell) still open |
| **T-B on 06.88, 2026-10-31 (the 7412, C430A and AS690H)** | **World A: the chip admits a handset it holds no key for.** Control first: with keys, both handsets connected. Then a fresh chip (service stopped, `stub.second` removed, started) with `nochipkeys,slotmask=0x07`, audit `1=- 2=- 3=- 4=- 5=- 6=-` (no key in any slot). The C430A opened a link 4.9 s after the chip came up (15:18:09), served from the host's store for its IPUI. After a power-cycle of both handsets, both located again: C430A 15:20:26, AS690H 15:20:31, the AS690H's location registration accepted (`sent the date and time`), no key in the chip. So **on 06.88 the host's identity gates decide, as M1 found on 07.56**; the controller's 7412 cells can load a roamer's key after its first locate. **08.25 differs** (world B, row above). Not tested: a ciphered call with no key in the chip (T-D), a page to a handset with no key |
| **T-C on 06.88, 2026-10-31 (the 7412, AS690H, `tpuioffset=0x40`)** | **Pass: a TPUI that is not the slot's own value works end to end, in the form `{00, 11, N}`.** With `-v`: LOCATE-ACCEPT carried `05 05 a0 94 00 11 43` (TPUI `0x1143`, slot 3 + 0x40), the handset answered with TEMPORARY-IDENTITY-ASSIGN-ACK (`85 41`), the first page `0c 11 43` was answered at once (`page response received`), the CC-SETUP rang it, authentication and ciphering ran (`0x0209`, link ciphered). Control, same handset and flow, offset off: page to `0c 11 03` answered at the first attempt. **An earlier offset run looked like a fail and was not:** 17 pages to `0x1143` unanswered, the ring came only 60 s later on the handset's own link. Its first link was not a location registration (no date and time sent), so the handset still held `0x1103` from the run before; **a handset takes a new TPUI only at its next location registration.** Two runs before that were void for other reasons: the 7412's daemon predated the option (`-a tpuioffset: unknown key, ignored`), and the C430A's re-registration over an existing record had not been saved (see below). Consequence for the controller's pool: assign the TPUI in LOCATE-ACCEPT and keep it stable per handset; never change it without the handset locating (a page by a TPUI the handset does not hold is silent, "TPUI never acknowledged" is the warning). Tested: one handset, the `{00,11,N}` form with N up to 0x43 only; not a 16-bit TPUI with another high byte |
| **Found on the way: re-registering a handset the base already holds** | The C430A was re-registered on the 7412 without a deregistration first (15:16:43): the log says `handset registered` but no `persisted`, `identity gate armed` or `subscription saved` follows, and `/etc/opendectcell-sub.2` kept its 2026-10-29 time stamp. The handset held a new key, the base the old one: later location registrations were not accepted and a call failed authentication (`WRONG RES`). A fresh registration into an empty store (the AS690H, the three handsets on the 7520) saved fine. Not yet analysed in the library |
| **T-B and T-C repeated with the C430A on the 7412 (06.88), 2026-10-31** | The C430A was deregistered and registered afresh (an empty record: `persisted`, `subscription saved`, read-back `MATCHES`, slot 2), which supports reading the earlier failure as a re-registration over an existing record. **T-B: pass.** Fresh chip, no key in any slot (audit all empty), mask `00 02`: the C430A connected (15:57:59) and its location registration was accepted (`sent the date and time`). **T-C: pass.** `tpuioffset=0x40`, after a power-cycle that made the C430A locate (a handset that merely relocks keeps its old TPUI): page `0c 11 42` answered at the first attempt (`page response received`), CC-SETUP, authentication, ciphered link. So T-B and T-C now hold on two handsets (AS690H, C430A) on 06.88 |
| **Correction, 2026-10-31 evening: the 08.25 "world B" above is NOT established** | Three more runs on the lab 7520 changed the reading. **07.56:** a fresh chip with no key in any slot hid the base from the handsets (A540, C430A: "no signal", mask pinned `05` and computed `00`), but with **one** other handset's key in the table (the C430A's) the **A540 connected keyless** (16:23:38, location registration accepted, then a list session) with its key held back until 16:27:21 (`holdback=240000`): **world A on 07.56 when the table is not empty**, and an **entirely empty table silences the base** on 07.56. **08.25:** the same design (C430A's key in, A540's held back 300 s) gave no connection from either handset, **including the keyed C430A**, and none for 70 s after all keys were installed (16:29:18); a plain restart with all keys then connected both within seconds. So that run is void: every 08.25 keyless run was a first run on a freshly uploaded chip, where T-A found that a read-back returns nothing (key writes may not be effective yet), and every one had an empty table. The 08.25 T-B (run 1 and run 2 above) therefore showed "an empty table on a cold chip is silent", not "the chip needs the handset's key". **Open on 08.25:** warm chip (a run after the upload), one other key present, the test handset's key never written in that chip lifetime. **06.88 is unaffected** (keyless, empty table, handsets located). Handset side: an AS690H holds one base only (registering on the 7412 dropped the 7520); a handset that failed to find the base needs a power-cycle, and it can take minutes to retry |
| **The chip room's answer, A-0133 (2026-10-31, 08.25 STUB, a static reading; 07.56 and 06.88 not swept)** | (1) **The chip holds a 5-byte identity per slot** (`0xF605` sub-op 2, six entries) and its link events are compared against them; a miss is not a refusal by itself (handsets connect on 06.88 and 07.56 with the block at its default), which corrects the room's A-0131 §3/§5. (2) **The six is baked into about nine arrays and masks** (key table, identity block, five one-byte masks, the `0x0247` install mask, the per-link records, loop bounds of 6 and 7): several masks are one byte wide, so **eight is a hard ceiling**, and the key table and identity block sit back to back and would have to be relocated into free RAM nobody has mapped. 06.88 is a different binary and a second port. The room's net: the host-side slot cache is the better route, a firmware change a last resort. (3) **`0x017C` has exactly one reader** (a handset-search loop) and **does not gate the radio on 08.25**: a zero mask is no beacon gate there. (4) A `0x020A` write is a synchronous copy with no hold-off in the chip; a lost first-run write happens before the handler runs, the mail is not accepted while the chip is still starting. Cheapest test: send a harmless read until it answers, then write keys. (5) A shortcut treats TPUIs `11 nn` with nn 1 to 6 as slot nn; our T-C values (slot + 0x40) avoid it. **Not answered:** why an empty table silences the base on 07.56, and the exact 08.25 admission refusal; the room offers a 07.56 pass and suggests reading three mask and flag bytes (`0x95E4`, `0x95E5`, `0x2451` on 08.25) on a silent versus a connecting chip |
| **The chip room's answer, A-0134 (2026-10-31): why an empty table silences 07.56** | **The eco no-emission state machine, not the key table.** On 07.56 (state record at `0xDD5`; 06.88 `0x772`, 08.25 differs) a periodic routine counts only while eco is enabled (`0x0153` bit 7), the **`0x0247` install mask is zero** (`0xDD9`; 06.88 `0x778`) and four further conditions hold (undecoded); at 501 passes it moves towards no-emission (the `0xF623` byte 13 bit 6). **Nothing in any build looks at the key table for emptiness**; `0x017C` is irrelevant (matches our pinned `05` and computed `00` runs). Why a key mattered: in our flow the key's slot is also installed with `0x0247`. **What our daemon sends in the keyless runs (checked in the code, desk):** no `0x0247` op 1 at all (a held-back handset is neither installed nor written, `held_back()`), only op 0 clears, so the install mask is zero, as the prediction needs; **but** the bring-up sends `0x0153` with parameter `00` (bit 7 clear), which the room reads as eco disabled, so the machine should not have counted: **a contradiction to resolve on the bench**, by reading the state byte, the eco bit and the install mask on a silent 07.56 chip (`memdump=0xDD0:0x10`, with `-p`, keyless run only so no key mail is printed). 08.25 is gated differently (flag `0x2451`, no read of the install mask). **Predicted fix, to try:** install one slot with `0x0247` (op 1, flag 0) without a key (`-M 0x22:0x02:0x0247:010100`), then the silent run should stay on air. Not decoded: conditions 3 to 5, the real delay. **WITHDRAWN by the room the same evening (A-0134 correction):** on 07.56 `0x0153` bit 7 set sets the eco enable bit and clear clears it (the handler is the only writer), boot value of `0xDD8` is `0x80` (capable, not enabled); with our `0x0153 00` the enable bit stays 0 and the eco machine cannot count, so **it does not explain the silent 07.56 runs; the cause is open.** Checked in our source: the only `0x0153` ever sent is the bring-up's static `00` (no option, no init path sends bit 7 set). The bench plan stays and now discriminates: if `0xDD8` reads `0x80` and `0xDD5` reads `00` on the silent chip, eco is excluded by measurement; add `0xF623` byte 13 bit 6 and `0xF602` byte 2 from the chip's periodic mails (run with `-p`): if the chip says it is emitting, the problem is on the handset side of the air, not a chip gate |
| **Test A on 07.56, 2026-10-31 17:00-17:09 (lab 7520, A540 and C430A watching, each run on a freshly loaded chip, controls in between)** | Four runs, all keyless unless stated, every one judged by the handsets' own display ("no base") and the log: **(1)** no key, no install, mask `00 00`: the chip's periodic status mail `0xF623` had byte 13 = `00` in 21 of 21 samples (bit 6, no-emission, clear: **the chip reports that it emits**), the `0xFFF1` sub 07 memory reads for `0xDD0` got **no reply at all** (27 requests; reads of RAM look unsupported on 07.56, so the eco state byte could not be read), no contact, handsets: no base. **(2)** `installdummy=1` (new test option: `0x0247` {1, 1, 0} after the clears, no `0x020A`): handsets: no base. **(3)** `installdummy=1,slotmask=0x01` (install and mask, still no key write): handsets: no base, also after their reboot. **(4) Control**, keys written (slots 1 to 3, mask `00 07`): both handsets showed a base at once, also at 16:11, 16:18 and 16:23. So on 07.56 **only a real `0x020A` key write brings the base back**; `0x0247` install, the `0x017C` mask and eco are each excluded (eco by the room's polarity answer, install and mask by (2) and (3)), and the chip says it emits while the handsets see no base. That contradicts "nothing reads the key table" at least as an observation: either a non-zero key table matters after all, or `0x020A` has a side effect beyond the copy (a flag, the config byte). Sent to the room as a question |
| **The chip room's A-0135 (2026-10-31 evening): no static mechanism found for "only a key write brings the base back" on 07.56** | The `0x020A` handler on 07.56 is a 16-byte copy plus mail byte 19 into the slot's flag byte; a slot byte of 0 or 7 and up is ignored (no overwrite risk on 07.56; **not so on 08.25**: 7 and up writes into the neighbouring block, 0 is a commit sub-op); no present mask, no state change, no reply. Nothing links the beacon to the table (one reader, the key copy at link setup). `0xFFF1` sub 7 **is** supported on 07.56 (destination task 0x23, reply 0x23 to 0x22, at least 32 bytes) and checks neither table, masks nor eco: the 27 unanswered reads mean the mails were not processed or the reply was lost, the shape of the 08.25 first-run-after-upload case. Common factor of our keyless runs 1 to 3: no read answered, no `0xF602`, no base; so the cause is more likely which mails arrive and when, or warm-up after the upload, than the table content. **Our check of the logs the same evening:** `0xF602` arrived 4 times in the keyed control run (run 4) and 0 times in run 3 (26 status mails `0xF623`, none `0xF602`). **Discriminators for the next bench day, 07.56 only:** (a) keyless fresh chip, a normal-length `0x020A` with slot byte 7 (ignored by the handler): if the base appears, the arrival of that mail is the trigger; (b) `0x020A` into slot 1 with 16 zero bytes and flag 0, then install and mask: if the base appears, any write works; (c) upload, wait several minutes, then the keyless sequence and the read: if it works, it is warm-up. Never (a) on 08.25 | **Addendum (room, same evening): on 07.56 the `0xF600` contact handler sends `0xF601` and then unconditionally `0xF602`; run 3 had 26 `0xF623` and no `0xF602`: either the contact was not processed or the longer mail was lost on the way to the host. Our rx histograms: run 3 (silent) received 26 each of `0xFFF1` (25 bytes), `0xF623` (14) and `0xF618` (2) and no air events; run 4 (keyed) also `0xF602` (23 bytes, 4 times), `0x024E` (25) and the air events (`0x0243`, `0x0205`, `0x0203`...). So mails of 25 bytes do arrive in the silent run, and `0xF602` is 23 bytes: a plain length limit is not the story; whether `0xF601` arrived is not visible in the rx log (the session layer consumes it) and needs the daemon's stop summary.** | **Second addendum (room): the `0xF602` absence says nothing (it follows the contact and appears at run time only on events: a presence change, a radio edge, a changed capability; run 3 had no air events). Control sent at once, 17:14, keyed run, same read (`0xFFF1` sub 7, `0xDD0`, 16 bytes, every 3 s, verbose log without parameters so no key mail is printed): 15 requests, 16 replies of 23 bytes (7 header plus the 16 requested), against 0 replies to 27 requests in the silent keyless run. So the memory read works on 07.56 as sent, and its silence in the keyless runs is a second symptom that goes with the missing key write. Still open: why, in the keyless run, the chip emits `0xF623` but nothing links and the reads get no reply.** | **Third addendum (room): the read reply carries the requested count capped at 32 (the room had it backwards: 16 asked, 16 returned, as seen). Further discriminator for block A2: with the radio on, send `0x0169 {1F}` then `0x0169 {07}` and look for a new `0xF602` after each; none from the keyless chip would mean host-to-chip mails are not processed after some point. The dispatcher before the `0x020A` case has no side effects (it computes two record pointers from the slot byte).** |
| right after a chip upload | the first daemon run reads back nothing; from the next run on it answers (warm-up, a few seconds to a minute; not measured further) |
| **slot clear** | after `0x0247 {slot, 0, 0}` the slot's 17 bytes still read as the old key, 1 s and 5 s later. The audit calls that "held" and says *the clear did not take*. On 08.25 a clear did stop a foreign handset in 2026-10-23 (`vF427`), so it likely resets a flag elsewhere and leaves the bytes: the audit's "held" would then be a false alarm. **Open: Q-0073.** For the cache: evict by overwriting with the new key, not by clearing, until the room says what the clear does |

## 5. Build order

1. T-A and T-B need no code (existing options: the audit, `nochipkeys`).
2. The base passes a record TPUI at every site that sets `cfg.sub_slot`
   (done as a test knob, §4a; per-record values come with the controller's
   pool).
3. The host store / chip slot split, then the cache in `dect_base`, behind
   the controller's placement rules. API bump, notes for the controller.
4. Standalone boxes keep six until T-C, T-D and T-E have passed.

## 6. More than six in the chip: the room's route (A-0136, A-0137)

The maintainer wants at least 16 chip slots (a makerspace cell). The room
(A-0137): sixteen cannot be reached by operand edits alone (eight is the
byte-mask ceiling), so some authored code is needed. The cheaper shape (I):
keep six to eight active handset numbers, grow only the key and identity
tables to 16, pick the entry by identity at link setup. About one in three or
four for a working 08.25 build, with bench iteration. Delivery would be a
plain-text patch list (offset, width, old bytes, new bytes, comment) applied
by a script to the image the user supplies, after a whole-image SHA-256
check; nothing patched is shipped and no image reaches this side. Whether the
list may live in the repo is the maintainer's decision (the room keeps CODE
entries out of it); nothing is committed. The room also asks first: if the
chip does not need the key at all for ciphering (A-0126: the per-call key
arrives with `0x0208`), 16 host-cached keys may already do everything and the
patch is unnecessary.

Free-RAM probe, lab 7520 on 08.25, idle, 2026-10-31 17:34 (`-a memdump`, 24
blocks of 32 bytes, three per window, 16 sweeps; `mem: probe` log lines print
the bytes of any block outside 0x9518..0x95EF without `-p`):

| window | blocks read | result |
|---|---|---|
| 0xA000..0xBC00, 0x6301..0x7855, 0x4D46..0x6100, 0x400D..0x4844, 0x893C..0x9000, 0x8400..0x8800, 0x97F4..0x9B85, 0xC001..0xCFFF (0xC001 and 0xC800 only) | 24, every sweep answered | all zero in every sweep, idle |
| control: 0x08B0, 0x010A, 0x1AB9 | 3 | non-zero, answered (the reads work) |

Open: all-zero cannot tell free RAM from memory that reads as zero but is not
RAM (0xC001 in particular: it answered all-zero like the others); the same
windows during a call are not read yet (needs a call, the user at the bench).
Both go to the room.

**Decision, 2026-10-31 (maintainer):** the host cache is the route (the
controller is a large computer; 16 or more handsets on standby cost it
nothing). The chip patch is parked, not dropped: it is worth taking up again
only if block E of the bench sheet shows the chip needs its key for ciphering.
The room's A-0138 (a write sibling of the memory read, `0xFFF1` sub `0x0D`,
for a write test of the free windows) is kept for that case; nothing was
written to chip memory.

**The room's A-0139 (2026-10-31 evening): the chip does not derive a cipher or
auth key from the stored key `[static trace, 08.25]`.** The key table is read
at run time in one place only: the copy made at link setup (plus the write and
the boot clear). No cipher, auth or paging routine reads it. The operative
cipher key is the host's per-call `0x0208` (A-0126), written into the
per-connection record before the over-air cipher request; the link-setup copy
puts the stored key into the same field as a default, which `0x0208` replaces
before any ciphered traffic. Paging is by TPUI (A-0131); authentication is
host-side. Caveats the room names: the on-air cipher engine's read of that
field was not single-stepped, and it did not prove that both writes go to the
identical record. **Prediction for bench block E (written before the run):** a
ciphered call to a handset the chip holds no key for works, authenticates and
ciphers normally, while the host sends the right `0x0208`; keep one other key
in the table so chip admission (open on 08.25) is not the variable. Fail
(call refused, no cipher, or a cipher failure): the stored key is used in a
way the trace missed, and the chip patch is back on the table. Consequence if
it passes: the host cache is sufficient and the patch stays parked.

**Block E, run 1: lab 7520 on 07.56, 2026-10-31 18:46-18:50 (A540, no key in the chip).** The chip was given
one key only (the C430A, store `.1`, slot 1; `holdback=3600000` kept the other
two stores' keys out, and the daemon's own line says it cleared every slot
but 1). 07.56 cannot read its table back, so "no key in the chip for the
A540" rests on that code path. The A540 (slot 3, held back) connected within
seconds of the daemon coming up, and over four link sessions (two list
sessions, a refused-then-repeated one, then a dialled call) each one went:
authenticate, call key to the chip (`0x0208`), CIPHER-REQUEST, "the chip
reports link N ciphered (`0x0209`)". The dialled call stayed up 28 s with the
audio bound and no error counters moving. **Pass for the prediction:** a
ciphered call on a handset whose key the chip does not hold. No dial tone and
no ring on the called mobile were seen; the cause is not the chip: the init
wrote no `lines` file and started no baresip (the handsets' profile side files
`/etc/opendectcell-sub.N.profile` are gone since today's re-registrations), so
nothing was dialled. Not yet shown: the same with the table read back (08.25,
where the slot audit exists), and a call to such a handset (paging by TPUI).

**Block E, run 1, addendum (maintainer, same evening):** after a line was
assigned to the A540 (still no key in the chip, slot 3, `holdback` not over),
an outgoing call went through at once with audio both ways, and an incoming
call rang with audio both ways. So both directions work for a handset whose
key the chip does not hold: the call key reaches the chip with `0x0208`, and
the chip pages by the TPUI. Open: the same with a read-back of the table
(08.25), and a handset that has never had a slot at all (here the A540 still
has its slot and identity entry; only its key is missing).

**Block E / B, run 2: lab 7520 on 08.25, 2026-10-31 18:55-19:05 (A540, warm chip, key verified absent).** Chip
reloaded, then two more daemon runs until the read-back answered (the first
two did not: cold chip). Slot audit: `1=OURS 2=- 3=- 4=- 5=- 6=-`: the
C430A's key in slot 1, nothing for the A540 (slot 3) or AS690H (slot 2);
`holdback=3600000` not over. **Block B answered on 08.25: with one other key
in the table and a warm chip, the A540 connected on its own within about 20
s of the daemon coming up, with no key in the chip** (world A, as on 06.88
and 07.56; the earlier "08.25 silent" runs were an empty table on a cold
chip). **Block E passed on 08.25:** authentication, call key `0x0208`,
CIPHER-REQUEST and "the chip reports link N ciphered (`0x0209`)" on three
links; an outgoing call (audio both ways, timer, number shown) and an
incoming one (ringing DECT 3, audio both ways) per the maintainer.
**Caveat on the incoming call:** the A540's TPUI is `11 03`, the form the chip
treats as slot 3 (A-0133 §1 item 4), so this page went by that shortcut, not
by an entry for the handset. A handset with no slot and a pool TPUI of 7 or
more has still not been paged; that is the remaining test for the host cache.

**TPUI test, run 3: lab 7520 on 08.25, 2026-10-31 19:15 (A540 with no key, no slot and no install in
the chip, `tpuioffset=0x40`).** After a power cycle the A540's TPUI was
`11 43` (diag `001143`, not a slot shortcut). Two calls to it: the page
`0x0300 {0c 11 43}` was answered at the first attempt both times, the link
came up, the call ringed, ciphered (`0x0209`, link 2) and had audio
(maintainer). **Pass: the chip pages a handset it holds no slot for by a pool
TPUI of 7 or more.** One log line to note: "WARNING: TPUI never acknowledged"
and `tpui_confirmed:false` in the diag, although the handset answered the
page by the new TPUI; that flag is not set on this path (the location
session where the assignment was acknowledged is a different link) and is a
logging question for the library, not a failure. Together with run 2
(keyless admission, keyless ciphered calls both ways) this answers the
design question: a standby handset needs no chip slot at all. The six slots
cache only what an active link needs; the host holds the rest.

**The room's A-0140 (static side of run 3, same evening):** the page `0x0300
{0c 11 43}` is sent by the chip verbatim from the mail: with a TPUI whose
high byte is `0x11` the chip short-circuits its full-TPUI builder and never
consults the identity table or the key table to build a page; the low byte
(7 or more) is not range-checked. Same structure on 07.56. So paging a
slotless handset is the normal assigned-TPUI path, not an edge case. **Rule
that follows for the controller's TPUI pool: keep the high byte `0x11`** (form
`{00, 11, N}`, N of 7 or more, per `notes-for-the-controller.md`, API 141). A
TPUI with another high byte needs the full form (20-bit TPUI in the mail's
bytes 5 to 8); the room read that path statically only, not on air. The chip
patch is closed; its notes stay on file in the room.

**Registration over an existing record, seen on air (7412, API 139 daemon, 2026-10-31 19:19):** the
maintainer registered the AS690H while the entry of an old A540 still held
record 0 ("enrolment into record 0 (slot 1)", "handset registered"), and no
"subscription saved" followed. Every session after it failed authentication
("WRONG RES", three times in a row) because the handset had the new key and
the store the old one. Deleting the entry and registering again saved it.
This is the defect fixed in API 140 (the registration persists after a 2 s
grace), which the 7412's daemon does not have yet; the registration itself
was therefore lost, and the AS690H must be registered again.

**Block E on 06.88: the 7412, 2026-10-31 19:33-19:42 (AS690H, no key in the chip, `tpuioffset=0x40`, API 139 daemon).**
The chip was reloaded and warm; the audit read `1=OURS 2=held 3=- 4=- 5=- 6=-`
(slot 1 the A540's key, slot 3 the AS690H's: empty; slot 2 an old key of
another handset). The AS690H, registered afresh into record 3 / slot 3 and
power-cycled, connected, authenticated and ciphered (`0x0209`), placed a call
with audio both ways, and, with DECT 1's line removed so that it was the only
ring target, was paged by `0x0300 {0c 11 43}` at the first attempt, rang and
had audio both ways (maintainer; log `run7`). **Pass on 06.88: a standby
handset needs no chip slot on any of the three builds.**

**A defect found on the way (task L6).** With both handsets on the line and
the A540 switched off, the AS690H did not ring: the main ring machine waited
for DECT 1's page response, the AS690H answered its own (side) page, and
`dect_base_main_page_response` rejected that answer with LCE-PAGE-REJECT
because its identity differs from the main ring target ("page response from a
handset we did not page (identity differs) ... still waiting", three times).
The side-ring page for DECT 3 went out correctly (its TPUI is built from its
own slot); it was the answer that was thrown away. The code is unchanged at
HEAD (`dect_base.c`, `dect_base_main_page_response`): it handles an internal
call's target and the deregistration and time pages, but not the target of a
bridge side ring. Effect on a cell: one handset that is out of range or
switched off can stop other handsets ringing for the same external call.

**The room's A-0141 (2026-10-31 night), on Q-0077: security and the number of calls.**
- *Cipher:* none of the three STUB builds contains AES tables; the chip's only
  cipher is the standard DECT cipher with a 64-bit key (the `0x0208` key
  field). `0x0208` has no cipher-type byte. So no DSC2 on any build. A handset
  that insists on it cannot be served that way.
- *Authentication:* entirely on the host (A-0139), so DSAA2 would be a host
  software change (AES-128). Its 128-bit key does not fit the chip's 64-bit
  cipher: DSAA2 authentication with the standard cipher on the air is the only
  combination the chip supports; whether the Gigasets accept it is a
  negotiation question for our NWK code.
- *Host-side speech ciphering:* not possible. The cipher is hardware in the
  chip's radio layer, and the speech reaches the host already deciphered and
  decoded (PCM). Stronger protection of the speech means SRTP on the VoIP leg.
- *Calls at once:* the limit is not "three" in general. It is the number of
  DSP channel chains in the build's microcode: **three on 07.56, five on
  08.25** (06.88, the 7412, not counted yet; at least three is observed).
  Our main plus two side channels are three PCM lanes we allocate, which
  coincides with 07.56. **Five speech calls on 08.25 needs host work only**: PCM
  audio lanes 4 and 5, a bearer each, and `0x0230`/`0x0242` driven for channels
  4 and 5 as for 1 to 3. Six is beyond every build. Not tried.

**The room's addendum to A-0141: the 7412 (06.88) has three channel chains.**
Capacity table: 06.88 = 3, 07.56 = 3, 08.25 = 5. The 06.88 configurator has
cases for channels 1 to 3 only and rejects 4 or more, so a fourth and fifth
call on the 7412 would need a DSP-microcode change (the parked chip-patch
category), not host work. Two routes if the controller needs more than three
calls per cell on the 7412: (1) run the 08.25 STUB on the 7412 (A-0003: same
SC14441, can boot the 7520's 08.25 STUB; caveats: the `dectmode` tag value, and
the 7412's own factory RFPI and calibration, not the 7520's), which gives
five calls with host work only; (2) edit the 06.88 microcode, hard and
parked. If a blob is ever swapped for more channels, the keyless-admission,
cipher and slotless-paging results above must be taken again on that blob
(they were measured on the native builds).
