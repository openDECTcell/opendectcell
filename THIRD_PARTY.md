# Third-party components

openDECTcell contains no source code copied from other projects. It builds with, links or loads the components below; each
keeps its own licence, which is the one to read. (How the repository's own files are licensed: [docs/licensing.md](docs/licensing.md)
and [LICENSES/](LICENSES/).) This list is kept by hand and is not a substitute for the licence files of the packages.

| Component | Used for | Licence (upstream) |
|---|---|---|
| [OpenWrt](https://openwrt.org) 25.12 and its SDK/ImageBuilder | the platform; the release packages and images are built with it | GPL-2.0 (the base system; packages carry their own) |
| [baresip](https://github.com/baresip/baresip) and libre | the SIP client of `opendectcell-baresip`; openDECTcell's module is loaded into it | BSD-3-Clause |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) 3.6 | the TLS link of `dect-cell` to the controller | Apache-2.0 OR GPL-2.0-or-later (used here under GPL-2.0-or-later) |
| [ucode](https://github.com/jow-/ucode) and its modules | `opendectcell-carddav`, the LuCI back end | ISC |
| [uclient](https://git.openwrt.org/project/uclient.git) | the CardDAV sync uses a patched copy (`ucode-mod-opendectcell-uclient`; the two patches are in `openwrt/utils/`) | ISC |
| [LuCI](https://github.com/openwrt/luci) | the web interface framework `luci-app-opendectcell` builds on | Apache-2.0 |
| `ca-bundle` (Mozilla CA list) | CardDAV trusts the system CAs | MPL-2.0 (data) |

**Not part of this repository and not shipped:** the DECT chip firmware and the per-device calibration data (each user takes
them from their own box's official image, [docs/firmware-sourcing.md](docs/firmware-sourcing.md)), and the ETSI and ITU-T
standards the code is written from (freely downloadable, not committed; [docs/specs/README.md](docs/specs/README.md)).

The G.722 codec in `src/libopendectcell/dect_g722.c` is the project's own implementation of ITU-T G.722, compared with ffmpeg
only as a black box. The DECT authentication algorithm (DSAA) is implemented from public descriptions of it; the standard
DECT cipher runs in the chip's hardware.

Credits to the projects whose public work was read or used as a tool are in the "Credits" section of [README.md](README.md).
