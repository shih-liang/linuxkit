#!/usr/bin/env python3
"""Generate UI choices from the C installer's application policy, or check drift."""
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parent
path = root / 'catalog.json'
catalog = json.loads(path.read_text())
apps = json.loads((root / '../guest-platform/installer/applications.json').read_text())['applications']
managers = {'alpine':'apk', 'ubuntu':'apt', 'debian':'apt', 'fedora':'dnf', 'archlinux':'pacman', 'archlinux-arm':'pacman'}
for distro in catalog['distributions']:
    manager = managers[distro.get('installerDistribution', distro['id'])]
    choices = []
    for app in apps:
        if not app.get('visible'):
            continue
        if not any(app.get(group, {}).get(manager, app.get(group, {}).get('all')) for group in ('packages', 'scripts')):
            continue
        choices.append({key:app[key] for key in ('id', 'name', 'description')})
    if '--write' in sys.argv:
        distro['software'] = choices
    else:
        assert distro['software'] == choices, f"{distro['id']}: regenerate with adapters/sync-apps.py --write"
if '--write' in sys.argv:
    catalog['revision'] = max(10, catalog['revision'])
    path.write_text(json.dumps(catalog, indent=2) + '\n')
print('application choices match the C installer policy')
