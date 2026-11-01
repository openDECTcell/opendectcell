#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The chip firmware files of this box, for LuCI (Network > DECT > Firmware):
# what the board needs, what is there, and putting a file in or taking it
# out. docs/firmware-sourcing.md: the user extracts the two files on a PC
# (tools/dect-fw-extract.py) and uploads them; LuCI leaves each one in
# /tmp first, and `install` checks it here before it replaces anything.
#
#   firmware.sh status                  JSON: board, folder, and per file
#                                       {role, name, expected, sha256, state}
#   firmware.sh install ROLE FILE       FILE: /tmp/opendectcell-fw-upload.*
#   firmware.sh delete ROLE
#
# ROLE is `first` or `second`. The name a file gets comes from the board
# profile (or the uci options first/second, as the init script reads them),
# never from the caller, so nothing the browser sends picks a path.
# state: ok (the build the profile pins, or a known build under its own
# name), mismatch (not the build the profile pins), unknown (no pin, and not
# a build openDECTcell knows), wrongname (a known build under another build's
# name), missing. fw_allow_unverified lets `install` take
# the last two, as the init script lets them run.
#
# Answers one JSON object on stdout; exit 0, or 1 with {"error": "..."}.
# OPENDECTCELL_* variables below are for tests.

BOARDS=${OPENDECTCELL_BOARDS:-/usr/share/opendectcell/boards}
BOARD_NAME=${OPENDECTCELL_BOARD_NAME:-/tmp/sysinfo/board_name}
UPLOAD=${OPENDECTCELL_UPLOAD_DIR:-/tmp}
MAXSIZE=${OPENDECTCELL_FW_MAX:-400000}

# The builds openDECTcell knows, with the name each is kept under (the init
# script's KNOWN_FW and tools/dect-fw-extract.py have the same hashes and
# names; tests/test_firmware_sh.py keeps them equal). The name matters:
# dect-stubd picks chip RAM addresses by it.
KNOWN_FW="
0d855b5929f8b2ad0b8d8fb38065b32e27f9357cd44a2b4e60573b1b5b6da895 dectfw_firstlevel_441_slow.hex
9bdb332678f981d83ac31cf1b68c489ab9c979e2ac601d18b81bb802f16d0d20 dectfw_secondlevel_441-07.56.hex
7cac33c72b2222468f34033a15c35487aa35062265ab0013bb480068ebab7b31 dectfw_secondlevel_441.hex
72f8e27a0d01c301a05a8b373a71887af882a6cebcb1a30bd4a435580e1045f2 dectfw_firstlevel_441-06.88-7412.hex
4fabee65d07a3214f9083de81ae54089f01bbfccb4cdbb672f37487a09cdfaa6 dectfw_secondlevel_441-06.88-7412.hex
8aae3b5e3450fd2c9999e003068e786096c11d8ef53f7d47c25cebd057a797d3 dectfw_secondlevel_441-07.18-7362sl.hex
"

# the 7520 defaults of the init script, where no board profile is found
fw_firstlevel=dectfw_firstlevel_441_slow.hex
fw_secondlevel=dectfw_secondlevel_441-07.56.hex
fw_firstlevel_sha256=
fw_secondlevel_sha256=

die() {
	printf '{"error":"%s"}\n' "$1"
	exit 1
}

# uci option $2 of opendectcell.main, only the characters $3 allow, into $1
uci_get() {
	local v=
	command -v uci >/dev/null 2>&1 &&
		v=$(uci -q get "opendectcell.main.$2" 2>/dev/null)
	v=$(printf '%s' "$v" | tr -cd "$3")
	[ -n "$v" ] || v=$4
	eval "$1=\$v"
}

board=$(cat "$BOARD_NAME" 2>/dev/null | tr -cd 'A-Za-z0-9,._-')
if [ -n "$board" ] && [ -r "$BOARDS/$board" ]; then
	. "$BOARDS/$board"
fi

uci_get dir firmware_dir 'A-Za-z0-9/._-' /etc/opendectcell/firmware
uci_get first first 'A-Za-z0-9._-' "$fw_firstlevel"
uci_get second second 'A-Za-z0-9._-' "$fw_secondlevel"
uci_get unverified fw_allow_unverified '01' 0
[ "$unverified" = 1 ] || unverified=0

# the profile's pin counts only for the file it names
pin_first=$fw_firstlevel_sha256
pin_second=$fw_secondlevel_sha256
[ "$first" = "$fw_firstlevel" ] || pin_first=
[ "$second" = "$fw_secondlevel" ] || pin_second=

role_name() {
	case "$1" in
	first)  name=$first;  pin=$pin_first ;;
	second) name=$second; pin=$pin_second ;;
	*) die "role must be first or second" ;;
	esac
}

sha_of() {
	local s
	s=$(sha256sum "$1" 2>/dev/null) || return 1
	printf '%s' "${s%% *}"
}

# state of sum $1 against pin $2 and file name $3: ok, mismatch (not the
# pinned build), unknown (not a build openDECTcell knows) or wrongname (a known
# build kept under another build's name)
judge() {
	local known
	if [ -n "$2" ]; then
		[ "$1" = "$2" ] && echo ok || echo mismatch
		return
	fi
	known=$(printf '%s\n' "$KNOWN_FW" | while read -r h n; do
		[ "$h" = "$1" ] && echo "$n" && break
	done)
	if [ -z "$known" ]; then
		echo unknown
	elif [ "$known" = "$3" ]; then
		echo ok
	else
		echo wrongname
	fi
}

# the name a known build belongs under, for the message
name_of() {
	printf '%s\n' "$KNOWN_FW" | while read -r h n; do
		[ "$h" = "$1" ] && echo "$n" && break
	done
}

status_of() {
	local role=$1 f sum st
	role_name "$role"
	f=$dir/$name
	if [ -f "$f" ]; then
		sum=$(sha_of "$f")
		st=$(judge "$sum" "$pin" "$name")
	else
		sum=
		st=missing
	fi
	printf '{"role":"%s","name":"%s","expected":"%s","sha256":"%s","state":"%s"}' \
		"$role" "$name" "$pin" "$sum" "$st"
}

cmd_status() {
	printf '{"board":"%s","dir":"%s","unverified":%s,"files":[%s,%s]}\n' \
		"$board" "$dir" "$([ "$unverified" = 1 ] && echo true || echo false)" \
		"$(status_of first)" "$(status_of second)"
}

cmd_install() {
	local role=$1 src=$2 size sum st tmp
	role_name "$role"
	case "$src" in
	"$UPLOAD"/opendectcell-fw-upload.*) ;;
	*) die "not an upload file" ;;
	esac
	case "${src#"$UPLOAD"/}" in */*|*..*) die "not an upload file" ;; esac
	[ -f "$src" ] && [ ! -L "$src" ] || die "no uploaded file"
	size=$(wc -c < "$src" | tr -cd 0-9)
	if [ "${size:-0}" -le 0 ] || [ "$size" -gt "$MAXSIZE" ]; then
		rm -f "$src"
		die "the file is empty or larger than $MAXSIZE bytes; it is not a chip firmware file"
	fi
	sum=$(sha_of "$src")
	st=$(judge "$sum" "$pin" "$name")
	if [ "$st" != ok ] && [ "$unverified" != 1 ]; then
		rm -f "$src"
		if [ "$st" = mismatch ]; then
			die "this is not the firmware build $name this box needs (expected SHA-256 $pin, got $sum); extract it with tools/dect-fw-extract.py from the FRITZ!OS version in docs/firmware-sourcing.md"
		fi
		if [ "$st" = wrongname ]; then
			die "this is the build kept as $(name_of "$sum"), not as $name: set the First-stage or Second-stage firmware name to $(name_of "$sum") under Advanced, save, then upload it again"
		fi
		die "this is not a firmware build opendectcell knows (SHA-256 $sum); extract it with tools/dect-fw-extract.py from the FRITZ!OS version in docs/firmware-sourcing.md"
	fi
	mkdir -p "$dir" || { rm -f "$src"; die "cannot make $dir"; }
	tmp=$dir/.$name.new
	if ! cp "$src" "$tmp" || ! chmod 0600 "$tmp" ||
	   [ "$(sha_of "$tmp")" != "$sum" ] || ! mv "$tmp" "$dir/$name"; then
		rm -f "$src" "$tmp"
		die "could not write $dir/$name"
	fi
	rm -f "$src"
	printf '{"ok":true,"file":%s}\n' "$(status_of "$role")"
}

cmd_delete() {
	role_name "$1"
	[ -f "$dir/$name" ] || die "there is no $name to delete"
	rm -f "$dir/$name" || die "could not delete $dir/$name"
	printf '{"ok":true,"file":%s}\n' "$(status_of "$1")"
}

case "$1" in
status)  cmd_status ;;
install) cmd_install "$2" "$3" ;;
delete)  cmd_delete "$2" ;;
*) die "usage: firmware.sh status | install ROLE FILE | delete ROLE" ;;
esac
