#!/usr/bin/ucode
// SPDX-License-Identifier: GPL-2.0-or-later
//
// opendectcell-carddav: the handsets' Contact List from a CardDAV address book,
// both ways. Reads the address book (RFC 6352) over HTTPS into the contacts
// file dect-stubd offers the handsets (-b), and writes what handsets change
// (FILE.changes) back. File formats: src/libopendectcell/dect_contacts.h.
//
// One address book, its collection URL configured; no discovery. Only
// PROPFIND, GET, PUT and DELETE: uclient has no REPORT, so a sync lists
// the ETags (Depth 1) and fetches what changed, after a Depth 0 look at
// the collection's CTag says anything did. Nothing is kept on flash: the
// cache is this process's memory, the files are under /var/run.
//
// Configuration (uci opendectcell.carddav): enabled, url (https:// only),
// username, password, interval (minutes, default 15), ca_file (a PEM CA
// for a self-hosted server; default the system bundle), allow_clear (a
// handset's 'delete list' empties the address book; default off).
//
// Usage: carddav.uc [once]       (once: one sync, then exit; for tests)

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
const SYSTEM_CA = '/etc/ssl/certs/ca-certificates.crt';
const MAX_CONTACTS = 500;	// DECT_CONTACTS_MAX
const NAME_MAX = 31;		// DECT_CONTACT_NAME_MAX - 1, UTF-8 bytes

let cfg;
let cache = {};		// href -> { etag, key, name, first, line, numbers }
let keys = {};		// key -> href
let hints = {};		// href -> the entry id a handset gave it
let ctag = null;
let synced = 0;		// the last journal change applied
let state = { ok: false, contacts: 0, last_sync: 0, error: null };

function log(msg) {
	warn('carddav: ', msg, '\n');
}

function config() {
	let c = uci.cursor();
	let s = c.get_all('opendectcell', 'carddav') || {};
	let url = s.url || '';

	if (substr(url, -1) != '/')
		url += '/';
	return {
		enabled: s.enabled == '1',
		url: url,
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
// transport or TLS error). Redirects are followed, up to three. With
// `challenge`, uclient answers the server's 401 itself (Basic or Digest,
// it learns which from the challenge) by sending the request once more.
function attempt(method, url, headers, body, challenge) {
	let res = { status: 0, headers: {}, body: '' };
	let redirects = 0;
	let cl;

	cl = uclient.new(url, challenge ? cfg.auth : null, {
		header_done: function() {
			let st = cl.status();

			if (st.redirect && redirects++ < 3 && cl.redirect())
				return;
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
	if (sized && index([ 'PUT', 'PROPFIND', 'DELETE', 'POST' ], method) >= 0)
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
function http(method, url, headers, body) {
	let h = { ...(headers || {}) };

	if (!cfg.auth)
		return attempt(method, url, h, body, false);
	h['Authorization'] = 'Basic ' + b64enc(cfg.auth);
	let r = attempt(method, url, h, body, false);
	if (r.status == 401)
		r = attempt(method, url, headers || {}, body, true);
	return r;
}

function ok(r) {
	return r.status >= 200 && r.status < 300;
}

// ---- WebDAV ----------------------------------------------------------------

function xml_text(s) {
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

function origin(url) {
	let m = match(url, /^(https?:\/\/[^\/]+)/);

	return m ? m[1] : '';
}

// PROPFIND: a list of { href (absolute URL), etag, collection, ctag }.
function propfind(url, depth, props) {
	let body = '<?xml version="1.0" encoding="utf-8"?>\n' +
		'<d:propfind xmlns:d="DAV:" xmlns:cs="http://calendarserver.org/ns/">' +
		'<d:prop>' + props + '</d:prop></d:propfind>';
	let r = http('PROPFIND', url, {
		'Depth': '' + depth,
		'Content-Type': 'application/xml; charset=utf-8',
	}, body);
	let out = [];

	if (r.status != 207)
		return { status: r.status, list: null };
	for (let part in split(r.body, /<([A-Za-z0-9_.-]+:)?response[ \t\r\n>]/)) {
		let href = xml_get(part, 'href');

		if (!href)
			continue;
		if (index(href, '/') == 0)
			href = origin(url) + href;
		push(out, {
			href: href,
			etag: xml_get(part, 'getetag'),
			ctag: xml_get(part, 'getctag'),
			collection: !!match(part, /<([A-Za-z0-9_.-]+:)?collection[ \t\r\n\/>]/),
		});
	}
	return { status: r.status, list: out };
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

function remember(href, etag, text) {
	let c = vcard_parse(text);

	c.href = href;
	c.etag = etag;
	c.key = key_for(href);
	cache[href] = c;
}

// Read the address book; true when the cache is current.
function sync() {
	let top = propfind(cfg.url, 0, '<cs:getctag/><d:resourcetype/>');

	if (!top.list) {
		state.error = sprintf('PROPFIND %s: status %d', cfg.url, top.status);
		return false;
	}
	let tag = length(top.list) ? top.list[0].ctag : null;
	if (tag && tag == ctag)
		return true;

	let all = propfind(cfg.url, 1, '<d:getetag/><d:resourcetype/>');
	if (!all.list) {
		state.error = sprintf('PROPFIND %s: status %d', cfg.url, all.status);
		return false;
	}
	let seen = {};
	for (let e in all.list) {
		if (e.collection || !e.etag)
			continue;
		seen[e.href] = true;
		if (cache[e.href] && cache[e.href].etag == e.etag)
			continue;
		let v = fetch(e.href);
		if (!v) {
			state.error = sprintf('GET %s failed', e.href);
			return false;
		}
		remember(e.href, e.etag, v.text);
	}
	for (let href, c in cache)
		if (!seen[href]) {
			delete keys[c.key];
			delete cache[href];
			delete hints[href];
		}
	ctag = tag;
	return true;
}

function uuid() {
	return trim(fs.readfile('/proc/sys/kernel/random/uuid') || sprintf('%x', time()));
}

// Write one change back; 'ok', 'skip' (the server's version wins) or
// 'retry' (try again later).
function apply_change(ch) {
	if (ch.op == 'put' && !ch.key) {
		let id = uuid();
		let href = cfg.url + id + '.vcf';
		let r = http('PUT', href, { 'Content-Type': 'text/vcard; charset=utf-8',
					    'If-None-Match': '*' }, vcard_new(id, ch));

		if (!ok(r))
			return r.status == 0 || r.status >= 500 ? 'retry' : 'skip';
		hints[href] = ch.id;
		ctag = null;
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
				ctag = null;
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
			ctag = null;
			return 'ok';
		}
		return r.status == 0 || r.status >= 500 ? 'retry' : 'skip';
	}
	if (ch.op == 'clear') {
		if (!cfg.allow_clear)
			return 'skip';
		for (let k, h in keys) {
			let r = http('DELETE', h, {});

			if (!ok(r) && r.status != 404)
				return 'retry';
		}
		ctag = null;
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
	if (!length(cache) && !sync()) {
		state.ok = false;
		save_state();
		log(state.error);
		return;
	}
	let had = changes();
	let fresh = sync();

	state.ok = fresh;
	if (fresh) {
		state.error = null;
		state.last_sync = time();
	}
	// after changes the file must say seq=N even if the read failed
	if (fresh || had)
		write_contacts();
	save_state();
	if (state.error)
		log(state.error);
}

cfg = config();
if (!cfg.enabled || index(cfg.url, 'https://') != 0) {
	log(cfg.enabled ? 'the url must start with https://' : 'disabled');
	exit(cfg.enabled ? 1 : 0);
}
fs.mkdir(DIR);
uloop.init();
stop_check();
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
