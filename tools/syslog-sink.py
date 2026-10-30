#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Collect a box's remote syslog (UDP) into a file that outlives a hang.

    python3 syslog-sink.py [--port 5514] [--dir D] [--name-prefix P]

Writes <dir>/<prefix>-<UTC date and time of the start>.log, one line per
datagram with the PC's receive time in front, flushed at once. The name
carries the start time and an existing file is never opened again: a second
start in the same second gets a -2, -3... suffix. Prints the file name.

The box (OpenWrt) is pointed at it with
    uci set system.@system[0].log_ip=<this PC>
    uci set system.@system[0].log_port=5514
    uci set system.@system[0].log_proto=udp
    uci commit system; /etc/init.d/log restart
and put back with `uci delete system.@system[0].log_ip` (and log_port).
Datagrams are logged as they come; nothing in them is interpreted, and the
box's log lines never carry keys (the daemon masks them).
"""
import argparse
import datetime
import os
import socket


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=5514)
    ap.add_argument("--dir", default=".")
    ap.add_argument("--name-prefix", default="syslog")
    a = ap.parse_args()
    now = datetime.datetime.now(datetime.timezone.utc)
    base = os.path.join(a.dir, "%s-%s" % (a.name_prefix,
                                          now.strftime("%Y%m%d-%H%M%S")))
    n, path = 1, base + ".log"
    while True:
        try:
            f = open(path, "x", encoding="utf-8", errors="replace")
            break
        except FileExistsError:
            n += 1
            path = "%s-%d.log" % (base, n)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", a.port))
    print("listening on UDP %d, writing %s" % (a.port, path), flush=True)
    with f:
        while True:
            data, src = s.recvfrom(4096)
            t = datetime.datetime.now(datetime.timezone.utc)
            line = data.decode("utf-8", "replace").rstrip("\r\n")
            f.write("%s %s %s\n" % (t.strftime("%H:%M:%S.%f")[:-3], src[0],
                                    line))
            f.flush()
            os.fsync(f.fileno())


if __name__ == "__main__":
    main()
