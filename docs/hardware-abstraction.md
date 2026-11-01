# Hardware abstraction: supporting several FRITZ!Box models

Status: **decided** 2026-10-17. Details marked *open* are settled during M2, M3 and M9.

## 1. Goal

openDECTcell must run on several FRITZ!Box models without per-model builds, and stay easy to install and upgrade for users who don't know the hardware details.

**What the user sees:**
- They install one package, `opendectcell`.
- They upload the DECT firmware once, extracted on their PC from an official AVM image (see `firmware-sourcing.md`).
- Nothing else. They never choose a model, a driver or a pin number.

## 2. How the models differ

The differences fall into three kinds. Values are from the model analyses in `docs/models/` (*derived statically*).

| Kind | 7412 | 7362 SL | 7520 / 7530 | Handled by |
|---|---|---|---|---|
| **Pins** (DECT reset, PCM clock and frame sync) | reset GPIO33, FSC GPIO9, DCL enable GPIO7 | reset GPIO30, FSC GPIO0, DCL GPIO40 | reset GPIO40, PCM GPIO20–27 | kernel board table (§3) |
| **PCM interface** (different hardware block) | VR9 SSC as SPI slave | VR9 TDM block | IPQ4019 ADSS TDM | kernel transport driver (§3) |
| **Firmware-load UART** | console UART | own USIF UART | console UART | board profile (§4) |
| **STUB and chip settings** | mode 0x119 | mode 0x101 | mode 0x105, `_slow` first level | board profile (§4) |
| **Frame layout, factory data** | words per frame, control-slot position, voice-slot map, factory config location, RFPI fallback (mostly *open*) | ← | ← | board profile (§4) |

Two facts drive the design:
- **The same GPIO number means different things on different boxes.** GPIO33 is DECT reset on the 7412 but the red power LED on the 7362 SL. GPIO11 is PCIe reset on the 7412 but the DECT UART RX on the 7362 SL. Pin numbers must therefore never be user-editable data.
- **Several models share one OpenWrt target.** The 7412 and 7362 SL are both `lantiq/xrx200`, so one build produces packages for both. The model has to be detected at runtime. OpenWrt provides that: `/tmp/sysinfo/board_name` holds the device-tree name, e.g. `avm,fritz7412`.

## 3. Kernel side: pins and PCM interface

- **One kernel module package per OpenWrt target**, containing every transport driver that target needs:

  | Package | Target | Transport drivers |
  |---|---|---|
  | `kmod-opendectcell-lantiq` | `lantiq/xrx200` | VR9 SSC slave (7412), VR9 TDM (7362 SL, later) |
  | `kmod-opendectcell-ipq40xx` | `ipq40xx` | ADSS TDM (7520/7530, later) |

- **Board table.** The module matches the board by its device-tree `compatible` and takes pins and the transport type from a small built-in table. Later this moves into a proper device-tree node, upstreamed to OpenWrt; userspace does not change when that happens.
- **Unknown board:** the module refuses to load and logs which board name it saw. It never touches a GPIO it doesn't know.
- **Uniform interface for every transport:** a character device `/dev/dect0` that offers
  - **chip reset** (needed by the firmware loader, so the loader doesn't need GPIO numbers either)
  - **control-slot** byte stream (mails)
  - **voice-slot** data
  - **configuration** of the frame layout, set by `dect-stubd` from the board profile before streaming starts
  - **information**: transport type, frame counters, error counters

  The exact interface is specified in M3 (*open*).
- **Userspace never sees GPIO numbers or register addresses.**

## 4. Userspace side: one package, one profile per model

- **One package for all models:** `opendectcell` contains `dect-fwload`, `dect-stubd` (with `libopendectcell` built in), the init script, all board profiles and the handset profiles.
  - It depends on the right kernel modules for the target (`+TARGET_lantiq_xrx200:kmod-opendectcell-lantiq`, `+TARGET_lantiq_xrx200:kmod-lantiq-ssc-pcm`, `+TARGET_ipq40xx:kmod-opendectcell-ipq40xx`), so installing `opendectcell` is enough.
  - The programs contain **no per-model code**. Everything model-specific is in the profile.
- **Board profiles** are read-only files at `/usr/share/opendectcell/boards/<board_name>`, e.g. `/usr/share/opendectcell/boards/avm,fritz7412`. In the repo they live in `boards/`.
  - Format: plain `key=value` lines with `#` comments, readable by people and sourceable by the shell init script.
  - They are replaced by every package upgrade, so fixes reach users automatically.
- **User settings** go into `/etc/config/opendectcell` (UCI). It holds only what a user may want to change (enabled, RFPI override, debug level) and, for experts, deliberate overrides of profile keys. UCI files survive upgrades, and the profile underneath stays current.
- **Start-up:** the procd init script reads `board_name`, loads the matching profile, applies UCI overrides and passes the result to the daemons as command-line options. The daemons don't parse files, which keeps them simple to test on the PC.
- **Box without a profile:** the init script logs "no board profile for '…'; using the 7520 defaults" and goes on with its built-in 7520/7530 values; UCI can override each of them.

### Profile keys

What the init script reads today: `fw_firstlevel`, `fw_secondlevel`,
`dect_mode`, `uart`, `uart_kmod`, `strap`, `factory_mtd`, `factory_offset`,
`led_path`, `pcm_kmod`, `pcm_dev` and `pcm_slots`. The other keys below are recorded
for the model but **read by nothing yet**; some are in no profile file yet.

| Key | Meaning | 7412 value |
|---|---|---|
| `name` | Display name | `FRITZ!Box 7412` |
| `transport` | Transport the kernel module must provide (consistency check) | `lantiq-ssc` |
| `fw_family` | STUB family; selects address translation (0 for `441`, 0xF0000 for `488`) | `441` |
| `fw_firstlevel` | First-level loader file name | `dectfw_firstlevel_441-06.88-7412.hex` |
| `fw_secondlevel` | STUB file name; `dect-stubd` picks chip RAM addresses by it, so a build gets a name of its own | `dectfw_secondlevel_441-06.88-7412.hex` |
| `fw_firstlevel_sha256`, `fw_secondlevel_sha256` | Builds our docs are verified against (reference by hash only). Not read: the init script checks both files against its own list of known builds (`KNOWN_FW`, the same table as `tools/dect-fw-extract.py`) | `72f8e27a…045f2`, `4fabee65…cfaa6` |
| `dect_mode` | Mode word: low byte goes into TLV tag 7, bit 8 is the first-level boot flag | `0x119` |
| `uart` | Firmware-load UART device; empty or missing: the service does not start | `/dev/ttyLTQ0` (*observed* 2026-10-29, [bench-7412.md](bench-7412.md)) |
| `uart_is_console` | The UART is also the serial console (console must be released). Not read: the init script and the uci-defaults script release whatever getty runs on `uart` | `1` |
| `uart_baud_rom`, `uart_baud_stub` | Baud rates for the ROM and the first-level stage | *open* |
| `pcm_kmod` | Transport module loaded before the chip boots, where another module than openDECTcell's carries the bus (it holds the PCM clock enable); empty: none | `lantiq_ssc_pcm` |
| `pcm_dev` | PCM device `dect-stubd` opens (`-d`); empty: `/dev/dect0`; `none`: the board has no PCM transport yet and the service does not start (7362 SL) | `/dev/ssc-pcm0` |
| `uart_kmod` | Module the service loads before checking `uart`, where the firmware-load UART is not the console's; empty: none. The 7362 SL: `lantiq_usif_uart` (`kmod-lantiq-usif-uart`, `/dev/ttyUSIF0`) | (empty) |
| `pcm_slots` | 16-bit slots per frame (`-c`); empty: the module's default | `36` (mode 0x119, *observed* 2026-10-28) |
| `pcm_words_per_frame`, `pcm_ctrl_slot`, `pcm_voice_slots` | Frame layout | 36 lanes (`pcm_slots`); control lane found by probing, every run elsewhere (no frame sync); speech of call *n* in control lane + *n*, same lane both ways [CR A-0015], set by the daemon (2026-10-29, `d1d2c8f`) |
| `strap` | `0`: the uploader leaves the chip's boot strap alone (`dect-fwload --no-strap`) | `0` |
| `led_path` | The DECT LED's brightness file, driven by `dect-stubd` | `/sys/class/leds/green:dect/brightness` |
| `reset_chip`, `reset_line` | The chip's reset line: GPIO chip label and line, as the openDECTcell module claims it (low = reset). The service's stop holds it through `/sys/class/gpio` when `dect-stubd` left no park record; defaults `1000000.pinctrl`, `40` (7520) | `gpio-xway`, `33` |
| `factory_mtd`, `factory_offset` | The factory DECT config block (RFPI and calibration) | `/dev/mtd0`, `0x1e800` |
| `factory_cfg` | Where the factory DECT config block is stored; superseded by `factory_mtd`/`factory_offset`, empty | (empty) |
| `rfpi_fallback` | Source for the RFPI if the factory block holds the placeholder | `maca` |
| `max_calls` | Parallel calls the STUB supports in this mode; `dect-stubd` allows three (its `twocalls` option) whatever this says | `3` |

New keys are added when a model needs them, with a default that keeps existing profiles valid.

## 5. Adding a model

1. Write the model analysis in `docs/models/`.
2. Add a board-table entry to the kernel module. If the model uses a PCM interface we don't support yet, write a transport driver for it first (this is the only case that needs new driver code).
3. Add `boards/<board_name>`.
4. Run the hardware gates G1–G7 on that model.

## 6. Alternatives considered

| Alternative | Why not |
|---|---|
| Config files with GPIO numbers, read by userspace | A wrong value drives the wrong pin (see §2). The GPIO sysfs interface is deprecated. Users would see hardware details they shouldn't have to care about. |
| Per-model packages or builds | Users would have to pick the right one, and every model would need its own build. No size benefit: a profile is about 1 KB, and each transport driver a few tens of KB. |
| Writing the profile into `/etc/config/opendectcell` on first boot (like OpenWrt's `board.d`) | Values written once go stale: profile fixes in later package versions would not reach existing installs. |
| One kernel module package per transport driver | More packages for users to tell apart. One per target is selected automatically by the dependency. |

## 7. Open questions

- Is the frame layout fixed per transport, or does it depend on the STUB mode word? (Decides whether it stays in the profile or moves into the board table.)
- ~~Can the 7362 SL also use SPI-TDM over the SSC, like the 7412?~~ **No** (2026-11-01, `models/7362SL-DECT-analysis.md` §7): its PCM pins (GPIO0/25/40/41) have no SSC function, and the SSC drives the SPI NOR flash with the urlader and tffs. The 7362 SL needs a VR9 TDM transport and a USIF UART driver, neither of which OpenWrt has.
- The factory block's partition and offset differ per model (7412: urlader `mtd0`, 0x1E800; 7362 SL: urlader `mtd2`, 0x1985, one box). Profiles name the device and offset; finding the partition by name (`urlader` in `/proc/mtd`), and perhaps the block by its header, would be sturdier.
- Device-tree binding name and properties for the later upstream patch.
