# Bench card: gate G1 on a FRITZ!Box 7362 SL (written 2026-11-01)

Gate G1 (`ROADMAP.md`): **the STUB boots** — both upload stages are
acknowledged — **and the PCM clocks (DCL, FSC) start.** This is the first
time openDECTcell boots the DECT chip on a 7362 SL.

**Only with the maintainer's agreement:** this boots the chip's firmware.
No mail is sent to it, so the radio stays off (it starts only on
`0x015B 01`, [mac-bringup.md](../mac-bringup.md)); at the end the chip is held
in reset anyway.

## What is known going in

- The firmware-load UART is the VR9 USIF, driven by `kmod-lantiq-usif-uart`
  (`/dev/ttyUSIF0`); tested on this box in loopback at 9600 to 460800 Bd
  with the pins claimed and released cleanly [CR 7362SL/A-0002, A-0003].
  Nothing has yet been sent over the real pins.
- The chip's TX line idles high while the module is loaded; the chip sent
  nothing in 5 s [CR 7362SL/A-0003].
- Reset is GPIO 30, low = reset [CR 7362SL/A-0001]; sysfs number **542**
  (gpiochip base 512).
- PCM pins: FSC GPIO 0 (**512**), DCL GPIO 40 (**552**), DI GPIO 41
  (**553**). In the stock board table they are inputs on the SoC side, so the
  chip should drive FSC and DCL [CR 7362SL/A-0001].
- The upload: stage 1 at 9600 Bd (mode 0x101 has bit 8, so the first 11
  attempts are "slow"), stage 2 at 230400 Bd (the first stage's file name
  has neither `_slow` nor `_fast`; [stub-loader-protocol.md](../stub-loader-protocol.md)
  §2-§3).
- `ubihealthd` reads NAND every 120 s with interrupts off for about 27 ms,
  longer than the USIF's receive FIFO lasts at 230400 Bd [CR 7362SL/A-0002
  §5]. It is stopped for the upload. (Since `kmod-lantiq-usif-uart`
  `f359106` the upload also passed with it running; see the results of
  that build below [CR 7362SL/A-0006].)

## 0. Prepare (workstation)

1. The firmware, from the official image (the user's step; the files are
   only copied, never inspected):

        python3 tools/dect-fw-extract.py FRITZ.Box_7362_SL-07.18.image

   Expect two `OK` lines: `dectfw_firstlevel_441-06.88-7412.hex` and
   `dectfw_secondlevel_441-07.18-7362sl.hex`.
2. Packages: openDECTcell from `main` (`OPENWRT=25.12.6 tools/release/build.sh
   lantiq/xrx200 OUT`: `opendectcell-*.apk`, `kmod-opendectcell-lantiq-*.apk`) and
   `kmod-lantiq-usif-uart-*.apk` from that repository's `deploy/`.
   `gpio-activity` cross-built with the SDK toolchain from
   `tools/gpio-activity.c`.
3. Copy to the box: the two `.hex` files to `/etc/opendectcell/firmware/`
   (create it), the three `.apk` files and `gpio-activity` to `/tmp/`.

## 1. Install and load (box)

    apk add --allow-untrusted /tmp/kmod-lantiq-usif-uart-*.apk \
        /tmp/kmod-opendectcell-lantiq-*.apk /tmp/opendectcell-*.apk
    /etc/init.d/opendectcell enabled && echo ENABLED || echo disabled   # expect disabled
    sha256sum /etc/opendectcell/firmware/*.hex     # 72f8e27a… and 8aae3b5e…
    insmod lantiq_usif_uart && insmod openDECTcell
    dmesg | tail -n 4                         # ttyUSIF0; "chip control loaded for avm,fritz7362sl"
    chmod +x /tmp/gpio-activity
    /tmp/gpio-activity 512 0,40,41            # before: expect no changes on any pin

## 2. Upload

    /etc/init.d/ubihealthd stop
    dect-fwload --uart /dev/ttyUSIF0 --dect-dev /dev/dect0 \
        --first /etc/opendectcell/firmware/dectfw_firstlevel_441-06.88-7412.hex \
        --second /etc/opendectcell/firmware/dectfw_secondlevel_441-07.18-7362sl.hex \
        --mode 0x101 --factory-mtd /dev/mtd2 --factory-offset 0x1985 \
        --listen 5 -v 2>&1 | tee /tmp/g1-fwload.log
    /etc/init.d/ubihealthd start

**Pass:** `upload OK`, with the stage-1 answer on a slow attempt and the
stage-2 checksum acknowledged; the log line about the factory block reports
TLVs patched (block at urlader 0x1985).
**Fail:** send `/tmp/g1-fwload.log` and `dmesg | tail -n 20`. The
`overrun` count matters: `cat /sys/kernel/debug/1da00000.usif/state`.

## 3. The PCM clocks

    /tmp/gpio-activity 512 0,40,41

**Pass (G1):** changes on GPIO 40 (DCL) and GPIO 0 (FSC): the STUB clocks
the bus. Whether DI (41) moves says whether the chip already sends on it.
**If nothing moves:** the chip may need a different mode word to clock the
bus (on the 7412 only its own build clocked, and the mode word set the
rate; [7412-spi-tdm-and-loader.md](../7412-spi-tdm-and-loader.md) §1a). Note
it; no other mode is tried without a new card.

## 4. Hold the chip in reset, unload

    echo 542 > /sys/class/gpio/export 2>/dev/null
    echo low > /sys/class/gpio/gpio542/direction
    /tmp/gpio-activity 512 0,40     # expect: no changes again
    rmmod openDECTcell; rmmod lantiq_usif_uart
    grep -E '^pin (0|11|12|30|40|41) ' /sys/kernel/debug/pinctrl/1e100b10.pinmux/pinmux-pins

The chip stays in reset (GPIO 542 low through sysfs) until the next boot or
`echo 542 > /sys/class/gpio/unexport`.

## 5. Send back

`/tmp/g1-fwload.log`, every `gpio-activity` output, the `dmesg` lines, the
USIF `state` file if the upload failed, and anything unexpected.

## Results, 2026-11-01: **G1 passed**

*Observed on hardware:* FRITZ!Box 7362 SL, OpenWrt 25.12.6, firmware from
`FRITZ.Box_7362_SL-07.18.image` (extracted with `tools/dect-fw-extract.py`,
which found both stages by hash; it also lists the image's production-test
STUB, `…p2test.hex`, as unknown). `kmod-lantiq-usif-uart` `50bd2e5`,
`dect-fwload` and `kmod-opendectcell-lantiq` from openDECTcell `e30640b`, installed
by hand (the `opendectcell` package itself would not install: it still required
`kmod-lantiq-ssc-pcm`, see the release script's dependency handling).

**First run, without the strap** (`--no-strap`, as the 7412): 3 rounds of 16
stage-1 attempts at 9600 and 115200 Bd, **no byte at all** from the chip
(USIF `rx 0`). The 7520 showed exactly this without its strap
([stub-loader-protocol.md](../stub-loader-protocol.md) §7).

**Strap probe:** with the USIF module unloaded, GPIO 11 (the chip's TX, the
USIF's RX pin) was held low or left as an input while reset (GPIO 30) was
released, and then sampled every 6.3 µs for 1 s:

| GPIO 11 during reset release | Changes on GPIO 11 afterwards |
|---|---|
| input (no strap), three runs | 0 |
| **held low (strap)**, two runs | 16, the first about 0.22 s after the strap was released; about 3.4 ms low in all, consistent with about four STX bytes at 9600 Bd |

So the 7362 SL's ROM needs the same strap as the 7520's, on the chip's TX
line. The openDECTcell module drives it since `e30640b` and gives the pin back
to the USIF afterwards.

**Second run, with the strap:**

| Step | Result |
|---|---|
| Factory block | 17 bytes at urlader `0x1985`, 5 TLVs patched |
| Stage 1 | STX on the **first** attempt (slow mode, 9600 Bd); 615-byte image, the chip's XOR `0x3A` acknowledged |
| Stage 2 | STX at **230400 Bd**; the 48,329-byte STUB in about 4 s, XOR `0xF4` acknowledged |
| Summary | `upload OK (rounds 1, stage 1 attempts 1, stage 2 attempts 1)` |
| USIF counters | 8 bytes received, 0 overruns, 1 framing error (the strap holds the RX pin low while it is a GPIO) |
| STUB on the UART | nothing in 5 s (as on the 7520) |

**The PCM clocks** (`gpio-activity 512 0,40,41`):

| Pin | Before the upload | After |
|---|---|---|
| GPIO 40, DCL | static high | toggling, 47.8 % high (a fast clock, aliased) |
| GPIO 0, FSC | static high | toggling, 0.8 % high (short frame pulses) |
| GPIO 41, DI | static high | toggling (the chip sends on the bus) |
| GPIO 0 and 40, chip held in reset again | | static high |

The chip drives DCL and FSC: **it is the PCM clock master**, as the stock
board table suggested [CR 7362SL/A-0001]. The bit clock's rate is not
measured (sysfs sampling only shows that it runs).

Afterwards the chip was held in reset (GPIO 542 low through sysfs), both
modules unloaded, the pins released.

## Results, 2026-11-01: `kmod-lantiq-usif-uart` `f359106`

*Observed on hardware*, same box and release [CR 7362SL/A-0006]. From
`f359106` on, the module drives the UART's TX pad (GPIO 12) **push-pull**,
as the stock board does; `50bd2e5`, the build of the G1 run above, drove
it open drain. The feed pins `f359106`.

- The same upload (`/root/dect-chip-boot.sh`, `ubihealthd` stopped): both
  stages on the first attempt, USIF `rx 8 tx 48946 overrun 0 frame 1`, as
  with the open-drain pad.
- The same upload with **`ubihealthd` running** and, on top, 128 KiB NAND
  reads looping on both CPUs for the whole upload (each holds its CPU's
  interrupts off about 27 ms): upload OK, both stages on the first
  attempt, `overrun 0`, `frame 1`. So stopping `ubihealthd` is no longer
  needed for the upload itself (observed once). The limit: the chip sends
  only 8 bytes during an upload, so this shows that transmit survives the
  blackouts, not that a long receive burst would (the receive FIFO lasts
  1.4 ms at 230400 Bd).
