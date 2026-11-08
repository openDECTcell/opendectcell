**Pre-release for testers, not announced. About 99 % of the code was written by AI agents and only smaller parts were reviewed by a human; there is no warranty whatsoever and use is purely experimental.** Packages and ready-made images for **OpenWrt {OPENWRT}**, built on the
maintainer's machine with the official SDK and ImageBuilder. The packages carry the SDK's own throwaway signature, not the
project's key, so install them with `apk add --allow-untrusted` (the images need nothing). The kernel module loads only into that OpenWrt version's kernel.

**Nothing of this release has been installed on a box yet:** the packages were built and checked (`make check`, the cell-link test with the sanitizers, the controller's tests), not run on hardware. The functional code is that of 0.3.6 plus the changes below.

What is new since 0.3.6:

- **The project is now called openDECTcell (it was openDECTcell).** Packages, programs, files, the UCI config and the paths carry the new name (`opendectcell`, `/etc/config/opendectcell`, `/etc/opendectcell`). **There is no migration from the openDECTcell 0.3.x packages:** remove them, install these, and register the handsets again. Keeping the registrations by copying `/etc/opendectcell-sub.*` to `/etc/opendectcell-sub.*` has not been tried.
- **A rename bug caught before it shipped:** the cell checked its enrolment string, the calls-list header and two certificate URN prefixes with lengths of the old name. These checks now use the length of the text itself (`sizeof`), so the enrolment of a cell works with the new name. The same bytes are compared as before.
- **The README, the controller's standard text and the notes say plainly** that the code is almost entirely AI-written, unreviewed in large parts, and without warranty.
- `dect-stubd.c` is split into nine files (no change in what the daemon does; tried on the lab 7520 and a 7412 start, no call).
- `dect-fwload` names the likely cause when the chip never answers after a short power pull (a 7412 may need five minutes without power; the install guide says so).
- Community files, licence texts and the legal notices of the controller are in the repositories.

Carried over from 0.3.6: a handset counts as present only once it has proved its key (API 157); the Motorola O201 profile; the 7362 SL as a
cell of a controller; "Leave cell mode" in LuCI no longer waits for the restart (not yet tried on a box); per-call audio levels in the log.

Known limits: the cell announces **three** voice channels for every box. The 7520's chip has five; that needs new work (ROADMAP).
`dect-stubd` and `dect-cell` still run as root, on purpose for now.

- **FRITZ!Box 7520 / 7530:** `kmod-opendectcell-ipq40xx`, `opendectcell` (`arm_cortex-a7_neon-vfpv4`), `opendectcell-cell`;
  image `openwrt-{OPENWRT}-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin` (OpenWrt ships the 7520 with
  the 7530's image).
- **FRITZ!Box 7412:** `kmod-opendectcell-lantiq`, `kmod-lantiq-ssc-pcm` (the transport; its own repository, commit `829a378`),
  `opendectcell`, `opendectcell-cell` (`mips_24kc`); image `openwrt-{OPENWRT}-lantiq-xrx200-avm_fritz7412-squashfs-sysupgrade.bin`.
  Open defect: in about 4 of 190 service stops (2026-10-30/31) the box's network died; 640 stops since could not
  reproduce it (docs/7412-stop-hang.md). A box that hangs needs a power cycle.
- **FRITZ!Box 7362 SL:** the same lantiq packages plus `opendectcell-fritz7362sl`, `kmod-lantiq-tdm-pcm` (commit `4b3a55e`) and
  `kmod-lantiq-usif-uart` (commit `1789c38`); image `openwrt-{OPENWRT}-lantiq-xrx200-avm_fritz7362sl-squashfs-sysupgrade.bin`
  (not flashed on a box yet).
- **All:** `luci-app-opendectcell` (web pages), `opendectcell-baresip` (SIP lines, standalone mode), `opendectcell-carddav`.

The DECT chip firmware is not included and never will be: each user extracts it from an official FRITZ!OS image
(docs/firmware-sourcing.md). With a controller, the whole way is in its
[wiki](https://github.com/openDECTcell/opendectcell-controller/blob/main/docs/wiki/Quick-start.md).
Check the files with `sha256sum -c SHA256SUMS --ignore-missing`.
