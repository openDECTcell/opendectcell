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
