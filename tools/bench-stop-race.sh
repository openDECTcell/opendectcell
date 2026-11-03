#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Start the openDECTcell service on a box, stop it again after N seconds, and see
# whether the box still answers (docs/journals/bench-later.md, U8f). Runs on the PC.
#
#   bench-stop-race.sh HOST KNOWN_HOSTS DELAY [DELAY...]
#
# For each DELAY: stop the service and wait until the daemon is gone; start
# it; wait DELAY seconds; stop it; then ask the box every 5 s for up to 90 s.
# Exit 0: it answered after every stop. Exit 2: it went silent; the last
# step is printed and the box has to be power-cycled by hand (nothing else
# is attempted). Every line carries the PC's time; save the output to a file
# with the run's time in its name, and point the box's log at
# tools/syslog-sink.py first, so the log outlives a hang.
#
# Set RADIO_ON=1 to wait for the radio to be up (state.json "radio":"up")
# before counting DELAY, the "radio already on" case.
set -u
HOST=${1:?host}; KH=${2:?known_hosts file}; shift 2
[ $# -gt 0 ] || { echo "no delays" >&2; exit 1; }
SSH=(ssh -o UserKnownHostsFile="$KH" -o BatchMode=yes -o ConnectTimeout=5 "root@$HOST")

say() { printf '%s %s\n' "$(date -u +%H:%M:%S)" "$*"; }

answers() { "${SSH[@]}" true 2>/dev/null; }

wait_answer() {
	local i
	for i in $(seq 1 18); do
		answers && return 0
		sleep 5
	done
	return 1
}

for d in "$@"; do
	say "== delay $d s: stop, then start"
	"${SSH[@]}" '/etc/init.d/opendectcell stop; i=0; while pidof dect-stubd >/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done; ls /var/run/opendectcell/parked 2>&1' || true
	say "started"
	"${SSH[@]}" '/etc/init.d/opendectcell start' || true
	if [ "${RADIO_ON:-0}" = 1 ]; then
		for _ in $(seq 1 60); do
			"${SSH[@]}" 'grep -q "\"radio\":\"up\"" /var/run/opendectcell/state.json' 2>/dev/null && break
			sleep 3
		done
		say "radio up (or 180 s passed)"
	fi
	sleep "$d"
	say "stopping now"
	"${SSH[@]}" '/etc/init.d/opendectcell stop' &
	STOP=$!
	sleep 2
	if wait_answer; then
		wait "$STOP" 2>/dev/null
		say "answered after the stop"
		"${SSH[@]}" 'ls /var/run/opendectcell/parked 2>&1; logread -e opendectcell | tail -4' || true
	else
		say "HUNG: no answer for 90 s after the stop at delay $d s, RADIO_ON=${RADIO_ON:-0}"
		exit 2
	fi
done
say "done: the box answered after every stop"
