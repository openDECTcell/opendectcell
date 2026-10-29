#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# After a release tag is pushed: write the tag's source archive hash into
# the feed (openwrt/README.md, "Versions and the source hash").
#
#   tools/release/feed-hash.sh          # the version in the Makefile
#
# Downloads exactly what the feed's PKG_SOURCE_URL names. While the
# repository is private, GitHub wants a token; `gh auth token` gives one.
# Then commit the Makefile ("openwrt: openDECTcell VERSION source hash").
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
mk=$root/openwrt/net/opendectcell/Makefile
version=$(sed -n 's/^PKG_VERSION:=//p' "$mk")
url=$(sed -n 's/^PKG_SOURCE_URL:=//p' "$mk" |
	sed "s/\$(PKG_VERSION)/$version/g; s/?\$//")

git -C "$root" rev-parse -q --verify "refs/tags/v$version" >/dev/null || {
	echo "no tag v$version here: tag and push the release first" >&2
	exit 1
}

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
token=${GITHUB_TOKEN:-$(gh auth token 2>/dev/null || true)}
curl -fsSL ${token:+-H "Authorization: token $token"} -o "$tmp" "$url"
hash=$(sha256sum "$tmp" | cut -d' ' -f1)
# the archive must unpack to opendectcell-VERSION/, as the feed expects
tar -tzf "$tmp" | head -n 1 | grep -qx "opendectcell-$version/" || {
	echo "$url does not unpack to opendectcell-$version/" >&2
	exit 1
}
sed -i "s/^PKG_HASH:=.*/PKG_HASH:=$hash/" "$mk"
echo "opendectcell $version: PKG_HASH:=$hash"
