# Installing openDECTcell from a release

Each release on GitHub has ready-built packages for OpenWrt. You need a box
that already runs OpenWrt, of exactly the version named in the release
(the kernel module only loads into the kernel it was built for).

## 1. Which files

| File | What it is | Needed |
|---|---|---|
| `kmod-opendectcell-ipq40xx-…_ipq40xx-generic.apk` | kernel module, FRITZ!Box 7520/7530 | yes, on a 7520/7530 |
| `kmod-opendectcell-lantiq-…_lantiq-xrx200.apk` | kernel module, FRITZ!Box 7412 | yes, on a 7412 (not yet tested on one) |
| `opendectcell-…_<arch>.apk` | the DECT base: firmware loader, daemon, service | yes |
| `luci-app-opendectcell-…_all.apk` | web pages: Network > DECT and Status > DECT | recommended |
| `opendectcell-baresip-…_<arch>.apk` | SIP: connects the handsets' lines to a SIP provider or telephone system | for phone calls outside the house |

`<arch>` is `arm_cortex-a7_neon-vfpv4` for the 7520/7530 and
`mips_24kc` for the 7412. `SHA256SUMS` lists every file's checksum, and
`opendectcell-signing-key.pem` is the key the packages are signed with (the same
as [openwrt/opendectcell-signing-key.pem](../openwrt/opendectcell-signing-key.pem) in
the repository).

## 2. Install

The box needs internet access while installing: OpenWrt's package manager
fetches what openDECTcell builds on, such as baresip, from OpenWrt.

### In the web interface

1. **Once: trust the project's key.** The packages are signed with the
   openDECTcell key, not OpenWrt's. Open System > Administration, tab
   *Repo Public Keys*, paste the content of `opendectcell-signing-key.pem` from
   the release and press *Add key*. Without this, the upload below ends in
   "UNTRUSTED signature".
2. Open System > Software and press *Update lists…*.
3. Press *Upload Package…* and install the files one by one, in this order:
   the kernel module, `opendectcell`, `luci-app-opendectcell`, and for SIP
   `opendectcell-baresip`.

### On the command line

Copy the files and the key to the box, for example into `/tmp`:

```sh
scp -O *.apk SHA256SUMS opendectcell-signing-key.pem root@192.168.1.1:/tmp/
```

On the box (`ssh root@192.168.1.1`):

```sh
cd /tmp
sha256sum -c SHA256SUMS --ignore-missing
cp opendectcell-signing-key.pem /etc/apk/keys/
apk update
apk add ./kmod-opendectcell-*.apk ./opendectcell-[0-9]*.apk ./luci-app-opendectcell-*.apk
```

For SIP add `./opendectcell-baresip-*.apk` to the same line (or install it later the
same way).

## 3. The DECT chip firmware

openDECTcell may not ship AVM's firmware for the DECT chip, so every user
extracts it once from an official FRITZ!OS image for their box, on their own
PC: see [firmware-sourcing.md](firmware-sourcing.md). OpenWrt upgrades keep
it.

## 4. Switch on and register

```sh
uci set opendectcell.main.enabled=1; uci commit opendectcell
/etc/init.d/opendectcell start
```

Then open Network > DECT in the web interface, tab Handsets, press
*Register a handset* and start the registration on the handset with the
PIN shown there. Each handset's Line 1 and Line 2 take its SIP account
(button *Edit*).

## Upgrading and removing

A newer release installs the same way, over the old one; your settings in
`/etc/config/opendectcell` stay. `apk del opendectcell-baresip luci-app-opendectcell openDECTcell
kmod-opendectcell-ipq40xx` removes everything again.
