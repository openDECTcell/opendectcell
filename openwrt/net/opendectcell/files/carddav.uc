#!/usr/bin/ucode
// SPDX-License-Identifier: GPL-2.0-or-later
//
// opendectcell-carddav: the handsets' Contact List from CardDAV address books,
// both ways. Reads the chosen address books (RFC 6352) over HTTPS into the
// contacts file dect-stubd offers the handsets (-b), and writes what
// handsets change (FILE.changes) back: an edit or a delete to the book the
// contact came from, a new contact to the default book. File formats:
// src/libopendectcell/dect_contacts.h. Contacts in several books are shown once
// per book; nothing is merged.
//
// Discovery (`discover`): from any URL of the account -- a server name, a
// principal, a home set, a book -- as RFC 6764 section 6 and RFC 6352
// section 7 describe it: the URL's own books, else /.well-known/carddav
// and the current-user-principal, its addressbook-home-set, the books in
// that. PROPFIND only.
//
// A sync looks at each book's CTag (Depth 0); when it changed, it lists
// the ETags (Depth 1) and fetches the cards that changed, in batches with
// REPORT addressbook-multiget, or one GET each where REPORT is missing
// (the stock uclient, or a server without it). Nothing is kept on flash:
// the cache is this process's memory, the files are under /var/run.
//
// Configuration (uci opendectcell.carddav): enabled, url (https:// only; where
// discovery starts, and the one book when no book is listed), list book
// (the books' URLs), default (the book for new contacts; default the
// first), username, password, interval (minutes, default 15), ca_file (a
// PEM CA for a self-hosted server; default the system bundle), allow_clear
// (a handset's 'delete list' empties every chosen book; default off).
//
// Usage: carddav.uc [once]          (once: one sync, then exit; for tests)
//        carddav.uc discover [FILE] (the account's books as JSON; FILE, a
//                                    JSON {url, username, password} that
//                                    is read and deleted, else uci)

'use strict';

let fs = require('fs');
let uci = require('uci');
let uloop = require('uloop');

// opendectcell_uclient (package ucode-mod-opendectcell-uclient) sends a body with
// its Content-Length; the system's uclient can only send it chunked,
// which some servers read as empty (docs/phonebook.md, Limits).
let uclient, sized = true;
try {
	uclient = require('opendectcell_uclient');
} catch (e) {
	uclient = require('uclient');
	sized = false;
}

const DIR = '/var/run/opendectcell';
const FILE = DIR + '/contacts';
const STATE = DIR + '/carddav.json';
const BOOKS = DIR + '/carddav-books.json';	// the last discovery, for LuCI
const MULTIGET = 50;		// cards per REPORT
const SYSTEM_CA = '/etc/ssl/certs/ca-certificates.crt';
const MAX_CONTACTS = 500;	// DECT_CONTACTS_MAX
const NAME_MAX = 31;		// DECT_CONTACT_NAME_MAX - 1, UTF-8 bytes

let cfg;
let cache = {};		// card href -> { book, etag, key, name, first, line, numbers }
let keys = {};		// key -> card href
let hints = {};		// card href -> the entry id a handset gave it
let ctags = {};		// book -> its CTag at the last good read
let report;		// REPORT works this round (only our uclient has it)
let synced = 0;		// the last journal change applied
let state = { ok: false, contacts: 0, last_sync: 0, error: null, books: [] };

function log(msg) {
	warn('carddav: ', msg, '\n');
}

function slash(url) {
	return substr(url, -1) == '/' ? url : url + '/';
}

function config() {
	let c = uci.cursor();
	let s = c.get_all('opendectcell', 'carddav') || {};
	let url = slash(s.url || '');
	let books = map(type(s.book) == 'array' ? s.book : (s.book ? [ s.book ] : []), slash);

	if (!length(books))
		books = [ url ];
	return {
		enabled: s.enabled == '1',
		url: url,
		books: books,
		default: s.default && index(books, slash(s.default)) >= 0 ? slash(s.default) : books[0],
		auth: s.username ? s.username + ':' + (s.password || '') : null,
		interval: (+s.interval || 15) * 60,
		ca: s.ca_file || SYSTEM_CA,
		allow_clear: s.allow_clear == '1',
	};
}

function save_state() {
	state.contacts = length(keys);
	fs.writefile(STATE + '.tmp', sprintf('%J\n', state));
	fs.rename(STATE + '.tmp', STATE);
}

// ---- HTTP ------------------------------------------------------------------

function origin(url) {
	let m = match(url, /^(https?:\/\/[^\/]+)/);

	return m ? m[1] : '';
}

// An href as a URL: absolute already, or a path on base's server.
function absolute(base, href) {
	if (index(href, 'http://') == 0 || index(href, 'https://') == 0)
		return href;
	return origin(base) + (index(href, '/') == 0 ? '' : '/') + href;
}


// A stop ends this process at once (maintainer, 2026-10-29: "If the
// service is stopped, we want things to stop immediately"). Under ucode's
// uloop a SIGTERM is caught and, with no handler of ours, dropped: procd
// waited 5 s and killed us (2026-10-30, 7412). So: our own handler, which
// ucode runs between two instructions (hence the short sleeps below), and
// after each request a look whether uloop's loop ended on one.
function stop() {
	exit(0);
}

function stop_check() {
	if (uloop.cancelling())
		exit(0);
	signal('SIGTERM', stop);
	signal('SIGINT', stop);
}

// One request on a fresh client: { status, headers, body } (status 0 on a
// transport or TLS error). With `challenge`, uclient answers the server's
// 401 itself (Basic or Digest, it learns which from the challenge) by
// sending the request once more. Redirects are http()'s: uclient's own
// would send the request again without its body.
function attempt(method, url, headers, body, challenge) {
	let res = { status: 0, headers: {}, body: '' };
	let cl;

	cl = uclient.new(url, challenge ? cfg.auth : null, {
		header_done: function() {
			let st = cl.status();

			res.status = st.status;
			res.headers = cl.get_headers() || {};
		},
		data_read: function() {
			let d;

			while (length(d = cl.read()))
				res.body += d;
		},
		data_eof: function() {
			uloop.end();
		},
		error: function(code) {
			res.status = 0;
			res.error = code;
			uloop.end();
		},
	});
	if (!cl)
		return res;
	if (index(url, 'https://') == 0 &&
	    !cl.ssl_init({ verify: true, ca_files: [ cfg.ca ] }))
		return res;
	cl.set_timeout(20000);
	let args = { headers: { ...(headers || {}) } };
	if (body != null)
		args.post_data = body;
	if (sized && index([ 'PUT', 'PROPFIND', 'REPORT', 'DELETE', 'POST' ], method) >= 0)
		args.headers['Content-Length'] = '' + length(body != null ? body : '');
	for (let tries = 0; tries < (challenge ? 2 : 1); tries++) {
		res = { status: 0, headers: {}, body: '' };
		if (!cl.connect() || !cl.request(method, args))
			break;
		uloop.run();
		stop_check();
		if (res.status != 401)
			break;
	}
	cl.disconnect();
	cl.free();
	return res;
}

// One request: Basic credentials sent up front, the usual case over HTTPS,
// so a request costs one round trip; on a 401 once more the slow way.
// Up to three redirects are followed, the whole request each time, to
// https:// only; r.url is where the answer came from.
function http(method, url, headers, body) {
	let r;

	for (let hops = 0; hops < 4; hops++) {
		let h = { ...(headers || {}) };

		if (cfg.auth)
			h['Authorization'] = 'Basic ' + b64enc(cfg.auth);
		r = attempt(method, url, h, body, false);
		if (r.status == 401 && cfg.auth)
			r = attempt(method, url, headers || {}, body, true);
		r.url = url;
		let to = r.headers.location;
		if (index([ 301, 302, 303, 307, 308 ], r.status) < 0 || !to)
			break;
		to = absolute(url, to);
		if (index(to, 'https://') != 0)
			break;
		url = to;
	}
	return r;
}

function ok(r) {
	return r.status >= 200 && r.status < 300;
}

// ---- WebDAV ----------------------------------------------------------------

function xml_text(s) {
	s = replace(s, /&#(1[03]|9);/g, (m, n) => chr(+n));
	s = replace(s, /&#x([0-9a-fA-F]{1,2});/g, (m, n) => hex(n) < 128 ? chr(hex(n)) : m);
	s = replace(s, /&quot;/g, '"');
	s = replace(s, /&apos;/g, "'");
	s = replace(s, /&lt;/g, '<');
	s = replace(s, /&gt;/g, '>');
	return replace(s, /&amp;/g, '&');
}

// The text of the first element with this local name, any prefix, or null.
function xml_get(s, name) {
	let m = match(s, regexp('<([A-Za-z0-9_.-]+:)?' + name + '([ \t\r\n][^>]*)?>([^<]*)<'));

	return m ? trim(xml_text(m[3])) : null;
}

// What follows the first opening tag with this local name, or null.
function xml_after(s, name) {
	let p = split(s, regexp('<([A-Za-z0-9_.-]+:)?' + name + '([ \t\r\n][^>]*)?>'), 2);

	return length(p) == 2 ? p[1] : null;
}

// An element's text that may hold markup-like content: CDATA, or text up
// to the closing tag.
function xml_body(s, name) {
	let rest = xml_after(s, name);

	if (rest == null)
		return null;
	if (index(rest, '<![CDATA[') == 0)
		return substr(rest, 9, index(rest, ']]>') - 9);
	let end = index(rest, '<');
	return xml_text(end < 0 ? rest : substr(rest, 0, end));
}

// The <response> parts of a multistatus body.
function responses(body) {
	return slice(split(body, /<([A-Za-z0-9_.-]+:)?response[ \t\r\n>]/), 1);
}

// The href inside an element, e.g. current-user-principal, or null.
function href_in(part, name) {
	let rest = xml_after(part, name);

	return rest == null ? null : xml_get(rest, 'href');
}

// PROPFIND: a list of { href (absolute URL), etag, ctag, name, collection,
// book (an address book), principal, home }.
function propfind(url, depth, props) {
	let body = '<?xml version="1.0" encoding="utf-8"?>\n' +
		'<d:propfind xmlns:d="DAV:" xmlns:cs="http://calendarserver.org/ns/"' +
		' xmlns:card="urn:ietf:params:xml:ns:carddav">' +
		'<d:prop>' + props + '</d:prop></d:propfind>';
	let r = http('PROPFIND', url, {
		'Depth': '' + depth,
		'Content-Type': 'application/xml; charset=utf-8',
	}, body);
	let out = [];

	if (r.status != 207)
		return { status: r.status, list: null };
	url = r.url;
	for (let part in responses(r.body)) {
		let href = xml_get(part, 'href');
		let principal = href_in(part, 'current-user-principal');
		let home = href_in(part, 'addressbook-home-set');

		if (!href)
			continue;
		push(out, {
			href: absolute(url, href),
			etag: xml_get(part, 'getetag'),
			ctag: xml_get(part, 'getctag'),
			name: xml_get(part, 'displayname'),
			collection: !!match(part, /<([A-Za-z0-9_.-]+:)?collection[ \t\r\n\/>]/),
			book: !!match(part, /<([A-Za-z0-9_.-]+:)?addressbook[ \t\r\n\/>]/),
			principal: principal ? absolute(url, principal) : null,
			home: home ? absolute(url, home) : null,
		});
	}
	return { status: r.status, list: out };
}

// ---- discovery ---------------------------------------------------------------

// The address books at a URL: itself if it is one, else those inside it.
function books_at(url) {
	let r = propfind(slash(url), 1, '<d:resourcetype/><d:displayname/>');

	if (!r.list)
		return { status: r.status, list: null };
	let self = filter(r.list, (e) => slash(e.href) == slash(url) && e.book);
	let list = length(self) ? self : filter(r.list, (e) => e.book && slash(e.href) != slash(url));

	return { status: r.status, list: map(list, (e) => ({ href: slash(e.href), name: e.name || e.href })) };
}

// RFC 6764 section 6 and RFC 6352 section 7: the URL's own books, else
// its principal (or the well-known URL's), that principal's home set and
// the books there.
function discover(url) {
	let b = books_at(url);

	if (b.list && length(b.list))
		return { ok: true, books: b.list };
	if (b.status == 401)
		return { ok: false, error: 'the server refused the user name or password' };
	let principal = null;
	for (let u in [ url, origin(url) + '/.well-known/carddav' ]) {
		let r = propfind(u, 0, '<d:current-user-principal/>');

		principal = r.list && length(r.list) ? r.list[0].principal : null;
		if (principal)
			break;
	}
	if (!principal)
		return { ok: false, error: sprintf('no CardDAV account found at %s', url) };
	let r = propfind(principal, 0, '<card:addressbook-home-set/>');
	let home = r.list && length(r.list) ? r.list[0].home : null;
	if (!home)
		return { ok: false, error: sprintf('%s names no address book home', principal) };
	b = books_at(home);
	if (!b.list)
		return { ok: false, error: sprintf('PROPFIND %s: status %d', home, b.status) };
	return { ok: true, books: b.list };
}

// ---- vCard -----------------------------------------------------------------

// Unfolded lines, each { name, params (upper-case, raw), value, raw }.
function vcard_lines(text) {
	text = replace(text, /\r?\n[ \t]/g, '');
	let out = [];

	for (let raw in split(text, /\r?\n/)) {
		if (raw == '')
			continue;
		let colon = index(raw, ':');
		if (colon < 0)
			continue;
		let head = split(substr(raw, 0, colon), ';');
		let name = uc(head[0]);
		let dot = index(name, '.');		// item1.TEL
		if (dot >= 0)
			name = substr(name, dot + 1);
		push(out, {
			name: name,
			params: uc(join(';', slice(head, 1))),
			value: substr(raw, colon + 1),
			raw: raw,
		});
	}
	return out;
}

function unescape(v) {
	v = replace(v, /\\[nN]/g, ' ');
	return replace(v, /\\(.)/g, (m, c) => c);
}

function escape(v) {
	v = replace(v, /\\/g, '\\\\');
	v = replace(v, /,/g, '\\,');
	return replace(v, /;/g, '\\;');
}

// Split on unescaped ';'.
function components(v) {
	let out = [], cur = '';

	for (let i = 0; i < length(v); i++) {
		let c = substr(v, i, 1);

		if (c == '\\' && i + 1 < length(v)) {
			cur += c + substr(v, i + 1, 1);
			i++;
		} else if (c == ';') {
			push(out, cur);
			cur = '';
		} else {
			cur += c;
		}
	}
	push(out, cur);
	return out;
}

// Text for a handset: no control characters, at most NAME_MAX bytes,
// never cut inside a UTF-8 character.
function clean(s) {
	s = trim(replace(s || '', /[[:cntrl:]]/g, ' '));
	if (length(s) > NAME_MAX) {
		let n = NAME_MAX;
		while (n > 0 && (ord(s, n) & 0xC0) == 0x80)
			n--;
		s = substr(s, 0, n);
	}
	return s;
}

// A dialable form for the handset: digits * #, a leading + as 00.
function digits(v) {
	v = trim(v);
	if (index(lc(v), 'tel:') == 0)
		v = substr(v, 4);
	v = replace(v, /;.*$/, '');		// tel: URI parameters, ext=
	let plus = index(v, '+') == 0;
	v = replace(v, /[^0-9*#]/g, '');
	return v == '' ? '' : (plus ? '00' : '') + v;
}

// 'Contact number' properties from a TEL's TYPEs; null = not a phone (fax,
// pager without voice).
function tel_props(params) {
	let t = params;

	if (match(t, /FAX|PAGER/) && !match(t, /VOICE|CELL/))
		return null;
	if (match(t, /CELL/))
		return 0x04;
	if (match(t, /WORK/))
		return 0x02;
	return 0x08;
}

// The fields a handset shows, from a vCard text: { name, first, line,
// numbers: [ { props, digits, raw } ] } -- raw = the TEL line it came from.
function vcard_parse(text) {
	let c = { name: '', first: '', line: '*', numbers: [] };
	let fn = '', org = '';

	for (let l in vcard_lines(text)) {
		if (l.name == 'N') {
			let p = components(l.value);

			c.name = unescape(p[0] || '');
			c.first = unescape(p[1] || '');
		} else if (l.name == 'FN') {
			fn = unescape(l.value);
		} else if (l.name == 'ORG') {
			org = unescape(components(l.value)[0] || '');
		} else if (l.name == 'TEL' && length(c.numbers) < 3) {
			let props = tel_props(l.params);
			let d = digits(l.value);

			if (props != null && d != '')
				push(c.numbers, { props: props, digits: d, raw: l.raw });
		} else if (l.name == 'X-OPENDECTCELL-LINE') {
			let v = trim(l.value);

			if (match(v, /^[0-9]+$/) && +v < 127)
				c.line = v;
		}
	}
	if (c.name == '' && c.first == '')
		c.name = fn != '' ? fn : org;
	c.name = clean(c.name);
	c.first = clean(c.first);
	return c;
}

function is_v4(text) {
	return !!match(text, /\nVERSION:4\.0/);
}

function tel_line(v4, n) {
	let type = n.props == 0x04 ? 'cell' : n.props == 0x02 ? 'work' : 'voice';

	return v4 ? 'TEL;VALUE=uri;TYPE=' + type + ':tel:' + n.digits
		  : 'TEL;TYPE=' + uc(type) + ':' + n.digits;
}

// The vCard text with a handset's change applied; everything the handset
// does not show (photos, addresses, fax numbers, ...) stays as it was.
function vcard_apply(text, ch) {
	let old = vcard_parse(text);
	let v4 = is_v4(text);
	let lines = vcard_lines(text);
	let out = [], tels = [], placed = false;
	let used = {};

	for (let n in old.numbers)
		used[n.raw] = true;
	// the new numbers: an unchanged one keeps its line, formatting and all
	for (let n in ch.numbers) {
		let keep = null;

		for (let o in old.numbers)
			if (o.digits == n.digits && o.props == n.props && used[o.raw]) {
				keep = o.raw;
				used[o.raw] = false;
				break;
			}
		push(tels, keep || tel_line(v4, n));
	}
	for (let o in old.numbers)
		used[o.raw] = true;	// every shown TEL is replaced

	let names = ch.name != old.name || ch.first != old.first;

	for (let l in lines) {
		if (l.name == 'END' && uc(l.value) == 'VCARD') {
			if (!placed)
				for (let t in tels)
					push(out, t);
			placed = true;
			if (names && !length(filter(lines, (x) => x.name == 'N')))
				push(out, 'N:' + escape(ch.name) + ';' + escape(ch.first) + ';;;');
			if (names && !length(filter(lines, (x) => x.name == 'FN')))
				push(out, 'FN:' + replace(escape(trim(ch.first + ' ' + ch.name)),
							  /\\;/g, ';'));
			if (ch.line != '*' && !length(filter(lines, (x) => x.name == 'X-OPENDECTCELL-LINE')))
				push(out, 'X-OPENDECTCELL-LINE:' + ch.line);
			push(out, l.raw);
			continue;
		}
		if (l.name == 'TEL' && used[l.raw]) {
			if (!placed)
				for (let t in tels)
					push(out, t);
			placed = true;
			continue;
		}
		if (names && l.name == 'N') {
			let p = components(l.value);

			p[0] = escape(ch.name);
			p[1] = escape(ch.first);
			while (length(p) < 5)
				push(p, '');
			push(out, substr(l.raw, 0, index(l.raw, ':') + 1) + join(';', p));
			continue;
		}
		if (names && l.name == 'FN') {
			let fn = trim(ch.first + ' ' + ch.name);

			push(out, substr(l.raw, 0, index(l.raw, ':') + 1) +
			     replace(escape(fn), /\\;/g, ';'));
			continue;
		}
		if (l.name == 'X-OPENDECTCELL-LINE') {
			if (ch.line != '*')
				push(out, 'X-OPENDECTCELL-LINE:' + ch.line);
			continue;
		}
		push(out, l.raw);
	}
	return join('\r\n', out) + '\r\n';
}

function vcard_new(uid, ch) {
	let out = [ 'BEGIN:VCARD', 'VERSION:3.0', 'PRODID:-//opendectcell//carddav//EN',
		    'UID:' + uid,
		    'N:' + escape(ch.name) + ';' + escape(ch.first) + ';;;',
		    'FN:' + replace(escape(trim(ch.first + ' ' + ch.name)), /\\;/g, ';') ];

	for (let n in ch.numbers)
		push(out, tel_line(false, n));
	if (ch.line != '*')
		push(out, 'X-OPENDECTCELL-LINE:' + ch.line);
	push(out, 'END:VCARD');
	return join('\r\n', out) + '\r\n';
}

// ---- the key a contact has in the contacts file -----------------------------

function fnv32(s) {
	let h = 0x811c9dc5;

	for (let i = 0; i < length(s); i++)
		h = ((h ^ ord(s, i)) * 16777619) & 0xffffffff;
	return h || 1;
}

function key_for(href) {
	if (cache[href] && cache[href].key)
		return cache[href].key;
	let k = fnv32(href);
	while (keys[k] && keys[k] != href)
		k = (k + 1) & 0xffffffff || 1;
	keys[k] = href;
	return k;
}

// ---- the files ----------------------------------------------------------------

function write_contacts() {
	let rows = [];

	for (let href, c in cache)
		push(rows, c);
	rows = sort(rows, (a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
	if (length(rows) > MAX_CONTACTS) {
		log(sprintf('%d contacts, the handsets get the first %d', length(rows), MAX_CONTACTS));
		rows = slice(rows, 0, MAX_CONTACTS);
	}
	let text = sprintf('#opendectcell-contacts 1 seq=%d%s\n', synced,
			   cfg.allow_clear ? ' clear=1' : '');
	for (let c in rows) {
		let f = [ sprintf('%08x', c.key), hints[c.href] ? '' + hints[c.href] : '-',
			  c.name, c.first, '0', c.line ];

		for (let n in c.numbers)
			push(f, sprintf('%02x:%s', n.props, n.digits));
		text += join('\t', f) + '\n';
	}
	fs.writefile(FILE + '.tmp', text);
	fs.rename(FILE + '.tmp', FILE);
}

// One journal line: { seq, op, id, key, name, first, line, numbers }.
function change_parse(line) {
	let f = split(line, '\t');
	let ch = { seq: +f[0], op: f[1], id: +f[2], key: f[3] ? hex(f[3]) : 0 };

	if (ch.op == 'put') {
		ch.name = f[4] || '';
		ch.first = f[5] || '';
		ch.line = f[7] || '*';
		ch.numbers = [];
		for (let i = 8; i < length(f); i++) {
			let m = match(f[i], /^([0-9a-f]{2}):([0-9*#]+)$/);

			if (m)
				push(ch.numbers, { props: hex(m[1]) & 0x1e, digits: m[2] });
		}
	}
	return ch;
}

// ---- sync -----------------------------------------------------------------------

function fetch(href) {
	let r = http('GET', href, { 'Accept': 'text/vcard' });

	if (!ok(r))
		return null;
	return { text: r.body, etag: r.headers.etag };
}

function remember(book, href, etag, text) {
	let c = vcard_parse(text);

	c.book = book;
	c.href = href;
	c.etag = etag;
	c.key = key_for(href);
	cache[href] = c;
}

function path_of(href) {
	return substr(href, length(origin(href)));
}

// Cards by REPORT addressbook-multiget (RFC 6352 8.7), MULTIGET at a time;
// the ones it did not bring, by GET. false when one could not be fetched.
function fetch_all(book, list) {
	let want = {};

	for (let e in list)
		want[e.href] = e.etag;
	for (let i = 0; report && i < length(list); i += MULTIGET) {
		let body = '<?xml version="1.0" encoding="utf-8"?>\n' +
			'<card:addressbook-multiget xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">' +
			'<d:prop><d:getetag/><card:address-data/></d:prop>';

		for (let e in slice(list, i, i + MULTIGET))
			body += '<d:href>' + replace(replace(path_of(e.href), /&/g, '&amp;'), /</g, '&lt;') + '</d:href>';
		body += '</card:addressbook-multiget>';
		let r = http('REPORT', book, { 'Depth': '1',
			'Content-Type': 'application/xml; charset=utf-8' }, body);
		if (r.status != 207) {
			// 405, 501, no answer at all, ...: GET for the rest of
			// this round; a book that is really gone fails there
			report = false;
			log(sprintf('REPORT %s: status %d; fetching cards one by one', book, r.status));
			break;
		}
		for (let part in responses(r.body)) {
			let href = xml_get(part, 'href');
			let text = xml_body(part, 'address-data');

			if (!href || text == null)
				continue;
			href = absolute(book, href);
			if (!(href in want))
				continue;
			remember(book, href, xml_get(part, 'getetag') || want[href], text);
			delete want[href];
		}
	}
	for (let href, etag in want) {
		let v = fetch(href);

		if (!v)
			return false;
		remember(book, href, etag, v.text);
	}
	return true;
}

// Read one address book; true when its part of the cache is current.
function sync_book(book) {
	let top = propfind(book, 0, '<cs:getctag/><d:resourcetype/>');

	if (!top.list)
		return sprintf('PROPFIND %s: status %d', book, top.status);
	let tag = length(top.list) ? top.list[0].ctag : null;
	if (tag && tag == ctags[book])
		return null;

	let all = propfind(book, 1, '<d:getetag/><d:resourcetype/>');
	if (!all.list)
		return sprintf('PROPFIND %s: status %d', book, all.status);
	let seen = {}, todo = [];
	for (let e in all.list) {
		if (e.collection || !e.etag)
			continue;
		seen[e.href] = true;
		if (!cache[e.href] || cache[e.href].etag != e.etag)
			push(todo, e);
	}
	if (!fetch_all(book, todo))
		return sprintf('could not fetch the cards of %s', book);
	for (let href, c in cache)
		if (c.book == book && !seen[href]) {
			delete keys[c.key];
			delete cache[href];
			delete hints[href];
		}
	ctags[book] = tag;
	return null;
}

// Every chosen book; true when all are current. A book that fails keeps
// what was read of it before.
function sync() {
	let errors = [];

	report = sized;
	state.books = [];
	for (let book in cfg.books) {
		let err = sync_book(book);
		let n = 0;

		for (let href, c in cache)
			n += c.book == book;
		push(state.books, { href: book, contacts: n, error: err });
		if (err)
			push(errors, err);
	}
	// the cache of books no longer chosen
	for (let href, c in cache)
		if (index(cfg.books, c.book) < 0) {
			delete keys[c.key];
			delete cache[href];
		}
	state.error = length(errors) ? join('; ', errors) : null;
	return !length(errors);
}

function uuid() {
	return trim(fs.readfile('/proc/sys/kernel/random/uuid') || sprintf('%x', time()));
}

// Write one change back; 'ok', 'skip' (the server's version wins) or
// 'retry' (try again later).
function apply_change(ch) {
	if (ch.op == 'put' && !ch.key) {
		let id = uuid();
		let href = cfg.default + id + '.vcf';
		let r = http('PUT', href, { 'Content-Type': 'text/vcard; charset=utf-8',
					    'If-None-Match': '*' }, vcard_new(id, ch));

		if (!ok(r))
			return r.status == 0 || r.status >= 500 ? 'retry' : 'skip';
		hints[href] = ch.id;
		ctags = {};
		return 'ok';
	}
	let href = keys[ch.key];
	if (ch.op == 'put') {
		for (let tries = 0; href && tries < 2; tries++) {
			let v = fetch(href);

			if (!v)
				return 'skip';		// gone on the server
			let r = http('PUT', href, { 'Content-Type': 'text/vcard; charset=utf-8',
						    'If-Match': v.etag }, vcard_apply(v.text, ch));
			if (ok(r)) {
				ctags = {};
				return 'ok';
			}
			if (r.status != 412)
				return r.status == 0 || r.status >= 500 ? 'retry' : 'skip';
		}
		return 'skip';
	}
	if (ch.op == 'del') {
		if (!href)
			return 'ok';
		let h = {};
		if (cache[href] && cache[href].etag)
			h['If-Match'] = cache[href].etag;
		let r = http('DELETE', href, h);
		if (ok(r) || r.status == 404) {
			ctags = {};
			return 'ok';
		}
		return r.status == 0 || r.status >= 500 ? 'retry' : 'skip';
	}
	if (ch.op == 'clear') {
		// every chosen book; the cards it holds that we know
		if (!cfg.allow_clear)
			return 'skip';
		for (let k, h in keys) {
			let r = http('DELETE', h, {});

			if (!ok(r) && r.status != 404)
				return 'retry';
		}
		ctags = {};
		return 'ok';
	}
	return 'skip';
}

// The handsets' changes: FILE.changes is taken (renamed) whole, worked
// off, and what could not be written yet stays for the next round.
function changes() {
	let work = FILE + '.changes.work';

	if (!fs.stat(work) && !(fs.stat(FILE + '.changes') &&
				fs.rename(FILE + '.changes', work)))
		return false;
	let lines = filter(split(fs.readfile(work) || '', '\n'), (l) => length(l));
	let left = [];

	for (let i = 0; i < length(lines); i++) {
		let ch = change_parse(lines[i]);
		let r = length(left) ? 'retry' : apply_change(ch);

		if (r == 'retry') {
			push(left, lines[i]);
			continue;
		}
		if (r == 'skip')
			log(sprintf('change %d (%s) not written: the server did not take it',
				    ch.seq, ch.op));
		if (ch.seq > synced)
			synced = ch.seq;
	}
	if (length(left)) {
		fs.writefile(work, join('\n', left) + '\n');
		state.error = 'the server did not answer; handset changes are kept';
	} else {
		fs.unlink(work);
	}
	return true;
}

function round() {
	// the hrefs the changes refer to: a fresh process has none yet
	if (!length(cache) && !sync() && !length(cache)) {
		state.ok = false;
		save_state();
		log(state.error);
		return;
	}
	let had = changes();
	let fresh = sync();

	state.ok = fresh;
	if (fresh)
		state.last_sync = time();
	// after changes the file must say seq=N even if the read failed
	if (fresh || had)
		write_contacts();
	save_state();
	if (state.error)
		log(state.error);
}

cfg = config();
fs.mkdir(DIR);
uloop.init();
stop_check();

if (ARGV[0] == 'discover') {
	// LuCI hands the form's values over in a root-only file, never on a
	// command line every local user could read
	if (ARGV[1]) {
		let req = json(fs.readfile(ARGV[1]) || '{}');

		fs.unlink(ARGV[1]);
		cfg.url = slash(req.url || '');
		cfg.auth = req.username ? req.username + ':' + (req.password || '') : null;
	}
	let r = index(cfg.url, 'https://') == 0 ? discover(cfg.url)
		: { ok: false, error: 'the URL must start with https://' };
	if (r.ok)
		fs.writefile(BOOKS, sprintf('%J\n', r.books));
	print(sprintf('%J\n', r));
	exit(r.ok ? 0 : 1);
}

if (!cfg.enabled || index(cfg.url, 'https://') != 0) {
	log(cfg.enabled ? 'the url must start with https://' : 'disabled');
	exit(cfg.enabled ? 1 : 0);
}
// the journal number the last run reached, from the file it left
{
	let head = fs.readfile(FILE, 128);
	let m = head ? match(head, /seq=([0-9]+)/) : null;

	if (m)
		synced = +m[1];
}

if (ARGV[0] == 'once') {
	round();
	exit(state.ok ? 0 : 1);
}

let next = 0;
while (true) {
	if (time() >= next || fs.stat(FILE + '.changes')) {
		round();
		next = time() + cfg.interval;
	}
	for (let i = 0; i < 10; i++)
		sleep(200);
}
