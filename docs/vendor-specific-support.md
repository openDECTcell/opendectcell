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
