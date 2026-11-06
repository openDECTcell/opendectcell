# Dedicated OpenWrt images for the 7520 and the 7412

Written 2026-11-01 after the maintainer's request (2026-10-31): the controller's
and the cells' builds are not enough, a user needs a flashable image.

**Status 2026-11-01 (night):** built and tried locally for the 7520 target;
nothing dispatched in CI (the maintainer watches CI minutes). `tools/release/images.sh`
makes the image from the packages `build.sh` made; the `images` job in
`packages.yml` runs it on a tag or when the Packages workflow is started by
hand with "images" ticked. Run locally on 2026-11-01 against the 25.12.5
ImageBuilder with the CI packages of run 41239885068: it produced
`openwrt-25.12.5-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin`
(11 MB; manifest lists openDECTcell, opendectcell-cell, opendectcell-baresip, the kernel
module, LuCI, baresip). **Not flashed.** The 7412 target is untried (see I2).
Answers of the maintainer, 2026-11-01: I1 one release (25.12.6) for both,
I2 first a read-only deploy key for the PCM module's repository; later (2026-11-06) the three Lantiq transport
repositories are published instead, so CI needs no key, I3 my lean, I5 images only on tags and manual runs.
The lab 7520 runs 25.12.5, so the new kernel module package fits it only after
it is flashed with an image (the userspace packages install over apk).

## Why images, and what the first look found

- The CI packages are built against **one** OpenWrt release (the SDK of
  `OPENWRT` in `packages.yml`). A kernel module package only fits the exact
  kernel it was built for. The two boxes in the lab run different releases:
  the 7520 OpenWrt 25.12.5 (kernel 6.12.94), the 7412 25.12.6 (6.12.112). So
  until 2026-11-01 the CI `kmod-opendectcell-lantiq` could not be installed on the
  7412 (kernel 6.12.94 vs 6.12.112); only the userspace packages could.
  `packages.yml` now builds each target against the release its box runs.
- OpenWrt ships the 7520 with the 7530's image: profile `avm_fritzbox-7530`
  (target `ipq40xx/generic`, titles "FRITZ!Box 7530, 7520"); the 7412 is
  `avm_fritz7412` (target `lantiq/xrx200`). Both have a `sysupgrade` image and
  an `initramfs` kernel; a first flash from AVM's firmware uses the initramfs
  path (docs/models/*), later ones sysupgrade.

## Approach

1. **ImageBuilder** of the same release as the SDK, per target, downloaded and
   checked against OpenWrt's `sha256sums` (as `tools/release/build.sh` does for
   the SDK). `make image PROFILE=... PACKAGES="..." FILES=files/`.
2. The openDECTcell packages come from the Packages job's output, as a local
   **apk repository signed with the project key** (secret `OPENDECTCELL_SIGN_KEY`),
   its public key placed in the image (`/etc/apk/keys/`), so the image's `apk`
   trusts later openDECTcell updates. ImageBuilder needs the repository's index
   (`apk mkndx`) and the key; `build.sh` already signs.
3. **Package set** per image: the openDECTcell set (`opendectcell`, `opendectcell-cell`,
   `opendectcell-baresip`, `opendectcell-carddav`, `luci-app-opendectcell`, the kernel module
   for the target) plus `luci`, `luci-ssl`, and for the 7412 the Lantiq module
   `kmod-lantiq-ssc-pcm` (its source repo is separate and, in CI, private:
   the 7412 build goes without it today, see the next section).
4. **What stays out:** AVM material. The chip firmware is extracted on the box
   from the user's AVM image or from the box's own partitions (AGENTS.md rule
   5); an image contains none of it. A first-boot note says so.
5. A workflow `images.yml` (dispatch and tag only, like Packages) that runs
   after Packages and uploads the images and `SHA256SUMS`; a tagged release
   attaches them to the draft release.

## Open points (need the maintainer)

| # | Question | My lean |
|---|---|---|
| I1 | One OpenWrt release for both boxes (25.12.6, so the 7520 changes from 25.12.5), or each box its own? | One release: fewer builds, one kernel to reason about |
| I2 | `kmod-lantiq-ssc-pcm`: public, or reachable by CI through a deploy key? Without it the 7412 image has no PCM transport | Public (decided 2026-11-06) |
| I3 | Which minimal base: stock OpenWrt packages plus openDECTcell, or also the baresip and CardDAV packages in the default image? | Everything the standalone base needs; the cell needs less |
| I4 | A cell image (role=cell preset, no baresip) as a second variant? | Later; role is a setting now |
| I5 | CI minutes: an image job is one SDK build plus one ImageBuilder run per target (about 15 minutes each). Only on tags and on manual dispatch? | Yes: never on push |

## Risks

- A wrong image can brick a box that has no serial access (AGENTS.md: no
  soldering, no serial). The first images are flashed on the spare 7520 via
  the initramfs route, never the household one.
- The 7412's NAND layout and AVM's environment variables matter for the first
  flash from AVM firmware: `docs/models/` has the notes; an image must not
  change them.

## The transport repositories (I2, settled 2026-11-06)

`kmod-lantiq-ssc-pcm` (7412) and the two repositories of the 7362 SL are public, so the Packages workflow fetches them like any
other source and the 7412 packages and image carry their PCM transport. No deploy key or secret is needed for it.

