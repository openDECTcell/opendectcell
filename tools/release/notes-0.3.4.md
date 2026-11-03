**Pre-release for testers, not announced.** Packages and ready-made images for **OpenWrt {OPENWRT}**, built on the
maintainer's machine with the official SDK and ImageBuilder. The packages carry the SDK's own throwaway signature, not the
project's key, so install them with `apk add --allow-untrusted` (the images need nothing). The kernel module loads only into that OpenWrt version's kernel.

What is new since 0.3.3:

- **Enrolling a cell in LuCI works and says what happens.** It runs in the background and gives up after 90 seconds
  (it used to wait for ever). LuCI now says when the box enrolled but its cell service does not run (the DECT service
  switched off, or the chip firmware missing), because the controller then shows the box as offline.
- **LuCI: "Permission denied" on Enrol, the firmware upload and the CardDAV import is fixed.** The rpcd access list named
  the hand-over files under `/var/run`; rpcd matches the resolved path `/tmp/run`.
- **Security review of 2026-11-02, the parts that touch the box:** a narrower rpcd access list and the hand-over files in a
  root-only directory (SEC-05); `baresip` runs as its own user and listens on the LAN address, named peers for accounts that
  do not register (SEC-03); an attempt budget for handset authentication, `{DETACH}` handling, no oracle in the FT
  authentication and wiped keys (SEC-04, libopendectcell API 155); logs without secrets (PRIV-02, API 156). Details:
  docs/security-review-2026-11-02.md.
- The base station still runs `dect-stubd` and `dect-cell` as root, on purpose for now (docs/security-review-2026-11-02.md).

Carried over from 0.3.3: internal calls between handsets on different cells, the Internal Names list across cells,
the phonebook per handset, `{DETACH}`, list access that waits for the handset's proof of its key, and only the board
profile of the box's own target installed.

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
