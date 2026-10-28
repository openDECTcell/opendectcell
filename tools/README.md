# Tools

Programs that run on a PC, not on the box. Python tools need only Python 3's
standard library; run them with `python3 -I`.

## For users

| Tool | What it does |
|---|---|
| [dect-fw-extract.py](dect-fw-extract.py) | Takes the DECT chip firmware out of an official FRITZ!OS image, checks it against the builds openDECTcell knows, and prints the commands that copy it to the box. See [docs/install.md](../docs/install.md), step 3 |
| [sqfs_be.py](sqfs_be.py) | SquashFS reader used by `dect-fw-extract.py` |

## For releases

| Tool | What it does |
|---|---|
| [release/build.sh](release/build.sh) | Builds the OpenWrt packages for one target with the official SDK, as the GitHub workflow does |
| [release/notes.md](release/notes.md) | The standard text of a release; a release can have its own `notes-<version>.md` |
| [github/seed_issues.py](github/seed_issues.py) | Creates GitHub milestones and issues from `ROADMAP.md` |

## For development and analysis

These helped bring up the chip and the calls. They read captures and logs
that `dect-stubd` writes during experiments (`-w FILE`, `-a memdump`, …).

| Tool | What it does |
|---|---|
| [voice-decode.py](voice-decode.py) | Works out how a captured voice slot is encoded |
| [dect_g722_extract.py](dect_g722_extract.py) | Extracts a call's G.722 stream from a capture |
| [g722-crosscheck.sh](g722-crosscheck.sh) | Checks our G.722 codec against ffmpeg's |
| [make-lane-g722.py](make-lane-g722.py), [make-lane-probe.py](make-lane-probe.py), [make-lane-matrix.py](make-lane-matrix.py) | Build test audio for `dect-stubd -a playraw=` |
| [decode-beacon.py](decode-beacon.py), [dect_qh7.py](dect_qh7.py) | Decode the base's broadcast capabilities from a memory dump |
| [dect_memdump.py](dect_memdump.py), [dect_memcmp.py](dect_memcmp.py) | Reassemble and compare chip memory snapshots |
| [gpio-activity.c](gpio-activity.c) | Checks whether a fast signal (such as the PCM clock) is toggling |
| [avm_nand_sqfs.py](avm_nand_sqfs.py) | Reads the NAND filesystem image of older FRITZ!Boxes (7362 SL, 7412) |
| [elfrel.py](elfrel.py) | Lists pointer tables in relocatable ELF files |

On-box development tools are in [src/tools/](../src/tools/): `dect-pcmcap`
(PCM bring-up and capture), `dect-maildump` (mails from a capture) and
`dect-sla` (signal capture).
