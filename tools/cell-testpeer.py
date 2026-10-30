#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A stand-in controller for testing a opendectcell cell (dect-cell, dect-stubd -K).

It speaks the cell side of opendectcell-controller docs/cell-protocol.md that a
first cell needs: its own cell CA, enrolment (§5, auto-approved), mutual
TLS 1.3 (§4), HELLO / HELLO_ACK (§6.1), heartbeats (§6.3), and it logs
MAIL_UP, FLOW and STATUS. It is not the controller: no DECT stack, no
database. Needs Python 3 and the `cryptography` package.

    python3 cell-testpeer.py --host 192.0.2.10 [--port 7300] [--dir D]
        prints an enrollment string, then serves
    python3 cell-testpeer.py ... --mail-test
        also sends harmless MAIL_DOWNs once a cell is active: a chip memory
        read of the radio switch (answered by 0xFFF1)
        and radio off (0x015B 00)

Mail parameters are never printed (§7.3): only direction, primitive and
length.
"""
import argparse
import base64
import datetime
import hashlib
import os
import secrets
import socket
import ssl
import struct
import sys
import threading
import time
import uuid

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

HELLO, HELLO_ACK, HEARTBEAT, ECHO, GOODBYE, ERROR, FLOW = 1, 2, 3, 4, 5, 6, 7
ENROLL_REQUEST, ENROLL_RESPONSE = 0x10, 0x11
MAIL_UP, MAIL_DOWN, MAIL_REJECT = 0x100, 0x101, 0x102
CELL_STATE, STATUS = 0x200, 0x203
NAMES = {HELLO: "HELLO", HEARTBEAT: "HEARTBEAT", ECHO: "HEARTBEAT_ECHO",
         GOODBYE: "GOODBYE", ERROR: "ERROR", FLOW: "FLOW",
         MAIL_UP: "MAIL_UP", MAIL_REJECT: "MAIL_REJECT", STATUS: "STATUS",
         0x301: "MEDIA_OPEN_ACK", 0x303: "MEDIA_CLOSED"}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def frame(t, corr, body=b""):
    return struct.pack(">HHH", 4 + len(body), t, corr) + body


def read_frame(s):
    hdr = b""
    while len(hdr) < 2:
        c = s.recv(2 - len(hdr))
        if not c:
            raise EOFError
        hdr += c
    n = struct.unpack(">H", hdr)[0]
    if n < 4 or n > 4100:
        raise ValueError("framing lost")
    rest = b""
    while len(rest) < n:
        c = s.recv(n - len(rest))
        if not c:
            raise EOFError
        rest += c
    t, corr = struct.unpack(">HH", rest[:4])
    return t, corr, rest[4:]


def tlv(items):
    out = b""
    for tag, v in items:
        out += struct.pack(">BH", tag, len(v)) + v
    return out


def parse_tlv(b):
    d, i = {}, 0
    while i < len(b):
        tag, n = struct.unpack(">BH", b[i:i + 3])
        d[tag] = b[i + 3:i + 3 + n]
        i += 3 + n
    return d


def spki_pin(cert):
    der = cert.public_key().public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo)
    return base64.urlsafe_b64encode(hashlib.sha256(der).digest()).rstrip(
        b"=").decode()


class CA:
    """The cell CA and the controller's server certificate (§4.1)."""

    def __init__(self, d):
        os.makedirs(d, exist_ok=True)
        self.d = d
        kp = os.path.join(d, "ca.key")
        if os.path.exists(kp):
            self.key = serialization.load_pem_private_key(
                open(kp, "rb").read(), None)
            self.cert = x509.load_pem_x509_certificate(
                open(os.path.join(d, "ca.pem"), "rb").read())
        else:
            self.key = ec.generate_private_key(ec.SECP256R1())
            name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,
                                                 "opendectcell test cell CA")])
            now = datetime.datetime.now(datetime.timezone.utc)
            self.cert = (x509.CertificateBuilder().subject_name(name)
                         .issuer_name(name).public_key(self.key.public_key())
                         .serial_number(x509.random_serial_number())
                         .not_valid_before(now - datetime.timedelta(days=1))
                         .not_valid_after(now + datetime.timedelta(days=365))
                         .add_extension(x509.BasicConstraints(True, None), True)
                         .sign(self.key, hashes.SHA256()))
            open(kp, "wb").write(self.key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption()))
            open(os.path.join(d, "ca.pem"), "wb").write(
                self.cert.public_bytes(serialization.Encoding.PEM))
        self.installation = uuid.uuid4()
        self.server_key = ec.generate_private_key(ec.SECP256R1())
        self.server = self.issue(self.server_key.public_key(),
                                 "urn:opendectcell:controller:%s" %
                                 self.installation,
                                 ExtendedKeyUsageOID.SERVER_AUTH, 30)
        for name, data in (("server.pem", self.server.public_bytes(
                serialization.Encoding.PEM) + self.cert.public_bytes(
                serialization.Encoding.PEM)),
                ("server.key", self.server_key.private_bytes(
                    serialization.Encoding.PEM,
                    serialization.PrivateFormat.PKCS8,
                    serialization.NoEncryption()))):
            open(os.path.join(d, name), "wb").write(data)

    def issue(self, pub, uri, eku, days):
        now = datetime.datetime.now(datetime.timezone.utc)
        return (x509.CertificateBuilder()
                .subject_name(x509.Name([]))
                .issuer_name(self.cert.subject).public_key(pub)
                .serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(minutes=5))
                .not_valid_after(now + datetime.timedelta(days=days))
                .add_extension(x509.SubjectAlternativeName(
                    [x509.UniformResourceIdentifier(uri)]), True)
                .add_extension(x509.ExtendedKeyUsage([eku]), False)
                .sign(self.key, hashes.SHA256()))


class Peer:
    def __init__(self, a):
        self.a = a
        self.ca = CA(a.dir)
        self.tokens = set()
        self.cells = {}            # cell uuid -> certificate fingerprints
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        self.ctx.load_cert_chain(os.path.join(a.dir, "server.pem"),
                                 os.path.join(a.dir, "server.key"))
        self.ctx.verify_mode = ssl.CERT_OPTIONAL
        self.ctx.load_verify_locations(os.path.join(a.dir, "ca.pem"))

    def enrollment_string(self):
        tok = secrets.token_bytes(20)
        self.tokens.add(tok)
        t = base64.b32encode(tok).decode().rstrip("=")
        return "opendectcell-enroll:v1?h=%s:%d&t=%s&ca=%s" % (
            self.a.host, self.a.port, t, spki_pin(self.ca.cert))

    def enroll(self, s, body):
        d = parse_tlv(body)
        tok = d.get(1, b"")
        if tok not in self.tokens:
            log("enrol: token rejected")
            s.sendall(frame(ENROLL_RESPONSE, 1, tlv([(1, b"\0\1")])))
            return
        self.tokens.discard(tok)
        csr = x509.load_der_x509_csr(d[2])
        if not csr.is_signature_valid:
            s.sendall(frame(ENROLL_RESPONSE, 1, tlv([(1, b"\0\2")])))
            return
        cid = uuid.uuid4()
        cert = self.ca.issue(csr.public_key(), "urn:opendectcell:cell:%s" % cid,
                             ExtendedKeyUsageOID.CLIENT_AUTH, 90)
        der = cert.public_bytes(serialization.Encoding.DER)
        self.cells[str(cid)] = hashlib.sha256(der).digest()
        log("enrol: cell %s (%s, %s, name %s) enrolled, active" % (
            cid, d.get(3, b"?").decode(), d.get(4, b"?").decode(),
            d.get(5, b"?").decode()))
        s.sendall(frame(ENROLL_RESPONSE, 1, tlv([
            (1, b"\0\0"), (2, cid.bytes), (3, der),
            (4, self.ca.cert.public_bytes(serialization.Encoding.DER)),
            (5, b"\1")])))

    def serve_cell(self, s, cid):
        t, corr, body = read_frame(s)
        if t != HELLO:
            log("cell %s: first frame 0x%04x, not HELLO" % (cid, t))
            return
        d = parse_tlv(body)
        log("HELLO: proto %d.%d, %s, stub %s, model %s, epoch %d, caps "
            "0x%x, radio %d, formats %s" % (
                d[1][0], d[1][1], d[2].decode(), d.get(3, b"-").decode(),
                d.get(4, b"-").decode(), struct.unpack(">I", d[6])[0],
                struct.unpack(">I", d[7])[0], d[14][0],
                list(d.get(12, b""))))
        ack = tlv([(1, b"\1\0"), (2, uuid.UUID(cid).bytes), (3, b"\1"),
                   (4, secrets.token_bytes(16)), (5, b"\0"),
                   (6, struct.pack(">H", 1000)), (7, struct.pack(">H", 3000)),
                   (8, struct.pack(">H", 7000)), (9, struct.pack(">H", 60)),
                   (10, d[7]), (11, struct.pack(">Q", int(time.time() * 1000)))])
        lock = threading.Lock()

        def send(t2, c2, b2=b""):
            with lock:
                s.sendall(frame(t2, c2, b2))

        send(HELLO_ACK, corr, ack)
        alive = [True]
        counts = {}

        def ticker():
            n = 0
            while alive[0]:
                time.sleep(1)
                n += 1
                try:
                    send(HEARTBEAT, 0, struct.pack(">I", n))
                    if self.a.mail_test and n == 3:
                        # read the radio switch, 1 octet at 0x010A, as
                        # dect-stubd's park check does (0x22 -> 0x23)
                        send(MAIL_DOWN, 0x0101, struct.pack(
                            ">BBHB", 0x22, 0x23, 0xFFF1, 7) +
                            bytes([7, 0x0A, 0x01, 0, 0, 1, 0]))
                        send(MAIL_DOWN, 0x0102, struct.pack(
                            ">BBHB", 0x22, 0x02, 0x015B, 1) + b"\0")
                        log("sent MAIL_DOWN 0xFFF1 (memory read) and "
                            "0x015B 00 (radio off)")
                    if n % 10 == 0:
                        log("cell %s: %s" % (cid, ", ".join(
                            "%s x%d" % (k, v) for k, v in
                            sorted(counts.items()))))
                except OSError:
                    return

        threading.Thread(target=ticker, daemon=True).start()
        try:
            while True:
                t, corr, body = read_frame(s)
                name = NAMES.get(t, "0x%04x" % t)
                if t == HEARTBEAT:
                    send(ECHO, corr, body)
                elif t == MAIL_UP:
                    prim, plen = struct.unpack(">HB", body[6:9])
                    key = "MAIL_UP 0x%04x" % prim
                    counts[key] = counts.get(key, 0) + 1
                    if self.a.verbose or prim == 0xFFF1:
                        log("MAIL_UP 0x%04x, %d octets" % (prim, plen))
                elif t == ECHO:
                    pass
                elif t == STATUS:
                    d = parse_tlv(body)
                    log("STATUS: radio %d, chip %d, epoch %d, uptime %d s, %s" %
                        (d[1][0], d[2][0], struct.unpack(">I", d[3])[0],
                         struct.unpack(">I", d[4])[0],
                         d.get(5, b"").decode()))
                elif t == MAIL_REJECT:
                    rc, why = struct.unpack(">HB", body)
                    log("MAIL_REJECT corr 0x%04x reason %d" % (rc, why))
                else:
                    log("%s corr %d, %d octets" % (name, corr, len(body)))
        finally:
            alive[0] = False

    def handle(self, raw, addr):
        try:
            s = self.ctx.wrap_socket(raw, server_side=True)
        except (ssl.SSLError, OSError) as e:
            log("%s: TLS failed: %s" % (addr[0], e))
            raw.close()
            return
        try:
            der = s.getpeercert(binary_form=True)
            if not der:
                t, corr, body = read_frame(s)
                if t == ENROLL_REQUEST:
                    self.enroll(s, body)
                else:
                    log("%s: no certificate and no ENROLL_REQUEST" % addr[0])
                return
            cert = x509.load_der_x509_certificate(der)
            uri = cert.extensions.get_extension_for_class(
                x509.SubjectAlternativeName).value.get_values_for_type(
                x509.UniformResourceIdentifier)[0]
            cid = uri.rsplit(":", 1)[1]
            if self.cells.get(cid) != hashlib.sha256(der).digest():
                log("%s: cell %s unknown to this run" % (addr[0], cid))
                s.sendall(frame(GOODBYE, 0, tlv([(1, b"\0\7")])))
                return
            log("cell %s connected from %s" % (cid, addr[0]))
            self.serve_cell(s, cid)
        except (EOFError, OSError, ValueError, KeyError) as e:
            log("%s: link closed (%s)" % (addr[0], e or type(e).__name__))
        finally:
            s.close()

    def serve(self):
        ls = socket.create_server(("0.0.0.0", self.a.port), reuse_port=True)
        log("listening on %d; enrol with:\n  dect-cell enroll '%s'" % (
            self.a.port, self.enrollment_string()))
        while True:
            c, addr = ls.accept()
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=self.handle, args=(c, addr),
                             daemon=True).start()


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--host", required=True,
                   help="this machine's address, as the cell reaches it")
    p.add_argument("--port", type=int, default=7300)
    p.add_argument("--dir", default="cell-testpeer",
                   help="the test CA and certificates (default ./cell-testpeer)")
    p.add_argument("--mail-test", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args()
    try:
        Peer(a).serve()
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
