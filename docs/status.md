# What works, and what doesn't yet

The state of openDECTcell **0.3.1** (October 2026), tested on a FRITZ!Box 7520 with
OpenWrt 25.12.5. "Tested" means seen working on real hardware with real
handsets, not only in the unit tests.

Legend: ✅ works and tested · 🟡 works with limits, or built but not fully
tested · ❌ not built yet · ➖ out of scope for now

## The base station

| Feature | State | Notes |
|---|---|---|
| Starting the DECT chip with AVM's chip firmware | ✅ | You supply the firmware yourself; see [install.md](install.md), step 3 |
| Base on the air, handsets find it | ✅ | |
| Registering handsets with a PIN | ✅ | Random 8-digit PIN by default; 6 or 4 digits for handsets that need it. Registration is open only for two minutes after you press *Register a handset* |
| Up to six handsets | 🟡 | Three handsets at once tested |
| Authentication and encryption | ✅ | Every call is encrypted by default |
| Deregistering a handset from the web page | ✅ | The handset must be switched on and in range |
| Deregistering from the handset's own menu | 🟡 | Only some handsets offer it; not yet working on the Gigaset A540 |
| Handset names (*DECT 1*, *Kitchen*, …) | ✅ | Set in the web page or renamed on the handset |
| Fon/DECT LED | ✅ | On while the radio is on, blinks on activity |
| Date and time on the handsets | ✅ | CAT-iq handsets, from the box's clock once NTP has set it (the A540, on a 7412). GAP handsets such as the C430A don't take it. Set the box's time zone under System > System |
| Signal strength and link quality per handset | ✅ | Status > DECT |
| Handset models with their own settings ("profiles") | 🟡 | Profiles for the Gigaset A540, C430A and AS690H; every other handset uses the standard behaviour |

## Calls

| Feature | State | Notes |
|---|---|---|
| Calls in and out through a SIP account | ✅ | |
| HD voice (wideband, G.722) | ✅ | Both directions, the default |
| Narrowband voice (handsets without HD) | 🟡 | Between handsets; to SIP since 2026-10-29, not yet tested on a call. Codecs are matched end to end, so the box converts only when the two sides share none |
| Caller's number on the display | ✅ | |
| Caller's name on the display | 🟡 | Sent the standard way; the C430A and AS690H show it only as a separate text screen, and on the A540 it is still being tested |
| Line name on the display ("via Line 1") | ✅ | CAT-iq handsets |
| Calls between handsets (internal calls) | ✅ | |
| Several calls at the same time | 🟡 | Up to three (the chip's voice channels). Two calls between handsets tested; several SIP calls built 2026-10-29, not yet tested |
| Two SIP accounts per handset (Line 1, Line 2) | 🟡 | Both ring the handset; a CAT-iq handset's calls go out on the line it picks, others' on Line 1 (built, not yet tested) |
| One SIP account ringing several handsets | 🟡 | A VoIP profile may be the line of several handsets; a call rings them all and the first to answer takes it (built 2026-10-29, not yet tested) |
| Redial and calls from the phonebook | 🟡 | A number sent complete goes to SIP; it was connected locally before 2026-10-29 |
| Call waiting, hold, switching between calls | 🟡 | CAT-iq handsets (the A540): call waiting, accept, reject, hold, resume, toggle, a second call out, release one. Built 2026-10-29, not yet tested; other handsets get "busy" for a second call |
| Keys during a call (DTMF, voicemail menus) | 🟡 | Sent to SIP as the profile's DTMF mode says; built 2026-10-29, not yet tested |
| "Busy" when all voice channels are taken | ✅ | |
| Phonebook on the handset | 🟡 | The Contact List from a CardDAV address book, both ways (`docs/phonebook.md`); tested against a test server, not yet on a handset |
| Call lists on the handset | 🟡 | Missed, accepted and outgoing calls in RAM, the missed call notification (message LED, message key) (`docs/phonebook.md`); on the A540 2026-10-30: a missed call notified, listed and marked read |
| Voicemail indicator on the handset | 🟡 | *Voicemail notification (MWI)* in the VoIP profile; CAT-iq handsets show the count. Built 2026-10-29, not yet tested |

## Web interface (LuCI)

| Feature | State | Notes |
|---|---|---|
| Network > DECT: settings, handsets, VoIP profiles | ✅ | |
| Status > DECT: DECT, VoIP and Log tabs | ✅ | Live view of the base, the handsets, SIP registrations and running calls with codec and encryption |
| Uploading the chip firmware in the web page | ❌ | Copy it with `scp` for now |

## Hardware

| Box | State | Notes |
|---|---|---|
| FRITZ!Box 7520 | ✅ | The development box |
| FRITZ!Box 7530 | 🟡 | Same hardware as the 7520 and the same OpenWrt image, but not tested on one yet |
| FRITZ!Box 7412 | 🟡 | With the `kmod-lantiq-ssc-pcm` transport (2026-10-29): two handsets registered, SIP calls in and out with HD voice, a call between handsets. Not released yet ([bench-7412.md](bench-7412.md)) |
| FRITZ!Box 7362 SL | ❌ | Analysed only |
| Other FRITZ!Boxes | ❌ | Many use the same DECT chip; see [hardware-abstraction.md](hardware-abstraction.md) for what a new model needs |

Only **OpenWrt 25.12.5** is supported: the kernel module must be built for the
exact kernel on the box.

## Handsets

| Handset | Standard | Tested with openDECTcell |
|---|---|---|
| Gigaset A540 CAT | CAT-iq (NG-DECT Part 3) | Registration, calls in and out, HD voice, encryption, number and line on the display, SIP calls both ways |
| Gigaset C430A | GAP / NG-DECT Part 1 | Registration, calls, HD voice, encryption, number. Shows no date and time from the base |
| Gigaset AS690H | GAP / NG-DECT Part 1 | Registration, calls, HD voice, encryption, number |
| AVM FRITZ!Fon C4 | CAT-iq | Registered and called during development; not tested with this release |

openDECTcell follows the DECT standards (GAP, CAT-iq) rather than one maker's
habits, so other GAP or CAT-iq handsets should work too. Reports of other
handsets are very welcome; see [CONTRIBUTING.md](../CONTRIBUTING.md).

## Not tested yet

- `tools/dect-fw-extract.py` has been tested on synthetic images only, not yet
  on a real FRITZ!OS image. If it fails for you, please open an issue.
- Installing on a freshly flashed box (the development box was set up step
  by step).
- Three calls at the same time (the chip has three voice channels; two were
  tested), and more than one SIP call at a time.

## Later, or out of scope

- **Several boxes as one DECT system** (multi-cell, roaming between boxes):
  researched, with a separate controller project. Handover during a call
  needs the boxes to be synchronised, which is unsolved. ➖ for now.
- **ULE** (DECT smart-home devices) and **SUOTA** (handset firmware updates):
  ➖.

What is planned next: [ROADMAP.md](../ROADMAP.md).
