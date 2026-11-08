# Licences and provenance

**This page describes the repository's declared licences. It is not legal advice.** It was assembled from the SPDX
headers of the source files, the `PKG_LICENSE` lines of the package Makefiles, the `LICENSE` file and the project's own
rules. The maintainers have not had a legal review of it. Where the repository does not state something, the page says
"not established here" rather than guess.

## 1. What the repository declares

`LICENSE` is the text of the GNU General Public License, version 2. `README.md` ("License") says: GPL-2.0, the kernel
driver under `kernel/` is GPL-2.0-only, everything else is GPL-2.0-or-later.
[CONTRIBUTING.md](../CONTRIBUTING.md) and [AGENTS.md](../AGENTS.md) section 5 give the same rule for new files, so that
the controller can link the library (section 5 below).

Counts are of files carrying `SPDX-License-Identifier` (build artefacts and git-ignored files are not counted).

| Component | Path | Declared licence | Notes |
|---|---|---|---|
| DECT protocol library | `src/libopendectcell/` | GPL-2.0-or-later | all 48 source and header files |
| Daemon | `src/stubd/` | GPL-2.0-or-later | all 3 files |
| Cell link | `src/cell/` | GPL-2.0-or-later | all 3 files; links mbedTLS (section 3) |
| Firmware loader | `src/fwload/` | GPL-2.0-or-later | all 5 files; includes `kernel/include/uapi/opendectcell.h` |
| baresip module | `src/baresip/` | GPL-2.0-or-later | 1 file; built as a baresip module (section 3) |
| Capture tools | `src/tools/` | GPL-2.0-or-later | all 3 files |
| Kernel modules | `kernel/` | GPL-2.0-only | `opendectcell_pcm.c`, `opendectcell_pcm.h`, `opendectcell_ipq40xx.c`, `opendectcell_lantiq.c`, `Makefile` |
| Kernel UAPI headers | `kernel/include/uapi/opendectcell.h`, `kernel/include/uapi/ssc_pcm.h` | GPL-2.0-only WITH Linux-syscall-note (`ssc_pcm.h`: GPL-2.0 WITH Linux-syscall-note) | the interface userspace includes; the exception keeps the programs that include them free to carry their own licence, as for the kernel's own UAPI headers |
| PC tools | `tools/` | GPL-2.0-or-later | 28 files with a header; `tools/release/OPENWRT_VERSION` (a version string) has none |
| Tests | `tests/` | GPL-2.0-or-later | 36 files; the `*.trace` replay files have no header |
| OpenWrt feed: package and scripts | `openwrt/net/opendectcell/` | GPL-2.0-or-later (headers) | `PKG_LICENSE:=GPL-2.0-or-later GPL-2.0-only`, `PKG_LICENSE_FILES:=LICENSE`. The config, keep lists and `opendectcell.ntp-hotplug` have no header |
| OpenWrt feed: LuCI app | `openwrt/applications/luci-app-opendectcell/` | GPL-2.0-or-later | `PKG_LICENSE:=GPL-2.0-or-later` in its `Makefile`; both JS views carry the header; `menu.d` and `acl.d` JSON and the `.pot` file have no header |
| OpenWrt feed: kernel-module packages | `openwrt/kernel/*/Makefile` | GPL-2.0-only (headers) | the Makefiles only; see section 4 |
| OpenWrt feed: patched uclient module | `openwrt/utils/ucode-mod-opendectcell-uclient/` | Makefile GPL-2.0-or-later; `PKG_LICENSE:=ISC` | two patches to uclient, no header on the patch files; `PKG_LICENSE_FILES` is empty |
| Board and handset profiles | `boards/`, `profiles/` | no header | data files; covered by the repository statement only |
| Documentation | `docs/`, `*.md` | no separate statement | not established here |

Notes:

- `kernel/include/uapi/opendectcell.h` carries the Linux-syscall-note since 2026-11-08 (the maintainers' decision; before, it was
  GPL-2.0-only without it while `dect-stubd`, `dect-fwload` and two tools include it).
- The three `openwrt/kernel/*/Makefile` files are GPL-2.0-only although AGENTS.md section 5 says GPL-2.0-only
  belongs under `kernel/`. They package kernel modules.
- Source files carry SPDX identifiers but no copyright lines; a statement of the copyright holders is not
  established here. `openwrt/opendectcell-signing-key.pem` has no header.

## 2. Kernel and userspace

The kernel modules (`kernel/`) are GPL-2.0-only, as is usual for Linux kernel code. The programs (`dect-stubd`,
`dect-fwload`, `dect-cell`, the library) are GPL-2.0-or-later. They are separate programs: they talk to the module
through a character device (`/dev/dect0`) and its ioctls, defined in the UAPI headers, and the userspace programs are
not linked with the kernel. The licences therefore apply to each side on its own terms, and the only shared text is the
header that defines the ioctl interface (see the note above). This is a description of how the pieces are built, not a
legal opinion.

## 3. Third-party components the packages link or load

The repository does not carry the text or the licence of these components, and the page asserts none it cannot read
from the repository.

| Component | How openDECTcell uses it | Licence |
|---|---|---|
| baresip | `opendectcell-baresip` builds `opendectcell.so` from `src/baresip/opendectcell.c` and `src/libopendectcell/dect_ctl.c`; baresip loads it as a module. The package depends on `baresip` and four `baresip-mod-*` packages (`openwrt/net/opendectcell/Makefile`) | not stated in the repository: see the baresip package |
| mbedTLS | `dect-cell` is linked with `-lmbedtls -lmbedx509 -lmbedcrypto`; the package depends on `libmbedtls`; the top-level `Makefile` says it needs mbedTLS 3.6 | not stated in the repository: see the mbedtls package |
| ucode and its modules | `opendectcell-carddav` runs `carddav.uc` with `ucode`, `ucode-mod-fs`, `-uci`, `-uloop` | not stated: see the ucode package |
| uclient | the CardDAV sync uses a patched copy, packaged as `ucode-mod-opendectcell-uclient`, built from the commit the base feed packages | `PKG_LICENSE:=ISC` in its Makefile; the patches are by the project and carry no header |
| LuCI | `luci-app-opendectcell` depends on `luci-base`; `openwrt/luci.mk` includes the LuCI feed's own `luci.mk` | not stated: see the luci packages |
| `ca-bundle` | CardDAV trusts the system CAs | not stated: see the package |
| OpenWrt | the platform; `libgcc`, the C library and the kernel come with it | not stated here |

If you redistribute openDECTcell in an image, check that the licences of these packages fit together with GPL-2.0, in
particular for the baresip module, which is GPL-2.0-or-later code loaded into the baresip process. The repository does
not make that check.

## 4. The separate kernel-module repositories

The 7412 and 7362 SL transports live in their own repositories and are fetched at build time by three packages in
`openwrt/kernel/`:

| Package | Source (git) | Pinned commit | Mirror hash | `PKG_LICENSE` |
|---|---|---|---|---|
| `lantiq-ssc-pcm` | `https://github.com/openDECTcell/kmod-lantiq-ssc-pcm.git` | `829a378daae951f02cf492cad7cddf3dedfd4957` (2026-10-29) | `9458deb7...7591759` | GPL-2.0-only |
| `lantiq-tdm-pcm` | `https://github.com/openDECTcell/kmod-lantiq-tdm-pcm.git` | `4b3a55ed8a33f6679c65084ef551d0b93c1e8741` (2026-11-01) | `e35e62e0...dc1a11` | GPL-2.0-only |
| `lantiq-usif-uart` | `https://github.com/openDECTcell/kmod-lantiq-usif-uart.git` | `1789c38ff01ca958bb2c05a6df46eb44241b7736` (2026-11-01) | `9858849c...72669` | GPL-2.0-or-later GPL-2.0-only |

The full hashes are in each `Makefile` (`PKG_MIRROR_HASH`). None of the three sets `PKG_LICENSE_FILES`. The provenance
review of those repositories (where their code came from, and under which licences) is a separate task and is the
maintainer's; this repository does not contain its result. [kernel/README.md](../kernel/README.md) and
[AGENTS.md](../AGENTS.md) rule 6 describe the intended rule for code derived from published GPL sources.

## 5. The controller

`libopendectcell` is GPL-2.0-or-later. The controller
([opendectcell-controller](https://github.com/openDECTcell/opendectcell-controller), a separate repository) is described in this
repository as AGPL-3.0 (AGENTS.md section 5: "so that the AGPL-3.0 controller can link `libopendectcell`";
[ARCHITECTURE.md](../ARCHITECTURE.md): it links the library through cgo). Its exact SPDX identifier is in its own
repository and is not established here.

The reason for the "or later" is this: a GPL-2.0-or-later work can be used under the terms of GPL-3.0, and
GPL-3.0 and AGPL-3.0 are designed to be combined, so that is how such a combination is normally made. The kernel
modules are not part of it (they are GPL-2.0-only and are not linked into the controller). This is a description of
the project's reasoning, not a legal opinion, and the maintainers have not had a legal review.

## 6. Provenance and the project's rules

[AGENTS.md](../AGENTS.md) section 2 and the "Clean room" section of [README.md](../README.md) set the rules, here in
summary:

- **No vendor code in the repository.** No vendor binaries, kernel modules, firmware, images, device-tree source,
  calibration data or analysed code are committed. Such material stays outside it. Facts about the chip's interface
  are written down in the project's own words, each with a label saying how it is known, and new code is written from
  those documents.
- **No vendor firmware.** The DECT chip firmware and the per-device calibration data are never shipped. Each user
  takes them from the vendor's own image for their own box, with `tools/dect-fw-extract.py`; documents name the files
  only by SHA-256 ([firmware-sourcing.md](firmware-sourcing.md)).
- **The standards are the basis.** The protocol stack follows the public ETSI standards (EN 300 175, EN 300 444,
  TS 102 527), which are downloaded, not committed ([specs/README.md](specs/README.md)), and ITU-T G.722.
- **Published constants are labelled.** Where code uses constants from published material (the DECT authentication
  algorithm's tables come from published research and its test vectors, and the G.722 codec was checked against an
  independent reference), the source file or the README's Credits says so. The README states that openDECTcell contains no
  code from other projects.
- **The vendor's published GPL source** may be read and used under its licence, with the original notices kept in
  derived code and the source file named (AGENTS.md rule 6).
- **Legal basis** as stated in the README: EU Directive 2009/24/EC Art. 5(3) and Art. 6 (interoperability), in Germany
  §69d and §69e UrhG. This is the project's statement, not an assessment of it.

The evidence labels (`[observed]`, `[analysed]`, `[CR A-NNNN]`, *assumed*) are explained in
[docs/README.md](README.md#labels-used-in-the-documents).

### Similarity check against libdect (2026-11-08)

libopendectcell is GPL-2.0-or-later so that the AGPL controller can link it; libdect (osmocom, GPL-2.0-only) was used as a
cross-check while it was written. To show that nothing was copied, `src/libopendectcell/` and `src/stubd/` were compared
mechanically with libdect's current source (`gitea.osmocom.org/dect/libdect`, all 89 `.c`/`.h` files): identical
code lines of 25 characters or more, comments removed, and shared runs of 12 tokens.

- Every file except one: at most a few identical lines, all generic (`for (i = 0; i < len; i++)`, `case
  DECT_MM_AUTHENTICATION_REQUEST:` and the like), and at most 5 % shared 12-token runs, from declarations and
  loops.
- `dect_dsaa.c` (the DSAA authentication cipher) shares the 256-byte S-box table and the names of the algorithm's
  steps (`bitperm`, `mix1`..`mix3`, `cassable`, `step1`, `step2`) with libdect's `dsaa.c`. Both come from the public
  description of DSAA (deDECTed, Tews' thesis appendix B.1, Lucks et al. 2010), where the cipher is called
  `cassable`; the table is the algorithm itself. The code differs: libopendectcell works on 64-bit words
  (`load64`/`store64`/`rev64`), libdect on byte arrays, and the functions' signatures and bodies are different.
  Outside the table the shared 8-token runs are loop headers and the calls of the named steps.

The check was mechanical; it says that no code was copied, not that the result is legally reviewed.

## 7. For redistributors

If you give out openDECTcell, in binary packages or in an image:

- **Offer the source** of the openDECTcell packages and of the kernel-module repositories in section 4 at the pinned commits.
  The release packages are built from the tag named in `PKG_SOURCE_URL` in `openwrt/net/opendectcell/Makefile` (currently
  `v0.3.3`), which is what you would point to.
- **Include the licence texts**: `LICENSE` (GPL-2.0) and those of the third-party packages you ship (section 3).
  Keep the SPDX headers and notices.
- **Say that the DECT chip firmware is not included** and that the user supplies their own, from their own box's image
  ([firmware-sourcing.md](firmware-sourcing.md)). Do not put it in an image, a backup you publish, or an issue: a
  configuration backup contains it ([privacy-and-data.md](privacy-and-data.md#3-backups)).
- **Do not ship personal data**: no handset stores, no `/etc/config/opendectcell` with passwords, no cell credentials
  ([privacy-and-data.md](privacy-and-data.md)).
- Release packages are signed with the project's key ([README.md](../README.md), [install.md](install.md)); packages you
  build yourself install with `apk add --allow-untrusted` ([CONTRIBUTING.md](../CONTRIBUTING.md)).
- Ask a lawyer about anything beyond this page.
