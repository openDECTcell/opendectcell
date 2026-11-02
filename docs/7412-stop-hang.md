# The 7412 stop hang: what is known, what to try

Status 2026-11-02. A FRITZ!Box 7412 running the service sometimes loses its
network when the service is stopped, and has to be power-cycled. This page
collects the evidence (from [bench-later.md](bench-later.md), U8e and U8f), the
hypotheses with the result each one predicts, and the fix to try. A guard that
reboots the box after the fact is **not** the fix: the aim is that the stop
never kills the network.

## What is established

- Four hangs in about 190 stops (two ordinary stops from "radio up", two stops
  2 to 3 s after a quick start); none in the last 80 stops of one campaign.
  One hang is not a rate, and nothing explains why the last 80 passed.
- The kernel and procd keep running: the hardware watchdog (works, tested) is
  fed for as long as the box is silent, and `panic_on_oops` did not fire.
  Only the network is gone.
- In the hangs from "radio up", the last daemon line is `radio parked`, the
  network answers up to about 180 ms after the park mails and then not any
  more, and no kernel marker of the module's teardown follows: the module was
  never asked to stop. The SSC engine (the DMA rings and the 1 ms collectors on
  each CPU) was running the whole time.
- `eth0` down and up and a network restart do not bring it back.
- The 7520 does not use this module and has not hung.

## Hypotheses and what each one predicts

**H1: the chip stops or changes what it clocks onto the SSC while the DMA
runs, and that stalls the DMA controller the Ethernet driver shares.** The two
hang entries (park mails; a quick-start stop, which resets the chip) are both
"the chip's side of the link changes while our DMA is armed". Predicts: a hang
can be provoked without the daemon's park, by holding the chip in reset (GPIO
545 to 0) while the module's engine runs, and the rate rises with the number of
cycles. Does not hold if 300 such cycles pass.

**H2: a race on the shared DMA channel-select register between this module and
the Ethernet driver.** Predicts: hangs also happen with the chip held in reset
and no radio (module only), at a low rate, and `dmesg` shows nothing. Does not
hold if 300 start/stop cycles of the module alone (no chip) pass.

**H3: the Ethernet DMA channel is blocked by an SSC burst that cannot finish
because the chip's clock is gone.** A variant of H1; separated from it by the
fix below (a fix that disarms the DMA before the chip's state changes helps
under H1 and H3, and not under H2).

## Experiments (run with the maintainer's OK: each hang power-cycles both boxes)

1. **Observation, no hang needed.** Service up, then a normal stop, with the
   module's `u8f:` markers and the SSC and DMA registers read (`ssc-pcm-test
   regs`) just before and right after the park mails. Shows whether the clock,
   the frame or the FIFO state change at the park. 10 stops.
2. **H2 first, because it needs no chip:** `ssc-pcm-test slots=36 hold=3`
   in a loop, 300 cycles, chip held in reset. A hang names H2.
3. **H1:** service running; hold the chip in reset (`echo 0 >
   /sys/class/gpio/gpio545/value`) at random moments 0.5 to 5 s apart, then
   start the service again; 300 cycles. A hang names H1 or H3.
4. Each run ends at the first silence (as `tools/bench-stop-race.sh` does) and
   the box's log goes to the PC's syslog sink first.

## The fix to try, under H1 and H3

Disarm the DMA before the chip's side changes, and re-arm it after:

- before the park mails and before any chip reset, stop the SSC DMA requests
  (`DMACON` off) and the collectors, then re-arm them once the chip answers a
  read again;
- have the daemon ask the module for this ("quiesce") instead of sending the
  park mails through a running DMA engine;
- open design question: the transport's interrupt path carries at most 8
  words a frame and the 7412's frame is 18 words (the module's notes), so
  the chip's reply to the park cannot be read without DMA by that path. Either
  the reply is read by polling the RX FIFO during the quiet window, or the
  window is cut short by arming only the receive channel.

If H2 is the cause, the fix is in the module's use of the DMA API (the module
must take the same lock the Ethernet driver uses for the channel-select
register, or use the channel through the API only).

Whatever is found, record the result here and in [bench-later.md](bench-later.md).
