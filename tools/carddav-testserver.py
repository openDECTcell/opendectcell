#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A small CardDAV server for testing opendectcell-carddav (openwrt/net/opendectcell/
files/carddav.uc) against: one address book in memory, HTTPS with its own
CA, Basic auth, and the four methods the sync uses (PROPFIND Depth 0/1,
GET, PUT, DELETE) with ETags, If-Match and If-None-Match as RFC 4918/6352
want them. Not a server for real use.

  tools/carddav-testserver.py --host 192.168.1.146 --port 8843 --dir /tmp/cdav

--dir gets ca.pem (give the box this as opendectcell.carddav.ca_file) and the
server's key and certificate, made with openssl on the first run. The
address book is https://HOST:PORT/addressbooks/test/, user "test", password
"test" (or --user / --password). Seed cards cover what the sync must cope
with: vCard 3.0 and 4.0 (tel: URIs), a company without N, a fax number, a
photo, folded lines. GET /_dump (with auth) returns every card as JSON, for
a test to check what the box wrote. --default-ns answers with the DAV:
namespace as default instead of a "d:" prefix, as some servers do;
--no-chunked reads a chunked request body as empty, as WSGI servers do.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import ssl
import subprocess
import sys
import threading
from xml.sax.saxutils import escape

BOOK = "/addressbooks/test/"

SEED = {
    "anna.vcf": (
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:anna\r\nN:Smith;Anna;;;\r\n"
        "FN:Anna Smith\r\nTEL;TYPE=CELL:+49 171 1111111\r\n"
        "TEL;TYPE=HOME,VOICE:030 2222222\r\nTEL;TYPE=WORK,FAX:030 2222299\r\n"
        "EMAIL:anna@example.org\r\nPHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRgABAQ\r\n"
        " EASABIAAD/2wBDAAEBAQEBAQEBAQEBAQEBAQE=\r\nEND:VCARD\r\n"),
    "luca.vcf": (
        "BEGIN:VCARD\r\nVERSION:4.0\r\nUID:luca\r\nN:Bordonado;Luca;;;\r\n"
        "FN:Luca Bordonado\r\nTEL;VALUE=uri;TYPE=\"work,voice\":tel:+39-02-3333333\r\n"
        "NOTE:keeps this note\r\nEND:VCARD\r\n"),
    "acme.vcf": (
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:acme\r\nFN:ACME Hardware\r\n"
        "ORG:ACME Hardware;Sales\r\nTEL;TYPE=WORK:0800 444 444\r\nEND:VCARD\r\n"),
    "müller.vcf": (
        "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:mueller\r\nN:Müller;Jörg;;;\r\n"
        "FN:Jörg Müller\r\nTEL:0049 89 5555555\r\nEND:VCARD\r\n"),
}


class Book:
    def __init__(self):
        self.lock = threading.Lock()
        self.cards = {}         # name -> (text, etag)
        self.ctag = 0
        for name, text in SEED.items():
            self.put(name, text)

    def put(self, name, text):
        etag = '"%s"' % hashlib.sha1(text.encode()).hexdigest()[:16]
        self.cards[name] = (text, etag)
        self.ctag += 1
        return etag


def make_certs(d, host):
    os.makedirs(d, exist_ok=True)
    ca_key, ca = os.path.join(d, "ca.key"), os.path.join(d, "ca.pem")
    key, csr = os.path.join(d, "server.key"), os.path.join(d, "server.csr")
    crt, ext = os.path.join(d, "server.pem"), os.path.join(d, "san.ext")
    if os.path.exists(crt):
        return crt, key
    run = lambda *a: subprocess.run(a, check=True, capture_output=True)
    run("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
        "ec_paramgen_curve:P-256", "-nodes", "-keyout", ca_key, "-out", ca,
        "-days", "30", "-subj", "/CN=opendectcell carddav test CA")
    run("openssl", "req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
        "-nodes", "-keyout", key, "-out", csr, "-subj", "/CN=" + host)
    with open(ext, "w") as f:
        san = ("IP:" if host.replace(".", "").isdigit() else "DNS:") + host
        f.write("subjectAltName=%s\nextendedKeyUsage=serverAuth\n" % san)
    run("openssl", "x509", "-req", "-in", csr, "-CA", ca, "-CAkey", ca_key,
        "-CAcreateserial", "-out", crt, "-days", "30", "-extfile", ext)
    return crt, key


def handler(book, user, password, default_ns, no_chunked=False):
    want = "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
    p = "" if default_ns else "d:"

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):
            sys.stdout.write("%s %s\n" % (self.command, fmt % a))
            sys.stdout.flush()

        def send(self, code, body=b"", ctype="text/plain", extra=None):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def body(self):
            # The stock uclient sends every request body chunked; nginx and
            # Apache take that, so this server does too -- unless
            # --no-chunked, where it reads such a body as empty, as a WSGI
            # server does
            if "chunked" in (self.headers.get("Transfer-Encoding") or ""):
                out = b""
                while True:
                    size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                    if not size:
                        self.rfile.readline()
                        return b"" if no_chunked else out
                    out += self.rfile.read(size)
                    self.rfile.readline()
            n = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(n) if n else b""

        def authed(self):
            if self.headers.get("Authorization") == want:
                return True
            self.body()
            self.send(401, b"auth", extra={"WWW-Authenticate": 'Basic realm="test"'})
            return False

        def card(self):
            path = self.path.split("?")[0]
            if not path.startswith(BOOK) or path == BOOK:
                return None
            from urllib.parse import unquote
            return unquote(path[len(BOOK):])

        def do_PROPFIND(self):
            if not self.authed():
                return
            self.body()
            depth = self.headers.get("Depth", "1")
            path = self.path.split("?")[0]
            ns = ('xmlns="DAV:"' if default_ns else 'xmlns:d="DAV:"')
            out = ['<?xml version="1.0" encoding="utf-8"?>',
                   '<%smultistatus %s xmlns:cs="http://calendarserver.org/ns/" '
                   'xmlns:card="urn:ietf:params:xml:ns:carddav">' % (p, ns)]
            with book.lock:
                if path == BOOK:
                    out.append(
                        '<%sresponse><%shref>%s</%shref><%spropstat><%sprop>'
                        '<%sresourcetype><%scollection/><card:addressbook/></%sresourcetype>'
                        '<cs:getctag>ctag-%d</cs:getctag></%sprop>'
                        '<%sstatus>HTTP/1.1 200 OK</%sstatus></%spropstat></%sresponse>'
                        % (p, p, BOOK, p, p, p, p, p, p, book.ctag, p, p, p, p, p))
                    if depth != "0":
                        from urllib.parse import quote
                        for name, (text, etag) in sorted(book.cards.items()):
                            out.append(
                                '<%sresponse><%shref>%s</%shref><%spropstat><%sprop>'
                                '<%sgetetag>%s</%sgetetag><%sresourcetype/></%sprop>'
                                '<%sstatus>HTTP/1.1 200 OK</%sstatus></%spropstat></%sresponse>'
                                % (p, p, BOOK + quote(name), p, p, p, p, escape(etag),
                                   p, p, p, p, p, p, p))
                else:
                    self.send(404)
                    return
            out.append('</%smultistatus>' % p)
            self.send(207, "\n".join(out).encode(), "application/xml; charset=utf-8")

        def do_GET(self):
            if not self.authed():
                return
            if self.path == "/_dump":
                with book.lock:
                    d = {k: v[0] for k, v in book.cards.items()}
                self.send(200, json.dumps(d, indent=1, ensure_ascii=False).encode(),
                          "application/json")
                return
            name = self.card()
            with book.lock:
                c = book.cards.get(name)
            if not c:
                self.send(404)
                return
            self.send(200, c[0].encode(), "text/vcard; charset=utf-8", {"ETag": c[1]})

        def do_PUT(self):
            if not self.authed():
                return
            data = self.body().decode()
            name = self.card()
            if not name or "BEGIN:VCARD" not in data:
                self.send(400)
                return
            with book.lock:
                c = book.cards.get(name)
                if self.headers.get("If-None-Match") == "*" and c:
                    self.send(412)
                    return
                im = self.headers.get("If-Match")
                if im and (not c or im != c[1]):
                    self.send(412)
                    return
                etag = book.put(name, data)
            self.send(201 if not c else 204, extra={"ETag": etag})

        def do_DELETE(self):
            if not self.authed():
                return
            self.body()
            name = self.card()
            with book.lock:
                c = book.cards.get(name)
                if not c:
                    self.send(404)
                    return
                im = self.headers.get("If-Match")
                if im and im != c[1]:
                    self.send(412)
                    return
                del book.cards[name]
                book.ctag += 1
            self.send(204)

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", required=True, help="address the box reaches")
    ap.add_argument("--port", type=int, default=8843)
    ap.add_argument("--dir", required=True, help="CA and certificate directory")
    ap.add_argument("--user", default="test")
    ap.add_argument("--password", default="test")
    ap.add_argument("--default-ns", action="store_true")
    ap.add_argument("--no-chunked", action="store_true",
                    help="read a chunked request body as empty, as WSGI servers do")
    a = ap.parse_args()
    crt, key = make_certs(a.dir, a.host)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    srv = http.server.ThreadingHTTPServer(
        ("0.0.0.0", a.port), handler(Book(), a.user, a.password, a.default_ns, a.no_chunked))
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("carddav test server: https://%s:%d%s (CA %s)"
          % (a.host, a.port, BOOK, os.path.join(a.dir, "ca.pem")), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
