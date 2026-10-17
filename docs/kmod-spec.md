# Task brief: Linux kernel module `opendectcell` for a FRITZ!Box 7520 running OpenWrt

**This document is self-contained.** You (an expert Linux kernel developer, e.g. an AI assistant) have no access to any repository or to the people who wrote it. Everything you need is below. Where something is not known, it says so; do not invent it.

## 0. How we will work together

- **You cannot compile or test.** Your output goes to an engineer (an AI coding agent) who has the lab: the target box, the OpenWrt SDK and SSH access. They build and test your code and send you compiler errors, kernel logs and test results. So: **prefer APIs you are sure exist in Linux 6.12**, comment every place where you are unsure, and keep each version small and conservative. A bug in a kernel module can hang or reboot the box (harmless to the flash, recoverable), so favour correctness over cleverness.
- **Phase A (reset and boot strap) is done and verified.** Its full source is in Appendix A; it is the starting point. **Your task now is Phase B** (section 7).
- **Answer format** (section 9) is strict, because the files are copied out of your answer by a program.

## 1. Project background

The project turns used AVM FRITZ!Boxes under OpenWrt into open DECT base stations. A DECT radio chip (Dialog SC14448) on the board runs **AVM's unchanged firmware, called "the STUB"**, which handles the radio, the DECT MAC layer and speech codecs. Everything above the MAC (DECT NWK/DLC protocol, registration, call control) is implemented by us in userspace C. A separate server translates DECT to SIP.

Terms: **STUB** = the chip firmware. **Mail** = a control message exchanged with the STUB. **FSC/DCL/MCL** = PCM frame sync, bit clock, master clock. **RFPI** = a DECT base station identity. The STUB is uploaded into the chip's RAM at every power-up over a UART (that part is userspace, section 3); after that, the chip talks to the SoC over the **PCM/TDM bus**.

The kernel module has two jobs: **Phase A** (chip reset and boot-strap control through `/dev/dect0`, replacing a fragile userspace hack; **done**) and **Phase B** (the PCM/TDM data path; **your task**, section 7).

## 2. Ground rules

- **Licence:** every file `SPDX-License-Identifier: GPL-2.0-only` (module and uapi header). Linux kernel coding style (tabs, 80-ish columns, `checkpatch`-clean as far as you can tell).
- **Clean room:** write only from this brief and from Linux kernel APIs. Do not reproduce AVM code. Qualcomm's GPL BSP code (QSDK) may be consulted for Phase B; keep its copyright headers. No firmware blobs in any file.
- **Never autoload.** The OpenWrt package must not load the module at boot; loading is a manual `insmod`.
- **Board matching:** the module must work only on this board and refuse to load elsewhere. The board's device-tree `compatible` contains `avm,fritzbox-7530` (the 7520 and 7530 are the same hardware; OpenWrt calls both `avm,fritzbox-7530`; `/tmp/sysinfo/board_name` shows it). Pins come from a small built-in **board table** (one entry for now). On a non-matching board: `pr_err` with the board name that was seen, return `-ENODEV`, touch nothing.
- **Userspace never handles GPIO numbers or register addresses** once the module exists; all of that is in the kernel.
- Write the code so that more boards/transports can be added later as new table entries (the same `/dev/dect0` interface for every box).

## 3. The target, and how the pin handling got to where it is (history and lab facts)

| Item | Value |
|---|---|
| Board | FRITZ!Box 7520 (= 7530 hardware), SoC Qualcomm IPQ4019, ARMv7 (Cortex-A7, 4 cores), 256 MB RAM |
| OS | OpenWrt **25.12.5** (r33051-f5dae5ece4), Linux **6.12.94**, target `ipq40xx/generic`, musl, gcc 14.3 |
| Kernel config | `CONFIG_DEVMEM` is off (**no `/dev/mem`**). sysfs GPIO (`/sys/class/gpio`, gpiochip base **512**), debugfs and pinctrl debugfs are available. No TDM/audio driver is built in. Modules are loaded with `insmod` |
| Build | OpenWrt SDK `openwrt-sdk-25.12.5-ipq40xx-generic_gcc-14.3.0_musl_eabi` (it contains the prepared 6.12.94 kernel tree with headers and `Module.symvers` but **no driver sources and no `arch/arm/boot/dts`**). Out-of-tree modules build against it |

### 3.1 Hardware facts for Phase A

- **DECT reset: SoC GPIO 40**, active **low** (drive low = chip held in reset; high = running). sysfs number 552. Observed behaviour: after OpenWrt boots it is an input; our tool drives it as an output.
- **UART to the chip: `serial@78af000`** (`msm_serial`, `ttyMSM0`), RX = **GPIO 16**, TX = **GPIO 17** (pinctrl function `blsp_uart0`, group names `gpio16`/`gpio17`; TLMM function numbering: func1 = UART, func0 = GPIO). **This UART is also the kernel console** and has a login on it. Stage-1 baud 9600 (set by userspace).
- **Boot-mode strap = the UART RX pin (GPIO 16)**: the chip drives that pin as its TX in normal operation, and **samples it at the rising edge of reset release**: **low → "UART boot" mode**, in which the ROM boot loader talks to the host. **If the strap is not low, the chip stays completely silent** (verified: 48 attempts without strap, zero bytes). So the sequence for every upload attempt is:

  1. drive GPIO 16 as a **GPIO output, low** (strap)
  2. assert reset (GPIO 40 low), wait about 10 ms
  3. (userspace sets the UART to 9600 baud)
  4. release reset (GPIO 40 high), wait about 15 ms
  5. **release the strap and give GPIO 16 back to the UART** (function `blsp_uart0`)
  6. the chip's ROM sends a 0x02 byte (STX) on the UART at 9600 baud

  The sleeps are done by userspace; the module only has to provide the primitives (or optionally the whole sequence atomically, section 5).

### 3.2 What the old userspace hack did (replaced by the Phase A module; kept for the facts it uncovered)

These shell commands are the **proven** sequence on the box (as root):

```sh
# strap low
echo 528 > /sys/class/gpio/export            # GPIO 16 (512 + 16)
echo low > /sys/class/gpio/gpio528/direction   # pin is now func0 = GPIO, driven low
# reset
echo 552 > /sys/class/gpio/export            # GPIO 40
echo low > /sys/class/gpio/gpio552/direction   # (hold reset low)
# ... wait ~10 ms, set UART to 9600 baud, then:
echo 1 > /sys/class/gpio/gpio552/value         # release reset
# ... wait ~15 ms
echo 528 > /sys/class/gpio/unexport            # release the strap pin
echo "gpio16 blsp_uart0" > /sys/kernel/debug/pinctrl/1000000.pinctrl/pinmux-select   # restore UART function
```

Facts learned doing this (all *observed on the real box*):
- Requesting GPIO 16 **switches the pin from func1 (UART) to func0 (GPIO)** even though the serial driver owns the pin's mux.
- **Releasing the GPIO does not switch it back**: the pin stays func0 and the UART receives nothing. The `pinmux-select` debugfs write (group first, then function) is what restores it. That is the part that must be done properly in the kernel without debugfs.
- **Unbinding and rebinding the `msm_serial` driver to restore the pin reboots the box** (it is the console). Do not do that.
- While the chip runs its firmware afterwards, resetting it again with the strap works again (the ROM comes back).
- The login process on `ttyMSM0` must be frozen (`kill -STOP`) during uploads; if it is resumed or killed, the port stays hung up until reboot. This is a userspace matter; the module must not interfere with it, but must **not hang up or otherwise break the console**.

### 3.3 How the UART function is restored (implemented and verified on the box)

An earlier version of this brief suggested `pinctrl_force_default()`. **That was wrong:** in Linux 6.12 it is not available to out-of-tree modules (it is not declared in the consumer header; it works on pinctrl controllers, not devices). What the Phase A module does instead, and what is **verified on the box (50 consecutive uploads, clean unload)**:

- It maps the TLMM control register of GPIO 16 (found through the pin controller's device-tree node, compatible `qcom,ipq4019-pinctrl`, register offset 16 x 0x1000) with `ioremap()` without requesting the region (the pinctrl driver owns it).
- After releasing the GPIO, it **writes only the function-select field (bits 5:2) of that one register back to 1** (function `blsp_uart0`) and reads it back to verify. It never touches GPIO 17 or other bits.
- This bypasses the pinctrl core's bookkeeping and its lock, which is acceptable because only this module changes that pin while the device is open.

Things that do not work: `pinctrl_select_state()` to the same state (no-op); a pinctrl mapping that claims `blsp_uart0` on `gpio16` for another owner (`-EBUSY`); calling into pinctrl internals (`struct pinctrl_dev` is private).

How to request the two GPIOs without a device-tree node: use whatever gpiolib API exists in 6.12 for looking up the chip with label `1000000.pinctrl` and the descriptor by hardware offset (e.g. `gpio_device_find_by_label()` and `gpio_device_get_desc()`), or fall back to the legacy global numbers (`512 + 40`, `512 + 16`, they are valid because sysfs GPIO works). The chip base (512) can change between kernels, so prefer the lookup by label.

## 4. Hardware facts for Phase B (PCM/TDM)

| Fact | Value |
|---|---|
| Pins (SoC GPIO numbers) | link 1: **MCL 20, DCL 21, FSC 22**; link 2: **MCL 24, DCL 25, FSC 26**; data: **DI 23** (chip → SoC), **DO 27** (SoC → chip) |
| AVM's pin-function numbers (stock device tree) | 20, 21, 22 → function 2; 23, 24, 25, 26, 27 → function 3. Cross-check with the pin function names in mainline `pinctrl-ipq4019.c` (names such as `audio_pcm`, `audio_pwm`); the mapping of AVM's numbers to names is not established |
| Clock master | **The DECT chip drives the clocks and frame sync; the SoC is a slave.** Observed: all eight pins are static while the chip is in reset; after the firmware runs, MCL and DCL toggle and FSC is a short pulse (high 0.8 % of the time) |
| Timing | frame rate **8 kHz**; DCL about **1.024 MHz** (the stock loader measured 1,025,735 Hz); FSC pulse about one bit clock wide, once per **128 DCL clocks (= 16 bytes per frame)**; both link groups (20-22 and 24-26) show identical timing; which group the SoC must use is not known |
| Channels | 4 DECT voice channels on the PCM link (stock driver message); one control slot carries the mails |
| SoC block | IPQ4019 **ADSS** audio subsystem. Stock device-tree nodes (enabled): `audio@7700000` (`qca,ipq4019-audio-adss`, regs `0x7700000` len 0x34 and `0x7707000` len 0x20, reset `blk_rst` = clock-controller reset id 0x2c); `pcm@7704000` (`qca,ipq4019-pcm`, regs 0x7704000 len 0x2000, DMA tx channel 6, rx channel 1, stereo tx port 3, rx port 0, clock `audio_pcm_clk`); `qca-pcm-tdm@7709000` (`qca,ipq4019-pcm-tdm`, regs 0x7709000 len 0x1000); `tdm@0` (`qca,ipq4019-tdm`, **DMA tx channel 0, rx channel 7, stereo tx port 0, rx port 3**, clocks `audio_tx_bclk`, `audio_tx_mclk`, `audio_rx_bclk`, `audio_rx_mclk` from the audio clock provider, ids 7, 5, 9, 3). MBOX DMA registers lie in `0x7706000`-`0x770e000` |
| Mainline support | **None** for this TDM block in Linux 6.12. Qualcomm's GPL BSP (QSDK, Code Aurora, directory `sound/soc/ipq/`: ADSS, MBOX DMA and TDM glue) has a driver for it; exact file names are not verified here, search for the compatible strings above |

Mail framing on the control slot (derived from the stock stack, **not yet verified on hardware**; all multi-byte fields little-endian; idle fill is 0xFF; the receive buffer is pre-filled with 0xFF):
- **STUB → host:** byte 0 = `0xAA` start marker (the parser skips bytes until it sees one); byte 1 = sequence number (increments per mail); bytes 2-3 = length L (max 0xFB; larger = framing error → resync); then the body (L + 2 bytes); then a 16-bit checksum = the 16-bit sum of all bytes from the length field to the end of the body.
- **host → STUB:** an element of (16-bit word count = (mail length + 5) / 2), 16-bit TX sequence, the mail bytes, 16-bit checksum (sum of the mail bytes), padded to an even length; unused TX bytes are 0xFF. The 16-bit primitive id sits at mail offset 4.

## 5. The device interface `/dev/dect0`

A `miscdevice` named `dect0` (node `/dev/dect0`, mode 0600 root). **Exclusive open** (`-EBUSY` for a second opener). The uapi header `opendectcell.h` must be usable from userspace C (only `<linux/ioctl.h>`, `<linux/types.h>`) and carry a version number. Ioctl numbers and struct layouts are yours to design; document them in the header.

Phase A operations:

| Operation | Required behaviour |
|---|---|
| `DECT_IOC_INFO` | Returns: interface version, board name string, transport name (`ipq40xx-tdm`), feature flags, and the stage the module implements (A) |
| `DECT_IOC_RESET(asserted)` | Drives GPIO 40: 1 = held in reset (low), 0 = released (high) |
| `DECT_IOC_STRAP(low)` | 1: GPIO 16 becomes a GPIO output driven low; 0: GPIO 16 is released and the UART function is restored (section 3.3). Idempotent (calling twice is fine) |

Do **not** add a composite "reset into boot mode" ioctl: the three primitives above are enough, and all timing (the 10 ms and 15 ms waits, the baud change) stays in userspace.

Safety obligations:
- On `release()` (close) and on module unload: restore GPIO 16 to the UART function if it was strapped, **release reset if it was left asserted**, free all GPIOs.
- `rmmod` while the device is open must fail cleanly (`try_module_get` semantics via the file operations' `owner`).
- Never leave the console UART broken: after any sequence of ioctls and `close`, `ttyMSM0` must work (kernel messages still appear on the serial console, and a process can open it, set baud rates and exchange bytes).
- No debugfs use in the module.

## 6. Phase A (done, for reference)

Implemented in `kernel/opendectcell_ipq40xx.c` (Appendix A) and verified on the box: 50 consecutive firmware uploads through `DECT_IOC_RESET`/`DECT_IOC_STRAP` without a failure; refusal to load on a wrong board name; `rmmod` and a second `open` refused while the device is open; clean unload (reset released, GPIO 16 back on the UART function, no GPIO left requested); no kernel warnings. Keep this behaviour intact: **every change you make must leave these ioctls and the cleanup guarantees working.**

## 7. Phase B: the PCM/TDM receive path (your task)

**Goal: "gate G2".** Capture valid control messages ("mails", section 4) that the running STUB sends on the PCM bus, on the real box, and learn the frame layout. Receive only; transmit comes later. Voice, DECT protocol and the userspace daemon are out of scope.

### 7.1 First: a feasibility statement (mandatory)

Start your answer with a short statement (at most 25 lines) of what you **know with confidence** about the IPQ4019 ADSS/TDM/MBOX hardware and Qualcomm's QSDK driver structure (register blocks, DMA descriptor ring format, clock setup, how slave mode is selected, which GPIO functions the PCM pins need), and what you would have to **guess**. Rules:
- **Do not invent register offsets, bit fields or descriptor layouts.** A wrong write to the ADSS block or its clocks can hang the SoC. If you cannot give a solid design for the full path, **say so and deliver the fallback (7.4) instead.** An honest "not enough information" is a good answer; list exactly what you would need (e.g. which QSDK source files and which functions).
- If you are confident, deliver 7.3.

### 7.2 Known facts (repeat of section 4, plus what the stock stack does)

- The chip is the clock master; the SoC is a slave on DCL/FSC. 128 DCL clocks (16 bytes) per 8 kHz frame is the working assumption, not confirmed.
- The stock stack configures the TDM interface through a kernel API `tdm_if_config_init(...)` with a word width of **8 or 16 bits**, a slot count derived from its link configuration, and a "master" flag, then `tdm_if_dma_init(...)` (derived statically; the 7520's stock device-tree nodes in section 4 are the matching hardware description).
- The stock driver reports 9 codec slots in total and **4 DECT channels**; the control mails live in one slot.
- The DECT chip only produces PCM clocks while the STUB runs. To test, the userspace uploader (`dect-fwload`, uses your Phase A ioctls) starts the STUB; the clocks then stay up until the next reset.

### 7.3 Main path: ADSS TDM receive, slave mode

Requirements:
1. Bring up the ADSS TDM **receive** path as a slave of the chip's DCL and FSC: pin functions for the PCM pins (section 4), ADSS clocks in external/slave mode, a MBOX DMA ring for receive, frame size a **module/ioctl parameter** (default 16 bytes per frame). Use the Phase A board table for the pins.
2. `read()` on `/dev/dect0` returns raw received data in blocks; each block has a small header (block counter, timestamp, overrun flag). `poll()` supported. A stats ioctl returns frames received, overruns, and the clock state seen by the hardware.
3. Configuration ioctl: frame size, word width, which of the two pin groups to use (section 4), start/stop of reception.
4. Keep all Phase A behaviour; stopping reception and unloading must leave the clocks and pins as found (the chip drives them; the SoC must not drive any PCM pin except the data-out pin, and not even that in Phase B).
5. Sustained operation at 8000 frames per second for at least 10 minutes without overrun.

### 7.4 Fallback: a polling capture (use this if 7.1 says the full path is not safe to write)

A deliberately simple **measurement tool inside the same module**, to learn the frame layout so the real driver can be written afterwards. Not a product feature; keep it small.
- New ioctl `DECT_IOC_CAPTURE`: for a **bounded window of at most 8 ms**, with local interrupts disabled (use `local_irq_save`/`restore`; never loop longer, the box has a watchdog and RCU), poll the PCM input signals by reading each pin's input-level register in the TLMM block (each GPIO has its own 0x1000-byte register block; the input level is bit 0 of the register at offset 4 within it; the control register at offset 0 holds the function select in bits 5:2: this layout is from the mainline `pinctrl-msm` driver for IPQ4019; Phase A already maps the control register of GPIO 16 the same way). Signals: **DCL = GPIO 21, FSC = GPIO 22, DI = GPIO 23, DO = GPIO 27** (optionally MCL = GPIO 20).
- Strategy: wait for DCL edges (the bit clock is about 1.024 MHz) and read FSC and DI right after each rising edge. **A read of a device register may take 100-300 ns; the half period of DCL is about 490 ns.** The module must therefore first run a **calibration**: time N back-to-back register reads with `ktime_get_ns()` and report nanoseconds per read, and report in the result how many register reads fit in one DCL period as it measured it. If three reads per half period are not possible, the capture result must be flagged "too slow" and we will stop there (that is a valid, useful result).
- Result returned to userspace: one byte per DCL clock (bit 0 = DCL, bit 1 = FSC, bit 2 = DI, bit 3 = DO), at least 16 KiB, plus the calibration numbers and the timestamps of window start/end.
- No DMA, no ADSS access, no clock or mux changes: **read-only on the TLMM input registers.**

### 7.5 Deliverables and tests for Phase B

Deliver the complete changed files (section 9). For the main path also a tiny userspace C capture tool (`src/tools/dect-pcmcap.c`: reads N blocks from `/dev/dect0` into a file and searches the data for the 0xAA mail marker with a valid checksum, printing sequence numbers). For the fallback also a userspace tool `src/tools/dect-sla.c` that calls `DECT_IOC_CAPTURE` and writes the raw samples to a file.

Acceptance (run by the engineer on the box, chip running the STUB):
1. Phase A tests still pass.
2. Main path: blocks arrive at 8 kHz frame rate (counter and timestamps consistent); no overrun for 10 minutes; `rmmod` after stopping reception is clean; no `WARN`/`BUG`; the box does not hang. Mails: the capture tool finds `0xAA`-framed mails with correct checksums and increasing sequence numbers (that is gate G2).
3. Fallback: calibration numbers are reported; if not "too slow", the raw samples show the frame sync pulses 125 microseconds apart and bit patterns on DI.
4. Questions the capture must let us answer (we will analyse the data offline): bytes per frame, control-slot position, voice-slot map, bit order, whether the mail framing applies in both directions, which pin group carries the signals.

## 8. Rules for the box (for whoever tests)

- Build with the SDK; copy the `.ko` to `/tmp`; `insmod /tmp/opendectcell.ko`. Never install it to flash, never autoload it.
- Never write flash/MTD partitions. The box can be rebooted at any time (that restores everything). Never run `sysupgrade -n` or a factory reset (it would switch DHCP on for the user's the local network).
- Never unbind/rebind `msm_serial`; never resume or kill the frozen login on `ttyMSM0` during a session.

## 9. Answer format (strict)

1. First the **feasibility statement** of 7.1.
2. Then a **short summary** (at most 15 lines) of your design and what you are unsure about.
3. Then **every changed or new file in full**, each introduced by a line exactly of the form
   `FILE: path/relative/to/repository/root`
   followed immediately by one fenced code block with the complete file contents (no ellipses, no "rest unchanged"). Allowed paths: `kernel/opendectcell_ipq40xx.c`, `kernel/opendectcell_pcm.c` (new, optional), `kernel/include/uapi/opendectcell.h`, `kernel/Makefile`, `src/tools/dect-pcmcap.c`, `src/tools/dect-sla.c`. Do not resend files you did not change.
4. Then the **test plan** and a bullet list **"Assumptions and unverified API points"** (every kernel function, header, register or bit field you were not certain about, with the fallback you would try).
5. If something in this brief is contradictory or missing, say so **before** writing code and ask; do not guess hardware facts.

