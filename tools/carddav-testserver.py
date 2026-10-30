#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A small CardDAV server for testing opendectcell-carddav (openwrt/net/opendectcell/
files/carddav.uc) against: address books in memory, HTTPS with its own CA,
Basic auth, discovery as RFC 6764 and RFC 6352 describe it, and the methods
the sync uses (PROPFIND Depth 0/1, REPORT addressbook-multiget, GET, PUT,
DELETE) with ETags, If-Match and If-None-Match. Not a server for real use.

  tools/carddav-testserver.py --host 192.168.1.146 --port 8843 --dir /tmp/cdav

--dir gets ca.pem (give the box this as opendectcell.carddav.ca_file) and the
server's key and certificate, made with openssl on the first run. User
"test", password "test" (or --user / --password).

  /.well-known/carddav        301 to /
  /                           current-user-principal /principals/test/
  /principals/test/           addressbook-home-set /addressbooks/test/
  /addressbooks/test/         the home: two books, "Contacts" and "Work"
  /addressbooks/test/BOOK/    a book; its cards are BOOK/NAME.vcf

The seed cards cover what the sync must cope with: vCard 3.0 and 4.0 (tel:
URIs), a company without N, a fax number, a photo, folded lines, and one
contact in both books (opendectcell does not merge duplicates). GET /_dump (with
auth) returns every card as JSON, for a test to check what the box wrote.

--default-ns answers with the DAV: namespace as default instead of a "d:"
prefix, as some servers do; --no-chunked reads a chunked request body as
empty, as WSGI servers do; --no-report answers REPORT with 405, as a
server without it would.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import re
import ssl
import subprocess
import sys
import threading
from urllib.parse import quote, unquote
from xml.sax.saxutils import escape

HOME = "/addressbooks/test/"
PRINCIPAL = "/principals/test/"

SEED = {
    "contacts": ("Contacts", {
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
    }),
    "work": ("Work", {
        "reception.vcf": (
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:reception\r\nN:Reception;;;;\r\n"
            "FN:Reception\r\nTEL;TYPE=WORK:0301234560\r\nEND:VCARD\r\n"),
        "anna-work.vcf": (
            "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:anna-work\r\nN:Smith;Anna;;;\r\n"
            "FN:Anna Smith\r\nTEL;TYPE=WORK:0301234567\r\nEND:VCARD\r\n"),
    }),
}


class Book:
    def __init__(self, name, cards):
        self.name = name
        self.cards = {}         # file name -> (text, etag)
        self.ctag = 0
        for n, text in cards.items():
            self.put(n, text)

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


def handler(books, user, password, opts):
    want = "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
    p = "" if opts.default_ns else "d:"
    lock = threading.Lock()

    def response(href, props):
        return ("<%sresponse><%shref>%s</%shref><%spropstat><%sprop>%s</%sprop>"
                "<%sstatus>HTTP/1.1 200 OK</%sstatus></%spropstat></%sresponse>"
                % (p, p, href, p, p, p, props, p, p, p, p, p))

    def book_props(b):
        return ("<%sresourcetype><%scollection/><card:addressbook/></%sresourcetype>"
                "<%sdisplayname>%s</%sdisplayname><cs:getctag>ctag-%d</cs:getctag>"
                % (p, p, p, p, escape(b.name), p, b.ctag))

    def multistatus(parts):
        ns = 'xmlns="DAV:"' if opts.default_ns else 'xmlns:d="DAV:"'
        return ('<?xml version="1.0" encoding="utf-8"?>\n<%smultistatus %s '
                'xmlns:cs="http://calendarserver.org/ns/" '
                'xmlns:card="urn:ietf:params:xml:ns:carddav">\n%s\n</%smultistatus>'
                % (p, ns, "\n".join(parts), p)).encode()

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
                        return b"" if opts.no_chunked else out
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

        def path_only(self):
            return unquote(self.path.split("?")[0])

        def where(self):
            """(book id, book, card file name or None) under the home."""
            path = self.path_only()
            if not path.startswith(HOME) or path == HOME:
                return None, None, None
            bn, _, card = path[len(HOME):].partition("/")
            return bn, books.get(bn), (card or None)

        def do_PROPFIND(self):
            if not self.authed():
                return
            self.body()
            if self.path == "/.well-known/carddav":
                self.send(301, extra={"Location": "/"})
                return
            depth = self.headers.get("Depth", "1")
            path = self.path_only()
            cup = ("<%scurrent-user-principal><%shref>%s</%shref></%scurrent-user-principal>"
                   % (p, p, PRINCIPAL, p, p))
            with lock:
                if path == "/":
                    parts = [response("/", cup)]
                elif path == PRINCIPAL:
                    parts = [response(PRINCIPAL, cup +
                        "<card:addressbook-home-set><%shref>%s</%shref></card:addressbook-home-set>"
                        % (p, HOME, p))]
                elif path == HOME:
                    parts = [response(HOME, "<%sresourcetype><%scollection/></%sresourcetype>"
                                      % (p, p, p))]
                    if depth != "0":
                        for bn, b in sorted(books.items()):
                            parts.append(response(HOME + bn + "/", book_props(b)))
                else:
                    bn, b, card = self.where()
                    if not b or card:
                        self.send(404)
                        return
                    parts = [response(HOME + bn + "/", book_props(b))]
                    if depth != "0":
                        for name, (text, etag) in sorted(b.cards.items()):
                            parts.append(response(HOME + bn + "/" + quote(name),
                                "<%sgetetag>%s</%sgetetag><%sresourcetype/>"
                                % (p, escape(etag), p, p)))
            self.send(207, multistatus(parts), "application/xml; charset=utf-8")

        def do_REPORT(self):
            if not self.authed():
                return
            req = self.body().decode()
            if opts.no_report:
                self.send(405)
                return
            bn, b, card = self.where()
            if not b or card or "addressbook-multiget" not in req:
                self.send(400)
                return
            parts = []
            with lock:
                for href in re.findall(r"<(?:\w+:)?href>([^<]*)</", req):
                    c = b.cards.get(unquote(href)[len(HOME + bn + "/"):])
                    if c:
                        parts.append(response(href, "<%sgetetag>%s</%sgetetag>"
                            "<card:address-data>%s</card:address-data>"
                            % (p, escape(c[1]), p, escape(c[0]).replace("\r", "&#13;"))))
            self.send(207, multistatus(parts), "application/xml; charset=utf-8")

        def do_GET(self):
            if self.path == "/.well-known/carddav":
                self.send(301, extra={"Location": "/"})
                return
            if not self.authed():
                return
            if self.path == "/_dump":
                with lock:
                    d = {bn: {k: v[0] for k, v in b.cards.items()} for bn, b in books.items()}
                self.send(200, json.dumps(d, indent=1, ensure_ascii=False).encode(),
                          "application/json")
                return
            bn, b, card = self.where()
            with lock:
                c = b.cards.get(card) if b and card else None
            if not c:
                self.send(404)
                return
            self.send(200, c[0].encode(), "text/vcard; charset=utf-8", {"ETag": c[1]})

        def do_PUT(self):
            if not self.authed():
                return
            data = self.body().decode()
            bn, b, card = self.where()
            if not b or not card or "BEGIN:VCARD" not in data:
                self.send(400)
                return
            with lock:
                c = b.cards.get(card)
                if self.headers.get("If-None-Match") == "*" and c:
                    self.send(412)
                    return
                im = self.headers.get("If-Match")
                if im and (not c or im != c[1]):
                    self.send(412)
                    return
                etag = b.put(card, data)
            self.send(201 if not c else 204, extra={"ETag": etag})

        def do_DELETE(self):
            if not self.authed():
                return
            self.body()
            bn, b, card = self.where()
            with lock:
                c = b.cards.get(card) if b and card else None
                if not c:
                    self.send(404)
                    return
                im = self.headers.get("If-Match")
                if im and im != c[1]:
                    self.send(412)
                    return
                del b.cards[card]
                b.ctag += 1
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
    ap.add_argument("--no-report", action="store_true",
                    help="answer REPORT with 405")
    a = ap.parse_args()
    crt, key = make_certs(a.dir, a.host)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(crt, key)
    books = {bn: Book(name, cards) for bn, (name, cards) in SEED.items()}
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", a.port),
                                          handler(books, a.user, a.password, a))
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("carddav test server: https://%s:%d/ (home %s, CA %s)"
          % (a.host, a.port, HOME, os.path.join(a.dir, "ca.pem")), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
