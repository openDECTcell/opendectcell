# Two calls at once: design

Status: design, 2026-10-25 afternoon. The maintainer's next focus after the
bench day. Nothing here is on the air yet. Chip facts asked: clean room
Q-0063.

## Where we are

One call at a time. The **main link** (`s->reg`, `struct reg_ctx`) carries
the call and the only audio path; **side links** (`struct side_link`) serve
location registration and list access for other handsets and refuse speech
with "insufficient resources" (`dect_cc_set_busy()`). The audio **lane** --
which TDM word carries the call, the play position and the two-frame phase
lock of the G.722 downlink, the delay ring of the loop-back, the cadence
counters -- is global state in `struct stubd`, used by the frame loop for
TDM word `audio.tone_word` (0).

## What the chip side needs (answered: A-0124)

From two stock captures with two calls up: the second connection binds
`0x0251 {h, 02, 2b}`, `0x0230 {h, 42, 04 00, 00, 00}`, `0x022D {h, 00}`. Only
the channel k changes: the flags nibble is a **bit** (k = 1, 2, 3 -> 0x1,
0x2, 0x4), the mask is `1 << k`, the `0x0251` selector is k. Stock takes the
lowest free channel; the kernel refuses a bind whose bit or mask collides.
**Channel k's audio is on raw TDM word k XOR 1** -- 1 -> 0, 2 -> 3, 3 -> 2 --
both directions, wideband too (one word). **Three concurrent voice
channels**, narrowband or wideband. Internal calls in stock are two bearers
on two channels **bridged on the host**; nothing is bridged in the chip.

Implemented (`-a twocalls`, step 1-3 in the variant below): lane 0 untouched,
extra lanes for side links on channels 2 and 3.

## Software plan

1. **Lanes.** Move the per-call audio state out of `struct stubd` into
   `struct lane { int bound; uint8_t handle, channel; unsigned word; int
   wideband; play position, phase lock, loop ring, cadence counters }`,
   `lanes[3]`. The frame loop serves every bound lane on its own word.
   Lane 0 = channel 1, word 0 = today's behaviour, unchanged.
2. **Calls on side links.** A side link that receives a speech `{CC-SETUP}`
   (or answers our page) gets a lane of its own instead of the busy
   refusal, while lanes are free; the busy refusal stays for the case of
   no free lane. Call-control state per link (bound, codec, staged
   connect) moves next to the lane.
3. **A source per lane.** What a lane plays: the test file (own position),
   the loop-back (own ring), silence -- or **another lane** (step 4).
4. **Internal call.** Handset A dials an internal number (the Gigaset INT
   key; call class 0x9, or the digits of a slot): we page handset B, and
   when both are connected, **bridge** lane A's uplink to lane B's downlink
   and back. For two wideband calls the coded G.722 words can be copied
   through with the phase lock on each side; for mixed calls the host
   decodes and re-encodes (`dect_g722.c` has both directions).

Steps 1-3 can be tested with two handsets each on its own call (each
hears the test file); step 4 is the first feature a user sees -- a call
between two handsets without any SIP side.

**Implemented (step 4, `-a twocalls`):** a handset's SETUP with call class
9 and the target's slot in its digits (the INT key; GAP N.31, EN 300 444
§8.18-8.19) gets CALL-PROC; we page the target by its TPUI and, when it
answers the page -- or comes up on a side link without a page response, as
the A540 does -- place our SETUP there (basic service 0x98). Its ALERTING
gives the caller ringback (ALERTING), its CONNECT our CONNECT to the
caller; each call binds its own channel, and the host copies each party's
uplink word into the other's downlink. No phase lock is needed for the
copy: every coded word is held for two frames, so the other channel always
reads a whole one. Either side hanging up releases the other. CC never
connects a class-9 call by itself, even with a complete number. Limits: one
internal call at a time; the caller must be on the main link or a side
link and the target on a side link; both calls the same codec (no
transcoding yet).

## Test plan

| step | how | unattended? |
|---|---|---|
| 1 | one call as today (wideband and narrowband), audio unchanged | no -- needs an ear |
| 2 | second handset dials while the first is in a call; each hears the file | no |
| 3 | as 2 with one on loop-back | no |
| 4 | handset A presses INT, selects B; B rings, answers; both talk | no |

The lane refactor (1) goes first and alone, so a regression in the one
call that works today shows up before anything else changes.
