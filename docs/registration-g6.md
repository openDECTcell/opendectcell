# Handset registration (M6 / gate G6) — groundwork

Status: started 2026-10-18, right after G5 passed. Transport facts below are
**observed on hardware** (spare 7520, FRITZ!Fon C4 registration attempt). The
NWK/MM/DSAA protocol itself is **public** (ETSI EN 300 175-4/-5/-7; DSAA was
reverse-engineered and published — deDECTed.org, the CT-RSA 2010 paper, osmocom
`libdect`, and the Linux kernel `net/dect` stack). G6 builds from those public
sources, **not** from AVM material, so it stays outside the clean-room boundary.

Identity note: the captured C-plane bytes, the handset tag and the RFPI are
identity data and are **redacted** here (shown as placeholders). Never commit them.

## What a C4 registration attempt looks like on our interface (observed)

After the base is up (`0x015B:01` + `0x022B:41`, see `mac-bringup.md`) and the
user triggers "register to Basis 3" on the C4, the STUB reports, via the control
slot, a **connection-oriented** access (not the short-form `0xF604`/`0x0E0D`):

```
rx 0x0200  (dst 0x03) CONNECTION_ESTABLISHED  {handle=0, wideband=0}
rx 0x0243  (dst 0x03) CONNECTION_PARAMETER    {handle=0, 0x29, 0x00}
rx 0x0242  (dst 0x10) AUDIO_CHANNEL_STATE     {handle=0, 0,0,0}
rx 0x0203  (dst 0x03) SIGNALLING_SEGMENT      (15 param bytes, see below)  ×N
rx 0x0201  (dst 0x03) CONNECTION_RELEASED     {handle=0}
```

The handset sends its first C_S (Cs) channel segment, then **retransmits the
exact same segment several times** and the connection is released — because our
side never acknowledges it. This is DECT LAPC **acknowledged** C-plane: the PP
will not advance to the next segment until the FP acknowledges the first. So G6
cannot be skipped at the DLC level; we must run the LAPC handshake to even see
the rest of the `ACCESS-RIGHTS-REQUEST`.

### `0x0203` SIGNALLING_SEGMENT param layout (observed, 15 bytes)

| Off | Size | Field | Observed |
|---|---|---|---|
| 0 | 1 | handle | `00` |
| 1 | 1 | marker | `01` |
| 2 | 5 | **C_S segment** (40-bit Cs tail) | `<cs[5]>` (redacted; same in every retransmit) |
| 7 | 2 | zero | `00 00` |
| 9 | 3 | handset tag (PMID-like) | `<tag[3]>` (redacted) |
| 12 | 1 | handle | `00` |
| 13 | 2 | ref/frame (first seg only) | non-zero on seg 1, `00 00` on retransmits |

This matches COMMAND_SET §8.2 `{handle,0x01,data[5],00,00,tag[3],handle}` plus a
2-byte trailer. The **5-byte `data` is one Cs segment**; reassemble consecutive
segments into an LAPC frame (ETSI EN 300 175-4), whose I-field is the NWK message.

## The G6 build

Architecture — new `libopendectcell` modules, injected transport + clock so they
unit-test on the PC like the M4 modules (`dect_mail`/`dect_session`):

0. **`dect_cs` (STUB-facing C-plane transport).** ✅ Built 2026-10-18
   (`src/libopendectcell/dect_cs.{c,h}`, PC-tested). Decodes inbound `0x0203` into
   `{handle, tag, 5-byte Cs segment}`; encodes outbound C-plane as `0x0244`
   APPEND_CONNECTION_DATA (per-connection buffer, ≤205 B) + `0x0202`
   TRIGGER_SIGNALLING [CR COMMAND_SET §6.4/§8.2]. **Outbound path confirmed:**
   C-plane signalling goes through `0x0244`/`0x0202`, *not* `0x015C`
   SEND_ACKED_DATA — the latter is 40-byte **B-field** reliable data addressed
   by tag [CR COMMAND_SET §6.4]. The STUB segments the appended byte stream into
   Cs over the air; inbound it hands us one 5-byte segment per `0x0203`, so
   reassembly is the host's job (next module).

1. **`dect_dlc` (LAPC, C-plane, acknowledged).** Reassemble inbound Cs segments
   (from `dect_cs`) into LAPC frames; generate acknowledgements and outbound
   frames; produce the outbound byte stream for `dect_cs_append_mail`. ETSI
   EN 300 175-4. Reference: kernel `net/dect/dlc_*`, osmocom `libdect` DLC.
2. **`dect_mm` (NWK Mobility Management registration FSM).** Drive
   `ACCESS-RIGHTS-REQUEST` → `AUTHENTICATION-REQUEST` (RAND/RS) →
   `AUTHENTICATION-REPLY` (RES) → `ACCESS-RIGHTS-ACCEPT`. ETSI EN 300 175-5.
   IEs: PORTABLE-IDENTITY (IPUI), FIXED-IDENTITY (ARI/PARK), AUTH-TYPE, RAND, RS,
   RES. Reference: kernel `net/dect/mm_*`, osmocom `libdect` `dect_mm_*`.
3. **`dect_dsaa` (authentication crypto).** A11/A12/A21/A22 + B1/B2 (KSG),
   key allocation from the PIN (AC) and PP authentication. Public: deDECTed.org,
   CT-RSA 2010 paper, osmocom `libdect` DSAA/DSC source.

Daemon wiring (`dect-stubd`): feed inbound `0x0203` (and `0x0200`/`0x0201`) into
`dect_dlc`; send outbound C-plane via host→STUB `0x0202` TRIGGER_SIGNALLING /
`0x0244` APPEND_CONNECTION_DATA / `0x015C` SEND_ACKED_DATA (COMMAND_SET §6.4);
store the resulting subscription with `0xF605` sub-op 2 (slot 0..5). The base
must be beaconing with registration open the whole time.

A background research pass (public DECT FP registration flow + DSAA + the exact
LAPC/Cs framing, from ETSI + kernel `net/dect` + osmocom `libdect`) was started
this session to serve as the implementation spec; re-run it if its report was not
saved. Then: implement 1→3, unit-test on PC, drive on hardware, retry the C4.

## Hardware progress (2026-10-18, FRITZ!Fon C4)

The whole stack is built and PC-tested (`dect_cs`/`dect_lc`/`dect_lapc`/`dect_nwk`/
`dect_dsaa`/`dect_mm`), wired into `dect-stubd -g -P <pin>`. Driven against a real
C4 with instrumented traces. Protocol framing bytes below are safe; handset
identity (IPUI, tag, RFPI) is redacted and must never be committed.

**Solved — the DLC link now works end to end:**
- The STUB reports the incoming connection as `0x0200` → `0x0243` → `0x0242`,
  then delivers the handset's Cs signalling as `0x0203` segments.
- The handset opens the link with an **empty LAPC I-frame on LLN=2** (a class-B
  request, `a1 00 01` + Lc checksum). We **refuse class B → class A (LLN 1)** and
  confirm establishment with an RR carrying **NLF set and N(R)=1** (`91 21`).
- **Critical fix:** do **not** send `0x0202` after a C-plane reply — in AVM's host
  driver `0x0202` is the *abnormal link release* primitive, and sending it dropped
  the connection after ~40 ms [CR AVM/Q-0010 analysis]. Reply with `0x0244` only.
  With that, the connection stays up (60 s+).
- Our own I-frames use **NLF clear + P/F=0** (class A, EN 300 175-4 §9.2.1.1).
- Result: the C4 sends its **full ACCESS-RIGHTS-REQUEST** (MM msg `0x44`, a 49-byte
  NWK message with PORTABLE-IDENTITY etc., reassembled from 11 Cs segments) and
  **acknowledges our replies** (`13 21` = RR N(R)=1, a PT response).

**Open — the MM exchange does not complete (no PIN prompt):**
- We have tried replying to ACCESS-RIGHTS-REQUEST with **AUTHENTICATION-REQUEST**
  (`0x40`) and with **KEY-ALLOCATE** (`0x42`). In both cases the handset
  **LAPC-acks** our I-frame but sends **no MM response** and shows **no PIN
  prompt**, then times out ("Anmeldung gescheitert").
- So it is an MM-content / transaction-level issue, not DLC. Leads for next
  session:
  1. **Transaction identifier.** The request uses TV=0, F=0 (originator). We reply
     TV=0, F=1 (responder). KEY-ALLOCATE may need to be a *new* FP-initiated
     transaction (own TV, F=0) rather than a response in the PP's transaction.
  2. **Decode the captured 49-byte ACCESS-RIGHTS-REQUEST** fully (it is in the
     `-p` trace) — especially its AUTH-TYPE IE (observed content `02 48 00`;
     `0x48` = key type AC + IPUI/PARK flag, but octet 3 = `0x02`, not DSAA `0x01`)
     and any SETUP/TERMINAL-CAPABILITY / IWU IEs — to match the exact algorithm
     id, key/code numbers and IE set the C4 expects.
  3. Try replying with **ACCESS-RIGHTS-ACCEPT directly** (PIN-less GAP accept) as
     an A/B test, to see whether the C4 registers without authentication.
  4. Get the **exact AVM MM response** (message type, IEs, TI handling) after
     ACCESS-RIGHTS-REQUEST via the an external review/hardware-analysis route (the clean-room
     Q-0010 was blocked by the safety filter; the user relayed the DLC answer as
     `the clean-room record/Q-0010_AVM_Analyse.md`, which did not fully trace the MM
     sequence — ask specifically for it).
- The crypto is ready and Annex-K-verified; `dect_mm` implements the
  key-allocation flow (ACCESS-RIGHTS-REQUEST → KEY-ALLOCATE → PP
  AUTHENTICATION-REQUEST → AUTHENTICATION-REPLY → ACCESS-RIGHTS-ACCEPT) and the
  daemon drains its message queue one per free LAPC window.

## AVM host-side MM facts to match (from A-0011/A-0012/A-0014) — not yet coded

These are host-driver (`avm_dect.ko`) facts to align our `dect_mm` with. All are
clean-room answers; cite `[CR A-00NN]`. No identity bytes here.

**Choosing KEY-ALLOCATE vs AUTHENTICATION-REQUEST.** On ACCESS-RIGHTS-REQUEST
(`0x44`) AVM reads the AUTH-TYPE IE's first payload octet (the "mode"); it accepts
`0x01` and `0x02`. Mode `0x02` → the FP sends **KEY-ALLOCATE** (`0x42`) first;
other modes → an authentication path. The C4's request carried mode `0x02`, so
KEY-ALLOCATE-first is correct for it. [CR A-0012 F1] (AVM routes this via an
internal host mail `0x0F9C` to task 5 — that is host-internal, **never** sent to
the STUB.)

**KEY-ALLOCATE exact bytes (mode 2), 34 NWK bytes** — our current frame differs:
| IE | AVM | ours now | fix |
|---|---|---|---|
| ALLOCATION-TYPE | `0B 02 02 88` | `0B 02 01 11` | payload → `02 88` |
| RAND | `0C 08` + 8 | `0C 08` + 8 | ok |
| RS | `0E 10` + **16** | `0E 08` + 8 | **RS must be 16 bytes** |
[CR A-0011, A-0012 F5]. AUTHENTICATION-REQUEST's AUTH-TYPE uses a constant 2nd
byte `0x18` and `0A 03`/`0A 05` variants [CR A-0011].

**ACCESS-RIGHTS-ACCEPT (what we already build correctly).** `85 45 | 05 07 80 A8
<5 id bytes> | 06 07 A0 A5 <5 RFPI bytes>`; FIXED-IDENTITY is PARK type with the
RFPI copied verbatim — **no `>>1` transform** [CR A-0011, A-0012]. Transaction
octet: incoming `0x05` → reply `0x85` (same TV, responder F-bit) [CR A-0011/A-0012].

**Validation is stateful and IE-dependent.** AVM's `0x44` handler inspects
PORTABLE-IDENTITY (`0x05`), AUTH-TYPE (`0x0A`), FIXED-IDENTITY (`0x06`) and
`0x63`/`0x77`/`0x78`/`0x7B`/`0x7C`; a syntactically valid LAPC I-frame can still
be rejected silently at MM. A LAPC RR is **not** MM acceptance. [CR A-0012 F5]

**Storing the subscription after ACCESS-RIGHTS-ACCEPT.** On enrol AVM calls, in
host order, `0x45` ACCESS-RIGHTS-ACCEPT → then `0x0247` (task 2: {slot 1-based,
op 1, state byte}) → `0x020A` (task 2: {index 1..6, 16-byte record, 1 byte}).
[CR A-0012 F3] The `0xF605` sub-op 2 producer layout is {`F605` LE, sub-op 2,
`00`, index 0..5, 5-byte identity, 4-byte aux, 1 flag} = 15 bytes [CR A-0013 F3].

**Next-session order:** fix KEY-ALLOCATE (RS 16 bytes, ALLOCATION-TYPE `02 88`),
retry the C4, watch for the PIN prompt; then wire AUTHENTICATION-REPLY /
ACCESS-RIGHTS-ACCEPT and the `0x0247`+`0x020A` store.

## G6 ACHIEVED — a FRITZ!Fon C4 registers (2026-10-18)

The C4 registered successfully and shows "connected to Basis 3". The full,
hardware-verified MM sequence (identities redacted) is:

```
C4 → FP  05 44  ACCESS-RIGHTS-REQUEST   (AUTH-TYPE proposes DSAA2, algo 02)
FP → C4  05 42  KEY-ALLOCATE            ALLOCATION-TYPE 01 88, RAND 0C 08, RS 0E 08
C4 → FP  85 40  AUTHENTICATION-REQUEST  (AUTH-TYPE algo 01, RAND_P, RES1)
FP → C4  05 41  AUTHENTICATION-REPLY    (RES2)                 -- RES1 verified, DSAA1
FP → C4  85 45  ACCESS-RIGHTS-ACCEPT    (PORTABLE-IDENTITY, FIXED-IDENTITY=PARK)
C4 → FP  05 54  LOCATE-REQUEST          (IPUI, PARK, LOCATION-AREA, caps)
FP → C4  85 55  LOCATE-ACCEPT           05 05 A0 94 <TPUI> + 07 01 64
C4 → FP  05 5d  TEMPORARY-IDENTITY-ASSIGN-ACK                  -- registration complete
```

**Corrections to the pre-hardware assumptions above:**

- **It is DSAA1, not DSAA2.** The C4 *proposes* DSAA2 (AUTH-TYPE algo `02`) but
  accepts a DSAA1 KEY-ALLOCATE and then authenticates with DSAA1 (its
  AUTHENTICATION-REQUEST comes back with algo `01`). So ALLOCATION-TYPE is
  `01 88` and RS is **8 bytes** (`0E 08`), *not* `02 88` / 16-byte RS. Our
  existing DSAA1 crypto verifies RES1 and produces RES2. [hardware]
- **The decisive fix was the transaction F-bit.** KEY-ALLOCATE and
  AUTHENTICATION-REPLY are FT-*initiated* (`05 ..`, F=0); ACCESS-RIGHTS-ACCEPT and
  LOCATE-ACCEPT are responders (`85 ..`, F=1). KEY-ALLOCATE as `85 42` was
  LAPC-acked but ignored at MM; `05 42` made the C4 proceed. [CR A-0022, hardware]
- **RS must be a fresh random seed** (was zeros).
- **LOCATE-ACCEPT must assign a TPUI** via PORTABLE-IDENTITY
  (`05 05 A0 94 {0, 0x11, slot}`) plus LOCATION-AREA; a LOCATION-AREA-only reply
  drew `05 5f` (ASSIGN-REJECT). [CR A-0023, hardware]
- No PIN prompt appears when the base PIN is the C4 factory default `0000`.

**Still open (stability / persistence, not basic registration):**

- After registration the C4 repeatedly re-connects and re-LOCATEs. We do **not**
  yet write the chip subscription store (`0x0247` then `0x020A`, per A-0023/A-0012)
  nor `0xF605`, so the base does not persist the handset for idle standby and
  paging. The 16-byte `0x020A` record layout is not yet known (must not be
  zero-filled). This is the next task for a stable, call-capable registration.
- Class-A (modulus-2) reassembly garbles the C4's retransmitted ~72-byte
  multi-segment LOCATE-REQUEST; harmless once the C4 is satisfied, but worth
  hardening [CR A-0023 Q5].
