# Vendor-specific handsets: a plan, with the Motorola O201 as the first case

**Status: plan, 2026-11-02. Nothing here is built.** Written before the device
arrived; every statement about the O201's behaviour is a hypothesis until the
first discovery run (§5) says otherwise.

## 1. Why this is needed

Handset profiles ([handset-profiles.md](handset-profiles.md)) change **how the
standard is applied** to one model: values from fixed tables. They cannot
express what a handset needs when it is **not** a GAP/CAT-iq handset on the
signalling level, or only partly:

- a **manufacturer-specific message or element** it expects from the base (or
  sends to it) that the standards do not define: <<ESCAPE-TO-PROPRIETARY>>
  (0x7B), vendor use of <<IWU-TO-IWU>> (0x77) or of <<MULTI-DISPLAY>>, vendor
  message types in {CC-INFO}/{FACILITY}/{MM-INFO};
- a **different call or registration sequence** (a step the handset waits for,
  or one it does only when told by a vendor message);
- **vendor lists** (a phonebook or call list reached over a vendor channel, not
  TS 102 527-3 list access).

[journals/handset-compatibility-2026-10.md](journals/handset-compatibility-2026-10.md) concluded in October that
per-device support should wait until "two demonstrably *conformant* handsets
need genuinely different FP behaviour". The Gigasets already needed framing
data (`cc.name.display`); a handset that **registers but cannot call** is the
first case that may need more than data.

### The O201

The Motorola O201 is an IP67 outdoor/rugged cordless handset sold with a
base by Motorola-branded consumer DECT products. Eventphone's compatibility
list ([wiki](https://eventphone.de/doku/dect_phone_compatibility_list)) shows
for the O201 **registration ✅** and every other column ❌ (inbound and outbound
calls, name, number, encryption, phonebook); the same row for the T101L,
T201 and T211, and the S120x and T301 fail even to register. What is **not**
known: why calls fail (a missing vendor message, a codec, ciphering required,
a different release/connect order, or a handset that is not GAP at all), and
what the handset sends and expects. Eventphone's system is a different base
design from ours, so a failure there is evidence about the handset only if the
cause is on the handset side.

Hypotheses, to be tested in this order (cheapest first; each one is a yes/no
the discovery run can answer from one trace):

| # | Hypothesis | If true, what we see |
|---|---|---|
| H1 | It is GAP and works with our standard defaults; Eventphone's base fails for its own reasons | the first ring/dial works with an empty profile |
| H2 | It needs a **specific basic service or codec** (narrowband only, no G.722; or G.722 only) | {CC-RELEASE} or no answer to our {CC-SETUP} with `0x88`; works with `0x80` |
| H3 | It needs **ciphering** before it will carry a call | the handset releases after {CC-CONNECT} or refuses {CC-SETUP} until {CIPHER-REQUEST}; works with `-a` ciphering as for the Gigasets |
| H4 | It needs a **vendor element** in {CC-SETUP} or a vendor {CC-INFO} (e.g. to show the number or to leave "ringing") | it rings but shows nothing / never connects; its own messages carry <<ESCAPE-TO-PROPRIETARY>> we do not answer |
| H5 | It follows a **non-GAP registration or location sequence** after the registration (it works on first attach, then does not locate/page) | page responses missing, location registration repeated or rejected |
| H6 | The handset is built for another **DECT variant** (a 1.9 GHz DECT 6.0 or a vendor-locked base): physically cannot work | no link at all; check the type plate and the EU/ETSI marking first, before any bench time |

## 2. Principles (unchanged from the clean-room rules)

1. **Observe the handset we own, on the air.** Facts come from our own trace
   of a handset we bought. Nothing from a vendor's firmware, disassembly or
   proprietary documentation enters the repository. Public sources (the
   standards, Eventphone's list, the handset's user manual) are cited as such.
2. **The standard stays the default.** A vendor behaviour is only ever applied
   to the handset that needs it, chosen by its profile (the user picks the
   model when registering).
3. **Data first, code last.** Try to express every quirk as profile data. Only
   a behaviour that cannot be data becomes a *vendor module*, small, isolated,
   named after the vendor, and never in the generic call or registration code.
4. **Every fact has a source** in the profile (`# source:`): a trace file
   name, a bench card, a standard's clause.

## 3. What to build, in order

### Step A: see everything (needed whatever the handset does)

1. **Unknown-message logging with raw octets.** For a handset in "learn" mode
   (`profile` key `learn = on`, or `dect-stubd -a learn`), log every NWK message
   it sends, and every message of ours it answers with a release or an error,
   as hex with the link, the transaction, and the time since the last message:
   `learn: DECT 4 -> FP  MM  5d 01 ...`. Unknown message types and unknown or
   proprietary elements are marked. The library already keeps this per link
   (`dect_diag`, [diagnostics.md](diagnostics.md)); the work is a stable text
   format, a `tools/handset-probe.sh` that runs a scripted sequence and
   collects the log, and a way to save it as a **replay trace**.
2. **<<ESCAPE-TO-PROPRIETARY>> and manufacturer codes surfaced.** `dect_nwk`
   already finds elements by id; add a decode of the element's header
   (discriminator, the 16-bit EMC) and report it to the host as an event
   `proprietary hs=N msg=NN emc=XXXX hex=...` (bridge interface, a new
   event), so a controller can log and count it too.
3. **The identity of the handset**: the manufacturer code (EMC) from the
   IPEI, <<MODEL-IDENTIFIER>> and <<TERMINAL-CAPABILITY>>, printed at each
   registration and shown on the controller's handset page (they are partly
   there already). This is also the future auto-suggestion for the profile
   picker.

### Step B: extend profiles with *actions* (data, still no code)

A profile key family that says **what to send when**, with the octets as data
and the same `{text}`/`{len}` placeholders as `cc.name.display`:

```
# send this {CC-INFO} content once the handset is ringing
on.ringing.cc_info = hex:7b 00 05 ...        # source: vendor-probe-2026-11-xx
# answer this unknown message type from the handset with that one
reply.cc.0x5c = hex:...
# accept (ack and ignore) a vendor element we do not parse
accept.ie.0x7b.emc.0x00fa = ignore
```

- Events are **named hook points** the stack already passes through
  (`after_registration`, `after_locate`, `on_setup`, `ringing`, `connected`,
  `release`), fixed in the format spec, not free-form.
- Everything is validated at load (`--check-profile`): known events only,
  octet strings bounded, no loops (an action never triggers another).
- This covers H2 (basic service/codec keys exist), H4 for **fixed** vendor
  elements, and many H5 variants, without a line of vendor code.

### Step C: a vendor-module hook, only if the probe shows state-dependent behaviour

If a handset needs a *conversation* (reply content depends on what it sent,
counters, challenge/response), data is not enough. Then, and only then:

- a small interface in libopendectcell: `struct dect_vendor_ops { emc, name,
  on_message(ctx, link, msg, len, out), on_event(...) }`, registered in a
  table in `src/libopendectcell/vendor/`, **one file per vendor** (`vendor_motorola.c`),
  selected only by a profile key (`vendor.module = motorola`);
- it may only read the handset's messages and queue NWK messages on that
  handset's own link: no access to keys, no other handsets, no chip;
- built into the controller's stack always, and into the OpenWrt package only
  through a Makefile switch (`opendectcell-vendor-<name>` as an optional package
  holding a profile plus the module is **not** possible for a statically linked
  stack, so the standalone image gets the modules that were enabled when it was
  built; the default builds carry none that are experimental);
- fuzzed and unit-tested like the rest (its input is radio data).

### Step D: replay tests

Save each probe run's trace (sanitised: synthetic IPUI, no key material) under
`tests/traces/<vendor>-<model>/`, and a test that feeds the handset's side of
it to the stack and checks our side's messages against the expected ones.
This keeps a vendor behaviour from regressing without the handset on the bench,
and lets a contributor with the same model verify a fix from a trace alone.

## 4. Where the code goes (the "hardware- and vendor-specific only where needed" rule)

| What | Where | Not in |
|---|---|---|
| per-model values and actions | `profiles/<vendor>-<model>.profile`, installed with the package (small data) | the stack |
| vendor modules (Step C only) | `src/libopendectcell/vendor/vendor_<name>.c`, off by default in standalone builds | `dect_base.c`, `dect_cc.c`, `dect_mm.c` |
| the O201's quirks in the controller | none: the controller runs the same stack and reads the same profile | web UI code |
| chip or box specifics | board files and kernel packages (as now) | handset profiles |

## 5. The first discovery run (one session, when the device is here)

Pre-commit the readings before running (the list in §1 is the list):

1. **Before the bench, 10 minutes:** read the type plate and manual: EU DECT
   (1880-1900 MHz) and GAP stated? (H6.) Note the firmware version if shown.
2. Register it on the lab 7520 with the empty profile and **learn mode on**
   (Step A must exist first; until then `dect-stubd` with the existing trace
   options and `dect-maildump`). Save the whole registration conversation.
3. In this order, one run each, the trace saved under a unique name each time:
   location registration after power-cycle; **page + ring** with `0x88`
   (wideband); the same with `0x80`; with ciphering as for the Gigasets; an
   outgoing call (dial a number, expect `call-out`); answer, audio both ways,
   release from each side; the INT list; a display name and number on ring.
4. Compare with the A540 trace of the same run. The first message pair that
   differs is where to look. Fill in the H1-H6 table in this file.
5. If H1: write a normal device profile and stop. If H2/H3: profile keys that
   exist. If H4/H5: Step B. Only if the trace shows a conversation: Step C.

## 6. Order of work, and what can be done without the device

| Now (desk) | When the device is here | After |
|---|---|---|
| Step A.1-A.3: learn mode, probe script, proprietary-element decode and event | §5 | Step B or C as the trace says |
| Step D scaffolding: trace format and replay test harness, with the existing Gigaset traces as the first cases | | profile + replay test for the O201 |
| Ask Eventphone (their helpdesk/Discord/mailing list) what exactly fails for the O201 on their system: nothing on the list says | | |

Nothing in A or D depends on what the O201 turns out to be, so they can be
built first; B and C are decided by the first trace, not before.

## 7. Decision of 2026-11-05: the tool starts on the box, in LuCI

**First evidence (the controller's debug log, 2026-11-05).** The O201 answers our incoming {CC-SETUP} (basic service
0x88, signal 0x41, a calling-party number; our stack starts authentication on the link at the same time) with
{CC-RELEASE-COM} and <<RELEASE-REASON>> 0x0E, "Partial release" (EN 300 175-5 V2.9.1 §7.6.7), about 380 ms after the
SETUP and before it alerts. Three attempts, the same each time. Its outgoing calls work, narrowband and ciphered. It
reports no terminal capability or model. H2 (wideband refused) is **not** confirmed: it would normally be 0x05 or
0x06. The reason names no cause, so the cause is found by varying what we send.

**Decision (the maintainer).** The tool is a **LuCI page on the box**, for the standalone box and for a box that is a
cell. People who want to support a handset have a second FRITZ!Box, or can take theirs off the controller for an
hour; so the tool **suspends cell mode without un-enrolling** and resumes it afterwards. Nothing is erased: the
controller's key, certificate and enrolment stay on the box.

### 7.1 What exists already (do not build twice)

- `dect-stubd -a basicsvc=N | signal=N | clip=NUMBER | cnip=NAME | dialwait=MS | bindfirst | nonlr | allcallstatus`
  set the lab variations of the call we send, per daemon start. They are the variants of the first sequence.
- The control socket (`DECT_CTL_PATH`): verbs `bridge` (with `call-in line=1 hs=N number=... name=...`),
  `register-window`, `reload-profiles`. `dect-stubd --bridge-echo ring` rings a handset from the shell.
- The profile format and its resolution (handset-profiles.md); profiles are read from `/etc/opendectcell/profiles/` and
  re-read on SIGHUP or `reload-profiles`.
- `dect_diag` per link; the debug log shows every 5-octet Lc frame of a link (`chip mail content` / `chip mail down`),
  which is how the O201's release was read; nothing decodes it for the reader yet.
- LuCI: `luci-app-opendectcell` with `settings.js` (Network > DECT) and `monitor.js` (Status > DECT).

### 7.2 Pieces

1. **A `probe` verb on the control socket** (libopendectcell + stubd): `probe call hs=N basicsvc=.. signal=.. clip=none|number|display
   cnip=none|.. auth=with|after-alert|none dialwait=..`: rings the handset with **this attempt's** variation, without
   restarting the daemon (a restart drops the handset's lock for seconds and some handsets, as seen with the AS690H,
   need more than that), and answers with a **decoded result event**: `probe-result outcome=alerting|connected|
   released|no-answer reason=0x0E name="partial release" by=handset after_ms=380 handset_msgs=[CC-RELEASE-COM ...]`.
2. **Trace capture** per attempt: every Lc frame in both directions with the time, reassembled into NWK messages and
   decoded (message type, element ids, release reason names), as text; the raw hex stays next to it. Keys and the
   IPUI are not written (synthetic IPUI).
3. **A runner** (`opendectcell-probe`, ucode) that does the sequence (§7.4), keeps its state in a file the LuCI page reads
   over rpcd, takes the human's answers, and writes the result bundle under `/etc/opendectcell/probe/<model>-<date>/`.
4. **Suspension** (cell mode only): stop the link service (`opendectcell-cell`), start the box's own stack for the probe,
   and **resume on every way out**: the end of the run, the Cancel button, a 60-minute limit, and a reboot (a flag file
   the init script reads at boot and clears, so a power cut in the middle never leaves a cell suspended).
5. **The LuCI page** (a tab next to the DECT Monitor: *DECT Command Finder*): the warning, the handset picker, the steps with
   live results, the questions ("did it ring?", "what does the display show?", "do you hear the other side?"), then
   **Download profile**, **Download trace bundle**, **Save to this box** (writes the profile and sets the handset's
   model).
6. **A profile writer**: maps conclusions to **existing keys only**; what no key can express is written as a comment in
   the profile (and the trace says where), never as an invented key.

### 7.3 What the user has to know (shown on the page before anything starts)

- **Cell mode: this box stops serving the controller for the duration.** Every handset on this cell loses coverage
  from this cell, calls on it end; the controller shows the cell offline and other cells may take the handsets. The
  run is 20 to 40 minutes; it ends by itself after 60.
- **The handset must be registered on this box for the run** (the box's own base, with its own identity: the page
  shows the registration window). A handset that holds **one** subscription only gives up its other registration
  when it takes this one: the page says to register it again on its real base afterwards, and offers to deregister
  the probe registration at the end. We do not know how many subscriptions the O201 holds; the first run finds out
  (register on the box while it is registered on the controller, and look at both).
- A probe can leave a handset confused (a restart of the handset cures it, as with the AS690H); it cannot damage it.
- The bundle contains no keys; it does contain the handset's manufacturer code and the messages it sent.

### 7.4 The first sequence (pre-committed; each line says what it would mean)

Run in this order, one incoming call each, narrowband first because it is what the O201 does outgoing; the run
stops asking about a variant as soon as an earlier one rings and records the later ones as "not needed".

| # | We send | If it rings | If it releases with ... |
|---|---|---|---|
| 1 | the controller's own SETUP (control: reproduce the failure on the box) | the controller's stack or the radio path is the difference | the same 0x0E: reproduced on the box |
| 2 | basic service 0x80 | H2: narrowband only; profile `cc.basic_service = basic` | another reason: note it |
| 3 | 2 without the calling-party number | the number element is refused | |
| 4 | 3 with signal 0x40 | the ring pattern is refused | |
| 5 | 4 with authentication only after the handset alerts (or none) | a parallel MM transaction is not handled | |
| 6 | 5 with {CALL-INFORMATION} (Part 3) | it wants it | |
| 7 | none of these: it still releases | go to Step C (what does *it* send first?) | |

Also in the run: location registration after a power cycle (the handset's), an outgoing call (audio delay as measured
from the dial pause: `dialwait` 0 and 3000), answer and audio both ways, release from each side.

### 7.5 Slices

S1 the `probe` verb, the decoded result and the frame decoder, with a bench command (`dect-stubd --probe ...`) and
unit tests (the O201's own frames from 2026-11-05 as the first decoder test); S2 the runner and the suspension with its
fail-safes (tested with a fake daemon); S3 the LuCI page; S4 the profile writer and the bundle; S5 (later) the same
sequence run **by the controller** for cell users who do not want to suspend anything.

### 7.6 Decisions of the same day: the 7412 is the development box, and the identity is always random

- **Development box: the FRITZ!Box 7412** (192.168.1.124, OpenWrt 25.12.6, lantiq/xrx200, packages 0.3.3, a cell of the
  lab controller). It stays enrolled; the tool suspends it (§7.2 piece 4). Code that runs on the PC is tested with
  `make check` first; the 7412 needs the MIPS build (`tools/release/build.sh lantiq/xrx200 OUT`, which downloads the
  OpenWrt SDK once).
- **The probe always runs under a random identity of its own**, drawn for each session: a class B identity with a made-up
  EIC, FPN, FPS and RPN 0, applied by a chip reload when the session starts and taken away when it ends (the box then has
  its own identity again: the factory one, or the controller's in cell mode). Two reasons:
  (A) the handset's **registration is captured from the first frame**, under an identity nobody else uses, so no
  stale subscription can hide or change what the handset does; (B) a subscription made under a throwaway identity
  **cannot collide with** or be mistaken for the identity the box or the controller uses later, in stand-alone or in
  controller mode. The price: the handset keeps a leftover entry under a name it cannot show; the page tells the user to
  delete that base on the handset when the run is over. The identity is in the bundle (it is made up and of no use
  afterwards).

## 7.7 The first manual run, 2026-11-05 (lab 7412, random identity 10729a3900, O201 as DECT 4)

Done by hand with the existing lab options; the Finder's own sequence is not built. The run is the template for it.

**Incoming calls: solved.**

| # | We sent | Result |
|---|---|---|
| 1 | normal setup, basic service 0x88 (the control) | reproduced on the box: the O201 answers with {CC-RELEASE-COM}, reason 0x0E, after about 380 ms, never alerting |
| 2 | basic service 0x80 (`-a basicsvc=0x80`) | authenticated, ciphered, **rings**, answers, narrowband audio flows both ways (H2 confirmed) |
| 3 | the profile `motorola-o201` alone (`cc.basic_service = basic`), no option | **rings**: the profile does the job |
| 4 | display | the number and the handset's own "Externer Anruf"; the name we send is not shown |

Variants 3 to 6 of §7.4 were not needed. Reason 0x0E ("partial release") is simply how the O201 ends **every** call, also a normal hang-up (its {CC-RELEASE}
carries it too), so the reason alone never named a cause: the variation did. The handset reports **GAP only** in its <<TERMINAL-CAPABILITY>> and puts a vendor
element, <<ESCAPE-TO-PROPRIETARY>> `7b 02 80 04`, in every outgoing {CC-SETUP}; we ignore it. Profile files: `profiles/motorola*.profile` (`887051d`).

**Outgoing calls: the digits and the setup work, the audio does not, and the cause is not found.**

- The O201 sends its digits in a {CC-INFO} keypad ("123"), no <<SENDING-COMPLETE>>; the base waits `dialwait` (3000 ms), then CALL-PROC, ALERTING, and CONNECT
  when the far end answers. Every handset has the same 3 s pause.
- **What was measured** (the call-end log line of `68da86d`, `from the handset X of Y frames with signal`): the radio link is clean in every call (quality 80,
  no CRC or sync errors), frames flow both ways with no underruns, and the base feeds the downlink all the time (0 of 8000 frames idle-filled). The O201's
  microphone signal reached us in **one call of about ten** (peak 26692, then peak 6280 in a later one); in the others it was silence (peak 258).
- **What the user heard**: the first call of the series, an echo after 10 s; the next two calls, nothing; later, counted beeps (`DECT_ECHO_TONE=1`):
  nothing in 40 s; nothing in a second call; once **at a call timer of 6 to 8 s**; and "none" with `bindfirst`, "none" with ciphering off (`-C`).
- **Ruled out by those runs:** the radio link, the audio flow through the stack, ciphering (off: no change), the order of bind and CONNECT (`bindfirst`: no
  change), the codec width (always narrowband), the dial pause.
- **Still open:** (a) the handset: its receive path opens at an unpredictable time after our CONNECT, or never, and its transmit path likewise
  (the spec lets a PT connect its receive U-plane late, §9.3.1.4); (b) **the 7412's PCM transport**: the stub's TDM lanes move during calls (`ssc: watch:
  LANES MOVED`) and the controller's early 7412 bench showed three Gigasets with working audio, so a comparison on the same box is needed; (c) a missing message
  the O201 waits for: a <<PROGRESS-INDICATOR>> "in-band information available" (§9.3.1.4) is not implemented in libopendectcell, nor is the vendor escape element.
- **Next, in this order:** 1. the same dial test with a **Gigaset registered on the same box** (the control handset: does it get audio at once on this box
  and identity?); 2. the O201 on the lab **7520** (same test: box or handset?); 3. only then a <<PROGRESS-INDICATOR>> variant (a library change), and the
  vendor element, from a trace of what the handset sends.

**What the Finder must therefore do** (additions to §7.2): measure each direction's level automatically (done in `68da86d`) and ask the human only
what no instrument can tell; run each variant more than once (a single trial would have said "works" on the first O201 call and "broken" on the next);
write `source =` lines of at most 128 characters (the parser's limit; details go in `# source:` comments).
