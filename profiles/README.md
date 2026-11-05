# profiles/: handset profiles

A handset profile says **how the standard is applied to one handset model**:
send this information element or omit it, show a caller's name as display
text, release the link after a call or keep it. A profile holds values only
(words, numbers, strings, hex octets); it never adds code or a protocol of its
own. openDECTcell implements GAP and CAT-iq, and most handsets need no profile at
all. Profiles exist for the few quirks the bench found.

These are not the **board** profiles of [../boards/](../boards/README.md),
which describe FRITZ!Box models.

The full format, every key with its values and default, the resolution rules
and the rules for writing a profile are in
[../docs/handset-profiles.md](../docs/handset-profiles.md). This page does not
repeat the key list. Vendor behaviour that needs more than values is covered by
[../docs/vendor-specific-support.md](../docs/vendor-specific-support.md).

## Files

| File | Kind | What it sets |
|---|---|---|
| [gigaset.profile](gigaset.profile) | manufacturer | Nothing yet: the common layer the Gigaset devices extend |
| [gigaset-a540.profile](gigaset-a540.profile) | device (NG-DECT Part 3) | `reg.tries_0000_first = yes` (it tries PIN 0000 first), `notify = off` (see the comment in the file) |
| [gigaset-c430a.profile](gigaset-c430a.profile) | device (NG-DECT Part 1) | `cc.name.method = both`, `cc.name.display` (the vendor's display-text framing as hex) |
| [gigaset-as690h.profile](gigaset-as690h.profile) | device (NG-DECT Part 1) | `cc.name.method = both` |
| [motorola.profile](motorola.profile) | manufacturer | Nothing yet: the common layer the Motorola devices extend |
| [motorola-o201.profile](motorola-o201.profile) | device (GAP only, narrowband only) | `cc.basic_service = basic` (it refuses a wideband 0x88 setup and rings on 0x80) and `cc.progress_in_band = yes` (it plays no audio on outgoing calls without the in-band indication). The first profile from a DECT Command Finder run |

## Format, by example

A device file extends a manufacturer file. This is `gigaset-as690h.profile`
as it is in the repository:

```
# Gigaset AS690H (NG-DECT Part 1).
format = 1
id = gigaset-as690h
kind = device
extends = gigaset
name = "Gigaset AS690H"
source = "bench 2026-10-28: test 1 (no name from <<CALLING-PARTY-NAME>>)"
# source: test 1 -- like the C430A, names only as display text
cc.name.method = both
```

- `key = value`, one per line, `#` starts a comment. Values are a word, an
  integer (decimal or `0x`), a double-quoted string, or `hex:` octets, which
  may contain the placeholders `{text}` and `{len}` (see
  `gigaset-c430a.profile`).
- The header keys are `format`, `id`, `kind` (`manufacturer` or `device`),
  `extends` (device files), `name`, `source`, and optionally `match.model`.
  `id` is also the file name without `.profile`. A file without `source` is
  rejected.
- Behaviour keys use dotted lower-case names (`cc.name.method`, `notify`). A key
  may appear once per section; a device file may add `[firmware A .. B]`
  sections for firmware ranges.
- The more specific layer wins: firmware section, then device, then
  manufacturer, then the standard's defaults. What the handset itself
  reports wins over a profile unless `trust.terminal_capability = no`.
- Put the evidence for each behaviour key in a `# source: ...` comment on its
  own line: a bench log, a clean-room answer (`[CR A-NNNN]`) or a clause of
  the standard.

## Where profiles go on a box

The package installs every `profiles/*.profile` into `/etc/opendectcell/profiles/`
(the directory is a conffile, so edits survive upgrades). `dect-stubd` reads
all files there at startup and again on `SIGHUP`; a reload applies to the next
registration, location registration or call of a handset, not to one in
progress. The user picks a model for each handset (LuCI, before registering
it); the daemon never chooses one automatically. `-a profiles=DIR` points the
daemon at another directory.

## Testing a profile

`dect-stubd --check-profile FILE [FIRMWARE]` reads one file without touching the
radio. It also loads the manufacturer file named by `extends`, which must sit
in the same directory as `<extends>.profile`. It prints either the errors (exit
status 1) or the resolved keys (exit status 0). Run from a checkout, with a
host build (`build/dect-stubd` after `make build/dect-stubd`):

```
build/dect-stubd --check-profile profiles/gigaset-c430a.profile
profile: profiles/gigaset-c430a.profile: gigaset-c430a "Gigaset C430A" (via gigaset), 0 firmware section(s)
profile: resolved: cc.name.method=both cc.name.display=(11 octets)
```

Pass a firmware version as the second argument to see a `[firmware]` section
applied. `dect-stubd --profile-keys` prints the key table as JSON (what the
LuCI page reads). The parser and resolver are in
`src/libopendectcell/dect_profile.c` and are covered by
[../tests/test_profile.c](../tests/test_profile.c) (`make check`).

On a box: copy the file to `/etc/opendectcell/profiles/`, `kill -HUP` the daemon
(`--check-profile` first), then run the test as a bench card. The log line
`profile: DECT n = <id> (via <manufacturer>): key=value ...` says which profile
took effect and which keys differ from the defaults. A key the build does not
act on yet is reported in the log.

## Adding a profile

1. Name the handset and find out what differs from the standard (a bench log
   or a capture). If nothing does, no profile is needed.
2. Pick the manufacturer layer, or add `<vendor>.profile` with
   `kind = manufacturer` if there is none. Keep manufacturer files to what
   the vendor's handsets share.
3. Create `<vendor>-<model>.profile` with `kind = device` and
   `extends = <vendor>`. `id` must equal the file name stem; use lower case,
   digits and `-`.
4. Set only keys from [../docs/handset-profiles.md](../docs/handset-profiles.md)
   section 5, each with a `# source:` comment; add the `source` header.
5. `dect-stubd --check-profile profiles/<file>` must exit 0 and print what you
   expect. If a behaviour needs a key that does not exist, that is a code
   change first, not a profile.
6. Record the handset's result in
   [../docs/handset-compatibility.md](../docs/handset-compatibility.md).
7. The package picks up new files in `profiles/` by wildcard, so no
   Makefile change is needed.
