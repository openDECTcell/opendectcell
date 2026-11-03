#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The service's own start and stop (as a cell the controller starts the radio, as a base the daemon does), stop 1..6 s after the radio is up.
# usage: real-stop.sh START END [TAG]; every step is logged to the PC's syslog sink.
START=${1:-1}; END=${2:-100}; TAG=${3:-e3}
i=$((START-1))
while [ $i -lt $END ]; do
	i=$((i+1))
	logger -t $TAG "cycle $i start"
	/etc/init.d/opendectcell start >/dev/null 2>&1
	n=0
	while [ $n -lt 150 ]; do
		grep -qs '"radio":"up"' /var/run/opendectcell/cell.json /var/run/opendectcell/state.json 2>/dev/null && break
		sleep 1; n=$((n+1))
	done
	logger -t $TAG "cycle $i radio wait $n s"
	sleep $((1 + i % 6))
	logger -t $TAG "cycle $i stopping"
	/etc/init.d/opendectcell stop >/dev/null 2>&1
	logger -t $TAG "cycle $i stopped rc=$?"
	sleep 2
done
logger -t $TAG "finished $END"
