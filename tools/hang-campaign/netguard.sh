#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# U8f diagnostics, runs ON the 7412: when the gateway stops answering for ~16 s,
# save what the kernel and the NIC look like (RAM: /tmp/hang-N), then try two
# remedies and say which one (if any) brought the network back. Writes only
# under /tmp. Started by the PC after every boot.
GW=${GW:-$(ip route | awk '/^default/ {print $3; exit}')}
n=0; fails=0
echo "netguard up $(date) $(cat /proc/uptime)" > /tmp/netguard.log
i=0
while :; do
	if ping -c1 -W1 $GW >/dev/null 2>&1; then
		fails=0
	else
		fails=$((fails + 1))
	fi
	i=$((i + 1))
	[ $((i % 5)) -eq 0 ] && echo "alive $(cut -d' ' -f1 /proc/uptime) fails=$fails" >> /tmp/netguard.log
	if [ $fails -ge 8 ]; then
		n=$((n + 1)); d=/tmp/hang-$n; mkdir -p $d
		{ date; cat /proc/uptime; cat /proc/loadavg; } > $d/when
		dmesg > $d/dmesg.txt 2>&1
		cat /proc/interrupts > $d/interrupts1.txt
		cat /proc/net/dev > $d/netdev1.txt
		ip link > $d/iplink.txt 2>&1
		ip addr > $d/ipaddr.txt 2>&1
		ip neigh > $d/neigh.txt 2>&1
		top -b -n1 | head -25 > $d/top.txt 2>&1
		ps > $d/ps.txt 2>&1
		sleep 3
		cat /proc/interrupts > $d/interrupts2.txt
		cat /proc/net/dev > $d/netdev2.txt
		echo "hang $n captured $(date)" >> /tmp/netguard.log
		# keep it across a power cycle: the overlay survives, /tmp does not
		e=/root/hangevid/$(date +%H%M%S)-$n; mkdir -p $e && cp -r $d/. $e/ 2>/dev/null
		cp /tmp/netguard.log /root/hangevid/netguard-$n.log 2>/dev/null
		sync
		# remedy 1: the CPU port down and up
		ip link set eth0 down 2>/dev/null; sleep 1; ip link set eth0 up 2>/dev/null; sleep 6
		if ping -c1 -W2 $GW >/dev/null 2>&1; then
			echo "remedy 1 (eth0 down/up) recovered it" > $d/result
			echo "hang $n: remedy 1 recovered" >> /tmp/netguard.log
			fails=0; continue
		fi
		# remedy 2: restart the network
		/etc/init.d/network restart >/dev/null 2>&1; sleep 20
		if ping -c1 -W2 $GW >/dev/null 2>&1; then
			echo "remedy 2 (network restart) recovered it" > $d/result
			echo "hang $n: remedy 2 recovered" >> /tmp/netguard.log
			fails=0; continue
		fi
		echo "no remedy brought the network back" > $d/result
		echo "hang $n: no remedy worked" >> /tmp/netguard.log
		fails=0
		sleep 30
	fi
	sleep 1
done
