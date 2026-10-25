'use strict';
'require view';
'require fs';
'require poll';

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

/* RSSI byte, larger is stronger; the absolute scale is not known yet. */
function level(link) {
	var r = link.radio;
	if (!r || !r.rssi || r.carrier >= r.rssi.length)
		return '-';
	return '0x' + ('0' + r.rssi[r.carrier].toString(16)).slice(-2);
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
			l && l.radio ? String(l.radio.carrier) : '-',
			l ? level(l) : '-'
		];
	});

	return E('div', {}, [
		E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Base')),
			table([ _('Radio'), _('Registration'), _('Chip firmware') ], [[
				st.radio === 'up' ? _('on') : _('parked'),
				st.registration === 'open' ? _('open') : _('closed'),
				st.stub
			]])
		]),
		E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Handsets')),
			table([ _('Name'), _('Slot'), _('State'), _('Last contact'),
				_('Carrier'), _('Level') ], handsets)
		])
	]);
}

return view.extend({
	load: readState,

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

		return E('div', {}, [
			E('h2', {}, _('DECT Monitor')),
			box
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
