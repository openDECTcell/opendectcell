#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""`enroll_start`, what LuCI uses to enrol: the enrolment runs in the background (a call to the box ends after 20 to
30 seconds, the service restart alone can take 15), the outcome goes to a status file, and it has a time limit. The
functions are cut out of opendectcell.init and run with a stand-in for `enroll`."""
import os
import re
import subprocess
import sys
import tempfile
import time

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
lib = '\n'.join(func(src, n) for n in ('ui_dir', 'enroll_start'))
check('ENROLL_LIMIT=${ENROLL_LIMIT:-90}' in src, 'the time limit is not 90 seconds')


def run(enroll_body, limit='90', wait=12, running=True, enabled='1'):
    """Start an enrolment whose `enroll` does enroll_body; return (what enroll_start returned, final status)."""
    with tempfile.TemporaryDirectory() as ui:
        pre = '''UI_DIR=%s
ENROLL_STATUS=$UI_DIR/enroll.status
ENROLL_LIMIT=%s
log() { echo "LOG: $*" >&2; }
enroll() { %s; }
ENROLL_SETTLE=0
pidof() { %s; }
uci() { echo %s; }
''' % (ui, limit, enroll_body, 'return 0' if running else 'return 1', enabled)
        r = subprocess.run(['sh', '-c', pre + lib + '\nenroll_start @/x; echo "rc=$?"'], capture_output=True, text=True, timeout=30)
        st = os.path.join(ui, 'enroll.status')
        first = open(st).read().strip() if os.path.exists(st) else ''
        end = time.time() + wait
        text = first
        while time.time() < end and text.startswith('running'):
            time.sleep(0.2)
            text = open(st).read().strip()
        return r.stdout.strip(), first, text, os.listdir(ui)


rc, first, final, left = run('echo "enrolled with c.example:7300; the cell is active"; echo "opendectcell: more"; return 0')
check(rc == 'rc=0', 'enroll_start returns at once: %r' % rc)
check(first.startswith('running ') or first.startswith('ok '), 'a status exists when it returns: %r' % first)
check(re.fullmatch(r'ok \d+ enrolled with c\.example:7300; the cell is active', final), 'success: %r' % final)
check(left == ['enroll.status'], 'nothing is left in the directory: %r' % left)

rc, first, final, left = run('echo "the controller said the token is not valid" >&2; return 1')
check(re.fullmatch(r'error \d+ the controller said the token is not valid', final), 'failure carries the reason: %r' % final)

# enrolled, but the cell service did not start: a warning that says why, not a plain success
rc, first, final, left = run('echo "enrolled with c.example:7300; the cell is active"; echo "opendectcell: chip firmware missing in /etc/opendectcell/firmware (a, b); not starting" >&2; return 0', running=False)
check(re.fullmatch(r'warn \d+ chip firmware missing in /etc/opendectcell/firmware \(a, b\); not starting', final), 'enrolled but not running (firmware): %r' % final)
rc, first, final, left = run('echo "enrolled with c.example:7300; the cell is active"; return 0', running=False, enabled='0')
check(re.fullmatch(r'warn \d+ the DECT service is switched off .*', final), 'enrolled but not running (switched off): %r' % final)
rc, first, final, left = run('echo "enrolled with c.example:7300; the cell is active"; return 0', running=False, enabled='1')
check(final.startswith('warn ') and 'logread' in final, 'enrolled but not running, no reason known: %r' % final)

# it does not wait for the enrolment, and a hung one is stopped at the limit and says so
t = time.time()
rc, first, final, left = run('sleep 30', limit='2', wait=10)
check(rc == 'rc=0' and first.startswith('running '), 'a slow enrolment is started, not waited for: %r %r' % (rc, first))
check(re.fullmatch(r'error \d+ no answer within 2 seconds', final), 'the time limit: %r' % final)
check(time.time() - t < 14, 'the stop came at the limit')

# one at a time
with tempfile.TemporaryDirectory() as ui:
    pre = 'UI_DIR=%s\nENROLL_STATUS=$UI_DIR/enroll.status\nENROLL_LIMIT=90\nlog() { echo "LOG: $*" >&2; }\nenroll() { sleep 5; }\n' % ui
    r = subprocess.run(['sh', '-c', pre + lib + '\nenroll_start @/x; enroll_start @/y; echo "rc=$?"'], capture_output=True, text=True, timeout=30)
    check('rc=1' in r.stdout and 'already running' in r.stderr, 'a second enrolment while one runs is refused: %r %r' % (r.stdout, r.stderr))

if failed:
    sys.exit(1)
print('test_enroll_start.py: ok')
