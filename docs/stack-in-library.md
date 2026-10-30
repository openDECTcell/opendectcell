# The base stack moves into libopendectcell

Decided 2026-10-30 by the maintainer (opendectcell-controller ADR 0002, option B):
the base-station stack that `dect-stubd` holds today moves into `libopendectcell`,
so the box and the controller run one implementation. This file is the plan
and the record of each stage.

## Where things are

`dect-stubd.c` is 13,400 lines, the library's C files 10,800 (2026-10-30).

| Part | Where |
|---|---|
| Layers: Cs, Lc, LAPC, NWK, MM, CC, DSAA, IWU, CLIP, call info, time, lists | `src/libopendectcell/` |
| Glue per link: `dect_link` (Cs + Lc + LAPC + MM + CC) | `dect_link.[ch]` |
| Chip transport: PCM/SSC framing, control-slot search, bring-up, park | `dect-stubd.c` |
| The stack above the links: registration window and slot writes, the subscription store, locate, side links, paging, calls in and out, internal calls, parallel calls, cipher, lists, MWI, date and time, CLIP, the missed call notification | `dect-stubd.c`, most of it |
| Control socket, cell mode, profiles, LED, state file | `dect-stubd.c` |

Mails enter the stack at `handle_reg_mail()` and leave through
`send_mail()`. Everything between them is what moves.

## The target

A `struct dect_base` in the library: one chip's worth of stack state
(links, handset records, calls, lists), driven by

- `dect_base_mail(base, mail, now)`: a mail from the chip;
- `dect_base_tick(base, now)`: time;
- `dect_base_ctl(base, message, now)`: the bridge's verbs
  (`docs/bridge-interface.md`);

and talking back through callbacks the host sets: send a mail, emit a
bridge event, store a handset record, randomness, log. No I/O, threads or
allocation inside, as for the rest of the library. `dect-stubd` keeps the
chip transport, the sockets, files and the LED, and becomes a host of
`dect_base`; the controller is the other host, over the mail tunnel.

## Stages

Each stage keeps `make check`, `make cross` and the golden traces green,
and changes no byte on the air unless it says so.

| Stage | What | Proof |
|---|---|---|
| **S0** | Daemon-level golden traces: a test build of `dect-stubd.c` with the chip replaced (mails in at `handle_reg_mail()`, out at `send_mail()`), scripted from a handset's NWK messages as `tests/test_trace.c` does for `dect_link` | Registration, locate, an incoming and an outgoing call, list access: the mails the daemon sends today, byte for byte |
| S1 | The handset records and the subscription store behind callbacks (`dect_base` holds them; the files stay in `dect-stubd`) | S0 traces unchanged |
| S2 | Registration and locate into `dect_base` | S0 traces; bench: register and relocate one handset |
| S3 | Calls: in, out, internal, parallel, side links, paging | S0 traces; bench: card A calls |
| S4 | Lists, MWI, date and time, CLIP, missed call notification | S0 traces; bench: card A lists |
| S5 | The control socket verbs as `dect_base_ctl()` | `test_ctl` and S0 |
| S6 | The controller's cgo host (opendectcell-controller C3) over the mail tunnel | a handset registers through the controller |

Bench time is needed from S2 on: each stage is one bench session on the
7412, with the card's outcomes written before the run.
