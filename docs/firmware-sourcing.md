# Firmware sourcing: how users get the STUB onto their box

Status: **decided** 2026-10-17.

## 1. Why every user needs it

The DECT chip has only a small boot loader in ROM; its RAM is lost at every power-off. FRITZ!OS therefore uploads the STUB over the UART after every chip reset (*derived statically*, the first-stage loader loader on 7412, 7362 SL and 7520). openDECTcell must do the same, and it may not ship the STUB (`AGENTS.md` §2.5). So each user fetches it once, for their own box, from an official AVM image.

The per-device **calibration data** is different: it is read at runtime from the box's own factory partition and never comes from an image (M1, `factory_cfg` in the board profile).

## 2. Decision: extract on the PC, upload to the box

1. The user downloads the official FRITZ!OS image for their model from AVM. The exact version is listed per model (§4).
2. On their PC they run `tools/dect-fw-extract.py <image>`. It
   - reads the image (both the Lantiq NAND format and plain SquashFS, using the existing `avm_nand_sqfs.py` / `sqfs_be.py` readers)
   - extracts the `dectfw_*.hex` files
   - checks their SHA-256 against the known builds and says which model and STUB build they belong to
   - prints the exact upload command for the next step
3. The user uploads the files to the box: with `scp` into `/etc/opendectcell/firmware/`, or later with the upload button in the LuCI app.
4. On the box, the init script checks the files against `fw_*_sha256` in the board profile (busybox `sha256sum`) and refuses to start with an unknown or wrong build, with a clear message saying which file is expected.

**Requirements for the PC tool:** a single tool in `tools/`, Python 3 standard library only, so it runs unchanged on Linux, macOS and Windows with a plain Python 3 install and no extra packages. (Linux distributions include Python; Windows and macOS users install it from python.org, and the user docs must say so.) Messages are written for non-technical users.

## 3. Storage on the box and upgrades

- Firmware lives in `/etc/opendectcell/firmware/` (two HEX files, under 200 KB together).
- The `opendectcell` package lists that folder in `/lib/upgrade/keep.d/opendectcell`, so an OpenWrt upgrade keeps it. Users upload once.
- If a new openDECTcell release supports a different STUB build, the box reports that on start-up and the user repeats steps 1–3.

## 4. Supported builds

Our protocol knowledge belongs to one specific STUB build per model, so the profile pins that build by hash, not "the latest FRITZ!OS".

| Model | AVM image | STUB SHA-256 |
|---|---|---|
| 7412 | `FRITZ.Box_7412.137.06.88.image` | `4fabee65d07a3214f9083de81ae54089f01bbfccb4cdbb672f37487a09cdfaa6` |
| 7362 SL | `FRITZ.Box_7362_SL-07.18.image` | `8aae3b5e3450fd2c9999e003068e786096c11d8ef53f7d47c25cebd057a797d3` |
| 7520 | `FRITZ.Box_7520-08.25.image` | `7cac33c72b2222468f34033a15c35487aa35062265ab0013bb480068ebab7b31` |
| 7520 | `FRITZ.Box_7520-07.56.image` | `9bdb332678f981d83ac31cf1b68c489ab9c979e2ac601d18b81bb802f16d0d20` |

For developers, the UCI option `fw_allow_unverified` lets an unknown build through, with a warning in the log.

**Two 7520 builds are listed since 2026-10-21.** Everything this project has measured was observed against the 08.25 second-stage STUB; the 07.56 one was added so the chip build can itself be tested as a variable, and because the stock-capture reference box runs 07.56 (see the maintainers' notes). Facts carried over from one build to the other must say which build they came from.

`[CR, the hardware-analysis step, 2026-10-21]` The **first-stage loader is byte-identical** between the two builds (`0d855b5929f8b2ad0b8d8fb38065b32e27f9357cd44a2b4e60573b1b5b6da895`), so the upload protocol is unchanged. The **factory parameter block** sits at the same position in both images with the same tag/length sequence — tags 1 (len 5), 2 (len 2), 3 (len 1), 4 (len 1), 6 (len 1), 7 (len 1) — so `dect-fwload`'s patching of tags 1–4 and the mode byte needs no change. `[derived statically, both images]`

## 5. Alternatives considered

| Alternative | Why not |
|---|---|
| Extractor running on the router | OpenWrt has no Python, so it would mean rewriting the NAND-image reader in C as an extra package, plus about 20 MB of image in the router's RAM for a step done once. |
| Download the image directly on the router | Same reasons; also depends on AVM's download URLs staying stable. |
| Ship the STUB in the package | Not allowed (blob policy, `AGENTS.md` §2.5). |

## 6. Open questions

- AVM may take old images offline. Fallback: the user's own copy, or the copy still on the box's flash under FRITZ!OS (to be checked with the M1 NAND backup; it is unknown whether the 7412 keeps an AVM copy after OpenWrt is installed).
- Should the PC tool optionally download the image from AVM itself, given a model name? Only if the URLs prove stable.
