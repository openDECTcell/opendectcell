# IPQ4019 PCM/TDM receive path: hardware reference for the kernel module (Phase B)

Status: research complete 2026-10-17. This document is the **only input** needed to write Phase B of `kernel/opendectcell_ipq40xx.c` (the PCM receive path to the DECT chip on the FRITZ!Box 7520/7530). It is written in our own words; no vendor code is reproduced.

**Sources and tags** (AGENTS.md §2):
- **[AVM]** *derived statically* from AVM's **published GPL source** for FRITZ!OS 08.25 on the 7520 (`osp.avm.de/fritzbox/fritzbox-7520/source-files-FRITZ.Box_7520-cortexa9-08.25.tar.gz`, kernel tree `sources/kernel/linux`). Function names are given for traceability (mostly `sound/soc/qcom/ipq/ipq-adss-avm.c`). This is the code that runs this DECT chip in stock FRITZ!OS, in **TDM slave mode**.
- **[QC]** Qualcomm code in the same tree (`ipq-adss.h`, `ipq-mbox.h`, `drivers/clk/qcom/adcc-ipq4019.c`, `qcom-ipq4019-audio.dtsi`).
- **[ML]** mainline Linux v6.12.94. **[OBS]** *observed on hardware* (spare 7520, OpenWrt 25.12.5, 2026-10-17).

**Rule for the session that writes the module:** work from this document, `docs/kmod-spec.md` and the existing module. Do **not** load AVM code into the conversation, neither binaries nor the GPL source; everything needed is here. If a fact is missing, record the question in this document (section 12) and ask the user.

## 1. What the module must do (Phase B, receive only)

Configure the IPQ4019 audio subsystem ("ADSS") as a **TDM slave** of the DECT chip's clocks, receive the chip's PCM frames by DMA, and deliver them to userspace through `/dev/dect0`. Goal "gate G2": find the STUB's control mails (0xAA framing, `docs/stub-pcm-control-channel.md`) in the received data. Transmit (pin 27, MBOX0/stereo 0) comes later.

## 2. Signals, frame format, pins

| Item | Value | Source |
|---|---|---|
| Clock master | **The DECT chip** (SoC is slave); clocks only run while the STUB runs | [OBS] |
| Bit clock DCL | 1.024 MHz (measured 1,019,367-1,027,749 Hz from the SoC) | [OBS] |
| Master clock MCL | 10.368 MHz ("external clock 10.368 MHz" in the clock-select comment) | [AVM] `adss_config_clksel` |
| Frame sync FSC | 8 kHz, about one bit clock wide | [OBS] |
| Frame | **8 slots × 16 bits = 128 bit clocks**, 16-bit words only; AVM's API comment: "slots: only 8 tested" | [AVM] `tdm_if_adss_config_init`, `adss_config_tdm` |
| RX data delay | parameter; "1 = serial data delay of 1 clock". The value AVM's DECT driver passes is unknown (proprietary module): **default 1, allow 0** | [AVM] |
| FSYNC high duration | `sync_num = 0` for both directions | [AVM] `__tdm_if_config_init` |

Pins (TLMM control register at `0x01000000 + 0x1000 × gpio`, function select in bits 5:2; input level at `+4` bit 0) [ML][QC][OBS]:

| GPIO | Signal | Function for TDM | AVM's direction | Phase B |
|---|---|---|---|---|
| 20 | MCL (rx group) | 2 (`i2s_rx`) | input | set function 2 |
| 21 | DCL (rx group) | 2 (`i2s_rx`) | input | set function 2 |
| 22 | FSC (rx group) | 2 (`i2s_rx`) | input | set function 2 |
| 23 | DI (chip → SoC) | 3 (`i2s_rx`) | input | set function 3 |
| 24, 25, 26 | MCL, DCL, FSC (tx group) | 3 (`i2s_tx`) | input | leave as GPIO input (function 0) |
| 27 | DO (SoC → chip) | 3 (`i2s_td`) | **output** | leave as GPIO input (function 0) until transmit |

After OpenWrt boots, all eight are unclaimed GPIO inputs with pull-up [OBS]. AVM's board tree marks 20-26 as inputs and 27 as output (its pin table, `param` 12 = input, 10 = output) [AVM]. Pull settings for the audio function are not documented: keep the current pull. Restore function 0 on stop, close and unload. Writing the function field directly (as Phase A does for GPIO 16) is the established method in this module.

## 3. Address map

All physical. Map with `ioremap()` (no `request_mem_region`, other users may exist).

| Block | Base | Size to map | Notes | Source |
|---|---|---|---|---|
| ADSS global + ADCC clock registers | `0x07700000` | `0x300` | GLB registers at the low offsets, ADCC (audio clock controller) at `0x38`-`0x213` | [QC][AVM] |
| MBOX3 (receive DMA) | `0x0770E000` | `0x1000` | interrupt **GIC SPI 159** | [QC] dtsi |
| STEREO3 (receive) | `0x0770F000` | `0x1000` | | [QC] dtsi |
| MBOX0 / STEREO0 (transmit, later) | `0x07708000` / `0x07709000` | `0x1000` each | interrupt SPI 156 | [QC] dtsi |
| GCC (clock controller, owned by OpenWrt's driver) | `0x01800000` | (do not map) | use the clock framework | [ML][OBS] |
| TLMM (pins) | `0x01000000` | per pin | already used by Phase A | [ML] |

Interrupt controller: node `interrupt-controller@b000000`, compatible `qcom,msm-qgic2`, `#interrupt-cells = 3` [OBS]. SPI 159 = Linux GIC hwirq **191**. AVM requests it as **level high**. There is no device-tree node for the audio blocks in OpenWrt, so map the interrupt yourself: `struct of_phandle_args { .np = <GIC node>, .args_count = 3, .args = { 0 /* SPI */, 159, 4 /* IRQ_TYPE_LEVEL_HIGH */ } }` and `irq_create_of_mapping()`; free with `irq_dispose_mapping()` on unload.

## 4. Clocks and reset outside the ADSS

| Item | Facts | What to do |
|---|---|---|
| `GCC_AUDIO_AHB_CLK` (GCC clock id 19, branch register `0x1B010`) | Register reads 0: enable bit 0 clear, but **bit 31 (clock off) clear too**, so the clock is very likely **running** (voted on by hardware) [OBS]. AVM's kernel never enables it explicitly [AVM]. | **Read the branch register first** (read-only mapping of `0x0181B010`): if bit 31 is clear, the clock runs and the module does not touch it. Only if it is set: GCC node compatible `qcom,gcc-ipq4019`, `of_clk_get_from_provider()` with `args = { 19 }`, `clk_prepare_enable()`, balanced disable later. Taking the reference while it runs on the vote is harmful: `clk_disable_unprepare()` then warns "gcc_audio_ahb_clk status stuck at 'on'" (a kernel `WARN`) because the off bit never sets [OBS] |
| `GCC_AUDIO_PWM_CLK` (id 20) | off (bit 31 set) [OBS]; not used by the TDM path | leave alone |
| `GCC_AUDIO_BCR` (reset id 44, register `0x1B008`) | reads 0 = block not in reset [OBS]. AVM pulses it once at probe: assert, wait 20-24 ms, deassert [AVM] `gcc_audio_blk_rst` | **Not needed**: receive works without it [OBS] (section 13) |

**Bus-hang rule:** never access `0x0770xxxx` before the GCC audio AHB clock reference is taken. A read of `ADSS_GLB_CHIP_CTRL_I2S` (offset `0x10`) is the first, harmless test access.

## 5. ADSS global registers (base `0x07700000`) [QC] `ipq-adss.h`

| Offset | Register | Bits used |
|---|---|---|
| `0x10` | CHIP_CTRL_I2S | bit 0 INTERFACE_EN |
| `0x14` | I2S_RST | write `0xF` = reset the I2S/MBOX blocks (IPQ4019 value), then `0` |
| `0x18` | CLK_I2S_CTRL | clock **output** enables: bit 16 RX_MCLK_OE, bit 17 TX_MCLK_OE, bit 27 RX_BCLK_OE, bit 28 TX_BCLK_OE (**all 0 in slave mode**) |
| `0x1C` | TDM_CTRL | bits 3:0 TX_CHAN_NUM (= slots − 1), bits 8:4 TX_SYNC_NUM, bits 19:16 RX_CHAN_NUM (= slots − 1), bits 24:20 RX_SYNC_NUM, bit 25 TX_DELAY, bit 26 RX_DELAY |
| `0x30` | AUDIO_MODE | bit 0 XMIT_TDM (else I2S), bit 2 RECV_TDM, bits 6:4 I2S0_TXD_OE (value 7), bit 7 I2S0_FS_OE, **bit 8 I2S3_FS_OE**, **bit 9 I2S3_RXD_OE**, bit 10 SPDIF_OUT_OE, bit 28 B1K ("cross 1K boundary") |

About bits 8 and 9: AVM sets them **in slave mode** on this box with the DECT chip driving FSC and DI; its comments call them "Frame Sync Port Enable for I2S3 RX" and "I2S3 RX Data Port Enable" [AVM] `adss_glb_rxtx_framesync_port_en`, `adss_glb_rxtx_data_port_en`. They are receiver port enables, not clock drivers. **Set them.** AVM also sets the I2S0 (transmit) bits 4-7 in the same calls; in receive-only Phase B leave those clear (pin 27 and the tx pins are not in their audio function anyway).

## 6. ADSS clock controller (ADCC), registers relative to `0x07700000`

The ADCC sits at `0x07700038`; AVM addresses its registers from the ADSS base [AVM] `ipq-avm.h`. OpenWrt has **no ADCC driver**, so the module programs these directly.

| Offset | Register | Use |
|---|---|---|
| `0x104` | RXB_CFG_MUXR | bits 10:8 bit-clock source: 0 XO, 1 PLL, **2 pad BCLK**, 3 pad MCLK |
| `0x108` | RXB_MISC | bits 9:1 bit-clock divider − 1 |
| `0x10C` | RXB_CBCR | bit 0 enable, bit 31 clock-off status |
| `0x124` | RXM_CFG_RCGR | bits 10:8 master-clock source (**2 = pad MCLK**), bits 4:0 divider |
| `0x128` | RXM_MISC | bits 7:4 divider |
| `0x12C` | RXM_CBCR | bit 0 enable, bit 31 clock-off status |
| `0x144`, `0x148`, `0x14C` | TXB_CFG_MUXR, TXB_MISC, TXB_CBCR | as RXB |
| `0x164`, `0x168`, `0x16C` | TXM_CFG_RCGR, TXM_MISC, TXM_CBCR | as RXM |
| `0x200` | AHB_CBCR (ADCC offset 0x1C8) | bit 0: audio AHB branch |
| `0x204`, `0x208`, `0x20C`, `0x210` | AHB_I2S0, **AHB_I2S3**, AHB_MBOX0, **AHB_MBOX3** CBCR | bit 0 enable |

AHB branches: AVM never writes them (they are on after the block reset) [AVM]. Read them at bring-up; if bit 0 of AHB, AHB_I2S3 or AHB_MBOX3 is 0, set it.

**Pad-clock selection, exactly as AVM does it for external clocks** [AVM] `adss_config_clksel(2)`, called from `adss_config_clk` on the IPQ4019 when `ext_clk = 1`:
1. Write `0` to RXB_CBCR, RXM_CBCR, TXB_CBCR, TXM_CBCR (gates off). Wait 10-12 ms.
2. Write `0` to RXB_MISC, TXB_MISC, RXM_MISC, TXM_MISC (dividers 1).
3. Write `0x200` (source 2) to RXB_CFG_MUXR, TXB_CFG_MUXR, RXM_CFG_RCGR, TXM_CFG_RCGR. (AVM does not set an "update" bit.)
4. Wait 10-12 ms. Write `1` to the four CBCRs.

Check: with the STUB running (chip clocks present), bit 31 of RXB_CBCR and RXM_CBCR should read **0** (clock running); with the chip in reset it should read 1. This is the first proof that the pad clocks reach the ADSS.

## 7. Stereo blocks (receive: STEREO3 at `0x0770F000`) [QC] `ipq-adss.h`

| Offset | Register |
|---|---|
| `0x00` | CONFIG: bit 8 MASTER, bit 9 sample-count clear type, **bit 10 MCK_SEL**, bit 11 I2S word size 32 (0 = 16), bits 13:12 DATA_WORD_SIZE, bit 14 MONO, bit 16 MIC word size 32 (0 = 16), bit 17 PCM_SWAP, bit 18 NO_I2S_DELAY, bit 19 RESET, bit 20 MIC_RESET, **bit 21 ENABLE**, bit 23 SPDIF_ENABLE, bit 24 MIC_SWAP |
| `0x04` | VOLUME |
| `0x08` | MASTER_CLOCK |
| `0x0C`, `0x10` | TX sample count LSB, MSB |
| `0x14`, `0x18` | **RX sample count** LSB, MSB |
| `0x1C` | underrun count |
| `0x20` | **overrun count** |
| `0x24` | TX complete count |
| `0x28` | RX complete count |

Slave configuration [AVM] `adss_config_tdm(enable = 0, master = 0, ext_clk = 1)`: read CONFIG; clear MASTER, ENABLE, DATA_WORD_SIZE, I2S word size 32, MIC word size 32, MCK_SEL; set **DATA_WORD_SIZE = 1** (16-bit) and **MCK_SEL** (AVM's comment: without MCK_SEL the external clock is "destroyed"); leave MASTER clear; write back. ENABLE is set later when DMA starts (section 9).

The **RX sample counter** (`0x14`) is a good bring-up check: it should count once the stereo block is enabled and clocks and frame sync arrive, even before DMA works.

## 8. Global configuration sequence (slave, slots = 8) [AVM] `intf_audio_adss_probe`, `__tdm_if_config_init`

Once (bring-up):
1. Take the GCC audio AHB clock reference (section 4).
2. CHIP_CTRL_I2S: set bit 0 (INTERFACE_EN), wait 4-5 ms.
3. AUDIO_MODE: set bit 28 (B1K).

Configuration:
4. I2S_RST: write `0xF`, wait 4-5 ms, write `0`.
5. Pad-clock selection (section 6).
6. With local interrupts off (AVM does this; keep the block short):
   - TDM_CTRL: TX_CHAN_NUM and RX_CHAN_NUM = slots − 1 = **7**.
   - CLK_I2S_CTRL: clear bits 16, 17, 27, 28 (no clock outputs).
   - AUDIO_MODE: set bit 9 (I2S3_RXD_OE) and bit 8 (I2S3_FS_OE).
   - AUDIO_MODE: set bit 0 and bit 2 (TDM transmit and receive mode).
   - STEREO3 CONFIG: slave configuration (section 7). (AVM also configures STEREO0 the same way; harmless, optional in Phase B.)
   - TDM_CTRL: RX_SYNC_NUM = 0, TX_SYNC_NUM = 0; RX_DELAY = rxdelay (default 1); TX_DELAY = txdelay.
7. Pins 20-23 to their audio functions (section 2).

Teardown [AVM] `tdm_if_adss_config_exit`: stereo CONFIG with ENABLE and MASTER clear; TDM_CTRL channel numbers 0; clock output enables clear; AUDIO_MODE bits 4-9 clear (all port enables). AVM's driver removal also clears CHIP_CTRL_I2S bit 0. **Our additions** (to leave the hardware as OpenWrt booted it): pins 20-23 back to function 0, the four rx/tx CBCRs to 0, release the GCC clock reference.

## 9. MBOX DMA (receive uses MBOX3) [QC] `ipq-mbox.h`, [AVM] `mbox_*` functions

**Naming trap:** the MBOX names directions from the DMA engine's view. **Receive (capture) uses the "TX" descriptor base, "TX" control register and the "TX DMA complete" interrupt bit** of MBOX3 [AVM] comment in `mbox_dma_start`.

Registers (MBOX3 base `0x0770E000`):

| Offset | Register | Bits |
|---|---|---|
| `0x10` | DMA_POLICY | bit 31 SW_RESET; bit 17 TX_INT_TYPE; bit 16 RX_INT_TYPE; bits 15:12 SRAM_AC = upper 4 bits of the descriptor physical address; bits 7:4 TX_FIFO_THRESHOLD; bit 10 / bit 8 swap options (AVM leaves them 0) |
| `0x18` | RX_DESCRIPTOR_BASE (used for *transmit*) | |
| `0x1C` | RX_CONTROL | bit 0 stop, bit 1 start, bit 2 resume |
| `0x20` | **TX_DESCRIPTOR_BASE (used for receive)** | physical address & `0x0FFFFFFF` |
| `0x24` | **TX_CONTROL (used for receive)** | bit 0 STOP, bit 1 START, bit 2 RESUME |
| `0x38` | FRAME | |
| `0x40` | FIFO_TIMEOUT | |
| `0x44` | INT_STATUS | bit 4 rx underflow, bit 5 tx overflow, **bit 6 TX_DMA_COMPLETE (= receive done)**, bit 10 RX_DMA_COMPLETE, bit 12 rx FIFO underflow, bit 13 tx FIFO overflow, bit 14 BI_TX_RX |
| `0x4C` | INT_ENABLE | **bit 6** TX_DMA_COMPLETE, bit 10 RX_DMA_COMPLETE |
| `0x58` | FIFO_RESET | bit 0 TX_INIT, bit 2 RX_INIT |

**Interrupt acknowledge on the IPQ4019 is inverted:** to clear bits `m`, write `status & ~m` to INT_STATUS (bits written as 0 are cleared, bits written as 1 are kept) [AVM] `mbox_interrupt_ack`, [QC] `IPQ_MBOX_IRQ_ACK`.

**DMA addresses:** the MBOX takes **28-bit** addresses (`phys & 0x0FFFFFFF`); the upper 4 bits go into DMA_POLICY SRAM_AC [AVM][QC]. RAM on this box is `0x80000000`-`0x8FFFFFFF` [OBS], so SRAM_AC = 8.

**Descriptor** (one per ring entry; little-endian 32-bit words) [QC][AVM]:

| Word | Bits | Field |
|---|---|---|
| 0 | 11:0 | length (bytes) |
| 0 | 23:12 | size (bytes) |
| 0 | 24 | vuc (S/PDIF only, 0) |
| 0 | 25 | **ei**: interrupt when this descriptor completes |
| 0 | 29:26 | reserved, 0 |
| 0 | 30 | EOM, 0 |
| 0 | 31 | **OWN**: 1 = hardware owns it (empty/ready), 0 = completed (CPU owns it) |
| 1 | 31:0 | buffer physical address (28-bit form) |
| 2 | 31:0 | next descriptor physical address (28-bit form) |
| 3-38 | | 36 words of S/PDIF channel-status data, unused, 0 |

Descriptor stride: 156 bytes rounded up to the L1 cache line size (the IPQ4019 uses the full 156-byte layout). Size and length must be ≤ 4095.

Buffers [AVM] `mbox_dma_init`: **two ping-pong buffers** of `slots × 2 × frames_per_dma` bytes each, cache-line aligned; `n_desc ≥ 2` descriptors in a ring whose buffer pointers alternate between the two buffers (descriptor *i* uses buffer *i* mod 2) and whose next pointers wrap around. AVM's comment suggests a 4 ms period (32 frames): **8 × 2 × 32 = 512 bytes per buffer**; make frames-per-buffer a parameter.

Memory: AVM uses kernel memory with explicit cache maintenance (flush descriptors and buffers before handing them to the hardware, invalidate a receive buffer before reading it). In 6.12 use the DMA API instead (`dma_alloc_coherent()` on a device with a 32-bit DMA mask, e.g. a small platform device the module registers itself; then mask addresses to 28 bits when writing them to the hardware).

Sequences:
- **Prepare** [AVM] `mbox_dma_prepare_desc`, `mbox_dma_prepare_reg`: fill the ring (OWN = 1, ei = 1, EOM = 0, size = length = buffer bytes); DMA_POLICY: set SW_RESET, wait 4-5 ms, clear it; STEREO3: clear ENABLE, write 0 to the counters at `0x0C`-`0x28`, set RESET and MIC_RESET, wait 5-6 ms; MBOX FIFO_RESET: write `0x5`; STEREO3: clear RESET and MIC_RESET; DMA_POLICY: clear SRAM_AC and TX_FIFO_THRESHOLD, set TX_INT_TYPE, SRAM_AC = 8, TX_FIFO_THRESHOLD = 8; TX_DESCRIPTOR_BASE = first descriptor & `0x0FFFFFFF`.
- **Start** [AVM] `mbox_dma_start`: INT_ENABLE |= bit 6; TX_CONTROL = START (bit 1); STEREO3 CONFIG |= ENABLE (bit 21).
- **Interrupt** [AVM] `mbox_dma_irq`, `mbox_ack_desc`: read INT_STATUS; if bit 6 (or 10) is set, acknowledge it (inverted, above) and walk all descriptors: each with OWN = 0 holds a filled buffer (identify which of the two by its buffer pointer); make it visible to the CPU, deliver it, set size = length = buffer bytes, ei = 1, OWN = 1, flush. More than one completed descriptor per interrupt means an interrupt was missed; none means "complete but no buffer" (AVM retries for up to about 100 µs). Count underflow/overflow bits (4, 5, 12, 13) as errors and acknowledge any remaining status.
- **Stop** [AVM] `mbox_dma_stop`: INT_ENABLE &= ~bit 6; TX_CONTROL = STOP (bit 0); wait 10-12 ms; STEREO3 CONFIG &= ~ENABLE. Then free the interrupt and the memory.

**Transmit first?** AVM always starts the transmit DMA (MBOX0) first, busy-waits for its first completion and starts receive about 125 µs later, so both buffers line up [AVM] `tdm_if_adss_dma_start`. Phase B should **try receive alone first**. If no receive interrupts arrive although the RX sample counter runs, add the transmit side with idle buffers (all 0xFF), pin 27 still not in its audio function.

## 10. Data layout to expect

Each 125 µs frame = 8 slots × 16-bit words; buffers contain whole frames in time order. Which slot carries the control mails, the byte order within a word and the bit order are **unknown** and are what the capture must reveal: the mails start with `0xAA`, the idle fill is `0xFF` (`docs/stub-pcm-control-channel.md`). Keep raw data unmodified (no swapping) and let userspace try the variants.

## 11. Bring-up plan with checkpoints (one ioctl step at a time)

| Step | Action | Expected |
|---|---|---|
| 1 | GCC clock reference, read CHIP_CTRL_I2S and the AHB CBCRs | reads return without hang; print the values |
| 2 | Interface enable, B1K | read-back matches |
| 3 | I2S reset, pad-clock selection | CBCRs read `0x00000001` or `0x80000001`; the bit clock only shows up after step 4 [OBS] |
| 4 | Global TDM config, stereo slave config, pins 20-23 to audio function | read-back of TDM_CTRL, AUDIO_MODE, CLK_I2S_CTRL, STEREO3 CONFIG; with the STUB running RXB_CBCR bit 31 = 0 now [OBS] |
| 5 | Enable STEREO3 without DMA | RX sample counter (`0x14`) moves, then stops at `0x00080008` (FIFO full); MBOX INT_STATUS bit 13 (FIFO overflow) set [OBS] |
| 6 | DMA prepare and start | interrupts at 1 per buffer period (250/s for 4 ms buffers); overrun counter stays 0 |
| 7 | Read data, search for `0xAA` mails | gate G2 |
| 8 | Stop, unload | all restored (section 8 teardown), no warnings |

Provide a register-dump ioctl that returns the GLB, ADCC (section 6 list), STEREO3 and MBOX3 registers at any time.

## 12. Open questions

- RX data delay 0 or 1 (AVM's DECT driver value unknown). The capture with 1 shows a clean, stable byte pattern (section 13), but that alone does not prove the alignment. Asked in Q-0001.
- Slot of the control mails, byte and bit order: not visible yet, because the STUB sends no mails on its own (section 13). Asked in Q-0001.
- Answered by the hardware (section 13): receive works without the transmit side; the GCC audio block reset is not needed; the RX sample counter counts 32-bit words.

## 13. Observed on hardware (Phase B module, 2026-10-17)

Source: *observed on hardware*, spare 7520, OpenWrt 25.12.5, `kernel/opendectcell_pcm.c` and `dect-pcmcap` (commit 088ebe6), STUB of FRITZ!OS 08.25 running, slots 8, rx_delay 1, 32 frames per block, 4 descriptors.

| Item | Observation |
|---|---|
| GCC audio AHB clock | branch register reads `0x00000000`: running on a hardware vote; enabling and disabling it through the clock framework triggers a `WARN` on disable (section 4) |
| ADSS state after boot | CHIP_CTRL_I2S 0, AUDIO_MODE 0, TDM_CTRL `0x00010001`, all ADCC AHB branches `0x00000001` (on), rx/tx CBCRs `0x00000001`, STEREO3 CONFIG `0x00000102`, MBOX3 DMA_POLICY `0x00008040`, INT_STATUS `0x00000001` |
| Bit clock into the ADSS | appears only once pins 20-23 are in their audio functions **and** AUDIO_MODE has the TDM receive bits; MCLK branch reads "running" even before |
| After step 4 | TDM_CTRL `0x06070007`, AUDIO_MODE `0x10000305`, CLK_I2S_CTRL 0, STEREO3 CONFIG `0x00001402`, TLMM 20-22 `0x480a`, 23 `0x480e` |
| DMA | DMA_POLICY `0x00028080` (SRAM_AC 8), coherent memory at `0x83xxxxxx`, Linux IRQ 70 for GIC SPI 159; **exactly 250 interrupts/s**, one block per interrupt, no empty or multiple completions, no error bits, no sequence gaps over 10 s. Receive works **without the transmit side** |
| RX sample counter | two identical 16-bit halves per register; about 32,000 per second = **4 per frame**, i.e. it counts 32-bit words. Overrun counter 0 while DMA runs |
| STUB upload during capture | the receiver keeps running while the chip is in reset (no clocks, no interrupts) and resumes when the chip restarts; first about 16 ms all 0xFF, then the STUB's pattern |
| Teardown | clean: pins, CBCRs, AUDIO_MODE and CHIP_CTRL back to the boot values, no warnings |

**Frame content** (16 bytes per frame in DMA-buffer order; each 16-bit slot is stored as a little-endian word, so the second byte of a slot is probably the first on the wire): bytes 0-1 `FE FE`, 2-3 `FF FF`, 4-7 `FE`, 8 `08` (briefly `00` after boot), 9 `40`, **10 A-law audio** (0x55/0xD5 at rest, a decaying waveform right after boot), 11 `00`, 12-15 `FF`. Everything except byte 10 is constant. **No 0xAA-framed mail** was found in any slot, byte order, bit order or bit shift, neither from an idle STUB nor across a fresh upload. Working assumption: the control channel is one of the idle 0xFF positions and the STUB only talks after the host has sent a mail (Q-0001).
