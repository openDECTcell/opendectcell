# Making a release

How an openDECTcell release is built and published. A release has the OpenWrt packages for the 7520/7530 (`ipq40xx`)
and the 7412 and 7362 SL (`lantiq`), a flashable image for each of the three with the packages inside, a checksum file and release notes.

## One OpenWrt version per release

The kernel module loads only into the kernel it was built for, so a release is built for one OpenWrt version, named
in **one place**: [`tools/release/OPENWRT_VERSION`](../tools/release/OPENWRT_VERSION). `build.sh` and `images.sh`
read it (override with `OPENWRT=...`), the `Packages` workflow must name the same version, and
`tests/test_release_files.py` (part of `make check`) fails if they differ. When OpenWrt moves on, change the file
and the workflow together.

## Before building

| Check | Why |
|---|---|
| `make check` passes, and `make check-cell MBEDTLS=<prefix>` if the cell link changed (mbedTLS 3.6, see [../tests/README.md](../tests/README.md)) | the unit tests and the cell-link tests |
| `PKG_VERSION` in `openwrt/net/opendectcell/Makefile` is the new version, `tools/release/notes-<version>.md` exists | `build.sh` names the files by it; the notes are the release text (placeholders `{VERSION}` and `{OPENWRT}`) |
| The kernel-module repositories are reachable (`git ls-remote` works for the URLs in `openwrt/kernel/*/Makefile`) | The 7412 has no audio without `kmod-lantiq-ssc-pcm`. `build.sh` stops if a transport is not reachable; `ALLOW_MISSING_KMOD=1` allows a build without it, and `TRANSPORTS="lantiq-ssc-pcm"` leaves the 7362 SL's modules out on purpose |
| Everything is committed and pushed | the packages are built from `git archive` of the tree (plus uncommitted changes of tracked files), and the release names a commit |
| Every `PKG_MIRROR_HASH` in `openwrt/kernel/*/Makefile` is a real hash | `make check` fails on `skip` |

## Building

On the maintainer's machine (about fifteen minutes with the SDKs and ImageBuilders cached in `build/release/`):

```sh
export OPENWRT=$(cat tools/release/OPENWRT_VERSION)
for t in lantiq/xrx200 ipq40xx/generic; do
  tools/release/build.sh $t build/release/out/apk-${t//\//-}                  # the .apk files
done
for t in lantiq/xrx200 ipq40xx/generic; do
  tools/release/images.sh $t build/release/out/apk-${t//\//-} build/release/out/img-${t//\//-}   # the sysupgrade image
done
```

Add `TRANSPORTS=lantiq-ssc-pcm` to the first command for `lantiq/xrx200` to leave the 7362 SL's modules and its
meta package out of a 7412 release. Check what came out: each image's `.manifest` lists the packages inside; `apk
adbdump --allow-untrusted FILE` (the SDK has an `apk`) shows a package's dependencies and the board profiles it
carries (`usr/share/opendectcell/boards`: only the target's own).

Collect the files for the release: the apks (the architecture-independent `luci-app-opendectcell` and
`opendectcell-carddav` are identical for both targets: take one copy), the images and manifests, and
`sha256sum *.apk *sysupgrade* > SHA256SUMS`. Leave out packages for boards the release does not support.

## Signing

`build.sh` signs each package with the project's key if `SIGN_KEY` names it; users then trust
`openwrt/opendectcell-signing-key.pem` once. Without it the SDK signs with its own throwaway key and users install with
`apk add --allow-untrusted`; the images need nothing. **The project's key exists only as a GitHub secret** and
the way it is handled is an open finding ([security-review-2026-11-02.md](security-review-2026-11-02.md), M-2: sign in a
last job that no build script runs in, protect the environment, keep an offline copy). Until that is settled, say in
the release notes which kind of signature the packages carry.

## Publishing

```sh
gh release create vX.Y.Z --target <commit> --prerelease --title "opendectcell X.Y.Z" \
   --notes-file notes.md  FILES...
```

The notes are `tools/release/notes-X.Y.Z.md` with the placeholders filled in. The tag is made by `gh` at `--target`.
The `Packages` workflow does **not** run on a tag any more (it is manual, so a tag costs no CI minutes and cannot
race a local release). To use CI instead, run the workflow by hand from the tag's ref: the `release` job runs only
on a `v*` ref and makes a *draft* release, which needs the signing secret; CI cannot reach the private
kernel-module repositories, so its 7412 build has no audio transport (`ALLOW_MISSING_KMOD=1` in the workflow).

After publishing, download `SHA256SUMS` from the release and compare it with the local copy.

## What a release must say

The OpenWrt version, the kernel the module fits, which boxes are supported and which were tested ([status.md](status.md)),
the kind of signature, the kernel-module commits that went in, and what is known to be wrong. Do not list a board
as supported if its transport module is not in the release.
