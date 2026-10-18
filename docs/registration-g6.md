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

## The G6 build (next session)

Architecture — three new `libopendectcell` modules, injected transport + clock so they
unit-test on the PC like the M4 modules (`dect_mail`/`dect_session`):

1. **`dect_dlc` (LAPC, C-plane, acknowledged).** Reassemble inbound Cs segments
   into LAPC frames; generate acknowledgements and outbound frames; segment
   outbound NWK messages into Cs. ETSI EN 300 175-4. Reference: kernel
   `net/dect/dlc_*`, osmocom `libdect` DLC.
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
