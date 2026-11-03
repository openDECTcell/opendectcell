# src/baresip: the baresip module "openDECTcell" (the SIP bridge)

`opendectcell.c` is a module for baresip, the SIP client on the box. Loaded into
baresip, it is the bridge between `dect-stubd` and SIP: it connects to
`dect-stubd`'s control socket, turns the handsets' calls into SIP calls and
SIP calls into rings on the handsets, and it is baresip's audio source and
player (device name `opendectcell`), fed through `dect-stubd`'s audio socket, one
connection per call. It is also baresip's "G722" codec: the coded octets pass
through unchanged, so a wideband call is coded by the two phones alone.
There is no separate daemon; the module runs inside baresip. Everything it
sends and receives is the protocol in
[../../docs/bridge-interface.md](../../docs/bridge-interface.md).

## Files

| File | What it is |
|---|---|
| `opendectcell.c` | The whole module (about 1,650 lines) |
| `peers.c`, `peers.h` | The list of addresses and prefixes an account that does not register may be called from (`opendectcell_peers` in the config, `list sip_peer` in UCI); needs no baresip header, tested by `tests/test_peers.c` |
| `../libopendectcell/dect_ctl.c`, `dect_ctl.h` | The control socket's line format (verb plus `key=value` pairs); compiled into the module, tested by `tests/test_ctl.c` |
| `../libopendectcell/dect_clip.h` | Only the size limits of the caller's number and name (`DECT_CLIP_NUMBER_MAX`, `DECT_CLIP_NAME_MAX`), so the module cuts them to what `dect-stubd` puts into the CLIP |

## How it is built and loaded

- **Built** as `opendectcell.so` from `opendectcell.c`, `peers.c` and `../libopendectcell/dect_ctl.c`,
  against the headers of the baresip and libre versions on the box. Two ways:
  the OpenWrt package `opendectcell-baresip` (`Build/Compile/baresip` in
  `openwrt/net/opendectcell/Makefile`, installs to `/usr/lib/baresip/modules/`;
  depends on `baresip` and its g711, auresamp, mwi and srtp modules), or, for a
  test build, `make baresip-mod BARESIP_SRC=... RE_SRC=... SDK=...` from the
  top-level `Makefile`.
- **Started** by the init script `openwrt/net/opendectcell/files/opendectcell.init`:
  when at least one handset has a VoIP line, `baresip_write()` writes
  `/var/run/opendectcell-sip/config` and `accounts` (one account per VoIP profile
  that is some handset's Line 1 or Line 2, from `/etc/config/opendectcell`) and the
  line map `/var/run/opendectcell-sip/lines` (`<handset> <line> <aor>`), and
  `start_service()` runs `baresip -f /var/run/opendectcell-sip` as a procd
  instance. Without a usable line, baresip is not started.
- **Loaded** by lines in that generated config: `module auresamp.so`,
  `g711.so`, `opendectcell.so`, then the NAT and media modules that exist
  (`stun`, `turn`, `ice`, `srtp`, `dtls_srtp`, `mwi`) and `module_app
  account.so`. `opendectcell.so` goes before `account.so` because the accounts
  name its codec. The config also sets `audio_source opendectcell,` and
  `audio_player opendectcell,`, and `opendectcell_any_source yes` only when the UCI
  option `sip_any_source` is on (otherwise INVITEs are accepted only from the
  address an account registers at: `sip_origin_ok()`).
- **Module entry:** `module_init()` registers the codec, the audio source and
  player, and the event handler, reads the line map, and connects to
  `/var/run/opendectcell.sock` (`ctl_connect()`, retrying every two seconds). On
  connect it says `bridge`; from then on `dect-stubd` sends it the handsets'
  external calls.

## What opendectcell.c holds

| Banner section | Functions |
|---|---|
| the line map | `lines_load()`, `line_ua()`, `line_of()`: which account belongs to which handset line |
| calls | `struct bcall` (one DECT call or one leg of a call in), `bc_by_id()`, `bc_by_call()`, `bc_new()`, `bc_free()`, `call_end()`. Up to `MAX_CALLS` (8) |
| the call lists | `calllog()`, `incall_of()`: one `calllog` message per incoming SIP call when it ends, for the handsets' call lists |
| the control socket | `ctl_connect()`, `ctl_read()`, `ctl_line()` (one branch per verb from `dect-stubd`: `call-out`, `call-in-ok`, `busy`, `answered`, `released`, `audio`, `hold`/`resume`, `digit`, `hs`, `error`), `ctl_send()`, `call_out()`, `call_in()` |
| the audio device | `aud_open()` (connects to the socket `dect-stubd` named), `aud_read()`, `aud_read_g722()`, `src_alloc()`, `play_alloc()`, `g722_through` (the pass-through codec), `narrow_begin()` / `narrow_end()` (offer G.711 only for narrowband handsets) |
| baresip's events | `event_handler()`: registration, ringing, answered, closed, re-INVITE codec changes, MWI; `mwi_notify()` sends `mwi` to `dect-stubd` |
| the VoIP status | `status_write()`, `status_tick()`: `/var/run/opendectcell-sip/voip.json` every two seconds and on registration events (registration per line; per call codec, endpoints, rates, loss, jitter, encryption). Shown in Status > DECT, tab VoIP |
| the module | `module_init()`, `module_close()`, `DECL_EXPORTS(opendectcell)` |

Codec rule (maintainer's rule, also in the file header): the phones code the
speech and nothing converts unless the two ends share no codec. The module
matches codecs through signalling (a narrowband handset's INVITE offers G.711
only; a caller who offers no G.722 rings the handsets narrowband) and the
audio socket runs at each call's SIP rate. What `dect-stubd` does with the
frames for each combination is the table in section 3 of
`bridge-interface.md`.

## Tests

There is no automated test of `opendectcell.c` itself; it needs baresip and libre.
The line format it shares with the daemon is tested by `tests/test_ctl.c`, and
the daemon's side of every message is in the golden traces
`tests/stubd-traces/` (see [../stubd/README.md](../stubd/README.md)). To try
the bridge without SIP, `dect-stubd --bridge-echo` plays a stand-in client.

## Where to look for...

| You want | Look at |
|---|---|
| The messages and who may send what | [../../docs/bridge-interface.md](../../docs/bridge-interface.md); `ctl_line()` here, `ctl_command()` and `bridge_command()` in `../stubd/dect-stubd.c` |
| Lines, which handset rings for which account | `lines_load()`, `call_in()`; [../../docs/lines-and-calls.md](../../docs/lines-and-calls.md) |
| Two calls at once, hold, call waiting | `struct bcall`, `call_hold()` use in `ctl_line()`; [../../docs/two-calls.md](../../docs/two-calls.md) |
| The account and baresip config the box generates | `bs_line()`, `baresip_write()` in the init script; VoIP profile fields in [../../docs/luci-manual.md](../../docs/luci-manual.md) |
| Audio framing and the G.722 pass-through | `aud_read()`, `g722_through`; [../../docs/bridge-interface.md](../../docs/bridge-interface.md) section 3 |
| Voicemail lamp (MWI) | `mwi_notify()`, `mwi_count()` |
| Call lists and names from the address book | `calllog()`; [../../docs/phonebook.md](../../docs/phonebook.md) |
| The Status > DECT VoIP tab | `status_write()`; `openwrt/applications/luci-app-opendectcell/` |
