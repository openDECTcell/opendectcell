'use strict';
'require view';
'require fs';
'require poll';
'require ui';
'require uci';
'require rpc';

/*
 * Status > DECT, the DECT Monitor: what the base is doing, for someone who
 * should not have to read a log. Three tabs: DECT reads
 * /tmp/opendectcell-state.json, which dect-stubd rewrites at most once a second
 * (docs/diagnostics.md); VoIP reads /var/run/opendectcell/voip.json, which the
 * baresip module writes every 2 s; both start with their service's state
 * from procd, so a stopped service is told apart from anything else that
 * goes wrong. Log shows the system log's lines of both, for when it does.
 *
 * Per connection it shows what AVM's DECT Monitor shows and we have
 * decoded from the chip's connection status: carrier and frequency, time
 * slot, field strength, quality, the eight error counters, frames and call
 * time [CR A-0121, A-0123]; plus the levels of all ten carriers and a live
 * graph of level and quality, kept in the browser while the page is open.
 * Deliberately not a debugging console: raw mails stay in dect-stubd -v.
 */
var STATE = '/tmp/opendectcell-state.json';
var VOIP = '/var/run/opendectcell/voip.json';
var HISTORY_S = 300;		/* the graph's span */
var history = {};		/* handle -> [{t, dbm, q}] */

var callServices = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

function readJson(path) {
	return fs.read(path).then(function(text) {
		try { return JSON.parse(text); } catch (e) { return null; }
	}).catch(function() { return null; });
}

function readState() {
	return readJson(STATE);
}

/*
 * The model profiles' pretty names, id -> name (the files' name = "..."),
 * read once: they change only with the package.
 */
var PROFILES = '/etc/opendectcell/profiles';
var modelNames = null;

function readModelNames() {
	if (modelNames)
		return Promise.resolve(modelNames);
	return L.resolveDefault(fs.list(PROFILES), []).then(function(files) {
		return Promise.all(files.filter(function(f) {
			return f.type == 'file' && /\.profile$/.test(f.name);
		}).map(function(f) {
			return L.resolveDefault(fs.read(PROFILES + '/' + f.name), '').then(function(t) {
				var m = (t || '').match(/^\s*name\s*=\s*"?([^"#\n]*?)"?\s*(#.*)?$/m);
				return [ f.name.replace(/\.profile$/, ''), m ? m[1] : null ];
			});
		}));
	}).then(function(list) {
		modelNames = {};
		list.forEach(function(x) { if (x[1]) modelNames[x[0]] = x[1]; });
		return modelNames;
	});
}

/* everything one refresh needs: [state, voip, procd's instances] */
function readAll() {
	return Promise.all([
		readState(),
		readJson(VOIP),
		L.resolveDefault(callServices('opendectcell'), {}).then(function(r) {
			return (r && r.opendectcell && r.opendectcell.instances) || {};
		}),
		/* the settings as they are now, not as when the page opened */
		Promise.resolve(uci.unload('opendectcell')).then(function() {
			return uci.load('opendectcell');
		}).catch(function() {}),
		readModelNames()
	]);
}

function ago(ms) {
	if (ms == null)
		return _('not seen since the base started');
	var s = Math.round(ms / 1000);
	if (s < 60)
		return _('%d s ago').format(s);
	if (s < 3600)
		return _('%d min ago').format(Math.round(s / 60));
	return _('%d h ago').format(Math.round(s / 3600));
}

function duration(s) {
	s = Math.floor(s);
	var h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60;
	return (h ? h + ':' + ('0' + m).slice(-2) : String(m)) + ':' +
		('0' + (s % 60)).slice(-2);
}

/*
 * The chip's raw level as FRITZ!OS converts it on the 7520 (clean room
 * A-0121): dBm = (raw - 20) * 80/37 - 110; 0 and 255 carry no value.
 */
function rawDbm(raw) {
	if (raw == null || raw == 0 || raw == 255)
		return null;
	return (raw - 20) * 80 / 37 - 110;
}

/* AVM's quality steps (>-40 dBm 100 %, then 10 % per 5 dB), two per bar. */
function bars(d) {
	if (d == null)
		return '-';
	var n = d > -40 ? 5 : d > -50 ? 4 : d > -60 ? 3 : d > -70 ? 2 : 1;
	return '▮'.repeat(n) + '▯'.repeat(5 - n) + ' ' +
		_('%.1f dBm').format(d);
}

/* DECT carrier c, EN 300 175-2: 1897.344 MHz - c * 1.728 MHz */
function freq(c) {
	return (1897.344 - 1.728 * c).toFixed(3);
}

var CALL = {
	idle: _('idle'), proceeding: _('setting up'), connecting: _('connecting'),
	active: _('in a call'), released: _('call ended'), calling: _('being called'),
	ringing: _('ringing'), dialling: _('dialling')
};

var CODEC = { 2: _('G.726, narrowband'), 3: _('G.722, wideband (HD)') };

/* the counters' names as FRITZ!OS's the vendor's DECT application has them [CR A-0123] */
var EVENTS = [
	[ 'acrc', _('A-field CRC errors') ],
	[ 'xcrc', _('X-field CRC errors') ],
	[ 'zcrc', _('Z-field CRC errors') ],
	[ 'qbit1', _('Q1 bit errors') ],
	[ 'qbit2', _('Q2 bit errors') ],
	[ 'handover', _('Handover requests') ],
	[ 'slideerr', _('Slide errors') ],
	[ 'syncerr', _('Sync errors') ]
];

/*
 * A cell's content as DOM children. LuCI's dom.append() sets innerHTML for a
 * plain string child but makes text nodes of an array's strings, and these
 * cells carry text from the outside: handset names, SIP peers, a
 * registrar's reason phrase. Elements pass through unchanged.
 */
function text(c) {
	return c == null ? [] : Array.isArray(c) ? c : [ c ];
}

function table(headers, rows, cls) {
	var t = E('table', { 'class': 'table' + (cls ? ' ' + cls : '') });

	if (headers)
		t.appendChild(E('tr', { 'class': 'tr table-titles' }, headers.map(function(h) {
			return E('th', { 'class': 'th' }, text(h));
		})));
	rows.forEach(function(r) {
		t.appendChild(E('tr', { 'class': 'tr' }, r.map(function(c, i) {
			return E('td', { 'class': 'td', 'style': !headers && i == 0 ? 'width:33%' : null }, text(c));
		})));
	});
	return t;
}

function yesno(v) {
	return v == null ? '-' : v ? _('on') : _('off');
}

function svg(tag, attrs, children) {
	var n = document.createElementNS('http://www.w3.org/2000/svg', tag);

	Object.keys(attrs || {}).forEach(function(k) { n.setAttribute(k, attrs[k]); });
	(children || []).forEach(function(c) { n.appendChild(c); });
	return n;
}

/*
 * The ten carriers' levels as the chip reports them on this connection,
 * the one in use highlighted: which part of the band is busy or quiet.
 */
function carrierChart(r) {
	var w = 360, h = 140, pad = 24, bw = (w - pad) / 10;
	var kids = [];

	(r.rssi || []).forEach(function(raw, c) {
		var d = rawDbm(raw);
		var v = d == null ? 0 : Math.max(0, Math.min(1, (d + 110) / 90));
		var bh = v * (h - 30);

		kids.push(svg('rect', {
			x: pad + c * bw + 3, y: h - 18 - bh, width: bw - 6, height: bh,
			fill: c == r.carrier ? '#2a7ab0' : '#9ab'
		}));
		kids.push(svg('text', {
			x: pad + c * bw + bw / 2, y: h - 4, 'font-size': 11,
			'text-anchor': 'middle', fill: 'currentColor'
		}, [ document.createTextNode(String(c)) ]));
		if (d != null) {
			kids.push(svg('title', {}, [ document.createTextNode(
				_('Carrier %d (%s MHz): %.1f dBm').format(c, freq(c), d)) ]));
		}
	});
	[ -20, -65, -110 ].forEach(function(db) {
		var y = h - 18 - (db + 110) / 90 * (h - 30);

		kids.push(svg('text', { x: 0, y: y + 4, 'font-size': 10, fill: 'currentColor' },
			[ document.createTextNode(String(db)) ]));
		kids.push(svg('line', { x1: pad, x2: w, y1: y, y2: y, stroke: '#ccc',
			'stroke-dasharray': '2,3' }));
	});
	return E('div', {}, [
		E('div', {}, _('Level per carrier, dBm (in use: dark)')),
		svg('svg', { width: w, height: h, viewBox: '0 0 ' + w + ' ' + h }, kids)
	]);
}

/* Level (dBm) and quality (%) over the last HISTORY_S seconds. */
function historyChart(hist) {
	var w = 360, h = 140, pad = 28, now = Date.now() / 1000;

	function x(t) { return pad + (w - pad) * (1 - (now - t) / HISTORY_S); }
	function line(key, lo, hi, colour) {
		var pts = hist.filter(function(p) { return p[key] != null; }).map(function(p) {
			var v = Math.max(lo, Math.min(hi, p[key]));

			return x(p.t).toFixed(1) + ',' +
				(h - 18 - (v - lo) / (hi - lo) * (h - 30)).toFixed(1);
		});
		return svg('polyline', { points: pts.join(' '), fill: 'none',
			stroke: colour, 'stroke-width': 2 });
	}

	var kids = [
		svg('line', { x1: pad, x2: w, y1: h - 18, y2: h - 18, stroke: '#ccc' }),
		svg('text', { x: pad, y: h - 4, 'font-size': 10, fill: 'currentColor' },
			[ document.createTextNode(_('-%d min').format(HISTORY_S / 60)) ]),
		svg('text', { x: w, y: h - 4, 'font-size': 10, fill: 'currentColor',
			'text-anchor': 'end' }, [ document.createTextNode(_('now')) ]),
		line('dbm', -110, -20, '#2a7ab0'),
		line('q', 0, 100, '#d08020')
	];
	return E('div', {}, [
		E('div', {}, [
			E('span', { 'style': 'color:#2a7ab0' }, _('level (-110 to -20 dBm)')), ', ',
			E('span', { 'style': 'color:#d08020' }, _('quality (0 to 100 %)'))
		]),
		svg('svg', { width: w, height: h, viewBox: '0 0 ' + w + ' ' + h }, kids)
	]);
}

function remember(l) {
	var r = l.radio, now = Date.now() / 1000, hist;

	if (!r)
		return [];
	hist = history[l.handle] = (history[l.handle] || []).filter(function(p) {
		return now - p.t <= HISTORY_S;
	});
	hist.push({ t: now, dbm: r.dbm != null ? r.dbm : null,
		    q: r.quality != null ? r.quality : null });
	return hist;
}

function handsetName(st, l) {
	var h = (st.handsets || []).filter(function(h) { return h.slot == l.slot; })[0];
	return h ? h.name : _('DECT %d').format(l.slot);
}

function connection(st, l) {
	var r = l.radio, rows, cards;

	rows = [
		[ _('Call'), (CALL[l.call] || l.call) +
			(l.call != 'idle' ? ' (' + (l.incoming ? _('incoming') : _('from the handset')) + ')' : '') ],
		[ _('Codec'), CODEC[l.codec] || (l.basic_service >> 4 == 2 ? _('list access, no speech') : '-') ],
		[ _('Encryption'), l.ciphered ? _('on') : _('off') ]
	];
	if (r) {
		rows.push([ _('Carrier'), _('%d (%s MHz)').format(r.carrier, freq(r.carrier)) ]);
		rows.push([ _('Time slot'), String(r.slot) ]);
		rows.push([ _('Field strength'), bars(r.dbm) ]);
		rows.push([ _('Quality'), r.quality != null ? _('%d %%').format(r.quality) : '-' ]);
		if (r.call_frames != null) {
			rows.push([ _('Connection time'), duration(r.call_frames / 100) ]);
			rows.push([ _('Frames'), String(r.call_frames) ]);
		}
	} else {
		rows.push([ _('Radio figures'), _('only during a speech call') ]);
	}

	cards = [ table(null, rows) ];
	if (r && r.events)
		cards.push(table([ _('Errors in the last interval'), '' ], EVENTS.map(function(e) {
			return [ e[1], String(r.events[e[0]] != null ? r.events[e[0]] : '-') ];
		})));
	if (r) {
		cards.push(E('div', { 'style': 'display:flex;flex-wrap:wrap;gap:2em;margin-top:1em' }, [
			carrierChart(r), historyChart(remember(l))
		]));
	}
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, text(handsetName(st, l) == _('DECT %d').format(l.slot) ?
			_('Connection: DECT %d').format(l.slot) :
			_('Connection: %s (DECT %d)').format(handsetName(st, l), l.slot)))
	].concat(cards));
}

function renderState(st) {
	if (!st)
		return E('p', {}, _('No figures from the base.'));

	var bySlot = {}, on = st.radio === 'up';
	if (on)
		(st.links || []).forEach(function(l) { bySlot[l.slot] = l; });

	var handsets = (st.handsets || []).slice().sort(function(a, b) {
		return a.slot - b.slot;
	}).map(function(h) {
		var l = bySlot[h.slot], r = l && l.radio;

		return [
			h.name,
			String(h.slot),
			h.ipei || '-',
			h.part3 == null ? '-' : h.part3 ? _('CAT-iq') : _('GAP'),
			h.profile ? (modelNames && modelNames[h.profile]) || h.profile : _('standard'),
			!on ? _('base off') : h.link ? (l ? (CALL[l.call] || l.call) : _('connected')) : _('idle'),
			h.link ? _('now') : ago(h.seen_ms_ago),
			r ? _('%d (%s MHz)').format(r.carrier, freq(r.carrier)) : '-',
			r ? bars(r.dbm) : '-',
			r && r.quality != null ? _('%d %%').format(r.quality) : '-'
		];
	});

	var out = [
		E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Base')),
			table(null, [
				[ _('Radio'), on ? _('on') : _('parked') ],
				[ _('Registration'), st.registration !== 'open' ? _('closed') :
					st.registration_left_s != null ?
						_('open, %d s left').format(st.registration_left_s) : _('open') ],
				[ _('Encryption'), yesno(st.cipher) ],
				[ _('Handset check on reporting in'), yesno(st.locauth) ],
				[ _('Wideband audio'), yesno(st.wideband) ],
				[ _('Second call'), yesno(st.twocalls) ],
				[ _('Chip firmware'), st.stub || '-' ],
				[ _('Running for'), st.uptime_ms != null ? duration(st.uptime_ms / 1000) : '-' ],
				[ _('openDECTcell interface'), st.api != null ? _('API %d').format(st.api) : '-' ]
			])
		]),
		E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Handsets')),
			table([ _('Name'), _('No.'), _('IPEI'), _('Type'), _('Model'), _('State'), _('Last contact'),
				_('Carrier'), _('Field strength'), _('Quality') ], handsets)
		])
	];

	if (on)
		(st.links || []).forEach(function(l) {
			if (l.call != 'idle' || l.radio)
				out.push(connection(st, l));
		});

	/* forget the graphs of connections that are gone */
	Object.keys(history).forEach(function(k) {
		if (!on || !(st.links || []).some(function(l) { return String(l.handle) == k && l.radio; }))
			delete history[k];
	});
	return E('div', {}, out);
}

/*
 * The service as procd sees it. 'main' is the DECT daemon's instance,
 * 'baresip' the SIP side's; the reason when it is not running comes from
 * what the settings say, so "switched off" and "failed" read differently.
 */
function anyLine() {
	return uci.sections('opendectcell', 'handset').some(function(h) {
		return !!(h.line1 || h.line2);
	});
}

function serviceRows(inst, st, voip, sip) {
	var on = uci.get('opendectcell', 'main', 'enabled') == '1';
	var d = inst.instance1, b = inst.baresip, rows = [];
	var dRun = d && d.running, bRun = b && b.running;
	var fresh = st && st.radio === 'up';

	if (!sip)
		rows.push([ _('DECT service'),
			dRun ? (fresh ? E('strong', {}, _('running')) :
				_('running, the radio is starting or parked')) :
			!on ? _('switched off (Network > DECT, General: Enable)') :
			d ? _('stopped after repeated failures: see the Log tab') :
			    _('not running although switched on: see the Log tab') ]);
	else {
		rows.push([ _('SIP client (baresip)'),
			bRun ? E('strong', {}, _('running')) :
			!dRun ? _('not running: it runs with the DECT service') :
			!anyLine() ? _('not running: no handset has a VoIP profile as a line (Network > DECT, Handsets)') :
			b ? _('stopped after repeated failures: see the Log tab') :
			    _('not running: is the package opendectcell-baresip installed?') ]);
		if (bRun)
			rows.push([ _('Bridge to the DECT base'),
				voip && voip.base ? _('connected') : _('not connected (yet)') ]);
	}
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, _('Service')), table(null, rows) ]);
}

/* Line n of DECT hs, by the VoIP profile it is */
function lineName(hs, line) {
	var p = uci.get('opendectcell', 'dect' + hs, 'line' + line);
	return _('Line %d').format(line) + (p ? ' (' + (uci.get('opendectcell', p, 'name') || p) + ')' : '');
}

function hsName(st, hs) {
	var h = st && (st.handsets || []).filter(function(x) { return x.slot == hs; })[0];
	return h ? h.name : _('DECT %d').format(hs);
}

var REG = {
	registered: _('registered'), registering: _('registering…'),
	failed: _('failed'), unregistering: _('signing off'),
	off: _('off: calls by address only'), 'no account': _('no account loaded')
};

function kbps(bps) {
	return bps == null ? '-' : _('%.1f kbit/s').format(bps / 1000);
}

function voipCall(st, c) {
	var codec = c.codec ? c.codec + (c.srate ? ' (' + _('%d kHz').format(c.srate / 1000) + ')' : '') +
		(c.codec_rx ? ' / ' + c.codec_rx : '') : '-';

	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, text(_('Call: %s, %s').format(hsName(st, c.hs), lineName(c.hs, c.line)))),
		table(null, [
			[ _('Direction'), c.direction == 'out' ? _('outgoing') : _('incoming') ],
			[ _('Other party'), c.peer || '-' ],
			[ _('State'), c.state || '-' ],
			[ _('Duration'), c.duration_s != null ? duration(c.duration_s) : '-' ],
			[ _('Codec'), codec ],
			[ _('Signalling'), (c.transport || '-').toUpperCase() ],
			[ _('Media, this end'), c.local || '-' ],
			[ _('Media, other end'), c.remote || '-' ],
			[ _('Data rate, sending'), kbps(c.tx_bps) ],
			[ _('Data rate, receiving'), kbps(c.rx_bps) ],
			[ _('Packets sent / received'), c.tx_packets != null ?
				'%d / %d'.format(c.tx_packets, c.rx_packets) : '-' ],
			[ _('Packets lost (receiving)'), c.rx_lost != null ? String(c.rx_lost) : '-' ],
			[ _('Jitter (receiving)'), c.rx_jitter_us != null ?
				_('%.1f ms').format(c.rx_jitter_us / 1000) : '-' ],
			[ _('Round trip'), c.rtt_us ? _('%.1f ms').format(c.rtt_us / 1000) : '-' ],
			[ _('Encryption'), c.secure ? E('strong', {}, text(_('on (%s)').format(c.encryption))) :
				c.encryption && c.encryption != 'none' ?
					_('%s asked for, not in use').format(c.encryption) : _('off') ]
		])
	]);
}

function renderVoip(st, voip, inst) {
	var out = [ serviceRows(inst, st, voip, true) ];

	if (!voip || !(inst.baresip && inst.baresip.running))
		return E('div', {}, out);
	out.push(E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, _('Lines')),
		(voip.lines || []).length ? table([ _('Handset'), _('Line (VoIP profile)'), _('Account'),
			_('Registration'), _('Interval') ], voip.lines.map(function(l) {
			var r = REG[l.registration] || l.registration;

			return [
				hsName(st, l.hs), lineName(l.hs, l.line), l.aor,
				l.registration == 'registered' ? E('strong', {}, text(r)) :
					l.reason ? _('%s: %s').format(r, l.reason) : r,
				l.interval_s ? _('%d s').format(l.interval_s) : '-'
			];
		})) : E('p', {}, _('No handset has a VoIP profile as a line.'))
	]));
	if ((voip.calls || []).length)
		voip.calls.forEach(function(c) { out.push(voipCall(st, c)); });
	else
		out.push(E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Calls')), E('p', {}, _('No call right now.')) ]));
	return E('div', {}, out);
}

function renderDect(st, inst) {
	return E('div', {}, [ serviceRows(inst, st, null, false), renderState(st) ]);
}

/*
 * The log lines of the base and its SIP side. logread has no "this or
 * that" filter, so the whole system log is read and these kept: the
 * daemons, the service's own messages (logger tag openDECTcell) and procd's
 * about either instance.
 */
var LOG_KIND = [
	[ /\bdect-stubd\[\d+\]:|procd: .*opendectcell::instance1/, 'dect' ],
	[ /\bbaresip\[\d+\]:|procd: .*opendectcell::baresip/, 'voip' ],
	[ /\bopendectcell(\[\d+\])?:/, 'both' ]
];
var LOG_MAX = 1000;		/* lines kept on the page */
var logWhich = 'all';

function readLog() {
	return L.resolveDefault(fs.exec_direct('/sbin/logread'), '').then(function(text) {
		var out = [];

		(text || '').split('\n').forEach(function(l) {
			l = l.replace(/\x1b\[[0-9;]*m/g, '');	/* baresip's colours */
			for (var i = 0; i < LOG_KIND.length; i++)
				if (LOG_KIND[i][0].test(l)) {
					out.push([ LOG_KIND[i][1], l ]);
					break;
				}
		});
		return out.slice(-LOG_MAX);
	});
}

function logText(lines) {
	return lines.filter(function(x) {
		return logWhich == 'all' || x[0] == 'both' || x[0] == logWhich;
	}).map(function(x) { return x[1]; }).join('\n') || _('No log lines from the base or its SIP side.');
}

/* Shown again; scrolled to the end unless the reader has scrolled up. */
function showLog(area, lines) {
	var atEnd = area.scrollTop + area.clientHeight >= area.scrollHeight - 20;
	var text = logText(lines), left = area.scrollLeft;

	/* unchanged text stays put: a selection for copying survives */
	if (area.value == text)
		return;
	area.value = text;
	area.scrollLeft = left;
	if (atEnd)
		area.scrollTop = area.scrollHeight;
}

function renderLogTab(lines) {
	var area = E('textarea', {
		'id': 'opendectcell-log', 'readonly': 'readonly', 'wrap': 'off',
		'style': 'width:100%;height:32em;font-family:monospace;font-size:12px'
	});
	var which = E('select', {
		'class': 'cbi-input-select',
		'change': function(ev) {
			logWhich = ev.target.value;
			showLog(area, area._lines || []);
		}
	}, [
		E('option', { 'value': 'all' }, _('DECT and VoIP')),
		E('option', { 'value': 'dect' }, _('DECT only')),
		E('option', { 'value': 'voip' }, _('VoIP only'))
	]);

	area._lines = lines;
	area.value = logText(lines);
	requestAnimationFrame(function() { area.scrollTop = area.scrollHeight; });
	return E('div', { 'class': 'cbi-section' }, [
		E('div', { 'style': 'margin-bottom:.5em' }, [ _('Show') + ' ', which, ' ',
			E('span', { 'class': 'cbi-value-description' },
				_('The newest %d lines from the system log, refreshed every 2 seconds while this tab is open.').format(LOG_MAX)) ]),
		area
	]);
}

function fill(node, child) {
	if (!node)
		return;
	while (node.firstChild)
		node.removeChild(node.firstChild);
	node.appendChild(child);
}

return view.extend({
	load: function() {
		return Promise.all([ readAll(), readLog() ]).then(function(r) {
			return r[0].concat([ r[1] ]);	/* readAll()'s five, then the log */
		});
	},

	render: function(r) {
		var dect = E('div', { 'id': 'opendectcell-monitor' }, renderDect(r[0], r[2]));
		var voip = E('div', { 'id': 'opendectcell-voip' }, renderVoip(r[0], r[1], r[2]));
		var logs = E('div', { 'data-tab': 'log', 'data-tab-title': _('Log') },
			[ renderLogTab(r[5]) ]);
		var tabs = E('div', {}, [
			E('div', { 'data-tab': 'dect', 'data-tab-title': _('DECT') }, [ dect ]),
			E('div', { 'data-tab': 'voip', 'data-tab-title': _('VoIP') }, [ voip ]),
			logs
		]);

		poll.add(function() {
			var area = document.getElementById('opendectcell-log');

			return Promise.all([
				readAll(),
				logs.getAttribute('data-tab-active') == 'true' ? readLog() : null
			]).then(function(x) {
				var r = x[0];

				fill(document.getElementById('opendectcell-monitor'), renderDect(r[0], r[2]));
				fill(document.getElementById('opendectcell-voip'), renderVoip(r[0], r[1], r[2]));
				if (x[1] && area) {
					area._lines = x[1];
					showLog(area, x[1]);
				}
			});
		}, 2);

		/* the tab menu goes before the panes' parent: it needs one */
		var page = E('div', {}, [
			E('h2', {}, _('DECT Monitor')),
			E('div', { 'class': 'cbi-map-descr' },
				_('Live figures of the DECT base and its phone lines, and their log, refreshed every 2 seconds. Settings, handsets and registration: Network > DECT.')),
			tabs
		]);
		ui.tabs.initTabGroup(tabs.childNodes);
		return page;
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
