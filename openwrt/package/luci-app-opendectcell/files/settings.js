'use strict';
'require view';
'require form';
'require fs';
'require ui';
'require uci';

/*
 * Network > DECT: the base's settings (/etc/config/opendectcell) and its
 * handsets. Save & Apply restarts the service (procd reload trigger), so a
 * call in progress ends. The handset actions go to the running daemon over
 * its control socket (dect-stubd --ctl, docs/bridge-interface.md §2) and
 * take effect at once; they need the service running.
 */
var STATE = '/tmp/opendectcell-state.json';
var CTL = '/usr/sbin/dect-stubd';
var PROFILES = '/etc/opendectcell/profiles';

function readState() {
	return fs.read(STATE).then(function(text) {
		try { return JSON.parse(text); } catch (e) { return null; }
	}).catch(function() { return null; });
}

/* A value for the control socket's line format: quoted, \ and " escaped. */
function quote(v) {
	return '"' + String(v).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
}

/* UTF-8 octets, the unit of the handsets' 15-octet name fields. */
function octets(v) {
	return unescape(encodeURIComponent(v)).length;
}

/*
 * One command to the daemon. Resolves to { ok, reason }: the reply line
 * for this verb ("ok verb=..." or "error verb=... reason=..."), or
 * reason 'not-running' when nothing answered.
 */
function ctl(line) {
	var verb = line.split(' ')[0];

	return fs.exec(CTL, [ '--ctl', line ]).then(function(r) {
		var out = r.stdout || '', m;

		if (new RegExp('^ok verb=' + verb + '\\b', 'm').test(out))
			return { ok: true };
		m = out.match(new RegExp('^error verb=' + verb + '\\b.*reason=(\\S+)', 'm'));
		return { ok: false, reason: m ? m[1] : 'not-running' };
	}).catch(function() {
		return { ok: false, reason: 'not-running' };
	});
}

var REASONS = {
	'not-running': _('the DECT service is not running'),
	'no-handset': _('the base does not hold that handset'),
	'bad-name': _('the name is too long (at most 15 octets) or not valid text'),
	'bad-hs': _('no such handset number'),
	'not-ready': _('the base is still starting up')
};

function report(r, done) {
	ui.addNotification(null, E('p', r.ok ? done :
		_('Not done: %s.').format(REASONS[r.reason] || r.reason)),
		r.ok ? 'info' : 'warning');
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

/* The handset's choice file next to its subscription store. */
function choicePath(h) {
	return h.store + '.profile';
}

function renameDialog(h) {
	var input = E('input', {
		'class': 'cbi-input-text', 'type': 'text', 'value': h.name,
		'maxlength': 15
	});

	ui.showModal(_('Rename DECT %d').format(h.slot), [
		E('p', {}, _('The name every handset shows for this one, at most 15 characters. Empty restores "DECT %d".').format(h.slot)),
		input,
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
			E('button', {
				'class': 'btn cbi-button-action',
				'click': function() {
					var v = input.value.trim();

					if (octets(v) > 15) {
						ui.addNotification(null, E('p', REASONS['bad-name']), 'warning');
						return;
					}
					ui.hideModal();
					return ctl('rename hs=' + h.slot + ' name=' + quote(v))
						.then(function(r) { report(r, _('DECT %d renamed.').format(h.slot)); });
				}
			}, _('Rename'))
		])
	]);
}

function deregisterDialog(h) {
	ui.showModal(_('Deregister DECT %d (%s)?').format(h.slot, h.name), [
		E('p', {}, _('The base erases this handset\'s registration. The handset is told if it is in range; to use it again it has to be registered anew. A handset that is lost or switched off is erased after about 30 seconds without its confirmation.')),
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
			E('button', {
				'class': 'btn cbi-button-negative',
				'click': function() {
					ui.hideModal();
					return ctl('deregister hs=' + h.slot)
						.then(function(r) { report(r, _('Deregistering DECT %d.').format(h.slot)); });
				}
			}, _('Deregister'))
		])
	]);
}

function profileSelect(h, profiles, current) {
	var sel = E('select', { 'class': 'cbi-input-select' }, [
		E('option', { 'value': '' }, _('automatic (standard)'))
	].concat(profiles.map(function(p) {
		return E('option', { 'value': p, 'selected': p == current ? '' : null }, p);
	})));

	sel.addEventListener('change', function() {
		var v = sel.value, w = v ? fs.write(choicePath(h), v + '\n')
					 : fs.remove(choicePath(h)).catch(function() {});

		return w.then(function() { return ctl('reload-profiles'); })
			.then(function(r) {
				report(r, v ? _('DECT %d now uses the profile %s.').format(h.slot, v)
					    : _('DECT %d uses the standard settings again.').format(h.slot));
			}).catch(function(e) {
				ui.addNotification(null, E('p', _('Could not save the profile choice: %s').format(e)), 'warning');
			});
	});
	return sel;
}

function handsetsTable(st, profiles, choices) {
	if (!st || st.radio !== 'up')
		return E('p', {}, _('The DECT service is not running. Enable it on the General tab and save, then come back here to manage handsets.'));

	var t = E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('No.')),
			E('th', { 'class': 'th' }, _('Name')),
			E('th', { 'class': 'th' }, _('Type')),
			E('th', { 'class': 'th' }, _('Profile')),
			E('th', { 'class': 'th' }, _('Last contact')),
			E('th', { 'class': 'th cbi-section-actions' }, '')
		])
	]);

	(st.handsets || []).forEach(function(h, i) {
		t.appendChild(E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td' }, String(h.slot)),
			E('td', { 'class': 'td' }, h.name),
			E('td', { 'class': 'td' }, h.part3 == null ? _('not yet known') :
				h.part3 ? _('CAT-iq') : _('GAP')),
			E('td', { 'class': 'td' }, profileSelect(h, profiles, choices[i])),
			E('td', { 'class': 'td' }, h.link ? _('connected now') : ago(h.seen_ms_ago)),
			E('td', { 'class': 'td cbi-section-actions' }, [
				E('button', { 'class': 'btn cbi-button-edit',
					'click': function() { renameDialog(h); } }, _('Rename')), ' ',
				E('button', { 'class': 'btn cbi-button-negative',
					'click': function() { deregisterDialog(h); } }, _('Deregister'))
			])
		]));
	});
	if (!(st.handsets || []).length)
		t.appendChild(E('tr', { 'class': 'tr placeholder' }, [
			E('td', { 'class': 'td' }, _('No handset is registered yet.'))
		]));

	return E('div', {}, [
		t,
		E('div', { 'class': 'cbi-page-actions' }, [
			E('button', {
				'class': 'btn cbi-button-action',
				'click': ui.createHandlerFn(this, function() {
					return fs.exec('/etc/init.d/opendectcell', [ 'register' ]).then(function(r) {
						ui.addNotification(null, E('p', r.code == 0 ?
							_('Registration is open for %s s. Start the registration on the handset now and enter the PIN %s.')
								.format(uci.get('opendectcell', 'main', 'regwindow') || '120',
									uci.get('opendectcell', 'main', 'pin') || '-') :
							REASONS['not-running']), r.code == 0 ? 'info' : 'warning');
					});
				})
			}, _('Register a handset'))
		])
	]);
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('opendectcell'),
			readState(),
			L.resolveDefault(fs.list(PROFILES), [])
		]).then(function(r) {
			var st = r[1];
			var profiles = r[2].filter(function(f) {
				return f.type == 'file' && /\.profile$/.test(f.name);
			}).map(function(f) { return f.name.replace(/\.profile$/, ''); }).sort();

			return Promise.all(((st && st.handsets) || []).map(function(h) {
				return L.resolveDefault(fs.read(choicePath(h)), '').then(function(t) {
					return (t || '').trim();
				});
			})).then(function(choices) {
				return [ st, profiles, choices ];
			});
		});
	},

	render: function(data) {
		var st = data[0], profiles = data[1], choices = data[2];
		var m, s, o;

		m = new form.Map('opendectcell', _('DECT'),
			_('The base station for DECT handsets. Saving restarts it with the new settings; a call in progress ends. Live figures are on Status > DECT.'));

		s = m.section(form.NamedSection, 'main', 'opendectcell');
		s.addremove = false;
		s.tab('general', _('General'));
		s.tab('handsets', _('Handsets'));
		s.tab('advanced', _('Advanced'));

		/* General */
		o = s.taboption('general', form.Flag, 'enabled', _('Enable the DECT base'),
			_('Boots the DECT chip and puts the base on the air. Needs the chip firmware (Advanced).'));
		o.rmempty = false;

		o = s.taboption('general', form.Value, 'pin', _('Registration PIN'),
			_('The PIN a handset asks for when it registers, 4 to 8 digits.'));
		o.password = true;
		o.validate = function(id, v) {
			return /^[0-9]{4,8}$/.test(v) ? true : _('4 to 8 digits');
		};

		o = s.taboption('general', form.Value, 'regwindow', _('Registration window'),
			_('Seconds that "Register a handset" keeps registration open.'));
		o.datatype = 'range(10,3600)';
		o.placeholder = '120';

		o = s.taboption('general', form.Flag, 'registration', _('Keep registration open'),
			_('Registration stays open while the base runs. Not recommended: anyone nearby who knows the PIN can register a handset.'));

		o = s.taboption('general', form.Value, 'line_name', _('Line name'),
			_('The line\'s name on CAT-iq handsets, at most 15 characters.'));
		o.placeholder = 'Line 1';
		o.validate = function(id, v) {
			if (v.indexOf(',') >= 0)
				return _('No commas');
			return octets(v) <= 15 ? true : _('At most 15 characters');
		};

		o = s.taboption('general', form.Flag, 'cipher', _('Encrypt calls'),
			_('DECT standard encryption on every call and list session. Switch off only to debug audio.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('general', form.Flag, 'locauth', _('Check handsets when they report in'),
			_('Each handset proves its key whenever it reports in to the base; a handset that cannot is turned away.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('general', form.Flag, 'wideband', _('Wideband audio (HD)'),
			_('G.722 when the handset offers it; off keeps every call narrowband.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('general', form.Flag, 'twocalls', _('Second call'),
			_('A second handset may call while another is in a call.'));
		o.rmempty = false;

		o = s.taboption('general', form.Flag, 'overlap', _('Collect dialled digits'),
			_('Wait for the number to be complete before connecting.'));
		o.default = '1';
		o.rmempty = false;

		o = s.taboption('general', form.Value, 'dialwait', _('Dialling pause'),
			_('Milliseconds after the last digit before the number counts as complete.'));
		o.datatype = 'range(1000,10000)';
		o.placeholder = '3000';
		o.depends('overlap', '1');

		/* Handsets: live, from the running daemon */
		o = s.taboption('handsets', form.DummyValue, '_handsets');
		o.render = function() {
			return E('div', { 'class': 'cbi-value' }, handsetsTable(st, profiles, choices));
		};

		/* Advanced */
		o = s.taboption('advanced', form.Value, 'firmware_dir', _('Chip firmware directory'),
			_('Where the two DECT chip firmware files from a FRITZ!OS image are kept (never shipped with opendectcell).'));
		o.placeholder = '/etc/opendectcell/firmware';
		o = s.taboption('advanced', form.Value, 'first', _('First-stage firmware'));
		o.placeholder = 'dectfw_firstlevel_441_slow.hex';
		o = s.taboption('advanced', form.Value, 'second', _('Second-stage firmware'));
		o.placeholder = 'dectfw_secondlevel_441-07.56.hex';
		o = s.taboption('advanced', form.Flag, 'fw_allow_unverified', _('Allow unknown firmware'),
			_('Run a chip firmware build opendectcell does not know. For developers.'));
		o = s.taboption('advanced', form.Value, 'uart', _('DECT UART'));
		o.placeholder = '/dev/ttyMSM0';
		o = s.taboption('advanced', form.Value, 'factory_mtd', _('Factory partition'),
			_('Holds the base\'s DECT identity and calibration.'));
		o.placeholder = '/dev/mtd5';
		o = s.taboption('advanced', form.Value, 'factory_offset', _('Factory offset'));
		o.placeholder = '0x3d000';
		o = s.taboption('advanced', form.Value, 'mode', _('Chip mode word'));
		o.placeholder = '0x105';
		o = s.taboption('advanced', form.Value, 'stores', _('Handset stores'),
			_('One file per registered handset.'));
		o.placeholder = '/etc/opendectcell-sub.*';
		o = s.taboption('advanced', form.Value, 'options', _('Extra options'),
			_('Further dect-stubd -a options, comma-separated.'));

		return m.render();
	}
});
