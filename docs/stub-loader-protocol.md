# STUB upload protocol over the UART

Status: *derived statically* from `the first-stage loader.ko` of FRITZ!OS 08.25 on the FRITZ!Box 7520 (function `the vendor's loader`, source file name `the vendor's loader.c`), 2026-10-17. Partly confirmed by the loader's log on a running 7520 (`7520-DECT-analysis.md` §8: "slow" boot mode, upload of the STUB in about 6 s). These notes describe behaviour in our own words; they contain no AVM code. Image formats: `stub-image-format.md`.

The host uploads two images in two **stages**, over the same UART:
1. **Stage 1:** the chip's ROM boot loader receives the **first-level image**.
2. **Stage 2:** the first-level loader, now running on the chip, receives the **STUB image**.

Both stages use the same handshake with control bytes STX (0x02), SOH (0x01), ACK (0x06) and NAK (0x15). The UART format is assumed to be 8N1.

## 1. Handshake (both stages)

| Step | Direction | Bytes |
|---|---|---|
| 1 | chip → host | **STX** (0x02) |
| 2 | host → chip | **SOH** (0x01), then the image's length header unchanged: stage 1 **2 bytes** (LE), stage 2 **4 bytes** (BE) |
| 3 | chip → host | **ACK** (0x06) |
| 4 | host → chip | All image bytes after the length header, **except the final XOR byte** |
| 5 | chip → host | **One byte: the XOR of the data** as the chip computed it |
| 6 | host → chip | **ACK** (0x06) if it equals the image's XOR byte, otherwise **NAK** (0x15) |

After a successful stage 1 the host waits 10 ms; stage 2 starts about 50 ms later with a baud-rate change (§3) and waits for the first-level loader's STX.

### Errors and retries

| Situation | Host reaction |
|---|---|
| No STX within the wait time (about 125 ms per byte, repeated up to 5 times) | Reset the chip and start stage 1 again ("false answer" / "communication failed") |
| STX again instead of ACK in step 3 | Logged as "STX violation"; after 3 of them, wait for STX again |
| NAK in step 3 | Stage 1: 2 more bytes follow (error code); stage 2: 4 more bytes follow. Logged, then wait for STX again |
| Timeout in step 3 | Reset and start over |
| Byte other than ACK/NAK/STX in step 3 | Logged, one attempt used up |
| During stage-2 data (step 4) the chip sends 0x00 | Ignored |
| During stage-2 data the chip sends NAK | One more byte follows (error code); reset and start over |
| During stage-2 data the chip sends anything else | Reset and start over |
| In step 5 the chip sends NAK instead of the XOR | One more byte follows (error code); reset and start over |
| XOR mismatch in step 5 | Host sends NAK; reset and start over |

**Attempt budget:** stage 1 up to **16** attempts (each starts with a chip reset), stage 2 up to **5**. The caller (`the loader routine`) repeats the whole two-stage upload up to **3** times.

The host also reads incoming bytes after each received control byte with a pause of about 10 ms. The exact timing is probably not critical; our implementation keeps the 125 ms byte timeout.

## 2. Chip reset and stage-1 boot mode

Two GPIOs come from the board's hardware config: **DECT reset** and a second line named **"rd"** (7520: reset GPIO40, rd GPIO16; GPIO16 is also the UART RX pin of the host, i.e. the line driven by the chip's TX).

Reset sequence, before every stage-1 attempt:
1. "Slow" boot mode only: switch the rd line to a GPIO output and **drive it low**.
2. Drive reset low (active), wait about **10 ms**.
3. Open the UART at the stage-1 baud rate (table below).
4. Release reset (high), wait about **15 ms**.
5. Return the rd line to its normal (UART) function.
6. Wait for STX.

So the rd line is a **boot-mode strap**: the chip samples it during reset. In "fast" mode it is left alone.

**Stage-1 baud rate and mode**, from the loader flags:

| Condition | Baud |
|---|---|
| Mode word bits 8 and 9 both set (`0x300`) | 19200 |
| "slow" boot mode | **9600** |
| "fast" boot mode | 115200 |

**Which boot mode:** stage 1 alternates between the two modes over its 16 attempts. With **bit 8 of the mode word set** (all analysed boxes: 0x105, 0x119, 0x101), attempts 1–11 use **slow** and attempts 12–16 use **fast**. Without bit 8 it is the other way round. The 7520 logs "slow" on a normal boot, so its chip answers on the first slow attempt.

## 3. Stage-2 baud rate

Set from the **first-level file name**, after a pause of about 50 ms; no chip reset:

| First-level file name contains | Baud |
|---|---|
| `_slow` | **115200** (7520: `dectfw_firstlevel_441_slow.hex`) |
| `_fast` | 460800 |
| neither | 230400 |

Check: the 7520's STUB image is 54,124 bytes; at 115200 baud 8N1 that is about 4.7 s, which matches the observed 6 s for the whole upload.

## 4. Other families

The `DCX81` family uses a different start: the chip sends the text `HELLO DCX81`, the host answers with the bytes `11 BA 5E BA`, then sends a 4-byte length (`len16` followed by two zero bytes) and the data, and stage 1 ends without the XOR check. Not needed for the `441` boxes; recorded for completeness.

## 5. Implications for openDECTcell

- `dect-fwload` needs: the UART (on the 7520 it is also the console, `ttyMSM0`), **reset** and the **rd strap**. Per `hardware-abstraction.md`, reset and the strap belong to the kernel module (`/dev/dect0`); the strap also needs the pin switched between GPIO and UART function.
- The protocol engine is pure byte handling plus three callbacks (UART read with timeout, UART write, set baud) and one reset callback, so it can be tested against a chip simulator on the PC.

## 6. Corrections to earlier notes

`7520-DECT-analysis.md` §4 says stage 2 "answers with a 16-bit CRC". The code compares **one byte** (the XOR) in both stages; "crc" in the log messages refers to that byte.

## 7. Open questions

- Whether the 7520's chip boots in "fast" mode without the strap (it would save the pin juggling). Test on hardware.
- Meaning of the NAK error codes (2 or 4 bytes in step 3, 1 byte elsewhere).
- The UART line format (8N1 assumed).
