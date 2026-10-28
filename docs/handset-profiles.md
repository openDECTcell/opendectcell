# Handset profiles: the file format

**Status (2026-10-28): format defined, loader not built yet.** This page is the
contract for whoever writes a profile: the maintainer, a contributor, or the
clean-room agent that turns facts about a vendor's handset into a file. The
loader in `dect-stubd` will implement exactly this page; every key below names
the behaviour it replaces today (a `dect-stubd -a` lab option or a built-in
default), so a profile can be tried by hand now with that option.

Decisions behind it: ROADMAP M8b, "Decided 2026-10-28", points 1-7, and
AGENTS.md §1a and §2.9.

## 1. What a profile is for

openDECTcell implements the standards (EN 300 444 GAP, TS 102 527 CAT-iq). A
profile changes **how the standard is applied to one handset model**: send
this element, omit that one, show a name as display text, release the link
or keep it. It never adds a protocol of its own.

- **Quirks are data, features are code.** A profile sets values from the
  tables below. It cannot add code: feature modules (call lists, contacts,
  call waiting) are part of openDECTcell, and a profile can only switch one on or
  off.
- **The more specific layer wins:** the device file over its manufacturer
  file over the standard defaults, and a matching firmware section over the
  rest of the device file.
- **What the handset reports about itself wins** over a profile, unless the
  profile says that report is wrong (`trust.terminal_capability = no`).
- **One format** for the cell, the standalone box and the controller.

## 2. Where files live, and how one is chosen

- One file per profile: `/etc/opendectcell/profiles/<id>.profile`, UTF-8.
- `dect-stubd` reads every file there at startup and again on `SIGHUP`. A
  reload applies to the next registration, location registration or call of
  a handset, never to a call in progress.
- **The user picks the model** for each handset, before registering it
  (decision 1): the choice is stored with the handset and names a device
  profile `id`. That way registration quirks apply too. Without a choice the
  handset gets the standard defaults. `match.model` (below) is only ever a
  suggestion shown to the user, never an automatic choice.
- `dect-stubd --check-profile FILE` reads one file without touching the radio
  and prints either the errors or the resolved key list (exit status 0 or 1).

## 3. Syntax

```
# a comment: from '#' to the end of the line
key = value
[firmware 42.010 .. 44.999]
key = value            # applies only to that firmware range
```

- One `key = value` per line; spaces around `=` are ignored; lines up to 255
  octets.
- **Values** are one of:
  - a **word** from the key's list (`on`, `off`, `auto`, `display` ...);
  - an **integer**, decimal or `0x` hex (`0x41`);
  - a **string** in double quotes, `\"` and `\\` as escapes;
  - **hex octets**, `hex:` then pairs of hex digits, spaces allowed
    (`hex:28 09 57 65`). A hex value may hold the placeholders `{text}` (the
    text, encoded as the key says) and `{len}` (one octet, the length of
    `{text}`), where a key allows them.
- Keys are lower case, dotted (`cc.name.method`). A key may appear once per
  section; a repeat is an error.
- **Sections:** only `[firmware A .. B]`, inclusive, in device files. Versions
  are dotted numbers compared field by field as integers (`42.10` is after
  `42.9`). A handset's firmware version is what the user entered with the
  model; no handset we know reports it in a message we parse.

## 4. Header keys (every file)

| key | values | required | meaning |
|---|---|---|---|
| `format` | `1` | yes | this page's format version |
| `id` | word: `a-z0-9-` | yes | unique, also the file name |
| `kind` | `manufacturer`, `device` | yes | the layer |
| `extends` | a manufacturer `id` | device files | the manufacturer layer under it |
| `name` | string | yes | what the user picks from, e.g. `"Gigaset C430A"` |
| `match.model` | hex | no | the handset's <<MODEL-IDENTIFIER>> value (EN 300 175-5 §7.7.30), to suggest this profile |
| `source` | string | yes | where the facts come from: a bench log name, a capture, a clean-room answer (`[CR A-NNNN]`), a standard's clause |

Every behaviour key should also say where it comes from in a comment on its
own line, `# source: ...`. A file without `source` is rejected.

## 5. Behaviour keys

"Today" names the lab option or default the key replaces. **Default** is the
standard behaviour, what a handset without a profile gets.

### 5.1 Call control

| key | values | default | effect | today |
|---|---|---|---|---|
| `cc.call_status` | `auto`, `on`, `off` | `auto` | <<CALL-INFORMATION>> call status in our CC messages. `auto`: only to a handset reporting NG-DECT Part 3 (TS 102 527-3 §7.4.6.4.1) | `-a allcallstatus` = `on`, `-a nocallstatus` = `off` |
| `cc.ring_callinfo` | `auto`, `full`, `line`, `none` | `auto` | the <<CALL-INFORMATION>> in our incoming {CC-SETUP}: `full` = line, line type, call id, CS call setup (TS 102 527-3 figure 42); `line` = the line only; `auto` = `full` to Part 3, `none` otherwise | `-a lineid=,linetype=,callid=,callstatus=` |
| `cc.connect_status_info` | `auto`, `on`, `off` | `auto` | "CS call connect" in a {CC-INFO} after the handset answers (§7.4.6.4.3); `auto` = with call status | built in since `2c76ace` |
| `cc.basic_service` | `auto`, `wideband`, `basic` | `auto` | <<BASIC-SERVICE>> of our {CC-SETUP}: 88H or 80H. `auto`: 88H when the handset offered G.722, else 80H (EN 300 444 table 21) | `-a basicsvc=`, `-a narrowband` |
| `cc.signal.external` | `0x40`..`0x48` | `0x41` | <<SIGNAL>> alerting pattern of an external call (EN 300 444 table 23) | `-a signal=` |
| `cc.signal.internal` | `0x40`..`0x48` | `0x40` | the same for an internal call | built in |
| `cc.bind_before_connect` | `yes`, `no` | `no` | bind the audio before our {CC-CONNECT} (EN 300 175-5 §9.3.1.8) | `-a bindfirst` |
| `cc.name.method` | `cnip`, `display`, `both`, `none` | `cnip` | how a caller's name reaches the handset: <<CALLING-PARTY-NAME>> in the {CC-SETUP} (EN 300 444 §8.42), and/or display text in a {CC-INFO} <<MULTI-DISPLAY>> once the handset rings | `-a cnip=` and `-a infoie=` |
| `cc.name.display` | hex with `{text}`, `{len}` | `hex:{text}` | the **contents** of the <<MULTI-DISPLAY>> element (tag 28H and its length are added) for `display`: a vendor's framing goes here as data. `{text}` is the name, else the number, in the DECT alphabet | `-a infoie=` |
| `cc.name.alphabet` | `auto`, `utf8`, `dect` | `auto` | <<CALLING-PARTY-NAME>> alphabet: `auto` = UTF-8 to Part 3, DECT (IA5) otherwise (TS 102 527-3 §7.4.17) | `-a cnipalpha=` |
| `cc.timer` | `handset`, `base` | `handset` | `base`: the base draws the call timer as display text once a second after the answer, as stock does for some Gigasets ([CR A-0125]) | planned |
| `cc.number.method` | `clip`, `display`, `both` | `clip` | the caller's number: <<CALLING-PARTY-NUMBER>>, and/or display text as for names | `-a clip=` |

### 5.2 Links, registration, lists

| key | values | default | effect | today |
|---|---|---|---|---|
| `lce.release_after_call` | `yes`, `no` | `yes` | release the link after its last call or list session (EN 300 175-5 §14.2.7) | `-a nonlr` = `no` |
| `reg.tries_0000_first` | `yes`, `no` | `no` | the handset tries PIN 0000 by itself before asking the user; the refused try is logged as expected, not as an attack | log text only |
| `lists.serve` | `yes`, `no` | `yes` | answer list access (TS 102 527-3 §7.4.10); `no` = every session "list not supported" | built in |
| `line.selection` | `auto`, `fp`, `hash` | `auto` | how a handset picks a line: `auto` = its <<CALL-INFORMATION>> (Part 3), else the FP's default line; `hash` = `#` + digit before the number (§7.4.5.2.3) | planned |
| `notify` | `auto`, `on`, `off` | `auto` | events notifications ({FACILITY} <<EVENTS-NOTIFICATION>>, §7.4.1); `auto` = to Part 3 handsets | built in since `80d40b7` |

### 5.3 What the handset reports

| key | values | default | effect |
|---|---|---|---|
| `trust.terminal_capability` | `yes`, `no` | `yes` | `no`: ignore the handset's <<TERMINAL-CAPABILITY>> profile bits and use `cap.ngdect` instead |
| `cap.ngdect` | `gap`, `part1`, `part3` | (from the handset) | the handset's CAT-iq level, when it reports it wrongly |

### 5.4 Feature switches

| key | values | default | module |
|---|---|---|---|
| `feature.call_waiting` | `on`, `off` | `on` | call waiting and parallel calls (lines-and-calls.md steps 5-6), Part 3 only |
| `feature.call_lists` | `on`, `off` | `on` | missed, incoming, outgoing and all calls lists (MVP-2, later) |
| `feature.contacts` | `on`, `off` | `on` | the contact list from the vCard source (MVP-3, later) |

A switch for a module that is not built yet is accepted and has no effect.

## 6. Resolution, validation, logging

1. Start from the defaults above.
2. Apply the manufacturer file named by `extends`, then the device file, then
   every `[firmware ..]` section of the device file whose range holds the
   handset's firmware version, in file order. The last value of a key wins.
3. Then what the handset reports, unless `trust.terminal_capability = no`.

- An **unknown key** is reported (`profile: <file>:<line>: unknown key ...`)
  and ignored, so an older openDECTcell reads a newer file.
- A **bad value** is reported and that key ignored.
- A missing required header key, a duplicate `id`, an `extends` that names no
  manufacturer file, or a syntax error rejects the whole file; the handset
  then gets its manufacturer layer, or the defaults.
- When a profile takes effect the log says which, and each key that differs
  from the default, once:
  `profile: DECT 2 = gigaset-c430a (via gigaset): cc.name.method=display`.

## 7. Rules for the clean-room agent

- A profile holds **values and octet strings only**: never code, never a
  vendor's text beyond the octets the handset must receive. A framing taken
  from a capture goes into `cc.name.display` as hex with placeholders.
- Cite every fact: `source` for the file, `# source:` per key, with the log
  name, the capture or the `[CR A-NNNN]` answer. Standards-backed defaults
  need no profile at all.
- Prefer the narrowest layer: a quirk seen on one model goes in its device
  file, not in the manufacturer file, until a second model shows it.
- Test by copying the file to `/etc/opendectcell/profiles/`, `kill -HUP` the
  daemon (or `--check-profile` first), and the bench card's run.

## 8. Examples (from the 2026-10-28 bench)

`gigaset.profile`, the manufacturer layer:

```
format = 1
id = gigaset
kind = manufacturer
name = "Gigaset"
source = "bench 2026-10-28, docs/bench-next.md results table"
```

`gigaset-c430a.profile`:

```
format = 1
id = gigaset-c430a
kind = device
extends = gigaset
name = "Gigaset C430A"
source = "bench 2026-10-28: vRINGC430A-20261028-095744-1; [CR A-0125]"
# source: test 1 -- the name shows only as base-drawn display text
cc.name.method = both
# source: [CR A-0125] section 5, stock's framing, as bench test S3b sent it
#   (`infoie=2811020a1a910202095765646e65736461790d` for "Wednesday")
cc.name.display = hex:02 0a 1a 91 02 02 {len} {text} 0d
```

(Test 1 put the number line, not the name, on the C430A with this framing,
so the file is an example of the syntax until the bench confirms it.)

`gigaset-a540.profile`: none is needed. Everything the A540 required on the
bench turned out to be the standard (TI 0, <<SIGNAL>>, the full
<<CALL-INFORMATION>> to a Part 3 handset), which is the default. Its PIN
habit is the one quirk:

```
format = 1
id = gigaset-a540
kind = device
extends = gigaset
name = "Gigaset A540"
source = "bench 2026-10-28 test 20, RES1 matched PIN 0000"
reg.tries_0000_first = yes
```

## 9. Open points

- Where the user's choice is stored: a UCI `handset` section per store
  (`option profile`), written by the LuCI page before registration opens.
- The display-text keys need the real Gigaset framing from `[CR A-0125]` §5,
  re-checked on the bench (test 1 showed the two-line framing put the number
  line, not the name, on the C430A).
- `cc.timer = base` and `line.selection = hash` need code first.
