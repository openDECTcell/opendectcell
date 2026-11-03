**Pre-release for testers, not announced.** Packages and ready-made images for **OpenWrt {OPENWRT}**, built on the
maintainer's machine with the official SDK and ImageBuilder. The packages carry the SDK's own throwaway signature, not the
project's key, so install them with `apk add --allow-untrusted` (the images need nothing). The kernel module loads only into that OpenWrt version's kernel.

What is new since 0.3.2:

- **Internal calls between handsets on different cells** (with the controller): libopendectcell API 153 takes a call-in
  from the bridge marked internal; the controller finds the number inside one account only.
- **The Internal Names list across cells** (API 152): a handset sees the account's handsets that sit on other cells,
  as well as the ones on its own; handsets without a list (Part 1, such as many Gigasets) type the number after INT.
- **Phonebook per handset** (API 151) and **{DETACH}** (a handset that switches itself off is counted, API 153).
- **List access waits for the handset's proof** of its key (API 154), and the other fixes of the 2026-11-02 security
  review that touch the box (bounds in the contact folding, the dial string filter, random numbers, CardDAV
  credentials). Details: docs/security-review-2026-11-02.md.
- **Only the board profile of the box's own target is installed**; the 7362 SL's is in its own package.

- **FRITZ!Box 7520 / 7530:** `kmod-opendectcell-ipq40xx`, `opendectcell` (`arm_cortex-a7_neon-vfpv4`), `opendectcell-cell`;
  image `openwrt-{OPENWRT}-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin` (OpenWrt ships the 7520 with
  the 7530's image).
- **FRITZ!Box 7412:** `kmod-opendectcell-lantiq`, `kmod-lantiq-ssc-pcm` (the transport; built from its own repository),
  `opendectcell`, `opendectcell-cell` (`mips_24kc`); image `openwrt-{OPENWRT}-lantiq-xrx200-avm_fritz7412-squashfs-sysupgrade.bin`.
  Open defect: in about 4 of 190 service stops (2026-10-30/31) the box's network died; 640 stops since could not
  reproduce it (docs/7412-stop-hang.md). A box that hangs needs a power cycle.
- **Both:** `luci-app-opendectcell` (web pages), `opendectcell-baresip` (SIP lines, standalone mode), `opendectcell-carddav`.

The DECT chip firmware is not included and never will be: each user extracts it from an official FRITZ!OS image
(docs/firmware-sourcing.md). With a controller, the whole way is in its
[wiki](https://github.com/openDECTcell/opendectcell-controller/blob/main/docs/wiki/Quick-start.md).
Check the files with `sha256sum -c SHA256SUMS --ignore-missing`.
