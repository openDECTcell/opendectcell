#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run one of the on-box scripts of this directory on a 7412 and survive a hang: if the box goes silent for 40 s
# the box's power is cycled (a Tasmota socket) and the campaign goes on at the next cycle. Runs on the PC.
#
#   BOX=192.0.2.124 KH=known_hosts SINK=7412-syslog.log TAS=192.0.2.123 \
#     SSC_TEST=path/to/ssc-pcm-test ./run.sh TAG SCRIPT TOTAL
#
# TAG is the tag the script logs under (e1, e2, e3...), SCRIPT one of module-only.sh, chip-reset.sh,
# real-stop.sh, TOTAL the number of cycles. The box's log must go to tools/syslog-sink.py (SINK is the file it
# writes): the cycle numbers are read from it. The service's autostart should be off for module-only.sh.
# Prints one line per event; exit 0 when all cycles ran (hangs counted), 3 or 4 when the box was not reachable.
TAG=$1; SCRIPT=$2; TOTAL=$3
: "${BOX:?}" "${KH:?}" "${SINK:?}" "${TAS:?}"
SSH=(ssh -o UserKnownHostsFile="$KH" -o BatchMode=yes -o ConnectTimeout=5 root@"$BOX")
say() { printf '%s %s\n' "$(date -u +%H:%M:%S)" "$*"; }
setup() {
	[ -n "${SSC_TEST:-}" ] && scp -O -q -o UserKnownHostsFile="$KH" "$SSC_TEST" root@"$BOX":/tmp/ssc-pcm-test
	scp -O -q -o UserKnownHostsFile="$KH" "$SCRIPT" root@"$BOX":/tmp/onbox.sh
	"${SSH[@]}" 'chmod +x /tmp/ssc-pcm-test /tmp/onbox.sh 2>/dev/null; /etc/init.d/opendectcell stop >/dev/null 2>&1
		if [ -x /root/netguard.sh ] && ! { [ -f /tmp/netguard.pid ] && kill -0 $(cat /tmp/netguard.pid) 2>/dev/null; }; then
			setsid /root/netguard.sh </dev/null >/dev/null 2>&1 & echo $! > /tmp/netguard.pid; fi; true'
}
wait_up() { for _ in $(seq 1 90); do "${SSH[@]}" true 2>/dev/null && return 0; sleep 4; done; return 1; }
last_cycle() { grep -a "$TAG: cycle" "$SINK" | tail -1 | sed -n 's/.*cycle \([0-9]*\) .*/\1/p'; }
next=1; hangs=0; before=$(grep -a -c "$TAG: finished" "$SINK")
while [ "$next" -le "$TOTAL" ]; do
	setup || { say "setup failed"; exit 3; }
	"${SSH[@]}" "setsid /tmp/onbox.sh $next $TOTAL $TAG </dev/null >/dev/null 2>&1 &"
	say "started at cycle $next"
	quiet=0
	while :; do
		sleep 5
		if [ "$(grep -a -c "$TAG: finished" "$SINK")" -gt "$before" ]; then say "FINISHED $TOTAL cycles, hangs=$hangs"; exit 0; fi
		if ping -c1 -W1 "$BOX" >/dev/null 2>&1; then quiet=0; else quiet=$((quiet + 5)); fi
		[ $quiet -ge 40 ] && break
	done
	lc=$(last_cycle); hangs=$((hangs + 1))
	say "HANG $hangs: the box is silent, the last cycle logged is $lc"
	curl -s -m5 "http://$TAS/cm?cmnd=Power%20Off" >/dev/null; sleep 6; curl -s -m5 "http://$TAS/cm?cmnd=Power%20On" >/dev/null
	sleep 20; wait_up || { say "box did not come back"; exit 4; }
	say "box is back"
	next=$((lc + 1))
done
say "done, hangs=$hangs"
