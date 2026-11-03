// SPDX-License-Identifier: GPL-2.0-or-later
'use strict';
'require view';
'require form';
'require fs';
'require ui';
'require uci';
'require rpc';
'require request';
'require dom';

/*
 * Network > DECT: the base's settings and its handsets (/etc/config/opendectcell).
 * Save & Apply restarts the service (procd reload trigger), so a call in
 * progress ends.
 *
 * Handsets: one 'config handset dect<N>' section per registered handset,
 * edited in a pop-up as the Wireless page edits an SSID: General (its name
 * and model profile, kept where the base keeps them, and its Line 1 and
 * Line 2, each a VoIP profile), then its own DECT settings in three tabs,
 * a 'list override key=value' laid over the model profile (the keys come
 * from dect-stubd --profile-keys). Renaming and deregistering need the
 * base running; they go to the daemon over its control socket
 * (docs/bridge-interface.md §2).
 *
 * VoIP Profiles: one 'config voip' section per SIP account; a handset's
 * option line1/line2 names one. A profile may be a line of several
 * handsets -- a call to it rings them all -- but not both lines of one.
 *
 * Cell mode (option role 'cell', package opendectcell-cell): the box is a cell
 * of an openDECTcell controller, which runs the base, its handsets and lines.
 * The page then shows only the controller link and how to leave; the
 * base's own settings stay in the config, unused, for when it does.
 *
 * Phonebook (package opendectcell-carddav, 'config carddav'): the handsets'
 * Contact List from a CardDAV address book, both ways; shown only when the
 * package is installed, with the last sync's result from carddav.json.
 */
var STATE = '/var/run/opendectcell/state.json';
var VOIP = '/var/run/opendectcell-sip/voip.json';
var CTL = '/usr/sbin/dect-stubd';
var PROFILES = '/etc/opendectcell/profiles';
var BOARDS = '/usr/share/opendectcell/boards/';
var INIT = '/etc/init.d/opendectcell';
var CELL_BIN = '/usr/sbin/dect-cell';
var LINK = '/var/run/opendectcell/cell-link.json';
var CARDDAV = '/usr/share/opendectcell/carddav.uc';
var CARDDAV_STATE = '/var/run/opendectcell/carddav.json';
var CARDDAV_BOOKS = '/var/run/opendectcell/carddav-books.json';
var CARDDAV_REQ = '/tmp/opendectcell-carddav-discover.json';
var FW = '/usr/share/opendectcell/firmware.sh';
var FW_TMP = '/tmp/opendectcell-fw-upload.';
var FW_MAX = 400000;	/* the box's check refuses more; the two files are under 200 KB */

var callServices = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

/*
 * Save & Apply reloads the service through its procd trigger, which exists
 * only while procd knows the service: after "/etc/init.d/opendectcell stop" it
 * does not, and saving "Enable the DECT base" did nothing (2026-10-30, the
 * maintainer). So after an apply, a service procd does not know is
 * started; with the base disabled that only registers the trigger.
 */
function startIfStopped() {
	return L.resolveDefault(callServices('opendectcell'), {}).then(function(res) {
		if (!res || !res.opendectcell)
			return fs.exec(INIT, [ 'start' ]);
	});
}

/*
 * The address books of the CardDAV account: "Fetch address books" asks the
 * server (carddav.uc discover; the form's URL and credentials go over in a
 * root-only file, not on a command line), then one row per book: used or
 * not, and the one new contacts go to. Stored as 'list book' and 'option
 * default' in the carddav section. The value this option compares is
 * "DEFAULT|BOOK BOOK ...".
 */
var BookList = form.Value.extend({
	__name__: 'CBI.OpenDECTcellBooks',

	books: null,		/* [ { href, name } ], the rows offered */

	cfgvalue: function() {
		var b = L.toArray(uci.get('opendectcell', 'carddav', 'book'));

		return (uci.get('opendectcell', 'carddav', 'default') || '') + '|' + b.join(' ');
	},

	rows: function(box, sid) {
		var self = this, chosen = {}, def, first;
		var v = this.current || this.cfgvalue(sid), bar = v.indexOf('|');

		v.substr(bar + 1).split(' ').filter(String).forEach(function(h) { chosen[h] = true; });
		first = this.books.filter(function(b) { return chosen[b.href]; })[0];
		def = v.substr(0, bar) || (first ? first.href : null);
		dom.content(box, this.books.length ? E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Use')),
				E('th', { 'class': 'th' }, _('New contacts')),
				E('th', { 'class': 'th' }, _('Address book'))
			])
		].concat(this.books.map(function(b) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, E('input', { 'type': 'checkbox', 'data-href': b.href,
					'class': 'cd-use', 'checked': chosen[b.href] ? '' : null })),
				E('td', { 'class': 'td' }, E('input', { 'type': 'radio', 'name': self.cbid(sid) + '.def',
					'data-href': b.href, 'class': 'cd-def',
					'checked': def == b.href ? '' : null })),
				E('td', { 'class': 'td' }, [ E('strong', {}, b.name || b.href), E('br'),
					E('small', {}, b.href) ])
			]);
		}))) : E('em', {}, _('No address books yet: fill in the server, user name and password, then fetch them.')));
	},

	renderWidget: function(sid) {
		var self = this, box = E('div', { 'class': 'cd-books' });
		var msg = E('span', { 'class': 'cd-msg' });

		this.rows(box, sid);
		return E('div', { 'id': this.cbid(sid) }, [
			box,
			E('p', {}, [
				E('button', { 'class': 'cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, function() {
					var req = {
						url: this.section.formvalue(sid, '_cd_url'),
						username: this.section.formvalue(sid, '_cd_username'),
						password: this.section.formvalue(sid, '_cd_password')
					};

					this.current = this.formvalue(sid);
					dom.content(msg, _('Asking the server…'));
					return fs.write(CARDDAV_REQ, JSON.stringify(req), 384).then(function() {
						return fs.exec(CARDDAV, [ 'discover', CARDDAV_REQ ]);
					}).then(function(r) {
						var res = {};

						try { res = JSON.parse(r.stdout || '{}'); } catch (e) {}
						if (!res.ok) {
							dom.content(msg, E('em', { 'class': 'error' },
								res.error || r.stderr || _('The server could not be asked.')));
							return;
						}
						/* rows: what the server has, then chosen ones it no longer lists */
						self.books.forEach(function(b) {
							if ((self.current || '').indexOf(b.href) >= 0 &&
							    !res.books.some(function(x) { return x.href == b.href; }))
								res.books.push(b);
						});
						self.books = res.books;
						self.rows(box, sid);
						dom.content(msg, _('%d address book(s) found. Tick the ones to use.').format(res.books.length));
					}).catch(function(e) {
						dom.content(msg, E('em', { 'class': 'error' }, e.message));
					});
				}) }, _('Fetch address books')),
				' ', msg
			])
		]);
	},

	formvalue: function(sid) {
		var node = document.getElementById(this.cbid(sid)), use = [], def = null;

		if (!node)
			return this.cfgvalue(sid);
		node.querySelectorAll('input.cd-use').forEach(function(c) {
			if (c.checked)
				use.push(c.getAttribute('data-href'));
		});
		node.querySelectorAll('input.cd-def').forEach(function(r) {
			if (r.checked && use.indexOf(r.getAttribute('data-href')) >= 0)
				def = r.getAttribute('data-href');
		});
		return (def || use[0] || '') + '|' + use.join(' ');
	},

	validate: function(sid) {
		var v = this.formvalue(sid);

		return v.substr(v.indexOf('|') + 1) ? true : _('Choose at least one address book.');
	},

	write: function(sid, v) {
		var bar = v.indexOf('|'), use = v.substr(bar + 1).split(' ').filter(String);

		uci.set('opendectcell', 'carddav', 'book', use);
		uci.set('opendectcell', 'carddav', 'default', v.substr(0, bar));
	},

	remove: function() {
		uci.unset('opendectcell', 'carddav', 'book');
		uci.unset('opendectcell', 'carddav', 'default');
	}
});

function readState() {
	return fs.read(STATE).then(function(text) {
		try { return JSON.parse(text); } catch (e) { return null; }
	}).catch(function() { return null; });
}

function readVoip() {
	return L.resolveDefault(fs.read(VOIP), '').then(function(text) {
		try { return JSON.parse(text); } catch (e) { return null; }
	});
}

/* The changes saved but not applied yet, per config. */
var callChanges = rpc.declare({
	object: 'uci',
	method: 'changes',
	expect: { changes: {} }
});

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

/*
 * The chip settings the service takes when Advanced leaves them empty, as
 * the init script finds them: its own defaults (the 7520's) with the box's
 * board profile, /usr/share/opendectcell/boards/<board_name>, over them.
 */
var CHIP_DEFAULTS = {
	fw_firstlevel: 'dectfw_firstlevel_441_slow.hex',
	fw_secondlevel: 'dectfw_secondlevel_441-07.56.hex',
	uart: '/dev/ttyMSM0',
	factory_mtd: '/dev/mtd5',
	factory_offset: '0x3d000',
	dect_mode: '0x105'
};

function readBoard() {
	return L.resolveDefault(fs.read('/tmp/sysinfo/board_name'), '').then(function(name) {
		name = (name || '').trim();
		return name ? L.resolveDefault(fs.read(BOARDS + name), '') : '';
	}).then(function(text) {
		var b = {};

		Object.keys(CHIP_DEFAULTS).forEach(function(k) { b[k] = CHIP_DEFAULTS[k]; });
		(text || '').split('\n').forEach(function(l) {
			var m = l.match(/^([a-z0-9_]+)="?([^"]*)"?\s*$/);
			if (m && b.hasOwnProperty(m[1]))
				b[m[1]] = m[2];
		});
		return b;
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
	/* control characters out: a line break would end the command */
	return '"' + String(v).replace(/[\x00-\x1f\x7f]/g, '')
		.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
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
	ui.addNotification(null, E('p', {}, [ r.ok ? done :
		_('Not done: %s.').format(REASONS[r.reason] || r.reason) ]),
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

function readLink() {
	return L.resolveDefault(fs.read(LINK), '').then(function(text) {
		try { return JSON.parse(text); } catch (e) { return null; }
	});
}

/* After enrolling or leaving: the page as the new role has it. */
function reloadSoon() {
	window.setTimeout(function() { location.reload(); }, 3000);
}

/*
 * Enrol with a controller: its enrollment string (its web interface makes
 * one per cell), then cell mode. The service restarts as a cell.
 */
function enrollDialog() {
	var input = E('input', {
		'class': 'cbi-input-text', 'type': 'text', 'style': 'width:100%',
		'placeholder': 'opendectcell-enroll:v1?h=…&t=…&ca=…'
	});

	ui.showModal(_('Make this box a cell of a controller'), [
		E('p', {}, _('The controller then runs the DECT base: its handsets, calls and phone lines. This box\'s own handsets, VoIP profiles and settings stay saved but unused, and its SIP client does not run. Leaving cell mode brings them back.')),
		E('p', {}, _('Paste the enrollment string from the controller\'s web interface:')),
		input,
		E('div', { 'class': 'right', 'style': 'margin-top:1em' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
			E('button', {
				'class': 'btn cbi-button-action',
				'click': function() {
					var v = input.value.trim();

					if (v.indexOf('opendectcell-enroll:v1?') != 0) {
						input.classList.add('cbi-input-invalid');
						return;
					}
					ui.showModal(_('Enrolling'), [ E('p', { 'class': 'spinning' },
						_('Asking the controller; this can take up to a minute.')) ]);
					/*
					 * the string carries the one-time token: a root-only
					 * file, so it is not on a command line for every local
					 * user to read; the init script removes the file
					 */
					var rnd = new Uint8Array(8), tmp = '/tmp/opendectcell-enroll.';

					window.crypto.getRandomValues(rnd);
					rnd.forEach(function(b) { tmp += ('0' + b.toString(16)).slice(-2); });
					return fs.write(tmp, v + '\n', 384 /* 0600 */).then(function() {
						return fs.exec(INIT, [ 'enroll', '@' + tmp ]);
					}).then(function(r) {
						ui.hideModal();
						if (r.code != 0) {
							ui.addNotification(null, E('p', {}, [ _('Not enrolled: %s').format(
								(r.stderr || r.stdout || String(r.code)).trim()) ]), 'warning');
							return;
						}
						ui.addNotification(null, E('p', {}, _('Enrolled: this box is now a cell. The page reloads.')), 'info');
						reloadSoon();
					});
				}
			}, _('Enrol'))
		])
	]);
}

function leaveDialog() {
	ui.showModal(_('Leave cell mode?'), [
		E('p', {}, _('This box forgets the controller: its key and certificate are erased, and it runs as a DECT base again with its own handsets and settings. To be a cell again it needs a new enrollment string.')),
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
			E('button', {
				'class': 'btn cbi-button-negative',
				'click': function() {
					ui.hideModal();
					return fs.exec(INIT, [ 'cell_leave' ]).then(function(r) {
						if (r.code != 0)
							ui.addNotification(null, E('p', {}, [ _('Could not leave cell mode: %s').format(
								(r.stderr || String(r.code)).trim()) ]), 'warning');
						else
							reloadSoon();
					});
				}
			}, _('Leave cell mode'))
		])
	]);
}

/*
 * Chip firmware (docs/firmware-sourcing.md): the two files the DECT chip
 * boots from, which the user extracts on a PC with tools/dect-fw-extract.py.
 * firmware.sh on the box says what is there and what the board needs, and
 * checks an upload (SHA-256 against the board's pin) before it replaces
 * anything. The file goes up to /tmp by LuCI's own upload (cgi-upload),
 * then firmware.sh install moves it into place.
 */
function readFw() {
	return L.resolveDefault(fs.exec(FW, [ 'status' ]), null).then(function(r) {
		try { return r && r.stdout ? JSON.parse(r.stdout) : null; } catch (e) { return null; }
	});
}

function fwAnswer(r) {
	var j = null;

	try { j = r && r.stdout ? JSON.parse(r.stdout) : null; } catch (e) {}
	if (r && r.code == 0 && j && !j.error)
		return null;
	return (j && j.error) || (r && r.stderr) || _('no answer from the box');
}

function fwFail(msg) {
	ui.hideModal();
	ui.addNotification(null, E('p', {}, [ _('Firmware not changed: %s').format(msg) ]), 'danger');
}

function fwDone(msg) {
	ui.hideModal();
	ui.addNotification(null, E('p', {}, msg), 'info');
	window.setTimeout(function() { window.location.reload(); }, 1500);
}

function fwUpload(f) {
	var input = E('input', { 'type': 'file', 'style': 'display:none' });

	input.addEventListener('change', function() {
		var file = input.files && input.files[0], data;

		input.parentNode.removeChild(input);
		if (!file)
			return;
		if (file.size <= 0 || file.size > FW_MAX)
			return fwFail(_('%s is %d bytes; a chip firmware file is smaller than %d.').format(file.name, file.size, FW_MAX));
		ui.showModal(_('Checking the firmware'), [ E('p', { 'class': 'spinning' }, file.name) ]);
		data = new FormData();
		data.append('sessionid', rpc.getSessionID());
		data.append('filename', FW_TMP + f.role);
		data.append('filedata', file);
		request.post(L.env.cgi_base + '/cgi-upload', data, { timeout: 0 }).then(function(res) {
			if (!res.ok)
				throw new Error(res.statusText || ('HTTP ' + res.status));
			return fs.exec(FW, [ 'install', f.role, FW_TMP + f.role ]);
		}).then(function(r) {
			var err = fwAnswer(r);

			if (err)
				return fwFail(err);
			fwDone(_('%s is in place. The chip loads it the next time it boots (a restart of the DECT service, or of the box).').format(f.name));
		}).catch(function(e) {
			fwFail(e.message || e);
		});
	});
	document.body.appendChild(input);
	input.click();
}

function fwDelete(f) {
	ui.showModal(_('Delete %s?').format(f.name), [
		E('p', {}, _('The DECT chip cannot boot without it, so the base does not start until a firmware file is uploaded again.')),
		E('div', { 'class': 'right' }, [
			E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')), ' ',
			E('button', { 'class': 'btn cbi-button-negative', 'click': function() {
				ui.showModal(_('Deleting'), [ E('p', { 'class': 'spinning' }, f.name) ]);
				return fs.exec(FW, [ 'delete', f.role ]).then(function(r) {
					var err = fwAnswer(r);

					if (err)
						return fwFail(err);
					fwDone(_('%s is deleted.').format(f.name));
				});
			} }, _('Delete'))
		])
	]);
}

/* The Firmware table: one row per file the board needs. */
function fwNode(fw) {
	var STATES = {
		ok: _('verified'),
		mismatch: _('wrong build'),
		unknown: _('unknown build'),
		wrongname: _('known build, kept under another build\'s name (see Advanced)'),
		missing: _('missing')
	};
	var ROLES = { first: _('First stage'), second: _('Second stage') };
	var rows;

	if (!fw)
		return E('p', {}, _('The firmware check is not installed on this box.'));
	rows = (fw.files || []).map(function(f) {
		var st = f.state;

		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td' }, ROLES[f.role] || f.role),
			E('td', { 'class': 'td' }, f.name),
			E('td', { 'class': 'td' }, f.expected ?
				E('span', { 'title': f.expected }, f.expected.substr(0, 12) + '…') :
				_('any build opendectcell knows')),
			E('td', { 'class': 'td' }, E('strong', { 'style': st == 'ok' ? 'color:#2a7d2a' :
				st == 'missing' ? '' : 'color:#b32d2d' }, STATES[st] || st)),
			E('td', { 'class': 'td' }, [
				E('button', { 'class': 'btn cbi-button cbi-button-action',
					'click': function() { fwUpload(f); } }, _('Upload…')), ' ',
				st == 'missing' ? '' : E('button', { 'class': 'btn cbi-button cbi-button-negative',
					'click': function() { fwDelete(f); } }, _('Delete'))
			])
		]);
	});
	return E('div', {}, [
		E('p', {}, _('The DECT chip boots from two firmware files of the FRITZ!OS version this box came with. opendectcell cannot ship them: extract them on your PC with tools/dect-fw-extract.py (needs Python 3; its messages say which image to use) and upload the two .hex files here. They are kept in %s and survive a firmware upgrade of the box.').format(fw.dir)),
		E('table', { 'class': 'table cbi-section-table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('File')),
				E('th', { 'class': 'th' }, _('Name')),
				E('th', { 'class': 'th' }, _('Expected SHA-256')),
				E('th', { 'class': 'th' }, _('State')),
				E('th', { 'class': 'th' }, '')
			])
		].concat(rows)),
		fw.unverified ? E('p', {}, _('Allow unknown firmware is on (Advanced): an upload that is not the expected build is accepted.')) : ''
	]);
}

/* Cell mode: the controller link, and nothing of the base. */
function renderCell(link, fw) {
	var m, s, o;
	var state = link ? link.state : null;

	m = new form.Map('opendectcell', _('DECT'),
		_('This box is a cell of an openDECTcell controller: the controller runs the DECT base, its handsets and its phone lines. Live figures are on Status > DECT.'));
	s = m.section(form.NamedSection, 'main', 'opendectcell');
	s.addremove = false;

	o = s.option(form.Flag, 'enabled', _('Enable the cell'),
		_('Boots the DECT chip and connects to the controller. The controller turns the radio on.'));
	o.rmempty = false;

	o = s.option(form.DummyValue, '_controller', _('Controller'));
	o.cfgvalue = function() { return link && link.controller || _('not enrolled'); };
	o = s.option(form.DummyValue, '_link', _('Link'));
	o.cfgvalue = function() {
		return !state ? _('not running') : state == 'up' ? _('up') :
			state == 'connecting' ? _('connecting') :
			state == 'not enrolled' ? _('not enrolled') : state;
	};
	if (link && link.cell_state) {
		o = s.option(form.DummyValue, '_cstate', _('This cell at the controller'));
		o.cfgvalue = function() {
			return { active: _('active'), pending: _('waiting for an admin to approve it'),
				 disabled: _('disabled'), revoked: _('revoked') }[link.cell_state] || link.cell_state;
		};
	}
	if (!link || !link.controller) {
		o = s.option(form.Button, '_enroll', _('Enrolment'));
		o.inputtitle = _('Enrol…');
		o.inputstyle = 'action';
		o.onclick = enrollDialog;
	}
	o = s.option(form.Button, '_leave', _('Cell mode'),
		_('Run this box as its own DECT base again.'));
	o.inputtitle = _('Leave cell mode…');
	o.inputstyle = 'negative';
	o.onclick = leaveDialog;

	o = s.option(form.DummyValue, '_fw', _('Chip firmware'));
	o.cfgvalue = function() { return fwNode(fw); };

	return m.render();
}

function deregisterDialog(h) {
	/* an array, so the handset's own name is text, not markup */
	ui.showModal([ _('Deregister DECT %d (%s)?').format(h.slot, h.name) ], [
		E('p', {}, _('The base erases this handset\'s registration. The handset is told if it is in range; to use it again it has to be registered anew. A handset that is lost or switched off is erased after about 30 seconds without its confirmation.')),
		E('p', {}, _('Its settings here go with it; its VoIP profiles stay, free for another handset.')),
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
 * 30 s): its section is removed. Not before: a restart would cut the
 * deregistration short.
 */
function deregisterDone(h) {
	var t0 = Date.now();

	ui.showModal(_('Deregistering DECT %d').format(h.slot), [
		E('p', { 'class': 'spinning' }, _('Waiting for the base to let the handset go…'))
	]);
	function gone() {
		return readState().then(function(st) {
			/* no state file (being rewritten): not gone yet */
			var still = !st || (st.handsets || []).some(function(x) { return x.slot == h.slot; });

			if (still && Date.now() - t0 < 40000)
				return new Promise(function(res) { window.setTimeout(res, 2000); }).then(gone);
			ui.hideModal();
			if (still) {
				ui.addNotification(null, E('p', {}, [ _('DECT %d is still registered after 40 s; its settings are kept. See Status > DECT, Log.').format(h.slot) ]), 'warning');
				return;
			}
			/*
			 * The base removes its section on its own (forget): other
			 * changes saved here but not applied stay pending. The page
			 * then reloads, as its rows came from the old state.
			 */
			return fs.exec('/etc/init.d/opendectcell', [ 'forget', String(h.slot) ]).then(function(r) {
				if (r.code != 0)
					ui.addNotification(null, E('p', {}, [ _('DECT %d is deregistered, but its settings could not be removed: %s').format(h.slot, r.stderr || r.code) ]), 'warning');
				window.location.reload();
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

/*
 * The key groups of the profile format and the pop-up tab each goes to;
 * the two small ones share a tab, each under its heading.
 */
var GROUPS = {
	call:    [ 'calls', null ],
	link:    [ 'link', null ],
	report:  [ 'caps', _('What the handset reports') ],
	feature: [ 'caps', _('Features') ]
};

/* The handset's DECT settings, one field per key of the profile format. */
function addKeyOptions(ss, keys) {
	var seen = {};

	keys.forEach(function(k) {
		var o, name = '_k_' + k.key.replace(/[^a-z0-9]/g, '_');
		var g = GROUPS[k.group] || [ 'caps', k.group ], t = g[0];

		if (g[1] && !seen[k.group]) {
			seen[k.group] = true;
			o = ss.taboption(t, form.DummyValue, '_g_' + k.group);
			o.modalonly = true;
			o.render = function() {
				return E('h4', { 'style': 'margin-top:1em' }, g[1]);
			};
		}

		if (k.type == 'words') {
			o = ss.taboption(t, form.ListValue, name, k.label);
			o.value('', _('as the model says (%s)').format(
				k['default'] == '-' ? _('from the handset') : k['default']));
			k.values.forEach(function(v) {
				if (v != '-')
					o.value(v, v);
			});
		} else {
			o = ss.taboption(t, form.Value, name, k.label);
			o.placeholder = k.type == 'int' ? k['default'] : _('as the model says');
			o.validate = function(sid, v) {
				if (v === '')
					return true;
				if (k.type == 'int') {
					var n = /^0x[0-9a-f]+$/i.test(v) ? parseInt(v, 16) :
						/^[0-9]+$/.test(v) ? parseInt(v, 10) : NaN;
					return n >= k.min && n <= k.max ? true :
						_('%d to %d (or 0x..)').format(k.min, k.max);
				}
				return (/^hex:[0-9a-fA-F {}a-z]+$/).test(v) ? true : _('hex:... with {text} or {len}');
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
	return (/[;"<>\s]/).test(v) ? _('baresip cannot take ; " < > or spaces here') : true;
}

/* A VoIP profile's name as the menus show it. */
function profileName(sid) {
	return sid && uci.get('opendectcell', sid, 'name') || sid || '-';
}

/* Where a profile is assigned: a list of [ handset section, line ]. */
function profileUse(psid) {
	var found = [];

	uci.sections('opendectcell', 'handset').forEach(function(h) {
		[ 1, 2 ].forEach(function(n) {
			if (h['line' + n] == psid)
				found.push([ h['.name'], n ]);
		});
	});
	return found;
}

/*
 * A VoIP profile: what a baresip account line takes, in the pop-up's tabs
 * Account, Server and NAT, Media, Encryption, Advanced.
 */
function addProfileOptions(ps) {
	var o;

	function opt(t, type, name, title, descr) {
		var x = ps.taboption(t, type, name, title, descr);

		x.modalonly = true;
		return x;
	}

	o = opt('acct', form.Value, 'name', _('Profile name'),
		_('How the Line menus of the handsets name this profile.'));
	o.rmempty = false;
	o.validate = function(sid, v) {
		if (!v.trim())
			return _('A name is needed');
		return uci.sections('opendectcell', 'voip').some(function(p) {
			return p['.name'] != sid && (p.name || '') == v;
		}) ? _('Another profile has this name') : true;
	};
	o = opt('acct', form.Value, 'line_name', _('Name on the handset'),
		_('What CAT-iq handsets show for this line, at most 15 characters (fewer with umlauts). Empty: the profile name, if it is that short.'));
	o.validate = function(sid, v) {
		return octets(v) <= 15 ? true : _('At most 15 characters');
	};
	opt('acct', form.Value, 'display', _('Display name'), _('The name the called party sees.'));
	o = opt('acct', form.Value, 'user', _('User name'), _('The SIP user, often the phone number.'));
	o.rmempty = false;
	o.validate = function(sid, v) {
		return v ? sipSafe(sid, v) : _('A user name is needed');
	};
	o = opt('acct', form.Value, 'authuser', _('Login name'),
		_('When the provider wants a login other than the user name.'));
	o.optional = true;
	o.validate = sipSafe;
	o = opt('acct', form.Value, 'password', _('Password'));
	o.password = true;
	o.validate = sipSafe;
	o = opt('acct', form.Value, 'domain', _('Registrar'),
		_('The provider\'s SIP server, host or host:port (e.g. sip.example.net, or 192.168.178.1 for a FRITZ!Box).'));
	o.datatype = 'or(hostport,host)';
	o.rmempty = false;
	/* one SIP account, one profile: the base could not tell two apart */
	o.validate = function(sid, v) {
		var u = this.map.lookupOption('user', sid), user = u ? u[0].formvalue(sid) : '';

		return uci.sections('opendectcell', 'voip').some(function(p) {
			return p['.name'] != sid && p.user == user && p.domain == v;
		}) ? _('Another profile has this account (%s@%s)').format(user, v) : true;
	};
	o = opt('acct', form.Flag, 'register', _('Register'),
		_('Register with the server. Off for a peer that calls this handset directly by address.'));
	o.default = '1';
	o.rmempty = false;
	o = opt('acct', form.Value, 'regint', _('Registration interval'), _('Seconds between registrations.'));
	o.datatype = 'range(60,86400)';
	o.placeholder = '3600';
	o.depends('register', '1');

	o = opt('net', form.ListValue, 'transport', _('Transport'),
		_('Use TLS if your provider supports it: it encrypts the signalling, so your password and the numbers you call do not cross the internet in the clear (the provider names the port, often 5061). Encrypted speech (Encryption tab) is only private with TLS here.'));
	o.value('udp', 'UDP');
	o.value('tcp', 'TCP');
	o.value('tls', _('TLS (encrypted signalling)'));
	o.default = 'udp';
	o = opt('net', form.Value, 'outbound', _('Outbound proxy'), _('Only when the provider names one.'));
	o.datatype = 'or(hostport,host)';
	o.optional = true;
	o = opt('net', form.Value, 'outbound2', _('Second outbound proxy'));
	o.datatype = 'or(hostport,host)';
	o.optional = true;
	o = opt('net', form.Flag, 'sipnat', _('SIP outbound (RFC 5626)'),
		_('Keep the connection to the server open through NAT, when the provider supports it.'));
	o = opt('net', form.ListValue, 'medianat', _('NAT traversal for media'));
	o.value('', _('none'));
	o.value('stun', 'STUN');
	o.value('turn', 'TURN');
	o.value('ice', 'ICE');
	o = opt('net', form.Value, 'stunserver', _('STUN/TURN server'), _('host or host:port.'));
	o.datatype = 'or(hostport,host)';
	o.depends('medianat', /./);
	o = opt('net', form.Value, 'stunuser', _('TURN user'));
	o.depends('medianat', 'turn');
	o = opt('net', form.Value, 'stunpass', _('TURN password'));
	o.password = true;
	o.validate = sipSafe;
	o.depends('medianat', 'turn');
	o = opt('net', form.ListValue, 'rtpkeep', _('Keep media open'),
		_('Packets that keep a NAT binding open while one side is silent.'));
	o.value('', _('off'));
	o.value('zero', _('empty RTP packets'));
	o.value('stun', 'STUN');
	o.value('dyna', _('dynamic'));
	o.value('rtcp', 'RTCP');

	o = opt('media', form.Flag, 'media_auto', _('Try to autodetect'),
		_('Offers every codec, HD first, and sends key tones the way the other side asks. Right for nearly every provider; untick to set them by hand.'));
	o.default = '1';
	o.rmempty = false;
	o = opt('media', form.MultiValue, 'codec', _('Codecs'),
		_('Offered in the order shown. G.722 keeps the handset\'s wideband (HD) audio end to end.'));
	o.value('g722', 'G.722 (HD)');
	o.value('pcma', 'G.711 A-law');
	o.value('pcmu', 'G.711 µ-law');
	o.default = 'g722 pcma pcmu';
	o.depends('media_auto', '0');
	o = opt('media', form.ListValue, 'ptime', _('Packet time'), _('Milliseconds of audio per RTP packet.'));
	[ '20', '10', '30', '40' ].forEach(function(v) { o.value(v, v + ' ms'); });
	o.default = '20';
	o.depends('media_auto', '0');
	o = opt('media', form.ListValue, 'dtmf', _('Key tones (DTMF)'));
	o.value('rtpevent', _('RTP events (RFC 4733)'));
	o.value('info', _('SIP INFO'));
	o.value('auto', _('automatic'));
	o.default = 'rtpevent';
	o.depends('media_auto', '0');
	o = opt('media', form.ListValue, 'rel100', _('Reliable provisional responses (100rel)'));
	o.value('yes', _('when the other side supports it'));
	o.value('no', _('never'));
	o.value('required', _('required'));
	o.default = 'yes';
	o.depends('media_auto', '0');
	o = opt('media', form.Flag, 'mwi', _('Voicemail notification (MWI)'));
	o = opt('media', form.Flag, 'transfer', _('Allow call transfer'));
	o.default = '1';
	o.rmempty = false;

	o = opt('enc', form.Flag, 'enc_auto', _('Try to autodetect'),
		_('Offers encrypted speech (SRTP) and falls back to plain speech when the other side does not take it. Untick to choose a method, or none.'));
	o.default = '1';
	o.rmempty = false;
	o = opt('enc', form.ListValue, 'mediaenc', _('Media encryption'),
		_('Encrypts the speech on the internet side. Use TLS as transport too, or the keys travel in the clear.'));
	o.value('', _('none'));
	o.value('srtp', _('SRTP when offered'));
	o.value('srtp-mand', _('SRTP required'));
	o.value('srtp-mandf', _('SRTP required (in the offer)'));
	o.value('dtls_srtp', 'DTLS-SRTP');
	o.depends('enc_auto', '0');

	o = opt('adv', form.Value, 'extra', _('Further account parameters'),
		_('Appended to the account as is, baresip syntax: ;key=value;key=value.'));
	o.optional = true;
	o.validate = function(sid, v) {
		return v === '' || /^(;[a-z0-9_]+=[^;\s"<>]+)+$/.test(v) ? true :
			_(';key=value pairs, no spaces');
	};
}

/*
 * A handset's Line 1 or Line 2: a menu of the VoIP profiles with a search
 * field above it. It offers every profile but the one the other Line menu
 * of this pop-up holds; handsets may share a profile.
 */
var LineValue = form.ListValue.extend({
	other: function() { return this.option == 'line1' ? 'line2' : 'line1'; },

	otherValue: function(sid) {
		var o = this.map.lookupOption(this.other(), sid);
		return o ? o[0].formvalue(sid) : uci.get('opendectcell', sid, this.other());
	},

	/* hide the other line and what does not match the search; keep the choice shown */
	/* the <select> itself: LuCI wraps it in a div that carries the cbid */
	select: function(sid) {
		var el = this.map.findElement('id', this.cbid(sid));

		return el && (el.tagName == 'SELECT' ? el : el.querySelector('select'));
	},

	refresh: function(sid) {
		var sel = this.select(sid), other, q;

		if (!sel)
			return;
		other = this.otherValue(sid);
		q = sel._query || '';
		Array.prototype.forEach.call(sel.options, function(op) {
			var hide = op.value !== '' && (op.value == other ||
				(q && op.text.toLowerCase().indexOf(q) < 0));
			op.hidden = op.disabled = !!hide && !op.selected;
		});
	},

	/*
	 * The profiles as they are now, not as at page load: one added,
	 * renamed or deleted in this visit shows at once. A line whose
	 * profile is gone shows as such, so a Save does not drop it unseen.
	 */
	choices: function(cfgvalue) {
		var list = uci.sections('opendectcell', 'voip').slice().sort(function(a, b) {
			return (a.name || a['.name']).localeCompare(b.name || b['.name']);
		});

		this.keylist = [ '' ];
		this.vallist = [ _('none') ];
		list.forEach(function(p) {
			this.keylist.push(p['.name']);
			this.vallist.push(p.name || p['.name']);
		}, this);
		if (cfgvalue && !uci.get('opendectcell', cfgvalue)) {
			this.keylist.push(cfgvalue);
			this.vallist.push(_('%s (deleted profile)').format(cfgvalue));
		}
	},

	renderWidget: function(sid, idx, cfgvalue) {
		this.choices(cfgvalue);
		var self = this, node = form.ListValue.prototype.renderWidget.apply(this, [ sid, idx, cfgvalue ]);
		var sel = node.tagName == 'SELECT' ? node : node.querySelector('select');
		var search = E('input', {
			'type': 'search', 'class': 'cbi-input-text',
			'placeholder': _('Search the profiles…'),
			'style': 'display:block;margin-bottom:.3em',
			'input': function() {
				if (sel) {
					sel._query = this.value.trim().toLowerCase();
					self.refresh(sid);
				}
			}
		});

		window.setTimeout(function() { self.refresh(sid); }, 0);
		return E('div', {}, [ search, node ]);
	},

	onchange: function(ev, sid, v) {
		var o = this.map.lookupOption(this.other(), sid);

		if (o)
			o[0].refresh(sid);
	},

	validate: function(sid, v) {
		if (!v)
			return true;
		if (!uci.get('opendectcell', v) || uci.get('opendectcell', v, '.type') != 'voip')
			return _('This profile was deleted; choose another or none');
		if (this.option == 'line2' && !this.otherValue(sid))
			return _('Choose Line 1 first: it is the line the handset\'s calls go out on unless it picks Line 2');
		return v == this.otherValue(sid) ? _('This profile is already the other line') : true;
	}
});

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('opendectcell'),
			readState(),
			readProfiles(),
			readKeys(),
			readVoip(),
			readBoard(),
			readLink(),
			L.resolveDefault(fs.stat(CELL_BIN), null),
			L.resolveDefault(fs.stat(CARDDAV), null),
			L.resolveDefault(fs.read(CARDDAV_STATE), ''),
			L.resolveDefault(fs.read(CARDDAV_BOOKS), ''),
			readFw()
		]).then(function(r) {
			var st = r[1], profiles = r[2], voip = r[4];

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
				var cd = null, found = [];

				try { cd = r[9] ? JSON.parse(r[9]) : null; } catch (e) {}
				try { found = r[10] ? JSON.parse(r[10]) : []; } catch (e) {}
				/* the section the Phonebook options live in */
				if (r[8] && !uci.get('opendectcell', 'carddav'))
					uci.add('opendectcell', 'carddav', 'carddav');
				return [ st, profiles, choices, r[3], voip, r[5], r[6], r[7], r[8], cd, found, r[11] ];
			});
		});
	},

	render: function(data) {
		var st = data[0], profiles = data[1], choices = data[2], keys = data[3], voip = data[4];

		if (!this.applyHook) {
			this.applyHook = true;
			document.addEventListener('uci-applied', startIfStopped);
		}
		var board = data[5], cellInstalled = !!data[7];

		if (uci.get('opendectcell', 'main', 'role') == 'cell')
			return renderCell(data[6], data[11]);
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
		s.tab('voip', _('VoIP Profiles'));
		if (data[8])
			s.tab('phonebook', _('Phonebook'));
		s.tab('firmware', _('Firmware'));
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

		o = s.taboption('general', form.Flag, 'twocalls', _('Several calls'),
			_('Up to three calls at once: other handsets may call and be called while one is in a call.'));
		o.default = '1';
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

		o = s.taboption('general', form.Value, 'time_update', _('Time update interval'),
			_('Hours between sending the date and time to all handsets. Each handset also gets it whenever it reports in to the base; 0 sends it only then.'));
		o.datatype = 'range(0,720)';
		o.placeholder = '24';

		if (cellInstalled) {
			o = s.taboption('general', form.Button, '_cell', _('Controller'),
				_('Make this box a cell of an openDECTcell controller, which then runs the base for several boxes.'));
			o.inputtitle = _('Make this box a cell…');
			o.inputstyle = 'action';
			o.onclick = enrollDialog;
		}

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
		ss.tab('calls', _('Calls'));
		ss.tab('link', _('Radio link and lists'));
		ss.tab('caps', _('Capabilities and features'));

		ss.renderRowActions = function(sid) {
			var td = form.GridSection.prototype.renderRowActions.apply(this, [ sid, _('Edit') ]);
			var h = hs(sid), box = td.querySelector('div') || td, self = this;

			if (h)
				box.appendChild(E('button', {
					'class': 'btn cbi-button-negative',
					'disabled': running ? null : '',
					'title': running ? null : _('Needs the DECT service running'),
					'click': function(ev) { ev.preventDefault(); deregisterDialog(h); }
				}, _('Deregister')));
			else if (running)
				/* the base does not hold it (any more): its settings can go */
				box.appendChild(E('button', {
					'class': 'btn cbi-button-negative',
					'title': _('This handset is not registered; remove its settings and free its VoIP profiles'),
					'click': ui.createHandlerFn(this, function() { return self.handleRemove(sid); })
				}, _('Remove')));
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
		var modelName = {};
		profiles.forEach(function(p) { modelName[p.id] = p.name; });
		o.cfgvalue = function(sid) {
			var h = hs(sid);
			return !h || h.part3 == null ? '-' : h.part3 ? _('CAT-iq') : _('GAP');
		};
		o = ss.option(form.DummyValue, '_model', _('Model'));
		o.modalonly = false;
		o.cfgvalue = function(sid) {
			var c = choiceBySlot[+uci.get('opendectcell', sid, 'slot')];
			return !hs(sid) ? '-' : c ? modelName[c] || c : _('standard');
		};

		[ 1, 2 ].forEach(function(n) {
			o = ss.option(form.DummyValue, '_l' + n, _('Line %d').format(n));
			o.modalonly = false;
			o.cfgvalue = function(sid) { return profileName(uci.get('opendectcell', sid, 'line' + n)); };
		});
		o = ss.option(form.DummyValue, '_seen', _('Last contact'));
		o.modalonly = false;
		o.cfgvalue = function(sid) {
			var h = hs(sid);
			return !h ? '-' : !running ? _('base off') :
				h.link ? _('connected now') : ago(h.seen_ms_ago);
		};

		/* General: name and profile where the base keeps them */
		o = ss.taboption('general', form.Value, '_rename', _('Name'),
			running ? _('What every handset shows for this one, at most 15 characters (fewer with umlauts). Empty restores "DECT n". Takes effect when this pop-up is saved.') :
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
			return ctl('rename hs=' + h.slot + ' name=' + quote(v)).then(function(r) {
				if (r.ok)
					h.name = v || 'DECT ' + h.slot;	/* what the page shows from now on */
				report(r, _('DECT %d renamed.').format(h.slot));
			});
		};
		o.remove = function(sid) { return this.write(sid, ''); };

		o = ss.taboption('general', form.ListValue, '_profile', _('Model'),
			_('Known quirks of this handset model; its own settings in the other tabs override it. Takes effect when this pop-up is saved.'));
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
			return w.then(function() {
				choiceBySlot[h.slot] = v;
				return ctl('reload-profiles');
			});
		};
		o.remove = function(sid) { return this.write(sid, ''); };

		/* General: its lines, a VoIP profile each */

		[ 1, 2 ].forEach(function(n) {
			o = ss.taboption('general', LineValue, 'line' + n, _('Line %d').format(n),
				n == 1 ? _('A VoIP profile (VoIP Profiles tab): calls to it ring this handset, and the handset\'s calls go out on it unless the handset picks Line 2.') :
					 _('An optional second VoIP profile, ringing this handset too.'));
			o.modalonly = true;
			o.optional = true;
		});

		addKeyOptions(ss, keys);

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
		o.renderWidget = function(sid, idx, cfg) {
			var w = form.ListValue.prototype.renderWidget.apply(this, [ sid, idx, cfg ]);
			var sel = w.tagName == 'SELECT' ? w : w.querySelector('select');
			if (sel)
				sel.style.minWidth = '20em';	/* the generic entry in full */
			return w;
		};

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

			/*
			 * The base registers with the PIN it was started with: not
			 * one only on the page, nor one saved but not applied.
			 */
			if (formval('pin') != uci.get('opendectcell', 'main', 'pin')) {
				ui.addNotification(null, E('p', _('The PIN shown is not in use yet: press Save & Apply first, then register.')), 'warning');
				return;
			}

			return L.resolveDefault(callChanges(), {}).then(function(ch) {
				if ((ch.opendectcell || []).some(function(c) { return c[1] == 'main' && c[2] == 'pin'; })) {
					ui.addNotification(null, E('p', _('A new PIN is saved but not in use yet: press Save & Apply first, then register.')), 'warning');
					return;
				}
				return register();
			});

			function register() {
				return ctl('register-window' + (prof ? ' profile=' + prof : '')).then(function(r) {
					report(r, _('Registration is open for %s s. Start the registration on the handset now and enter the PIN %s.')
						.format(uci.get('opendectcell', 'main', 'regwindow') || '120',
							uci.get('opendectcell', 'main', 'pin') || '-'));
				});
			}
		};

		o = s.taboption('handsets', form.Value, 'regwindow', _('Registration window'),
			_('Seconds that "Register a handset" keeps registration open.'));
		o.datatype = 'range(10,3600)';
		o.placeholder = '120';

		o = s.taboption('handsets', form.Flag, 'registration', _('Keep registration open'),
			_('Registration stays open while the base runs. Not recommended: anyone nearby who knows the PIN can register a handset.'));

		/* VoIP Profiles: the list first, then what all of them share */
		var regBy = {};
		((voip && voip.lines) || []).forEach(function(l) { regBy[l.hs + '/' + l.line] = l; });

		o = s.taboption('voip', form.SectionValue, '_voip', form.GridSection, 'voip', null,
			_('One profile per SIP account; assign them under Handsets (Edit, Line 1 / Line 2). A profile may be a line of several handsets: a call to it rings them all.'));
		var ps = o.subsection;
		ps.anonymous = true;
		ps.addremove = true;
		ps.nodescriptions = true;
		ps.addbtntitle = _('Add a profile');
		ps.modaltitle = function(sid) {
			return _('VoIP profile %s').format(uci.get('opendectcell', sid, 'name') || _('(new)'));
		};
		ps.tab('acct', _('Account'));
		ps.tab('net', _('Server and NAT'));
		ps.tab('media', _('Media'));
		ps.tab('enc', _('Encryption'));
		ps.tab('adv', _('Advanced'));
		/* a named section, so a handset's line1/line2 can point at it for good */
		ps.handleAdd = function(ev) {
			var n = 1;

			while (uci.get('opendectcell', 'voip' + n))
				n++;
			return form.GridSection.prototype.handleAdd.call(this, ev, 'voip' + n);
		};
		/* deleting a profile frees every line that held it */
		ps.handleRemove = function(sid, ev) {
			profileUse(sid).forEach(function(u) {
				uci.unset('opendectcell', u[0], 'line' + u[1]);
			});
			return form.GridSection.prototype.handleRemove.call(this, sid, ev);
		};

		o = ps.option(form.DummyValue, 'name', _('Name'));
		o.modalonly = false;
		o = ps.option(form.DummyValue, '_account', _('Account'));
		o.modalonly = false;
		o.cfgvalue = function(sid) {
			return (uci.get('opendectcell', sid, 'user') || '?') + '@' + (uci.get('opendectcell', sid, 'domain') || '?');
		};
		o = ps.option(form.DummyValue, '_use', _('Line of'));
		o.modalonly = false;
		o.cfgvalue = function(sid) {
			var u = profileUse(sid);
			return u.length ? u.map(function(x) {
				return _('DECT %s, Line %d').format(uci.get('opendectcell', x[0], 'slot') || '?', x[1]);
			}).join(', ') : _('free');
		};
		o = ps.option(form.DummyValue, '_reg', _('Registration'));
		o.modalonly = false;
		o.cfgvalue = function(sid) {
			/* one account: every line it is reports the same */
			var u = profileUse(sid), l = u.length && regBy[(uci.get('opendectcell', u[0][0], 'slot') || '') + '/' + u[0][1]];
			return !u.length ? '-' : !l ? _('not in use (yet)') :
				l.registration + (l.reason ? ': ' + l.reason : '');
		};
		addProfileOptions(ps);

		o = s.taboption('voip', form.DummyValue, '_shared');
		o.render = function() {
			return E('h3', { 'style': 'margin-top:1.5em' }, _('Settings for all profiles'));
		};

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
			_('The network device SIP and speech go out on, e.g. br-lan or pppoe-wan (not an interface name like wan). Empty: the default route.'));
		o.placeholder = _('automatic');
		o = s.taboption('voip', form.Value, 'dns_server', _('DNS server'),
			_('An IP address, optionally with :port, for the SIP servers\' names. Empty: the system\'s.'));
		o.datatype = 'or(ipaddr,ipaddrport(1))';
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

		/* Phonebook */
		if (data[8]) {
			var cd = data[9], found = data[10] || [];

			o = s.taboption('phonebook', form.DummyValue, '_cd_intro');
			o.rawhtml = true;
			o.default = E('div', {}, [
				E('p', {}, _('The handsets\' phonebook comes from CardDAV address books (Nextcloud, Baïkal, a mail provider\'s). What a handset changes or deletes is written back to the book the contact came from; a contact made on a handset goes to the book chosen for new contacts.')),
				E('p', {}, _('Contacts are not merged: one that is in two books appears twice on the handsets.'))
			]);
			o = s.taboption('phonebook', form.Flag, '_cd_enabled', _('Use address books'));
			o.cfgvalue = function() { return uci.get('opendectcell', 'carddav', 'enabled') || '0'; };
			o.write = function(sid, v) { uci.set('opendectcell', 'carddav', 'enabled', v); };
			o.remove = function() { uci.set('opendectcell', 'carddav', 'enabled', '0'); };
			[
				[ 'url', form.Value, _('Server'),
				  _('Any address of the account, https:// only: the server (https://cloud.example.org), or an address book\'s own URL.') ],
				[ 'username', form.Value, _('User name') ],
				[ 'password', form.Value, _('Password'),
				  _('An app password where the server offers one.') ]
			].forEach(function(f) {
				o = s.taboption('phonebook', f[1], '_cd_' + f[0], f[2], f[3]);
				o.depends('_cd_enabled', '1');
				o.retain = true;	/* switched off, the settings stay */
				o.cfgvalue = function() { return uci.get('opendectcell', 'carddav', f[0]); };
				o.write = function(sid, v) { uci.set('opendectcell', 'carddav', f[0], v); };
				o.remove = function() { uci.unset('opendectcell', 'carddav', f[0]); };
				if (f[0] == 'password')
					o.password = true;
				if (f[0] == 'url') {
					o.rmempty = false;
					o.validate = function(sid, v) {
						return (/^https:\/\/[^\/]+/).test(v || '') ? true
							: _('An https:// address');
					};
				}
			});
			o = s.taboption('phonebook', BookList, '_cd_books', _('Address books'));
			o.depends('_cd_enabled', '1');
			o.retain = true;
			o.books = found.slice();
			/* chosen books the last fetch did not list, by URL */
			L.toArray(uci.get('opendectcell', 'carddav', 'book')).forEach(function(h) {
				if (!o.books.some(function(b) { return b.href == h; }))
					o.books.push({ href: h, name: h });
			});
			[
				[ 'interval', form.Value, _('Check every'), _('Minutes. Changes on a handset are written back at once.') ],
				[ 'ca_file', form.Value, _('CA certificate'),
				  _('Only for a server with its own certificate authority: a PEM file on this box. Empty: the usual public CAs.') ],
				[ 'allow_clear', form.Flag, _('Allow "delete list"'),
				  _('A handset\'s "delete list" empties every address book used here, on the server. Off: the base refuses it.') ]
			].forEach(function(f) {
				o = s.taboption('phonebook', f[1], '_cd_' + f[0], f[2], f[3]);
				o.depends('_cd_enabled', '1');
				o.retain = true;
				o.cfgvalue = function() { return uci.get('opendectcell', 'carddav', f[0]); };
				o.write = function(sid, v) { uci.set('opendectcell', 'carddav', f[0], v); };
				o.remove = function() { uci.unset('opendectcell', 'carddav', f[0]); };
				if (f[0] == 'interval') {
					o.placeholder = '15';
					o.datatype = 'range(1,1440)';
				}
			});
			o = s.taboption('phonebook', form.DummyValue, '_cd_state', _('Last sync'));
			o.depends('_cd_enabled', '1');
			o.rawhtml = true;
			o.cfgvalue = function() {
				if (!cd)
					return _('none yet');
				return E('div', {}, [
					E('div', {}, cd.error ? _('failed: %s').format(cd.error)
						: _('%d contacts, %s').format(cd.contacts,
							cd.last_sync ? new Date(cd.last_sync * 1000).toLocaleString() : '-'))
				].concat((cd.books || []).map(function(b) {
					var f = found.filter(function(x) { return x.href == b.href; })[0];

					return E('div', {}, E('small', {}, '%s: %s'.format(f ? f.name : b.href,
						b.error ? b.error : _('%d contacts').format(b.contacts))));
				})));
			};
		}

		/* Firmware */
		o = s.taboption('firmware', form.DummyValue, '_fw', _('Chip firmware'));
		o.cfgvalue = function() { return fwNode(data[11]); };

		/* Advanced; a placeholder is what the service takes when it is empty */
		o = s.taboption('advanced', form.Value, 'firmware_dir', _('Chip firmware directory'),
			_('Where the two DECT chip firmware files from a FRITZ!OS image are kept (never shipped with opendectcell).'));
		o.placeholder = '/etc/opendectcell/firmware';
		o = s.taboption('advanced', form.Value, 'first', _('First-stage firmware'));
		o.placeholder = board.fw_firstlevel;
		o = s.taboption('advanced', form.Value, 'second', _('Second-stage firmware'));
		o.placeholder = board.fw_secondlevel;
		o = s.taboption('advanced', form.Flag, 'fw_allow_unverified', _('Allow unknown firmware'),
			_('Run a chip firmware build opendectcell does not know. For developers.'));
		o = s.taboption('advanced', form.Value, 'uart', _('DECT UART'));
		o.placeholder = board.uart;
		o = s.taboption('advanced', form.Value, 'factory_mtd', _('Factory partition'),
			_('Holds the base\'s DECT identity and calibration.'));
		o.placeholder = board.factory_mtd;
		o = s.taboption('advanced', form.Value, 'factory_offset', _('Factory offset'));
		o.placeholder = board.factory_offset;
		o = s.taboption('advanced', form.Value, 'mode', _('Chip mode word'));
		o.placeholder = board.dect_mode;
		o = s.taboption('advanced', form.Value, 'stores', _('Handset stores'),
			_('One file per registered handset.'));
		o.placeholder = '/etc/opendectcell-sub.*';
		o = s.taboption('advanced', form.Value, 'options', _('Extra options'),
			_('Further dect-stubd -a options, comma-separated, for developers. They follow the service\'s own, so a key set here wins.'));

		return m.render();
	}
});
