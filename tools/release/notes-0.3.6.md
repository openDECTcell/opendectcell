**Pre-release for testers, not announced.** Packages and ready-made images for **OpenWrt {OPENWRT}**, built on the
maintainer's machine with the official SDK and ImageBuilder. The packages carry the SDK's own throwaway signature, not the
project's key, so install them with `apk add --allow-untrusted` (the images need nothing). The kernel module loads only into that OpenWrt version's kernel.

What is new since 0.3.5:

- **Same programs, cleaned up for publication.** No change in what the daemons do. The wording of a few comments and of the 7362 SL board profile
  was made neutral, and the LuCI monitor page has one text corrected. Nothing here needs a new test on a box beyond the usual first start.
- **Release signing moved into its own CI job** (the key is used by no build step); this release was built on the maintainer's machine, so its
  packages again carry the SDK's throwaway signature.
- Community files (security policy, contributing guide, licence texts, issue templates) and the legal notices of the controller are in the repositories.

Carried over from 0.3.5: a handset counts as present only once it has proved its key (API 157); the Motorola O201 profile; the 7362 SL as a
cell of a controller; "Leave cell mode" in LuCI no longer waits for the restart (not yet tried on a box); per-call audio levels in the log.

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
  `kmod-lantiq-usif-uart` (commit `f359106`); image `openwrt-{OPENWRT}-lantiq-xrx200-avm_fritz7362sl-squashfs-sysupgrade.bin`
  (added to the release after it was first published; not flashed on a box yet).
- **All:** `luci-app-opendectcell` (web pages), `opendectcell-baresip` (SIP lines, standalone mode), `opendectcell-carddav`.

The DECT chip firmware is not included and never will be: each user extracts it from an official FRITZ!OS image
(docs/firmware-sourcing.md). With a controller, the whole way is in its
[wiki](https://github.com/openDECTcell/opendectcell-controller/blob/main/docs/wiki/Quick-start.md).
Check the files with `sha256sum -c SHA256SUMS --ignore-missing`.
