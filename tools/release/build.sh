#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the openDECTcell OpenWrt packages for one target with the official
# OpenWrt SDK, the same way here and in the Packages workflow:
#
#   tools/release/build.sh ipq40xx/generic OUTDIR   # FRITZ!Box 7520/7530
#   tools/release/build.sh lantiq/xrx200 OUTDIR     # FRITZ!Box 7412
#
# The SDK is downloaded once into build/release (or $CACHE) and checked
# against OpenWrt's sha256sums. OUTDIR gets the .apk files, named with
# their architecture so the two targets' files do not collide.
#
# OPENWRT=25.12.5 picks the OpenWrt release; TAG=v0.3.0 makes the build
# fail unless the tag matches openwrt/version.mk.
set -eu

target=${1:?usage: build.sh TARGET/SUBTARGET OUTDIR}
out=${2:?usage: build.sh TARGET/SUBTARGET OUTDIR}
root=$(cd "$(dirname "$0")/../.." && pwd)
openwrt=${OPENWRT:-25.12.5}
cache=${CACHE:-$root/build/release}
base=https://downloads.openwrt.org/releases/$openwrt/targets/$target

version=$(sed -n 's/^OPENDECTCELL_VERSION:=//p' "$root/openwrt/version.mk")
if [ -n "${TAG:-}" ] && [ "$TAG" != "v$version" ]; then
	echo "tag $TAG does not match openwrt/version.mk ($version)" >&2
	exit 1
fi

case $target in
ipq40xx/*) kmod=kmod-opendectcell-ipq40xx ;;
lantiq/*) kmod=kmod-opendectcell-lantiq ;;
*) echo "no openDECTcell kernel module for $target" >&2; exit 1 ;;
esac
packages="opendectcell $kmod luci-app-opendectcell opendectcell-baresip"

# The SDK, checked against the release's sha256sums
mkdir -p "$cache" "$out"
out=$(cd "$out" && pwd)
cd "$cache"
curl -fsSL -o sha256sums.tmp "$base/sha256sums"
line=$(grep -E '\*openwrt-sdk-.*\.tar\.zst$' sha256sums.tmp)
rm -f sha256sums.tmp
tarball=${line##*\*}
[ -f "$tarball" ] || curl -fL -o "$tarball" "$base/$tarball"
echo "$line" | sha256sum -c -
sdk=$cache/${tarball%.tar.zst}
[ -d "$sdk" ] || tar --zstd -xf "$tarball"
cd "$sdk"

# Signing. apk checks the signature of every package file it is handed
# (LuCI's Upload Package runs a plain apk add), but OpenWrt signs only the
# feed indexes; so with SIGN_KEY, the project's private key, each package
# is signed as apk mkpkg makes it. Users trust the public half once:
# openwrt/opendectcell-signing-key.pem.
if [ -n "${SIGN_KEY:-}" ]; then
	cp "$SIGN_KEY" private-key.pem
	openssl ec -in private-key.pem -pubout -out public-key.pem 2>/dev/null
	pub=$root/openwrt/opendectcell-signing-key.pem
	if [ -f "$pub" ] && ! cmp -s public-key.pem "$pub"; then
		echo "SIGN_KEY is not the key of $pub" >&2
		exit 1
	fi
	grep -q -- '--sign-key' include/package-pack.mk ||
		sed -i 's|^\(\t  --output "\$\$(PACK_\$(1))"\)$|\t  --sign-key "$(BUILD_KEY_APK_SEC)" \\\n\1|' \
			include/package-pack.mk
	grep -q -- '--sign-key' include/package-pack.mk ||
		{ echo "could not add signing to this SDK" >&2; exit 1; }
fi

# Our packages, and the feeds with what they build on: the SDK brings
# only the toolchain, so even zlib and OpenSSL come from the base feed;
# baresip from telephony, spandsp's libtiff from packages, luci-base
for p in $packages; do
	ln -sfn "$root/openwrt/package/$p" "package/$p"
done
./scripts/feeds update base packages telephony luci
./scripts/feeds install -a -p base
./scripts/feeds install -a -p packages
./scripts/feeds install -a -p telephony
./scripts/feeds install luci-base

# Only ours and what they depend on: the SDK's default builds every
# package it knows, all of baresip's modules among them
{
	echo '# CONFIG_ALL is not set'
	echo '# CONFIG_ALL_NONSHARED is not set'
	echo '# CONFIG_ALL_KMODS is not set'
	for p in $packages; do
		echo "CONFIG_PACKAGE_$p=m"
	done
} > .config
make defconfig >/dev/null

jobs=$(nproc 2>/dev/null || echo 2)
for p in $packages; do
	make "package/$p/clean" >/dev/null
	make "package/$p/compile" -j"$jobs" V=s ||
		{ echo "build of $p failed" >&2; exit 1; }
done

# Collect them, the architecture in the file name
for p in $packages; do
	f=$(find bin -name "$p-[0-9]*.apk" | sort | tail -n 1)
	[ -n "$f" ] || { echo "no package file for $p" >&2; exit 1; }
	case $f in
	bin/targets/*) arch=$(echo "$target" | tr / -) ;;	# kernel module
	*) arch=$(basename "$(dirname "$(dirname "$f")")") ;;	# bin/packages/ARCH/FEED
	esac
	case $p in
	luci-app-*) arch=all ;;
	esac
	name=$(basename "$f" .apk)
	cp "$f" "$out/${name}_$arch.apk"
	echo "built $out/${name}_$arch.apk"
done

# With a key: every file must pass apk's own check against its public half
if [ -n "${SIGN_KEY:-}" ]; then
	rm -rf tmp/opendectcell-keys
	mkdir -p tmp/opendectcell-keys
	cp public-key.pem tmp/opendectcell-keys/opendectcell.pem
	staging_dir/host/bin/apk --keys-dir tmp/opendectcell-keys verify "$out"/*.apk
fi
