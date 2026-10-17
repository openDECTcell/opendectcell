# Specification: kernel module `kmod-opendectcell-ipq40xx`

For an implementer (human or AI agent) who has **no access to the conversation that produced this project**. Read `AGENTS.md` first (binding rules), then this file. Every hardware fact below names its source and whether it was *observed on hardware* or *derived statically*.

Status: ready to implement. Issue references are to `github.com/openDECTcell/opendectcell`.

## 1. Why this module exists

openDECTcell turns a FRITZ!Box 7520 (OpenWrt) into a DECT base station. The DECT chip (Dialog SC14448, running AVM's unchanged firmware "STUB") is reached over two links:

| Link | What | Who handles it today |
|---|---|---|
| UART (`ttyMSM0`) | Chip reset handshake and firmware upload | Userspace tool `dect-fwload`, **using sysfs GPIO and a debugfs file as a bring-up hack** (see §3) |
| PCM/TDM bus | Control messages ("mails") and voice, after the STUB runs | **Nothing yet. This module's main job (Phase B)** |

The module replaces the bring-up hack (Phase A) and adds the PCM data path (Phase B). Design background: `docs/hardware-abstraction.md` (board table, one module per target, uniform `/dev/dect0`) and `docs/stub-loader-protocol.md` §2 and §7.

**Non-goals:** the DECT protocol (it lives in userspace: `libopendectcell`), the firmware upload protocol (userspace), any AVM code.

## 2. Ground rules

- **Licence:** `SPDX-License-Identifier: GPL-2.0-only` in every file under `kernel/`. Linux kernel coding style.
- **Clean room:** write from this spec and the Linux kernel APIs. Do not use AVM binaries, AVM kernel modules, analysed code or AVM device-tree *source*. Qualcomm's GPL kernel code (QSDK / Code Aurora, `sound/soc/ipq`) may be read and ported; keep its copyright headers. Facts in this document come from AVM's device tree and firmware, written in our own words.
- **No blobs** in the repository. The STUB firmware is user-supplied at runtime.
- **Never load the module at boot.** The OpenWrt package must not autoload it. Loading is manual (`insmod`) until the user decides otherwise.
- **Board matching:** the module matches the board by the device-tree `compatible` (7520 and 7530 are the same hardware: **`avm,fritzbox-7530`**, also `/tmp/sysinfo/board_name`) and takes pins from a built-in table. On an unknown board it refuses to load and logs the name it saw. It never touches a GPIO it does not know.
- **Userspace never sees GPIO numbers** after Phase A (`AGENTS.md` §5).

## 3. The target and the lab

| Item | Value |
|---|---|
| Box | FRITZ!Box 7520, spare unit, OpenWrt **25.12.5** (r33051-f5dae5ece4), kernel **6.12.94**, `ipq40xx/generic`, ARMv7 4 cores, 256 MB RAM |
| Access | `ssh root@192.168.1.1` with the user's key. Ask the user for access and for a root password if the key is not installed. The box shares the user's **the local network**; its DHCP and IPv6 router advertisements are disabled and **must stay disabled** |
| SDK | `openwrt-sdk-25.12.5-ipq40xx-generic_gcc-14.3.0_musl_eabi.Linux-x86_64` (download from `downloads.openwrt.org/releases/25.12.5/targets/ipq40xx/generic/`, SHA-256 `af3d1564…2281515`). It contains a prepared kernel build tree for 6.12.94 (headers and `Module.symvers`) at `build_dir/target-arm_cortex-a7+neon-vfpv4_musl_eabi/linux-ipq40xx_generic/linux-6.12.94`, so out-of-tree modules build with the SDK toolchain. The tree has **no** `arch/arm/boot/dts` and no driver sources; read mainline 6.12 for those |
| Kernel config | **No `/dev/mem`**, no TDM/audio driver, no GPIO sloppy logic analyser. debugfs is mounted. Modules are loaded with `insmod` |
| Userspace tools in this repo | `make cross` builds `build/dect-fwload.arm`; `tools/gpio-activity.c`; lab scripts are described in the maintainers' notes |
| Backup | A full flash backup of the box exists (not in git). **Never write to flash.** Factory partitions are read-only |

**Hazards found in the lab (observed on hardware):**
- The UART the DECT chip uses is **the console UART**: `ttyMSM0` = `serial@78af000`, 115200 baud, kernel console, and a login (`askfirst`) runs on it. Freeze that login (`kill -STOP`, never resume or kill it) and set `/proc/sys/kernel/printk` to 1 before a firmware upload; otherwise it interferes. Resuming/killing it leaves the port hung up until reboot.
- **Never unbind and rebind the `msm_serial` driver:** it reboots the box (it is the console).
- A reboot restores everything; flash is intact.

## 4. Hardware facts

Sources: [OBS] observed on the spare 7520 (OpenWrt, 2026-10-17); [DT] AVM FRITZ!OS 08.25 stock device tree (decoded from the image, facts only); [FW] FRITZ!OS 08.25 loader, derived statically. Details and further context: `docs/models/7520-DECT-analysis.md` (§3, §8) and `docs/stub-loader-protocol.md`.

### 4.1 Reset and boot strap (Phase A)

| Fact | Value | Source |
|---|---|---|
| DECT reset | **GPIO 40**, active **low** (drive low = chip in reset). On OpenWrt the gpiochip base is 512, so sysfs number 552 | [DT] [OBS] |
| Boot-mode strap ("rd") | **GPIO 16** = the UART RX pin (chip TX → SoC RX). The chip samples it at the release of reset: **low = UART boot** ("slow" mode, stage-1 baud 9600). Without the strap the chip stays silent in every attempt | [FW] [OBS] |
| Pin 16 mux | Normal state: function `blsp_uart0`, owned by `78af000.serial`. TLMM: pin 16 `func1` = UART, `func0` = GPIO | [OBS] |
| Reset sequence (before every stage-1 attempt) | strap low (drive pin 16 as a GPIO output low) → reset asserted (low) → wait ~10 ms → set the UART to 9600 baud → reset released (high) → wait ~15 ms → **strap released and pin 16 returned to UART function** → the chip's ROM sends STX | [FW] [OBS] |
| Pin 16 after a GPIO request | Requesting the GPIO switches the pin to `func0` even though the serial driver owns it; **releasing the GPIO does not restore `func1`**, so the UART receives nothing. The bring-up tool restores it by writing `gpio16 blsp_uart0` to `/sys/kernel/debug/pinctrl/1000000.pinctrl/pinmux-select` | [OBS] |
| Pin 17 | UART TX (SoC → chip), stays `func1` | [DT] [OBS] |

**Phase A design freedom:** how the module restores the UART function must not rely on debugfs and must not unbind the serial driver. Options: re-apply the serial device's own `default` pinctrl state (look up `78af000.serial`, `pinctrl_lookup_state`/`pinctrl_select_state`), or `pinctrl_gpio_request`/`pinctrl_gpio_free` plus selecting the function through the pinctrl API. Verify that after release the UART works (the stage-1 ACK arrives) and that the console keeps working.

### 4.2 PCM/TDM bus (Phase B)

| Fact | Value | Source |
|---|---|---|
| Pins (SoC GPIO) | link 1: **MCL 20, DCL 21, FSC 22**; link 2: **MCL 24, DCL 25, FSC 26**; data: **DI 23** (chip → SoC), **DO 27** (SoC → chip). AVM's pin-function numbers: 20/21/22 `func2`; 23, 24–27 `func3` (to be cross-checked against `pinctrl-ipq4019.c` in mainline 6.12: function names, e.g. `audio_pcm`/`audio_pwm`) | [DT] |
| Clock master | **The DECT chip drives the clocks and the frame sync.** All eight pins are static with the chip in reset; after the STUB boots, MCL and DCL toggle and the frame sync is a short pulse. The SoC does not drive them (so the SoC side is a **slave**) | [OBS] `docs/models/7520-DECT-analysis.md` §8.2 |
| Frame sync | Pulse, high 0.8 % of the time ≈ **one bit clock wide, once per 128 clocks**; frame rate **8 kHz**; DCL ≈ **1.024 MHz** (measured 1,025,735 Hz by the stock loader). So **128 DCL clocks (16 bytes) per frame** is the working assumption | [OBS] [FW] |
| Both link groups | MCL/DCL/FSC of both groups toggle identically; probably the PCB routes the chip's signals to both. Which group the SoC's TDM block must use is **open** | [OBS] |
| Voice channels | The stock stack reports **4 DECT channels** on the PCM link and 9 codec slots in total | [FW] |
| Control channel | In the PCM frames, "mails": start byte 0xAA, sequence byte, 16-bit LE length, body, 16-bit LE sum; idle fill 0xFF. See `docs/stub-pcm-control-channel.md` | [FW] derived |
| SoC block | IPQ4019 **ADSS** (audio) block. Stock device-tree nodes below | [DT] |

Stock device-tree facts for the TDM path (AVM's tree, enabled nodes):

| Node | compatible | Registers | Other |
|---|---|---|---|
| `audio@7700000` | `qca,ipq4019-audio-adss` | `0x7700000` (0x34), `0x7707000` (0x20) | reset line `blk_rst` (clock-controller reset id 0x2c) |
| `pcm@7704000` | `qca,ipq4019-pcm` | `0x7704000` (0x2000) | DMA tx channel 6, rx channel 1; stereo tx port 3, rx port 0; clock `audio_pcm_clk` |
| `qca-pcm-tdm@7709000` | `qca,ipq4019-pcm-tdm` | `0x7709000` (0x1000) | |
| `tdm@0` | `qca,ipq4019-tdm` | (none) | **DMA tx channel 0, rx channel 7; stereo tx port 0, rx port 3**; clocks `audio_tx_bclk`, `audio_tx_mclk`, `audio_rx_bclk`, `audio_rx_mclk` (from the audio clock provider, ids 7, 5, 9, 3) |
| MBOX DMA | | `0x7706000`-`0x770e000` | shared with the audio blocks |

Where the driver code comes from: Qualcomm's GPL BSP (QSDK, `sound/soc/ipq/`: ADSS, MBOX DMA and the TDM/PCM glue). **File names and the exact driver for the `qca,ipq4019-tdm` compatible are not verified here**; search the QSDK tree for the compatible strings above. Mainline Linux 6.12 has **no** support for this TDM block (checked in the SDK tree).

## 5. Interface: `/dev/dect0`

One misc character device. Only one opener at a time (`-EBUSY` otherwise). The exact ioctl numbers are up to the implementer; document them in a header `kernel/include/uapi/opendectcell.h` (usable from userspace C, no kernel types).

**Phase A (must have):**

| Operation | Behaviour |
|---|---|
| `DECT_IOC_INFO` | Returns the interface version, board name, transport name (`ipq40xx-tdm`), and feature flags |
| `DECT_IOC_RESET(asserted)` | Drives GPIO 40: 1 = held in reset (low), 0 = released |
| `DECT_IOC_STRAP(low)` | 1: pin 16 driven low as GPIO; 0: pin returned to UART function (§4.1). Must be safe to call twice and must restore the UART on `close()` or module unload, and release reset if it was left asserted |
| (optional) `DECT_IOC_RESET_BOOT(slow)` | The whole documented reset sequence with the timings of §4.1, so userspace does not depend on sleep jitter |

The UART itself stays a normal tty (`/dev/ttyMSM0`); the module does not touch it except for the pin function.

**Phase B:** see §7.

## 6. Phase A: tasks and acceptance

Issue #53 (kernel module skeleton: board table, `/dev/dect0` with reset and strap, no PCM); afterwards issue "dect-fwload on /dev/dect0 and the board profile" (the userspace migration, not part of this module).

Deliverables:
1. `kernel/` module sources, `kernel/include/uapi/opendectcell.h`, an out-of-tree `Makefile`.
2. An OpenWrt package definition `openwrt/package/kmod-opendectcell-ipq40xx/Makefile` using the standard `KernelPackage` mechanism (not autoloaded; no `AUTOLOAD`).
3. Board table with one entry (`avm,fritzbox-7530`: reset 40, strap 16, UART device `78af000.serial`).

Acceptance tests (run on the box; the user runs hardware tests from test cards in the issue, or gives access):
1. `insmod` on the 7520: `/dev/dect0` appears, kernel log names the matched board. On a non-matching board name (test by changing the table entry) the load fails with a clear message and no GPIO is touched.
2. With the login on `ttyMSM0` frozen and printk 1, run the existing upload (`dect-fwload`, modified to use the ioctls instead of sysfs): **stage 1 ACK on the first slow attempt and stage 2 ACK, repeatedly**. Check with `tools/gpio-activity` (GPIO 20-27: static with the chip in reset, toggling after the upload).
3. 50 consecutive upload cycles succeed.
4. After each cycle the UART still works and `/proc/tty/driver/msm_serial` shows normal counters; the **console** (kernel messages) keeps working after `rmmod`.
5. `rmmod` with the device open fails cleanly; `rmmod` otherwise leaves reset released, pin 16 back on the UART, and no exported GPIO.
6. No `WARN`/`BUG`/oops in `dmesg`; no reboot.

## 7. Phase B: PCM data path

Issues #15 (streaming), #16 (real-time path), #17 (`/dev/dect0` data interface), #18 (capture tool), #19 (determine words per frame, control-slot position, voice-slot map), #20 (**gate G2**: valid STUB mails captured). Work on the **receive side first**, which is all gate G2 needs.

Requirements:
1. Bring up the ADSS TDM receive path as a **slave** of the chip's DCL and FSC (§4.2): configure the pins' function (§4.2), the ADSS clocks as external/slave, the MBOX DMA ring for receive, and frame size 16 bytes (128 clocks) as the starting point; the frame size must be a parameter because it is not fully known (#19).
2. Interface to userspace: `read()` returns raw received data with a header per block (a counter and a timestamp); `poll()` support; overrun counters in `DECT_IOC_INFO`/a stats ioctl. `write()` (transmit) comes second.
3. A configuration ioctl for frame size, word width and which pin group is used.
4. A **capture tool** (userspace, `tools/` or `src/`): dump N frames to a file and look for the 0xAA marker with a valid checksum. Gate G2 passes when valid STUB mails (checksum OK, increasing sequence numbers) are captured from a running STUB.
5. Real-time: sustained operation at 8000 frames per second without losing a frame for at least 10 minutes; report overruns.

Open questions the capture should answer (record the results in `docs/stub-pcm-control-channel.md` and the 7520 analysis, tagged "observed on hardware"): words/bytes per frame, control-slot position, voice-slot map, bit order and word width, whether the 0xAA mail framing applies in both directions, which pin group the SoC must use.

To start the STUB for tests: upload it with the userspace tool and keep the chip running (the PCM clocks only exist while the STUB runs); a reset through the module stops them.

## 8. Rules for working on the box

- Build with the SDK. Copy the `.ko` to `/tmp`; `insmod /tmp/opendectcell.ko`. Do not install it to flash and do not autoload it.
- The box can be rebooted at any time; configuration is restored (do **not** run `sysupgrade -n` or a factory reset: DHCP on the the local network would come back).
- Never write MTD partitions. Reading `/dev/mtd*` is fine.
- Report results as an issue comment with the exact commands, outputs and the observed/derived tags used in the docs.

## 9. Open questions (resolve and record)

- Cleanest pin-function restore for pin 16 without debugfs (§4.1).
- Which of the two pin groups and which of the ADSS TDM clock modes work as a slave of the chip's clocks (§4.2).
- Whether 128 clocks per frame is the real frame size or the DCL is double-rate (#19).
- Interaction with the console UART (RX noise while the STUB runs).
