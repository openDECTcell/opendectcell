# STUB upload images: format and patching

Status: *derived statically* from `the first-stage loader.ko` of FRITZ!OS 08.25 on the FRITZ!Box 7520 (ARM build; functions `the HEX reader`, `(vendor-internal name)`, `replace_tag`, `the loader patch routine`, `the loader routine`), 2026-10-17. Checked against the 7520 STUB file (`dectfw_secondlevel_441.hex`, SHA-256 `7cac33c7…7b31`) and against the loader's log on a running 7520 (`7520-DECT-analysis.md` §8). These notes describe behaviour in our own words; they contain no AVM code.

The firmware files are Intel HEX. Before upload, the host turns each file into a binary **upload image**. The first-level loader and the STUB use two different image layouts.

## 1. Firmware families

The family is chosen from the **second-level file name**, by substring, in this order:

| Name contains | First-level load address | STUB patch area | Notes |
|---|---|---|---|
| `441` | 0x10080 | via descriptor at chip address **0x0** | All analysed boxes (7412, 7362 SL, 7520) |
| `488` | 0x10080 | via descriptor at chip address **0xF0000** | |
| `DA14495` | 0x20000000 | fixed: 50 bytes at 0x200000C0 | |
| `DCX81` | 0xA0000 | fixed: 50 bytes at 0x400000 | Different loader flags |
| (none) | 0x10080 | as `488` | |

The first-level file name selects the boot speed: `_slow` and `_fast` set different loader flags (see the loader-protocol notes).

## 2. First-level image (16-bit length)

| Offset | Size | Content |
|---|---|---|
| 0 | 2 | Data length N, **little-endian** |
| 2 | N | Data bytes in file order |
| 2 + N | 1 | XOR of the N data bytes |

The data is laid out **linearly from the family's first-level load address**, with no address information in the image. Gaps between records (and between the load address and the first record) are filled with zero bytes, so N = end address − load address. The 7520's `dectfw_firstlevel_441_slow.hex` has no gaps: 618 bytes from 0x10080.

## 3. Second-level (STUB) image (32-bit length, segmented)

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | Payload length P, **big-endian** |
| 4 | P | Payload: a sequence of **segments** |
| 4 + P | 1 | XOR of the P payload bytes (recomputed after patching) |

Each segment:

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | Chip start address, **big-endian** |
| 4 | 4 | Data length L, **big-endian** |
| 8 | L | Data |

**Segment boundaries:** a new segment starts
- at the first data record
- whenever a data record's 16-bit address field differs from the end of the previous record (address + count)
- after every extended-address record (type 02 or 04) and at the end-of-file record (type 01)

The segment address is the record address plus the current extended base: type 02 sets base = value << 4, type 04 sets base = value << 16. Every record's checksum is verified. The image must not exceed 0x2FFFF bytes of data.

**Check (7520, FRITZ!OS 08.25):** the STUB file has 54,047 data bytes in 9 segments, which gives P = 54,047 + 9 × 8 = **54,119**. That is exactly the length the loader logs on the box.

## 4. Patching the STUB

1. **Locate the patch area.** For `441`/`488`, a 6-byte descriptor at the family's chip address (0x0 or 0xF0000) holds the patch-area address (32-bit LE) and size (16-bit LE). A chip address is resolved by walking the segments; the whole range must lie inside one segment. On the 7520 STUB: patch area at **0xA0, 53 bytes**.
2. **Default TLV list.** The patch area already holds a TLV list (tag u8, length u8, value), ended by tag 0. On the 7520 STUB: tags **1 (5 bytes), 2 (2), 3 (1), 4 (1), 6 (1), 7 (1), 10 (1)**.
3. **Replace, never append.** A TLV is patched only if the area already contains the same tag **with the same length**; its value is overwritten. Unknown tags and length mismatches are skipped (logged).
4. **Factory TLVs.** The host reads up to **64 bytes** of TLVs from the box's factory DECT configuration (AVM prom config, id 1) and applies each one as in step 3. If that block is missing, only the mode byte is patched. If it is longer than 64 bytes, it is ignored.
5. **RFPI placeholder.** If a factory TLV has tag 1, length 5 and starts with the bytes `30 32 37 30` (ASCII "0270"), it is a placeholder. The host then builds the RFPI from the urlader variable `maca` (MAC address text `xx:xx:xx:xx:xx:xx`), using the hex digits at character positions 10, 12, 13, 15 and 16:
   - value v = 0x70B00000 | (d16 << 3) | (d15 << 7) | (d13 << 11) | (d12 << 15) | ((d10 << 19) & 0x80000)
   - RFPI bytes: `02 70` followed by the low 24 bits of v, most significant byte first
6. **Mode byte.** If the low byte of `the loader mode value` is not zero, TLV **tag 7, length 1** is set to it (0x05 on the 7520, 0x19 on the 7412, 0x01 on the 7362 SL).
7. **Tag 9 = STUB version information.** It is not patched: the host only looks for tag 9 with length 22 in the patch area and logs it. The 7520's 08.25 STUB has none ("tag 9 not found").
8. **Checksum.** The XOR of the whole payload is recomputed and stored after it.

**Skipped in factory test mode:** if the urlader variable `ptest` is set to the calibration value, no patching happens.

**Check:** with the a second unit's RFPI and mode 5 patched, the XOR does not match the logged value. That is expected: that box's factory block also overrides other default TLVs (probably calibration: tags 2, 3, 4, 6, 10), and their values are unknown. Re-check with the spare box's factory block once it is found in the backup.

## 5. Correction to earlier notes

`7412-spi-tdm-and-loader.md` §2 says the descriptor sits at chip address 0x0006. The 08.25 code reads the **6-byte** descriptor at chip address **0x0** (`441`), and the 7520 STUB has a valid descriptor there. The earlier note most likely mixed up the size with the address. Verify on a 7412 STUB when one is available.

## 6. Open questions

- Meaning of the default TLVs 2, 3, 4, 6 and 10 (probably RF calibration).
- Where the factory DECT config (prom config id 1) lives in flash: urlader or TFFS. Look in the spare box's backup.
- Exact `ptest` value that disables patching (the 7412 notes say `dect=cal`).
