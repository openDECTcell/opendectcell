# Bench card: gate G2 on a FRITZ!Box 7362 SL (written 2026-11-01)

Gate G2 (`ROADMAP.md`): **the PCM link is alive**: frames read from the
transport hold `0xAA`-framed STUB mails with valid checksums, and the chip
answers the daemon's contact. On the 7362 SL the link is the VR9 TDM block,
carried by `kmod-lantiq-tdm-pcm` (`/dev/tdm-pcm0`).

**Only with the maintainer's agreement:** this boots the chip's firmware and
talks to it over the control slot. `dect-stubd` runs **without `-g`**, so it
sends no radio-on mail (`0x015B 01`) and the radio stays off; at the end the
chip is parked explicitly and held in reset anyway.

## What is known going in

- G1 passed: the 07.18 STUB uploads over `kmod-lantiq-usif-uart` and the chip
  clocks the PCM bus as master ([bench-7362sl.md](bench-7362sl.md)).
- `kmod-lantiq-tdm-pcm` has the SSC transport's uapi v3, so `dect-stubd`
  takes its SSC path (`SSC_PCM_IOC_INFO`). In mode 0x101: 8 slots of 16 bits
  per 125 µs frame, aligned to FSC by the hardware, no bit offset; a lost
  frame is lost whole and the transmitter repeats its last frame
  [CR 7362SL/A-0004]. The SSC path's offset search should therefore find
  bit offset 0. A repeated frame may duplicate control-slot bytes; the
  daemon's sequence check would show it.
- Loss-free for 15 minutes with `ubihealthd` running and under NAND reads,
  default ring of 16 blocks [CR 7362SL/A-0005]. Kernel console lines still
  cost ~50 frames each unless the console log level is 4 or lower
  [CR 7362SL/A-0007]; the box has `conloglevel` 4.
- Untested: whether the daemon finds the control slot unchanged.

## 0. Prepare (workstation)

1. Packages: openDECTcell from `main` (`OPENWRT=25.12.6 tools/release/build.sh
   lantiq/xrx200 OUT`) with the feed's `kmod-lantiq-tdm-pcm` and
   `kmod-lantiq-usif-uart` (the script builds them where their repositories
   are reachable; otherwise take their `.apk` from each repository's
   `deploy/`).
2. Copy all `.apk` files to the box's `/tmp/`. The firmware is already in
   `/etc/opendectcell/firmware/` from G1.

## 1. Install and load (box)

    apk add --allow-untrusted /tmp/kmod-lantiq-usif-uart-*.apk \
        /tmp/kmod-lantiq-tdm-pcm-*.apk /tmp/kmod-opendectcell-lantiq-*.apk \
        /tmp/opendectcell-*.apk
    /etc/init.d/opendectcell enabled && echo ENABLED || echo disabled   # expect disabled
    cat /proc/sys/kernel/printk               # first field 4 or lower
    echo 542 > /sys/class/gpio/unexport 2>/dev/null   # let go of a sysfs reset hold
    insmod lantiq_tdm_pcm && insmod lantiq_usif_uart && insmod openDECTcell
    ls -l /dev/tdm-pcm0 /dev/ttyUSIF0 /dev/dect0
    dmesg | tail -n 6

## 2. Upload (as in G1)

    dect-fwload --uart /dev/ttyUSIF0 --dect-dev /dev/dect0 \
        --first /etc/opendectcell/firmware/dectfw_firstlevel_441-06.88-7412.hex \
        --second /etc/opendectcell/firmware/dectfw_secondlevel_441-07.18-7362sl.hex \
        --mode 0x101 --factory-mtd /dev/mtd2 --factory-offset 0x1985 \
        -v 2>&1 | tee /tmp/g2-fwload.log

`ubihealthd` stays running (A-0006). **Expect** `upload OK`.

## 3. The control channel, radio off

    dect-stubd -d /dev/tdm-pcm0 -c 8 -v -t 60 2>&1 | tee /tmp/g2-stubd.log

**Pass (G2):** an `ssc: lane bit offset 0` line, a control slot found (TX
and RX), the contact answered (`0xFFF2`, `0xF601`, `0xF602` on the 7412),
the periodic mails (`0xFFF1`, `0xF618`, `0xF623` every 5 s on the 7412),
and at exit 0 sequence gaps and 0 checksum errors, with the transport's
dropped-frame count.
**Partial:** contact, but sequence gaps or checksum errors: note when they
come and the transport's counters; a repeated frame (A-0004) is the first
suspect.
**Fail:** `ssc: no frames` (the transport sees no clock), `no idle lane`, a
bit offset other than 0, or "control slot not found" after twenty passes.

## 4. Park, hold the chip in reset, unload

    dect-stubd -d /dev/tdm-pcm0 -c 8 -M "0x22:0x02:0x022c:01" \
        -M "0x22:0x02:0x015b:00" -t 6
    echo 542 > /sys/class/gpio/export 2>/dev/null
    echo low > /sys/class/gpio/gpio542/direction
    rmmod openDECTcell; rmmod lantiq_usif_uart; rmmod lantiq_tdm_pcm

## 5. Send back

`/tmp/g2-fwload.log`, `/tmp/g2-stubd.log`, `dmesg | tail -n 30`, and
anything unexpected.

## Results, 2026-11-01: **G2 passed**

*Observed on hardware:* FRITZ!Box 7362 SL, OpenWrt 25.12.6, the 07.18
STUB, `ubihealthd` running, console log level 4. `kmod-lantiq-tdm-pcm`
from `ae40553` (the room's packaged `lantiq_tdm_pcm.ko`),
`kmod-lantiq-usif-uart` `f359106`, `dect-fwload` and `kmod-opendectcell-lantiq`
as installed for G1; `dect-stubd` as installed, then built from `main`
with the fix below. Radio off throughout (no `-g`); every run ended with
the park, the chip held in reset and all three modules unloaded.

| Run | Started | Result |
|---|---|---|
| A, installed `dect-stubd` | right after the upload | "control slot not found" after 20 passes (43 s); all 1536 lanes idle at bit offset 0 (1024 at offset 8) |
| B, installed | right after run A | contact at once: **control slot tx 0, rx 1** (`f601`), RFPI broadcast, `f602`, then `fff1`/`f618`/`f623` every 5 s; 60 s, 38 mails, 0 sequence gaps, 0 checksum errors, 0 dropped frames |
| C1, installed | 50 s after the upload | as A: not found |
| C2, installed, `-T 0 -R 1` | right after the upload | contact at once; the first answer after the upload is `fff2` (build stamp) then `f601`, `f602` |
| C3, fixed `dect-stubd` | right after the upload | **found on the first pass** (tx 0, rx 1), as B; 30 s, 20 mails, 0 gaps, 0 errors |

The transport lost no frame in any run (`stopped: … about 0 lost`; 482,000
frames in the longest). Bit offset 0 as expected from the FSC-aligned
transport [CR 7362SL/A-0004]; the daemon delays receive by 7 lanes, since
the chip answers one lane after the one it reads (control lane 0 both
ways, speech lane 1).

**The fault was ours:** the control-slot search looked only at the first
mail in each lane and wanted a contact answer there; a freshly booted
chip sends its `0xFFF2` build stamp first
([stub-pcm-control-channel.md](../stub-pcm-control-channel.md)). The search
also dropped what did not fit the 1024-byte receive buffer. Fixed in
`d697936`, with a test.

Not yet run: the service itself (`/etc/init.d/opendectcell start` with the
new profile and packages); the box still has the G1-era `opendectcell`
packages and no `kmod-lantiq-tdm-pcm` package.
