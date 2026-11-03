# Bench status, 2026-11-02: what is done, what is likely, what matters

This reconciles every open bench test in [bench-next.md](bench-next.md),
[bench-later.md](bench-later.md), [bench-7412.md](bench-7412.md) and
[bench-2026-11-01.md](bench-2026-11-01.md) against the results recorded in
those files, in `docs/more-than-six.md` and in the controller's work log. The
older run sheets list about 50 open tests; most of them were run later, or
were overtaken by the controller. Where this page says "covered", it names the
later run that covers it. Nothing here is a new measurement.

**Status words.** *Done*: passed on hardware (date). *Covered*: not run as
written, but a later run exercised the same path. *Partial*: some of it ran.
*Open*: no run. *Drop*: no longer needed (reason given).

**Likely to work** is a judgement from earlier results, not a result: *high*
means the same code path already ran on air; *medium* means built and
tested at every layer except the radio; *unknown* means we have no evidence
either way.

**★** marks the tests that are actually important: they decide whether a
claim we want to make in public holds, or they guard a change made since the
last time the path ran on air.

## ★ The tests that matter

| # | Test | Why it matters | Likely to work | Needs |
|---|---|---|---|---|
| ★1 | **Internal call between two handsets on different cells**, A540 (cell on the 7412) and AS690H (7520), both directions; also the A540 typing the internal number | Built 2026-11-02, never on a handset. It is the first call that joins two cells' audio in the controller. Same-account isolation is covered by unit tests; the air part is the call itself | Signalling: high (the internal SETUP and the INT key ran on air in 2026-10-28 cards 5/11, and the AS690H rang). Audio relay between two cells: medium (simulation only). The AS690H as caller (Part 1, types a number after INT): unknown | Both handsets, both cells up |
| ★2 | **Presence**: power a handset off in range of its cell, then on again | The maintainer asked for "Offline at once". That works only if the handset sends {DETACH} on power-off, which no run has shown | The fallback (Unresponsive after the roaming interval, Offline after 10 minutes) high: it needs only last-seen times and failed pages, and the page-failed path ran on air. The immediate Offline: unknown per model. Run it for all three Gigasets | A540, C430A, AS690H, one at a time |
| ★3 | **A call each way through the confined worker processes**, then an hour of idle soak | The seccomp confinement is new since the last call on air; a denied syscall kills a worker (and its cell) only when it is used. Startup is verified, a call is not | Medium: the call path is only a few syscalls more than startup, but "denied syscall on a path we did not run" is exactly the failure this test is for | One handset, the lab |
| ★4 | **A call in and out through the real SIP account after the security fixes** (INVITE source check, target filtering, per-account caps) | These changes touch every call and were tested only against the test suites and a LAN registrar. The lab's registrar is on a private address; the new target filter refuses private targets unless allowed | Medium. Check first (no handset needed) that the account still shows as registered in the web UI | One handset, the real account |
| ★5 | **A540 phonebook, INT list and call lists through the controller** after API 154 (list access now waits for the handset's proof of its key) | Phonebook and call lists on the controller path (API 151) have never been seen on a handset, and 154 changed when the lists are served after the INT list was last seen working | Reads: high (the same library code read the 388-contact book and the missed-call list on the A540 on 2026-10-30). The wait for authentication: medium (traces cover it, the A540 does authenticate at locate: card 19). Edits from the handset: medium | A540 |
| ★6 | **A wideband handset calling a G.711-only SIP peer** (the controller transcodes G.722 to PCMA) | Many providers offer G.711 only, so this is the common case, and no call has run it. Only the G.722-through path was heard | Medium: the media engine's transcoding is tested at the desk, and PCMA over the cell link is the same lane as narrowband | One handset, a peer or account limited to PCMA |
| ★7 | **Walk test**: carry a handset between two cells that cannot hear each other, idle, then call it, then call from it | The multi-cell claim itself: handset-chosen roaming, "last cell first" ringing and move history have run only by switching cells off. The apartment is too small, so it needs a larger site | Medium: the pieces ran (the A540 moved between cells when a base went away; two cells run side by side). Whether each model re-locates promptly while idle is unknown | A larger site, two cells, the maintainer |
| ★8 | **7412 stop hang** (not a test: a defect with a missing cause) | Four hangs in about 190 stops of a 7412 (ordinary stops from radio up, and quick-start stops): the network dies within 180 ms of the park mails and needs a power cycle. A cell in the field cannot be power-cycled by the controller. The last 80 stops passed, so the rate is unknown | n/a | The next hang's evidence (the guard script now saves it to flash) |

Blocked on desk work first (not yet a bench test):

- **Two handsets of the same slot class ringing together (the ring policy).**
  The collision itself was seen on air (two handsets of one class); what is
  missing is the policy (retry order, what the caller hears) and a listening
  test of the audio. Step 4 in the controller's `docs/next-steps-2026-11-02.md`.

## Everything else, with status

### Cards 1-19 (bench-next.md, 2026-10-28)

| Card | Status | Evidence / note |
|---|---|---|
| 1 Caller name on the C430A | Done 2026-10-28 | The name shows only as base-drawn display text (separate screen, then the ringer screen); a Gigaset profile now sends it |
| 2 A540 silent ring | Done | Full CALL-INFORMATION makes it ring aloud; default since |
| 3 Call timer | Done | Pass on C430A and A540 |
| 4 Encryption | Done | Plain key order; ciphered calls since |
| 5 Internal ring, redial | Partial | Internal SETUP with SIGNAL seen; whether the ring pattern differs was not judged; the redial run was void. Low value |
| 6 Wrong PIN | Done | AUTHENTICATION-REJECT; "PIN ungültig" |
| 7 nlr | Done | Default since |
| 8, 9 A540 rings, dials out | Done 2026-10-28 | |
| 10 Two calls | Done | Call ids 1 and 2, both heard |
| 11 Internal caller shown | Partial | C430A shows "1"; the A540 showed "via Intern DECT" before the name-in-CC-INFO fix (card 24). Not re-run. Likely fixed (it is what stock sends); folded into ★1 |
| 12 Busy when full | Done | |
| 13 Key during a call | Done | |
| 14 System settings list | Done | |
| 15/16 Rename, list change | Done | |
| 17 Deregister from the base | Done (A540, AS690H) | |
| 17 Lost handset (the fix `0b11de0`) | Covered in part | The 2026-11-01 controller runs deregistered an A540 that was out of range: ten pages, then "deleted without its confirmation". The case of another handset connecting meanwhile (card 22) was not run |
| 17b, 17c Deregister from the handset | Blocked | Only the A540's "Mobilteil abmelden" qualifies, and it needs a ciphered list call; low value, drop unless a user asks |
| 18 Profile effect | Done | |
| 19 Authentication at locate | Done, all three | |

### Cards 20-37 (bench-next.md)

| Card | Status | Likely | Note |
|---|---|---|---|
| 20 Ciphered list session and PIN | Covered in part | high | Ciphered list sessions have run since (388-contact phonebook, call lists, A540 location syncs). The PIN-guarded delete was not run |
| 21 Ring during the list sync | Covered | high | A call to the shared line on 2026-10-30 rang both handsets around a list sync |
| 22 Deregistration while another handset connects | Open | high | Low value; a corner of 17 |
| 23 Second call ciphered | Covered | high | Side links cipher by default; the shared-line ring on a side link ran 2026-10-30 |
| 24 Internal caller on the A540 | Open, folded into ★1 | medium | |
| 25 Bridge without SIP | Drop | | MVP reached 2026-10-28 with SIP; the controller path replaces it |
| 26 Line names | Covered | high | "via Line 1 …" shows on the A540; names reach handsets since 2026-11-01 |
| 27 First SIP call | Done 2026-10-28 | | |
| 28 8-digit PIN, chosen model | Done | | |
| 29 IPEI against the label | Open | high | 2 minutes, any run; the controller shows the IPEI "as on the label" |
| 30 Incoming call from the FRITZ!Box | Covered | high | Cards 27, 10-30 calls, 11-01 controller calls with audio |
| 31 Outgoing call from each Gigaset | Covered | high | 2026-10-28 basics: all three called out ciphered. The call-time authentication of A1 was not singled out |
| 32 Internal call and list access, wrong PIN three times | Partial | medium | Internal call and rename done; the PIN lockout path is open |
| 33 Status > DECT and files | Drop | | A shell check, no handset; the controller replaces the page |
| 34 Impostor key | Drop | | Optional; no one asked for it |
| 35 7412 | Covered | | By card A below |
| 36 Periodic date and time | Partial | high | The A540 shows the date after a reboot (2026-10-30); the periodic round on a short interval is open |
| 37 Fresh registration | Covered | high | Three handsets on the 7520, one on the 7412 (2026-10-31), then the controller's enrolment on air 2026-11-01. The two defects found were fixed |

### N, M, T, U, V series (bench-later.md)

| Test | Status | Note |
|---|---|---|
| N1, N2a-d | Covered | V2b pass; fresh registrations and restarts, 2026-10-31; controller enrolment 2026-11-01 |
| N3a-d assigned IPUI O | Drop for now | Deferred in the 11-01 sheet; revisit with class B multi-site |
| N4a, N4b class B RFPI | Done | 7412 (U2) and the lab 7520 on 08.25 (identity push, 2026-11-01). Open only for 07.56, which we no longer run |
| N4c handset finds a class B base | Covered | The A540 attaches to the lab's class B cells (RPN 0) |
| N5 Call lists | Partial | Missed list on the A540 done 2026-10-30; accepted and outgoing not checked on air; see ★5 |
| N6 STATS, N7 resume | Done 2026-10-30 | Two bugs found and fixed |
| M1a Wideband call through the controller | Done 2026-11-01 | G.722 end to end, audio both ways |
| M1b Narrowband far end | Open | ★6 |
| M1c Hang-up either end | Covered | Calls have been ended from both sides |
| T-B, T-C, E (keyless handset, TPUI form, key for a ciphered call) | Done | World A on 06.88, 07.56 and 08.25; `{00,11,N}` works; 06.88 hears by TPUI low nibble, no chip slot. The open corner (a TPUI of 7 or more, paged) was settled by the per-cell pool (2026-11-02) |
| T-D, T-E (eviction under a call) | Drop | The controller's rule avoids evicting a handset in a call |
| A, A2, B, D (07.56 eco machine, discriminators, free-RAM probe) | Done or drop | A and B done; A2 and D belong to the 07.56 and chip-patch lines, which are parked |
| C Re-registration over an existing record | Covered | Fixed in the library; the controller's enrolment of a known handset ran 2026-11-01 |
| L7 Cell LED | Open | Trivial; look at it the next time a cell runs |
| L8 No stale key at cell start | Done on 06.88 (2026-10-31); covered on 08.25 | The identity push reloaded the chip in cell mode. The dummy-key path is unit-tested only |
| U1-U6b, U8a-d, V1-V3, V1b, V1c, T-A | Done | |
| U8e, U8f | Defect, see ★8 | |
| 4b Registration open during a call | Done 2026-10-31 | |

### Card A (the standalone daemon on the 7412)

| Test | Status | Note |
|---|---|---|
| A1-A12 | Open | Shared line, two SIP calls, call waiting, R key, DTMF, voicemail, narrowband, redial, phonebook edits, call lists, delete missed call. These test the **standalone** SIP bridge, not the controller. Run them only if the standalone daemon is to be released as a product; the controller has its own paths. Likely: high for DTMF, redial, the phonebook edits (same library code as the A540 reads); medium for call waiting and hold (CAT-iq A540 only) |
| A13, A14 | Done 2026-10-30 | The A540 losing the base after `kill -9` was not watched |

### Built 2026-11-02, never on a handset, not marked ★

| Item | Likely | Note |
|---|---|---|
| Message waiting from SIP | unknown | The NOTIFY path is tested at every layer. Standalone card A6 (the handset's count) never ran either, so we have no evidence a handset shows it. Needs a provider or a hand-made NOTIFY. Worth one run when a handset is at hand |
| Call waiting, hold, transfer on the controller | n/a | Transfer is not built; hold and waiting are not seen on air |
| Three parallel calls on a cell | unknown | The chip has three voice channels; two were tested |

## What to do at the next lab session

In this order, about two hours with one person: the no-handset check of ★4
first (is the SIP account still registered?), then ★3 and ★4 together (one call
each way), ★5 and ★6 on the A540, ★1 and ★2 with the AS690H and C430A as well,
and ★7 on another day at another site. Write each outcome down before the run.
