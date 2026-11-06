# src/stubd: dect-stubd, the daemon on the box

`dect-stubd` is the program that talks to the DECT chip. It opens the PCM
transport device, finds the slot that carries the chip's control mails, and
keeps that link alive; on top of it, it is the host of the base-station
stack in `libopendectcell` (`struct dect_base`): it feeds the chip's mails into
the stack, sends the stack's mails to the chip, and supplies everything the
library leaves to its host (files, clocks, randomness, sockets, the LED, the
radio switch, the park). In base mode (`-g`) it runs the registration
window, the handsets and their calls and talks to the SIP side (baresip)
over two local sockets. In cell mode (`-K`) it keeps no stack at all: every
chip mail goes verbatim to `dect-cell` ([../cell/](../cell/README.md)) and
the radio stays parked until the controller switches it on. The daemon is
nine source files around one shared header (below); the DECT layers
themselves are in [../libopendectcell/](../libopendectcell/).

## Files

| File | What it is |
|---|---|
| `dect-stubd.c` | The core: clock and signals, `handle_reg_mail()` (every chip mail enters the stack here), the main loop (`loop_tick()`, `run_loop()`, the park at the end of a run) and `main()` |
| `stubd_link.c` | The link to the chip: which STUB build runs, the mail log and verification table, SSC realignment glue, sending mails, control-slot search, bring-up and tear-down |
| `stubd_audio.c` | Speech: the lanes of a bridged call, the transmit block `tx_block()`, the audio socket |
| `stubd_host.c` | The host of the base stack: the `dect_base` callbacks, the subscription store and the keys written into the chip, roaming fetch, stored capabilities, the park and its read-back |
| `stubd_calls.c` | Calls: binding the chip's audio, connection status, the ring state machine, side links (second and third call), internal calls, deregistration, the library's call callbacks |
| `stubd_handsets.c` | Handset data and state: the LED, `state.json`, the registration window, names, lines, contact and call lists, handset profiles |
| `stubd_ctl.c` | The control socket and its commands, the bridge to the SIP side |
| `stubd_cell.c` | Cell mode (`-K`): the socket to `dect-cell`, the chip's mails passed up and down, the controller's media sessions bound to chip lanes, `cell.json` |
| `stubd_opts.c` | Options: the `-a` key list, `usage()`, profile loading, and the helper modes (`--ctl`, `--bridge-echo`, `--check-profile`) |
| `stubd_internal.h` | What the files share: the structs (`struct stubd` and the rest), the constants, and the functions and variables that more than one file uses. What one file alone uses is `static` in it |
| `ssc_align.c`, `ssc_align.h` | Bit and byte realignment for the 7412 / 7362 SL transports ("SSC": no frame sync, so the lanes start at an arbitrary bit). No I/O; tested by `tests/test_ssc_align.c` |

Headers it needs come from `../libopendectcell/` (stack, mail framing, session,
control-line format) and `../../kernel/include/uapi/` (`opendectcell.h` for the
7520 module, `ssc_pcm.h` for the 7412 / 7362 SL transports).

## Process model on a box

The init script `openwrt/net/opendectcell/files/opendectcell.init` starts everything
under procd, after `load_chip()` has loaded the kernel module and run
`dect-fwload` ([../fwload/](../fwload/README.md)) to boot the chip.

```
  handsets ~~ air ~~ DECT chip  (firmware uploaded by dect-fwload at start)
                        |
                        |  PCM bus: control slot carries 0xAA-framed mails
                        |
        kernel transport: /dev/dect0 (7520/7530), /dev/ssc-pcm0 (7412),
                          /dev/tdm-pcm0 (7362 SL)       -d and -c pick it
                        |
  base mode (role "base")                       cell mode (role "cell")
  ----------------------                        -----------------------
  dect-stubd -g ...                             dect-stubd -K ...
     |  /var/run/opendectcell.sock    (control)         |  /var/run/opendectcell-cell.sock
     |  /var/run/opendectcell-audio.sock (speech)       |  /var/run/opendectcell-audio.sock
  baresip + module opendectcell.so ---- SIP/RTP       dect-cell run ---- TLS 1.3 ---- controller
  (../baresip/)                                  (../cell/)
```

- procd instances in base mode: `dect-stubd` (the unnamed main instance),
  `baresip` (only when at least one handset has a VoIP line), `carddav`
  (a ucode script, only when the CardDAV address book is enabled). In cell
  mode: `dect-stubd -K` and the instance `cell` running `dect-cell run`;
  nothing of the base (no handsets, no PIN, no baresip).
- The service is off by default (`option enabled '0'`). Stopping it makes
  the daemon park the radio on SIGTERM and read the park back from the chip;
  if the park was not confirmed, the init script's `park_guard()` holds the
  chip in reset (the daemon writes `/var/run/opendectcell/parked` when the park
  is done).
- The board's values (`uart`, `pcm_dev`, `pcm_slots`, `led_path`, firmware
  file names and checksums) come from `boards/<board name>` through
  `dect_load_profile()`; see [../../docs/hardware-abstraction.md](../../docs/hardware-abstraction.md).
  `PCM_ARGS` in the init script turns `pcm_dev` and `pcm_slots` into `-d` and
  `-c`; the 7520 uses the defaults (`/dev/dect0`, 8 slots).
- The daemon's files live in `/var/run/opendectcell/` (root only): `state.json`
  (base state for LuCI), `cell.json` (cell state), `stub.second` (which STUB
  build the chip runs; written by the init script, read at start),
  `parked`, `pin`, `calls`, `contacts`, `deregister`. Handset records are
  the stores `/etc/opendectcell-sub.N` (`-s`) with side files `.name`, `.profile`,
  `.cap`, and `.deregistered` after a deregistration. Per-handset overrides
  and line names are written by the init script to
  `/etc/opendectcell/handset<N>.override` and `.lines`.

## Finding things

`grep -n 'name(' src/stubd/*.c` finds a function. The files above are the map: a function
that more than one file calls has its prototype in `stubd_internal.h`, under the heading
naming the file that defines it. A new function goes into the file whose row fits it; one
that another file needs gets its prototype there and loses `static`.

The two timing-sensitive pieces are kept in the files that run every frame: `tx_block()`
(`stubd_audio.c`) builds each outgoing block, and `run_loop()` (`dect-stubd.c`) reads the
blocks and pumps the mails. The 7412's transport already loses frames at high rates, so
do not move work into them or split them across more files without measuring.

### Park and stop

The park is part of the main loop, not of shutdown. On SIGTERM, SIGINT or the
end of `-t SECS`, `run_loop()` queues "close registration" (`0x022c`) and
"radio off" (`0x015b`), keeps pumping until the transmit queue is empty plus
`PARK_FLUSH_MS`, and only then calls `park_check_start()`. It gives up
(`RADIO NOT PARKED`) after `PARK_DRAIN_MS`. This applies to `-g` and `-K`
runs; a run with neither exits without sending a park. `main()` writes
`state.json` as "parked" afterwards and removes `/var/run/opendectcell/parked` at
start, so the file always belongs to the current run.

## Options

`dect-stubd [options]`; the full text is `usage()` (`stubd_opts.c`). Options are
parsed by `getopt` with `"d:c:T:R:t:vpM:gKb:P:s:k:w:V:Ca:h"`.

| Option | Meaning |
|---|---|
| `-d DEV`, `-c N` | Transport device (default `/dev/dect0`) and slots per frame (default 8) |
| `-T SLOT`, `-R SLOT` | Give the transmit and receive control slot instead of probing |
| `-t SECS` | Run this long, then park (if `-g` or `-K`) and exit |
| `-v`, `-p` | Log every mail and NWK message, challenges, list contents and party numbers left out; `-p` the parameter bytes and the NWK messages in full (identity data and secrets: do not share such logs) |
| `-M SRC:DST:PRIM[:HEX]` | Send this mail once the link is up, one per loop tick; repeatable |
| `-g` | Base mode: bring the radio up (registration closed unless `-a regopen`) and run the host stack; parks at exit |
| `-K` | Cell mode, see above; cannot be combined with `-g` |
| `-P PIN` | Registration PIN (4 to 8 digits); `-P @FILE` reads it from a root-only file. `-g` refuses to start without it: there is no default PIN |
| `-s PATH` | Subscription store; repeat for every handset slot. `-s ""` turns the store off |
| `-b FILE` | Offer the handsets a contact list from FILE (the CardDAV sync writes it); changes are appended to `FILE.changes` |
| `-k off` | Do not send the call key to the chip before cipher requests |
| `-C` | Do not start call ciphering |
| `-w FILE`, `-V SLOT` | Capture the call's TDM slot(s) to FILE (holds call audio, keep it local). Read with `dect-maildump` ([../tools/](../tools/README.md)) |
| `-a SPEC` | Comma-separated `key=value` list, below |

Important `-a` keys (all in `usage()`; the init script builds its list from
UCI in `start_service()`):

| Group | Keys |
|---|---|
| Audio bind | `sel`, `gain`, `flags`, `mask`, `mode`, `codec`, `slotmask`, `noslotcfg`, `remode`, `noremode`, `toneword`, `txidle`, `voiceidle`, `lane2`, `lane3` |
| Registration | `regopen[=SECS]`, `regwindow=SECS`, `subslot`, `ipuio`, `locauth`/`nolocauth`, `authfirst`/`noauthfirst` |
| Calls | `twocalls`, `narrowband`, `basicsvc`, `signal`/`nosignal`, `dialwait`, `nooverlap`, `alertms`, `bindfirst`, `setuptries`, `codeclist`, `infoie`, `nocallstatus`, `allcallstatus`, `nonlr`, `ring=MS`, `ringattach=MS` |
| Lines, caller id | `linename`, `lineid`, `linetype`, `callid`, `callstatus`, `clip`, `cnip` |
| Clock | `timesync=auto|force|off`, `timeupdate=MIN` |
| Files, sockets, display | `ctl=PATH` / `noctl`, `statefile=PATH` / `nostatefile`, `led=PATH` / `noled`, `profiles=DIR` (not in `usage()`) |
| Debug and bench | `play`, `playpcm`, `playraw`, `playsync`, `loopback`, `capall`, `memdump`, `memdumpms`, `status=MS`, `seq`/`seqstep`, `latemail`, `installdummy`, `tpuioffset`, `keycheckms`, `attcfg`, `attflag`, `holdback`, `nochipkeys`, `remote`, `fetchdelay`, `locvalid`, `rfpicheck` (not in `usage()`) |

Helper modes, which do not touch the chip:

| Command | Purpose |
|---|---|
| `dect-stubd --version` | Version |
| `dect-stubd --ctl LINE...` | Send lines to the control socket and print the answers for one second (`OPENDECTCELL_CTL` names another socket). The box has no `socat` |
| `dect-stubd --bridge-echo [ring [NUMBER [HS]]]` | A test bridge without SIP: answers every dialled number and echoes the caller's voice through the audio socket |
| `dect-stubd --check-profile FILE [FIRMWARE]` | Validate one handset profile and print what it resolves to |
| `dect-stubd --profile-keys` | Print the profile keys as JSON |

Signals: SIGTERM / SIGINT stop and park; SIGUSR1 opens the registration
window (as `/etc/init.d/opendectcell register`); SIGUSR2 deregisters the slots
named in `/var/run/opendectcell/deregister`; SIGHUP reloads the handset profiles.

## Running it by hand

The real command line is built in `start_service()` of
`openwrt/net/opendectcell/files/opendectcell.init`. In base mode it is, in shape:

```
dect-stubd $PCM_ARGS -g -P @/var/run/opendectcell/pin [-C] -s /etc/opendectcell-sub.N ... \
    [-b /var/run/opendectcell/contacts] -a "mask=0x0002,codec=3,status=8000,...,led=PATH"
```

In cell mode: `dect-stubd $PCM_ARGS -K [-a rfpicheck=...,led=PATH]`.
To see the command a running service uses, `ubus call service list
'{"name":"openDECTcell"}'` prints the procd instances with their arguments. Its
output (stdout and stderr) goes to the system log (`logread`).

To debug without the service:

1. `/etc/init.d/opendectcell stop`. This parks the radio. The chip stays booted
   when the park was confirmed (`/var/run/opendectcell/stub.second` stays), so the
   next daemon finds it ready; when it was not confirmed, the init script
   holds the chip in reset and the next `start` uploads the firmware again.
2. Run the daemon yourself with `-v` and a time limit, for example the link
   only, with the radio untouched: `dect-stubd $PCM_ARGS -v -t 30`
   (`PCM_ARGS` for your board is in `boards/<board>`: `-d /dev/ssc-pcm0 -c 36`
   on the 7412, `-d /dev/tdm-pcm0 -c 8` on the 7362 SL, nothing on the 7520).
   Without `-g` or `-K` the daemon does not switch the radio on and does not
   park.
3. A run with `-g` puts the radio on the air and parks it at exit. End it with
   SIGTERM (Ctrl-C), never SIGKILL, so the park is sent. The stores, PIN file
   and options are the service's; copy them from the instance listing above.
4. Start the service again with `/etc/init.d/opendectcell start`.

`-a statefile=PATH` and `-a ctl=PATH` let a hand-started daemon run beside
files of the service for a test; the daemon needs `/var/run/opendectcell/stub.second`
to know the STUB build (without it the log says "STUB second stage unknown"
and key read-back uses a guessed address).

## Tests

The daemon's files are compiled into the tests as one unit (`tests/stubd_unit.h`) with `-DSTUBD_TEST`: `main()` hands
its set-up state to `stubd_test_run()` where it would open the chip, each mail
the daemon sends goes to `stubd_test_sent()`, and the clocks are the test's
own (`stubd_test_ms`, `stubd_test_wall`). The history is in
[../../docs/stack-in-library.md](../../docs/stack-in-library.md).

| Test | What it does |
|---|---|
| `tests/test_stubd.c` with `tests/stubd-traces/*.trace` | Golden traces of the whole daemon above the transport: a script plays the chip and the handsets, and every mail the daemon sends is compared with the expectations in the trace. Traces: `register`, `register-ipuio`, `locate`, `locate-second`, `call-in`, `call-out`, `call-waiting`, `side-ring`, `internal-call`, `dial-local`, `deregister`, `promote`, `list-access`, `list-access-unauthenticated`, `notices`, `time-round`. Format of the script: the comment at the top of `tests/test_stubd.c`. `GOLDEN_UPDATE=1 build/test_stubd` rewrites the expectations (review the diff). The daemon's log goes to `build/test_stubd.log` |
| `tests/test_stubd_cell.c` | Cell mode media: `dect-cell` played on a socket pair, the chip by scripted mails; MEDIA_OPEN and MEDIA_CLOSE, lane detection, speech both ways through the audio socket, snapshot, release |
| `tests/test_ssc_align.c` | `ssc_align.c` |
| `tests/test_base.c`, `tests/test_trace.c` and the per-layer tests | The library underneath (`dect_base`, `dect_link`, layers) |

Run them with `make check`; `make build/test_stubd` builds one. The sources
used by the daemon and its tests are listed in `STUBD_C` and `STUBD_SRC` in the top-level
`Makefile`; a new file of the daemon goes into both `STUBD_C` and `tests/stubd_unit.h`. `make cross` builds the ARM binary (plain `make` builds a PC
build that compiles but only runs on a box). Several of the stack's
behaviours (for instance the 7412's 06.88 specifics, or the park read-back)
can only be shown on hardware; the golden traces do not cover the transport.

## Where to look for...

| You want | Look at |
|---|---|
| How the control slot is found, and why it moves | `find_control_slot()`, `bring_up()`; [../../docs/stub-pcm-control-channel.md](../../docs/stub-pcm-control-channel.md) |
| The 7412's bit offset and byte order | `ssc_align.c`, `ssc_measure()`, `ssc_rx_fix()`, `pcm_write()`; [../../docs/7412-spi-tdm-and-loader.md](../../docs/7412-spi-tdm-and-loader.md) |
| The chip's mails | `../libopendectcell/dect_mail.c`, `dect_proto.h`; [../../docs/stub-api.md](../../docs/stub-api.md) |
| Radio on, registration mode, beacon | `main()` (the `-M` list built for `-g`), `reg_window()`; [../../docs/mac-bringup.md](../../docs/mac-bringup.md) |
| Where a mail from the chip goes | `run_loop()`, then `handle_reg_mail()`, which sorts the mail by primitive into `dect_base_contact()`, `dect_base_conn_up()`, `dect_base_conn_down()`, `dect_base_segment()` (C-plane data), `dect_base_cipher_on()` and others in `../libopendectcell/dect_base.c` |
| What the library asks of its host | `base_ops` and the `base_*` functions; [../../docs/hosting-dect-base.md](../../docs/hosting-dect-base.md), [../../docs/api-versions.md](../../docs/api-versions.md) |
| Registration window, PIN, handset records | `reg_window()`, `sub_save()`, `sub_load()`, `send_subscription()` |
| A call's audio path | `send_audio_bind()`, `ab_*()`, `tx_block()`; [../../docs/bridge-interface.md](../../docs/bridge-interface.md) section 3; [../../docs/two-calls.md](../../docs/two-calls.md) |
| The bridge's control messages | `ctl_command()`, `bridge_command()`, `ctl_hello()`; [../../docs/bridge-interface.md](../../docs/bridge-interface.md) |
| Park, stop, "RADIO NOT PARKED" | `run_loop()`, `park_check_*()`, `park_guard()` in the init script |
| The state file and Status > DECT | `state_write()`, `cell_state_write()`; [../../docs/diagnostics.md](../../docs/diagnostics.md) |
| Cell mode | `cell_*()` (line 6327 on); `../cell/`; the cell protocol is in the controller repository |
| Handset names, lines, contact list, call lists | `lines_load()`, `contacts_tick()`, `calls_tick()`, `../libopendectcell/dect_contacts.c`, `dect_calls.c`; [../../docs/phonebook.md](../../docs/phonebook.md) |
| Handset profiles | `profile_resolve()`, `../libopendectcell/dect_profile.c`; [../../docs/handset-profiles.md](../../docs/handset-profiles.md) |
| Calls, lines, ringing several handsets | `../libopendectcell/dect_base.c`; [../../docs/lines-and-calls.md](../../docs/lines-and-calls.md) |
| An option and what it does | `usage()` and `parse_audio()`; the init script's `start_service()` for the values the box uses |
