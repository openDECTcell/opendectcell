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
     once (finding 6);
  7. media (cell-protocol.md §8, D3): MEDIA_OPEN reaches dect-stubd without
     keys or address; the stand-in's 20 ms frames come up as SRTP the
     controller side decrypts here with its own SRTP (Python's
     cryptography, not mbedTLS), G.722 with AEAD_AES_128_GCM at 20 ms and
     L16 with AES_CM_128_HMAC_SHA1_80 at 10 ms; packets down arrive as
     frames, a replayed or forged one does not; plain RTP is refused
     without -P; MEDIA_CLOSE ends the flow;
  8. renewal (§5.4, D4): enrolled with a certificate two thirds through its
     life, the cell asks for a new one at once, stores it with its new key
     and presents it on the next connection; a renewal cut short between
     the two renames is completed at the next start; an expired
     certificate stops the cell before it dials.

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
import datetime
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, hmac, serialization
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "testpeer", os.path.join(HERE, "..", "tools", "cell-testpeer.py"))
tp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tp)

HELLO, HELLO_ACK, HEARTBEAT, ECHO, GOODBYE, ERROR = 1, 2, 3, 4, 5, 6
RENEW_REQUEST, RENEW_RESPONSE = 0x210, 0x211
MAIL_DOWN, CELL_STATE, STATUS = 0x101, 0x200, 0x203
MEDIA_OPEN, MEDIA_OPEN_ACK, MEDIA_CLOSE, MEDIA_CLOSED = 0x300, 0x301, 0x302, \
    0x303
# HELLO_ACK tag 10: mail, media, SRTP, stats, flow, resume (not plain RTP)
CAPS = 0x01 | 0x02 | 0x04 | 0x10 | 0x20 | 0x40
failed = 0
SEND_LOCK = threading.Lock()


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
    """The local socket dect-stubd -K serves: a STATUS, then it listens.

    MEDIA_OPEN is taken (status 0) and MEDIA_CLOSE answered as dect-stubd
    does; on the audio socket a connection's hello names its session, and
    what it sends down is kept per session.
    """

    def __init__(self, path, audio=None):
        self.frames = []
        self.audio = {}            # media id -> connection
        self.hello = {}            # media id -> its hello
        self.down = {}             # media id -> frames received
        self.ls = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.ls.bind(path)
        self.ls.listen(1)
        threading.Thread(target=self.run, daemon=True).start()
        if audio:
            self.als = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            self.als.bind(audio)
            self.als.listen(4)
            threading.Thread(target=self.audio_run, daemon=True).start()

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
                d = tp.parse_tlv(m[6:]) if t in (MEDIA_OPEN,
                                                 MEDIA_CLOSE) else {}
                if t == MEDIA_OPEN:
                    c.send(tp.frame(MEDIA_OPEN_ACK, corr, tp.tlv([
                        (1, d[1]), (2, b"\0\0")])))
                elif t == MEDIA_CLOSE:
                    c.send(tp.frame(MEDIA_CLOSED, corr, tp.tlv([
                        (1, d[1]), (2, b"\0")])))
                    a = self.audio.pop(struct.unpack(">I", d[1])[0], None)
                    if a:
                        a.close()

    def audio_run(self):
        while True:
            c, _ = self.als.accept()
            hello = c.recv(64).decode()
            mid = int(hello.split()[0].split("=")[1])
            self.hello[mid] = hello
            self.audio[mid] = c
            self.down[mid] = []

            def rx(c=c, mid=mid):
                while True:
                    try:
                        m = c.recv(1024)
                    except OSError:
                        return
                    if not m:
                        return
                    self.down[mid].append(m)
            threading.Thread(target=rx, daemon=True).start()

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
        self.certs = []            # each connection's cell certificate
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
            self.certs.append(s.getpeercert(binary_form=True))
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
        (9, struct.pack(">H", 60)), (10, struct.pack(">I", CAPS)),
        (11, struct.pack(">Q", int(time.time() * 1000)))]))


def xor(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


def aes_cm(key, iv, n):
    return Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor().update(
        bytes(n))


class Srtp:
    """The controller's SRTP, written from RFC 3711 and RFC 7714 apart from
    dect_srtp.c: profile 1 AES_CM_128_HMAC_SHA1_80, 2 AEAD_AES_128_GCM."""

    def __init__(self, profile, key):
        sl = 14 if profile == 1 else 12
        mk, ms = key[:16], key[16:16 + sl]

        def prf(label, n):         # §4.3.3, the rate 0
            x = bytearray(ms + bytes(14 - sl))
            x[7] ^= label
            return aes_cm(mk, bytes(x) + b"\0\0", n)
        self.p, self.k, self.s = profile, prf(0, 16), prf(2, sl)
        self.a = prf(1, 20) if profile == 1 else None
        self.roc, self.last = 0, None

    def _iv(self, ssrc, seq):
        if self.p == 1:
            i = (self.roc << 16 | seq).to_bytes(6, "big")
            return xor(self.s + b"\0\0", bytes(4) + ssrc + i + b"\0\0")
        return xor(b"\0\0" + ssrc + self.roc.to_bytes(4, "big") +
                   seq.to_bytes(2, "big"), self.s)

    def _roc(self, seq):           # in order here: a wrap bumps the ROC
        if self.last is not None and seq < self.last and \
                self.last - seq > 0x8000:
            self.roc += 1
        self.last = seq

    def protect(self, pkt):
        seq = int.from_bytes(pkt[2:4], "big")
        self._roc(seq)
        iv = self._iv(pkt[8:12], seq)
        if self.p == 2:
            return pkt[:12] + AESGCM(self.k).encrypt(iv, pkt[12:], pkt[:12])
        enc = pkt[:12] + xor(pkt[12:], aes_cm(self.k, iv, len(pkt) - 12))
        h = hmac.HMAC(self.a, hashes.SHA1())
        h.update(enc + self.roc.to_bytes(4, "big"))
        return enc + h.finalize()[:10]

    def unprotect(self, pkt):
        seq = int.from_bytes(pkt[2:4], "big")
        self._roc(seq)
        iv = self._iv(pkt[8:12], seq)
        if self.p == 2:
            return pkt[:12] + AESGCM(self.k).decrypt(iv, pkt[12:], pkt[:12])
        h = hmac.HMAC(self.a, hashes.SHA1())
        h.update(pkt[:-10] + self.roc.to_bytes(4, "big"))
        if h.finalize()[:10] != pkt[-10:]:
            raise ValueError("tag")
        return pkt[:12] + xor(pkt[12:-10], aes_cm(self.k, iv,
                                                  len(pkt) - 22))


def reader(s, got, renew=None):
    """Every frame the cell sends, answering its heartbeats (and, given
    renew, its RENEW_REQUEST)."""
    def run():
        try:
            while True:
                t, corr, body = tp.read_frame(s)
                if t == HEARTBEAT:
                    with SEND_LOCK:     # media() writes from its thread
                        s.sendall(tp.frame(ECHO, corr, body))
                got.append((t, corr, body))
                if t == RENEW_REQUEST and renew:
                    with SEND_LOCK:
                        s.sendall(renew(body, corr))
        except (EOFError, OSError, ValueError):
            pass
    threading.Thread(target=run, daemon=True).start()


def mail_down(corr, n):
    """A memory read of n octets at 0x010A: a harmless MAIL_DOWN."""
    return tp.frame(MAIL_DOWN, corr, struct.pack(">BBHB", 0x22, 0x23,
                                                 0xFFF1, 7) +
                    bytes([7, 0x0A, 0x01, 0, 0, n, 0]))


def media(stubd, ctl_send, got):
    """Case 7: two sessions through dect-cell, both ways, then closed."""
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", 0))
    u.settimeout(2)
    addr = b"\4" + socket.inet_aton("127.0.0.1") + struct.pack(
        ">H", u.getsockname()[1])

    def open_(corr, mid, ch, fmt, pt, prof, ku, kd, ptime=None):
        items = [(1, struct.pack(">I", mid)), (2, bytes([ch])),
                 (3, bytes([fmt])), (5, bytes([pt])), (6, addr),
                 (7, struct.pack(">I", 0xA0 + ch)),
                 (8, struct.pack(">I", 0xB0 + ch)), (9, bytes([prof]))]
        if prof:
            items += [(10, ku), (11, kd)]
        if ptime:
            items.append((4, bytes([ptime])))
        ctl_send(tp.frame(MEDIA_OPEN, corr, tp.tlv(items)))

    def acked(corr):
        a = [tp.parse_tlv(b) for t, c, b in got
             if t == MEDIA_OPEN_ACK and c == corr]
        return struct.unpack(">H", a[0][2])[0] if a else None

    src = [None]                   # dect-cell's end of the latest flow

    def packets(n):
        out = []
        try:
            for _ in range(n):
                p, src[0] = u.recvfrom(2048)
                out.append(p)
        except socket.timeout:
            pass
        return out

    # A: G.722, 20 ms, AEAD_AES_128_GCM
    ka, kb = os.urandom(28), os.urandom(28)
    open_(0x51, 0x1001, 1, 6, 9, 2, ka, kb)
    check(wait_for(lambda: acked(0x51) is not None, 3) and acked(0x51) == 0,
          "media A: MEDIA_OPEN_ACK status 0")
    fwd = [tp.parse_tlv(b) for t, c, b in stubd.of(MEDIA_OPEN) if c == 0x51]
    check(fwd and sorted(fwd[0]) == [1, 2, 3, 4, 13] and fwd[0][3] == b"\6",
          "... dect-stubd got it without keys or address: tags %s" %
          (sorted(fwd[0]) if fwd else None))
    check(wait_for(lambda: 0x1001 in stubd.audio, 3) and
          stubd.hello[0x1001] == "media=4097 codec=g722",
          "... and the audio connection named it (%s)" %
          stubd.hello.get(0x1001))
    up = [bytes((k * 160 + j) & 0xFF for j in range(160)) for k in range(5)]
    for f in up:
        stubd.audio[0x1001].send(f)
        time.sleep(0.02)
    rx = Srtp(2, ka)
    pk = packets(5)
    try:
        plain = [rx.unprotect(p) for p in pk]
    except Exception:
        plain = []
    seqs = [struct.unpack(">H", p[2:4])[0] for p in plain]
    tss = [struct.unpack(">I", p[4:8])[0] for p in plain]
    check(len(plain) == 5 and [p[12:] for p in plain] == up and
          [p[1] for p in plain] == [0x89, 9, 9, 9, 9] and
          all(p[8:12] == b"\0\0\0\xa1" for p in plain) and
          all((seqs[i + 1] - seqs[i]) & 0xFFFF == 1 for i in range(4)) and
          all((tss[i + 1] - tss[i]) & 0xFFFFFFFF == 160 for i in range(4)),
          "... 5 frames up as SRTP (GCM) the controller decrypts: marker, "
          "pt 9, SSRC, seq +1, ts +160 (%d packets)" % len(plain))
    tx = Srtp(2, kb)
    down = [bytes((0x40 + k * 7 + j) & 0xFF for j in range(160))
            for k in range(3)]
    sent = []
    for k, f in enumerate(down):
        p = tx.protect(struct.pack(">BBHII", 0x80, 9, 100 + k, 1000 + 160 * k,
                                   0xB1) + f)
        sent.append(p)
        u.sendto(p, src[0])
        time.sleep(0.02)
    u.sendto(sent[1], src[0])                               # a replay
    forged = bytearray(tx.protect(struct.pack(">BBHII", 0x80, 9, 103, 1480,
                                              0xB1) + down[0]))
    forged[-1] ^= 1
    u.sendto(bytes(forged), src[0])
    time.sleep(0.5)
    check(stubd.down.get(0x1001) == down,
          "... 3 packets down arrive as 3 frames; the replay and the "
          "forged one do not (%d frames)" % len(stubd.down.get(0x1001, [])))

    # B: L16 8 kHz, 10 ms, AES_CM_128_HMAC_SHA1_80
    ka, kb = os.urandom(30), os.urandom(30)
    open_(0x52, 7, 2, 1, 96, 1, ka, kb, ptime=10)
    check(wait_for(lambda: acked(0x52) is not None, 3) and acked(0x52) == 0
          and wait_for(lambda: 7 in stubd.audio, 3) and
          stubd.hello[7] == "media=7 rate=8000",
          "media B: open, its audio connection says rate=8000")
    up = [struct.pack("<160h", *[(k * 1000 + j * 3) - 2000
                                 for j in range(160)]) for k in range(2)]
    for f in up:
        stubd.audio[7].send(f)
        time.sleep(0.02)
    rx = Srtp(1, ka)
    pk = [p for p in packets(4) if p[8:12] == b"\0\0\0\xa2"]
    try:
        plain = [rx.unprotect(p) for p in pk]
    except Exception:
        plain = []
    want = []
    for f in up:
        be = struct.pack(">160h", *struct.unpack("<160h", f))
        want += [be[:160], be[160:]]
    tss = [struct.unpack(">I", p[4:8])[0] for p in plain]
    check(len(plain) == 4 and [p[12:] for p in plain] == want and
          all((tss[i + 1] - tss[i]) & 0xFFFFFFFF == 80 for i in range(3)),
          "... 2 frames up as 4 SRTP packets (HMAC-SHA1), 10 ms each, "
          "in network order, ts +80 (%d packets)" % len(plain))
    tx = Srtp(1, kb)
    pcm = struct.pack(">160h", *range(-80, 80))
    for k in range(2):
        u.sendto(tx.protect(struct.pack(">BBHII", 0x80, 96, 500 + k,
                                        80 * k, 0xB2) +
                            pcm[k * 160:(k + 1) * 160]), src[0])
    time.sleep(0.3)
    check(stubd.down.get(7) == [struct.pack("=160h", *range(-80, 80))],
          "... two 10 ms packets down make one 20 ms frame in host order")

    # plain RTP: not without -P
    open_(0x53, 9, 1, 6, 9, 0, b"", b"")
    check(wait_for(lambda: acked(0x53) is not None, 3) and acked(0x53) == 4
          and not [f for f in stubd.of(MEDIA_OPEN) if f[1] == 0x53],
          "plain RTP refused (status 4) without -P, dect-stubd not asked")

    # MEDIA_CLOSE: answered with dect-stubd's MEDIA_CLOSED, the flow stops
    ctl_send(tp.frame(MEDIA_CLOSE, 0x54, tp.tlv([(1, struct.pack(
        ">I", 0x1001))])))
    check(wait_for(lambda: [1 for t, c, b in got
                            if t == MEDIA_CLOSED and c == 0x54], 3),
          "MEDIA_CLOSE: MEDIA_CLOSED comes back")
    u.close()


def issue(ca, pub, uri, eku, start, end):
    """A certificate from the test CA valid from start to end days from
    now (tools/cell-testpeer.py's CA.issue, with the dates free)."""
    now = datetime.datetime.now(datetime.timezone.utc)
    return (x509.CertificateBuilder().subject_name(x509.Name([]))
            .issuer_name(ca.cert.subject).public_key(pub)
            .serial_number(x509.random_serial_number())
            .not_valid_before(now + datetime.timedelta(days=start))
            .not_valid_after(now + datetime.timedelta(days=end))
            .add_extension(x509.SubjectAlternativeName(
                [x509.UniformResourceIdentifier(uri)]), True)
            .add_extension(x509.ExtendedKeyUsage([eku]), False)
            .sign(ca.key, hashes.SHA256()))


def enrol_dated(exe, ctl, cell, env, start, end):
    """Enrol cell with a certificate valid from start to end days."""
    ca = ctl.peer.ca
    plain = ca.issue
    ca.issue = lambda pub, uri, eku, days: issue(ca, pub, uri, eku, start,
                                                 end)
    try:
        return subprocess.run([exe, "-d", cell, "enroll",
                               ctl.peer.enrollment_string()],
                              capture_output=True, text=True, timeout=30,
                              env=env)
    finally:
        ca.issue = plain


def enrol_hidden(exe, ctl, d, env):
    """The string by @FILE and @- keeps it off the command line."""
    for how in ("file", "stdin", "missing"):
        cell = os.path.join(d, "cell-" + how)
        os.mkdir(cell, 0o700)
        s = ctl.peer.enrollment_string()
        if how == "file":
            path = os.path.join(d, "enroll.txt")
            with open(path, "w") as f:
                f.write(s + "\n")
            arg, inp = "@" + path, None
        elif how == "stdin":
            arg, inp = "@-", s + "\n"
        else:
            arg, inp = "@" + os.path.join(d, "no-such-file"), None
        r = subprocess.run([exe, "-d", cell, "enroll", arg], input=inp,
                           capture_output=True, text=True, timeout=30,
                           env=env)
        have = os.path.exists(os.path.join(cell, "enrollment"))
        check((r.returncode == 0 and have) if how != "missing"
              else (r.returncode != 0 and not have),
              "enrolment with the string from %s" % how)


def cell_uri(der):
    c = x509.load_der_x509_certificate(der)
    return c.extensions.get_extension_for_class(
        x509.SubjectAlternativeName).value.get_values_for_type(
        x509.UniformResourceIdentifier)[0]


def expired(exe, ctl, d, env):
    """Case 8, the end: an expired certificate stops the cell."""
    cell = os.path.join(d, "cell-expired")
    os.mkdir(cell, 0o700)
    r = enrol_dated(exe, ctl, cell, env, -91, -1)
    sock = os.path.join(d, "stubd3.sock")
    Stubd(sock)
    hellos = ctl.hellos
    p = subprocess.Popen([exe, "-d", cell, "-s", sock, "run"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, env=env)
    time.sleep(3)
    p.send_signal(signal.SIGTERM)
    out = p.communicate(timeout=5)[0]
    check(r.returncode == 0 and "has expired: enrol this cell again" in out
          and ctl.hellos == hellos,
          "an expired certificate: the cell says so and does not dial")
    return out


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
    stubd = Stubd(os.path.join(d, "stubd.sock"), os.path.join(d, "audio.sock"))
    ctl = Controller(os.path.join(d, "ctl"))
    cell = os.path.join(d, "cell")
    os.mkdir(cell, 0o700)
    env = dict(os.environ, ASAN_OPTIONS="abort_on_error=0:exitcode=99")
    enrol_hidden(exe, ctl, d, env)
    # 61 of 90 days gone: renewal is due at once (§5.4)
    r = enrol_dated(exe, ctl, cell, env, -61, 29)
    check(r.returncode == 0, "enrolment (%s)" % r.stdout.strip())
    old_cert = open(os.path.join(cell, "cert.der"), "rb").read()
    old_key = open(os.path.join(cell, "key.pem"), "rb").read()
    renewed = []

    def renew(body, corr):
        csr = x509.load_der_x509_csr(tp.parse_tlv(body)[1])
        if not csr.is_signature_valid:
            return tp.frame(RENEW_RESPONSE, corr, tp.tlv([(1, b"\0\2")]))
        c = ctl.peer.ca.issue(csr.public_key(), cell_uri(ctl.certs[-1]),
                              x509.oid.ExtendedKeyUsageOID.CLIENT_AUTH, 90)
        der = c.public_bytes(serialization.Encoding.DER)
        renewed.append(der)
        return tp.frame(RENEW_RESPONSE, corr, tp.tlv([(1, b"\0\0"),
                                                      (2, der)]))
    if r.returncode:
        print(r.stderr)
        return 1

    got1, got2 = [], []
    link2 = []

    def send2(b):
        with SEND_LOCK:
            link2[0].sendall(b)

    def first(s, corr):
        reader(s, got1, renew)
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
        link2.append(s)
        time.sleep(60)

    ctl.scripts = [first, second]
    p = subprocess.Popen([exe, "-d", cell, "-s",
                          os.path.join(d, "stubd.sock"), "-a",
                          os.path.join(d, "audio.sock"), "run"],
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
        check([1 for t, c, b in got1 if t == RENEW_REQUEST] and renewed and
              open(os.path.join(cell, "cert.der"), "rb").read() ==
              renewed[0] and
              open(os.path.join(cell, "key.pem"), "rb").read() != old_key
              and not os.path.exists(os.path.join(cell, "cert.der.new")),
              "renewal due at once: asked, stored with a new key (§5.4)")
        check(ctl.certs[0] == old_cert and len(ctl.certs) > 1 and
              renewed and ctl.certs[1] == renewed[0],
              "... and the next connection presents the new certificate")
        check(wait_for(lambda: len(stubd.of(MAIL_DOWN)) == 600, 5),
              "600 MAIL_DOWNs in one burst all forwarded (finding 3): %d"
              % len(stubd.of(MAIL_DOWN)))
        time.sleep(4)
        check(ctl.hellos == 2, "... and the link held (%d HELLOs)" %
              ctl.hellos)
        if link2:
            media(stubd, send2, got2)
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
    # a renewal cut short between its renames: new certificate, old key
    if renewed and old_key:
        new_key = open(os.path.join(cell, "key.pem"), "rb").read()
        open(os.path.join(cell, "key.pem.new"), "wb").write(new_key)
        open(os.path.join(cell, "key.pem"), "wb").write(old_key)
    s_out = silent(exe, cell, env)
    out += s_out
    check("renewal cut short is completed" in s_out and renewed and
          open(os.path.join(cell, "key.pem"), "rb").read() == new_key and
          not os.path.exists(os.path.join(cell, "key.pem.new")),
          "a renewal cut short is completed at the next start")
    out += expired(exe, ctl, d, env)
    if "Sanitizer" in out or failed:
        print(out)
    check("Sanitizer" not in out, "no sanitizer report")
    print("tests/test_cell_link.py: %s" %
          ("ok" if not failed else "%d failed" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
