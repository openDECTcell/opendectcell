# Multi-cell experiments on one box

ROADMAP M11. A handset roams between cells (FPs) managed by one controller.
Each cell's chip holds at most six subscriptions (`DECT_MM_MAX_SLOT`), so a
large installation cannot preload every handset into every cell. Whether it
must depends on one chip property, measurable on a single box.

## Test M1: does the chip admit a handset whose key it does not hold? `[pre-commit, 2026-10-25 morning, unattended]`

Two worlds, two designs:

- **A, the chip admits any handset locked to our RFPI.** The handset opens a
  link and sends `{LOCATE-REQUEST}` with its IPUI; the cell asks the
  controller for the record and installs it. Budget: the handset's
  `<MM_locate.1>`, seconds.
- **B, the chip refuses handsets it holds no key for.** The host never hears
  from the handset, so on-demand loading needs some chip report of the
  access attempt; otherwise records must be in the chip before the handset
  arrives, six per cell.

Evidence so far conflicts. `vF427` (08.25 STUB, 2026-10-23): clearing the
other chip slots stopped a foreign C4 dead, zero connections: **B**. Run 23
(07.56, 2026-10-25): the A540 connected twenty times after its slot was
"cleared"; but on 07.56 the clear cannot be audited (`Q-0059`), so that is
either A or a clear that did not take.

**Method.** Reload the chip firmware (`fwload.sh`, 07.56): a chip reset
empties its RAM, so no stale slot survives. Then `dect-stubd` with **only**
the A540's store (`ALLSTORES=0`) for 180 s, no ring, `-p`. The C430A and
AS690H stay on, idle, out of the host's records too.

| shows | meaning |
|---|---|
| C430A/AS690H send `{LOCATE-REQUEST}` (we answer "IPUI unknown") | **world A on 07.56**: the chip admits; on-demand loading at location registration is possible |
| **no connection from them at all in 180 s**, the A540 connects | **world B on 07.56**: the chip gates on its table; go to M2 and M3 |
| nobody connects, the A540 included | void: the fresh chip or the bring-up failed; read the log |

## Test M2: how fast does a handset attach once its key is installed? `[pre-commit]`

Only if M1 shows world B. Fresh chip, all three stores held by the host,
but the C430A's key installed in the chip only at t = 60 s
(`-a holdback=60000`). Measure from the install to the C430A's first
contact.

| shows | meaning |
|---|---|
| contact within a few seconds of the install | a cell can load on demand **if** it learns of the attempt (M3); the handset retries access often |
| contact only after tens of seconds or more | the handset backs off; on-demand loading would feel like a dead cell; preloading per cell |
| no contact by the end | the late install does not work at all; void, read the log |

## Test M3: does the chip report an access attempt by an unknown handset? `[pre-commit]`

From M1's log when it shows world B: compare the received mails against a
run with all stores loaded. A mail type, or a count of an existing one,
that appears only while the unknown handsets are trying, is the hook for
on-demand loading. Nothing: on-demand loading is not possible host-side.

## M1 first runs: void `[2026-10-25 04:12-04:16]`

`M1-041214`: started seconds after the chip reload, "control slot not found",
exited before any mail; nothing transmitted. `M1b-041531`: ran 180 s on the
fresh chip, parked, and **nobody connected, the A540 included**, although its
key was installed. The void row. The handsets had been without a base for
~70 minutes, and an idle Gigaset's search backs off (162 s after 17 min off,
run 20), so 180 s proves nothing about the chip.

## M1+M2 combined: one run that separates back-off from gating `[pre-commit]`

Fresh chip (still the one loaded at 04:10), all three stores held by the
host, the A540's key installed at start (positive control), the C430A's and
AS690H's keys **held back to t = 300 s** (`-a holdback=300000`), 600 s.

| shows | meaning |
|---|---|
| C430A/AS690H connect **before** 300 s (keys not in the chip) | **world A**: the chip admits without a key |
| they connect only **after** 300 s, within seconds of the install, while the A540 connected before | **world B**, and a handset retries access fast enough for on-demand loading once the cell knows to load |
| they connect after 300 s but late (minutes) | world B likely; the delay is handset back-off, not proof either way |
| nobody connects in 600 s, the A540 included | void again: the handsets are not searching; needs a key press |

## M1+M2 result: void, and a suspect `[M12-*, 2026-10-25 04:17-04:27]`

600 s, nobody connected, the A540 included (key installed at start); the
held-back keys went in at 300 s. The radio-on mail went out and the chip
answered its status and RSSI scans, but there was not one connection.

The suspect is our own `noregopen` (`8abc37c`): on a **freshly booted**
chip it sends close-registration (`0x022c 01`) and never `0x022b 41`, which
the verification table calls the "enable token". Last night's `noregopen`
check ran on a chip that had had `0x022b 41` in every earlier run, and chip
RAM keeps it. If the chip needs the token once to admit anyone, `noregopen`
on a fresh chip locks every handset out -- and the service defaults to it.

### M0: the same fresh chip without noregopen `[pre-commit]`

300 s, the A540's store only, registration as the lab always ran it
(`0x022b 41`).

| shows | meaning |
|---|---|
| the A540 connects | the token is needed; `noregopen` must send `0x022b 41` and then `0x022c 01`; M1/M2 rerun |
| nobody connects again | not the token; the handsets are simply not searching (key press needed) |

### M0 result: the token is not needed; the voids were back-off `[2026-10-25 04:28-04:37]`

`M0-*` (fresh chip, `0x022b 41`): the A540 connected at 293.7 s, which fits
either reading. The decisive pair, with the A540 awake from that contact:
chip reset again, then `M0n-043310` **with `noregopen`**: first contact at
**8.9 s**, `{LOCATE-ACCEPT}`; `M0o-043511` without: 1.4 s. **`noregopen` works
on a fresh chip.** The suspect row is falsified, and M1b and M1+M2 were void
for the reason first given: the handsets were not searching.

### M1, redone with an awake subject `[pre-commit]`

The A540 is awake; the others are not. So the A540 is the subject: chip
reset, `-s c430a -s a540 -s as690h`, `holdback=60000` (the C430A's key at
start; the A540's and AS690H's at 60 s), 150 s, host holds all three.

| shows | meaning |
|---|---|
| the A540 connects **before** 60 s | **world A** on 07.56: the chip admits without a key |
| the A540 connects only **after** 60 s, within seconds of the install | **world B**; a handset retries fast enough for on-demand loading, if the cell learns of the attempt (M3) |
| the A540 does not connect at all | void: it went back to sleep; run again after M0-style wake-up |

### M1 result: world A on 07.56 -- the chip admits handsets it holds no key for `[M1c-*, 2026-10-25 04:38]`

Fresh chip (reset by the firmware reload), the C430A's key installed at
start, the A540's and AS690H's held back to 60 s; `clear_foreign_slots`
also cleared slots 1 and 3 explicitly at start.

| t | event |
|---|---|
| 29.2 s | **AS690H** connects, `{LOCATE-REQUEST}` with its IPUI; key not in the chip |
| 33.3 s | **A540** connects (its list-access call); key not in the chip |
| 52.1 s | A540 again, `{LOCATE-REQUEST}` |
| 60.0 s | the held-back keys installed |

Pre-committed row: "the A540 connects before 60 s -- world A on 07.56: the
chip admits without a key". **That is what happened, twice, and the AS690H
did the same.** The chip's subscription table does not decide who may open
a link on this build; the host's identity gates do. The UAK slots matter for
what the chip itself does with the key (ciphering), still to be pinned.

**For multi-cell:** a cell learns a roaming handset's IPUI from its
`{LOCATE-REQUEST}` and can ask the controller for the record then; the
budget is the handset's `<MM_locate.1>`. The six chip slots do not cap how
many handsets a cell can admit.

**This contradicts `vF427`** (08.25, 2026-10-23), where clearing the other
slots stopped a foreign C4 dead, minutes after it had connected 51 times.
Either the builds differ (08.25 gates, 07.56 does not), or the clear does
something on 08.25 that it does not on 07.56 -- run 23 already saw a
"cleared" handset connect on 07.56. Unresolved; a run on 08.25 with the same
design would settle it. Until then, every multi-cell claim here is
**07.56 only**.

### M2': does ringing work for a handset with no key in the chip? `[pre-commit]`

Admission is not the whole of roaming: the cell must also page and ring a
handset it has just learned about. Fresh chip, `-a nochipkeys` (every slot
cleared, no key installed), ring the A540, ~5 s.

| shows | meaning |
|---|---|
| ALERTING | paging and ringing need no chip key either: a cell can serve a roamer end to end from host records |
| page sent, no page response | the chip pages only handsets in its table: a roamer must be installed before it can be called |
| page response, SETUP refused | something in the call path wants the key; read the release reason |

### M2' result: the keyless handset rang, but answered the page with a location registration `[vRINGA540-20261025-074255-1]`

Fresh chip, `nochipkeys`: page `0c 11 01` at 21.5 s; the A540's link up at
22.9 s; on it, **`{LOCATE-REQUEST}`** (accepted, TPUI acked) instead of
`{LCE-PAGE-RESPONSE}`; the daemon's page-response deadline passed, our
SETUP went out on that link at 23.5 s, **ALERTING** at 24.7 s. So a handset
with no key in the chip can be called -- but the page did not come back the
way it does in keyed runs.

**Control** `[pre-commit]`: same conditions, keys installed (fresh chip,
awake A540, ring 10 s after first contact).

| shows | meaning |
|---|---|
| `{LCE-PAGE-RESPONSE}` after the page | the key matters for how a paged handset comes back; a roamer should be installed before it is paged, or the cell must accept a LOCATE as the answer |
| `{LOCATE-REQUEST}` again | the fresh chip (the handset's location state), not the key; keyless paging behaves like keyed paging |

### M2' control result: keyless paging behaves like keyed paging `[vRINGA540-20261025-074747-1]`

Keys installed, fresh chip: the A540 answered the page with
`{LOCATE-REQUEST}` too, and rang. Runs 31 and 34 (keyed, last night) did the
same. Pre-committed row: "the fresh chip (the handset's location state), not
the key". **On 07.56 a handset with no key in the cell's chip can attach,
locate, be paged and ring.** The key matters for ciphering only, not yet
exercised (`-C`).

## Test M4: how long may a cell take to fetch a roamer's record? `[pre-commit]`

With admission free, roaming becomes: a `{LOCATE-REQUEST}` arrives with an
IPUI the cell does not hold; the cell asks the controller; the answer comes
back; the cell accepts. EN 300 175-5 annex A: the PT's `<MM_locate.1>` is
**20 s**, retransmit at the first expiry, abort at the second; the FT may
restart it with `{MM-NOTIFY}` <<TIMER-RESTART>> (NOTE 4: "communication with
external networks").

Method: MM holds an unknown `{LOCATE-REQUEST}` instead of rejecting it
(API 18); `dect-stubd -a remote=PATH,fetchdelay=MS` plays the controller:
the A540's store is not held until MS after its request. Delays 2, 15, 30,
45 s, back to back so the A540 stays awake.

| delay | predicted |
|---|---|
| 2 s, 15 s | accepted on the first `{LOCATE-REQUEST}` |
| 30 s | the A540 retransmits at ~20 s; our accept still lands |
| 45 s | past both timers: the A540 aborts; our late accept finds no transaction |

**Falsifying:** a handset that gives up well before 20 s, which would make
on-demand fetching far tighter than the standard promises.

### M4 results so far: 2 s works; and a roamer need not locate at all `[2026-10-25 04:54-05:03]`

| run | delay | result |
|---|---|---|
| `M4-2000-045441` | 2 s | the A540's `{LOCATE-REQUEST}` held, record "fetched", **`{LOCATE-ACCEPT}` at +2.000 s**, TPUI acknowledged: it registered with a cell that did not know it two seconds earlier |
| `M4-15000-045732` | 15 s | **void**: the A540 sent **no** `{LOCATE-REQUEST}`; it came straight back with its list-access `{CC-SETUP}`, which the CC gate turned away (`0x0A`), twice |

The 15 s run is the finding. Three minutes after an accepted location
registration, with the same RFPI and location area, the A540 considered
itself located and simply used the base. In the 2 s run the same happened
in miniature: while its request was held, its list-access call arrived and
was turned away; only after the accept was it served.

**For multi-cell:** cells under one controller share a location area, and
a handset moving between them **does not re-locate** (EN 300 175-5 §13.4:
location registration is triggered by a new location area, switch-on, or an
expired validity). The first thing a cell sees from a roamer is then a
`{CC-SETUP}`, a page response or a list-access call from an unknown IPUI.
So either:

1. **fetch on any first message from an unknown IPUI**, holding a
   `{CC-SETUP}` the way MM now holds a `{LOCATE-REQUEST}` (the PT's
   `<CC.03>` is 20 s too, annex A), or
2. **the controller pushes records to every cell of the area** before they
   are needed -- and since the chip's six slots do not limit admission on
   07.56 (M1), host-side records are cheap.

Option 2 is simpler and has no first-call latency; option 1 scales without
bound. Either way the identity gate on CC must fetch or know, not just
reject.

A tool for M4's longer delays: our `{LOCATE-ACCEPT}` may carry <<DURATION>>
(§13.4.1), a validity after which the PT must locate again. A short one
would make the A540 re-locate on demand; not built yet. Until then a long
radio-off pause is the only way to trigger a fresh location registration.
