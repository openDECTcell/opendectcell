# Diagnostics: what a DECT base should be able to tell you

**Status: design, not built.** Written 2026-10-23 after the maintainer pointed
at AVM's **DECT Monitor**, a feature of stock FRITZ!OS. Roadmap items are in
`ROADMAP.md` M8/M9 and in the controller's C5/C7.

## Why this is a feature and not a debugging aid

Every hard question of the week before this was written is a field on AVM's
Monitor screen: long-slot vs full-slot, the time slot in use, the carrier,
field strength, signal quality, which handset holds which subscription. We
were inferring, across multiple withdrawn hypotheses and several bench runs,
things a shipping product simply prints.

Two consequences:

1. **The information is obtainable.** A displayed field means the host can
   read it from the chip.
2. **Users want it too.** "Why does this handset drop in the kitchen" is the
   same question as "why will the A540 not attach", and the answer comes from
   the same place.

So this belongs in the product, not in a scratch script.

## The data

Per registered handset:

| field | source | status |
|---|---|---|
| identity (IPUI) | our own MM records; per-slot chip identity is `Q-0054` §1 | partly available |
| subscription slot, and whether the chip really holds it | slot audit at `0x9518 + (slot−1) × 0x11` `[CR A-0085]` | **built** |
| connected / idle | `0x0200` CONN_UP/DOWN | available |
| manufacturer, model | NWK `<<MODEL-IDENTIFIER>>` (0x78) | available, not captured |
| codecs offered / in use | `<<CODEC-LIST>>` we send, `0x0230` codec byte | available |
| encryption on this call | we run with `-C`; chip side is `Q-0049` | not available |
| **slot type (long / full)** | `dir1+6` = `0x11718`, `0x28`/`0x50` `[CR A-0079]`; `Q-0054` §3 asks what the Monitor uses | candidate |
| **time slot in use** | `Q-0054` §2/§5 | not available |
| **carrier (Tx/Rx frequency)** | `Q-0054` §2/§5 | not available |
| **field strength** | `0xF623`, ten values ≈ per-carrier RSSI; `Q-0054` §4 | received, undecoded |
| **signal quality** | `Q-0054` §4 | not available |

Per base: RFPI (have), radio parked/up (have), the dummy bearer's slot,
carrier, radio power, DECT Eco state.

Per call, for history rather than live view: duration, codec, release reason,
and the quality figures above sampled over the call.

## Where each piece lives

### `libopendectcell` — the data, no I/O

A `struct dect_diag` per link plus a base-wide one, filled from what the
library already sees (CC/MM state, codec, identities) and from chip values the
caller hands in. The library must not read the chip itself: it has no
transport and that stays true.

So: `dect_diag_from_link()` for the protocol half, and plain fields the daemon
fills for the radio half. One struct, one place, so the LuCI app and the
controller cannot drift apart in what they mean by "quality".

### `dect-stubd` — the chip half

Periodic reads of the addresses above into that struct, and a way out:
a JSON line on a UNIX socket is the obvious one, since both consumers are
out-of-process. The slot audit and the UAK read-back already exist and should
feed the same structure rather than only printing.

### LuCI — the user-facing Monitor

A status page: one row per registered handset with the fields above, a base
section, and a live-ish refresh. The point is that a non-technical user can
answer "is this handset in range and what is it doing" without reading a log.

Deliberately **not** a debugging console — raw mails stay in `dect-stubd -v`.

### Controller — across cells

The single-cell view is the LuCI one. The controller's job is what one cell
cannot see:

- **which cell a handset is on**, and its level there, which is the input to
  any future roaming decision;
- **history**: quality per call in `call_log`, so "it was bad last Tuesday" is
  answerable;
- **alerting**: a handset whose subscription audit fails, or that has not been
  seen for *n* hours, is a support call waiting to happen.

The cell→controller protocol needs to carry the struct; `docs/cell-protocol.md`
has no diagnostics message today.

## Order, and what is blocked

1. **Now, no new facts needed:** the struct, the protocol-side fields, the
   slot audit and UAK read-back feeding it instead of only printing, and
   `<<MODEL-IDENTIFIER>>` captured from the handset.
2. **After `Q-0054`:** slot, carrier, field strength, signal quality — the
   radio half, which is most of the user-visible value.
3. **After `Q-0049`:** encryption state, which we cannot show because we do
   not yet do it.

Step 1 is worth doing before step 2 lands: it is the half that does not depend
on an answer, and it gives the chip values somewhere to go when they arrive.
