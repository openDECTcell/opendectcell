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
# fail unless the tag matches PKG_VERSION in openwrt/net/opendectcell/Makefile.
#
# The packages build the way an OpenWrt feed builds them (openwrt/README.md):
# the feed is a copy of openwrt/, and the openDECTcell source is an archive of
# the checkout put where the download would land, its hash written into the
# copy's PKG_HASH. Tracked files only, with their uncommitted changes
# (git stash create); a new file needs a `git add` first.
# kmod-lantiq-ssc-pcm and kmod-lantiq-usif-uart download from their own
# repositories; where one is not reachable (a private repository in CI) the
# lantiq build goes without it.
set -eu

target=${1:?usage: build.sh TARGET/SUBTARGET OUTDIR}
out=${2:?usage: build.sh TARGET/SUBTARGET OUTDIR}
root=$(cd "$(dirname "$0")/../.." && pwd)
openwrt=${OPENWRT:-25.12.5}
cache=${CACHE:-$root/build/release}
base=https://downloads.openwrt.org/releases/$openwrt/targets/$target

version=$(sed -n 's/^PKG_VERSION:=//p' "$root/openwrt/net/opendectcell/Makefile")
if [ -n "${TAG:-}" ] && [ "$TAG" != "v$version" ]; then
	echo "tag $TAG does not match openwrt/net/opendectcell/Makefile ($version)" >&2
	exit 1
fi

# source packages to compile, and the binary packages they make
sources="opendectcell luci-app-opendectcell ucode-mod-opendectcell-uclient"
packages="opendectcell opendectcell-baresip opendectcell-cell opendectcell-carddav ucode-mod-opendectcell-uclient luci-app-opendectcell"
case $target in
ipq40xx/*) packages="$packages kmod-opendectcell-ipq40xx" ;;
lantiq/*) packages="$packages kmod-opendectcell-lantiq" ;;
*) echo "no openDECTcell kernel module for $target" >&2; exit 1 ;;
esac
# The Lantiq transports live in their own repositories: lantiq-ssc-pcm
# (7412's PCM bus) and lantiq-usif-uart (7362 SL's firmware-load UART).
dropped=""
case $target in
lantiq/*)
	for t in lantiq-ssc-pcm lantiq-usif-uart; do
		url=$(sed -n 's/^PKG_SOURCE_URL:=//p' \
			"$root/openwrt/kernel/$t/Makefile")
		if GIT_TERMINAL_PROMPT=0 git ls-remote "$url" >/dev/null 2>&1; then
			sources="$sources $t"
			packages="$packages kmod-$t"
		else
			echo "warning: $url not reachable: no kmod-$t," \
				"and opendectcell's dependency on it is dropped" >&2
			dropped="$dropped $t"
		fi
	done
	;;
esac

# The feed and the source archive, from the checkout: outside any git
# work tree, or LuCI's version lookup would ask this repository's history
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
tree=$(cd "$root" && git stash create)
tree=${tree:-HEAD}
(cd "$root" && git archive "$tree" openwrt) | tar -x -C "$stage"
case $target in
lantiq/*) for t in $dropped; do rm -rf "$stage/openwrt/kernel/$t"; done ;;
esac
(cd "$root" && git archive --format=tar --prefix="opendectcell-$version/" "$tree") |
	gzip -n > "$stage/opendectcell-$version.tar.gz"
hash=$(sha256sum "$stage/opendectcell-$version.tar.gz" | cut -d' ' -f1)
sed -i "s/^PKG_HASH:=.*/PKG_HASH:=$hash/" "$stage/openwrt/net/opendectcell/Makefile"

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

# Our feed, and the feeds with what it builds on: the SDK brings only
# the toolchain, so even zlib and OpenSSL come from the base feed;
# baresip from telephony, spandsp's libtiff from packages, luci-base and
# luci.mk from luci. (Before the feed layout, build.sh linked each
# package into package/: those links go.)
old="package/opendectcell package/opendectcell-baresip package/kmod-opendectcell-ipq40xx
	package/kmod-opendectcell-lantiq package/luci-app-opendectcell"
for l in $old; do
	if [ -L "$l" ]; then
		rm -f $old
		rm -rf tmp	# its package index still names them as core packages
		break
	fi
done
grep -v '^src-link opendectcell ' feeds.conf.default > feeds.conf
echo "src-link opendectcell $stage/openwrt" >> feeds.conf
mkdir -p dl
cp "$stage/opendectcell-$version.tar.gz" dl/
./scripts/feeds update base packages telephony luci opendectcell
./scripts/feeds install -a -p base
./scripts/feeds install -a -p packages
./scripts/feeds install -a -p telephony
./scripts/feeds install luci-base
./scripts/feeds install -a -p opendectcell

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

# LuCI numbers an app by the git history of its directory, which the
# staged feed has none of: ours carries the openDECTcell version instead.
jobs=$(nproc 2>/dev/null || echo 2)
for p in $sources; do
	extra=
	[ "$p" = luci-app-opendectcell ] && extra="PKG_SRC_VERSION=$version"
	make "package/$p/clean" >/dev/null
	make "package/$p/compile" -j"$jobs" V=s $extra ||
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
	luci-app-*|opendectcell-carddav) arch=all ;;
	esac
	name=$(basename "$f" .apk)
	cp "$f" "$out/${name}_$arch.apk"
	echo "built $out/${name}_$arch.apk"
done

# With a key: every file must pass apk's own check against its public half
if [ -n "${SIGN_KEY:-}" ]; then
	# (apk takes --keys-dir relative to the root: an absolute path)
	rm -rf "$sdk/tmp/opendectcell-keys"
	mkdir -p "$sdk/tmp/opendectcell-keys"
	cp public-key.pem "$sdk/tmp/opendectcell-keys/opendectcell.pem"
	staging_dir/host/bin/apk --keys-dir "$sdk/tmp/opendectcell-keys" verify "$out"/*.apk
fi
