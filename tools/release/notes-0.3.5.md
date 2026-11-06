**Pre-release for testers, not announced.** Packages and ready-made images for **OpenWrt {OPENWRT}**, built on the
maintainer's machine with the official SDK and ImageBuilder. The packages carry the SDK's own throwaway signature, not the
project's key, so install them with `apk add --allow-untrusted` (the images need nothing). The kernel module loads only into that OpenWrt version's kernel.

What is new since 0.3.4:

- **A handset counts as present only once it has proved its key** (libopendectcell API 157). A frame that merely carries a handset's
  identity, which is sent in the clear, no longer makes the base treat that handset as alive.
- **Motorola O201:** a device profile (`motorola-o201`, basic service 0x80) lets it ring and play audio both ways. For handsets that
  need the progress indication inside the call message there is the key `cc.progress_in_band` (API 159). The reason a handset gives
  when it releases a call is kept and named in the log (API 158). Details: docs/vendor-specific-support.md §7.
- **FRITZ!Box 7362 SL:** runs as a cell of a controller (calls in and out seen on the air). The firmware-load UART's device node is
  awaited after its module loads; frames in the first seconds after start are still dropped.
- **LuCI: "Leave cell mode" reloads the page.** The restart took longer than LuCI's call to the box may last, so the click found no
  answer; the restart now runs in the background. (Not yet tried on a box when this was written.)
- **Per-call audio levels in the log** of `dect-stubd`, for finding a one-way audio problem.
- Documentation: the handset-variation catalogue (507 variations from the standards, docs/handset-variation-catalog.md), the plan
  for the DECT Command Finder, the Motorola O201 in docs/handset-compatibility.md.

Known limits: the cell announces **three** voice channels for every box. The 7520's chip has five; that needs new work (ROADMAP).
`dect-stubd` and `dect-cell` still run as root, on purpose for now.

- **FRITZ!Box 7520 / 7530:** `kmod-opendectcell-ipq40xx`, `opendectcell` (`arm_cortex-a7_neon-vfpv4`), `opendectcell-cell`;
  image `openwrt-{OPENWRT}-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin` (OpenWrt ships the 7520 with
  the 7530's image).
- **FRITZ!Box 7412:** `kmod-opendectcell-lantiq`, `kmod-lantiq-ssc-pcm` (the transport; its own repository, commit `e3c898d`),
  `opendectcell`, `opendectcell-cell` (`mips_24kc`); image `openwrt-{OPENWRT}-lantiq-xrx200-avm_fritz7412-squashfs-sysupgrade.bin`.
  Open defect: in about 4 of 190 service stops (2026-10-30/31) the box's network died; 640 stops since could not
  reproduce it (docs/7412-stop-hang.md). A box that hangs needs a power cycle.
- **FRITZ!Box 7362 SL:** the same lantiq packages plus `opendectcell-fritz7362sl`, `kmod-lantiq-tdm-pcm` (commit `4450891`) and
  `kmod-lantiq-usif-uart` (commit `f359106`). It has no image of its own: install the packages on OpenWrt {OPENWRT}.
- **All:** `luci-app-opendectcell` (web pages), `opendectcell-baresip` (SIP lines, standalone mode), `opendectcell-carddav`.

The DECT chip firmware is not included and never will be: each user extracts it from an official FRITZ!OS image
(docs/firmware-sourcing.md). With a controller, the whole way is in its
[wiki](https://github.com/openDECTcell/opendectcell-controller/blob/main/docs/wiki/Quick-start.md).
Check the files with `sha256sum -c SHA256SUMS --ignore-missing`.
