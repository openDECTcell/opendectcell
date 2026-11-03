# Bench card: gate G5 on a FRITZ!Box 7362 SL (2026-11-01)

Gate G5 (`ROADMAP.md`): **a handset sees the base.** The radio transmits:
run only with the maintainer's agreement and a handset searching (given
2026-11-01). Run as `/tmp/g5.sh` on the box after G2-G4
([bench-7362sl-g2.md](bench-7362sl-g2.md), [bench-7362sl-g3-g4.md](bench-7362sl-g3-g4.md)),
same modules and firmware, `dect-stubd` from `d697936` (MIPS build),
without `-g` (no host stack): the bring-up mails go out as `-M` after the
contact.

A handset searches for a new base only in its registration menu; one
already registered elsewhere shows "Keine Basis" and does not search.

## Results (*observed on hardware*)

| Run (box time) | Mails after the contact | Handset (registration search) |
|---|---|---|
| 16:43, 120 s | `015B 00`, `015B 01`, `022B 41` | not searching ("Keine Basis") |
| 16:50, 300 s | the same | **searched, found no base** |
| 16:56, 300 s | `015B 00`, then `0153 00`, `0169 07`, `0157 00` from task `0x14`, then `015B 01`, `022B 41` | **found the base and asked for the system PIN** |

Each run ended with the park (`022C 01`, `015B 00`), then the chip held
in reset and the modules unloaded. No handset reached the chip in any
run (no link mail); in the third run the handset stopped at the PIN
prompt, as nothing on the base side could complete a registration.

**G5 passed** with the third run. On the 07.18 STUB the 7520's two-mail
bring-up (radio on, open registration, [mac-bringup.md](../mac-bringup.md)) did
not give a beacon a handset finds; stock's three configuration mails
before the radio did. Those are what `dect-stubd -g` sends (the
"stock's MAC bring-up" comment in `dect-stubd.c`), so the service's path
is the working one. With one run of each, it is not shown which of the
three mails matters.

The chip's `0xF602` reply shows the steps: its second parameter byte is
`00` until open registration and `08` after it, and `00` again after the
park's close; its third byte reads `02` after `0169 07` and `01` otherwise
(meaning not established). `0xF623` radio-status reports arrive every
5 s with the radio on or off on this build, so they do not show the
beacon on their own; their level bytes changed during the third run.
