# boards/: board profiles

A board profile holds everything that differs per FRITZ!Box model and is not a
pin or a register: which chip firmware files the model needs, the mode word,
which UART loads the firmware, where the factory DECT data sits, which PCM
device the daemon opens, and so on. The programs (`dect-fwload`, `dect-stubd`)
contain no per-model code; the init script reads the profile and hands the
values to them as command-line options. Pins and register addresses are in the
kernel's board table instead ([../kernel/README.md](../kernel/README.md)).
The design and its reasons are in
[../docs/hardware-abstraction.md](../docs/hardware-abstraction.md).

Do not confuse these with the **handset** profiles in
[../profiles/](../profiles/README.md), which describe handset models.

## Files

| File | Box | OpenWrt target |
|---|---|---|
| [avm,fritzbox-7530](avm,fritzbox-7530) | FRITZ!Box 7520 and 7530 (IPQ4019) | `ipq40xx` |
| [avm,fritz7412](avm,fritz7412) | FRITZ!Box 7412 (Lantiq VR9, SSC) | `lantiq/xrx200` |
| [avm,fritz7362sl](avm,fritz7362sl) | FRITZ!Box 7362 SL (Lantiq VR9, TDM, USIF UART) | `lantiq/xrx200` |

**Naming rule:** the file name is the box's OpenWrt `board_name`, the content
of `/tmp/sysinfo/board_name` on a running box (the 7520 reports the 7530's
name). The init script reads that file at start and sources
`/usr/share/opendectcell/boards/<board_name>`. No file for the name: it logs
`no board profile for '...'; using the 7520 defaults` and carries on with the
built-in 7520/7530 values.

## Format and installation

Plain `key=value` lines with `#` comments, sourced by `/bin/sh`: no spaces
around `=`, quote values that contain spaces (`name="FRITZ!Box 7412"`). The
file is read by shell scripts (init script, `firmware.sh`, the uci-defaults
script) and, for six keys, by the LuCI settings page, which parses it with a
regular expression.

`openwrt/net/opendectcell/Makefile` installs the profile of the target being built
(`OPENDECTCELL_BOARDS`) into the `opendectcell` package, so a 7520 does not carry the
7412's. The 7362 SL's profile ships in its own package, `opendectcell-fritz7362sl`,
because that model shares a target with the 7412. Profiles are replaced by
every package upgrade. A user's overrides live in `/etc/config/opendectcell` (UCI)
and win over the profile.

## Keys

"Read by" lists what consumes the key today (checked against the code). The
init script is [../openwrt/net/opendectcell/files/opendectcell.init](../openwrt/net/opendectcell/files/opendectcell.init).
Defaults are the init script's built-in 7520/7530 values where it has one.

| Key | Meaning | In profile | Read by |
|---|---|---|---|
| `name` | Display name | all | nothing yet |
| `transport` | Transport kind (`ipq40xx-tdm`, `lantiq-ssc`, `lantiq-tdm`) | all | nothing yet |
| `fw_family` | STUB family (`441`) | 7412, 7362 SL | nothing yet |
| `fw_firstlevel` | File name of the first-level loader in the firmware folder | all | init script (upload; UCI `first`), `firmware.sh`, LuCI (placeholder) |
| `fw_secondlevel` | File name of the STUB. `dect-stubd` picks chip RAM addresses by this name, so a different build needs a name of its own | all | init script (UCI `second`; recorded in `/var/run/opendectcell/stub.second`), `firmware.sh`, LuCI |
| `fw_firstlevel_sha256`, `fw_secondlevel_sha256` | Checksums of the builds the board was verified with | 7412, 7362 SL | `firmware.sh` (checks an upload, LuCI's firmware table). The init script checks files against its own `KNOWN_FW` list, not these |
| `dect_mode` | Chip mode word: low byte goes into TLV tag 7, bit 8 is the first-level boot flag. Default `0x105` | all | init script (`dect-fwload --mode`; UCI `mode`), LuCI |
| `uart` | Firmware-load UART device. Default `/dev/ttyMSM0`; if the device does not exist the service does not start | all | init script (UCI `uart`), uci-defaults (disables a login getty on it), LuCI |
| `uart_is_console` | The UART is also the serial console | all | nothing: the init and uci-defaults scripts release any getty on `uart` anyway |
| `uart_kmod` | Module loaded before `uart` is checked, where the UART is not the console's (7362 SL: `lantiq_usif_uart`) | 7362 SL | init script |
| `pcm_kmod` | Transport module loaded before the chip boots, where another module than `opendectcell` carries the bus | 7412, 7362 SL | init script |
| `pcm_dev` | PCM device `dect-stubd` opens (`-d`). Empty: `/dev/dect0`. `none`: no transport yet, the service does not start | 7412, 7362 SL | init script |
| `pcm_slots` | 16-bit slots per frame (`-c`). Empty: the driver's default | 7412, 7362 SL | init script |
| `quiet_console` | `1`: kernel console level set to 1 before the base starts, for transports that lose frames to console lines | 7362 SL | init script |
| `strap` | `0`: the uploader leaves the boot strap alone (`dect-fwload --no-strap`). Default `1` (UCI option `strap` overrides) | all | init script |
| `led_path` | Brightness file of the DECT LED, driven by `dect-stubd` (`led=` option). Default the 7520's `green:fon` | all | init script |
| `reset_chip`, `reset_line` | GPIO chip label and line of the chip's reset, as the kernel module claims it; the service uses them to hold the chip in reset on stop when no park record exists. Defaults `1000000.pinctrl`, `40` | 7412, 7362 SL | init script |
| `factory_mtd`, `factory_offset` | Where the factory DECT block (RFPI, calibration) sits: device and offset. Defaults `/dev/mtd5`, `0x3d000` | all | init script (`dect-fwload --factory-mtd/--factory-offset`; UCI overrides), LuCI |
| `factory_cfg` | Older way of naming the factory block; empty, superseded by the two above | 7412, 7362 SL | nothing |
| `rfpi_fallback` | Source of the RFPI if the factory block holds the placeholder | 7412, 7362 SL | nothing yet |
| `max_calls` | Parallel calls the STUB supports in this mode | all | nothing: `dect-stubd` decides itself |

The 7520/7530 profile is the shortest: keys it leaves out (`reset_*`, `pcm_*`,
`uart_kmod`, `quiet_console`) come from the init script's defaults, which are
that board's values.
[../docs/hardware-abstraction.md](../docs/hardware-abstraction.md) section 4
lists further keys recorded for models but read by nothing; its "not read"
remarks on the checksum keys are out of date (see above).

## Adding a new box

Follow [../AGENTS.md](../AGENTS.md) section 1b: work on one model must not
change the others, and nothing in userspace may be keyed on a model name.

1. Write the model analysis in `docs/models/` (see
   [7412](../docs/models/7412-DECT-analysis.md) for the shape).
2. Find the box's `board_name` (`cat /tmp/sysinfo/board_name`) and add a
   board-table entry in the kernel module for its target (`compatible`,
   reset and strap GPIOs, transport). Compare the device-tree `compatible`
   with `board_name` on the real box. If the box uses a PCM block no driver
   covers, a transport driver comes first
   ([../kernel/README.md](../kernel/README.md)).
3. Create `boards/<board_name>`. Copy the closest existing profile and set the
   firmware files, `dect_mode`, UART, factory block, PCM device, LED and reset
   line. Prefer a key over a code branch: if the init script needs new
   behaviour, key it on a setting of this board's own, not on one other boards
   share (a switch keyed on `pcm_kmod` would have run on the 7412).
4. Packaging, in [../openwrt/net/opendectcell/Makefile](../openwrt/net/opendectcell/Makefile):
   add the file to `OPENDECTCELL_BOARDS` for its target. If the box shares a target
   with another model, give it a per-board meta package like
   `opendectcell-fritz7362sl` with its own kernel-module dependencies; never add
   them to `opendectcell`'s `DEPENDS`.
5. A new chip firmware build: add its SHA-256 to `KNOWN_FW` in the init script
   and in `firmware.sh`, and to `tools/dect-fw-extract.py`
   ([../docs/firmware-sourcing.md](../docs/firmware-sourcing.md)).
6. Do not change `libopendectcell` for a model; a transport difference belongs in
   the kernel module or `dect-stubd`.
7. Run the hardware gates G1-G7 on the box (they are in `ROADMAP.md`), and
   record changes to shared code in the maintainers' notes, with how to bench them on
   the other boxes.
8. Run `make check`; `tests/test_firmware_sh.py` exercises `firmware.sh`
   against a fake profile.
