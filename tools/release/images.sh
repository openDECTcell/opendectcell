#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds a flashable OpenWrt image with the openDECTcell packages inside, with the
# official ImageBuilder, from the .apk files tools/release/build.sh made:
#
#   tools/release/images.sh ipq40xx/generic APKDIR OUTDIR   # FRITZ!Box 7520/7530
#   tools/release/images.sh lantiq/xrx200 APKDIR OUTDIR     # FRITZ!Box 7412
#
# OPENWRT picks the release; it must be the one APKDIR's packages were built
# against (the kernel module only fits that kernel). The ImageBuilder is
# downloaded once into build/release (or $CACHE) and checked against
# OpenWrt's sha256sums. OUTDIR gets the sysupgrade image, the manifest and
# SHA256SUMS-<target>.images. No AVM material goes into an image: the chip firmware is
# extracted on the box (docs/firmware-sourcing.md).
#
# Needs the ImageBuilder's host tools: make gawk python3 file wget zstd rsync
# unzip tar gettext git bzip2 patch (what the Packages workflow installs).
set -eu

target=${1:?usage: images.sh TARGET/SUBTARGET APKDIR OUTDIR}
apks=${2:?usage: images.sh TARGET/SUBTARGET APKDIR OUTDIR}
out=${3:?usage: images.sh TARGET/SUBTARGET APKDIR OUTDIR}
root=$(cd "$(dirname "$0")/../.." && pwd)
openwrt=${OPENWRT:?set OPENWRT to the release the packages were built against}
cache=${CACHE:-$root/build/release}
base=https://downloads.openwrt.org/releases/$openwrt/targets/$target

# packages every image gets, and what is target-specific
pkgs="opendectcell opendectcell-cell opendectcell-baresip opendectcell-carddav luci luci-ssl luci-app-opendectcell"
case $target in
ipq40xx/*) profile=avm_fritzbox-7530; pkgs="$pkgs kmod-opendectcell-ipq40xx" ;;
lantiq/*)
	profile=avm_fritz7412; pkgs="$pkgs kmod-opendectcell-lantiq"
	# only if the build made it (its repository may not be reachable in CI)
	ls "$apks"/kmod-lantiq-ssc-pcm-*.apk >/dev/null 2>&1 && pkgs="$pkgs kmod-lantiq-ssc-pcm"
	;;
*) echo "no image for $target" >&2; exit 1 ;;
esac

mkdir -p "$cache" "$out"
slug=$(echo "$target" | tr / -)
archive=openwrt-imagebuilder-$openwrt-$slug.Linux-x86_64.tar.zst
if [ ! -s "$cache/$archive" ]; then
	wget -q -O "$cache/$archive" "$base/$archive"
fi
wget -q -O "$cache/sha256sums-$openwrt-$slug" "$base/sha256sums"
(cd "$cache" && grep " \*\?$archive\$" "sha256sums-$openwrt-$slug" | sed 's/ \*/  /' | sha256sum -c -)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar --zstd -xf "$cache/$archive" -C "$work"
ib=$work/openwrt-imagebuilder-$openwrt-$slug.Linux-x86_64

# The index lists packages as name-version.apk; build.sh names its files with
# the architecture added so the two targets' files do not collide.
for f in "$apks"/*.apk; do
	n=$(basename "$f" .apk)
	for arch in all "$slug" arm_cortex-a7_neon-vfpv4 mips_24kc; do
		n=${n%_"$arch"}
	done
	cp "$f" "$ib/packages/$n.apk"
done

# the project's public key, so the box's apk trusts later openDECTcell updates
files=$work/files
mkdir -p "$files/etc/apk/keys"
cp "$root/openwrt/opendectcell-signing-key.pem" "$files/etc/apk/keys/"

(cd "$ib" && make image PROFILE="$profile" PACKAGES="$pkgs" FILES="$files")

bin=$ib/bin/targets/$target
cp "$bin"/*sysupgrade* "$bin"/*.manifest "$out/"
(cd "$out" && sha256sum ./*sysupgrade* ./*.manifest | sed 's#\./##' >> SHA256SUMS-$slug.images)
echo "image(s) in $out:"
ls -l "$out"
