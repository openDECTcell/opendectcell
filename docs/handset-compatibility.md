# Handset compatibility

What each handset has been seen to do against openDECTcell, as of 2026-11-03. It is a reference for users ("will my
handset work, and what will it not do?") and for developers ("is this a quirk of the handset or a gap in our
stack?"). The short list is in [status.md](status.md#handsets); how October's findings were made is in
[journals/handset-compatibility-2026-10.md](journals/handset-compatibility-2026-10.md).

Every row is something seen on a real handset against a real box (`[observed]`, with the date), or it says what is
built but not seen. A blank is not a "no": it means nobody has looked. Please
[report what yours does](../CONTRIBUTING.md#reporting-a-handset-or-a-box).

## The tested handsets

| | Gigaset A540 CAT | Gigaset C430A | Gigaset AS690H | AVM FRITZ!Fon C4 |
|---|---|---|---|---|
| Standard | CAT-iq, NG-DECT Part 3 | GAP, NG-DECT Part 1 | GAP, NG-DECT Part 1 | CAT-iq |
| Profile | `gigaset-a540` | `gigaset-c430a` | `gigaset-as690h` | none |
| Registration with a PIN | yes; tries the PIN `0000` by itself first (profile) | yes | yes | yes (during development) |
| Wrong PIN | | | "PIN ungültig" on the handset, 2026-10-28 | |
| Calls in and out over SIP | yes | yes | yes | yes (during development) |
| HD voice (G.722) | yes | yes | yes | yes (during development) |
| Ciphering | yes | yes | yes | |
| Caller number on the display | yes, with line name ("via Line 1 …") | yes | yes | yes |
| Caller name | | only as text drawn by the base: a separate screen, then the ringer screen with the number | the same | |
| Call timer | at once (needs call status, sent to Part 3 only) | appears by itself after a few seconds | | |
| Date and time from the base | yes, after a reboot | no (Part 1 has no such setting) | no | |
| Phonebook from the base | yes (a 388-contact CardDAV book read, 2026-10-30); edits from the handset not yet seen on air | no (its own list only) | no | |
| Call lists | missed calls listed and marked read, 2026-10-30; accepted and outgoing not yet checked on air | no | no | |
| Internal Names list (INT) | yes, also for handsets on other cells (2026-11-02) | | **none**: the number is typed after INT | |
| Internal call, how the caller shows | "via Intern DECT" before the caller's name was sent in a `{CC-INFO}` (2026-10-28); not re-checked | shows "1" | rang for 29 s and sent nothing until the caller hung up: its red key only silences the ringer | |
| Presence when switched off in range | goes **Offline at once** (2026-11-03) | not tried | goes **Offline at once** (2026-11-03) | |
| Battery pulled | "Unresponsive", then Offline after the roaming interval and 10 minutes (2026-11-03) | | | |
| Deregister from the handset's own menu | does not work yet ([status.md](status.md)) | only a handset reset offered | only a handset reset offered | |
| Deregister from the base (web page) | yes, handset on and in range | | yes | |
| Call waiting, hold, second call | built for CAT-iq handsets, not yet tried | "busy" for a second call | "busy" for a second call | |

## What is special about each, and why

These are the few places where a handset needed something other than plain standard behaviour. Each is a profile
key, so it is data and not code ([handset-profiles.md](handset-profiles.md)).

| Handset | The habit | Profile |
|---|---|---|
| **A540** | **Rings aloud only with the full `<<CALL-INFORMATION>>`** (line, line type, call identifier, call status); a bare line id gives a silent ring. That is the standard, so it is the default and needs no key. | none |
| A540 | Tries the PIN `0000` before the one the user entered. | `reg.tries_0000_first = yes` |
| A540 | After its location registration the list change notices make it read the List of Supported Lists and give up the list session, so it never reads the Internal Names list and keeps no name. Without the notices it reads the lists by itself. | `notify = off` |
| **C430A**, **AS690H** | A caller's name shows only as display text the base draws (a Gigaset display framing), not from `<<CALLING-PARTY-NAME>>`. With stock's two-line framing the C430A showed the number line, not the name (unconfirmed). | `cc.name.method = both`, `cc.name.display` (C430A) |
| C430A, AS690H | Part 1: they must not get call status (the standard forbids it), and they have no names list. | the default for Part 1 |

## What we learned about reading these results

- **Most differences were our own non-conformance.** In October every difference that looked like a vendor quirk
  (the A540 refusing calls, the silent ring) turned out to be something we did not send the way the standard
  says; the FRITZ!Fon C4 tolerated it and the Gigasets did not. That is why the library implements the standard
  and the per-handset exceptions are small, data-only profiles. The reasoning and the runs are in
  [journals/handset-compatibility-2026-10.md](journals/handset-compatibility-2026-10.md).
- **Check the standard first.** A handset that "does not work" is a gap in our stack until a second, conformant
  handset needs genuinely different behaviour. Only then does it become a profile key.
- **Part 1 and Part 3 handsets differ in what they can show at all** (names list, call status, date and time,
  phonebook), not in whether they work. A missing feature on a Part 1 handset is usually the handset.

## Handsets that are not plain GAP or CAT-iq

Some handsets register but do not do more. They may be strict about things the standard leaves optional, or use
manufacturer-specific commands. [vendor-specific-support.md](vendor-specific-support.md) is the plan for finding out
what they need, and [handset-variation-catalog.md](handset-variation-catalog.md) lists every variation of the basic
functions that the standards permit, with what we can send today.

### The Motorola O201

Seen 2026-11-05 on a standalone 7412 under a random identity, and registered with the controller. It is GAP only and
narrowband only; it reports no terminal capability or model. Three things were needed, and each is data now:

| What it does | Why | Profile key |
|---|---|---|
| Releases an incoming {CC-SETUP} with basic service 0x88 (wideband) after about 380 ms, <<RELEASE-REASON>> 0x0E ("partial release", EN 300 175-5 §7.6.7); rings on 0x80 | it refuses the wideband setup | `cc.basic_service = basic` |
| Plays no audio on a call it places until our first answer carries <<PROGRESS-INDICATOR>> "in-band information available" (cause 8); audio at once with it (3 of 3 calls) | EN 300 175-5 §9.3.1.4 lets a PT keep its receive path closed without it | `cc.progress_in_band = yes` (API 159) |
| Ends every call with reason 0x0E, whatever the reason | generic for this handset, not a fault | none |

Not needed (tried and ruled out): a CONNECT-ACK variant, the vendor element `7b 02 80 04` it sends in its SETUP. Open: its
display shows the number and its own "Externer Anruf", not the caller's name; it waits about 3 s after the last digit
(`dialwait`, the same for every handset). It keeps a base entry for each identity it registered with; delete the unused one in its menu.
The run that found this: [vendor-specific-support.md §7.7](vendor-specific-support.md).

## Adding a handset

1. Register it, call in and out, and note what you see (the report form in [../CONTRIBUTING.md](../CONTRIBUTING.md)).
2. If something needs special treatment, write a profile file ([handset-profiles.md](handset-profiles.md),
   [../profiles/README.md](../profiles/README.md)) and check it with `dect-stubd --check-profile FILE`.
3. Add a column here with what you observed, each cell dated and from a real handset.
