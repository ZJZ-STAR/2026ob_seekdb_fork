#!/usr/bin/env python3
"""Collect checksums and passing test records for the isolated kernel build."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
from datetime import datetime, timezone
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('run_kernel_tests.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
baseline = '248daab26650470be6a29ece5d8ef4d7b27873d6'

def git(*args):
    return subprocess.check_output(['git'] + list(args), cwd=str(runner.repo), universal_newlines=True).strip()

def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()

official = json.loads((runner.output / 'mysqltest/summary.json').read_text())
regression = json.loads((runner.output / 'mysqltest/ai_function-summary.json').read_text())
faults = json.loads((runner.output / 'fault-integration.json').read_text())
unit = ET.parse(str(runner.output / 'cpp/results.xml')).getroot()
assert len(official) == 5 and all(t['exit_code'] == 0 for t in official)
assert len(regression) == 2 and all(t['exit_code'] == 0 for t in regression)
assert len(faults) == 11 and all(t['passed'] for t in faults)
assert unit.get('tests') == '10' and unit.get('failures') == '0'
names = set(git('diff', '--name-only', baseline).splitlines())
names.update(git('ls-files', '--others', '--exclude-standard').splitlines())
sources = {}
for name in sorted(names):
    if name.startswith(('src/', 'unittest/')) and '__pycache__' not in name:
        path = runner.repo / name
        if path.is_file():
            sources[name] = {'sha256': digest(path), 'bytes': path.stat().st_size}
logs = runner.output / 'evidence-logs'
logs.mkdir(exist_ok=True)
for source, target in [('ai-column-build.log', 'build.txt'), ('ai-column-all-tests.log', 'all-tests.txt'),
                       ('ai-column-fault-tests.log', 'fault-tests.txt')]:
    shutil.copyfile(runner.workspace / 'logs' / source, logs / target)
manifest = {'collected_at': datetime.now(timezone.utc).isoformat(), 'baseline': baseline,
            'commit': git('rev-parse', 'HEAD'), 'branch': git('branch', '--show-current'),
            'dirty': bool(git('status', '--porcelain', '--', 'src', 'unittest')),
            'binary': str(runner.binary), 'binary_sha256': digest(runner.binary),
            'unit_tests': 10, 'official_mysqltests': official, 'regression_mysqltests': regression,
            'integration_scenarios': faults, 'source_files': sources}
(runner.output / 'build-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({key: manifest[key] for key in ['commit', 'dirty', 'unit_tests', 'binary_sha256']}, indent=2))
