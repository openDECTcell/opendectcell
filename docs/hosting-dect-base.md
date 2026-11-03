# Hosting `struct dect_base`: a guide for a second host

Written 2026-10-30 for the controller's C3 (opendectcell-controller ADR 0002,
option B), at libopendectcell **API 137**. `dect-stubd` is the first host of the
base stack; this page says what a second host, the controller hosting one
`dect_base` per cell over the mail tunnel, has to provide, and what
`dect-stubd` does in each place so you can read the reference.

The record of the move, stage by stage, is
[stack-in-library.md](stack-in-library.md); every public change is in
[api-versions.md](api-versions.md); the header is
`src/libopendectcell/dect_base.h`, which is the authority where this page and it
differ.

## 1. What the library does, and what stays with the host

| The library (`dect_base`) | The host |
|---|---|
| Handset records: identity, key, slot, names, profiles, capabilities | Where records are stored (`save()`), and loading them at start |
| Registration, locate, authentication, ciphering, deregistration | The registration window's policy: when it is open, the PIN (`cfg.pin`) |
| The chip's slot table: 0x0247, 0x020A, 0x017C, the read-back and the audit | Which records go into the chip (`install()`), and which STUB build is loaded (`chip.uak_table`) |
| Links: the main link, side links, adoption, the C-plane segments in | Taking frames out to the chip when the transport has room (`pump()`) |
| Calls: ring, page, call out, dialling, internal calls, side rings, parallel calls, call waiting, the bridge's verbs | The SIP side: the bridge events (`event()`) and the verbs back (`dect_base_ctl_call()`) |
| Lists and notices: names, lines, settings, contacts and call lists over the list access service, MWI, missed calls, the date and time | Where contacts and call lists live (`contacts`, `call_lists`), and the clock (`clock()`, `wall()`) |
| Timers, in one tick (`dect_base_tick()`) | Calling it, with a monotonic millisecond clock |
| — | Audio: binding a call's voice path, lanes, codecs (`call_up()`, `call_down()`, `side_call()`, `lanes()`, `side_down()`) |
| — | The transport to the chip, the chip's session and contact, the radio switch, the park |

The library does no I/O, holds no threads, allocates nothing and keeps no
global state. Everything it needs from outside comes through
`struct dect_base_ops` and the `now` arguments.

## 2. Setting one up

```c
struct dect_base *b = calloc(1, sizeof(*b));	/* about 48 KB at API 137 */

dect_base_init(b);
b->ops = &my_ops;
b->ctx = my_cell;		/* passed back to every callback */

/* the stack's settings: struct dect_base_cfg (pin, cipher, wideband, ...) */
b->cfg.pin = ...;

/* the chip, per cell: see §6 */
b->chip.att_flag = ...;
b->chip.uak_table = ...;

/* the records the cell serves */
b->n_hs = n;
memcpy(b->hs, records, n * sizeof(b->hs[0]));
dect_base_load(b, 0);		/* rec = hs[0], the current one */
```

`dect-stubd` does this in `main()`: `cfg` copied from its `-a` options,
`chip` from `attflag=`, `attcfg=`, `noslotcfg` and the STUB record, the
records from its store files. The trace harness (`tests/test_stubd.c`) is
the smallest complete example of the set-up.

## 3. The chip's mails in

Once the chip's session is up (the cell's job), every mail from the chip
goes to one place:

| Mail | Call | Notes |
|---|---|---|
| `0xF601` contact answer | `dect_base_contact(b, m)` | Carries the RFPI |
| `0x0200` connection up (`DECT_PRIM_CONN_UP`) | `dect_base_conn_up(b, handle, now)` | `handle` = `param[0]`, 0 if none |
| `0x0201` connection down (`DECT_PRIM_CONN_DOWN`) | `dect_base_conn_down(b, handle, known)` | `known` = the mail named a handle |
| `0x0203` C-plane segment (`DECT_PRIM_SIGNALLING`) | `dect_base_segment(b, m, now)` | -1 when no link took it. On 06.88 the mail has 7 octets and no handset tag (cell-protocol.md §7.1) |
| `0x0209` cipher on | `dect_base_cipher_on(b, param[0])` | Ends `<MM_cipher.1>` |
| `0xFFF1` memory read answer | `dect_base_mem_reply(b, m)` | The key read-back and the slot audit (S2.2b) |
| `0x0242` audio channel | the host | The channel the chip assigned; `dect-stubd` connects its audio there. See §5 |
| `0xF625` connection status | the host | Radio figures for diagnostics (`dect_diag.h`) |

`dect-stubd`: `handle_reg_mail()`. The prim constants are in
`dect_proto.h`.

## 4. The callbacks

All of them may be NULL; the library then does without. `ctx` is
`b->ctx`.

| Callback | When | What `dect-stubd` does | What a controller does |
|---|---|---|---|
| `send(m)` | The library has a mail for the chip | Queues it on the control slot; logs it, keys masked | MAIL_DOWN to the cell. Return -1 if it cannot take it now |
| `pump(bl)` | C-plane frames are queued on link `bl` | `while (room) dect_link_poll_append_mail(&bl->link, &m)` then send. Take a frame out only when the transport can hold a whole mail: LAPC retransmits a lost I-frame, never a lost RR or UA | The same, with the tunnel's flow control (FLOW) as the room |
| `random(out, n)` | Keys, challenges | `getrandom()` | A CSPRNG. Never a test source in production |
| `log(line)` | A log line, no newline | stdout | Your logger. Lines never carry keys; they do carry IPUIs and handset names |
| `save(i, r)` | Record `i` changed (`r` is the working copy) | Writes the store file | Store it. `r` holds the UAK: **encrypted at rest** (controller `AGENTS.md` §5) |
| `install()` | A record's key changed: the chip's slots must be rewritten | `send_subscription()`: `dect_base_slot_clear()`, `_slot_install()` per record, `_uak_verify()`, `_slot_mask()` | The same for the records this cell serves |
| `mask()` | The records in the chip changed | `dect_base_slot_mask(b, live_slot_mask())` | The same |
| `forget(i, why)` | Record `i` is deregistered, before its slot is cleared | Moves the store aside, tells its clients | Delete or archive the record |
| `named(i)` | A handset renamed record `i` | Keeps `b->name[i]`, tells its clients | Store the name |
| `name_visible(viewer, slot)` | A link's Internal Names List is built or a handset's entry is spread (API 138) | NULL: every handset sees all | Answer by the user's account: 1 if the handset in slot `viewer` may see slot `slot`. Set `cfg.names_readonly` too (no handset renames). After an account change call `dect_base_names_refresh()` |
| `cap_learned(i)` | Record `i`'s capability is proven | Keeps `b->cap[i]` | Store it |
| `switched(why)` | The stack made `b->cur` the current record | Serves that store from now on | Your view of the current handset |
| `registered(now)` | An enrolment completed and the key is in the chip | Applies the profile chosen for the window, closes the window | The same |
| `event(o)` | A line for the bridge (`docs/bridge-interface.md` §2.1) | Sends it to baresip's module | Your SIP side: ring, connect, release, call info |
| `lanes(p)` | Can a new call on link `p` (-1 main) still get an audio channel? | Counts its free lanes | The cell's free voice channels (cell-protocol.md §7.5) |
| `call_up(now)` | After each main-link segment, before the call's cipher steps | Binds the call's audio when due (0x0230, then 0x022D on 0x0242) | MEDIA_OPEN, see §5 |
| `call_down()` | The main call is over and had audio | Releases its audio | MEDIA_CLOSE |
| `side_call(k, now)` | Side link `k`'s call, around each segment | Its lane, routing a dialled number | The same for side calls |
| `side_down(k)` | Side link `k` goes | Releases its lane first | MEDIA_CLOSE for it |
| `swapped(k)` | Side link `k` and the main link swapped | Swaps its per-link state | The same |
| `link_up(p)` | Link `p` was set up afresh | Resets its per-link audio and diagnostics | The same |
| `clock(dt, off_s)` | The date and time, if known to be right | From the system clock once NTP set it | Your clock: return 0 until it is right |
| `wall(dt)` | The date and time for a call list entry | The system clock | The same |

## 5. Audio

Audio stays with the host on purpose: on a box it is PCM lanes, on the
controller it is the cell's media channel. `dect-stubd`'s path, for
reading:

1. `call_up()` sees the call connected and binds it: `0x0251` (channel
   parameters), then `0x0230` ASSIGN_AUDIO_CHANNEL
   (`dect_base_audio_assign()`), from `dect_base_audio_param()`.
2. The chip answers `0x0242` naming the channel; the host sends `0x022D`
   (`dect_base_audio_path()`) to connect it.
3. `call_down()` releases it (`dect_base_audio_release()`).

Over the tunnel **the controller sends these mails** (the maintainer,
2026-10-30): `call_up()` sends 0x0251 and 0x0230 as MAIL_DOWN, takes the
long-form 0x0242 on the call's handle from MAIL_UP, sends 0x022D, then
MEDIA_OPEN with the channel (format 6 for G.722, 1 for L16);
`call_down()` sends the release, then MEDIA_CLOSE. The bind has no air
timer, so the tunnel costs one round trip at the start of a call. If a
bench shows it is too time-critical after all, the bind moves to the cell
(cell-protocol.md §8.1 and the controller maintainers' notes have the trigger).

## 6. Per-cell chip settings (`struct dect_base_chip`)

| Field | What | Where it comes from |
|---|---|---|
| `att_flag`, `att_cfg` | 0x0247's flag and 0x020A's byte after the key [CR A-0113] | `dect-stubd` sends 0 for both unless `-a attflag=`/`attcfg=` say otherwise (stock sends 0x01 per Gigaset); take the same |
| `no_mask` | Send no 0x017C | Off |
| `uak_table` | The STUB build's UAK table address, 0 = unknown: no read-back, no audit | From the cell's `stub_version` in HELLO: `0x9518` for 08.25, `0x067F` for 06.88 (the 7412), **0 for 07.56** (the 7520's usual build: no address known) |
| `uak_guess` | The table is a guess for an unnamed build | 0 on the controller: the cell always names its build |
| `check_delay_ms` | Hold the reads back after the writes | 1000 for 06.88, 0 otherwise (observed on a 7412, 2026-10-29) |
| `stub` | The build's name, for the log | The cell's `stub_version` |

The chip keeps its subscription slots across daemon restarts and parks
(cell-protocol.md §7.4): after every radio-on, rewrite the slots this cell
should hold and clear the others (`install()` does both), then audit.

## 7. The tick and the verbs

- **Tick:** `dect_base_tick(b, now, radio_on)` once per loop pass:
  `dect-stubd` runs its steps on every block from the chip's bus and at
  least every 100 ms. `radio_on` is the chip's session being up: the ring and the
  date and time wait for it. `dect-stubd` calls the same steps one by one,
  because its own work sits between them. A host with nothing to
  interleave calls `dect_base_tick()`.
- **Bridge verbs** (call-in, answer, remote ring, MWI, call log; deregister,
  rename): `dect_base_ctl_call()` and `dect_base_ctl_admin()` take a parsed
  `struct dect_ctl_msg` and fill a `struct dect_base_reply`. The verbs and
  their replies are `docs/bridge-interface.md` §2.
- **Registration window:** `dect_base_reg_window()`; the policy (who may
  enrol) is `dect_base_reg_policy()`.

## 8. Testing a host

The sixteen golden traces in `tests/stubd-traces/` are the behaviour, mail
for mail: what goes to the chip and what goes to the bridge after each
segment from a handset. They carry made-up identities and keys only. A
second host that replays them through its own callbacks and gets the same
mails and events is the same base. The trace format and the actions are at
the top of `tests/test_stubd.c`; `tests/test_base.c` shows the library
driven directly. Recording a new trace: `GOLDEN_UPDATE=1 build/test_stubd
<file>`, then replay it unchanged on the code before your change.

## 9. Limits at API 137

- One `dect_base` serves one radio: one chip, up to six records in its
  slots. Several cells under one controller means one `dect_base` each, and
  a handset registered through cell A is in cell A's records only. Sharing
  records across cells (roaming) is not built: see
  [multicell-identities.md](multicell-identities.md) and
  [multicell-experiments.md](journals/multicell-experiments.md).
- `dect-stubd` still holds its controller-fetch experiment (M4) and the
  control verbs that are not the bridge's (S5c).
- Behaviour on the air was checked on two box models and three Gigaset
  models through `dect-stubd`. A second host is untested until its first
  handset registers through it.
