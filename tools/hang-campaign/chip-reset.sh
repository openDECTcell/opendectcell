#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# the chip loses its reset (GPIO 545 low) while the daemon's SSC engine is armed, then the service is stopped.
# usage: START END [TAG]; every step is logged to the PC's syslog sink.
START=${1:-1}; END=${2:-40}; TAG=${3:-e1}
i=$((START-1))
while [ $i -lt $END ]; do
	i=$((i+1))
	logger -t $TAG "cycle $i start"
	[ -d /sys/class/gpio/gpio545 ] && echo 545 > /sys/class/gpio/unexport
	/etc/init.d/opendectcell start >/dev/null 2>&1
	n=0
	while [ $n -lt 150 ]; do
		grep -qs '"radio":"up"' /var/run/opendectcell/cell.json /var/run/opendectcell/state.json 2>/dev/null && break
		sleep 1; n=$((n+1))
	done
	logger -t $TAG "cycle $i radio wait $n s"
	sleep $((1 + i % 4))
	logger -t $TAG "cycle $i holding the chip in reset"
	echo 545 > /sys/class/gpio/export 2>/dev/null
	echo out > /sys/class/gpio/gpio545/direction
	echo 0 > /sys/class/gpio/gpio545/value
	sleep 3
	logger -t $TAG "cycle $i stopping"
	/etc/init.d/opendectcell stop >/dev/null 2>&1
	logger -t $TAG "cycle $i stopped rc=$?"
	sleep 2
done
logger -t $TAG "finished $END"
