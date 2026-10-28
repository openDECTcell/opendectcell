'use strict';
'require view';
'require fs';
'require poll';
'require ui';
'require uci';

/*
 * Status > DECT, the DECT Monitor: what the base is doing, for someone who
 * should not have to read a log. Reads /tmp/opendectcell-state.json, which
 * dect-stubd rewrites at most once a second (docs/diagnostics.md).
 *
 * Per connection it shows what AVM's DECT Monitor shows and we have
 * decoded from the chip's connection status: carrier and frequency, time
 * slot, field strength, quality, the eight error counters, frames and call
 * time [CR A-0121, A-0123]; plus the levels of all ten carriers and a live
 * graph of level and quality, kept in the browser while the page is open.
 * Deliberately not a debugging console: raw mails stay in dect-stubd -v.
 */
var STATE = '/tmp/opendectcell-state.json';
var HISTORY_S = 300;		/* the graph's span */
var history = {};		/* handle -> [{t, dbm, q}] */

function readState() {
	return fs.read(STATE).then(function(text) {
		try { return JSON.parse(text); } catch (e) { return null; }
	}).catch(function() { return null; });
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

function table(headers, rows, cls) {
	var t = E('table', { 'class': 'table' + (cls ? ' ' + cls : '') });

	if (headers)
		t.appendChild(E('tr', { 'class': 'tr table-titles' }, headers.map(function(h) {
			return E('th', { 'class': 'th' }, h);
		})));
	rows.forEach(function(r) {
		t.appendChild(E('tr', { 'class': 'tr' }, r.map(function(c, i) {
			return E('td', { 'class': 'td', 'style': !headers && i == 0 ? 'width:33%' : null }, c);
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
		E('h3', {}, handsetName(st, l) == _('DECT %d').format(l.slot) ?
			_('Connection: DECT %d').format(l.slot) :
			_('Connection: %s (DECT %d)').format(handsetName(st, l), l.slot))
	].concat(cards));
}

function renderState(st) {
	if (!st)
		return E('p', {}, _('No state: dect-stubd is not running, or the service is disabled (Network > DECT).'));

	var bySlot = {}, on = st.radio === 'up';
	if (on)
		(st.links || []).forEach(function(l) { bySlot[l.slot] = l; });

	var handsets = (st.handsets || []).map(function(h) {
		var l = bySlot[h.slot], r = l && l.radio;

		return [
			h.name,
			String(h.slot),
			h.part3 == null ? '-' : h.part3 ? _('CAT-iq') : _('GAP'),
			h.profile || _('standard'),
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
				[ _('Line'), st.line || '-' ],
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
			table([ _('Name'), _('No.'), _('Type'), _('Profile'), _('State'), _('Last contact'),
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

return view.extend({
	load: function() {
		return Promise.all([ readState(), uci.load('opendectcell').catch(function() {}) ])
			.then(function(r) { return r[0]; });
	},

	render: function(st) {
		var box = E('div', { 'id': 'opendectcell-monitor' }, renderState(st));

		poll.add(function() {
			return readState().then(function(s) {
				var node = document.getElementById('opendectcell-monitor');
				if (!node)
					return;
				while (node.firstChild)
					node.removeChild(node.firstChild);
				node.appendChild(renderState(s));
			});
		}, 2);

		/*
		 * Registration is closed unless asked for: this opens it for the
		 * service's regwindow (120 s) and the daemon closes it again
		 * after one handset enrolled (/etc/init.d/opendectcell register).
		 */
		var reg = E('button', {
			'class': 'cbi-button cbi-button-action',
			'click': ui.createHandlerFn(this, function() {
				return fs.exec('/etc/init.d/opendectcell', [ 'register' ])
					.then(function(r) {
						ui.addNotification(null, E('p', r.code == 0 ?
							_('Registration is open. Start the registration on the handset now.') :
							_('Could not open registration: is the DECT service running?')),
							r.code == 0 ? 'info' : 'warning');
					});
			})
		}, _('Register a handset'));

		return E('div', {}, [
			E('h2', {}, _('DECT Monitor')),
			E('div', { 'class': 'cbi-map-descr' },
				_('Live figures of the DECT base, refreshed every 2 seconds. Settings and handset management: Network > DECT.')),
			box,
			E('div', { 'class': 'cbi-page-actions' }, [
				reg, ' ',
				E('span', {}, _('Registration PIN: %s').format(
					uci.get('opendectcell', 'main', 'pin') || '-'))
			])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
