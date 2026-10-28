'use strict';
'require view';
'require form';
'require fs';
'require ui';
'require uci';

/*
 * Network > DECT: the base's settings and its handsets (/etc/config/opendectcell).
 * Save & Apply restarts the service (procd reload trigger), so a call in
 * progress ends.
 *
 * Handsets: one 'config handset dect<N>' section per registered handset,
 * edited in a pop-up as the Wireless page edits an SSID: General (its name
 * and profile, kept where the base keeps them, and its own settings, a
 * 'list override key=value' laid over the profile; the keys come from
 * dect-stubd --profile-keys), Line 1 and Line 2 (its SIP accounts, for the
 * bridge). Renaming and deregistering need the base running; they go to the
 * daemon over its control socket (docs/bridge-interface.md §2).
 */
var STATE = '/tmp/opendectcell-state.json';
var CTL = '/usr/sbin/dect-stubd';
var PROFILES = '/etc/opendectcell/profiles';

function readState() {
	return fs.read(STATE).then(function(text) {
		try { return JSON.parse(text); } catch (e) { return null; }
	}).catch(function() { return null; });
}

function readKeys() {
	return fs.exec(CTL, [ '--profile-keys' ]).then(function(r) {
		try { return JSON.parse(r.stdout || '[]'); } catch (e) { return []; }
	}).catch(function() { return []; });
}

/*
 * The handset profiles with their pretty names: each file's id, kind
 * (manufacturer or device), name and the manufacturer it extends.
 */
function readProfiles() {
	return L.resolveDefault(fs.list(PROFILES), []).then(function(files) {
		return Promise.all(files.filter(function(f) {
			return f.type == 'file' && /\.profile$/.test(f.name);
		}).map(function(f) {
			return L.resolveDefault(fs.read(PROFILES + '/' + f.name), '').then(function(t) {
				var p = { id: f.name.replace(/\.profile$/, '') };

				(t || '').split('\n').forEach(function(l) {
					var m = l.match(/^\s*(id|kind|name|extends)\s*=\s*"?([^"#]*?)"?\s*(#.*)?$/);
					if (m)
						p[m[1]] = m[2];
				});
				p.name = p.name || p.id;
				return p;
			});
		}));
	}).then(function(list) {
		return list.sort(function(a, b) { return a.name.localeCompare(b.name); });
	});
}

/* A random PIN of n digits (rejection sampling keeps every digit equally likely). */
function randomPin(n) {
	var out = '', b = new Uint8Array(1);

	while (out.length < n) {
		window.crypto.getRandomValues(b);
		if (b[0] < 250)
			out += String(b[0] % 10);
	}
	return out;
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
 * for this verb, or reason 'not-running' when nothing answered.
 */
function ctl(line) {
	var verb = line.split(' ')[0];

	return fs.exec(CTL, [ '--ctl', line ]).then(function(r) {
		var out = r.stdout || '', m;

		if (new RegExp('^ok verb=' + verb + '\\b', 'm').test(out))
			return { ok: true };
		m = out.match(new RegExp('^error verb=' + verb + '\\b.*reason=(\\S+)', 'm'));
		if (!m && /^error reason=busy/m.test(out))
			return { ok: false, reason: 'busy' };	/* every connection taken */
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
	'not-ready': _('the base is still starting up'),
	'bad-profile': _('the base does not know that handset model'),
	'busy': _('the base is busy with other requests; try again in a moment')
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

function deregisterDialog(h) {
	ui.showModal(_('Deregister DECT %d (%s)?').format(h.slot, h.name), [
		E('p', {}, _('The base erases this handset\'s registration. The handset is told if it is in range; to use it again it has to be registered anew. A handset that is lost or switched off is erased after about 30 seconds without its confirmation.')),
		E('p', {}, _('Its settings here go with it, its lines (SIP accounts) too.')),
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
			E('button', {
				'class': 'btn cbi-button-negative',
				'click': function() {
					ui.hideModal();
					return ctl('deregister hs=' + h.slot).then(function(r) {
						if (!r.ok)
							return report(r, '');
						return deregisterDone(h);
					});
				}
			}, _('Deregister'))
		])
	]);
}

/*
 * After the base has let the handset go (it pages it first, up to about
 * 30 s): its section and lines are removed and applied. Not before: the
 * apply restarts the service, which would cut the deregistration short.
 */
function deregisterDone(h) {
	var t0 = Date.now();

	ui.showModal(_('Deregistering DECT %d').format(h.slot), [
		E('p', { 'class': 'spinning' }, _('Waiting for the base to let the handset go…'))
	]);
	function gone() {
		return readState().then(function(st) {
			var still = st && (st.handsets || []).some(function(x) { return x.slot == h.slot; });

			if (still && Date.now() - t0 < 40000)
				return new Promise(function(res) { window.setTimeout(res, 2000); }).then(gone);
			ui.hideModal();
			if (still) {
				ui.addNotification(null, E('p', _('DECT %d is still registered after 40 s; its settings are kept. See Status > DECT, Log.').format(h.slot)), 'warning');
				return;
			}
			uci.remove('opendectcell', 'dect' + h.slot);
			return uci.save().then(function() {
				ui.addNotification(null, E('p', _('DECT %d deregistered; its settings and lines are removed.').format(h.slot)), 'info');
				return ui.changes.apply(true);
			});
		});
	}
	return gone();
}

/* The override list of a section, as { key: value }. */
function overrides(sid) {
	var o = {};

	L.toArray(uci.get('opendectcell', sid, 'override')).forEach(function(kv) {
		var i = kv.indexOf('=');
		if (i > 0)
			o[kv.substr(0, i)] = kv.substr(i + 1);
	});
	return o;
}

function setOverride(sid, key, value) {
	var list = L.toArray(uci.get('opendectcell', sid, 'override')).filter(function(kv) {
		return kv.indexOf(key + '=') != 0;
	});

	if (value != null && value !== '')
		list.push(key + '=' + value);
	uci.set('opendectcell', sid, 'override', list.length ? list : null);
}

var GROUPS = {
	call: _('Calls'), link: _('Radio link, registration and lists'),
	report: _('What the handset reports'), feature: _('Features')
};

/* The handset's settings, one field per key of the profile format. */
function addKeyOptions(ss, keys) {
	var seen = {};

	keys.forEach(function(k) {
		var o, name = '_k_' + k.key.replace(/[^a-z0-9]/g, '_');

		if (!seen[k.group]) {
			seen[k.group] = true;
			o = ss.taboption('general', form.DummyValue, '_g_' + k.group);
			o.modalonly = true;
			o.render = function() {
				return E('h4', { 'style': 'margin-top:1em' }, GROUPS[k.group] || k.group);
			};
		}

		if (k.type == 'words') {
			o = ss.taboption('general', form.ListValue, name, k.label);
			o.value('', _('as the profile says (%s)').format(
				k['default'] == '-' ? _('from the handset') : k['default']));
			k.values.forEach(function(v) {
				if (v != '-')
					o.value(v, v);
			});
		} else {
			o = ss.taboption('general', form.Value, name, k.label);
			o.placeholder = k.type == 'int' ? k['default'] : _('as the profile says');
			o.validate = function(sid, v) {
				if (v === '')
					return true;
				if (k.type == 'int') {
					var n = /^0x[0-9a-f]+$/i.test(v) ? parseInt(v, 16) :
						/^[0-9]+$/.test(v) ? parseInt(v, 10) : NaN;
					return n >= k.min && n <= k.max ? true :
						_('%d to %d (or 0x..)').format(k.min, k.max);
				}
				return /^hex:[0-9a-fA-F {}a-z]+$/.test(v) ? true : _('hex:... with {text} or {len}');
			};
		}
		o.modalonly = true;
		o.optional = true;
		o.cfgvalue = function(sid) { return overrides(sid)[k.key] || ''; };
		o.write = function(sid, v) { setOverride(sid, k.key, v); };
		o.remove = function(sid) { setOverride(sid, k.key, null); };
	});
}

/* What a baresip account line can carry in a parameter. */
function sipSafe(sid, v) {
	return /[;"<>\s]/.test(v) ? _('baresip cannot take ; " < > or spaces here') : true;
}

/*
 * Sub-tabs inside a pop-up tab. LuCI forms have no nested tabs, so each
 * option is noted with its sub-tab here and the rendered pop-up is
 * regrouped (subTabs); LuCI's own tab code then draws and switches them,
 * and hides a sub-tab whose options are all hidden.
 */
var SUBTAB = {}, SUBTABS = {};

function subTabs(els) {
	Array.prototype.forEach.call(els.childNodes, function(pane) {
		var list = pane.getAttribute && SUBTABS[pane.getAttribute('data-tab')], panes = {}, box;

		if (!list)
			return;
		box = E('div', { 'class': 'cbi-section-node-tabbed' });
		/* the first is active from the start: LuCI's tab code fails on a
		 * group whose panes are all empty, as when the line is off */
		list.forEach(function(t, i) {
			panes[t[0]] = E('div', { 'data-tab': t[0], 'data-tab-title': t[1],
						 'data-tab-active': i == 0 ? 'true' : 'false' });
			box.appendChild(panes[t[0]]);
		});
		Array.prototype.slice.call(pane.children).forEach(function(n) {
			var t = SUBTAB[n.getAttribute('data-name')];
			if (t)
				panes[t].appendChild(n);
		});
		pane.appendChild(box);
	});
	return els;
}

/*
 * A SIP account on tab t, its options prefixed p ('l1_', 'l2_'): what a
 * baresip account line takes, grouped as the provider's documents group it.
 */
function addLineOptions(ss, t, p, optional, n) {
	var on = p + 'enabled', sub, o;

	SUBTABS[t] = [];
	function group(id, title) {
		sub = p + id;
		SUBTABS[t].push([ sub, title ]);
	}

	function opt(type, name, title, descr) {
		var x = ss.taboption(t, type, p + name, title, descr);

		SUBTAB[p + name] = sub;
		x.modalonly = true;
		x.depends(on, '1');
		return x;
	}

	/* shown only when cond holds too (separate depends() calls are ORed) */
	function only(x, cond) {
		x.deps = [];
		x.depends(Object.assign({ [on]: '1' }, cond));
		return x;
	}

	o = ss.taboption(t, form.Flag, on, _('Use this line'),
		optional ? _('An optional second SIP account for this handset.') :
			   _('The handset\'s SIP account: its calls go out on it and calls to it ring this handset.'));
	o.modalonly = true;

	group('acct', _('Account'));
	o = opt(form.Value, 'name', _('Line name'),
		_('What CAT-iq handsets show for this line, at most 15 characters.'));
	o.placeholder = _('Line %d').format(n);
	o.validate = function(sid, v) {
		return octets(v) <= 15 ? true : _('At most 15 characters');
	};
	opt(form.Value, 'display', _('Display name'), _('The name the called party sees.'));
	o = opt(form.Value, 'user', _('User name'), _('The SIP user, often the phone number.'));
	o.rmempty = false;
	o.validate = sipSafe;
	o = opt(form.Value, 'authuser', _('Login name'),
		_('When the provider wants a login other than the user name.'));
	o.optional = true;
	o.validate = sipSafe;
	o = opt(form.Value, 'password', _('Password'));
	o.password = true;
	o.validate = sipSafe;
	o = opt(form.Value, 'domain', _('Registrar'),
		_('The provider\'s SIP server, host or host:port (e.g. sip.example.net, or 192.168.178.1 for a FRITZ!Box).'));
	o.datatype = 'or(hostport,host)';
	o.rmempty = false;
	o = opt(form.Flag, 'register', _('Register'),
		_('Register with the server. Off for a peer that calls this handset directly by address.'));
	o.default = '1';
	o = opt(form.Value, 'regint', _('Registration interval'), _('Seconds between registrations.'));
	o.datatype = 'range(60,86400)';
	o.placeholder = '3600';
	only(o, { [p + 'register']: '1' });

	group('net', _('Server and NAT'));
	o = opt(form.ListValue, 'transport', _('Transport'),
		_('Use TLS if your provider supports it: it encrypts the signalling, so your password and the numbers you call do not cross the internet in the clear (the provider names the port, often 5061). Encrypted speech (Encryption tab) is only private with TLS here.'));
	o.value('udp', 'UDP');
	o.value('tcp', 'TCP');
	o.value('tls', _('TLS (encrypted signalling)'));
	o.default = 'udp';
	o = opt(form.Value, 'outbound', _('Outbound proxy'), _('Only when the provider names one.'));
	o.datatype = 'or(hostport,host)';
	o.optional = true;
	o = opt(form.Value, 'outbound2', _('Second outbound proxy'));
	o.datatype = 'or(hostport,host)';
	o.optional = true;
	o = opt(form.Flag, 'sipnat', _('SIP outbound (RFC 5626)'),
		_('Keep the connection to the server open through NAT, when the provider supports it.'));
	o = opt(form.ListValue, 'medianat', _('NAT traversal for media'));
	o.value('', _('none'));
	o.value('stun', 'STUN');
	o.value('turn', 'TURN');
	o.value('ice', 'ICE');
	o = opt(form.Value, 'stunserver', _('STUN/TURN server'), _('host or host:port.'));
	o.datatype = 'or(hostport,host)';
	only(o, { [p + 'medianat']: /./ });
	o = opt(form.Value, 'stunuser', _('TURN user'));
	only(o, { [p + 'medianat']: 'turn' });
	o = opt(form.Value, 'stunpass', _('TURN password'));
	o.password = true;
	o.validate = sipSafe;
	only(o, { [p + 'medianat']: 'turn' });
	o = opt(form.ListValue, 'rtpkeep', _('Keep media open'),
		_('Packets that keep a NAT binding open while one side is silent.'));
	o.value('', _('off'));
	o.value('zero', _('empty RTP packets'));
	o.value('stun', 'STUN');
	o.value('dyna', _('dynamic'));
	o.value('rtcp', 'RTCP');

	group('media', _('Media'));
	o = opt(form.Flag, 'media_auto', _('Try to autodetect'),
		_('Offers every codec, HD first, and sends key tones the way the other side asks. Right for nearly every provider; untick to set them by hand.'));
	o.default = '1';
	o.rmempty = false;
	function manual(x, flag) {
		return only(x, { [p + flag]: '0' });
	}
	o = manual(opt(form.MultiValue, 'codec', _('Codecs'),
		_('Offered in the order shown. G.722 keeps the handset\'s wideband (HD) audio end to end.')), 'media_auto');
	o.value('g722', 'G.722 (HD)');
	o.value('pcma', 'G.711 A-law');
	o.value('pcmu', 'G.711 µ-law');
	o.default = 'g722 pcma pcmu';
	o = manual(opt(form.ListValue, 'ptime', _('Packet time'), _('Milliseconds of audio per RTP packet.')), 'media_auto');
	[ '20', '10', '30', '40' ].forEach(function(v) { o.value(v, v + ' ms'); });
	o.default = '20';
	o = manual(opt(form.ListValue, 'dtmf', _('Key tones (DTMF)')), 'media_auto');
	o.value('rtpevent', _('RTP events (RFC 4733)'));
	o.value('info', _('SIP INFO'));
	o.value('auto', _('automatic'));
	o.default = 'rtpevent';
	o = manual(opt(form.ListValue, 'rel100', _('Reliable provisional responses (100rel)')), 'media_auto');
	o.value('yes', _('when the other side supports it'));
	o.value('no', _('never'));
	o.value('required', _('required'));
	o.default = 'yes';
	o = opt(form.Flag, 'mwi', _('Voicemail notification (MWI)'));
	o = opt(form.Flag, 'transfer', _('Allow call transfer'));
	o.default = '1';

	group('enc', _('Encryption'));
	o = opt(form.Flag, 'enc_auto', _('Try to autodetect'),
		_('Offers encrypted speech (SRTP) and falls back to plain speech when the other side does not take it. Untick to choose a method, or none.'));
	o.default = '1';
	o.rmempty = false;
	o = manual(opt(form.ListValue, 'mediaenc', _('Media encryption'),
		_('Encrypts the speech on the internet side. Use TLS as transport too, or the keys travel in the clear.')), 'enc_auto');
	o.value('', _('none'));
	o.value('srtp', _('SRTP when offered'));
	o.value('srtp-mand', _('SRTP required'));
	o.value('srtp-mandf', _('SRTP required (in the offer)'));
	o.value('dtls_srtp', 'DTLS-SRTP');
	o.value('zrtp', 'ZRTP');

	group('adv', _('Advanced'));
	o = opt(form.Value, 'extra', _('Further account parameters'),
		_('Appended to the account as is, baresip syntax: ;key=value;key=value.'));
	o.optional = true;
	o.validate = function(sid, v) {
		return v === '' || /^(;[a-z0-9_]+=[^;\s"<>]+)+$/.test(v) ? true :
			_(';key=value pairs, no spaces');
	};
}

function lineSummary(sid, p) {
	var u = uci.get('opendectcell', sid, p + 'user'), d = uci.get('opendectcell', sid, p + 'domain');

	if (uci.get('opendectcell', sid, p + 'enabled') != '1')
		return '-';
	return (u || '?') + '@' + (d || '?');
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('opendectcell'),
			readState(),
			readProfiles(),
			readKeys()
		]).then(function(r) {
			var st = r[1], profiles = r[2];

			/* a section for every registered handset that has none yet */
			((st && st.handsets) || []).forEach(function(h) {
				var sid = 'dect' + h.slot;

				if (!uci.get('opendectcell', sid)) {
					uci.add('opendectcell', 'handset', sid);
					uci.set('opendectcell', sid, 'slot', String(h.slot));
				}
			});

			return Promise.all(((st && st.handsets) || []).map(function(h) {
				return L.resolveDefault(fs.read(h.store + '.profile'), '').then(function(t) {
					return (t || '').trim();
				});
			})).then(function(choices) {
				return [ st, profiles, choices, r[3] ];
			});
		});
	},

	render: function(data) {
		var st = data[0], profiles = data[1], choices = data[2], keys = data[3];
		var running = st && st.radio === 'up';
		var bySlot = {}, choiceBySlot = {};
		var m, s, o, ss;

		((st && st.handsets) || []).forEach(function(h, i) {
			bySlot[h.slot] = h;
			choiceBySlot[h.slot] = choices[i];
		});
		function hs(sid) {
			return bySlot[+uci.get('opendectcell', sid, 'slot')];
		}

		m = new form.Map('opendectcell', _('DECT'),
			_('The base station for DECT handsets. Saving restarts it with the new settings; a call in progress ends. Live figures are on Status > DECT.'));

		s = m.section(form.NamedSection, 'main', 'opendectcell');
		s.addremove = false;
		s.tab('general', _('General'));
		s.tab('handsets', _('Handsets'));
		s.tab('voip', _('VoIP'));
		s.tab('advanced', _('Advanced'));

		/* General */
		o = s.taboption('general', form.Flag, 'enabled', _('Enable the DECT base'),
			_('Boots the DECT chip and puts the base on the air. Needs the chip firmware (Advanced).'));
		o.rmempty = false;

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

		/* Handsets: one row each, edited in a pop-up */
		o = s.taboption('handsets', form.SectionValue, '_handsets', form.GridSection, 'handset',
			null, running ? null :
			_('The DECT service is not running: renaming and deregistering need it, and handsets registered later appear here once it runs.'));
		ss = o.subsection;
		ss.anonymous = true;
		ss.addremove = false;
		ss.nodescriptions = true;
		ss.modaltitle = function(sid) {
			var h = hs(sid);
			return _('Handset DECT %s').format(uci.get('opendectcell', sid, 'slot')) +
				(h && h.name != 'DECT ' + h.slot ? ' (' + h.name + ')' : '');
		};
		ss.tab('general', _('General'));
		ss.tab('line1', _('Line 1'));
		ss.tab('line2', _('Line 2'));
		ss.addModalOptions = function(ms) {
			var render = ms.renderTabContainers;
			ms.renderTabContainers = function(sid, nodes) {
				return subTabs(render.call(this, sid, nodes));
			};
		};

		ss.renderRowActions = function(sid) {
			var td = form.GridSection.prototype.renderRowActions.apply(this, [ sid, _('Edit') ]);
			var h = hs(sid), box = td.querySelector('.cbi-section-actions') || td;

			if (h)
				box.appendChild(E('button', {
					'class': 'btn cbi-button-negative',
					'disabled': running ? null : '',
					'title': running ? null : _('Needs the DECT service running'),
					'click': function(ev) { ev.preventDefault(); deregisterDialog(h); }
				}, _('Deregister')));
			return td;
		};

		/* the grid's columns */
		o = ss.option(form.DummyValue, 'slot', _('No.'));
		o.modalonly = false;
		o = ss.option(form.DummyValue, '_name', _('Name'));
		o.modalonly = false;
		o.cfgvalue = function(sid) {
			var h = hs(sid);
			return h ? h.name : st ? _('not registered') : _('unknown: the service is off');
		};
		o = ss.option(form.DummyValue, '_ipei', _('IPEI'));
		o.modalonly = false;
		o.cfgvalue = function(sid) { var h = hs(sid); return h && h.ipei || '-'; };
		o = ss.option(form.DummyValue, '_type', _('Type'));
		o.modalonly = false;
		o.cfgvalue = function(sid) {
			var h = hs(sid);
			return !h || h.part3 == null ? '-' : h.part3 ? _('CAT-iq') : _('GAP');
		};
		o = ss.option(form.DummyValue, '_l1', _('Line 1'));
		o.modalonly = false;
		o.cfgvalue = function(sid) { return lineSummary(sid, 'l1_'); };
		o = ss.option(form.DummyValue, '_l2', _('Line 2'));
		o.modalonly = false;
		o.cfgvalue = function(sid) { return lineSummary(sid, 'l2_'); };
		o = ss.option(form.DummyValue, '_seen', _('Last contact'));
		o.modalonly = false;
		o.cfgvalue = function(sid) {
			var h = hs(sid);
			return !h ? '-' : !running ? _('base off') :
				h.link ? _('connected now') : ago(h.seen_ms_ago);
		};

		/* General: name and profile where the base keeps them */
		o = ss.taboption('general', form.Value, '_rename', _('Name'),
			running ? _('What every handset shows for this one, at most 15 characters. Empty restores "DECT n".') :
				  _('Can only be changed while the DECT service is running.'));
		o.modalonly = true;
		o.readonly = !running;
		o.cfgvalue = function(sid) { var h = hs(sid); return h ? h.name : ''; };
		o.validate = function(sid, v) {
			return octets(v) <= 15 ? true : REASONS['bad-name'];
		};
		o.write = function(sid, v) {
			var h = hs(sid);
			if (!h || v == h.name)
				return;
			return ctl('rename hs=' + h.slot + ' name=' + quote(v))
				.then(function(r) { report(r, _('DECT %d renamed.').format(h.slot)); });
		};
		o.remove = function(sid) { return this.write(sid, ''); };

		o = ss.taboption('general', form.ListValue, '_profile', _('Profile'),
			_('Known quirks of this handset model; its own settings below override it.'));
		o.modalonly = true;
		o.value('', _('automatic (standard)'));
		profiles.filter(function(p) { return p.kind == 'manufacturer'; }).forEach(function(mf) {
			o.value(mf.id, _('%s (other models)').format(mf.name));
			profiles.filter(function(p) { return p.extends == mf.id; }).forEach(function(p) {
				o.value(p.id, p.name);
			});
		});
		profiles.filter(function(p) {
			return p.kind != 'manufacturer' && !profiles.some(function(m) { return m.id == p.extends; });
		}).forEach(function(p) { o.value(p.id, p.name); });
		o.cfgvalue = function(sid) { return choiceBySlot[+uci.get('opendectcell', sid, 'slot')] || ''; };
		o.write = function(sid, v) {
			var h = hs(sid);
			if (!h || v == (choiceBySlot[h.slot] || ''))
				return;
			var w = v ? fs.write(h.store + '.profile', v + '\n')
				  : fs.remove(h.store + '.profile').catch(function() {});
			return w.then(function() { return ctl('reload-profiles'); });
		};
		o.remove = function(sid) { return this.write(sid, ''); };

		addKeyOptions(ss, keys);
		addLineOptions(ss, 'line1', 'l1_', false, 1);
		addLineOptions(ss, 'line2', 'l2_', true, 2);

		/*
		 * Handsets: registering a new one, under the grid. The model
		 * picked first becomes its profile once it has enrolled
		 * (register-window profile=, docs/bridge-interface.md §2.2).
		 */
		var mfrs = profiles.filter(function(p) { return p.kind == 'manufacturer'; });

		function formval(name) {
			var opt = m.lookupOption(name, 'main');
			return opt ? opt[0].formvalue('main') : null;
		}

		function notSaved(o) {
			o.cfgvalue = function() { return ''; };
			o.write = o.remove = function() {};
			return o;
		}

		o = notSaved(s.taboption('handsets', form.ListValue, '_reg_mfr', _('Manufacturer'),
			_('Of the handset you are about to register. A known model gets the settings it needs from the start.')));
		o.value('', _('Generic DECT GAP/CAT-iq handset'));
		mfrs.forEach(function(mf) { o.value(mf.id, mf.name); });

		mfrs.forEach(function(mf) {
			o = notSaved(s.taboption('handsets', form.ListValue, '_reg_model_' + mf.id, _('Model')));
			o.depends('_reg_mfr', mf.id);
			profiles.filter(function(p) { return p.extends == mf.id; }).forEach(function(p) {
				o.value(p.id, p.name);
			});
			o.value(mf.id, _('another %s model').format(mf.name));
		});

		o = s.taboption('handsets', form.Value, 'pin', _('Registration PIN'),
			_('The PIN a handset asks for when it registers. Random; "New PIN" makes another.'));
		o.rmempty = false;
		/*
		 * 8 digits unless fewer were chosen on purpose: a PIN that does
		 * not match the length (one made before the choice existed) is
		 * offered as a new 8-digit one, saved with the page.
		 */
		o.default = randomPin(+(uci.get('opendectcell', 'main', 'pin_length') || 8));
		o.cfgvalue = function(sid) {
			var v = uci.get('opendectcell', sid, 'pin') || '',
			    n = +(uci.get('opendectcell', sid, 'pin_length') || 8);
			return v.length == n && /^[0-9]+$/.test(v) ? v : null;
		};
		o.validate = function(id, v) {
			var n = +(formval('pin_length') || 8);
			return new RegExp('^[0-9]{' + n + '}$').test(v) ? true :
				_('%d digits, as PIN length says').format(n);
		};

		o = s.taboption('handsets', form.Button, '_newpin', ' ');
		o.inputtitle = _('New PIN');
		o.onclick = function() {
			var pin = m.lookupOption('pin', 'main')[0];
			pin.getUIElement('main').setValue(randomPin(+(formval('pin_length') || 8)));
			pin.triggerValidation('main');
		};

		o = s.taboption('handsets', form.ListValue, 'pin_length', _('PIN length'),
			_('8 digits are the most DECT allows and the hardest to guess. Choose fewer only for a handset that cannot take 8.'));
		o.value('8', _('8 digits'));
		o.value('6', _('6 digits'));
		o.value('4', _('4 digits'));
		o.default = '8';
		o.onchange = function(ev, sid, v) {
			var pin = m.lookupOption('pin', 'main')[0];
			pin.getUIElement('main').setValue(randomPin(+v));
			pin.triggerValidation('main');
		};

		o = s.taboption('handsets', form.Button, '_register', _('New handset'),
			running ? _('Opens registration for the registration window; start the registration on the handset then and enter the PIN below.') :
				  _('Needs the DECT service running.'));
		o.readonly = !running;
		o.inputtitle = _('Register a handset');
		o.inputstyle = 'action';
		o.onclick = function() {
			var mf = formval('_reg_mfr'), prof = mf ? formval('_reg_model_' + mf) || mf : '';

			/* the base registers with the saved PIN, not one only on the page */
			if (formval('pin') != uci.get('opendectcell', 'main', 'pin')) {
				ui.addNotification(null, E('p', _('The PIN shown is not in use yet: press Save & Apply first, then register.')), 'warning');
				return;
			}

			return ctl('register-window' + (prof ? ' profile=' + prof : '')).then(function(r) {
				report(r, _('Registration is open for %s s. Start the registration on the handset now and enter the PIN %s.')
					.format(uci.get('opendectcell', 'main', 'regwindow') || '120',
						uci.get('opendectcell', 'main', 'pin') || '-'));
			});
		};

		o = s.taboption('handsets', form.Value, 'regwindow', _('Registration window'),
			_('Seconds that "Register a handset" keeps registration open.'));
		o.datatype = 'range(10,3600)';
		o.placeholder = '120';

		o = s.taboption('handsets', form.Flag, 'registration', _('Keep registration open'),
			_('Registration stays open while the base runs. Not recommended: anyone nearby who knows the PIN can register a handset.'));

		/* VoIP: what every handset's lines share (baresip's config) */
		o = s.taboption('voip', form.Value, 'sip_port', _('SIP port'),
			_('The local port for SIP signalling (UDP and TCP; TLS on the next port).'));
		o.datatype = 'port';
		o.placeholder = '5060';
		o = s.taboption('voip', form.Value, 'rtp_ports', _('RTP port range'),
			_('Local ports for the speech, first-last.'));
		o.placeholder = '10000-20000';
		o.validate = function(id, v) {
			var m = /^(\d+)-(\d+)$/.exec(v);
			return v === '' || (m && +m[1] >= 1024 && +m[1] < +m[2] && +m[2] <= 65535) ? true :
				_('first-last, 1024 to 65535');
		};
		o = s.taboption('voip', form.Value, 'net_interface', _('Network interface'),
			_('Where SIP and speech go out, e.g. wan or br-lan. Empty: the default route.'));
		o.placeholder = _('automatic');
		o = s.taboption('voip', form.Value, 'dns_server', _('DNS server'),
			_('For the SIP servers\' names. Empty: the system\'s.'));
		o.datatype = 'or(ipaddr,hostport)';
		o.optional = true;
		o = s.taboption('voip', form.ListValue, 'rtp_dscp', _('Speech priority (DSCP)'),
			_('Marks the speech packets so routers can prefer them.'));
		o.value('46', _('EF (46), the usual for voice'));
		o.value('34', 'AF41 (34)');
		o.value('0', _('none (0)'));
		o.default = '46';
		o = s.taboption('voip', form.ListValue, 'sip_dscp', _('Signalling priority (DSCP)'));
		o.value('24', 'CS3 (24)');
		o.value('26', 'AF31 (26)');
		o.value('0', _('none (0)'));
		o.default = '24';
		o = s.taboption('voip', form.Value, 'jitter', _('Jitter buffer'),
			_('Milliseconds of speech held back to smooth out the network, min-max.'));
		o.placeholder = '20-160';
		o.validate = function(id, v) {
			var m = /^(\d+)-(\d+)$/.exec(v);
			return v === '' || (m && +m[1] <= +m[2] && +m[2] <= 1000) ? true : _('min-max, up to 1000');
		};
		o = s.taboption('voip', form.Flag, 'tls_verify', _('Check the server\'s certificate'),
			_('For lines with TLS transport. Off accepts any certificate, which defeats the encryption.'));
		o.default = '1';
		o.rmempty = false;
		o = s.taboption('voip', form.Value, 'tls_cafile', _('Certificate authorities'),
			_('A PEM file of the CAs to trust; empty: the system\'s (ca-bundle).'));
		o.placeholder = '/etc/ssl/certs/ca-certificates.crt';
		o.depends('tls_verify', '1');

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
