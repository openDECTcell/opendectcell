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

- **One package for all models:** `opendectcell` contains `dect-fwload`, `dect-stubd`, `dect-probe`, `libopendectcell`, the init script and all board profiles.
  - It depends on the right kernel module for the target (`+TARGET_lantiq:kmod-opendectcell-lantiq`, …), so installing `opendectcell` is enough.
  - The programs contain **no per-model code**. Everything model-specific is in the profile.
- **Board profiles** are read-only files at `/usr/share/opendectcell/boards/<board_name>`, e.g. `/usr/share/opendectcell/boards/avm,fritz7412`. In the repo they live in `boards/`.
  - Format: plain `key=value` lines with `#` comments, readable by people and sourceable by the shell init script.
  - They are replaced by every package upgrade, so fixes reach users automatically.
- **User settings** go into `/etc/config/opendectcell` (UCI). It holds only what a user may want to change (enabled, RFPI override, debug level) and, for experts, deliberate overrides of profile keys. UCI files survive upgrades, and the profile underneath stays current.
- **Start-up:** the procd init script reads `board_name`, loads the matching profile, applies UCI overrides and passes the result to the daemons as command-line options. The daemons don't parse files, which keeps them simple to test on the PC.
- **Unsupported box:** the init script prints one clear message ("This box (…) is not supported by openDECTcell") and starts nothing.

### Profile keys (initial set)

| Key | Meaning | 7412 value |
|---|---|---|
| `name` | Display name | `FRITZ!Box 7412` |
| `transport` | Transport the kernel module must provide (consistency check) | `lantiq-ssc` |
| `fw_family` | STUB family; selects address translation (0 for `441`, 0xF0000 for `488`) | `441` |
| `fw_firstlevel` | First-level loader file name | `dectfw_firstlevel_441.hex` |
| `fw_secondlevel` | STUB file name | `dectfw_secondlevel_441.hex` |
| `fw_firstlevel_sha256`, `fw_secondlevel_sha256` | Builds our docs are verified against (reference by hash only); checked before loading | `72f8e27a…045f2`, `4fabee65…cfaa6` |
| `dect_mode` | Mode word: low byte goes into TLV tag 7, bit 8 is the first-level boot flag | `0x119` |
| `uart` | Firmware-load UART device | `/dev/ttyLTQ0` (*assumed*) |
| `uart_is_console` | The UART is also the serial console (console must be released) | `1` |
| `uart_baud_rom`, `uart_baud_stub` | Baud rates for the ROM and the first-level stage | *open* |
| `pcm_words_per_frame`, `pcm_ctrl_slot`, `pcm_voice_slots` | Frame layout | *open* (M3, gate G2) |
| `factory_cfg` | Where the factory DECT config block is stored | *open* (M1) |
| `rfpi_fallback` | Source for the RFPI if the factory block holds the placeholder | *open* (M1) |
| `max_calls` | Parallel calls the STUB supports in this mode | *open* (about 3) |

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
- Can the 7362 SL also use SPI-TDM over the SSC, like the 7412? `PCM-link` on the 7362 SL contains both links, but its GPIO table lists only the TDM pins.
- Device-tree binding name and properties for the later upstream patch.
