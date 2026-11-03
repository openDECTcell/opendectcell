# src/fwload: dect-fwload, the chip firmware loader

The DECT chip keeps only a small boot loader in ROM; its RAM is empty after
every power-off or reset. `dect-fwload` boots it: it resets the chip, uploads
a small first-level loader and then the second-level firmware (the "STUB")
over the chip's UART, patches the STUB image with the box's own identity and
settings on the way, and then listens for a few seconds to the STUB's first
bytes. The chip reset and the boot-mode strap go through the kernel module
(`/dev/dect0`, ioctls `DECT_IOC_RESET` and `DECT_IOC_STRAP`); the UART is an
ordinary tty. No GPIO numbers or register addresses are handled here.

**The chip firmware is not in this repository and never ships with openDECTcell.**
Every user extracts the two `.hex` files from an official FRITZ!OS image for
their own box. See [../../docs/firmware-sourcing.md](../../docs/firmware-sourcing.md)
for why, which builds are known (identified by SHA-256) and how
`tools/dect-fw-extract.py` does it, and [../../docs/install.md](../../docs/install.md)
for the steps. The files go to `/etc/opendectcell/firmware/` on the box; the init
script refuses a build it does not know unless the UCI option
`fw_allow_unverified` is set.

## Files

| File | What it is |
|---|---|
| `dect-fwload.c` | The program: command line, UART and `/dev/dect0` callbacks, factory block read, `main()` |
| `loader.c`, `loader.h` | The upload engine (`fwl_upload()`, `fwl_stage()`): handshake, retries, baud rates, reset order. All I/O through `struct fwl_io` callbacks, so it runs on the box and in tests. Specification: [../../docs/stub-loader-protocol.md](../../docs/stub-loader-protocol.md) |
| `fwimage.c`, `fwimage.h` | Firmware images: Intel HEX to upload image, firmware family from the file name, patch-area lookup, TLV patching, RFPI from the MAC address, checksum. Pure functions on buffers. Specification: [../../docs/stub-image-format.md](../../docs/stub-image-format.md) |

## What a run does

`main()` in `dect-fwload.c`, in order:

1. Reads the two hex files (`slurp()`), picks the firmware family from the
   second-level file name (`fwi_family_from_name()`), and builds the images
   (`fwi_build_firstlevel()`, `fwi_build_secondlevel()`).
2. Reads the factory DECT block from a flash partition when `--factory-mtd`
   and `--factory-offset` are given (`read_factory()`: a four-byte header,
   then a list of tag-length-value entries holding RFPI and calibration).
3. Patches the STUB image (`fwi_patch()`): replaces matching entries in its
   patch area with the factory block, fills the RFPI placeholder from the MAC
   address if needed, sets the mode byte (`--mode`), or sets the RFPI
   to `--rfpi` if given, and updates the checksum.
4. Opens the UART and sets it raw at 9600 baud (before `/dev/dect0`, so the tty
   does not echo to the chip), then opens `/dev/dect0` or takes an inherited
   descriptor (`--dect-fd`).
5. `fwl_upload()`: reset the chip (`chip_reset()`; in the "slow" boot mode the
   strap is held low around the reset release, in "fast" mode it is not, the
   mode word chooses which comes first), stage 1 (first-level image, 16-bit
   length header, up to 16 attempts), a pause, then stage 2 at the baud rate
   named by the first-level file name (`_slow` 115200, `_fast` 460800, else
   230400; STUB image, 32-bit length), with up to three full rounds. The
   attempt limits and timings are in `loader.h`. `--no-strap` replaces the
   strap step by a no-op on boards without a strap line.
6. `listen()`: prints what the STUB sends for `--listen` seconds (default 5).

On failure the reset and the strap are released again, so the chip is not left
held. SIGINT, SIGTERM and SIGPIPE release the reset before exit.

## Command line

```
dect-fwload --uart DEV {--dect-dev /dev/dect0 | --dect-fd N}
            --first FILE.hex --second FILE.hex
            [--factory-mtd DEV --factory-offset N] [--mode 0x105]
            [--listen SECONDS] [--no-strap] [--rfpi HEX10] [-v]
dect-fwload --probe DEV [BAUD]       open the port and report what arrives
dect-fwload --version
```

The init script's `load_chip()` (`openwrt/net/opendectcell/files/opendectcell.init`)
runs it with these values from the board profile in `boards/<board name>`
(keys `uart`, `fw_firstlevel`, `fw_secondlevel`, `dect_mode`, `factory_mtd`,
`factory_offset`, `strap`; see [../../docs/hardware-abstraction.md](../../docs/hardware-abstraction.md))
and only when the chip has not been booted since the last module load
(`/var/run/opendectcell/stub.second` marks a booted chip and names the STUB build
`dect-stubd` has to expect). `--rfpi` is used for the bench option `rfpi_test`
and for the identity a controller sets on a cell. `--dect-fd` lets another
program (`dect-pcmcap -e`, see [../tools/](../tools/README.md)) hand over its
open device.

## Tests

| Test | What it covers |
|---|---|
| `tests/test_fwimage.c` | `fwimage.c` against the format document, with synthetic HEX always. The real firmware files are used only if `OPENDECTCELL_STUB_HEX` and `OPENDECTCELL_FL_HEX` point to them; otherwise those cases are skipped |
| `tests/test_fw_extract.py` | `tools/dect-fw-extract.py` against synthetic images |
| `tests/test_firmware_sh.py` | `openwrt/net/opendectcell/files/firmware.sh`, the box's side of LuCI's firmware upload, with a fake board profile and synthetic bytes |

`loader.c` has no test of its own in `tests/`; it is exercised on hardware.
`make check` runs the others; `make cross` builds `dect-fwload` for the 7520
(ARM).

## Where to look for...

| You want | Look at |
|---|---|
| The handshake, retries, baud rates | `loader.c` (`stage1()`, `stage2()`, `fwl_stage()`); [../../docs/stub-loader-protocol.md](../../docs/stub-loader-protocol.md) |
| The image layouts and what is patched | `fwimage.c` (`fwi_build_*()`, `fwi_patch()`, `fwi_tlv_replace()`); [../../docs/stub-image-format.md](../../docs/stub-image-format.md) |
| The family table (first-level load address, patch area) | `families[]` in `fwimage.c` |
| Chip reset and boot strap | `dev_reset()`, `dev_strap()`, `dev_strap_none()`; the kernel side in `../../kernel/` and [../../docs/kmod-spec.md](../../docs/kmod-spec.md) |
| Boards whose UART or strap differ | `boards/<board name>`, [../../docs/hardware-abstraction.md](../../docs/hardware-abstraction.md), [../../docs/7412-spi-tdm-and-loader.md](../../docs/7412-spi-tdm-and-loader.md) |
| Which firmware files to use | [../../docs/firmware-sourcing.md](../../docs/firmware-sourcing.md), `tools/dect-fw-extract.py` |
| How the service starts it | `load_chip()`, `warm_chip()`, `park_guard()` in the init script |
