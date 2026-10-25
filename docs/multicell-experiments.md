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
