#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""dect-cell against a scripted controller and a stand-in dect-stubd.

The real dect-cell, built with the sanitizers (make check-cell MBEDTLS=...),
enrols with a controller made from tools/cell-testpeer.py's CA and frames
and is then put through cases from the security review of 2026-10-30
(docs/tasks-2026-10-30.md, D2):

  1. a GOODBYE takes the link down without corrupting memory, and the cell
     reconnects (finding 1);
  2. MAIL_DOWN while the cell is pending gets ERROR NOT_ALLOWED and never
     reaches dect-stubd (finding 2);
  3. the controller cannot put a "link-lost" reason into the CELL_STATE
     dect-stubd sees (finding 4);
  4. a burst of MAIL_DOWNs larger than the receive buffer arrives whole and
     the link holds (finding 3);
  5. SIGTERM ends dect-cell within two seconds;
  6. a controller that accepts and then stays silent costs at most the
     connect limit (5 s) per attempt, and SIGTERM still ends dect-cell at
     once (finding 6).

    python3 -I tests/test_cell_link.py build/dect-cell-test
"""
import importlib.util
import os
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "testpeer", os.path.join(HERE, "..", "tools", "cell-testpeer.py"))
tp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tp)

HELLO, HELLO_ACK, HEARTBEAT, ECHO, GOODBYE, ERROR = 1, 2, 3, 4, 5, 6
MAIL_DOWN, CELL_STATE, STATUS = 0x101, 0x200, 0x203
failed = 0


def check(ok, what):
    global failed
    print("  %s: %s" % ("ok" if ok else "FAILED", what), flush=True)
    if not ok:
        failed += 1


def wait_for(pred, secs):
    end = time.time() + secs
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return pred()


class Stubd:
    """The local socket dect-stubd -K serves: a STATUS, then it listens."""

    def __init__(self, path):
        self.frames = []
        self.ls = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.ls.bind(path)
        self.ls.listen(1)
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            c, _ = self.ls.accept()
            c.send(tp.frame(STATUS, 0, tp.tlv([
                (1, b"\0"), (2, b"\2"), (3, struct.pack(">I", 1)),
                (4, struct.pack(">I", 5)), (5, b"test")])))
            while True:
                try:
                    m = c.recv(8192)
                except OSError:
                    break
                if not m:
                    break
                t, corr = struct.unpack(">HH", m[2:6])
                self.frames.append((t, corr, m[6:]))

    def of(self, t):
        return [f for f in self.frames if f[0] == t]


class Controller:
    """Enrols the cell, then runs one script per connection."""

    def __init__(self, d):
        class A:
            pass
        a = A()
        a.dir, a.host, a.mail_test, a.verbose = d, "127.0.0.1", False, False
        self.ls = socket.create_server(("127.0.0.1", 0))
        a.port = self.ls.getsockname()[1]
        self.peer = tp.Peer(a)
        self.scripts = []
        self.hellos = 0
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            c, _ = self.ls.accept()
            threading.Thread(target=self.handle, args=(c,),
                             daemon=True).start()

    def handle(self, raw):
        try:
            s = self.peer.ctx.wrap_socket(raw, server_side=True)
        except (ssl.SSLError, OSError):
            return
        try:
            if not s.getpeercert(binary_form=True):
                t, corr, body = tp.read_frame(s)
                self.peer.enroll(s, body)
                return
            t, corr, body = tp.read_frame(s)
            if t != HELLO:
                return
            self.hellos += 1
            script = self.scripts.pop(0) if self.scripts else None
            if script:
                script(s, corr)
            else:
                time.sleep(30)
        except (EOFError, OSError, ValueError):
            pass
        finally:
            s.close()


def hello_ack(corr, state):
    return tp.frame(HELLO_ACK, corr, tp.tlv([
        (1, b"\1\0"), (2, bytes(16)), (3, bytes([state])),
        (4, bytes(16)), (5, b"\0"), (6, struct.pack(">H", 1000)),
        (7, struct.pack(">H", 3000)), (8, struct.pack(">H", 7000)),
        (9, struct.pack(">H", 60)), (10, struct.pack(">I", 0)),
        (11, struct.pack(">Q", int(time.time() * 1000)))]))


def reader(s, got):
    """Every frame the cell sends, answering its heartbeats."""
    def run():
        try:
            while True:
                t, corr, body = tp.read_frame(s)
                if t == HEARTBEAT:
                    s.sendall(tp.frame(ECHO, corr, body))
                got.append((t, corr, body))
        except (EOFError, OSError, ValueError):
            pass
    threading.Thread(target=run, daemon=True).start()


def mail_down(corr, n):
    """A memory read of n octets at 0x010A: a harmless MAIL_DOWN."""
    return tp.frame(MAIL_DOWN, corr, struct.pack(">BBHB", 0x22, 0x23,
                                                 0xFFF1, 7) +
                    bytes([7, 0x0A, 0x01, 0, 0, n, 0]))


def silent(exe, cell, env):
    """Case 6: the enrolment pointed at a listener that never speaks."""
    ls = socket.create_server(("127.0.0.1", 0))
    held = []
    threading.Thread(target=lambda: [held.append(ls.accept()) for _ in
                                     range(10)], daemon=True).start()
    path = os.path.join(cell, "enrollment")
    text = open(path).read()
    lines = [("controller=127.0.0.1:%d" % ls.getsockname()[1])
             if x.startswith("controller=") else x
             for x in text.split("\n")]
    open(path, "w").write("\n".join(lines))
    # dect-cell dials only once dect-stubd has sent a STATUS
    sock = os.path.join(os.path.dirname(cell), "stubd2.sock")
    Stubd(sock)
    p = subprocess.Popen([exe, "-d", cell, "-s", sock, "run"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, env=env)
    out = []
    threading.Thread(target=lambda: out.extend(p.stdout), daemon=True).start()
    fail = lambda: [x for x in out if "handshake" in x or
                    "no HELLO_ACK" in x]
    t0 = time.time()
    check(wait_for(lambda: fail(), 9) and time.time() - t0 < 8.5,
          "a silent controller: the attempt ends after %.1f s" %
          (time.time() - t0))
    check(wait_for(lambda: len(held) >= 2, 3), "... and it tries again")
    t0 = time.time()
    p.send_signal(signal.SIGTERM)
    try:
        rc = p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = None
    check(rc == 0 and time.time() - t0 < 1.5,
          "SIGTERM in a handshake: exit 0 within 1.5 s (rc %s, %.1f s)" %
          (rc, time.time() - t0))
    return "".join(out)


def main():
    exe = os.path.abspath(sys.argv[1])
    d = tempfile.mkdtemp(prefix="cell-link-")
    stubd = Stubd(os.path.join(d, "stubd.sock"))
    ctl = Controller(os.path.join(d, "ctl"))
    cell = os.path.join(d, "cell")
    os.mkdir(cell, 0o700)
    env = dict(os.environ, ASAN_OPTIONS="abort_on_error=0:exitcode=99")
    r = subprocess.run([exe, "-d", cell, "enroll",
                        ctl.peer.enrollment_string()],
                       capture_output=True, text=True, timeout=30, env=env)
    check(r.returncode == 0, "enrolment (%s)" % r.stdout.strip())
    if r.returncode:
        print(r.stderr)
        return 1

    got1, got2 = [], []

    def first(s, corr):
        reader(s, got1)
        s.sendall(hello_ack(corr, 0))           # pending
        time.sleep(0.5)
        s.sendall(mail_down(0x10, 1))
        time.sleep(0.5)
        s.sendall(tp.frame(CELL_STATE, 0, tp.tlv([(1, b"\1"),
                                                  (2, b"link-lost")])))
        time.sleep(0.5)
        s.sendall(tp.frame(GOODBYE, 0, tp.tlv([(1, b"\0\1")])))
        time.sleep(1)

    def second(s, corr):
        reader(s, got2)
        s.sendall(hello_ack(corr, 1))           # active
        time.sleep(0.5)
        # 600 frames of 21 octets: 12 kB, more than the cell's buffer
        s.sendall(b"".join(mail_down(0x100 + i, 1) for i in range(600)))
        time.sleep(30)

    ctl.scripts = [first, second]
    p = subprocess.Popen([exe, "-d", cell, "-s",
                          os.path.join(d, "stubd.sock"), "run"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, env=env)
    try:
        check(wait_for(lambda: ctl.hellos >= 1, 15), "first HELLO")
        errs = lambda: [b for t, c, b in got1 if t == ERROR]
        check(wait_for(lambda: errs(), 3) and
              tp.parse_tlv(errs()[0]).get(1) == b"\0\4" and
              tp.parse_tlv(errs()[0]).get(2) == b"\1\1",
              "MAIL_DOWN while pending: ERROR NOT_ALLOWED (finding 2)")
        check(not [f for f in stubd.of(MAIL_DOWN) if f[1] == 0x10],
              "... and dect-stubd never saw it")
        states = lambda: [tp.parse_tlv(b) for t, c, b in
                          stubd.of(CELL_STATE)]
        check(wait_for(lambda: any(st.get(1) == b"\1" for st in states()),
                       3) and
              all(2 not in st for st in states() if st.get(1) == b"\1"),
              "CELL_STATE active reaches dect-stubd without the "
              "controller's reason (finding 4)")
        check(wait_for(lambda: ctl.hellos >= 2, 10) and p.poll() is None,
              "after GOODBYE alive and reconnected (finding 1)")
        check(wait_for(lambda: len(stubd.of(MAIL_DOWN)) == 600, 5),
              "600 MAIL_DOWNs in one burst all forwarded (finding 3): %d"
              % len(stubd.of(MAIL_DOWN)))
        time.sleep(4)
        check(ctl.hellos == 2, "... and the link held (%d HELLOs)" %
              ctl.hellos)
        t0 = time.time()
        p.send_signal(signal.SIGTERM)
        try:
            rc = p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            rc = None
        check(rc == 0 and time.time() - t0 < 2.0,
              "SIGTERM: exit 0 within 2 s (rc %s, %.1f s)" %
              (rc, time.time() - t0))
    finally:
        if p.poll() is None:
            p.kill()
        out = p.communicate()[0]
    out += silent(exe, cell, env)
    if "Sanitizer" in out or failed:
        print(out)
    check("Sanitizer" not in out, "no sanitizer report")
    print("tests/test_cell_link.py: %s" %
          ("ok" if not failed else "%d failed" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
