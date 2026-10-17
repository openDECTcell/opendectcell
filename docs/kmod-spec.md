# Task brief: Linux kernel module `opendectcell` for a FRITZ!Box 7520 running OpenWrt

**This document is self-contained.** You (an expert Linux kernel developer, e.g. an AI assistant) have no access to any repository or to the people who wrote it. Everything you need is below. Where something is not known, it says so; do not invent it.

## 0. How we will work together

- **You cannot compile or test.** Your output goes to an engineer (an AI coding agent) who has the lab: the target box, the OpenWrt SDK and SSH access. They build and test your code and send you compiler errors, kernel logs and test results. So: **prefer APIs you are sure exist in Linux 6.12**, comment every place where you are unsure, and keep each version small and conservative. A bug in a kernel module can hang or reboot the box (harmless to the flash, recoverable), so favour correctness over cleverness.
- **Phase A (reset and boot strap) is done and verified**; a read-only polling capture was added and proved too slow. Their source is in **Appendix A** and is the starting point.
- **Your task is Phase B** (section 7): the PCM/TDM receive path through the SoC's audio hardware. **Qualcomm's original driver sources for that hardware are in Appendix B.** Base every register access on them.
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

Tags: [OBS] observed on the box; [DT] AVM's stock device tree of FRITZ!OS 08.25; [ML] mainline Linux v6.12.94 sources; [QC] Qualcomm's 2016 IPQ4019 ASoC patch series (Appendix B).

### 4.1 Signals and pins

| GPIO | Signal (our name) | Mux slot | Function name in 6.12 [ML] | Name in Appendix B [QC] |
|---|---|---|---|---|
| 20 | MCL, receive group | 2 | `i2s_rx` | `i2s_rx_mclk` |
| 21 | DCL (bit clock), receive group | 2 | `i2s_rx` | `i2s_rx_bclk` |
| 22 | FSC (frame sync), receive group | 2 | `i2s_rx` | `i2s_rx_fsync` |
| 23 | DI (data chip → SoC) | 3 | `i2s_rx` | `i2s_rxd` |
| 24 | MCL, transmit group | 3 | `i2s_tx` | `i2s_tx_mclk` |
| 25 | DCL, transmit group | 3 | `i2s_tx` | `i2s_tx_bclk` |
| 26 | FSC, transmit group | 3 | `i2s_tx` | `i2s_tx_fsync` |
| 27 | DO (data SoC → chip) | 3 | `i2s_td` | `i2s_txd1` |

- The mux slots are identical in [DT], [ML] and [QC]. The TLMM register layout [ML]: control register at `0x1000 * gpio`, function select in bits 5:2; input register at `0x1000 * gpio + 4`, level in bit 0. TLMM base: the node with compatible `qcom,ipq4019-pinctrl` (physical 0x01000000). Phase A already maps registers this way.
- [OBS] Today all eight pins are **unclaimed GPIO inputs (function 0, pull-up)**; no driver owns them. Setting their function is safe for the chip only as **inputs**: the SoC must never drive MCL, DCL or FSC (the chip drives them). Do not switch pin 27 (DO) to its function until transmit is implemented.
- [OBS] **The DECT chip is the clock master.** With the chip in reset, all eight pins are static. With the STUB running, MCL/DCL toggle on both groups, FSC is a short pulse. The SoC measured **DCL = 1.024 MHz** (1,019,367-1,027,749 Hz in three runs) and the stock loader logged FS 8000 Hz. FSC is high about 0.8 % of the time ≈ **one bit clock per 128 clocks**, so the working assumption is **128 bit clocks = 16 bytes per frame** (not confirmed; it might be several channels of 8/16 bits).
- Both receive and transmit clock inputs carry the chip's clocks [OBS], so the receiver can be clocked from the **receive group (20-22)**.

### 4.2 The ADSS block and its clocks/resets

| Item | Value | Source |
|---|---|---|
| ADSS registers | `0x07700000` (len 0x34) and `0x07707000` (len 0x20) | [DT] node `audio@7700000`, compatible `qca,ipq4019-audio-adss` |
| PCM/TDM registers | `0x07709000` (len 0x1000) | [DT] `qca-pcm-tdm@7709000` |
| "pcm" (stereo/mbox host) | `0x07704000` (len 0x2000) | [DT] `pcm@7704000` |
| MBOX DMA | `0x07706000`-`0x0770e000` | [DT] |
| TDM DMA channels | **tx 0, rx 7; stereo tx port 0, rx port 3** | [DT] node `tdm@0`, compatible `qca,ipq4019-tdm` |
| ADSS clocks used by TDM | `audio_tx_bclk`, `audio_tx_mclk`, `audio_rx_bclk`, `audio_rx_mclk` from the **ADSS clock controller** (compatible `qcom,adcc-ipq4019` in [QC] patch 2; AVM's tree references the same controller with ids 7, 5, 9, 3) | [DT] [QC] |
| GCC clocks feeding ADSS | `GCC_AUDIO_AHB_CLK` (id 19, enable register 0x1B010 bit 0), `GCC_AUDIO_PWM_CLK` (id 20, enable 0x1B00C bit 0), `AUDIO_CLK_SRC` (id 1, RCG at 0x1B000) | [ML] `include/dt-bindings/clock/qcom,gcc-ipq4019.h`, `drivers/clk/qcom/gcc-ipq4019.c` |
| ADSS block reset | `GCC_AUDIO_BCR` (reset id 44 = 0x2c, register 0x1B008) | [ML] [DT] (`blk_rst`) |
| GCC node | compatible `qcom,gcc-ipq4019` at 0x01800000, `#clock-cells = <1>`, `#reset-cells = <1>` | [OBS] |
| State after boot | GCC 0x1B000 = 0x80000000 (audio RCG root off), 0x1B008 = 0 (**audio block not in reset**), 0x1B00C = 0x80000000 (PWM clock off, not enabled), 0x1B010 = 0 (AHB clock not enabled); the kernel's clock summary shows all three with enable count 0 | [OBS] via `/sys/kernel/debug/regmap/1800000.clock-controller/registers` |
| Drivers present in OpenWrt | **None** for ADSS, ADCC, MBOX or TDM; no node for them in OpenWrt's device tree; `/proc/iomem` shows no audio region | [OBS] |

**Bus-hang warning:** on Qualcomm SoCs, accessing a peripheral whose interface clock is off hangs the bus (the box freezes). **Enable `GCC_AUDIO_AHB_CLK` (and whatever else [QC] enables first) through the clock framework before touching any ADSS/MBOX/TDM register**, and keep it enabled until the last access. A way to get GCC clocks without a device-tree node: look up the GCC node (`of_find_compatible_node(NULL, NULL, "qcom,gcc-ipq4019")`), build `struct of_phandle_args { .np = gcc, .args_count = 1, .args[0] = 19 }` and call `of_clk_get_from_provider()` (exported GPL in 6.12); verify, and say if you choose differently. The ADSS clock controller (ADCC) has **no driver** in OpenWrt, so its registers must be programmed by your module, following [QC] patch 2 (it shows how the pad clocks `padbclk`/`padmclk` are selected as sources: **this is how slave mode gets its clocks**).

### 4.3 Slave mode: what the sources show

The 2016 driver [QC] always runs the SoC as clock **master**, but the hardware has the switches for slave operation (identifiers from Appendix B):
- `STEREOn_CONFIG_MASTER` (BIT 8) in the stereo configuration, set by `ipq4019_config_master(ENABLE, ...)`: **clear it for slave**.
- The "raw master clock" selection: "Val 0: divided audio PLL clock, Val 1: MCLK IN" (comment next to `ipq4019_config_master`).
- Pin output enables in `ADSS_GLB_CLK_I2S_CTRL_REG` (0x18): `GLB_CLK_I2S_CTRL_RX_BCLK_OE` (bit 27), `..._TX_BCLK_OE` (28), `..._RX_MCLK_OE` (16), `..._TX_MCLK_OE` (17), set by `ipq4019_glb_clk_enable_oe()`: **must stay off in slave mode** (the chip drives these pins).
- ADCC clock parents `padbclk` and `padmclk` [QC patch 2]: select them as the source of `rxb`/`rxm` clocks for slave operation.
- TDM framing: `ADSS_GLB_TDM_CTRL_REG` (0x1C) with `RX_CHAN_NUM` (bits 19:16), `RX_SYNC_NUM` (bits 24:20), `RX_DELAY` (bit 26); `GLB_AUDIO_MODE_RECV_TDM` (bit 2); `PCM_CTRL_FRAME_SYNC_LEN`, `PCM_CTRL_PCM_CLK_MODE`; helpers `ipq4019_glb_tdm_ctrl_ch_num/sync_num/delay`, `ipq4019_glb_audio_mode`, `ipq4019_glb_rx_data_port_en`, `ipq4019_glb_rx_framesync_port_en`.
- MBOX DMA: `ipq4019_mbox_dma_prepare/start/stop`, `ipq4019_mbox_desc_own`, `ipq4019_mbox_get_elapsed_size`, `ipq4019_mbox_fifo_reset`; descriptor ring and interrupt handling in `ipq4019-mbox.c/.h`.

Whether the receiver accepts a **frame-sync pulse one bit wide every 128 clocks** in slave mode, and how the 128 clocks map to channels × bit width, is not documented anywhere we have: the TDM settings must be **parameters**.

### 4.4 What the data should contain: control mails

Derived from AVM's stock stack, **not yet verified on hardware**. All multi-byte fields little-endian. Idle fill is 0xFF (the stock receive buffer is pre-filled with 0xFF too). The mails are carried in one slot (channel) of the PCM frame; which one is unknown.
- **STUB → host:** byte 0 = `0xAA` start marker (the stock parser skips bytes until it sees one); byte 1 = sequence number (+1 per mail); bytes 2-3 = length L (at most 0xFB; larger means framing error and resync); then the body (L + 2 bytes); then a 16-bit checksum = the 16-bit sum of all bytes from the length field to the end of the body. Mails shorter than 10 bytes in total are rejected.
- **host → STUB** (for later): 16-bit word count = (mail length + 5) / 2, 16-bit TX sequence, mail bytes, 16-bit checksum (sum of the mail bytes), padded to an even length; unused TX bytes 0xFF. The 16-bit primitive id sits at mail offset 4.
- The stock stack configures its TDM API with a **word width of 8 or 16 bits**, a slot count from its link configuration and a "master" flag; it reports 9 codec slots in total, 4 of them DECT channels.

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

**Goal: "gate G2".** Receive the PCM frames the running STUB sends and capture valid control messages ("mails", section 4.4). Receive only; transmit comes later. Voice processing, the DECT protocol and the userspace daemon are out of scope.

### 7.1 History

A previous round chose a fallback (polling the pins through TLMM input registers, `DECT_IOC_CAPTURE`, still in the module; keep it). It is **proven too slow**: one TLMM register read takes **268 ns**, half a DCL period is about 488 ns. It also asked for Qualcomm's driver sources for the ADSS/MBOX/TDM path: **they are now in Appendix B** (the original 2016 upstream submission, 4 patches, ISC licence / `Dual BSD/GPL`, compatible with this GPL-2.0-only module; keep their copyright notice for any code you derive from them).

### 7.2 How to approach it

1. Read Appendix B: patch 1 (device-tree bindings and nodes: register layout of every block), patch 2 (ADSS clock controller: clock sources including the pads), patch 3 (pin functions), patch 4 (ADSS globals, MBOX DMA, TDM PCM driver, stereo blocks, CPU DAI).
2. **Do not port the ASoC framework parts** (no sound card, no codec, no ALSA PCM). Port only what the receive path needs into the module, as plain kernel code: ADSS global control, the ADCC clock settings for pad input, the stereo/TDM receive configuration in **slave** mode, and one MBOX DMA receive channel (rx channel 7, stereo rx port 3 from section 4.2).
3. Every register you write must be traceable to Appendix B or this brief; mark each with a comment naming its source (`/* [QC] ipq4019-adss.h ADSS_GLB_TDM_CTRL_REG */`). Where Appendix B does not show a value for slave operation, make it a parameter and say so.

### 7.3 Requirements

1. **Order of operations and safety:** enable the GCC audio clocks before any ADSS/ADCC/MBOX access (4.2). Never drive MCL/DCL/FSC (output enables off, 4.3). Pin functions: set GPIO 20-23 to their audio functions only while receiving; restore function 0 (GPIO input) on stop, close and unload. Do not touch pins 24-27 in Phase B.
2. **Bring-up as an explicit, step-wise ioctl sequence** so the engineer can test step by step and stop before a hang: e.g. `DECT_IOC_PCM_CLOCKS_ON`, `DECT_IOC_PCM_CONFIG` (frame size, channels, bit width, sync length, delay, edge/polarity), `DECT_IOC_PCM_START`, `DECT_IOC_PCM_STOP`, plus a `DECT_IOC_PCM_DUMP_REGS` that returns the ADSS/TDM/MBOX register values you configured (read back) for debugging. The design is yours; keep it testable in small steps.
3. `read()` on `/dev/dect0` returns received data in blocks with a small header (block counter, timestamp, overrun flag); `poll()` supported; a stats ioctl returns bytes and blocks received, overruns, DMA interrupts.
4. Sustained reception at 8000 frames per second for 10 minutes without overrun.
5. **Stop, close and unload restore everything:** DMA stopped, interrupts freed, pins back to function 0, clocks disabled again (balanced `clk_disable_unprepare`), no memory leaks. Phase A and the fallback capture keep working.
6. If something cannot be done safely with what is in Appendix B, stop at that point and say what is missing, rather than guessing register values.

### 7.4 Userspace tool and acceptance

Also deliver `src/tools/dect-pcmcap.c` (GPL-2.0-or-later): runs the ioctl sequence, reads N blocks into a file, and searches the data for `0xAA`-framed mails with a valid checksum (section 4.4), printing sequence numbers and lengths; an option to only dump without searching.

Acceptance on the box (STUB running, uploaded by the existing userspace tool, which closes `/dev/dect0` when done):
1. Phase A tests still pass; the box does not hang or reboot; no `WARN`/`BUG`.
2. Step-wise bring-up works; register dumps match what was configured.
3. Blocks arrive at the expected rate (8000 frames/s × frame size); no overrun for 10 minutes.
4. **Gate G2:** the capture tool finds `0xAA`-framed mails with correct checksums and increasing sequence numbers. If the framing is not found, the raw data must still let us determine the frame layout offline (so make bit width and channel count configurable and document what each setting does).
5. Stop/close/unload leave pins, clocks and DMA as they were.

## 8. Rules for the box (for whoever tests)

- Build with the SDK; copy the `.ko` to `/tmp`; `insmod /tmp/opendectcell.ko`. Never install it to flash, never autoload it.
- Never write flash/MTD partitions. The box can be rebooted at any time (that restores everything). Never run `sysupgrade -n` or a factory reset (it would switch DHCP on for the user's the local network).
- Never unbind/rebind `msm_serial`; never resume or kill the frozen login on `ttyMSM0` during a session.

## 9. Answer format (strict)

1. First a **feasibility statement** (at most 25 lines): based on Appendix B, can the SoC receive TDM as a **slave** of external DCL/FSC? Which registers and values configure that, which of them Appendix B shows directly, and which you must infer. If you conclude it is not safely possible, say so, explain what is missing, and stop there (no code).
2. Then a **short summary** (at most 20 lines) of your design: module structure, the ioctl sequence, clock/reset/pin handling, DMA, interrupt and locking strategy, and what you are unsure about.
3. Then **every changed or new file in full**, each introduced by a line exactly of the form
   `FILE: path/relative/to/repository/root`
   followed immediately by one fenced code block with the complete file contents (no ellipses, no "rest unchanged"). Allowed paths: `kernel/opendectcell_ipq40xx.c`, `kernel/opendectcell_pcm.c` (new), `kernel/opendectcell_pcm.h` (new), `kernel/include/uapi/opendectcell.h`, `kernel/Makefile`, `src/tools/dect-pcmcap.c` (new). Do not resend files you did not change.
4. Then a **test plan in steps** (what to run, what to look for, where to stop if a step fails) and a bullet list **"Assumptions and unverified points"**: every register, bit field, value, kernel function or header you were not certain about, with its source in Appendix B or the reason it is a guess.
5. If something in this brief is contradictory or missing, say so **before** writing code and ask; do not guess hardware facts.
