# openDECTcell: open DECT base station for AVM FRITZ!Box under OpenWrt

**Goal:** turn cheap, used AVM FRITZ!Boxes into **open-source DECT base stations** running OpenWrt, so that GAP/CAT-iq handsets can make calls via SIP. In a later stage, several boxes should form a **multi-cell DECT-over-IP network** with one central controller.

> Status: **research / reverse-engineering phase.** Nothing works on hardware yet.

## Why this is possible

- **The DECT chip is ordinary.** FRITZ!Boxes use a Dialog SC144xx DECT chip. At boot the router uploads AVM's chip firmware (the "STUB") over a UART. The STUB handles the radio, the DECT MAC and speech-codec conversion.
- **The rest runs on the router.** Everything above the MAC (DLC, NWK, registration, authentication, call control) runs on the router CPU in AVM's closed kernel module `avm_dect.ko`. It talks to the chip through checksummed **mails carried in the PCM/TDM bus**.
- **That split is where an open stack fits.** If we reimplement the host side, the unchanged chip firmware can be driven by open code. That covers the firmware loader, the PCM driver, the control channel and a DECT stack following ETSI EN 300 175.
- **The same split suits multi-cell.** Each box acts as a radio unit that tunnels mails and RTP to a central controller.

## Target hardware

| Box | SoC | DECT PCM bus | Status |
|---|---|---|---|
| **FRITZ!Box 7520 / 7530** (first target) | Qualcomm IPQ4019 (4× ARM Cortex-A7), 256 MB RAM / 128 MB NAND, DECT chip SC14448 | IPQ40xx ADSS TDM | Analysed, first observations on a running box ([docs/models/7520-DECT-analysis.md](docs/models/7520-DECT-analysis.md)) |
| FRITZ!Box 7412 | Lantiq VRX220 (MIPS 34Kc, 2 VPEs), 128 MB RAM / 128 MB NAND | VR9 **SSC in SPI-slave "SPI-TDM" mode** | Analysed ([docs/models/7412-DECT-analysis.md](docs/models/7412-DECT-analysis.md)) |
| FRITZ!Box 7362 SL | Lantiq VR9 | VR9 TDM | Analysed ([docs/models/7362SL-DECT-analysis.md](docs/models/7362SL-DECT-analysis.md)) |

All three boxes run almost the same host DECT stack. For example, 98 % of the `avm_dect` functions are shared between the 7412 and the 7362 SL. Findings therefore transfer between models.

## Principles

- **Clean room.** The docs describe interfaces and behaviour in our own words: pins, register values, frame formats, primitive names. **No AVM code, no analysed code and no AVM device-tree source** is ever committed. New code is written from the docs.
- **No proprietary blobs in this repo or in images.** AVM's STUB firmware (`dectfw_*.hex`) and the per-device DECT calibration data are extracted **at runtime** from an AVM image or from the box itself. The docs reference them by SHA-256 only.
- **Legal basis.** EU Directive 2009/24/EC Art. 5(3) (observation) and Art. 6 (analysis for interoperability), implemented in Germany as §69d/§69e UrhG.

## Repository layout

| Path | Content |
|---|---|
| `docs/models/` | Per-model DECT analyses (image format, components, GPIO map, boot protocol) |
| `docs/stub-pcm-control-channel.md` | Host↔STUB mail framing on the PCM bus |
| `docs/7412-spi-tdm-and-loader.md` | 7412 SSC/SPI-TDM setup, STUB patching (TLV, mode byte), control channel |
| `docs/hardware-abstraction.md` | How several models are supported: kernel board table, per-model board profiles, packaging |
| `docs/firmware-sourcing.md` | How users get the STUB: extract on the PC from an AVM image, upload to the box |
| `tools/avm_nand_sqfs.py` | Reader for AVM's Lantiq NAND `filesystem.image` (BE SquashFS, sector-index headers, gaps, circular layout) |
| `tools/sqfs_be.py` | Minimal SquashFS 4 reader (BE/LE, xz) |
| `tools/elfrel.py` | Decodes pointer/name tables in relocatable ELF modules |
| `the analysis tooling headless analysis script` | Headless the analysis tooling helper (local analysis only; its output is never committed) |
| `ROADMAP.md` | Phases and milestones |
| `work/` | **git-ignored**: AVM binaries, the analysis tooling projects, analyser output |

## Tools quick start

```sh
# list / extract files from an AVM Lantiq-NAND firmware image (7362 SL, 7412)
python3 tools/avm_nand_sqfs.py FRITZ.Box_7412.137.06.88.image ls
python3 tools/avm_nand_sqfs.py FRITZ.Box_7412.137.06.88.image extract work/7412 /lib/modules/dectfw_secondlevel_441.hex
```

## License

GPL-2.0 (see [LICENSE](LICENSE)): kernel code under `kernel/` is GPL-2.0-only; all other code (userspace, `libopendectcell`, tools) is **GPL-2.0-or-later**, so it can be combined with the AGPL-3.0 [opendectcell-controller](https://github.com/openDECTcell/opendectcell-controller).
