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
the radio stays parked until the controller switches it on. Almost all of it
is one file, `dect-stubd.c` (about 9,100 lines); the DECT layers themselves
are in [../libopendectcell/](../libopendectcell/).

## Files

| File | What it is |
|---|---|
| `dect-stubd.c` | The daemon: transport, mail pump, host callbacks of `dect_base`, sockets, cell mode, option parsing, `main()`. Map below |
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

## Map of dect-stubd.c

Line numbers are those of this checkout and drift; the function names and
the banner comments are the stable handles (`grep -n '^static .* name('`).
The file reads roughly from the chip upward to the sockets, then the loop,
then options and `main()`.

| Lines | Region | Functions and what to find there |
|---|---|---|
| 1-335 | Header, constants, clock, signals | File comment, includes. `clock_is_trustworthy()`, `clock_now()` (is the box clock fit to be sent to handsets: `adjtimex` plus the NTP marker file `/var/run/opendectcell/time-valid`). `DECT_CTL_PATH`, park timing (`PARK_DRAIN_MS`, `PARK_FLUSH_MS`), the signal handlers `on_signal`, `on_usr1`, `on_usr2`, `on_hup`, `mono_ms()`. `#ifdef STUBD_TEST` hooks for the tests (`stubd_test_ms`, `stubd_test_run()`) |
| 337-1150 | Data structures | `struct handset`, `struct side_link`, `struct xlane` (audio lane of a side link), `struct ab_lane` (speech lane on the audio socket), `struct stubd` (the whole daemon state, including `struct audio_cfg`, which holds what `-a` keys set) |
| 1150-1240 | STUB build record | `DECT_RUN_DIR`, `stub_build_read()`: which second-stage firmware the chip runs, from `stub.second`; the chip RAM addresses used for key read-back belong to one build. `fopen_root_only()` (files only root may have written) |
| 1240-1475 | Mail log and verification table | `log_mail()` (`-v`, `-p`), `verify_count()`, `verify_report()` (what we can and cannot check after a command) |
| 1475-1733 | Transport glue, mail send | SSC (7412 / 7362 SL) alignment: `pcm_block_ok()`, `ssc_rx_fix()`, `ssc_watch()`, `pcm_write()`. `send_mail()` (queues mails for the control slot), `chip_mail()`, `chip_mem_read()` |
| 1733-2220 | Speech lanes and transmit block | `ab_pop()`, `ab_up()`, `ab_down()`, `ab_frame()` (a bridged call's speech, frame by frame, G.722 or linear), `xlane_tx()`, `tx_templates()`, `lane0_play()`, `tx_block()` (builds each outgoing DMA block: pacing of control mails, idle fill, speech) |
| 2221-2594 | Control-slot search and bring-up | `parse_mail_spec()` / `send_mail_spec()` (the `-M` and `latemail=` text form), `find_control_slot()` (probes every slot for the contact answer, transmit and receive separately), `ssc_measure()`, `ssc_flip()`, `ssc_bring_up()`, `bring_up()` (the six `DECT_IOC_PCM_STEP` steps on `/dev/dect0`, or the SSC info ioctl), `tear_down()` |
| 2594-3400 | Host callbacks of dect_base | `gen_rand()` (kernel randomness only). The subscription store (`struct sub_record`, `sub_save()`, `sub_load()`, `hs_load()`). `base_ops` and the `base_*` callbacks (`base_save`, `base_send`, `base_install`, `base_event`, `base_pump`, `base_lanes`, `base_forget`, ...). Roaming fetch (`fetch_tick()`), stored capabilities (`cap_save()`, `cap_load()`), thin `*_tick()` wrappers around library timers, `send_subscription()` (writes the handset keys into the chip's slots) |
| 3400-3540 | Park and park check | `PARK_RECORD`, `park_record()`, `park_check_start()`, `park_check_reply()`, `park_check_done()`: after the park mails, read the radio switch and beacon timer back (done for the 7412's 06.88 STUB, otherwise the mails on the wire count). `mem_reply()` |
| 3540-4000 | Chip audio bind, debug reads, ring | `send_audio_bind()`, `send_audio_connect()`, `send_audio_disconnect()`, `remode_tick()`, `seq_tick()` (`-a seq=`), `mem_dump_tick()` (`-a memdump=`), connection-status polling (`send_conn_status_req()`, `report_conn_status()`), `ring_tick()` |
| 4004-4245 | LED, state file | `led_open()`, `led_write()`, `led_tick()` (on while the radio is up, blinks on traffic). `state_write()` writes `state.json` at most once a second: stub build, radio, links, handsets. Field meanings: [../../docs/diagnostics.md](../../docs/diagnostics.md). `pump_lapc()` |
| 4246-4780 | Registration window, handset data | `reg_window()`, `reg_window_tick()` (SIGUSR1 and the control socket's `register-window`), `base_link_up()`. Names, lines, contact list and call lists: `hs_name()`, `lines_load()`, `contacts_tick()`, `calls_tick()`, `name_load()`, `name_save()`. Handset profiles: `profile_resolve()`, `reg_profile_apply()`, `profiles_resolve_all()` ([../../docs/handset-profiles.md](../../docs/handset-profiles.md)) |
| 4780-5105 | Side links, internal call, deregistration | `side_find()`, `xlane_*()` (audio lanes for the second and third call), `side_route()`, `side_call_tick()`, `ic_*()` (internal call), `base_forget()`, `dereg_start()` (SIGUSR2 reads `/var/run/opendectcell/deregister`), `dereg_tick()`, `time_tick()` |
| 5105-5330 | Control socket | `ctl_drop()`, `ctl_send*()`, `ctl_hello()`, `ctl_hs()`, `bridge_call_out()`, `bridge_command()`, `bridge_tick()`. Banner comment at 5105. Protocol: [../../docs/bridge-interface.md](../../docs/bridge-interface.md) |
| 5330-5570 | Audio socket | `ab_listen()`, `ab_open()`, `ab_hello()` (a connection names its call: `call=N codec=g722` or `rate=`), `ab_tick()`. Used by baresip and, in cell mode, by `dect-cell` |
| 5569-5780 | Control commands and clients | `ctl_command()` (verbs: `register-window`, `reload-profiles`, plus `dect_base_ctl_admin()` and `bridge_command()`), `ctl_open()`, `ctl_close()`, `ctl_read()`, `ctl_tick()`, `ic_tick()` |
| 5779-6020 | Call plumbing and library callbacks | `side_pump()`, `side_cipher_tick()`, `start_call_cipher()`, `base_registered()`, `base_call_up()`, `base_side_call()`, `base_call_down()`, `base_swapped()` |
| 6020-6327 | Mail entry, starvation, capture | `handle_reg_mail()`: every mail from the chip enters the base stack here (with `-g`). `report_tx_starvation()`, `voice_slot_probe()`, `voice_capture()` (`-w`, `-V`) |
| 6327-7050 | Cell mode (`-K`) | Banner comment at 6327. `cell_open()` (listens on `/var/run/opendectcell-cell.sock`), `cell_send()`, `cell_status()`, `cell_stats()`, `cell_snapshot()`, `cell_mail_up()` (every chip mail up as it came), `cell_mail_down()` (MAIL_DOWN to the chip), `cell_state()` (CELL_STATE: radio on and off, grace and orphan timers), `cell_media_open()`, `cell_media_close()`, `cell_media_tx()` (the controller's sessions bound to lanes), `cell_frame()` (dispatch on frame type), `cell_state_write()` (`cell.json`), `cell_tick()`, `rfpi_check()` |
| 7054-7450 | Main loop | `loop_tick()` (all periodic work), `run_loop()`: reads PCM blocks, runs `tx_block()`, pulls mails, hands them to the session and `handle_reg_mail()` or `cell_mail_up()`, then `loop_tick()`. The park at the end of a run is done inside this loop; see below |
| 7450-7892 | The `-a` option string | `audio_word_keys[]`, `parse_audio()`, `parse_chip_cipher()` (`-k`) |
| 7893-8096 | Usage text | `usage()`: the authoritative list of options and `-a` keys |
| 8097-8390 | Profile loading and helper modes | `profile_load()`; `ctl_client()` (`--ctl`), `bridge_echo()` (`--bridge-echo`), `check_profile()` (`--check-profile`) |
| 8391-9138 | `main()` | Helper modes first (`--version`, `--ctl`, `--profile-keys`, `--bridge-echo`, `--check-profile`), defaults, `getopt` loop, auto bring-up mail list for `-g`, copy of options into `s.base.cfg`, handset stores, profiles, then `open(dev)`, `bring_up()`, control-slot search (three passes, twenty on the SSC transports), lane set-up, `run_loop()`, shutdown |

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

`dect-stubd [options]`; the full text is `usage()` (line 7893). Options are
parsed by `getopt` with `"d:c:T:R:t:vpM:gKb:P:s:k:w:V:Ca:h"`.

| Option | Meaning |
|---|---|
| `-d DEV`, `-c N` | Transport device (default `/dev/dect0`) and slots per frame (default 8) |
| `-T SLOT`, `-R SLOT` | Give the transmit and receive control slot instead of probing |
| `-t SECS` | Run this long, then park (if `-g` or `-K`) and exit |
| `-v`, `-p` | Log every mail; `-p` also its parameter bytes (may contain identity data: do not share such logs) |
| `-M SRC:DST:PRIM[:HEX]` | Send this mail once the link is up, one per loop tick; repeatable |
| `-g` | Base mode: bring the radio up (registration closed unless `-a regopen`) and run the host stack; parks at exit |
| `-K` | Cell mode, see above; cannot be combined with `-g` |
| `-P PIN` | Registration PIN (4 to 8 digits); `-P @FILE` reads it from a root-only file |
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

`dect-stubd.c` is compiled into the tests with `-DSTUBD_TEST`: `main()` hands
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
used by the daemon and its tests are listed in `STUBD_SRC` in the top-level
`Makefile`. `make cross` builds the ARM binary (plain `make` builds a PC
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
