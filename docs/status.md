# What works, and what doesn't yet

The state of openDECTcell **0.3.7** (a pre-release; November 2026), tested on a FRITZ!Box 7520 (OpenWrt 25.12.5)
and a FRITZ!Box 7412 (25.12.6). "Tested" means seen working on real hardware with real handsets, not only in the
unit tests. The 0.3.5 packages (OpenWrt 25.12.6) run on the lab 7520 and the 7412; the images are built but not yet flashed on a box.
Per feature, what is only unit-tested or only built is marked 🟡. The dated bench results are in
[journals/bench-status-2026-11-02.md](journals/bench-status-2026-11-02.md).

Legend: ✅ works and tested · 🟡 works with limits, or built but not fully
tested · ❌ not built yet · ➖ out of scope for now

## The base station

| Feature | State | Notes |
|---|---|---|
| Starting the DECT chip with AVM's chip firmware | ✅ | You supply the firmware yourself; see [install.md](install.md), step 3 |
| Base on the air, handsets find it | ✅ | |
| Registering handsets with a PIN | ✅ | Random 8-digit PIN by default; 6 or 4 digits for handsets that need it. Registration is open only for two minutes after you press *Register a handset* |
| Up to six handsets | 🟡 | Three handsets at once tested |
| Authentication and encryption | ✅ | A handset proves its key before a call is routed or a list is served, and calls are ciphered with the DECT standard cipher when the handset supports it. DECT's own cipher and authentication are weak (published attacks), so this is not a substitute for end-to-end security; a call whose cipher request the handset rejects or never completes goes on unciphered |
| Deregistering a handset from the web page | ✅ | The handset must be switched on and in range |
| Deregistering from the handset's own menu | 🟡 | Only some handsets offer it; not yet working on the Gigaset A540 |
| Handset names (*DECT 1*, *Kitchen*, …) | ✅ | Set in the web page or renamed on the handset |
| Fon/DECT LED | ✅ | On while the radio is on, blinks on activity |
| Date and time on the handsets | ✅ | CAT-iq handsets, from the box's clock once NTP has set it (the A540, on a 7412). GAP handsets such as the C430A don't take it. Set the box's time zone under System > System |
| Signal strength and link quality per handset | ✅ | Status > DECT |
| Handset models with their own settings ("profiles") | 🟡 | Profiles for the Gigaset A540, C430A and AS690H and the Motorola O201; every other handset uses the standard behaviour |

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
| Phonebook on the handset | 🟡 | The Contact List from CardDAV address books, both ways (`docs/phonebook.md`); the A540 reads a Nextcloud book of 388 contacts (2026-10-30); edits from the handset not yet tested on the air |
| Call lists on the handset | 🟡 | Missed, accepted and outgoing calls in RAM, the missed call notification (message LED, message key) (`docs/phonebook.md`); on the A540 2026-10-30: a missed call notified, listed and marked read |
| Voicemail indicator on the handset | 🟡 | *Voicemail notification (MWI)* in the VoIP profile; CAT-iq handsets show the count. Built 2026-10-29, not yet tested |

## Web interface (LuCI)

| Feature | State | Notes |
|---|---|---|
| Network > DECT: settings, handsets, VoIP profiles | ✅ | Field by field in [luci-manual.md](luci-manual.md) |
| Network > DECT: Phonebook (CardDAV) | 🟡 | With `opendectcell-carddav`; set up with a Nextcloud account (2026-10-30) |
| Network > DECT: cell mode (enrol with a controller, leave) | 🟡 | With `opendectcell-cell`; enrolment, link and leave tested on a 7412 against a controller in Docker (2026-10-30), radio parked. Built since and tested only against a scripted controller: certificate renewal, RTP/SRTP media for the controller's calls, the enrolment string passed in a root-only file. Not on air: audio through the controller; CA rotation (trust update) is not built |
| The DECT base stack in the library, for the controller | 🟡 | `libopendectcell` runs the base stack (API 138); the controller hosts it next. Library hook for the controller's handset visibility (a handset sees only its account's handsets and cannot rename any): unit-tested, not on air |
| Status > DECT: DECT, VoIP and Log tabs | ✅ | Live view of the base, the handsets, SIP registrations and running calls with codec and encryption |
| Uploading the chip firmware in the web page | 🟡 | Network > DECT > Firmware (2026-10-31): upload the two `.hex` files from `tools/dect-fw-extract.py`; the box checks them. Backend tested with a fake board; the browser side (upload, check, delete) not yet clicked through on a box. `scp` still works |

## Hardware

| Box | State | Notes |
|---|---|---|
| FRITZ!Box 7520 | ✅ | The development box |
| FRITZ!Box 7530 | 🟡 | Same hardware as the 7520 and the same OpenWrt image, but not tested on one yet |
| FRITZ!Box 7412 | 🟡 | Works as a standalone base and as a cell of a controller, with the `kmod-lantiq-ssc-pcm` transport, which the release includes: handsets register, SIP calls in and out with HD voice, calls between handsets (2026-10-29 to 2026-11-02). In rare cases the box's network died at a service stop (4 in about 190 stops on 2026-10-30/31; 640 stops since did not reproduce it): [7412-stop-hang.md](7412-stop-hang.md) |
| FRITZ!Box 7362 SL | 🟡 | With `kmod-lantiq-usif-uart` and `kmod-lantiq-tdm-pcm` (their own repositories; the release includes the packages): works as a cell of a controller (2026-11-04): a handset registered, SIP calls in and out with HD voice, the cell survives service restarts and reboots. As a standalone base with SIP it is not tested yet; how many calls at once is not measured. In the first seconds after a boot the transport once lost frames (the maintainers' notes; bench: [journals/bench-7362sl-g6-g7.md](journals/bench-7362sl-g6-g7.md)) |
| Other FRITZ!Boxes | ❌ | Many use the same DECT chip; see [hardware-abstraction.md](hardware-abstraction.md) for what a new model needs |

A release is built for **one OpenWrt version** (named in `tools/release/OPENWRT_VERSION`, and on the release page):
the kernel module must be built for the exact kernel on the box. Flash the release's image for your box, or install
the packages on exactly that OpenWrt release; a box on another release needs the release built for it.

## Handsets

| Handset | Standard | Tested with openDECTcell |
|---|---|---|
| Gigaset A540 CAT | CAT-iq (NG-DECT Part 3) | Registration, calls in and out, HD voice, encryption, number and line on the display, SIP calls both ways |
| Gigaset C430A | GAP / NG-DECT Part 1 | Registration, calls, HD voice, encryption, number. Shows no date and time from the base |
| Gigaset AS690H | GAP / NG-DECT Part 1 | Registration, calls, HD voice, encryption, number |
| AVM FRITZ!Fon C4 | CAT-iq | Registered and called during development; not tested with this release |
| Motorola O201 | GAP only, narrowband only (not CAT-iq) | With its profile `motorola-o201` (2026-11-05, a standalone 7412): registration, calls in and out with audio both ways, 3 of 3 calls. It needs the profile: without it incoming calls are refused and outgoing calls are silent. Name display not working (it shows "Externer Anruf"); through the controller on a 7520 (2026-11-09): calls in and out with several handsets ringing from one line, narrowband speech converted to the line's G.722. **Known problem:** after the caller hangs up while it rings, it goes on ringing for 10 to 15 seconds; the release is acknowledged and its call is gone, so it looks like the handset's own ring timer (an extra "alerting off" signal did not help); [handset-compatibility.md](handset-compatibility.md#the-motorola-o201) |
| Siemens C45, C450 | GAP only (old) | Register, dial out, and the end of a call from the far side works. **Incoming calls do not ring:** the base pages the handset every 3 seconds and never gets an answer, although the handset acknowledged its temporary identity at registration. Examined against GAP (2026-11-10): nothing mandatory is missing; open suspects are the validity limit we send in the registration answer and paging by the default identity. Under investigation |

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
  a separate controller project. A box can already join a controller as a
  cell (`opendectcell-cell`), and the DECT stack now sits in the shared library
  `libopendectcell` so the controller can run it; the controller running
  handsets through its cells has been done since November 2026 (a 7520 and a 7412 as cells; see the controller's wiki, "Limits" for what has been tried). Handover during a call
  needs the boxes to be synchronised, which is unsolved.
- **ULE** (DECT smart-home devices) and **SUOTA** (handset firmware updates):
  ➖.

What is planned next: [ROADMAP.md](../ROADMAP.md).
