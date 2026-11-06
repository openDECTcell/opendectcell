# src/cell: dect-cell, the link to an openDECTcell controller

`dect-cell` makes a box a cell of an openDECTcell controller (the separate project
that runs one DECT system over several boxes). It holds the TLS 1.3 link to
the controller with mutual authentication, enrols the box with a one-time
enrollment string, says HELLO, keeps the heartbeat, reconnects with back-off,
renews its certificate, and relays frames between the controller and
`dect-stubd -K` ([../stubd/](../stubd/README.md)) over a local socket in the
same frame format. It also carries the cell's media: it terminates the
controller's SRTP sessions (`dect_srtp.c`) and passes the speech to
`dect-stubd` through the audio socket. TLS lives in its own process so that a
handshake never delays the chip's control lane. It is built against mbedTLS
3.6 and only exists in cell mode; a box in base mode does not run it.

The wire protocol is not defined in this repository. It is
`docs/cell-protocol.md` in the opendectcell-controller repository
(<https://github.com/openDECTcell/opendectcell-controller>); comments in the code cite
its sections as "§N". The frame and TLV builders and parsers that both sides
use are `../libopendectcell/dect_cell.c` and `dect_cell.h` (parsing and building
only, no I/O).

## Files

| File | What it is |
|---|---|
| `dect-cell.c` | The program: `enroll`, `run`, `forget`; TLS, certificates, link state machine, media relay, renewal, identity handling |
| `dect_srtp.c`, `dect_srtp.h` | SRTP for the cell's media (RFC 3711 AES_CM_128_HMAC_SHA1_80 and RFC 7714 AEAD_AES_128_GCM): `dect_srtp_init()`, `dect_srtp_protect()`, `dect_srtp_unprotect()`, `dect_rtp_header_len()`. Built on mbedTLS |
| `../libopendectcell/dect_cell.c`, `dect_cell.h` | Cell protocol frames and TLVs (shared with `dect-stubd -K`) |

## Commands

```
dect-cell [-d DIR] [-s SOCKET] [-a SOCKET] [-R CMD] [-P] [-v] COMMAND
```

| Command | What it does |
|---|---|
| `enroll STRING` | Enrol with a controller. STRING is `opendectcell-enroll:v1?h=HOST:PORT&t=TOKEN&ca=PIN`, or `@FILE` / `@-` (standard input) holding it, so it stays out of the process list. Makes a key and a certificate request, pins the controller's CA, stores the result |
| `run` | Keep the link and relay frames. Waits for credentials if the box is not enrolled |
| `forget` | Erase the cell's key and certificate |

| Option | Meaning | Default |
|---|---|---|
| `-d DIR` | Credentials directory | `/etc/opendectcell/cell` |
| `-s SOCKET` | `dect-stubd`'s cell socket | `/var/run/opendectcell-cell.sock` |
| `-a SOCKET` | `dect-stubd`'s audio socket | `/var/run/opendectcell-audio.sock` |
| `-R CMD` | What restarts the service when the controller sets a new DECT identity (called as `CMD restart`) | `/etc/init.d/opendectcell` |
| `-P` | Allow plain, unencrypted RTP for media sessions (an admin's choice, trusted networks only) | off |
| `-v` | Verbose | off |

Credentials in the directory: `key.pem`, `cert.der`, `ca.der`, `enrollment`,
and during a renewal `key.pem.new` and `cert.der.new`; `rfpi` and `rfpi.tried`
hold the DECT identity the controller set (the init script reads `rfpi` at
start and has `dect-fwload` patch it into the chip image). The link state for
LuCI is written to `/var/run/opendectcell/cell-link.json`.

On a box the init script runs it: `opendectcell.init` `start_cell()` starts
`dect-stubd -K` and a second procd instance `dect-cell run` (with `-P` when the
UCI option `cell_plain_rtp` is set); `/etc/init.d/opendectcell enroll '<string>'`
calls `dect-cell enroll` and switches the role to cell (`enroll_start @FILE` is the same in the
background, with a limit of 90 seconds, its outcome in `/var/run/opendectcell-ui/enroll.status`: what LuCI uses, since a call
to the box ends after 20 to 30 seconds), `cell_leave` calls `dect-cell forget`. Built as the OpenWrt package `opendectcell-cell`
(`openwrt/net/opendectcell/Makefile`).

## Structure of dect-cell.c

The file is divided by banner comments:

| Section | Functions |
|---|---|
| files, encodings | `file_put()`, `file_get()`, `base32_decode()`, `base64url()`, `query_get()` |
| the controller's certificate (§4.2) | `spki_pin()`, `verify_controller()` (chain must end in a pinned CA, leaf carries the controller URI) |
| TLS | `tcp_connect()`, `tls_open()`, `tls_send()`, `tls_recv()`, `tls_close()` |
| enrolment (§5) | `make_key_csr()`, `enroll()`, `forget()`, `creds_load()`, `pair_ok()` |
| the running link | `hello()`, `link_up()`, `link_down()`, `backoff()`, `ctl_send()`, `ctl_error()`, `from_controller()`, `from_stubd()`, `stubd_connect()`, `run()`, `link_state_write()` |
| media (§8) | `media_open()`, `media_up()`, `media_down()`, `media_close()`, `media_drop_all()`: one UDP socket and one audio-socket connection per session, SRTP both ways |
| renewal (§5.4) | `renew_due()` (two thirds of the certificate's life), `renew_tick()`, `renew_response()` |
| the DECT identity (CONFIG_SET tag 6) | `identity_apply()`, `identity_clear()`, `restart_service()` |
| entry | `enroll_text()`, `usage()`, `main()` |

## Tests

| Test | What it covers | How to run |
|---|---|---|
| `tests/test_cell.c` | Frame and TLV round trips of `dect_cell.c` | `make check` |
| `tests/test_srtp.c` | `dect_srtp.c` | `make check-cell MBEDTLS=<prefix>` |
| `tests/test_cell_link.py` | The real `dect-cell` (built with the sanitizers as `build/dect-cell-test`) against a scripted controller (`tools/cell-testpeer.py`) and a stand-in `dect-stubd`: GOODBYE, MAIL_DOWN while pending, oversized bursts, SIGTERM, silent controller, media with SRTP, renewal, identity | `make check-cell MBEDTLS=<prefix>` (needs mbedTLS 3.6 and Python's `cryptography`) |
| `tests/test_stubd_cell.c` | The `dect-stubd -K` side of the same relay | `make check` |

`make dect-cell CC=<SDK gcc> MBEDTLS=<prefix> STAGING_DIR=<SDK>/staging_dir`
builds the program (see the top-level `Makefile`); plain `make check` does
not need mbedTLS.

## Where to look for...

| You want | Look at |
|---|---|
| The wire protocol itself | `docs/cell-protocol.md` in the opendectcell-controller repository; frame layouts in `../libopendectcell/dect_cell.h` |
| The `-K` side of the relay | `cell_*()` in `../stubd/stubd_cell.c`; [../stubd/README.md](../stubd/README.md) |
| How a cell is enrolled and started | `enroll()`, `run()`; `start_cell()`, `enroll()` and `cell_leave()` in `openwrt/net/opendectcell/files/opendectcell.init` |
| Certificate checks and pinning | `verify_controller()`, `spki_pin()`, `creds_load()` |
| Media and SRTP | `media_*()` in `dect-cell.c`, `dect_srtp.c`; the audio socket is `ab_*()` in `../stubd/stubd_audio.c` |
| Why the controller can host a cell's stack | [../../docs/hosting-dect-base.md](../../docs/hosting-dect-base.md), [../../docs/stack-in-library.md](../../docs/stack-in-library.md), [../../docs/notes-for-the-controller.md](../../docs/notes-for-the-controller.md) |
| Identities (RFPI) for several cells | [../../docs/multicell-identities.md](../../docs/multicell-identities.md) |
