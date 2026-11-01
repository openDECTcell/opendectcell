# Bench card: gate G1 on a FRITZ!Box 7362 SL (written 2026-11-01)

Gate G1 (`ROADMAP.md`): **the STUB boots** — both upload stages are
acknowledged — **and the PCM clocks (DCL, FSC) start.** This is the first
time openDECTcell boots the DECT chip on a 7362 SL.

**Only with the maintainer's agreement:** this boots the chip's firmware.
No mail is sent to it, so the radio stays off (it starts only on
`0x015B 01`, [mac-bringup.md](mac-bringup.md)); at the end the chip is held
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
  has neither `_slow` nor `_fast`; [stub-loader-protocol.md](stub-loader-protocol.md)
  §2-§3).
- `ubihealthd` reads NAND every 120 s with interrupts off for about 27 ms,
  longer than the USIF's receive FIFO lasts at 230400 Bd [CR 7362SL/A-0002
  §5]. It is stopped for the upload.

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
        --no-strap --listen 5 -v 2>&1 | tee /tmp/g1-fwload.log
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
rate; [7412-spi-tdm-and-loader.md](7412-spi-tdm-and-loader.md) §1a). Note
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
