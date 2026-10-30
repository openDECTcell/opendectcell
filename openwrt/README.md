# The openDECTcell OpenWrt feed

This directory is an OpenWrt package feed laid out the way OpenWrt's own
repositories are, so that each package can be submitted as it stands:

| here | package(s) | upstream home |
|---|---|---|
| `net/opendectcell/` | `opendectcell`, `opendectcell-baresip`, `opendectcell-cell`, `opendectcell-carddav`, `kmod-opendectcell-ipq40xx`, `kmod-opendectcell-lantiq` | [openwrt/telephony](https://github.com/openwrt/telephony) `net/opendectcell/` (next to baresip) |
| `kernel/lantiq-ssc-pcm/` | `kmod-lantiq-ssc-pcm` (the 7412's PCM/TDM transport, from its own repository) | openwrt/telephony, or the core repository's `package/kernel/lantiq/` |
| `utils/ucode-mod-opendectcell-uclient/` | `ucode-mod-opendectcell-uclient` (uclient's ucode module with sized request bodies, for `opendectcell-carddav`) | none of its own: the patch belongs in [uclient](https://git.openwrt.org/?p=project/uclient.git), and then this package goes |
| `applications/luci-app-opendectcell/` | `luci-app-opendectcell` | [openwrt/luci](https://github.com/openwrt/luci) `applications/luci-app-opendectcell/` |

Not part of any submission: `luci.mk` (hands `include ../../luci.mk` over
to the LuCI feed of the build tree), `opendectcell-signing-key.pem` (our own
releases' signing key) and this file.

## Using it

In an OpenWrt build tree or SDK:

```
echo "src-link opendectcell /path/to/opendectcell/openwrt" >> feeds.conf
./scripts/feeds update base packages telephony luci opendectcell
./scripts/feeds install -a -p opendectcell
```

`net/opendectcell` downloads the release archive named by `PKG_VERSION` and
checks it against `PKG_HASH`, like any feed package. To build the checkout
instead, `tools/release/build.sh TARGET OUTDIR` does the same with an
archive of the working tree (and is what the Packages workflow runs).

## Versions and the source hash

`PKG_VERSION` in `net/opendectcell/Makefile` is the project's version (the
binaries' `--version`, the release tag `v<PKG_VERSION>`). A Makefile cannot
carry the hash of the archive of its own commit, so a release takes two
commits:

1. Set `PKG_VERSION` (and `PKG_RELEASE:=1`), commit, tag `v<version>`, push
   the tag.
2. `tools/release/feed-hash.sh` downloads the tag's archive from GitHub,
   writes its `PKG_HASH`, and that is committed as
   `openwrt: opendectcell <version> source hash`.

After step 2 the feed is ready to submit as it stands. `luci-app-opendectcell`
has no version of its own: LuCI's `luci.mk` numbers it from the git history
(and `build.sh` gives it the openDECTcell version).

## Conventions followed

From [openwrt/packages CONTRIBUTING.md](https://github.com/openwrt/packages/blob/master/CONTRIBUTING.md),
the [submission guidelines](https://openwrt.org/submitting-patches), the
LuCI repository's layout and the formalities bot's checks:

- Source from a fixed release archive with `PKG_HASH` (`kmod-lantiq-ssc-pcm`:
  a pinned git commit with `PKG_MIRROR_HASH`, as its repository has no
  releases), SPDX `PKG_LICENSE`, `PKG_LICENSE_FILES`, `PKG_MAINTAINER`.
- One source package for everything built from one archive; literal
  package names in `call`/`eval`; two-space metadata blocks, tab recipes,
  unindented `conffiles`.
- Every user-modifiable file the package installs under `/etc` is in
  `conffiles` (`/etc/config/opendectcell`, `/etc/opendectcell/profiles/`).
- procd init script; no autoload of the kernel modules (the service loads
  them only when enabled).
- `TARGET_CFLAGS`, `TARGET_LDFLAGS` (OpenWrt's hardening flags) reach every
  binary; every program answers `--version` with `PKG_VERSION`, which the
  package CI checks.
- LuCI app: `luci.mk`, `htdocs/` and `root/`, a translation template in
  `po/templates/opendectcell.pot`.

## Known gaps

Things a reviewer would raise that this layout cannot fix by itself:

- `PKG_MAINTAINER` is the project handle with a real address; the
  formalities check accepts that but warns that the name looks like a
  nickname, and the submission guidelines ask for a real name. A
  submission's commits also need a matching `Signed-off-by`.
- The kernel modules find their hardware by board compatible and fixed
  register addresses (`kernel/opendectcell_ipq40xx.c`, `kernel/opendectcell_lantiq.c`);
  reviewers will ask for device-tree nodes in the boards' DTS, which is a
  change to OpenWrt's core repository.
- `files/opendectcell.uci-defaults` comments out the console getty in
  `/etc/inittab`, and the init script stops a running one, because the
  DECT chip's UART is the serial console on these boxes; that belongs in
  the boards' definitions in the core repository.
- The LuCI ACL lets the app run `dect-stubd --ctl` with any argument; LuCI
  prefers an rpcd/ubus plugin with fixed methods.
- The translation template holds every `_('…')` of the JavaScript files
  and the ACL's description, in the order `xgettext --sort-by-file` gives,
  but is not made with LuCI's `build/i18n-scan.pl`; the LuCI repository
  regenerates templates with its own tools anyway.
- OpenWrt takes new packages on its main branch only, which builds a newer
  kernel (6.18) than the 25.12 releases this feed is built and tested for.
- OpenWrt's AI policy: contributions must disclose LLM use and must not be
  made by autonomous agents.
