#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# the module alone. The chip is held in reset (no clock), the engine is started and stopped.
# usage: START END [TAG] (cycle numbers); every cycle is logged to the PC's syslog sink.
START=${1:-1}; END=${2:-300}; TAG=${3:-e2}
[ -d /sys/class/gpio/gpio545 ] || echo 545 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio545/direction; echo 0 > /sys/class/gpio/gpio545/value
i=$((START-1))
while [ $i -lt $END ]; do
	i=$((i+1))
	logger -t $TAG "cycle $i start"
	/tmp/ssc-pcm-test slots=36 hold=2 >/dev/null 2>&1
	logger -t $TAG "cycle $i done rc=$?"
done
logger -t $TAG "finished $END"
