#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The init script's SIP exposure settings: which address baresip listens on (sip_bind) and the
peers an account that does not register may be called from (list sip_peer). The functions are cut
out of opendectcell.init and run with stand-ins for ubus, jsonfilter, sleep and the UCI helpers."""
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
INIT = os.path.join(ROOT, 'openwrt', 'net', 'opendectcell', 'files', 'opendectcell.init')
failed = 0


def check(cond, what):
    global failed
    if not cond:
        failed += 1
        print('FAIL:', what)


def func(src, name):
    m = re.search(r'^%s\(\) \{\n.*?^\}\n' % re.escape(name), src, re.M | re.S)
    if not m:
        raise SystemExit('function %s not found in opendectcell.init' % name)
    return m.group(0)


with open(INIT, encoding='utf-8') as f:
    src = f.read()
lib = '\n'.join(func(src, n) for n in ('uci_ok', 'uci_get', 'sip_runas', 'bs_listen_addr', 'bs_peer'))

STUBS = r'''
log() { echo "LOG: $*" >&2; }
sleep() { :; }
config_get() { eval "$1=\"\$CFG_$3\""; [ -n "$(eval echo \"\$CFG_$3\")" ] || eval "$1=\"$4\""; }
id() { [ "$HAVE_USER" = 1 ]; }
ubus() { [ "$LAN_UP" = 1 ] && echo '{"ipv4-address":[{"address":"192.0.2.7","mask":24}]}'; }
jsonfilter() { sed -n 's/.*"address":"\([0-9.]*\)".*/\1/p'; }
'''


def run(code, **env):
    e = dict(os.environ, **env)
    r = subprocess.run(['sh', '-c', STUBS + lib + '\n' + code], capture_output=True, text=True, env=e)
    return r.stdout.strip(), r.stderr


out, _ = run('bs_listen_addr all')
check(out == '0.0.0.0', 'sip_bind all: %r' % out)
out, _ = run('bs_listen_addr 198.51.100.4')
check(out == '198.51.100.4', 'an address is used as it is: %r' % out)
out, _ = run('bs_listen_addr lan', LAN_UP='1')
check(out == '192.0.2.7', 'the lan interface address: %r' % out)
out, err = run('bs_listen_addr lan', LAN_UP='0')
check(out == '0.0.0.0' and 'listening on all addresses' in err, 'no address yet: all addresses, and it says so: %r %r' % (out, err))
out, _ = run('BS_PEERS=; bs_peer 192.0.2.5; bs_peer 198.51.100.0/24; bs_peer "2001:db8::/32"; echo "$BS_PEERS"')
check(out == '192.0.2.5,198.51.100.0/24,2001:db8::/32', 'peers are joined: %r' % out)
out, err = run('BS_PEERS=; bs_peer 192.0.2.5; bs_peer "x; rm -rf /"; bs_peer "a b"; echo "$BS_PEERS"')
check(out == '192.0.2.5' and err.count('LOG:') == 2, 'entries with other characters are refused: %r %r' % (out, err))
# the user baresip runs as
out, _ = run('sip_runas', HAVE_USER='1')
check(out == 'opendectcell', 'baresip runs as opendectcell by default: %r' % out)
out, _ = run('sip_runas', HAVE_USER='1', CFG_sip_user='root')
check(out == '', 'sip_user root keeps root: %r' % out)
out, err = run('sip_runas', HAVE_USER='0')
check(out == '' and 'does not exist' in err, 'no such user: root, and it says so: %r %r' % (out, err))
out, err = run('sip_runas', HAVE_USER='1', CFG_sip_port='80')
check(out == '' and 'below 1024' in err, 'a privileged port keeps root: %r %r' % (out, err))
out, _ = run('sip_runas', HAVE_USER='1', CFG_sip_port='5062')
check(out == 'opendectcell', 'a high port is fine: %r' % out)
check('procd_set_param user "$SIP_USER"' in src and 'sockgroup=$SIP_USER' in src,
      'baresip and the sockets are not set up for the unprivileged user')
# the generated config line is built from the same helpers
check('echo "sip_listen $(bs_listen_addr "$sipbind"):$v"' in src, 'the baresip config does not use bs_listen_addr')
check('echo "opendectcell_peers $BS_PEERS"' in src, 'the baresip config does not carry the peers')

if failed:
    sys.exit(1)
print('test_sip_init.py: ok')
