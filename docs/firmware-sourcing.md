# Firmware sourcing: how users get the STUB onto their box

Status: **decided** 2026-10-17. **Upload page built** 2026-10-31 (§2a). **Built** 2026-10-25: `tools/dect-fw-extract.py` (tar, SquashFS, the 7412/7362 SL NAND `filesystem.image` since 2026-11-01, or folder; synthetic tests in `tests/test_fw_extract.py`; not yet run on a real image), the package's `/etc/opendectcell/firmware/` with `keep.d`. The on-box SHA-256 check in the init script is built too (refuses unknown builds unless `fw_allow_unverified`; tested on the box).

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
3. The user uploads the files to the box: in LuCI (Network > DECT > Firmware: one *Upload…* button per file), or with `scp` into `/etc/opendectcell/firmware/`.
4. On the box, the init script checks the files against `fw_*_sha256` in the board profile (busybox `sha256sum`) and refuses to start with an unknown or wrong build, with a clear message saying which file is expected.

**Requirements for the PC tool:** a single tool in `tools/`, Python 3 standard library only, so it runs unchanged on Linux, macOS and Windows with a plain Python 3 install and no extra packages. (Linux distributions include Python; Windows and macOS users install it from python.org, and the user docs must say so.) Messages are written for non-technical users.

### 2a. The LuCI upload page (2026-10-31)

The page does **not** read an AVM image in the browser: the user runs the extractor on the PC first, then uploads the two `.hex` files it wrote (option 2c; reading the image in JavaScript stays possible later). On the Firmware tab, and on the cell page in cell mode, one row per file the board needs: its name, the SHA-256 the board profile pins (or "any build openDECTcell knows" on the 7520, whose profile has no pin), its state, *Upload…* and *Delete*.

- The browser refuses a file over 400 000 bytes before sending it, and LuCI's own `cgi-upload` leaves it in `/tmp/opendectcell-fw-upload.<role>`.
- `/usr/share/opendectcell/firmware.sh install <role> <file>` (the package `opendectcell`) takes it from there: it accepts only that path (no `..`, no symlink), takes the destination name from the board profile or the `first`/`second` options, never from the browser, and checks the SHA-256. Wrong build for the board, unknown build, or a known build under another build's name (the name decides the chip RAM addresses the daemon uses) are refused, and the old file stays. With `fw_allow_unverified` it lets them in, as the init script lets them run. A good file is written to `.<name>.new` with mode 0600 and moved into place.
- A new file takes effect at the next chip boot, not at once.
- ACL: `luci-app-opendectcell` may run only `firmware.sh status|install *|delete *`, write `/tmp/opendectcell-fw-upload.*` and use `cgi-io` upload.
- The hash table is kept in three places: this script, the init script (hashes) and `tools/dect-fw-extract.py` (hashes and names). `tests/test_firmware_sh.py` (in `make check`) fails if they drift. The extractor also knows the 7362 SL build, which no board profile uses.
- Tested: the script, under dash and busybox ash, against a scratch folder, board profile and `uci`. **Not yet tested on a box:** the browser side (file picker, `cgi-upload` and its ACL, the table, delete) and the extractor on a real image.

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

**Which build is actually on the chip** is visible in the log: the STUB sends its ASCII build stamp (`0xFFF2`, "RAM: <date>") as the first mail after the first contact *following a chip boot*, so a `dect-stubd` run that reuses an already-loaded STUB never shows it. Run `dect-stubd` straight after `fwload.sh` and read the first mails. `[observed]` 2026-10-17 at gate G2, and `dect-stubd` now decodes the stamp as text. The two builds stamp **`Jan 29 2026`** (08.25) and **`Mar 31 2022`** (07.56) `[CR, the hardware-analysis step, 2026-10-21]`, so label chip-side findings with the stamp rather than the FRITZ!OS version — nearly four years separate them.

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
