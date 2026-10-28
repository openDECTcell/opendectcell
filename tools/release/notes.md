Packages for **OpenWrt {OPENWRT}**, built with its official SDK. The kernel
module loads only into that OpenWrt version's kernel.

- **FRITZ!Box 7520 / 7530:** `kmod-opendectcell-ipq40xx`, `opendectcell` (`arm_cortex-a7_neon-vfpv4`)
- **FRITZ!Box 7412:** `kmod-opendectcell-lantiq`, `opendectcell` (`mips_24kc`). Built, but not yet tested on a 7412.
- **Both:** `luci-app-opendectcell` (web pages), `opendectcell-baresip` (SIP lines)

The DECT chip firmware is not included and never will be: each user
extracts it from an official FRITZ!OS image.

Install in the web interface: add `opendectcell-signing-key.pem` once under
System > Administration > Repo Public Keys, then System > Software >
Upload Package. Details, and the firmware step:
[docs/install.md](https://github.com/openDECTcell/opendectcell/blob/v{VERSION}/docs/install.md).
Check the files with `sha256sum -c SHA256SUMS --ignore-missing`.
