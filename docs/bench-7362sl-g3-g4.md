# Bench card: gates G3 and G4 on a FRITZ!Box 7362 SL (2026-11-01)

Gate G3 (`ROADMAP.md`): **a request mail gets the matching answer.** Gate
G4: **the audio path**: the STUB's test tone in a voice slot, or the
loop-back. On the 7520 the literal test tone was never heard (this STUB
build appears to have no usable tone generator, ROADMAP M4), and the
loop-back on a contact-only bench was the audio-path proof
([stub-pcm-control-channel.md](stub-pcm-control-channel.md), "Loop-back
works"). The same proof is used here.

**Radio off throughout:** no `-g`, no `0x015B 01`. Every session ends; the
last one parks (`0x022C 01`, `0x015B 00`), then the chip is held in reset
and the modules are unloaded.

Run on the box as `/tmp/g4.sh` after G2 ([bench-7362sl-g2.md](bench-7362sl-g2.md)),
same modules and firmware, `dect-stubd` from `d697936` (MIPS build).

## G3: request and answer

Every daemon session of G2 and G4 opened with the contact request
(`0xF600`) and got the matching answers (`0xF601`, `0xF602`) within 12 ms,
followed by the periodic `0xFFF1`/`0xF618`/`0xF623` every 5 s; no sequence
gaps, checksum or framing errors in any session. That is the 7520's G3
evidence of 2026-10-17. **G3 passed** (*observed on hardware*). Explicit
version or capability requests are still not in `dect-stubd` (ROADMAP M4).

## G4: loop-back

One upload, then four 12 s sessions of

    dect-stubd -d /dev/tdm-pcm0 -c 8 -v -t 12 -a capall,txidle=0x5a \
        -w /tmp/g4-RUN.raw -M "0x22:0x23:0xfff1:0900000000" \
        -M "0x22:0x23:0xfff1:090N000000"

with N = 0, 1, 2, 3: every lane but the control lane transmits `0x5A`
(`txidle`; `voiceidle` applies only during a call), all loops are switched
off, then loop channel N on [CR A-0004: `0xFFF1` sub-command `0x09`,
32-bit channel, 0 = off]; every receive lane is captured from start-up.

| Run | Receive lane 1 | Lane 2 | Lane 3 |
|---|---|---|---|
| loops off | `FEFE` (chip idle) | `FEFE` | `FEFE` |
| loop 1 | **`5A5A`** from 23 ms on | `FEFE` | `FEFE` |
| off, loop 2 | `FEFE` (back within 6 ms) | **`5A5A`** | `FEFE` |
| off, loop 3 | `FEFE` | `FEFE` (back) | **`5A5A`** |

Lanes 0 (the control lane, blanked by the capture), 6 and 7 read `FFFF`,
lane 5 a constant `080C` and lane 4 a few slowly changing values
(`D7D7`, `D7D4`) in every run, loop or not. **The loop-back works:**
channel N returns transmit lane N in receive lane N (after the daemon's
lane delay); off returns the chip's idle pattern; without a loop nothing
of ours comes back. The 7520 mapped channels 1/2/3 to slots 0/3/2; on
this transport the numbering is the daemon's (control lane 0, speech
lane 1, side lanes 2 and 3), so channel N lands on the lane the daemon
already gives call N. **G4 passed** on the loop-back criterion, as on the
7520 (*observed on hardware*). Not done: a real tone through the loop
(the 7520's A-law sine), and the test tone (channel 15).

Transport: 0 frames lost in every session (~98,000 frames each).

**A limit found on the way:** the loop setting stays in the chip after the
daemon exits. A first try switched loops 1, 2 and 3 on in turn without
switching them off; then no lane carried the chip's idle pattern, the
daemon's lane-offset measurement (which looks for `FEFE`) refused to
start ("no idle lane"), and so did the park. The radio had never been
on, and the chip went into reset. Lab-only (nothing outside a bench run
sends the loop mail), but a daemon start on an FSC-aligned transport
could fall back to offset 0 instead of giving up.
