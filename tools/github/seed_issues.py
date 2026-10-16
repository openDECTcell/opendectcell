# SPDX-License-Identifier: GPL-2.0-or-later
"""Create GitHub milestones + issues from a ROADMAP.md (idempotent: skips existing titles).

Each "## <title>" section becomes a milestone, each open "- [ ] item" an issue.
Labels: "hardware" for items containing [hw]; "research" for M0 items and items
starting with Research/Evaluate; "design" for sections whose title contains "design".

Usage: python3 seed_issues.py ROADMAP.md OWNER/REPO [--dry-run]
Env:   GH=/path/to/gh (default: gh in PATH)"""
import json
import re
import subprocess
import sys

import os
GH = os.environ.get('GH', 'gh')
roadmap, repo = sys.argv[1], sys.argv[2]
dry = '--dry-run' in sys.argv


def gh(*args, data=None):
    cmd = [GH, 'api', *args]
    if dry and any(a in ('-X', '--method') for a in args):
        print('DRY', ' '.join(args), json.dumps(data or {})[:120])
        return {}
    res = subprocess.run(cmd + (['--input', '-'] if data else []), input=json.dumps(data) if data else None,
                         capture_output=True, text=True, check=True)
    return json.loads(res.stdout) if res.stdout.strip() else {}


sections = []
for line in open(roadmap, encoding='utf-8'):
    m = re.match(r'^## (.+)$', line)
    if m:
        sections.append({'title': m.group(1).strip(), 'items': []})
        continue
    m = re.match(r'^- \[( |x)\] (.+)$', line)
    if m and sections:
        sections[-1]['items'].append((m.group(1) == 'x', m.group(2).strip()))

labels = {'hardware': ('needs a real box', 'd93f0b'), 'research': ('reverse engineering / analysis', '5319e7'),
          'design': ('design / specification work', '0e8a16')}
existing_labels = {l['name'] for l in gh(f'repos/{repo}/labels?per_page=100')}
for name, (desc, color) in labels.items():
    if name not in existing_labels:
        gh(f'repos/{repo}/labels', '-X', 'POST', data={'name': name, 'description': desc, 'color': color})

ms = {m['title']: m['number'] for m in gh(f'repos/{repo}/milestones?state=all&per_page=100')}
issues = {i['title'] for i in gh(f'repos/{repo}/issues?state=all&per_page=100')}
for sec in sections:
    if sec['title'] not in ms:
        r = gh(f'repos/{repo}/milestones', '-X', 'POST', data={'title': sec['title']})
        ms[sec['title']] = r.get('number')
    for done, text in sec['items']:
        if done:
            continue
        title = re.sub(r'\s*\*\*\[hw\]\*\*\s*', ' ', text).replace('`', '').strip()
        title = title[:120]
        if title in issues:
            continue
        lab = []
        if '[hw]' in text:
            lab.append('hardware')
        if sec['title'].startswith('M0') or text.lower().startswith(('research', 'evaluate', 'stub (cr16c)')):
            lab.append('research')
        if 'design' in sec['title'].lower():
            lab.append('design')
        body = f"{text}\n\nMilestone: **{sec['title']}** — see [ROADMAP.md](../blob/main/ROADMAP.md)."
        gh(f'repos/{repo}/issues', '-X', 'POST',
           data={'title': title, 'body': body, 'milestone': ms[sec['title']], 'labels': lab})
        print('issue:', title)
