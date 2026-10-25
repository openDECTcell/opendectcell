'use strict';
'require view';
'require fs';
'require poll';
'require ui';
'require uci';

/*
 * The DECT Monitor: what the base is doing, for someone who should not have
 * to read a log. Reads /tmp/opendectcell-state.json, which dect-stubd rewrites
 * at most once a second (docs/diagnostics.md). Deliberately not a debugging
 * console: raw mails stay in dect-stubd -v.
 */
var STATE = '/tmp/opendectcell-state.json';

function readState() {
	return fs.read(STATE).then(function(text) {
		try { return JSON.parse(text); } catch (e) { return null; }
	}).catch(function() { return null; });
}

function ago(ms) {
	if (ms == null)
		return _('never');
	var s = Math.round(ms / 1000);
	if (s < 60)
		return _('%d s ago').format(s);
	if (s < 3600)
		return _('%d min ago').format(Math.round(s / 60));
	return _('%d h ago').format(Math.round(s / 3600));
}

/*
 * The chip's raw RSSI of the call's carrier, as FRITZ!OS converts it on
 * the 7520 (clean room A-0121): dBm = (raw - 20) * 80/37 - 110; 0 and 255
 * carry no value. The bars follow AVM's own quality steps (>-40 dBm 100 %,
 * then 10 % per 5 dB down to 20 %), two steps per bar.
 */
function dbm(link) {
	var r = link.radio;
	if (!r || !r.rssi || r.carrier >= r.rssi.length)
		return null;
	var raw = r.rssi[r.carrier];
	if (raw == 0 || raw == 255)
		return null;
	return (raw - 20) * 80 / 37 - 110;
}

function level(link) {
	var d = dbm(link);
	if (d == null)
		return '-';
	var bars = d > -40 ? 5 : d > -50 ? 4 : d > -60 ? 3 : d > -70 ? 2 : 1;
	return '\u25ae'.repeat(bars) + '\u25af'.repeat(5 - bars) + ' ' +
		_('%.1f dBm').format(d);
}

/* DECT carrier c, EN 300 175-2: 1897.344 MHz - c * 1.728 MHz */
function channel(link) {
	var r = link.radio;
	if (!r || r.carrier == null)
		return '-';
	return _('%d (%.1f MHz)').format(r.carrier, 1897.344 - 1.728 * r.carrier);
}

function callText(link) {
	var names = {
		idle: _('idle'), proceeding: _('setting up'),
		connecting: _('connecting'), active: _('in a call'),
		released: _('released'), calling: _('being called'),
		ringing: _('ringing'), dialling: _('dialling')
	};
	return names[link.call] || link.call;
}

function table(headers, rows) {
	var t = E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, headers.map(function(h) {
			return E('th', { 'class': 'th' }, h);
		}))
	]);
	rows.forEach(function(r) {
		t.appendChild(E('tr', { 'class': 'tr' }, r.map(function(c) {
			return E('td', { 'class': 'td' }, c);
		})));
	});
	return t;
}

function renderState(st) {
	if (!st)
		return E('p', {}, _('No state: dect-stubd is not running, or the service is disabled (/etc/config/opendectcell).'));

	var bySlot = {}, on = st.radio === 'up';
	if (on)
		(st.links || []).forEach(function(l) { bySlot[l.slot] = l; });

	var handsets = (st.handsets || []).map(function(h) {
		var l = bySlot[h.slot];
		return [
			h.name,
			String(h.slot),
			!on ? _('base off') :
			h.link ? (l ? callText(l) : _('connected')) : _('idle'),
			ago(h.seen_ms_ago),
			l ? channel(l) : '-',
			l ? level(l) : '-',
			l && l.radio && l.radio.quality != null ?
				_('%d %%').format(l.radio.quality) : '-'
		];
	});

	return E('div', {}, [
		E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Base')),
			table([ _('Radio'), _('Registration'), _('Chip firmware') ], [[
				st.radio === 'up' ? _('on') : _('parked'),
				st.registration !== 'open' ? _('closed') :
				st.registration_left_s != null ?
					_('open, %d s left').format(st.registration_left_s) :
					_('open'),
				st.stub
			]])
		]),
		E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Handsets')),
			table([ _('Name'), _('Slot'), _('State'), _('Last contact'),
				_('Channel'), _('Signal Strength'), _('Quality') ], handsets)
		])
	]);
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
