# Capacity: handsets, concurrent calls, and how to go beyond six

Status: analysis only, 2026-10-18. Nothing here is on the critical path before
G7 — it records a finding and a plan so we don't lose it. Sources are the
clean-room firmware review and the reconstructed command set; **nothing was
analysed here** and none of these limits is yet proven on hardware.

## The two different "sixes"

"Six phones" conflates two independent ceilings. Keep them apart:

1. **Registered (subscribed) handsets** — how many phones can be *paired* to the
   base. The STUB firmware carries six subscription slots (`0xF605`, slots 0..5)
   and six handset records (`0x020A`, records 1..6).
   [CR SC14441_Firmware_Review §5]
2. **Concurrent active calls** — how many conversations can run *at once*. The
   firmware has six connection contexts and six audio-channel entries in its
   tables, but the review is explicit that the DSP/PCM paths do **not** guarantee
   six simultaneous conversations. Our own working assumption from M3/M4 is a
   STUB audio limit of roughly three PCM voice channels. [CR SC14441_Firmware_Review §5]

For a home base, **six registered handsets is the ordinary GAP / FRITZ!Box
number** — it matches what AVM's own product offers. The ceiling that actually
bites a busy local is concurrent *calls*, and that is a DSP/radio constraint,
not a table you can simply enlarge.

## These are firmware table sizes, not a proven chip hardlimit

The review stresses that the sixes are **six-slot tables observed in the Intel-HEX
image**, not independently analysed bound checks, and there is **no evidence
of a chip hardware limit**. [CR SC14441_Firmware_Review §5, "Grenzen dieser
Untersuchung"] We therefore do not yet know whether six is a hard ceiling or just
the table size AVM compiled in.

Other table sizes that are easy to misread as a handset count, but are not:

| Table | Primitive | Size | What it actually is |
|---|---|---|---|
| Admission list | `0x015F` | numbers 1..439 | Not the subscription table [CR SC14441_Firmware_Review §5] |
| Pending paging list | `0x0300` | 40 entries | Simultaneous paging operations, not registrations |
| Bearer-slot mask | `0x017C` | 8 candidates, default `0x3F` | Allowable slots; still only six connection contexts |

## Empirical test (cheap, do it once G6 passes)

Before any engineering effort: register a **seventh** handset and watch the STUB.
If subscription slot allocation (`0xF605` / `0x020A`) refuses the seventh, six is
the real registration ceiling for this firmware. If it accepts it, the table is
larger than assumed. This costs one test card and settles the premise.

## Options considered for exceeding six

1. **Modify the AVM STUB firmware — rejected.** It is proprietary CR16C machine
   code we may not redistribute; the STUB is re-uploaded from the user's own AVM
   image at every boot, so a patch means shipping a modified AVM blob. It breaks
   the clean-room, publishable premise of the project (AGENTS.md §2) and we have
   no CR16C toolchain in the clean room anyway. Not viable.
2. **Write our own SC14441 firmware — rejected for now.** The hardest,
   least-documented part of the whole domain (DECT burst timing, the DIP radio
   interface, RF calibration) is exactly what AVM's STUB does for us. Using the
   STUB is the shortcut that makes openDECTcell feasible. Replacing it is a different,
   far larger project with no public documentation for the radio path.
3. **Host-side slot virtualisation — the clean-room-safe lever (preferred, post-G7).**
   We own the host and can manage far more subscription data than the chip's six
   slots. An *idle* handset only needs its subscription resident in a chip slot
   when it actually reaches the base. If an unknown-handset access event
   (`0xF604`) becomes visible to the host early enough to load that subscription
   into a free slot (`0xF605` / `0x020A`) **before** the connection request is
   refused, we can register many handsets and keep only the active set of ≤6
   resident. This is precisely the open question the review flags.
   [CR SC14441_Firmware_Review §5 "Zentrale Unbekannte", §7.2] It changes **no
   firmware** and stays entirely on our side of the clean-room boundary.
4. **More cells — the scalable answer (already roadmapped, M11).** Real capacity,
   for both registrations and concurrent calls, comes from more coordinated base
   stations under the controller, not from one SC14441. Multi-cell is already the
   committed architecture (ROADMAP M11).

## Host-driver confirmations (A-0013 / A-0014)

Analysis of AVM's host driver (`avm_dect.ko`, 7520) sharpens the picture:

- **The "439" ceiling is a TPUI namespace, not a handset count.** The boot log's
  `avmTpui 16/390, etsiTpui 406/40` means two TPUI number ranges (AVM 16..405,
  ETSI 406..445); these explain the `0xF602` fields and the `0x196..0x1BD` special
  range. They do not bound simultaneous subscriptions, which stays six.
  [CR A-0013 F2]
- **Extended/ULE accounts are a real second mechanism** (16-bit TPUI +
  `0x015F` ADMIT_HANDSET admission from persisted extended records), separate from
  the six `0xF605`/`0x020A` on-chip GAP slots. But `a stock account routine`
  returning `6` is an account-*class* code, **not** a 7th subscription slot, and
  nothing here shows an ordinary GAP voice handset using the ULE path. So >6
  ordinary GAP handsets is still not demonstrated; host-side virtualisation
  (option 3) remains the lever. [CR A-0014 F1/F2, A-0012 F2]
- **Concurrent voice is likely below six.** The host reports `DECT=4` logical
  DECT codec/channel resources (`PCM-link`), and the TDM timing (`FS 8000 Hz`,
  CLK ~1.026 MHz) confirms 8×16-bit PCM frames. Six connection contexts ≠ six
  simultaneous voice paths; measure `PCM-link` allocation before claiming a number.
  [CR A-0013 F6]
- **Chip-identity caveat:** the 7520 boots the `_441` STUB image, but the filename
  does not prove SC14441 silicon — some reports identify the 7520/7530 DECT chip
  as **SC14448**. Treat "SC14441" in our docs as the firmware family, not a
  confirmed part number. [CR A-0013 F1]
- **DSAA2/AES is present in the host** (`a stock key routine` → AES; `ExtSecurity=1`,
  `AVMULEAES=1`, `CATIQ20SUPPORT=1`). A future controller should keep
  authentication/key-negotiation capability independent of on-air cipher state.
  [CR A-0013 F7, A-0014 F7]

## Open questions

- Is `0xF604` (unknown-handset access) delivered to the host early enough, and
  with enough identity, to load a subscription before the chip refuses the
  connection? (Decides whether virtualisation in option 3 is possible at all.)
- Where exactly are the `>= 6` / table-index checks for the connection-context
  pool, and is that pool relocatable? (A firmware-RE question, deferred.)
- Real concurrent-call ceiling: how many of `0x0230` / `0xF626` DSP/PCM voice
  paths can run at once on the 7520 (IPQ4019) TDM bus? Measure one-to-six calls.
