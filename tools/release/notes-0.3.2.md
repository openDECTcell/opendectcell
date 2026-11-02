**Pre-release for the lab, 2026-11-02. Not announced, not for general use.** Packages for **OpenWrt {OPENWRT}**,
built with its official SDK. The kernel module loads only into that OpenWrt version's kernel.

What is new since 0.3.1:

- **Cell mode** (`opendectcell-cell`): the box can be a cell of a
  [openDECTcell controller](https://github.com/openDECTcell/opendectcell-controller): TLS 1.3 with mutual authentication,
  enrolment with a one-time string, certificate renewal. A firmware upgrade now **keeps the cell's enrolment**
  (`/etc/opendectcell/cell/`).
- **libopendectcell API 150**: the TPUI a 06.88 chip (FRITZ!Box 7412) hears; a page response nobody waits for is rejected and
  its link released; a handset that locates while a call holds the main link is told when to register again.
- Handsets can move between cells; the box works as a cell with a 7520 and a 7412 on one controller.

- **FRITZ!Box 7520 / 7530:** `kmod-opendectcell-ipq40xx`, `opendectcell` (`arm_cortex-a7_neon-vfpv4`), `opendectcell-cell`
- **FRITZ!Box 7412:** `kmod-opendectcell-lantiq`, `opendectcell`, `opendectcell-cell` (`mips_24kc`). Works as a cell with
  `kmod-lantiq-ssc-pcm`, which is **not in this release** (its own repository): without it calls have no audio.
  Open defect: in about 4 of 190 service stops the box's network died.
- **Both:** `luci-app-opendectcell` (web pages), `opendectcell-baresip` (SIP lines, standalone mode)

The DECT chip firmware is not included and never will be: each user extracts it from an official FRITZ!OS image.

Install in the web interface: add `opendectcell-signing-key.pem` once under System > Administration > Repo Public Keys,
then System > Software > Upload Package. Details, and the firmware step:
[docs/install.md](https://github.com/openDECTcell/opendectcell/blob/v{VERSION}/docs/install.md). With a controller, the whole way
is in its [wiki](https://github.com/openDECTcell/opendectcell-controller/blob/main/docs/wiki/Quick-start.md).
Check the files with `sha256sum -c SHA256SUMS --ignore-missing`.
